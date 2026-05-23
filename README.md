# agent9

A Windows XP-themed desktop environment for [9front](http://9front.org)
(Plan 9 from Bell Labs), plus a native Plan 9 LLM coding agent, plus a
working web browser. All running as a single QEMU image you can boot
in 15 seconds.

![Start menu open, with the Pi9 LLM agent already running in vtwin in the background](docs/start-menu-and-pi9.png)

The Start menu shows the launcher's app list (Pi9, Rc Shell, Stats,
Acme, Faces, Files, Clock, Reboot, Halt). The vtwin window behind it
is running pi9 — the cyan title bar reads `pi9— moonshotai/kimi-k2.5`.

## What's in the box

| Component   | Role | Lang |
|---|---|---|
| **mxio**    | Window manager. Rio fork with Luna titlebars, decorations, drag, z-order, minimize/maximize/close. | C / libdraw |
| **xena-panel** | Taskbar daemon. Start button, window list, clock. | C / libdraw |
| **launcher**| Start menu popup. Plumber-backed app launching. | C |
| **vts**     | Terminal session server. 9P file server multiplexing VT100 sessions; replaces st+tmux+rc with one fs at `/srv/vts`. | C |
| **vtwin**   | libdraw frontend for vts. Reads cell diffs, paints into a rio window. | C / libdraw |
| **pi9**     | Plan 9-native LLM coding agent. Bubble Tea TUI, streaming, tool calling, sessions/skills/memory, OAuth to Anthropic Pro and GitHub Copilot. | Go |
| **NetSurf** | Web browser (from [netsurf-plan9](https://github.com/netsurf-plan9)). | C |

![pi9 LLM agent running in a vtwin window](docs/pi9-running.png)

pi9 in vtwin. Cyan header shows the current model and vts session id;
the dashed input box is where you type. The status line tells you to
set an API key with `/login`.

## Try it now

Download `agent9-v0.1.0.qcow2` (273 MB) from the
[Releases page](https://github.com/Alino/agent9/releases),
drop it next to the runner script for your OS, run.

```
# macOS
brew install qemu
./run-macos.sh

# Linux (KVM if available)
sudo apt install qemu-system-x86
./run-linux.sh

# Windows
# install QEMU, then double-click run-windows.bat
```

At first boot you'll get two text prompts. Press Enter to both.
After ~15 seconds you're in the desktop. See `release/RUNNING.md`
for details.

## Status

This is **v0.1.0**. The basics work and are dogfooded daily. Known
rough edges:

- Pi9's TUI header occasionally scrolls off-screen after a clear
  (Phase 12 target).
- Mouse-wheel scrolling in pi9 is implemented but not yet tested with
  real input devices (QMP can't simulate it cleanly).
- The launcher app list is hardcoded C. Adding entries requires a
  recompile. A config-file rewrite is planned.
- File manager (`xfiles`) is a stub. Use acme or rc for now.
- The xfiles entry in the Start menu doesn't do anything yet.

## Why

Two reasons.

**One:** I wanted a modern coding agent on Plan 9, the way pi.dev runs
on Mac/Linux. Pi is TypeScript-on-Node and Node won't run on Plan 9,
so I rewrote the shape of it in Go using Bubble Tea, mirroring pi's
OAuth flows, slash commands, model picker, session tree, and skills
system. Then I added Plan 9-native tools that exploit primitives Linux
agents can't access — `plumb`, `walk`, `ns`, `bind`, `mount` — so the
agent can compose namespace sandboxes in single tool calls and read
its own runtime environment as files.

**Two:** Plan 9 ships with `rio`, which works great for Plan 9 people
and is alien for everyone else. I wanted to find out what 9front looks
like wearing a familiar window manager. Turns out, perfectly fine.
mxio inherits rio's tiling sweep behavior but adds chrome that people
recognize: titlebars, minimize/maximize/close, taskbar, a Start
button. The Luna palette is hardcoded, not themed. v0.1 isn't trying
to be configurable; it's trying to be a comfortable place to land.

## Architecture

```
  ┌─────────────────────────────────────────────────────────┐
  │ vtwin (libdraw cell-grid renderer)                      │
  │   ↓ reads /n/vts/<sess>/cells (binary diff stream)      │
  │   ↑ writes /n/vts/<sess>/cons (keystrokes)              │
  │                                                         │
  │ vts (9P session server at /srv/vts)                     │
  │   ↓ VT100 parser, cellbuf maintenance                   │
  │   ↓ ptyfork-equivalent (rfork) into rc                  │
  │                                                         │
  │ pi9 / acme / rc / netsurf / anything                    │
  │                                                         │
  │ mxio (window manager, drawn from rio.c +titlebars)      │
  │ xena-panel (taskbar; reads /dev/wsys for window list)   │
  │ launcher (start menu, plumber-driven app launch)        │
  └─────────────────────────────────────────────────────────┘
```

Source for each component lives in `src/<name>/` with its own
`mkfile`. Build inside the VM:

```
cd /sys/src/cmd/mxio && mk install
```

There's no cross-build from macOS — Plan 9 has no hosted toolchain.

For pi9 specifically, Go cross-compiles from any host:

```
cd src/pi9 && GOOS=plan9 GOARCH=amd64 go build .
```

## Hacking on it

The QEMU image has the source under `/sys/src/cmd/{mxio,vts,vtwin,xena-panel,launcher}/`
ready to `mk` (Plan 9 build tool). Edit on the host, rsync into the VM
via the included listen1 + http shuttle pattern in `tools/`, or just
ssh in (`ssh -p 2222 glenda@localhost`, no password) and edit with
acme.

Wiki (architecture, gotchas, per-phase write-ups for pi9 etc.) lives
at `wiki/`. Start with `wiki/index.md` for a table of contents.

For day-to-day workflow patterns, see `docs/development.md`.

## Credits

- [Plan 9 from Bell Labs](https://9p.io/plan9/) / [9front](http://9front.org) — the OS this all runs on.
- [pi.dev](https://pi.dev) — the agent shape pi9 mirrors. Source at <https://github.com/earendil-works/pi>.
- [charm.sh](https://charm.sh) — Bubble Tea / Lipgloss / Bubbles, the TUI stack pi9 builds on.
- [netsurf-plan9](https://github.com/netsurf-plan9) — the NetSurf port.

## License

MIT. See LICENSE.
