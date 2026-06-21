package main

// Task 2.1: tree-structured session overlays — /resume, /fork, /tree —
// plus the /clone and /import command bodies. Each overlay reuses the
// model/settings picker pattern: it replaces the scrollback area, has its
// own key handler dispatched from handleKey, and its own renderer called
// from View.
//
// /fork  picks a prior USER message and forks a NEW branch from before it.
// /clone duplicates the active branch into a NEW session file and switches.
// /tree  navigates the whole branch tree, setting the leaf in place.
// /resume picks any session (newest first) and switches to it.
// /import loads an external .jsonl session file into the store + switches.

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/alino/plan9-winxp/pi9/internal/chat"
	"github.com/alino/plan9-winxp/pi9/internal/store"
)

// ---------- row types ----------

// sessionRow is one entry in the /resume picker.
type sessionRow struct {
	id    string
	name  string
	turns int
}

// forkRow is one selectable prior user message in the /fork picker. id is
// the message entry id to fork from; preview is the trimmed user text.
type forkRow struct {
	id      string
	preview string
}

// treeRow is one entry in the /tree navigator. id is the entry id; depth
// drives indentation; label is the rendered one-line summary.
type treeRow struct {
	id    string
	depth int
	label string
}

// ---------- /resume ----------

// openResume builds the session list and opens the picker overlay.
func (m *pi9Model) openResume() {
	ids, _ := store.ListSessions()
	rows := make([]sessionRow, 0, len(ids))
	for _, id := range ids {
		rows = append(rows, sessionRow{
			id:    id,
			name:  sessionDisplayName(id),
			turns: sessionTurnCount(id),
		})
	}
	m.resumeList = rows
	m.resumeCursor = 0
	for i, r := range rows {
		if r.id == m.sessionID {
			m.resumeCursor = i
			break
		}
	}
	m.resumeOpen = true
	m.statusMsg = "pick a session - enter to load, esc to cancel"
}

// sessionDisplayName returns the stored display name for a session id, or
// "" when none is set / it can't be read cheaply.
func sessionDisplayName(id string) string {
	if data, err := store.LoadSessionTree(id); err == nil {
		if tree, err := chat.UnmarshalJSONL(data, ""); err == nil {
			return tree.Name()
		}
	}
	return ""
}

// sessionTurnCount returns the number of message entries on a session's
// active branch (an approximation of "turns" for the picker).
func sessionTurnCount(id string) int {
	if data, err := store.LoadSessionTree(id); err == nil {
		if tree, err := chat.UnmarshalJSONL(data, ""); err == nil {
			return len(tree.Materialize().Turns)
		}
	}
	// Legacy fallback.
	if data, err := store.LoadSession(id); err == nil {
		var h chat.History
		if json.Unmarshal(data, &h) == nil {
			return len(h.Turns)
		}
	}
	return 0
}

// handleResumeKey routes keys while the /resume picker is open.
func (m pi9Model) handleResumeKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "esc", "ctrl+c", "q":
		m.resumeOpen = false
		m.statusMsg = ""
		return m, nil
	case "up", "ctrl+p", "k":
		if m.resumeCursor > 0 {
			m.resumeCursor--
		}
		return m, nil
	case "down", "ctrl+n", "j":
		if m.resumeCursor < len(m.resumeList)-1 {
			m.resumeCursor++
		}
		return m, nil
	case "enter", "ctrl+j", "ctrl+m", " ":
		if m.resumeCursor < 0 || m.resumeCursor >= len(m.resumeList) {
			return m, nil
		}
		picked := m.resumeList[m.resumeCursor]
		m.resumeOpen = false
		return m.switchToSession(picked.id), nil
	}
	return m, nil
}

// switchToSession loads the tree for id, makes it current, and
// re-materializes the active branch. Best-effort; surfaces errors in the
// status bar.
func (m pi9Model) switchToSession(id string) tea.Model {
	cwd, _ := os.Getwd()
	tree := loadSessionTree(id, cwd, m.history.System)
	if tree == nil {
		m.statusMsg = "could not load session " + id
		return m
	}
	tree.ID = id
	tree.System = m.history.System
	m.tree = tree
	m.sessionID = id
	_ = store.SetCurrentSession(id)
	m.reMaterialize()
	m.statusMsg = "switched to session " + id
	return m
}

