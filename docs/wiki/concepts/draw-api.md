---
title: Plan 9 Draw API
created: 2026-05-15
updated: 2026-05-16
type: concept
tags: [plan9, draw, libdraw]
---

# Plan 9 Draw API

## Overview

Plan 9 graphics are file-based. Every program that wants to draw opens `/dev/draw`
and communicates with the kernel draw server via read/write messages.

## /dev/draw files

```
/dev/draw/new        -- open = allocate new display ID, returns "ID depth width height"
/dev/draw/N/data     -- write = graphics commands (draw operations)
/dev/draw/N/refresh  -- read = blocks until display server signals a refresh
/dev/draw/N/ctl      -- write = control commands (flush, resize, ...)
```

## libdraw

Library that wraps raw `/dev/draw` into a C API. Headers in `/sys/include/draw.h`.

Key types:
```c
Display*  — connection to the draw server (opens /dev/draw/new)
Image*    — a rectangle on a Display, can be on-screen or off-screen
Font*     — bitmap font (Plan 9 has no native TTF; freetype is an addon)
Point     — {int x, y}
Rectangle — {Point min, max}  (min inclusive, max exclusive)
```

Key functions:
```c
Display* initdraw(void(*errfn)(Display*, char*), char* font, char* label);
Image*   allocimage(Display*, Rectangle, ulong chan, int repl, ulong col);
void     draw(Image *dst, Rectangle r, Image *src, Image *mask, Point p);
void     string(Image*, Point, Image *src, Point, Font*, char*);
void     flushimage(Display*, int vis);  // vis=1 = push to screen
void     freeimage(Image*);
```

## libmemdraw — in-memory drawing

`libmemdraw` is a version of libdraw that works without /dev/draw — you draw into
memory (Memimage) rather than directly to a display. Key for the browser renderer
in [[browser-webview-plan9]]:

```c
#include <draw.h>
#include <memdraw.h>

Memimage *img = allocmemimage(r, RGB24);
memdraw(img, r, src, mask, p);
// ... render HTML layout here ...
// then push to on-screen Image in a single draw() call
```

Advantage: the entire rendering pipeline can run without a display connection;
the result is pushed to /dev/draw in one shot — no screen tearing.

Plan9port source: `src/libmemdraw/` — MIT, read freely.
Online man: https://9fans.github.io/plan9port/man/man3/memdraw.html

## devdraw

On macOS / Linux: `devdraw` from plan9port emulates `/dev/draw` via X11 or Quartz.
In the 9front VM: the kernel draw server is native.

## Colors

Plan 9 colors are 32-bit: `0xRRGGBBFF` (alpha is the last byte, FF = opaque).

```c
ulong WINXP_ACTIVE_TITLE = 0x003399FF;   // luna blue
ulong WINXP_INACTIVE     = 0x808080FF;
ulong WINXP_PANEL_BG     = 0xECE9D8FF;
ulong WINXP_TEXT         = 0x000000FF;
ulong WINXP_BTN_FACE     = 0xD4D0C8FF;
```

## See Also

- [[rio-architecture]] — how Rio uses libdraw
- [[mxio-design]] — how mxio extends draw primitives with decorations
- [[winxp-visual-spec]] — exact colors and dimensions
- [[9fans-ecosystem]] — plan9port sources, libmemdraw, drawfcall wire protocol
