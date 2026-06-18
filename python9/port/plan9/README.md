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

## Source patches

kencc (6c) lacks several constructs CPython uses. Patches live in `patches/`
(`plan9-cpython.patch`, applied with `patches/apply.sh`). They cover:

- **compound literals** `(T){...}` (kencc unsupported) → static-inline helpers:
  `_PyStatus_*`, `_PyWideStringList_INIT`, `_PyCompilerFlags_INIT`, plus a raw
  one in `initconfig.c`.
- **GNU `, ## __VA_ARGS__`** comma-elision (kencc cpp unsupported) → fold the
  fixed arg into `__VA_ARGS__` (pure C99) in `Parser/pegen.h`.
- **`Py_UNREACHABLE`** → `do { Py_FatalError(...); } while (1)` so kencc's
  "no return at end of function" analysis sees it as non-returning (the GCC
  `__builtin_unreachable` path is unavailable and we neutralize `__attribute__`,
  so `Py_FatalError`'s `noreturn` is invisible to kencc).

pyconfig.h additionally: 4-byte `wchar_t` predefine (resolve APE 2-byte vs
kencc 4-byte `L""` conflict), undef `HAVE_COMPUTED_GOTOS` (no `&&label`), undef
`HAVE_STD_ATOMIC`/`HAVE_BUILTIN_ATOMIC`, `NAN`/`UINT*_C` shims, etc.

## Status — core compile progress

Batch-compiling the 112-file core set (`Objects/`+`Python/`+`Parser/`, the
object set the host build produced) via `pybatch.rc`:

| pass | PASS | FAIL | what changed |
|---|---|---|---|
| 1 | 73 | 39 | first run (config + wchar shim only) |
| 2 | 84 | 28 | + PyStatus helpers, pegen macros, small undefs |
| 3 | 97 | 15 | + 4-byte wchar_t, computed-gotos off, more compound literals |
| 4 | 104 | 8 | + no-return macros (`while(1)`), undef mmap, static_assert |
| 5 | 107 | 5 | + missing errnos, langinfo shim, last compound literal |
| 6 | 108 | 4 | + undef realpath |
| 7 | 110 | 2 | + stdin/stdout/stderr kencc-cpp `##` workaround (pystate, pylifecycle) |
| 8 | **112** | **0** | + parser.c array compound literals hoisted, floatobject `typestr` rename + C99 math shims (copysign/round/isfinite) |

**All 112 core files compile.** (`Objects/`+`Python/`+`Parser/`, the object
set the host build produced.) Last two cracked in pass 8:
- `Parser/parser.c`: hoisted the generated `(KeywordToken[]){...}` array
  compound literals to named statics (`_kw0`.. `_kw8`).
- `floatobject.c`: kencc has a parser quirk rejecting the bare identifier
  `typestr` in a declarator (param or local) -- renamed to `typestr_`; plus C99
  math shims (`copysign`/`round` in plan9_compat.c, `isfinite` macro; APE has
  `isnan`/`isinf`).

Then: enumerate objects in the mkfile, build `Modules/` + static `Setup`, link a
`python`, boot the REPL, and run `parity/run_suite.py` for the first score.


## Link + boot status

**The interpreter links and boots.** `pylink.rc` compiles all 147 sources to
`/tmp/obj` and links a 15.4 MB `python` binary via `pcc` (auto-links libap).
Running it reaches `_PySys_InitCore` (sys module init) during `Py_Initialize`:

```
Fatal Python error: _PySys_InitCore: can't initialize sys module
KeyError: 'exce\x00\x00\x00\x00ok'   # "excepthook" with bytes 4-7 zeroed
```

Link fixes beyond compilation:
- thread.c: pthread-stubs path (undef THREAD_STACK_SIZE +
  PTHREAD_SYSTEM_SCHED_SUPPORTED).
- pytime.c: disable INT64 two's-complement #if (kencc cpp can't evaluate it);
  define CLOCK_* constants; clock_getres in plan9_compat.c.
- plan9_compat.c shims: setenv/unsetenv, getentropy (/dev/random),
  clock_gettime/getres (gettimeofday), copysign/round. APE provides
  localtime_r/gmtime_r (undeclared) -- declared in pyconfig, not redefined.
- dictobject.c empty_keys_struct: flexible-array static init -> fixed struct.
- getpath.c: define PREFIX/EXEC_PREFIX/VERSION/VPATH/PLATLIBDIR in pyconfig.
- config.c (Plan 9 builtin table), faulthandler_stub.c.

### Next: the _PySys_InitCore crash
A global interned string ("excepthook") reads back with a zeroed 4-byte word at
offset 4 (`exce\0\0\0\0ok`). kencc initializes char arrays from string
literals correctly (verified standalone and with a 48-byte struct prefix), so
the corruption is at runtime, not static-init. Needs in-VM debugging (acid) to
find the bad 4-byte store. Candidates: unicode hash caching, interning, or a
kencc codegen issue in a 64-bit path (pyhash/siphash).
