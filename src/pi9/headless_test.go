package main

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/alino/plan9-winxp/pi9/internal/chat"
	"github.com/alino/plan9-winxp/pi9/internal/provider"
)

// fakeProvider is a scripted provider.Provider for headless tests. Each
// call to Stream pops the next scripted turn off the queue and replays
// its chunks, so a multi-turn (tool round-trip) conversation can be
// driven with no network. It records the messages it was handed each
// turn so tests can assert the tool result was fed back to the model.
type fakeProvider struct {
	turns    [][]provider.Chunk // one slice of chunks per Stream call
	calls    int                // how many times Stream was called
	gotMsgs  [][]provider.Message
	failWith error // if set, the first Stream errors instead of streaming
}

func (f *fakeProvider) Name() provider.ProviderID { return provider.ProviderOpenRouter }

func (f *fakeProvider) Stream(ctx context.Context, cfg provider.Config, msgs []provider.Message) (<-chan provider.Chunk, <-chan error) {
	chunks := make(chan provider.Chunk)
	errs := make(chan error, 1)

	// Snapshot the messages this turn was asked to answer.
	cp := make([]provider.Message, len(msgs))
	copy(cp, msgs)
	f.gotMsgs = append(f.gotMsgs, cp)

	turn := f.calls
	f.calls++

	go func() {
		defer close(chunks)
		if f.failWith != nil && turn == 0 {
			errs <- f.failWith
			return
		}
		if turn >= len(f.turns) {
			// Nothing scripted: emit an empty terminal chunk so the loop
			// treats it as a final, tool-less answer.
			chunks <- provider.Chunk{Done: true}
			return
		}
		for _, c := range f.turns[turn] {
			chunks <- c
		}
	}()
	return chunks, errs
}

// loopProvider is a provider.Provider that ALWAYS returns the same tool
// call, on every Stream call, and never produces a tool-less final
// answer. It exists to exercise the runaway-tool-loop iteration cap
// (M13): without the cap, runHeadless would call Stream forever.
type loopProvider struct {
	calls int
}

func (l *loopProvider) Name() provider.ProviderID { return provider.ProviderOpenRouter }

func (l *loopProvider) Stream(ctx context.Context, cfg provider.Config, msgs []provider.Message) (<-chan provider.Chunk, <-chan error) {
	chunks := make(chan provider.Chunk)
	errs := make(chan error, 1)
	l.calls++
	go func() {
		defer close(chunks)
		chunks <- provider.Chunk{Delta: "looping "}
		chunks <- provider.Chunk{Done: true, ToolCalls: []provider.ToolCall{{
			ID:       "loop_call",
			Type:     "function",
			Function: provider.ToolCallFn{Name: "ls", Arguments: `{"path":"."}`},
		}}}
	}()
	return chunks, errs
}

// collectEvents runs runHeadless against the fake and returns the
// ordered event log plus the loop error.
func collectEvents(t *testing.T, prov provider.Provider, history *chat.History) ([]headlessEvent, error) {
	t.Helper()
	var events []headlessEvent
	err := runHeadless(context.Background(), prov, provider.Config{Model: "test"}, history, func(ev headlessEvent) {
		events = append(events, ev)
	})
	return events, err
}

func eventTypes(events []headlessEvent) []string {
	out := make([]string, len(events))
	for i, e := range events {
		out[i] = e.Type
	}
	return out
}

