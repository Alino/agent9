// Package provider: OpenAI Codex (ChatGPT Plus/Pro) Responses API implementation.
//
// Phase 10 (Task 3.1) — TOOL CALLS:
//
//	This file ports pi.dev's openai-codex-responses.ts (plus the shared
//	openai-responses-shared.ts stream helper) so a user can /login with
//	ChatGPT Plus/Pro and run the full agent loop, including tool calls.
//	What works:
//
//	  - Single-turn text streaming
//	  - Multi-turn text WITHIN a session (no previous_response_id —
//	    we send the full history each turn, like Anthropic/OpenAI Chat
//	    Completions; see "previous_response_id" note below)
//	  - TOOL CALLS. The Responses API emits function_call items in the
//	    output stream rather than Chat-Completions-style tool_calls
//	    deltas. We parse response.output_item.added (function_call),
//	    accumulate arguments via response.function_call_arguments.delta,
//	    finalize on response.function_call_arguments.done /
//	    response.output_item.done, and surface them as provider.ToolCall
//	    entries on the terminal (Done) Chunk — exactly like
//	    openrouter.go / anthropic.go. The agent loop in main.go then
//	    executes them and feeds the results back. Tool history (prior
//	    function_call + function_call_output items) is round-tripped in
//	    the input array using the call_id|item_id encoding so multi-turn
//	    tool conversations work.
//	  - Token usage parsed into Chunk.Usage from response.completed.
//	  - Reasoning deltas parsed into Chunk.Reasoning from
//	    response.reasoning_text.delta and
//	    response.reasoning_summary_text.delta.
//	  - SSE event handling: response.output_text.delta, function_call
//	    events, response.completed/done/incomplete, response.failed,
//	    and bare error events.
//
//	What does NOT work (deferred):
//
//	  - Refusal blocks (response.refusal.delta). Refusals look like
//	    streaming errors to the user.
//	  - Image inputs / image tool results. Text only.
//	  - previous_response_id for server-side conversation continuity.
//	    Pi.dev only uses this on its WebSocket continuation path; the
//	    streaming HTTP path (which is what we port) sends full history
//	    every turn. We do the same: slightly more bandwidth, but no
//	    server state to manage and tool round-trips stay self-contained.
//
// References ported from pi.dev:
//   - packages/ai/src/providers/openai-codex-responses.ts (request body
//     shape, URL routing, headers, mapCodexEvents)
//   - packages/ai/src/providers/openai-responses-shared.ts (message
//     conversion, tool conversion, processResponsesStream event types)
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
		// for plain text turns, plus function_call / function_call_output
		// items for tool round-trips.
		input := convertToResponsesInput(userMessages)

		body := map[string]interface{}{
			"model":               cfg.Model,
			"instructions":        systemPrompt,
			"input":               input,
			"store":               false,
			"stream":              true,
			"tool_choice":         "auto",
			"parallel_tool_calls": true,
			"text":                map[string]interface{}{"verbosity": "low"},
		}

		// Honor the thinking level (matches upstream buildRequestBody's
		// reasoning block). Codex/gpt-5 accepts "xhigh" directly, so we use
		// the Codex-aware mapping that passes it through (the generic Chat
		// Completions mapping collapses xhigh->high). Returns "" for off, in
		// which case we omit the field so non-reasoning use is unaffected.
		if eff := levelToCodexReasoningEffort(cfg.ThinkingLevel); eff != "" {
			body["reasoning"] = map[string]interface{}{"effort": eff, "summary": "auto"}
		}

		// Advertise tools so the model can call them. parallel_tool_calls
		// stays true (matches upstream buildRequestBody) — the agent loop
		// in main.go executes the assembled calls and feeds results back.
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

// splitCodexToolCallID splits pi9's stored tool-call ID into the
// Responses API's (call_id, item_id) pair. We encode assembled calls
// as "call_id|item_id" (see assembleResponsesEvent); on the way back
// out we split them so function_call / function_call_output items use
// the right field. If there's no "|", the whole string is the call_id
// and the item_id is empty (the API tolerates an omitted item id).
func splitCodexToolCallID(id string) (callID, itemID string) {
	if i := strings.IndexByte(id, '|'); i >= 0 {
		return id[:i], id[i+1:]
	}
	return id, ""
}

