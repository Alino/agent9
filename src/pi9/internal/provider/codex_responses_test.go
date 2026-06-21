package provider

import (
	"strings"
	"testing"
)

// drainResponsesSSE runs readResponsesSSE over a raw event stream and
// collects every Chunk emitted. No network — pure parser exercise.
func drainResponsesSSE(t *testing.T, raw string) []Chunk {
	t.Helper()
	out := make(chan Chunk, 128)
	if err := readResponsesSSE(strings.NewReader(raw), out); err != nil {
		t.Fatalf("readResponsesSSE: %v", err)
	}
	close(out)
	var chunks []Chunk
	for c := range out {
		chunks = append(chunks, c)
	}
	return chunks
}

// drainResponsesSSEErr is like drainResponsesSSE but returns the terminal
// error instead of failing the test, for exercising stream-failure paths.
func drainResponsesSSEErr(raw string) ([]Chunk, error) {
	out := make(chan Chunk, 128)
	err := readResponsesSSE(strings.NewReader(raw), out)
	close(out)
	var chunks []Chunk
	for c := range out {
		chunks = append(chunks, c)
	}
	return chunks, err
}

func TestResponsesSSE_TextDeltas(t *testing.T) {
	raw := strings.Join([]string{
		`data: {"type":"response.created","response":{"id":"resp_1"}}`,
		`data: {"type":"response.output_item.added","output_index":0,"item":{"type":"message","id":"msg_1"}}`,
		`data: {"type":"response.output_text.delta","delta":"Hel"}`,
		`data: {"type":"response.output_text.delta","delta":"lo"}`,
		`data: {"type":"response.completed","response":{"id":"resp_1","status":"completed","usage":{"input_tokens":12,"output_tokens":3,"total_tokens":15}}}`,
		``,
	}, "\n")

	chunks := drainResponsesSSE(t, raw)

	var text string
	var done *Chunk
	for i := range chunks {
		text += chunks[i].Delta
		if chunks[i].Done {
			done = &chunks[i]
		}
	}
	if text != "Hello" {
		t.Errorf("text = %q, want Hello", text)
	}
	if done == nil {
		t.Fatal("no Done chunk")
	}
	if len(done.ToolCalls) != 0 {
		t.Errorf("expected no tool calls, got %d", len(done.ToolCalls))
	}
	if done.Usage == nil || done.Usage.PromptTokens != 12 || done.Usage.CompletionTokens != 3 || done.Usage.TotalTokens != 15 {
		t.Errorf("usage = %+v, want 12/3/15", done.Usage)
	}
}

func TestResponsesSSE_FunctionCallAssembly(t *testing.T) {
	// The Responses API emits a function_call output item, then streams
	// its arguments as deltas, then a .done with the full arguments.
	raw := strings.Join([]string{
		`data: {"type":"response.created","response":{"id":"resp_2"}}`,
		`data: {"type":"response.output_item.added","output_index":0,"item":{"type":"function_call","id":"fc_abc","call_id":"call_xyz","name":"read","arguments":""}}`,
		`data: {"type":"response.function_call_arguments.delta","output_index":0,"delta":"{\"path\":"}`,
		`data: {"type":"response.function_call_arguments.delta","output_index":0,"delta":"\"/tmp\"}"}`,
		`data: {"type":"response.function_call_arguments.done","output_index":0,"arguments":"{\"path\":\"/tmp\"}"}`,
		`data: {"type":"response.output_item.done","output_index":0,"item":{"type":"function_call","id":"fc_abc","call_id":"call_xyz","name":"read","arguments":"{\"path\":\"/tmp\"}"}}`,
		`data: {"type":"response.completed","response":{"id":"resp_2","status":"completed","usage":{"input_tokens":40,"output_tokens":8,"total_tokens":48}}}`,
		``,
	}, "\n")

	chunks := drainResponsesSSE(t, raw)

	var done *Chunk
	for i := range chunks {
		if chunks[i].Done {
			done = &chunks[i]
		}
	}
	if done == nil {
		t.Fatal("no Done chunk")
	}
	if len(done.ToolCalls) != 1 {
		t.Fatalf("expected 1 tool call, got %d", len(done.ToolCalls))
	}
	tc := done.ToolCalls[0]
	if tc.ID != "call_xyz|fc_abc" {
		t.Errorf("tool call ID = %q, want call_xyz|fc_abc", tc.ID)
	}
	if tc.Type != "function" {
		t.Errorf("tool call Type = %q, want function", tc.Type)
	}
	if tc.Function.Name != "read" {
		t.Errorf("tool call name = %q, want read", tc.Function.Name)
	}
	if tc.Function.Arguments != `{"path":"/tmp"}` {
		t.Errorf("tool call args = %q, want {\"path\":\"/tmp\"}", tc.Function.Arguments)
	}
}

