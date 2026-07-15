/*
 * gpu9 M8 — A/B the PAT LLC bits against blit throughput.
 *
 * The BIOS left PAT[0] = 0x03 (WB, llc-field=0 = "eLLC override"). Linux i915
 * uses GEN8_PPAT_WB|GEN8_PPAT_LLC = 0x07 for normal objects. cirno's Celeron has
 * NO eLLC, so "eLLC override" may mean the GPU never uses the LLC at all — which
 * would explain a flat, memory-bound 684 MB/s.
 *
 * Same blit, only PAT[0] changes. Restores the BIOS value before exit: PAT is a
 * global register and the display's framebuffer reads go through it too.
 */
#include <u.h>
#include <libc.h>

#define BAR0SZ	(16*1024*1024)
#define APSZ	0x4000000
#define FORCEWAKE_MT		0xa188
#define FORCEWAKE_ACK_HSW	0x130044
#define GEN8_PRIVATE_PAT_LO	0x40e0
#define GEN8_PRIVATE_PAT_HI	0x40e4

#define BCS_TAIL	0x22030
#define BCS_HEAD	0x22034
#define BCS_START	0x22038
#define BCS_CTL		0x2203c
#define MI_NOOP		0
#define BLT_WRITE_RGBA		(3<<20)
#define BLT_DEPTH_32		(3<<24)
#define BLT_ROP_SRC_COPY	(0xcc<<16)
#define XY_SRC_COPY_BLT_GEN8	((2<<29)|(0x53<<22))
#define MI_FLUSH_DW_GEN8	((0x26<<23)|(1<<14)|2)
#define MI_FLUSH_DW_USE_GTT	(1<<2)

#define RINGPG	2048
#define FENCEPG	2049
#define SRCPG	3072
#define DSTPG	11264
#define NPAGE	1024			/* 4 MB */
#define FENCEVAL 0x5eed0000

static u32int *mmio;
static uchar *aper;
static u32int rd(u32int o){ return mmio[o/4]; }
static void wr(u32int o, u32int v){ mmio[o/4] = v; }

static vlong
blit(u32int *ring, volatile u32int *fence, int seq)
{
	int n;
	vlong t0;
	n = 0;
	ring[n++] = XY_SRC_COPY_BLT_GEN8 | BLT_WRITE_RGBA | (10-2);
	ring[n++] = BLT_DEPTH_32 | BLT_ROP_SRC_COPY | 4096;
	ring[n++] = 0;
	ring[n++] = (NPAGE<<16) | 1024;
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
	wr(BCS_CTL, 1);
	rd(BCS_CTL);
	t0 = nsec();
	wr(BCS_TAIL, n*4);
	rd(BCS_TAIL);
	while(fence[0] != (u32int)(FENCEVAL+seq))
		if(nsec()-t0 > 3000000000LL) return -1;
	return nsec()-t0;
}

static vlong
bench(u32int *ring, volatile u32int *fence, int *seq)
{
	vlong t, best = 0;
	int i;
	for(i = 0; i < 8; i++){
		t = blit(ring, fence, (*seq)++);
		if(t < 0) return -1;
		if(best == 0 || t < best) best = t;
	}
	return best;
}

void
main(int argc, char **argv)
{
	u32int *ring, *src, save_lo, save_hi;
	volatile u32int *fence;
	vlong t;
	int i, seq = 1;
	vlong nb = (vlong)NPAGE*4096;

	USED(argc); USED(argv);
	mmio = segattach(0, "igfxmmio", 0, BAR0SZ);
	if(mmio == (void*)-1) sysfatal("segattach igfxmmio: %r");
	aper = segattach(0, "igfxscreen", 0, APSZ);
	if(aper == (void*)-1) sysfatal("segattach igfxscreen: %r");
	wr(FORCEWAKE_MT, (1<<16)|1); rd(FORCEWAKE_MT);
	for(i=0;i<500 && !(rd(FORCEWAKE_ACK_HSW)&1);i++) sleep(1);

	ring = (u32int*)(aper + RINGPG*4096);
	fence = (volatile u32int*)(aper + FENCEPG*4096);
	src = (u32int*)(aper + SRCPG*4096);
	for(i=0;i<4096/4;i++) ring[i] = MI_NOOP;
	for(i=0;i<1024;i++) src[i] = 0x5a5a0000+i;

	save_lo = rd(GEN8_PRIVATE_PAT_LO);
	save_hi = rd(GEN8_PRIVATE_PAT_HI);
	print("BIOS PAT = %.8ux_%.8ux\n", save_hi, save_lo);

	t = bench(ring, fence, &seq);
	print("PAT[0]=0x03 (BIOS: WB, eLLC-override): %6lld us -> %7.1f MB/s\n",
		t/1000, (double)nb/(double)t*1000.0);

	/* i915's value for normal objects: WB | LLC */
	wr(GEN8_PRIVATE_PAT_LO, (save_lo & ~0xff) | 0x07);
	rd(GEN8_PRIVATE_PAT_LO);
	t = bench(ring, fence, &seq);
	print("PAT[0]=0x07 (i915:  WB | LLC)       : %6lld us -> %7.1f MB/s\n",
		t/1000, (double)nb/(double)t*1000.0);

	/* WB | LLCeLLC (3<<2)|3 = 0x0f */
	wr(GEN8_PRIVATE_PAT_LO, (save_lo & ~0xff) | 0x0f);
	rd(GEN8_PRIVATE_PAT_LO);
	t = bench(ring, fence, &seq);
	print("PAT[0]=0x0f (WB | LLCeLLC)          : %6lld us -> %7.1f MB/s\n",
		t/1000, (double)nb/(double)t*1000.0);

	wr(GEN8_PRIVATE_PAT_LO, save_lo);
	wr(GEN8_PRIVATE_PAT_HI, save_hi);
	rd(GEN8_PRIVATE_PAT_LO);
	print("PAT restored to BIOS value\n");
	wr(FORCEWAKE_MT, 1<<16);
	exits(nil);
}
