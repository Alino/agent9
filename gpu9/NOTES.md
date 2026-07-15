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
- **M2 — GPU memory for free (but LESS than M2 claimed).** The BIOS GTT-maps the
  aperture (M2 said "16384/16384 PTEs live" — that scan looped `i < APSZ/4096`,
  so it only ever looked at the 64MB it had already assumed, and it tested only
  PTE bit0, which uninitialised junk sets half the time. See M11-M15 at the
  bottom: 259 of those 16384 pages are not usable memory.) to contiguous stolen memory at 0xa4000000,
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

## ⭐ THE HEADLINE: the GPU was asleep. RPS makes it 6.7x faster.

**Nothing on 9front runs RPS (Render P-State), so Broadwell sits at its 100MHz
floor forever — 1/8 of the 800MHz it can do.** Requesting RP0 (what a real driver
does: read RP_STATE_CAP, write RPNSWREQ) is the single biggest performance factor
on this box, and it is now part of `gpu9_open()`.

    blitter, 4MB copy:
      @100 MHz (BIOS default)  6129 us ->  684 MB/s
      @200 MHz                 3108 us -> 1349 MB/s
      @400 MHz                 1599 us -> 2623 MB/s
      @600 MHz                 1147 us -> 3654 MB/s
      @800 MHz (RP0)            915 us -> 4582 MB/s     <- 6.7x, linear in clock

**Honest head-to-head at full clock (GPU is flat; the CPU falls out of cache):**

    bytes      GPU blit      CPU memcpy (cached RAM)   verdict
      256K   4602 MB/s       8464 MB/s                 GPU 0.54x
        1M   4631 MB/s       6115 MB/s                 GPU 0.76x
        4M   4576 MB/s       4204 MB/s                 GPU 1.09x  <- crossover
       16M   4563 MB/s       4084 MB/s                 GPU 1.12x
       32M   4504 MB/s       4100 MB/s                 GPU 1.10x

So: the CPU wins while the data fits in cache; **beyond ~4MB the GPU wins — and
does it asynchronously, leaving the CPU free.** This also means every earlier
gpu9 number was taken at 1/8 clock, and that whenever iris lands, 3D would have
been 8x slower than the hardware can do.

BDW/HSW register traps (wrong shift = you read garbage and conclude nonsense):
  RPNSWREQ: freq in bits **31:24** (HSW_FREQUENCY), not 31:25
  RPSTAT1 : CAGF at shift **7** (HSW_CAGF_SHIFT), not 8 — both in 50MHz units

Dead ends worth not repeating: it is NOT overhead (throughput was flat 684 MB/s
from 256KB to 32MB — perfectly linear, zero fixed cost) and NOT cache policy
(PAT[0] WB/eLLC-override vs i915's WB|LLC vs WB|LLCeLLC: 684.4 / 684.1 / 684.4
MB/s — identical). It was always the clock.

## M4 DONE — the GPU does real, verified work (numbers above supersede these)

`XY_SRC_COPY_BLT` on the blitter ring (BCS, 0x22000 — BLT moved off the render
ring at Gen6). Gen8 form is 10 dwords, per Linux i915 `intel_migrate.c`
emit_copy(); field layout from Mesa `genxml/gen5.xml` (gen6+ excludes it because
Mesa switched to BLORP). Copied 1 MB, verified all 262144 dwords.

    GPU blit (stolen mem)    : 1534 us  ( 683 MB/s)
    CPU memcpy (cached RAM)  :  180 us  (5806 MB/s)   <- fair baseline
    CPU memcpy (via aperture): 76354 us (  13 MB/s)   <- uncached, NOT fair

    GPU vs cached-RAM CPU : 0.118 x   -> the GPU is 8.5x SLOWER

**^ That conclusion was WRONG, and instructively so.** I blamed the hardware
("cirno's GT1 is the weakest Broadwell, blitters do not beat SSE memcpy") when the
real cause was that the GPU was running at 1/8 clock because *I* had not written
the RPS code a driver owes it. Attributing a measurement to inherent hardware
limits is a comfortable story; it stopped me looking. The flat-with-size curve was
the clue that eventually cracked it.

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

