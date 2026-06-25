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

void __cc9_run(void)
{
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

__attribute__((naked, used)) void _start(void)
{
	__asm__ volatile("andq $-16, %rsp\n\t"
	                 "call __cc9_run\n\t"
	                 "hlt");
}
