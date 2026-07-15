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
  **Investigated to confirm intractability (not assumed):** there is no name→nav
  lookup to even start from — exports live in `Zcu.single_exports`/`multi_exports`
  keyed by `AnalUnit` and are populated only *after* a nav is analyzed, and
  compiler-rt fns are analyzed lazily (a `.lib` call creates no dependency, so
  `__udivti3` is never analyzed); `getBuiltin`-style module-decl-by-name lookup
  doesn't exist here. So this spans force-analysis-by-name + a name→nav map +
  linker registration — genuinely new infra, unlike the 3 gaps this session that
  *looked* upstream-incomplete but were one-mechanism plan9 bugs (alloc/mem/align).
  **Most decisive (`Compilation.zig`):** `canBuildLibCompilerRt` returns **`false`
  for plan9** outright — compiler_rt is never built, so there is no object and no
  navs. The only in-module alternative is the `.zcu` `RtStrat`, which
  `@import("compiler_rt")` and **force-compiles *all* of it** (incl. f16/f128
  softfloat) into the program; on plan9 that risks tripping still-unimplemented
  codegen paths and **breaking the verified build** for a 2-file payoff. The
  next-session path, if attempted: flip plan9 to `.zcu` strat, confirm compiler_rt
  even *compiles* for plan9, then add a name→nav-export resolution in
  `genExternSymbolRef` + `Plan9` flush. Real work, real risk — not a blind change.
  **TRIED IT (2026-06-28):** flipped plan9 to `.zcu` strat — it force-compiles all
  of compiler_rt into every program and broke the build (even `01_arith`):
  `genMulDivBinOp` (`CodeGen.zig:89364`) does `mat_rhs_mcv.register_pair[1]` after a
  switch that materializes `.load_symbol` but not plan9's `.load_direct`, so a
  128-bit const operand panicked — same class as the keystone. **The codegen cascade
  turned out BOUNDED: exactly 9 `.load_symbol`-materialize sites, all the same
  `copyToTmpRegister(.usize, X.address())` idiom — now fixed (patch 14), verified
  non-regressing (1773/0, corpus 13/13). So the codegen half of compiler-rt is
  DONE.** With the cascade fixed, the `.zcu` build advanced *past* every codegen
  panic to "external symbols unimplemented" — i.e. the *only* thing left is
  **symbol resolution**: compiler_rt fns call each other (`__muloti4`→`__multi3`…)
  via the `.symbol`/`.lib` extern path, which plan9 doesn't resolve. The design is
  now clear and the scaffolding exists: `.zcu` strat makes those fns navs that
  `addNavExports` already turns into **named symbols with addresses in `self.syms`**
  at flush; the missing piece is a backend→linker **by-name reloc** — add an
  `extern_named` `Reloc` type carrying the name, have plan9 `genExternSymbolRef`/the
  `.symbol` operand emit it, and resolve it in the flush reloc loop against the
  export syms (which are all present by then). That's the bounded remaining work
  (one `Reloc` variant + ~2 backend arms + ~1 flush lookup) — `.zcu` strat reverted
  for now so the committed compiler stays the verified 1773, but patch 14 (the
  cascade) is kept because it's a correct keystone-class fix on its own.
  **The one genuinely-hard part (traced):** the backend references an extern via
  `asmImmediate(.{._,.call}, .rel(.{ .sym_index = … }))` (ELF/MachO) — the `.rel`
  Immediate + Emit reloc machinery is **sym_index-centric for every target**, so
  plan9 needs a *new* by-name path threaded through `Immediate`→`Emit`→`addReloc`
  (the `.symbol` operand at 107101 has the same shape). Not a one-liner: it's the
  emit-layer plumbing, which is why this is the first gap this session that's real
  multi-layer infra rather than a single-mechanism bug. Plan9's nav calls already
  go through `call qword ptr ds:[GOT]` (`getOffsetTableAddress`), so an alternative
  is a name→GOT-slot resolved at flush — but the `got_len == atomCount` assert
  (Plan9 flush) means extern GOT slots need accounting for.
  **IMPLEMENTED IT — and it WORKS (2026-06-28).** Took the GOT-slot route (cleaner
  than the by-name reloc — no Emit machinery needed, since plan9 already calls navs
  via `call ds:[GOT]` at a codegen-time-known address). Added: `extern_got` map +
  `getExternGotAddr` (name→GOT slot, vaddr known at codegen) in `Plan9.zig`; a flush
  pass that fills each slot from the like-named export sym `addNavExports` creates;
  the `got_len` assert += `extern_got.count()`; and plan9 arms in *both*
  `genExternSymbolRef` (`.lib` calls → `call ds:[GOT]`) and the `.symbol` Select
  operand (107111 → `mov reg, ds:[GOT]`). With `.zcu` strat on, **every "external
  symbols unimplemented" error disappeared** — compiler_rt's internal libcalls
  (`__muloti4`→`__multi3`, …) all resolve. **So the plan9 *port-side* compiler-rt
  work is essentially done.** The new (and now the *only*) blocker is **upstream**:
  `.zcu` force-compiles ALL of compiler_rt, and the f16 softfloat functions
  (`__powihf2`, `__mulhc3`) hit **"ran out of registers (Zig compiler bug)"** — the
  self-hosted x86_64 backend's own register-allocator limitation (the same
  incompleteness behind the suite's `stage2_x86_64` self-skips), not a plan9 issue.
  Because `.zcu` is all-or-nothing (and without it `genExternSymbolRef` would turn
  honest compile-errors into runtime faults), it can't ship until either Zig fixes
  that backend limit or compiler_rt is made to skip the f16 fns on plan9. **The
  proven implementation is saved at `port/plan9/wip-compiler-rt-symbol-resolution.patch`**
  (not in `apply.sh` — inert without `.zcu`); reverted from the tree so the committed
  compiler stays the verified 1773. This is the real frontier now: a Zig-backend
  limitation, not missing plan9 infrastructure.
  **Went further (2026-06-28) — gated the f16/f80/f128 softfloat block out of
  `lib/compiler_rt.zig` for plan9** (a legitimate "feature deferred" like threads;
  f32/f64 use SSE, softfloat tests self-skip). The WIP now carries this 4th file.
  With float gated + `.zcu` + the symbol resolution, **compiler_rt's integer routines
  compile, resolve, and RUN CORRECTLY** — `01_arith` (which now force-compiles
  compiler_rt) prints `ok`. So the whole port-side compiler-rt path is proven
  end-to-end. **Yet the two target files still don't pass, for reasons past the port:**
  (1) `x86_64` hits **"ran out of registers"** in its *own* 16 205-line test function
  (`x86_64/math.zig`), nothing to do with compiler_rt — an upstream backend limit on
  the test itself; (2) `zon` finally **compiles** (its `__udivti3` libcall resolves!)
  but **crashes at runtime** — a deeper `__udivti3`/u128-division codegen bug that
  needs the trap PC to chase. And `.zcu` force-compiles compiler_rt into *every*
  binary (bigger/slower), with no suite gain, so it's not worth shipping. Reverted
  to the clean 1773; WIP (now 4 files) preserves the complete, proven approach.
  **Chased `zon`'s crash to the end (2026-06-28):** isolated u128 division
  (`__udivti3`/`__umodti3`) in a standalone test — it **compiles, resolves, and
  computes the correct quotient/remainder** (`ok zz_u128`). So u128 division is NOT
  the problem. Bisected `zon`: it dies in its **"floats" test**, whose `T` struct
  is literally `.{ f16, f32, f64, f128, … }` — it needs **f16 and f128 softfloat**,
  which is the *same* upstream register-exhaustion limit (gated → missing fn → GOT
  slot 0 → fault; un-gated → won't compile). **So both remaining "real" files are
  the identical upstream blocker:** `x86_64` and `zon` each need f16/f80/f128
  softfloat the self-hosted backend can't compile. **Conclusion: there is NO
  remaining plan9 port bug.** The port-side compiler-rt path is proven end-to-end
  (integer incl. u128 works correctly); every still-unrunnable file is the suite's
  own self-skips, Plan 9's nature, or Zig's own self-hosted-backend incompleteness
  on softfloat — none is missing plan9 infrastructure.
  **Mode-independence confirmed by test (2026-06-28):** `OutOfRegisters` is raised by
  the general `RegisterManager` (`CodeGen.zig:1001`, no plan9 gating). Built compiler_rt
  un-gated and compiled the same program in **both `-OReleaseSmall` and `-OReleaseFast`
  — each hits "ran out of registers" exactly 4×, identically.** So no optimization mode
  dodges it; it's fundamental to the self-hosted allocator on these softfloat fns,
  target- and mode-independent. (Also: `-OReleaseFast` works on plan9 just like
  `-OReleaseSmall`; `-ODebug`/`-OReleaseSafe` additionally pull in `ubsan-rt`, whose
  externs would need the same `extern_got` resolution.)
- `@wasmMemorySize` (wasm.zig) and translate-c `@cImport` (import_c_keywords) are
  genuinely N/A for plan9 (no wasm runtime, no C frontend).

### Zig's std-lib test suite on plan9 (investigated)
Beyond `test/behavior`, Zig's std modules carry `test{}` blocks. **Re-checked
2026-06-28 (post-patch-12/14):** a root that references the pure modules
(`comptime { _ = std.mem; _ = std.sort; _ = std.ascii; _ = std.fmt; _ = std.math;
_ = std.unicode; _ = std.base64; _ = std.hash; _ = std.meta; _ = std.enums; }`)
**now compiles clean for plan9** — the `mem()` gap that used to block math/sort/
base64 is fixed. But **0 tests run**: Zig only collects `test{}` blocks from the
**root module**, and these are in the separate `std` module, so referencing them
doesn't include their tests. To actually *run* std's tests, a std file must *be*
the root module — which (a) double-includes via `--zig-lib-dir <patched tree>`
("file exists in multiple modules"), (b) without `--zig-lib-dir` pulls the
**unpatched** host plan9 layer, and (c) the minimal runner itself `@import("std")`,
re-introducing the conflict. So a std-unit-test run on plan9 is its *own*
infrastructure project (a runner that doesn't import std + correct module wiring),
on top of the unsupported transitive deps (threads/DWARF/fs) and `format_float`'s
compiler-rt. The pure stdlib is already validated two ways though: it **compiles**
for plan9, and `test/behavior` exercises `std.mem`/`fmt`/`sort`/`AutoHashMap`/… at
runtime (1773 tests). Running std's *own* blocks is the remaining, separate suite.

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

## Native compiler ON 9front — the CBE + cc9 route (2026-07-15, WIP)

Goal: run `zig` itself on 9front, not just cross-compile to it. The self-hosted
backend can't compile the compiler to a.out (its own f16/f128 softfloat +
u128/comptime-float paths hit "ran out of registers"), so this takes Zig's
**official CBE bootstrap**: emit the whole compiler as C, compile that with cc9.

### Pipeline (built, works end-to-end for small programs)
`port/plan9/native/build.py`:
1. host `vendor/zig-host/zig build-exe -ofmt=c -OReleaseSmall -fsingle-threaded
   -target x86_64-plan9` over the patched tree → **zig2.c (~213 MB)** +
   `compiler_rt.c`. (`build-obj` for compiler_rt.) config in `native/config.zig`
   (= bootstrap.c's, `dev=.core`, `have_llvm=false`, + `mem_leak_frames=0`).
2. cc9 clang (`--target=x86_64-unknown-none -nostdlib -femulated-tls -fno-pic
   -mno-red-zone -D__plan9__ -isystem cc9/runtime/include -I vendor/zig/lib`) →
   objects; `-O0` for zig2.c (**-O1/-O2 OOM host clang on the 213 MB TU**; -O0 =
   19 s, 3.3 GB, 64 MB .o). ld.lld + `cc9/test/plan9.ld` + libcc9cxx/libcc9m +
   `native/zig9syscall.s` (generic SysV→Plan9 thunk) + `native/zig9compat.c`
   (`zig9_tos()` accessor) → elf2aout → **zig9.aout (~56 MB)**.
3. Ship: `build.py --libtar` (lib/ minus libc/libcxx/…, ~5 MB gz) → `/sys/lib/zig9`;
   hget the a.out (56 MB — byte-writer too slow, use `http.server` + `hget`).

### Patches added
- **15-cbe-plan9-entry** (start.zig + os/plan9/x86_64.zig): under
  `object_format==.c`, export a C `main` (CBE can't emit the naked `_start`);
  init `std.os.plan9.tos` from cc9's `zig9_tos()`, wire `environ`. And route the
  four `syscallN` shims through `zig9_syscall` for `stage2_c` (clang rejects the
  inline-asm clobber list). ofmt==c-guarded → **zero effect on the a.out path.**
- **16-plan9-native-runtime** (os/plan9.zig, fs/Dir.zig, get_app_data_dir.zig,
  os.zig): the std.posix/std.fs surface the compiler's own runtime needs —
  `getcwd`, `Stat`+`fstat`/`stat`/`fstatat` (9P2000 decode), `openMode` (POSIX
  O over OPEN/CREATE), `mkdirat`/`unlinkat`/`renameat` (wstat name-change,
  same-dir), `rename`, dir `Iterator` (9P entry decode), `lseek`, clocks via
  `/dev/bintime`, `poll`(fake), `rfork`/`exec`/`pipe`/`dup2`/`await`/`fork`,
  `Child` spawnPosix-path stubs, `errnoNeg` (errstr→errno), `getFdPath` via
  fd2path. Mostly additive + plan9-guarded.
- **17-plan9-single-threaded-gpa** (main.zig): SmpAllocator asserts
  `!single_threaded`; the native build is `-fsingle-threaded`, so pick
  DebugAllocator (the proven plan9 heap).

### Status: compiler builds, runs, dispatches — crashes in AstGen worker
`zig9.aout` runs on the QEMU VM: argv works, core-command dispatch works
(non-core `version`/`env`/`fmt`/`ast-check` correctly panic via `dev.check`,
whose message lands in Plan 9's **exit-status string** — invisible to `>[2=1]`,
seen via `echo $status`). `build-exe` gets all the way into `comp.update`.
**Verified working in isolation via CBE+cc9 probes** (deliver.py + `/tmp/cc9bin
>[2=1]`): raw+std stdout, argv, native target resolution, `cleanExit`, 56 MB
file reads, 200k-op DebugAllocator stress, **and full `Ast.parse` + `AstGen.
generate` of std/mem.zig (33257 ZIR insts).**

**The crash** (checkpoint-bisected with raw-`write(2)` probes to a file — listen1
only flushes network output on process *exit*, so a faulting proc shows nothing
over the socket; redirect fd2 to a file and `cat` it): reaches
`performAllTheWork` → **`astgen phase`** then faults (proc goes `Broken`, ~205 MB
resident). NOT stack overflow (1 GB stack via `-DCC9_STACK_BYTES` didn't help),
NOT the allocator, NOT AstGen computation itself (all proven working standalone).
So it's in the **astgen worker machinery** — `workerUpdateFile` +
`@import` path resolution + Zcu file/import-table management + WaitGroup — that
wraps AstGen, not AstGen's parse/lower. Next: per-file probes inside
`workerUpdateFile` (getSource vs import-resolution vs Zcu integration), or a
standalone reproducer that drives `@import` resolution through the plan9
`openat`/`realpath` arms.

### Debugging-on-9front gotchas learned
- **listen1 drops** `;`/`&&` chains, `{...}`, most multi-line input, `rc script`
  invocations, and `&`+`$apid`. Reliable: ONE command per nc; env via single
  `VAR=val cmd` prefix (one assignment) OR flags (`--zig-lib-dir`,
  `--global-cache-dir`); file-redirect for probe output (`>[2]/tmp/log`) then
  `cat` separately — network output is buffered until process exit.
- Reliable output for a fresh a.out: `deliver.py` (byte-writer) materializes
  `/tmp/cc9bin`, then `/tmp/cc9bin >[2=1]` merges stderr. (56 MB is too big for
  the byte-writer → hget.)
- A faulted proc stays `Broken` (205 MB here); acid attaches but the elf2aout
  symbol table doesn't match its expectations (`image does not match text`,
  `lstk()` "syntax error near PC") — raw fault PC extraction still TODO.
- cc9 default stack is 256 MB (`CC9_STACK_BYTES`, crt0.c:323, `#ifndef`); rebuild
  crt0.c with `-DCC9_STACK_BYTES=…` and link it FIRST (`-z muldefs`) to change it.

### Regression note
The cross a.out path (corpus 13/13, behavior 1773/0) is validated with the
**docker-built patched backend**, NOT `vendor/zig-host` (which lacks the src
backend patches — `zig-host` cross-building hashmap hits the pre-patch
`store: [direct]` panic; that's expected, not a regression). A full
behavior-suite re-run with the rebuilt patched zig + patches 15-17 is the
pending regression gate; patches 15/17 are ofmt==c/single-threaded-guarded and
16 is additive plan9 arms, so no a.out-path change is expected.

## Native compiler — MAJOR advance (2026-07-15 continued): full compile pipeline runs

Picked the AstGen-worker crash apart and drove the native compiler MUCH further.
`build-exe hello.zig` on the QEMU VM now runs the **entire front-to-back pipeline**:
dispatch → cache dir setup → **builtin.zig generation** → **AstGen of the whole std
library** (~500 files, ~35 MB ZIR cache written) → **Sema** → **codegen** (the
self-hosted x86_64 backend emitting machine code ON 9front) → **processExports**.
Confirmed by checkpoint probes reaching `performAllTheWork DONE` + `processExports
done`. Only the very last mile — collecting errors + the linker flush — doesn't
complete yet.

### Bugs found + fixed to get here (each isolated with a CBE+cc9 smoke, then fixed)
- **getrandom** (patch 16): `std.crypto.random` (used by `std.fs.AtomicFile`'s
  random temp names, which `Builtin.writeFile` uses to write builtin.zig) seeds
  from `posix.getrandom`, which I'd stubbed `void` → it panicked. Fix: read
  `/dev/random` (`os/plan9.zig readRandom` + a `posix.getrandom` plan9 arm). This
  was THE AstGen-phase crash (in `workerUpdateBuiltinZigFile` → `populateFile`).
- **renameat** (patch 16): the hand-rolled Twstat rename is flaky across the 9P
  server (works at 2-level paths, fails "is a directory" at 1-level — cause never
  fully pinned; message is byte-identical to cc9's `build_wstat`). Under the CBE
  build, defer the same-dir case to cc9's proven `rename()` from n9libc; report
  clean XDEV for cross-dir so std can fall back to copy.
- **cross-directory rename** (patch 18): `renameTmpIntoCache` moves
  `tmp/<rand>` → `o/<hex>` — a CROSS-DIR rename, which Plan 9 CANNOT do (wstat only
  renames a leaf in place). My errno mapping made it retry forever. Fix: a plan9
  arm (`plan9MoveTree`) that recursively copies then deletes, using ONLY primitives
  proven to work on 9P — hand-rolled recursion because **`std.fs.Dir.walk` and
  `deleteTree` misbehave on plan9** (deleteTree fails mid-tree; walk mis-drove the
  copy). `plan9RmTree`/`plan9CopyTree` use openDir/iterate/makePath/copyFile/
  deleteFile/deleteDir, all individually smoke-verified.

### The remaining blocker: a non-deterministic phantom error
After codegen+exports, `anyErrors(comp)` (→ `getAllErrorsAlloc().errorMessageCount()`)
returns a **non-deterministic** count: sometimes >0 (→ updateModule early-returns,
skipping flush → no binary), sometimes 0. A second `getAllErrorsAlloc` in
buildOutputType then finds 0 errors, so nothing is rendered — hence the earlier
"exits silently, no binary, no error text". The cross-compile of the same
hello.zig with the same patched lib succeeds with **zero** errors, so these are
phantom. Non-determinism = **uninitialized memory** — almost certainly a CBE/cc9
miscompilation of some compiler struct (a field Zig assumes zero-init that the
C lowering + cc9's DebugAllocator leaves garbage), OR an uninitialized field in
the error-tracking state (`failed_analysis`/`failed_files`/the ErrorBundle).
Next: dump `failed_analysis`/`failed_files` counts + `misc_failures` in the FIRST
`getAllErrorsAlloc`; or bisect the CBE miscompile (build zig2.c at -O1 once the
host-OOM is worked around, to see if optimization changes the non-determinism).

### Debugging technique that cracked it
The listen1 buffering (network output only flushes on process exit) hid
everything. The winning loop: **fd2→file probes** (`>[2]/tmp/log`, `cat`
separately) for the running compiler; and for every suspected op, a **standalone
CBE+cc9 smoke** (`build.py --smoke X.zig` + `deliver.py` + `/tmp/cc9bin >[2=1]`)
that reproduces just that op in a 1 MB binary — dozens of these (fs_smoke,
atomicfile_smoke, cache_smoke, movetree_smoke, ccrename_smoke, …) each cost
seconds and pinned a bug that would've cost a ~5-min full rebuild to find.

## ★ NATIVE COMPILE WORKS (2026-07-15) — a Zig compiler inside 9front built + ran a program

`zig build-exe hello.zig` running natively on the 9front QEMU VM produced
`/usr/glenda/hello` (4061-byte Plan 9 a.out), which then **ran and printed
"native zig9 built this"**. The full self-hosted x86_64 backend + Plan9 linker,
driven by the CBE+cc9-built `zig` binary, compiled a real program end-to-end ON
THE BOX. This is the "compiler inside plan9" milestone.

**The phantom-error root cause + the fix (patch 17):** the non-deterministic
error count came from an **uninitialized-field read** in the compiler that the
allocator choice exposes. The host build uses SmpAllocator (fresh pages ~ zero);
the plan9 single-threaded build uses DebugAllocator, which (safety off in
ReleaseSmall) reuses freed memory **non-zeroed** → the uninit read returns
garbage → phantom errors → `anyErrors` true → flush skipped → empty/no binary.
Fix: a `ZeroAllocator` wrapper (main.zig) that `@memset(0)`s every fresh
allocation, making those reads deterministically 0 (matching the host). With it,
the compile completes and emits a working binary.

**RELIABLE after the grown-tail fix:** the residual non-determinism WAS the
`ZeroAllocator` not zeroing the newly-exposed bytes on a growing `resize`/`remap`
(an ArrayList grows, the compiler reads the new tail before writing it). Zeroing
`[old_len..new_len]` on grow made it deterministic: 3/3 consecutive hello builds
+ a separate `compute.zig` (loop summing 1..100 -> "compute OK: sum=5050") all
succeed, each producing a working a.out. Remaining: it's slow on TCG (~2-4 min/
compile); the underlying compiler still has a latent uninit-field read (masked by
the zeroing wrapper) worth finding for upstream; then corpus 13/13 natively
(Phase 5), `zig build` (Phase 6), pac9 release (Phase 7).

## Phase 5 acceptance (2026-07-15)

**Native compile — diverse programs compile ON 9front and run correctly:**
- `hello.zig` (raw plan9.write) -> "native zig9 built this" (3/3 reliable)
- `compute.zig` (loop 1..100) -> "compute OK: sum=5050"
- `01_arith.zig` -> "ok 01_arith (43)" (16854 B)
- `06_alloc.zig` -> "ok 06_alloc" (17776 B; ArrayList/heap)
- `10_hashmap.zig` -> "ok 10_hashmap" (33758 B; AutoHashMap — needs backend
  patch 04, which the native compiler carries in its source)
- `12_comptime.zig` -> "ok 12_comptime" (comptime eval)
Each ~3 min on the TCG-emulated VM (the full 13-file corpus run is time-bound
there; better on cirno bare metal). rc wrapper at native/z.rc — note the
`-mcpu=x86_64_v2` arg MUST be quoted (`'-mcpu=...'`) because rc treats a bare `=`
as a syntax error, and pass the lib dir via `--zig-lib-dir` FLAG (the ZIG_LIB_DIR
env var doesn't reach the CBE compiler through rc's /env).

**Cross regression (patches 15-18 must not break the verified a.out path):**
docker-rebuilt patched compiler + patched lib -> `run_corpus.py qemu` = **13/13
pass**, `run_behavior.py qemu` (full) = **1773 pass / 0 fail / 0 crash / 291 skip, 115/119 files** — EXACTLY the committed baseline (a first run read 1543 due to VM-contention output truncation; a clean re-run confirmed 1773). The lib/std
arms are additive/plan9-guarded and the src changes (ZeroAllocator, plan9MoveTree)
are single-threaded/plan9-guarded, so the ELF/self-hosted cross path is unchanged.

## Code review + fixes (2026-07-15)

An adversarial review (cross-checked against cc9's proven n9syscall.s/fs.c/
posix_llvm.c) verified the core is correct — the syscall thunk's arg marshaling,
all 9P2000 stat offsets, the Twstat body-length math, the `O` packed-struct bit
layout, `openMode`'s open-then-create data-loss avoidance, and patch 15's
tos/environ init. It found real bugs behind larger inputs, now FIXED:

- **waitpid reported every clean child as exit 1** (would break `zig build`/
  `std.process.Child`): Plan 9 AWAIT rc-quotes the status, so a clean exit is `''`
  not empty. Rewrote to skip the opening quote, treat `''`/empty as 0, parse a
  decimal code (Zig's plan9 exit writes decimal), else map to a signal — mirrors
  cc9's `cc9_wait_decode`. Also added an 8-slot zombie table + WNOHANG handling so
  waiting on one child doesn't discard another's (un-redeliverable) status.
- **Dir iterator could overflow name_buf / read OOB** on a long or garbage 9P
  name: added bounds guards (entry fits the buffer; name_len clamped to the
  entry's remaining bytes AND to name_buf).
- **plan9RmTree deleted while iterating** (breaks past the 8 KB read batch) and
  **openDir succeeds on regular files** on Plan 9: rewrote the cache-move helpers
  to stat for file-vs-dir and to snapshot all entry names before any delete.
- **plan9MoveTree was non-atomic**: added an errdefer that removes a half-written
  `o/<hash>` on copy failure, so a later run can't treat a truncated tree as a
  valid cache hit.
- a.out-path `renameat` Twstat buffer: reject names > 207 (NAMETOOLONG) instead
  of overflowing the 256-byte stack buffer.

Left as documented limitations: the ZeroAllocator masks (doesn't root-cause) the
latent uninitialized-field read; `clock_gettime(MONOTONIC)` returns wall time
(compiler timing only); several error wrappers return a generic errno on failure
(callers check success/failure, not the specific errno).

## Heavy demos — native codegen proven bit-exact (2026-07-15)

Two non-trivial programs compiled by `zig9` **natively on bare-metal cirno**
(`build-exe -OReleaseFast`) and run there, both producing output **byte-identical
to an `aarch64-linux-musl` reference** (built with `vendor/zig-host`, run in the
`zig9build` docker — use aarch64, NOT x86_64: Apple-Silicon docker's Rosetta
overflows bss on the x86_64 build). Sources in `native/demos/`:
- `raytrace.zig` — recursive ray tracer (spheres, lambertian+metal, AA, gamma) →
  172815-byte PPM (=15 B header + 320·180·3), `checksum=0xca574372fbbe3537`.
- `sha_wordcount.zig` — SHA-256 + `AutoHashMap` word-count over 120k PCG words →
  `sha256=42d64d8e…033140`, top `acme:6150`.
Both checksums matched the reference exactly → the CBE+cc9 compiler's self-hosted
x86_64 codegen is correct across FP, integer/bitwise/crypto, and allocator/hashmap
paths, not just the corpus.

## `zig build` natively — infra done, blocked by a latent heap-OOB

`zig build` needs the self-hosted backend to compile the whole `build_runner.zig`
(the std.Build system) *in-process*, which stresses the compiler far past any
corpus program. Implemented and shipped:
- `lib/std/process/Child.zig` `spawnPlan9` — rfork(RFPROC|RFFDG|RFREND) + pipe +
  dup2 + `/bin/<name>` exec, no CLOEXEC/errpipe (Plan 9 has none); wait via the
  fixed rc-quoted-status `waitpid`.
- `lib/std/io.zig` `pollPlan9` — blocking-read poller for `evalZigProcess`'s
  stdout/stderr pipes (Plan 9 has no poll(2)).
- `lib/std/Progress.zig` — `.plan9` added to `have_ipc = false` (kills the
  `ZIG_PROGRESS` fd-inheritance path).
- `src/main.zig` `cmdBuild` — force the build runner to ReleaseSmall +
  single_threaded + strip on plan9 (the backend panics in safety modes;
  `Thread.spawn` is a compile-error).

Then a cascade of latent bugs, each fixed, each moving the failure deeper (debug
loop: hget the binary, run a `.rc` probe redirecting to files, symbolize the fault
PC with `llvm-symbolizer --obj=_out/zig9big.elf 0x<pc>` — elf2aout preserves vaddrs):
1. `error.RenameAcrossMountPoints` — `Package.Fetch.renameTmpIntoCache`
   (`main.zig:7435`, the `dependencies.zig` cache move) does a cross-dir tmp→o
   rename Plan 9 wstat can't do, and wasn't routed through Compilation's
   `plan9MoveTree`. Fixed: plan9 branch + copy+delete helpers in `Fetch.zig`
   (the circular import forbids sharing Compilation's copy).
2. `getOffset: [ds:0xNNNN]` panic — `CodeGen.getOffset` (`arch/x86_64`) had
   `else => panic` on a `.memory` MCValue (the `[ds:0x..]` is `MCValue.format`).
   Added `.memory`/`.indirect` to the `.load_symbol,.load_frame` arm (materialize
   pointer→register_offset). Also hardened `Plan9.zig` `seeNav` to allocate
   `got_index` on the found-existing path (`getOffsetTableAddress`'s `.?` is UB
   with safety off). LLVM elides these dead paths on the cross build → 1773 never
   hit them; the self-hosted backend does not, so they shipped latent.
3. GP fault in `SbrkAllocator.free` — the std sbrk page allocator corrupts its
   free-list under the runner compile's heavy alloc load. Fixed systemically by
   backing the plan9 single-threaded compiler heap with **cc9's malloc/free**
   (n9libc — the heap that runs rustc/CPython) via a hand-rolled `Cc9Allocator`
   (`main.zig`), still wrapped by ZeroAllocator for the uninit-read fix.
4. After 1-3 the fault became a **null-deref inside cc9 `malloc_u`**
   (`fault read addr=0x8` — a null `Header.s.ptr` in the K&R free-list) = an **OOB
   write in the compiler corrupts the heap** at build_runner scale. This is THE
   remaining blocker: worse than the known uninit *read* (it's an OOB *write*), and
   not tractable to root-cause blind — no ASAN on the CBE/cc9 path, ~2 min/probe on
   cirno, and it triggers only on a compile far larger than any corpus program.

**RESOLVED (2026-07-15, later the same day): `zig build` WORKS natively.** The
"latent heap-OOB" hunt concluded — see the next section for the full chain.

## `zig build` natively — LANDED (2026-07-15)

`zig build -Doptimize=ReleaseSmall` runs end-to-end on bare-metal cirno: the
compiler builds `build_runner.zig` in-process (self-hosted backend + Plan 9
linker), spawns the runner (spawnPlan9), the runner drives a child
`zig build-exe --listen=-` over pipes (pollPlan9), and installs to
`zig-out/bin`. Fresh build ≈ 5-8 min; cached rebuild ≈ 15 s (exit 0, silent).
Verified with the canonical `standardOptimizeOption(.{})` + `-Doptimize`.
The heavy-demo checksums stay bit-exact under the final binary.

**The debugging method that worked:** rebuild the patched compiler in docker at
Debug (safety on) and cross-compile a build_runner-equivalent to plan9
(`--dep @build --dep @dependencies -Mroot=.../build_runner.zig` + stub modules).
That surfaced every semantic gap as a clean compile error list instead of
native-side corruption, at minutes per iteration instead of a 25-minute
CBE rebuild + on-box probe per theory.

**The full fix chain (in order discovered):**
1. **Runner semantic gaps** (lib, found by the Debug cross-compile): Watch.init
   runtime-fails on OSes with `Os == void` instead of failing to compile;
   plan9.zig gained `CLOCK`, `symlinkat` (EOPNOTSUPP — no symlinks), `futimens`
   (no-op success; Cache content-hashes when mtime is unreliable); the
   build_runner's `--fuzz`/`std.net` block comptime-gated for plan9;
   `Target.zig`'s 3 stage2_x86_64+coff vector workarounds extended to plan9.
2. **compiler-rt in the zcu** (`Compilation.zig` RtStrat): Exe builds used
   `.lib` strat — a static library the Plan 9 linker silently never linked (the
   real reason named externs "didn't work"). Plan9 now uses `.zcu` like Obj
   builds. `common.linkage` forced `.strong` on plan9 even in test builds (the
   `.internal` test default produced zero exports to resolve against).
3. **Named-symbol resolution in the linker** (`link/Plan9.zig` + CodeGen +
   Lower): `globals` table of phantom atoms (name → GOT slot), resolved at
   flush against same-named zcu exports (fail-by-name if missing);
   `genExternSymbolRef` + the Select `.symbol` operand emit ds-relative GOT
   loads/calls on plan9; Lower's call/jmp promotion extended to memory operands
   (`call m32` has no encoding). With the FULL compiler-rt this briefly closed
   the historical "only 2 unrunnable behavior files" gap (116/119 files, 1792
   pass / 0 fail) — the float-family trim in step 7 then gave the extra file
   back, so the SHIPPED state is exactly the committed baseline: 115/119 files,
   1773 pass / 0 fail (the 4 non-building files are the same platform-N/A set
   as before; `zon` now fails on a cleanly-named missing float libcall).
4. **The heap-corruption root cause — TWO ALLOCATORS, ONE BREAK:** Zig's
   `plan9.sbrk` kept its own break cursor rooted at `end`; cc9's `n9_sbrk`
   keeps another, also rooted at `end`. In the CBE compiler both run in one
   process → overlapping grants → the entire "latent corruption" family
   (phantom errors, SbrkAllocator free-list GP, malloc null-deref). Fix: one
   break owner — n9libc exports a locked `cc9_sbrk`, and `plan9.sbrk` delegates
   to it under `zig_backend == .stage2_c` (native-backend target programs keep
   the local cursor).
5. **Retain-forever gpa free** (`main.zig` Cc9Allocator): with a real
   malloc/free, a still-unpinned stale-pointer write in the compiler corrupts
   the K&R heap at build_runner scale (DebugAllocator's retained buckets had
   masked it for years). freeFn is a deliberate no-op — a compile is a one-shot
   process; the kernel reclaims at exit. Root-causing needs a tracking
   allocator.
6. **Uninitialized linker bases** (`link/Plan9.zig` createEmpty): `.bases =
   undefined`, set only by `open()`. cmdBuild's in-process whole-cache flow
   goes createEmpty → makeWritable (opens the file, never sets bases) → every
   atom got offset ~0, entry 0, "exec header invalid". The docker one-shot
   repro used open() — flow-sensitive, not environment-sensitive. Fix:
   initialize bases in createEmpty. Related hardening: the entry point is now
   derived from the `_start` export's atom offset after layout (writeSyms's
   name-scan assigned a garbage sym `.value` on this path) and fails loudly if
   absent; flushModule validates/reopens a dead emit fd (fstat + createFile)
   and reports the kernel errstr on write failures.
7. **The 2 MB text boundary:** the kernel places the data segment at
   roundup(text_end, 2MB) while the linker's data base is fixed at 0x400000 —
   text > 2MB breaks every ds-relative GOT immediate. In-module compiler-rt's
   float families pushed the runner to 2.13 MB. Fix: exclude the float families
   from compiler_rt.zig on plan9-stage2 (integer/mem/BigInt cover what std
   libcalls; the backend can't compile several f16/f80/f128 helpers anyway —
   "ran out of registers"). Program text ≤ ~2MB is a documented ceiling; linker
   GC of unreferenced atoms is the upgrade path (needs codegen→linker reference
   tracking that doesn't exist yet).
8. **Runner-side cross-dir renames** (lib/std/Build): Options.zig copies+deletes
   its single generated file; Run.zig moves output trees via a local
   plan9MoveTree (same snapshot-names-before-delete discipline as
   Compilation.zig's).

**rc-scripting gotchas that burned hours here** (now permanently in memory):
unquoted `=` in an argument (`-Doptimize=ReleaseSmall`) is an rc SYNTAX ERROR
that silently aborts the script mid-run — quote it; `rfork s` (not `n`) is the
note-group detach that survives listen1 disconnects; `log.warn` is compiled out
of ReleaseSmall (use `log.err` for diagnostics that must surface); a partial
`tar x` from a dropped connection leaves a silently stale lib tree — verify by
grepping for a sentinel after extraction.

## Packaging (Phase 7)

`build.py --package` assembles `_out/zig9-amd64.tar.gz` (`amd64/bin/zig9`,
`sys/lib/zig9/lib/…`, `rc/bin/zig`). The `rc/bin/zig` wrapper injects
`--zig-lib-dir /sys/lib/zig9/lib --global-cache-dir $home/lib/zig9-cache` after the
subcommand — the CBE-built compiler does not reliably read `ZIG_LIB_DIR` from the
environment, and the compile path was always driven by the flag. Fresh-install
tested end-to-end on the QEMU VM (extract at `/` → `zig build-exe hello.zig` →
runs). Registry row added; `pac9 install zig9`.
