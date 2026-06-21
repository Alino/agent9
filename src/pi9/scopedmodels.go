package main

// Task 2.4 /scoped-models (pi parity).
//
// pi maintains an "enabled models" set: the subset of available models
// that Ctrl+P / Shift+Ctrl+P cycle through. --models seeds it from the
// CLI; /scoped-models opens an overlay to toggle which models are in the
// set. Each cycle records a model_change entry via the session tree (the
// same plumbing /model uses) so branch navigation sees the switch.
//
// This file owns the pure cycling/seeding logic (unit-tested) plus the
// scoped-models overlay (reusing the model-picker overlay pattern).

import (
	"strings"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/alino/plan9-winxp/pi9/internal/provider"
)

// cycleModel returns the model that comes after cur in the ordered
// enabled set, wrapping around. dir is +1 (forward) or -1 (backward).
// When the enabled set is empty, or cur is the only entry, cur is
// returned unchanged. When cur is not in the set, the first entry (for
// forward) or last entry (for backward) is returned so cycling always
// lands somewhere useful.
func cycleModel(enabled []string, cur string, dir int) string {
	if len(enabled) == 0 {
		return cur
	}
	idx := -1
	for i, m := range enabled {
		if m == cur {
			idx = i
			break
		}
	}
	if idx < 0 {
		if dir >= 0 {
			return enabled[0]
		}
		return enabled[len(enabled)-1]
	}
	n := len(enabled)
	next := (idx + dir%n + n) % n
	return enabled[next]
}

// seedEnabledModels parses a --models flag value (comma-separated
// patterns) into an ordered, de-duplicated list of concrete model IDs by
// matching each pattern against the curated+supplied catalog with
// provider.FuzzyMatch. Patterns are tried in order; within one pattern,
// catalog order is preserved. A pattern matching nothing is kept verbatim
// as a literal ID so an exact id the catalog doesn't know about still
// works. Empty/whitespace patterns are skipped. An empty flag yields nil.
func seedEnabledModels(flagVal string, catalog []provider.ModelInfo) []string {
	flagVal = strings.TrimSpace(flagVal)
	if flagVal == "" {
		return nil
	}
	seen := make(map[string]bool)
	var out []string
	add := func(id string) {
		if id == "" || seen[id] {
			return
		}
		seen[id] = true
		out = append(out, id)
	}
	for _, pat := range strings.Split(flagVal, ",") {
		pat = strings.TrimSpace(pat)
		if pat == "" {
			continue
		}
		matched := false
		for _, mi := range catalog {
			if provider.FuzzyMatch(pat, mi.ID) || provider.FuzzyMatch(pat, mi.Label) {
				add(mi.ID)
				matched = true
			}
		}
		if !matched {
			// Unknown pattern: keep it literally so an exact id the
			// catalog hasn't heard of is still cyclable.
			add(pat)
		}
	}
	return out
}

// toggleEnabled adds id to enabled if absent, removes it if present, and
// returns the new slice (preserving order; appends at the end when
// adding). Used by the /scoped-models overlay.
func toggleEnabled(enabled []string, id string) []string {
	for i, m := range enabled {
		if m == id {
			return append(append([]string{}, enabled[:i]...), enabled[i+1:]...)
		}
	}
	return append(append([]string{}, enabled...), id)
}

// applyModelCycle cycles the active model through the enabled set in the
// given direction, records a model_change entry, and sets a status line.
// A no-op (empty/singleton set, or unchanged model) leaves a hint instead.
func (m *pi9Model) applyModelCycle(dir int) {
	if len(m.enabledModels) == 0 {
		m.statusMsg = "no scoped models - run /scoped-models to enable some"
		return
	}
	old := m.model
	next := cycleModel(m.enabledModels, m.model, dir)
	if next == old {
		m.statusMsg = "model: " + m.model + " (only one scoped model)"
		return
	}
	m.model = next
	if m.tree != nil {
		m.tree.AppendModelChange(m.model)
		m.saveSession()
	}
	m.statusMsg = "model: " + old + " -> " + m.model
}

// ---------- /scoped-models overlay ----------
//
// Mirrors the model picker: a fuzzy search box at the top, a windowed
// list below, type to filter. Space/enter toggles whether the
// highlighted model is in the enabled (cycling) set; a leading [x]/[ ]
// shows membership.

