/*
 * gpu9_dev.c — the gpu9 driver, cc9-native, for the in-process "kernel" that
 * backs iris. Same hardware logic as gpu9/lib/gpu9.c (the native tool), ported
 * to cc9's primitives: n9_segattach for the MMIO/aperture, n9_sleep for the
 * poll loops, clock_gettime for timing. Register constants + the Gpu9 struct
 * come from the shared gpu9.h (its __STDC_HOSTED__ branch).
 *
 * The gpu9_ioctl.c shim calls these to answer iris's GEM_CREATE / GEM_MMAP /
 * EXECBUFFER2. Errors go to stderr (no werrstr/%r in cc9).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "gpu9.h"

extern void *n9_segattach(unsigned long attr, const char *cls, void *va, unsigned long len);
extern long  n9_sleep(long ms);
extern int   clock_gettime(int, void *);

struct n9ts { long sec; long nsec; };
static long long
nowns(void)
{
	struct n9ts ts;
	clock_gettime(0, &ts);
	return (long long)ts.sec * 1000000000LL + ts.nsec;
}

static Gpu9 g;
static int g_open;

/* ---- the arena: same measured logic as lib/gpu9.c (PTE + write/readback) ---- */
static uint64_t
gtt_pte(uint32_t i)
{
	uint32_t lo = ((volatile uint32_t*)g.mmio)[(GPU9_GTT_OFFSET + i*8)/4];
	uint32_t hi = ((volatile uint32_t*)g.mmio)[(GPU9_GTT_OFFSET + i*8)/4 + 1];
	return (uint64_t)hi<<32 | lo;
}

static int
usable(uint32_t i)
{
	volatile uint32_t *p;
	uint32_t s0, s1;
	int ok;

	if((gtt_pte(i) & ~0xfffULL) != GPU9_STOLEN_BASE + (uint64_t)i*4096)
		return 0;
	p = (volatile uint32_t*)(g.aper + (uint64_t)i*4096);
	s0 = p[0]; s1 = p[1023];
	p[0] = 0xa5a5c3c3; p[1023] = 0x5a5a3c3c;
	ok = p[0] == 0xa5a5c3c3 && p[1023] == 0x5a5a3c3c;
	p[0] = s0; p[1023] = s1;
	return ok;
}

static void
arena(void)
{
	uint32_t i, start, best, bestat;

	best = 0; bestat = 0; start = 2048;
	for(i = 2048; i <= GPU9_GTT_ENTRIES; i++){
		if(i == GPU9_GTT_ENTRIES || !usable(i)){
			if(i - start > best){ best = i - start; bestat = start; }
			start = i + 1;
		}
	}
	g.next_page = bestat;
	g.end_page = bestat + best;
}

static int
forcewake_get(void)
{
	int i;
	gpu9_wr(&g, GPU9_FORCEWAKE_MT, (GPU9_FW_KERNEL<<16) | GPU9_FW_KERNEL);
	gpu9_rd(&g, GPU9_FORCEWAKE_MT);
	for(i = 0; i < 500; i++){
		if(gpu9_rd(&g, GPU9_FORCEWAKE_ACK_HSW) & GPU9_FW_KERNEL)
			return 0;
		n9_sleep(1);
	}
	return -1;
}

static int
cur_clock(void)
{
	return ((gpu9_rd(&g, GPU9_RPSTAT1) & GPU9_HSW_CAGF_MASK) >> GPU9_HSW_CAGF_SHIFT) * 50;
}

static void
max_clock(void)
{
	int rp0 = gpu9_rd(&g, GPU9_RP_STATE_CAP) & 0xff, i;
	if(rp0 == 0) return;
	gpu9_wr(&g, GPU9_RPNSWREQ, GPU9_HSW_FREQUENCY(rp0));
	gpu9_rd(&g, GPU9_RPNSWREQ);
	for(i = 0; i < 400 && cur_clock() < rp0*50; i++)
		n9_sleep(1);
}