// convertToResponsesInput translates pi9's neutral messages to the
// Responses API's input array shape. Tool calls and tool results ARE
// round-tripped now (Task 3.1) so multi-turn tool conversations work:
//
//	user      → {type: "message", role: "user", content: [{type: "input_text", text}]}
//	assistant → {type: "message", role: "assistant", content: [{type: "output_text", text, annotations: []}]}
//	            plus one {type: "function_call", call_id, id, name, arguments} per tool call
//	tool      → {type: "function_call_output", call_id, output}
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
			// Text first (if any), then the function_call items. The
			// Responses API wants assistant text and tool calls as
			// separate output items, in order.
			if m.Content != "" {
				input = append(input, map[string]interface{}{
					"type": "message",
					"role": "assistant",
					"content": []map[string]interface{}{
						{"type": "output_text", "text": m.Content, "annotations": []interface{}{}},
					},
					"status": "completed",
				})
			}
			for _, tc := range m.ToolCalls {
				callID, itemID := splitCodexToolCallID(tc.ID)
				args := tc.Function.Arguments
				if args == "" {
					args = "{}"
				}
				item := map[string]interface{}{
					"type":      "function_call",
					"call_id":   callID,
					"name":      tc.Function.Name,
					"arguments": args,
				}
				// The Responses API requires the item id to start with
				// "fc_"; only include it when we have one (assembled
				// calls always do). Omitting it is allowed and avoids
				// pairing-validation errors for foreign IDs.
				if itemID != "" {
					item["id"] = itemID
				}
				input = append(input, item)
			}
		case RoleTool:
			callID, _ := splitCodexToolCallID(m.ToolCallID)
			input = append(input, map[string]interface{}{
				"type":    "function_call_output",
				"call_id": callID,
				"output":  m.Content,
			})
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

// ---------- streaming SSE parsing ----------

// responsesEvent is the union of Responses-API stream event fields we
// care about. The API sends many event types; this struct models just
// the ones assembleResponsesEvent acts on. Unmodeled fields are
// ignored by encoding/json.
type responsesEvent struct {
	Type string `json:"type"`

	// response.output_text.delta / response.reasoning_text.delta /
	// response.reasoning_summary_text.delta / function_call_arguments.delta
	Delta string `json:"delta,omitempty"`

	// response.function_call_arguments.done carries the full arguments.
	Arguments string `json:"arguments,omitempty"`

	// response.output_item.added / .done. ItemID/CallID tie the event
	// to a specific in-flight tool call; OutputIndex orders items.
	ItemID      string             `json:"item_id,omitempty"`
	OutputIndex int                `json:"output_index,omitempty"`
	Item        *responsesOutItem  `json:"item,omitempty"`
	Response    *responsesResponse `json:"response,omitempty"`

	// bare "error" event
	Code    string `json:"code,omitempty"`
	Message string `json:"message,omitempty"`
}

// responsesOutItem is the `item` payload on output_item.added/.done.
type responsesOutItem struct {
	Type      string `json:"type"` // "message" | "function_call" | "reasoning"
	ID        string `json:"id,omitempty"`
	CallID    string `json:"call_id,omitempty"`
	Name      string `json:"name,omitempty"`
	Arguments string `json:"arguments,omitempty"`
}

// responsesResponse is the `response` payload on response.completed /
// response.failed.
type responsesResponse struct {
	ID     string          `json:"id,omitempty"`
	Status string          `json:"status,omitempty"`
	Usage  *responsesUsage `json:"usage,omitempty"`
	Error  *struct {
		Code    string `json:"code,omitempty"`
		Message string `json:"message,omitempty"`
	} `json:"error,omitempty"`
}