// TestRunHeadlessToolRoundTrip is the core test: turn 1 emits text and
// requests a tool, turn 2 (after the tool runs) emits final text and no
// tools. We assert the tool executed, its result was fed back to the
// model, the final assembled text is correct, and the event sequence is
// well-formed.
func TestRunHeadlessToolRoundTrip(t *testing.T) {
	fp := &fakeProvider{
		turns: [][]provider.Chunk{
			// Turn 1: some text, then a tool call on the terminal chunk.
			{
				{Delta: "Let me check. "},
				{Done: true, ToolCalls: []provider.ToolCall{{
					ID:   "call_1",
					Type: "function",
					Function: provider.ToolCallFn{
						Name:      "ls",
						Arguments: `{"path":"."}`,
					},
				}}},
			},
			// Turn 2: final answer, no tools.
			{
				{Delta: "Done!"},
				{Done: true},
			},
		},
	}

	history := &chat.History{System: "sys"}
	history.AppendUser("what's here?")

	events, err := collectEvents(t, fp, history)
	if err != nil {
		t.Fatalf("runHeadless error: %v", err)
	}

	// Provider was called exactly twice (initial turn + after tool).
	if fp.calls != 2 {
		t.Fatalf("expected 2 Stream calls, got %d", fp.calls)
	}

	// The 2nd Stream call must have been fed the assistant tool_call and
	// the tool result message, proving the round-trip closed.
	second := fp.gotMsgs[1]
	var sawAssistantCall, sawToolResult bool
	for _, m := range second {
		if m.Role == provider.RoleAssistant {
			for _, tc := range m.ToolCalls {
				if tc.ID == "call_1" && tc.Function.Name == "ls" {
					sawAssistantCall = true
				}
			}
		}
		if m.Role == provider.RoleTool && m.ToolCallID == "call_1" {
			sawToolResult = true
		}
	}
	if !sawAssistantCall {
		t.Errorf("2nd turn missing assistant tool_call message: %+v", second)
	}
	if !sawToolResult {
		t.Errorf("2nd turn missing tool result message: %+v", second)
	}

	// Event sequence sanity: starts with agent_start, ends with
	// agent_end, contains the tool execution pair and two message turns.
	types := eventTypes(events)
	if types[0] != "agent_start" {
		t.Errorf("first event = %q, want agent_start", types[0])
	}
	if types[len(types)-1] != "agent_end" {
		t.Errorf("last event = %q, want agent_end", types[len(types)-1])
	}
	if got := countType(events, "tool_execution_start"); got != 1 {
		t.Errorf("tool_execution_start count = %d, want 1", got)
	}
	if got := countType(events, "tool_execution_end"); got != 1 {
		t.Errorf("tool_execution_end count = %d, want 1", got)
	}
	if got := countType(events, "message_start"); got != 2 {
		t.Errorf("message_start count = %d, want 2", got)
	}

	// tool_execution_end must carry the call id + name and not be an error
	// (listing "." is always valid).
	te := firstEvent(events, "tool_execution_end")
	if te == nil || te.ToolCallID != "call_1" || te.ToolName != "ls" {
		t.Errorf("tool_execution_end event malformed: %+v", te)
	}
	if te.IsError {
		t.Errorf("ls . should not error, got result: %q", string(te.Result))
	}

	// H5: turn lifecycle events bracket each agent turn (one per
	// iteration), and tool_execution_start carries args as a raw JSON
	// value (an object), not a pre-serialized string.
	if got := countType(events, "turn_start"); got != 2 {
		t.Errorf("turn_start count = %d, want 2", got)
	}
	if got := countType(events, "turn_end"); got != 2 {
		t.Errorf("turn_end count = %d, want 2", got)
	}
	ts := firstEvent(events, "tool_execution_start")
	if ts == nil {
		t.Fatal("missing tool_execution_start")
	}
	var argObj map[string]any
	if err := json.Unmarshal(ts.Args, &argObj); err != nil {
		t.Errorf("tool_execution_start args not a JSON object: %q: %v", string(ts.Args), err)
	} else if argObj["path"] != "." {
		t.Errorf("tool_execution_start args = %v, want path=.", argObj)
	}

	// turn_end for the tool round carries the assistant message + tool
	// results with raw-JSON results.
	turnEnds := allEvents(events, "turn_end")
	if len(turnEnds) > 0 {
		first := turnEnds[0]
		if len(first.ToolResults) != 1 || first.ToolResults[0].ToolCallID != "call_1" {
			t.Errorf("first turn_end toolResults malformed: %+v", first.ToolResults)
		}
	}

	// message_end must carry the full message object, not a flat text
	// field. The first message_end holds the turn-1 text + the tool call.
	me := firstEvent(events, "message_end")
	if me == nil || me.Message == nil {
		t.Fatalf("message_end missing nested message: %+v", me)
	}
	if me.Message.Role != "assistant" || me.Message.Content != "Let me check. " {
		t.Errorf("message_end message = %+v, want assistant/'Let me check. '", me.Message)
	}
	if len(me.Message.ToolCalls) != 1 || me.Message.ToolCalls[0].ID != "call_1" {
		t.Errorf("message_end message.toolCalls malformed: %+v", me.Message.ToolCalls)
	}

	// Final assistant text on the last turn is the 2nd-turn text.
	last := history.Turns[len(history.Turns)-1]
	if last.Assistant != "Let me check. Done!" {
		t.Errorf("final assistant text = %q, want %q", last.Assistant, "Let me check. Done!")
	}

	// The tool invocation was recorded on the turn.
	if len(last.Calls) != 1 || last.Calls[0].Name != "ls" {
		t.Errorf("expected 1 recorded ls call, got %+v", last.Calls)
	}
}