func TestResponsesSSE_ParallelToolCalls(t *testing.T) {
	// Two function_call items at distinct output indexes; deltas may
	// interleave. Both must come back in encounter order.
	raw := strings.Join([]string{
		`data: {"type":"response.output_item.added","output_index":0,"item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"read","arguments":""}}`,
		`data: {"type":"response.output_item.added","output_index":1,"item":{"type":"function_call","id":"fc_2","call_id":"call_2","name":"ls","arguments":""}}`,
		`data: {"type":"response.function_call_arguments.delta","output_index":1,"delta":"{\"dir\":\".\"}"}`,
		`data: {"type":"response.function_call_arguments.delta","output_index":0,"delta":"{\"path\":\"a\"}"}`,
		`data: {"type":"response.completed","response":{"status":"completed"}}`,
		``,
	}, "\n")

	chunks := drainResponsesSSE(t, raw)
	var done *Chunk
	for i := range chunks {
		if chunks[i].Done {
			done = &chunks[i]
		}
	}
	if done == nil {
		t.Fatal("no Done chunk")
	}
	if len(done.ToolCalls) != 2 {
		t.Fatalf("expected 2 tool calls, got %d", len(done.ToolCalls))
	}
	if done.ToolCalls[0].Function.Name != "read" || done.ToolCalls[0].Function.Arguments != `{"path":"a"}` {
		t.Errorf("call[0] = %+v", done.ToolCalls[0])
	}
	if done.ToolCalls[1].Function.Name != "ls" || done.ToolCalls[1].Function.Arguments != `{"dir":"."}` {
		t.Errorf("call[1] = %+v", done.ToolCalls[1])
	}
}

func TestResponsesSSE_TextThenToolCall(t *testing.T) {
	// Mixed turn: assistant says something, then calls a tool.
	raw := strings.Join([]string{
		`data: {"type":"response.output_item.added","output_index":0,"item":{"type":"message","id":"msg_1"}}`,
		`data: {"type":"response.output_text.delta","delta":"Let me check."}`,
		`data: {"type":"response.output_item.added","output_index":1,"item":{"type":"function_call","id":"fc_9","call_id":"call_9","name":"grep","arguments":""}}`,
		`data: {"type":"response.function_call_arguments.done","output_index":1,"arguments":"{\"q\":\"foo\"}"}`,
		`data: {"type":"response.completed","response":{"status":"completed"}}`,
		``,
	}, "\n")

	chunks := drainResponsesSSE(t, raw)
	var text string
	var done *Chunk
	for i := range chunks {
		text += chunks[i].Delta
		if chunks[i].Done {
			done = &chunks[i]
		}
	}
	if text != "Let me check." {
		t.Errorf("text = %q", text)
	}
	if done == nil || len(done.ToolCalls) != 1 {
		t.Fatalf("expected 1 tool call on Done, got %+v", done)
	}
	if done.ToolCalls[0].Function.Name != "grep" || done.ToolCalls[0].Function.Arguments != `{"q":"foo"}` {
		t.Errorf("tool call = %+v", done.ToolCalls[0])
	}
	if done.ToolCalls[0].ID != "call_9|fc_9" {
		t.Errorf("ID = %q, want call_9|fc_9", done.ToolCalls[0].ID)
	}
}

func TestResponsesSSE_ReasoningDelta(t *testing.T) {
	raw := strings.Join([]string{
		`data: {"type":"response.reasoning_text.delta","delta":"think "}`,
		`data: {"type":"response.reasoning_summary_text.delta","delta":"summary"}`,
		`data: {"type":"response.output_text.delta","delta":"answer"}`,
		`data: {"type":"response.completed","response":{"status":"completed"}}`,
		``,
	}, "\n")

	chunks := drainResponsesSSE(t, raw)
	var reasoning, text string
	for _, c := range chunks {
		reasoning += c.Reasoning
		text += c.Delta
	}
	if reasoning != "think summary" {
		t.Errorf("reasoning = %q, want 'think summary'", reasoning)
	}
	if text != "answer" {
		t.Errorf("text = %q, want answer", text)
	}
}

func TestResponsesSSE_Failed(t *testing.T) {
	raw := strings.Join([]string{
		`data: {"type":"response.failed","response":{"error":{"code":"server_error","message":"boom"}}}`,
		``,
	}, "\n")
	_, err := drainResponsesSSEErr(raw)
	if err == nil {
		t.Fatal("expected terminal error to be returned (forwarded to errs channel)")
	}
	if !strings.Contains(err.Error(), "boom") {
		t.Errorf("error = %v, want it to contain boom", err)
	}
}

