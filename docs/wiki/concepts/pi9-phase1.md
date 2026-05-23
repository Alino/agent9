---
title: pi9 Phase 1 — Bubble Tea Running in vts
created: 2026-05-16
updated: 2026-05-16
type: concept
tags: [arch, status-done]
---

# pi9 Phase 1 — Bubble Tea Running in vts

> **Status: done 2026-05-16.** End-to-end verified: pi9 (Go + Bubble
> Tea + lipgloss) compiles for plan9/amd64, launches inside a vts
> session, renders to vts cells with the correct Luna palette colors,
> stays alive in event loop, receives keystrokes from the vts cons
> file, processes `q` keymsg, and exits cleanly.

## What works

1. **Cross-compile**: `GOOS=plan9 GOARCH=amd64 go build` produces a
   plan9 amd64 a.out binary (magic `0000 8a97`) from `src/pi9/`.
2. **Load on plan9**: binary copies into the 9front VM via `hget` from
   a Mac-side http.server.
3. **Self-mount /srv/vts**: pi9 at startup detects `$vts` env var,
   creates `/n/vts`, calls `/bin/mount` to bring vts's file system
   into its namespace. No wrapper script needed.
4. **Bubble Tea event loop**: 4-5 Go procs alive in `Tsemacqu` /
   `Semacqui` / `Pread` state. Stays alive indefinitely waiting for
   input.
5. **Render**: lipgloss-styled text is emitted as ANSI escapes →
   vts parses VT100 → cells stored in `/n/vts/<s>/cells`. Verified
   character-by-character via `testtools/celldump.c`:

   ```
   Row 1: " pi9   plan9-native LLM agent"    fg=12 (Luna blue) bold
   Row 3: "phase 1: hello world"             white
   Row 4: "running in: vts session 1"        white
   Row 5: "terminal:   80x24"                white
   Row 6: "bubbletea:  working on plan9/amd64" white
   Row 8: "press q to quit"                  fg=8 (dim gray) italic
   ```

   Full text capture in `wiki/assets/pi9-phase1-render.txt`. Visual
   render captured earlier (Luna gradient titlebar + WinXP-style
   window) in `wiki/assets/pi9-phase1-rendered.png` — taken when
   vtwin was previously running; this VM session ran without vtwin
   and verified via the cells protocol directly.

6. **Resize detection**: `signals_plan9.go` polls `/n/vts/<s>/ctl`
   every 500ms, parses the `RxC` field, sends a `WindowSizeMsg{Width:
   cols, Height: rows}` to Update. The terminal-size readout in pi9's
   View() goes from `0x0` (pre-poll) to `80x24` (post-poll) as
   expected.
7. **Quit-on-q**: `echo q > /n/vts/<s>/cons` delivers a keystroke
   that bubbletea's input reader processes; Update matches "q"
   in tea.KeyMsg, returns `tea.Quit`; bubbletea exits cleanly. Pi9
   procs drop from 4-5 to 0 within 1-2 seconds.

## Shim summary (`src/pi9/vendor-patches/bubbletea/`)

Three files added to a checkout of bubbletea v1.3.10:

### `tty_plan9.go`

- `initInput`: in vts mode, leaves `p.ttyInput` and `p.ttyOutput` nil.
  vts manages raw mode itself; bubbletea reading stdin works fine.
  Crucially this prevents `checkResize → term.GetSize → /dev/wctl`
  from ever firing (which would crash with "buffer too small" because
  pi9's namespace doesn't have a rio window).
- `readVtsSize`: parses `/n/vts/ctl` text output. The wire spec
  claimed `/n/vts/<s>/size` was a separate file but vts only exposes
  size via the ctl status line "RxC". Adapter lives in our shim.
- `openInputTTY`: opens `/n/vts/<s>/cons` if in vts, falls back to
  `/dev/cons` otherwise. Rarely reached because pi9 always uses
  `WithInput(os.Stdin)`.
- `suspendSupported = false` and `suspendProcess()` no-op: plan9 has
  no SIGTSTP / job control.

### `signals_plan9.go`

- `listenForResize`: polls `/n/vts/<s>/ctl` every 500ms via
  `readVtsSize`, sends `WindowSizeMsg` directly (bypasses upstream
  `checkResize` which would crash on `/dev/wctl`).
- `vtsListenForResize(p, done)`: hook called from `tea.go`'s
  `handleResize` when `p.ttyOutput == nil`. Starts the polling
  goroutine if running under vts. Without this hook,
  `listenForResize` would never run on plan9 — the upstream code
  only spawns it in the `p.ttyOutput != nil` branch.

