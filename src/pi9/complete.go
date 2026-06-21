package main

// Task 2.3 autocomplete dropdown (pi parity).
//
// A non-intrusive completion overlay that appears only while a "/" or
// "@" token is being typed in the editor:
//
//   - input starting with "/"  → matching built-in slash commands +
//     prompt-template names (from store.ListPromptTemplates), each with
//     a short description.
//   - a token starting with "@" → fuzzy file search under cwd.
//
// Tab or Enter completes the highlighted item; Esc dismisses; up/down
// move the selection. The candidate computation is pure (no TUI state)
// so it can be unit-tested; the model just caches the current list.

import (
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/charmbracelet/lipgloss"

	"github.com/alino/plan9-winxp/pi9/internal/provider"
	"github.com/alino/plan9-winxp/pi9/internal/store"
)

// completionKind classifies what the dropdown is completing.
type completionKind int

const (
	completeNone completionKind = iota
	completeSlash
	completeAt
)

// completion is one dropdown row.
type completion struct {
	// value is the text that replaces the active token when accepted
	// (without any leading "/" or "@" - those are re-added on accept to
	// match the trigger).
	value string
	// display is what's shown in the dropdown (may include a description).
	display string
}

// builtinSlashCommands is the static list of built-in commands shown in
// the "/" dropdown, each with a one-line description. Kept in sync with
// handleSlash's switch. Prompt templates are appended dynamically.
var builtinSlashCommands = []completion{
	{"help", "help - list commands"},
	{"hotkeys", "hotkeys - key bindings"},
	{"login", "login - manage credentials"},
	{"logout", "logout - clear credentials"},
	{"model", "model - switch model"},
	{"scoped-models", "scoped-models - toggle Ctrl+P cycle set"},
	{"settings", "settings - thinking, compaction, modes"},
	{"trust", "trust - load project .pi/ resources"},
	{"new", "new - start a new session"},
	{"resume", "resume - pick a previous session"},
	{"name", "name - set session display name"},
	{"session", "session - show session info"},
	{"tree", "tree - navigate the branch tree"},
	{"fork", "fork - fork from a prior message"},
	{"clone", "clone - duplicate the active branch"},
	{"compact", "compact - summarize older context"},
	{"copy", "copy - copy last reply to snarf"},
	{"export", "export - write conversation to a file"},
	{"reload", "reload - re-read config + resources"},
	{"clear", "clear - start a fresh branch"},
	{"quit", "quit - exit pi9"},
}

// detectCompletion inspects the editor buffer (text + cursor) and returns
// the completion kind, the active token's start index (rune offset into
// text), and the partial query (the token text after its trigger char).
//
//   - completeSlash only when the whole line starts with "/" and the
//     cursor is within the first whitespace-delimited token (the command
//     name itself); arguments after the command don't trigger it.
//   - completeAt when the token under/just-before the cursor starts with
//     "@". The token boundary is whitespace; "@" must be at the token's
//     start.
//
// Returns completeNone when neither applies.
func detectCompletion(text string, cursor int) (completionKind, int, string) {
	runes := []rune(text)
	if cursor < 0 {
		cursor = 0
	}
	if cursor > len(runes) {
		cursor = len(runes)
	}

	// Slash: only when the line itself starts with '/'.
	if len(runes) > 0 && runes[0] == '/' {
		// Active only within the first token (the command name). Find the
		// end of the first token.
		end := 0
		for end < len(runes) && !isSpace(runes[end]) {
			end++
		}
		if cursor <= end {
			return completeSlash, 0, string(runes[1:cursor])
		}
		// Cursor is in the args region - no slash completion.
		return completeNone, 0, ""
	}

	// @file: find the token boundary just before the cursor.
	start := cursor
	for start > 0 && !isSpace(runes[start-1]) {
		start--
	}
	if start < len(runes) && runes[start] == '@' {
		// '@' must be at a real token boundary (start of line or after
		// whitespace) - start already is, by construction.
		return completeAt, start, string(runes[start+1 : cursor])
	}
	return completeNone, 0, ""
}

