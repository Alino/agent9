# Running agent9

You need two things:
1. **QEMU 7.2+** on your host.
2. **agent9-v0.1.0.qcow2** — the disk image. Download from the
   GitHub Releases page (~273 MB).

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

## First-boot prompts

You'll see two text prompts before the desktop comes up:

1. `bootargs is (tcp, tls, il, local!device)[local!/dev/sdF0/fs -m 296]`
   → press **Enter**
2. `user[glenda]:`
   → press **Enter**

Then init runs, mxio paints the desktop, xena-panel paints the
taskbar, and a vtwin terminal pops up. About 15 seconds end-to-end on
KVM, 30-45 on Apple Silicon TCG.

## Using Pi9

`/usr/glenda/lib/pi9/config` has the config template. Either edit it
with an API key:

```
api_key=sk-or-v1-yourkeyhere
model=anthropic/claude-sonnet-4.5
```

…or run pi9 and use `/login` to start an OAuth flow with Anthropic
Claude Pro / GitHub Copilot. The OAuth callback runs on host port
53692, which the run scripts forward into the VM.

From the rc terminal:

```
new-pi9
```

Or click the **Start** menu and pick **Pi9**.

## Networking

Out of the box you get NAT through QEMU's user-mode networking.
Outbound HTTP/HTTPS works (pi9 hits Anthropic / OpenRouter, mothra
loads pages). Inbound: ports 22 (ssh), 17010 (listen1 dev shell),
564 (9P export), 53692 (OAuth callback) are forwarded from host
ports 2222 / 1717 / 1564 / 53692.

To SSH in: `ssh -p 2222 glenda@localhost` (no password — for dev
convenience, this image is not hardened).

## What's installed

- mxio (window manager, Rio fork with Luna titlebars)
- xena-panel (taskbar)
- launcher (start menu)
- vts + vtwin (terminal server + libdraw client)
- pi9 (LLM agent, see source at /sys/src/cmd/... in repo)
- NetSurf 3.x (web browser, Plan 9 port from netsurf-plan9)
- Plus the standard 9front stack: acme, plumber, mothra, hget, git9,
  9pm, etc.

## Persistence

The qcow2 stores all your state — sessions, downloaded files, edited
source. Back up the qcow2 file to back up your environment. Snapshot
with `qemu-img snapshot -c name agent9-v0.1.0.qcow2` between
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
