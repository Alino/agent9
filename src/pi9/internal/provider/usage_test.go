package provider

import (
	"strings"
	"testing"
)

func TestParseOpenAIUsage(t *testing.T) {
	if got := parseOpenAIUsage(nil); got != nil {
		t.Errorf("nil usage should parse to nil, got %+v", got)
	}
	if got := parseOpenAIUsage(&sseUsage{}); got != nil {
		t.Errorf("all-zero usage should parse to nil, got %+v", got)
	}
	got := parseOpenAIUsage(&sseUsage{PromptTokens: 10, CompletionTokens: 20, TotalTokens: 30})
	if got == nil || got.PromptTokens != 10 || got.CompletionTokens != 20 || got.TotalTokens != 30 {
		t.Errorf("usage mismatch: %+v", got)
	}
	// total computed when provider omits it
	got = parseOpenAIUsage(&sseUsage{PromptTokens: 5, CompletionTokens: 7})
	if got == nil || got.TotalTokens != 12 {
		t.Errorf("total should be computed: %+v", got)
	}
}

// drainSSE runs readSSE over the given raw stream and collects all
// chunks emitted. Helper for usage/reasoning parsing tests.
func drainSSE(t *testing.T, raw string) []Chunk {
	t.Helper()
	out := make(chan Chunk, 64)
	if err := readSSE(strings.NewReader(raw), out); err != nil {
		t.Fatalf("readSSE: %v", err)
	}
	close(out)
	var chunks []Chunk
	for c := range out {
		chunks = append(chunks, c)
	}
	return chunks
}

func TestReadSSE_UsageOnFinalChunk(t *testing.T) {
	// OpenAI include_usage shape: content, then a finish_reason chunk,
	// then a trailing choices-less chunk with usage, then [DONE].
	raw := strings.Join([]string{
		`data: {"choices":[{"delta":{"content":"hello"},"finish_reason":null}]}`,
		`data: {"choices":[{"delta":{},"finish_reason":"stop"}]}`,
		`data: {"choices":[],"usage":{"prompt_tokens":11,"completion_tokens":22,"total_tokens":33}}`,
		`data: [DONE]`,
		``,
	}, "\n")

	chunks := drainSSE(t, raw)

	var done *Chunk
	var content string
	for i := range chunks {
		if chunks[i].Delta != "" {
			content += chunks[i].Delta
		}
		if chunks[i].Done {
			done = &chunks[i]
		}
	}
	if content != "hello" {
		t.Errorf("content = %q, want hello", content)
	}
	if done == nil {
		t.Fatal("no Done chunk emitted")
	}
	if done.Usage == nil {
		t.Fatal("Done chunk missing Usage")
	}
	if done.Usage.PromptTokens != 11 || done.Usage.CompletionTokens != 22 || done.Usage.TotalTokens != 33 {
		t.Errorf("usage = %+v, want 11/22/33", done.Usage)
	}
}

func TestReadSSE_ReasoningDelta(t *testing.T) {
	raw := strings.Join([]string{
		`data: {"choices":[{"delta":{"reasoning":"thinking..."},"finish_reason":null}]}`,
		`data: {"choices":[{"delta":{"content":"answer"},"finish_reason":null}]}`,
		`data: [DONE]`,
		``,
	}, "\n")

	chunks := drainSSE(t, raw)

	var reasoning, content string
	for _, c := range chunks {
		reasoning += c.Reasoning
		content += c.Delta
	}
	if reasoning != "thinking..." {
		t.Errorf("reasoning = %q, want thinking...", reasoning)
	}
	if content != "answer" {
		t.Errorf("content = %q, want answer", content)
	}
}

func TestReadSSE_NoUsageWhenAbsent(t *testing.T) {
	raw := strings.Join([]string{
		`data: {"choices":[{"delta":{"content":"hi"},"finish_reason":"stop"}]}`,
		`data: [DONE]`,
		``,
	}, "\n")
	chunks := drainSSE(t, raw)
	for _, c := range chunks {
		if c.Done && c.Usage != nil {
			t.Errorf("usage should be nil when absent, got %+v", c.Usage)
		}
	}
}

func TestAnthropicUsageParsing(t *testing.T) {
	// message_start carries input_tokens; message_delta carries output.
	start := parseAnthropicEvent(`{"type":"message_start","message":{"usage":{"input_tokens":100,"output_tokens":1}}}`)
	if start == nil || start.Message.Usage.InputTokens != 100 {
		t.Fatalf("message_start parse: %+v", start)
	}
	delta := parseAnthropicEvent(`{"type":"message_delta","usage":{"output_tokens":250}}`)
	if delta == nil || delta.Usage.OutputTokens != 250 {
		t.Fatalf("message_delta parse: %+v", delta)
	}

	var u Usage
	u.PromptTokens = start.Message.Usage.InputTokens
	u.CompletionTokens = delta.Usage.OutputTokens
	final := anthropicUsage(u)
	if final == nil || final.PromptTokens != 100 || final.CompletionTokens != 250 || final.TotalTokens != 350 {
		t.Errorf("anthropicUsage = %+v, want 100/250/350", final)
	}

	if anthropicUsage(Usage{}) != nil {
		t.Error("empty usage should yield nil")
	}
}

func TestParseAnthropicEvent_Invalid(t *testing.T) {
	if parseAnthropicEvent("not json") != nil {
		t.Error("invalid JSON should parse to nil")
	}
	if parseAnthropicEvent(`{"type":"ping"}`) == nil {
		t.Error("valid JSON should parse")
	}
}
