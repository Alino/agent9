package main

// Compaction: summarize older turns to free up the context window.
//
// This mirrors pi.dev's compaction (see docs/compaction.md). When the
// estimated context tokens approach the active model's context window,
// we summarize the oldest turns into ONE synthetic assistant turn and
// drop the originals, keeping the most recent ~keepRecentTokens worth
// of turns verbatim.
//
// Token estimation here is byte-based (≈4 bytes/token), the same rough
// heuristic pi uses when the provider doesn't report usage. When the
// provider DOES report usage (Chunk.Usage on the terminal chunk), the
// real total is used for the "NN% ctx" indicator instead.
//
// The summary itself is produced by the current provider via the same
// streaming path used for normal turns (a one-shot variant —
// summarizeCmd). The structured format (Goal / Constraints / Progress /
// Key Decisions / Next Steps / Critical Context + read/modified files)
// is lifted verbatim from pi's SUMMARIZATION_PROMPT.

import (
	"context"
	"encoding/json"
	"fmt"
	"sort"
	"strings"
	"time"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/alino/plan9-winxp/pi9/internal/chat"
	"github.com/alino/plan9-winxp/pi9/internal/provider"
)

// summarizeTimeout bounds a one-shot summarization request. Generous,
// since summaries can be long and the model may be reasoning.
const summarizeTimeout = 3 * time.Minute

// timeNow is a tiny indirection so the synthetic compaction turn gets a
// real timestamp; kept here to avoid importing time in callers.
func timeNow() time.Time { return time.Now() }

// Compaction defaults, in tokens. Match pi.dev's settings.json defaults.
const (
	defaultReserveTokens    = 16384 // headroom left for the model's reply
	defaultKeepRecentTokens = 20000 // most-recent turns kept verbatim
)

// toolResultMaxChars caps each tool result when serializing a turn for
// summarization — tool output (read/run_rc) dominates context, and the
// summary doesn't need the full body. Matches pi's TOOL_RESULT_MAX_CHARS.
const toolResultMaxChars = 2000

// summarizationPrompt is the structured-checkpoint instruction appended
// after the serialized conversation. Lifted from pi's SUMMARIZATION_PROMPT.
const summarizationPrompt = `The messages above are a conversation to summarize. Create a structured context checkpoint summary that another LLM will use to continue the work.

Use this EXACT format:

## Goal
[What is the user trying to accomplish? Can be multiple items if the session covers different tasks.]

## Constraints & Preferences
- [Any constraints, preferences, or requirements mentioned by user]
- [Or "(none)" if none were mentioned]

## Progress
### Done
- [x] [Completed tasks/changes]

### In Progress
- [ ] [Current work]

### Blocked
- [Issues preventing progress, if any]

## Key Decisions
- **[Decision]**: [Brief rationale]

## Next Steps
1. [Ordered list of what should happen next]

## Critical Context
- [Any data, examples, or references needed to continue]
- [Or "(none)" if not applicable]

Keep each section concise. Preserve exact file paths, function names, and error messages.`

// estimateTokens approximates the token count of a string at ~4 bytes
// per token. Deliberately cheap — called on every render for the status
// footer and on every cut-point search.
func estimateTokens(s string) int {
	return (len(s) + 3) / 4
}

// estimateTurnTokens estimates the tokens one turn contributes to the
// provider context: user + reasoning + assistant + each tool call's
// args and (truncated) output. Local turns contribute nothing (they're
// never sent to the model).
func estimateTurnTokens(t chat.Turn) int {
	if t.Local {
		return 0
	}
	n := estimateTokens(t.User) + estimateTokens(t.Assistant) + estimateTokens(t.Reasoning)
	for _, c := range t.Calls {
		n += estimateTokens(c.Name) + estimateTokens(c.Args)
		out := c.Output
		if len(out) > toolResultMaxChars {
			out = out[:toolResultMaxChars]
		}
		n += estimateTokens(out)
	}
	return n
}

