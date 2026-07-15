# gpu9 — a real 3D GPU driver for 9front (Intel Broadwell)

> Status: **scope / not started.** Captured 2026-07-15, after llvmpipe landed
> ([[llvm9]], ~15x softpipe on bare metal). This is the "actually use the GPU"
> project that llvmpipe was chosen *instead of*. Nothing here is committed to.

## What this would be

As far as I can tell, **the first hardware 3D driver for Plan 9, ever.** Verified
against the tree, not from memory: the only 3D-related string in 9front's entire
kernel graphics stack is a *comment* in `vgasavage.c:347` ("texture surface tag,
vertex buffer tag") describing an S3 register field. What exists is:

- `devdraw.c` (~2k lines) + `libdraw`/`libmemdraw` — software 2D, by design
- `vga*.c` — modesetting; 6 legacy cards do 2D blit/fill (`hwfill`/`hwscroll`)
- `libgeometry` — 3D *math* (matrices), pure software

No command submission, no shaders, no GPU memory manager, for any chip.

**Epistemic honesty:** I can grep this tree; I cannot verify a negative across
~35 years of Plan 9 (Bell Labs, 9front, 9legacy, Harvey, unmerged experiments).
"First ever" is a claim to check with the 9front community before putting it in a
README — not to take from me.

## Why it's suddenly plausible

The userspace half — the part that took this entire session to port for llvmpipe
— **already exists on 9front**, and two facts make the rest bounded:

1. **Mesa's `iris` supports cirno's exact GPU, by device ID:**
   `iris_pci_ids.h:2: CHIPSET(0x1606, bdw_gt1, "BDW GT1", "Intel(R) HD Graphics")`
   We do not have to write a userspace 3D driver. It exists and knows the chip.

2. **iris funnels EVERY kernel interaction through one function:**
   `src/intel/common/intel_gem.h:77 — intel_ioctl(int fd, unsigned long request, void *arg)`
   and it uses exactly **22 distinct i915 ioctls** (enumerated below). That is the
   entire kernel/userspace contract. It is a knowable, finite surface.

Plus: **Broadwell is the best-case target.** Intel publishes the Gen8 PRMs (AMD/
NVIDIA would mean reverse-engineering), and Gen8's GuC/HuC firmware is *optional*
— no signed blobs, unlike later gens and all discrete cards.

## It is mostly a USERSPACE package, not a kernel driver

The instinct is "GPU driver = kernel". On Plan 9 that is wrong, and 9front already
has the mechanism — `aux/vga` is a **userspace** program that maps GPU MMIO and
pokes registers. Verified in the tree:

    kernel    vgaigfx.c:51  addvgaseg("igfxmmio", p->mem[0].bar&~0x0F, p->mem[0].size);
              vgaigfx.c:55  addvgaseg("igfxscreen", scr->paddr, scr->apsize);
              (generic: vga.c:251 addvgaseg -> port/segment.c:706 addphysseg)
    userspace igfx.c:432    igfx->mmio = segattach(0, "igfxmmio", 0, size);
              igfx.c:207    igfx->mmio[a/4] = v;      /* GPU registers, from userspace */

The kernel exposes a physical range as a NAMED SEGMENT; userspace segattaches it
and drives the hardware directly. On cirno: **BAR0 = 16MB** (Gen8: registers AND
the GTT) and **BAR2 = 256MB** (aperture) — both already exposable this way.

So userspace can do: forcewake, all register access, **GTT programming** (it lives
in BAR0), ring setup, command submission. That is nearly the whole driver.

**What genuinely needs kernel** (small, and wxallow-shaped — gated + opt-in):
1. Expose the BARs *without* taking over the display. `addvgaseg("igfxmmio")`
   currently only fires when the igfx DISPLAY driver attaches — which blanks an
   HDMI panel (we proved this the hard way). A few lines, gated.
2. Physical pages for buffers: the GTT needs real physical addresses. Either a
   small contiguous DMA heap, or — cheaper for the spike — reuse the BIOS
   **stolen memory** (GSM) that already backs the framebuffer.
3. Interrupts: NOT needed. We poll.

**Shape: `pac9 install gpu9` (userspace) + a small optional kernel patch.**
Exactly the gl9/llvmpipe model (package + wxallow gate).

**This kills the project's biggest risk.** The "reboot per iteration, kernel bug =
panic" fear below is largely void: in userspace a bug is a dead process. aux/vga
machine-checked during the igfx experiment and killed only itself; the box never
blinked. That is a normal edit-run-debug loop, and it materially lowers the cost
estimate.

**The tradeoff, stated plainly — SECURITY.** A userspace process holding GPU MMIO
+ GTT can program the engine to DMA anywhere in physical memory: read or write
all of RAM, kernel included. Full privilege escalation for anyone who can open
that segment. This is a real reason Linux keeps DRM in the kernel. aux/vga already
has this property but is display-only and far less dangerous than a 3D engine.
Same shape as the wxallow call: a gated, opt-in, genuinely-lower-security mode —
fine on a box you own, not a default.

## The architecture (the one good idea here)

Plan 9 has no `ioctl`. That looks like a wall and is actually the seam:

    iris (UNMODIFIED)  ->  intel_ioctl(fd, req, arg)
                              |
                       cc9 runtime: ioctl() shim        <- new, ~1-2k lines
                              |  marshal 22 request codes
                       /dev/i915/* (Plan 9 file server) <- new kernel device
                              |
                       Broadwell GT1 (8086/1606)

cc9 already has exactly this shape: `posix_llvm.c` is a POSIX surface that lets
unmodified Unix code link. Adding `ioctl()` that marshals i915 structs over a
Plan 9 device means **iris compiles unmodified** — no fork of Mesa's driver, no
rebasing pain. The kernel side is a normal Plan 9 device (`#G`/`/dev/i915`), not
a DRM clone.

### The 22 ioctls (the whole contract)

Essential for a first triangle (~10):
  GEM_CREATE, GEM_MMAP / GEM_MMAP_OFFSET, GEM_EXECBUFFER2, GEM_WAIT, GEM_BUSY,
  GEM_CONTEXT_CREATE / _DESTROY, GETPARAM, GEM_SET_DOMAIN

Needed soon after (~8):
  GEM_SET_TILING / GET_TILING, GEM_SET_CACHING, GEM_MADVISE, GEM_GET_APERTURE,
  I915_QUERY (ENGINE_INFO, TOPOLOGY_INFO, MEMORY_REGIONS), CONTEXT_GET/SETPARAM

Probably stub-able:
  GEM_USERPTR, REG_READ, GET_RESET_STATS, GEM_CREATE_EXT, CONTEXT_CREATE_EXT,
  QUERY_HWCONFIG_BLOB, QUERY_GEOMETRY_SUBSLICES

`GEM_EXECBUFFER2` is the entire ballgame. The rest is bookkeeping.

## What has to be built (the kernel half)

This is the part that does not exist, at any scale, anywhere in Plan 9:

1. **PCI + MMIO** — map GTTMMADR (BAR0: 16MB regs + GTT) and GMADR (BAR2:
   aperture). 9front can do this today (`pci.c`, `devarch`).
2. **Forcewake** — the GT sleeps; touching render registers without waking it
   returns garbage. Per-domain (render/media/blitter) on Gen8.
3. **GTT** — program the global translation table: GPU address -> physical page.
   Start GGTT-only; skip PPGTT (48-bit, 4-level) until it's forced.
4. **Buffer objects** — allocate + pin pages, map into the GTT, give the CPU a
   mapping. This is "GEM" minus everything optional.
5. **Command submission** — write a batch, point the ring at it, kick the tail
   register, wait for completion. **Poll instead of interrupts** at first.
6. **Contexts** — a logical ring context per client. Minimal: one.

Deliberately OUT of scope for v1: reset/hang recovery, power management, multi-
engine (blitter/media), PPGTT, interrupts, GPU scheduling, multi-process.

## Milestones — the decisive spike is M3

- **M0 · Recon (days).** Read the Gen8 PRM volumes that matter. Settle the one
  question that shapes everything: **execlists vs legacy ring on Gen8.** Linux
  i915 uses execlists (ELSP) for Gen8+; the legacy RCS ring may still function in
  silicon. Legacy ring is *dramatically* simpler. **This answer sizes the project.**
- **M1 · Talk to the GPU (days).** Kernel: map BARs, forcewake, read GPU ID +
  fuse registers. Success = read back a sane device ID from MMIO. Cheap, proves
  the plumbing.
- **M2 · Memory (1-2 weeks).** Allocate pages, program GTT entries, CPU-map a
  buffer, verify the GPU aperture sees the same bytes.
- **M3 · ⭐ SUBMIT ONE COMMAND (the "f()=42" moment).** Build a batch containing
  a single `MI_STORE_DATA_IMM`, submit it, and see the DWORD land in memory.
  **If this works, everything after is grind. If it can't be made to work
  (forcewake / execlists / GTT wrong), the project is blocked and we learn it in
  week 3, not month 4.** Do this before anything else is built.
- **M4 · Blitter (1 week).** A GPU 2D copy (XY_SRC_COPY_BLT). First *visible*
  GPU-produced result; validates submission end-to-end.
- **M5 · The device + ioctl shim (2-4 weeks).** `/dev/i915` file server + cc9
  `ioctl()` marshalling. Implement the ~10 essential ioctls.
- **M6 · Build iris with cc9 (1-2 weeks).** ~160k lines (iris 31k, intel/compiler
  98k, isl 13k, common/dev/blorp 18k). Mechanically the same grind as gl9/llvm9 —
  we know this pattern cold now. Expect a long tail of shim fills, not walls.
- **M7 · First GPU triangle.** Point gl9's OSMesa/EGL seam at iris instead of
  llvmpipe.
- **M8 · Parity.** Diff against the llvmpipe/softpipe goldens — we already have
  the corpus and the harness. This is why the parity suite was worth building.

## Risks, honestly

- **Execlists.** If Gen8 legacy ring submission is dead in silicon, M3 means
  implementing execlist context images (a complex, poorly-loved format). Biggest
  single unknown. M0 exists to answer it.
- **Debugging blind — MOSTLY DEFUSED** by the userspace design above (a bug is a
  dead process, not a panic). What remains: a *wedged GPU* (bad batch -> hung
  engine) may still need a reboot to recover, and you cannot restore a video mode
  remotely (no `/dev/realmode` on amd64 — VESA is bootloader-only). So: reboots
  per *hang*, not per *bug*. Tolerable. Keep the render engine off the display
  path during bring-up and the screen never has to be at risk at all.
- **Broadwell-specific.** ~None of the kernel half transfers to another GPU. If
  cirno dies, the work dies.
- **Machine checks are real here.** `display=7` in the igfx experiment produced
  `sys: trap: machine check` — touching wrong MMIO on this chip faults hard. The
  kernel side will do this repeatedly, and in the *kernel* it's a panic, not a
  dead process.
- **Scale.** 3-6 person-months, realistically. Not weeks.

## Should we?

The honest case against: **nothing needs it.** llvmpipe already delivers 60-68fps
at 512x512 with per-pixel Phong; the real workloads (alacritty9, neovim9) are 2D
and were fine on *softpipe*. At 1080p a heavy shader would drop llvmpipe to
~8fps where a GPU would do hundreds — a real gap, with no current consumer.

The honest case for: it would be a genuine first for Plan 9; Broadwell is the
best-case chip (public PRMs, no firmware); the expensive userspace half is
*already done and proven*; and the contract is 22 ioctls behind one function.
That is a far better position than anyone porting this has started from.

So: this is a **research project pursued for its own sake**, not a needs-driven
one. That is a legitimate reason — but it should be chosen with eyes open, not
drifted into because llvmpipe made 3D feel close.

**Recommended first step regardless: M0 + M3.** Two to three weeks answers
"is this possible on this chip at all" for ~5% of the total cost. Everything else
is contingent on that DWORD landing in memory.
