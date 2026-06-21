# agent9

A Windows XP-themed desktop environment for [9front](http://9front.org)
(Plan 9 from Bell Labs), plus a native Plan 9 LLM coding agent, plus a
working web browser. All running as a single QEMU image you can boot
in 15 seconds.

![Start menu open, with the Pi9 LLM agent already running in vtwin in the background](docs/start-menu-and-pi9.png)

The Start menu shows the launcher's app list (Pi9, Rc Shell, Stats,
Acme, Faces, Files, Clock, Reboot, Halt). The vtwin window behind it
is running pi9 — the cyan title bar reads `pi9— moonshotai/kimi-k2.5`.

## Built with AI

Most of agent9 — pi9, the python9 port, the desktop plumbing — was written
with heavy AI assistance (Claude, with some Hermes), and the commit history
reflects that. The work and the results are real: pi9 is dogfooded daily, and
python9 scores 100% parity on CPython's own core regression batch. I'd rather
say so up front than leave it to the log.

## What's in the box

| Component   | Role | Lang |
|---|---|---|
| **mxio**    | Window manager. Rio fork with Luna titlebars, decorations, drag, z-order, minimize/maximize/close. | C / libdraw |
| **xena-panel** | Taskbar daemon. Start button, window list, clock. | C / libdraw |
| **launcher**| Start menu popup. Plumber-backed app launching. | C |
| **vts**     | Terminal session server. 9P file server multiplexing VT100 sessions; replaces st+tmux+rc with one fs at `/srv/vts`. | C |
| **vtwin**   | libdraw frontend for vts. Reads cell diffs, paints into a rio window. | C / libdraw |
| **pi9**     | Plan 9-native LLM coding agent. Bubble Tea TUI, streaming, tool calling, tree-structured sessions, skills/memory, steering, headless modes, OAuth to Anthropic Pro / GitHub Copilot / OpenAI ChatGPT. At feature parity with upstream [pi](https://pi.dev). | Go |
| **NetSurf** | Web browser (from [netsurf-plan9](https://github.com/netsurf-plan9)). | C |
| **python9** | CPython 3.11.14 ported to 9front (kencc/APE), validated at **100% parity** against CPython's own regression suite. Source + parity harness under `python9/`. | C |

![pi9 LLM agent running in a vtwin window](docs/pi9-running.png)

pi9 in vtwin. Cyan header shows the current model and vts session id;
the dashed input box is where you type. The status line tells you to
set an API key with `/login`.

## Try it now

Download `agent9-v0.2.0.qcow2` (524 MB) from the
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

This is **v0.2.0**. New since v0.1.0: the **python9** CPython 3.11
port is now baked into the image (`python` / `python3` on PATH), and
**pi9** is at feature parity with upstream pi (tree-structured
sessions with `/fork` `/clone` `/tree`, steering/follow-up, `@file` +
autocomplete, headless `-p` / `--mode json`, Codex tool calls, and
the AGENTS.md/skills/prompt-template/compaction surface). The basics
work and are dogfooded daily. Known rough edges:

- Pi9's TUI header occasionally scrolls off-screen after a clear
  (Phase 12 target).
- Mouse-wheel scrolling in pi9 is implemented but not yet tested with
  real input devices (QMP can't simulate it cleanly).
- The launcher app list is hardcoded C. Adding entries requires a
  recompile. A config-file rewrite is planned.
- File manager (`xfiles`) is a stub. Use acme or rc for now.
- The xfiles entry in the Start menu doesn't do anything yet.

The **python9** CPython 3.11.14 port is **included in the v0.2.0 image** —
`python` and `python3` are on PATH, with the stdlib at
`/sys/lib/python/lib/python3.11`. It's a hand-authored kencc/APE build that
scores 100.00% (6120/6120, 0 regressions) on the core regression batch against
the host 3.11.14 reference. The source lives under `python9/` (a patch + APE
shims + build scripts + parity harness; the pristine CPython tree is fetched,
not vendored). It's a prerequisite for richer Plan 9 tooling, not a finished
app — porting it does **not** by itself run hermes-agent, whose Rust-backed deps
can't build on Plan 9. See [`python9/README.md`](python9/README.md) for the
parity contract and [`python9/port/plan9/README.md`](python9/port/plan9/README.md)
for the build + bug-class archaeology.

### Installing python9 on a stock 9front (without the agent9 image)

If you run your own 9front and just want the interpreter, build it from the
port in this repo. You need **APE** installed (the `pcc` C driver). The build
runs *in the VM* (no hosted Plan 9 toolchain); it's slow under TCG.

```sh
# On any host, fetch the exact source + apply the Plan 9 patches:
python9/parity/fetch_cpython.sh                 # -> python9/cpython/src (3.11.14)
python9/port/plan9/patches/apply.sh             # applies plan9-cpython.patch

# Copy the port files into the source tree, then push the tree into your 9front
# box (hget / 9P / git9):
#   - python9/port/plan9/pyconfig.h        -> <src>/pyconfig.h
#   - python9/port/plan9/ape-shim/         -> on the include path (-I)
#   - python9/port/plan9/config.c, faulthandler_stub.c  -> built in
```

Then, in 9front, compile + link with `pcc` using the settled flag set
(`pybuild.rc` compiles one file; `pylink.rc` compiles all sources and links a
`python`):

```
pcc -c -D_POSIX_SOURCE -D_BSD_EXTENSION '-Dclockid_t=int' -DPy_BUILD_CORE \
    -I<ape-shim> -I<src> -I<src>/Include -I<src>/Include/internal <src>/<file>.c -o <file>.o
# ... link all objects -> python
```

Finally install:

```
# stdlib (from the CPython source Lib/) and the linked binary:
dircp <src>/Lib /sys/lib/python/lib/python3.11
cp python /$cputype/bin/python
{ echo '#!/bin/rc'; echo 'exec /'^$cputype^'/bin/python $*'; } > /$cputype/bin/python3
chmod +x /$cputype/bin/python3
python --version            # -> Python 3.11.14
```

The PREFIX (`/sys/lib/python`) is compiled into the binary, so it self-locates
the stdlib. Full build narrative, flag rationale, and the kencc/APE bug classes
are in [`python9/port/plan9/README.md`](python9/port/plan9/README.md).

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