// TestRunHeadlessNoTools: a single text-only turn finishes in one round.
func TestRunHeadlessNoTools(t *testing.T) {
	fp := &fakeProvider{
		turns: [][]provider.Chunk{
			{
				{Delta: "Hello"},
				{Delta: " world"},
				{Done: true},
			},
		},
	}
	history := &chat.History{System: "sys"}
	history.AppendUser("hi")

	events, err := collectEvents(t, fp, history)
	if err != nil {
		t.Fatalf("runHeadless error: %v", err)
	}
	if fp.calls != 1 {
		t.Fatalf("expected 1 Stream call, got %d", fp.calls)
	}
	if got := countType(events, "tool_execution_start"); got != 0 {
		t.Errorf("expected no tool executions, got %d", got)
	}
	end := firstEvent(events, "message_end")
	if end == nil || end.Message == nil || end.Message.Content != "Hello world" {
		t.Errorf("message_end message = %v, want content %q", end, "Hello world")
	}

	// H5: a text-only run still emits a message_update carrying a
	// text_delta assistantMessageEvent (not a flat top-level text field).
	mu := firstEvent(events, "message_update")
	if mu == nil || mu.AssistantMessageEvent == nil {
		t.Fatalf("message_update missing assistantMessageEvent: %+v", mu)
	}
	if mu.AssistantMessageEvent.Type != "text_delta" || mu.AssistantMessageEvent.Delta != "Hello" {
		t.Errorf("first message_update event = %+v, want text_delta/'Hello'", mu.AssistantMessageEvent)
	}
	if mu.Message == nil || mu.Message.Content != "Hello" {
		t.Errorf("message_update message = %+v, want content 'Hello'", mu.Message)
	}
	if history.Turns[0].Assistant != "Hello world" {
		t.Errorf("history text = %q", history.Turns[0].Assistant)
	}
}

// TestRunHeadlessStreamError: a streaming error aborts the loop and is
// reported on agent_end.
func TestRunHeadlessStreamError(t *testing.T) {
	fp := &fakeProvider{failWith: context.DeadlineExceeded}
	history := &chat.History{System: "sys"}
	history.AppendUser("hi")

	events, err := collectEvents(t, fp, history)
	if err == nil {
		t.Fatal("expected error, got nil")
	}
	end := firstEvent(events, "agent_end")
	if end == nil || end.Error == "" {
		t.Errorf("agent_end should carry error, got %+v", end)
	}
	// History's last turn records the error too.
	if history.Turns[len(history.Turns)-1].Err == nil {
		t.Error("expected turn error recorded")
	}
}