/* Open the GPU: segattach MMIO + aperture, wake it, measure the arena, set up
 * the render ring. Returns 0, or -1 (message on stderr). Idempotent. */
int
gpu9dev_open(void)
{
	if(g_open) return 0;
	memset(&g, 0, sizeof g);

	g.mmio = n9_segattach(0, "igfxmmio", 0, GPU9_BAR0SZ);
	if(g.mmio == (void*)-1 || g.mmio == 0){
		fprintf(stderr, "[gpu9dev] segattach igfxmmio failed (echo type igfx >/dev/vgactl?)\n");
		return -1;
	}
	g.aper = n9_segattach(0, "igfxscreen", 0, GPU9_APERTURE_SZ);
	if(g.aper == (void*)-1 || g.aper == 0){
		fprintf(stderr, "[gpu9dev] segattach igfxscreen failed\n");
		return -1;
	}
	g.apsz = GPU9_APERTURE_SZ;
	arena();
	if(g.end_page - g.next_page < 64){
		fprintf(stderr, "[gpu9dev] only %u usable GPU pages\n", g.end_page - g.next_page);
		return -1;
	}
	if(forcewake_get() < 0){ fprintf(stderr, "[gpu9dev] forcewake timeout\n"); return -1; }
	if(gpu9_rd(&g, GPU9_GFX_MODE_GEN7) & (1<<15)){
		fprintf(stderr, "[gpu9dev] execlists enabled — legacy ring won't work\n"); return -1;
	}

	g.ring_page = g.next_page++;
	g.ring = (uint32_t*)(g.aper + g.ring_page*4096);
	g.ring_dwords = 4096/4;
	g.fence_page = g.next_page++;
	g.fence = (volatile uint32_t*)(g.aper + g.fence_page*4096);
	g.fence[0] = 0;
	g.seqno = 0x9000;

	max_clock();

	memset((void*)g.ring, 0, 4096);
	gpu9_wr(&g, GPU9_RCS_BASE+GPU9_RING_CTL, 0);
	gpu9_wr(&g, GPU9_RCS_BASE+GPU9_RING_HEAD, 0);
	gpu9_wr(&g, GPU9_RCS_BASE+GPU9_RING_TAIL, 0);
	gpu9_wr(&g, GPU9_RCS_BASE+GPU9_RING_START, g.ring_page*4096);
	gpu9_wr(&g, GPU9_RCS_BASE+GPU9_RING_CTL, ((1-1)<<12) | GPU9_RING_VALID);
	gpu9_rd(&g, GPU9_RCS_BASE+GPU9_RING_CTL);
	g.tail = 0;

	g_open = 1;
	fprintf(stderr, "[gpu9dev] open: %d MHz, arena pages %u..%u (%u MB), aper %p\n",
		cur_clock(), g.next_page, g.end_page,
		(g.end_page - g.next_page)*4/1024, g.aper);
	return 0;
}

/* allocate GPU-visible memory; returns GGTT offset (== aperture offset), or 0.
 * *cpu (if non-null) gets the CPU pointer through the aperture. */
uint32_t
gpu9dev_alloc(uint32_t bytes, void **cpu)
{
	uint32_t pages = (bytes + 4095)/4096, ggtt;
	if(pages == 0) pages = 1;
	if(g.next_page + pages > g.end_page){
		fprintf(stderr, "[gpu9dev] out of GPU memory (%u pages left, wanted %u)\n",
			g.end_page - g.next_page, pages);
		return 0;
	}
	ggtt = g.next_page * 4096;
	g.next_page += pages;
	if(cpu) *cpu = g.aper + ggtt;
	return ggtt;
}

void *
gpu9dev_cpu(uint32_t ggtt)
{
	return g.aper + ggtt;
}

/* how much GPU memory is left (bytes) — for GEM_GET_APERTURE */
uint64_t
gpu9dev_aperture_free(void)
{
	return (uint64_t)(g.end_page - g.next_page) * 4096;
}

