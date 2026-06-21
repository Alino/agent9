// Package tools owns pi9's tool palette and dispatch.
//
// Tool tiers:
//   - portable file ops: read, write, edit  (canonical names; read_file
//     and write_file remain as hidden aliases for back-compat)
//   - portable search:   grep, find, ls  (pure-Go via regexp + filepath;
//     compile and run on host AND plan9, never shell out)
//   - portable misc:     run_rc, remember, read_skill
//   - plan9 shell-out:   plumb, hget, walk, ns  (shell out to /bin tools;
//     work compilation-wise everywhere but fail at runtime off-plan9)
//   - plan9 syscall:     bind, mount  (golang.org/x/sys/plan9 calls;
//     build-tagged so the binary still compiles on non-plan9)
//
// Each tool has:
//   - a name and human description shown to the model
//   - an optional set of hidden aliases that still dispatch via Run()
//     but are not advertised by Schemas()
//   - a JSON-schema for the parameters block
//   - a Run() that takes the model's argument JSON and returns
//     a string result (stdout-ish) and an error
//
// Tool results are truncated to maxResultBytes so a single
// `walk /` doesn't blow out the model's context.
package tools

import (
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"

	"github.com/alino/plan9-winxp/pi9/internal/provider"
	"github.com/alino/plan9-winxp/pi9/internal/store"
)

// maxResultBytes caps each tool's output so a noisy command doesn't
// inflate the prompt. Tool results exceeding this are truncated with
// a "[…N more bytes truncated]" suffix.
const maxResultBytes = 16 * 1024

// truncate clips s to at most maxResultBytes; if clipped, appends a
// suffix noting how many bytes were dropped.
func truncate(s string) string {
	if len(s) <= maxResultBytes {
		return s
	}
	dropped := len(s) - maxResultBytes
	return s[:maxResultBytes] + fmt.Sprintf("\n[… %d more bytes truncated]", dropped)
}

// ToolFn is the runner signature: take JSON arg string, return result
// text and optional error.
type ToolFn func(argsJSON string) (string, error)

// Tool is one registered tool. Internal — the schema returned by
// Schemas() is provider.Tool.
type Tool struct {
	Name        string
	Aliases     []string // hidden alternate names that still dispatch via Run
	Description string
	Parameters  map[string]any
	Run         ToolFn
}

