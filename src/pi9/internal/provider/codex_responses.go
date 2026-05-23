// Package provider: OpenAI Codex (ChatGPT Plus/Pro) Responses API implementation.
//
// Phase 10 Session 4 (MINIMUM VIABLE):
//
//   This file ports JUST ENOUGH of pi.dev's openai-codex-responses.ts
//   to let a user log in with ChatGPT Plus/Pro and actually send a
//   message. Specifically what works:
//
//     - Single-turn text streaming
//     - Multi-turn text WITHIN a session (no previous_response_id —
//       we send the full history each turn, like Anthropic/OpenAI Chat
//       Completions)
//     - Basic SSE event handling: response.output_text.delta,
//       response.completed, response.failed, error
//
//   What does NOT work (deferred):
//
//     - Tool calls. The Responses API has a different tool_call shape
//       than Chat Completions (function_call items in output, |-separated
//       call_id|item_id encoding with fc_ prefix requirement). Pi9 will
//       send tools in the request but the assembly logic for streaming
//       tool deltas isn't here. If the model emits a tool call, pi9
//       drops it on the floor and returns whatever text came before.
//     - Reasoning blocks (response.reasoning_*). The model's chain-of-
//       thought is silently dropped. Final text answers come through fine.
//     - Refusal blocks (response.refusal.delta). Refusals look like
//       streaming errors to the user.
//     - previous_response_id for conversation continuity. Pi.dev uses
//       this to let the server keep the context. We send full history
//       every turn — slightly more bandwidth but no state to manage.
//
//   The minimum-viable framing here means: a user can /login Codex
//   then "hello" → get a streamed text response. Anything fancier
//   probably fails ungracefully.
//
// References ported from pi.dev:
//   - packages/ai/src/providers/openai-codex-responses.ts (request body
//     shape, URL routing, headers)
//   - packages/ai/src/providers/openai-responses-shared.ts (message
//     conversion, SSE event types)
package provider

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
)

const (
	codexResponsesBaseURL = "https://chatgpt.com/backend-api"
	// `originator` is set on every request — pi.dev uses "pi"; we
	// use "pi9" so traffic is distinguishable in OpenAI's logs.
	codexResponsesOriginator = "pi9"
)

// codexResponsesProvider implements Provider for ChatGPT Plus/Pro
// via OpenAI's Responses API at chatgpt.com/backend-api/codex/responses.
//
// Selected at request time by ProviderForModel/Get: an OAuth token
// for ProviderOpenAI returns this provider instead of openaiCompat.
// API key OpenAI requests still use openaiCompat (api.openai.com
// /v1/chat/completions).
type codexResponsesProvider struct{}

func (codexResponsesProvider) Name() ProviderID { return ProviderOpenAI }

