/*
 * gpu9 M9 — what clock is the GPU actually running at?
 *
 * A flat 684 MB/s regardless of size OR cache policy looks like a CLOCK limit.
 * Linux i915 runs RPS (Render P-State) management: it reads the hardware's
 * min/max/efficient frequencies and REQUESTS one. With no driver, nothing ever
 * asks — so Broadwell may be sitting at its minimum frequency forever. Forcewake
 * wakes the GT; it does not raise the clock.
 *
 *   GEN6_RPSTAT1 (0xa01c): current freq in bits 8..14 (CAGF), units of 50MHz
 *   GEN6_RP_STATE_CAP (0x145998, MCHBAR mirror): RP0 (max), RP1 (efficient),
 *     RPn (min) — bytes 0,1,2 respectively, each in 50MHz units
 *   GEN6_RPNSWREQ (0xa008): request a frequency, value in bits 31:25
 */
#include <u.h>
#include <libc.h>

#define BAR0SZ	(16*1024*1024)
#define FORCEWAKE_MT		0xa188
#define FORCEWAKE_ACK_HSW	0x130044
#define GEN6_RPSTAT1		0xa01c
#define GEN6_RPNSWREQ		0xa008
#define GEN6_RP_STATE_CAP	0x145998
#define GEN6_RP_CONTROL		0xa024
#define GEN6_RC_CONTROL		0xa090
#define GEN6_RC_STATE		0xa094

static u32int *mmio;
static u32int rd(u32int o){ return mmio[o/4]; }
static void wr(u32int o, u32int v){ mmio[o/4] = v; }

void
main(int argc, char **argv)
{
	u32int stat, cap, req, rpctl, rcctl, rcstate;
	int cur, rp0, rp1, rpn;

	USED(argc); USED(argv);
	mmio = segattach(0, "igfxmmio", 0, BAR0SZ);
	if(mmio == (void*)-1) sysfatal("segattach igfxmmio: %r");
	wr(FORCEWAKE_MT, (1<<16)|1); rd(FORCEWAKE_MT);
	{ int i; for(i=0;i<500 && !(rd(FORCEWAKE_ACK_HSW)&1);i++) sleep(1); }
	if(!(rd(FORCEWAKE_ACK_HSW)&1)) sysfatal("forcewake");

	stat = rd(GEN6_RPSTAT1);
	cap  = rd(GEN6_RP_STATE_CAP);
	req  = rd(GEN6_RPNSWREQ);
	rpctl = rd(GEN6_RP_CONTROL);
	rcctl = rd(GEN6_RC_CONTROL);
	rcstate = rd(GEN6_RC_STATE);

	cur = (stat >> 8) & 0x7f;
	rp0 = (cap >> 0)  & 0xff;	/* max */
	rp1 = (cap >> 8)  & 0xff;	/* efficient */
	rpn = (cap >> 16) & 0xff;	/* min */

	print("RPSTAT1   %.8ux -> current  %3d = %4d MHz\n", stat, cur, cur*50);
	print("RP_STATE_CAP %.8ux\n", cap);
	print("   RP0 (max)       %3d = %4d MHz\n", rp0, rp0*50);
	print("   RP1 (efficient) %3d = %4d MHz\n", rp1, rp1*50);
	print("   RPn (min)       %3d = %4d MHz\n", rpn, rpn*50);
	print("RPNSWREQ  %.8ux -> requested %d = %d MHz\n", req, (req>>25)&0x7f, ((req>>25)&0x7f)*50);
	print("RP_CONTROL %.8ux  RC_CONTROL %.8ux  RC_STATE %.8ux\n", rpctl, rcctl, rcstate);

	if(cur > 0 && rp0 > 0 && cur <= rpn + 1)
		print("\n*** GPU IS PINNED AT ITS MINIMUM CLOCK (%d MHz of %d MHz max) ***\n"
		      "*** nothing runs RPS — a real driver would request RP0 ***\n",
		      cur*50, rp0*50);
	wr(FORCEWAKE_MT, 1<<16);
	exits(nil);
}
