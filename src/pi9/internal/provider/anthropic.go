// Package provider: Anthropic native API support.
//
// Anthropic does NOT speak OpenAI's /chat/completions format. The
// Anthropic Messages API has its own shape:
//
//   - Endpoint: POST https://api.anthropic.com/v1/messages
//   - Auth header: x-api-key: sk-ant-...
//   - Version header: anthropic-version: 2023-06-01
//   - Request body: { model, max_tokens, system?, messages[], tools? }
//   - Messages: { role: "user"|"assistant", content: string | content[] }
//   - Tool calls are content blocks (type=tool_use) within an
//     assistant message; tool results are user messages with content
//     blocks (type=tool_result, tool_use_id, content).
//   - Streaming events: message_start, content_block_start/delta/stop,
//     message_delta, message_stop. text comes from
//     content_block_delta where delta.type=="text_delta".
//
// Pi9's neutral Message/Tool/ToolCall types translate cleanly enough
// — we adapt at the wire boundary.
//
// Reference: https://docs.anthropic.com/en/api/messages-streaming
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

// interleavedThinkingBeta is the anthropic-beta feature flag that lets
// Claude interleave thinking with tool use. Pi.dev appends it by default
// for non-adaptive thinking models; we send it whenever extended
// thinking is enabled on a request. Matches anthropic.ts's
// INTERLEAVED_THINKING_BETA.
const interleavedThinkingBeta = "interleaved-thinking-2025-05-14"

// anthropic implements Provider against the native Anthropic API.
type anthropic struct{}

func (anthropic) Name() ProviderID { return ProviderAnthropic }

func (anthropic) Stream(ctx context.Context, cfg Config, messages []Message) (<-chan Chunk, <-chan error) {
	chunks := make(chan Chunk, 8)
	errs := make(chan error, 1)

	url := cfg.APIURL
	if url == "" {
		url = "https://api.anthropic.com/v1/messages"
	}

	// OAuth tokens look like `sk-ant-oat01-...`. When detected, we
	// switch to the Claude Code identity headers AND prepend the
	// required "You are Claude Code" system message — Anthropic
	// rejects OAuth requests that don't claim Claude Code identity.
	// API keys (`sk-ant-api03-...`) use the standard x-api-key path.
	isOAuth := strings.Contains(cfg.APIKey, "sk-ant-oat")

	go func() {
		defer close(chunks)
		defer close(errs)

		// Prepend Claude Code identity to system prompt if using OAuth.
		// We mutate a copy of messages so the upstream history stays
		// clean.
		msgsToSend := messages
		if isOAuth {
			msgsToSend = withClaudeCodeIdentity(messages)
		}

		body, err := buildAnthropicBody(cfg, msgsToSend)
		if err != nil {
			errs <- fmt.Errorf("anthropic: build body: %w", err)
			return
		}

		req, err := http.NewRequestWithContext(ctx, "POST", url, bytes.NewReader(body))
		if err != nil {
			errs <- fmt.Errorf("anthropic: new request: %w", err)
			return
		}
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("Accept", "text/event-stream")
		req.Header.Set("anthropic-version", "2023-06-01")

		// Interleaved-thinking beta: when extended thinking is enabled,
		// pi.dev appends "interleaved-thinking-2025-05-14" to anthropic-beta
		// by default for non-adaptive thinking models (every pi9 thinking
		// request). Only add it when thinking is on, so non-thinking turns
		// are unaffected.
		thinkingOn := thinkingEnabled(cfg.ThinkingLevel) && levelToBudget(cfg.ThinkingLevel) > 0

		if isOAuth {
			// Claude Code identity. Match pi.dev's anthropic.ts:
			// `anthropic-beta: claude-code-20250219,oauth-2025-04-20`
			// plus user-agent + x-app: cli. No x-api-key.
			req.Header.Set("Authorization", "Bearer "+cfg.APIKey)
			beta := "claude-code-20250219,oauth-2025-04-20"
			if thinkingOn {
				beta += "," + interleavedThinkingBeta
			}
			req.Header.Set("anthropic-beta", beta)
			req.Header.Set("user-agent", "claude-cli/2.1.75")
			req.Header.Set("x-app", "cli")
		} else {
			req.Header.Set("x-api-key", cfg.APIKey)
			if thinkingOn {
				req.Header.Set("anthropic-beta", interleavedThinkingBeta)
			}
		}

		client := httpClient()
		resp, err := client.Do(req)
		if err != nil {
			errs <- fmt.Errorf("anthropic: do: %w", err)
			return
		}
		defer resp.Body.Close()

		if resp.StatusCode != 200 {
			buf := make([]byte, 4096)
			n, _ := resp.Body.Read(buf)
			errs <- fmt.Errorf("anthropic: http %d: %s", resp.StatusCode, string(buf[:n]))
			return
		}

		if err := readAnthropicSSE(resp.Body, chunks); err != nil {
			errs <- err
		}
	}()

	return chunks, errs
}

