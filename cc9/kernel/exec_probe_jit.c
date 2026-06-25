/*
 * exec_probe_jit.c — verify the optional-JIT kernel patch (wxallow + SG_EXEC).
 * Build/run on the box: 6c exec_probe_jit.c && 6l -o jit exec_probe_jit.6 && jit
 *
 * With the patched kernel + plan9.ini wxallow=1: prints "result = 42".
 * With wxallow=0 (or stock kernel): the call faults (NX) — proving the gate.
 * Pass 0 instead of SG_EXEC to confirm a plain segment is never executable.
 */
#include <u.h>
#include <libc.h>

enum { SG_EXEC = 04000 };	/* must match the patched kernel's portdat.h */

void
main(int argc, char **argv)
{
	uchar *p;
	uchar code[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};	/* MOVL $42,AX; RET */
	int (*f)(void);
	int i, attr;

	attr = SG_EXEC;
	if(argc > 1 && strcmp(argv[1], "-plain") == 0)
		attr = 0;	/* request a NON-executable segment (control) */

	p = segattach(attr, "memory", 0, 0x1000);
	if(p == (void*)-1){
		print("segattach(attr=%#o) failed: %r\n", attr);
		exits("segattach");
	}
	for(i = 0; i < sizeof code; i++)
		p[i] = code[i];

	f = (int(*)(void))p;
	print("seg @ %p attr=%#o; calling... result = %d (expect 42 iff exec allowed)\n",
		p, attr, f());
	exits(nil);
}
