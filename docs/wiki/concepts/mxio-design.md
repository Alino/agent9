---
title: mxio Design
created: 2026-05-15
updated: 2026-05-16
type: decision
tags: [plan9, rio, wm, arch, winxp]
status: done
---

# mxio Design

mxio is our fork of Rio with WinXP decorations. Name: **m**odified **xio** (Plan 9 naming convention).

## Status (2026-05-15)

**Verified working:**
- ✅ WinXP Luna gradient titlebar (#0A246A → #A6CAF0)
- ✅ Close/Min/Max buttons rendering (15×15px, right-aligned)
- ✅ Close button kills window (button-1 click)
- ✅ Window title text shows (e.g. "rterm%", "hostname(2)")
- ✅ Desktop background #3A6EA5 (WinXP blue)
- ✅ Right-click menu simplified: "New Terminal" / "Exit"
- ✅ Binary: /amd64/bin/mxio, 362965 bytes, builds clean

**Not yet implemented / blocked on tooling:**
- ⏸ Titlebar drag-to-move (untested — needs working absolute mouse)
- ⏸ Min/Max button handlers (stubs only, no taskbar yet for min)
- ⏸ New Terminal spawn confirmation (menu shows but spawn untested due to QMP relative mouse — see [[testing-harness]])
- ⏸ xena-panel (taskbar daemon)
- ⏸ launcher (Start menu)
- ⏸ xfiles (file manager)

## What We Keep from Rio

- The entire 9P file server (xfid.c) — clients get the same /dev/cons, /dev/consctl, /dev/winname
- Mouse and keyboard handling core
- Client/Window lifecycle (fork, exit, cleanup)
- Plumber integration (/srv/rio → /srv/mxio, rename only)
- Scrollbar mechanism (terminal windows)

## What We Change

### wind.c — drawing engine

Original:
```c
#define Borderwidth 4
void wborder(Window *v, int type) {
    border(screen, v->screenr, Borderwidth, type ? red : white, ZP);
}
```

Implemented (actual values in `dat.h`):
```c
#define TITLEBAR_H   25    // titlebar height
#define Titlebar     22    // legacy alias
#define BTNSIZE      15    // 15×15 close/min/max
#define BTNMARGIN     3    // right margin
#define Btnw          2    // legacy alias
#define Selborder     2    // active border (was 4 — slimmer)
#define Unselborder   1    // inactive border

/* WinXP Luna gradient */
0x0A246AFF  → 0xA6CAF0FF   /* active titlebar */
0x7A96DFFF  → 0xC8D4F5FF   /* inactive */
0xC75050FF  /* close button red */
0x4A7EBBFF  /* min/max button blue */

void wdrawtitlebar(Window *v, int active);   /* gradient + 3 buttons */
Rectangle wcontentrect(Window *v);            /* inner rect minus titlebar */
int whittest(Window *v, Point p);             /* 0=content 1=close 2=min 3=max 4=titlebar */
```

### Performance: replchan trick for gradient

The naive approach (alloc 1px image per column → 640+ allocs per draw) exhausted the image pool.
Solution: one 1×1 `Image` with `setalpha`+`replchan`, draw a rect for each column reusing the same image.
See `wind.c::wdrawtitlebar()`.

### rio.c — mouse hit testing + simplified menu

Original: button2 anywhere in a window = move, button3 = sweep+rubber-band rio menu (New/Resize/Move/Delete/Hide/Exit).

Our hit-test:
```c
int whittest(Window *v, Point p);
/* returns:
   0 = HitContent      forward to client
   1 = HitClose        wsendctlmesg(w, Deleted, ZR, nil)
   2 = HitMin          stub (needs [[xena-panel-design]])
   3 = HitMax          stub (toggle fullscreen, todo)
   4 = HitTitle        drag-to-move (drag(w))
*/
```

Hit-test is wired in `rio.c::mousethread`.

Our button-3 menu (rio.c::button3menu):
```c
enum { MxNew = 0, MxExit = 1, Hidden = 2 };  /* Hidden = sentinel */
char *menu3str[] = { [MxNew] "New Terminal", [MxExit] "Exit", nil };

case MxNew:
  /* fixed-size terminal at (50,130)..(640,350), no rubber-band sweep */
  r = screen->r;  r.min.x += 50; r.min.y += 130;
  r.max.x = r.min.x + 640; r.max.y = r.min.y + 350;
  ni = allocwindow(wscreen, r, Refbackup, DNofill);
  new(ni, FALSE, scrolling, 0, nil, "/bin/rc", nil);

case MxExit:
  confirmexit();
```

### GOTCHA: framebuffer console bleed-through

`/bin/riostart` (mxio's `-i` script) is executed by the bootstrap rc
that launched mxio. That rc's fd1/fd2 are bound to the kernel
`/dev/cons` (raw framebuffer text console). Anything `riostart` forks
inherits those fds. If a daemon prints to fd1 or fd2, the text lands
on the framebuffer **beneath** mxio's wscreen. devdraw doesn't track
those writes → wscreen recompose can't repaint over them → stuck
white-on-blue text fragments outside every window rectangle.

Symptom: "vts: hello (build ...)" and "vts: posted at /srv/vts"
fragments visible at fixed screen positions, immovable, disappearing
only when a window happens to drag across them.

**Fix**: `>/dev/null >[2]/dev/null` on every daemon spawn in riostart.
Both fds — Plan 9 `print()` goes to fd 1, `fprint(2, ...)` to fd 2.

```rc
aux/listen1 -t tcp!*!17010 /bin/rc -i >/dev/null >[2]/dev/null &
/amd64/bin/vts >/dev/null >[2]/dev/null &
{ ... } >/dev/null >[2]/dev/null &
```

DO NOT use a single `exec >/dev/null >[2]/dev/null` at the top of the
script — that broke spawning entirely in testing (window commands
silently failed, aux/listen1 children immediately RST'd). Per-daemon
redirects work.

Why mxio's own initial `draw(view, viewr, background, nil, ZP)` at
startup doesn't fix this: vts prints AFTER mxio's clear, because
riostart is spawned by `proccreate(initcmd, ...)` in parallel with
the rest of mxio's setup. The race favours mxio paint → vts paint →
no further recompose.

### GOTCHA: sweep orphan pixels (Refnone)

Stock rio's `sweep()` (rio.c, rubber-band loop for window creation) used
`allocwindow(wscreen, r, Refnone, DNofill)` per mouse-tick. **Refnone**
means devdraw keeps no backing store, so when the image is freed it does
NOT auto-recompose the parent (background blue) over the freed rectangle.

Stock rio's invariant: user drags outward only → each new rect contains
the previous one → final `Refbackup` window covers everything ever
painted. The invariant breaks the moment the user drags inward, aborts
mid-sweep, or a client crashes mid-paint. Result: a fringe of orphan
white pixels around the final window that only disappear when another
window happens to drag over them (because *that* window uses Refbackup
and the compositor DOES recompose its old rect on freeimage).

**Fix**: change `Refnone` → `Refbackup` on the per-tick alloc.

```c
/* rio.c::sweep, inside the mouse-buttons==4 loop */
i = allocwindow(wscreen, r, Refbackup, DNofill);
```

Cost: one transient screen-fragment backing store per mouse-tick during
sweep. We are not memory-constrained in a 2GB VM. `drawborder` in
`drag()` already uses Refbackup for the same reason.

Note: as of v0.1 our button-3 menu's "New Terminal" uses a fixed rect
(no sweep). sweep() is currently dead code in the normal flow, but
guarded against re-enablement and against any future path that calls it.

### GOTCHA: title text needs explicit redraw

The window title (`w->label`) is set via 9P writes to `/dev/label` (handled by xfid.c::Qlabel)
or via `wsetpid()`. Stock Rio draws the titlebar only on border-color changes — never for text.
With a WinXP titlebar that SHOWS the label you must force an explicit redraw:

```c
/* xfid.c::Qlabel — after `memmove(w->label, x->data, cnt)`: */
wborder(w, w==input ? Selborder : Unselborder);
flushimage(display, 1);

/* wind.c::wsetpid() — after `w->label = estrdup(buf)`: */
wborder(w, w==input ? Selborder : Unselborder);
flushimage(display, 1);
```

Without this: the terminal window shows an empty "rc" label in the titlebar even when the process is running.

### GOTCHA: squished 2-pixel titlebar for libdraw clients

For libdraw client windows (vtwin, stats, anything spawned via `window cmd`),
the initial `wmk` paint of the 22-row Luna gradient gets **only the first 2
rows committed to the framebuffer** — even though all 22 row-paints succeed
in libdraw's queue. Result: the window appears with a 2-pixel sliver of blue
at the top and the rest of the titlebar area is white.

Diagnostic logging in `wdrawtitlebar` confirms `allocimage` succeeds for all
22 rows (`ok=22 fail=0`) — they ARE queued. They just don't all commit
before vtwin/etc preempt with their own painting.

Native rio text windows (rc, the right-click `New Terminal`) are NOT
affected, because rio paints into the same image it later writes text into,
and libdraw flushes naturally between operations.

**Root cause**: `wmk` runs in the xfid thread. It queues 22 row-draws then
returns. The next event in the worker loop (window-creation completion,
focus change, the client's own getwindow) preempts before libdraw's
auto-flush threshold hits. Most of the queued draws languish.

**Two-part fix (commits fbbd93e + 9ead064)**:

1. **mxio side**: `flushimage(display, 1)` at the end of `wdrawtitlebar`'s
   gradient row-loop. Cheap, idempotent. Forces libdraw to commit before
   any caller event preempts.

   ```c
   /* wind.c::wdrawtitlebar, AFTER the gradient row-loop */
   flushimage(display, 1);
   ```

2. **vtwin side**: after `initdraw`, write `"current\n"` to `/dev/wctl`.
   This makes mxio call `wcurrent → wsendctlmesg(Topped) → winctl thread
   → wrepaint → wborder → wdrawtitlebar`. Critically, this runs in the
   **winctl thread**, which is the proper draw-thread context — unlike
   the xfid thread that runs the initial wmk.

   ```c
   /* vtwin/main.c, just after initdraw */
   int wfd = open("/dev/wctl", OWRITE);
   if(wfd >= 0){
       write(wfd, "current\n", 8);
       close(wfd);
   }
   ```

**False starts**:

- Tried `resize -r <same rect>` first. Triggers `wresize` which ALSO
  paints titlebar, but runs in the xfid thread (not winctl). Result:
  same half-painted gradient AND invalidates the screen pointer,
  causing subsequent draws to go to a stale image — vtwin's content
  area went WHITE.
- Tried pre-allocating gradient strip images and using `loadimage`
  for batched paint. Caused mxio GPF crashes (memory corruption from
  refcount issues).
- Tried `wsendctlmesg(w, Refresh, ...)` from xfid context — also
  crashes (wrong thread for libdraw).

The wcurrent path is the ONLY safe way for an external app to trigger
a clean titlebar paint from outside the WM. Verified by `titlecheck
vtwin` reading `/dev/screen` directly: 22 gradient rows visible from
(8,36,107) to (165,203,247) after the fix.

### GOTCHA: absolute path in profile

`/usr/glenda/lib/profile` invokes mxio. Stock 9front profile uses bare
`mxio` (relies on `bind -a $home/bin/$cputype /bin` at top of profile).
At init time those binds aren't always settled — leading to
`./mxio: file does not exist` and a crash-loop where init can't start
the WM.

**Fix**: use absolute path. `src/_riostart/profile` is the version we
deploy:

```rc
/amd64/bin/mxio -i riostart >[2] /tmp/mxio.log
```

Also redirects stderr so crash dumps and warnings survive (rio's first
window obscures /dev/cons).

### /dev/windows — new file for the panel

We add a new pseudo-file `/dev/windows` in xfid.c:
```
Format (read): each line = "PID LABEL X Y W H\n"
```
xena-panel reads it every 200ms and updates the taskbar buttons.

### Desktop background — data.c

`iconinit()` in `data.c` allocates a global `background` image. WinXP blue:
```c
background = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x3A6EA5FF);
```

Previously it was grey 0x777777FF.

### Focus visuals

Active window = gradient titlebar + colored buttons.
Inactive = flat faded titlebar, buttons darkened.
Focus change triggers `wdrawtitlebar()` on both windows.

## What We Do NOT Implement (out of scope for v1)

- Virtual desktops / workspaces
- Window snapping (aero snap style)
- Transparency / compositor effects
- Animations (minimize/maximize bounce)
- System tray (icons in panel) — v2
- Themes / skin engine — hardcoded Luna in v1

## Build

```
cd /sys/src/cmd/mxio
mk install    # compiles and installs to /amd64/bin/mxio
```

After editing sources: `mk clean && mk install`. Build is fast (~30-40s).

To launch instead of rio, edit `/usr/glenda/lib/profile`:
```rc
mxio -i riostart   # (was `rio -i riostart`)
```

`riostart` is an rc script that starts stats(1) and the initial terminal.

On reboot (`fshalt -r`) mxio launches automatically from profile.

## Build Pipeline (Mac → VM)

Details: [[build-toolchain]] and [[testing-harness]].

Short version:
1. Edit source on Mac in `~/Projects/plan9-winxp/src/mxio/`
2. `python3 -m http.server 8765 --bind 0.0.0.0` in that directory
3. From VM via QMP/typeln: `hget http://10.0.2.2:8765/wind.c > /sys/src/cmd/mxio/wind.c`
4. `cd /sys/src/cmd/mxio && mk install >/tmp/bld.log >[2=1]`
5. `fshalt -r` reboot to load the new binary

## See Also

- [[rio-architecture]] — the original code we fork
- [[winxp-visual-spec]] — colors, dimensions, shapes
- [[xena-panel-design]] — panel that reads /dev/windows
- [[build-toolchain]] — how to compile
- [[testing-harness]] — QMP + screendump + vision loop, hget/HTTP transfer