uint64_t
gpu9dev_aperture_total(void)
{
	return (uint64_t)GPU9_APERTURE_SZ;
}

/* submit a batch at a GGTT address on the render ring; wait for the fence.
 * Same re-arm-every-submit discipline as lib/gpu9.c. Returns 0 or -1. */
int
gpu9dev_exec(uint32_t batch_ggtt)
{
	uint32_t n = 0, want;
	long long t0;

	g.ring[n++] = MI_BATCH_BUFFER_START_GEN8;
	g.ring[n++] = batch_ggtt;
	g.ring[n++] = 0;
	want = ++g.seqno;
	g.ring[n++] = PIPE_CONTROL_GEN8;
	g.ring[n++] = PC_POST_SYNC_WRITE_IMM | PC_DAT_GGTT | PC_CS_STALL |
			PC_RT_CACHE_FLUSH | PC_DC_FLUSH;
	g.ring[n++] = g.fence_page*4096;
	g.ring[n++] = 0;
	g.ring[n++] = want;
	g.ring[n++] = 0;
	while(n & 1) g.ring[n++] = MI_NOOP;

	g.fence[0] = 0;
	gpu9_wr(&g, GPU9_RCS_BASE+GPU9_RING_CTL, 0);
	gpu9_wr(&g, GPU9_RCS_BASE+GPU9_RING_HEAD, 0);
	gpu9_wr(&g, GPU9_RCS_BASE+GPU9_RING_TAIL, 0);
	gpu9_wr(&g, GPU9_RCS_BASE+GPU9_RING_START, g.ring_page*4096);
	gpu9_wr(&g, GPU9_RCS_BASE+GPU9_RING_CTL, ((1-1)<<12) | GPU9_RING_VALID);
	gpu9_rd(&g, GPU9_RCS_BASE+GPU9_RING_CTL);
	gpu9_wr(&g, GPU9_RCS_BASE+GPU9_RING_TAIL, n*4);
	gpu9_rd(&g, GPU9_RCS_BASE+GPU9_RING_TAIL);

	t0 = nowns();
	while(g.fence[0] != want){
		if(nowns() - t0 > 2000000000LL){
			fprintf(stderr, "[gpu9dev] exec hung (fence %.8x want %.8x HEAD %x)\n",
				g.fence[0], want, gpu9_rd(&g, GPU9_RCS_BASE+GPU9_RING_HEAD));
			return -1;
		}
	}
	return 0;
}

/* program GTT entries so GPU virtual address `va` aliases the physical pages
 * backing GGTT offset `ggtt` (both within our 64MB window). This is how iris's
 * softpin address for a BO is made to resolve, without PPGTT: the GPU reading
 * `va` hits the same stolen pages the CPU wrote at `ggtt`. */
void
gpu9dev_bind(uint32_t va, uint32_t ggtt, uint32_t bytes)
{
	uint32_t pages = (bytes + 4095)/4096, i;
	uint64_t phys = GPU9_STOLEN_BASE + ggtt;
	volatile uint32_t *gtt = (volatile uint32_t*)((char*)g.mmio + GPU9_GTT_OFFSET);
	for(i = 0; i < pages; i++){
		uint64_t pte = (phys + (uint64_t)i*4096) | 1;	/* present */
		gtt[(va/4096 + i)*2]     = (uint32_t)pte;
		gtt[(va/4096 + i)*2 + 1] = (uint32_t)(pte>>32);
	}
	gtt[(va/4096 + pages - 1)*2];			/* posting read of the last PTE */
	/* Invalidate the GGTT TLB so the GPU sees these fresh entries. Without this,
	 * a batch that references a just-bound address reads a stale/absent TLB entry
	 * and the engine wedges (HEAD frozen in the batch). GFX_FLSH_CNTL: write 1. */
	gpu9_wr(&g, GPU9_GFX_FLSH_CNTL, 1);
	gpu9_rd(&g, GPU9_GFX_FLSH_CNTL);
}
