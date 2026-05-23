---
title: vtwin — libdraw Client for vts
created: 2026-05-16
updated: 2026-05-16
type: concept
tags: [arch, plan9, libdraw, draw, ipc, status-done]
---

# vtwin — libdraw Client for vts

> **Status (2026-05-16):** v1 working in 9front VM. Renders cells, forwards
> keystrokes, survives resize, repaints under mxio's WinXP titlebar.
> Mouse handling + scrollback are future work.

vtwin is the libdraw frontend for a vts session. It is **not** a VT-100
emulator — it does not parse escape codes. It mounts `/srv/vts`, reads a
binary cell-diff stream from `<sess>/cells`, paints each cell with libdraw,
and forwards keystrokes to `<sess>/cons`.

Pairs with [[vt-architecture]] (the daemon) and [[vt-9p-namespace]] (the
wire protocol). The split exists so the same vts session can be rendered
locally (by vtwin, native cells) or remotely (by vt-attach, re-serialized
VT100), and so vtwin stays small — under 650 lines of C.

Source: `src/vtwin/main.c`, `src/vtwin/mkfile`.

## Why a separate client

Plan 9's natural pattern: state in a 9P server, presentation in a separate
process. vts owns the cell buffer, scrollback, and parser; vtwin owns one
window and one font. Multiple vtwins could in principle attach to the same
session (currently untested), and a future curses-style client could attach
to the same `cells` stream without modifying vts.

This also keeps vtwin completely free of terminal-emulation logic. It
doesn't know what ANSI is. It draws cells.

## Threads / procs

Standard libthread layout. Three procs share the cell buffer:

| Proc / thread        | Role                                                         |
|----------------------|--------------------------------------------------------------|
| `threadmain`         | initdraw, mount /srv/vts, run `alt()` over kbd/frames/resize |
| `cellpoll` (proc)    | blocking reads on `<sess>/cells`, posts frame to channel     |
| `titlebar_kicker`    | one-shot: writes `current` to /dev/wctl so mxio repaints     |

`cellpoll` runs in its own kernel proc (proccreate) because its read on
`cells` is blocking — putting it in a libthread coroutine would stall the
main `alt()` loop. The frame buffer (`uchar lastframe[131072]`) is
`static` because a 128 KB local on a 32 KB libthread proc stack will smash
straight past the guard page — silently. See [[vt-architecture]] for that
postmortem.

## Wire format (read side)

