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
- **Upstream behavior suite: 1773 of Zig's own `test/behavior/*.zig` tests pass**
  on 9front (`test/parity/manifests/behavior-qemu.json`), via a minimal plan9
  test runner — **0 failures, 0 crashes: 100% of the tests the suite considers
  runnable on the self-hosted x86_64 backend.** The 291 "skips" are the suite skipping
  *itself* (`error.SkipZigTest`: TODO markers + self-hosted-backend feature
  gates); **zero are plan9-specific** — this port adds no skip gate and hides no
  failure behind one. See `port/plan9/NOTES.md`.

Reaching this required, beyond a working cross-compiler, **rebuilding the Zig
compiler from patched source** to fix real self-hosted-backend bugs in its
experimental plan9 target (see `port/plan9/README.md`).

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
- A large fraction of Zig's upstream `test/behavior` suite (1773 tests)

Rule for source that compiles today: build `-OReleaseSmall`/`-OReleaseFast`
(safety-on modes trip a backend codegen bug). The `host/zig9` driver and the test
harnesses bake in the right flags (`-target x86_64-plan9 -mcpu=x86_64_v2
-OReleaseSmall --zig-lib-dir`).

> Zig's **stock `zig test` runner now compiles and runs on 9front** too
> (`std.debug.SelfInfo` routes plan9 through a no-op `Module` — empty backtraces,
> since Plan 9 has no DWARF). The harness defaults to a minimal `--test-runner`
> only because it isolates per-file failures and is faster.

## What does NOT work yet

- Named external symbols / compiler-rt (`x86_64`'s u128 division, `zon`'s
  `format_float`) — the only 2 unrunnable files that aren't platform-N/A. The
  *codegen* half is now done (patch 14 materializes 128-bit symbol operands); what
  remains is a backend↔linker **name→export reloc** so libcalls (`__udivti3`) resolve
  against the in-module compiler_rt (`.zcu` strat). See `port/plan9/NOTES.md`.
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