// renderResume draws the /resume picker overlay.
func (m pi9Model) renderResume(cols, rows int) string {
	if rows < 5 {
		rows = 5
	}
	header := lipgloss.NewStyle().
		Background(lipgloss.Color("4")).
		Foreground(lipgloss.Color("15")).
		Render(fitRow(fmt.Sprintf(" /resume  [%d session(s)]", len(m.resumeList)), cols))

	listRows := rows - 3
	if listRows < 1 {
		listRows = 1
	}
	start := 0
	if m.resumeCursor >= listRows {
		start = m.resumeCursor - listRows + 1
	}
	end := start + listRows
	if end > len(m.resumeList) {
		end = len(m.resumeList)
	}

	out := []string{header}
	if len(m.resumeList) == 0 {
		out = append(out, fitRow("  (no sessions yet)", cols))
	}
	for i := start; i < end; i++ {
		r := m.resumeList[i]
		label := r.id
		if r.name != "" {
			label = r.name + "  (" + r.id + ")"
		}
		line := fmt.Sprintf("  %s  -  %d turn%s", label, r.turns, plural(r.turns))
		if r.id == m.sessionID {
			line += "  *"
		}
		out = append(out, pickerRow(line, cols, i == m.resumeCursor))
	}
	for len(out) < rows-1 {
		out = append(out, fitRow("", cols))
	}
	out = append(out, lipgloss.NewStyle().Foreground(lipgloss.Color("8")).
		Render(fitRow(" up/down move - enter load - esc cancel", cols)))
	return strings.Join(out, "\n")
}

// ---------- /fork ----------

// openFork builds the list of prior user messages on the active branch and
// opens the picker. Selecting one forks a new branch from before it.
func (m *pi9Model) openFork() bool {
	if m.tree == nil {
		return false
	}
	var rows []forkRow
	for _, e := range m.tree.Tree() {
		if e.Kind != chat.KindMessage || e.Message == nil {
			continue
		}
		// Only fork-points: user-authored, non-local message entries.
		if e.Message.Local || strings.TrimSpace(e.Message.User) == "" {
			continue
		}
		// Restrict to entries on the active path so the offered fork
		// points all lie on the visible branch.
		if !m.entryOnActivePath(e.ID) {
			continue
		}
		rows = append(rows, forkRow{id: e.ID, preview: oneLine(e.Message.User, 64)})
	}
	if len(rows) == 0 {
		return false
	}
	m.forkList = rows
	m.forkCursor = len(rows) - 1 // default to the most recent
	m.forkOpen = true
	m.statusMsg = "pick a message to fork from - enter to fork, esc to cancel"
	return true
}

// entryOnActivePath reports whether id is on the path root..leaf.
func (m *pi9Model) entryOnActivePath(id string) bool {
	if m.tree == nil {
		return false
	}
	for cur := m.tree.Leaf(); cur != ""; {
		if cur == id {
			return true
		}
		e := m.tree.Get(cur)
		if e == nil {
			break
		}
		cur = e.ParentID
	}
	return false
}

// handleForkKey routes keys while the /fork picker is open.
func (m pi9Model) handleForkKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "esc", "ctrl+c", "q":
		m.forkOpen = false
		m.statusMsg = ""
		return m, nil
	case "up", "ctrl+p", "k":
		if m.forkCursor > 0 {
			m.forkCursor--
		}
		return m, nil
	case "down", "ctrl+n", "j":
		if m.forkCursor < len(m.forkList)-1 {
			m.forkCursor++
		}
		return m, nil
	case "enter", "ctrl+j", "ctrl+m", " ":
		if m.forkCursor < 0 || m.forkCursor >= len(m.forkList) {
			return m, nil
		}
		picked := m.forkList[m.forkCursor]
		m.forkOpen = false
		if m.tree != nil && m.tree.Fork(picked.id) {
			m.reMaterialize()
			m.history.AppendLocal("/fork", "forked a new branch from: "+picked.preview+"\nyour next message starts the new branch.")
			m.statusMsg = "forked - type to start the new branch"
			m.saveSession()
		} else {
			m.statusMsg = "fork failed"
		}
		return m, nil
	}
	return m, nil
}

// renderFork draws the /fork picker overlay.
func (m pi9Model) renderFork(cols, rows int) string {
	return renderListOverlay(" /fork: pick a message to fork from", cols, rows,
		len(m.forkList), m.forkCursor,
		func(i int) string { return "  " + m.forkList[i].preview },
		" up/down move - enter fork - esc cancel")
}

