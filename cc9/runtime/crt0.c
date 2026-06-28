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
/* __cxa_thread_atexit / _impl now live in pthread.c: they run per-thread on
 * THREAD exit (not process exit), so thread_local dtors and
 * promise::set_value_at_thread_exit fire correctly. */
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
int cc9_note_handler(unsigned long framesp)
{
	const char *msg = (const char *)(framesp + 24);
#ifndef CC9_RECURSE_PROBE
	/* "alarm" note (Plan 9 alarm(2)/setitimer): the SIGALRM analogue. Run the
	 * registered handler and resume (NCONT) — don't kill the process. Checked
	 * before the in-note guard so repeated timers keep working. Resuming after a
	 * note that interrupted a SLEEP syscall returns to user code past the syscall
	 * (the syscall reports interrupted); nanosleep re-sleeps the remaining time. */
	if (msg[0]=='a' && msg[1]=='l' && msg[2]=='a' && msg[3]=='r' && msg[4]=='m') {
		extern void cc9_run_sigalrm(void);
		cc9_run_sigalrm();
		return 0;   /* NCONT */
	}
#endif
	if (cc9_in_note) return 1;     /* reentrant fault: NDFLT (die) */
	cc9_in_note = 1;
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
	return 1;   /* NDFLT: report done, let the kernel terminate */
}

#ifdef CC9_STAGE_MARK
/* markers to fd 2 (unbuffered syscall, reaches the connection immediately) — robust
 * against the flaky on-box fs. Format: <STG:x> so they're greppable amid clang stderr. */
static void cc9_stage(char c){ char b[7]={'<','S','T','G',':',c,'>'}; n9_pwrite(2,b,7,-1); }
#define STAGE(c) cc9_stage(c)
#else
#define STAGE(c)
#endif
void __cc9_run(void)
{
	STAGE('a');
	cc9_fpmask();
	STAGE('b');
#ifndef CC9_NO_NOTE
	n9_notify((void *)cc9_notetramp);   /* report faults instead of dying silently */
#endif
	STAGE('c');
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
	STAGE('d');
	__cc9_build_environ();
	STAGE('e');
#ifdef CC9_PAUSE_ATTACH
	/* sleep at startup so a debugger can attach by pid (acid <pid>) — the process
	 * is kernel-exec'd so its bss stack is valid (unlike acid's own new()), and it
	 * sits in the SLEEP syscall (a note-point) so acid can cleanly stop it. */
	{ extern char *getenv(const char *); extern long n9_sleep(long);
	  if (getenv("CC9_PAUSE")) { for (int s = 0; s < 25; s++) n9_sleep(1000); } }
#endif
	STAGE('f');
	for (cc9_fn *p = __init_array_start; p < __init_array_end; ++p)
		(*p)();
	STAGE('g');
	int rc = main(argc, argv);
	STAGE('h');
	/* Run atexit/__cxa_atexit handlers LIFO, RE-READING atexit_n each step so a
	 * handler that registers a new one (e.g. a thread_local/static object whose
	 * destructor constructs another) has it run NEXT — [basic.start.term]: an
	 * object whose construction completes later is destroyed first. Decrement
	 * before calling so the new registration appends past the current entry. */
	while (atexit_n > 0) {
		int i = --atexit_n;
		atexit_tab[i].fn(atexit_tab[i].arg);
	}
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

/* Plan 9 per-process Top-Of-Stack page. The kernel passes its address in AX at
 * _start (saved there); libc's name for it is `_tos`. It carries the pid and the
 * process cycle counters — the native source for getpid()/clock(), with no
 * syscall. _tos sits 72 bytes below USTKTOP (0x7ffffffff000) and is exactly 72
 * bytes; an rfork(RFMEM) thread reading it gets ITS OWN Tos (own pid/cycles).
 *
 * Offsets are NOT computed from a portable mirror of <tos.h> — Plan 9 amd64 is
 * non-LP64 and the kernel's prof-header padding doesn't match what clang would lay
 * out, so the leading prof block is treated as opaque bytes. The tail offsets here
 * were verified by dumping _tos on the target (cirno): cyclefreq@40, pcycles@56,
 * pid@64 — cyclefreq read back as the 1.5 GHz CPU clock and pid as the live pid. */
typedef struct {
	unsigned long long _prof[5];    /* opaque prof header: 40 bytes, unused */
	unsigned long long cyclefreq;   /* @40: cycle-clock Hz, 0 if the machine has none */
	long long kcycles;              /* @48: cycles spent in kernel */
	long long pcycles;              /* @56: cycles spent in process (kernel+user) */
	unsigned pid;                   /* @64 */
	unsigned clock;                 /* @68 */
} Cc9Tos;
void *__cc9_tos = 0;   /* = AX at _start: the Plan 9 _tos pointer */
unsigned          cc9_tos_pid(void)       { Cc9Tos *t = __cc9_tos; return t ? t->pid : 0; }
long long         cc9_tos_pcycles(void)   { Cc9Tos *t = __cc9_tos; return t ? t->pcycles : 0; }
unsigned long long cc9_tos_cyclefreq(void){ Cc9Tos *t = __cc9_tos; return t ? t->cyclefreq : 0; }

#ifdef CC9_STAGE_MARK
char cc9_Ymsg[] = "<STG:Y>";   /* written by _start before any stack switch (raw syscall) */
#endif
__attribute__((naked, used)) void _start(void)
{
	__asm__ volatile(
		/* AX holds the Plan 9 _tos pointer at entry — save it before anything
		 * (the STAGE block below clobbers rax). */
		"movq %rax, __cc9_tos(%rip)\n\t"
#ifdef CC9_STAGE_MARK
		/* raw pwrite("<STG:Y>", fd 2) on the kernel entry stack (valid here), to prove
		 * _start actually runs. rsp is restored (net +/-48) before saving __cc9_ksp. */
		"subq $48, %rsp\n\t"
		"movl $2, 8(%rsp)\n\t"
		"leaq cc9_Ymsg(%rip), %rax\n\t"
		"movq %rax, 16(%rsp)\n\t"
		"movl $7, 24(%rsp)\n\t"
		"movq $-1, 32(%rsp)\n\t"
		"movq $51, %rbp\n\t"
		"syscall\n\t"
		"addq $48, %rsp\n\t"
#endif
		"movq %rsp, __cc9_ksp(%rip)\n\t"
		"leaq __cc9_main_stack(%rip), %rsp\n\t"
		"addq $" CC9_STR(CC9_STACK_BYTES) ", %rsp\n\t"   /* top of the stack */
		"andq $-16, %rsp\n\t"
		"call __cc9_run\n\t"
		/* __cc9_run never returns (n9_exits); spin rather than execute the
		 * privileged HLT if it somehow does. */
		"1: jmp 1b");
}
