/*
 * gpu9 M4 — real GPU work, measured: XY_SRC_COPY_BLT on the blitter ring.
 *
 * Gen8 encoding per Linux i915 intel_migrate.c emit_copy() (ver >= 8), 10 dwords:
 *   DW0 (2<<29)|(0x53<<22)|BLT_WRITE_RGBA|(10-2)
 *   DW1 BLT_DEPTH_32 | BLT_ROP_SRC_COPY | dst_pitch
 *   DW2 dst_y1<<16|dst_x1     DW3 dst_y2<<16|dst_x2
 *   DW4 dst_lo  DW5 dst_hi    DW6 src_y1<<16|src_x1
 *   DW7 src_pitch             DW8 src_lo  DW9 src_hi
 * Trick from i915: treat the buffer as 1024px-wide 32bpp rows of 4096B pitch, so
 * height == number of pages. BLT lives on the BLITTER ring (BCS) since Gen6.
 */
#include <u.h>
#include <libc.h>

#define BAR0SZ	(16*1024*1024)
#define APSZ	0x4000000

#define FORCEWAKE_MT		0xa188
#define FORCEWAKE_ACK_HSW	0x130044
#define FW_KERNEL		1
#define MASKED_ENABLE(x)	((x)|((x)<<16))
#define MASKED_DISABLE(x)	((x)<<16)

#define BCS_TAIL	0x22030
#define BCS_HEAD	0x22034
#define BCS_START	0x22038
#define BCS_CTL		0x2203c
#define RING_VALID	1
#define MI_NOOP		0

#define BLT_WRITE_RGBA		(3<<20)
#define BLT_DEPTH_32		(3<<24)
#define BLT_ROP_SRC_COPY	(0xcc<<16)
#define XY_SRC_COPY_BLT_GEN8	((2<<29)|(0x53<<22))
/* MI_FLUSH_DW (gen8, storing): flush blit writes, then post a seqno we can poll.
 * HEAD==TAIL is NOT completion — the blit is pipelined behind the parser. */
#define MI_FLUSH_DW_GEN8	((0x26<<23)|(1<<14)|2)	/* op=store dword, len=4dw */
#define MI_FLUSH_DW_USE_GTT	(1<<2)

#define RINGPG	2048			/* GPU 0x800000 */
#define SRCPG	2560			/* GPU 0xa00000 */
#define DSTPG	3072			/* GPU 0xc00000 */
#define FENCEPG	4096			/* GPU 0x1000000 */
#define FENCEVAL 0x5eed1234
#define NPAGE	256			/* 1 MB */
#define NBYTE	(NPAGE*4096)

static u32int *mmio;
static uchar *aper;
static u32int rd(u32int o){ return mmio[o/4]; }
static void wr(u32int o, u32int v){ mmio[o/4] = v; }

static int
fwget(void)
{
	int i;
	wr(FORCEWAKE_MT, MASKED_ENABLE(FW_KERNEL));
	rd(FORCEWAKE_MT);
	for(i=0;i<500;i++){ if(rd(FORCEWAKE_ACK_HSW)&FW_KERNEL) return 1; sleep(1); }
	return 0;
}

