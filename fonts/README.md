# pi9 / vtwin fonts

## TL;DR

Pi9 defaults to **Terminus 16** (a hand-drawn bitmap terminal font that
ships with 9front). Nothing to do — `pi9-install` sets the default,
`new-pi9` falls back to it even without explicit config.

To switch:
```rc
echo /lib/font/bit/terminus/unicode.14.font > $home/lib/pi9/font   # smaller
echo /lib/font/bit/terminus/unicode.18.font > $home/lib/pi9/font   # larger
window new-pi9 &
```

## Recommended fonts (hand-drawn, already on plan9)

| Path | Cell (WxH) | Notes |
|---|---|---|
| `/lib/font/bit/terminus/unicode.14.font` | 8x14 | tightest |
| `/lib/font/bit/terminus/unicode.16.font` | 8x16 | **default**, matches taskbar |
| `/lib/font/bit/terminus/unicode.18.font` | 10x18 | bigger, easier on eyes |
| `/lib/font/bit/fixed/unicode.9x15.font` | 9x15 | X11 classic |
| `/lib/font/bit/fixed/unicode.10x20.font` | 10x20 | bigger version |
| `/lib/font/bit/vga/unicode.font` | 8x16 | plan9 default (boring but works) |

All hand-drawn pixel-perfect at each size. Just work.

## Why not JetBrains Mono / Source Code Pro / iTerm2-style fonts?

We tried. Don't. Here's why:

**TTF fonts like JetBrains Mono are vector designs hinted for high-DPI
rendering with antialiasing.** When you slam them to 1-bit bitmaps at
small pixel sizes (12-16px), the result has bouncy baselines, distorted
letter shapes, and inconsistent stroke weights. The auto-rasterized
output is "JBM-shaped pixels" but the design doesn't survive.

Hand-drawn bitmap fonts (Terminus, vga.font, X11 fixed) don't have
this problem — they're literally pixel-by-pixel drawings by humans
who tuned every glyph for that exact pixel grid.

If you really want a TTF on plan9, `src/pi9/testtools/ttf2p9.py`
will convert one. It works mechanically. The output won't look as
clean as Terminus at the same size. Use ≥18px to get tolerable
results (the glyphs have more room to breathe). See the script for
details.

## How vtwin loads fonts

1. `vtwin -f <path>` if `-f` was passed (highest precedence)
2. `$home/lib/pi9/font` config file (what `new-pi9` reads and passes
   via `-f`)
3. `$pi9font` env var (rarely propagates — see wiki/concepts/pi9-phase11.md)
4. `$font` env var (same)
5. `/lib/font/bit/terminus/unicode.16.font` (new-pi9's hardcoded fallback)
6. vtwin's libdraw default (vga.font typically)

`pi9-install` writes step 2 to Terminus 16 on first run if no config
exists.

## Verifying the active font

```rc
cat /tmp/new-pi9.log
```

Shows every recent pi9 launch with the resolved font path and the
`-f` argument vtwin received.
