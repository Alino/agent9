/*
 * gpu9 — talk to the Intel GPU from 9front.
 *
 *   gpu9 info     what the GPU is, and what clock it is actually running at
 *   gpu9 clock    request the maximum P-state (RP0) and report it
 *   gpu9 rps      prove the headline: the same blit at RPn vs RP0
 *   gpu9 bench    GPU blitter vs CPU memcpy, across sizes
 *   gpu9 fill     accelerated framebuffer clear, GPU vs CPU (the real use)
 *   gpu9 scroll   accelerated screen scroll, GPU vs CPU
 *   gpu9 demo     draw on the actual screen so you can SEE the GPU work
 *   gpu9 test     self-check: submit work and verify the GPU did it
 *
 * This is a real driver in userspace — no kernel component. 9front already
 * exposes BAR0 and the aperture as named physical segments (see gpu9/NOTES.md).
 * Run `echo type igfx >/dev/vgactl` once first so the kernel registers them;
 * that only maps + registers, it does not touch your display.
 */
#include <u.h>
#include <libc.h>
#include "gpu9.h"

#define BCS_BASE	0x22000
#define BLT_WRITE_RGBA		(3<<20)
#define BLT_DEPTH_32		(3<<24)
#define BLT_ROP_SRC_COPY	(0xcc<<16)
#define XY_SRC_COPY_BLT_GEN8	((2<<29)|(0x53<<22))

static char *memtype[] = { "UC", "WC", "WT", "WB" };

static void
usage(void)
{
	fprint(2, "usage: gpu9 [info | clock | rps | bench | fill | scroll | demo | test]\n");
	exits("usage");
}

static void
cmdinfo(Gpu9 *g)
{
	u32int cap, pat_lo, pat_hi, mode;

	cap = gpu9_rd(g, GPU9_RP_STATE_CAP);
	pat_lo = gpu9_rd(g, 0x40e0);
	pat_hi = gpu9_rd(g, 0x40e4);
	mode = gpu9_rd(g, GPU9_GFX_MODE_GEN7);

	print("Intel Gen8 (Broadwell)\n");
	print("  mmio         %p  (BAR0, 16MB: registers + GTT at +8MB)\n", g->mmio);
	print("  aperture     %p  (%d MB — a CPU window through the GTT,\n",
		g->aper, g->apsz/1024/1024);
	print("                            so aperture offset == GGTT address)\n");
	print("  submission   %s\n", (mode & (1<<15)) ? "EXECLISTS (gpu9 needs legacy!)" :
		"legacy ring (nothing enabled execlists — 9front has no i915)");
	print("  clock        %d MHz now   [min %d, efficient %d, MAX %d]\n",
		gpu9_cur_clock(g), ((cap>>16)&0xff)*50, ((cap>>8)&0xff)*50, (cap&0xff)*50);
	if(gpu9_cur_clock(g) < (int)((cap&0xff)*50))
		print("               ^ below max: nothing but gpu9 runs RPS on this box\n");
	print("  PAT          %.8ux_%.8ux  (entry0 = %s)\n", pat_hi, pat_lo,
		memtype[pat_lo & 3]);
	print("  free GPU mem %d MB from GGTT %#ux (largest run that survives BOTH a\n",
		(g->end_page - g->next_page)*4/1024, g->next_page*4096);
	print("                            PTE check and a write/readback — of the\n");
	print("                            16384 aperture pages, 3 map elsewhere and\n");
	print("                            256 refuse writes. Measured, not assumed.)\n");
}

