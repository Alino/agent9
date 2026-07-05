# Agent Orientation — agent9

Read this first. Every session.

## What this project is

A Windows XP-themed desktop environment for 9front, plus a Plan-9-native
LLM coding agent. Native libdraw, written in C (and Go for pi9),
targeting 9front amd64.

Components:

- `src/mxio/`       — window manager (Rio fork with titlebars, decorations, drag, z-order)
- `src/xena-panel/` — taskbar daemon (30px strip, window list, clock, start button)
- `src/launcher/`   — start menu + app launcher (plumber-backed)
- `src/vts/`        — terminal session 9P server (cells/cons/ctl)
- `src/vtwin/`      — libdraw frontend for vts
- `src/pi9/`        — LLM coding agent (Go + Bubble Tea)
- `src/xfiles/`     — file manager (TODO)
- `pac9/`           — package manager (rc): `pac9 install <name-or-git-url>`

## Wiki — read before doing anything

`wiki/` documents architecture, gotchas, and per-phase decisions.

**Always orient first.** Before writing code, fixing bugs, or
answering architecture questions:

1. `read_file wiki/SCHEMA.md`        — conventions, tag taxonomy, page rules
2. `read_file wiki/index.md`         — what pages exist and their summaries
3. `read_file wiki/log.md` (last 30) — recent decisions and activity

Then `search_files` inside `wiki/` for whatever you're about to touch
— there is probably already a concept page that answers your question.

**Update the wiki when you learn something.** Architecture decisions,
API gotchas, Plan 9 quirks discovered the hard way — all go in
`wiki/concepts/`. Append to `wiki/log.md` for any non-trivial change.
This is how the project's memory compounds across sessions instead of
evaporating.

## Build / VM context

- Build host: macOS Apple Silicon or Linux
- 9front VM: see `release/` for the prebuilt qcow2 + runner scripts.
  For development, you'll typically have a separate dev VM (`9front-disk.qcow2`)
  next to it.
- Cross-compile pi9 (Go) on the host. Cross-compile C components only
  inside the VM (no hosted Plan 9 toolchain exists).
- See `docs/development.md` for the dev loop.

## Style

- C, K&R style, 8-space tabs (Plan 9 convention)
- No malloc/free patterns from Linux — use Plan 9 idioms (mallocz, sysfatal, threads, channels)
- libdraw for all rendering — never assume X11, never assume framebuffer
- Build with `mk`, not make. Mkfile per component.

## What NOT to do

- Rust now runs on 9front: `rust9/` has `x86_64-unknown-plan9` as a built-in rustc
  target + a `std` port over the cc9 runtime — threads/sync, panic=unwind, fs,
  process, TCP+UDP all real; ~2309 upstream coretests pass. The real rustc
  (cranelift) self-hosts on the box: `pac9 install rust9`. See `rust9/README.md`.
- Don't propose Python anywhere in the runtime. Python 3 isn't runtime-ready
  (a CPython 3.11 port is in progress under `python9/` — see
  `docs/wiki/concepts/python3-on-plan9.md` — but it doesn't run yet).
- Don't propose neovim. 9vim exists, or learn acme.
- Don't bring Linuxisms into the C code (epoll, pthreads, signalfd, etc.). Plan 9
  has threads(2), channels, alt(), 9P, and rfork. Use them.
