# node9 Phase 1 — QuickJS engine port to 9front/kencc

Status: **`qjs` RUNS MODERN JAVASCRIPT ON 9front.** quickjs-ng v0.15.1 engine
(quickjs.c 63,529 lines + libregexp + libunicode + dtoa) + a minimal `n9_main.c`
link into a 1.46 MB `qjs`. Verified: closures, recursion, filter/map/reduce, regex
with capture groups + global replace, Unicode (`[..."café"]`==4, `"straße".toUpperCase()`
=="STRASSE"), try/catch, Map/Set, spread, JSON, **BigInt** (`2n**64n` correct), template
literals. See `test.js`.

**quickjs-libc.c also compiles, and a real CLI works.** `qjs` (1.85 MB, installed at
`/amd64/bin/qjs`) runs scripts/modules and an interactive REPL. Verified: `console.log`,
`scriptArgs`, `Date.now()`, the `std` + `os` modules, `std.getenv('home')`→`/usr/glenda`,
**real file I/O** (`std.open`/`puts`/`readAsString` round-trip), `os.getcwd()`. CLI source
is `n9_cli.c` (own minimal main, not upstream qjs.c — avoids the qjsc repl-bytecode
bootstrap). Build: `rc build-cli.rc`. Test: `cli-test.js`.

**Phase 2 (event loop) works too.** Timers + async fd I/O + Promises/async-await all run
on the loop. Verified: `os.setTimeout`/`setInterval` (correct chronological ordering),
`os.pipe`+`os.setReadHandler` async read fires on the loop, `Promise.all` over two
concurrent async workers, microtask-before-timer ordering. Tests: `timer-test.js`,
`async-test.js`, `promise-test.js`.

**Phase 3 (Node module layer) works too** — see `../../lib/boot.js` + `../../examples/`.
CommonJS `require` (builtins + relative files + JSON + `node:` scheme + `__dirname`) and
Tier 1 modules (path, events, util, buffer, process, fs, os, assert, stream) all pass
Node-doc examples. `n9_cli.c` loads `/amd64/lib/node9/boot.js` at startup (override:
`$NODE9_BOOT`). Note: `node9_port.h`/stubs gotcha — quickjs-ng's `os` module is the *low
level*; Node's `fs`/`os` are JS wrappers over it in boot.js. No `TextEncoder` global in this
qjs build → boot.js has a hand-rolled UTF-8 codec for Buffer.

**Parity track (real Node lib running 1:1):** `path` 24/24, `events` 16/16, `os` 17/17 (over a
Plan 9-native `internalBinding`). Bootstrap + adapted modules in `../../nodelib/`. Full node9
test suite (9/9) runs via `run-all-tests.rc`.

## THE kencc codegen bug (the deep one)
`os`'s dep chain contains a nested function declaration that shadows a parameter
(`function f(a,b){ function a(){} }` — repro: `../../examples/repro-arghoist-crash.js`). That
hits quickjs's arg-hoist write `args[var_idx - ARGUMENT_VAR_OFFSET]...`. **kencc distributes the
`*sizeof(JSVarDef)=24` multiply** into `args + var_idx*24 - ARGUMENT_VAR_OFFSET*24`. With
`var_idx == 0x20000000`, `var_idx*24 == exactly 0x300000000` — it forms `args + 0x300000000`
and botches subtracting it back, producing a wild store (acid regs: `SI=0x4ea790` clean vs
`DI=0x3004ea790`). Fix (patch.sh): a **bitmask barrier on the index** —
`((var_idx - ARGUMENT_VAR_OFFSET) & 0x7fffffff)` — forces the subtraction to materialize before
any address math. `(int)`/`(int64_t)` casts did NOT work; only the mask did. Debug gotcha:
kencc/APE `printf` truncates 64-bit pointers (`%p` and `%llx`) — use `acid regs()`, not printf.