/* blit npage*4096 bytes on the BLITTER ring; returns ns, or -1 on hang */
static vlong
blit(Gpu9 *g, u32int ringg, u32int *ring, volatile u32int *fence,
	u32int src, u32int dst, int npage, int seq)
{
	int n;
	vlong t0;

	n = 0;
	ring[n++] = XY_SRC_COPY_BLT_GEN8 | BLT_WRITE_RGBA | (10-2);
	ring[n++] = BLT_DEPTH_32 | BLT_ROP_SRC_COPY | 4096;
	ring[n++] = 0;
	ring[n++] = (npage<<16) | 1024;
	ring[n++] = dst; ring[n++] = 0;
	ring[n++] = 0;   ring[n++] = 4096;
	ring[n++] = src; ring[n++] = 0;
	/* BCS -> MI_FLUSH_DW (the RENDER ring would need PIPE_CONTROL instead) */
	ring[n++] = MI_FLUSH_DW_GEN8;
	ring[n++] = (u32int)((uintptr)fence - (uintptr)g->aper) | MI_FLUSH_DW_USE_GTT;
	ring[n++] = 0;
	ring[n++] = 0xf00d0000 + seq;
	while(n & 1) ring[n++] = MI_NOOP;

	fence[0] = 0;
	gpu9_wr(g, BCS_BASE+GPU9_RING_CTL, 0);
	gpu9_wr(g, BCS_BASE+GPU9_RING_HEAD, 0);
	gpu9_wr(g, BCS_BASE+GPU9_RING_TAIL, 0);
	gpu9_wr(g, BCS_BASE+GPU9_RING_START, ringg);
	gpu9_wr(g, BCS_BASE+GPU9_RING_CTL, GPU9_RING_VALID);
	gpu9_rd(g, BCS_BASE+GPU9_RING_CTL);
	t0 = nsec();
	gpu9_wr(g, BCS_BASE+GPU9_RING_TAIL, n*4);
	gpu9_rd(g, BCS_BASE+GPU9_RING_TAIL);
	while(fence[0] != (u32int)(0xf00d0000+seq))
		if(nsec()-t0 > 3000000000LL)
			return -1;
	return nsec()-t0;
}

/* Check the blit actually copied. A throughput number from a blit nobody
 * verified is worthless — and specifically: gpu9_alloc used to hand out GTT
 * pages 8190/8191, which are not backed by our memory, straight through the
 * middle of a 16MB dst. Sample on a prime stride so we touch most pages
 * cheaply; reading all of it through the uncached aperture would cost seconds. */
static int
blitok(u32int *src, u32int *dst, int npage)
{
	int i, n = npage*1024;		/* dwords copied */

	for(i = 0; i < n; i += 1021)
		if(dst[i] != src[i]){
			print("  VERIFY FAIL at dword %d (page %d): got %.8ux want %.8ux\n",
				i, i/1024, dst[i], src[i]);
			return 0;
		}
	return 1;
}

static void
cmdbench(Gpu9 *g)
{
	int sizes[4];
	u32int ringg, srcg, dstg, *ring, *src, *dst;
	volatile u32int *fence;
	int i, k, seq = 1, avail, maxpg;
	vlong t, best, bc, nb;
	void *ra, *rb;

	ringg = gpu9_alloc(g, 4096, (void**)&ring);
	fence = (volatile u32int*)gpu9_cpu(g, gpu9_alloc(g, 4096, nil));
	/* Size to the arena we actually have, rather than assuming 16MB pairs fit.
	 * The usable run is ~32MB (the rest of the aperture is the framebuffer, or
	 * the unbacked pages gpu9_arena excludes), so a 16MB+16MB pair does NOT. */
	avail = g->end_page - g->next_page;
	maxpg = avail/2;
	if(maxpg > 4096)
		maxpg = 4096;
	sizes[0] = 64; sizes[1] = 256; sizes[2] = 1024; sizes[3] = maxpg;
	srcg = gpu9_alloc(g, maxpg*4096, (void**)&src);
	dstg = gpu9_alloc(g, maxpg*4096, (void**)&dst);
	if(ringg == 0 || srcg == 0 || dstg == 0)
		sysfatal("alloc: %r");
	for(i = 0; i < 4096/4; i++) ring[i] = MI_NOOP;
	/* fill ALL of src: the verify compares the whole copied range, and an
	 * uninitialised tail would compare equal to an uninitialised dst by luck. */
	for(i = 0; i < maxpg*1024; i++) src[i] = 0x5a5a0000+i;
	for(i = 0; i < maxpg*1024; i++) dst[i] = 0;

	print("GPU at %d MHz. Best of 8; CPU baseline is memcpy in CACHED RAM\n", gpu9_cur_clock(g));
	print("(never benchmark the CPU through the aperture — it is uncached, ~13 MB/s)\n\n");
	print("     bytes |   GPU blit   |  CPU memcpy  | GPU vs CPU\n");
	for(k = 0; k < nelem(sizes); k++){
		nb = (vlong)sizes[k]*4096;
		best = 0;
		for(i = 0; i < 8; i++){
			t = blit(g, ringg, ring, fence, srcg, dstg, sizes[k], seq++);
			if(t < 0){ print("%10lld | GPU HUNG (reboot to clear)\n", nb); return; }
			if(best == 0 || t < best) best = t;
		}
		if(!blitok(src, dst, sizes[k])){
			print("%10lld | copy is WRONG — refusing to report a speed for it\n", nb);
			return;
		}
		bc = 0;
		ra = malloc(nb); rb = malloc(nb);
		if(ra && rb){
			memset(ra, 0x5a, nb);
			memmove(rb, ra, nb);
			for(i = 0; i < 5; i++){
				vlong t0 = nsec();
				memmove(rb, ra, nb);
				t = nsec()-t0;
				if(bc == 0 || t < bc) bc = t;
			}
		}
		print("%10lld | %6.0f MB/s  | %6.0f MB/s  | %.2fx%s\n", nb,
			(double)nb/(double)best*1000.0,
			bc ? (double)nb/(double)bc*1000.0 : 0.0,
			bc ? (double)bc/(double)best : 0.0,
			bc && bc > best ? "  <- GPU wins" : "");
		free(ra); free(rb);
	}
	print("\nThe GPU is flat (clock-bound); the CPU falls off once it leaves cache.\n");
}

