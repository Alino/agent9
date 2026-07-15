/*
 * gpu9 — a Broadwell (Intel Gen8) GPU driver that lives entirely in userspace.
 *
 * There is no kernel component. 9front already hands userspace everything:
 *   segattach("igfxmmio")   -> BAR0: registers + the GTT (Gen8: GTT at +8MB)
 *   segattach("igfxscreen") -> the aperture: a CPU window THROUGH the GTT, so
 *                              aperture offset == GGTT address (no phys addrs)
 * The BIOS already GTT-maps the whole 64MB aperture to contiguous stolen memory,
 * so we get GPU-visible memory for free. Proven by gpu9/probe/m1..m4.
 *
 * Mesa's iris reaches the kernel through exactly one function — intel_gem.h's
 * intel_ioctl(), a plain POSIX ioctl(). So gpu9 implements ioctl() and the
 * "kernel half" collapses into the same process:
 *
 *     iris (unmodified) -> ioctl(fd, DRM_IOCTL_I915_*, arg) -> gpu9 -> the ring
 */
#ifndef GPU9_H
#define GPU9_H

/* Types: native Plan 9 (kencc) has no <stdint.h> — it has u32int from <u.h>,
 * which callers include first. cc9 (the clang cross-toolchain that will build
 * iris) does have stdint.h, so map onto it there. One header, both worlds. */
#ifdef __STDC_HOSTED__
#include <stdint.h>
typedef uint32_t g9u32;
typedef uint8_t  g9u8;
#else
typedef u32int g9u32;
typedef uchar  g9u8;
#endif

/* ---- MMIO map (Gen8 / Broadwell) ---- */
#define GPU9_BAR0SZ		(16u*1024*1024)
#define GPU9_GTT_OFFSET		(8u*1024*1024)	/* GTT lives at BAR0 + 8MB */
/* 64MB. MEASURED, not assumed (probe/m11 bisects segattach): this is the size
 * the kernel registers for "igfxscreen", and the GGTT is exactly the same
 * 16384 PTEs — GTT[16384] already reads uninitialised junk. NB PCI reports
 * BAR2 = 256MB; that is the decode window, not what we get. */
#define GPU9_APERTURE_SZ	0x4000000u
#define GPU9_GTT_ENTRIES	(GPU9_APERTURE_SZ/4096)		/* 16384 */
/* The BIOS maps stolen memory linearly from here — but NOT every page; see
 * gpu9_arena(). Never assume: probe/m13 found 3 entries that are not this. */
#define GPU9_STOLEN_BASE	0xa4000000ULL

/* forcewake: HSW/BDW use the MT request + the HSW ack. Render regs (0x2000+)
 * read 0 until the GT is woken — verified in probe/m0.c. */
#define GPU9_FORCEWAKE_MT	0xa188
#define GPU9_FORCEWAKE_ACK_HSW	0x130044
#define GPU9_FW_KERNEL		1

/* rings. RCS = 3D/render, BCS = blitter (BLT moved off RCS at Gen6). */
#define GPU9_RCS_BASE		0x2000
#define GPU9_BCS_BASE		0x22000
#define GPU9_RING_TAIL		0x30
#define GPU9_RING_HEAD		0x34
#define GPU9_RING_START		0x38
#define GPU9_RING_CTL		0x3c
#define GPU9_RING_VALID		1

#define GPU9_GFX_MODE_GEN7	0x229c		/* bit15 = execlist enable (we need it OFF) */
#define GPU9_GFX_FLSH_CNTL	0x101008	/* write 1 = invalidate the GGTT TLB */

/* RPS (Render P-State). THE single biggest performance factor on this box.
 * With no driver, nothing ever requests a frequency and Broadwell sits at its
 * 100MHz floor — 1/8 of the 800MHz it can do. Managing the P-state is a real
 * driver's job; doing it made the blitter 6.7x faster (684 -> 4582 MB/s) and is
 * the difference between the GPU losing to memcpy and beating it.
 *
 * BDW/HSW register details that differ from gen6 (wrong = you read garbage):
 *   RPNSWREQ: freq in bits 31:24 (HSW_FREQUENCY), NOT 31:25
 *   RPSTAT1 : CAGF at shift 7 (HSW_CAGF_SHIFT), NOT 8
 * All frequencies are in 50MHz units. */