// registry holds the tools available to the model. Order-insensitive.
var registry = []Tool{
	{
		Name:        "read",
		Aliases:     []string{"read_file"},
		Description: "Read the contents of a text file. Use offset (1-indexed start line) and limit (max lines) for large files. Output is truncated to 16KB.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path": map[string]any{
					"type":        "string",
					"description": "Absolute or relative path to the file.",
				},
				"offset": map[string]any{
					"type":        "integer",
					"description": "Line number to start reading from (1-indexed).",
				},
				"limit": map[string]any{
					"type":        "integer",
					"description": "Maximum number of lines to read.",
				},
			},
			"required": []string{"path"},
		},
		Run: readFile,
	},
	{
		Name:        "write",
		Aliases:     []string{"write_file"},
		Description: "Write content to a file. Creates parent dirs if needed. Overwrites if exists. Use edit for changing existing files.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path":    map[string]any{"type": "string"},
				"content": map[string]any{"type": "string"},
			},
			"required": []string{"path", "content"},
		},
		Run: writeFile,
	},
	{
		Name:        "edit",
		Description: "Edit a file by exact text replacement. Each edits[].oldText must match a unique region of the original file (matching is against the original content, not applied incrementally). Errors if any oldText is missing or occurs more than once. Returns a unified-diff-style summary.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path": map[string]any{
					"type":        "string",
					"description": "Path to the file to edit.",
				},
				"edits": map[string]any{
					"type":        "array",
					"description": "One or more targeted replacements, each matched against the original file.",
					"items": map[string]any{
						"type": "object",
						"properties": map[string]any{
							"oldText": map[string]any{
								"type":        "string",
								"description": "Exact text to replace. Must be unique in the original file.",
							},
							"newText": map[string]any{
								"type":        "string",
								"description": "Replacement text.",
							},
						},
						"required": []string{"oldText", "newText"},
					},
				},
			},
			"required": []string{"path", "edits"},
		},
		Run: editFile,
	},
	{
		Name:        "grep",
		Description: "Search file contents for a pattern (regex by default, or literal). Walks the tree under path, emits path:line:text with optional surrounding context lines. Pure Go — works on host and Plan 9.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"pattern": map[string]any{
					"type":        "string",
					"description": "Search pattern (regex, or literal string when literal=true).",
				},
				"path": map[string]any{
					"type":        "string",
					"description": "Directory or file to search (default: current directory).",
				},
				"glob": map[string]any{
					"type":        "string",
					"description": "Only search files whose base name matches this glob, e.g. '*.go'.",
				},
				"ignoreCase": map[string]any{
					"type":        "boolean",
					"description": "Case-insensitive search (default: false).",
				},
				"literal": map[string]any{
					"type":        "boolean",
					"description": "Treat pattern as a literal string instead of a regex (default: false).",
				},
				"context": map[string]any{
					"type":        "integer",
					"description": "Lines of context to show before and after each match (default: 0).",
				},
				"limit": map[string]any{
					"type":        "integer",
					"description": "Maximum number of matches to return (default: 100).",
				},
			},
			"required": []string{"pattern"},
		},
		Run: grepTool,
	},
	{
		Name:        "find",
		Description: "Find files by glob pattern. Use '**' to recurse into subdirectories (e.g. '**/*.go'). Returns matching paths relative to the search directory. Pure Go — works on host and Plan 9.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"pattern": map[string]any{
					"type":        "string",
					"description": "Glob pattern, e.g. '*.go', '**/*.json', 'src/**/*_test.go'.",
				},
				"path": map[string]any{
					"type":        "string",
					"description": "Directory to search in (default: current directory).",
				},
				"limit": map[string]any{
					"type":        "integer",
					"description": "Maximum number of results (default: 1000).",
				},
			},
			"required": []string{"pattern"},
		},
		Run: findTool,
	},
	{
		Name:        "ls",
		Description: "List directory contents, sorted alphabetically, with a trailing '/' on directories. Pure Go — works on host and Plan 9.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path": map[string]any{
					"type":        "string",
					"description": "Directory to list (default: current directory).",
				},
				"limit": map[string]any{
					"type":        "integer",
					"description": "Maximum number of entries to return (default: 500).",
				},
			},
		},
		Run: lsTool,
	},
	{
		Name:        "run_rc",
		Description: "Run a shell command. Uses rc on Plan 9, sh on unix. Returns combined stdout+stderr and exit status.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"command": map[string]any{
					"type":        "string",
					"description": "The command line to run.",
				},
			},
			"required": []string{"command"},
		},
		Run: runShell,
	},
	{
		Name:        "remember",
		Description: "Save a durable fact, preference, or note to pi9's long-term memory. Appended to memory.md and re-loaded into the system prompt on next launch. Use for facts that will still matter in future sessions.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"content": map[string]any{
					"type":        "string",
					"description": "A short, declarative fact. One per call.",
				},
			},
			"required": []string{"content"},
		},
		Run: remember,
	},
	{
		Name:        "read_skill",
		Description: "Load a skill's full content. The system prompt lists available skill names; call this when you need the detailed instructions for one.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"name": map[string]any{
					"type":        "string",
					"description": "Skill name as it appears in the system prompt's skill list.",
				},
			},
			"required": []string{"name"},
		},
		Run: readSkill,
	},

	// ---------- Plan9-native tools (Phase 5) ----------
	//
	// These shell out to /bin/<tool> on plan9. They compile (and run)
	// on unix too — failure happens at runtime via the missing
	// binary. We don't gate them by GOOS to keep the schema stable
	// across builds.

	{
		Name:        "plumb",
		Description: "Send text through Plan 9's plumber to a named port. Equivalent to right-click 'open with' on other OSes — the plumber routes the text to the right handler (e.g. 'edit' opens it in the editor, 'web' opens it in a browser). Plan 9 only.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"port": map[string]any{
					"type":        "string",
					"description": "Plumber port: edit, web, image, etc.",
				},
				"content": map[string]any{
					"type":        "string",
					"description": "The text/path/url to plumb.",
				},
			},
			"required": []string{"port", "content"},
		},
		Run: plumb,
	},
	{
		Name:        "hget",
		Description: "Fetch a URL via Plan 9's native HTTP client (no curl needed). Uses /sys/lib/tls/ca.pem for TLS. Returns the response body. Plan 9 only.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"url": map[string]any{
					"type":        "string",
					"description": "http:// or https:// URL.",
				},
			},
			"required": []string{"url"},
		},
		Run: hget,
	},
	{
		Name:        "walk",
		Description: "Walk a directory tree and list files (Plan 9's recursive directory lister). Use this instead of 'find' or 'ls -R'. Returns one path per line. Plan 9 only.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path": map[string]any{
					"type":        "string",
					"description": "Starting directory.",
				},
				"depth": map[string]any{
					"type":        "integer",
					"description": "Max depth (default unlimited).",
				},
			},
			"required": []string{"path"},
		},
		Run: walk,
	},
	{
		Name:        "ns",
		Description: "Dump the current namespace — what's mounted where in this process's view of the filesystem. Plan 9's namespaces are per-process and this is the killer feature of plan9 vs unix. Plan 9 only.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"filter": map[string]any{
					"type":        "string",
					"description": "Optional grep-style substring to filter entries.",
				},
			},
		},
		Run: nsTool,
	},
	{
		Name:        "bind",
		Description: "Bind a filesystem path into pi9's own namespace. Plan 9's bind is like Linux's bind mount but per-process and reversible. Affects pi9's view only — not the user's shell. Plan 9 only.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"src": map[string]any{
					"type":        "string",
					"description": "Source path (existing).",
				},
				"dst": map[string]any{
					"type":        "string",
					"description": "Destination mountpoint.",
				},
				"flag": map[string]any{
					"type":        "string",
					"description": "One of: replace (default), before, after, create.",
				},
			},
			"required": []string{"src", "dst"},
		},
		Run: bindTool,
	},
	{
		Name:        "mount",
		Description: "Mount a 9P service (typically from /srv/<name>) into pi9's namespace. Affects pi9's view only. Plan 9 only.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"srv": map[string]any{
					"type":        "string",
					"description": "Path to a /srv/ file or 9P address.",
				},
				"mountpoint": map[string]any{
					"type":        "string",
					"description": "Where to mount it (e.g. /n/foo).",
				},
				"flag": map[string]any{
					"type":        "string",
					"description": "One of: replace (default), before, after.",
				},
			},
			"required": []string{"srv", "mountpoint"},
		},
		Run: mountTool,
	},
}