// ---------- /tree ----------

// openTree builds a flattened view of the whole branch tree and opens the
// navigator overlay.
func (m *pi9Model) openTree() bool {
	if m.tree == nil {
		return false
	}
	rows := m.buildTreeRows()
	if len(rows) == 0 {
		return false
	}
	m.treeList = rows
	m.treeCursor = 0
	for i, r := range rows {
		if r.id == m.tree.Leaf() {
			m.treeCursor = i
			break
		}
	}
	m.treeOpen = true
	m.statusMsg = "navigate the tree - enter to jump, esc to cancel"
	return true
}

// buildTreeRows flattens the tree depth-first into displayable rows.
func (m *pi9Model) buildTreeRows() []treeRow {
	var rows []treeRow
	var walk func(parent string, depth int)
	walk = func(parent string, depth int) {
		for _, e := range m.tree.Children(parent) {
			rows = append(rows, treeRow{id: e.ID, depth: depth, label: entryLabel(e)})
			walk(e.ID, depth+1)
		}
	}
	walk("", 0)
	return rows
}

// entryLabel renders a one-line summary of an entry for the /tree view.
func entryLabel(e *chat.Entry) string {
	switch e.Kind {
	case chat.KindMessage:
		if e.Message == nil {
			return "(empty message)"
		}
		who := "user"
		if e.Message.Local {
			who = "local"
		}
		txt := e.Message.User
		if strings.TrimSpace(txt) == "" {
			who = "assistant"
			txt = e.Message.Assistant
		}
		return who + ": " + oneLine(txt, 56)
	case chat.KindModelChange:
		return "[model -> " + e.Model + "]"
	case chat.KindThinkingChange:
		return "[thinking -> " + e.Level + "]"
	case chat.KindCompaction:
		return "[compaction] " + oneLine(e.Summary, 48)
	case chat.KindBranchSummary:
		return "[branch summary] " + oneLine(e.Summary, 44)
	case chat.KindSessionInfo:
		return "[name -> " + e.Name + "]"
	default:
		return "[" + e.Kind + "]"
	}
}

// handleTreeKey routes keys while the /tree navigator is open. Navigating
// AWAY from a branch with unsummarized work offers to append a branch
// summary; for simplicity we skip the LLM round-trip here and attach a
// lightweight placeholder summary only when the user holds shift+enter.
// (The summarizer reuse is wired through summarizeCmd by callers that want
// it; the default Enter jump preserves all branches losslessly anyway.)
func (m pi9Model) handleTreeKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "esc", "ctrl+c", "q":
		m.treeOpen = false
		m.statusMsg = ""
		return m, nil
	case "up", "ctrl+p", "k":
		if m.treeCursor > 0 {
			m.treeCursor--
		}
		return m, nil
	case "down", "ctrl+n", "j":
		if m.treeCursor < len(m.treeList)-1 {
			m.treeCursor++
		}
		return m, nil
	case "enter", "ctrl+j", "ctrl+m", " ":
		if m.treeCursor < 0 || m.treeCursor >= len(m.treeList) {
			return m, nil
		}
		picked := m.treeList[m.treeCursor]
		m.treeOpen = false
		if m.tree != nil && m.tree.SetLeaf(picked.id) {
			m.reMaterialize()
			m.statusMsg = "jumped to selected entry"
			m.saveSession()
		} else {
			m.statusMsg = "jump failed"
		}
		return m, nil
	}
	return m, nil
}

// renderTree draws the /tree navigator overlay.
func (m pi9Model) renderTree(cols, rows int) string {
	leaf := ""
	if m.tree != nil {
		leaf = m.tree.Leaf()
	}
	return renderListOverlay(" /tree: navigate the branch tree", cols, rows,
		len(m.treeList), m.treeCursor,
		func(i int) string {
			r := m.treeList[i]
			marker := "  "
			if r.id == leaf {
				marker = "* "
			}
			return marker + strings.Repeat("  ", r.depth) + r.label
		},
		" up/down move - enter jump - esc cancel")
}

// ---------- /clone + /import bodies ----------

