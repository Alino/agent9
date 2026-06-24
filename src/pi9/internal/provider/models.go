// Package provider: model registry — curated static list + live fetches.
//
// Pi.dev has ~16k LOC of generated model metadata. We take a leaner
// approach: a static curated list of ~30 popular models across all
// providers (the ones people actually use), plus a live OpenRouter
// fetch that pulls the full 200+ list directly from their /v1/models
// endpoint when the user has an OpenRouter key.
//
// The /model picker merges static + live and dedupes. Models the user
// has typed before (in session history) get a "recent" tag and float
// to the top.
package provider

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"sort"
	"strings"
	"time"
)

// ModelInfo describes one entry in the model picker. Provider is the
// ProviderID, ID is the model identifier as you'd pass it to
// /model (e.g. "anthropic/claude-sonnet-4" for openrouter, or
// "claude-sonnet-4" for direct Anthropic). Label is the user-facing
// display name. Recent is set by the caller for ranking.
type ModelInfo struct {
	Provider      ProviderID
	ID            string
	Label         string
	ContextWindow int // tokens, 0 if unknown
	Recent        bool
}

// CuratedModels returns a hardcoded list of popular models across
// providers. Maintained by hand — small enough to skim, big enough
// that "most users find what they want" without a live fetch.
//
// Sorted in a sensible default order: frontier models first, then
// strong defaults, then specialized/older/cheap.
func CuratedModels() []ModelInfo {
	return []ModelInfo{
		// Anthropic (via OpenRouter — most users have OR key)
		{ProviderOpenRouter, "anthropic/claude-sonnet-4.5", "Claude Sonnet 4.5", 200000, false},
		{ProviderOpenRouter, "anthropic/claude-opus-4.1", "Claude Opus 4.1", 200000, false},
		{ProviderOpenRouter, "anthropic/claude-haiku-4.5", "Claude Haiku 4.5", 200000, false},

		// OpenAI via OpenRouter
		{ProviderOpenRouter, "openai/gpt-5", "GPT-5", 200000, false},
		{ProviderOpenRouter, "openai/gpt-5-mini", "GPT-5 Mini", 200000, false},
		{ProviderOpenRouter, "openai/o3", "OpenAI o3", 200000, false},
		{ProviderOpenRouter, "openai/o4-mini", "OpenAI o4 Mini", 200000, false},

		// Google
		{ProviderOpenRouter, "google/gemini-2.5-pro", "Gemini 2.5 Pro", 2000000, false},
		{ProviderOpenRouter, "google/gemini-2.5-flash", "Gemini 2.5 Flash", 1000000, false},

		// Open weights
		{ProviderOpenRouter, "meta-llama/llama-3.3-70b-instruct", "Llama 3.3 70B", 128000, false},
		{ProviderOpenRouter, "deepseek/deepseek-chat", "DeepSeek Chat v3", 128000, false},
		{ProviderOpenRouter, "deepseek/deepseek-r1", "DeepSeek R1", 128000, false},
		{ProviderOpenRouter, "qwen/qwen3-235b-a22b", "Qwen3 235B", 128000, false},

		// Moonshot (Kimi) — pi9's default
		{ProviderOpenRouter, "moonshotai/kimi-k2.5", "Kimi K2.5", 200000, false},
		{ProviderOpenRouter, "moonshotai/kimi-k2", "Kimi K2", 200000, false},

		// xAI
		{ProviderOpenRouter, "x-ai/grok-4", "Grok 4", 256000, false},

		// Mistral
		{ProviderOpenRouter, "mistralai/mistral-large", "Mistral Large", 128000, false},

		// Direct Anthropic (requires Anthropic key/OAuth)
		{ProviderAnthropic, "claude-sonnet-4-5", "Claude Sonnet 4.5 (direct)", 200000, false},
		{ProviderAnthropic, "claude-opus-4-1", "Claude Opus 4.1 (direct)", 200000, false},
		{ProviderAnthropic, "claude-haiku-4-5", "Claude Haiku 4.5 (direct)", 200000, false},

		// Direct OpenAI (requires OpenAI key)
		{ProviderOpenAI, "gpt-5", "GPT-5 (direct)", 200000, false},
		{ProviderOpenAI, "gpt-5-mini", "GPT-5 Mini (direct)", 200000, false},

		// Fast/cheap options
		{ProviderGroq, "llama-3.3-70b-versatile", "Llama 3.3 70B (Groq, fast)", 128000, false},
		{ProviderCerebras, "llama-4-maverick-17b-128e-instruct", "Llama 4 Maverick (Cerebras, fast)", 128000, false},

		// DeepSeek direct
		{ProviderDeepSeek, "deepseek-chat", "DeepSeek Chat (direct)", 128000, false},
		{ProviderDeepSeek, "deepseek-reasoner", "DeepSeek Reasoner (direct)", 128000, false},

		// MiniMax direct — Anthropic-compatible endpoint. Model IDs from
		// pi.dev's minimax.models.ts. Requires `/login → MiniMax` first.
		{ProviderMiniMax, "MiniMax-M3", "MiniMax-M3 (direct)", 512000, false},
		{ProviderMiniMax, "MiniMax-M2.7", "MiniMax-M2.7 (direct)", 204800, false},
		{ProviderMiniMax, "MiniMax-M2.7-highspeed", "MiniMax-M2.7 Highspeed (direct)", 204800, false},

		// GitHub Copilot subscription — pi9 routes via "copilot/" prefix.
		// Requires `/login → GitHub Copilot` first (OAuth device flow).
		// Model IDs taken from pi.dev's models.generated.ts; subset of
		// what's actually available depends on your Copilot tier.
		{ProviderCopilot, "copilot/claude-sonnet-4.5", "Claude Sonnet 4.5 (Copilot)", 200000, false},
		{ProviderCopilot, "copilot/claude-opus-4.5", "Claude Opus 4.5 (Copilot)", 200000, false},
		{ProviderCopilot, "copilot/claude-haiku-4.5", "Claude Haiku 4.5 (Copilot)", 200000, false},
		{ProviderCopilot, "copilot/gpt-5", "GPT-5 (Copilot)", 200000, false},
		{ProviderCopilot, "copilot/gpt-5-mini", "GPT-5 Mini (Copilot)", 200000, false},
		{ProviderCopilot, "copilot/gpt-4.1", "GPT-4.1 (Copilot)", 200000, false},
		{ProviderCopilot, "copilot/gpt-4o", "GPT-4o (Copilot)", 128000, false},
		{ProviderCopilot, "copilot/gemini-2.5-pro", "Gemini 2.5 Pro (Copilot)", 1000000, false},
		{ProviderCopilot, "copilot/grok-code-fast-1", "Grok Code Fast 1 (Copilot)", 256000, false},
	}
}

