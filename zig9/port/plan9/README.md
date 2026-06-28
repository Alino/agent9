# zig9 port — 9front/amd64 archaeology

How Zig 0.14.1 is made to target 9front, what each fix is and why, what is still
blocked, and exactly how to finish it. The running log is in `NOTES.md`; this is
the consolidated picture.

## Architecture

- **No LLVM.** LLVM cannot emit Plan 9 objects. Zig's self-hosted x86_64 backend
  + Plan 9 a.out linker (`src/link/Plan9.zig`) are the only path, and the reason
  we pin **0.14.1** (the backend was deleted in 0.15.1 — see top-level README).
- **No C runtime.** Zig writes its own `_start` and talks to the kernel via raw
  syscalls (`lib/std/os/plan9/x86_64.zig`). This sidesteps the kencc "not-LP64"
  trap that bit `python9`/`node9`: Zig controls its own integer/pointer widths.
- **cc9 as oracle.** cc9 (clang/LLVM/C++ on 9front) already mapped 9front
  amd64's low-level gotchas on real hardware. Zig's experimental plan9 layer has
  the same ones; we cross-check against cc9's findings and validate on `cirno`.
- **Patches are pure `lib/std`.** All three fixes are in the standard library,
  so the prebuilt 0.14.1 host compiler picks them up immediately via
  `--zig-lib-dir vendor/zig/lib`. No compiler rebuild needed for them.

## The fixes (patches/)

### 01 — syscall clobbers callee-saved registers
`lib/std/os/plan9/x86_64.zig`. The Plan 9 amd64 `SYSCALL` does **not** preserve
the SysV callee-saved registers — cc9 proved on `cirno` that `rbx`, `rbp`, `r13`
come back holding kernel (KZERO-range) values. Zig's syscall asm declared only
`rcx,rax,rbp,r11` clobbered, so the compiler could keep a live value in
`rbx`/`r13` across a syscall and have it silently corrupted (fault at an address
starting `0xffffffff80…`). Fix: list **all** callee-saved registers
(`rbx,r12,r13,r14,r15`) as clobbered. QEMU's syscall path preserves them, hiding
the bug — this is a bare-metal-only hazard. *Status: applied; exercised by every
corpus test (which interleave syscalls with computation) on cirno.*

### 02 — `std.Thread` for plan9
`lib/std/Thread.zig`. plan9 fell through to `UnsupportedImpl`, so any reference
to `Thread.getCurrentId` (pulled in by the default `std.log`/stderr-lock path)
was a hard compile error. Added a minimal `Plan9ThreadImpl`: `getCurrentId`
returns the pid from the `_tos` page, `getCpuCount` returns 1; real thread spawn
over `rfork(RFMEM)`+semaphores is deferred (`@compileError` until then). *Status:
applied; unblocks `std.io`/`std.log`-touching programs.*

### 03 — FP-exception masking + safe env
`lib/std/start.zig`, in `callMainWithArgs`.
- **FP masking (validated on cirno).** Bare-metal 9front leaves the x87/SSE FP
  exceptions *unmasked*, so `1.0/0.0` raises a hardware trap that kills the
  process instead of yielding IEEE `+inf` (the same class of bug node9 hit:
  `npm help` → `sys: fp: division by zero`). We set `MXCSR=0x1F80` and x87
  `CW=0x037F` on the main thread. **Proven necessary on real hardware:** with the
  mask, `test/corpus/02_floats` and an explicit `1.0/0.0` test print `+inf`; with
  the mask reverted, the same binary on cirno produces *no output* — it traps and
  suicides. QEMU's TCG FPU masks by default, so this is invisible in emulation.
- **Env safety.** Plan 9 has no envp array on the entry stack (the kernel lays
  out only `{argc, argv[0..argc], null}`); walking past argv as a null-terminated
  env array reads undefined stack memory. We leave the environment empty on
  plan9. Surfacing the real environment from `/env` (à la `cc9/runtime/fs.c`) is
  a follow-up. *Status: applied; `argv` confirmed correct on cirno.*

## Backend fixes (patch 04, in the rebuilt compiler)

These live in `src/` (the compiler), so they need a rebuilt `zig`. We build it in
a Linux container (`linux-build.sh`) — see "Building the compiler" below.

