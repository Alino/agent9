# cc9 — modern C++ for 9front, cross-compiled with clang/LLVM

9front (Plan 9) has no modern C++ compiler. kencc is C-only, APE has no C++
runtime, and the only native C++ is cfront 3.0.1 (1980s, no templates/STL). That
wall blocked V8, native npm addons, Rust, and a large slice of modern software.

**cc9 removes the wall by cross-compiling.** The compiler does *not* run on Plan 9.
A host **clang/LLVM** targets `x86_64`, a from-source **libc++/libc++abi** + a small
runtime bridge supply the standard library over Plan 9 syscalls, and an
**ELF→a.out converter** repackages the output so it runs on a **stock, unmodified
9front amd64 kernel**.

The result: **full modern C++ runs on 9front** — exceptions, the STL, iostreams,
threads, `<regex>`, wide characters, `<filesystem>`, RTTI, `thread_local`,
`std::filesystem`, and real third-party software (nlohmann/json; **Stockfish**,
which self-verifies). Validated against the upstream conformance suites of all
three runtime libraries cc9 ships — libc++, libc++abi, and libunwind — at ~100%
of applicable tests, `rfail=0`.

## Gallery — what cc9 makes possible

A **C++ path tracer** (Peter Shirley's "Ray Tracing in One Weekend" — pure
`<cmath>` + STL + `std::thread`, ~250 lines, no external libraries) rendering on
9front. Reflections, refraction through glass, soft shadows from diffuse bounces,
gamma — all computed by native C++ on a stock Plan 9 kernel. There is no Go
equivalent that *is* this; native C++ compute is something cc9 uniquely brings to
Plan 9. (`cc9/test/raytrace.cpp`)

![C++ raytracer rendered on 9front](screenshots/raytrace.png)

*The image above is the binary's actual output: it ran on the 9front VM and wrote
a 270,015-byte `P6` PPM (15-byte header + 400×225×3 pixels); that file was pulled
off the VM with `xd` (listen1 corrupts raw binary) and re-encoded to PNG. The
grain is the 24-sample path tracing.*

And the demo the existing Go runtime **cannot** match on Plan 9 — a **brainfuck
JIT** that emits x86-64 machine code at runtime and executes it. Stock 9front
NX-enforces all writable memory; cc9's opt-in W^X kernel patch is what unlocks it:

```text
$ bfjit            # gate off (wxallow=0): NX blocks it
bfjit 584: suicide: sys: trap: fault read addr=0x7ffffeffe000

$ bfjit            # gate on (wxallow=1): runs the JIT'd code
bfjit: compiled BF to 372 bytes of x86-64, executed it -> "Hello World!"

$ exceptions       # throw/catch, RAII unwinding across frames, rethrow
unwind=3 base=ok rethrow=ok custom=7 what=too big PASS

$ threads          # 100 std::threads + thread_local + std::call_once
sum=4950 tls=ok once=1 staticctor=1 PASS
```
(`cc9/test/bfjit.cpp`, `cc9/test/suite/`)

### Stockfish runs on 9front — and self-verifies

The headline real-world demo: **Stockfish 11** (a world-class chess engine —
multithreaded, compute-heavy, modern C++) cross-compiled with cc9 and run on a
stock 9front box. `stockfish bench` searches 46 fixed positions to depth 13 and
prints a **deterministic, ISA-independent node count** — so matching it is a
bit-for-bit proof that the search is *correct*, not merely that it ran:

```text
$ stockfish bench
Stockfish 11 64 POPCNT
===========================
Total time (ms) : 7977
Nodes searched  : 5156767      # identical to the host (macOS/clang) reference
Nodes/second    : 646454
```

Building it shook out — and fixed — a real cc9 condition-variable lost-wakeup
bug (the thread-pool search handshake deadlocked); see the conformance notes
below. cc9 now compiles a famous third-party C++ program that proves its own
correctness on Plan 9. (Build recipe: `git clone --branch sf_11`, compile
`src/*.cpp` + `syzygy/tbprobe.cpp` at `-std=c++14 -DUSE_POPCNT`, `cc9-link`.)