// withClaudeCodeIdentity prepends the magic system message Anthropic
// requires for OAuth-authenticated requests. If the user has their
// own system prompt, it stays — Anthropic concatenates multiple
// system entries (we only collapse to one in buildAnthropicBody by
// taking the FIRST, so we prepend ours and keep the user's after).
//
// Pi.dev's implementation puts this as a content block with
// cache_control set; we keep it simpler — just a string system
// message that buildAnthropicBody handles.
func withClaudeCodeIdentity(messages []Message) []Message {
	out := make([]Message, 0, len(messages)+1)
	out = append(out, Message{
		Role:    RoleSystem,
		Content: "You are Claude Code, Anthropic's official CLI for Claude.",
	})
	out = append(out, messages...)
	return out
}

// anthropicReqBody is the wire shape for /v1/messages.
type anthropicReqBody struct {
	Model     string             `json:"model"`
	MaxTokens int                `json:"max_tokens"`
	System    string             `json:"system,omitempty"`
	Messages  []anthropicMessage `json:"messages"`
	Tools     []anthropicTool    `json:"tools,omitempty"`
	Stream    bool               `json:"stream"`
	Thinking  *anthropicThinking `json:"thinking,omitempty"`
}

// anthropicThinking enables extended thinking. budget_tokens is the
// number of tokens Claude may spend reasoning before answering; it must
// be smaller than max_tokens (buildAnthropicBody raises max_tokens if
// needed). Sent only when Config.ThinkingLevel is set and non-off.
type anthropicThinking struct {
	Type         string `json:"type"` // always "enabled"
	BudgetTokens int    `json:"budget_tokens"`
}

// anthropicMessage is one message in the Anthropic shape. Content can
// be a plain string (typical user) or a list of content blocks
// (assistant w/ tool_use, user w/ tool_result). We use json.RawMessage
// so we can switch shape per message without two struct types.
type anthropicMessage struct {
	Role    string          `json:"role"`
	Content json.RawMessage `json:"content"`
}

// anthropicTool is a tool advertised to the model. Anthropic uses
// input_schema (not parameters) for the JSON Schema field, and the
// outer wrapper has name/description at the top level (no type=
// "function" wrapping like OpenAI).
type anthropicTool struct {
	Name        string                 `json:"name"`
	Description string                 `json:"description,omitempty"`
	InputSchema map[string]interface{} `json:"input_schema"`
}

