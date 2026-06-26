# cc9/native — toolchain to cross-build LLVM/clang itself with cc9

Plumbing for the native-compiler goal: cross-compile a reduced **clang + lld** to
9front amd64 a.out so a developer can compile C++ *on the box*. This directory is
the CMake glue; the heavy lifting reuses `cc9/host` (elf2aout) and `cc9/lib`.

| File | Role |
|---|---|
| `toolchain.cmake` | CMake toolchain file. `cmake -DCMAKE_TOOLCHAIN_FILE=.../cc9/native/toolchain.cmake`. Wires the compile/link wrappers + llvm-ar/ranlib, forces a **static-library** try-compile (CMake never runs a 9front binary on the host), and sets the executable link rule to `cc9-link`. |
| `cc9-clang` / `cc9-clang++` | Transparent compile wrappers: forward all of CMake's flags to host clang, injecting only the cc9 freestanding target (`--target=x86_64-unknown-none -nostdlib -fno-pic -femulated-tls`, the from-source libc++ headers, `runtime/include`). They do **not** force `-fexceptions/-frtti` — LLVM controls that. |
| `cc9-link` | Executable link rule: `ld.lld` with the cc9 runtime archives + `plan9.ld`, then `elf2aout.py`. One `--start-group` wraps everything (LLVM's static libs are mutually recursive). |

Env: `CC9_LLVM` (brew llvm bin), `CC9_LLD` (ld.lld), `CC9_LIBCXX` (from-source libc++
headers) — same as `host/cc9`.

**Status:** the toolchain is proven on a trivial CMake project (std::vector +
std::accumulate cross-built and run on 9front). Cross-building libLLVMSupport (G1)
is the next gate — see `docs`/the plan.
