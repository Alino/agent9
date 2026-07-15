# gpu9 — 3D on the Broadwell GPU (Intel Gen8) for 9front

Scope: see `docs/plans/2026-07-15-gpu9-broadwell-3d.md`. Intel Gen8+ only; in
practice Broadwell (cirno's 8086/1606), the only chip it is tested on.

## M0-M3 DONE (2026-07-15) — the GPU executes our commands

Everything below runs in **USERSPACE with no kernel patch**. 9front already
exposes what we need: the kernel's igfx bits call `addvgaseg("igfxmmio", BAR0)`
and `addvgaseg("igfxscreen", aperture)`, and userspace `segattach`es them — the
same mechanism `aux/vga` uses to poke display registers. `echo type igfx
>/dev/vgactl` triggers the registration (igfxenable only vmaps + registers
segments; it does NOT program the display, so the screen is untouched).

- **M1 — MMIO reachable.** `segattach(0,"igfxmmio",0,16MB)` then read
  PIPE_A_SRC (0x6001c) = `0x03ff02ff` — the KNOWN value (1024x768, the live VESA
  mode). Verified against a known constant, not a plausible-looking one.
- **M0 — LEGACY RING, not execlists.** `GFX_MODE (0x229c) = 0x00002800`, bit15
  (GFX_RUN_LIST_ENABLE) = 0. **Nothing put this GPU into execlist mode because
  9front has no i915 driver** — so we get the simple ring. This was the single
  biggest unknown and it fell the easy way. Forcewake works: HSW/BDW use
  FORCEWAKE_MT (0xa188) + FORCEWAKE_ACK_HSW (0x130044), `MASKED_ENABLE(1)`;
  render regs (0x2000+) read 0 before forcewake and real values after.
- **M2 — GPU memory for free.** The BIOS already GTT-maps the ENTIRE 64MB
  aperture (16384/16384 PTEs live) to contiguous stolen memory at 0xa4000000,
  linearly. The aperture (BAR2, "igfxscreen") is a CPU window THROUGH the GTT, so
  **aperture offset == GGTT address** — no physical addresses, no GTT
  programming, no DMA heap, no kernel help. The framebuffer only uses the first
  ~1.5MB; everything past that is free GPU-visible memory.
- **M3 — ⭐ THE SPIKE PASSED.** Built a legacy ring at GPU 0x800000, put one
  `MI_STORE_DATA_IMM` in it, kicked RING_TAIL, and the GPU wrote `0x42424242` to
  GPU 0x801000. HEAD advanced 0 -> 0x10. **The GPU executes commands we submit.**

### Recipe that works (Gen8 legacy ring)

    ring:  RCS_CTL(0x203c)=0; HEAD(0x2034)=0; TAIL(0x2030)=0;
           START(0x2038)=<ggtt addr, 4K aligned>; CTL=((pages-1)<<12)|1
    go:    write commands at ring[0..]; TAIL = nbytes (8-byte aligned)
    done:  HEAD advances to TAIL

    MI_STORE_DATA_IMM (Gen8) = (0x20<<23) | (1<<22 /*use GGTT*/) | 2
      DW1 = addr_lo, DW2 = addr_hi, DW3 = data     (4 dwords, len = dwords-2)

Probes: `gpu9/probe/{m1,m0,m2,m3}.c` — native kencc, build on-box with
`6c x.c && 6l -o x x.6`. Serve them from the Mac over the LAN and hget.

## M4 DONE — the GPU does real, verified work. The blitter is NOT a speedup.

`XY_SRC_COPY_BLT` on the blitter ring (BCS, 0x22000 — BLT moved off the render
ring at Gen6). Gen8 form is 10 dwords, per Linux i915 `intel_migrate.c`
emit_copy(); field layout from Mesa `genxml/gen5.xml` (gen6+ excludes it because
Mesa switched to BLORP). Copied 1 MB, verified all 262144 dwords.

    GPU blit (stolen mem)    : 1534 us  ( 683 MB/s)
    CPU memcpy (cached RAM)  :  180 us  (5806 MB/s)   <- fair baseline
    CPU memcpy (via aperture): 76354 us (  13 MB/s)   <- uncached, NOT fair

    GPU vs cached-RAM CPU : 0.118 x   -> the GPU is 8.5x SLOWER

Expected, not a bug: cirno's GT1 is the weakest Broadwell (Celeron, fewest EUs),
copying through stolen memory; a blit engine does not beat modern CPU SSE memcpy.
Blitters win on async offload, not throughput. Says nothing about 3D, where
parallel EUs beat per-pixel CPU shading — that is still the open question.

### TWO MEASUREMENT TRAPS (both nearly produced a false headline)

1. **HEAD==TAIL is NOT completion.** It means the command was PARSED; the blit is
   pipelined behind the parser. The first run "failed" while printing the CORRECT
   value — the check ran before the copy landed. Real fix: `MI_FLUSH_DW` with
   store-dword (`(0x26<<23)|(1<<14)|2`, addr|USE_GTT(1<<2), addr_hi, value) after
   the blit, and poll THAT fence.
2. **Never benchmark the CPU through the aperture.** It is uncached/WC device
   memory: memcpy there runs at 13 MB/s vs 5.8 GB/s in normal RAM. Comparing the
   GPU against it yields a flattering, meaningless "30-50x faster". The fair CPU
   baseline is cached RAM — which is where a CPU renderer actually works.
   Also: poll with a BUSY loop, not `sleep(1)` — Plan 9's 1ms tick is the same
   order as the blit, so sleeping measures the scheduler (2539us -> 1534us real).

## Next

M5-M8 for real OpenGL: the 22-ioctl shim (implementable ENTIRELY in userspace —
we already have MMIO, GTT, aperture and a working ring in-process, so the "kernel
half" collapses into the same process), then iris through cc9 (~160k lines, the
gl9/llvm9 grind), then measure GL vs llvmpipe. Open risk: iris wants PPGTT +
softpin while we have a 64MB GGTT window — either map softpin addresses into it
or build PPGTT page tables.
