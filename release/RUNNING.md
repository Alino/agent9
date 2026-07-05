# Running agent9

You need two things:
1. **QEMU 7.2+** on your host.
2. **agent9-v0.5.0.qcow2** — the disk image. Download from the
   GitHub Releases page. New in v0.5.0: **cc9** — modern C++ on 9front —
   now also compiles **on the box** (a reduced native clang + `ld.lld`), and
   the image ships seven prebuilt C++ demos under `/usr/glenda/cc9/` (run
   `/usr/glenda/cc9/RUNDEMOS`): **Stockfish** (self-verifying bench), a
   raytracer, nlohmann/json, STL, exceptions, threads, and a brainfuck JIT.
   The C++ runtime is validated against the upstream libc++ / libc++abi /
   libunwind conformance suites. The image also carries the opt-in **W^X
   kernel patch** (off by default), boots unattended, accepts **drawterm**
   connections out of the box (see below), and fixes mxio painting
   titlebars over graphical clients like acme. v0.4.0 introduced cc9 (clang/LLVM
   cross-toolchain). v0.3.0 added **node9** — a Node.js-compatible runtime
   running the real npm (`node` / `npm` on PATH). v0.2.0 added the python9
   CPython 3.11 port (`python` on PATH) and pi9 at feature parity with
   upstream pi.

Drop the qcow2 next to the run script for your OS, then run it.

## macOS

```
brew install qemu
./run-macos.sh
```

Apple Silicon runs in TCG (no cross-arch HVF). About 50-70% of native
speed, which is enough for the desktop and pi9 turns. NetSurf renders
pages in seconds. Builds inside the VM are slow (40-60s for mxio,
~20min for NetSurf) but you almost never need to rebuild anything.

Intel Macs get a 2-3x speed-up via HVF. Edit the script to add
`-accel hvf` if you want it.

## Linux

```
sudo apt install qemu-system-x86  # or dnf / pacman / zypper
./run-linux.sh
```

Linux gets KVM automatically if available. ~native speed.

## Windows

Install QEMU from https://www.qemu.org/download/#windows, add to PATH,
then double-click `run-windows.bat`. WHPX acceleration kicks in
automatically on Windows 10+.

## First boot

The image boots unattended (`nobootprompt` and `user=glenda` are baked
into plan9.ini) — no keypresses needed. Init runs, mxio paints the
desktop, xena-panel paints the taskbar, and a vtwin terminal pops up.
About 15 seconds end-to-end on KVM, 30-45 on Apple Silicon TCG.

## Connecting with drawterm

