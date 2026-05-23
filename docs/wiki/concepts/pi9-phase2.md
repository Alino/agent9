---
title: pi9 Phase 2 — Streaming Chat
created: 2026-05-16
updated: 2026-05-16
type: concept
tags: [arch, status-done]
---

# pi9 Phase 2 — Streaming Chat

> **Status: done 2026-05-16.** pi9 has chat-shaped UI (header, scrollback, input box, status bar), OpenRouter-compatible SSE streaming, full interactive flow: type → send → watch tokens stream in. Verified end-to-end against a mock server inside the 9front VM.

## What it does

You type a message, press Enter, pi9 POSTs to OpenRouter (or any OpenAI-compatible /chat/completions endpoint with `stream: true`), parses Server-Sent Events line by line, and each token gets appended to the current turn in the scrollback. When the stream finishes, pi9 shows the elapsed time and unlocks the input box for the next turn.

Screenshot: `wiki/assets/pi9-phase2-rendered.png` shows a real run inside vtwin on 9front, with the mock server's canned response rendered.

## Architecture

```
src/pi9/
├── main.go                    Bubble Tea model + main()
├── internal/
│   ├── chat/chat.go           History, Turn, lipgloss-styled rendering
│   └── provider/openrouter.go SSE streaming HTTP client
├── vendor-patches/bubbletea/  (unchanged from Phase 1)
└── testtools/
    └── mock-openrouter.py     Local SSE mock server
```

### internal/provider/openrouter.go

OpenAI-compatible streaming client. Exposes:

- `Config{APIKey, Model, MaxTokens}` — per-request config
- `Message{Role, Content}` — OpenAI shape
- `Chunk{Delta, Done}` — one streamed token or end-of-stream marker
- `StreamRequest(ctx, cfg, msgs) (<-chan Chunk, <-chan error)` — fires the request, returns a channel that yields chunks until done

Plan9-specific bits:

- **TLS**: reads `$SSL_CERT_FILE` manually (plan9 Go runtime ignores it). Allows `INSECURE_TLS=1` for testing against http mocks.
- **Endpoint override**: `$OPENROUTER_API_URL` overrides the default `https://openrouter.ai/api/v1/chat/completions`. Lets us point at the mock server.
- **No outer timeout**: streaming responses can be arbitrarily long. We use `ResponseHeaderTimeout: 30s` to bound the initial connect+headers, then trust the underlying TCP stream.
- **DisableCompression**: ensures SSE comes through immediately without gzip buffering — important on plan9 where the gzip implementation might buffer differently than unix.

### internal/chat/chat.go

In-memory conversation state + rendering. `History.Turns` is the ordered list; each `Turn` has User text, Assistant text (built up by AppendDelta), timestamps, optional error.

`Render(*History, width)` returns the full scrollback as a formatted string with lipgloss colors:
- `you: …` in **bright yellow** (palette 11)
- `pi9: …` label in **bright blue** (palette 12, Luna)
- assistant body in white (palette 7)
- elapsed time hint in dim italic gray (palette 8)

Word-wrapping is hand-rolled (no `runewidth` to avoid the dep on plan9). Existing newlines from streamed content survive.

### main.go

Bubble Tea model. Layout (heights sum to terminal height):

```
┌─ header (1 row, Luna-blue bg)
├─ scrollback (variable, fills remaining)
├─ input box (3 rows, lipgloss border)
└─ status bar (1 row)
```

Streaming plumbing:

```
User presses Enter
  ↓
submitInput() — appends user turn, returns runStream tea.Cmd
  ↓
runStream goroutine
  ├─ calls provider.StreamRequest
  ├─ on each chunk: teaSendFn(chunkMsg{delta: …})
  └─ when done: returns streamDoneMsg{err: …}
  ↓
Update receives chunkMsg → history.AppendDelta → view re-renders
Update receives streamDoneMsg → history.FinishTurn → unlock input
```

`teaSendFn` is a package-level pointer installed after `tea.NewProgram` so the streaming goroutine can call `p.Send(msg)` without dragging a `*tea.Program` reference through the model.

Raw mode is enabled at startup via vts ctl: `setVtsRaw(true)` writes `rawon\nedit off\n` to `/n/vts/<s>/ctl` so each keystroke arrives immediately. On exit, `defer setVtsRaw(false)` restores `rawoff\nedit on`.

## Tests verified