#define GPU9_RPSTAT1		0xa01c
#define GPU9_RPNSWREQ		0xa008
#define GPU9_RP_STATE_CAP	0x145998	/* RP0=byte0 max, RP1=byte1, RPn=byte2 */
#define GPU9_HSW_FREQUENCY(x)	((x)<<24)
#define GPU9_HSW_CAGF_SHIFT	7
#define GPU9_HSW_CAGF_MASK	(0x7fu<<GPU9_HSW_CAGF_SHIFT)

/* ---- commands (verified working in probe/m3, m4) ---- */
#define MI_NOOP			0u
#define MI_BATCH_BUFFER_END	(0x0au << 23)
/* MI_STORE_DATA_IMM: hdr | addr_lo | addr_hi | data. bit22 = use GGTT. */
#define MI_STORE_DWORD_IMM_GEN8	((0x20u<<23) | (1u<<22) | 2u)
/* MI_BATCH_BUFFER_START (gen8): hdr | addr_lo | addr_hi. bit8 = use GGTT (1=ppgtt) */
#define MI_BATCH_BUFFER_START_GEN8 ((0x31u<<23) | 1u)
/* Completion signalling. HEAD==TAIL means PARSED, not done — the engine is
 * pipelined behind the parser — so we always post a seqno and wait on THAT.
 *
 * The command differs BY ENGINE, and getting this wrong fails silently:
 *   BCS (blitter/media): MI_FLUSH_DW
 *   RCS (render):        PIPE_CONTROL     <- MI_FLUSH_DW is not a render command;
 *                        on RCS it is consumed but never posts the write, so the
 *                        fence never lands and it looks exactly like a GPU hang.
 */
#define MI_FLUSH_DW_GEN8	((0x26u<<23) | (1u<<14) | 2u)	/* BCS only */
#define MI_FLUSH_DW_USE_GTT	(1u<<2)

/* 2D blitter ops (XY_COLOR_BLT / XY_SRC_COPY_BLT). Encodings from
 * genxml/gen5.xml — the 2D BLT format is unchanged gen4..gen8 EXCEPT gen8
 * widened the base address to 64-bit (so one extra dword each). Verified byte
 * layout matches gpu9's working SRC_COPY (ROP<<16, depth<<24, pitch in low 16).
 * These are why a 2D GPU exists: the CPU reaches the framebuffer only through
 * the uncached aperture, the blitter is native to it. */
#define GPU9_XY_COLOR_BLT	((2u<<29)|(0x50u<<22))	/* solid fill */
#define GPU9_XY_SRC_COPY_BLT	((2u<<29)|(0x53u<<22))	/* copy (scroll) */
#define GPU9_BLT_WRITE_RGBA	(3u<<20)
#define GPU9_ROP_FILL		(0xF0u<<16)		/* PATCOPY: dest = pattern */
#define GPU9_ROP_COPY		(0xCCu<<16)		/* SRCCOPY: dest = source */
#define GPU9_DEPTH16		1u			/* r5g6b5 — the framebuffer */
#define GPU9_DEPTH32		3u			/* x8r8g8b8 */

/* PIPE_CONTROL (gen8, 6 dwords) per genxml/gen8.xml:
 *   DW0 = type3<<29 | subtype3<<27 | opcode2<<24 | len(=6-2)
 *   DW1 flags: PostSyncOp(46:47)=1 -> bit14 ; DAT(56)=GGTT(1) -> bit24
 *   DW2/3 = address (66:111), DW4/5 = immediate data (128:191, 64-bit) */
#define PIPE_CONTROL_GEN8	((3u<<29)|(3u<<27)|(2u<<24)|4u)
#define PC_POST_SYNC_WRITE_IMM	(1u<<14)
#define PC_DAT_GGTT		(1u<<24)
#define PC_CS_STALL		(1u<<20)
#define PC_RT_CACHE_FLUSH	(1u<<12)
#define PC_DC_FLUSH		(1u<<5)

