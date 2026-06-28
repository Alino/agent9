# zig9 port archaeology

Running log of decisions, obstacles, and fixes porting Zig to 9front/amd64.

## Version pin: Zig 0.14.1 (not 0.16.0)

**Decision (2026-06-28):** Pin to **0.14.1**, the newest Zig release that can
target Plan 9 at all.

The session originally chose 0.16.0 (latest stable), on the assumption — which
turned out to be wrong — that its `x86_64-plan9` target worked. It does not:

- Zig's Plan 9 a.out linker backend lived at `src/link/Plan9.zig`. It was
  **removed in 0.15.1** during the "new linker" rework. Confirmed by file
  presence at release tags: `0.14.1` and `0.14.0` have it (HTTP 200); `0.15.1`,
  `0.15.2`, and `master` do not (404).
- In 0.16.0, `src/link.zig` returns `error.UnsupportedObjectFormat` for the
  `.plan9` object format in both `File.open` and `File.createEmpty`; all other
  `.plan9` switch arms are `unreachable`. There is no backend to call. So 0.16.0
  literally cannot emit a 9front binary — `zig build-exe -target x86_64-plan9`
  fails immediately with `UnsupportedObjectFormat`.

LLVM cannot emit Plan 9 objects, so the self-hosted x86_64 backend + the Plan9
linker are the *only* path. 0.14.1 is the last release where both exist together.

**Implication:** "Faithful port" means newest Zig that actually compiles for
9front. If 0.16+ support is ever wanted, that's a separate, much larger project:
re-implement `src/link/Plan9.zig` against 0.16's reworked linker API and re-wire
the `.plan9` arms in `src/link.zig`. Out of scope here.

## The three known bare-metal bugs (from the cc9 oracle)

cc9 (clang/LLVM/C++ on 9front) already mapped 9front/amd64's low-level gotchas
empirically on real hardware. Zig's plan9 std layer has the same ones. Tracked
in `patches/`:

1. **Syscall clobbers callee-saved regs.** Plan 9 amd64 SYSCALL trashes SysV
   `rbx`/`rbp`/`r13` (cc9 proved on cirno; leaves KZERO-range values). Zig's
   `lib/std/os/plan9/x86_64.zig` syscall asm must list `rbx,r13` (and to be safe
   `r12,r14,r15`) as clobbers. Symptom if wrong: fault at an address starting
   `0xffffffff80…`. Hidden by QEMU.
2. **FP exceptions unmasked.** Bare-metal 9front traps on `x/0`/`0/0`. The plan9
   start path must mask them (MXCSR 0x1F80, x87 CW 0x037F). Hidden by QEMU's TCG.
3. **No `/env`, dir-read + sigaction gaps.** Env is files under `/env`, not a
   stack array.

## Session results (2026-06-28)

The port reached **native compilation of a large Zig subset for 9front, with
Zig's own upstream behavior test suite running on real hardware, plus Zig's stock
test runner.** Patches in `patches/01..09`; apply with `apply.sh`. The
compiler-source patches (04, 06, 08) require rebuilding the compiler — done in a
Linux container (`linux-build.sh`) because `zig build` cannot link on this
macOS-26 host.

### Headline numbers (x86_64-plan9, -mcpu=x86_64_v2 -OReleaseSmall)
- **Corpus: 13/13** self-checking feature tests pass, **identical on QEMU and
  cirno bare metal** (`test/parity/manifests/{qemu,cirno}.json`). Covers int/FP
  arithmetic, structs, tagged unions, generics, comptime, error unions, **heap
  allocation, `std.mem.sort`, `std.AutoHashMap`, `ArrayList`, `std.fmt`**.
