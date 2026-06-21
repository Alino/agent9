package provider

import "testing"

func TestContextWindowFor(t *testing.T) {
	for _, tc := range []struct {
		model string
		want  int
	}{
		// Exact curated-list hit.
		{"claude-sonnet-4-5", 200000},
		{"moonshotai/kimi-k2.5", 200000},
		// Prefix/family fallbacks for models not in the curated list.
		{"claude-3-7-sonnet-latest", 200000},
		{"gemini-2.5-pro-exp", 2000000},
		{"gemini-1.5-flash", 1000000},
		{"grok-4-fast", 256000},
		{"gpt-5-turbo", 200000},
		// Unknown → 0 (caller hides the percentage).
		{"some-random-model", 0},
		{"", 0},
	} {
		if got := ContextWindowFor(tc.model); got != tc.want {
			t.Errorf("ContextWindowFor(%q) = %d, want %d", tc.model, got, tc.want)
		}
	}
}