// Schemas returns the provider-shaped tool list suitable for passing
// in provider.Config.Tools.
func Schemas() []provider.Tool {
	out := make([]provider.Tool, 0, len(registry))
	for _, t := range registry {
		out = append(out, provider.Tool{
			Name:        t.Name,
			Description: t.Description,
			Parameters:  t.Parameters,
		})
	}
	return out
}

// Run dispatches a single tool invocation by name. The name may be a
// tool's canonical Name or one of its hidden Aliases. Returns the
// result string (always — even on error, so the model can see what
// went wrong) and the error.
func Run(name, argsJSON string) (string, error) {
	for _, t := range registry {
		if t.Name == name {
			return t.Run(argsJSON)
		}
		for _, a := range t.Aliases {
			if a == name {
				return t.Run(argsJSON)
			}
		}
	}
	return "", fmt.Errorf("unknown tool %q", name)
}

// ---------- tool implementations ----------

func readFile(argsJSON string) (string, error) {
	var args struct {
		Path     string `json:"path"`
		FilePath string `json:"file_path"` // alias pi tolerates
		Offset   int    `json:"offset"`
		Limit    int    `json:"limit"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	if args.Path == "" {
		args.Path = args.FilePath
	}
	if args.Path == "" {
		return "", fmt.Errorf("path is required")
	}
	b, err := os.ReadFile(args.Path)
	if err != nil {
		return "", err
	}
	// When neither offset nor limit is given, return the whole file (the
	// common case) so we don't perturb behaviour with line splitting.
	if args.Offset == 0 && args.Limit == 0 {
		return truncate(string(b)), nil
	}
	lines := strings.Split(string(b), "\n")
	// offset is 1-indexed; default to the first line.
	start := args.Offset - 1
	if args.Offset == 0 {
		start = 0
	}
	if start < 0 {
		start = 0
	}
	if start >= len(lines) {
		return "", fmt.Errorf("offset %d is beyond end of file (%d lines total)", args.Offset, len(lines))
	}
	end := len(lines)
	if args.Limit > 0 && start+args.Limit < end {
		end = start + args.Limit
	}
	return truncate(strings.Join(lines[start:end], "\n")), nil
}

func writeFile(argsJSON string) (string, error) {
	var args struct {
		Path     string `json:"path"`
		FilePath string `json:"file_path"` // alias pi tolerates
		Content  string `json:"content"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	if args.Path == "" {
		args.Path = args.FilePath
	}
	if args.Path == "" {
		return "", fmt.Errorf("path is required")
	}
	if dir := filepath.Dir(args.Path); dir != "." && dir != "" {
		_ = os.MkdirAll(dir, 0755)
	}
	if err := os.WriteFile(args.Path, []byte(args.Content), 0644); err != nil {
		return "", err
	}
	return fmt.Sprintf("wrote %d bytes to %s", len(args.Content), args.Path), nil
}

// editFile applies a list of exact-text replacements to a file. Every
// oldText is matched against the ORIGINAL file content (not the result
// of earlier edits), and must occur exactly once — a missing or
// ambiguous oldText is an error naming the offending edit. All edits are
// applied to the original content, the file is rewritten, and a concise
// unified-diff-style summary is returned.
func editFile(argsJSON string) (string, error) {
	var args struct {
		Path     string `json:"path"`
		FilePath string `json:"file_path"` // alias pi tolerates
		Edits    []struct {
			OldText string `json:"oldText"`
			NewText string `json:"newText"`
		} `json:"edits"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	if args.Path == "" {
		args.Path = args.FilePath
	}
	if args.Path == "" {
		return "", fmt.Errorf("path is required")
	}
	if len(args.Edits) == 0 {
		return "", fmt.Errorf("edits must contain at least one replacement")
	}
	b, err := os.ReadFile(args.Path)
	if err != nil {
		return "", err
	}
	original := string(b)

	// Validate every oldText against the ORIGINAL content first, recording
	// each match's byte span, so we reject the whole call atomically before
	// writing anything.
	type span struct {
		start, end int // byte offsets into original
		newText    string
		idx        int // 1-based edit number, for errors
	}
	var spans []span
	for i, e := range args.Edits {
		if e.OldText == "" {
			return "", fmt.Errorf("edit %d: oldText is empty", i+1)
		}
		n := strings.Count(original, e.OldText)
		if n == 0 {
			return "", fmt.Errorf("edit %d: oldText not found in %s: %q", i+1, args.Path, snippet(e.OldText))
		}
		if n > 1 {
			return "", fmt.Errorf("edit %d: oldText occurs %d times in %s (must be unique): %q", i+1, n, args.Path, snippet(e.OldText))
		}
		start := strings.Index(original, e.OldText)
		spans = append(spans, span{start: start, end: start + len(e.OldText), newText: e.NewText, idx: i + 1})
	}

	// Apply by descending start offset so earlier offsets stay valid and a
	// replacement's newText can never be mistaken for a later edit's oldText
	// (matching pi's match-against-original semantics). Reject overlaps.
	sort.Slice(spans, func(i, j int) bool { return spans[i].start > spans[j].start })
	for i := 1; i < len(spans); i++ {
		// spans[i] starts before spans[i-1] (descending order); they overlap
		// if the earlier-starting one extends into the later-starting one.
		if spans[i].end > spans[i-1].start {
			return "", fmt.Errorf("edits %d and %d overlap in %s", spans[i].idx, spans[i-1].idx, args.Path)
		}
	}
	updated := original
	for _, s := range spans {
		updated = updated[:s.start] + s.newText + updated[s.end:]
	}
	if err := os.WriteFile(args.Path, []byte(updated), 0644); err != nil {
		return "", err
	}
	summary := fmt.Sprintf("edited %s (%d replacement(s))\n%s",
		args.Path, len(args.Edits), unifiedDiff(args.Path, original, updated))
	return truncate(summary), nil
}

// snippet shortens a string for inclusion in an error message.
func snippet(s string) string {
	s = strings.ReplaceAll(s, "\n", "\\n")
	const max = 60
	if len(s) > max {
		return s[:max] + "…"
	}
	return s
}

// unifiedDiff produces a compact unified-diff-style summary between two
// versions of a file. It is intentionally simple (no hunk coalescing
// beyond grouping consecutive changes) and adds no dependencies.
func unifiedDiff(path, oldText, newText string) string {
	oldLines := strings.Split(oldText, "\n")
	newLines := strings.Split(newText, "\n")
	ops := diffLines(oldLines, newLines)

	var b strings.Builder
	fmt.Fprintf(&b, "--- %s\n+++ %s\n", path, path)
	for _, op := range ops {
		switch op.kind {
		case diffEqual:
			b.WriteString("  " + op.text + "\n")
		case diffDel:
			b.WriteString("- " + op.text + "\n")
		case diffAdd:
			b.WriteString("+ " + op.text + "\n")
		}
	}
	return strings.TrimRight(b.String(), "\n")
}

type diffKind int

const (
	diffEqual diffKind = iota
	diffDel
	diffAdd
)

type diffOp struct {
	kind diffKind
	text string
}

// diffLines computes a line-level diff via the classic LCS dynamic
// program. Adequate for edit summaries; not optimised for huge files.
func diffLines(a, b []string) []diffOp {
	n, m := len(a), len(b)
	// lcs[i][j] = length of LCS of a[i:] and b[j:].
	lcs := make([][]int, n+1)
	for i := range lcs {
		lcs[i] = make([]int, m+1)
	}
	for i := n - 1; i >= 0; i-- {
		for j := m - 1; j >= 0; j-- {
			if a[i] == b[j] {
				lcs[i][j] = lcs[i+1][j+1] + 1
			} else if lcs[i+1][j] >= lcs[i][j+1] {
				lcs[i][j] = lcs[i+1][j]
			} else {
				lcs[i][j] = lcs[i][j+1]
			}
		}
	}
	var ops []diffOp
	i, j := 0, 0
	for i < n && j < m {
		if a[i] == b[j] {
			ops = append(ops, diffOp{diffEqual, a[i]})
			i++
			j++
		} else if lcs[i+1][j] >= lcs[i][j+1] {
			ops = append(ops, diffOp{diffDel, a[i]})
			i++
		} else {
			ops = append(ops, diffOp{diffAdd, b[j]})
			j++
		}
	}
	for ; i < n; i++ {
		ops = append(ops, diffOp{diffDel, a[i]})
	}
	for ; j < m; j++ {
		ops = append(ops, diffOp{diffAdd, b[j]})
	}
	return ops
}

// grepTool searches file contents under a path for a pattern, emitting
// path:line:text (with '-' separators for context lines). It walks the
// tree with filepath.WalkDir and uses regexp (or a literal matcher),
// so it is fully portable across host and Plan 9.
func grepTool(argsJSON string) (string, error) {
	var args struct {
		Pattern    string `json:"pattern"`
		Path       string `json:"path"`
		Glob       string `json:"glob"`
		IgnoreCase bool   `json:"ignoreCase"`
		Literal    bool   `json:"literal"`
		Context    int    `json:"context"`
		Limit      int    `json:"limit"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	if args.Pattern == "" {
		return "", fmt.Errorf("pattern is required")
	}
	root := args.Path
	if root == "" {
		root = "."
	}
	limit := args.Limit
	if limit <= 0 {
		limit = 100
	}

	// Build the matcher. Literal mode quotes the pattern; ignoreCase adds
	// the (?i) flag. We always compile a regexp for a single code path.
	pat := args.Pattern
	if args.Literal {
		pat = regexp.QuoteMeta(pat)
	}
	if args.IgnoreCase {
		pat = "(?i)" + pat
	}
	re, err := regexp.Compile(pat)
	if err != nil {
		return "", fmt.Errorf("bad pattern: %w", err)
	}

	info, err := os.Stat(root)
	if err != nil {
		return "", err
	}

	var out []string
	matches := 0
	limitHit := false

	searchFile := func(path, rel string) error {
		if matches >= limit {
			limitHit = true
			return nil
		}
		b, err := os.ReadFile(path)
		if err != nil {
			return nil // unreadable file: skip silently
		}
		lines := strings.Split(string(b), "\n")
		for idx, line := range lines {
			if matches >= limit {
				limitHit = true
				break
			}
			if !re.MatchString(line) {
				continue
			}
			matches++
			if args.Context > 0 {
				start := idx - args.Context
				if start < 0 {
					start = 0
				}
				end := idx + args.Context
				if end >= len(lines) {
					end = len(lines) - 1
				}
				for c := start; c <= end; c++ {
					if c == idx {
						out = append(out, fmt.Sprintf("%s:%d: %s", rel, c+1, lines[c]))
					} else {
						out = append(out, fmt.Sprintf("%s-%d- %s", rel, c+1, lines[c]))
					}
				}
			} else {
				out = append(out, fmt.Sprintf("%s:%d: %s", rel, idx+1, line))
			}
		}
		return nil
	}

	if !info.IsDir() {
		if err := searchFile(root, filepath.Base(root)); err != nil {
			return "", err
		}
	} else {
		err = filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
			if err != nil {
				return nil
			}
			if d.IsDir() {
				if skipDir(d.Name()) && path != root {
					return filepath.SkipDir
				}
				return nil
			}
			if args.Glob != "" {
				ok, _ := filepath.Match(args.Glob, d.Name())
				if !ok {
					return nil
				}
			}
			if matches >= limit {
				limitHit = true
				return filepath.SkipAll
			}
			rel, rerr := filepath.Rel(root, path)
			if rerr != nil {
				rel = path
			}
			return searchFile(path, filepath.ToSlash(rel))
		})
		if err != nil {
			return "", err
		}
	}

	if len(out) == 0 {
		return "No matches found", nil
	}
	res := strings.Join(out, "\n")
	if limitHit {
		res += fmt.Sprintf("\n\n[%d matches limit reached. Use limit=%d for more, or refine pattern]", limit, limit*2)
	}
	return truncate(res), nil
}

