/*
 * gpu9 — talk to the Intel GPU from 9front.
 *
 *   gpu9 info     what the GPU is, and what clock it is actually running at
 *   gpu9 clock    request the maximum P-state (RP0) and report it
 *   gpu9 bench    GPU blitter vs CPU memcpy, across sizes
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
	fprint(2, "usage: gpu9 [info | clock | bench | test]\n");
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
	print("  free GPU mem %d MB from GGTT %#ux (framebuffer sits below)\n",
		(g->end_page - g->next_page)*4/1024, g->next_page*4096);
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

static void
cmdbench(Gpu9 *g)
{
	int sizes[] = { 64, 256, 1024, 4096 };	/* 256KB..16MB */
	u32int ringg, srcg, dstg, *ring, *src;
	volatile u32int *fence;
	int i, k, seq = 1;
	vlong t, best, bc, nb;
	void *ra, *rb;

	ringg = gpu9_alloc(g, 4096, (void**)&ring);
	fence = (volatile u32int*)gpu9_cpu(g, gpu9_alloc(g, 4096, nil));
	/* src+dst must both fit in what is left of the 64MB aperture after the
	 * framebuffer; 16MB each is the largest pair that does. */
	srcg = gpu9_alloc(g, 4096*4096, (void**)&src);
	dstg = gpu9_alloc(g, 4096*4096, nil);
	if(ringg == 0 || srcg == 0 || dstg == 0)
		sysfatal("alloc: %r");
	for(i = 0; i < 4096/4; i++) ring[i] = MI_NOOP;
	for(i = 0; i < 1024; i++) src[i] = 0x5a5a0000+i;

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
	else if(strcmp(cmd, "bench") == 0)
		cmdbench(&g);
	else if(strcmp(cmd, "test") == 0)
		cmdtest(&g);
	else {
		gpu9_close(&g);
		usage();
	}
	gpu9_close(&g);
	exits(nil);
}
