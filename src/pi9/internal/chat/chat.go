// Package chat owns the in-memory conversation state and rendering.
//
// Phase 3 additions:
//   - Turn.Calls : list of tool invocations executed during this turn
//   - render tool blocks (collapsed by default: "▸ name(args) [duration] N bytes")
//
// Kept separate from main.go so we can swap UI experiments without
// touching the agent logic.
package chat

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"strings"
	"time"

	"github.com/charmbracelet/lipgloss"

	"github.com/alino/plan9-winxp/pi9/internal/provider"
)

// newTurnID returns a fresh, stable per-turn identifier. It is assigned
// once when a turn is created (AppendUser/AppendLocal) and never changes as
// the turn's content streams in, which is what lets SessionTree.SyncFromHistory
// match a growing in-flight turn to its existing entry (and update it in
// place) instead of appending a duplicate every save.
func newTurnID() string {
	var b [8]byte
	if _, err := rand.Read(b[:]); err != nil {
		return fmt.Sprintf("t%x", time.Now().UnixNano())
	}
	return "t" + hex.EncodeToString(b[:])
}

// ToolInvocation is one tool call + its result, rendered inline in
// the assistant turn.
type ToolInvocation struct {
	ID       string    `json:"id"`
	Name     string    `json:"name"`
	Args     string    `json:"args"` // raw JSON
	Output   string    `json:"output"`
	Err      error     `json:"-"` // not serializable; ErrText carries it
	ErrText  string    `json:"err,omitempty"`
	Started  time.Time `json:"started"`
	Finished time.Time `json:"finished"`
}

// Turn is one display-level entry in the conversation history.
// One Turn per user→assistant exchange. Streaming assistant tokens
// append to Turn.Assistant. Tool calls (executed between sub-stream
// rounds) accumulate in Turn.Calls.
//
// Local turns (slash commands like /help, /memory) are persisted +
// rendered like any other turn, but excluded from ToProviderMessages
// — the LLM doesn't see them.
type Turn struct {
	ID        string           `json:"id,omitempty"` // stable identity (see newTurnID); set at creation
	User      string           `json:"user"`
	Assistant string           `json:"assistant"`
	Reasoning string           `json:"reasoning,omitempty"` // streamed thinking text, if any
	Calls     []ToolInvocation `json:"calls,omitempty"`
	Started   time.Time        `json:"started"`
	Finished  time.Time        `json:"finished"`
	Err       error            `json:"-"`
	ErrText   string           `json:"err,omitempty"`
	Local     bool             `json:"local,omitempty"`
	Synthetic bool             `json:"synthetic,omitempty"` // injected by Materialize (compaction/branch summary); skipped by SyncFromHistory
}

// History is the ordered list of turns plus the system prompt.
type History struct {
	System string `json:"system"`
	Name   string `json:"name,omitempty"` // user-set display name (/name slash command)
	Turns  []Turn `json:"turns"`
}

// AppendUser begins a new turn from a user message. The assistant
// portion starts empty and is filled by stream chunks.
func (h *History) AppendUser(msg string) {
	h.Turns = append(h.Turns, Turn{ID: newTurnID(), User: msg, Started: time.Now()})
}

// AppendLocal begins a local (slash-command) turn. The user line
// shows what they typed; the assistant string is set immediately and
// FinishTurn marks it complete in one call.
func (h *History) AppendLocal(user, response string) {
	now := time.Now()
	h.Turns = append(h.Turns, Turn{
		ID:        newTurnID(),
		User:      user,
		Assistant: response,
		Local:     true,
		Started:   now,
		Finished:  now,
	})
}

// AppendDelta appends streamed bytes to the current (last) turn's
// assistant message. Must be called only after AppendUser.
func (h *History) AppendDelta(delta string) {
	if len(h.Turns) == 0 {
		return
	}
	h.Turns[len(h.Turns)-1].Assistant += delta
}

// AppendReasoning appends streamed thinking/reasoning bytes to the
// current (last) turn. Rendered (dimmed) before the assistant message;
// the caller decides whether to display it. Must be called only after
// AppendUser.
func (h *History) AppendReasoning(delta string) {
	if len(h.Turns) == 0 {
		return
	}
	h.Turns[len(h.Turns)-1].Reasoning += delta
}

