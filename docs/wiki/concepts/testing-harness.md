---
title: Testing Harness
created: 2026-05-15
updated: 2026-05-16
type: reference
tags: [qemu, vnc, qmp, testing, tooling, plan9]
status: done
sources: []
---

# Testing Harness

How to programmatically test UI in the VM without a physical mouse. Hard-won knowledge.

## Components

```
┌──────────────────────────────────────────┐
│ Mac Studio (host)                        │
│  ┌────────────┐   ┌─────────────────┐    │
│  │ QMP socket │←──│ qmp.py          │    │
│  │ /tmp/...   │   │ key/typeln/mouse│    │
│  └────────────┘   │ screendump/raw  │    │
│        ↑          └─────────────────┘    │
│        │                                 │
│  ┌─────┴──────────────────────────────┐  │
│  │ QEMU 9front VM                     │  │
│  │  ├─ VNC :5905 → Jump Desktop      │  │
│  │  ├─ TCP :17010 → host :1717       │  │
│  │  └─ SLIRP host = 10.0.2.2          │  │
│  └────────────────────────────────────┘  │
│                                          │
│  ┌─────────────────────┐                 │
│  │ python http server  │ ← serve sources │
│  │ :8765 0.0.0.0       │                 │
│  └─────────────────────┘                 │
└──────────────────────────────────────────┘
```

## Layers of Testing

| Layer | Tool | Good for |
|-------|------|----------|
| Programmatic | QMP `mouse_move`/`mouse_button`/`typeln` | reproducible scripted tests |
| Visual verify | `screendump` + `vision_analyze` | "does it look right?" |
| Manual | VNC client to your VM's display (e.g. `vnc://127.0.0.1:5905`) | interactive debug, sweep menus |
| Shell | aux/listen1 TCP → `nc 127.0.0.1 1717` | fast command execution |

Rule: for **visual** things verify via screendump+vision. For **interactive** things
(menu hits, drag) when programmatic mouse fails, always fall back to VNC.

## QMP Mouse — GOTCHA: relative coordinates

QEMU PS/2 mouse uses **RELATIVE** delta coordinates, NOT absolute. This is the biggest
trap in this setup.

```
help mouse_move
  → mouse_move dx dy [dz] -- send mouse move events
```

`dx, dy` are DELTAS from the last position. Not absolute pixels.

**Fix for the future:** add `-device usb-tablet` to the QEMU command line.
The tablet device provides absolute coordinates via USB HID protocol.

```bash
# add to boot-9front.sh:
-usb -device usb-tablet
```

(Plan 9 mousewheel/tablet driver must be compatible — verify after restart.)

## QMP Mouse Button Mapping (PS/2 in Plan 9)

QEMU `mouse_button` takes a bitmask. Note — the Plan 9 PS/2 driver maps differently
from the standard:

| QEMU bitmask | Linux/X11 | Plan 9 PS/2 |
|--------------|-----------|-------------|
| 1            | left      | left        |
| 2            | middle    | **right**   |
| 4            | right     | **middle**  |

So on Plan 9: right-click = `mouse_button 2`, middle-click = `mouse_button 4`.

Example correct sequence for right-click (hold + screenshot + release):
```python
hmp("mouse_move 100 50")      # delta move
hmp("mouse_button 2")          # press button-3 (Plan 9 right)
time.sleep(0.5)
hmp("screendump /tmp/menu.ppm")
hmp("mouse_button 0")          # release
```

## screendump + vision_analyze loop

```python
# 1. Snapshot
subprocess.run(["python3", "qmp.py", "screendump", "/tmp/s.ppm"])
# 2. Convert (vision tools want PNG/JPG)
subprocess.run(["sips", "-s", "format", "png", "/tmp/s.ppm",
                "--out", "/tmp/s.png"])
# 3. Ask
mcp.vision_analyze("/tmp/s.png", "Is the menu visible? Highlighted item?")
```

`sips` is macOS built-in, no installation needed.

### Pitfall: vision returns stale info

