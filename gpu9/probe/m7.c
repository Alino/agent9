/*
 * gpu9 M7 — is our GPU memory UNCACHED? A flat 684 MB/s at every size says the
 * blitter is not overhead-bound, it is memory-bound at a suspiciously round
 * number. On Gen8 a GGTT PTE's cache behaviour is an INDEX into the PAT
 * register; the BIOS programs PAT for a display framebuffer, which wants UC.
 *
 * PTE (gen8 GGTT): bit0 = present; the PAT index is assembled from
 *   PWT(bit3), PCD(bit4), PAT(bit7)  ->  index = PAT<<2 | PCD<<1 | PWT
 * Our PTEs read 0xa4000001 -> flags 0x1 -> index 0.
 *
 * GEN8_PRIVATE_PAT_LO/HI (0x40e0/0x40e4) hold 8 entries, one byte each:
 *   bits0-1 memtype (0=UC 1=WC 2=WT 3=WB), bits2-3 LLC, bits4-5 age
 * If entry 0 is UC, every GPU access to our memory bypasses the LLC — which
 * would throttle 3D exactly as it throttles the blit.
 */
#include <u.h>
#include <libc.h>

#define BAR0SZ	(16*1024*1024)
#define GTTOFF	(8*1024*1024)
#define FORCEWAKE_MT		0xa188
#define FORCEWAKE_ACK_HSW	0x130044
#define GEN8_PRIVATE_PAT_LO	0x40e0
#define GEN8_PRIVATE_PAT_HI	0x40e4

static u32int *mmio;
static u32int rd(u32int o){ return mmio[o/4]; }
static void wr(u32int o, u32int v){ mmio[o/4] = v; }

static char *memtype[] = { "UC", "WC", "WT", "WB" };
static char *llc[] = { "eLLC-override", "LLC", "LLC+eLLC", "LLCeLLC" };

void
main(int argc, char **argv)
{
	u32int lo, hi, pte_lo, pte_hi;
	uvlong pat;
	int i;

	USED(argc); USED(argv);
	mmio = segattach(0, "igfxmmio", 0, BAR0SZ);
	if(mmio == (void*)-1) sysfatal("segattach igfxmmio: %r");
	wr(FORCEWAKE_MT, (1<<16)|1); rd(FORCEWAKE_MT);
	for(i=0;i<500 && !(rd(FORCEWAKE_ACK_HSW)&1);i++) sleep(1);

	lo = rd(GEN8_PRIVATE_PAT_LO);
	hi = rd(GEN8_PRIVATE_PAT_HI);
	pat = (uvlong)hi<<32 | lo;
	print("GEN8_PRIVATE_PAT = %.8ux_%.8ux\n", hi, lo);
	for(i = 0; i < 8; i++){
		int e = (pat >> (i*8)) & 0xff;
		print("  PAT[%d] = %.2ux  memtype=%s  llc=%s  age=%d%s\n",
			i, e, memtype[e&3], llc[(e>>2)&3], (e>>4)&3,
			i==0 ? "   <- what our PTEs (flags 0x1) select" : "");
	}

	pte_lo = mmio[(GTTOFF + 2048*8)/4];
	pte_hi = mmio[(GTTOFF + 2048*8)/4 + 1];
	print("\nGTT[2048] = %.8ux_%.8ux  flags=%.2ux -> PAT index %d\n",
		pte_hi, pte_lo, pte_lo & 0xff,
		(((pte_lo>>7)&1)<<2) | (((pte_lo>>4)&1)<<1) | ((pte_lo>>3)&1));

	if((pat & 3) == 0)
		print("\n*** PAT[0] is UC — the GPU is bypassing the LLC on ALL our memory ***\n");
	else
		print("\nPAT[0] memtype=%s — not the bottleneck\n", memtype[pat&3]);
	wr(FORCEWAKE_MT, 1<<16);
	exits(nil);
}