// TestRunHeadlessToolLoopCap (M13): a provider that ALWAYS returns a
// tool call never terminates on its own. This is the only infinite-loop
// guard for non-interactive use: runHeadless must stop at the iteration
// cap, return the cap error, and finish the turn (Err recorded). Without
// the cap, this test would hang.
func TestRunHeadlessToolLoopCap(t *testing.T) {
	// A single scripted turn that requests a tool; loopProvider replays
	// it on EVERY Stream call so the model never produces a tool-less
	// final answer.
	fp := &loopProvider{}

	history := &chat.History{System: "sys"}
	history.AppendUser("go forever")

	events, err := collectEvents(t, fp, history)
	if err == nil {
		t.Fatal("expected cap error, got nil")
	}
	if !strings.Contains(err.Error(), "max tool iterations") {
		t.Errorf("error = %v, want runaway-tool-loop cap error", err)
	}

	// The provider was called exactly headlessMaxIterations times: the
	// loop stopped at the cap rather than spinning forever.
	if fp.calls != headlessMaxIterations {
		t.Errorf("Stream calls = %d, want cap %d", fp.calls, headlessMaxIterations)
	}

	// Each iteration ran the tool once: cap-many tool executions.
	if got := countType(events, "tool_execution_start"); got != headlessMaxIterations {
		t.Errorf("tool_execution_start count = %d, want %d", got, headlessMaxIterations)
	}

	// agent_end carries the cap error.
	end := firstEvent(events, "agent_end")
	if end == nil || end.Error == "" {
		t.Fatalf("agent_end should carry cap error, got %+v", end)
	}

	// The turn was finished with the cap error recorded (M13: the turn
	// is finished, not left dangling).
	last := history.Turns[len(history.Turns)-1]
	if last.Err == nil {
		t.Error("expected cap error recorded on the turn")
	}
	if last.Finished.IsZero() {
		t.Error("expected the turn to be marked finished")
	}
}

// TestRunHeadlessThinkingDelta (H5): reasoning chunks surface as a
// message_update carrying a thinking_delta assistantMessageEvent (the
// invented "reasoning_update" type is gone).
func TestRunHeadlessThinkingDelta(t *testing.T) {
	fp := &fakeProvider{
		turns: [][]provider.Chunk{
			{
				{Reasoning: "hmm"},
				{Delta: "answer"},
				{Done: true},
			},
		},
	}
	history := &chat.History{System: "sys"}
	history.AppendUser("think")

	events, err := collectEvents(t, fp, history)
	if err != nil {
		t.Fatalf("runHeadless error: %v", err)
	}

	// No legacy reasoning_update events.
	if got := countType(events, "reasoning_update"); got != 0 {
		t.Errorf("reasoning_update count = %d, want 0 (removed)", got)
	}

	// Exactly one message_update is a thinking_delta.
	var sawThinking bool
	for _, ev := range allEvents(events, "message_update") {
		if ev.AssistantMessageEvent != nil && ev.AssistantMessageEvent.Type == "thinking_delta" {
			if ev.AssistantMessageEvent.Delta != "hmm" {
				t.Errorf("thinking_delta delta = %q, want %q", ev.AssistantMessageEvent.Delta, "hmm")
			}
			sawThinking = true
		}
	}
	if !sawThinking {
		t.Error("no thinking_delta assistantMessageEvent emitted for reasoning chunk")
	}
}

// TestRunPrintMode: the print emitter writes assistant text to out and
// tool markers to errOut. Uses the same tool round-trip script.
func TestRunPrintMode(t *testing.T) {
	fp := &fakeProvider{
		turns: [][]provider.Chunk{
			{
				{Delta: "checking "},
				{Done: true, ToolCalls: []provider.ToolCall{{
					ID:       "c1",
					Type:     "function",
					Function: provider.ToolCallFn{Name: "ls", Arguments: `{"path":"."}`},
				}}},
			},
			{
				{Delta: "all good"},
				{Done: true},
			},
		},
	}
	var out, errOut strings.Builder
	err := runPrintMode(context.Background(), fp, provider.Config{Model: "test"}, "sys", "prompt", &out, &errOut)
	if err != nil {
		t.Fatalf("runPrintMode error: %v", err)
	}
	if !strings.Contains(out.String(), "checking ") || !strings.Contains(out.String(), "all good") {
		t.Errorf("stdout missing assistant text: %q", out.String())
	}
	if !strings.Contains(errOut.String(), "[tool] ls") {
		t.Errorf("stderr missing tool marker: %q", errOut.String())
	}
	// Tool result text should NOT leak to stdout.
	if strings.Contains(out.String(), "[tool]") {
		t.Errorf("tool markers leaked to stdout: %q", out.String())
	}
}