// responsesUsage is the Responses-API usage block. cached_tokens are
// INCLUDED in input_tokens, so we subtract to report non-cached input
// (matches processResponsesStream).
type responsesUsage struct {
	InputTokens        int `json:"input_tokens,omitempty"`
	OutputTokens       int `json:"output_tokens,omitempty"`
	TotalTokens        int `json:"total_tokens,omitempty"`
	InputTokensDetails struct {
		CachedTokens int `json:"cached_tokens,omitempty"`
	} `json:"input_tokens_details,omitempty"`
}

// parseResponsesUsage maps a Responses-API usage block to the neutral
// Usage. Returns nil when the block is nil or reports no tokens. Pure +
// testable. PromptTokens reports non-cached input; cached tokens are
// folded into the total via the provider's own total_tokens.
func parseResponsesUsage(u *responsesUsage) *Usage {
	if u == nil {
		return nil
	}
	if u.InputTokens == 0 && u.OutputTokens == 0 && u.TotalTokens == 0 {
		return nil
	}
	cached := u.InputTokensDetails.CachedTokens
	prompt := u.InputTokens - cached
	if prompt < 0 {
		prompt = u.InputTokens
	}
	total := u.TotalTokens
	if total == 0 {
		total = u.InputTokens + u.OutputTokens
	}
	return &Usage{
		PromptTokens:     prompt,
		CompletionTokens: u.OutputTokens,
		TotalTokens:      total,
	}
}

// responsesAssembler holds the running state while parsing one
// Responses-API stream: text/reasoning are emitted as deltas, while
// tool calls are accumulated and surfaced only on the terminal chunk
// (matching openrouter.go / anthropic.go and what the agent loop
// expects).
type responsesAssembler struct {
	// toolCalls in encounter order, keyed by output_index so deltas
	// can find the right in-flight call. The Responses API streams one
	// function_call's argument deltas contiguously, but we key by index
	// to be robust against interleaving.
	toolCalls map[int]*ToolCall
	order     []int
	usage     *Usage
}

func newResponsesAssembler() *responsesAssembler {
	return &responsesAssembler{toolCalls: map[int]*ToolCall{}}
}

// toolCallAt fetches (creating if needed) the in-flight ToolCall for an
// output index, preserving first-seen order.
func (a *responsesAssembler) toolCallAt(idx int) *ToolCall {
	tc, ok := a.toolCalls[idx]
	if !ok {
		tc = &ToolCall{Type: "function"}
		a.toolCalls[idx] = tc
		a.order = append(a.order, idx)
	}
	return tc
}

// flushTools returns the assembled tool calls in encounter order.
func (a *responsesAssembler) flushTools() []ToolCall {
	if len(a.order) == 0 {
		return nil
	}
	out := make([]ToolCall, 0, len(a.order))
	for _, idx := range a.order {
		out = append(out, *a.toolCalls[idx])
	}
	return out
}

