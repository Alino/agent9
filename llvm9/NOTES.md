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

## Phase 1 — full JIT lib set through cc9 — **DONE (2026-07-15)**

`host/build-llvm9.py`: compiles all configured /lib TUs from build9's
compile_commands with cc9 (best-effort, -j8, incremental), archives successes
into `_out/libllvm9.a`, reports failures grouped by signature. Strategy: compile
all, let the hello-JIT link's undefined symbols drive which failures matter (the
archive pulls on demand — extra objects are harmless).

**Result: 1931/1938 TUs (99.6%) → libllvm9.a = 157 MB.** The 7 skips are all
irrelevant to an in-process ELF JIT: COFF_x86_64 / COFFDirectiveParser /
COFFLinkGraphBuilder (Windows object format), llvm-dlltool + llvm-lib (tool
drivers), Program.cpp + Signals.cpp (subprocess spawn + crash handlers). No
codegen/JIT wall.

Headers generated first (no full native compile needed): docker `ninja
intrinsics_gen X86CommonTableGen analysis_gen vt_gen target_parser_gen omp_gen
acc_gen` builds llvm-tblgen + emits the .inc/.h. cc9 shim gaps filled on contact
(all trivial): created build9's `VCSRevision.h` (empty; AsmPrinter/IRSymtab),
and cc9/runtime/include `malloc.h`, `spawn.h`, `execinfo.h`, `sys/auxv.h` +
shm_open/shm_unlink decls in `sys/mman.h`; posix_llvm.c gained shm_open/
shm_unlink/getauxval stubs (fail-clean; the in-process JIT never calls them).

## Phase 2 — hello-JIT — **DONE (2026-07-15): `f() = 42` on 9front** 🎉

`test/hellojit.cpp`: MCJIT builds IR for `i32 @f(){ret 42}`, JIT-compiles it with
the X86 backend, calls it. Links libllvm9.a + cc9 runtime → 39 MB a.out → runs on
the wxallow dev VM → prints `LLVM JIT on 9front: f() = 42`. LLVM's codegen+JIT
RUN on 9front. The link-undefined chase (20 → 0) that got us here, all small:
- **RWX memory manager** (the one real runtime piece): a custom RTDyldMemoryManager
  whose allocateCode/DataSection mmap PROT_READ|WRITE|EXEC up front (cc9 routes to
  segattach SG_EXEC) and finalizeMemory is a no-op — sidesteps the mmap(RW)+
  mprotect(RX) that 9front can't do.
- **C files**: LLVM Support has 9 .c TUs (regex, BLAKE3) — driver now compiles .c
  as C (clang, gnu11). BLAKE3 needs `-DBLAKE3_NO_{AVX512,AVX2,SSE41,SSE2}` (portable
  only; the x86 SIMD is hand-written .S we don't build).
- **cc9 shim fills** (Process/Signals/Program needed at link): sys/auxv.h AT_PAGESZ +
  getauxval(AT_PAGESZ)=4096; signal.h stack_t/MINSIGSTKSZ/sigaltstack; malloc.h
  mallinfo/mallinfo2 structs; arc4random (stdlib.h); posix_spawn*/backtrace/
  __register_frame/__deregister_frame stubs (posix_llvm.c) — none called on the JIT
  path. **dlopen(NULL) must return a non-null sentinel** (main-program handle) — MCJIT
  opens it at startup for symbol search and treats null as a hard failure (was the
  final `EE create failed: cc9: no dynamic loading`).

## Phase 3 — **llvmpipe RENDERS ON 9FRONT, pixel-exact (2026-07-15)** 🎉

`02_triangle` under `GALLIUM_DRIVER=llvmpipe` on the wxallow VM: `SIG mean=84,67,8
px=64x64` — and its PPM is **byte-identical (31413 B) to the softpipe run**. LLVM
JIT-compiled shaders produce pixel-exact output vs the reference rasterizer.

Build: `host/mesa-llvmpipe-configure.sh` (meson, llvm=enabled, separate
`gl9/build-gen-llvm`) → `host/build-llvmpipe.py` (917/917 TUs → 31 MB
`gl9/_out/libgl9mesa-llvm.a`) → link with `libllvm9.a` + cc9 runtime → 73 MB a.out.

The four real bugs found (each a durable lesson):
1. **cc9's atexit had a fixed 256 cap and returned -1 when full.** LLVM's
   ManagedStatic/cl::opt register hundreds, so Mesa's
   `if (atexit(destroy_st_manager) != 0) return;` bailed -> `global_fscreen` NULL
   -> fault at `st_api_create_context+0x27` reading `0x28(NULL)`. crt0's table now
   GROWS (realloc). A silent cap is a trap for any LLVM-scale C++ program.
2. **9front can't mprotect-upgrade to exec**, but LLVM's SectionMemoryManager does
   mmap(RW)+mprotect(RX) -> NX fault (`pc==addr`) on the first JIT'd shader call.
   Fixed IN LLVM (`patches/01-memory-rwx-plan9.patch`, guarded by `-DCC9_JIT_RWX`):
   `allocateMappedMemory` ORs in PROT_EXEC up front — one place, every JIT client.
3. **One segattach per exec allocation exhausts Plan 9's small per-proc segment
   table (NSEG)** -> `LLVM ERROR: Unable to allocate section memory!`. cc9's
   posix_llvm.c now segattaches ONE 64MB exec pool and bump-sub-allocates; the
   range check also tells munmap to keep out. (Mesa's rtasm_execmem does the same.)
4. Meson/harvest traps: fake `llvm-config` + `-Dcpp_rtti=false` (our LLVM has no
   RTTI); key objects on meson's **output** path not the source (mapi's entry.c is
   compiled 3x with different -D — keying on source collides them and loses the
   gl* entrypoints); exclude `dummy_errors.c` + glsl `standalone` scaffolding (dup
   `_mesa_*`); remap container paths **inside -D values** (MAPI_ABI_HEADER);
   `-flifetime-dse` is GCC-only; libc++ `-isystem` must precede cc9's C headers.

Known cosmetic: gallivm passes `-avx512er`/`-avx512pf`, removed in LLVM 22 →
"not a recognized feature (ignoring)" noise. Harmless; scrub in util_cpu_caps if
it bothers.

## Next

- **Measure the speedup** (the whole point): llvmpipe vs softpipe on a real
  workload (cube_demo), ideally on bare-metal cirno — TCG timings are noise.
- Wire llvmpipe into gl9's launcher/parity suite + the pac9 package.

## Older next-steps (superseded)

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
