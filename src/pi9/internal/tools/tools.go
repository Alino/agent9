// Package tools owns pi9's tool palette and dispatch.
//
// Tool tiers:
//   - portable: read_file, write_file, run_rc, remember, read_skill
//   - plan9 shell-out: plumb, hget, walk, ns  (shell out to /bin tools;
//     work compilation-wise everywhere but fail at runtime off-plan9)
//   - plan9 syscall: bind, mount  (golang.org/x/sys/plan9 calls;
//     build-tagged so the binary still compiles on non-plan9)
//
// Each tool has:
//   - a name and human description shown to the model
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
	"runtime"
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
	Description string
	Parameters  map[string]any
	Run         ToolFn
}

// registry holds the tools available to the model. Order-insensitive.
var registry = []Tool{
	{
		Name:        "read_file",
		Description: "Read a text file from the host. Returns the file contents.",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path": map[string]any{
					"type":        "string",
					"description": "Absolute or relative path to the file.",
				},
			},
			"required": []string{"path"},
		},
		Run: readFile,
	},
	{
		Name:        "write_file",
		Description: "Write content to a file. Creates parent dirs if needed. Overwrites if exists.",
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

// Run dispatches a single tool invocation by name. Returns the
// result string (always — even on error, so the model can see what
// went wrong) and the error.
func Run(name, argsJSON string) (string, error) {
	for _, t := range registry {
		if t.Name == name {
			return t.Run(argsJSON)
		}
	}
	return "", fmt.Errorf("unknown tool %q", name)
}

// ---------- tool implementations ----------

func readFile(argsJSON string) (string, error) {
	var args struct {
		Path string `json:"path"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	if args.Path == "" {
		return "", fmt.Errorf("path is required")
	}
	b, err := os.ReadFile(args.Path)
	if err != nil {
		return "", err
	}
	return truncate(string(b)), nil
}

func writeFile(argsJSON string) (string, error) {
	var args struct {
		Path    string `json:"path"`
		Content string `json:"content"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
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

func runShell(argsJSON string) (string, error) {
	var args struct {
		Command string `json:"command"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	if strings.TrimSpace(args.Command) == "" {
		return "", fmt.Errorf("command is required")
	}
	var cmd *exec.Cmd
	if runtime.GOOS == "plan9" {
		cmd = exec.Command("/bin/rc", "-c", args.Command)
	} else {
		cmd = exec.Command("/bin/sh", "-c", args.Command)
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
