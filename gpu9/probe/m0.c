/*
 * gpu9 M0 — settle the question that sizes the whole project:
 * does Broadwell's LEGACY RING submission work here, or is it execlists-only?
 *
 * Linux i915 uses execlists (ELSP) on Gen8+, which needs complex context images.
 * But 9front has NO i915 driver — nothing has put this GPU into execlist mode —
 * so the legacy ring may simply be live. That would make M3 dramatically simpler.
 *
 * Render registers (0x2000+) are behind FORCEWAKE: the GT sleeps, and reads
 * return garbage (0 / ~0) until woken. HSW/BDW use FORCEWAKE_MT + ACK_HSW.
 * Reading the ring regs before AND after forcewake also proves forcewake works.
 */
#include <u.h>
#include <libc.h>

#define BAR0SZ		(16*1024*1024)

#define FORCEWAKE_MT		0xa188		/* HSW/BDW request */
#define FORCEWAKE_ACK_HSW	0x130044	/* HSW/BDW ack */
#define FW_KERNEL		1
#define MASKED_ENABLE(x)	((x) | ((x)<<16))
#define MASKED_DISABLE(x)	((x)<<16)

#define RCS_TAIL	0x2030
#define RCS_HEAD	0x2034
#define RCS_START	0x2038
#define RCS_CTL		0x203c
#define GFX_MODE_GEN7	0x229c		/* bit15 = execlist enable */
#define EXECLIST_STATUS	0x2234

static u32int *mmio;
static u32int rd(u32int o){ return mmio[o/4]; }
static void wr(u32int o, u32int v){ mmio[o/4] = v; }

static int
fwget(void)
{
	int i;

	wr(FORCEWAKE_MT, MASKED_ENABLE(FW_KERNEL));
	rd(FORCEWAKE_MT);			/* posting read */
	for(i = 0; i < 500; i++){
		if(rd(FORCEWAKE_ACK_HSW) & FW_KERNEL)
			return 1;
		sleep(1);
	}
	return 0;
}

static void
fwput(void)
{
	wr(FORCEWAKE_MT, MASKED_DISABLE(FW_KERNEL));
	rd(FORCEWAKE_MT);
}

void
main(int argc, char **argv)
{
	u32int mode, ctl;

	USED(argc); USED(argv);
	mmio = segattach(0, "igfxmmio", 0, BAR0SZ);
	if(mmio == (void*)-1)
		sysfatal("segattach igfxmmio: %r");

	print("--- before forcewake (expect garbage/0) ---\n");
	print("RCS_CTL   %.8ux   GFX_MODE %.8ux\n", rd(RCS_CTL), rd(GFX_MODE_GEN7));

	if(!fwget()){
		print("FORCEWAKE FAILED: ack never set (%.8ux)\n", rd(FORCEWAKE_ACK_HSW));
		exits("forcewake");
	}
	print("--- forcewake acquired (ack %.8ux) ---\n", rd(FORCEWAKE_ACK_HSW));

	print("RCS_TAIL  0x2030 = %.8ux\n", rd(RCS_TAIL));
	print("RCS_HEAD  0x2034 = %.8ux\n", rd(RCS_HEAD));
	print("RCS_START 0x2038 = %.8ux\n", rd(RCS_START));
	ctl = rd(RCS_CTL);
	print("RCS_CTL   0x203c = %.8ux  (ring %s, len %d pages)\n",
		ctl, (ctl&1)? "ENABLED":"disabled", ((ctl>>12)&0x1ff)+1);
	mode = rd(GFX_MODE_GEN7);
	print("GFX_MODE  0x229c = %.8ux  (execlists %s)\n",
		mode, (mode & (1<<15))? "ON":"OFF");
	print("EXECLIST_STATUS 0x2234 = %.8ux\n", rd(EXECLIST_STATUS));

	if((mode & (1<<15)) == 0)
		print("M0 RESULT: execlists OFF -> LEGACY RING is the path. Much simpler.\n");
	else
		print("M0 RESULT: execlists ON -> must implement context images.\n");

	fwput();
	exits(nil);
}