// slashCandidates returns slash + prompt-template completions matching
// query (fuzzy), sorted with exact-prefix matches first. cwd/trusted feed
// store.ListPromptTemplates.
func slashCandidates(query, cwd string, trusted bool) []completion {
	var all []completion
	all = append(all, builtinSlashCommands...)
	for _, t := range store.ListPromptTemplates(cwd, trusted) {
		desc := t.Name
		if t.Description != "" {
			desc = t.Name + " - " + t.Description
		} else {
			desc = t.Name + " - prompt template"
		}
		all = append(all, completion{value: t.Name, display: desc})
	}

	q := strings.ToLower(strings.TrimSpace(query))
	if q == "" {
		return all
	}
	var out []completion
	for _, c := range all {
		if provider.FuzzyMatch(q, c.value) {
			out = append(out, c)
		}
	}
	// Prefix matches first, then the rest, each alphabetical.
	sort.SliceStable(out, func(i, j int) bool {
		pi := strings.HasPrefix(strings.ToLower(out[i].value), q)
		pj := strings.HasPrefix(strings.ToLower(out[j].value), q)
		if pi != pj {
			return pi
		}
		return out[i].value < out[j].value
	})
	return out
}

// fileCandidates fuzzy-searches files under cwd matching query, returning
// up to limit relative paths. Directories are included with a trailing
// "/" so the user can drill in. VCS/dependency dirs are pruned (skipDir
// in tools mirrors this, but we keep an inline copy to avoid coupling).
// readDir defaults to a filepath.WalkDir over cwd; it's a parameter for
// testability.
func fileCandidates(query, cwd string, limit int) []completion {
	if limit <= 0 {
		limit = 20
	}
	q := strings.ToLower(strings.TrimSpace(query))

	var paths []string
	_ = filepath.WalkDir(cwd, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if path == cwd {
			return nil
		}
		name := d.Name()
		if d.IsDir() {
			if completeSkipDir(name) {
				return filepath.SkipDir
			}
		}
		rel, rerr := filepath.Rel(cwd, path)
		if rerr != nil {
			return nil
		}
		rel = filepath.ToSlash(rel)
		if d.IsDir() {
			rel += "/"
		}
		if q == "" || provider.FuzzyMatch(q, strings.ToLower(rel)) {
			paths = append(paths, rel)
		}
		return nil
	})

	// Rank: prefix-of-base-name matches first, then shorter paths, then
	// lexical - keeps the most relevant files near the top.
	sort.SliceStable(paths, func(i, j int) bool {
		bi := strings.ToLower(filepath.Base(strings.TrimSuffix(paths[i], "/")))
		bj := strings.ToLower(filepath.Base(strings.TrimSuffix(paths[j], "/")))
		pi := strings.HasPrefix(bi, q)
		pj := strings.HasPrefix(bj, q)
		if pi != pj {
			return pi
		}
		if len(paths[i]) != len(paths[j]) {
			return len(paths[i]) < len(paths[j])
		}
		return paths[i] < paths[j]
	})

	if len(paths) > limit {
		paths = paths[:limit]
	}
	out := make([]completion, 0, len(paths))
	for _, p := range paths {
		out = append(out, completion{value: p, display: p})
	}
	return out
}

// completeSkipDir mirrors tools.skipDir for the file completer.
func completeSkipDir(name string) bool {
	switch name {
	case ".git", "node_modules", ".hg", ".svn":
		return true
	}
	return false
}

// ---------- pi9Model integration ----------

// refreshCompletions recomputes the dropdown for the current editor state,
// updating m.completeKind / m.completeList / m.completeStart / m.completeCursor.
// Called after every editor keystroke that changes text or cursor. When no
// trigger is active the dropdown is cleared.
func (m *pi9Model) refreshCompletions() {
	if m.streaming {
		m.clearCompletions()
		return
	}
	kind, start, query := detectCompletion(string(m.input), m.inputCursor)
	switch kind {
	case completeSlash:
		cwd, _ := os.Getwd()
		m.completeList = slashCandidates(query, cwd, m.trusted)
	case completeAt:
		cwd, _ := os.Getwd()
		m.completeList = fileCandidates(query, cwd, 20)
	default:
		m.clearCompletions()
		return
	}
	if len(m.completeList) == 0 {
		m.clearCompletions()
		return
	}
	m.completeKind = kind
	m.completeStart = start
	if m.completeCursor >= len(m.completeList) {
		m.completeCursor = len(m.completeList) - 1
	}
	if m.completeCursor < 0 {
		m.completeCursor = 0
	}
}

