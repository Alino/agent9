package main

// Task 3.2 headless operation modes.
//
// pi9's day-to-day UX is the Bubble Tea TUI in main.go. But for
// scripting, CI, and piping into other tools we also need a
// non-interactive path: run a prompt, stream the answer, exit. This
// file adds that WITHOUT touching the working TUI loop.
//
// Two surfaces, both built on one synchronous agent loop:
//
//   -p / --print     one-shot: assistant TEXT to stdout, tool markers
//                    to stderr, exit 0.
//   --mode json      same loop, but a JSONL event stream to stdout
//                    (agent_start, message_*, tool_execution_*, ...),
//                    modelled on pi's docs/json.md event names.
//
// The agent loop (runHeadless) takes a provider.Provider as an
// interface so tests can inject a scripted fake with no network. It
// reuses the same chat.History append helpers the TUI uses, so the
// provider-message shape (assistant + tool turns) is identical.

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strings"
	"time"

	"github.com/alino/plan9-winxp/pi9/internal/chat"
	"github.com/alino/plan9-winxp/pi9/internal/provider"
	"github.com/alino/plan9-winxp/pi9/internal/store"
	"github.com/alino/plan9-winxp/pi9/internal/tools"
)

// headlessMaxIterations caps how many model<->tool round-trips a single
// runHeadless call will perform before giving up. Mirrors the TUI's
// maxTurnLoops so behaviour matches across modes; prevents a runaway
// tool loop from spinning forever in a non-interactive context.
const headlessMaxIterations = maxTurnLoops

// headlessMessage is the nested message object carried by the
// message_* / turn_end events. It mirrors pi's AgentMessage shape
// closely enough for downstream consumers: a role, the accumulated
// text content, and any tool calls the assistant turn requested.
//
// Content is the full assistant text as assembled so far (it grows on
// each message_update and is complete on message_end), matching
// docs/json.md where every message_* event carries the message object.
type headlessMessage struct {
	Role      string             `json:"role"`
	Content   string             `json:"content"`
	ToolCalls []headlessToolCall `json:"toolCalls,omitempty"`
}

// headlessToolCall is one tool call on a message object, as a raw JSON
// args value (not a pre-serialized string) per docs/json.md.
type headlessToolCall struct {
	ID   string          `json:"id"`
	Name string          `json:"name"`
	Args json.RawMessage `json:"args,omitempty"`
}

// assistantMessageEvent is the per-delta sub-event nested inside a
// message_update, matching docs/json.md's AssistantMessageEvent. Its
// Type is "text_delta" for assistant text or "thinking_delta" for
// reasoning; Delta carries the incremental chunk.
type assistantMessageEvent struct {
	Type  string `json:"type"`  // "text_delta" | "thinking_delta"
	Delta string `json:"delta"` // the incremental text
}

// headlessEvent is one observable event emitted by runHeadless. It is
// the single channel both print mode and JSON mode consume — print mode
// renders a human view, JSON mode marshals it verbatim to a JSONL line.
//
// Event types (conforming to pi's docs/json.md):
//
//	agent_start          run begins
//	turn_start           an agent turn begins
//	message_start        a new assistant message begins (Message populated)
//	message_update       an assistant delta (Message + AssistantMessageEvent populated)
//	message_end          the assistant message is complete (Message populated)
//	turn_end             an agent turn finished (Message + ToolResults populated)
//	tool_execution_start a tool call is about to run (ToolCallID/ToolName/Args)
//	tool_execution_end   a tool call finished (Result/IsError populated)
//	agent_end            run is finished (Error populated if it failed)
//
// docs/json.md models the message_* events as carrying a nested message
// object plus, for message_update, an assistantMessageEvent. pi9's turn
// model collapses an agent turn into a single message stream, so we map
// one runHeadless iteration to one turn_start/turn_end pair wrapping one
// message lifecycle. Tool args/results are emitted as raw JSON values
// (json.RawMessage) rather than pre-serialized strings.
//
// Fields are omitempty so each line carries only what's relevant to its
// type. The JSON tags are the stable wire contract for --mode json.
type headlessEvent struct {
	Type                  string                 `json:"type"`
	Message               *headlessMessage       `json:"message,omitempty"`               // message_* / turn_end
	AssistantMessageEvent *assistantMessageEvent `json:"assistantMessageEvent,omitempty"` // message_update
	ToolCallID            string                 `json:"toolCallId,omitempty"`            // tool_execution_*
	ToolName              string                 `json:"toolName,omitempty"`              // tool_execution_*
	Args                  json.RawMessage        `json:"args,omitempty"`                  // tool_execution_start (raw JSON args)
	Result                json.RawMessage        `json:"result,omitempty"`                // tool_execution_end (raw JSON result)
	IsError               bool                   `json:"isError,omitempty"`               // tool_execution_end
	ToolResults           []headlessToolResult   `json:"toolResults,omitempty"`           // turn_end
	Error                 string                 `json:"error,omitempty"`                 // agent_end
}

