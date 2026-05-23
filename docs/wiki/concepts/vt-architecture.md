---
title: vt — Architecture
created: 2026-05-16
updated: 2026-05-16
type: concept
tags: [arch, plan9, ipc, status-done]
---

# vt — Architecture

> **Status (2026-05-16):** v1.0 implemented. Phases 0, 1, 2, 4, 5, 6, 7, 8
> are done. Phase 3 (mxio renderer client) is intentionally deferred —
> the protocol and daemon are ready, mxio just needs a client that mounts
> /srv/vts and draws cells with libdraw.
>
> Tests: 62/62 unit tests on macOS, 6/6 integration scripts in 9front VM.

vts is a 9P file server that collapses what Unix splits across three tools — st
(VT100 rendering), tmux (session persistence + multiplexing), and zsh (line
editing + completion) — into a single Plan 9 service. See
[[vt-9p-namespace]] for the wire interface and [[vt100-parsing]] for the
escape-code subset we implement.

## Goal

Bring "modern terminal" features to Plan 9 without inheriting Unix's
PTY/termios baggage. Each component (rendering, persistence, line editing) is
something Plan 9's primitives already accommodate — they were just never
plumbed together. vt is the plumbing.

## Non-goals

- PTYs. Programs that gate features on `isatty(3)` (htop, lazygit, …) will
  still see a dumb pipe. That is an APE-kernel problem, not a terminal problem.
- A different shell. rc is the session shell; line editing happens *outside*
  rc, intercepted by vt before keystrokes reach the shell.
- Mouse reporting, 256-color, sixel, image protocols. Scoped out of v1.

## Components

```
                ┌──────────────────────────┐
                │            vt            │   ← single process
                │  ┌──────────────────┐    │
                │  │  per-session     │    │
                │  │  state:          │    │
   rc stdin  ←──┼──┤   cell buffer    │    │
   rc stdout ─→ │  │   scrollback     │    │
                │  │   line editor    │    │
                │  │   VT100 parser   │    │
                │  └──────────────────┘    │
                │           │              │
                │   lib9p Tree exposes:    │
                │   /srv/vt/<name>/cons    │
                │                  cells   │
                │                  size    │
                │                  scroll  │
                │                  ctl     │
                └──────────┬───────────────┘
                           │ 9P over file descriptor
              ┌────────────┴────────────┐
              │                         │
      ┌───────▼────────┐        ┌───────▼────────┐
      │  mxio client   │        │   vt-attach    │
      │ (libdraw cells)│        │ (VT100 stdout) │
      └────────────────┘        └────────────────┘
        native local              ssh, drawterm
```

The cell buffer is the source of truth. Two output modes exist because the
buffer can be rendered two ways:

- Locally: send binary diffs to mxio, which draws them with libdraw.
- Remotely: re-serialize back into VT100 escape codes for a dumb TTY pipe.

## Why this works on Plan 9

| Unix problem               | Unix tool | Plan 9 mechanism vt uses |
|----------------------------|-----------|--------------------------|
| Programs emit escape codes | st parses | vt parses the same codes, but the *buffer* is the interface, not the codes |
| Multiplexing               | tmux panes | Window manager already multiplexes — vt just serves multiple sessions |
| Detach / reattach          | tmux server | 9P file server stays alive when clients disconnect; namespace persists |
| Line editing / completion  | zsh / readline + termios raw | `/dev/consctl rawon` (kernel primitive) + line editor in vt |
| Remote attach              | ssh + tmux | ssh + vt-attach speaking the same 9P |

## See Also
- [[vt-9p-namespace]] — wire interface and file semantics
- [[vt100-parsing]] — escape-code subset
- [[rio-architecture]] — how rio currently serves /dev/cons
- [[mxio-design]] — where the renderer client plugs in

## Pitfalls discovered the hard way

### Repaint races on session startup (fixed 2026-05-16)

vtwin's startup originally went: mount /srv/vts → open
/n/vts/<s>/cells → recalc_grid → notify_vts_size → repaint_all →
spawn cellpoll. Two problems with this:

1. **Repaint race.** Between open(cells) and notify_vts_size, vts may
   already emit the all_dirty backlog for whatever size vts boot-set
   the buffer to. cellbuf_resize after notify_vts_size re-marks
   all_dirty but the cell stream readers can interleave such that
   some buffer state is lost.
2. **First-frame fragmentation.** A full all_dirty drain emits cells
   in chunks bounded by the read buffer size. With a 16KB buffer
   that's 1363 cells/read — a 30×88 grid needs two reads.

Symptom: vtwin appears blank or shows partial output even though vts
is healthy; keystrokes work but display lags reality.

Fix: new `redraw` ctl command in vts (`s->buf.all_dirty = 1`, no
resize). vtwin calls it at startup and on every Aresize. 128KB cells
read buffer so 100×100 full repaint drains in one read.

### Stack overflow in cellpoll (fixed 2026-05-16)

`proccreate(cellpoll, cells_path, 32*1024)` allocates a 32KB stack.
A 128KB `uchar buf[131072]` local variable in the proc body smashes
past it. The libthread proc crashes silently — no log, no error,
just stops looping.

Always declare large buffers `static` (BSS) or `malloc` them (heap)
rather than putting them on a libthread proc stack.

