/*
 * gpu9 M10 — RPS: request the GPU's real clock, then re-measure.
 *
 * The GPU has been at 100MHz of an 800MHz max because nothing runs RPS. i915
 * reads RP_STATE_CAP and REQUESTS a frequency; with no driver, nobody asks.
 * That is a real driver's job and it is why the blit was a flat 684 MB/s.
 *
 * BDW register details that differ from gen6 (getting these wrong reads garbage):
 *   RPNSWREQ  : BDW/HSW put the freq in bits 31:24 (HSW_FREQUENCY), NOT 31:25
 *   RPSTAT1   : BDW/HSW read CAGF at shift 7 (HSW_CAGF_SHIFT), NOT 8
 * Frequencies are in 50MHz units.
 */
#include <u.h>
#include <libc.h>

#define BAR0SZ	(16*1024*1024)
#define APSZ	0x4000000
#define FORCEWAKE_MT		0xa188
#define FORCEWAKE_ACK_HSW	0x130044
#define GEN6_RPSTAT1		0xa01c
#define GEN6_RPNSWREQ		0xa008
#define GEN6_RP_STATE_CAP	0x145998
#define GEN6_RP_CONTROL		0xa024
#define HSW_FREQUENCY(x)	((x)<<24)
#define HSW_CAGF_SHIFT		7
#define HSW_CAGF_MASK		(0x7f<<HSW_CAGF_SHIFT)

#define BCS_TAIL 0x22030
#define BCS_HEAD 0x22034
#define BCS_START 0x22038
#define BCS_CTL	 0x2203c
#define MI_NOOP	0
#define BLT_WRITE_RGBA		(3<<20)
#define BLT_DEPTH_32		(3<<24)
#define BLT_ROP_SRC_COPY	(0xcc<<16)
#define XY_SRC_COPY_BLT_GEN8	((2<<29)|(0x53<<22))
#define MI_FLUSH_DW_GEN8	((0x26<<23)|(1<<14)|2)
#define MI_FLUSH_DW_USE_GTT	(1<<2)

#define RINGPG 2048
#define FENCEPG 2049
#define SRCPG 3072
#define DSTPG 11264
#define NPAGE 1024		/* 4MB */
#define FENCEVAL 0x5eed0000

static u32int *mmio;
static uchar *aper;
static u32int rd(u32int o){ return mmio[o/4]; }
static void wr(u32int o, u32int v){ mmio[o/4] = v; }
static int curfreq(void){ return (rd(GEN6_RPSTAT1) & HSW_CAGF_MASK) >> HSW_CAGF_SHIFT; }

static vlong
blit(u32int *ring, volatile u32int *fence, int seq)
{
	int n; vlong t0;
	n=0;
	ring[n++] = XY_SRC_COPY_BLT_GEN8 | BLT_WRITE_RGBA | (10-2);
	ring[n++] = BLT_DEPTH_32 | BLT_ROP_SRC_COPY | 4096;
	ring[n++] = 0;
	ring[n++] = (NPAGE<<16) | 1024;
	ring[n++] = DSTPG*4096; ring[n++] = 0;
	ring[n++] = 0; ring[n++] = 4096;
	ring[n++] = SRCPG*4096; ring[n++] = 0;
	ring[n++] = MI_FLUSH_DW_GEN8;
	ring[n++] = (FENCEPG*4096)|MI_FLUSH_DW_USE_GTT;
	ring[n++] = 0; ring[n++] = FENCEVAL+seq;
	while(n&1) ring[n++] = MI_NOOP;
	fence[0]=0;
	wr(BCS_CTL,0); wr(BCS_HEAD,0); wr(BCS_TAIL,0);
	wr(BCS_START, RINGPG*4096); wr(BCS_CTL,1); rd(BCS_CTL);
	t0 = nsec();
	wr(BCS_TAIL, n*4); rd(BCS_TAIL);
	while(fence[0] != (u32int)(FENCEVAL+seq))
		if(nsec()-t0 > 3000000000LL) return -1;
	return nsec()-t0;
}

static vlong
bench(u32int *ring, volatile u32int *fence, int *seq)
{
	vlong t, best=0; int i;
	for(i=0;i<8;i++){ t=blit(ring,fence,(*seq)++); if(t<0) return -1;
		if(best==0||t<best) best=t; }
	return best;
}

void
main(int argc, char **argv)
{
	u32int *ring, *src, cap, saved;
	volatile u32int *fence;
	vlong t, nb = (vlong)NPAGE*4096;
	int i, seq=1, rp0, want;

	USED(argc); USED(argv);
	mmio = segattach(0, "igfxmmio", 0, BAR0SZ);
	if(mmio==(void*)-1) sysfatal("segattach igfxmmio: %r");
	aper = segattach(0, "igfxscreen", 0, APSZ);
	if(aper==(void*)-1) sysfatal("segattach igfxscreen: %r");
	wr(FORCEWAKE_MT,(1<<16)|1); rd(FORCEWAKE_MT);
	for(i=0;i<500 && !(rd(FORCEWAKE_ACK_HSW)&1);i++) sleep(1);

	ring=(u32int*)(aper+RINGPG*4096);
	fence=(volatile u32int*)(aper+FENCEPG*4096);
	src=(u32int*)(aper+SRCPG*4096);
	for(i=0;i<4096/4;i++) ring[i]=MI_NOOP;
	for(i=0;i<1024;i++) src[i]=0x5a5a0000+i;

	cap = rd(GEN6_RP_STATE_CAP);
	rp0 = cap & 0xff;
	saved = rd(GEN6_RPNSWREQ);
	print("RP0 (max) = %d MHz, current = %d MHz\n", rp0*50, curfreq()*50);

	t = bench(ring,fence,&seq);
	print("\n@ %3d MHz (BIOS default): %6lld us -> %7.1f MB/s\n",
		curfreq()*50, t/1000, (double)nb/(double)t*1000.0);

	/* what a real driver does: ask for the top P-state */
	for(want = 4; want <= rp0; want += 4){
		wr(GEN6_RPNSWREQ, HSW_FREQUENCY(want));
		rd(GEN6_RPNSWREQ);
		for(i=0;i<200 && curfreq() < want;i++) sleep(1);
		t = bench(ring,fence,&seq);
		print("@ %3d MHz (req %3d MHz)  : %6lld us -> %7.1f MB/s\n",
			curfreq()*50, want*50, t/1000, (double)nb/(double)t*1000.0);
	}

	wr(GEN6_RPNSWREQ, saved); rd(GEN6_RPNSWREQ);
	print("\nRPNSWREQ restored (now %d MHz)\n", curfreq()*50);
	wr(FORCEWAKE_MT, 1<<16);
	exits(nil);
}