// buildAnthropicBody translates pi9's neutral types into Anthropic's
// request body. The system message is extracted (Anthropic puts it at
// top level, not in messages[]).
func buildAnthropicBody(cfg Config, messages []Message) ([]byte, error) {
	body := anthropicReqBody{
		Model:     cfg.Model,
		MaxTokens: cfg.MaxTokens,
		Stream:    true,
	}
	if body.MaxTokens == 0 {
		body.MaxTokens = 4096
	}

	// Extended thinking: when ThinkingLevel is set (and not off), enable
	// thinking with a budget derived from the level. budget_tokens must
	// be strictly below max_tokens, so raise max_tokens to leave room for
	// at least minThinkingOutput tokens of answer if it's too small.
	if thinkingEnabled(cfg.ThinkingLevel) {
		if budget := levelToBudget(cfg.ThinkingLevel); budget > 0 {
			const minThinkingOutput = 1024
			if body.MaxTokens <= budget {
				body.MaxTokens = budget + minThinkingOutput
			}
			body.Thinking = &anthropicThinking{
				Type:         "enabled",
				BudgetTokens: budget,
			}
		}
	}

	for _, m := range messages {
		switch m.Role {
		case RoleSystem:
			// First system wins; Anthropic only supports one.
			if body.System == "" {
				body.System = m.Content
			}
			continue

		case RoleUser:
			c, _ := json.Marshal(m.Content)
			body.Messages = append(body.Messages, anthropicMessage{
				Role: "user", Content: c,
			})

		case RoleAssistant:
			// Assistant may have text content + tool_use blocks.
			// If only text, send as plain string. If tool_calls
			// present, send as block list.
			if len(m.ToolCalls) == 0 {
				c, _ := json.Marshal(m.Content)
				body.Messages = append(body.Messages, anthropicMessage{
					Role: "assistant", Content: c,
				})
				continue
			}
			var blocks []map[string]interface{}
			if m.Content != "" {
				blocks = append(blocks, map[string]interface{}{
					"type": "text", "text": m.Content,
				})
			}
			for _, tc := range m.ToolCalls {
				var args interface{}
				_ = json.Unmarshal([]byte(tc.Function.Arguments), &args)
				blocks = append(blocks, map[string]interface{}{
					"type":  "tool_use",
					"id":    tc.ID,
					"name":  tc.Function.Name,
					"input": args,
				})
			}
			c, _ := json.Marshal(blocks)
			body.Messages = append(body.Messages, anthropicMessage{
				Role: "assistant", Content: c,
			})

		case RoleTool:
			// Tool result. Anthropic puts these as user messages
			// with a tool_result content block.
			blocks := []map[string]interface{}{
				{
					"type":        "tool_result",
					"tool_use_id": m.ToolCallID,
					"content":     m.Content,
				},
			}
			c, _ := json.Marshal(blocks)
			body.Messages = append(body.Messages, anthropicMessage{
				Role: "user", Content: c,
			})
		}
	}

	if len(cfg.Tools) > 0 {
		for _, t := range cfg.Tools {
			body.Tools = append(body.Tools, anthropicTool{
				Name:        t.Name,
				Description: t.Description,
				InputSchema: t.Parameters,
			})
		}
	}

	return json.Marshal(body)
}