// TestRunJSONMode: every emitted event is a valid JSON line, starting
// with a session header, and the event types appear in order.
func TestRunJSONMode(t *testing.T) {
	fp := &fakeProvider{
		turns: [][]provider.Chunk{
			{
				{Delta: "hi"},
				{Done: true},
			},
		},
	}
	var out strings.Builder
	err := runJSONMode(context.Background(), fp, provider.Config{Model: "test"}, "sys", "prompt", "sess123", "/tmp", &out)
	if err != nil {
		t.Fatalf("runJSONMode error: %v", err)
	}

	lines := strings.Split(strings.TrimSpace(out.String()), "\n")
	if len(lines) < 4 {
		t.Fatalf("expected several JSONL lines, got %d: %q", len(lines), out.String())
	}

	// Line 1 is the session header.
	var hdr map[string]any
	if err := json.Unmarshal([]byte(lines[0]), &hdr); err != nil {
		t.Fatalf("header not valid JSON: %v", err)
	}
	if hdr["type"] != "session" || hdr["id"] != "sess123" {
		t.Errorf("bad session header: %v", hdr)
	}
	// M12: header must include version (3) and a timestamp matching pi,
	// and must NOT carry a top-level "mode" field.
	if v, ok := hdr["version"].(float64); !ok || v != 3 {
		t.Errorf("header version = %v, want 3", hdr["version"])
	}
	if ts, ok := hdr["timestamp"].(string); !ok || ts == "" {
		t.Errorf("header missing timestamp: %v", hdr["timestamp"])
	}
	if _, ok := hdr["mode"]; ok {
		t.Errorf("header should not carry top-level mode field: %v", hdr)
	}

	// Every remaining line is a valid headlessEvent.
	var types []string
	for _, ln := range lines[1:] {
		var ev headlessEvent
		if err := json.Unmarshal([]byte(ln), &ev); err != nil {
			t.Fatalf("event line not valid JSON: %q: %v", ln, err)
		}
		types = append(types, ev.Type)
	}
	if types[0] != "agent_start" {
		t.Errorf("first event = %q, want agent_start", types[0])
	}
	if types[len(types)-1] != "agent_end" {
		t.Errorf("last event = %q, want agent_end", types[len(types)-1])
	}
}

// TestResolveHeadlessPrompt covers prompt precedence: args, stdin, and
// the merge of both.
func TestResolveHeadlessPrompt(t *testing.T) {
	t.Run("args only", func(t *testing.T) {
		got := resolveHeadlessPrompt("", []string{"hello", "world"}, strings.NewReader(""), false)
		if got != "hello world" {
			t.Errorf("got %q", got)
		}
	})
	t.Run("stdin only", func(t *testing.T) {
		got := resolveHeadlessPrompt("", nil, strings.NewReader("piped text\n"), true)
		if got != "piped text" {
			t.Errorf("got %q", got)
		}
	})
	t.Run("args plus stdin merged", func(t *testing.T) {
		got := resolveHeadlessPrompt("", []string{"summarize"}, strings.NewReader("the file body"), true)
		if !strings.Contains(got, "summarize") || !strings.Contains(got, "the file body") {
			t.Errorf("merge missing parts: %q", got)
		}
	})
	t.Run("stdin not a pipe is ignored", func(t *testing.T) {
		got := resolveHeadlessPrompt("", []string{"just args"}, strings.NewReader("ignored"), false)
		if got != "just args" {
			t.Errorf("got %q", got)
		}
	})
}

func countType(events []headlessEvent, typ string) int {
	n := 0
	for _, e := range events {
		if e.Type == typ {
			n++
		}
	}
	return n
}

func firstEvent(events []headlessEvent, typ string) *headlessEvent {
	for i := range events {
		if events[i].Type == typ {
			return &events[i]
		}
	}
	return nil
}

func allEvents(events []headlessEvent, typ string) []headlessEvent {
	var out []headlessEvent
	for i := range events {
		if events[i].Type == typ {
			out = append(out, events[i])
		}
	}
	return out
}
