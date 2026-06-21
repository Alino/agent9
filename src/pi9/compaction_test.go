package main

import (
	"strings"
	"testing"

	"github.com/alino/plan9-winxp/pi9/internal/chat"
)

// makeTurn builds a non-local turn with the given user/assistant text.
func makeTurn(user, assistant string) chat.Turn {
	return chat.Turn{User: user, Assistant: assistant}
}

func TestEstimateTokens(t *testing.T) {
	// ~4 bytes per token, rounding up.
	for _, tc := range []struct {
		in   string
		want int
	}{
		{"", 0},
		{"abcd", 1},
		{"abcde", 2},
		{strings.Repeat("x", 40), 10},
	} {
		if got := estimateTokens(tc.in); got != tc.want {
			t.Errorf("estimateTokens(%q) = %d, want %d", tc.in, got, tc.want)
		}
	}
}

func TestEstimateTurnTokens_LocalSkipped(t *testing.T) {
	local := chat.Turn{User: strings.Repeat("x", 1000), Assistant: strings.Repeat("y", 1000), Local: true}
	if got := estimateTurnTokens(local); got != 0 {
		t.Errorf("local turn should cost 0 tokens, got %d", got)
	}
}

func TestEstimateTurnTokens_TruncatesToolOutput(t *testing.T) {
	// A huge tool output should be capped at toolResultMaxChars when
	// estimating, so the estimate is bounded regardless of body size.
	big := chat.Turn{
		Calls: []chat.ToolInvocation{{
			Name:   "read",
			Args:   `{"path":"x"}`,
			Output: strings.Repeat("z", 1_000_000),
		}},
	}
	got := estimateTurnTokens(big)
	// Output contributes at most ceil(2000/4)=500 tokens plus small
	// name/args. Assert it's well under what the full body would cost.
	if got > 600 {
		t.Errorf("tool output not truncated in estimate: got %d tokens", got)
	}
}

func TestFindCompactionCut(t *testing.T) {
	// Each turn here is ~250 tokens (1000 bytes of assistant text).
	mk := func(n int) []chat.Turn {
		out := make([]chat.Turn, n)
		for i := range out {
			out[i] = makeTurn("hi", strings.Repeat("a", 1000))
		}
		return out
	}

	// keepRecent budget of 600 tokens keeps ~2-3 recent turns; with 6
	// turns the cut should land somewhere in the middle (>0, <6).
	turns := mk(6)
	cut := findCompactionCut(turns, 600)
	if cut <= 0 || cut >= len(turns) {
		t.Fatalf("expected a mid cut for 6 turns, got %d", cut)
	}

	// Tiny history that fits entirely in the keep budget → no cut.
	if cut := findCompactionCut(mk(1), defaultKeepRecentTokens); cut != 0 {
		t.Errorf("expected no cut for single small turn, got %d", cut)
	}

	// Empty history → no cut.
	if cut := findCompactionCut(nil, defaultKeepRecentTokens); cut != 0 {
		t.Errorf("expected no cut for empty history, got %d", cut)
	}
}

func TestExtractFileOps(t *testing.T) {
	turns := []chat.Turn{
		{Calls: []chat.ToolInvocation{
			{Name: "read", Args: `{"path":"a.go"}`},
			{Name: "read", Args: `{"path":"b.go"}`},
			{Name: "edit", Args: `{"path":"b.go","edits":[]}`},
			{Name: "write", Args: `{"path":"c.go","content":"x"}`},
			{Name: "run_rc", Args: `{"command":"ls"}`}, // no path → ignored
			{Name: "read", Args: `not json`},           // unparsable → ignored
		}},
	}
	read, modified := extractFileOps(turns)

	// b.go was read then edited → counts as modified, not read-only.
	wantRead := []string{"a.go"}
	wantMod := []string{"b.go", "c.go"}
	if strings.Join(read, ",") != strings.Join(wantRead, ",") {
		t.Errorf("readFiles = %v, want %v", read, wantRead)
	}
	if strings.Join(modified, ",") != strings.Join(wantMod, ",") {
		t.Errorf("modifiedFiles = %v, want %v", modified, wantMod)
	}
}

func TestFormatFileOps(t *testing.T) {
	out := formatFileOps([]string{"a.go"}, []string{"b.go"})
	if !strings.Contains(out, "<read-files>\na.go\n</read-files>") {
		t.Errorf("missing read-files block: %q", out)
	}
	if !strings.Contains(out, "<modified-files>\nb.go\n</modified-files>") {
		t.Errorf("missing modified-files block: %q", out)
	}
	if formatFileOps(nil, nil) != "" {
		t.Errorf("empty file ops should produce empty string")
	}
}

