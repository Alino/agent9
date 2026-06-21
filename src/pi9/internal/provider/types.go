// Package provider abstracts LLM backends.
//
// Pre-Phase-10, this package was OpenRouter-only. Phase 10 introduces
// a Provider interface so pi9 can speak directly to Anthropic, OpenAI,
// Groq, etc. without going through OpenRouter as a relay.
//
// Provider selection happens at request time via ProviderForModel —
// the model name's prefix tells us which backend it lives on. OAuth
// providers (Claude Pro, ChatGPT Plus, Copilot) come later.
package provider

import (
	"context"
	"strings"
)

// ---------- shared wire types (used by all providers) ----------

// Role tags a message in the conversation.
type Role string

const (
	RoleSystem    Role = "system"
	RoleUser      Role = "user"
	RoleAssistant Role = "assistant"
	RoleTool      Role = "tool"
)

// Message is provider-neutral. Each provider translates to its wire
// format (OpenAI-compatible vs Anthropic's messages format vs etc).
type Message struct {
	Role       Role       `json:"role"`
	Content    string     `json:"content,omitempty"`
	ToolCalls  []ToolCall `json:"tool_calls,omitempty"`   // assistant emitted these
	ToolCallID string     `json:"tool_call_id,omitempty"` // tool result is for this id
	Name       string     `json:"name,omitempty"`         // tool's name (for tool results)
}

// Tool is the schema we advertise to the model.
type Tool struct {
	Name        string                 `json:"name"`
	Description string                 `json:"description"`
	Parameters  map[string]interface{} `json:"parameters"`
}

// ToolCall is the model asking us to run a tool. Args is the raw
// JSON arguments string (each provider reassembles it differently
// during streaming, but the end result is the same).
type ToolCall struct {
	ID       string     `json:"id"`
	Type     string     `json:"type"` // always "function"
	Function ToolCallFn `json:"function"`
}

// ToolCallFn is the inner detail of a tool call.
type ToolCallFn struct {
	Name      string `json:"name"`
	Arguments string `json:"arguments"`
}

// Chunk is one streaming event from a provider. Most chunks are
// content deltas. The terminal chunk has Done=true and may carry
// assembled ToolCalls if the model requested any, plus final token
// Usage.
type Chunk struct {
	Delta     string
	Reasoning string // thinking/reasoning delta text; empty unless the model streams reasoning
	Done      bool
	ToolCalls []ToolCall
	Usage     *Usage // set only on the terminal (Done) chunk when the provider reports usage
	Err       error  // set only on the terminal chunk if streaming failed
}

// Usage reports token accounting for one assistant turn. Populated on
// the terminal (Done) Chunk from the provider's final SSE usage block.
// Fields are 0 when the provider doesn't report that count.
type Usage struct {
	PromptTokens     int // input/prompt tokens
	CompletionTokens int // output/completion tokens
	TotalTokens      int // prompt + completion (or provider-reported total)
}

// Config is the per-request configuration. APIKey + Model are
// required. APIURL overrides the provider's default endpoint
// (rarely needed; mostly for testing against mock servers).
type Config struct {
	APIKey    string
	Model     string
	APIURL    string // optional override (used for mock testing)
	MaxTokens int
	Tools     []Tool

	// ThinkingLevel requests extended thinking / reasoning effort from
	// models that support it. Valid values: "off", "minimal", "low",
	// "medium", "high", "xhigh". Empty string or "off" disables it.
	//
	// Each provider maps this differently:
	//   - Anthropic: enables extended thinking with a budget_tokens value
	//     derived from the level (see levelToBudget).
	//   - OpenAI-compatible: sends reasoning_effort (see
	//     levelToReasoningEffort); xhigh collapses to "high".
	// Providers/models that don't support reasoning ignore it.
	ThinkingLevel string
}

// ---------- Provider abstraction (Phase 10) ----------

