---
title: xena-panel Design
created: 2026-05-15
updated: 2026-05-16
type: decision
tags: [arch, panel, taskbar, winxp, plan9]
status: wip
---

# xena-panel Design

WinXP-style taskbar daemon for mxio. Single C binary, ~370 lines, links against
libdraw + libthread + libc.

## Status (2026-05-16)

**v0.1 (verified visually in mxio VM):**
- ✅ Bottom 30px strip, blue gradient background
- ✅ "Start" button (left, green) with hit-test
- ✅ Window list — enumerated via `/dev/wsys/N/{label,wctl,winid}`
- ✅ Clock (right, HH:MM, refreshes every 500ms)
- ✅ Self-resizes to bottom of screen via `/dev/wctl`
- ✅ Auto-launches from riostart on boot
- ✅ Button-1 click dispatched: Start → launcher popup; window button → focus that window

**Not yet:**
- ⏸ Click-verified end-to-end (focus/mouse plumbing flaky in QMP — needs Jump Desktop manual test)
- ⏸ Minimize integration (panel needs to keep "hidden" windows in list)
- ⏸ Drag-reordering window buttons

## Architecture

```
xena-panel (C binary in /amd64/bin/)
  │
  ├─ initdraw → own rio window
  ├─ /dev/wctl  ← write "resize -r 0 738 1024 768"   (move self to bottom)
  ├─ /dev/wsys/ ← read dir, list window IDs
  ├─ /dev/wsys/N/label  ← read each window's label
  ├─ /dev/wsys/N/wctl   ← read for "current" state; write "current\n" to focus
  ├─ /dev/winid         ← read own ID (to skip self in list)
  │
  └─ proccreate(ticker) → sleep(500) → readwins() → redraw()
```

## Layout

```
┌──────────────────────────────────────────────────────────┐
│ [Start]   [stats] [rc]  [acme]  ...              12:34 │
└──────────────────────────────────────────────────────────┘
 60px       middle (equal-width, capped 160px each)    70px
```

Constants in `xena-panel.c`:
- `PanelH = 30` — panel height
- `StartBtnW = 60`, `ClockW = 70`, `WinBtnMaxW = 160`, `WinBtnMinW = 40`
- `BtnPad = 4`
- `Refresh = 500` — ticker interval in ms

Colors:
- Panel background `0x3c5b91` (blue-gray gradient approximation)
- Start button `0x4d8c4c` green (active `0x65b463`)
- Window button `0x6080b8` (current/focused: `0xa6cbf0` highlight)

## Click dispatch

In the main event loop, button-1 press (edge-triggered to avoid double-fire on hold):

```c
if((m.buttons & 1) && !(prevbtns & 1)){
    panelclick(m.xy);
}
prevbtns = m.buttons;
```

`panelclick(p)`:
- in startbtnr → `spawnlauncher()` forks `window -r 0 518 200 738 launcher`
- in wins[i].btnr → `focuswin(id)` writes "current\n" to `/dev/wsys/id/wctl`

## File hooks on the VM

```
/usr/glenda/lib/profile     calls 'mxio -i riostart' (set during initial setup)
/usr/glenda/bin/rc/riostart adds 'window 0,738,1024,768 xena-panel' before
                            the initial terminal spawn
/amd64/bin/xena-panel       built via 'cd /sys/src/cmd/xena-panel && mk install'
```

## Minimize plan (not implemented)

Plan 9 has no native minimize.  Approach when implemented:

1. mxio's button-2 (min) handler currently calls `wtopme(w)`.  Change to:
   - Save w->screenr in w->savedrect
   - Set w->screenr = ZR (hide visually)
   - Add to "hidden window list" in mxio
   - Mark in /dev/wsys/N/wctl as "hidden"
2. xena-panel reads the "hidden" state from wctl and shows those buttons
   with a different visual (dimmed).
3. Click on hidden window button → write "current" + the wctl will restore
   visibility via mxio reacting to current request.

Alternative: panel maintains its own hidden list via a /srv pipe, mxio
publishes hide/show events.  More work but cleaner separation.

## Build

```rc
cd /sys/src/cmd/xena-panel
mk install        # produces /amd64/bin/xena-panel
```

Mkfile uses the standard `/$objtype/mkfile` prelude with `BIN=/$objtype/bin`
and links libdraw + libthread + libc (not libplumb yet).

## Pitfalls discovered

- **Initial mkfile from `mkone` template needs `BIN=/$objtype/bin`** explicitly;
  otherwise `cp 6.out $BIN/...` fails with rc error "null list in concatenation".
- **`/dev/wctl` resize syntax**: `resize -r minx miny maxx maxy\n` — newline
  matters, both minx,miny AND maxx,maxy required.
- **Skip own winid**: read `/dev/winid` once at startup, skip that ID in
  the window list to avoid showing yourself in the taskbar.

## See Also

- [[mxio-design]] — the host WM that hosts this panel
- [[testing-harness]] — how to verify panel changes
- [[build-toolchain]] — mkfile structure and build pipeline
