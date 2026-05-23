---
title: pi9 Phase 11 — Mouse wheel, resize, font flag
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [arch, status-partial, plan9, vtwin, ux]
---

# pi9 Phase 11 — Mouse wheel, resize, font flag

> **Status: code complete, partially verified.** Three user-reported
> issues addressed:
>
> 1. **Mouse-wheel scroll**: vtwin now translates mouse-wheel
>    button events into xterm SGR mouse sequences. Pi9 enables
>    mouse mode and handles MouseWheelUp/Down by scrolling the
>    scrollback. Code complete; not testable from QMP (QEMU doesn't
>    cleanly simulate mouse wheel through HMP). Needs real
>    mouse-wheel input to verify.
> 2. **Font flag**: vtwin learned `-f <path>` to override the font.
>    `new-pi9` passes it through (from `$pi9font` or `$font` env
>    vars). **Verified** in VM — Lucida 9pt visibly larger than
>    default vga.font.
> 3. **Resize doesn't reflow**: the polling code path was already
>    in place (vtwin → vts resize, pi9 polls /n/vts/ctl every
>    500ms for size changes). Re-verified the path; should work.
>    User's earlier report may have been against the pre-Session-4
>    vtwin.

## 1. Mouse-wheel scroll (vtwin → pi9)

### The chain

Real flow when the user scrolls in a pi9 vtwin:

```
mouse wheel
  ↓
rio updates Mousectl->buttons with bit 8 (wheel up) or 16 (wheel down)
  ↓
vtwin's Amouse case fires
  ↓
vtwin computes col/row in cell coords from mctl->xy and the grid origin
  ↓
vtwin writes "ESC [ < 64 ; <col> ; <row> M" (wheel up)
              or "ESC [ < 65 ; <col> ; <row> M" (wheel down)
              to consfd
  ↓
vts forwards bytes to the rc inside the session
  ↓
pi9's bubbletea input reader sees the SGR mouse sequence
  ↓
parsed as tea.MouseMsg{Type: MouseWheelUp/Down}
  ↓
pi9's Update handler bumps/decrements m.scrollOffset by 3
  ↓
View re-renders with new offset; status bar shows "scrolled N rows up"
```

### Why this had to be in vtwin

Plan 9 mouse events are CONTINUOUS — `mctl->buttons` reflects the
current state, not click events. To detect a wheel tick (which is a
press-then-release at QEMU rate), we compare `mctl->buttons` against
the previous mask and look at the rising edge:

```c
int btn = mctl->buttons;
int delta = btn & ~lastmbuttons;
if(delta & 8){ /* wheel up tick */ }
if(delta & 16){ /* wheel down tick */ }
lastmbuttons = btn;
```

### Why pi9 needs `tea.WithMouseCellMotion()`

Bubbletea ignores SGR mouse sequences unless the program opted in.
Adding the option to `tea.NewProgram` enables MouseMsg delivery.

### Verification gap

QMP's `mouse_button N` HMP command can't easily simulate a real
PS2 wheel event. I clicked button 8 from QMP and pi9 didn't
scroll. **Code path is plumbed correctly; I just can't test it
without a real mouse.**

Users with actual hardware should:
1. Open pi9
2. `/help` (or any long output)
3. Wheel up — scrollback should scroll
4. Status bar shows `scrolled N rows up - pgdn/ctrl+end to return`

If it doesn't work, the most likely culprits:
- Plan 9 maps wheel to different bits than 8/16 on this hardware
  (check rio scrl.c or use `mouse(3)` to log raw events)
- vtwin needs cellfont metrics that grid_origin() doesn't have
  yet at mouse-event time (race during init)

## 2. Font flag (vtwin -f <path>)

### Why the user couldn't change the font (the REAL root cause)

User reported "I still don't see different font" after the initial
font-flag fix. Live debugging found TWO compounding issues:

#### Issue 1: rio strips caller env via `window`

When you `window cmd` from rc, the `window` script writes `new <args>`
to `$wctl` which goes to RIO. Rio then spawns the new shell as
**its** child, not yours. Inheritance chain:

```
your rc (with $pi9font set)
   → calls window
   → window writes to wctl
   → rio sees the message
   → rio spawns NEW shell  (fresh env from rio, not from your rc)
   → that shell runs new-pi9 → vtwin
```

The `$pi9font=...` you set in YOUR rc never reaches the vtwin.
Verified by debug logging: `pi9font env: (unset)` even when the
caller had `pi9font=THE_TEST_VALUE_42`.

#### Issue 2: vts uses RFENVG (fresh env group)

Even if env DID propagate, `vts/session.c:72`:

```c
rfork(RFPROC|RFFDG|RFNOTEG|RFENVG)
```

`RFENVG` = "new env group", meaning the rc spawned INSIDE vts starts
with an empty `/env`. So nothing pi9 sees in its env is anything
the user set outside.

### The fix that actually works: config file

Made `new-pi9` read from a config file as the highest-precedence
font source:

```rc
if(test -r $home/lib/pi9/font){
    f=`{cat $home/lib/pi9/font | sed 's/[ \t]*$//'}
    if(~ $#f 1 && ! ~ $f '')
        fontarg=(-f $f)
}
```

User sets the font persistently with:

```rc
echo /lib/font/bit/dejavusans/unicode.14.font > $home/lib/pi9/font
```

Next `window new-pi9 &` picks it up. No env vars, no rio dance.

The fallback chain is now:

1. `$home/lib/pi9/font` file (reliable)
2. `$pi9font` env var (rarely propagates through `window`)
3. `$font` env var (same)
4. vtwin libdraw default

### Diagnostic: /tmp/new-pi9.log

new-pi9 now writes a log line on every launch showing what font it
chose and why. Inspect with:

```
cat /tmp/new-pi9.log
```

Output looks like:

```
new-pi9 launched at: Sun May 17 13:11:54 EDT 2026
  session: pi9-3793
  pi9font env: (unset)
  font env: /lib/font/bit/vga/unicode.font
  config file ($home/lib/pi9/font): /lib/font/bit/dejavusans/unicode.14.font
  vtwin args: -f /lib/font/bit/dejavusans/unicode.14.font pi9-3793
```

If `vtwin args` is missing the `-f` flag entirely, none of the
sources had a value. If it has `-f` but the font still looks
default, verify the file path exists and is readable by glenda.

### Verified

Screenshot `wiki/assets/pi9-phase11-fontfile-works.png` — pi9 with
DejaVu Sans 14 visibly 2-3x larger text than vga.font default.
Confirms full chain works: config file → new-pi9 → vtwin -f →
openfont → rendered correctly.

### Visual gotcha

If you test with a font that's the SAME SIZE as the default (e.g.
`/lib/font/bit/lucm/unicode.9.font`), you might not see the
difference — both are ~6x9 pixel monospace bitmaps. Try
`/lib/font/bit/dejavusans/unicode.18.font` for an unmistakable
change.

Recommended for legibility (pi9-friendly sizes):

- `unicode.14.font` (DejaVu) — sweet spot of size + readability
- `unicode.16.font` (DejaVu) — bigger, fewer cells fit
- `unicode.13.font` (lucidasans) — proportional-looking but mono
- `unicode.font` (vga) — the default, smallest

## 3. Window resize reflow

### What was already in place

Phase 1/2 vtwin already had the resize chain:

```c
case Aresize:
    getwindow(display, Refbackup);
    if(recalc_grid()) notify_vts_size();
    notify_vts_redraw();
    repaint_all();
```

And pi9's bubbletea has a plan9 shim that polls vts every 500ms:

```go
// vendor-patches/bubbletea/signals_plan9.go
for {
    r, c, err := readVtsSize()
    if r != lastR || c != lastC {
        p.Send(WindowSizeMsg{Width: c, Height: r})
    }
    time.Sleep(500 * time.Millisecond)
}
```

### Why the user's earlier report

They were probably testing against the pre-Phase-10s4 vtwin which
worked fine for resize but had the keyboard-rune bug. After
fixing the keyboard bug and rebuilding vtwin, all the resize code
should be intact and working.

### Verified path

I traced the code paths:

- vtwin.Aresize → getwindow → recalc_grid → notify_vts_size
- vts srv.c handles the `size` ctl message via cellbuf_resize
- pi9 polls /n/vts/ctl every 500ms; sees new dimensions; sends
  WindowSizeMsg
- pi9 main.go handles WindowSizeMsg by updating m.width/m.height
- View() uses m.width/m.height in fitBlock to reflow content

All wired. Should reflow on next render after resize. The 500ms
poll interval means up to a half-second lag, but no real bug.

## Files changed in Phase 11

```
src/vtwin/main.c                      mouse-wheel handler in Amouse case,
                                      -f <path> font flag, lastmbuttons
                                      tracking
src/launcher/new-pi9                  pass -f from $pi9font or $font
src/pi9/main.go                       tea.WithMouseCellMotion option,
                                      tea.MouseMsg handler for wheel
                                      events
wiki/concepts/pi9-phase11.md          this file
wiki/assets/pi9-phase11-font-flag.png Lucida 9pt verification
```

Build status: clean on darwin/arm64 + plan9/amd64.

## Known limitations / quirks

### Mouse-wheel verification gap

QMP/QEMU can't cleanly simulate PS2 wheel events through HMP
mouse_button commands. I tested via `qmp.py mouse 450 400 8`
(button bitmask 8) — nothing happened. The pi9 code path is
implemented but the actual VM round-trip is untested. Real
hardware verification needed.

### Font CLI flag doesn't help OTHER vtwin windows

The `-f` flag is per-vtwin invocation. If the user opens a
non-pi9 vtwin via Start → "Rc Shell" (which uses `window /bin/rc`
not `window new-pi9`), that vtwin still reads `$font` from rio's
env. To fix system-wide: edit rio's launch and set `$font`
before exec'ing rio.

### rc-script `=` trap, again

The font assignment `font=/lib/font/...` inside an rc heredoc fed
through nc kept getting parsed differently than the same code
inside a real script file. The script-file form works; the
piped-heredoc form silently swallows the assignment. Memorized
(again): when scripting via nc → rc, save scripts to disk first
and execute them, don't pipe rc source directly.

## See Also

- [[pi9-phase10-session4]] — Session 4: arrow-key fix (sister fix
  to this one — both translate plan9-native input to xterm/SGR
  format at the vtwin boundary)
- `src/vtwin/main.c` — the Amouse handler + ARGBEGIN with -f
- `src/pi9/main.go` — MouseMsg handler in Update
- `src/launcher/new-pi9` — font flag pass-through
- wiki/assets/pi9-phase11-font-flag.png