// ContextWindowFor returns the context window (in tokens) for a model
// id, consulting the curated list first and then a few prefix-based
// fallbacks. Returns 0 when unknown — callers should treat 0 as "don't
// show a percentage" rather than dividing by it.
//
// The curated list carries explicit windows; live OpenRouter/Copilot
// entries aren't consulted here (this is a cheap, offline lookup used
// every render). For models the curated list doesn't name, we fall back
// to family defaults that match the common case.
func ContextWindowFor(model string) int {
	for _, mi := range CuratedModels() {
		if mi.ID == model {
			return mi.ContextWindow
		}
	}
	m := strings.ToLower(model)
	switch {
	case strings.Contains(m, "gemini-2.5-pro"):
		return 2000000
	case strings.Contains(m, "gemini"):
		return 1000000
	case strings.Contains(m, "claude"):
		return 200000
	case strings.Contains(m, "gpt-5"), strings.Contains(m, "o3"), strings.Contains(m, "o4"):
		return 200000
	case strings.Contains(m, "grok"):
		return 256000
	case strings.Contains(m, "kimi"):
		return 200000
	}
	return 0
}

// FetchOpenRouterModels pulls the live model list from OpenRouter's
// /v1/models endpoint. No auth required — it's a public catalog.
// Returns a list with ProviderOpenRouter set on every entry.
//
// Capped at ~3s timeout; if OpenRouter is slow we just fall back to
// the curated static list.
//
// Output is sorted by ID for stable display.
func FetchOpenRouterModels(ctx context.Context) ([]ModelInfo, error) {
	ctx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()

	req, err := http.NewRequestWithContext(ctx, "GET", "https://openrouter.ai/api/v1/models", nil)
	if err != nil {
		return nil, fmt.Errorf("build request: %w", err)
	}
	req.Header.Set("user-agent", "pi9/1.0 (plan9)")

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("fetch: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("openrouter /models: HTTP %d", resp.StatusCode)
	}

	// Cap response size at 1MB — the model list is typically ~300KB.
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return nil, fmt.Errorf("read body: %w", err)
	}

	var raw struct {
		Data []struct {
			ID            string `json:"id"`
			Name          string `json:"name"`
			ContextLength int    `json:"context_length"`
		} `json:"data"`
	}
	if err := json.Unmarshal(body, &raw); err != nil {
		return nil, fmt.Errorf("parse: %w", err)
	}

	out := make([]ModelInfo, 0, len(raw.Data))
	for _, m := range raw.Data {
		label := m.Name
		if label == "" {
			label = m.ID
		}
		out = append(out, ModelInfo{
			Provider:      ProviderOpenRouter,
			ID:            m.ID,
			Label:         label,
			ContextWindow: m.ContextLength,
		})
	}

	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out, nil
}

