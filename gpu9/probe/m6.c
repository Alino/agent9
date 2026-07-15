/*
 * gpu9 M6 — characterise the blitter honestly: sweep sizes to separate FIXED
 * per-submission overhead from actual THROUGHPUT.
 *
 * m4 measured 683 MB/s on a 1MB copy and I reported the GPU as "8.5x slower than
 * memcpy". But a single small copy cannot distinguish "the engine is slow" from
 * "submission costs ~1ms and the copy is 100us". If overhead dominates, the real
 * bandwidth is much higher and the honest framing changes: the GPU is not slow,
 * it is LATENCY-bound, which matters for how you'd actually use it.
 *
 * Linear fit: time(n) = overhead + n/bandwidth.
 */
#include <u.h>
#include <libc.h>

#define BAR0SZ	(16*1024*1024)
#define APSZ	0x4000000

#define GEN6_RPSTAT1		0xa01c
#define GEN6_RPNSWREQ		0xa008
#define GEN6_RP_STATE_CAP	0x145998
#define HSW_FREQUENCY(x)	((x)<<24)
#define HSW_CAGF_SHIFT		7
#define HSW_CAGF_MASK		(0x7f<<HSW_CAGF_SHIFT)
#define FORCEWAKE_MT		0xa188
#define FORCEWAKE_ACK_HSW	0x130044
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
#define MI_FLUSH_DW_GEN8	((0x26<<23)|(1<<14)|2)
#define MI_FLUSH_DW_USE_GTT	(1<<2)

#define RINGPG	2048
#define SRCPG	3072			/* GPU 0xc00000 */
#define DSTPG	11264			/* GPU 0x2c00000 — 32MB apart */
#define FENCEPG	2049
#define FENCEVAL 0x5eed0000

static u32int *mmio;
static uchar *aper;
static u32int rd(u32int o){ return mmio[o/4]; }
static void wr(u32int o, u32int v){ mmio[o/4] = v; }

static vlong
blit(u32int *ring, volatile u32int *fence, int npage, int seq)
{
	int n, i;
	vlong t0;

	n = 0;
	ring[n++] = XY_SRC_COPY_BLT_GEN8 | BLT_WRITE_RGBA | (10-2);
	ring[n++] = BLT_DEPTH_32 | BLT_ROP_SRC_COPY | 4096;
	ring[n++] = 0;
	ring[n++] = (npage<<16) | 1024;
	ring[n++] = DSTPG*4096;
	ring[n++] = 0;
	ring[n++] = 0;
	ring[n++] = 4096;
	ring[n++] = SRCPG*4096;
	ring[n++] = 0;
	ring[n++] = MI_FLUSH_DW_GEN8;
	ring[n++] = (FENCEPG*4096) | MI_FLUSH_DW_USE_GTT;
	ring[n++] = 0;
	ring[n++] = FENCEVAL+seq;
	while(n & 1) ring[n++] = MI_NOOP;

	fence[0] = 0;
	wr(BCS_CTL, 0); wr(BCS_HEAD, 0); wr(BCS_TAIL, 0);
	wr(BCS_START, RINGPG*4096);
	wr(BCS_CTL, ((1-1)<<12)|RING_VALID);
	rd(BCS_CTL);
	t0 = nsec();
	wr(BCS_TAIL, n*4);
	rd(BCS_TAIL);
	while(fence[0] != (u32int)(FENCEVAL+seq)){
		if(nsec()-t0 > 3000000000LL) return -1;
	}
	USED(i);
	return nsec() - t0;
}

void
main(int argc, char **argv)
{
	u32int *ring, *src, *dst;
	volatile u32int *fence;
	int sizes[] = { 64, 256, 1024, 4096, 8192 };	/* pages: 256KB..32MB */
	int i, k, seq = 1;
	vlong t, best;
	void *ra, *rb;

	USED(argc); USED(argv);
	mmio = segattach(0, "igfxmmio", 0, BAR0SZ);
	if(mmio == (void*)-1) sysfatal("segattach igfxmmio: %r");
	aper = segattach(0, "igfxscreen", 0, APSZ);
	if(aper == (void*)-1) sysfatal("segattach igfxscreen: %r");
	wr(FORCEWAKE_MT, MASKED_ENABLE(1));
	rd(FORCEWAKE_MT);
	for(i=0;i<500 && !(rd(FORCEWAKE_ACK_HSW)&1);i++) sleep(1);
	if(!(rd(FORCEWAKE_ACK_HSW)&1)) sysfatal("forcewake");

	/* RPS — what a real driver does. Without this the GPU sits at its 100MHz
	 * floor and every number below is 8x too low. */
	{
		int rp0 = rd(GEN6_RP_STATE_CAP) & 0xff, i;
		wr(GEN6_RPNSWREQ, HSW_FREQUENCY(rp0));
		rd(GEN6_RPNSWREQ);
		for(i=0;i<400 && ((rd(GEN6_RPSTAT1)&HSW_CAGF_MASK)>>HSW_CAGF_SHIFT) < rp0;i++)
			sleep(1);
		print("RPS: requested RP0 = %d MHz, GPU now at %d MHz\n\n", rp0*50,
			((rd(GEN6_RPSTAT1)&HSW_CAGF_MASK)>>HSW_CAGF_SHIFT)*50);
	}

	ring = (u32int*)(aper + RINGPG*4096);
	fence = (volatile u32int*)(aper + FENCEPG*4096);
	src = (u32int*)(aper + SRCPG*4096);
	dst = (u32int*)(aper + DSTPG*4096);
	for(i=0;i<4096/4;i++) ring[i] = MI_NOOP;
	for(i=0;i<1024;i++) src[i] = 0xa5a50000 + i;

	print("bytes      GPU blit        -> MB/s      | CPU memcpy (cached RAM) -> MB/s\n");
	for(k = 0; k < nelem(sizes); k++){
		int np = sizes[k];
		vlong nb = (vlong)np*4096;
		best = 0;
		for(i = 0; i < 8; i++){
			t = blit(ring, fence, np, seq++);
			if(t < 0){ print("%lld  GPU HUNG\n", nb); goto cpu; }
			if(best == 0 || t < best) best = t;
		}
		print("%8lld  %7lld us  -> %7.1f MB/s | ", nb, best/1000,
			(double)nb/(double)best*1000.0);
		/* fair CPU baseline: cached RAM, same byte count */
		ra = malloc(nb); rb = malloc(nb);
		if(ra && rb){
			vlong bc = 0;
			memset(ra, 0x5a, nb);
			memmove(rb, ra, nb);
			for(i=0;i<5;i++){
				vlong t0 = nsec();
				memmove(rb, ra, nb);
				t = nsec()-t0;
				if(bc==0 || t<bc) bc = t;
			}
			print("%7lld us -> %7.1f MB/s  (GPU %.2fx)\n", bc/1000,
				(double)nb/(double)bc*1000.0, (double)bc/(double)best);
			free(ra); free(rb);
		} else
			print("(malloc failed)\n");
	}
cpu:
	wr(FORCEWAKE_MT, MASKED_DISABLE(1));
	exits(nil);
}