// estimateHistoryTokens estimates the full context cost of a history:
// the system prompt plus every non-local turn.
func estimateHistoryTokens(h *chat.History) int {
	n := estimateTokens(h.System)
	for _, t := range h.Turns {
		n += estimateTurnTokens(t)
	}
	return n
}

// findCompactionCut walks backwards over turns accumulating estimated
// tokens until keepRecentTokens is reached, then returns the index of
// the first turn to KEEP. Turns[:cut] are summarized; turns[cut:] stay.
//
// Cut points land on turn boundaries (a turn = one user→assistant
// exchange including its tool calls), so tool results never get
// separated from their call. Local turns are skipped when accounting
// but the cut index still refers to the raw slice.
//
// Returns 0 when nothing should be summarized (everything fits in the
// keep budget) or when there are too few turns to bother.
func findCompactionCut(turns []chat.Turn, keepRecentTokens int) int {
	if keepRecentTokens <= 0 {
		keepRecentTokens = defaultKeepRecentTokens
	}
	acc := 0
	cut := len(turns)
	for i := len(turns) - 1; i >= 0; i-- {
		acc += estimateTurnTokens(turns[i])
		if acc > keepRecentTokens {
			cut = i + 1 // keep this turn and everything after it
			break
		}
		cut = i
	}
	// Always leave at least one turn to summarize and at least one to
	// keep; otherwise compaction is pointless / destructive.
	if cut <= 0 {
		return 0
	}
	if cut >= len(turns) {
		// Everything fit in the keep budget — nothing to summarize.
		return 0
	}
	return cut
}

// serializeTurns renders turns as plain text for summarization. The
// conversational framing ([User]/[Assistant]/[Tool result]) prevents
// the model from continuing the chat instead of summarizing it. Tool
// results are truncated to toolResultMaxChars. Local turns are skipped.
func serializeTurns(turns []chat.Turn) string {
	var b strings.Builder
	for _, t := range turns {
		if t.Local {
			continue
		}
		if strings.TrimSpace(t.User) != "" {
			fmt.Fprintf(&b, "[User]: %s\n", t.User)
		}
		if r := strings.TrimSpace(t.Reasoning); r != "" {
			fmt.Fprintf(&b, "[Assistant thinking]: %s\n", r)
		}
		if strings.TrimSpace(t.Assistant) != "" {
			fmt.Fprintf(&b, "[Assistant]: %s\n", t.Assistant)
		}
		for _, c := range t.Calls {
			args := strings.ReplaceAll(strings.TrimSpace(c.Args), "\n", " ")
			fmt.Fprintf(&b, "[Assistant tool call]: %s(%s)\n", c.Name, args)
			out := c.Output
			if c.Err != nil {
				out = "ERROR: " + c.Err.Error() + "\n" + out
			}
			if len(out) > toolResultMaxChars {
				out = out[:toolResultMaxChars] +
					fmt.Sprintf("\n\n[... %d more characters truncated]", len(c.Output)-toolResultMaxChars)
			}
			if strings.TrimSpace(out) != "" {
				fmt.Fprintf(&b, "[Tool result]: %s\n", out)
			}
		}
	}
	return b.String()
}

// extractFileOps walks tool calls in turns and returns the sorted lists
// of read-only and modified files. read/write/edit tool calls carry a
// "path" argument; write/edit count as modified, read counts as
// read-only unless the same path was later modified. Mirrors pi's
// computeFileLists.
func extractFileOps(turns []chat.Turn) (readFiles, modifiedFiles []string) {
	read := map[string]bool{}
	modified := map[string]bool{}
	for _, t := range turns {
		for _, c := range t.Calls {
			var a struct {
				Path string `json:"path"`
			}
			if json.Unmarshal([]byte(c.Args), &a) != nil || a.Path == "" {
				continue
			}
			switch c.Name {
			case "read":
				read[a.Path] = true
			case "write", "edit":
				modified[a.Path] = true
			}
		}
	}
	for p := range modified {
		modifiedFiles = append(modifiedFiles, p)
	}
	for p := range read {
		if !modified[p] {
			readFiles = append(readFiles, p)
		}
	}
	sort.Strings(readFiles)
	sort.Strings(modifiedFiles)
	return readFiles, modifiedFiles
}

