# cc9 — modern C++ for 9front via clang/LLVM

9front (Plan 9) has no modern C++ compiler. kencc is C-only, APE ships no C++
runtime, and the only native C++ is cfront 3.0.1 (1980s — no templates, no STL).
That wall has kept a large slice of modern software off Plan 9.

**cc9 removes the wall.** A host **clang/LLVM** targets freestanding `x86_64`, a
from-source **libc++ / libc++abi / libunwind** plus a small runtime bridge supply
the standard library over Plan 9 syscalls, and an **ELF→a.out converter**
repackages the output to run on a **stock, unmodified 9front amd64 kernel** — no
kernel patches, no dynamic loader.

The result is **full modern C++ on 9front**: exceptions, the STL, iostreams,
threads, `<regex>`, wide characters, `<filesystem>`, RTTI, `thread_local`,
`std::format`, C++23 `import std;`, and real third-party software. It is validated
against the upstream conformance suites of all three runtime libraries it ships,
at ~100% of applicable tests with **zero runtime failures**. And the toolchain
itself — a reduced clang + lld — now also runs **natively on 9front**.

## What cc9 makes possible

**A C++ path tracer** — Peter Shirley's *Ray Tracing in One Weekend*: pure
`<cmath>` + STL + `std::thread`, ~250 lines, no external libraries — rendering on
a stock Plan 9 kernel. Reflection, refraction through glass, soft shadows from
diffuse bounces, gamma. Native C++ compute like this is something cc9 uniquely
brings to Plan 9. (`test/raytrace.cpp`)

![C++ raytracer rendered on 9front](screenshots/raytrace.png)

*The image is the binary's actual output: a 400×225, 24-sample path-traced render
written as a PPM by native C++ on 9front.*

**Stockfish runs on 9front — and proves its own correctness.** Stockfish 11 (a
world-class, multithreaded chess engine) cross-compiled with cc9. Its `bench`
command searches 46 fixed positions to a set depth and prints a **deterministic,
ISA-independent node count** — so matching the reference is a bit-for-bit proof
that the search is *correct*, not merely that it ran:

```text
$ stockfish bench
Stockfish 11 64 POPCNT
===========================
Total time (ms) : 7977
Nodes searched  : 5156767      # identical to the host (clang) reference
Nodes/second    : 646454
```

**The exception / threading runtime, exercised directly:**

```text
$ exceptions     # throw/catch, RAII unwinding across frames, rethrow
unwind=3 base=ok rethrow=ok custom=7 what=too big PASS

$ threads        # 100 std::threads + thread_local + std::call_once
sum=4950 tls=ok once=1 staticctor=1 PASS
```

(`test/`, `test/suite/`)

## Quick start

```sh
# host prerequisites: brew llvm + lld, a from-source libc++ header tree, openlibm
# (see "Building the runtime"). Then build the runtime archives once and compile:
host/cc9 build prog.cpp        # -> a Plan 9 amd64 a.out
host/cc9 run   prog.cpp        # build + run on a configured 9front target
```

```cpp
// prog.cpp — compiles and runs on 9front:
#include <iostream>
#include <vector>
#include <algorithm>
#include <stdexcept>
int main() {
    std::vector<int> v{5, 3, 8, 1};
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
     │          -isystem <libc++ headers> -isystem runtime/include
     ▼
  prog.o
     │  ld.lld -T test/plan9.ld  --start-group libcc9cxx.a libcc9m.a --end-group
     ▼
  prog.elf
     │  host/elf2aout.py        (ELF -> Plan 9 amd64 a.out; virtual addresses preserved)
     ▼
  prog            ──►  runs on stock 9front amd64
```

- **Code model / ABI.** clang emits ordinary System V x86_64. The runtime reaches
  the kernel only through hand-written **syscall thunks** (`test/n9syscall.s`) that
  marshal SysV register-args into the Plan 9 ABI (args on the stack, syscall number
  in `RBP`, `SYSCALL`), saving/restoring the callee-saved registers the kernel
  clobbers. Everything links statically — there is no dynamic loader on a.out.
