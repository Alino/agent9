/*
 * gpu9 M11 — how much GPU memory is ACTUALLY backed? (measure, don't assume)
 *
 * Everything before this assumed a 64MB aperture and "confirmed" it by scanning
 * exactly 64MB worth of PTEs and reporting 16384/16384 live. That is an
 * assumption checking itself. Worse, "live" only tested bit0 — and the GTT
 * beyond the BIOS-initialised region is uninitialised GARBAGE, which has bit0
 * set about half the time. So "all 16384 mapped" was partly counting noise.
 *
 * Round 1 of this probe measured two things properly:
 *   - the kernel's "igfxscreen" segment really is 64MB (bisected via segattach),
 *     so gpu9.h's hardcode matches what userspace can touch; and
 *   - GTT[0] = 0xa4000001 (a real PTE) but GTT[16383] = 0xeb37aa1fe76e6abf,
 *     which is not a PTE at all. So the mapped region ENDS somewhere below 64MB.
 *
 * That distinction is not academic: gpu9_alloc() hands out pages up to
 * apsz/4096 = 16384. Any page past the real mapping is not memory — writes go
 * to nowhere and reads return junk, silently. `gpu9 bench` allocates 16MB+16MB
 * from page 2048, so it would cross a ~32MB boundary.
 *
 * So: find the exact backed region, by LINEARITY rather than by bit0.
 * The BIOS maps stolen memory contiguously from GTT[0], so the real region is
 * the maximal prefix where PTE[i] == PTE[0] + i*4096 (flags aside). The first
 * index that breaks that is the true edge, and everything past it is not ours.
 */
#include <u.h>
#include <libc.h>

#define BAR0SZ	(16*1024*1024)
#define GTTOFF	(8*1024*1024)		/* Gen8: GTT = BAR0 + BAR0SZ/2 */
#define GTTMAX	(1024*1024)		/* 8MB of 8-byte PTEs, if all real */
#define PAMASK	0x7ffffffffffULL	/* 43-bit phys; above this = not a PTE */

static u32int *mmio;

static u64int
gtt64(int i)
{
	u32int lo = mmio[(GTTOFF + i*8)/4], hi = mmio[(GTTOFF + i*8)/4 + 1];
	return (u64int)hi<<32 | lo;
}

/* Does this even look like a Gen8 GGTT PTE? Present, sane physical address,
 * page-aligned. Garbage fails this nearly always; a real PTE always passes. */
static int
plausible(u64int pte)
{
	u64int pa = pte & ~0xfffULL;
	return (pte & 1) && pa != 0 && pa <= PAMASK;
}

static uvlong
segsize(char *name)
{
	uvlong lo, hi, mid;
	void *p;

	lo = 0; hi = 4096ULL*1024*1024;
	while(lo < hi){
		mid = lo + (hi - lo + 4095)/4096/2*4096;
		if(mid <= lo)
			break;
		p = segattach(0, name, 0, mid);
		if(p == (void*)-1)
			hi = mid - 4096;
		else {
			segdetach(p);
			lo = mid;
		}
	}
	return lo;
}

void
main(int argc, char **argv)
{
	int i, linear, plaus;
	u64int base, pte;
	uvlong apsz;

	USED(argc); USED(argv);
	mmio = segattach(0, "igfxmmio", 0, BAR0SZ);
	if(mmio == (void*)-1)
		sysfatal("segattach igfxmmio: %r (need: echo type igfx >/dev/vgactl)");

	apsz = segsize("igfxscreen");
	print("aperture segment : %llud bytes = %llud MB (what the kernel lets us map)\n",
		apsz, apsz/1024/1024);

	base = gtt64(0);
	print("GTT[0]           : %.16llux -> phys %#llux\n", base, base & ~0xfffULL);
	if(!plausible(base))
		sysfatal("GTT[0] is not a PTE — GTT is not at BAR0+8MB on this part");

	/* the real region: maximal linear prefix from GTT[0] */
	for(linear = 1; linear < GTTMAX; linear++){
		pte = gtt64(linear);
		if(!plausible(pte) || (pte & ~0xfffULL) != (base & ~0xfffULL) + (u64int)linear*4096)
			break;
	}
	print("linear from GTT[0]: %d pages = %d MB   <-- REAL, BIOS-mapped GPU memory\n",
		linear, linear*4/1024);
	print("first break at    : GTT[%d] = %.16llux (GGTT %#ux)\n",
		linear, gtt64(linear), linear*4096);

	/* how far does anything plausible go at all? */
	plaus = 0;
	for(i = 0; i < GTTMAX; i++)
		if(plausible(gtt64(i)))
			plaus++;
	print("plausible PTEs    : %d of %d scanned (the rest is uninitialised junk;\n",
		plaus, GTTMAX);
	print("                    counting bit0 alone would call ~half of it 'mapped')\n");

	print("\ngpu9 today lets gpu9_alloc reach page %llud (apsz/4096).\n", apsz/4096);
	if(linear < (int)(apsz/4096))
		print("*** BUG: pages %d..%llud are NOT backed. Allocating there writes to\n"
		      "*** nowhere and reads junk. end_page must be %d, not %llud.\n",
			linear, apsz/4096 - 1, linear, apsz/4096);
	else
		print("All aperture pages are backed; end_page = apsz/4096 is correct.\n");
	exits(nil);
}
