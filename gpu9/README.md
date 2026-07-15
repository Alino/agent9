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
makes it **6.7x faster**:

    blitter, 4MB copy      @100 MHz (BIOS default)   684 MB/s
                           @800 MHz (RP0)           4582 MB/s

## Is it faster than the CPU? Sometimes — and here is exactly when

    gpu9 bench

         bytes |   GPU blit   |  CPU memcpy  | GPU vs CPU
        262144 |   4538 MB/s  |   8528 MB/s  | 0.53x
       1048576 |   4570 MB/s  |   6212 MB/s  | 0.74x
       4194304 |   4533 MB/s  |   4179 MB/s  | 1.08x  <- GPU wins
      16777216 |   4555 MB/s  |   4086 MB/s  | 1.11x  <- GPU wins

The GPU is **flat** (clock-bound). The CPU is faster while the data fits in
cache, then falls to ~4.1 GB/s. **Past ~4 MB the GPU wins — and it works
asynchronously, leaving the CPU free.**

No cherry-picking: the CPU baseline is `memcpy` in **normal cached RAM**, best of
several. (Benchmarking the CPU *through the aperture* would show ~13 MB/s and let
us claim "50x faster" — that memory is uncached, and the number would be a lie.)

## Use

    echo type igfx >/dev/vgactl    # once: makes the kernel register the segments
    gpu9 info                      # what the GPU is; what clock it is really at
    gpu9 clock                     # request the max P-state
    gpu9 bench                     # GPU blitter vs CPU memcpy
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

The BIOS already GTT-maps the whole 64MB aperture to contiguous stolen memory, so
GPU-visible memory is free: no physical addresses, no GTT programming, no DMA
heap, no kernel patch. The framebuffer uses only the low ~1.5MB.

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
buffers with correct fences, the 2D blitter. **Not yet: 3D/OpenGL** — that needs
Mesa's `iris` (builds: 1014/1029 TUs via cc9) plus an `ioctl()` shim; see NOTES.md.

## Gotchas that cost real time (all in NOTES.md)

- **The flush command is per-engine.** `MI_FLUSH_DW` is blitter/media; on the
  render ring it is consumed but never posts the write — indistinguishable from a
  hang. Render needs `PIPE_CONTROL`.
- **`HEAD==TAIL` is not completion** — it means *parsed*. Always wait on a fence.
- **A wedged engine only clears on reboot.** `HEAD` frozen at 0 = wedged; every
  later submit is ignored, including ones that worked a minute ago.
- **Get encodings from `gl9/vendor/mesa/src/intel/genxml/gen8.xml`**, the
  machine-readable spec. Do not guess.
- BDW/HSW register shifts differ from gen6: `RPNSWREQ` freq is bits **31:24**,
  `RPSTAT1` CAGF is shift **7**. Wrong shift = garbage = false conclusions.