typedef struct Gpu9 Gpu9;
struct Gpu9 {
	volatile g9u32 *mmio;	/* BAR0 */
	g9u8 *aper;			/* aperture == GGTT window */
	g9u32 apsz;

	/* bump allocator over the aperture. Page 0.. is the live framebuffer, so
	 * we start well clear of it. ponytail: no free list — a bump allocator is
	 * enough to answer "does 3D work and how fast"; add reuse if a real
	 * workload churns buffers. */
	g9u32 next_page;
	g9u32 end_page;

	/* the render ring (RCS): 3D/compute batches, gpu9_exec */
	g9u32 ring_page;
	g9u32 *ring;
	g9u32 ring_dwords;
	g9u32 tail;

	g9u32 fence_page;
	volatile g9u32 *fence;
	g9u32 seqno;

	/* the blitter ring (BCS): 2D fill/copy, gpu9_fill/gpu9_blt */
	g9u32 bring_page;
	g9u32 *bring;
	g9u32 bfence_page;
	volatile g9u32 *bfence;
	g9u32 bseq;
};

int   gpu9_open(Gpu9 *g);
void  gpu9_close(Gpu9 *g);
int   gpu9_forcewake_get(Gpu9 *g);
void  gpu9_forcewake_put(Gpu9 *g);

/* Frequency control. gpu9_max_clock() asks for RP0 and is what makes the GPU
 * worth using at all. Returns the MHz actually reached.
 * ponytail: pins the clock high for the process's lifetime rather than tracking
 * load like i915's RPS does — it burns power while idle. Fine for a benchmark or
 * a render loop; add up/down thresholds (RP_CONTROL + RP_UP/DOWN_EI) if gpu9
 * ever runs in the background. */
int   gpu9_cur_clock(Gpu9 *g);		/* MHz */
int   gpu9_max_clock(Gpu9 *g);		/* request RP0, return MHz reached */
int   gpu9_set_clock(Gpu9 *g, int rp);	/* request a P-state (50MHz units) */
int   gpu9_rp0(Gpu9 *g);		/* max P-state */
int   gpu9_rpn(Gpu9 *g);		/* min P-state — where the BIOS leaves it */

/* allocate GPU-visible memory. Returns the GGTT address; *cpu gets the CPU
 * pointer (the same bytes, through the aperture). */
g9u32 gpu9_alloc(Gpu9 *g, g9u32 bytes, void **cpu);
void    *gpu9_cpu(Gpu9 *g, g9u32 ggtt);

/* submit a batch buffer at a GGTT address on the render ring, and wait for it
 * to actually complete (MI_FLUSH_DW fence, not HEAD==TAIL). */
int  gpu9_exec(Gpu9 *g, g9u32 batch_ggtt, g9u32 nbytes);

/* 2D acceleration — what makes gpu9 worth installing on a box with no hw accel.
 * Both target any GGTT address (framebuffer = GGTT 0) with a byte pitch, and
 * block until the blitter has finished. Coordinates in pixels, w/h in pixels.
 * Return 0, or -1 if the blitter hung.
 *
 *   gpu9_fill  solid rectangle (clear, background, filled shape)
 *   gpu9_blt   copy a rectangle dst<-src (scroll = copy the screen onto itself
 *              shifted; blit a window's backing store to the screen)
 */
int  gpu9_fill(Gpu9 *g, g9u32 dst, int pitch, int x, int y, int w, int h,
	g9u32 color, int depth);
int  gpu9_blt(Gpu9 *g, g9u32 dst, int dx, int dy, g9u32 src, int sx, int sy,
	int pitch, int w, int h, int depth);

static inline g9u32 gpu9_rd(Gpu9 *g, g9u32 o){ return g->mmio[o/4]; }
static inline void     gpu9_wr(Gpu9 *g, g9u32 o, g9u32 v){ g->mmio[o/4] = v; }

#endif
