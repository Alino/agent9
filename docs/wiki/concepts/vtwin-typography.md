---
title: vtwin Typography
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [plan9, libdraw, draw, status-wip]
---

# vtwin Typography

How vtwin picks its font, why "libdraw can't do nice fonts" is a myth, and
what we actually need to do to make our terminal look like a 2026 product
instead of a 1989 X11 xterm.

## TL;DR

libdraw renders any TTF/OTF you point it at, antialiased, via 9front's
**fontsrv**. The "Plan 9 fonts are ugly" perception is entirely about the
*default* font — `/lib/font/bit/vga/unicode.font`, an 8×16 bitmap subfont
from the 1980s. Swap it for fontsrv-served JetBrains Mono or Inconsolata
and vtwin is visually competitive with iTerm2, alacritty, or wezterm.

The decision belongs to vtwin (the renderer), not to vts (the daemon),
not to pi9 (a tenant), and not to libdraw (which is just a graphics API).

## How vtwin picks a font (post 2026-05-17)

Canonical Plan 9 pattern — same as rio(1), 9term(1), acme(1):

```
1. $font env var    — primary mechanism, set per-user in /usr/glenda/lib/profile
2. defaultfont      — libdraw's fallback when $font is unset or unloadable
```

A future `-f <path>` flag will pre-empt both. The code lives in
`src/vtwin/main.c` immediately after `initkeyboard`, ~line 539:

```c
char *fn = getenv("font");
if(fn != nil && *fn != '\0'){
    cellfont = openfont(display, fn);
    if(cellfont == nil)
        fprint(2, "vtwin: openfont %s: %r (falling back to defaultfont)\n", fn);
    free(fn);
}
if(cellfont == nil)
    cellfont = display->defaultfont;
```

`openfont` accepts both bitmap-subfont paths (`/lib/font/bit/.../*.font`) and
fontsrv-synthesised TTF paths (`/n/font/<Family>/<size>a/font`) transparently.
The `a` suffix on the size means "antialiased" — fontsrv synthesises a
subfont at that pixel size from the TTF.

## fontsrv — Plan 9's TTF rasteriser

`/bin/fontsrv` ships with 9front. It mounts a synthetic 9P tree at
`/n/font` (after `9fs fontsrv` or manual `mount`), exposing every TTF/OTF
on the system as a directory tree of subfonts.

```
% 9fs fontsrv
% lc /n/font
DejaVuSans/   DejaVuSansMono/   Inconsolata/   Lucida/   ...
% lc /n/font/Inconsolata
16/    16a/    18/    18a/    20/    20a/    ...
% openfont(disp, "/n/font/Inconsolata/16a/font")  → antialiased 16px Inconsolata
```

To make Inconsolata available system-wide, add to `/usr/glenda/lib/profile`
(or `src/_riostart/profile` for us):

```
9fs fontsrv
font=/n/font/Inconsolata/16a/font
```

Currently `src/_riostart/profile` line 5 sets
`font=/lib/font/bit/vga/unicode.font` — the bitmap VGA subfont. Changing
that single line propagates to vtwin (after our patch lands) and to any
other libdraw program in the namespace that honours `$font`.

## Font picks worth trying

Picked from [[../../../plan9-agent/wiki/font-references.md|nothing yet]]
plus what Hermes-web's "Mono" theme ships (Inter + JetBrains Mono). For
terminal use only the mono pick matters.

| Family            | License      | Why it's good for vtwin                      |
|-------------------|--------------|-----------------------------------------------|
| **JetBrains Mono**| OFL          | Hermes-web default. Generous x-height. Ligatures (ignorable). |
| **Inconsolata**   | OFL          | Plan 9 community favourite. Tight, no ligatures, calm. |
| **Iosevka**       | OFL          | Narrow — fits more cols. ~Fira-Code shaped.   |
| **IBM Plex Mono** | OFL          | Hermes-web "Plex" theme. Slightly serif-y.    |
| **Source Code Pro**| OFL         | Adobe. Plain, neutral. Good fallback default. |
| **Berkeley Mono** | commercial   | If we ever wanted to splash. Beautiful.       |