## Quick start

```sh
# prerequisites (host): brew llvm + lld; a from-source libc++ header tree; openlibm
#   (see "Building the runtime" below). Then:
cc9/host/cc9 build prog.cpp          # -> /tmp/prog.cpp.aout  (a Plan 9 a.out)
cc9/host/cc9 run   prog.cpp          # build, ship to the 9front VM, run it
```

```cpp
// prog.cpp — this compiles and runs on 9front:
#include <iostream>
#include <vector>
#include <algorithm>
#include <stdexcept>
int main() {
    std::vector<int> v{5,3,8,1};
    std::sort(v.begin(), v.end());
    try { throw std::runtime_error("ok"); }
    catch (const std::exception& e) { std::cout << e.what() << " "; }
    for (int x : v) std::cout << x << ' ';
    std::cout << '\n';
}
```

## How it works

```
  prog.cpp
     │  clang++ --target=x86_64-unknown-none -nostdlib -fexceptions -frtti
     │          -funwind-tables -fno-pic -femulated-tls   (freestanding SysV LP64)
     │          -isystem <from-source libc++ headers> -isystem cc9/runtime/include
     ▼
  prog.o ──┐
           │  ld.lld -T cc9/test/plan9.ld  --start-group libcc9cxx.a libcc9m.a --end-group
           ▼
  prog.elf
     │  cc9/host/elf2aout.py        (ELF -> Plan 9 amd64 a.out; vaddrs preserved)
     ▼
  prog.aout  ──►  runs on stock 9front amd64
```

- **Code model / ABI.** clang emits ordinary System V x86_64. The runtime reaches
  the Plan 9 kernel only through hand-written **syscall thunks**
  (`test/n9syscall.s`) that marshal SysV register-args into the Plan 9 ABI (args on
  the stack, syscall number in `RBP`, `SYSCALL`). Everything links statically; there
  is no dynamic loader on a.out.
- **Layout.** `plan9.ld` lays text (R+X) at `0x200028` and the data+bss segment at
  the next 2 MB boundary (where the 9front kernel maps it). `.eh_frame` is KEPT in
  the text segment so the bare-metal unwinder can find it. `elf2aout.py` repackages
  by vaddr into the 40-byte-header Plan 9 a.out format.

## The runtime bridge (`cc9/runtime`, packaged as `lib/libcc9cxx.a`)

| Piece | What it provides |
|---|---|
| `n9syscall.s` | SysV→Plan9 syscall thunks (pwrite/pread/open/brk/rfork/sem*/stat/wstat/…). **Saves/restores all SysV callee-saved regs around `SYSCALL`** — the Plan 9 kernel clobbers rbx/rbp/r13. |
| `crt0.c` | `_start`, **real `argc`/`argv`** (from the Plan 9 entry stack) + **`environ`** (from `/env`), `.init_array`/`.fini_array`, `atexit`/`__cxa_atexit`, an 8 MiB BSS main stack (shared via RFMEM so thread captures work), and **FP-exception masking** (bare-metal 9front traps on div-by-zero otherwise). |
| `n9libc.c` | freestanding libc: a K&R heap over the `brk` syscall (overflow-guarded `malloc`/`calloc`/`realloc`/`aligned_alloc`), `mem*`/`str*`, `strto*` (base/0x/sign/exponent), ctype, `strftime`, `div`/`ldiv`, time over `/dev/bintime`, GCC atomics. |
| `printf.c` | real `vsnprintf`/`vsscanf` incl. float conversion (long-double digit extraction over openlibm) and precision. |
| `stdio.c` | `FILE` layer over Plan 9 fds (stdio + real files; short-transfer-safe `fwrite`/`fread`). |
| `fs.c` | POSIX-over-9P: `open`/`stat`/`read`/`dir`/`wstat` (a slice of APE) backing `std::filesystem` + `std::fstream`. |
| `pthread.c` | pthreads over `rfork(RFMEM)` + semaphores: create/join/detach, mutex, **FIFO-queue condvar** (per-waiter semaphores — no signal-steal/lost-wakeup), once, TLS + emulated-TLS with **POSIX key destructors at thread exit** (`thread_local`), keyed by a stack-region thread id. |
| `cxxrt.cpp`, `exception_ptr.cpp`, `typeinfo_min.cpp` | `operator new/delete` (all replaceable forms), thread-safe static guards, `std::exception_ptr`/`rethrow_exception`, `type_info`. |
| `fenv.c` | `<fenv.h>` over MXCSR/x87 — `fenv_t` is **byte-identical to openlibm's** so its math routines and cc9 agree on the FP environment (a layout mismatch silently unmasked SSE exceptions and trapped). |
| real `charconv.cpp` + libc++abi `cxa_demangle.cpp` | full `to_chars`/`from_chars` incl. correctly-rounded float (LLVM-libc parser), and the real Itanium demangler (readable fault/terminate names). |
| from-source **libc++/libc++abi** objects | the STL runtime (string/locale/ios/regex/filesystem/chrono/…) + the **DWARF exception runtime** (libunwind bare-metal). |
| `lib/libcc9m.a` | **openlibm** cross-compiled for the target — a real correctly-rounded libm with 80-bit `long double`. |

