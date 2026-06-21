// Package provider: thinking/reasoning level helpers.
//
// Pi9 exposes a single provider-neutral knob — Config.ThinkingLevel —
// that each backend maps to its own wire representation:
//
//   - Anthropic uses an explicit token budget for extended thinking.
//   - OpenAI-compatible backends use a `reasoning_effort` string.
//
// These helpers centralize the mapping so the per-provider code (and
// tests) share one source of truth. Mirrors pi.dev's simple-options.ts
// (adjustMaxTokensForThinking / clampReasoning) and the per-provider
// reasoning_effort handling.
package provider

import "strings"

// thinkingEnabled reports whether the level requests any reasoning.
// "" and "off" (case-insensitive) disable it.
func thinkingEnabled(level string) bool {
	switch strings.ToLower(strings.TrimSpace(level)) {
	case "", "off":
		return false
	}
	return true
}

// levelToBudget maps a ThinkingLevel to an Anthropic extended-thinking
// token budget. Returns 0 for off/unknown levels (caller should treat
// 0 as "don't enable thinking").
//
// Values match pi's adjustMaxTokensForThinking defaults (simple-options.ts);
// xhigh has no distinct budget upstream — clampReasoning collapses it to
// high, so we return the high budget for it.
//
//	minimal → 1024
//	low     → 2048
//	medium  → 8192
//	high    → 16384
//	xhigh   → 16384 (clamped to high)
func levelToBudget(level string) int {
	switch strings.ToLower(strings.TrimSpace(level)) {
	case "minimal":
		return 1024
	case "low":
		return 2048
	case "medium":
		return 8192
	case "high", "xhigh":
		return 16384
	}
	return 0
}

// levelToReasoningEffort maps a ThinkingLevel to an OpenAI-compatible
// `reasoning_effort` value for the Chat Completions API (OpenRouter,
// Groq, Copilot, etc.). xhigh collapses to "high" (the highest value
// the Chat Completions API accepts). Returns "" for off/unknown levels,
// signalling the caller to omit the field entirely.
//
//	minimal → "minimal"
//	low     → "low"
//	medium  → "medium"
//	high    → "high"
//	xhigh   → "high"
func levelToReasoningEffort(level string) string {
	return mapReasoningEffort(level, false)
}

// levelToCodexReasoningEffort maps a ThinkingLevel for the OpenAI Codex
// Responses path. Unlike the generic Chat Completions mapping, Codex
// (gpt-5) accepts "xhigh" directly, so we pass it THROUGH instead of
// collapsing it to "high". Returns "" for off/unknown levels.
//
//	minimal → "minimal"
//	low     → "low"
//	medium  → "medium"
//	high    → "high"
//	xhigh   → "xhigh"
func levelToCodexReasoningEffort(level string) string {
	return mapReasoningEffort(level, true)
}

// mapReasoningEffort is the shared mapping. When preserveXHigh is true
// (Codex/gpt-5), xhigh maps to "xhigh"; otherwise it collapses to "high"
// (the ceiling the Chat Completions API accepts).
func mapReasoningEffort(level string, preserveXHigh bool) string {
	switch strings.ToLower(strings.TrimSpace(level)) {
	case "minimal":
		return "minimal"
	case "low":
		return "low"
	case "medium":
		return "medium"
	case "high":
		return "high"
	case "xhigh":
		if preserveXHigh {
			return "xhigh"
		}
		return "high"
	}
	return ""
}
