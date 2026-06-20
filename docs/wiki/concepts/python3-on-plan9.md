---
title: Python 3 on Plan 9 — port strategy + parity harness
created: 2026-06-18
updated: 2026-06-18
type: concept
tags: [toolchain, cross-compile, status-wip, fs]
---

# Python 3 on Plan 9

Effort to bring **CPython 3.11** to 9front/amd64, validated against CPython's
own regression suite rather than by inspection. Lives under `python9/` in the
repo. Complements [[build-toolchain]] and [[llm-porting-workflow]].

> **Why this exists.** The north-star is running the Nous `hermes-agent` on
> 9front. See the honest blocker analysis below — a working interpreter is a
> *prerequisite, not the finish line*. Contrast with [[pi9-architecture]],
> which deliberately went the other way (Go-native agent, "not Hermes-on-plan9").

## State of the art (what already exists)

- **Python 2.5.1** historically shipped with 9front (only because Mercurial
  needed it). Archived in 9front's `pyhg` repo.
- **CPython 2.7.5** — real port by Jeff Sickel ("jas", github.com/vat9),
  sponsored by Coraid, ~2013. `plan9` branch off 2.7; added a native `_p9ssl`
  on libsec/`ssl(3)`/`tls(3)`. 386/amd64. **No longer maintained.**
- **Python 3** — never finished. The 2013 9fans "Python3 for Plan9" thread is
  the high-water mark (3.3.2 cross-compiled for ARM, never tested). **No
  maintained CPython 3 port for 9front exists today.**
- **Cosmopolitan "APE-python"** (jart) builds 2.7.18/3.6.14 fat binaries — but
  targets Linux/Mac/Windows/BSD, **not Plan 9**. Name collision: Cosmopolitan
  "APE" (Actually Portable Executable) ≠ Plan 9 "APE" (ANSI/POSIX Environment).

## Why 3.11

Lowest version `hermes-agent` allows (`requires-python >=3.11,<3.14`), so it
minimizes the interpreter delta while staying in range. Host reference pinned to
**3.11.14** so host and port run byte-identical `Lib/test`.

## Parity is the contract

The bar is **not** "100% of the suite" — no CPython port reaches that on any
platform (CPython has tiered platform support). Measurable bar:

> Of every testcase that passes on the reference 3.11.14 build and is not on the
> justified skip-list, what fraction passes on the 9front port?
> `parity = port_pass / applicable_ref_pass`

Harness (`python9/parity/`): `run_suite.py` (portable, stdlib-only, 3.11 syntax —
runs identically on host and in the VM) emits a normalized JSON manifest from
`python -m test --junit-xml`; `score.py` diffs port vs reference and reports the
parity score + regression work-queue; `skiplist.txt` is curated and justified.

**Reference baseline captured:** 38,259 passing testcases on 3.11.14
(darwin/arm64), 2m08s. That is the denominator. `test_distutils` fails on the
host and self-excludes (parity only counts ref-passing tests).

## APE build environment (recon, 9front GEFS SP1, amd64)

Verified live in the dev VM (`sysname cirno`):

- **Compilers:** `pcc` (APE C driver → `6c`/`6l`), native `6c`/`6l`.
- **APE utilities — only 12:** `basename cc dirname expr kill make psh sed sh
  stty tar uname`. **No `ls`/`pwd`/`grep`/`tr`/`cat`/`mkdir`/`rm`.**
