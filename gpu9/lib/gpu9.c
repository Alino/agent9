/*
 * gpu9.c — the driver. See gpu9.h for why there is no kernel component.
 * Native 9front (kencc) for now; the same logic backs the ioctl() shim that
 * cc9-built iris will call.
 */
#include <u.h>
#include <libc.h>
#include "gpu9.h"

/* The framebuffer occupies the low aperture (1024x768x16 = 1.5MB today, but a
 * mode change could grow it). Start at 8MB in: comfortably clear, and it is the
 * page probe/m2 verified. */
#define FIRST_FREE_PAGE	2048
#define RING_DWORDS	(4096/4)

int
gpu9_open(Gpu9 *g)
{
	memset(g, 0, sizeof *g);

	g->mmio = segattach(0, "igfxmmio", 0, GPU9_BAR0SZ);
	if(g->mmio == (void*)-1){
		werrstr("segattach igfxmmio: %r (need: echo type igfx >/dev/vgactl)");
		return -1;
	}
	g->aper = segattach(0, "igfxscreen", 0, GPU9_APERTURE_SZ);
	if(g->aper == (void*)-1){
		werrstr("segattach igfxscreen: %r");
		return -1;
	}
	g->apsz = GPU9_APERTURE_SZ;
	g->next_page = FIRST_FREE_PAGE;
	g->end_page = GPU9_APERTURE_SZ/4096;

	if(gpu9_forcewake_get(g) < 0){
		werrstr("forcewake: GT never acked");
		return -1;
	}

	/* We rely on the legacy ring. If something ever turns execlists on, our
	 * ring writes would be silently ignored — fail loudly instead. */
	if(gpu9_rd(g, GPU9_GFX_MODE_GEN7) & (1<<15)){
		werrstr("execlists are ENABLED — legacy ring submission won't work");
		return -1;
	}

	/* ring + fence */
	g->ring_page = g->next_page++;
	g->ring = (g9u32*)(g->aper + g->ring_page*4096);
	g->ring_dwords = RING_DWORDS;
	g->fence_page = g->next_page++;
	g->fence = (volatile g9u32*)(g->aper + g->fence_page*4096);
	g->fence[0] = 0;
	g->seqno = 0x9000;

	/* Ask for the real clock. Without this the GPU runs at its 100MHz floor
	 * (1/8 of RP0) because nothing else on 9front runs RPS. */
	gpu9_max_clock(g);

	memset((void*)g->ring, 0, 4096);
	gpu9_wr(g, GPU9_RCS_BASE+GPU9_RING_CTL, 0);
	gpu9_wr(g, GPU9_RCS_BASE+GPU9_RING_HEAD, 0);
	gpu9_wr(g, GPU9_RCS_BASE+GPU9_RING_TAIL, 0);
	gpu9_wr(g, GPU9_RCS_BASE+GPU9_RING_START, g->ring_page*4096);
	gpu9_wr(g, GPU9_RCS_BASE+GPU9_RING_CTL, ((1-1)<<12) | GPU9_RING_VALID);
	gpu9_rd(g, GPU9_RCS_BASE+GPU9_RING_CTL);	/* posting read */
	g->tail = 0;
	return 0;
}

void
gpu9_close(Gpu9 *g)
{
	gpu9_forcewake_put(g);
}

int
gpu9_forcewake_get(Gpu9 *g)
{
	int i;

	gpu9_wr(g, GPU9_FORCEWAKE_MT, (GPU9_FW_KERNEL<<16) | GPU9_FW_KERNEL);
	gpu9_rd(g, GPU9_FORCEWAKE_MT);
	for(i = 0; i < 500; i++){
		if(gpu9_rd(g, GPU9_FORCEWAKE_ACK_HSW) & GPU9_FW_KERNEL)
			return 0;
		sleep(1);
	}
	return -1;
}

void
gpu9_forcewake_put(Gpu9 *g)
{
	gpu9_wr(g, GPU9_FORCEWAKE_MT, GPU9_FW_KERNEL<<16);
	gpu9_rd(g, GPU9_FORCEWAKE_MT);
}

int
gpu9_cur_clock(Gpu9 *g)
{
	return ((gpu9_rd(g, GPU9_RPSTAT1) & GPU9_HSW_CAGF_MASK) >> GPU9_HSW_CAGF_SHIFT) * 50;
}

