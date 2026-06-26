/*
 * cc9 C runtime startup (crt0). The kernel jumps to _start; we 16-align the
 * stack, run the global constructors registered in .init_array, call
 * int main(), then run .fini_array and exit with main's status. This is what
 * makes a normal `int main()` program work — global objects get constructed,
 * unlike a bare _start.
 */
typedef void (*cc9_fn)(void);
extern cc9_fn __init_array_start[];
extern cc9_fn __init_array_end[];
extern cc9_fn __fini_array_start[];
extern cc9_fn __fini_array_end[];
extern int main(int, char **);
extern void n9_exits(const char *);

/* atexit registry (libc++ and user code register destructors here). */
#define CC9_ATEXIT_MAX 64
static cc9_fn atexit_fns[CC9_ATEXIT_MAX];
static int atexit_n = 0;
int atexit(cc9_fn f) { if (atexit_n < CC9_ATEXIT_MAX) { atexit_fns[atexit_n++] = f; return 0; } return -1; }
/* __cxa_atexit(func, arg, dso): C++ static-dtor registration. We ignore arg/dso
 * (only matters for objects with state-bearing dtors run at exit). */
int __cxa_atexit(void (*f)(void *), void *arg, void *dso) { (void)arg; (void)dso; if (atexit_n < CC9_ATEXIT_MAX) { atexit_fns[atexit_n++] = (cc9_fn)f; return 0; } return -1; }
void *__dso_handle = 0;

/* Mask SSE + x87 FP exceptions. Bare-metal 9front leaves them UNMASKED, so a
 * divide-by-zero / 0.0÷0.0 / overflow raises a fault and the process suicides
 * (QEMU TCG hides this — same hazard class as the node9 FP-divzero trap). Each
 * process needs this: main here, and every rfork(RFMEM) thread (which starts
 * with fresh FP state) calls it from the pthread trampoline. */
void cc9_fpmask(void)
{
	unsigned int mxcsr = 0x1F80;    /* all SSE exception masks set, round-nearest */
	unsigned short cw = 0x037F;     /* x87 control word: all masks, 64-bit precision */
	__asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
	__asm__ volatile("fldcw %0" :: "m"(cw));
}

void __cc9_run(void)
{
	cc9_fpmask();
	for (cc9_fn *p = __init_array_start; p < __init_array_end; ++p)
		(*p)();
	static char *argv[] = { (char *)"a.out", 0 };
	int rc = main(1, argv);
	for (int i = atexit_n; i > 0; --i)
		(*atexit_fns[i - 1])();
	for (cc9_fn *p = __fini_array_end; p > __fini_array_start; )
		(*--p)();
	n9_exits(rc == 0 ? (char *)0 : (char *)"cc9: nonzero exit");
}

/* main() runs on this BSS stack, NOT the kernel-provided per-process stack.
 * rfork(RFMEM) threads share data/bss/heap but each process keeps its OWN stack
 * (same vaddr, different memory), so a captured-by-reference local would not be
 * visible across threads unless it lives in a shared segment. BSS is shared, so
 * putting main's stack here makes the ubiquitous `std::thread([&]{...})` pattern
 * work. Thread stacks are heap-allocated (also shared). */
#define CC9_MAIN_STACK (8*1024*1024)
__attribute__((aligned(16), used)) char __cc9_main_stack[CC9_MAIN_STACK];

__attribute__((naked, used)) void _start(void)
{
	__asm__ volatile(
		"leaq __cc9_main_stack(%rip), %rsp\n\t"
		"addq $" "8388608" ", %rsp\n\t"   /* top of the 8 MiB stack */
		"andq $-16, %rsp\n\t"
		"call __cc9_run\n\t"
		/* __cc9_run never returns (n9_exits); spin rather than execute the
		 * privileged HLT if it somehow does. */
		"1: jmp 1b");
}