// clearCompletions dismisses the dropdown.
func (m *pi9Model) clearCompletions() {
	m.completeKind = completeNone
	m.completeList = nil
	m.completeCursor = 0
	m.completeStart = 0
}

// completionsOpen reports whether the dropdown is currently shown.
func (m pi9Model) completionsOpen() bool {
	return m.completeKind != completeNone && len(m.completeList) > 0
}

// acceptCompletion replaces the active token with the highlighted
// candidate. For slash it rewrites the leading command (preserving any
// args already typed after the first space). For @ it replaces the token
// in place. A trailing space is added so the user can keep typing.
func (m *pi9Model) acceptCompletion() {
	if !m.completionsOpen() {
		return
	}
	if m.completeCursor < 0 || m.completeCursor >= len(m.completeList) {
		return
	}
	pick := m.completeList[m.completeCursor]
	runes := []rune(string(m.input))

	switch m.completeKind {
	case completeSlash:
		// Replace from index 0..end-of-first-token with "/value ".
		end := 0
		for end < len(runes) && !isSpace(runes[end]) {
			end++
		}
		repl := []rune("/" + pick.value + " ")
		newRunes := append(append([]rune{}, repl...), runes[end:]...)
		m.input = newRunes
		m.inputCursor = len(repl)
	case completeAt:
		// Replace the @token spanning [completeStart, tokenEnd).
		start := m.completeStart
		end := start
		for end < len(runes) && !isSpace(runes[end]) {
			end++
		}
		// Directories complete without a trailing space so the user can
		// keep drilling in; files get a trailing space.
		suffix := " "
		if strings.HasSuffix(pick.value, "/") {
			suffix = ""
		}
		repl := []rune("@" + pick.value + suffix)
		newRunes := append(append([]rune{}, runes[:start]...), append(repl, runes[end:]...)...)
		m.input = newRunes
		m.inputCursor = start + len(repl)
	}
	// Recompute (a directory completion may re-trigger an @ search).
	m.refreshCompletions()
}

// handleCompletionKey routes navigation keys while the dropdown is open.
// Returns handled=true when the key was consumed by the dropdown so the
// caller skips its normal editor handling. Tab/Enter accept; Esc
// dismisses; up/down move; ctrl+n/ctrl+p too.
func (m *pi9Model) handleCompletionKey(key string) (handled bool) {
	if !m.completionsOpen() {
		return false
	}
	switch key {
	case "up":
		if m.completeCursor > 0 {
			m.completeCursor--
		}
		return true
	case "down":
		if m.completeCursor < len(m.completeList)-1 {
			m.completeCursor++
		}
		return true
	case "tab":
		m.acceptCompletion()
		return true
	case "esc":
		m.clearCompletions()
		return true
	}
	return false
}

// renderCompletions draws the dropdown as up to maxRows lines, windowed
// around the cursor, fitted to cols. Returns "" when closed.
func (m pi9Model) renderCompletions(cols, maxRows int) string {
	if !m.completionsOpen() {
		return ""
	}
	if maxRows < 1 {
		maxRows = 1
	}
	if maxRows > 8 {
		maxRows = 8
	}
	n := len(m.completeList)
	start := 0
	if m.completeCursor >= maxRows {
		start = m.completeCursor - maxRows + 1
	}
	end := start + maxRows
	if end > n {
		end = n
	}
	var lines []string
	for i := start; i < end; i++ {
		c := m.completeList[i]
		prefix := "  "
		if m.completeKind == completeSlash {
			prefix = "  /"
		} else {
			prefix = "  @"
		}
		line := prefix + c.display
		if i == m.completeCursor {
			line = lipgloss.NewStyle().
				Background(lipgloss.Color("14")).
				Foreground(lipgloss.Color("0")).
				Render(fitRow(line, cols))
		} else {
			line = lipgloss.NewStyle().
				Foreground(lipgloss.Color("7")).
				Render(fitRow(line, cols))
		}
		lines = append(lines, line)
	}
	return strings.Join(lines, "\n")
}