// readAnthropicSSE consumes Anthropic's streaming events and emits
// Chunks. The event types we care about:
//
//	message_start              — initial; ignore (just metadata)
//	content_block_start        — new content block; if tool_use, capture id/name
//	content_block_delta        — text_delta or input_json_delta
//	content_block_stop         — block done; finalize tool_use args if any
//	message_delta              — stop_reason etc.
//	message_stop               — terminal
//
// Anthropic SSE uses both `event:` and `data:` lines. We parse the
// data JSON and switch on its "type" field rather than relying on
// the event: line.
func readAnthropicSSE(r io.Reader, out chan<- Chunk) error {
	br := bufio.NewReaderSize(r, 64*1024)

	// In-progress tool calls, indexed by content_block index.
	// toolBuilder is package-level (defined at end of file) so
	// finalizeAnthropicTools can be a non-nested helper.
	tools := map[int]*toolBuilder{}

	// Running token usage. input_tokens arrives in message_start;
	// output_tokens in message_delta (and is updated as the turn
	// progresses, last value wins).
	var usage Usage

	for {
		line, err := br.ReadString('\n')
		if err != nil {
			if err == io.EOF {
				out <- Chunk{Done: true, ToolCalls: finalizeAnthropicTools(tools), Usage: anthropicUsage(usage)}
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

		ev := parseAnthropicEvent(data)
		if ev == nil {
			// Skip unknown / unparseable events rather than blowing
			// up the stream. Anthropic occasionally adds new event
			// types and pre-existing parsers should keep working.
			continue
		}

		switch ev.Type {
		case "message_start":
			// Initial usage snapshot: input + any prompt-cache counts
			// are already accounted in input_tokens by Anthropic; we
			// fold them into PromptTokens.
			usage.PromptTokens = ev.Message.Usage.InputTokens
			if ev.Message.Usage.OutputTokens > 0 {
				usage.CompletionTokens = ev.Message.Usage.OutputTokens
			}
		case "content_block_start":
			if ev.ContentBlock.Type == "tool_use" {
				tb := &toolBuilder{
					id:   ev.ContentBlock.ID,
					name: ev.ContentBlock.Name,
				}
				if len(ev.ContentBlock.Input) > 0 && string(ev.ContentBlock.Input) != "{}" {
					tb.argsBuf.Write(ev.ContentBlock.Input)
				}
				tools[ev.Index] = tb
			}
		case "content_block_delta":
			switch ev.Delta.Type {
			case "text_delta":
				if ev.Delta.Text != "" {
					out <- Chunk{Delta: ev.Delta.Text}
				}
			case "thinking_delta":
				// Extended-thinking reasoning text. Emit as Reasoning
				// so the UI can render it separately from the answer.
				if ev.Delta.Thinking != "" {
					out <- Chunk{Reasoning: ev.Delta.Thinking}
				}
			case "input_json_delta":
				if tb, ok := tools[ev.Index]; ok {
					tb.argsBuf.WriteString(ev.Delta.PartialJSON)
				}
			}
		case "message_delta":
			// Final usage update — output_tokens is authoritative here.
			if ev.Usage.OutputTokens > 0 {
				usage.CompletionTokens = ev.Usage.OutputTokens
			}
			if ev.Usage.InputTokens > 0 {
				usage.PromptTokens = ev.Usage.InputTokens
			}
		case "message_stop":
			out <- Chunk{Done: true, ToolCalls: finalizeAnthropicTools(tools), Usage: anthropicUsage(usage)}
			return nil
		}
	}
}

// anthropicEvent is the parsed shape of one Anthropic SSE data payload.
// We model only the fields pi9 consumes; everything else is ignored.
type anthropicEvent struct {
	Type  string `json:"type"`
	Index int    `json:"index"`

	// message_start
	Message struct {
		Usage anthropicUsageWire `json:"usage"`
	} `json:"message,omitempty"`

	// content_block_start
	ContentBlock struct {
		Type  string          `json:"type"`
		ID    string          `json:"id,omitempty"`
		Name  string          `json:"name,omitempty"`
		Input json.RawMessage `json:"input,omitempty"`
	} `json:"content_block,omitempty"`

	// content_block_delta
	Delta struct {
		Type        string `json:"type"`
		Text        string `json:"text,omitempty"`
		Thinking    string `json:"thinking,omitempty"`
		PartialJSON string `json:"partial_json,omitempty"`
		StopReason  string `json:"stop_reason,omitempty"`
	} `json:"delta,omitempty"`

	// message_delta
	Usage anthropicUsageWire `json:"usage,omitempty"`
}

// anthropicUsageWire is the raw usage block Anthropic sends in
// message_start and message_delta events.
type anthropicUsageWire struct {
	InputTokens  int `json:"input_tokens"`
	OutputTokens int `json:"output_tokens"`
}

// parseAnthropicEvent decodes one SSE data payload. Returns nil if the
// payload isn't valid JSON (caller skips it). Pure + testable.
func parseAnthropicEvent(data string) *anthropicEvent {
	var ev anthropicEvent
	if err := json.Unmarshal([]byte(data), &ev); err != nil {
		return nil
	}
	return &ev
}

// anthropicUsage finalizes a running Usage into a *Usage for the Done
// chunk, computing TotalTokens. Returns nil if no tokens were reported
// (so callers that never saw usage emit Usage=nil rather than zeros).
func anthropicUsage(u Usage) *Usage {
	if u.PromptTokens == 0 && u.CompletionTokens == 0 {
		return nil
	}
	u.TotalTokens = u.PromptTokens + u.CompletionTokens
	return &u
}

// finalizeAnthropicTools converts the in-progress builder map into
// the neutral ToolCall slice pi9 expects.
func finalizeAnthropicTools(tools map[int]*toolBuilder) []ToolCall {
	if len(tools) == 0 {
		return nil
	}
	out := make([]ToolCall, 0, len(tools))
	for _, tb := range tools {
		args := tb.argsBuf.String()
		if args == "" {
			args = "{}"
		}
		out = append(out, ToolCall{
			ID:   tb.id,
			Type: "function",
			Function: ToolCallFn{
				Name:      tb.name,
				Arguments: args,
			},
		})
	}
	return out
}

// toolBuilder is defined inside readAnthropicSSE; promoted here so
// finalizeAnthropicTools can take it as a parameter without nesting.
type toolBuilder struct {
	id      string
	name    string
	argsBuf strings.Builder
}