// BeginCall registers a tool invocation as in-flight. Returns the
// index inside the current Turn.Calls slice so the caller can update
// it when execution finishes.
func (h *History) BeginCall(id, name, args string) int {
	if len(h.Turns) == 0 {
		return -1
	}
	t := &h.Turns[len(h.Turns)-1]
	t.Calls = append(t.Calls, ToolInvocation{
		ID:      id,
		Name:    name,
		Args:    args,
		Started: time.Now(),
	})
	return len(t.Calls) - 1
}

// FinishCall records output and elapsed time for a previously-begun
// call. idx is the value returned by BeginCall.
func (h *History) FinishCall(idx int, output string, err error) {
	if len(h.Turns) == 0 || idx < 0 {
		return
	}
	t := &h.Turns[len(h.Turns)-1]
	if idx >= len(t.Calls) {
		return
	}
	c := &t.Calls[idx]
	c.Output = output
	c.Err = err
	if err != nil {
		c.ErrText = err.Error()
	}
	c.Finished = time.Now()
}

// FinishTurn marks the last turn complete. Idempotent.
func (h *History) FinishTurn(err error) {
	if len(h.Turns) == 0 {
		return
	}
	t := &h.Turns[len(h.Turns)-1]
	if t.Finished.IsZero() {
		t.Finished = time.Now()
	}
	if err != nil {
		t.Err = err
		t.ErrText = err.Error()
	}
}

// RestoreErrs rehydrates the Err interfaces from ErrText fields after
// JSON unmarshal. Sessions stored to disk only carry the text.
func (h *History) RestoreErrs() {
	for i := range h.Turns {
		t := &h.Turns[i]
		if t.ErrText != "" && t.Err == nil {
			t.Err = fmt.Errorf("%s", t.ErrText)
		}
		for j := range t.Calls {
			c := &t.Calls[j]
			if c.ErrText != "" && c.Err == nil {
				c.Err = fmt.Errorf("%s", c.ErrText)
			}
		}
	}
}

// ToProviderMessages converts the history into the OpenAI-compatible
// shape used by provider.StreamRequest. The system prompt is always
// the first message. Local turns (slash commands) are skipped.
// For tool calls, we emit an assistant message with tool_calls
// followed by a tool message per result, so the model has full
// context on what it asked for and what came back.
func (h *History) ToProviderMessages() []provider.Message {
	out := []provider.Message{{Role: provider.RoleSystem, Content: h.System}}
	for _, t := range h.Turns {
		if t.Local {
			continue
		}
		out = append(out, provider.Message{Role: provider.RoleUser, Content: t.User})

		// If this turn had tool calls, emit:
		//   assistant(content, tool_calls)
		//   tool(call_id_1, name_1, output_1)
		//   tool(call_id_2, name_2, output_2)
		//   ...
		// Only finished calls are emitted (in-flight calls aren't
		// part of the historical message stream yet).
		var finishedCalls []provider.ToolCall
		for _, c := range t.Calls {
			if c.Finished.IsZero() {
				continue
			}
			finishedCalls = append(finishedCalls, provider.ToolCall{
				ID:   c.ID,
				Type: "function",
				Function: provider.ToolCallFn{
					Name:      c.Name,
					Arguments: c.Args,
				},
			})
		}
		if len(finishedCalls) > 0 || t.Assistant != "" {
			out = append(out, provider.Message{
				Role:      provider.RoleAssistant,
				Content:   t.Assistant,
				ToolCalls: finishedCalls,
			})
		}
		for _, c := range t.Calls {
			if c.Finished.IsZero() {
				continue
			}
			body := c.Output
			if c.Err != nil {
				body = "ERROR: " + c.Err.Error() + "\n" + body
			}
			out = append(out, provider.Message{
				Role:       provider.RoleTool,
				ToolCallID: c.ID,
				Name:       c.Name,
				Content:    body,
			})
		}
	}
	return out
}