func TestSerializeTurns_TruncatesAndSkipsLocal(t *testing.T) {
	turns := []chat.Turn{
		{User: "do a thing", Assistant: "ok"},
		{User: "/help", Assistant: "help text", Local: true},
		{Calls: []chat.ToolInvocation{{
			Name:   "read",
			Args:   `{"path":"x"}`,
			Output: strings.Repeat("Q", toolResultMaxChars+500),
		}}},
	}
	s := serializeTurns(turns)
	if !strings.Contains(s, "[User]: do a thing") {
		t.Errorf("expected user line in serialized output")
	}
	if strings.Contains(s, "help text") {
		t.Errorf("local turns must not be serialized")
	}
	if !strings.Contains(s, "more characters truncated") {
		t.Errorf("expected truncation marker for long tool output")
	}
}

func TestApplyCompaction(t *testing.T) {
	h := &chat.History{
		System: "sys",
		Turns: []chat.Turn{
			makeTurn("u1", "a1"),
			makeTurn("u2", "a2"),
			makeTurn("u3", "a3"),
			makeTurn("u4", "a4"),
		},
	}
	dropped := applyCompaction(h, 2, "## Goal\nstuff")
	if dropped != 2 {
		t.Fatalf("dropped = %d, want 2", dropped)
	}
	// One synthetic turn + the 2 kept turns = 3.
	if len(h.Turns) != 3 {
		t.Fatalf("len(Turns) = %d, want 3", len(h.Turns))
	}
	if !strings.Contains(h.Turns[0].Assistant, "Summary of earlier conversation") {
		t.Errorf("first turn should be the synthetic summary, got %q", h.Turns[0].Assistant)
	}
	if h.Turns[0].Local {
		t.Errorf("synthetic summary turn must be non-local so the model sees it")
	}
	if h.Turns[1].User != "u3" || h.Turns[2].User != "u4" {
		t.Errorf("kept turns not preserved: %q, %q", h.Turns[1].User, h.Turns[2].User)
	}

	// Out-of-range cuts are no-ops.
	before := len(h.Turns)
	if got := applyCompaction(h, 0, "x"); got != 0 || len(h.Turns) != before {
		t.Errorf("cut=0 should be a no-op")
	}
	if got := applyCompaction(h, 999, "x"); got != 0 || len(h.Turns) != before {
		t.Errorf("out-of-range cut should be a no-op")
	}
}

func TestContextStatus(t *testing.T) {
	// Known window + provider usage → percentage, no "est".
	m := pi9Model{model: "claude-sonnet-4-5", lastTotalTokens: 100000}
	s := m.contextStatus()
	if !strings.Contains(s, "%") || strings.Contains(s, "est") {
		t.Errorf("expected percentage without est for reported usage, got %q", s)
	}

	// No usage reported → byte estimate marked "est".
	m2 := pi9Model{model: "claude-sonnet-4-5", history: chat.History{System: strings.Repeat("x", 4000)}}
	s2 := m2.contextStatus()
	if !strings.Contains(s2, "est") {
		t.Errorf("expected est marker for byte-estimated usage, got %q", s2)
	}

	// Unknown model + empty history → empty indicator.
	m3 := pi9Model{model: "totally-unknown-model"}
	if got := m3.contextStatus(); got != "" {
		t.Errorf("expected empty context status, got %q", got)
	}
}

func TestShouldAutoCompact(t *testing.T) {
	// Auto off → never.
	m := pi9Model{model: "claude-sonnet-4-5", autoCompact: false, lastTotalTokens: 199000, reserveTokens: defaultReserveTokens}
	if m.shouldAutoCompact() {
		t.Errorf("auto-compaction disabled should never trigger")
	}

	// On + over the threshold (200k window - 16k reserve = 184k) → yes.
	m.autoCompact = true
	if !m.shouldAutoCompact() {
		t.Errorf("expected auto-compaction to trigger at 199k/200k")
	}

	// On + well under threshold → no.
	m.lastTotalTokens = 1000
	if m.shouldAutoCompact() {
		t.Errorf("did not expect auto-compaction at 1k tokens")
	}

	// Unknown window → never (can't compute a threshold).
	m4 := pi9Model{model: "unknown", autoCompact: true, lastTotalTokens: 10_000_000}
	if m4.shouldAutoCompact() {
		t.Errorf("unknown window should not trigger auto-compaction")
	}
}