// findTool returns file paths under a directory matching a glob. A '**'
// segment matches any number of intervening directories; otherwise each
// segment is matched with filepath.Match. Portable across host/Plan 9.
func findTool(argsJSON string) (string, error) {
	var args struct {
		Pattern string `json:"pattern"`
		Path    string `json:"path"`
		Limit   int    `json:"limit"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	if args.Pattern == "" {
		return "", fmt.Errorf("pattern is required")
	}
	root := args.Path
	if root == "" {
		root = "."
	}
	limit := args.Limit
	if limit <= 0 {
		limit = 1000
	}

	var out []string
	limitHit := false
	err := filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if path == root {
			return nil
		}
		if d.IsDir() {
			if skipDir(d.Name()) {
				return filepath.SkipDir
			}
			return nil
		}
		if len(out) >= limit {
			limitHit = true
			return filepath.SkipAll
		}
		rel, rerr := filepath.Rel(root, path)
		if rerr != nil {
			rel = path
		}
		rel = filepath.ToSlash(rel)
		if matchGlob(args.Pattern, rel) {
			out = append(out, rel)
		}
		return nil
	})
	if err != nil {
		return "", err
	}
	if len(out) == 0 {
		return "No files found matching pattern", nil
	}
	sort.Strings(out)
	res := strings.Join(out, "\n")
	if limitHit {
		res += fmt.Sprintf("\n\n[%d results limit reached. Use limit=%d for more]", limit, limit*2)
	}
	return truncate(res), nil
}

// matchGlob matches a slash-separated path against a glob pattern that
// may contain '**' to span directory boundaries. Patterns without a
// slash match against the path's base name (like find's default).
func matchGlob(pattern, path string) bool {
	if !strings.Contains(pattern, "/") {
		ok, _ := filepath.Match(pattern, filepath.Base(path))
		return ok
	}
	return matchSegments(strings.Split(pattern, "/"), strings.Split(path, "/"))
}

// matchSegments matches glob segments against path segments, treating a
// '**' segment as "zero or more path segments".
func matchSegments(pat, name []string) bool {
	if len(pat) == 0 {
		return len(name) == 0
	}
	if pat[0] == "**" {
		// '**' matches zero segments...
		if matchSegments(pat[1:], name) {
			return true
		}
		// ...or one-or-more, by consuming a name segment.
		for i := 0; i < len(name); i++ {
			if matchSegments(pat, name[i+1:]) {
				return true
			}
		}
		return false
	}
	if len(name) == 0 {
		return false
	}
	if ok, _ := filepath.Match(pat[0], name[0]); !ok {
		return false
	}
	return matchSegments(pat[1:], name[1:])
}

// lsTool lists a directory's entries, alphabetically, marking
// directories with a trailing '/'. Portable across host and Plan 9.
func lsTool(argsJSON string) (string, error) {
	var args struct {
		Path  string `json:"path"`
		Limit int    `json:"limit"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	dir := args.Path
	if dir == "" {
		dir = "."
	}
	limit := args.Limit
	if limit <= 0 {
		limit = 500
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return "", err
	}
	if len(entries) == 0 {
		return "(empty directory)", nil
	}
	sort.Slice(entries, func(i, j int) bool {
		return strings.ToLower(entries[i].Name()) < strings.ToLower(entries[j].Name())
	})
	var out []string
	limitHit := false
	for _, e := range entries {
		if len(out) >= limit {
			limitHit = true
			break
		}
		name := e.Name()
		if e.IsDir() {
			name += "/"
		}
		out = append(out, name)
	}
	res := strings.Join(out, "\n")
	if limitHit {
		res += fmt.Sprintf("\n\n[%d entries limit reached. Use limit=%d for more]", limit, limit*2)
	}
	return truncate(res), nil
}

// skipDir reports whether a directory name should be pruned during
// grep/find walks. Keeps noisy VCS/dependency dirs out of results.
func skipDir(name string) bool {
	switch name {
	case ".git", "node_modules", ".hg", ".svn":
		return true
	}
	return false
}

func runShell(argsJSON string) (string, error) {
	var args struct {
		Command string `json:"command"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	return RunShellCommand(args.Command)
}

// RunShellCommand runs a raw command line via rc (Plan 9) or sh (unix),
// returning combined stdout+stderr (truncated to the tool result cap) and
// an exit-status note appended on failure. Exported so the TUI's inline
// "!cmd" / "!!cmd" path reuses the exact same shell-out as the run_rc
// tool. An empty command is an error.
func RunShellCommand(command string) (string, error) {
	if strings.TrimSpace(command) == "" {
		return "", fmt.Errorf("command is required")
	}
	var cmd *exec.Cmd
	if runtime.GOOS == "plan9" {
		cmd = exec.Command("/bin/rc", "-c", command)
	} else {
		cmd = exec.Command("/bin/sh", "-c", command)
	}
	out, err := cmd.CombinedOutput()
	s := string(out)
	if err != nil {
		s += "\n[exit: " + err.Error() + "]"
	}
	return truncate(s), nil
}

func remember(argsJSON string) (string, error) {
	var args struct {
		Content string `json:"content"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	if strings.TrimSpace(args.Content) == "" {
		return "", fmt.Errorf("content is required")
	}
	if err := store.AppendMemory(args.Content); err != nil {
		return "", err
	}
	return fmt.Sprintf("saved to %s", store.MemoryPath()), nil
}

func readSkill(argsJSON string) (string, error) {
	var args struct {
		Name string `json:"name"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	if strings.TrimSpace(args.Name) == "" {
		return "", fmt.Errorf("name is required")
	}
	body, err := store.ReadSkillBody(args.Name)
	if err != nil {
		return "", err
	}
	return truncate(body), nil
}

// ---------- Plan9-native tools (shell-out tier) ----------

// runCmd executes a /bin/<name> command with the given args and
// returns its combined output. Used by the plan9 shell-out tools.
// Marks runtime.GOOS != "plan9" so the model gets a clean error
// rather than a confusing missing-binary message.
func runCmd(name string, args ...string) (string, error) {
	if runtime.GOOS != "plan9" {
		return "", fmt.Errorf("%s is Plan 9 only (running on %s)", name, runtime.GOOS)
	}
	cmd := exec.Command(filepath.Join("/bin", name), args...)
	out, err := cmd.CombinedOutput()
	s := string(out)
	if err != nil {
		s += "\n[exit: " + err.Error() + "]"
	}
	return truncate(s), nil
}

func plumb(argsJSON string) (string, error) {
	var args struct {
		Port    string `json:"port"`
		Content string `json:"content"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	if args.Port == "" || args.Content == "" {
		return "", fmt.Errorf("port and content are required")
	}
	if runtime.GOOS != "plan9" {
		return "", fmt.Errorf("plumb is Plan 9 only (running on %s)", runtime.GOOS)
	}
	cmd := exec.Command("/bin/plumb", "-d", args.Port)
	cmd.Stdin = strings.NewReader(args.Content)
	out, err := cmd.CombinedOutput()
	s := strings.TrimSpace(string(out))
	if err != nil {
		return s, err
	}
	if s == "" {
		s = fmt.Sprintf("plumbed %d bytes to port %q", len(args.Content), args.Port)
	}
	return s, nil
}

func hget(argsJSON string) (string, error) {
	var args struct {
		URL string `json:"url"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	if args.URL == "" {
		return "", fmt.Errorf("url is required")
	}
	return runCmd("hget", args.URL)
}

func walk(argsJSON string) (string, error) {
	var args struct {
		Path  string `json:"path"`
		Depth int    `json:"depth"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	if args.Path == "" {
		return "", fmt.Errorf("path is required")
	}
	cmdArgs := []string{}
	if args.Depth > 0 {
		cmdArgs = append(cmdArgs, "-d", fmt.Sprintf("%d", args.Depth))
	}
	cmdArgs = append(cmdArgs, args.Path)
	return runCmd("walk", cmdArgs...)
}

func nsTool(argsJSON string) (string, error) {
	var args struct {
		Filter string `json:"filter"`
	}
	_ = json.Unmarshal([]byte(argsJSON), &args)
	out, err := runCmd("ns")
	if err != nil {
		return out, err
	}
	if args.Filter == "" {
		return out, nil
	}
	// Filter line-by-line.
	var b strings.Builder
	for _, line := range strings.Split(out, "\n") {
		if strings.Contains(line, args.Filter) {
			b.WriteString(line)
			b.WriteByte('\n')
		}
	}
	return truncate(strings.TrimRight(b.String(), "\n")), nil
}