- **Upstream behavior suite: 1773 of Zig's own `test/behavior/*.zig` tests pass**
  on 9front — full QEMU run **1773 pass / 0 fail / 291 skip across 115/119 files**
  that compile+run, **0 crashes** (`test/parity/manifests/behavior-qemu.json`);
  **100% of the tests that run.** Top files: `cast` 126, `eval` 107, `union` 101,
  `array` 66, `error` 64. Run via a minimal plan9 test runner
  (`test/plan9_test_runner.zig`), compiling each file through a `test/`-level root
  that *imports* `behavior/<file>.zig` (so `@typeName` yields `behavior.<file>.X`
  like the upstream aggregate — this turned 7 earlier `@typeName`/`string_literals`
  "failures" into passes; they were harness artifacts, not plan9 bugs). **The last
  3 failures — alignment edge cases in `align.zig` (`align(128)` local const,
  `align(4)` global, `align(0x1000)` function) — are now fixed by patch 10** (the
  Plan9 linker honors atom alignment; see below). **`basic` (+97) and `var_args`
  (+11) now run too** — they weren't a codegen bug at all but the SbrkAllocator not
  honoring alignment, which corrupted the GeneralPurposeAllocator (`std.testing.
  allocator`); fixed by patch 11. **`floatop` (+25) and `math` (+50) now run too —
  the `mem()` keystone is fixed (patch 12)**: a symbol const used as a memory-operand
  base is now materialized into a register via *tracked* `toBase` (the prior two
  attempts used untracked `allocReg(null)` and miscompiled). **`select`/`shuffle`/
  `vector` (+36) now run too — and crashes hit 0** — they weren't upstream-incomplete
  vector codegen but a plan9 bug: the linker under-aligned anonymous const data, so
  a 16-byte `@shuffle` pshufb mask / spilled `@Vector` used as a legacy-SSE memory
  operand GP-faulted; fixed by patch 13 (`lowerUav` floors alignment at the value's
  natural ABI alignment). The remaining 4 unrunnable files are all compile-fail: the
  named-external / compiler-rt-extern feature (`x86_64`'s u128 ops + `zon`'s
  `format_float`, 2) and genuinely-N/A builtins (`@wasmMemorySize`, translate-c
  `@cImport`, 2). The trajectory this session:
  **9/13-blocked → 752 → 883 → 1163 → 1404 → 1440 → 1446 → 1551 (stock runner +
  struct) → 1554, 0 fail (linker alignment, patch 10) → 1662 (GPA/sbrk alignment,
  patch 11) → 1737 (mem() symbol-base keystone, patch 12) → 1773, 0 crashes
  (UAV natural alignment, patch 13)** as each bug was fixed.

### What the 163 skips actually are (faithfulness check, 2026-06-28)
Audited the skip count against the test source — **the 163 skips are the Zig
suite skipping *itself*, not gaps this port introduced.** Evidence:
- **Zero plan9-specific skips.** `grep -i plan9 test/behavior/*.zig | grep skip`
  = 0. Not one `os.tag == .plan9 -> SkipZigTest` gate exists; the port adds no
  skip logic and hides no plan9 failure behind a skip.
- Every skip is an **upstream-authored `error.SkipZigTest`** with an OS-agnostic
  gate. The 4 897 skip sites in the suite break down as:
  - `if (true) ... // TODO` — unconditional (e.g. **all 65 of `async_fn`**; async
    is shelved in Zig itself). Skips on Linux/macOS too.
  - `zig_backend == .stage2_x86_64 and ofmt != .elf and .macho` (143 sites) and
    bare `stage2_x86_64` (52) — **self-hosted-x86_64-backend maturity gates**.
    plan9 is *forced* onto that backend (LLVM can't emit plan9), so it correctly
    hits these — exactly as any non-ELF/Mach-O self-hosted target does.
  - The rest gate other backends (`stage2_aarch64/arm/riscv64/wasm/c/sparc64...`),
    `os.tag == .wasi`, or endianness — none reachable/relevant on x86_64-plan9.
- **Conclusion:** a faithful port that honestly runs the suite on the self-hosted
  backend *must* report these skips. They are the suite's own TODO markers and
  backend-feature gates, identical in kind to an x86_64-linux self-hosted run.
  The honest denominator is "tests the suite considers runnable on this backend,"
  and among those the port passes **1551/1554 (99.8%)** — the 3 misses being the
  documented `align` edge cases.

### The fixes that got us here (beyond the original 3 std patches)
4. **`store: [direct:N]` backend panic (patch 04, compiler).** The self-hosted
   x86_64 backend's `Temp.store` switch handled `.lea_symbol`/`.load_symbol` but
   not the non-PIC `.lea_direct`/`.load_direct` that plan9 uses for globals →
   storing a value living at a direct symbol address hit `else => panic`. Added
   them alongside their symbol siblings. Unblocked sort/strings/hashmap.
5. **Syscall clobbered the global-base register (patch 01, rewrite).** The real
   killer for heap: the self-hosted backend keeps the base register it uses to
   reach globals/externs in **rbp** across calls; the kernel SYSCALL trashes rbp
   (it carries the syscall number). A clobber list did NOT save it. Fix: pass the
   syscall number in **rdx**, move it to rbp *inside* the asm, and **explicitly
   push/pop rbp + rbx + r12-r15** around the syscall (cc9 n9syscall.s style), so
   the compiler keeps the global base in rbp and the asm protects it. This is
   what made `&end`/`sbrk`/all heap survive a syscall. Pure bare-metal bug —
   QEMU preserved the registers and hid it.
6. **std.posix plan9 stubs (patch 05, lib).** `isatty`, `timespec`/`timeval`/…,
   `ucontext_t`, `dl_phdr_info` so the large fraction of std (and the behavior
   tests) that transitively imports `std.posix` compiles. `isatty` alone
   unblocked ~9 behavior files (e.g. array=66, slice=53 tests).

7. **`@errorName` / lazy symbols (patches 04 + 06).** plan9's lazy-symbol support
   lived only in the backend's `genSetReg` GOT path (`CodeGen.zig` ~96301,
   ds-relative), not the newer `Select.Operand` path — so `@errorName`,
   `std.testing.expectError`, and `!void` main failed with `external symbols
   unimplemented`. Fix (patch 04): in the `.lazy_symbol` operand resolver, for
   plan9 materialize the address into a register via `genLazySymbolRef` (the GOT
   path) and return a plain register operand. That then exposed a **latent linker
   bug** (patch 06): `Plan9.zig` flush looked up the deferred `anyerror`
   error-name table under key `.none`, but `getOrCreateAtomForLazySymbol` stores
   it under `.anyerror_type` (the ELF backend uses `.anyerror_type`) — so its code
   was never generated (`getOwnedCode().?` null). One-word fix. Together these
   made **`!void main works`** and took the behavior suite 883 → **1163** (+280).