**Exceptions** use the DWARF path (clang's x86_64 SJLJ codegen is buggy). libunwind
is built bare-metal and finds `.eh_frame` via linker symbols — no dynamic loader
needed. **Threads** use `rfork(RFMEM)`; main and thread stacks live in shared memory
so `std::thread([&]{…})` captures work.

## What runs

Verified on real 9front: **C++ exceptions** (throw/catch, RAII unwinding,
rethrow, `e.what()`), **STL** (vector/string/map/unordered/set/optional/sort…),
**iostreams** (`std::cout`/`cin`/stringstreams, formatted I/O), **threads**
(`std::thread`/mutex/condvar/future/`call_once`/atomics), **`<regex>`**,
**wide characters** (`std::wstring`/`wcout`), **`<filesystem>`** + `std::fstream`,
**RTTI** (`dynamic_cast`/`typeid`), **`thread_local`**, **`std::format`** floats,
**nlohmann/json** (parse + mutate + serialize), and **Stockfish 11** (a real
multithreaded chess engine whose `bench` self-verifies to the exact reference
node count).

### Conformance parity — the full runtime triad

The compiler is upstream clang (its x86_64 codegen is already trusted), so the
faithful-port question is about what cc9 *ported*: the runtime. cc9 is validated
against the **actual upstream conformance suites of all three runtime libraries
it ships**, at llvmorg-22.1.8, run on real 9front. Every suite uses a faithful
`lit` applicability filter (reusing lit's own `BooleanExpression` + the feature
set cc9's `_LIBCPP_*` macros actually define), so a test `lit` would skip
(`UNSUPPORTED`/`REQUIRES` for a feature cc9 lacks — no-tzdb, no-symlinks, ASan,
…) is a skip, not a failure.

| Suite | What it exercises | Result on 9front |
|---|---|---|
| **libc++ `test/std`** (≈6,770 applicable) | the STL / language library | ~99.9% build, ~99.x% run, **`rfail=0`** |
| **libcxxabi/test** | exceptions, RTTI, `dynamic_cast`, `exception_ptr`, guards, the demangler | **all applicable pass** (29,917/29,917 demangler symbols) |
| **libunwind/test** | the bundled DWARF unwinder | **8/9 applicable pass** (rest are wrong-arch / dl-loader-only) |
| **libcxx/test/libcxx** (312 RUN) | libc++ *implementation* internals + hardening | **312/312 RUN pass** |

`rfail=0` is the headline: **nothing that compiles and links has ever
miscompiled or faulted on 9front** — remaining gaps are compile/link-time
(genuinely unsupported platform features), not runtime bugs. Drivers:
`cc9/host/run-libcxx-tests.sh` (the std suite); the hand-written smoke/regression
suite is in `test/suite/` (`cc9/host/run-tests.sh`).

Runtime fixes that closed the last conformance clusters: an **`fenv_t` ABI
layout match** with openlibm (the x87/SSE FP-trap + complex-GPF root cause); a
**FIFO-queue condition variable** (the old one let a new waiter steal a signal —
deadlocked std::thread-pool handshakes); the full **death-test machinery**
(`pipe`/`dup2`/`fork`/`waitpid` + exit-code propagation + trap→SIGILL mapping);
**POSIX TSD destructors** + `[basic.start.term]` atexit ordering; **signal-over-
note** (`sigaction`/SIGALRM, deadline `nanosleep`); and the **real Itanium
demangler** (was a stub) so fault/terminate messages print readable C++ names.

## Optional JIT — the W^X kernel patch (`cc9/kernel`)

Stock 9front enforces NX on all writable memory, so a JIT (V8, LuaJIT) can't run.
A small, **opt-in** kernel patch (`cc9/kernel/`) adds a per-segment `SG_EXEC` flag
gated by a `plan9.ini` `wxallow` switch — **secure by default** (off ⇒ identical to
stock), executable-writable memory only when a process explicitly requests it and
the gate is on. Verified: with `wxallow=1`, a `segattach(SG_EXEC)` segment runs
generated machine code; everything else stays NX. This makes V8-class JIT
*reachable* on a patched kernel while leaving stock binaries unaffected (cc9's
static C++ never requests `SG_EXEC`). See `cc9/kernel/README.md`.

## Building the runtime

```sh
# one-time: a from-source libc++ header tree + openlibm (see docs/ for the recipe)
cc9/host/build-runtime.sh     # -> cc9/lib/libcc9cxx.a   (the C++ runtime archive)
cc9/host/build-libm.sh        # -> cc9/lib/libcc9m.a     (openlibm)
cc9/host/build-modules.sh     # -> cc9/lib/modules/      (optional: std / std.compat
                              #    C++23 module BMIs — `import std;` works on 9front)
```

Environment: `CC9_LLVM` (brew llvm bin), `CC9_LLD` (ld.lld), `CC9_LIBCXX`
(from-source libc++ headers), `CC9_LLVMSRC` (llvm-project tree), `CC9_DEV`
(`host port` for the VM listener).

## Limitations (honest)

- **Host-only compiler.** cc9 cross-compiles; clang does not run on 9front
  (self-hosting is far off). Output is **static** a.out only (no dynamic linking —
  by design on Plan 9).
- **No Plan 9 native-lib linking.** cc9 code is internally SysV and reaches the
  kernel only through the syscall thunks; it cannot link against Plan 9's own libc
  or existing Plan 9 C libraries (that needs an LLVM Plan 9 calling-convention).
- **JIT needs the opt-in kernel patch** (above); stock 9front is NX-enforced.
- **Bare-metal FP** is masked at startup but only fully verifiable on real hardware
  (QEMU TCG doesn't trap on FP exceptions regardless).
- Minor documented gaps: `getenv` returns a shared static buffer; wide numeric
  parsing caps at 127 chars. (`crt0` now passes the real kernel `argc`/`argv` and
  populates `environ` from `/env`.)

The runtime has been through adversarial multi-agent review rounds plus the full
runtime-triad conformance pass (libc++/libc++abi/libunwind, see above) and a
real-world Stockfish bring-up — each round's fixes are in the git history.

## Layout

```
cc9/host/        cross-toolchain: cc9 wrapper, build-runtime.sh, build-libm.sh,
                 build-modules.sh, elf2aout.py, run-tests.sh, run-libcxx-tests.sh
cc9/runtime/     the runtime bridge (libc shim, C++ runtime, pthreads, fs, stdio)
cc9/runtime/include/  minimal C headers
cc9/test/        n9syscall.s, plan9.ld, demos (json/stl/…), suite/ (regression)
cc9/kernel/      optional W^X/JIT kernel patch (wxallow + SG_EXEC)
cc9/lib/         built archives (libcc9cxx.a, libcc9m.a); modules/ (std BMIs)
cc9/vendor/      third-party headers used in demos (nlohmann/json)
```
