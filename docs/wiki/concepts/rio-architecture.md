---
title: Rio Architecture
created: 2026-05-15
updated: 2026-05-16
type: concept
tags: [plan9, rio, wm, draw]
---

# Rio Architecture

Rio is the WM / window multiplexer in Plan 9. Source: `/sys/src/cmd/rio/` (~8000 LOC C).

## Key Files

```
rio.c      -- main, event loop, Display init, mouse/kbd thread
wind.c     -- Window struct, create/destroy/resize, draw window content
xfid.c     -- 9P file server (each window exposes /dev/cons, /dev/consctl, ...)
rioaux.c   -- helper functions (geometry, label, ...)
dat.h      -- data structures: Window, Dirtab, Client
fns.h      -- declarations of all functions
```

## Window Struct (dat.h)

```c
struct Window {
    QLock       ql;
    Ref         ref;
    Client      *client;
    Image       *i;           // Image for window content (interior)
    Rectangle   screenr;      // position on screen (outer rectangle)
    Rectangle   inner;        // interior (screenr minus border)
    // ... mouse state, keyboard buf, scroll, label, ...
    char        name[256];    // window title/label
};
```

## Event Loop (rio.c)

```
main()
  ├─ initdraw()              -- opens /dev/draw, initializes Display
  ├─ initkeyboard()          -- /dev/kbd thread
  ├─ initmouse()             -- /dev/mouse thread
  ├─ thread: mousectl()      -- receives mouse events, dispatches
  └─ thread: keyboardctl()   -- receives key events, dispatches
```

Mouse event flow:
```
/dev/mouse → mousectl thread → button hit test →
  if border    = resize
  if title area (does not exist in stock Rio) = drag
  if content   = forward to client window
```

## What Rio Does NOT Have (what mxio adds)

1. Titlebars — Rio has only a 4px border, no titlebar
2. Decoration buttons — no close/min/max
3. Drag-to-move via titlebar — in Rio windows move via button2 anywhere
4. Z-order on click — Rio has primitive stacking
5. /dev/windows list — no global window list for a panel

## Border and Resize

Stock Rio draws the border via `drawborder()` in `wind.c`. It is simple:
```c
border(screen, r, Borderwidth, display->black, ZP);
```
`Borderwidth = 4`. Our mxio changes this to:
```c
#define TITLEBAR_H  22
#define BORDER_W    3
// and draws a gradient titlebar instead of a plain border
```

## Plumber Integration

Rio listens on `/srv/rio` — clients register via the plumber. We preserve this.

## See Also

- [[draw-api]] — graphics primitives that Rio uses
- [[mxio-design]] — our fork and what we change
- [[xena-panel-design]] — panel needs /dev/windows which Rio lacks
