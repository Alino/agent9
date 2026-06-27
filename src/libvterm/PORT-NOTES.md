# libvterm 0.3.3 — Plan 9 (9front / kencc) port notes

Upstream: libvterm 0.3.3 (Paul "LeoNerd" Evans),
http://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz

This is a vendored, lightly-patched copy that builds with the native 9front
toolchain (`6c`/`mksyslib`) into `/$objtype/lib/libvterm.a`, linked by `vts`.

## What was kept

Engine only: `vterm.c state.c screen.c parser.c pen.c encoding.c unicode.c`
plus headers `vterm.h vterm_keycodes.h vterm_internal.h utf8.h rect.h` and the
table includes `DECdrawing.inc uk.inc fullwidth.inc`.

## What was dropped

- `keyboard.c`, `mouse.c` — these encode key/mouse *events* into byte
  sequences. vts forwards keystrokes itself; not needed for the parse/render
  path. Re-add later if we want libvterm to own input encoding.

## What 9front's kencc accepts (verified on-box, so NOT patched)

Probed directly with `6c` on the target — these compile unchanged:
- designated struct initializers (`{ .decode = &fn }`)
- designated array initializers (`[0x60] = 0x25C6`), incl. the `.inc` tables
- nested designated aggregate initializers (the `StaticTableEncoding` pattern)
- `static inline`
- C99 for-loop declarations (`for(int i = 0; ...)`)

So `encoding.c`, the `.inc` tables, `unicode.c`, `pen.c`, `parser.c` needed
**no language edits**.

## Patches applied (only what kencc rejects)

1. **Compound literals** — the one C99 feature `6c` rejects (`(Type){...}`).
   Rewritten as a zeroed local + field assignments (or `memset`), tagged with
   a `/* ported: no compound literals */` comment. Sites:
   - `vterm.c`: `vterm_new`, `vterm_new_with_allocator` (VTermBuilder)
   - `screen.c`: ScreenPen reset; two `(VTermLineInfo){0}` in resize
   - `state.c`: two `(VTermLineInfo){0}` in scroll; one in resize; three
     `(VTermStringFragment){...}` passed as call args (OSC-52 selection path)

2. **Hosted headers** — Plan 9 native libc has no `<stdint.h> <stdbool.h>
   <stdlib.h> <stdio.h> <stdarg.h> <string.h>`. Added `vtcompat.h` (fixed-width
   types, `size_t`, `bool`, `snprintf`→`snprint`, `vsnprintf`→`vsnprint` over
   `<u.h>/<libc.h>`). All sources drop their `<std*.h>` includes; `vterm.h`
   includes `vtcompat.h` instead, so the shim reaches everything transitively.
   `vterm_internal.h` drops `<stdarg.h>` (`va_list` comes from `libc.h`).

3. **`.inc` include paths** — flattened `encoding/DECdrawing.inc` →
   `DECdrawing.inc` etc. since the whole port lives in one flat directory
   (p9sync only mirrors maxdepth-1 files).

That's the complete patch set. To bump upstream: re-vendor, re-apply (1)-(3).
Everything else compiles as-is.