// headlessToolResult is one entry in a turn_end event's toolResults
// array, mirroring docs/json.md's ToolResultMessage shape (raw JSON
// result value, not a pre-serialized string).
type headlessToolResult struct {
	ToolCallID string          `json:"toolCallId"`
	Result     json.RawMessage `json:"result,omitempty"`
	IsError    bool            `json:"isError,omitempty"`
}

// rawJSON wraps a tool args/result string as a json.RawMessage. Tool
// args are already JSON; tool results may be arbitrary text. If s is not
// valid JSON we encode it as a JSON string so the field is always a
// well-formed JSON value (never a syntax error on the wire). An empty
// string yields a nil RawMessage so the field is omitted.
func rawJSON(s string) json.RawMessage {
	if s == "" {
		return nil
	}
	if json.Valid([]byte(s)) {
		return json.RawMessage(s)
	}
	b, err := json.Marshal(s)
	if err != nil {
		return nil
	}
	return json.RawMessage(b)
}

// runHeadless drives a synchronous agent loop: stream the assistant
// turn, execute any tool calls it requests, append both to history, and
// repeat until a turn requests no tools (or the iteration cap is hit).
//
// It is the headless analogue of the TUI's beginStream -> streamDone ->
// toolResult -> continueLoop cycle, collapsed into one blocking call.
// history must already contain the user turn(s) to answer; runHeadless
// appends assistant text and tool results to its last turn the same way
// the TUI does (AppendDelta / BeginCall / FinishCall), so
// history.ToProviderMessages produces the identical wire shape.
//
// prov is an interface so callers (and tests) can inject any
// provider.Provider, including a scripted fake with no network. cfg's
// Tools field advertises the toolset to the model; tool execution goes
// through tools.Run, the same dispatcher the TUI uses.
//
// emit receives every observable event in order. It must not be nil;
// pass a no-op if you don't care. runHeadless returns the first fatal
// error encountered (a streaming error), or nil on clean completion.
func runHeadless(ctx context.Context, prov provider.Provider, cfg provider.Config, history *chat.History, emit func(ev headlessEvent)) error {
	emit(headlessEvent{Type: "agent_start"})

	var runErr error
	for iter := 1; iter <= headlessMaxIterations; iter++ {
		if err := ctx.Err(); err != nil {
			runErr = err
			break
		}

		// turn_start wraps one agent turn (one model<->tool round-trip),
		// per docs/json.md's turn lifecycle.
		emit(headlessEvent{Type: "turn_start"})
		emit(headlessEvent{Type: "message_start", Message: &headlessMessage{Role: "assistant", Content: ""}})

		msgs := history.ToProviderMessages()
		text, toolCalls, err := streamHeadlessTurn(ctx, prov, cfg, msgs, emit)
		if text != "" {
			history.AppendDelta(text)
		}
		if err != nil {
			history.FinishTurn(err)
			runErr = err
			break
		}

		// The complete message for this turn: assembled text plus any
		// tool calls it requested (raw-JSON args).
		msg := &headlessMessage{Role: "assistant", Content: text}
		for _, tc := range toolCalls {
			msg.ToolCalls = append(msg.ToolCalls, headlessToolCall{
				ID:   tc.ID,
				Name: tc.Function.Name,
				Args: rawJSON(tc.Function.Arguments),
			})
		}
		emit(headlessEvent{Type: "message_end", Message: msg})

		// No tools requested: the turn is the final answer. Close the
		// turn and finish.
		if len(toolCalls) == 0 {
			emit(headlessEvent{Type: "turn_end", Message: msg})
			history.FinishTurn(nil)
			break
		}

		// Execute each requested tool, recording it on the current turn
		// exactly as the TUI does so the next ToProviderMessages emits
		// the assistant(tool_calls) + tool(result) pair the model needs.
		var toolResults []headlessToolResult
		for _, tc := range toolCalls {
			emit(headlessEvent{
				Type:       "tool_execution_start",
				ToolCallID: tc.ID,
				ToolName:   tc.Function.Name,
				Args:       rawJSON(tc.Function.Arguments),
			})
			idx := history.BeginCall(tc.ID, tc.Function.Name, tc.Function.Arguments)
			out, toolErr := tools.Run(tc.Function.Name, tc.Function.Arguments)
			history.FinishCall(idx, out, toolErr)

			result := out
			if toolErr != nil {
				result = toolErr.Error()
			}
			emit(headlessEvent{
				Type:       "tool_execution_end",
				ToolCallID: tc.ID,
				ToolName:   tc.Function.Name,
				Result:     rawJSON(result),
				IsError:    toolErr != nil,
			})
			toolResults = append(toolResults, headlessToolResult{
				ToolCallID: tc.ID,
				Result:     rawJSON(result),
				IsError:    toolErr != nil,
			})
		}

		// turn_end closes the turn, carrying the assistant message plus
		// the tool results gathered this round (docs/json.md).
		emit(headlessEvent{Type: "turn_end", Message: msg, ToolResults: toolResults})

		// Loop: feed the tool results back to the model for the next
		// turn. If we exhaust the iteration cap, fall through to the
		// post-loop guard below.
		if iter == headlessMaxIterations {
			runErr = fmt.Errorf("hit max tool iterations (%d)", headlessMaxIterations)
			history.FinishTurn(runErr)
		}
	}

	end := headlessEvent{Type: "agent_end"}
	if runErr != nil {
		end.Error = runErr.Error()
	}
	emit(end)
	return runErr
}