Default recommendation for first cut: **Inconsolata 16a** — small footprint
(under 200 KB TTF), historically good on Plan 9, no shaping requirements,
renders cleanly at 14-18 px on every monitor density we care about.

To install on 9front: copy the .ttf into `/lib/font/ttf/`, fontsrv discovers
it automatically on next mount.

## Why this is not a libdraw limitation

Two real differences from modern stacks, neither of which matters for
monospace terminal text:

- **No subpixel positioning.** Each glyph snaps to integer x. Invisible
  for monospace; would matter for proportional UI text.
- **No HarfBuzz shaping.** No Arabic, no Devanagari, no emoji ZWJ
  sequences. Latin + Cyrillic + Greek + Han via separate subfont
  ranges = full coverage for everything we render. Box-drawing
  characters work because the cell grid is the renderer.

Things libdraw + fontsrv DO get right:
- Antialiased grayscale outlines from TTF at runtime
- Hinted glyphs (via FreeType, which fontsrv links against on 9front)
- Per-codepoint subfont selection — fall back from a Latin font to a
  CJK font on the same line transparently

## Action items

| # | Action | Owner | Status |
|---|--------|-------|--------|
| 1 | vtwin honours `$font` | vtwin | done 2026-05-17 |
| 2 | Ship Inconsolata-Regular.ttf in `src/_riostart/fonts/` (copied into `/lib/font/ttf/` on the VM via `install-fonts.rc`) | build | done 2026-05-17 |
| 3 | `src/_riostart/profile` runs `9fs fontsrv` at terminal boot | build | done 2026-05-17 |
| 4 | Default `$font` → `/n/font/Inconsolata/16a/font` for terminal sessions | build | done 2026-05-17 |
| 5 | vtwin `-f <path>` CLI flag (overrides env) | vtwin | future |
| 6 | Cell-width caching across font changes | vtwin | future |

Items 1–4 done. Next VM boot picks up the new font automatically, provided
`/lib/font/ttf/Inconsolata-Regular.ttf` has been installed (run
`install-fonts.rc` once on a fresh VM, or copy the TTF into the disk image
before booting).

## Pitfalls

### `openfont` with retina-class sizes blows out cell width

`stringwidth(font, "M")` returns the M-glyph advance, which for a TTF at
24a is ~14 px — fine. But the floor in our resize code assumed bitmap-era
8×14 cells. If you push the font past ~24px the math still works (cells
just get bigger, grid gets smaller), but cell counts under ~40×12 lose
the WinXP feel. Default 16a → 80×30 in a 760×540 mxio window, which is
the sweet spot.

### fontsrv not mounted

If `$font` points at `/n/font/...` but fontsrv hasn't been mounted yet
(no `9fs fontsrv`), `openfont` returns nil and we fall back to defaultfont.
The console warning lands on stderr, which is `/tmp/vtwin.log` from
launch-vtwin.rc. Watch for it on startup if a swap doesn't visibly land.

### Subfont vs. font files

Plan 9 distinguishes:
- **Subfont** — one bitmap range, single file (`.subfont` or via fontsrv synthesis)
- **Font** — text manifest listing height + per-range subfont paths (`.font`)

You always pass `openfont` the `.font` file. fontsrv synthesises both in
its tree (`Inconsolata/16a/font` is the manifest, `Inconsolata/16a/x0000`
etc. are the subfonts).

## See Also

- [[vtwin-client]] — where the font is actually used to render cells
- [[draw-api]] — `openfont`, `stringwidth`, `string` in libdraw
- [[vt-architecture]] — why the renderer (vtwin) owns the font, not the daemon (vts)
- [[pi9-architecture]] — pi9 inherits whatever vtwin renders
- [9front fontsrv(8)](http://9front.org/) — TTF/OTF 9P file server
- [Inconsolata](https://fonts.google.com/specimen/Inconsolata) — OFL
- [JetBrains Mono](https://www.jetbrains.com/lp/mono/) — OFL
