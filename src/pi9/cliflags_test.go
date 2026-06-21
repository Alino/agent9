package main

import (
	"errors"
	"strings"
	"testing"
)

func TestApplyModelProviderOverride(t *testing.T) {
	cases := []struct {
		name      string
		base      string
		model     string
		provider  string
		want      string
		wantLevel string
	}{
		{"no overrides", "moonshotai/kimi-k2.5", "", "", "moonshotai/kimi-k2.5", ""},
		{"model only", "moonshotai/kimi-k2.5", "claude-3-5-sonnet", "", "claude-3-5-sonnet", ""},
		{"provider prepended to bare model", "moonshotai/kimi-k2.5", "kimi", "anthropic", "anthropic/kimi", ""},
		{"provider not duplicated when model qualified", "moonshotai/kimi-k2.5", "openai/gpt-4o", "anthropic", "openai/gpt-4o", ""},
		{"provider with no model leaves default alone", "moonshotai/kimi-k2.5", "", "anthropic", "moonshotai/kimi-k2.5", ""},
		{"provider prepended to default bare model", "kimi", "", "openrouter", "openrouter/kimi", ""},
		{"whitespace flags ignored", "moonshotai/kimi-k2.5", "  ", "  ", "moonshotai/kimi-k2.5", ""},
		{"empty base with provider stays empty", "", "", "anthropic", "", ""},
		// M8: trailing :LEVEL thinking shorthand.
		{"thinking suffix stripped from bare model", "moonshotai/kimi-k2.5", "sonnet:high", "", "sonnet", "high"},
		{"thinking suffix with provider prefixing", "moonshotai/kimi-k2.5", "sonnet:medium", "anthropic", "anthropic/sonnet", "medium"},
		{"thinking suffix on qualified model", "moonshotai/kimi-k2.5", "openai/gpt-4o:low", "", "openai/gpt-4o", "low"},
		{"off level recognized", "moonshotai/kimi-k2.5", "sonnet:off", "", "sonnet", "off"},
		{"non-level suffix preserved", "moonshotai/kimi-k2.5", "deepseek/r1:exacto", "", "deepseek/r1:exacto", ""},
		{"only last colon considered", "moonshotai/kimi-k2.5", "vendor:model:xhigh", "", "vendor:model", "xhigh"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got, level := applyModelProviderOverride(c.base, c.model, c.provider)
			if got != c.want {
				t.Fatalf("applyModelProviderOverride(%q,%q,%q) model = %q, want %q",
					c.base, c.model, c.provider, got, c.want)
			}
			if level != c.wantLevel {
				t.Fatalf("applyModelProviderOverride(%q,%q,%q) level = %q, want %q",
					c.base, c.model, c.provider, level, c.wantLevel)
			}
		})
	}
}

func TestMostRecentSession(t *testing.T) {
	t.Run("newest first picks head", func(t *testing.T) {
		list := func() ([]string, error) {
			return []string{"newest", "older", "oldest"}, nil
		}
		if got := mostRecentSession(list); got != "newest" {
			t.Fatalf("got %q, want %q", got, "newest")
		}
	})

	t.Run("empty list returns empty", func(t *testing.T) {
		list := func() ([]string, error) { return nil, nil }
		if got := mostRecentSession(list); got != "" {
			t.Fatalf("got %q, want empty", got)
		}
	})

	t.Run("error returns empty", func(t *testing.T) {
		list := func() ([]string, error) { return []string{"x"}, errors.New("boom") }
		if got := mostRecentSession(list); got != "" {
			t.Fatalf("got %q, want empty on error", got)
		}
	})
}

func TestBuildSystemPromptCLIOverrides(t *testing.T) {
	// Save + restore the process-global override vars so the test is
	// isolated from default behaviour and from other tests.
	origSys, origApp := cliSystemPrompt, cliAppendSystemPrompt
	t.Cleanup(func() { cliSystemPrompt, cliAppendSystemPrompt = origSys, origApp })

	t.Run("system-prompt replaces base", func(t *testing.T) {
		cliSystemPrompt = "REPLACEMENT-PROMPT-SENTINEL"
		cliAppendSystemPrompt = ""
		got := buildSystemPrompt()
		if !strings.Contains(got, "REPLACEMENT-PROMPT-SENTINEL") {
			t.Fatalf("override text missing from prompt: %q", got)
		}
		if baseSystemPrompt != "" && strings.Contains(got, baseSystemPrompt) {
			t.Fatalf("base prompt should be replaced, not present: %q", got)
		}
	})

	t.Run("append-system-prompt adds text", func(t *testing.T) {
		cliSystemPrompt = ""
		cliAppendSystemPrompt = "APPENDED-PROMPT-SENTINEL"
		got := buildSystemPrompt()
		if !strings.Contains(got, "APPENDED-PROMPT-SENTINEL") {
			t.Fatalf("appended text missing from prompt: %q", got)
		}
		if !strings.Contains(got, baseSystemPrompt) {
			t.Fatalf("base prompt should still be present when only appending: %q", got)
		}
	})
}