// handleScopedModelsKey routes keys while the /scoped-models overlay is
// open.
func (m pi9Model) handleScopedModelsKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	filtered := m.scopedFiltered()

	switch msg.String() {
	case "esc", "ctrl+c":
		m.scopedModelsOpen = false
		m.statusMsg = ""
		return m, nil

	case "up", "ctrl+p":
		if m.scopedCursor > 0 {
			m.scopedCursor--
		}
		return m, nil

	case "down", "ctrl+n":
		if m.scopedCursor < len(filtered)-1 {
			m.scopedCursor++
		}
		return m, nil

	case "pgup":
		m.scopedCursor -= 5
		if m.scopedCursor < 0 {
			m.scopedCursor = 0
		}
		return m, nil

	case "pgdown":
		m.scopedCursor += 5
		if m.scopedCursor >= len(filtered) {
			m.scopedCursor = len(filtered) - 1
		}
		if m.scopedCursor < 0 {
			m.scopedCursor = 0
		}
		return m, nil

	case "enter", "ctrl+j", "ctrl+m", " ":
		if m.scopedCursor >= 0 && m.scopedCursor < len(filtered) {
			id := filtered[m.scopedCursor].ID
			m.enabledModels = toggleEnabled(m.enabledModels, id)
			m.statusMsg = scopedStatus(m.enabledModels)
		}
		return m, nil

	case "ctrl+a":
		// Enable all currently-visible (filtered) models.
		for _, mi := range filtered {
			if !containsStr(m.enabledModels, mi.ID) {
				m.enabledModels = append(m.enabledModels, mi.ID)
			}
		}
		m.statusMsg = scopedStatus(m.enabledModels)
		return m, nil

	case "ctrl+x":
		// Clear all currently-visible (filtered) models.
		for _, mi := range filtered {
			m.enabledModels = removeStr(m.enabledModels, mi.ID)
		}
		m.statusMsg = scopedStatus(m.enabledModels)
		return m, nil

	case "backspace", "ctrl+h":
		if m.scopedQueryCur > 0 {
			m.scopedQuery = append(m.scopedQuery[:m.scopedQueryCur-1], m.scopedQuery[m.scopedQueryCur:]...)
			m.scopedQueryCur--
			m.scopedCursor = 0
		}
		return m, nil

	case "ctrl+u":
		m.scopedQuery = m.scopedQuery[:0]
		m.scopedQueryCur = 0
		m.scopedCursor = 0
		return m, nil
	}

	for _, r := range msg.Runes {
		if r >= 0x20 && r != 0x7f {
			m.scopedQuery = append(m.scopedQuery, 0)
			copy(m.scopedQuery[m.scopedQueryCur+1:], m.scopedQuery[m.scopedQueryCur:])
			m.scopedQuery[m.scopedQueryCur] = r
			m.scopedQueryCur++
		}
	}
	m.scopedCursor = 0
	return m, nil
}

// scopedFiltered returns the catalog filtered by the search query.
func (m pi9Model) scopedFiltered() []provider.ModelInfo {
	q := string(m.scopedQuery)
	if q == "" {
		return m.modelList
	}
	out := make([]provider.ModelInfo, 0, len(m.modelList))
	for _, mi := range m.modelList {
		if provider.FuzzyMatch(q, mi.ID) || provider.FuzzyMatch(q, mi.Label) {
			out = append(out, mi)
		}
	}
	return out
}

// renderScopedModels draws the /scoped-models overlay.
func (m pi9Model) renderScopedModels(cols, rows int) string {
	filtered := m.scopedFiltered()
	if rows < 5 {
		rows = 5
	}

	query := string(m.scopedQuery)
	header := " /scoped-models: " + query + "_  [" +
		itoa(len(m.enabledModels)) + " enabled / " + itoa(len(m.modelList)) + " total]"
	headerStyled := lipgloss.NewStyle().
		Background(lipgloss.Color("4")).
		Foreground(lipgloss.Color("15")).
		Render(fitRow(header, cols))

	listRows := rows - 3
	if listRows < 1 {
		listRows = 1
	}
	start := 0
	if m.scopedCursor >= listRows {
		start = m.scopedCursor - listRows + 1
	}
	end := start + listRows
	if end > len(filtered) {
		end = len(filtered)
	}

	out := []string{headerStyled}
	for i := start; i < end; i++ {
		mi := filtered[i]
		box := "[ ]"
		if containsStr(m.enabledModels, mi.ID) {
			box = "[x]"
		}
		line := "  " + box + " " + mi.ID
		if i == m.scopedCursor {
			line = lipgloss.NewStyle().
				Background(lipgloss.Color("14")).
				Foreground(lipgloss.Color("0")).
				Render(fitRow(line, cols))
		} else {
			line = fitRow(line, cols)
		}
		out = append(out, line)
	}
	for len(out) < rows-1 {
		out = append(out, fitRow("", cols))
	}
	footer := " ^v nav - space/enter toggle - ctrl+a all - ctrl+x none - type filter - esc close"
	out = append(out, lipgloss.NewStyle().
		Foreground(lipgloss.Color("8")).
		Render(fitRow(footer, cols)))
	return strings.Join(out, "\n")
}

// scopedStatus summarizes the enabled set for the status line.
func scopedStatus(enabled []string) string {
	if len(enabled) == 0 {
		return "scoped models: none (Ctrl+P cycling disabled)"
	}
	return "scoped models: " + itoa(len(enabled)) + " enabled (Ctrl+P to cycle)"
}

// containsStr reports whether s contains v.
func containsStr(s []string, v string) bool {
	for _, x := range s {
		if x == v {
			return true
		}
	}
	return false
}

// removeStr returns s with the first occurrence of v removed.
func removeStr(s []string, v string) []string {
	for i, x := range s {
		if x == v {
			return append(append([]string{}, s[:i]...), s[i+1:]...)
		}
	}
	return s
}

// itoa is a tiny int->string helper to avoid importing strconv in this
// render-heavy file alongside the fmt-free style used elsewhere here.
func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	var buf [20]byte
	i := len(buf)
	for n > 0 {
		i--
		buf[i] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		i--
		buf[i] = '-'
	}
	return string(buf[i:])
}