// Styles for rendering Turn content. Contemporary dark palette (the
// 16 indices are remapped in vtwin; semantics here are unchanged).
var (
	userStyle = lipgloss.NewStyle().
			Bold(true).
			Foreground(lipgloss.Color("11")) // bright yellow

	asstStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("7")) // white

	asstLabel = lipgloss.NewStyle().
			Bold(true).
			Foreground(lipgloss.Color("12")) // bright blue (Luna)

	toolLabel = lipgloss.NewStyle().
			Foreground(lipgloss.Color("14")) // bright cyan

	toolMeta = lipgloss.NewStyle().
			Foreground(lipgloss.Color("8")). // dim
			Italic(true)

	errStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("9")) // bright red

	hintStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("8")). // dim
			Italic(true)

	// thinkingStyle renders streamed reasoning text — dimmed + italic so
	// it reads as secondary to the assistant's actual reply.
	thinkingStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("8")). // dim
			Italic(true)

	// Local turns (slash commands) use magenta to visually distinguish
	// them from model exchanges.
	localUserStyle = lipgloss.NewStyle().
			Bold(true).
			Foreground(lipgloss.Color("13")) // bright magenta

	localBodyStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("7")) // white

	// Lightweight markdown styles for assistant prose.
	mdHeading = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("12")) // bright blue
	mdCodeBlk = lipgloss.NewStyle().Foreground(lipgloss.Color("10"))            // green
	mdGutter  = lipgloss.NewStyle().Foreground(lipgloss.Color("8"))             // dim │ rail
	mdBullet  = lipgloss.NewStyle().Foreground(lipgloss.Color("6"))             // cyan •
	mdQuote   = lipgloss.NewStyle().Foreground(lipgloss.Color("8")).Italic(true)
)

// renderMarkdown renders a lightweight block-level markdown subset
// (headings, fenced code blocks, bullet/numbered lists, blockquotes)
// to styled, wrapped lines. Inline spans are left as-is — the visible
// win is structural (code blocks especially). Returns un-indented
// lines; the caller indents the whole block under its bar label.
func renderMarkdown(body string, width int) string {
	var out []string
	inCode := false
	for _, ln := range strings.Split(body, "\n") {
		t := strings.TrimSpace(ln)
		if strings.HasPrefix(t, "```") {
			inCode = !inCode
			continue // drop fence lines; the gutter rail marks the block
		}
		if inCode {
			out = append(out, mdGutter.Render("│ ")+mdCodeBlk.Render(ln))
			continue
		}
		// ATX heading: 1-6 leading '#' then a space.
		hn := 0
		for hn < len(t) && t[hn] == '#' {
			hn++
		}
		if hn >= 1 && hn <= 6 && hn < len(t) && t[hn] == ' ' {
			out = append(out, mdHeading.Render(strings.TrimSpace(t[hn:])))
			continue
		}
		// Bullet list: -, *, + followed by a space.
		if len(t) > 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ' {
			wrapped := strings.Split(wrap(t[2:], width-2), "\n")
			for i, w := range wrapped {
				if i == 0 {
					out = append(out, mdBullet.Render("• ")+asstStyle.Render(w))
				} else {
					out = append(out, "  "+asstStyle.Render(w))
				}
			}
			continue
		}
		// Blockquote.
		if strings.HasPrefix(t, "> ") {
			out = append(out, mdGutter.Render("│ ")+mdQuote.Render(t[2:]))
			continue
		}
		if t == "" {
			out = append(out, "")
			continue
		}
		for _, w := range strings.Split(wrap(ln, width), "\n") {
			out = append(out, asstStyle.Render(w))
		}
	}
	return strings.Join(out, "\n")
}

// renderCall formats one tool invocation. Collapsed-by-default:
//
//	▸ run_rc({"command":"ls /tmp"})  [42ms] 73 bytes
//
// Errors render the error text in red.
//
// vtwin's Terminus font has full box-drawing/arrow/marker coverage, so
// we use real Unicode glyphs (see Tier-0 capability check).
func renderCall(c ToolInvocation, width int) string {
	prefix := toolLabel.Render("  ▸ ")
	args := compactArgs(c.Args)
	head := fmt.Sprintf("%s(%s)", c.Name, args)
	// Trim if it overflows.
	if len(head) > width-8 {
		head = head[:width-11] + "...)"
	}

	var meta string
	if c.Finished.IsZero() {
		meta = toolMeta.Render(" running...")
	} else {
		dur := c.Finished.Sub(c.Started).Round(time.Millisecond)
		if c.Err != nil {
			meta = errStyle.Render(fmt.Sprintf(" [%s] %s", dur, c.Err.Error()))
		} else {
			meta = toolMeta.Render(fmt.Sprintf("  [%s] %d bytes", dur, len(c.Output)))
		}
	}
	return prefix + toolLabel.Render(head) + meta
}

