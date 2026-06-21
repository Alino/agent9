// Package provider: GitHub Copilot Streamer implementation.
//
// Copilot uses OpenAI Chat Completions wire format BUT requires:
//   - Special headers: Editor-Version, Copilot-Integration-Id, etc.
//   - Dynamic base URL extracted from the token (proxy-ep=... field)
//   - Bearer auth (not x-api-key)
//
// This is similar enough to openaiCompat that we extract the
// streamOpenAICompat helper and just override the URL + headers
// before calling into it.
//
// Models: pi.dev's catalog has ~15 Copilot models (gpt-5, claude-sonnet-4,
// gemini-2.5-pro, etc.). Pi9 doesn't curate a model list — user types
// the model name with `/model <name>` and we pass it through. If
// Copilot doesn't recognize it the API returns an error which we
// surface.
package provider

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
)

// copilotProvider implements Provider for github-copilot.
type copilotProvider struct{}

func (copilotProvider) Name() ProviderID { return ProviderCopilot }

func (copilotProvider) Stream(ctx context.Context, cfg Config, messages []Message) (<-chan Chunk, <-chan error) {
	chunks := make(chan Chunk, 8)
	errs := make(chan error, 1)

	go func() {
		defer close(chunks)
		defer close(errs)

		// Derive base URL from token. Falls back to
		// api.individual.githubcopilot.com if no proxy-ep.
		baseURL := CopilotBaseURL(cfg.APIKey)
		fullURL := baseURL + "/chat/completions"

		// Pi9 routes Copilot via the `copilot/` prefix in model
		// names (see ProviderForModel). Strip it before sending —
		// Copilot's API expects bare model names like
		// "claude-sonnet-4" or "gpt-5".
		modelName := cfg.Model
		if len(modelName) >= 8 && modelName[:8] == "copilot/" {
			modelName = modelName[8:]
		}

		// Build OpenAI-format body (reuse openrouter.go helpers).
		// Mirror streamOpenAICompat: opt into usage accounting on the
		// terminal chunk and honor the thinking level. Without these,
		// Copilot turns always report Usage=nil and ignore the thinking
		// level. readSSE parses both fields.
		body := requestBody{
			Model:         modelName,
			Messages:      messages,
			Stream:        true,
			StreamOptions: &streamOptions{IncludeUsage: true},
		}
		if cfg.MaxTokens > 0 {
			body.MaxTokens = cfg.MaxTokens
		}
		if thinkingEnabled(cfg.ThinkingLevel) {
			body.ReasoningEffort = levelToReasoningEffort(cfg.ThinkingLevel)
		}
		if len(cfg.Tools) > 0 {
			for _, t := range cfg.Tools {
				body.Tools = append(body.Tools, toolWrapper{
					Type:     "function",
					Function: t,
				})
			}
		}
		buf, err := json.Marshal(body)
		if err != nil {
			errs <- fmt.Errorf("copilot: marshal: %w", err)
			return
		}

		req, err := http.NewRequestWithContext(ctx, "POST", fullURL, bytes.NewReader(buf))
		if err != nil {
			errs <- fmt.Errorf("copilot: new request: %w", err)
			return
		}
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("Accept", "text/event-stream")
		req.Header.Set("Authorization", "Bearer "+cfg.APIKey)
		// Pi.dev's required identity headers — VS Code claims.
		for k, v := range copilotHeaders {
			req.Header.Set(k, v)
		}
		// OpenAI-intent: chat (some Copilot endpoints check this)
		req.Header.Set("openai-intent", "conversation-other")

		client := httpClient()
		resp, err := client.Do(req)
		if err != nil {
			errs <- fmt.Errorf("copilot: do: %w", err)
			return
		}
		defer resp.Body.Close()

		if resp.StatusCode != 200 {
			buf := make([]byte, 4096)
			n, _ := resp.Body.Read(buf)
			errs <- fmt.Errorf("copilot: http %d: %s", resp.StatusCode, string(buf[:n]))
			return
		}

		// Reuse the existing SSE reader — Copilot speaks the same
		// SSE protocol as OpenAI/OpenRouter.
		if err := readSSE(resp.Body, chunks); err != nil {
			errs <- err
		}
	}()

	return chunks, errs
}