func TestResponsesSSE_ErrorEvent(t *testing.T) {
	raw := strings.Join([]string{
		`data: {"type":"error","code":"rate_limited","message":"slow down"}`,
		``,
	}, "\n")
	_, err := drainResponsesSSEErr(raw)
	if err == nil {
		t.Fatal("expected terminal error to be returned (forwarded to errs channel)")
	}
	if !strings.Contains(err.Error(), "slow down") {
		t.Errorf("error = %v, want it to contain 'slow down'", err)
	}
}

func TestResponsesSSE_DoneSentinel(t *testing.T) {
	// [DONE] sentinel terminates the stream even without response.completed.
	raw := strings.Join([]string{
		`data: {"type":"response.output_text.delta","delta":"hi"}`,
		`data: [DONE]`,
		``,
	}, "\n")
	chunks := drainResponsesSSE(t, raw)
	var text string
	var sawDone bool
	for _, c := range chunks {
		text += c.Delta
		if c.Done {
			sawDone = true
		}
	}
	if text != "hi" || !sawDone {
		t.Errorf("text=%q sawDone=%v, want hi/true", text, sawDone)
	}
}

func TestParseResponsesUsage(t *testing.T) {
	if got := parseResponsesUsage(nil); got != nil {
		t.Errorf("nil usage should parse to nil, got %+v", got)
	}
	if got := parseResponsesUsage(&responsesUsage{}); got != nil {
		t.Errorf("all-zero usage should parse to nil, got %+v", got)
	}
	// cached tokens are subtracted from input to report non-cached input.
	u := &responsesUsage{InputTokens: 100, OutputTokens: 20, TotalTokens: 120}
	u.InputTokensDetails.CachedTokens = 30
	got := parseResponsesUsage(u)
	if got == nil || got.PromptTokens != 70 || got.CompletionTokens != 20 || got.TotalTokens != 120 {
		t.Errorf("usage = %+v, want prompt=70 completion=20 total=120", got)
	}
	// total computed when provider omits it.
	got = parseResponsesUsage(&responsesUsage{InputTokens: 5, OutputTokens: 7})
	if got == nil || got.TotalTokens != 12 {
		t.Errorf("total should be computed: %+v", got)
	}
}

func TestConvertToResponsesInput_ToolRoundTrip(t *testing.T) {
	// An assistant turn with a tool call, then the tool result, must
	// produce a function_call item (split id) and a function_call_output
	// item paired by call_id.
	messages := []Message{
		{Role: RoleUser, Content: "read /tmp/x"},
		{Role: RoleAssistant, ToolCalls: []ToolCall{{
			ID:       "call_xyz|fc_abc",
			Type:     "function",
			Function: ToolCallFn{Name: "read", Arguments: `{"path":"/tmp/x"}`},
		}}},
		{Role: RoleTool, ToolCallID: "call_xyz|fc_abc", Content: "file contents"},
	}

	input := convertToResponsesInput(messages)
	if len(input) != 3 {
		t.Fatalf("expected 3 input items, got %d: %+v", len(input), input)
	}

	// item[0]: user message
	if input[0]["type"] != "message" || input[0]["role"] != "user" {
		t.Errorf("input[0] = %+v, want user message", input[0])
	}

	// item[1]: function_call with split ids
	fc := input[1]
	if fc["type"] != "function_call" {
		t.Fatalf("input[1] type = %v, want function_call", fc["type"])
	}
	if fc["call_id"] != "call_xyz" {
		t.Errorf("call_id = %v, want call_xyz", fc["call_id"])
	}
	if fc["id"] != "fc_abc" {
		t.Errorf("id = %v, want fc_abc", fc["id"])
	}
	if fc["name"] != "read" {
		t.Errorf("name = %v, want read", fc["name"])
	}
	if fc["arguments"] != `{"path":"/tmp/x"}` {
		t.Errorf("arguments = %v", fc["arguments"])
	}

	// item[2]: function_call_output paired by call_id (item id dropped)
	fo := input[2]
	if fo["type"] != "function_call_output" {
		t.Fatalf("input[2] type = %v, want function_call_output", fo["type"])
	}
	if fo["call_id"] != "call_xyz" {
		t.Errorf("output call_id = %v, want call_xyz", fo["call_id"])
	}
	if fo["output"] != "file contents" {
		t.Errorf("output = %v", fo["output"])
	}
}

func TestSplitCodexToolCallID(t *testing.T) {
	c, i := splitCodexToolCallID("call_1|fc_1")
	if c != "call_1" || i != "fc_1" {
		t.Errorf("split = %q/%q, want call_1/fc_1", c, i)
	}
	c, i = splitCodexToolCallID("call_only")
	if c != "call_only" || i != "" {
		t.Errorf("split = %q/%q, want call_only/empty", c, i)
	}
}
