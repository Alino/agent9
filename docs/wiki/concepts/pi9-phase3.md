---
title: pi9 Phase 3 — Tool Calling
created: 2026-05-16
updated: 2026-05-16
type: concept
tags: [arch, status-done]
---

# pi9 Phase 3 — Tool Calling

> **Status: done 2026-05-16.** pi9 advertises tools to the provider, parses streaming tool_call deltas, dispatches to local tool implementations, threads the results back into the conversation, and loops until the model emits a turn without tool_calls. Verified end-to-end inside the 9front VM with a real `run_rc("ls /tmp")` executed against the live plan9 filesystem.

## What's new vs Phase 2

| | Phase 2 | Phase 3 |
|---|---|---|
| Provider request | content + stream | content + stream + **tools advertised** |
| SSE parsing | content deltas | content deltas + **tool_call deltas assembled by index** |
| Done chunk | empty | **carries assembled ToolCalls** |
| Agent loop | 1 round per user message | **N rounds**: stream→tools→stream until no tool_calls |
| chat.Turn | User + Assistant + Started/Finished | **+ Calls[]** (ToolInvocation list) |
| UI | text | text + **inline tool blocks** (cyan ▸ markers) |

## Architecture

```
src/pi9/
├── main.go                          agent loop, key handling, layout
├── internal/
│   ├── chat/chat.go                 + ToolInvocation, BeginCall, FinishCall
│   ├── provider/openrouter.go       + Tool, ToolCall, streaming assembly
│   └── tools/tools.go               read_file, write_file, run_rc + registry
└── testtools/
    └── mock-openrouter.py           extended to emit tool_call sequences
```

## Tool palette (Phase 3 minimal set)

| Tool | Signature | Plan9 specifics |
|---|---|---|
| `read_file` | path → contents | `os.ReadFile` — works everywhere |
| `write_file` | path, content → "wrote N bytes to P" | creates parent dirs |
| `run_rc` | command → combined stdout+stderr + exit | uses `/bin/rc -c` on plan9, `/bin/sh -c` elsewhere |

Schemas are JSON Schema fragments encoded in Go maps. `tools.Schemas()` returns the provider-shaped list pi9 passes in `provider.Config.Tools`. Dispatch is a switch in `tools.Run(name, argsJSON)`.

Plan9-native tools (plumb, hget, walk, mount, ns) are Phase 5 — that's where pi9 stops being a port and starts being a plan9 thing. Phase 3's three tools are the minimum useful set; they get a coding agent off the ground.

## Streaming tool call assembly

OpenAI / OpenRouter delivers tool_calls as **deltas indexed by call number**:

```
data: {"choices":[{"delta":{"tool_calls":[{
   "index":0,
   "id":"call_abc",
   "type":"function",
   "function":{"name":"read_file","arguments":""}}]}}]}

data: {"choices":[{"delta":{"tool_calls":[{
   "index":0,
   "function":{"arguments":"{\"path\":"}}]}}]}

data: {"choices":[{"delta":{"tool_calls":[{
   "index":0,
   "function":{"arguments":"\"/tmp\"}"}}]}}]}

data: {"choices":[{"delta":{},"finish_reason":"tool_calls"}]}
data: [DONE]
```

The `id`/`name` arrive once in the first delta for each call. The `arguments` string accumulates across many deltas (often character-by-character from streaming models).

`readSSE` keeps a `map[int]*ToolCall` indexed by `td.Index` and concatenates new bits onto the in-flight calls. On `finish_reason` or `[DONE]`, it emits `Chunk{Done: true, ToolCalls: <assembled list>}`.

Multiple tool calls in one round (the model wants to do two things in parallel) work the same way — `index: 0` and `index: 1` accumulate independently and both get emitted in the final Done chunk.

## Agent loop

Triggered on user submit:

```
beginStream()
  ↓
runStream tea.Cmd
  ├─ provider.StreamRequest
  ├─ on each delta: send chunkMsg
  └─ on Done: return streamDoneMsg{toolCalls}
  ↓
Update(streamDoneMsg)
  ├─ len(toolCalls) == 0: FinishTurn, unlock input  (terminal case)
  └─ len(toolCalls)  > 0: tea.Batch(runTool for each)
  ↓
Update(toolResultMsg, ...) — one per call
  ├─ FinishCall(idx, output, err)
  └─ if allCallsFinished: return loopContinueMsg{}
  ↓
Update(loopContinueMsg)
  ↓
beginStream()  (with updated history including tool results)
```

`allCallsFinished` checks every Call in the current turn has a non-zero `Finished` timestamp. Once true, `continueLoop` is responsible for firing `loopContinueMsg` which triggers the next stream round. A safety bound of `maxTurnLoops*4` tool calls per turn prevents runaway loops.

## History → provider messages

`History.ToProviderMessages()` walks every turn and emits:

```
system: <systemPrompt>
user: turn[0].User
assistant: turn[0].Assistant + tool_calls=[turn[0].Calls...]
tool: turn[0].Calls[0].Output  (tool_call_id, name)
tool: turn[0].Calls[1].Output
user: turn[1].User
...
```