vtwin is the reference consumer of the [[vt-9p-namespace#/<session>/cells]]
binary format. Constants live duplicated in `vtwin/main.c`:

```
CD_MAGIC       = 0x76746331   /* 'vtc1' */
CD_HEADER_BYTES = 22
CD_CELL_BYTES   = 12
```

One `read(cells)` returns at most one frame. `process_frame()` decodes the
header, reallocates `gridstate` if the daemon-reported rows/cols changed
(can happen when our `size` write reached vts after the previous frame),
then walks the cell array applying each diff.

The deduped-last-write-wins semantic from the daemon means the client
doesn't need its own coalescing: every cell in the frame is drawn exactly
once.

## Drawing under mxio's titlebar

mxio paints a 22 px WinXP gradient titlebar inside its 2 px window border.
Naively drawing into `screen->r` would overpaint it. vtwin computes a
shifted origin:

```
origin.x = screen->r.min.x + 2 (Selborder) + 1 (wcontentrect inset)
origin.y = screen->r.min.y + 2 (Selborder) + 22 (titlebar) if window tall enough
```

The "tall enough" threshold mirrors mxio's own behaviour: windows below
`Titlebar + 20 = 42 px` get no titlebar (panels, launchers), and vtwin
treats those as full-bleed content.

Flags:

- `-B` — stock rio mode: no titlebar offset (use the whole window).
- `-T n` — override titlebar height (debugging).

The grid is sized as `(screen_dy - top_offset) / cellh` rows and
`(screen_dx - 2*side_inset) / cellw` cols. After resize, vtwin writes
`size R C` to `<sess>/ctl`, then `redraw` to force a full re-emit, then
calls `repaint_all()`.

## Why we call `redraw` so much

vtwin calls `notify_vts_redraw()` at startup AND on every Aresize. Reasons:

1. **Startup race.** Between `open(cells)` and `notify_vts_size`, vts may
   already emit cells for its boot-default buffer size. The first read can
   interleave with our resize ctl in unrecoverable ways.
2. **Resize re-emit.** After `size R C`, vts marks all-dirty internally,
   but if our cellpoll happened to drain the buffer right before the
   resize, the new dimensions land on a fresh empty diff. `redraw`
   guarantees the next read returns every visible cell.

Cost: full repaint of a 30×88 grid (2640 cells × 12 B = ~31 KB) fits
trivially in our 128 KB read buffer.

## Resize and the titlebar wipe

libdraw's `getwindow(display, Refbackup)` reallocates the client image and
fills it with `DWhite` — clobbering the gradient mxio just painted. vtwin
can't suppress the wipe (hardcoded in libdraw `gengetwindow`), so it
instead **triggers mxio to repaint** by writing `current\n` to
`/dev/wctl`. mxio's winctl thread then dispatches Topped → wrepaint →
wborder → wdrawtitlebar in its own draw-thread context, restoring the
gradient.

Same trick on startup (`titlebar_kicker` proc, 200 ms delay) — without
it, libdraw's initial white fill leaves a flat-white titlebar until the
first real interaction.

## Keyboard path

`initkeyboard(nil)` opens `/dev/cons` + `/dev/consctl` and writes
`rawon`, so each keystroke arrives on `kbd->c` individually as a `Rune`.
The main `alt()` loop's Akbd case encodes UTF-8 and writes straight to
`consfd` (the open fid on `<sess>/cons`).

vts's session-spawn code bound that cons file over `/dev/cons` inside
rc's namespace (RFNAMEG), so rc reads our keystrokes as if they were
plain console input. No PTY, no termios.

## Launching

Three entry points exist:

1. **From riostart**, native rio: `window 100,300,820,720 /amd64/bin/vtwin`
   (see `src/_riostart/riostart`). Spawns one default-session vtwin at
   boot.
2. **From mxio's wsys**, via `src/_riostart/launch-vtwin.rc`: binds the
   rio service and writes `new -r 80 80 760 540 /amd64/bin/vtwin` to
   `/mnt/wsys/wctl`. This is the path the start menu will eventually use.
3. **Bare invocation** inside an existing window — explicitly rejected.
   `initmouse` returns nil because `/dev/mouse` is already taken by the
   parent. vtwin fails fast with a helpful message rather than leaking a
   half-initialised orphan into mxio's screen. Use `window vtwin` instead.

CLI: `vtwin [-B] [-T px] [session]`. Session defaults to `"1"`.

## Pitfalls

### `vtwin` invoked twice in the same window

Resulted in a white orphan rectangle on mxio's screen until the next
redraw. Now caught early — see the `initmouse` fail-fast path in
`threadmain` (line ~523). Always launch via `window vtwin` or via mxio's
wsys ctl.

### 128 KB on a 32 KB proc stack

`cellpoll`'s frame buffer must be `static uchar buf[131072]` (BSS), not a
local. proccreate gives the proc a 32 KB stack and the smash is silent —
the proc just stops looping, no crash, no log. If frames mysteriously
stop arriving, check stack sizes first.

### Logs

Two debug log files in `/tmp`:

- `/tmp/vtwin.log` — cellpoll: every Nth frame and any frame with
  ncells > 0
- `/tmp/vtwin-render.log` — process_frame: per-frame geometry dump
  (dimensions, cursor, screen rect). Useful when geometry drifts.

Both are unconditional. Remove the opens once mxio integration ships and
we have a real diagnostics surface.

## See Also

- [[vt-architecture]] — the vts daemon and the two-output-mode design
- [[vt-9p-namespace]] — the wire protocol vtwin consumes
- [[mxio-design]] — titlebar geometry vtwin compensates for
- [[vtwin-typography]] — how vtwin picks its font (`$font` → fontsrv TTF → defaultfont)
- [[draw-api]] — libdraw primitives used here
- [[pi9-architecture]] — the in-progress TUI that runs inside vts/vtwin
