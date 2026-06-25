/*
 * exec_probe — does 9front amd64 allow running machine code from writable
 * memory? (the JIT/V8 gate). Each memory class is tested in a forked child,
 * so a fault in one class doesn't kill the others; the parent reads the
 * child's wait message ("" = clean exit = executed; "sys: trap..." = faulted).
 *
 * Build:  6c -FTVw exec_probe.c && 6l -o exec_probe exec_probe.6
 */
#include <u.h>
#include <libc.h>

/* amd64: mov eax,42 ; ret  ==  b8 2a 00 00 00 c3  -> a function returning 42 */
static uchar code[] = { 0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3 };
typedef int (*fn)(void);

static uchar bssbuf[64];

static void
try(char *label, uchar *buf)
{
	int pid;
	Waitmsg *w;

	if(buf == nil){ print("%s: no buffer\n", label); return; }
	pid = fork();
	if(pid < 0){ print("%s: fork failed: %r\n", label); return; }
	if(pid == 0){
		fn f;
		memcpy(buf, code, sizeof code);
		f = (fn)buf;
		if(f() == 42)
			exits(nil);          /* clean: executed correctly */
		exits("wrongval");
	}
	w = wait();
	if(w == nil){ print("%s: wait failed: %r\n", label); return; }
	if(w->msg[0] == 0)
		print("%s: EXEC OK (returned 42)\n", label);
	else
		print("%s: FAULT/err: %s\n", label, w->msg);
	free(w);
}

void
main(void)
{
	uchar *heap;
	uchar stackbuf[64];
	void *seg;

	print("== exec-memory probe (9front amd64) ==\n");

	try("bss   ", bssbuf);

	heap = malloc(64);
	try("malloc", heap);

	try("stack ", stackbuf);

	seg = segattach(0, "memory", 0, 64);
	if(seg == (void*)-1)
		print("segatt: segattach failed: %r\n");
	else
		try("segatt", (uchar*)seg);

	print("PROBE_DONE\n");
	exits(nil);
}