Only finished calls are emitted (in-flight calls don't have a result yet). The model sees the full audit trail: what it asked for, what came back.

## UI: tool blocks

Rendered inline within an assistant turn:

```
pi9: Looking at the output:

Phase 3 tool calling works ✓
  ▸ run_rc({"command": "ls /tmp"})  [31ms] 2654 bytes
  (679ms)
```

- `▸` and tool name in **bright cyan** (palette 14)
- args trimmed to fit width (60 char max for display)
- meta `[31ms] N bytes` in dim italic
- on error: `[31ms] error message` in **bright red** (palette 9)
- multiple tool calls in a turn render as separate `▸` lines

In-flight calls show ` running…` instead of duration. Streaming the args delta could be shown but we wait for assembly to complete before display — cleaner.

## Verified flow (screenshot: `wiki/assets/pi9-phase3-rendered.png`)

User typed: `ls /tmp`

Mock server saw it, returned a `tool_calls` finish reason carrying `run_rc({"command": "ls /tmp"})`.

pi9 dispatched `tools.Run("run_rc", "{\"command\":\"ls /tmp\"}")` which executed via `/bin/rc -c "ls /tmp"` on the live 9front filesystem. Took 31ms, produced 2654 bytes of real `ls` output.

pi9 sent a second stream request to the mock with `role=tool` message included. Mock saw `role=tool` and returned `"Looking at the output:\n\nPhase 3 tool calling works ✓"` with `finish_reason=stop`.

pi9 rendered everything inline:
- assistant prefix `pi9:` (blue)
- "Looking at the output:" (white)
- "Phase 3 tool calling works ✓" (white)
- "▸ run_rc({"command": "ls /tmp"}) [31ms] 2654 bytes" (cyan)
- "(679ms)" — total turn time (dim italic)

Total round-trip: 679ms (two stream calls + one tool execution + UI updates).

## Acceptance criteria — all met

| Criterion | Verified |
|---|---|
| Tool schemas advertised in request | ✅ requestBody.Tools populated |
| Streaming tool_call deltas parsed | ✅ assembled by index, full JSON args reconstructed |
| Tool dispatch works | ✅ run_rc executed `ls /tmp` for real |
| Tool result threaded into history | ✅ role=tool message in second stream |
| Agent loop continues after tool round | ✅ second stream fired, content streamed |
| Loop terminates on no-tool-calls turn | ✅ FinishTurn called, input unlocked |
| UI renders tool blocks | ✅ cyan ▸ markers, args, duration, byte count |
| Error in tool surfaces in UI | wired but not retested (errStyle path exists) |
| Cancel mid-loop | wired (ctrl-c cancels ctx, breaks loop) |
| run_rc uses rc on plan9 | ✅ runtime.GOOS=="plan9" path took 31ms |

## Quirks worth remembering

### Bubbletea-streamDoneMsg and channel close ordering

The runStream goroutine has a tricky race: the chunks channel close happens AFTER the producer goroutine returns. If we return `streamDoneMsg` immediately on seeing `c.Done`, we might miss the final error if any. Solution: capture `toolCalls` from the Done chunk but DON'T return — let the loop continue until `!ok` from the channel, then check errs.

### Tool result body shape

Sending tool results back to the model uses OpenAI's `role=tool` message with `tool_call_id` matching the original call id. Plus `name` (the tool name). Some providers also accept the result under `content` as a string; we use string content unconditionally.

### Errors are content, not errors

When a tool fails, pi9 still sends the model a `role=tool` message — with `ERROR: <msg>` prepended to whatever output was captured. The model treats this as context, often deciding to retry or report back. Returning a transport-level error here would abort the agent loop unhelpfully.

### maxTurnLoops budget

A single turn can run up to `maxTurnLoops*4 = 40` tool calls before pi9 force-finishes with "max tool loops". This catches infinite loops (model asks for the same file repeatedly). 10 logical rounds × 4 parallel calls per round is the rough ceiling. Tune later.

### `/proc/$pid/ctl` for killing pi9 in tests

Phase 2 quirk still applies: `slay pi9` or notes via `/proc/$pid/note` don't kill Go processes on plan9. Use `echo kill > /proc/$pid/ctl`. The Phase 3 e2e test loops over `ps | grep pi9` and writes to each procctl.

## What's deferred

### Phase 4 (sessions + skills + memory)

- Persist conversation to `/lib/pi9/sessions/<id>.json`
- Load skills from `/lib/pi9/skills/` as on-demand markdown
- `/lib/pi9/memory.md` prefix on system prompt
- `/model`, `/clear`, `/save` slash commands

### Phase 5 (plan9-native tools)

- `plumb(text, dst)` — route to plumber port
- `hget(url)` — plan9's native HTTP client
- `walk(pattern)` / `ns()` / `mount()` / `bind()` — namespace tools
  that no Linux agent can do

### Phase 6 (polish)

- Fix stale-frame artifacts in input box during streaming (lipgloss border vs bubbletea diff renderer)
- Multi-line input (shift+enter)
- Scrollback navigation (page up/down)
- Word-wrap with rune-width awareness

## See Also

- [[pi9-architecture]] — overall design and phasing
- [[pi9-phase1]] — Bubble Tea on plan9 (foundation)
- [[pi9-phase2]] — streaming chat without tools
- `src/pi9/internal/tools/tools.go` — tool registry
- `src/pi9/internal/provider/openrouter.go` — tool_call streaming assembly
- `src/pi9/internal/chat/chat.go` — ToolInvocation rendering
- `src/pi9/testtools/mock-openrouter.py` — mock with tool_call branch
- `wiki/assets/pi9-phase3-rendered.png` — verification screenshot