Not yet done: proper `alloca` (still leaks); more parity modules (`util`/`buffer` pure-JS, then
`fs`/`net` native bindings — `net` via `/net`); `crypto` via APE libsec; Phase 4 `http`.

## Phase 2 — the event loop (how it works on Plan 9)
quickjs-libc's `os` module already implements timers + `setReadHandler`/`setWriteHandler`,
driven by `js_std_loop` → `js_os_poll_internal`. Two halves:
- **Timers**: when there are no fd handlers, `js_os_poll_internal` sleeps via `nanosleep`
  (APE has it) — never calls `poll`. So timers worked from day one, no changes needed.
- **Async fd I/O**: only this path calls `poll()`. Replaced the stub `poll.h` with a real
  `poll()` implemented over **APE `select()`** (verified working: detects pipe readiness,
  times out correctly). APE's select multiplexes fds with Plan 9 helper procs internally —
  so this IS the rfork-based native loop the plan envisioned, via a tested path rather than
  a hand-rolled rfork+channels reimplementation. `poll.h` is now a ~45-line translation
  layer (pollfd[] ↔ fd_set), not a stub.
This means async TCP via `/net` will work through the same path once a `net` module exists.

## quickjs-libc.c (POSIX) port — extra flag + stubs
Flag: add **`-D_BSD_EXTENSION`** (APE gates `sys/ioctl.h`/`sys/resource.h`/`sys/select.h`
behind it, same way `unistd.h` needs `_POSIX_SOURCE`).
Stub headers (in `-I.` path, `os.*` features degrade gracefully): **`poll.h`** and
**`dlfcn.h`** (both absent from APE) — `poll`/`dlopen`/`dlsym`/`dlclose` return error.
Shims added to `node9_port.h` for APE-absent POSIX: `realpath` (passthrough),
`sighandler_t`, `js_once`/`js_once_t` (threadless — cutils only defines them under
JS_HAVE_THREADS), `setenv`/`unsetenv` (no-op), `environ` (extern), `TIOCGWINSZ`+`winsize`,
`RUSAGE_SELF`+`getrusage`, `mkstemp`/`mkdtemp`, `utimes`, `setgroups`. Source edits in
patch.sh: APE `struct stat` has no `st_blocks` nor nanosecond `st_atim`/`st_mtim`/`st_ctim`
→ use seconds-based `st_atime*1000` and zero `st_blocks`.
SMOKE/degraded at runtime: `os.poll`, `os.dlopen`, `os.setenv`, tty size, getrusage,
mkstemp/mkdtemp, utimes — all return errors but don't crash. Core file/console/env/cwd work.

## How to build
1. Host: download quickjs-ng master, extract to `quickjs-master/` next to `patch.sh`.
2. Host: `bash patch.sh` → applies all fixes to a `work/` copy, produces `qjs-patched.tar.gz`.
3. Serve over LAN (`python3 -m http.server 8833`), `hget` into the dev VM.
4. VM: `rc build-all.rc` — fetches patched tree + `n9_main.c`, compiles all, links `./qjs`.
5. `qjs file.js` runs a script; bare `qjs` runs a built-in demo expression.

Compile flags: `pcc -c -I. -DPLAN9 -D__DJGPP -DNO_TM_GMTOFF -DJS_NAN_BOXING=0 -D_POSIX_SOURCE -D__STDC_NO_ATOMICS__=1`

## THE data-model bug (the one that mattered)
First run faulted: `js_dup()` deref of `proto=0xffffffff0044d4b8` — a 64-bit pointer with
its high 32 bits clobbered. acid backtrace pinned it. Root cause: quickjs.h does
`#if INTPTR_MAX < INT64_MAX → #define JS_NAN_BOXING 1`. APE typedefs `intptr_t` as 32-bit
`long`, so this 64-bit box was misdetected as a 32-bit build and turned on NaN-boxing,
which extracts pointers via `(intptr_t)(v)` — truncating them. Fix: **`-DJS_NAN_BOXING=0`**
(struct JSValue, full 8-byte `void *ptr`). The canonical [[python9-plan9-datamodel]] trap.
Switching to the struct rep then exposed kencc's no-compound-literals-on-unions limit in
`JS_MKPTR/JS_MKVAL/JS_NAN` → rewritten as static-inline helpers (see patch.sh).