- **Headers:** full `/sys/include/ape/*` (stdio, unistd, bio, draw, …).
- **SSL:** `/amd64/lib/ape/libsec.a` + `libsec.h` + `mp.h` present → native
  `_ssl`/`_p9ssl` path viable (reuse jas's approach).
- **Missing — the two hard constraints:**
  - **No `dlfcn.h`** → no dynamic loading. All C extensions must be **static**,
    listed in `Modules/Setup` and linked into the interpreter. `importlib`
    dynamic `.so` loading is out.
  - **No `pthread.h`** → CPython 3.7+ makes threads mandatory. Need a **Plan 9
    thread backend** (`Python/thread_plan9.h`: `rfork(RFMEM)`/`proccreate` +
    locks). This is the single biggest interpreter-side task and what jas had
    to solve for 2.7.

## Decision: bypass `configure`

CPython's autoconf `configure` **does not run on this APE**. Confirmed: it
shells out to `ls -di .`/`pwd`/`grep`/`tr`/etc., APE ships none of those, the
PATH falls back to native Plan 9 tools whose flags differ, and configure dies at
`working directory cannot be determined` (native `ls` rejecting autoconf flags).
Shimming a full POSIX coreutils is a rabbit hole.

**Strategy = hand-author `pyconfig.h` + a custom `mkfile`** (6c/6l or pcc
directly) with a static `Modules/Setup`. This is what jas's 2.7 port and the
historical 9front python did. Do **not** fight autoconf.

## Hermes reality check (the real blocker)

Porting the interpreter does **not** make `hermes-agent` run. Its 219-package
tree includes **Rust-backed** extensions — `pydantic-core`, `jiter`,
`cryptography`, `rpds-py`, `tokenizers`, `watchfiles` — which **cannot compile
on Plan 9 (no rustc target)**, plus OS-locked C extensions (`uvloop` = libuv,
`psutil`, `httptools`). Running Hermes would require a de-Rusted fork
(pydantic v1 pure-Python, stdlib http instead of aiohttp/uvloop). The Go-native
route ([[pi9-architecture]]) remains the cheaper path for the agent itself.

## VM workflow gotchas (this port specifically)

See also [[testing-harness]] for listen1 pitfalls. Hard-won here:

- **Single-line, absolute-path commands only** over listen1. Multi-line, `&&`,
  and `;`-chains silently return empty on the slow TCG VM.
- **`hget` has no `-o`** on this build — use shell redirect:
  `hget URL > /abs/path`.
- 25 MB source transfer: host `python3 -m http.server`, VM
  `hget http://10.0.2.2:PORT/... > /usr/glenda/py.tgz`, then
  `gunzip < py.tgz | tar x`.

## Status / next

**WORKING: CPython 3.11.14 runs on 9front, and the regression harness runs on
it.** `/tmp/python` runs real Python programs, the pure-Python stdlib, 24
static C extensions, `subprocess`, and `python -m test`. ~150 sources +
bundled Expat compile + link into an ~19 MB binary; stdlib at
`/sys/lib/python/lib/python3.11`.

The bug cascade, all fixed (see `python9/port/plan9/README.md`):
1. kencc has no compound literals / GNU `,##__VA_ARGS__` / computed-gotos;
   pads tiny structs to 8; rejects the token `typestr` in params.
2. Plan 9 amd64 is **not LP64**: long/size_t/time_t=4, ptr/longlong/uintptr=8.
   Wrong SIZEOF_* corrupted every word-at-a-time op.
3. Inline-cache skip used `sizeof(cache)/2` (padded) instead of the
   `_PyOpcode_Caches` counts -> eval loop hit CACHE opcodes.
4. wchar_t: APE's 2-byte vs CPython's 4-byte; the wchar_shim must predefine
   4-byte wchar_t before any system header, else wcslen truncates strings.
5. Locale decode used APE's 2-byte mbstowcs; `_Py_FORCE_UTF8_LOCALE` routes to
   CPython's own UTF-8 decoder (Plan 9 is UTF-8).
6. **Timsort GP fault** (`import signal` -> enum sort): `binarysort()` stored
   `lo.values - lo.keys` (a difference between two *independent* allocations:
   stack keys array vs heap items array) in a 4-byte `Py_ssize_t`, truncating a
   47-bit pointer delta to garbage. Rewrote to index both arrays by the same
   small in-array index. This is the canonical hazard of keeping Py_ssize_t at
   4 bytes while pointers are 8 -- watch for `ptrA - ptrB` stored in Py_ssize_t
   elsewhere as more of the suite runs.

Then, to unblock the regression harness:
7. `sys.platform` was "unknown" and `sys.abiflags` was missing (configure
   normally sets them) -> `sysconfig._get_sysconfigdata_name()` crashed. Fixed
   with `PLATFORM "plan9"` + `ABIFLAGS ""` in pyconfig.h and a hand-authored
   `Lib/_sysconfigdata__plan9_.py` (`build_time_vars`).
8. `faulthandler` was a boot-only stub with no Python API; regrtest calls
   `faulthandler.enable()`. Added no-op `enable/disable/dump_traceback*/
   register/...` methods (a Plan 9 fault -> Broken proc, inspect with acid).
9. `--junit-xml` needs `pyexpat`; built it + bundled Expat (xmlparse/xmltok/
   xmlrole). Expat's `PREFIX` type collided with pyconfig's install-path
   `PREFIX` macro -> `#undef PREFIX` in expat_config.h.
10. regrtest `-j` spawns manager **threads** (`RunWorkers`), which the
    single-threaded pthread stubs reject ("can't start new thread"). The port
    must run **sequentially in-process** (no `-j`); `run_suite.py --jobs -1`
    omits `-j`. Trade-off: no per-test process isolation, so a hard crash
    aborts the batch -- curate toward expected-pass modules until threads land.

**Parity harness is operational on the port.** `run_suite.py` runs under the
ported interpreter, spawns regrtest via `subprocess` (APE fork/exec works),
emits junit XML, and produces a v1 manifest with per-testcase IDs identical to
the host reference.

**Parity result (curated 42-module core batch, 2026-06):**
- **99.17%** (6061/6112) over *applicable* testcases (ref-passing, not on the
  justified skip-list) across the **39 modules that run to completion**.
- 145 justified skips, all with one-line reasons in `skiplist.txt`: 32-bit
  `Py_hash_t` (hash values/order differ), 4-byte-`Py_ssize_t` `sizeof`/native
  struct sizes, `_testcapi`/`_testinternalcapi`-only tests, tz-data, thread
  tests, locale. 51 regressions remain *visible* (not hidden): **47 are the
  math family** (test_math/cmath/float/complex/statistics/fractions) -- now
  almost entirely **APE libm transcendental *precision*** (sin/cos/tan/exp/pow
  ULP error), the remaining frontier; 12 a narrow non-math tail (dtoa rounding,
  breakpoint/pdb env edge, struct half-float, top-level-await flag).
- Reports: `parity/reports/parity-scored.json` (work queue),
  `parity/manifests/9front-port.json` (port manifest).

Root-cause bugs found+fixed via the parity loop (all in the patch set / shims):
- **kencc float comparison codegen is wrong for NaN** -- `nan == nan` compiled
  to true, `nan != nan` to false, `nan < x` to true (IEEE-unordered case not
  handled). Corrupted float equality interpreter-wide (dicts/sets/`in`/sort/
  json/array). Guarded `float_richcompare` (and the `_json` float encoder).
- **APE `printf` reads 8 bytes for the `%z` length modifier**, but Py_ssize_t
  is 4 bytes -> `%zd`/`%zu` print garbage. Broke `PyUnicode_FromFormat`
  (reprs/error messages) and `_pickle`'s protocol-0 memo (emitted garbage memo
  indices -> `OverflowError` unpickling set subclasses). Fixed to `%l` + cast.