// assembleResponsesEvent applies one parsed Responses-API event to the
// assembler and returns the streaming Chunks it produces (text or
// reasoning deltas — never tool calls; those land on the terminal
// chunk). `done` is true when the event terminates the stream;
// `termErr` is set if it terminated with an error.
//
// This is the pure, testable core of the SSE loop: no IO, no channels.
func (a *responsesAssembler) assembleResponsesEvent(ev *responsesEvent) (out []Chunk, done bool, termErr error) {
	switch ev.Type {
	case "response.output_text.delta":
		if ev.Delta != "" {
			out = append(out, Chunk{Delta: ev.Delta})
		}

	case "response.reasoning_text.delta", "response.reasoning_summary_text.delta":
		if ev.Delta != "" {
			out = append(out, Chunk{Reasoning: ev.Delta})
		}

	case "response.output_item.added":
		// A new output item begins. For function_call items, seed the
		// in-flight tool call with its ids/name and any initial args.
		if ev.Item != nil && ev.Item.Type == "function_call" {
			tc := a.toolCallAt(ev.OutputIndex)
			tc.ID = encodeCodexToolCallID(ev.Item.CallID, ev.Item.ID)
			if ev.Item.Name != "" {
				tc.Function.Name = ev.Item.Name
			}
			if ev.Item.Arguments != "" {
				tc.Function.Arguments = ev.Item.Arguments
			}
		}

	case "response.function_call_arguments.delta":
		if ev.Delta != "" {
			tc := a.toolCallAt(ev.OutputIndex)
			tc.Function.Arguments += ev.Delta
		}

	case "response.function_call_arguments.done":
		// The full arguments string. Authoritative — replace whatever
		// the deltas accumulated (they should match, but the .done
		// value is canonical, matching processResponsesStream).
		if ev.Arguments != "" {
			tc := a.toolCallAt(ev.OutputIndex)
			tc.Function.Arguments = ev.Arguments
		}

	case "response.output_item.done":
		// Finalize a function_call item: make sure ids/name/args are
		// set even if we missed the .added/.delta events.
		if ev.Item != nil && ev.Item.Type == "function_call" {
			tc := a.toolCallAt(ev.OutputIndex)
			if tc.ID == "" {
				tc.ID = encodeCodexToolCallID(ev.Item.CallID, ev.Item.ID)
			}
			if tc.Function.Name == "" {
				tc.Function.Name = ev.Item.Name
			}
			if tc.Function.Arguments == "" && ev.Item.Arguments != "" {
				tc.Function.Arguments = ev.Item.Arguments
			}
		}

	case "response.completed", "response.done", "response.incomplete":
		if ev.Response != nil {
			if u := parseResponsesUsage(ev.Response.Usage); u != nil {
				a.usage = u
			}
		}
		done = true

	case "response.failed":
		msg := "response.failed (no detail)"
		if ev.Response != nil && ev.Response.Error != nil && ev.Response.Error.Message != "" {
			msg = ev.Response.Error.Message
		}
		done = true
		termErr = fmt.Errorf("codex: response.failed: %s", msg)

	case "error":
		msg := ev.Message
		if msg == "" {
			msg = ev.Code
		}
		if msg == "" {
			msg = "error event (no detail)"
		}
		done = true
		termErr = fmt.Errorf("codex: %s", msg)
	}
	return out, done, termErr
}

// encodeCodexToolCallID joins the Responses-API (call_id, item_id) pair
// into pi9's single ToolCall.ID, using "|" as the separator. The item
// id is needed when round-tripping the call back to the API (it must
// start with "fc_"); the call id is what function_call_output pairs on.
// When item_id is empty we just use the call_id alone.
func encodeCodexToolCallID(callID, itemID string) string {
	if itemID == "" {
		return callID
	}
	return callID + "|" + itemID
}

// readResponsesSSE consumes the Responses-API event stream and emits
// Chunks. Text/reasoning deltas stream as they arrive; assembled tool
// calls and final usage ride on the terminal Done chunk (like
// openrouter.go / anthropic.go), so the agent loop can execute the
// calls and feed results back.
//
// The per-event logic lives in assembleResponsesEvent (pure +
// unit-tested); this function only does line framing and channel IO.
func readResponsesSSE(r io.Reader, out chan<- Chunk) error {
	br := bufio.NewReaderSize(r, 64*1024)
	asm := newResponsesAssembler()

	emit := func(termErr error) {
		out <- Chunk{Done: true, ToolCalls: asm.flushTools(), Usage: asm.usage, Err: termErr}
	}

	for {
		line, err := br.ReadString('\n')
		if err != nil {
			if err == io.EOF {
				emit(nil)
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
			emit(nil)
			return nil
		}

		var ev responsesEvent
		if err := json.Unmarshal([]byte(data), &ev); err != nil {
			// Skip unparseable — Responses API occasionally has events
			// with shapes we don't model.
			continue
		}

		chunks, done, termErr := asm.assembleResponsesEvent(&ev)
		for _, c := range chunks {
			out <- c
		}
		if done {
			if termErr != nil {
				// Surface stream-level failures via the RETURN value so
				// Stream forwards them on the errs channel — matching
				// anthropic.go / openrouter.go. Consumers (runStream,
				// streamOnce, headless) read errs, not Chunk.Err, so a
				// Done chunk here would look like an empty successful turn
				// and the error (e.g. a ChatGPT usage limit) would vanish.
				return termErr
			}
			emit(nil)
			return nil
		}
	}
}