// compactArgs trims whitespace and clips long JSON args for display.
func compactArgs(args string) string {
	args = strings.TrimSpace(args)
	args = strings.ReplaceAll(args, "\n", " ")
	if len(args) > 60 {
		args = args[:57] + "..."
	}
	return args
}

// RenderTurn formats one turn as a multi-line string for the
// scrollback viewport. Width is the wrap width. When hideThinking is
// true, any streamed reasoning text is omitted from the render.
func RenderTurn(t Turn, width int, hideThinking bool) string {
	if t.Local {
		return renderLocalTurn(t, width)
	}
	var b strings.Builder
	cw := width - 2 // content is indented two columns under its bar label

	// Each speaker gets a colored left-bar label ("▌ you" / "▌ pi9") with
	// its message indented beneath — a clearer block structure than the
	// old inline "you:" / "pi9:" prefixes.
	b.WriteString(userStyle.Render("▌ you"))
	b.WriteString("\n")
	b.WriteString(indent(wrap(t.User, cw), 2))
	b.WriteString("\n\n")

	// Streamed reasoning renders before the reply, dimmed + labelled.
	if !hideThinking {
		if reasoning := strings.TrimSpace(t.Reasoning); reasoning != "" {
			b.WriteString(thinkingStyle.Render("▌ thinking"))
			b.WriteString("\n")
			b.WriteString(indent(thinkingStyle.Render(wrap(reasoning, cw)), 2))
			b.WriteString("\n\n")
		}
	}

	b.WriteString(asstLabel.Render("▌ pi9"))
	b.WriteString("\n")
	if t.Err != nil {
		b.WriteString(indent(errStyle.Render("error: "+t.Err.Error()), 2))
	} else {
		body := t.Assistant
		if body == "" && t.Finished.IsZero() && len(t.Calls) == 0 {
			body = "..."
		}
		if body != "" {
			b.WriteString(indent(renderMarkdown(body, cw), 2))
		}
	}
	for _, c := range t.Calls {
		b.WriteString("\n")
		b.WriteString(renderCall(c, width))
	}
	if !t.Finished.IsZero() {
		dur := t.Finished.Sub(t.Started).Round(time.Millisecond)
		b.WriteString("\n")
		b.WriteString(hintStyle.Render(fmt.Sprintf("  %s", dur)))
	}
	b.WriteString("\n")
	return b.String()
}

// indent prefixes every line of s with n spaces. Used to set message
// bodies in under their bar labels.
func indent(s string, n int) string {
	pad := strings.Repeat(" ", n)
	lines := strings.Split(s, "\n")
	for i := range lines {
		lines[i] = pad + lines[i]
	}
	return strings.Join(lines, "\n")
}

// renderLocalTurn formats a slash-command turn. Compact: user line in
// magenta, body indented two spaces, no "pi9:" label or timing.
func renderLocalTurn(t Turn, width int) string {
	var b strings.Builder
	b.WriteString(localUserStyle.Render("▌ " + t.User))
	b.WriteString("\n")
	for _, line := range strings.Split(t.Assistant, "\n") {
		b.WriteString("  ")
		b.WriteString(localBodyStyle.Render(wrap(line, width-2)))
		b.WriteString("\n")
	}
	return b.String()
}

// wrap re-wraps text to the given width on word boundaries, preserving
// existing newlines from the streamed content (so model-emitted
// paragraphs survive).
func wrap(text string, width int) string {
	if width < 10 {
		return text
	}
	var out strings.Builder
	for i, line := range strings.Split(text, "\n") {
		if i > 0 {
			out.WriteByte('\n')
		}
		out.WriteString(wrapLine(line, width))
	}
	return out.String()
}

func wrapLine(line string, width int) string {
	if len(line) <= width {
		return line
	}
	var out strings.Builder
	words := strings.Fields(line)
	col := 0
	for i, w := range words {
		if i > 0 {
			if col+1+len(w) > width {
				out.WriteByte('\n')
				col = 0
			} else {
				out.WriteByte(' ')
				col++
			}
		}
		out.WriteString(w)
		col += len(w)
	}
	return out.String()
}

// Render is the full scrollback as one string. When hideThinking is
// true, streamed reasoning text is omitted from every turn.
func Render(h *History, width int, hideThinking bool) string {
	var b strings.Builder
	for _, t := range h.Turns {
		b.WriteString(RenderTurn(t, width, hideThinking))
		b.WriteString("\n")
	}
	return b.String()
}