/*
 * The headline claim, made reproducible: run the SAME verified blit at the
 * P-state the BIOS leaves you at (RPn) and at the one a driver asks for (RP0).
 * The whole difference is one register write. Don't take 6.7x on trust — this
 * re-derives it on the box, every time.
 */
static void
cmdrps(Gpu9 *g)
{
	u32int ringg, srcg, dstg, *ring, *src, *dst;
	volatile u32int *fence;
	int i, npage, seq = 1, rpn, rp0, lo, hi;
	vlong t, tlo, thi;

	rpn = gpu9_rpn(g); rp0 = gpu9_rp0(g);
	npage = 1024;					/* 4MB: past the CPU's cache */
	ringg = gpu9_alloc(g, 4096, (void**)&ring);
	fence = (volatile u32int*)gpu9_cpu(g, gpu9_alloc(g, 4096, nil));
	srcg = gpu9_alloc(g, npage*4096, (void**)&src);
	dstg = gpu9_alloc(g, npage*4096, (void**)&dst);
	if(ringg == 0 || srcg == 0 || dstg == 0)
		sysfatal("alloc: %r");
	for(i = 0; i < 4096/4; i++) ring[i] = MI_NOOP;
	for(i = 0; i < npage*1024; i++) src[i] = 0x5a5a0000+i;

	print("Same 4MB blit, same code, one register apart:\n\n");
	lo = gpu9_set_clock(g, rpn);
	tlo = 0;
	for(i = 0; i < 8; i++){
		for(int j = 0; j < npage*1024; j += 4096) dst[j] = 0;
		t = blit(g, ringg, ring, fence, srcg, dstg, npage, seq++);
		if(t < 0) sysfatal("hung at RPn");
		if(tlo == 0 || t < tlo) tlo = t;
	}
	if(!blitok(src, dst, npage)) sysfatal("RPn copy wrong");
	print("  RPn (BIOS default, no driver) %3d MHz : %6.0f MB/s\n",
		lo, (double)(npage*4096)/(double)tlo*1000.0);

	hi = gpu9_set_clock(g, rp0);
	thi = 0;
	for(i = 0; i < 8; i++){
		for(int j = 0; j < npage*1024; j += 4096) dst[j] = 0;
		t = blit(g, ringg, ring, fence, srcg, dstg, npage, seq++);
		if(t < 0) sysfatal("hung at RP0");
		if(thi == 0 || t < thi) thi = t;
	}
	if(!blitok(src, dst, npage)) sysfatal("RP0 copy wrong");
	print("  RP0 (what gpu9 asks for)      %3d MHz : %6.0f MB/s\n",
		hi, (double)(npage*4096)/(double)thi*1000.0);
	print("\n  %.1fx faster, for one write to RPNSWREQ. Both copies verified.\n",
		(double)tlo/(double)thi);
	print("  9front ships no GPU driver, so nothing else on the box ever asks.\n");
}

