# zig9 — Zig for 9front

A port of the [Zig](https://ziglang.org) toolchain to **9front/amd64**, in the
spirit of the sibling ports `python9`, `node9`, and `cc9`. zig9 compiles Zig into
native Plan 9 `a.out` executables that run on stock 9front — **with no LLVM and no
C runtime**: it uses Zig's own self-hosted x86_64 backend and Plan 9 a.out linker,
talking to the kernel through raw syscalls.

**Status: a large subset of the language runs natively on 9front, and Zig's own
upstream behavior test suite runs on real hardware.**

- **Corpus: 13/13** self-checking feature tests pass, **identical on the QEMU dev
  VM and `cirno` (bare-metal Shuttle 9front)** — including heap allocation,
  `std.mem.sort`, `std.AutoHashMap`, `ArrayList`, `std.fmt`, FP math, generics,
  comptime, tagged unions, error unions.
- **Upstream behavior suite: 1792 of Zig's own `test/behavior/*.zig` tests pass**
  on 9front (`test/parity/manifests/behavior-qemu.json`), via a minimal plan9
  test runner — **0 failures, 0 crashes: 100% of the tests the suite considers
  runnable on the self-hosted x86_64 backend.** The 291 "skips" are the suite skipping
  *itself* (`error.SkipZigTest`: TODO markers + self-hosted-backend feature
  gates); **zero are plan9-specific** — this port adds no skip gate and hides no
  failure behind one. See `port/plan9/NOTES.md`.

Reaching this required, beyond a working cross-compiler, **rebuilding the Zig
compiler from patched source** to fix real self-hosted-backend bugs in its
experimental plan9 target (see `port/plan9/README.md`).

## The compiler itself runs ON 9front (not just cross)

Beyond cross-compiling, **the Zig compiler runs natively on 9front** and compiles
Zig programs on-box. `pac9 install zig9`, then:

```
zig build-exe hello.zig -OReleaseFast     # or: zig run / zig test
```

**How.** Zig's self-hosted backend can't compile the compiler itself to a Plan 9
a.out (its f16/f128 softfloat + comptime-float hit "ran out of registers"), so we
use Zig's own **C-backend (CBE) bootstrap**: a host zig emits the whole patched
compiler as one C file (`zig2.c`, ~213 MB) targeting `x86_64-plan9`, and
[cc9](../cc9)'s clang→ld.lld→elf2aout pipeline compiles that into a ~56 MB native
a.out linked against n9libc (real fork/exec, POSIX-over-9P fs, pthreads, malloc).
The resulting `zig9` drives the *same* self-hosted x86_64 backend + Plan 9 linker
as the cross path, so its output carries the same 1792-test verification.
Build it with `port/plan9/native/build.py`; package with `--package`.

**Proven reliable — two heavy programs, bit-exact.** Compiled natively on
bare-metal `cirno` and run there, producing output **byte-identical to an
aarch64-linux reference build** (`port/plan9/native/demos/`):

| Demo | What it exercises | Result on native 9front |
|---|---|---|
| Recursive **ray tracer** (spheres, lambertian+metal, AA, gamma) | f64 FP, recursion, structs | 320×180 PPM, `checksum=0xca574372fbbe3537` ✓ |
| **SHA-256 + `AutoHashMap`** word-count (120k words) | integer/bitwise, crypto, allocator, hashmap, sort | `sha256=42d64d8e…033140`, `acme:6150` ✓ |

**`zig build` works natively too.** The compiler compiles `build_runner.zig`
in-process with the self-hosted backend, spawns the runner (rfork+exec+pipes),
and the runner drives child `zig build-exe --listen` compiles over a
blocking-read poller — the standard `build.zig` workflow, on the box:

```
cd myproject && zig build -Doptimize=ReleaseSmall && ./zig-out/bin/myprog
```

Fresh build of a hello project ≈ 5-8 min on real hardware (the runner compile
dominates); cached rebuilds ≈ 15 s. Landing this meant fixing a chain of real
bugs the corpus never reached — two allocators sharing one program break (the
root cause of a years-latent heap-corruption family), an uninitialized linker
`bases` on the in-process path, a GOT operand gap in the backend, in-module
compiler-rt with named-symbol resolution in the Plan 9 linker, and cross-dir
cache renames — see `port/plan9/NOTES.md` for the full accounting.

**The linker garbage-collects.** compiler-rt (integer *and* float families —
`f128`/`f80` arithmetic works in target programs) is compiled into every build
and swept per-program: reference edges are recorded during codegen, flush marks
from `_start`, and unreferenced atoms are dropped. hello-world is ~26 KB, not
the ~540 KB of embedded runtime it briefly was. The ~2 MB text ceiling (the
kernel places the data segment at `roundup(text_end, 2MB)`; the linker's data
base is fixed at codegen time) is now a **hard link error** instead of a
silently corrupt binary — and with GC, hard to reach.

**Native limits.** ReleaseFast/ReleaseSmall only (Debug/ReleaseSafe trip a
backend panic + a `std.debug.SelfInfo` gap); no f16 arithmetic or `@cImport` in
target programs; program text ≤ ~2 MB (hard error past it). The compiler frees
memory normally — the years-latent "heap corruption" that once forced a
retain-forever allocator was root-caused to the Plan 9 linker's deinit freeing
`undefined`-initialized fields (deterministic 0xAA under the C backend) plus
two sbrk cursors sharing one program break; both are fixed at the source, and
the allocator's magic-validated headers now convert any future wild free into
a logged diagnostic instead of a fault.

## Why Zig 0.14.1 (and not 0.16)

LLVM cannot emit Plan 9 object files, so Plan 9 depends entirely on Zig's
self-hosted x86_64 backend plus its Plan 9 a.out linker (`src/link/Plan9.zig`).
**That linker backend was removed in Zig 0.15.1** during the "new linker" rework;
0.15.x and 0.16.x return `error.UnsupportedObjectFormat` for the plan9 object
format. **0.14.1 is the newest release that can target 9front at all** (verified:
`src/link/Plan9.zig` present at 0.14.0/0.14.1, 404 at 0.15.1+/master).

## What works

- Integer & floating-point arithmetic; `@sqrt`/`@floor` (via `-mcpu=x86_64_v2`)
- Structs, methods, arrays, slices, tagged unions, optionals, error unions
- Comptime, generics, recursion, bit builtins
- **Heap allocation** (`page_allocator`/sbrk), `ArrayList`, `AutoHashMap`
- `std.mem` (sort, indexOf, split…), `std.fmt`, raw `std.os.plan9` syscalls
- **`!void` main, `@errorName`, `std.testing.expectError`** (error-name table via GOT)
- Correct IEEE FP on bare metal (FP-exception masking — proven necessary on cirno)
- A large fraction of Zig's upstream `test/behavior` suite (1792 tests)

Rule for source that compiles today: build `-OReleaseSmall`/`-OReleaseFast`
(safety-on modes trip a backend codegen bug). The `host/zig9` driver and the test
harnesses bake in the right flags (`-target x86_64-plan9 -mcpu=x86_64_v2
-OReleaseSmall --zig-lib-dir`).

> Zig's **stock `zig test` runner now compiles and runs on 9front** too
> (`std.debug.SelfInfo` routes plan9 through a no-op `Module` — empty backtraces,
> since Plan 9 has no DWARF). The harness defaults to a minimal `--test-runner`
> only because it isolates per-file failures and is faster.

## What does NOT work yet

- ~~Named external symbols / compiler-rt~~ — **DONE** (patches 19-20): compiler-rt
  is compiled into the zcu and libcalls (`__udivti3`, the float families) resolve
  by name through phantom GOT atoms in the Plan 9 linker, garbage-collected
  per-program. `zon` now builds and passes; the only non-N/A unrunnable file left
  is `x86_64` (a register-pressure case in its 16k-line generated math test).
- N/A on Plan 9: `@wasmMemorySize` (no wasm runtime), translate-c `@cImport`
  (no C frontend).
- Threads (`std.Thread.spawn`) — deferred (rfork); `getCurrentId`/`getCpuCount` stubbed.
- (Fixed since: over-alignment via the Plan9 linker, patch 10; the GeneralPurposeAllocator
  via the SbrkAllocator honoring alignment, patch 11; the `mem()` symbol-base keystone
  (`floatop`/`math`) via tracked register materialization, patch 12; **SIMD —
  `@shuffle`/`@select`/`vector` — via UAV natural alignment, patch 13** (the pshufb
  control-mask was under-aligned, faulting legacy SSE); 128-bit symbol-operand
  materialization in mul/div, patch 14.)

See `port/plan9/README.md` → "Still open".

## Build & test

The patched compiler is built in a Linux container (the macOS-26 host can't link
`zig build`; the container sidesteps it). See `port/plan9/linux-build.sh`.

```sh
zig9/fetch.sh                                   # vendor Zig 0.14.1 (src + host toolchain)
sh zig9/port/plan9/apply.sh                     # apply the 14 plan9 patches
sh zig9/port/plan9/linux-build.sh build         # build the patched compiler in the container
python3 zig9/test/run_corpus.py qemu            # 13/13 corpus -> test/parity/manifests/
python3 zig9/test/run_corpus.py cirno           # same, on bare metal
python3 zig9/test/run_behavior.py qemu          # Zig's upstream behavior suite on 9front
```

A quick std-lib-only path (no container, no backend fixes) also works for the
narrower subset that avoids heap/sort/hashmap: `host/zig9 run SRC` cross-compiles
with the prebuilt host toolchain + patched `lib/std` and delivers over listen1.

## Layout

```
zig9/
  fetch.sh                      vendor Zig 0.14.1 (pinned + shasum'd; gitignored)
  host/zig9                     std-lib-only cross/run driver (prebuilt host compiler)
  port/plan9/
    README.md                   the port archaeology: every fix, every blocker, path forward
    NOTES.md                    running log + headline numbers
    linux-build.sh              build the patched compiler in the Linux container
    patches/*.patch (01-14)     the fixes; 01-03,05,07,09,11 lib/std; 04,06,08,10,12,13,14 compiler src
    apply.sh
  test/
    corpus/*.zig                self-checking feature tests (print "ok <name>")
    plan9_test_runner.zig       minimal test runner (raw writes, no @errorName)
    run_corpus.py               corpus harness -> parity manifests
    run_behavior.py             runs Zig's upstream test/behavior suite on 9front
    parity/manifests/*.json     committed scoreboards
  vendor/                       Zig 0.14.1 (gitignored; regenerate with fetch.sh)
```

Delivery reuses `cc9/host/deliver.py` (ships the a.out as a generated C
byte-writer, since listen1 mangles raw binary). cc9 also serves as the
dependency-free **oracle** for 9front's amd64 low-level gotchas.
