# alacritty9 window protocol (gl9win2 ⇄ GL app)

Two processes, three fds. gl9win2 (native kencc/libdraw, owns the rio window)
spawns the GL app (cc9-world a.out) with:

- **fd 0 (app stdin): events, win → app.** Fixed 16-byte big-endian records.
- **fd 1 (app stdout): frames + control, app → win.** The gl9 `GL9F` stream,
  extended with a title record.
- **fd 2: stderr passthrough** to gl9win2's stderr.

Rationale: the GL9F frame stream and its fd-1 direction are inherited from gl9
(gl9egl's eglSwapBuffers already writes it); events need their own channel and
stdin is free. Source of truth: this file. Rust consts:
`vendor/winit/src/platform_impl/plan9/protocol.rs`. C: `win/gl9win2.c`.

## Event records (fd 0, win → app), 16 bytes big-endian

| off | size | field |
|-----|------|-------|
| 0 | 1 | type |
| 1 | 1 | state |
| 2 | 2 | modifiers (u16be) |
| 4 | 4 | a (u32be) |
| 8 | 4 | b (u32be) |
| 12 | 4 | reserved (0) |

Types:

| type | name | state | a | b |
|------|------|-------|---|---|
| 1 | key | 1=down, 0=up | rune (UCS-4; Plan 9 K-runes for specials, see below) | 0 |
| 2 | mouse move | 0 | x (px, window-relative) | y |
| 3 | mouse button | 1=down, 0=up | button: 1=left, 2=middle, 3=right | 0 |
| 4 | scroll | 0 | line delta as i32be (+ = up) | 0 |
| 5 | resize | 0 | width px | height px |
| 6 | focus | 1=gained, 0=lost | 0 | 0 |
| 7 | quit (window closed) | 0 | 0 | 0 |

Modifiers bitmask (tracked by gl9win2 from /dev/kbd down/up runes):
bit0 shift, bit1 ctrl, bit2 alt, bit3 super (unused on Plan 9, always 0).

Key runes: printable keys carry their rune. Specials use Plan 9
/sys/include/keyboard.h values (Kup 0xF00E, Kdown 0x80, Kleft 0xF011,
Kright 0xF012, Khome 0xF00D, Kend 0xF018, Kpgup 0xF00F, Kpgdown 0xF013,
Kins 0xF014, Kdel 0x7F, Kbs 0x08, Kesc 0x1B, KF|n 0xF001+n-1, Kshift 0xF016,
Kctl 0xF017, Kalt 0xF015). Modifier keys are sent as key events too (winit
wants them) AND tracked into the mask.

Guarantees:
- gl9win2 sends one `resize` with the initial window size **before any other
  record**, so the app never guesses the surface size.
- `quit` is the last record; gl9win2 then waits briefly for the app to exit.

## Frame/control stream (fd 1, app → win)

Records are distinguished by 4-byte magic:

- `"GL9F"` + u32be w + u32be h + w*h*4 bytes RGBA (== Plan 9 ABGR32 memory
  order, no repack). Full frame; unchanged from gl9; emitted by gl9egl
  eglSwapBuffers. Supersedes everything queued before it.
- `"GL9B"` + u32be w + u32be h + w*h*4 bytes BGRA8888 (== Plan 9 ARGB32 memory
  order). Full frame, identical framing to GL9F but the pixels are in
  Skia/Ladybird BGRA order; gl9win2 allocs the blit image as ARGB32 so libdraw
  converts to the screen with no per-frame R/B repack. Used by the ladybird9
  UI/Plan9 presenter (Skia CPU raster is always BGRA). Supersedes the queue.
- `"GL9D"` + u32be x + y + w + h + w*h*4 RGBA rows: damage delta — patch the
  given rect (top-left window coords) of the persistent image. Applied in
  arrival order; emitted by gl9egl_swap_damage (one per damage rect).
- `"GL9S"` + u32be y0 + y1 + dy: rows [y0, y1) scrolled UP by dy pixels —
  shift the persistent image and the screen (a top-down blit is overlap-safe
  for upward moves). Emitted by gl9egl_scroll before the frame's GL9D rects;
  scroll-down is never emitted (falls back to full/damage frames).
- `"GL9T"` + u32be len + len bytes UTF-8: set window title (rio /dev/label).
  Unknown magic is fatal (stream is framed, nothing to resync to). Only
  these four magics exist.

Frames may be any size; gl9win2 centers smaller frames and clips larger ones
(the app is expected to track `resize` events and re-render at window size).

## Not in the protocol (deliberate)

- Cursor icon changes, IME, clipboard (app reads/writes /dev/snarf directly —
  same namespace), window move/minimize (rio owns the window).
- Win→app flow control: none; events are tiny, the kernel pipe buffers them.
