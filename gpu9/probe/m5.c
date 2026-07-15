/*
 * gpu9 M5 — validate the driver library: execute a BATCH BUFFER.
 *
 * m3 put commands directly in the ring. Real drivers (and iris) build a batch
 * buffer and have the ring jump to it with MI_BATCH_BUFFER_START. This proves
 * that path, which is exactly what the ioctl shim's EXECBUFFER2 will do.
 */
#include <u.h>
#include <libc.h>
#include "gpu9.h"

#define MAGIC1	0xabcd0001
#define MAGIC2	0xabcd0002

void
main(int argc, char **argv)
{
	Gpu9 g;
	g9u32 batch_ggtt, dst_ggtt, *batch;
	volatile g9u32 *dst;
	int n;

	USED(argc); USED(argv);
	if(gpu9_open(&g) < 0)
		sysfatal("gpu9_open: %r");
	print("gpu9 open: mmio %p aper %p, first free page %ud\n",
		g.mmio, g.aper, g.next_page);

	dst_ggtt = gpu9_alloc(&g, 4096, (void**)&dst);
	batch_ggtt = gpu9_alloc(&g, 4096, (void**)&batch);
	if(dst_ggtt == 0 || batch_ggtt == 0)
		sysfatal("alloc: %r");
	dst[0] = 0; dst[1] = 0;

	/* a batch that writes two dwords, then ends */
	n = 0;
	batch[n++] = MI_STORE_DWORD_IMM_GEN8;
	batch[n++] = dst_ggtt;
	batch[n++] = 0;
	batch[n++] = MAGIC1;
	batch[n++] = MI_STORE_DWORD_IMM_GEN8;
	batch[n++] = dst_ggtt + 4;
	batch[n++] = 0;
	batch[n++] = MAGIC2;
	batch[n++] = MI_BATCH_BUFFER_END;
	while(n & 1) batch[n++] = MI_NOOP;

	print("batch at GGTT %#ux -> dst %#ux\n", batch_ggtt, dst_ggtt);
	if(gpu9_exec(&g, batch_ggtt, n*4) < 0)
		sysfatal("gpu9_exec: %r");

	print("dst[0] = %.8ux (want %.8ux)\n", dst[0], MAGIC1);
	print("dst[1] = %.8ux (want %.8ux)\n", dst[1], MAGIC2);
	if(dst[0] == MAGIC1 && dst[1] == MAGIC2)
		print("*** M5 PASS: batch buffer executed — the iris EXECBUFFER2 path works ***\n");
	else
		print("M5 FAIL\n");
	gpu9_close(&g);
	exits(nil);
}