- **kencc compiles unary `-x` as `0.0-x`**, losing negative zero -> `float_neg`
  flips the sign bit. `HAVE_COPYSIGN`/`HAVE_ROUND` route to our shims.
- **`_socket` built** (~520 tests that failed on `import socket`).
- **APE libm violates C99 special-value/errno rules** -> exact `sqrt` (SQRTSD
  asm) + special-value-correct fmod/log1p/expm1/acosh/asinh/atanh/cbrt/exp2;
  `m_exp`/`m_cosh`/`m_sinh`/`m_acos` wrappers in mathmodule.c guard inf/nan
  inputs and drop APE's spurious errno (delegating finite values to APE).
- **APE `log2` is wrong for subnormals** (`log2(1e-308)` off by 52) -> undef
  `HAVE_LOG2` so CPython's frexp+log fallback is used.
- `WITH_DOC_STRINGS` added to the hand-authored sysconfigdata (docstring tests
  were wrongly skipped); `lib-dynload` dir created (silences the path warning).

**Deployment note:** the parity batch sets `PYTHONPYCACHEPREFIX=/tmp/pyc`. The
stdlib install at `/sys/lib/python` is not writable by `glenda`, so its in-tree
`__pycache__` is stale (it predates the `float_neg` fix and marshaled `-0.0`
constants as `+0.0`). A clean deploy must either install the `.pyc` *after* the
final interpreter build, or run with a writable `PYTHONPYCACHEPREFIX` so Python
recompiles fresh -- otherwise signed-zero literals are wrong.

## The libm-precision frontier (the remaining ~47 math-family regressions)