// Provider speaks one specific backend. The Stream method matches
// the legacy free-function StreamRequest signature for ease of
// migration — main.go can switch to provider.Get(model).Stream(...)
// in one line.
type Provider interface {
	// Name is the short id ("anthropic", "openrouter", etc.).
	Name() ProviderID

	// Stream is the one streaming call per assistant turn.
	// Implementations close `chunks` when done. Errors during
	// streaming are surfaced via `errs` channel rather than on
	// individual chunks, matching the original openrouter.go pattern.
	Stream(ctx context.Context, cfg Config, messages []Message) (chunks <-chan Chunk, errs <-chan error)
}

// ProviderID enumerates known providers. Stable string keys used as
// map keys in auth.json and as the names typed in /login picker.
// Matches pi.dev's naming where possible (see pi9-phase8.md).
type ProviderID string

const (
	ProviderAnthropic  ProviderID = "anthropic"
	ProviderOpenAI     ProviderID = "openai"
	ProviderOpenRouter ProviderID = "openrouter"
	ProviderGroq       ProviderID = "groq"
	ProviderDeepSeek   ProviderID = "deepseek"
	ProviderXAI        ProviderID = "xai"
	ProviderGoogle     ProviderID = "google"
	ProviderMistral    ProviderID = "mistral"
	ProviderTogether   ProviderID = "together"
	ProviderFireworks  ProviderID = "fireworks"
	ProviderCerebras   ProviderID = "cerebras"
	ProviderCopilot    ProviderID = "github-copilot" // Phase 10 S3: separate from OpenAI/OpenRouter; has its own URL + headers
)

// AllProviders returns the canonical list shown in /login. Order
// matters — most-likely-used first. OpenRouter at top for upgrade
// continuity (pre-Phase-10 pi9 only spoke OpenRouter).
func AllProviders() []ProviderID {
	return []ProviderID{
		ProviderOpenRouter,
		ProviderAnthropic,
		ProviderOpenAI,
		ProviderCopilot,
		ProviderDeepSeek,
		ProviderGroq,
		ProviderXAI,
		ProviderGoogle,
		ProviderMistral,
		ProviderTogether,
		ProviderFireworks,
		ProviderCerebras,
	}
}

// DisplayName is the user-facing label in /login.
func DisplayName(p ProviderID) string {
	switch p {
	case ProviderAnthropic:
		return "Anthropic"
	case ProviderOpenAI:
		return "OpenAI"
	case ProviderOpenRouter:
		return "OpenRouter"
	case ProviderGroq:
		return "Groq"
	case ProviderDeepSeek:
		return "DeepSeek"
	case ProviderXAI:
		return "xAI"
	case ProviderGoogle:
		return "Google Gemini"
	case ProviderMistral:
		return "Mistral"
	case ProviderTogether:
		return "Together AI"
	case ProviderFireworks:
		return "Fireworks"
	case ProviderCerebras:
		return "Cerebras"
	case ProviderCopilot:
		return "GitHub Copilot"
	default:
		return string(p)
	}
}

// KeyPrefix hints at API key format for /login validation. Empty if
// unknown / variable. The picker uses this only as soft guidance —
// providers can change formats, so we never reject on prefix.
func KeyPrefix(p ProviderID) string {
	switch p {
	case ProviderAnthropic:
		return "sk-ant-"
	case ProviderOpenAI:
		return "sk-"
	case ProviderOpenRouter:
		return "sk-or-"
	case ProviderGroq:
		return "gsk_"
	case ProviderDeepSeek:
		return "sk-"
	case ProviderXAI:
		return "xai-"
	}
	return ""
}

