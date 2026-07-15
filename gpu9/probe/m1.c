/*
 * gpu9 M1 — prove userspace can reach Broadwell GPU registers.
 *
 * 9front already exposes BAR0 as a named physical segment (kernel vgaigfx.c:51,
 * addvgaseg("igfxmmio", ...)), so a userspace program can segattach it and read
 * GPU registers directly — this is exactly what aux/vga does. No kernel patch.
 *
 * Verification is against a KNOWN value, not a plausible one: aux/vga -p read
 * PIPE_A_SRC (0x6001c) = 0x03ff02ff on this box (1023+1 x 767+1 = 1024x768,
 * the current VESA mode). If we read that back, MMIO access is real.
 *
 * Display registers need no forcewake (display power well is on while the screen
 * is up). Render registers (0x2000+) do — that is M1b.
 */
#include <u.h>
#include <libc.h>

#define BAR0SZ	(16*1024*1024)

static u32int *mmio;

static u32int
rd(u32int off)
{
	return mmio[off/4];
}

void
main(int argc, char **argv)
{
	u32int src, ht, vt, id;

	USED(argc); USED(argv);

	mmio = segattach(0, "igfxmmio", 0, BAR0SZ);
	if(mmio == (void*)-1)
		sysfatal("segattach igfxmmio: %r (is vgactl type igfx set?)");
	print("mmio mapped at %p\n", mmio);

	/* display regs — no forcewake needed */
	src = rd(0x6001c);		/* PIPE_A_SRCSZ */
	ht  = rd(0x60000);		/* PIPE_A_HTOTAL */
	vt  = rd(0x6000c);		/* PIPE_A_VTOTAL */
	print("PIPE_A_SRC   0x6001c = %.8ux  (%d x %d)\n",
		src, ((src>>16)&0xfff)+1, (src&0xfff)+1);
	print("PIPE_A_HTOT  0x60000 = %.8ux  (hactive %d, htotal %d)\n",
		ht, (ht&0xffff)+1, ((ht>>16)&0xffff)+1);
	print("PIPE_A_VTOT  0x6000c = %.8ux  (vactive %d, vtotal %d)\n",
		vt, (vt&0xffff)+1, ((vt>>16)&0xffff)+1);

	if(src == 0x03ff02ff)
		print("M1 PASS: read the known value 0x03ff02ff (1024x768)\n");
	else
		print("M1 UNSURE: expected 0x03ff02ff, got %.8ux\n", src);

	/* Gen8 GTTMMADR: 0..2MB regs, GTT at +8MB. Peek the first GTT PTE. */
	id = rd(0x800000);
	print("GTT[0] (BAR0+8MB) = %.8ux\n", id);
	exits(nil);
}