The image runs a Plan 9 auth server + rcpu listener, so you can attach
real desktops from your host with [drawterm](https://drawterm.9front.org)
instead of (or alongside) the QEMU console:

```
drawterm -h tcp!127.0.0.1!17019 -a tcp!127.0.0.1!1567 -u glenda
```

Password: `agent9agent9` (authdom `agent9`). On Windows use
`drawterm-amd64.exe` with the same arguments.

Why bother: drawterm windows resize dynamically, you get your host
filesystem inside the VM at `/mnt/term`, and each connection is an
independent desktop — run as many as you like. The dial strings carry
non-standard ports because the run scripts forward host 17019 → guest
17019 (rcpu) and host 1567 → guest 567 (auth; 567 is privileged on the
host side).

The credentials are public, like everything else in this dev image —
don't expose the forwarded ports beyond localhost. To change them:
`auth/wrkey` (nvram) + `auth/changeuser glenda` (/adm/keys) inside the
VM, then update the factotum key line in
`/usr/glenda/bin/rc/riostart`.

## Using Pi9

`/usr/glenda/lib/pi9/config` has the config template. Either edit it
with an API key:

```
api_key=sk-or-v1-yourkeyhere
model=anthropic/claude-sonnet-4.5
```

…or run pi9 and use `/login` to start an OAuth flow with Anthropic
Claude Pro, GitHub Copilot, or OpenAI ChatGPT (Codex). The OAuth
callbacks run on host ports 53692 (Anthropic/Copilot) and 1455
(Codex), which the run scripts forward into the VM.

(The image ships without credentials. On first run pi9 writes a
config template to `/usr/glenda/lib/pi9/config` and tells you to add
a key or `/login`.)

From the rc terminal:

```
new-pi9
```

Or click the **Start** menu and pick **Pi9**.

## Networking

Out of the box you get NAT through QEMU's user-mode networking.
Outbound HTTP/HTTPS works (pi9 hits Anthropic / OpenRouter, mothra
loads pages). Inbound: ports 22 (ssh), 17010 (listen1 dev shell),
17019 (rcpu, for drawterm), 567 (auth server, for drawterm), 564
(9P export), 53692 + 1455 (OAuth callbacks) are forwarded from host
ports 2222 / 1717 / 17019 / 1567 / 1564 / 53692 / 1455.

To SSH in: `ssh -p 2222 glenda@localhost` (no password — for dev
convenience, this image is not hardened).

## What's installed

- mxio (window manager, Rio fork with Luna titlebars)
- xena-panel (taskbar)
- launcher (start menu)
- vts + vtwin (terminal server + libdraw client)
- pi9 (LLM agent, see source at /sys/src/cmd/... in repo)
- python9 (CPython 3.11.14, `python` / `python3` on PATH; stdlib at
  /sys/lib/python)
- node9 (Node.js-compatible runtime on QuickJS-ng + the real npm 10;
  `node` / `npm` on PATH; runtime at /amd64/lib/node9)
- NetSurf 3.x (web browser, Plan 9 port from netsurf-plan9)
- Plus the standard 9front stack: acme, plumber, mothra, hget, git9,
  9pm, etc.

## Using Python

The python9 port (CPython 3.11.14, the Plan 9 way) is on PATH:

```
python --version          # Python 3.11.14
python -c 'print(2+2)'
python script.py
```

The standard library lives at `/sys/lib/python/lib/python3.11`. Native
extensions that need a C/Rust toolchain or OS primitives Plan 9 lacks
won't import; the pure-Python stdlib works. See
https://github.com/Alino/agent9 (`python9/`) for the port + parity
harness, and the README for building it on a stock 9front.

## Using node9 (Node.js + npm)

node9 is a Node.js-compatible runtime (built on QuickJS-ng, not V8)
with the **real, unmodified npm** on top. `node` and `npm` are on PATH:

```
node script.js
npm --version             # 10.9.8
mkdir /tmp/app && cd /tmp/app
npm install left-pad      # real install from registry.npmjs.org over TLS
```

`npm install` fetches from the registry over TLS, SHA-512-verifies the
tarball, and extracts to `node_modules` with a lockfile; dependency
graphs resolve. Pure-JS packages (CommonJS and ESM) work — 30/30 popular
packages were verified. No native addons (`.node`), no HTTP/2, and it's
interpreter-speed (large packages install slowly). See
https://github.com/Alino/agent9 (`node9/DOCUMENTATION.md`) for the full
fidelity/limitations write-up and stock-9front install steps.

## Persistence

The qcow2 stores all your state — sessions, downloaded files, edited
source. Back up the qcow2 file to back up your environment. Snapshot
with `qemu-img snapshot -c name agent9-v0.5.0.qcow2` between
experiments.

## Troubleshooting

**The display window doesn't appear.** Try a different `-display`
backend: edit the run script and change `cocoa` / `gtk` / `sdl`.

**No network in the VM.** Check that QEMU's user-mode networking is
working: `ping 8.8.8.8` inside the VM, then `host 9front.org`.
If `host` fails but ping works, edit `/lib/ndb/local` to add a DNS
server.

**Pi9 says "no auth".** Edit `/usr/glenda/lib/pi9/config` with your
API key, or run pi9 and type `/login`.

**VM is slow.** Make sure you're using virtio devices (the run scripts
do), and that hardware acceleration is enabled (KVM on Linux, WHPX
on Windows, HVF if you tweak macOS). Apple Silicon TCG will always
be slow.

## Reporting bugs

Source at https://github.com/Alino/agent9. File issues for
anything weird; PRs welcome for fixes.