// FetchCopilotModels pulls the live model list available to a logged-in
// GitHub Copilot account. Requires the user's Copilot access token
// (from auth.json after OAuth device flow).
//
// Endpoint: api.individual.githubcopilot.com/models — undocumented
// but stable. Returns the actual list available to your specific
// Copilot tier (Free / Pro / Business / Enterprise differ).
//
// All returned models get ID prefixed with "copilot/" so pi9's
// ProviderForModel routes them correctly.
func FetchCopilotModels(ctx context.Context, accessToken, baseURL string) ([]ModelInfo, error) {
	if accessToken == "" {
		return nil, fmt.Errorf("no copilot access token")
	}
	if baseURL == "" {
		baseURL = "https://api.individual.githubcopilot.com"
	}
	ctx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()

	url := strings.TrimRight(baseURL, "/") + "/models"
	req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
	if err != nil {
		return nil, fmt.Errorf("build request: %w", err)
	}
	req.Header.Set("Authorization", "Bearer "+accessToken)
	req.Header.Set("Editor-Version", "vscode/1.107.0")
	req.Header.Set("Editor-Plugin-Version", "copilot-chat/0.35.0")
	req.Header.Set("Copilot-Integration-Id", "vscode-chat")
	req.Header.Set("User-Agent", "GitHubCopilotChat/0.35.0")

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("fetch: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("copilot /models: HTTP %d", resp.StatusCode)
	}

	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return nil, fmt.Errorf("read body: %w", err)
	}

	var raw struct {
		Data []struct {
			ID          string `json:"id"`
			Name        string `json:"name"`
			ModelPicker *struct {
				Enabled bool `json:"enabled"`
			} `json:"model_picker_enabled,omitempty"`
			Capabilities struct {
				Limits struct {
					MaxContextWindowTokens int `json:"max_context_window_tokens"`
				} `json:"limits"`
			} `json:"capabilities"`
		} `json:"data"`
	}
	if err := json.Unmarshal(body, &raw); err != nil {
		return nil, fmt.Errorf("parse: %w", err)
	}

	out := make([]ModelInfo, 0, len(raw.Data))
	for _, m := range raw.Data {
		label := m.Name
		if label == "" {
			label = m.ID
		}
		label += " (Copilot)"
		out = append(out, ModelInfo{
			Provider:      ProviderCopilot,
			ID:            "copilot/" + m.ID,
			Label:         label,
			ContextWindow: m.Capabilities.Limits.MaxContextWindowTokens,
		})
	}

	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out, nil
}

// MergedModels returns curated + live OpenRouter models, deduped by
// (Provider, ID). Curated entries with their nicer Label win over
// generic OpenRouter Labels when both exist.
//
// recentIDs is a list of model IDs the user has used before in this
// session; they get the Recent flag and float to the top of the list.
//
// Falls back to just curated if the live fetch fails.
//
// copilotToken (optional): if non-empty, also fetches the live
// Copilot model list. copilotURL is the proxy-ep from the Copilot
// auth.json (may be empty for default).
func MergedModels(ctx context.Context, recentIDs []string, copilotToken, copilotURL string) []ModelInfo {
	curated := CuratedModels()
	live, _ := FetchOpenRouterModels(ctx)

	// Index curated by (provider, id) so live entries don't duplicate
	// or overwrite them.
	type key struct {
		p ProviderID
		i string
	}
	seen := map[key]int{}
	for i, m := range curated {
		seen[key{m.Provider, m.ID}] = i
	}

	for _, m := range live {
		k := key{m.Provider, m.ID}
		if _, ok := seen[k]; ok {
			continue
		}
		seen[k] = len(curated)
		curated = append(curated, m)
	}

	// Live Copilot models if we have an access token. These take
	// priority over the hardcoded copilot entries (live = actually
	// available to your account).
	if copilotToken != "" {
		copilotLive, _ := FetchCopilotModels(ctx, copilotToken, copilotURL)
		for _, m := range copilotLive {
			k := key{m.Provider, m.ID}
			if idx, ok := seen[k]; ok {
				// Update curated entry's metadata from live
				curated[idx].Label = m.Label
				if m.ContextWindow > 0 {
					curated[idx].ContextWindow = m.ContextWindow
				}
				continue
			}
			seen[k] = len(curated)
			curated = append(curated, m)
		}
	}

	// Mark recent
	recentSet := map[string]bool{}
	for _, id := range recentIDs {
		recentSet[id] = true
	}
	for i := range curated {
		if recentSet[curated[i].ID] {
			curated[i].Recent = true
		}
	}

	// Stable sort: Recent first, then curated (preserving curated
	// order), then live (sorted alphabetically).
	sort.SliceStable(curated, func(i, j int) bool {
		if curated[i].Recent != curated[j].Recent {
			return curated[i].Recent
		}
		return false
	})

	return curated
}

// FuzzyMatch returns true if `query` matches `text` in a fuzzy sense
// (chars of query appear in order in text, case-insensitive). Used to
// filter the model picker as the user types.
//
// Pi.dev uses a more sophisticated scoring algorithm but this is
// good enough for a few hundred entries.
func FuzzyMatch(query, text string) bool {
	q := strings.ToLower(strings.TrimSpace(query))
	t := strings.ToLower(text)
	if q == "" {
		return true
	}
	i := 0
	for j := 0; j < len(t) && i < len(q); j++ {
		if q[i] == t[j] {
			i++
		}
	}
	return i == len(q)
}
