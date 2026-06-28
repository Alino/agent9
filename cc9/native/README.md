# cc9/native — the compiler running on 9front

cc9 normally cross-compiles. This directory builds a reduced **clang + `ld.lld`**
*for* the target — with cc9 itself — so the toolchain also runs **on 9front**: a
developer can compile and link C++ on the box, no host involved.

It works. The full chain (clang → ld.lld → `elf2aout`) runs on real hardware, and
**37/37 of the libc++ `.pass.cpp` conformance tests compile and run natively** on
9front.

```text
term% cc hello.cpp -o hello     # clang -> ld.lld -> elf2aout, all on the box
term% hello
hello, 9front
```

## How it's built

The native clang/lld are produced by cross-building LLVM with cc9 (a standard
LLVM cross-build: host tablegen tools first, then the target binaries).

| File | Role |
|---|---|
| `toolchain.cmake` | CMake toolchain file for cross-building LLVM with cc9. Wires the compile/link wrappers + `llvm-ar`/`ranlib`, forces a static-library try-compile (CMake never runs a target binary on the host), and sets the executable link rule to `cc9-link`. |
| `cc9-clang` / `cc9-clang++` | Transparent compile wrappers: forward CMake's flags to host clang, injecting only the cc9 freestanding target + headers. They do not force `-fexceptions`/`-frtti` — LLVM controls that. |
| `cc9-link` | Executable link rule: `ld.lld` with the cc9 runtime archives + `plan9.ld`, then `elf2aout`. One `--start-group` wraps everything (LLVM's static libs are mutually recursive). |
| `build-llvm-support.sh` | Cross-build `libLLVMSupport`/`TargetParser`/`Demangle` (the OS-abstraction layer — the first thing that has to work). |
| `build-clang.sh` | Cross-build the reduced clang (X86 only, threads off, no exceptions/RTTI, Release) → a target a.out. |
| `build-lld.sh` | Cross-build `ld.lld` for the target. (Pass `--mmap-output-file` when linking on the box.) |
| `patches/` | A small LLVM source patch (a Plan 9 fallback in `getMainExecutable`). |

## How it runs on the box

| File | Role |
|---|---|
| `cc.rc` | The on-box `cc` driver. Takes a normal command line and runs clang → ld.lld → `elf2aout` via response files (`@file`), the standard driver pattern. |
| `cc1.template`, `gen-cc1-template.sh` | The `clang -cc1` argument template the driver fills in (include paths, target flags). |

Stage clang, `ld.lld`, `elf2aout`, the driver, the runtime archives, `plan9.ld`,
and the libc++/cc9 header trees onto a 9front box; then `cc src.cpp` produces a
runnable a.out.

## Status

Native compilation of programs works (gates G1–G4: Support runs → `clang
--version` → `-emit-obj` → native compile + link + run, all passed). The native
clang is X86-only and currently staged from `/tmp`. Remaining stretch goals: a
proper install layout and **true self-hosting** — clang rebuilding clang on the
box.

Environment: `CC9_LLVM`, `CC9_LLD`, `CC9_LIBCXX`, `CC9_LLVMSRC` (same as
`host/cc9`).
