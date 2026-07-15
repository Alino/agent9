/*
 * gpu9 M3 — THE SPIKE. Make the GPU execute one instruction.
 *
 * Build a legacy render ring in GTT-mapped stolen memory, put a single
 * MI_STORE_DATA_IMM in it, kick RING_TAIL, and see the GPU write a magic DWORD
 * into memory the CPU can read. This is the "f()=42" of a GPU driver: if the
 * value lands, the GPU is executing OUR commands and everything after is grind.
 *
 * Layout (aperture offset == GGTT address, see m2.c):
 *   page 2048 -> GPU 0x800000 : the ring
 *   page 2049 -> GPU 0x801000 : the target
 */
#include <u.h>
#include <libc.h>

#define BAR0SZ	(16*1024*1024)
#define APSZ	0x4000000

#define FORCEWAKE_MT		0xa188
#define FORCEWAKE_ACK_HSW	0x130044
#define FW_KERNEL		1
#define MASKED_ENABLE(x)	((x) | ((x)<<16))
#define MASKED_DISABLE(x)	((x)<<16)

#define RCS_TAIL	0x2030
#define RCS_HEAD	0x2034
#define RCS_START	0x2038
#define RCS_CTL		0x203c

#define RING_VALID	1
#define MI_NOOP		0x00000000
/* MI_STORE_DATA_IMM: opcode 0x20, bit22 = use GGTT, len = dwords-2.
 * Gen8 form: DW0 hdr | DW1 addr_lo | DW2 addr_hi | DW3 data  -> len 2 */
#define MI_STORE_DWORD_IMM_GEN8	((0x20<<23) | (1<<22) | 2)

#define RINGPG	2048
#define DSTPG	2049
#define RINGGPU	(RINGPG*4096)
#define DSTGPU	(DSTPG*4096)
#define MAGIC	0x42424242

static u32int *mmio;
static uchar  *aper;
static u32int rd(u32int o){ return mmio[o/4]; }
static void wr(u32int o, u32int v){ mmio[o/4] = v; }

static int
fwget(void)
{
	int i;
	wr(FORCEWAKE_MT, MASKED_ENABLE(FW_KERNEL));
	rd(FORCEWAKE_MT);
	for(i = 0; i < 500; i++){
		if(rd(FORCEWAKE_ACK_HSW) & FW_KERNEL) return 1;
		sleep(1);
	}
	return 0;
}

void
main(int argc, char **argv)
{
	u32int *ring, *dst, ctl, head, tail;
	int i, n;

	USED(argc); USED(argv);
	mmio = segattach(0, "igfxmmio", 0, BAR0SZ);
	if(mmio == (void*)-1) sysfatal("segattach igfxmmio: %r");
	aper = segattach(0, "igfxscreen", 0, APSZ);
	if(aper == (void*)-1) sysfatal("segattach igfxscreen: %r");

	if(!fwget()) sysfatal("forcewake failed");

	ring = (u32int*)(aper + RINGGPU);
	dst  = (u32int*)(aper + DSTGPU);

	/* clear target so we can't fool ourselves with stale data */
	dst[0] = 0;
	dst[1] = 0;
	print("target before: %.8ux (GPU addr %#ux)\n", dst[0], DSTGPU);

	/* fill ring with NOOPs, then our store at offset 0 */
	for(i = 0; i < 4096/4; i++)
		ring[i] = MI_NOOP;
	n = 0;
	ring[n++] = MI_STORE_DWORD_IMM_GEN8;
	ring[n++] = DSTGPU;		/* addr lo (GGTT) */
	ring[n++] = 0;			/* addr hi */
	ring[n++] = MAGIC;		/* the data */
	/* pad to a QWORD boundary with NOOPs; TAIL must be 8-byte aligned */
	while(n & 1) ring[n++] = MI_NOOP;

	/* stop the ring, point it at ours, restart */
	wr(RCS_CTL, 0);
	wr(RCS_HEAD, 0);
	wr(RCS_TAIL, 0);
	wr(RCS_START, RINGGPU);
	ctl = ((1-1)<<12) | RING_VALID;		/* 1 page */
	wr(RCS_CTL, ctl);
	rd(RCS_CTL);
	print("ring armed: START %.8ux CTL %.8ux HEAD %.8ux\n",
		rd(RCS_START), rd(RCS_CTL), rd(RCS_HEAD));

	/* GO: advance TAIL past our commands */
	wr(RCS_TAIL, n*4);
	rd(RCS_TAIL);
	print("kicked TAIL to %d bytes\n", n*4);

	for(i = 0; i < 1000; i++){
		head = rd(RCS_HEAD) & 0x1ffffc;
		tail = rd(RCS_TAIL);
		if(dst[0] == MAGIC) break;
		if(head == tail && i > 10) break;
		sleep(1);
	}
	print("after: HEAD %.8ux TAIL %.8ux  target = %.8ux\n",
		rd(RCS_HEAD), rd(RCS_TAIL), dst[0]);

	if(dst[0] == MAGIC)
		print("*** M3 PASS: THE GPU EXECUTED OUR COMMAND (wrote %.8ux) ***\n", MAGIC);
	else
		print("M3 FAIL: target still %.8ux (expected %.8ux)\n", dst[0], MAGIC);

	wr(FORCEWAKE_MT, MASKED_DISABLE(FW_KERNEL));
	exits(nil);
}
