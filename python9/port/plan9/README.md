# Plan 9 / APE build for CPython 3.11

Hand-authored build (no autoconf — `configure` is a dead end on APE, see the
wiki). Compiler is `pcc` (APE C driver → `6c`/`6l`); APE supplies the POSIX
headers CPython needs, native `6c` does not.

## Files

- `pyconfig.h` — adapted from the darwin autoconf output of 3.11.14. First cut:
  only critical-correctness macros flipped (no dlopen/dynamic-loading, no
  pthread, no kqueue/forkpty). Remaining `HAVE_*` are tuned iteratively as the
  compiler surfaces failures. Lands in the VM at the source-tree root.
- `mkfile` — skeleton build (not yet runnable; blocked on the wchar shim).
- `ape-shim/` — APE gap-fillers (wchar.h/wctype.h + impls) — **TODO**.

## Compile recipe (single file, in the VM)

```
R=/usr/glenda/Python-3.11.14
pcc -c -D_POSIX_SOURCE -DPy_BUILD_CORE \
    -I$R -I$R/Include -I$R/Include/internal \
    $R/Objects/boolobject.c -o /tmp/boolobject.o
```

## Obstacle stack (discovered by compiling, in priority order)

1. **[done] pyconfig.h flips** — dlopen/dynamic-loading/pthread/kqueue/forkpty
   undef'd. Encodes the two structural constraints: static modules only, no
   pthreads.
2. **[done] `-D_POSIX_SOURCE`** — APE headers (`unistd.h` etc.) hard-`#error`
   outside POSIX mode. Must be defined for every compile. Some files will also
   need `_BSD_EXTENSION` (sockets) / `_C99_SNPRINTF` / `_LARGEFILE_SOURCE`.
3. **[BLOCKER] no `<wchar.h>` / `<wctype.h>` in APE** — APE's libc predates C95
   wide-char support; only `stddef.h` is present. CPython 3 depends on wide-char
   functions (`wcslen`, `wcscpy`, `wcscmp`, `wcschr`, `mbstowcs`, `wcstombs`,
   `wcscoll`, `wcsxfrm`, `wmemcmp`, …). Next step: write `ape-shim/wchar.h` +
   `wctype.h` with the needed prototypes and a `wchar_shim.c` implementing the
   subset CPython actually calls (grep `Include/`/`Objects/`/`Python/` for `wcs`
   and `wmem`). jas's 2.7 port faced the same gap.
4. **[later] Plan 9 thread backend** — `Python/thread_plan9.h` (rfork(RFMEM) /
   proccreate + locks). Not on the critical path for non-thread object files,
   but required before the interpreter links.
5. **[later] static `Modules/Setup`** — enumerate built-in modules; drop the
   ones that need dlopen / OS primitives Plan 9 lacks.

## Status

Compiling `boolobject.c` (a tiny core file) currently fails at obstacle 3.
Clearing the wchar shim should get the first `.o` produced, after which the
mkfile work and the long per-file grind begin.