| Criterion | How verified |
|---|---|
| Provider client compiles for plan9/amd64 | `GOOS=plan9 GOARCH=amd64 go build` produces 7.6MB a.out |
| Mounts /n/vts on startup | `os.Stat("/n/vts/ctl")` returns OK after `/bin/mount` runs |
| Raw mode enabled | typed chars appear immediately (no waiting for Enter) |
| LF and CR both submit | `enter`, `ctrl+j`, `ctrl+m` all map to submitInput() |
| View() renders header + scrollback + input + status | Debug log dumped each render's structure |
| SSE parsing works | Mock server streamed 28 tokens; all appeared in history |
| Streaming UI updates per chunk | View re-renders on each chunkMsg arrival |
| Stream completion handled | finish_reason fires streamDoneMsg, elapsed-time shown |
| Input box resets after stream | Next message can be sent immediately |

## Key gotchas

### LF vs CR for Enter

Bubbletea maps byte 0x0d (CR) to "enter" and byte 0x0a (LF) to "ctrl+j". Plan 9 sends LF on Enter (rio convention). Solution: accept all three in `handleKey`:

```go
case "enter", "ctrl+j", "ctrl+m":
    return m.submitInput()
```

This makes pi9 work whether invoked from rio terminal, vts session, or piped input.

### Plan 9 doesn't kill processes via /proc/$pid/note

Go's signal handling on plan9 catches and discards notes that aren't explicitly handled. `slay pi9` from outside the process group uses `/proc/$pid/note` which doesn't kill Go processes reliably.

For testing/cleanup, use `/proc/$pid/ctl`:

```rc
for(p in `{ps | grep ' pi9$' | awk '{print $2}'}) echo kill > /proc/$p/ctl
```

### vts session size can change between runs

Each pi9 launch enables raw mode + edit off via vts ctl, which seems to cause vts to occasionally resize the session buffer. The cells dimensions we observed varied: 24x80, 19x78, 44x126 across different launches. pi9 handles this fine via `WindowSizeMsg`, but it means screenshots aren't pixel-stable across runs.

### Alt-screen restoration in vtwin shows stale frames

In the Phase 2 screenshot, the input box area shows multiple historical input states stacked (`> _`, `> Hello pi9!_`, `> streaming…`, `> _`). Bubbletea's standard renderer diffs and overwrites only the lines that changed, but lipgloss border characters of different widths leave trailing chars. Cosmetic — doesn't affect functionality. Fix planned for Phase 3 polish: either pad border to consistent width or use `tea.ClearScreen` between renders.

### Cells stream cannot be re-read after redraw

vts's `redraw` ctl command sets `all_dirty = 1`, but if any process reads cells in the meantime the flag gets cleared. For diagnostic snapshots after multiple operations, can't easily get the latest full-frame state. Worked around by trusting the view-log debug instrumentation instead.

## Testing infrastructure

### Mock server (`testtools/mock-openrouter.py`)

Listens on `:8766/chat` and streams a canned response. Tests pi9's streaming pipeline without burning real API credits.

```sh
python3 testtools/mock-openrouter.py

# Inside the VM:
OPENROUTER_API_KEY=mockkey \
OPENROUTER_API_URL=http://10.0.2.2:8766/chat \
/tmp/pi9
```

Returns ~28 word-sized chunks at 50ms each, then `[DONE]`.

## Phase 2 deliverables — all met

- ✅ Provider package: SSE streaming HTTP client, plan9-aware TLS
- ✅ Chat package: History/Turn types, lipgloss rendering
- ✅ Refactored main.go: header + scrollback + input + status layout
- ✅ Streaming integration: runStream tea.Cmd + chunkMsg dispatch
- ✅ Input handling: typed chars, backspace, cursor movement (left/right/home/end), ctrl-u clear, Enter (both LF and CR)
- ✅ Cancel-in-flight: ctrl-c during stream cancels the context
- ✅ End-to-end test against mock server
- ✅ Visual verification in vtwin

## What's deferred to Phase 3

- **Tools**: read_file, write_file, run_rc — the real agent loop
- **Plan9-native tools**: plumb, hget, walk, mount, ns
- **Provider switching**: `/model` slash command
- **Sessions**: persist to /lib/pi9/sessions/
- **Skills**: load from /lib/pi9/skills/
- **Memory**: /lib/pi9/memory.md as system-prompt prefix

## What's deferred to Phase 6+ polish

- Stale-frame artifacts in input box during stream (renderer differ issue)
- Multi-line input via shift-enter
- Scrollback navigation (page up/down, jump to top/bottom)
- Word-wrap with rune-width awareness (emoji, CJK)
- Themes beyond Luna

## See Also

- [[pi9-architecture]] — overall design and phasing
- [[pi9-phase1]] — Bubble Tea on plan9 (foundation)
- `src/pi9/main.go` — current entry point
- `src/pi9/internal/provider/openrouter.go` — SSE client
- `src/pi9/internal/chat/chat.go` — history + rendering
- `src/pi9/testtools/mock-openrouter.py` — mock SSE server
- `wiki/assets/pi9-phase2-rendered.png` — screenshot from VM