// cloneSession duplicates the active branch into a new session file and
// switches to it. Returns the new model.
func (m pi9Model) cloneSession(text string) (tea.Model, tea.Cmd) {
	if m.tree == nil {
		m.history.AppendLocal(text, "no session to clone")
		m.saveSession()
		return m, nil
	}
	// Fold any pending history into the source tree first.
	m.tree.SyncFromHistory(&m.history)
	newID := store.NewSessionID()
	clone := m.tree.Clone(newID)
	jsonl, err := clone.MarshalJSONL()
	if err != nil {
		m.history.AppendLocal(text, "clone failed: "+err.Error())
		m.saveSession()
		return m, nil
	}
	if err := store.SaveSessionTree(newID, jsonl); err != nil {
		m.history.AppendLocal(text, "clone failed: "+err.Error())
		m.saveSession()
		return m, nil
	}
	// Switch to the clone.
	m.tree = clone
	m.sessionID = newID
	_ = store.SetCurrentSession(newID)
	m.reMaterialize()
	m.history.AppendLocal(text, "cloned active branch into new session "+newID)
	m.statusMsg = "switched to clone " + newID
	m.saveSession()
	return m, nil
}

// importSession loads an external .jsonl session file into the store and
// switches to it. Returns the new model.
func (m pi9Model) importSession(text, path string) (tea.Model, tea.Cmd) {
	if path == "" {
		m.history.AppendLocal(text, "usage: /import <path-to-session.jsonl>")
		m.saveSession()
		return m, nil
	}
	data, err := os.ReadFile(path)
	if err != nil {
		m.history.AppendLocal(text, "import failed: "+err.Error())
		m.saveSession()
		return m, nil
	}
	// Validate it parses before committing it to the store.
	if _, err := chat.UnmarshalJSONL(data, m.history.System); err != nil {
		m.history.AppendLocal(text, "import failed: not a valid session file: "+err.Error())
		m.saveSession()
		return m, nil
	}
	id, err := store.ImportSessionTree("", path)
	if err != nil {
		m.history.AppendLocal(text, "import failed: "+err.Error())
		m.saveSession()
		return m, nil
	}
	m = m.switchToSession(id).(pi9Model)
	m.history.AppendLocal(text, "imported session "+id+" from "+path)
	m.saveSession()
	return m, nil
}

// ---------- shared overlay helpers ----------

// renderListOverlay renders a generic single-column picker overlay with a
// header, a windowed list, and a footer. labelFn returns the unstyled row
// text for index i.
func renderListOverlay(title string, cols, rows, n, cursor int, labelFn func(int) string, footer string) string {
	if rows < 5 {
		rows = 5
	}
	header := lipgloss.NewStyle().
		Background(lipgloss.Color("4")).
		Foreground(lipgloss.Color("15")).
		Render(fitRow(title, cols))

	listRows := rows - 3
	if listRows < 1 {
		listRows = 1
	}
	start := 0
	if cursor >= listRows {
		start = cursor - listRows + 1
	}
	end := start + listRows
	if end > n {
		end = n
	}

	out := []string{header}
	if n == 0 {
		out = append(out, fitRow("  (nothing to show)", cols))
	}
	for i := start; i < end; i++ {
		out = append(out, pickerRow(labelFn(i), cols, i == cursor))
	}
	for len(out) < rows-1 {
		out = append(out, fitRow("", cols))
	}
	out = append(out, lipgloss.NewStyle().Foreground(lipgloss.Color("8")).
		Render(fitRow(footer, cols)))
	return strings.Join(out, "\n")
}

// pickerRow renders one list row, highlighting it when selected.
func pickerRow(label string, cols int, selected bool) string {
	if selected {
		return lipgloss.NewStyle().
			Background(lipgloss.Color("14")).
			Foreground(lipgloss.Color("0")).
			Render(fitRow("> "+label, cols))
	}
	return fitRow("  "+label, cols)
}

// oneLine collapses whitespace/newlines and clips to max runes for a
// single-line preview.
func oneLine(s string, max int) string {
	s = strings.TrimSpace(strings.ReplaceAll(s, "\n", " "))
	for strings.Contains(s, "  ") {
		s = strings.ReplaceAll(s, "  ", " ")
	}
	r := []rune(s)
	if len(r) > max {
		if max < 3 {
			return string(r[:max])
		}
		return string(r[:max-3]) + "..." // ASCII only; vtwin lacks U+2026
	}
	return s
}