**The seam is confirmed as a single POSIX call.** `intel_gem.h:77` is literally:

    static inline int intel_ioctl(int fd, unsigned long request, void *arg)
    { do { ret = ioctl(fd, request, arg); } while (...EINTR/EAGAIN); return ret; }

Plain `ioctl()` — NOT libdrm's drmIoctl. So implementing `ioctl()` in cc9's
runtime (posix_llvm.c already exists for exactly this) routes iris to our driver
with iris UNMODIFIED. And because M1-M3 proved MMIO + GTT + ring all work from
userspace, the "kernel half" collapses into the SAME PROCESS: no kernel device,
no /dev/i915, no context switch. iris -> ioctl() -> our code -> the ring.

**Expectation-setting on the payoff (do the arithmetic before the work):**
cirno's BDW GT1 is ~12 EUs @ ~700MHz ~= 134 GFLOPS. The Celeron 3205U is 2 cores
@ 1.5GHz with AVX2 ~= 48 GFLOPS. So the GPU has only ~2.8x the raw FLOPS, plus
fixed-function texture/raster units, minus shared stolen-memory bandwidth. A
realistic 3D win over llvmpipe is maybe **3-5x, not 15x**. The M4 blitter result
(8.5x SLOWER than memcpy) is the same story: this is the weakest Broadwell made.
That is worth knowing BEFORE spending weeks.

M5-M8 for real OpenGL: the 22-ioctl shim (implementable ENTIRELY in userspace —
we already have MMIO, GTT, aperture and a working ring in-process, so the "kernel
half" collapses into the same process), then iris through cc9 (~160k lines, the
gl9/llvm9 grind), then measure GL vs llvmpipe. Open risk: iris wants PPGTT +
softpin while we have a 64MB GGTT window — either map softpin addresses into it
or build PPGTT page tables.


## M11-M15 (2026-07-15) — measuring what M2 assumed

M2's "the whole 64MB aperture is GPU memory" was an assumption that checked
itself. Four probes to settle it, after `gpu9 bench` was caught reporting
4555 MB/s for a 16MB copy that never finished:

- **m11 — the aperture really IS 64MB.** Bisects `segattach("igfxscreen")` for
  the size the kernel actually registers: 67108864. So the hardcode was right
  and my suspicion (PCI reports BAR2 = 256MB) was wrong — 256MB is the decode
  window, not what we get. The GGTT matches it exactly: GTT[16384] already reads
  uninitialised junk.
  m11's first cut then got it wrong the other way: it measured the "maximal
  linear PREFIX" from GTT[0], hit an anomaly at 8190, and concluded 31MB of 64MB
  was real. Two odd entries in the middle do not mean the rest is unbacked.
- **m12/m13 — 3 of 16384 PTEs are not the linear stolen mapping**: 8190 and 8191
  (at the 32MB mark, holding `0309400103093001` / `0309600103095001`) and 16383.
  16381 of 16384 DO match `0xa4000000 + i*4096`, so the linear-mapping story is
  right; it just has holes. `gpu9 bench` allocated its dst across 8190/8191.
- **m14 — the blit fails at an ADDRESS, not a size.** 3840 pages copy correctly;
  4000 and 4090 both fail at the *same absolute dword*, GGTT `0x3F00000` — the
  63MB edge.
- **m15 — mapped is not yours.** Non-destructive write/readback (save, write,
  compare, restore) over every aperture page: the top 1MB (16128..16383) does
  **not hold writes**, behind PTEs identical to every other. Firmware lives
  there. This is why m14 stopped exactly there.

**The lesson worth keeping: a present, perfectly-linear PTE is not evidence the
page is yours.** Only touching the memory is. `gpu9_arena()` now checks the PTE
first (never write through a PTE pointing somewhere unknown — that is how you
DMA into something you cannot name) and then proves each page by write/readback,
taking the largest run that survives both: pages 8192..16127, 31MB.

And the meta-lesson: **an unverified benchmark is a rumour.** The 16MB row was
wrong for weeks and looked like the best result in the table. Verification
turned "1.11x at 16MB" into "ties at 4MB, ~8% ahead past it", and `gpu9 rps`
now re-derives the 6.7x headline on the box instead of quoting a number.