- **Layout.** `plan9.ld` places text (R+X) at `0x200028` and the data+bss segment
  at the next 2 MB boundary, where the kernel maps it. `.eh_frame` is kept inside
  the text segment so the unwinder can find it with no loader. `elf2aout` repackages
  by virtual address into the Plan 9 a.out format.
- **Exceptions** use the DWARF path; libunwind is built bare-metal and locates
  `.eh_frame` via linker symbols. **Threads** use `rfork(RFMEM)` with main and
  thread stacks in shared memory, so `std::thread([&]{…})` captures work.

## The runtime bridge

The standard library is the real from-source libc++/libc++abi/libunwind; cc9 adds
a thin freestanding C layer and a few C++ runtime pieces over Plan 9 syscalls,
packaged into `lib/libcc9cxx.a` (+ `lib/libcc9m.a`, openlibm).

| Piece | What it provides |
|---|---|
| `n9syscall.s` | SysV→Plan 9 syscall thunks (read/write/open/brk/rfork/sem*/stat/dup/pipe/await/…). |
| `crt0.c` | `_start`, real `argc`/`argv` + `environ` (from `/env`), init/fini arrays, `atexit` with `[basic.start.term]` ordering, the main stack (shared via RFMEM), startup FP-exception masking. |
| `n9libc.c` | freestanding libc: heap over `brk`, `mem*`/`str*`, `strto*`, ctype, `strftime`, `div`/`ldiv`, time over `/dev/bintime`, GCC atomics. |
| `printf.c`, `stdio.c` | `vsnprintf`/`vsscanf` with float conversion; a `FILE` layer over Plan 9 fds (incl. `fmemopen`). |
| `fs.c` | POSIX-over-9P (`open`/`stat`/`read`/`dir`/`wstat`, errno from Plan 9 error strings) backing `std::filesystem` and `std::fstream`. |
| `pthread.c` | pthreads over `rfork(RFMEM)`: create/join/detach, mutex, a **FIFO-queue condition variable** (per-waiter semaphores — no lost wakeups), `once`, TLS / emulated-TLS with POSIX key destructors at thread exit. |
| `fenv.c` | `<fenv.h>` over MXCSR/x87, with `fenv_t` byte-identical to openlibm's so the math library and cc9 agree on the FP environment. |
| `cxxrt.cpp`, `exception_ptr.cpp` | `operator new/delete` (all replaceable forms), thread-safe static guards, `std::exception_ptr`. |
| libc++abi runtime | the DWARF exception runtime, RTTI, and the Itanium demangler (readable terminate/fault messages). |
| `lib/libcc9m.a` | openlibm cross-compiled for the target — correctly-rounded libm with 80-bit `long double`. |

## Conformance

The compiler is upstream clang, whose x86_64 codegen is already trusted, so the
question for a *port* is the runtime. cc9 is validated against the **actual
upstream conformance suites of the three runtime libraries it ships**, at
llvmorg-22.1.8, run on real 9front. Each suite uses a faithful `lit` applicability
filter (lit's own `BooleanExpression` evaluator plus the feature set cc9's
`_LIBCPP_*` macros define), so a test `lit` would skip — `UNSUPPORTED`/`REQUIRES`
for a feature the platform lacks (no time-zone DB, no symlinks, no sanitizers) —
is a skip, not a failure.

| Suite | Exercises | Result on 9front |
|---|---|---|
| **libc++ `test/std`** (≈6,770 applicable) | the STL / language library | ~99.9% build, ~99% run, **0 runtime failures** |
| **libc++abi/test** | exceptions, RTTI, `dynamic_cast`, `exception_ptr`, the demangler | all applicable pass (29,917/29,917 demangler symbols) |
| **libunwind/test** | the DWARF unwinder | 8/9 applicable pass (rest are wrong-arch / dynamic-loader-only) |
| **libcxx/test/libcxx** (312 RUN) | libc++ implementation internals + hardening | 312/312 |

