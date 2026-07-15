# gpu9 — an Intel GPU driver for 9front, in userspace

`gpu9` gives 9front what it has never had: **hardware 2D acceleration**. 9front
runs with `hwaccel off` and `softscreen on` — every pixel on screen is pushed by
the CPU, by hand, through an uncached aperture. gpu9 hands those operations to
the GPU's blitter, where they belong.

It targets **Intel Gen8 (Broadwell)**. Tested on cirno, a Shuttle DS57U
(`8086/1606`, Broadwell-U GT1). See `docs/plans/2026-07-15-gpu9-broadwell-3d.md`
for scope and `NOTES.md` for the engineering detail.

## The headline: it accelerates the actual screen, 300–650x

The CPU can only reach framebuffer memory through the aperture, and that memory
is uncached: **13–15 MB/s**. The GPU's blitter is native to it. So for the
operations a display does all day — clearing a region, scrolling the screen —
the GPU is not a little faster, it is **two orders of magnitude** faster:

    gpu9 fill      GPU 9892 MB/s  vs CPU 15 MB/s  = 646x   (clear 1024x768)
    gpu9 scroll    GPU 4426 MB/s  vs CPU 13 MB/s  = 328x   (scroll up 16px)

Both are readback-verified — the numbers are for copies that provably landed
the right pixels. This is the honest comparison: both sides write the same
framebuffer VRAM, and it is exactly the work 9front's softscreen does by hand
today. `gpu9 demo` draws a grid straight onto the live screen so you can see it.

## Why it is fast: the GPU was asleep

That 9892 MB/s is only there because gpu9 also does the one thing every real
driver does and 9front does not: **manage the clock**. With no driver, nothing
runs RPS (Render P-State) and Broadwell parks at its 100 MHz floor forever — 1/8
of the 800 MHz it can do. One register write fixes it:

    gpu9 rps
    RPn (BIOS default, no driver) 100 MHz :    684 MB/s
    RP0 (what gpu9 asks for)      800 MHz :   4568 MB/s
    6.7x faster, for one write to RPNSWREQ. Both copies verified.

## What about plain RAM-to-RAM copies?

There the GPU is *not* a win — and gpu9 says so. `gpu9 bench` copies VRAM and
compares against `memcpy` in cached RAM: the CPU cache wins below 4 MB, they tie
at 4 MB, the GPU is ~8% ahead past it. That is the wrong workload for a GPU (it
does not copy your cached RAM), but the benchmark is kept, and honest, so the
300–650x screen numbers above cannot be mistaken for a copy-anything claim.
(An earlier version of this file *did* overclaim here — "1.11x at 16MB" — while
timing a copy that never finished; every blit is verified now.)

## Use

    echo type igfx >/dev/vgactl    # once: makes the kernel register the segments
    gpu9 info                      # what the GPU is; what clock it is really at
    gpu9 fill                      # accelerated framebuffer clear, GPU vs CPU
    gpu9 scroll                    # accelerated screen scroll, GPU vs CPU
    gpu9 demo                      # draw a grid on the real screen (visible!)
    gpu9 clock                     # request the max P-state
    gpu9 rps                       # prove the 6.7x: same blit at RPn vs RP0
    gpu9 bench                     # GPU blitter vs CPU memcpy (verifies each copy)
    gpu9 test                      # self-check: submit work, verify the GPU did it

The acceleration is also a library — `gpu9_fill()` and `gpu9_blt()` in
`lib/gpu9.c` are the primitives; the commands above just exercise them. That is
the seam a `draw(3)`-backed accelerator or a terminal would hook into.

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
buffers with correct fences, and **accelerated 2D fill + copy** (`gpu9_fill` /
`gpu9_blt`) — all verified on bare metal, 300–650x the CPU on screen work.

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