1. **FIXED — `store: [direct:N]` backend panic (patch 04).**
   `src/arch/x86_64/CodeGen.zig` `Temp.store` handled `.lea_symbol`/`.load_symbol`
   but not the non-PIC `.lea_direct`/`.load_direct` plan9 uses for globals →
   `else => panic` when storing a value that lives at a direct symbol address.
   Added them alongside their symbol siblings. Unblocked `std.mem.sort`,
   `std.mem.indexOf`/`splitScalar`, `AutoHashMap` (corpus 04/05/10).
2. **FIXED — heap crash, was misdiagnosed as a relocation bug (patch 01).**
   `@intFromPtr(&ExecData.end)` faulted at a **KZERO** address *after a syscall*.
   Root cause is not the linker: the backend keeps the register it uses to reach
   globals (**rbp**) live across calls, and the kernel SYSCALL trashes rbp. The
   syscall wrapper now passes the number in rdx, sets rbp inside the asm, and
   explicitly push/pops rbp+rbx+r12-r15 (cc9 n9syscall.s style). All heap
   (`page_allocator`/`sbrk`/`ArrayList`, corpus 06) now survives syscalls.
3. **OPEN — `external symbols` / `@errorName` lazy symbols.** plan9's lazy/named
   symbol support exists in the backend's `genSetReg` GOT path (`CodeGen.zig`
   ~96301, ds-relative) but NOT in the newer `Select.Operand` / `Lower.zig`
   (`Lower.zig:558` is `TODO: bin format 'plan9'`). So `!void` main, `std.debug`
   traces, and `std.testing.expectError`'s message don't compile. Producing
   `.lea_direct` for the lazy symbol gets past the resolver but then hits an
   `unreachable` in the `.lea` operand lowering — the GOT logic from `genSetReg`
   needs replicating in the Select path. This is the next backend project.
   (`@floor` without SSE4.1 was a *different* external-symbol case — a libm
   libcall — sidestepped with `-mcpu=x86_64_v2` → `roundsd`.)

Also open: a second `.load_direct`-family gap (`getLimb: [direct:N]`, big-int),
fixable like patch 04; and `std.debug.SelfInfo` (DWARF) is not plan9-portable.

## Building the compiler (Linux container)

`zig build` cannot run on this macOS-26 host: the native build runner won't link
libSystem with Zig 0.14.1's bundled darwin stubs (undefined `_abort`,
`_realpath$DARWIN_EXTSN`, `__availability_version_check`, …). `--sysroot <older
SDK>` fixes a direct `build-exe` but `zig build` doesn't forward it. So we build
in a Linux container (static-musl runner, no SDK issue):

```sh
docker run -d --name zig9build -v <repo>:/work -w /work/zig9 debian:stable-slim sleep infinity
# install curl+xz, fetch zig-aarch64-linux-0.14.1 into /opt/zig-host, then:
sh port/plan9/linux-build.sh build        # -> /tmp/zig-out/bin/zig (patched, no-LLVM)
```
The container compiler cross-compiles to `x86_64-plan9`; the `a.out` lands in the
mounted tree and is delivered to 9front from the host with `cc9/host/deliver.py`.

### Path forward (to a compiler running *on* 9front, G2)
With (3) fixed, cross-build `zig` itself for `x86_64-plan9`
(`-Dtarget=x86_64-plan9 -Denable-llvm=false`) and run it on cirno. The fixes are
upstreamable to ziglang/zig's plan9 target (the backend ones would need
forward-porting past the 0.15 linker rework).

## Validating on hardware

```sh
# QEMU dev VM (listen1 127.0.0.1:1717 -> guest 17010) and cirno bare metal:
python3 test/run_corpus.py qemu      # 13/13, both targets
python3 test/run_corpus.py cirno     # 192.168.88.159:17010
python3 test/run_behavior.py qemu    # Zig's upstream test/behavior suite on 9front
```
The corpus scores **13/13 identically on QEMU and cirno** — the working subset
behaves the same in emulation and on real hardware (the major fixes, especially
the syscall/rbp and FP ones, are bare-metal hazards QEMU hides). The upstream
behavior suite runs via the minimal `test/plan9_test_runner.zig`. Delivery uses
`cc9/host/deliver.py` (ships the a.out as a generated C byte-writer because
listen1 mangles raw binary).
