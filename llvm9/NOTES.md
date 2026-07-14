# llvm9 — LLVM's codegen/JIT running natively on 9front (for llvmpipe)

Goal: get LLVM's ORC/MCJIT + X86 backend to **run on 9front** (cross-built by cc9),
so Mesa's **llvmpipe** (JIT-vectorized software rasterizer) can replace softpipe —
the CPU path to fast OpenGL without a GPU driver. Prereq W^X JIT memory is already
proven (segattach SG_EXEC + wxallow kernel; see cc9 + gl9 work).

## Phase 0 — go/no-go: does cc9 digest LLVM's C++? — **GREEN (2026-07-15)**

Smallest decisive experiment before committing to the full multi-session build.

- `host/Dockerfile` + `host/configure.sh`: linux/amd64 container (arch defines match
  cc9's x86_64-unknown-none target), cmake-configure LLVM **X86-only, minimal**
  (no tools/utils/tests, ORC on, `EXPORT_COMPILE_COMMANDS`) into `$LLVMSRC/build9`,
  then native `ninja LLVMSupport LLVMMC` — generates the tablegen/config headers +
  `compile_commands.json`, and proves upstream builds clean (349/349, both .a linked).
- `host/cc9-try.py`: takes build9's compile_commands, recompiles a sample of
  lib/Support + lib/MC TUs with cc9's target + freestanding libc++ (`/tmp/libcxx-thr`)
  + cc9 libc headers (the gl9 harvest pattern: remap /work→host, drop glibc/libstdc++
  -D and host -f flags), compile-only. **Result: 22/22 clean**, including the hairy
  ones — APFloat/APInt (bit-twiddling templates), JSON/YAMLParser (heavy STL),
  Path.cpp (POSIX fs, satisfied by cc9/runtime/include), and MC's MCAssembler/
  MCStreamer/MCObjectStreamer/ELFObjectWriter/MCSymbolELF (the in-memory object
  emission the JIT relies on). No toolchain wall.

Prereq restored on the way: `/tmp/libcxx-thr` (freestanding libc++ headers) is
ephemeral and was gone — `cc9/host/regen-libcxx.sh` rebuilds it.

## Next (the grind, not a wall)

1. Widen the cc9 build to the full JIT lib set: Core, CodeGen, X86 target
   (X86CodeGen/AsmPrinter/Desc/Info), ExecutionEngine, ORC + JITLink (or RuntimeDyld),
   Analysis, TransformUtils, Object, BinaryFormat, TargetParser, Demangle, IRReader.
   Expect a long tail of cc9 POSIX-shim fills (like gl9's) — volume, per Phase 0.
2. Host-detection: LLVM's getHostCPUName/Features reads /proc/cpuinfo — hardcode the
   target CPU (Broadwell/generic x86-64-v2) for 9front.
3. **RWX memory manager** (the one genuinely new runtime piece): LLVM's default JIT
   does mmap(RW)+mprotect(RX); 9front can't upgrade malloc'd memory to exec. Point
   ORC's memory mapper at mmap(PROT_EXEC) upfront → cc9 routes it to segattach(SG_EXEC)
   (already wired in cc9/runtime/posix_llvm.c for the Mesa SSE win).
4. Hello-JIT (~40 lines, LLJIT: build `int f(){return 42;}`, JIT, call) → link with
   cc9 runtime → run on the wxallow VM → expect 42. That's the spike's end.
5. Then: `-Dgallium-drivers=llvmpipe -Dllvm=enabled` in the gl9 Mesa build, gallivm
   over this LLVM, parity vs softpipe goldens, measure the speedup.
