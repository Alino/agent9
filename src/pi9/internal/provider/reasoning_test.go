package provider

import (
	"strings"
	"testing"
)

func TestLevelToBudget(t *testing.T) {
	cases := []struct {
		level string
		want  int
	}{
		{"", 0},
		{"off", 0},
		{"OFF", 0},
		{"minimal", 1024},
		{"low", 2048},
		{"medium", 8192},
		{"high", 16384},
		{"xhigh", 16384}, // clamped to high, matching pi
		{"XHigh", 16384},
		{" high ", 16384},
		{"bogus", 0},
	}
	for _, c := range cases {
		if got := levelToBudget(c.level); got != c.want {
			t.Errorf("levelToBudget(%q) = %d, want %d", c.level, got, c.want)
		}
	}
}

func TestLevelToReasoningEffort(t *testing.T) {
	cases := []struct {
		level string
		want  string
	}{
		{"", ""},
		{"off", ""},
		{"minimal", "minimal"},
		{"low", "low"},
		{"medium", "medium"},
		{"high", "high"},
		{"xhigh", "high"}, // xhigh collapses to high
		{"XHIGH", "high"},
		{"bogus", ""},
	}
	for _, c := range cases {
		if got := levelToReasoningEffort(c.level); got != c.want {
			t.Errorf("levelToReasoningEffort(%q) = %q, want %q", c.level, got, c.want)
		}
	}
}

func TestLevelToCodexReasoningEffort(t *testing.T) {
	cases := []struct {
		level string
		want  string
	}{
		{"", ""},
		{"off", ""},
		{"minimal", "minimal"},
		{"low", "low"},
		{"medium", "medium"},
		{"high", "high"},
		{"xhigh", "xhigh"}, // Codex/gpt-5 preserves xhigh
		{"XHIGH", "xhigh"},
		{" xhigh ", "xhigh"},
		{"bogus", ""},
	}
	for _, c := range cases {
		if got := levelToCodexReasoningEffort(c.level); got != c.want {
			t.Errorf("levelToCodexReasoningEffort(%q) = %q, want %q", c.level, got, c.want)
		}
	}
}

// TestReasoningEffortXHighDivergence pins the key M7 behavior: the Codex
// mapping preserves xhigh while the generic Chat Completions mapping
// collapses it to high.
func TestReasoningEffortXHighDivergence(t *testing.T) {
	if got := levelToReasoningEffort("xhigh"); got != "high" {
		t.Errorf("generic mapping should collapse xhigh->high, got %q", got)
	}
	if got := levelToCodexReasoningEffort("xhigh"); got != "xhigh" {
		t.Errorf("Codex mapping should preserve xhigh, got %q", got)
	}
}

func TestThinkingEnabled(t *testing.T) {
	for _, c := range []struct {
		level string
		want  bool
	}{
		{"", false},
		{"off", false},
		{" OFF ", false},
		{"low", true},
		{"xhigh", true},
	} {
		if got := thinkingEnabled(c.level); got != c.want {
			t.Errorf("thinkingEnabled(%q) = %v, want %v", c.level, got, c.want)
		}
	}
}

func TestAnthropicThinkingInBody(t *testing.T) {
	// off => no thinking field
	b, err := buildAnthropicBody(Config{Model: "claude-x", MaxTokens: 4096}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(b), "thinking") {
		t.Errorf("off level should omit thinking: %s", b)
	}

	// high => thinking enabled with budget 16384, max_tokens raised above budget
	b, err = buildAnthropicBody(Config{Model: "claude-x", MaxTokens: 4096, ThinkingLevel: "high"}, nil)
	if err != nil {
		t.Fatal(err)
	}
	s := string(b)
	if !strings.Contains(s, `"type":"enabled"`) || !strings.Contains(s, `"budget_tokens":16384`) {
		t.Errorf("high level should enable thinking with budget 16384: %s", s)
	}
	if !strings.Contains(s, `"max_tokens":17408`) { // 16384 + 1024 minThinkingOutput
		t.Errorf("max_tokens should be raised above budget: %s", s)
	}
}