static void
cmdtest(Gpu9 *g)
{
	u32int batch_g, dst_g, *batch;
	volatile u32int *dst;
	int n, fail = 0;

	dst_g = gpu9_alloc(g, 4096, (void**)&dst);
	batch_g = gpu9_alloc(g, 4096, (void**)&batch);
	dst[0] = 0; dst[1] = 0;
	n = 0;
	batch[n++] = MI_STORE_DWORD_IMM_GEN8;
	batch[n++] = dst_g;      batch[n++] = 0; batch[n++] = 0x11112222;
	batch[n++] = MI_STORE_DWORD_IMM_GEN8;
	batch[n++] = dst_g+4;    batch[n++] = 0; batch[n++] = 0x33334444;
	batch[n++] = MI_BATCH_BUFFER_END;
	while(n & 1) batch[n++] = MI_NOOP;

	if(gpu9_exec(g, batch_g, n*4) < 0)
		sysfatal("exec: %r");
	if(dst[0] != 0x11112222){ print("FAIL dst[0] = %.8ux\n", dst[0]); fail++; }
	if(dst[1] != 0x33334444){ print("FAIL dst[1] = %.8ux\n", dst[1]); fail++; }
	if(gpu9_cur_clock(g) < 400){ print("FAIL clock only %d MHz\n", gpu9_cur_clock(g)); fail++; }
	if(fail == 0)
		print("PASS: GPU executed a batch buffer and wrote both dwords; clock %d MHz\n",
			gpu9_cur_clock(g));
	else
		exits("fail");
}

/* the framebuffer 9front is running: cat /dev/vgactl says 1024x768x16 r5g6b5 */
#define FBW	1024
#define FBH	768
#define FBPITCH	(FBW*2)
#define FBBYTES	(FBW*FBH*2)

static vlong
nsmin(vlong a, vlong b){ return a == 0 || b < a ? b : a; }

/*
 * fill — the operation a 2D accelerator is FOR. Clear/paint a framebuffer-sized
 * region. The CPU can only reach framebuffer memory through the uncached
 * aperture; the blitter is native to it. This is the honest comparison (both
 * write the same VRAM), and it is exactly what 9front's softscreen flush does
 * by hand today with no acceleration.
 */
static void
cmdfill(Gpu9 *g)
{
	u32int bufg;
	u16int *buf;
	int i, k;
	vlong t0, tg, tc;
	u16int color = 0x07ff;		/* cyan-ish in r5g6b5 */

	bufg = gpu9_alloc(g, FBBYTES, (void**)&buf);
	if(bufg == 0) sysfatal("alloc: %r");

	if(gpu9_fill(g, bufg, FBPITCH, 0, 0, FBW, FBH, color, GPU9_DEPTH16) < 0)
		sysfatal("fill: %r");
	for(i = 0; i < FBW*FBH; i += 1021)	/* verify a prime-strided sample */
		if(buf[i] != color){
			print("VERIFY FAIL: pixel %d = %.4ux want %.4ux\n", i, buf[i], color);
			return;
		}

	tg = 0;
	for(k = 0; k < 8; k++){
		t0 = nsec();
		if(gpu9_fill(g, bufg, FBPITCH, 0, 0, FBW, FBH, color, GPU9_DEPTH16) < 0)
			sysfatal("fill: %r");
		tg = nsmin(tg, nsec()-t0);
	}
	tc = 0;
	for(k = 0; k < 5; k++){
		t0 = nsec();
		for(i = 0; i < FBW*FBH; i++) buf[i] = color;	/* CPU, through aperture */
		tc = nsmin(tc, nsec()-t0);
	}
	print("fill %dx%d (%d KB), verified:\n", FBW, FBH, FBBYTES/1024);
	print("  GPU blitter %6.0f MB/s   (%lld us)\n", (double)FBBYTES/(double)tg*1000.0, tg/1000);
	print("  CPU aperture%6.0f MB/s   (%lld us)\n", (double)FBBYTES/(double)tc*1000.0, tc/1000);
	print("  -> GPU is %.1fx faster at clearing the framebuffer\n", (double)tc/(double)tg);
}

/*
 * scroll — the terminal operation: shift the whole screen up. A framebuffer-to-
 * framebuffer copy. The CPU must READ every pixel back through the uncached
 * aperture (memmove), which is the worst case for it; the blitter stays in VRAM.
 */