### `signals_other.go`

- Stub `vtsListenForResize` returning false on non-plan9 platforms,
  so the same `handleResize` change compiles for unix/windows/etc.

### `tea.go` (patched)

`handleResize()` adds an `else if vtsListenForResize(...)` clause
between the `p.ttyOutput != nil` path and the `close(ch)` fallback.
Without this, plan9-vts would skip resize-listening entirely.

## main.go highlights

```go
// Self-mount vts
if os.Getenv("vts") != "" {
    _ = os.MkdirAll("/n/vts", 0755)
    _ = exec.Command("/bin/mount", "/srv/vts", "/n/vts").Run()
}

// Force input/output so bubbletea uses pipes directly
p := tea.NewProgram(m,
    tea.WithInput(os.Stdin),
    tea.WithOutput(os.Stdout),
    tea.WithAltScreen(),
    tea.WithoutSignalHandler(),
)
```

The `WithInput(os.Stdin)` path bypasses the `term.IsTerminal` probe
that would otherwise try to detect a tty and reach for `/dev/cons`
instead of the vts session pipes.

`WithoutSignalHandler()` skips Go's `signal.Notify(SIGINT, ...)` —
plan9 has no SIGINT, and our shim handles quit semantically inside
Update().

## Quirks worth remembering

1. **Input is line-buffered.** Because we skip `MakeRaw` on vts pipes
   (vts owns the line discipline), input arrives at pi9 as cooked
   lines. `echo q > /n/vts/<s>/cons` works; `echo -n q` doesn't (no
   newline = no flush). For interactive use we'll want pi9 to write
   `rawon` to `/n/vts/<s>/ctl` so each keystroke arrives immediately.
   Logged as Phase 2 prep.

2. **/n/vts/<s>/size doesn't exist** — only `/n/vts/<s>/ctl` does.
   The wiki vt-9p-namespace page documents a separate `size` file
   that vts hasn't implemented. Our `readVtsSize` parses the ctl
   status line instead. If size becomes its own file later, update
   `tty_plan9.go`.

3. **The em-dash in pi9's title renders as space in `celldump`** —
   not a bug in pi9 or vts. `celldump.c` only prints printable ASCII
   (`r >= 0x20 && r < 0x80`); the em-dash is `U+2014` and gets
   skipped. vts stores the full rune in cells. Fix the dumper if you
   care.

4. **/dev/wctl was the entire previous-session puzzle.** Upstream
   x/term's `getSize` reads `/dev/wctl` and plan9 sends a fatal note
   when the calling process has no rio window (vts session pipes
   don't). Our shim's solution: don't set ttyOutput, don't call
   getSize. Done.

5. **VM ops are fragile under foreground-tool timeouts.** Long-running
   QEMU launched via the tool framework was getting killed on
   wrapper exit. Workaround:
   `~/Projects/plan9-agent/launch-9front-detached.sh` uses `nohup` to
   fully detach. Use that instead of inline QEMU invocation.

## Phase 1 acceptance criteria — all met

| Criterion | Result |
|---|---|
| Cross-compile plan9/amd64 | ✅ |
| Loads in VM via hget | ✅ |
| Launches inside vts session | ✅ |
| Bubble Tea event loop stays alive | ✅ (4-5 procs steady) |
| Renders to vts cells | ✅ (verified via celldump) |
| Correct Luna palette colors | ✅ (fg=12 blue title, fg=8 dim hint) |
| Lipgloss styles applied | ✅ (bold, italic) |
| Window-size detection | ✅ (80x24 in render after poll) |
| Receives keystrokes | ✅ |
| q quits cleanly | ✅ (procs → 0) |

## What's deferred to Phase 2+

- Raw-mode toggle so keys don't need newlines (write `rawon` to vts
  ctl at pi9 startup; restore on exit)
- vtwin running in this VM (vtwin's binary isn't installed; either
  build it from src/vtwin/ or rebuild on each boot)
- Polish: handle terminal resize visually (signal repaint when
  WindowSizeMsg arrives)
- Bubbletea + x/term upstream PRs (deferred to a dedicated session)

These don't block Phase 2 (provider client + streaming response).

## See Also

- [[pi9-architecture]] — overall design
- [[vt-architecture]] — vts internals
- [[vt-9p-namespace]] — vts wire protocol
- `src/pi9/testtools/celldump.c` — cell-level verification tool
- `src/pi9/testtools/README.md` — verification workflow
- `wiki/assets/pi9-phase1-render.txt` — captured render output
- `wiki/assets/pi9-phase1-rendered.png` — visual screenshot