func (codexResponsesProvider) Stream(ctx context.Context, cfg Config, messages []Message) (<-chan Chunk, <-chan error) {
	chunks := make(chan Chunk, 8)
	errs := make(chan error, 1)

	go func() {
		defer close(chunks)
		defer close(errs)

		// Account ID is required in the chatgpt-account-id header.
		// It's stashed in auth.json's account_id field at login time.
		// We don't have direct access to the AuthEntry here — the
		// caller passes the account ID via cfg.APIURL field abuse
		// (see runStream wiring). If empty, the API returns a 400.
		// Cleaner would be a dedicated field on Config, but this is
		// minimum viable.
		accountID := codexAccountIDFromConfig(cfg)
		if accountID == "" {
			errs <- fmt.Errorf("codex: missing chatgpt-account-id. Try /login (oauth) again — the token didn't include account metadata")
			return
		}

		// Extract system prompt; Codex puts it in `instructions`, not
		// in the messages array.
		systemPrompt := extractSystemPrompt(messages)
		userMessages := stripSystemMessages(messages)

		// Build the Responses-format input array. Codex expects
		// `{type: "message", role: ..., content: [{type: "input_text"|"output_text", text: ...}]}`
		// for plain text turns. We keep it simple — no tool history,
		// no reasoning items.
		input := convertToResponsesInput(userMessages)

		body := map[string]interface{}{
			"model":                cfg.Model,
			"instructions":         systemPrompt,
			"input":                input,
			"store":                false,
			"stream":               true,
			"tool_choice":          "auto",
			"parallel_tool_calls":  true,
			"text":                 map[string]interface{}{"verbosity": "low"},
		}

		// Tools are advertised in the request but we don't parse
		// streamed tool calls. The model might call them anyway and
		// we'll just lose those calls. Documented limitation.
		if len(cfg.Tools) > 0 {
			body["tools"] = convertToolsForResponses(cfg.Tools)
		}

		buf, err := json.Marshal(body)
		if err != nil {
			errs <- fmt.Errorf("codex: marshal body: %w", err)
			return
		}

		req, err := http.NewRequestWithContext(ctx, "POST", codexResponsesBaseURL+"/codex/responses", bytes.NewReader(buf))
		if err != nil {
			errs <- fmt.Errorf("codex: new request: %w", err)
			return
		}
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("Accept", "text/event-stream")
		req.Header.Set("Authorization", "Bearer "+cfg.APIKey)
		req.Header.Set("chatgpt-account-id", accountID)
		req.Header.Set("OpenAI-Beta", "responses=experimental")
		req.Header.Set("originator", codexResponsesOriginator)
		// User-Agent mirrors pi.dev's format. The platform/release
		// strings would normally come from runtime/os; we just pin
		// "plan9" since this is the plan9 build.
		req.Header.Set("User-Agent", "pi9 (plan9)")

		client := httpClient()
		resp, err := client.Do(req)
		if err != nil {
			errs <- fmt.Errorf("codex: do: %w", err)
			return
		}
		defer resp.Body.Close()

		if resp.StatusCode != 200 {
			buf := make([]byte, 4096)
			n, _ := resp.Body.Read(buf)
			errs <- fmt.Errorf("codex: http %d: %s", resp.StatusCode, string(buf[:n]))
			return
		}

		if err := readResponsesSSE(resp.Body, chunks); err != nil {
			errs <- err
		}
	}()

	return chunks, errs
}

// codexAccountIDFromConfig pulls the chatgpt-account-id from the
// abused cfg.APIURL field. main.go's runStream sets cfg.APIURL =
// "codex:" + accountID before dispatching to this provider —
// avoids adding a dedicated field for one-provider metadata.
//
// Returns "" if the URL doesn't have the prefix (caller errors).
func codexAccountIDFromConfig(cfg Config) string {
	const prefix = "codex:"
	if !strings.HasPrefix(cfg.APIURL, prefix) {
		return ""
	}
	return cfg.APIURL[len(prefix):]
}

// extractSystemPrompt pulls the first system message's content out.
// Codex puts it in the top-level `instructions` field, not in input.
// Multiple system messages: only the first wins (matches our
// Anthropic provider's behavior, and the Responses API's design).
func extractSystemPrompt(messages []Message) string {
	for _, m := range messages {
		if m.Role == RoleSystem && m.Content != "" {
			return m.Content
		}
	}
	return "You are a helpful assistant."
}

// stripSystemMessages returns messages with system entries removed.
// Used to build the `input` array (which only takes user/assistant/tool).
func stripSystemMessages(messages []Message) []Message {
	out := make([]Message, 0, len(messages))
	for _, m := range messages {
		if m.Role == RoleSystem {
			continue
		}
		out = append(out, m)
	}
	return out
}