static void
cmdscroll(Gpu9 *g)
{
	u32int bufg;
	u16int *buf;
	int i, k, y, lines = FBH - 16;	/* scroll up 16px: almost the whole screen moves */
	vlong t0, tg, tc;

	bufg = gpu9_alloc(g, FBBYTES, (void**)&buf);
	if(bufg == 0) sysfatal("alloc: %r");

	/* pattern: every pixel on line y holds y. After scrolling up 16, line y
	 * must hold y+16. */
	for(y = 0; y < FBH; y++)
		for(i = 0; i < FBW; i++) buf[y*FBW + i] = y;
	if(gpu9_blt(g, bufg, 0, 0, bufg, 0, 16, FBPITCH, FBW, lines, GPU9_DEPTH16) < 0)
		sysfatal("scroll: %r");
	for(y = 0; y < lines; y += 37)
		if(buf[y*FBW] != (u16int)(y+16)){
			print("VERIFY FAIL: line %d = %d want %d\n", y, buf[y*FBW], y+16);
			return;
		}

	tg = 0;
	for(k = 0; k < 8; k++){
		t0 = nsec();
		if(gpu9_blt(g, bufg, 0, 0, bufg, 0, 16, FBPITCH, FBW, lines, GPU9_DEPTH16) < 0)
			sysfatal("scroll: %r");
		tg = nsmin(tg, nsec()-t0);
	}
	tc = 0;
	for(k = 0; k < 5; k++){
		t0 = nsec();
		memmove(buf, buf + 16*FBW, (long)lines*FBPITCH);	/* CPU, through aperture */
		tc = nsmin(tc, nsec()-t0);
	}
	print("scroll %dx%d up 16px (%ld KB moved), verified:\n",
		FBW, FBH, (long)lines*FBPITCH/1024);
	print("  GPU blitter %6.0f MB/s   (%lld us)\n", (double)lines*FBPITCH/(double)tg*1000.0, tg/1000);
	print("  CPU aperture%6.0f MB/s   (%lld us)\n", (double)lines*FBPITCH/(double)tc*1000.0, tc/1000);
	print("  -> GPU is %.1fx faster at scrolling the screen\n", (double)tc/(double)tg);
}

/*
 * demo — draw on the ACTUAL screen (GGTT 0 == the framebuffer) so you can see
 * the GPU do it. Softscreen redraws over these on the next damage, so it is
 * self-healing; move the mouse or refresh to clear. Reports the fill rate.
 */
static void
cmddemo(Gpu9 *g)
{
	static u16int col[] = { 0xf800, 0x07e0, 0x001f, 0xffe0, 0xf81f, 0x07ff, 0xffff };
	int cols = 8, rows = 6, cw = FBW/8, ch = FBH/6, r, c, k, fills = 0;
	vlong t0, t;

	print("drawing %d rectangles on your screen with the GPU blitter...\n", cols*rows);
	t0 = nsec();
	for(k = 0; k < 20; k++)			/* 20 passes so the rate is measurable */
		for(r = 0; r < rows; r++)
			for(c = 0; c < cols; c++){
				if(gpu9_fill(g, 0, FBPITCH, c*cw, r*ch, cw-4, ch-4,
					col[(r*cols+c+k) % nelem(col)], GPU9_DEPTH16) < 0)
					sysfatal("fill: %r");
				fills++;
			}
	t = nsec() - t0;
	print("%d fills in %lld ms = %.0f fills/s (%.0f MB/s of framebuffer)\n",
		fills, t/1000000, (double)fills/(double)t*1e9,
		(double)fills*(cw-4)*(ch-4)*2/(double)t*1000.0);
	print("look at cirno's screen — that grid was drawn entirely by the GPU.\n");
	print("(9front's softscreen will paint over it on the next redraw.)\n");
}

void
main(int argc, char **argv)
{
	Gpu9 g;
	char *cmd;

	cmd = argc > 1 ? argv[1] : "info";
	if(gpu9_open(&g) < 0)
		sysfatal("gpu9_open: %r");

	if(strcmp(cmd, "info") == 0)
		cmdinfo(&g);
	else if(strcmp(cmd, "clock") == 0)
		print("GPU clock: %d MHz\n", gpu9_max_clock(&g));
	else if(strcmp(cmd, "rps") == 0)
		cmdrps(&g);
	else if(strcmp(cmd, "bench") == 0)
		cmdbench(&g);
	else if(strcmp(cmd, "fill") == 0)
		cmdfill(&g);
	else if(strcmp(cmd, "scroll") == 0)
		cmdscroll(&g);
	else if(strcmp(cmd, "demo") == 0)
		cmddemo(&g);
	else if(strcmp(cmd, "test") == 0)
		cmdtest(&g);
	else {
		gpu9_close(&g);
		usage();
	}
	gpu9_close(&g);
	exits(nil);
}