`mcp__browser_vision` (browser screenshot) is NOT the same as `mcp__vision_analyze`
on a PNG. For QMP screendumps use **vision_analyze** with a file path/URL.

### Pitfall: vision interprets stats widget as menu

The stats(1) widget looks like a menu (rows with colored bar graphs). Vision models
have described it as e.g. "hostname(2) menu". Be explicit in the prompt: "ignore the top-left
stats widget".

## aux/listen1 TCP Shell

Drives shell commands from the Mac over TCP:

```bash
# On VM (via QMP typeln into a window):
aux/listen1 -t tcp!*!17010 /bin/rc -i &

# On Mac:
echo 'date' | nc -w 3 127.0.0.1 1717
```

QEMU forward: `-netdev user,hostfwd=tcp::1717-:17010`

### Pitfall: listen1 silent failures

`aux/listen1` forks a child per connection. If nc disconnects while rc is writing,
the child gets stuck in a `Pwrite` state forever. After several `nc -w` calls you can
end up with 10+ stuck listen1 processes that produce no output.

```rc
ps | grep listen     # if you see many Pwrite, kill them
kill listen1 | rc    # kills all listen1 processes
```

### Pitfall: noisy listen1 incoming messages

`aux/listen1` (without `-q`) logs `incoming call for tcp!*!17010` to stderr
(= stderr of the window where it was started). After 20+ nc calls the terminal
window is flooded with noise and you can't see your output.

Workaround:
```rc
aux/listen1 -t tcp!*!17010 /bin/rc -i >[2]/dev/null &
```

OR: start listen1 in a dedicated window you don't use for reading output.

## File Transfer Mac → VM (hget)

The VM has `hget` (HTTP client). Serve from Mac via `python -m http.server`.

```bash
# On Mac:
cd ~/Projects/plan9-winxp/src/mxio
python3 -m http.server 8765 --bind 0.0.0.0
```

```rc
# On VM:
hget http://10.0.2.2:8765/wind.c > /sys/src/cmd/mxio/wind.c
```

### Pitfall: --bind 0.0.0.0 IS REQUIRED

Default `python3 -m http.server` on macOS binds ONLY to IPv6 `::`. The VM via SLIRP
sends IPv4, gets connection refused, `hget` creates a 0-byte file (SILENTLY).

The build then fails with a confusing error. ALWAYS use `--bind 0.0.0.0`.

### Verify before building

Check that sources were actually copied:
```rc
wc -c /sys/src/cmd/mxio/wind.c    # 41185 bytes (correct size)
                                   # 0 = hget failed
```

## Script Structure (battle-tested)

```python
def shell(cmd):
    """typeln via QMP."""
    subprocess.run(["python3", "qmp.py", "typeln", cmd], capture_output=True)
    time.sleep(0.5)

def focus_terminal():
    """Click into terminal window to give it focus."""
    subprocess.run(["python3", "qmp.py", "mouse", "400", "400", "1"])

def screenshot(name):
    """Take + convert + return PNG path."""
    subprocess.run(["python3", "qmp.py", "screendump", f"/tmp/{name}.ppm"])
    subprocess.run(["sips", "-s", "format", "png",
                    f"/tmp/{name}.ppm", "--out", f"/tmp/{name}.png"])
    return f"/tmp/{name}.png"

# Usage:
focus_terminal()
shell("hget http://10.0.2.2:8765/wind.c > /sys/src/cmd/mxio/wind.c")
shell("cd /sys/src/cmd/mxio && mk install >/tmp/bld.log >[2=1]")
time.sleep(60)   # build
shell("ls -l /amd64/bin/mxio")
shell("date")
png = screenshot("buildresult")
# → mcp__vision_analyze with png to verify timestamps
```

## Reboot Reset Pattern

After significant changes to `wind.c` / `rio.c`, a restart is cleaner than pkill/relaunch:

```python
shell("fshalt -r")
time.sleep(35)            # kernel halt + reboot
shell_key("ret")          # bootargs prompt
time.sleep(2)
shell_key("ret")          # user prompt
time.sleep(15)            # rio/mxio start from profile
png = screenshot("after_reboot")
```