int
gpu9_max_clock(Gpu9 *g)
{
	int rp0, i;

	rp0 = gpu9_rd(g, GPU9_RP_STATE_CAP) & 0xff;	/* byte 0 = RP0 = max */
	if(rp0 == 0)
		return gpu9_cur_clock(g);
	gpu9_wr(g, GPU9_RPNSWREQ, GPU9_HSW_FREQUENCY(rp0));
	gpu9_rd(g, GPU9_RPNSWREQ);
	for(i = 0; i < 400 && gpu9_cur_clock(g) < rp0*50; i++)
		sleep(1);
	return gpu9_cur_clock(g);
}

g9u32
gpu9_alloc(Gpu9 *g, g9u32 bytes, void **cpu)
{
	g9u32 pages, ggtt;

	pages = (bytes + 4095)/4096;
	if(pages == 0)
		pages = 1;
	if(g->next_page + pages > g->end_page){
		werrstr("gpu9: out of aperture (%ud pages left)", g->end_page - g->next_page);
		return 0;
	}
	ggtt = g->next_page * 4096;
	g->next_page += pages;
	if(cpu)
		*cpu = g->aper + ggtt;
	return ggtt;
}

void *
gpu9_cpu(Gpu9 *g, g9u32 ggtt)
{
	return g->aper + ggtt;
}

int
gpu9_exec(Gpu9 *g, g9u32 batch_ggtt, g9u32 nbytes)
{
	g9u32 n, want;
	vlong t0;

	USED(nbytes);
	n = 0;
	/* jump to the batch, then flush + post the seqno. Order matters: the
	 * flush is what makes the batch's writes visible before the fence lands. */
	g->ring[n++] = MI_BATCH_BUFFER_START_GEN8;	/* GGTT (bit8 clear) */
	g->ring[n++] = batch_ggtt;
	g->ring[n++] = 0;
	/* RENDER ring -> PIPE_CONTROL, not MI_FLUSH_DW (a blitter command: RCS
	 * consumes it but never posts the write, which looks just like a hang). */
	want = ++g->seqno;
	g->ring[n++] = PIPE_CONTROL_GEN8;
	g->ring[n++] = PC_POST_SYNC_WRITE_IMM | PC_DAT_GGTT | PC_CS_STALL |
			PC_RT_CACHE_FLUSH | PC_DC_FLUSH;
	g->ring[n++] = g->fence_page*4096;	/* addr lo */
	g->ring[n++] = 0;			/* addr hi */
	g->ring[n++] = want;			/* imm lo */
	g->ring[n++] = 0;			/* imm hi */
	while(n & 1)
		g->ring[n++] = MI_NOOP;

	g->fence[0] = 0;
	/* Fully re-arm the ring: disable, set START/HEAD/TAIL, enable, THEN kick.
	 * Writing HEAD while RING_VALID is set is undefined and the engine simply
	 * never advances (observed: HEAD stayed 0 while TAIL moved). This is the
	 * exact sequence probe/m3 proved. */
	gpu9_wr(g, GPU9_RCS_BASE+GPU9_RING_CTL, 0);
	gpu9_wr(g, GPU9_RCS_BASE+GPU9_RING_HEAD, 0);
	gpu9_wr(g, GPU9_RCS_BASE+GPU9_RING_TAIL, 0);
	gpu9_wr(g, GPU9_RCS_BASE+GPU9_RING_START, g->ring_page*4096);
	gpu9_wr(g, GPU9_RCS_BASE+GPU9_RING_CTL, ((1-1)<<12) | GPU9_RING_VALID);
	gpu9_rd(g, GPU9_RCS_BASE+GPU9_RING_CTL);
	gpu9_wr(g, GPU9_RCS_BASE+GPU9_RING_TAIL, n*4);
	gpu9_rd(g, GPU9_RCS_BASE+GPU9_RING_TAIL);

	/* busy-wait on the FENCE, never on HEAD==TAIL (that only means parsed).
	 * sleep(1) is 1ms on Plan 9 — the same order as the work — so it would
	 * measure the scheduler. */
	t0 = nsec();
	while(g->fence[0] != want){
		if(nsec() - t0 > 2000000000LL){	/* 2s */
			werrstr("gpu9: GPU hung (fence %.8ux want %.8ux HEAD %.8ux TAIL %.8ux)",
				g->fence[0], want,
				gpu9_rd(g, GPU9_RCS_BASE+GPU9_RING_HEAD),
				gpu9_rd(g, GPU9_RCS_BASE+GPU9_RING_TAIL));
			return -1;
		}
	}
	return 0;
}