8. **`call r32` encoder bug (patch 08, `Lower.zig`).** The `.code` lazy-symbol
   Select patterns (~849 of them) reference the call target as `.tmpNd` (dword) —
   for elf that lowers to a `call rel32` immediate, but plan9 materializes the
   lazy symbol into a register, so it became `call r32`, which has no 64-bit
   encoding. Fix: in `Lower.emit`, promote a register call/jmp target to 64-bit
   (no-op for elf/macho — their target is an immediate). Unblocked enum/union/fmt;
   **1163 → 1404** (+241).
9. **`getLimb: [direct:N]` big-int gap (patch 04).** `getLimb` handled
   `.load_symbol`/`.lea_symbol` but not plan9's `.load_direct`/`.lea_direct`.
   Added them (materialize the symbol address via `genSetReg(.lea_direct)`, then
   load the limb at `+limb_index*8`). Unblocked `for`/`memcpy` (+36).

10. **Linker ignored atom alignment (patch 10, `Plan9.zig`) — closed the last 3
    behavior failures, 1551→1554, 0 fail.** `flushModule` wrote every text/data
    atom back-to-back (`text_i/data_i += code.len`) with no alignment padding, and
    `lowerUav` literally `_ = explicit_alignment`. So `align(N)` was silently
    dropped: a `const x align(128)` (a UAV), a global `foo: u8 align(4)`, and an
    `align(0x1000)` function all landed at unaligned addresses — the 3 `align.zig`
    fails. Fix: a `padSegment` helper inserts a zero-fill padding iovec before each
    atom so `base + seg_i` hits the required boundary (`bases.text`=0x200028 isn't
    page-aligned, so the pad is computed on the *absolute* vaddr, not the segment
    offset); function/data-nav alignment comes from `pt.navAlignment`, and UAV
    alignment is captured into a new `uav_alignment` side-map in `lowerUav`.
    Subtleties: `pwritevAll` writes the **whole** `iovecs` slice (not `[0..i]`), so
    the array is grown to `2*atomCount()+4` and unused tail slots are filled with
    zero-length no-op iovecs; the exact-count `assert` gains `+ n_pad`. `.none`
    alignment ⇒ no padding, so every non-explicitly-aligned atom keeps its old
    byte layout — which is *why the corpus stayed 13/13 and the other 105 files
    were untouched*. Verified: `align.zig` 24/3 → **27/0**, corpus **13/13 on QEMU
    *and* cirno bare metal** (a layout desync would crash every binary loudly —
    this is the property that made the fix safe to land autonomously, unlike the
    `mem()` gap whose miscompiles are silent).