// KeyURL points at where the user goes to mint an API key. Shown in
// /login picker. Empty if we don't have a stable URL or if the
// provider doesn't accept API keys (e.g. Copilot is OAuth-only).
func KeyURL(p ProviderID) string {
	switch p {
	case ProviderAnthropic:
		return "https://console.anthropic.com/settings/keys"
	case ProviderOpenAI:
		return "https://platform.openai.com/api-keys"
	case ProviderOpenRouter:
		return "https://openrouter.ai/keys"
	case ProviderGroq:
		return "https://console.groq.com/keys"
	case ProviderDeepSeek:
		return "https://platform.deepseek.com/api_keys"
	case ProviderXAI:
		return "https://console.x.ai/"
	case ProviderGoogle:
		return "https://aistudio.google.com/apikey"
	}
	return ""
}

// RequiresOAuth returns true if the provider is OAuth-only (no API key
// path). Picker UI uses this to skip the auth-method choice and go
// directly to OAuth.
//
// Currently only GitHub Copilot — they don't issue API keys for the
// Copilot subscription, just OAuth tokens.
func RequiresOAuth(p ProviderID) bool {
	return p == ProviderCopilot
}

// ProviderForModel infers which provider serves a model name. Used
// when /model is called without explicit provider selection.
//
// Rules:
//   - "copilot/*"                   → GitHub Copilot (explicit prefix
//     required because Copilot models
//     overlap with other providers)
//   - "claude-*" or "claude/*"      → Anthropic
//   - "gpt-*", "o1-*", "o3-*", "o4-*"   → OpenAI
//   - "grok-*"                      → xAI
//   - "gemini-*"                    → Google
//   - "mistral-*", "codestral-*"    → Mistral
//   - "deepseek-*"                  → DeepSeek
//   - vendor/model (has "/")        → OpenRouter
//   - everything else                → OpenRouter (catch-all)
func ProviderForModel(model string) ProviderID {
	m := strings.ToLower(model)
	switch {
	case strings.HasPrefix(m, "copilot/"):
		return ProviderCopilot
	case strings.HasPrefix(m, "claude-"), strings.HasPrefix(m, "claude/"):
		return ProviderAnthropic
	case strings.HasPrefix(m, "gpt-"),
		strings.HasPrefix(m, "o1-"),
		strings.HasPrefix(m, "o3-"),
		strings.HasPrefix(m, "o4-"):
		return ProviderOpenAI
	case strings.HasPrefix(m, "grok-"):
		return ProviderXAI
	case strings.HasPrefix(m, "gemini-"):
		return ProviderGoogle
	case strings.HasPrefix(m, "mistral-"), strings.HasPrefix(m, "codestral-"):
		return ProviderMistral
	case strings.HasPrefix(m, "deepseek-"):
		return ProviderDeepSeek
	}
	return ProviderOpenRouter
}

// Get returns the Provider implementation for the named id. Returns
// nil if the id isn't implemented yet (Phase 10 ships only a subset).
// Callers must check for nil before calling Stream.
//
// Note: ProviderOpenAI here returns the API-key-style openaiCompat
// (Chat Completions at api.openai.com). For OAuth-authed OpenAI
// (ChatGPT Plus/Pro), main.go's runStream calls GetCodexResponses
// instead — different URL + wire format. See codex_responses.go.
func Get(id ProviderID) Provider {
	switch id {
	case ProviderAnthropic:
		return anthropic{}
	case ProviderCopilot:
		return copilotProvider{}
	case ProviderOpenRouter, ProviderOpenAI, ProviderGroq, ProviderDeepSeek,
		ProviderXAI, ProviderMistral, ProviderTogether, ProviderFireworks,
		ProviderCerebras:
		return openaiCompat{id: id}
	}
	return nil
}

// GetCodexResponses returns the ChatGPT Plus/Pro Codex Responses
// API provider. Distinct from Get(ProviderOpenAI) — that returns
// the regular openaiCompat (Chat Completions @ api.openai.com).
//
// Codex Responses uses a different URL (chatgpt.com/backend-api/
// codex/responses) and wire format. Called from main.go's runStream
// when the OpenAI auth entry is OAuth (vs api_key).
//
// See codex_responses.go for the implementation details +
// MVP limitations.
func GetCodexResponses() Provider {
	return codexResponsesProvider{}
}
