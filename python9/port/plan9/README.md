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

## Build flags (settled)

`pybuild.rc` compiles one file with the proven flag set:

```
pcc -c -D_POSIX_SOURCE -D_BSD_EXTENSION '-Dclockid_t=int' -DPy_BUILD_CORE \
    -I<shim> -I<root> -I<root>/Include -I<root>/Include/internal <src> -o <obj>
```

(Invoke it via a short `rc /usr/glenda/pybuild.rc <relpath>` — listen1 drops
long command lines on the slow VM.)

## Obstacle stack (resolved to get the first `.o`)

1. [done] pyconfig flips — dlopen/dynamic-loading/pthread/kqueue/forkpty undef.
2. [done] `-D_POSIX_SOURCE` — APE headers `#error` outside POSIX mode.
3. [done] wchar shim — `ape-shim/wchar.h` + `wchar_shim.c`; `SIZEOF_WCHAR_T 2`
   (APE wchar_t is `unsigned short`); multibyte `HAVE_*` undef'd → CPython's
   internal UTF-8 fallbacks.
4. [done] `-D_BSD_EXTENSION` — needed for `sys/select.h`.
5. [done] `HAVE_PTHREAD_STUBS` — CPython's single-threaded pthread stub; gets a
   booting interpreter now, real Plan 9 thread backend later.
6. [done] `-Dclockid_t=int` — APE lacks `clockid_t` (used in a stub decl).
7. [done] neutralize `__attribute__` — `#ifndef __GNUC__ #define __attribute__(x)`
   in pyconfig.h (6c has no GCC attrs; Plan 9 cpp rejects function-like `-D`).
8. [done] undef `HAVE_STD_ATOMIC` + `HAVE_BUILTIN_ATOMIC` — no C11 `<stdatomic.h>`
   / `__atomic` builtins; use CPython's volatile fallback (ok single-threaded).

## Status

**First core object compiled:** `Objects/boolobject.c` → 138 KB `.o`, rc=0.
The config + shim foundation is proven. Next: compile the broader core file set
(each surfaces its own APE gaps), then enumerate objects in the mkfile, link a
`python`, and boot the REPL. Real thread backend + module trimming follow.
