package main

// /settings overlay: a tiny picker, reusing the model/login overlay
// pattern. Lets the user set the thinking level, toggle auto-compaction,
// and (placeholder) pick a theme without leaving the TUI.
//
// Settings here are session-scoped model state — pi9's on-disk config is
// a flat key=value file, not JSON, and these knobs (thinking level,
// auto-compaction) are session preferences rather than credentials, so
// they live in pi9Model and reset to defaults on relaunch (thinking
// level is still settable at launch via -thinking).

import (
	"fmt"
	"strings"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

// settingsRowCount is the number of selectable rows in the overlay.
const settingsRowCount = 5

const (
	settingsRowThinking = iota
	settingsRowAutoCompact
	settingsRowSteering
	settingsRowFollowUp
	settingsRowTheme
)

// handleSettingsKey routes keys while the /settings overlay is open.
//
//	up/down (ctrl+p/ctrl+n)  move between rows
//	left/right / enter / space change the highlighted setting
//	esc / ctrl+c             close the overlay
func (m pi9Model) handleSettingsKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "esc", "ctrl+c", "q":
		m.settingsOpen = false
		m.statusMsg = ""
		return m, nil

	case "up", "ctrl+p", "k":
		if m.settingsCursor > 0 {
			m.settingsCursor--
		} else {
			m.settingsCursor = settingsRowCount - 1
		}
		return m, nil

	case "down", "ctrl+n", "j":
		if m.settingsCursor < settingsRowCount-1 {
			m.settingsCursor++
		} else {
			m.settingsCursor = 0
		}
		return m, nil

	case "enter", "ctrl+j", "ctrl+m", " ", "right", "l":
		return m.applySettingChange(+1)

	case "left", "h":
		return m.applySettingChange(-1)
	}
	return m, nil
}

// applySettingChange mutates the highlighted setting by `dir` (+1 next,
// -1 previous). Toggles are direction-insensitive.
func (m pi9Model) applySettingChange(dir int) (tea.Model, tea.Cmd) {
	switch m.settingsCursor {
	case settingsRowThinking:
		old := m.thinkingLevel
		if dir >= 0 {
			m.thinkingLevel = nextThinkingLevel(m.thinkingLevel)
		} else {
			m.thinkingLevel = prevThinkingLevel(m.thinkingLevel)
		}
		if m.tree != nil && m.thinkingLevel != old {
			m.tree.AppendThinkingChange(m.thinkingLevel)
			m.saveSession()
		}
		m.statusMsg = "thinking: " + m.thinkingLevel
	case settingsRowAutoCompact:
		m.autoCompact = !m.autoCompact
		m.statusMsg = "auto-compaction: " + onOff(m.autoCompact)
	case settingsRowSteering:
		// Toggle between the two modes (direction-insensitive; only two
		// values). Steering messages (Enter while running) are delivered at
		// the safe point between tool rounds.
		m.steeringMode = toggleDrainMode(m.steeringMode)
		m.statusMsg = "steering mode: " + m.steeringMode.String()
	case settingsRowFollowUp:
		m.followUpMode = toggleDrainMode(m.followUpMode)
		m.statusMsg = "follow-up mode: " + m.followUpMode.String()
	case settingsRowTheme:
		// Placeholder: pi9 only ships the Luna theme today.
		m.statusMsg = "theme: only \"luna\" available in this build"
	}
	return m, nil
}

// renderSettings draws the settings overlay, sized to (cols, rows). It
// fills the scrollback area like the model picker.
func (m pi9Model) renderSettings(cols, rows int) string {
	if rows < 5 {
		rows = 5
	}
	header := lipgloss.NewStyle().
		Background(lipgloss.Color("4")).
		Foreground(lipgloss.Color("15")).
		Render(fitRow(" /settings", cols))

	think := m.thinkingLevel
	if think == "" {
		think = "off"
	}
	labels := []string{
		fmt.Sprintf("thinking level      < %s >", think),
		fmt.Sprintf("auto-compaction     < %s >", onOff(m.autoCompact)),
		fmt.Sprintf("steering mode       < %s >", m.steeringMode.String()),
		fmt.Sprintf("follow-up mode      < %s >", m.followUpMode.String()),
		"theme               < luna >  (placeholder)",
	}

	out := []string{header, fitRow("", cols)}
	for i, l := range labels {
		line := "    " + l
		if i == m.settingsCursor {
			line = lipgloss.NewStyle().
				Background(lipgloss.Color("14")).
				Foreground(lipgloss.Color("0")).
				Render(fitRow("  > "+l, cols))
		} else {
			line = fitRow(line, cols)
		}
		out = append(out, line)
	}

	// Pad to fill, then a footer.
	for len(out) < rows-1 {
		out = append(out, fitRow("", cols))
	}
	footer := " up/down move - left/right/enter change - esc close"
	out = append(out, lipgloss.NewStyle().
		Foreground(lipgloss.Color("8")).
		Render(fitRow(footer, cols)))
	return strings.Join(out, "\n")
}

// onOff renders a bool as "on"/"off".
func onOff(b bool) string {
	if b {
		return "on"
	}
	return "off"
}

// prevThinkingLevel returns the level before cur in thinkingLevels,
// wrapping around. Mirror of nextThinkingLevel for the settings overlay.
func prevThinkingLevel(cur string) string {
	for i, l := range thinkingLevels {
		if l == cur {
			return thinkingLevels[(i-1+len(thinkingLevels))%len(thinkingLevels)]
		}
	}
	return thinkingLevels[0]
}

// hotkeysHelp lists the keyboard bindings (the /hotkeys command).
func hotkeysHelp() string {
	return `pi9 key bindings:

  editing
    enter            send the current message
    ctrl+u           clear the input line
    ctrl+a / home    jump to start of line
    ctrl+e / end     jump to end of line
    left / right     move cursor
    backspace        delete char before cursor

  steering (while a run is in flight)
    enter            queue a steering message (delivered between tool
                     rounds, mid-run)
    alt+enter        queue a follow-up message (delivered after the run
                     finishes)
    alt+up           pull the most recent queued message back to the editor
    esc              abort the run; queued messages return to the editor

  scrollback
    pgup / ctrl+b    page up
    pgdn / ctrl+f    page down
    shift+up/down    scroll one row
    ctrl+end         jump to latest (live tail)
    mouse wheel      scroll

  model / thinking
    shift+tab        cycle thinking level (off->minimal->...->xhigh->off)
    ctrl+t           toggle showing the model's thinking text
    ctrl+p           cycle active model forward through the scoped set
    shift+ctrl+p     cycle active model backward
                     (/scoped-models picks which models are in the set)

  editor completion
    /                open slash-command + prompt-template dropdown
    @path            fuzzy-complete a file under cwd
    !cmd / !!cmd     run a shell command (send / local-only)
    tab / enter      accept the highlighted completion
    esc              dismiss the completion dropdown

  session
    ctrl+c           cancel a stream / quit when idle
    ctrl+d           quit
    esc              cancel a stream / quit when idle

  overlays (model/login/settings pickers)
    up / down        navigate
    enter            select / apply
    esc              cancel / close`
}
