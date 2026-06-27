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

/* atexit registry (libc++ and user code register destructors here). Each entry
 * is a (fn, arg) pair: __cxa_atexit MUST pass the object pointer back as arg, or
 * a global/static object's destructor runs with a garbage `this` and faults. A
 * plain atexit fn takes no arg, so we store arg=0 and the extra (ignored) SysV
 * register is harmless. */
#define CC9_ATEXIT_MAX 256
static struct { void (*fn)(void *); void *arg; } atexit_tab[CC9_ATEXIT_MAX];
static int atexit_n = 0;
int atexit(cc9_fn f) { if (atexit_n < CC9_ATEXIT_MAX) { atexit_tab[atexit_n].fn = (void (*)(void *))f; atexit_tab[atexit_n].arg = 0; atexit_n++; return 0; } return -1; }
int __cxa_atexit(void (*f)(void *), void *arg, void *dso) { (void)dso; if (atexit_n < CC9_ATEXIT_MAX) { atexit_tab[atexit_n].fn = f; atexit_tab[atexit_n].arg = arg; atexit_n++; return 0; } return -1; }
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
	__asm__ volatile("fninit");     /* reset x87 to a known-clean state first */
	__asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
	__asm__ volatile("fldcw %0" :: "m"(cw));
}

extern unsigned long __cc9_ksp;
extern void __cc9_build_environ(void);
extern long n9_pwrite(int, const void *, long, long long);
extern long n9_notify(void *);
extern void cc9_notetramp(void);

/* Note handler: cc9 had none, so a faulting program died silently. Now crt0
 * registers cc9_notetramp (asm), whose C side writes the kernel's note string —
 * "sys: trap: fault read addr=0x.. pc=0x.." — to fd 2, then noted(NDFLT) lets the
 * kernel print its `prog pid: suicide:` line and terminate. The note string sits
 * inline in the kernel-built note frame on amd64 (empirically at framesp+24); a
 * reentry guard means a bad read can't loop. (A stack-overflow fault can't be
 * reported — the kernel can't build the note frame on a blown stack.) */
static volatile int cc9_in_note;
void cc9_note_handler(unsigned long framesp)
{
	if (cc9_in_note) return;       /* reentrant fault: bail, asm calls noted() */
	cc9_in_note = 1;
	const char *msg = (const char *)(framesp + 24);
	int n = 0; while (n < 200 && msg[n] >= 32 && msg[n] < 127) n++;
#ifdef CC9_FAULT_FILE
	/* Write the note + a 46-slot frame dump to a FILE — survives the listener
	 * connection dying on a bare-metal fault (where fd 2 is the dead socket). */
	{ extern long n9_create(const char *, int, int);
	  int ffd = n9_create("/tmp/cc9fault", 1 /*OWRITE*/, 0666);
	  if (ffd >= 0) {
		if (n > 4) { n9_pwrite(ffd, "note: ", 6, -1); n9_pwrite(ffd, msg, n, -1); n9_pwrite(ffd, "\n", 1, -1); }
		unsigned long *f = (unsigned long *)framesp;
		n9_pwrite(ffd, "frame:\n", 7, -1);
		for (int i = 0; i < 46; i++) {
			unsigned long v = f[i];
			char b[36]; int k=0;
			b[k++]='['; if(i>=10)b[k++]='0'+i/10; b[k++]='0'+i%10; b[k++]=']'; b[k++]=' ';
			b[k++]='0'; b[k++]='x';
			for (int j=15;j>=0;j--){ int d=(v>>(j*4))&0xf; b[k++]=d<10?'0'+d:'a'+d-10; }
			b[k++]=' ';
			for (int j=0;j<8;j++){ char c=(char)(v>>(j*8)); b[k++]=(c>=32&&c<127)?c:'.'; }
			b[k++]='\n'; n9_pwrite(ffd, b, k, -1);
		}
	  }
	}
#endif
#ifdef CC9_RECURSE_PROBE
	/* On an alarm note (timer fired mid-recursion, stack not yet blown), walk the
	 * INTERRUPTED frame chain from the Ureg's saved RBP — amd64 Ureg sits at
	 * framesp+160 with bp at slot 26 (framesp+208), per the kernel layout. Dumps
	 * return addresses to fd 2 (the recursion cycle), then exits. */
	if (msg[0]=='a' && msg[1]=='l' && msg[2]=='a' && msg[3]=='r') {
		/* raw frame dump (no deref — can't fault): find PC/SP/BP slots for the
		 * async-note Ureg empirically. */
		unsigned long *f = (unsigned long *)framesp;
		n9_pwrite(2, "CC9-ALARM-FRAME:\n", 17, -1);
		for (int i = 0; i < 60; i++) {
			unsigned long v = f[i];
			char b[28]; int k=0;
			b[k++]='['; if(i>=10)b[k++]='0'+i/10; b[k++]='0'+i%10; b[k++]=']'; b[k++]=' ';
			b[k++]='0'; b[k++]='x';
			for (int j=15;j>=0;j--){ int d=(v>>(j*4))&0xf; b[k++]=d<10?'0'+d:'a'+d-10; }
			b[k++]='\n'; n9_pwrite(2, b, k, -1);
		}
		n9_exits("cc9-alarm");
	}
#endif
	if (n > 4) { n9_pwrite(2, "cc9 fault: ", 11, -1); n9_pwrite(2, msg, n, -1); n9_pwrite(2, "\n", 1, -1); }
#ifdef CC9_RECURSE_PROBE
	/* robust fallback: the note-frame layout varies by kernel build, so msg may not
	 * be at framesp+24. Dump 46 frame slots (hex + inline ASCII) so the note string
	 * + Ureg PC are recoverable regardless of offset. */
	else {
		unsigned long *f = (unsigned long *)framesp;
		n9_pwrite(2, "cc9 note frame:\n", 16, -1);
		for (int i = 0; i < 46; i++) {
			unsigned long v = f[i];
			char b[36]; int k=0;
			b[k++]='['; if(i>=10)b[k++]='0'+i/10; b[k++]='0'+i%10; b[k++]=']'; b[k++]=' ';
			b[k++]='0'; b[k++]='x';
			for (int j=15;j>=0;j--){ int d=(v>>(j*4))&0xf; b[k++]=d<10?'0'+d:'a'+d-10; }
			b[k++]=' ';
			for (int j=0;j<8;j++){ char c=(char)(v>>(j*8)); b[k++]=(c>=32&&c<127)?c:'.'; }
			b[k++]='\n'; n9_pwrite(2, b, k, -1);
		}
	}
#endif
}