// streamHeadlessTurn runs one streaming request to completion, emitting
// each text/thinking delta as a message_update event (carrying the
// nested message object plus an assistantMessageEvent) and returning the
// assembled assistant text plus any tool calls the terminal chunk
// carried. It mirrors the TUI runStream's channel-drain logic, minus the
// tea plumbing.
func streamHeadlessTurn(ctx context.Context, prov provider.Provider, cfg provider.Config, msgs []provider.Message, emit func(ev headlessEvent)) (string, []provider.ToolCall, error) {
	chunks, errs := prov.Stream(ctx, cfg, msgs)
	var b strings.Builder
	var toolCalls []provider.ToolCall
	for {
		select {
		case <-ctx.Done():
			return b.String(), toolCalls, ctx.Err()
		case c, ok := <-chunks:
			if !ok {
				// Channel closed: surface a trailing error if one is
				// queued, otherwise the turn completed cleanly.
				select {
				case e := <-errs:
					return b.String(), toolCalls, e
				default:
					return b.String(), toolCalls, nil
				}
			}
			if c.Reasoning != "" {
				// Reasoning/thinking deltas ride the same message_update
				// event but with a thinking_delta assistantMessageEvent
				// (docs/json.md). The message content is the assistant
				// text assembled so far; thinking is not appended to it.
				emit(headlessEvent{
					Type:                  "message_update",
					Message:               &headlessMessage{Role: "assistant", Content: b.String()},
					AssistantMessageEvent: &assistantMessageEvent{Type: "thinking_delta", Delta: c.Reasoning},
				})
			}
			if c.Delta != "" {
				b.WriteString(c.Delta)
				emit(headlessEvent{
					Type:                  "message_update",
					Message:               &headlessMessage{Role: "assistant", Content: b.String()},
					AssistantMessageEvent: &assistantMessageEvent{Type: "text_delta", Delta: c.Delta},
				})
			}
			if c.Done {
				toolCalls = c.ToolCalls
			}
		case e := <-errs:
			if e != nil {
				return b.String(), toolCalls, e
			}
		}
	}
}