## VNC for Interactive Debug

When programmatic mouse fails (drag, menu hits) → Jump Desktop:
- URL: `vnc://127.0.0.1:5905` (or whichever VNC display your VM exposes)
- macOS Screen Sharing fails (RFB 3.8 mismatch)
- Jump Desktop works out-of-the-box
- For remote dev: a tunnel (Tailscale, ssh -L, socat) forwards the VM host to `127.0.0.1:5905`

## Skip vision: use 9P + framebuffer directly

`screendump` + `vision_analyze` is slow (~3-5s per question), costs tokens,
and the model gets confused by overlapping windows / similar-looking
widgets. For most WM-state questions you can read the data directly:

### Window state — `/mnt/wsys/wsys/<id>/`

mxio (and stock rio) exposes per-window directories under its 9P service.
For each window you can read:

| file       | content                                                  |
|------------|----------------------------------------------------------|
| `label`    | window title text                                        |
| `wctl`     | `minx miny maxx maxy {current\|notcurrent} {visible\|hidden}` |
| `winname`  | the libdraw image name (e.g. `window.4.0`)               |
| `winid`    | the integer id                                           |

```rc
mount -bc /srv/rio.glenda.449 /mnt/wsys
dumpwins                                  # built in src/dumpwins/
# id=4 label=vtwin wctl=  104 304 816 716 current visible
```

`dumpwins` (in `src/dumpwins/`) iterates `/mnt/wsys/wsys/?` and prints
state per window. wctl reads BLOCK until pending state changes — windows
in steady state show `wctl=?` after a 1-time read. Inducing a state
change (e.g. clicking the window) makes wctl deliver immediately.

### Framebuffer pixels — `/dev/screen`

`/dev/screen` is the whole screen as a libdraw image. Format:

```
+-----------------------------------------+
| 12-byte chan name (e.g. "r5g6b5")       |
| 12-byte minx                            |
| 12-byte miny                            |
| 12-byte maxx                            |
| 12-byte maxy                            |  ← 60-byte ASCII header
| ...raw pixel rows...                    |
+-----------------------------------------+
```

For RGB16 (`r5g6b5`): 2 bytes per pixel, little-endian word, layout
`RRRRRGGGGGGBBBBB` (5R 6G 5B).

```c
ushort v = buf[off] | (buf[off+1] << 8);
int R = ((v >> 11) & 0x1F) << 3;
int G = ((v >>  5) & 0x3F) << 2;
int B = ((v      ) & 0x1F) << 3;
```

`/dev/screen` is read-only and only the rio owner can open it.

### titlecheck tool

`src/titlecheck/` is a Plan 9 program that:

1. Finds a window by label via `/mnt/wsys/wsys/<id>/label`
2. Parses its wctl rect
3. Reads `/dev/screen` row-by-row to its titlebar y-range
4. Decodes the RGB16 pixels and counts gradient rows at the center column

```rc
mount -bc /srv/rio.glenda.449 /mnt/wsys
titlecheck vtwin
# found window: id=4 label=vtwin rect=(104,304)-(816,716) current visible
# screen: chan="r5g6b5" rect=(0,0)-(1024,768)
# titlebar pixel scan (col=460):
#   y=306: RGB=(8,36,104)   <- gradient
#   ...
#   y=327: RGB=(160,200,240) <- gradient
# result: 22 gradient rows
```

This is **exact pixel verification** of a UI rendering question, in
~50ms, without LLM tokens. Use it whenever the question is "is this
specific pixel/row/area painted correctly?".

### When to still use vision

- "Does this look weird?" — open-ended visual aesthetic questions
- "Is text rendered correctly?" — text-shape recognition is what vision is good at
- "What does the user see?" — UX-level questions, multiple windows interacting
- First time diagnosing a new visual bug — vision describes the symptom

After that — switch to `dumpwins` / `titlecheck` / direct 9P reads for
mechanical verification.

## See Also

- [[mxio-design]] — what we are testing
- [[build-toolchain]] — push + build pipeline
- [[rio-architecture]] — mouse/keyboard handling in rio
