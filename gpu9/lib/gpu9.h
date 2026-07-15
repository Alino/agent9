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
#define GPU9_APERTURE_SZ	0x4000000u	/* 64MB, per /dev/vgactl */

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

	/* the render ring */
	g9u32 ring_page;
	g9u32 *ring;
	g9u32 ring_dwords;
	g9u32 tail;

	g9u32 fence_page;
	volatile g9u32 *fence;
	g9u32 seqno;
};

int   gpu9_open(Gpu9 *g);
void  gpu9_close(Gpu9 *g);
int   gpu9_forcewake_get(Gpu9 *g);
void  gpu9_forcewake_put(Gpu9 *g);

/* allocate GPU-visible memory. Returns the GGTT address; *cpu gets the CPU
 * pointer (the same bytes, through the aperture). */
g9u32 gpu9_alloc(Gpu9 *g, g9u32 bytes, void **cpu);
void    *gpu9_cpu(Gpu9 *g, g9u32 ggtt);

/* submit a batch buffer at a GGTT address on the render ring, and wait for it
 * to actually complete (MI_FLUSH_DW fence, not HEAD==TAIL). */
int  gpu9_exec(Gpu9 *g, g9u32 batch_ggtt, g9u32 nbytes);

static inline g9u32 gpu9_rd(Gpu9 *g, g9u32 o){ return g->mmio[o/4]; }
static inline void     gpu9_wr(Gpu9 *g, g9u32 o, g9u32 v){ g->mmio[o/4] = v; }

#endif
