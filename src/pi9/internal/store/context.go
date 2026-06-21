package store

// Project context, system-prompt override, and append-system support.
//
// This mirrors pi's resource-loader/system-prompt behavior:
//
//   - AGENTS.md / CLAUDE.md context files: a global one in Home(), then the
//     cwd walked UP to the git (or filesystem) root, collecting the first
//     AGENTS.md or CLAUDE.md in each directory. Each file is wrapped in a
//     <project_context path="..."> ... </project_context> block.
//   - SYSTEM.md: a full replacement system prompt. Project .pi/SYSTEM.md wins
//     when trusted, otherwise the global Home()/SYSTEM.md.
//   - APPEND_SYSTEM.md: text appended to the system prompt. Global plus
//     project (when trusted), concatenated.
//
// Project-local files (anything under cwd) are honored only when the project
// is trusted; global files (under Home()) are always honored.

import (
	"os"
	"path/filepath"
	"strings"
)

// contextFileNames are the candidate context filenames, in priority order
// within a single directory. Matching is case-insensitive.
var contextFileNames = []string{"AGENTS.md", "CLAUDE.md"}

// loadContextFileFromDir returns the absolute path and contents of the first
// context file found in dir (case-insensitive match against contextFileNames),
// or ("", "", false) if none exists or none is readable.
func loadContextFileFromDir(dir string) (path, content string, ok bool) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return "", "", false
	}
	// Build a case-insensitive lookup of the directory's files.
	byLower := make(map[string]string, len(entries))
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		byLower[strings.ToLower(e.Name())] = e.Name()
	}
	for _, cand := range contextFileNames {
		actual, present := byLower[strings.ToLower(cand)]
		if !present {
			continue
		}
		p := filepath.Join(dir, actual)
		b, err := os.ReadFile(p)
		if err != nil {
			continue
		}
		return p, string(b), true
	}
	return "", "", false
}

// LoadContextFiles returns all discovered AGENTS.md/CLAUDE.md context files
// wrapped in pi's single outer block (matching system-prompt.ts):
//
//	<project_context>
//
//	Project-specific instructions and guidelines:
//
//	<project_instructions path="ABS_PATH">
//	CONTENT
//	</project_instructions>
//	...
//	</project_context>
//
// Order: the global Home()/AGENTS.md (or CLAUDE.md) first, then files
// discovered while walking from the filesystem root DOWN to cwd (i.e.
// outermost ancestor first, cwd last — pi's unshift semantics). Files are
// deduped by absolute path.
//
// Unlike .pi/ resources, AGENTS.md/CLAUDE.md context files are NOT
// trust-gated — pi loads them regardless (they are data wrapped in
// <project_context>, not a system-prompt override). The trusted argument is
// accepted for signature symmetry with the other loaders but unused here.
//
// Returns "" when nothing is found.
func LoadContextFiles(cwd string, trusted bool) string {
	_ = trusted // context files load regardless of trust, matching pi
	type cf struct {
		path    string
		content string
	}
	var files []cf
	seen := make(map[string]bool)

	// Global context file from Home() — always included, first.
	if p, c, ok := loadContextFileFromDir(Home()); ok && !seen[p] {
		seen[p] = true
		files = append(files, cf{path: p, content: c})
	}

	// Walk cwd UP to the filesystem root, collecting context files. We
	// prepend each so the final order is outermost-ancestor-first, with cwd
	// last (matching pi's loadProjectContextFiles, which walks to "/").
	if abs, err := filepath.Abs(cwd); err == nil {
		var ancestors []cf
		dir := abs
		for {
			if p, c, ok := loadContextFileFromDir(dir); ok && !seen[p] {
				seen[p] = true
				ancestors = append([]cf{{path: p, content: c}}, ancestors...)
			}
			parent := filepath.Dir(dir)
			if parent == dir {
				break // filesystem root
			}
			dir = parent
		}
		files = append(files, ancestors...)
	}

	if len(files) == 0 {
		return ""
	}

	var b strings.Builder
	b.WriteString("<project_context>\n\n")
	b.WriteString("Project-specific instructions and guidelines:\n\n")
	for _, f := range files {
		b.WriteString(`<project_instructions path="`)
		b.WriteString(f.path)
		b.WriteString("\">\n")
		b.WriteString(f.content)
		if !strings.HasSuffix(f.content, "\n") {
			b.WriteString("\n")
		}
		b.WriteString("</project_instructions>\n\n")
	}
	b.WriteString("</project_context>\n")
	return b.String()
}

// gitOrFSRoot returns the nearest ancestor of abs (inclusive) containing a
// .git entry, or the filesystem root if none is found. Used by project-local
// skill discovery to bound the upward walk at the repository root.
func gitOrFSRoot(abs string) string {
	dir := abs
	for {
		if _, err := os.Stat(filepath.Join(dir, ".git")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return dir // filesystem root
		}
		dir = parent
	}
}

// SystemOverride returns the replacement system prompt (SYSTEM.md), if any.
//
// When trusted and a project .pi/SYSTEM.md exists, that wins. Otherwise the
// global Home()/SYSTEM.md is used. The bool is false when no override file
// is present.
func SystemOverride(cwd string, trusted bool) (string, bool) {
	if trusted {
		if abs, err := filepath.Abs(cwd); err == nil {
			p := filepath.Join(abs, configDirName, "SYSTEM.md")
			if b, err := os.ReadFile(p); err == nil {
				return string(b), true
			}
		}
	}
	p := filepath.Join(Home(), "SYSTEM.md")
	if b, err := os.ReadFile(p); err == nil {
		return string(b), true
	}
	return "", false
}

// AppendSystem returns the APPEND_SYSTEM.md text to append to the system
// prompt: the global Home()/APPEND_SYSTEM.md plus the project
// .pi/APPEND_SYSTEM.md (when trusted), concatenated in that order. Returns
// "" when neither exists.
func AppendSystem(cwd string, trusted bool) string {
	var parts []string
	if b, err := os.ReadFile(filepath.Join(Home(), "APPEND_SYSTEM.md")); err == nil {
		parts = append(parts, string(b))
	}
	if trusted {
		if abs, err := filepath.Abs(cwd); err == nil {
			p := filepath.Join(abs, configDirName, "APPEND_SYSTEM.md")
			if b, err := os.ReadFile(p); err == nil {
				parts = append(parts, string(b))
			}
		}
	}
	return strings.Join(parts, "\n\n")
}

// HasProjectResources reports whether the project directory has a
// .pi/ config dir holding loadable resources (SYSTEM.md, APPEND_SYSTEM.md,
// or a non-empty prompts/ or skills/ subdir). Used to decide whether to
// surface the "/trust" hint on startup for untrusted projects.
//
// This is a pure presence check — it does not consult the trust store.
func HasProjectResources(cwd string) bool {
	abs, err := filepath.Abs(cwd)
	if err != nil {
		return false
	}
	base := filepath.Join(abs, configDirName)
	for _, name := range []string{"SYSTEM.md", "APPEND_SYSTEM.md"} {
		if fi, err := os.Stat(filepath.Join(base, name)); err == nil && !fi.IsDir() {
			return true
		}
	}
	for _, sub := range []string{"prompts", "skills"} {
		if entries, err := os.ReadDir(filepath.Join(base, sub)); err == nil {
			for _, e := range entries {
				if !e.IsDir() && strings.HasSuffix(e.Name(), ".md") {
					return true
				}
			}
		}
	}
	return false
}