// formatFileOps renders the <read-files>/<modified-files> XML blocks
// appended to a summary. Empty string when there are no files.
func formatFileOps(readFiles, modifiedFiles []string) string {
	var parts []string
	if len(readFiles) > 0 {
		parts = append(parts, "<read-files>\n"+strings.Join(readFiles, "\n")+"\n</read-files>")
	}
	if len(modifiedFiles) > 0 {
		parts = append(parts, "<modified-files>\n"+strings.Join(modifiedFiles, "\n")+"\n</modified-files>")
	}
	if len(parts) == 0 {
		return ""
	}
	return "\n\n" + strings.Join(parts, "\n\n")
}

// buildSummaryRequest constructs the one-shot message list sent to the
// provider to produce a summary: a single user message holding the
// serialized conversation wrapped in <conversation> tags, followed by
// the structured-format instruction (plus any extra focus instructions).
//
// No tools are advertised — we want prose back, not tool calls.
func buildSummaryRequest(turns []chat.Turn, extraInstructions string) []provider.Message {
	prompt := summarizationPrompt
	if strings.TrimSpace(extraInstructions) != "" {
		prompt += "\n\nAdditional focus: " + strings.TrimSpace(extraInstructions)
	}
	body := "<conversation>\n" + serializeTurns(turns) + "\n</conversation>\n\n" + prompt
	return []provider.Message{
		{Role: provider.RoleSystem, Content: "You are a summarization assistant. Follow the requested format exactly."},
		{Role: provider.RoleUser, Content: body},
	}
}

// summaryDoneMsg is delivered when the one-shot summarization stream
// finishes. summary is the model's text; err is set on failure. cut is
// the turn index passed through so the Update handler knows which turns
// to replace (the history may not have changed in the meantime, but we
// recompute defensively).
type summaryDoneMsg struct {
	summary string
	cut     int
	extra   string
	auto    bool // true when triggered by auto-compaction (not /compact)
	err     error
}

// summarizeCmd runs a one-shot, tool-less streaming request that
// produces a compaction summary, accumulating the streamed text and
// returning it via summaryDoneMsg. It reuses the same provider
// selection + credential resolution as runStream by delegating to the
// shared streamOnce helper.
func summarizeCmd(apiKey, model, thinkingLevel string, turns []chat.Turn, cut int, extra string, auto bool) tea.Cmd {
	msgs := buildSummaryRequest(turns[:cut], extra)
	return func() tea.Msg {
		ctx, cancel := context.WithTimeout(context.Background(), summarizeTimeout)
		defer cancel()
		text, err := streamOnce(ctx, apiKey, model, thinkingLevel, msgs)
		return summaryDoneMsg{summary: text, cut: cut, extra: extra, auto: auto, err: err}
	}
}

// applyCompaction replaces turns[:cut] with one synthetic assistant
// turn carrying the summary (+ file-op footer), preserving turns[cut:].
// The synthetic turn is a normal (non-local) assistant turn so it goes
// to the model on the next request as historical context.
func applyCompaction(h *chat.History, cut int, summary string) (dropped int) {
	if cut <= 0 || cut > len(h.Turns) {
		return 0
	}
	summarized := h.Turns[:cut]
	kept := h.Turns[cut:]

	readFiles, modifiedFiles := extractFileOps(summarized)
	body := strings.TrimSpace(summary) + formatFileOps(readFiles, modifiedFiles)

	synthetic := chat.Turn{
		User:      "(earlier conversation compacted)",
		Assistant: "Summary of earlier conversation:\n\n" + body,
	}
	now := timeNow()
	synthetic.Started = now
	synthetic.Finished = now

	newTurns := make([]chat.Turn, 0, 1+len(kept))
	newTurns = append(newTurns, synthetic)
	newTurns = append(newTurns, kept...)
	h.Turns = newTurns
	return cut
}