11. **SbrkAllocator ignored alignment → GPA `@panic("Invalid free")` (patch 11,
    lib) — unblocked `basic` (+97) and `var_args` (+11), 1554→1662.** Both files
    "compiled but crashed"; the crash looked like a codegen bug but wasn't. Traced
    it by bisecting `basic` with a progress-printing runner (died in test
    *"allocation and looping over 3-byte integer"*), then instrumenting
    `DebugAllocator.free`: it `@panic`s on a canary mismatch. Root cause: on plan9
    `std.heap.page_allocator` is `SbrkAllocator(plan9.sbrk)`, and its
    `allocBigPages` returns `sbrk(...)` verbatim — only page-aligned. But the
    `DebugAllocator` (= `std.testing.allocator`) uses `default_page_size` = 128 KiB
    and recovers a slot's base on free by masking `addr & ~(page_size-1)`, which
    assumes the backing block is `page_size`-aligned. sbrk's 4 KiB alignment broke
    that → wrong bucket header → bad canary → panic (which aborts under the
    void-main runner, hence the silent "crash"). Fix: `allocBigPages` burns the
    break up to `slot_size_bytes` (the slot's natural power-of-two size) before
    claiming it, so big slots — and the small buckets tiled inside them — are
    naturally aligned. `u24` codegen, `page_allocator.free`/munmap, and
    `captureStackTrace` were all ruled out first (each tested in isolation).
    lib-only (no compiler rebuild). Verified: `basic` 97/0, `var_args` 11/0 on QEMU
    **and** cirno; corpus 13/13 (no regression). Upstreamable — a real plan9 bug in
    Zig's own SbrkAllocator that breaks any GeneralPurposeAllocator use.

### Default test runner — WORKS (patches 05/07/09)
**Zig's stock `zig test` runner now compiles AND runs on 9front** — e.g.
`behavior/bool.zig` prints `1/6 ... OK ... All 6 tests passed.` The keystone was
**patch 09**: `std.debug.SelfInfo` routes plan9 through a **no-op `Module`** (and
`getModuleForAddress` returns `error.MissingDebugInfo`), so the ELF/DWARF debug
reader (`lookupModuleDl → readElfDebugInfo → mmap`) becomes **dead code** for
plan9 — which *also* made the earlier `Stat`/`open()`/`MAP.TYPE` errors vanish
(they were only referenced *inside* that reader). Backtraces are empty (Plan 9
has no DWARF; it debugs with acid). Plus patch 05's `MSF`/`munmap`/`msync` stubs.
Patch 07 contributed `isatty`/`ino_t`/`writev`/`selfExePath`/`flock`/`PROT`/the
`os_flags` arm. This also unblocked behavior `struct` (105 tests). The custom
`--test-runner` remains the default in the harness (it isolates per-file failures
and is faster), but the stock runner is now a working option.

### The `mem()` symbol-base keystone — FIXED (patch 12)
**The single highest-leverage backend gap, fixed 2026-06-28 (1662→1737).**
`MCValue.mem()` (`CodeGen.zig:483`) is `unreachable` for plan9's `.load_direct`/
`.lea_direct`, so any instruction using a symbol const as a **memory-operand base**
(e.g. `@abs`'s SSE sign-mask, FP/int lookup tables) faulted the *compiler* with
"reached unreachable". plan9 is non-PIC: there's no RIP-relative `.reloc` base like
ELF's `.load_symbol`, so the symbol's address must live in a register.

The fix (patch 12, one hunk at the `.mem` `lower()` case): when the base resolves
to `.load_direct`/`.lea_direct` on plan9, materialize it via **`temp.toBase()`**,
which uses a **tracked** `allocReg(temp_index,…)`+`genSetReg` and rewrites the temp
to `.indirect`; `valueOf` then sees a register base and `mem()` lowers it. Diagnosed
precisely first (diagnostic build): the offending base is a **`tmp` temp set to
`.load_direct` mid-body**, so a pre-emit pass can't catch it — but doing it at
`lower()` *with the tracked `toBase`* works, because the register manager knows the
scratch is live (it can't collide). **This is why the two earlier attempts failed:
they used untracked `allocReg(null)`, which grabbed a live operand reg → runtime
GP-fault.** Verified: `floatop` 25/0 and `math` 50/0 — their float-result asserts
*pass* (a miscompile would trip them) — on QEMU **and cirno bare metal**; full
suite **1737/0**, corpus **13/13**, **zero regressions** (the change is
plan9-guarded and only fires for symbol `.mem` bases). `x86_64` advanced *past*
this to the compiler-rt extern gap below.

### SIMD/vector — FIXED (patch 13), was an alignment bug, NOT upstream-incomplete
**`select`/`shuffle`/`vector` (+36, and crashes → 0), fixed 2026-06-28.** I first
mis-judged these as "genuine upstream vector-codegen gaps" — wrong. Instrumented
`@shuffle` to the exact op: it *compiles* but hard-faults at runtime *inside the
shuffle* (not a `mem()` compile error). Root cause: the general shuffle path uses a
16-byte `pshufb` control-mask constant (and spilled `@Vector`s), and a **legacy
(non-VEX) SSE memory operand GP-faults if unaligned**. `Plan9.lowerUav` stored
`explicit_alignment orelse 1` — but auto-generated consts arrive with
`explicit_alignment == .none`, so the mask landed 1-aligned. Fix (patch 13): floor
the stored alignment at the value's natural ABI alignment
(`uav_ty.abiAlignment(zcu)`), so a 16-byte vector const is 16-aligned. Verified:
`vector` 34/0, `shuffle` 1/0, `select` 1/0 — their result asserts *pass* — on QEMU
**and cirno**; full suite **1773/0, 0 crashes**, corpus 13/13, zero regressions.
Lesson: "looks like upstream incompleteness" was, again, a plan9-specific alignment
bug (cf. patches 10/11) — worth instrumenting before writing something off.
- **Named external symbols / compiler-rt** (`zon` → `format_float`'s u128 `/`,`%`
  = `__udivti3`/`__umodti3`; and `import_c_keywords` → any extern call). CONFIRMED
  (2026-06-28) to be a *substantial unimplemented feature*, not a one-liner:
  plan9 has **no `getGlobalSymbol`** (0 in `Plan9.zig`), `genExternSymbolRef`
  (`CodeGen.zig:96264`) is **COFF-only** → `fail("TODO implement calling extern
  functions")`, the `.symbol` Select operand fails at `CodeGen.zig:107101`
  ("external symbols unimplemented for plan9"), and `Plan9.getDeclVAddr` has
  "TODO handle other extern variables and functions". There is **nothing to
  resolve against** — compiler-rt is built as a separate `compiler_rt_obj`/`_lib`
  and **linked by lld** for normal targets (`link.zig:1114-1198`); plan9's
  self-hosted linker links no objects. And crucially **no Zig backend resolves a
  libcall to a nav** — LLVM/C/all emit an external symbol + rely on object linking
  — so plan9 would be the *first* to need a libcall→nav path (look up `__udivti3`
  in the imported compiler-rt module, force-analyze it as a nav, emit a nav call).
  This is the one remaining gap that **doesn't reuse existing machinery** (unlike
  patches 12/13, which leaned on `toBase`/`abiAlignment`): it's a from-scratch
  feature build with uncertain feasibility — deferred rather than risk the verified
  1773. (`@floor`/`@sqrt` sidestep this via `-mcpu=x86_64_v2`; only 128-bit int
  division and a few f16/f128 ops actually need compiler-rt.)
- `@wasmMemorySize` (wasm.zig) and translate-c `@cImport` (import_c_keywords) are
  genuinely N/A for plan9 (no wasm runtime, no C frontend).

### Zig's std-lib test suite on plan9 (investigated)
Beyond `test/behavior`, Zig's std modules carry `test{}` blocks. Running them on
plan9 is awkward: Zig only collects tests from the **root** module, so a std module
must *be* the root — but then `--zig-lib-dir <patched tree>` double-includes it
("file exists in multiple modules"), and *without* `--zig-lib-dir` the minimal
runner pulls the **unpatched** host plan9 layer (broken syscalls). On top of that,
the pure modules that would otherwise run (`math`/`sort`/`base64`) hit the `mem()`
gap above, and many others need fs/net/threads. `std.hash.wyhash` compiles. A
clean std-suite run needs the proper module config **and** the `mem()` fix.

### Discovery findings worth keeping
- The prebuilt host compiler uses its **own** bundled `vendor/zig-host/lib/std`;
  our patches live in `vendor/zig/lib` → always pass `--zig-lib-dir vendor/zig/lib`.
  A compiler-source (src/) patch needs a full rebuild; lib patches do not.
- `zig build` can't link on macOS 26 with 0.14.1 (libSystem stubs too old;
  `--sysroot <old SDK>` fixes a direct `build-exe` but `zig build` doesn't forward
  it). The Linux container (`linux-build.sh`) sidesteps it entirely.
- `*m` is not a valid Zig 0.14 asm constraint (use `"m"`).
- Plan 9 amd64 a.out magic Zig emits is `0x00008A97` (amd64 + HDR_MAGIC), and the
  backend folds bss into data (header bss=0), so `end`==`edata` is correct.
- The self-hosted backend's safety-mode codegen also panics, so plan9 needs
  `-OReleaseSmall`/`-OReleaseFast`; `-mcpu=x86_64_v2` gives `roundsd` so
  `@floor`/`@ceil` need no libm extern.