## The fixes (each an empirically-found kencc/APE gap)

**Via compile -D flags:**
- `-D__STDC_NO_ATOMICS__=1` — skip C11 `<stdatomic.h>` (kencc lacks it); disables
  `Atomics`/`SharedArrayBuffer` (need real threads anyway).
- `-D__DJGPP` — clean lever: quickjs uses `__DJGPP` in exactly 4 spots, all wanted —
  skips `<pthread.h>` include, skips thread typedefs, sets `JS_HAVE_THREADS 0`, and
  uses `gettimeofday` instead of `clock_gettime`/`CLOCK_MONOTONIC`. No DOS code attached.
- `-DNO_TM_GMTOFF` — selects the `gmtime_r`/`difftime` offset path; APE `struct tm`
  has no `tm_gmtoff`.
- `-D_POSIX_SOURCE` — APE's `<unistd.h>` `#error`s "not defined in pure ANSI" otherwise.

**Via node9_port.h (injected at top of cutils.h):**
- `alloca` → routed through a `static` malloc wrapper (`n9_alloca`) defined *before*
  quickjs.c's `malloc` poisoning (line 2116). SMOKE-grade: LEAKS, no free. **TODO Phase 1
  proper:** convert the ~7 alloca sites to heap+free-on-every-return-path.
- `#define __attribute__(x)` and `#define __attribute(x)` — kencc rejects GCC attrs.
- C99 math absent from APE: shimmed `round trunc rint nearbyint lrint isfinite signbit
  acosh asinh atanh expm1 log1p exp2 cbrt fmax remainder`. SMOKE accuracy — tighten later.
  APE *has* (don't shim): `isnan isinf cosh sinh tanh log2 hypot fmin fmod`.
- GCC builtins absent: portable fallbacks for `__builtin_clz/clzll/ctz/ctzll`, and
  `__builtin_expect`→identity, `__builtin_frame_address`→NULL (stack-overflow guard
  is neutered — **TODO** give it a real address-of-local on Plan 9).
- `INFINITY`/`NAN`/`copysign` — bit-pattern helpers (APE math.h lacks the C99 macros).
- `scalbn`→`ldexp`; `localtime_r`/`gmtime_r` → wrap APE's non-reentrant `localtime`/`gmtime`.

**Via host-side sed (patch.sh):**
- `DIRECT_DISPATCH 1` → `0` — kencc has no computed goto; use switch dispatch.
- hex-float literal `0x1p63` → `9223372036854775808.0` (kencc can't parse C99 hex floats).
- `minimum_length(n) static n` → `n` — kencc rejects the C11 `arr[static N]` param syntax.
- `(JSAtomKindEnum){-1}` → `(JSAtomKindEnum)-1` — kencc rejects compound literals on enums.
- `return js_free_cstring(...)` → drop `return` — kencc rejects `return <void-expr>;`
  in void functions (C99-legal). Grow the `VOID_CALLS` list as more surface.

## Gotchas (workflow)
- Plan 9 `sed` treats `(` as a regex metachar — paren patterns silently no-match. Patch
  on the **host** (BSD sed) and transfer the tree; don't sed on the VM.
- After a child process (pcc) runs in a listen1 rc script, trailing `echo` output may not
  stream back. Write results to a file and read it in a **separate** `nc` call.
- kencc objects are `.6` (amd64), not `.o`.

## Data model
`long=4, ptr=8, int=4` (not LP64). Watch QuickJS NaN-boxing/pointer-tagging — not yet
stress-tested (no execution yet). See the `python9-plan9-datamodel` memory.