The headline is **zero runtime failures**: nothing that compiles and links has
miscompiled or faulted on 9front. The remaining gaps are compile/link-time —
genuinely unsupported platform features — not runtime bugs. The `test/std` suite
is driven by `host/run-libcxx-tests.sh`; a hand-written smoke/regression suite
lives in `test/suite/` (`host/run-tests.sh`).

## Running the compiler on 9front

Beyond cross-compiling, a reduced clang and `ld.lld` (built for the target with
cc9 itself) plus a C `elf2aout` run **on 9front**, chained by the `cc` driver
(`native/cc.rc`):

```text
term% cc hello.cpp -o hello     # clang -> ld.lld -> elf2aout, entirely on the box
term% hello
hello, 9front
```

This is proven on real hardware, and **37/37 of the libc++ `.pass.cpp`
conformance tests compile *and* run natively** on 9front. The native clang is
X86-only and currently staged from `/tmp`; the remaining stretch goal is true
self-hosting (clang rebuilding clang on the box) and a proper install layout. See
`native/README.md`.

## Optional JIT — the W^X kernel patch

Stock 9front enforces NX on all writable memory, so a JIT (V8, LuaJIT) cannot run.
A small, **opt-in** kernel patch (`kernel/`) adds a per-segment `SG_EXEC` flag
gated by a `plan9.ini` `wxallow` switch — **secure by default**: off is identical
to stock, and executable-writable memory is granted only when a process explicitly
requests it *and* the gate is on. With the gate on, a `segattach(SG_EXEC)` segment
runs generated machine code; everything else stays NX. cc9's static C++ never
requests it, so stock binaries are unaffected. This makes V8-class JIT *reachable*
on a patched kernel. See `kernel/README.md`.

A bundled demo, a brainfuck JIT (`test/bfjit.cpp`), emits x86-64 at runtime:

```text
$ bfjit          # gate off: NX blocks it
bfjit: suicide: sys: trap: fault read ...
$ bfjit          # gate on (wxallow=1): runs the JIT'd code
bfjit: compiled BF to 372 bytes of x86-64, executed it -> "Hello World!"
```

## Building the runtime

```sh
host/build-runtime.sh     # -> lib/libcc9cxx.a   (the C++ runtime archive)
host/build-libm.sh        # -> lib/libcc9m.a     (openlibm)
host/build-modules.sh     # -> lib/modules/      (optional: std / std.compat C++23
                          #    module BMIs, so `import std;` works on 9front)
```

The build archives are regenerated by these scripts and are not committed.
Configuration is via environment variables: `CC9_LLVM` (brew llvm `bin`),
`CC9_LLD` (ld.lld), `CC9_LIBCXX` (from-source libc++ headers), `CC9_LLVMSRC`
(llvm-project tree).

## Limitations

- **Static a.out only** — no dynamic linking, by design on Plan 9.
- **No linking against Plan 9's own C libraries.** cc9 code is internally System V
  and reaches the kernel only through the syscall thunks; linking Plan 9 libc would
  require teaching LLVM the Plan 9 stack-args calling convention.
- **JIT needs the opt-in kernel patch**; stock 9front is NX-enforced.
- **Native (on-box) compilation** works for programs but is not yet self-hosting,
  and the native toolchain is X86-only.

## Layout

```
host/        cross-toolchain: the cc9 wrapper, build-{runtime,libm,modules}.sh,
             elf2aout.py, run-tests.sh, run-libcxx-tests.sh
runtime/     the runtime bridge (libc shim, C++ runtime, pthreads, fs, stdio, fenv)
runtime/include/  minimal C headers
native/      the on-box toolchain: reduced clang/lld build + the `cc` driver
test/        n9syscall.s, plan9.ld, demos (raytrace/json/bfjit/…), suite/ (regression)
kernel/      optional, opt-in W^X/JIT kernel patch (wxallow + SG_EXEC)
lib/         built archives (generated; libcc9cxx.a, libcc9m.a, modules/)
vendor/      third-party headers used in demos
```