void __cc9_run(void)
{
	cc9_fpmask();
#ifndef CC9_NO_NOTE
	n9_notify((void *)cc9_notetramp);   /* report faults instead of dying silently */
#endif
#ifdef CC9_RECURSE_PROBE
	{ extern long n9_alarm(unsigned long); n9_alarm(1500); }  /* timer to sample a runaway recursion */
#endif
	/* Real argc/argv from the kernel entry stack: at SP sits argc, then the
	 * NULL-terminated argv pointer array (Plan 9 amd64 entry ABI). */
	static char *fallback[] = { (char *)"a.out", 0 };
	int argc; char **argv;
	if (__cc9_ksp) {
		long *ks = (long *)__cc9_ksp;
		argc = (int)ks[0];
		argv = (char **)(ks + 1);
	} else { argc = 1; argv = fallback; }
	__cc9_build_environ();
#ifdef CC9_PAUSE_ATTACH
	/* sleep at startup so a debugger can attach by pid (acid <pid>) — the process
	 * is kernel-exec'd so its bss stack is valid (unlike acid's own new()), and it
	 * sits in the SLEEP syscall (a note-point) so acid can cleanly stop it. */
	{ extern char *getenv(const char *); extern long n9_sleep(long);
	  if (getenv("CC9_PAUSE")) { for (int s = 0; s < 25; s++) n9_sleep(1000); } }
#endif
	for (cc9_fn *p = __init_array_start; p < __init_array_end; ++p)
		(*p)();
	int rc = main(argc, argv);
	for (int i = atexit_n; i > 0; --i)
		atexit_tab[i - 1].fn(atexit_tab[i - 1].arg);
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
/* clang -cc1 recurses deeply (AST/type canonicalization, codegen); an 8 MiB
 * stack overflows on non-trivial C++. In a dedicated NOLOAD section so lld keeps
 * it NOBITS — otherwise lld file-backs this huge array (p_filesz == p_memsz) and
 * the a.out balloons by the full stack size. NOLOAD ⇒ zero file bytes; the kernel
 * zero-fills it as bss, demand-paged, so the size is virtual until touched. */
#ifndef CC9_STACK_BYTES
#define CC9_STACK_BYTES 268435456
#endif
__attribute__((section(".cc9stack"), aligned(16), used)) char __cc9_main_stack[CC9_STACK_BYTES];
#define CC9_STR2(x) #x
#define CC9_STR(x) CC9_STR2(x)

/* The kernel-provided entry stack (argc + the NULL-terminated argv pointer array
 * live here). _start saves SP before switching to __cc9_main_stack; __cc9_run
 * parses argv from it. */
unsigned long __cc9_ksp = 0;

__attribute__((naked, used)) void _start(void)
{
	__asm__ volatile(
		"movq %rsp, __cc9_ksp(%rip)\n\t"
		"leaq __cc9_main_stack(%rip), %rsp\n\t"
		"addq $" CC9_STR(CC9_STACK_BYTES) ", %rsp\n\t"   /* top of the stack */
		"andq $-16, %rsp\n\t"
		"call __cc9_run\n\t"
		/* __cc9_run never returns (n9_exits); spin rather than execute the
		 * privileged HLT if it somehow does. */
		"1: jmp 1b");
}