void
main(int argc, char **argv)
{
	u32int *ring, *src, *dst, *fence;
	int i, n, ok;
	vlong t0, tgpu, tcpu;

	USED(argc); USED(argv);
	mmio = segattach(0, "igfxmmio", 0, BAR0SZ);
	if(mmio == (void*)-1) sysfatal("segattach igfxmmio: %r");
	aper = segattach(0, "igfxscreen", 0, APSZ);
	if(aper == (void*)-1) sysfatal("segattach igfxscreen: %r");
	if(!fwget()) sysfatal("forcewake failed");

	ring = (u32int*)(aper + RINGPG*4096);
	src  = (u32int*)(aper + SRCPG*4096);
	dst  = (u32int*)(aper + DSTPG*4096);
	fence = (u32int*)(aper + FENCEPG*4096);
	fence[0] = 0;

	for(i=0;i<NBYTE/4;i++) src[i] = 0x1000 + i;
	for(i=0;i<NBYTE/4;i++) dst[i] = 0;
	print("src/dst prepared: %d KB\n", NBYTE/1024);

	for(i=0;i<4096/4;i++) ring[i] = MI_NOOP;
	n = 0;
	ring[n++] = XY_SRC_COPY_BLT_GEN8 | BLT_WRITE_RGBA | (10-2);
	ring[n++] = BLT_DEPTH_32 | BLT_ROP_SRC_COPY | 4096;	/* dst pitch */
	ring[n++] = 0;						/* dst y1,x1 */
	ring[n++] = (NPAGE<<16) | 1024;				/* dst y2,x2 (px) */
	ring[n++] = DSTPG*4096;					/* dst lo */
	ring[n++] = 0;						/* dst hi */
	ring[n++] = 0;						/* src y1,x1 */
	ring[n++] = 4096;					/* src pitch */
	ring[n++] = SRCPG*4096;					/* src lo */
	ring[n++] = 0;						/* src hi */
	/* completion fence AFTER the blit, in the same ring: ordered + flushed */
	ring[n++] = MI_FLUSH_DW_GEN8;
	ring[n++] = (FENCEPG*4096) | MI_FLUSH_DW_USE_GTT;
	ring[n++] = 0;
	ring[n++] = FENCEVAL;
	while(n & 1) ring[n++] = MI_NOOP;

	wr(BCS_CTL, 0); wr(BCS_HEAD, 0); wr(BCS_TAIL, 0);
	wr(BCS_START, RINGPG*4096);
	wr(BCS_CTL, ((1-1)<<12)|RING_VALID);
	rd(BCS_CTL);
	print("BCS armed: START %.8ux CTL %.8ux\n", rd(BCS_START), rd(BCS_CTL));

	{
		int run;
		vlong best = 0, t;
		for(run = 0; run < 10; run++){
			fence[0] = 0;
			for(i=0;i<NBYTE/4;i++) dst[i] = 0;
			wr(BCS_CTL, 0); wr(BCS_HEAD, 0); wr(BCS_TAIL, 0);
			wr(BCS_START, RINGPG*4096);
			wr(BCS_CTL, ((1-1)<<12)|RING_VALID);
			rd(BCS_CTL);
			t0 = nsec();
			wr(BCS_TAIL, n*4);
			rd(BCS_TAIL);
			/* pure busy-poll: sleep(1) is 1ms on Plan 9, same order as the
			 * blit — sleeping measures the scheduler, not the GPU */
			while(fence[0] != FENCEVAL)
				;
			t = nsec() - t0;
			if(best == 0 || t < best) best = t;
		}
		tgpu = best;
		print("GPU blit best of 10: %lld us\n", tgpu/1000);
	}

	ok = 1;
	for(i=0;i<NBYTE/4;i++)
		if(dst[i] != (u32int)(0x1000+i)){ ok = 0; break; }
	print("HEAD %.8ux TAIL %.8ux\n", rd(BCS_HEAD), rd(BCS_TAIL));
	if(!ok){ print("M4 FAIL: dst[%d] = %.8ux\n", i, dst[i]); exits("blt"); }

	/* Two CPU baselines. The aperture is UNCACHED/WC device memory, so a memcpy
	 * through it is crippled — comparing the GPU to that would flatter the GPU
	 * with a number that means nothing. The honest baseline is normal cached RAM,
	 * which is what a CPU renderer (llvmpipe/softpipe) actually works in. */
	for(i=0;i<NBYTE/4;i++) dst[i] = 0;
	t0 = nsec();
	memmove(dst, src, NBYTE);
	tcpu = nsec() - t0;
	{
		void *ra, *rb;
		vlong tram;
		ra = malloc(NBYTE); rb = malloc(NBYTE);
		if(ra == nil || rb == nil) sysfatal("malloc");
		memset(ra, 0x5a, NBYTE);
		memmove(rb, ra, NBYTE);		/* warm */
		t0 = nsec();
		memmove(rb, ra, NBYTE);
		tram = nsec() - t0;
		print("\n--- honest comparison, %d KB ---\n", NBYTE/1024);
		print("GPU blit (stolen mem)   : %6lld us  (%g MB/s)\n",
			tgpu/1000, (double)NBYTE/(double)tgpu*1000.0);
		print("CPU memcpy (cached RAM) : %6lld us  (%g MB/s)  <- fair baseline\n",
			tram/1000, (double)NBYTE/(double)tram*1000.0);
		print("CPU memcpy (via aperture): %5lld us  (%g MB/s)  <- uncached, NOT a fair baseline\n",
			tcpu/1000, (double)NBYTE/(double)tcpu*1000.0);
		print("\nGPU vs cached-RAM CPU : %g x\n", (double)tram/(double)tgpu);
		print("GPU vs aperture CPU   : %g x  (flattering, ignore)\n", (double)tcpu/(double)tgpu);
		free(ra); free(rb);
	}

	print("*** M4 PASS: GPU blitted %d KB correctly ***\n", NBYTE/1024);
	wr(FORCEWAKE_MT, MASKED_DISABLE(FW_KERNEL));
	exits(nil);
}
