/*
 * gpu9 M2 — find GPU-visible memory WITHOUT needing physical addresses.
 *
 * The trick: the aperture (BAR2, exposed by the kernel as "igfxscreen") is a CPU
 * window THROUGH the GTT. If GTT[n] maps page P, then CPU writes at
 * aperture + n*4096 land in P, and the GPU reaches the same P at GTT address
 * n*4096. So aperture offset == GPU address. No virt->phys plumbing needed, and
 * no kernel patch.
 *
 * The BIOS already GTT-maps stolen memory (GTT[0]=0xa4000001 was a live PTE).
 * The framebuffer only uses the first ~1.5MB (1024x768x16). Anything mapped
 * beyond that is GPU-visible memory nobody is using — our ring and target.
 *
 * This scans the GTT for that region and proves CPU<->aperture coherency.
 */
#include <u.h>
#include <libc.h>

#define BAR0SZ	(16*1024*1024)
#define GTTOFF	(8*1024*1024)		/* Gen8: GTT lives at BAR0 + 8MB */
#define APSZ	(0x4000000)		/* 64MB, per /dev/vgactl */
#define FBBYTES	(1024*768*2)		/* current VESA mode in use — stay clear */

static u32int *mmio;
static uchar  *aper;

static u32int gtt(int i){ return mmio[(GTTOFF + i*8)/4]; }   /* Gen8 PTE = 64-bit */
static u64int gtt64(int i){
	u32int lo = mmio[(GTTOFF + i*8)/4], hi = mmio[(GTTOFF + i*8)/4 + 1];
	return (u64int)hi<<32 | lo;
}

void
main(int argc, char **argv)
{
	int i, mapped, first, last, pg;
	u64int pte;
	u32int *p;

	USED(argc); USED(argv);
	mmio = segattach(0, "igfxmmio", 0, BAR0SZ);
	if(mmio == (void*)-1) sysfatal("segattach igfxmmio: %r");
	aper = segattach(0, "igfxscreen", 0, APSZ);
	if(aper == (void*)-1) sysfatal("segattach igfxscreen: %r");
	print("mmio %p  aperture %p (%d MB)\n", mmio, aper, APSZ/1024/1024);

	/* how much of the aperture is GTT-mapped? */
	mapped = 0; first = -1; last = -1;
	for(i = 0; i < APSZ/4096; i++){
		pte = gtt64(i);
		if(pte & 1){
			mapped++;
			if(first < 0) first = i;
			last = i;
		}
	}
	print("GTT: %d/%d pages mapped (first %d, last %d) = %d MB\n",
		mapped, APSZ/4096, first, last, mapped*4/1024);
	print("GTT[0]     = %.16llux\n", gtt64(0));
	print("GTT[1024]  = %.16llux  (4MB in)\n", gtt64(1024));
	print("GTT[4096]  = %.16llux  (16MB in)\n", gtt64(4096));

	/* pick a page well past the framebuffer, and prove CPU<->aperture works */
	pg = 2048;				/* 8MB into the aperture */
	if(!(gtt64(pg) & 1)){
		print("page %d NOT mapped — need to program GTT ourselves\n", pg);
		exits("unmapped");
	}
	print("using page %d (aperture off %d MB), PTE %.16llux\n", pg, pg*4/1024, gtt64(pg));
	p = (u32int*)(aper + pg*4096);
	p[0] = 0xdeadbeef;
	p[1] = 0xcafebabe;
	if(p[0] == 0xdeadbeef && p[1] == 0xcafebabe)
		print("M2 PASS: aperture read/write coherent; GPU addr for this page = %#ux\n", pg*4096);
	else
		print("M2 FAIL: readback %.8ux %.8ux\n", p[0], p[1]);
	exits(nil);
}
