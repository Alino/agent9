---
name: vts-cells
description: How vts exposes terminal state as a 9P file system (cons, cells, ctl, scroll)
---

# vts / vtwin

vts is the terminal server running at `/srv/vts`. Per-session files:

```
/n/vts/<session>/
├── cons      — write keystrokes here; bytes go to rc's stdin
├── ctl       — rawon, rawoff, edit on/off, size R C, redraw
├── cells     — binary cell-diff stream (for libdraw clients like vtwin)
├── scroll    — UTF-8 scrollback, one row per line
└── size      — spec says it's a separate file but vts hasn't
              implemented this; size lives in the ctl output
```

## Common operations

- **Mount the file system:** `mount /srv/vts /n/vts`
- **List active sessions:** `cat /n/vts/ctl`
- **Type into a session:** `echo "ls" > /n/vts/1/cons`
- **Force a full repaint of vtwin:** `echo redraw > /n/vts/1/ctl`
- **Switch to raw mode:** `echo rawon > /n/vts/1/ctl`
- **Capture scrollback to file:** `cp /n/vts/1/scroll /tmp/log`

## Pi9 inside vts

When pi9 runs inside a vts session:
- The env var `$vts` is set to the session name (typically `1`)
- fd 0/1/2 are pipes from/to vts, not /dev/cons
- Pi9 self-mounts /srv/vts at /n/vts on startup
- Pi9 writes `rawon\nedit off` to ctl at startup, `rawoff\nedit on` on exit

## Killing pi9 properly

`slay pi9` and notes via `/proc/$pid/note` don't kill Go processes
(the runtime swallows notes). Use:

```
for(p in `{ps | grep ' pi9$' | awk '{print $2}'}) echo kill > /proc/$p/ctl
```