// ---------- entry points ----------

// runPrintMode is the -p/--print one-shot. It assembles a fresh history
// (system prompt + the user prompt), runs runHeadless, streams the
// assistant TEXT to stdout, and prints brief tool markers to stderr.
// Returns the loop error (caller maps to a non-zero exit).
//
// prov/cfg are resolved by the caller (resolveProvider), keeping all
// credential/model logic in one place. out/errOut are injectable for
// testing; pass os.Stdout / os.Stderr in production.
func runPrintMode(ctx context.Context, prov provider.Provider, cfg provider.Config, system, prompt string, out, errOut io.Writer) error {
	history := &chat.History{System: system}
	history.AppendUser(prompt)

	emit := func(ev headlessEvent) {
		switch ev.Type {
		case "message_update":
			// Only assistant text deltas go to stdout; thinking deltas
			// are suppressed in print mode.
			if ev.AssistantMessageEvent != nil && ev.AssistantMessageEvent.Type == "text_delta" {
				fmt.Fprint(out, ev.AssistantMessageEvent.Delta)
			}
		case "tool_execution_start":
			fmt.Fprintf(errOut, "[tool] %s %s\n", ev.ToolName, firstLine(string(ev.Args)))
		case "tool_execution_end":
			if ev.IsError {
				fmt.Fprintf(errOut, "[tool] %s -> error: %s\n", ev.ToolName, firstLine(string(ev.Result)))
			} else {
				fmt.Fprintf(errOut, "[tool] %s -> %d bytes\n", ev.ToolName, len(ev.Result))
			}
		}
	}

	err := runHeadless(ctx, prov, cfg, history, emit)
	// Always finish with a newline so the shell prompt lands cleanly,
	// matching pi's print mode.
	fmt.Fprintln(out)
	if err != nil {
		fmt.Fprintf(errOut, "pi9: %v\n", err)
	}
	return err
}

// runJSONMode is --mode json. Same loop as print mode, but every event
// is marshalled to a single-line JSON object on stdout (JSONL). The
// stream opens with a "session" header line (matching pi), then the
// runHeadless events in order. out is injectable for testing.
func runJSONMode(ctx context.Context, prov provider.Provider, cfg provider.Config, system, prompt, sessionID, cwd string, out io.Writer) error {
	history := &chat.History{System: system}
	history.AppendUser(prompt)

	enc := json.NewEncoder(out)
	// Header line announces the session, mirroring docs/json.md's
	// {"type":"session","version":3,"id":...,"timestamp":...,"cwd":...}
	// first line. version and timestamp match pi; there is no top-level
	// "mode" field in the spec, so it is dropped.
	_ = enc.Encode(map[string]any{
		"type":      "session",
		"version":   3,
		"id":        sessionID,
		"timestamp": time.Now().UTC().Format(time.RFC3339),
		"cwd":       cwd,
	})

	emit := func(ev headlessEvent) {
		// Encode failures (closed pipe) are non-fatal: the loop keeps
		// running tools, we just stop being able to report. Swallow.
		_ = enc.Encode(ev)
	}

	return runHeadless(ctx, prov, cfg, history, emit)
}

// ---------- main() dispatch ----------

// headlessOptions captures the parsed CLI inputs a headless run needs.
// Kept tiny and explicit so main()'s dispatch stays readable.
type headlessOptions struct {
	print     bool   // -p / --print
	mode      string // --mode value ("" | "json" | "rpc")
	prompt    string // resolved prompt (flag value, args, or stdin)
	model     string
	apiKey    string
	thinking  string
	sessionID string
	cwd       string
	noSession bool
}

