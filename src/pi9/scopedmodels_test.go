package main

import (
	"testing"

	"github.com/alino/plan9-winxp/pi9/internal/provider"
)

func TestCycleModel(t *testing.T) {
	set := []string{"a", "b", "c"}
	cases := []struct {
		name string
		set  []string
		cur  string
		dir  int
		want string
	}{
		{"forward", set, "a", +1, "b"},
		{"forward-wrap", set, "c", +1, "a"},
		{"backward", set, "b", -1, "a"},
		{"backward-wrap", set, "a", -1, "c"},
		{"empty", nil, "a", +1, "a"},
		{"singleton", []string{"x"}, "x", +1, "x"},
		{"not-in-set-fwd", set, "z", +1, "a"},
		{"not-in-set-bwd", set, "z", -1, "c"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if got := cycleModel(c.set, c.cur, c.dir); got != c.want {
				t.Fatalf("cycleModel(%v, %q, %d) = %q, want %q", c.set, c.cur, c.dir, got, c.want)
			}
		})
	}
}

func TestSeedEnabledModels(t *testing.T) {
	catalog := []provider.ModelInfo{
		{ID: "anthropic/claude-sonnet-4.5", Label: "Claude Sonnet 4.5"},
		{ID: "anthropic/claude-opus-4.1", Label: "Claude Opus 4.1"},
		{ID: "openai/gpt-5", Label: "GPT-5"},
	}

	t.Run("empty yields nil", func(t *testing.T) {
		if got := seedEnabledModels("", catalog); got != nil {
			t.Fatalf("want nil, got %v", got)
		}
	})

	t.Run("exact id matches", func(t *testing.T) {
		got := seedEnabledModels("openai/gpt-5", catalog)
		if len(got) != 1 || got[0] != "openai/gpt-5" {
			t.Fatalf("got %v", got)
		}
	})

	t.Run("fuzzy pattern matches multiple", func(t *testing.T) {
		got := seedEnabledModels("claude", catalog)
		if len(got) != 2 {
			t.Fatalf("want 2 claude models, got %v", got)
		}
	})

	t.Run("multiple comma patterns dedup", func(t *testing.T) {
		got := seedEnabledModels("claude, gpt-5, claude", catalog)
		// claude -> 2, gpt-5 -> 1; dedup keeps 3 unique.
		if len(got) != 3 {
			t.Fatalf("want 3 unique, got %v", got)
		}
	})

	t.Run("unknown pattern kept literally", func(t *testing.T) {
		got := seedEnabledModels("some/unknown-model", catalog)
		if len(got) != 1 || got[0] != "some/unknown-model" {
			t.Fatalf("got %v", got)
		}
	})
}

func TestToggleEnabled(t *testing.T) {
	en := []string{"a", "b"}
	en = toggleEnabled(en, "c")
	if len(en) != 3 || en[2] != "c" {
		t.Fatalf("add failed: %v", en)
	}
	en = toggleEnabled(en, "b")
	if containsStr(en, "b") || len(en) != 2 {
		t.Fatalf("remove failed: %v", en)
	}
}

func TestApplyModelCycleRecordsChange(t *testing.T) {
	m := &pi9Model{
		model:         "a",
		enabledModels: []string{"a", "b", "c"},
	}
	m.applyModelCycle(+1)
	if m.model != "b" {
		t.Fatalf("expected model b, got %q", m.model)
	}
	// No scoped models: cycling is a no-op with a hint.
	m2 := &pi9Model{model: "a"}
	m2.applyModelCycle(+1)
	if m2.model != "a" {
		t.Fatalf("expected unchanged, got %q", m2.model)
	}
}
