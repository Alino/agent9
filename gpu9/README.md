# gpu9 — an Intel GPU driver for 9front, in userspace

`gpu9` drives the Intel GPU on 9front: it submits commands to the hardware, waits
for them properly, and — crucially — **manages the GPU's clock**, which nothing
else on 9front does.

It targets **Intel Gen8 (Broadwell)**. Tested on cirno, a Shuttle DS57U
(`8086/1606`, Broadwell-U GT1). See `docs/plans/2026-07-15-gpu9-broadwell-3d.md`
for scope and `NOTES.md` for the engineering detail.

## The headline: your GPU is probably asleep

Linux's i915 runs RPS (Render P-State) — it reads the hardware's frequency limits
and asks for one. **9front has no GPU driver, so nothing ever asks, and the GPU
sits at its minimum clock forever.** On cirno that is **100 MHz out of 800 MHz**.

Asking for the top P-state — one register write a real driver owes the hardware —
makes it **6.7x faster**. Don't take that on trust; `gpu9 rps` re-derives it on
your box, running the same verified 4MB blit at each P-state:

    RPn (BIOS default, no driver) 100 MHz :    684 MB/s
    RP0 (what gpu9 asks for)      800 MHz :   4568 MB/s
    6.7x faster, for one write to RPNSWREQ. Both copies verified.

## Is it faster than the CPU? Barely, and only on big copies

    gpu9 bench

         bytes |   GPU blit   |  CPU memcpy  | GPU vs CPU
        262144 |   4411 MB/s  |   8609 MB/s  | 0.51x
       1048576 |   4459 MB/s  |   6290 MB/s  | 0.71x
       4194304 |   4428 MB/s  |   4455 MB/s  | 0.99x
      16244736 |   4418 MB/s  |   4101 MB/s  | 1.08x  <- GPU wins

The GPU is **flat** (clock-bound). The CPU is faster while the data fits in
cache, then falls to ~4.1 GB/s. They **tie around 4 MB**, and past that the GPU
is ~8% ahead — while working asynchronously, which is the real win.

That is a weaker claim than this file used to make. It said "1.11x at 16MB", and
that number was **timing a copy that never happened**: nothing verified the
blit, and the last 1MB of the aperture silently swallows writes (see below).
`gpu9 bench` now checks every copy and refuses to print a speed for a wrong one.

No cherry-picking in the other direction either: the CPU baseline is `memcpy` in
**normal cached RAM**, best of several. (Benchmarking the CPU *through the
aperture* would show ~13 MB/s and let us claim "50x faster" — that memory is
uncached, and the number would be a lie.)

## Use

    echo type igfx >/dev/vgactl    # once: makes the kernel register the segments
    gpu9 info                      # what the GPU is; what clock it is really at
    gpu9 clock                     # request the max P-state
    gpu9 rps                       # prove the 6.7x: same blit at RPn vs RP0
    gpu9 bench                     # GPU blitter vs CPU memcpy (verifies each copy)
    gpu9 test                      # self-check: submit work, verify the GPU did it

`echo type igfx` only makes the kernel map BAR0/the aperture and register them as
named segments — it does **not** program your display.

## How it works (no kernel component)

9front already hands userspace everything needed. The kernel's igfx code calls
`addvgaseg("igfxmmio", BAR0)` and `addvgaseg("igfxscreen", aperture)`; userspace
`segattach`es them — the same mechanism `aux/vga` uses to poke display registers.

    segattach("igfxmmio")   -> BAR0: all GPU registers, and the GTT at +8MB
    segattach("igfxscreen") -> the aperture: a CPU window THROUGH the GTT,
                               so **aperture offset == GGTT address**

The BIOS already GTT-maps the aperture to contiguous stolen memory, so
GPU-visible memory is free: no physical addresses, no GTT programming, no DMA
heap, no kernel patch. The framebuffer uses only the low ~1.5MB.

**But the aperture is not all yours, and the page tables will not tell you so.**
Of cirno's 16384 aperture pages: 3 carry PTEs that are not the linear stolen
mapping (8190/8191, and the last), and 256 more — the top 1MB — hold PTEs
identical to every other yet silently **discard writes**. `gpu9_arena()`
therefore trusts neither the aperture size nor the PTEs: it checks the PTE
first (never write through one pointing somewhere unknown), then proves the page
with a write/readback that restores what it found, and takes the largest run
that survives both. On cirno that is 31MB, not 62MB.

Submission uses the **legacy ring** — available because 9front has no i915 to put
the GPU into execlist mode, which is exactly why this is simpler here than on
Linux.

## Security — read this before installing

A process holding GPU MMIO + the GTT can program the engine to DMA **anywhere in
physical memory**: read or write all of RAM, kernel included. That is full
privilege escalation for anyone who can open those segments. It is a real reason
Linux keeps DRM in the kernel. `aux/vga` already has this shape, but display-only
is far less dangerous than a command-submitting engine. Fine on a box you own;
never a default.

## Status

Working: MMIO, forcewake, GTT/aperture memory, RPS, the legacy ring, batch
buffers with correct fences, the 2D blitter — all verified on bare metal.

**Not yet: 3D/OpenGL.** Mesa's `iris` now **compiles and links** for 9front
(1027/1027 TUs via cc9, zero undefined symbols), but wiring it to gpu9 is a big
job, and the wall is addressing, not the `ioctl()` shim: iris is softpin-only
(`EXEC_OBJECT_PINNED`, no relocation fallback — and i965, the old
relocation-based Broadwell driver, was deleted in Mesa 22), and its VMA zones
span 0..12GB+ while gpu9 has a 64MB GGTT. That means compressing iris's memzones
into the GGTT (a real Mesa fork) or building PPGTT page tables. See NOTES.md.

## Gotchas that cost real time (all in NOTES.md)

- **The flush command is per-engine.** `MI_FLUSH_DW` is blitter/media; on the
  render ring it is consumed but never posts the write — indistinguishable from a
  hang. Render needs `PIPE_CONTROL`.
- **`HEAD==TAIL` is not completion** — it means *parsed*. Always wait on a fence.
- **A benchmark that does not verify its result is not a benchmark.** The 16MB
  blit was "1.11x faster" for weeks while not copying the last 8%.
- **Mapped is not yours.** A present, correctly-linear PTE can still point at
  memory that ignores your writes. Test the memory, not the page table.
- **A wedged engine only clears on reboot.** `HEAD` frozen at 0 = wedged; every
  later submit is ignored, including ones that worked a minute ago.
- **Get encodings from `gl9/vendor/mesa/src/intel/genxml/gen8.xml`**, the
  machine-readable spec. Do not guess.
- BDW/HSW register shifts differ from gen6: `RPNSWREQ` freq is bits **31:24**,
  `RPSTAT1` CAGF is shift **7**. Wrong shift = garbage = false conclusions.
