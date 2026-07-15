/* m5b — isolate: is the fault in the LIBRARY's ring setup, or in
 * MI_BATCH_BUFFER_START? Run the same store both ways through gpu9.c's state. */
#include <u.h>
#include <libc.h>
#include "gpu9.h"

void
main(int argc, char **argv)
{
	Gpu9 g;
	g9u32 dst_ggtt, n, want;
	volatile g9u32 *dst;
	int i;

	USED(argc); USED(argv);
	if(gpu9_open(&g) < 0) sysfatal("gpu9_open: %r");
	dst_ggtt = gpu9_alloc(&g, 4096, (void**)&dst);
	dst[0] = 0;

	print("ring page %ud (GGTT %#ux), fence page %ud\n", g.ring_page, g.ring_page*4096, g.fence_page);
	print("CTL %.8ux START %.8ux HEAD %.8ux TAIL %.8ux\n",
		gpu9_rd(&g, 0x203c), gpu9_rd(&g, 0x2038),
		gpu9_rd(&g, 0x2034), gpu9_rd(&g, 0x2030));

	/* Direct store in the ring — exactly what m3 did and it worked. */
	n = 0;
	g.ring[n++] = MI_STORE_DWORD_IMM_GEN8;
	g.ring[n++] = dst_ggtt;
	g.ring[n++] = 0;
	g.ring[n++] = 0x11112222;
	want = ++g.seqno;
	g.ring[n++] = MI_FLUSH_DW_GEN8;
	g.ring[n++] = (g.fence_page*4096) | MI_FLUSH_DW_USE_GTT;
	g.ring[n++] = 0;
	g.ring[n++] = want;
	while(n & 1) g.ring[n++] = MI_NOOP;
	g.fence[0] = 0;

	gpu9_wr(&g, 0x203c, 0);
	gpu9_wr(&g, 0x2034, 0);
	gpu9_wr(&g, 0x2030, 0);
	gpu9_wr(&g, 0x2038, g.ring_page*4096);
	gpu9_wr(&g, 0x203c, ((1-1)<<12) | 1);
	gpu9_rd(&g, 0x203c);
	gpu9_wr(&g, 0x2030, n*4);
	gpu9_rd(&g, 0x2030);
	for(i=0;i<2000000;i++) if(g.fence[0] == want) break;
	print("DIRECT: HEAD %.8ux TAIL %.8ux dst %.8ux fence %.8ux %s\n",
		gpu9_rd(&g, 0x2034), gpu9_rd(&g, 0x2030), dst[0], g.fence[0],
		dst[0]==0x11112222 ? "PASS" : "FAIL");

	gpu9_close(&g);
	exits(nil);
}