// isHeadless reports whether the parsed options request a non-TUI run.
func (o headlessOptions) isHeadless() bool {
	return o.print || o.mode != ""
}

// dispatchHeadless runs the requested headless mode end-to-end and
// returns the process exit code. It resolves the provider with the
// caller-supplied credentials/model (same path the TUI uses), builds
// the system prompt, and routes to print or JSON mode. rpc is a clear
// not-implemented stub for now.
//
// Persistence is intentionally NOT wired here: headless runs are
// ephemeral by design (and --no-session forces it). This matches pi,
// where -p/--mode are one-shot and don't mutate the session store.
func dispatchHeadless(ctx context.Context, opts headlessOptions) int {
	if strings.TrimSpace(opts.prompt) == "" {
		fmt.Fprintln(os.Stderr, "pi9: no prompt. pass -p \"...\", positional args, or pipe via stdin")
		return 2
	}

	// rpc mode is documented in pi but not yet implemented here.
	if opts.mode == "rpc" {
		fmt.Fprintln(os.Stderr, "pi9: --mode rpc is not implemented yet")
		return 2
	}
	// "text" is pi's default mode and an explicit alias for print mode.
	if opts.mode != "" && opts.mode != "json" && opts.mode != "text" {
		fmt.Fprintf(os.Stderr, "pi9: unknown --mode %q (want: json or text)\n", opts.mode)
		return 2
	}

	if opts.apiKey == "" && store.LookupAPIKey(string(provider.ProviderForModel(opts.model))) == "" {
		// resolveProvider would also catch this, but a direct message
		// here is clearer for a scripting context.
		fmt.Fprintln(os.Stderr, "pi9: no API key set. run pi9 interactively and /login first")
		return 1
	}

	prov, cfg, err := resolveProvider(ctx, opts.apiKey, opts.model, opts.thinking, tools.Schemas())
	if err != nil {
		fmt.Fprintf(os.Stderr, "pi9: %v\n", err)
		return 1
	}

	system := buildSystemPrompt()

	var runErr error
	switch {
	case opts.mode == "json":
		runErr = runJSONMode(ctx, prov, cfg, system, opts.prompt, opts.sessionID, opts.cwd, os.Stdout)
	default: // print mode (-p with no/unknown mode already rejected above)
		runErr = runPrintMode(ctx, prov, cfg, system, opts.prompt, os.Stdout, os.Stderr)
	}
	if runErr != nil {
		return 1
	}
	return 0
}

// stdinIsPipe reports whether stdin is a pipe or regular file (i.e.
// data was piped/redirected in) rather than an interactive terminal.
// Uses only os.FileMode bits so it stays portable to plan9 without an
// isatty dependency. On any stat error we assume no pipe (interactive).
func stdinIsPipe() bool {
	fi, err := os.Stdin.Stat()
	if err != nil {
		return false
	}
	// A character device is the terminal; anything else (named pipe,
	// regular file from a redirect) means data is waiting on stdin.
	return fi.Mode()&os.ModeCharDevice == 0
}

// resolveHeadlessPrompt picks the prompt for a headless run, in
// precedence order: explicit -p flag value, then any positional args
// (joined with spaces), then piped stdin. Piped stdin is also APPENDED
// to a flag/arg prompt when both are present, matching pi's
// "cat file | pi -p 'summarize'" behaviour. @file tokens in the
// flag/arg portion are expanded the same way the TUI expands them.
func resolveHeadlessPrompt(flagVal string, args []string, stdin io.Reader, stdinIsPipe bool) string {
	base := strings.TrimSpace(flagVal)
	if base == "" && len(args) > 0 {
		base = strings.TrimSpace(strings.Join(args, " "))
	}
	if base != "" {
		base = expandAtFiles(base, os.ReadFile)
	}

	if stdinIsPipe {
		if piped, err := io.ReadAll(stdin); err == nil {
			p := strings.TrimRight(string(piped), "\n")
			if p != "" {
				if base == "" {
					return p
				}
				return base + "\n\n" + p
			}
		}
	}
	return base
}