`port/plan9/ape-shim/plan9_libm.c` vendors correctly-structured fdlibm
`exp`/`cosh`/`sinh`/`tanh` (compiled + linked like the other shims; they
override APE's at link). They are measurably more accurate than APE's (e.g.
`exp(709)`: APE ~40 ulp off, ours ~2) -- **but integrating them is net-neutral
on the parity score.** The remaining `test_math`/`test_cmath`/`test_float`/
`test_complex`/`test_statistics`/`test_fractions` failures do **not** flip,
because:
1. `test_math`'s `test_testfile`/`test_mtestfile` compare against reference
   values at <= ~few-ulp tolerance, and APE's *and* fdlibm's results are both
   ~1 ulp -- you need a **correctly-rounded (<= 0.5 ulp)** libm to pass, not
   merely <1 ulp. That is a research-grade implementation (CORE-MATH-style),
   not a straight fdlibm port.
2. Some failures are not libm at all: `test_fsum` (CPython's own exact-sum
   algorithm), `test_complex` `(-4+infj)**-n` (IEEE complex-with-infinity edge
   cases), `test_dist`/`testHypot` accumulation.
So the honest bound: replacing APE libm one function at a time at fdlibm
accuracy will not move parity; closing this frontier needs a correctly-rounded
math library (large) -- or these stay as documented FP-accuracy limits.
`log` is written but its subnormal path NaNs under kencc (`#if 0`'d); APE's
log is kept. `sin`/`cos`/`tan` (range reduction) and `pow` are not yet vendored.

Also fixed: `math.pow(0, neg)` raised OverflowError instead of ValueError (APE
`pow(0,neg)` returns DBL_MAX+ERANGE, not the divide-by-zero inf) -- handled in
`math_pow_impl`. `testLog2Exact`/`testRemainder` are skip-listed: both need
correct `frexp` on subnormals (`log2(2**-1074)`, `float.as_integer_ratio`), and
APE's `frexp`/`ldexp`/`modf` are wrong for subnormals *and* bundled in one libc
object, so overriding one needs all three -- deferred.

Fixes that got here (this push), all in the patch set / shims:
- **`_socket` built** -- unblocked test_functools/builtin/random/hashlib (~520
  tests) that failed purely on `import socket` via `test.support`. Needed
  disabling inherited Apple/BSD config (`net/if.h`, `sys_domain.h`,
  `kern_control.h`, `SOCKADDR_SA_LEN`) + an `hstrerror` shim.
- **APE `fmod` infinite-loops on NaN/Inf** -> correct fmod (binary long
  division for finite values) in plan9_compat.c.
- **APE libm violates C99 special-value/errno rules** (sqrt(nan)=0+EDOM,
  exp(inf)=DBL_MAX+ERANGE): exact `sqrt` via SQRTSD asm (`hwsqrt.s`), and
  special-value-correct shims for log1p/expm1/acosh/asinh/atanh/cbrt/exp2.
- **kencc compiles unary `-x` as `0.0-x`, losing negative zero**; patched
  `float_neg` to flip the sign bit. `HAVE_COPYSIGN`/`HAVE_ROUND` make CPython
  use our shims instead of its atan2-based `_Py_copysign` (also broken on APE).
- Created `lib-dynload` so the "Could not find platform dependent libraries"
  warning stops polluting subprocess output (fixed test_json.test_tool).

Remaining 116 regressions (the work queue): ~55 are the math family
(test_math/cmath/float/complex/statistics/fractions) -- residual **APE libm
precision** on sin/cos/tan/exp/pow/sinh/cosh, a genuine platform limit short of
replacing libm. The rest are a long tail: a real `_pickle`/`ssize_t` framing
bug (`OverflowError` pickling set subclasses), enum Flag composite, functools/
contextlib edge cases. `test_queue` busy-waits unkillably (needs the thread
backend) and is skip-listed.

[x] C extension modules the suite needs (math/_datetime/_struct/array/_json/
    _pickle/unicodedata/pyexpat/... -- 24 statically linked in config.c).
[x] Harness runs end-to-end on the port; junit + subprocess + sysconfig work.
[ ] Score the curated batch vs a module-filtered slice of `host-reference.json`
    (full 38,259 denominator needs the whole suite, impractical on TCG speed).
[ ] Widen coverage batch-by-batch; triage regressions into `skiplist.txt`.
[ ] Real Plan 9 thread backend (rfork/proccreate) -> unblocks regrtest `-j`
    worker isolation and actual threading tests.