// convertToResponsesInput translates pi9's neutral messages to the
// Responses API's input array shape. Tool calls and tool results
// are SKIPPED — we don't handle them in the MVP, and including them
// with wrong IDs would cause server errors.
//
// Each message becomes one item:
//
//   user      → {type: "message", role: "user", content: [{type: "input_text", text}]}
//   assistant → {type: "message", role: "assistant", content: [{type: "output_text", text, annotations: []}]}
//   tool      → skipped
func convertToResponsesInput(messages []Message) []map[string]interface{} {
	input := make([]map[string]interface{}, 0, len(messages))
	for _, m := range messages {
		switch m.Role {
		case RoleUser:
			if m.Content == "" {
				continue
			}
			input = append(input, map[string]interface{}{
				"type": "message",
				"role": "user",
				"content": []map[string]interface{}{
					{"type": "input_text", "text": m.Content},
				},
			})
		case RoleAssistant:
			if m.Content == "" {
				// Skip empty assistant messages (could be turns
				// that only had tool calls — we don't preserve those).
				continue
			}
			input = append(input, map[string]interface{}{
				"type": "message",
				"role": "assistant",
				"content": []map[string]interface{}{
					{"type": "output_text", "text": m.Content, "annotations": []interface{}{}},
				},
				"status": "completed",
			})
		case RoleTool:
			// MVP limitation: tool history skipped. If the model
			// previously called a tool and we round-tripped the
			// result, sending the tool message back would require
			// proper function_call/function_call_output items with
			// matching call_id encoding. Documented as known break.
			continue
		}
	}
	return input
}

// convertToolsForResponses translates pi9's neutral Tool slice to the
// Responses API tool shape. Different from Chat Completions — flat
// {type: "function", name, description, parameters, strict: false}
// instead of {type: "function", function: {...}}.
func convertToolsForResponses(tools []Tool) []map[string]interface{} {
	out := make([]map[string]interface{}, 0, len(tools))
	for _, t := range tools {
		out = append(out, map[string]interface{}{
			"type":        "function",
			"name":        t.Name,
			"description": t.Description,
			"parameters":  t.Parameters,
			"strict":      false,
		})
	}
	return out
}

// readResponsesSSE consumes the Responses-API event stream and emits
// Chunks. Handled event types:
//
//   response.output_text.delta   → text chunk (the main one)
//   response.completed           → terminal success
//   response.failed              → terminal error
//   error                        → terminal error
//
// Ignored: response.created, response.output_item.added,
// response.content_part.added, response.reasoning_*, function_call_*
// (we lose tool calls in MVP), refusal_*.
func readResponsesSSE(r io.Reader, out chan<- Chunk) error {
	br := bufio.NewReaderSize(r, 64*1024)

	for {
		line, err := br.ReadString('\n')
		if err != nil {
			if err == io.EOF {
				out <- Chunk{Done: true}
				return nil
			}
			return err
		}
		line = strings.TrimRight(line, "\r\n")
		if !strings.HasPrefix(line, "data:") {
			continue
		}
		data := strings.TrimSpace(line[len("data:"):])
		if data == "" {
			continue
		}
		if data == "[DONE]" {
			out <- Chunk{Done: true}
			return nil
		}

		// Parse event type + payload.
		var ev struct {
			Type string `json:"type"`
			// response.output_text.delta:
			Delta string `json:"delta,omitempty"`
			// response.failed / error:
			Response struct {
				Status string `json:"status,omitempty"`
				Error  struct {
					Message string `json:"message,omitempty"`
				} `json:"error,omitempty"`
			} `json:"response,omitempty"`
			Message string `json:"message,omitempty"`
		}
		if err := json.Unmarshal([]byte(data), &ev); err != nil {
			// Skip unparseable — Responses API occasionally has events
			// with shapes we don't model.
			continue
		}

		switch ev.Type {
		case "response.output_text.delta":
			if ev.Delta != "" {
				out <- Chunk{Delta: ev.Delta}
			}
		case "response.completed":
			out <- Chunk{Done: true}
			return nil
		case "response.failed":
			msg := ev.Response.Error.Message
			if msg == "" {
				msg = "response.failed (no detail)"
			}
			out <- Chunk{Done: true, Err: fmt.Errorf("codex: response.failed: %s", msg)}
			return nil
		case "error":
			msg := ev.Message
			if msg == "" {
				msg = "error event (no detail)"
			}
			out <- Chunk{Done: true, Err: fmt.Errorf("codex: %s", msg)}
			return nil
		}
		// All other event types ignored. The MVP doesn't render
		// reasoning, tool calls, or refusals.
	}
}
