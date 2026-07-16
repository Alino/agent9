/* cc9 pthreads over Plan 9 rfork(RFMEM) + semaphores (SEMACQUIRE/SEMRELEASE).
 * pthread_t is the Plan 9 pid. Threads share the address space (RFMEM), so
 * mutexes/condvars are plain shared-memory semaphores and "thread-local" data
 * is keyed by pid. The thread-creation handoff (n9_th_*) is serialized by a
 * lock since it goes through shared globals (see n9syscall.s). */
typedef unsigned long size_t;
extern long n9_rfork_thread(void *, void (*)(void *), void *);
extern int n9_semacquire(int *, int);
extern int n9_semrelease(int *, int);
extern long n9_sleep(long);
extern long n9_open(const char *, int);
extern long n9_pread(int, void *, long, long long);
extern long n9_close(int);
extern void *malloc(size_t);
extern void *realloc(void *, size_t);
extern void free(void *);
extern void cc9_fpmask(void);   /* mask FP exceptions for this rfork'd thread (crt0.c) */
extern void n9_exits(const char *);

#include <pthread.h>

#define STACKSIZE (256*1024)

/* Per-create stack size override. Callers that need a bigger stack than the
 * 256K default (e.g. rustc's deeply-recursive compile thread wants 8M) set this
 * right before pthread_create; 0 means "use STACKSIZE". Left sticky — threads in
 * such programs are few and want uniform large stacks. */
static long cc9_thread_stack = 0;
void cc9_set_thread_stack(long n){ cc9_thread_stack = n; }

/* pid is the SYNTHETIC tid (pthread_t, a counter from 2) used for join/table
 * lookups. realpid is the kernel pid from rfork's return in the parent — the
 * ONLY value /proc accepts. kill_threads used to post notes to the tid
 * (/proc/2/note, /proc/3/note...): on a fresh boot those are real early
 * system processes (webfs! rio!) — every threaded cc9 program exit was
 * murdering them — while the actual threads never died. */
/* stack = the raw malloc pointer (what gets freed); stkbase = the pow2-aligned
 * base the thread actually runs above (see the TL1 redzone in pthread_create). */
typedef struct { void *(*start)(void *); void *arg; void *ret; int joinsem; int done; int detached; void *stack; unsigned long stkbase; unsigned long stksize; unsigned long pid; unsigned long realpid; } n9_thread;

/* Identify the current thread WITHOUT a real getpid (/dev/pid isn't in the
 * listener namespace, and main's BSS stack has no kernel TOS). Each thread has a
 * distinct stack region, so the current %rsp tells us which thread we are. The
 * thread table (below) records each thread's stack range; main is the default. */
static int create_lock = 1;       /* serializes the rfork handoff */
static int handoff_sem = 0;       /* child signals it has consumed the handoff */

#define MAXTH 1024
/* stklo/stkhi are SCALARS (not derived from t->stack at scan time) so cur_pid
 * never dereferences t — t/stack may be freed by a concurrent join. On amd64
 * (TSO) the lock-free scan is safe: the writer sets stklo/stkhi/pid BEFORE used,
 * and a cleared slot (used=0) is skipped, so no stale match and no use-after-free. */
/* dead=1: the thread finished (trampoline end / pthread_exit park) but its
 * slot may still be needed for pthread_join lookup. kill_threads MUST skip
 * dead entries — Plan 9 reuses pids, and a "kill" note aimed at a finished
 * thread's recycled pid murders an innocent process (this killed webfs and
 * rio's wsys srv during nvim sessions). */
/* etls/etls_n: this thread's emulated-TLS array (upstream emutls.c keeps it in a
 * pthread key; we hang it here because Plan 9 rfork(RFMEM) has no per-thread %fs).
 * tsd: this thread's POSIX key array (upstream: pthread_self()->tsd) — same reason. */
static struct { int used; int dead; unsigned long pid; n9_thread *t; unsigned long stklo, stkhi; int errno_v; char errstr_v[160]; int errstr_errno_v; void **etls; unsigned long etls_n; void **tsd; void *mcache; } th_tab[MAXTH];
static int th_lock = 1;
/* High-water mark: highest claimed slot + 1. The %rsp scans below run on EVERY
 * errno access and EVERY thread_local access (emutls), so scanning all MAXTH=1024
 * slots — which is what they did — cost microseconds per call. Monotonic and
 * published (under th_lock) BEFORE `used`, so a scanner that sees a slot's
 * used=1 also sees a th_hi covering it. A scanner only ever needs to find its
 * OWN slot, registered before it ran, so a stale (smaller) read is still safe. */
static int th_hi = 0;

/* Main thread's stack is the BSS array crt0 switches %rsp to — an exact range
 * check, so main resolves without touching the table at all. */
extern char __cc9_main_stack[];
#ifndef CC9_STACK_BYTES
#define CC9_STACK_BYTES 268435456
#endif
static inline int on_main_stack(unsigned long sp){
	return sp >= (unsigned long)__cc9_main_stack &&
	       sp <  (unsigned long)__cc9_main_stack + (unsigned long)CC9_STACK_BYTES;
}

/* TL1 redzone: pthread_create makes every spawned stack pow2-aligned and lays
 * 5 words just BELOW the aligned base (inside the same malloc block):
 *   base[-1..-3]  canary   — first thing a stack overflow smashes (S1)
 *   base[-4]      th_tab index of the owning thread (the "m at stack base" stash)
 *   base[-5]      sentinel, XOR-bound to base so arbitrary stack bytes can't fake it
 * Keeping them below base (not carved out of the top) means the thread gets every
 * byte of the ss it asked for, and a pow2 ss doesn't round the alignment to 2*ss. */
#define STK_RED      64
#define STK_CANARY   0xC0DEFACEDEADBEEFUL
#define STK_STASH    0x9BA5E0FC0DE517A5UL
#define STK_MINALIGN ((unsigned long)STACKSIZE)   /* smallest stack == smallest align */
static unsigned long stk_floor = ~0UL;         /* lowest aligned base ever: probes never read below it */
static unsigned long stk_maxal = STK_MINALIGN; /* largest alignment in use: bounds the probe loop */

/* th_tab index of the CALLING thread, or -1 for main (whose per-thread state
 * lives in file-scope main_* below, since main has no slot). This is cc9's
 * pthread_self()->tcb: upstream reaches per-thread storage with one %fs load,
 * but rfork(RFMEM) threads share %fs, so %rsp range is the closest we get.
 *
 * O(1), not a scan (Go's "m at the stack base" trick): the stack is pow2-aligned,
 * so masking %rsp recovers the base where create stashed our th_tab index. The
 * alignment varies per thread (attr stacksize), so probe each pow2 from the 256K
 * floor up — 1 probe for default stacks, ~6 for an 8M stylo stack, never O(threads).
 * Probe reads are safe: a candidate below the true alignment lands inside our own
 * mapped stack; anything at/above stk_floor is inside the contiguous sbrk arena;
 * below stk_floor we stop. A probe hit is VERIFIED against the slot's published
 * range — live stack ranges are disjoint, so a slot whose range contains sp IS the
 * caller's slot, and a forged/stale sentinel can never mis-resolve (it just falls
 * through). Same lock-free TSO discipline as cur_pid: `used` is read first, the
 * scalar range after, t is never dereferenced, and th_hi bounds everything. */
static int th_slot(void){
	unsigned long sp; __asm__ volatile("movq %%rsp,%0":"=r"(sp));
	if(on_main_stack(sp)) return -1;   /* main: O(1), no scan */
	unsigned long maxal = stk_maxal, flo = stk_floor;
	for(unsigned long al = STK_MINALIGN; al <= maxal; al <<= 1){
		unsigned long base = sp & ~(al-1);
		if(base < flo) break;              /* below every stack: could be unmapped */
		unsigned long *b = (unsigned long *)base;
		if(b[-5] != (STK_STASH ^ base)) continue;
		unsigned long i = b[-4];
		if(i >= (unsigned long)th_hi || !th_tab[i].used) continue;
		__asm__ volatile("":::"memory");   /* acquire: don't hoist range reads before used */
		if(sp>=th_tab[i].stklo && sp<th_tab[i].stkhi) return (int)i;
	}
	/* Fallback: the pre-registration window, a smashed stash (stack overflow),
	 * or a >1G stack (create caps the alignment) — the original range scan. */
	int hi = th_hi;
	for(int i=0;i<hi;i++){
		if(!th_tab[i].used) continue;
		__asm__ volatile("":::"memory");   /* acquire: don't hoist range reads before used */
		if(sp>=th_tab[i].stklo && sp<th_tab[i].stkhi) return i;
	}
	return -1;
}

/* Per-thread errno slot, resolved by %rsp stack-range like cur_pid (lock-free,
 * same TSO discipline). Weak-referenced from n9libc's __n9_errno so thread-free
 * links never pull pthread.o. Main thread (or a reaped slot) -> 0 = use the
 * global. A slot reused after join could absorb a stale write through a saved
 * errno pointer — errno is written-then-read immediately, so harmless. */
int *cc9_thread_errno_slot(void){
	int s = th_slot();
	return s < 0 ? 0 : &th_tab[s].errno_v;       /* main (or a reaped slot) -> global errno */
}

/* The errstr text that goes with that errno, resolved the same way.
 *
 * It has to be per-thread for the same reason errno is. It used to be one
 * process-global buffer guarded by "the caller compares the stashed errno to the
 * one it holds", which does not work: two unrelated failures on two threads
 * routinely share an errno (ENOENT), the guard passes, and the caller prints a
 * DIFFERENT thread's error text. Servo reported a missing font directory as
 * "file does not exist: '.../reg.sqlite-wal'" — a real file, from another
 * thread, named in an error that had nothing to do with it. A misleading message
 * is worse than a vague one, because it sends the reader somewhere else. */
/* Hands back the slot's capacity too: the buffer lives here but is written in
 * fs.c, and a bare char* would leave its size duplicated in two translation
 * units with nothing to keep them in step — one edit apart from overflowing the
 * whole thread table. */
char *cc9_thread_errstr_slot(int **eno_out, int *cap_out){
	int i = th_slot();   /* O(1) stash lookup; -1 for main, as the old scan-miss was */
	if(i < 0) return 0;
	if(eno_out) *eno_out = &th_tab[i].errstr_errno_v;
	if(cap_out) *cap_out = (int)sizeof th_tab[i].errstr_v;
	return th_tab[i].errstr_v;
}

/* Stack bounds of the CALLING thread, resolved by %rsp against the same table
 * cc9_thread_errno_slot uses (same lock-free TSO discipline). Conservative
 * garbage collectors need this to know what range to scan for roots —
 * SpiderMonkey's GetNativeStackBase is the first caller.
 *
 * A thread not in the table is the main thread, whose stack is crt0's NOBITS
 * .cc9stack arena rather than a malloc'd block, so it has no th_tab slot. */
#ifndef CC9_STACK_BYTES
#define CC9_STACK_BYTES 268435456
#endif
extern char __cc9_main_stack[];

int cc9_stack_bounds(void **lo, void **hi){
	int i = th_slot();   /* O(1) stash lookup; -1 for main, as the old scan-miss was */
	if(i >= 0){
		*lo = (void*)th_tab[i].stklo;
		*hi = (void*)th_tab[i].stkhi;
		return 0;
	}
	*lo = __cc9_main_stack;
	*hi = __cc9_main_stack + (unsigned long)CC9_STACK_BYTES;
	return 0;
}

/* S1: fault triage for crt0's note handler — did a thread blow its heap stack?
 * No guard page is possible on Plan 9 (mprotect(PROT_NONE) is ENOSYS), so
 * overflow is detected after the fact: a smashed canary below a live stack's
 * base, or a faulting sp that leapt just below one (a big frame's `sub rsp,N`
 * can jump the redzone without ever writing it). Read-only, lock-free (same
 * used-then-range discipline as th_slot); this runs on a dying process, so a
 * rare race with a concurrent reap at worst mislabels the death message. */
int cc9_thread_stack_overflowed(unsigned long sp){
	int hi = th_hi, near = 0, instk = 0;
	for(int i=0;i<hi;i++){
		if(!th_tab[i].used) continue;
		__asm__ volatile("":::"memory");
		unsigned long lo = th_tab[i].stklo, shi = th_tab[i].stkhi;
		if(!lo) continue;
		unsigned long *b = (unsigned long *)lo;
		if(b[-1]!=STK_CANARY || b[-2]!=STK_CANARY || b[-3]!=STK_CANARY) return 1;   /* hard evidence */
		if(sp >= lo && sp < shi) instk = 1;                 /* a NORMAL in-stack sp */
		else if(sp && sp < lo && lo - sp < 65536) near = 1; /* leapt past the redzone? */
	}
	instk |= on_main_stack(sp);   /* main faulting on its own BSS stack is never a heap-thread overflow */
	/* The near-a-base verdict only counts if sp is inside NO live stack —
	 * adjacent heap stacks sit close together, and a thread faulting for an
	 * unrelated reason just below a neighbor's base is not that neighbor's
	 * overflow. */
	return near && !instk;
}

/* Darwin spelling: the stack BASE, i.e. the highest address (stacks grow down).
 * ponytail: self only — the argument is ignored. Every caller we have passes
 * pthread_self(), and answering for another thread would need a table lookup by
 * pid that can race with that thread exiting. Widen it when something needs it.
 */
void *pthread_get_stackaddr_np(pthread_t th){
	void *lo, *hi; (void)th;
	cc9_stack_bounds(&lo, &hi);
	return hi;
}

static unsigned long cur_pid(void);   /* defined below; used by cc9_kill_threads */

static n9_thread *find_thread(unsigned long pid){
	n9_thread *t=0;
	n9_semacquire(&th_lock,1);
	for(int i=0;i<MAXTH;i++) if(th_tab[i].used && th_tab[i].pid==pid){ t=th_tab[i].t; break; }
	n9_semrelease(&th_lock,1);
	return t;
}

/* Kill every live worker thread at process exit. cc9 threads are rfork(RFPROC)
 * siblings; when main exits they'd otherwise be ORPHANED (Plan 9 doesn't reparent),
 * left blocked in Semacqui forever — leaking procs and, because they hold shared
 * resources, wedging the parent shell. Called from crt0 just before n9_exits: post
 * a "kill" note to each thread's /proc/PID/note. Lock-free scan (same TSO-safe
 * discipline as cur_pid) so a thread that died holding th_lock can't deadlock exit. */
extern long n9_pwrite(int, const void *, long, long long);
void cc9_kill_threads(void){
	unsigned long self = cur_pid();
	for(int i=0;i<MAXTH;i++){
		if(!th_tab[i].used || th_tab[i].dead || th_tab[i].pid==0 || th_tab[i].pid==self) continue;
		n9_thread *kt = th_tab[i].t;
		unsigned long pid = kt ? kt->realpid : 0;   /* KERNEL pid, not the tid */
		if(pid == 0) continue;
		char path[40]; int k=0;
		for(const char *p="/proc/"; *p; p++) path[k++]=*p;
		char num[20]; int n=0; unsigned long v=pid;
		do { num[n++]='0'+(v%10); v/=10; } while(v);
		while(n>0) path[k++]=num[--n];
		for(const char *p="/note"; *p; p++) path[k++]=*p;
		path[k]=0;
		long fd = n9_open(path, 1 /*OWRITE*/);
		long wr = -2;
		if(fd>=0){ wr = n9_pwrite((int)fd,"kill",4,-1); n9_close((int)fd); }
		{ extern char *getenv(const char *);
		  if(getenv("CC9_EXIT_TRACE")){
			char b[64]; int k=0;
			for(const char *q="kil "; *q; q++) b[k++]=*q;
			for(int j=0;j<k;j++) ; /* noop */
			unsigned long v2=pid; char num2[20]; int n2=0;
			do { num2[n2++]='0'+(v2%10); v2/=10; } while(v2);
			while(n2>0) b[k++]=num2[--n2];
			b[k++]=' '; b[k++] = fd>=0 ? 'o' : 'X';
			b[k++] = wr==4 ? 'w' : 'x';
			b[k++]='\n';
			n9_pwrite(2,b,k,-1);
		  }
		}
	}
}

/* S1 abort. main's KERNEL pid, captured in pthread_create the first time main
 * (not a worker) spawns a thread — cc9_kill_threads can't reach main (no slot),
 * so the overflow abort needs it to take main down too. 0 = never captured. */
extern unsigned cc9_tos_pid(void);
static unsigned long main_realpid = 0;
static void note_kill(unsigned long pid){   /* single-proc /proc/PID/note — NOT notepg (that hits the listen1 parent) */
	if(!pid) return;
	char path[40]; int k=0;
	for(const char *p="/proc/"; *p; p++) path[k++]=*p;
	char num[20]; int n=0; unsigned long v=pid;
	do { num[n++]='0'+(v%10); v/=10; } while(v);
	while(n>0) path[k++]=num[--n];
	for(const char *p="/note"; *p; p++) path[k++]=*p;
	path[k]=0;
	long fd = n9_open(path, 1 /*OWRITE*/);
	if(fd>=0){ n9_pwrite((int)fd,"kill",4,-1); n9_close((int)fd); }
}
/* A smashed canary means this thread overflowed and corrupted the SHARED heap
 * (Plan 9's heap is contiguous+mapped, so the write never faulted). The whole
 * process is doomed — abort it LOUDLY, not just this proc. A bare n9_exits()
 * kills only the caller, which would (a) hang a JOINED main forever in
 * pthread_join and (b) leave the rest running on corrupted memory. So: note
 * every sibling worker (cc9_kill_threads) AND main (single-proc note, so the
 * shared-group listen1 parent is untouched), THEN wake the joiner as a
 * belt-and-suspenders against a hang if main's pid was never captured (main
 * spawned no thread directly), THEN exit self. A deliberate fault instead of
 * notes was rejected: a Plan 9 trap kills only the faulting proc, not the
 * RFMEM group, so it would hang the joiner exactly like n9_exits. */
static void cc9_stack_overflow_abort(n9_thread *t){
	n9_pwrite(2, "cc9: thread stack overflow (canary smashed)\n", 44, -1);
	cc9_kill_threads();                        /* notes every sibling worker (skips self + main) */
	note_kill(main_realpid);                   /* and main — the real abort of the joiner */
	t->done = 1; n9_semrelease(&t->joinsem, 1);/* fallback: an uncaptured main pid still can't hang */
	n9_exits("cc9: thread stack overflow");
}

/* Claim a thread's table slot exactly once (returns 1 to the single caller that
 * wins). The winner owns reclaiming t/stack — prevents the double-free when both
 * the detached-trampoline path and a detach-after-done both try to reap. */
static int reap_slot(unsigned long pid){
	int claimed=0;
	n9_semacquire(&th_lock,1);
	for(int i=0;i<MAXTH;i++) if(th_tab[i].used && th_tab[i].pid==pid){ th_tab[i].used=0; claimed=1; break; }
	n9_semrelease(&th_lock,1);
	return claimed;
}

/* DEFERRED reclamation for detached threads. A detached thread runs ON its own
 * t->stack, so it must not free that stack itself (it would free the memory it's
 * returning through into the EXITS syscall). Instead it parks (stack,t) here and
 * a LATER pthread_create/join frees them — by then the dead thread has long since
 * EXITS'd and nothing executes on that stack. */
static struct { void *stack; n9_thread *t; } dead_tab[MAXTH];
static int dead_n = 0, dead_lock = 1;
static void dead_park(void *stack, n9_thread *t){
	n9_semacquire(&dead_lock,1);
	if(dead_n < MAXTH){ dead_tab[dead_n].stack=stack; dead_tab[dead_n].t=t; dead_n++; }
	n9_semrelease(&dead_lock,1);   /* table full -> leak rather than free-in-thread */
}
/* KNOWN NARROW RACE (documented, not fixed — the clean fix is outside these two
 * files). A detached thread calls dead_park(t->stack,t) and then RETURNS into the
 * n9_rfork_thread asm epilogue (n9syscall.s), which does `subq $16,%rsp; movq
 * $0,8(%rsp); EXITS` — ONE 8-byte store at top-8 of t->stack, AFTER the park. If a
 * concurrent pthread_create runs dead_reap() and free()s that same t->stack inside
 * the ~3-instruction gap between the park and that store, the store lands in freed
 * (possibly already re-handed-out) memory. The window is those 3 instructions AND
 * requires malloc to have recycled the exact block first — astronomically narrow,
 * but real. It cannot be closed in C: Plan 9's EXITS must write its status arg to
 * the stack (syscall ABI) and that stack IS the thread's own. The real fix lives in
 * n9syscall.s — switch %rsp to a tiny per-proc scratch before EXITS. A /proc/realpid
 * liveness gate here was rejected: Plan 9 REUSES pids, so a recycled realpid reads as
 * "still alive" forever and would leak the entry (the pid-reuse hazard cc9_kill_threads
 * already guards against). Left as-is deliberately rather than half-fixed. */
static void dead_reap(void){
	n9_semacquire(&dead_lock,1);
	for(int i=0;i<dead_n;i++){ free(dead_tab[i].stack); free(dead_tab[i].t); }
	dead_n=0;
	n9_semrelease(&dead_lock,1);
}

/* current "pid" = a stable per-thread id derived from which stack %rsp is in. */
static unsigned long cur_pid(void){
	int s = th_slot();
	return s < 0 ? 1 : th_tab[s].pid;   /* no slot == main thread */
}

/* Per-thread __cxa_thread_atexit: thread_local destructors + promise::
 * set_value_at_thread_exit must run when THIS thread exits, not at process exit.
 * Keyed by cur_pid(); the trampoline runs the current thread's list (LIFO) after
 * start() returns. Main thread (pid 1) routes to the process atexit, as before. */
extern int __cxa_atexit(void (*)(void *), void *, void *);
struct n9_tae { unsigned long pid; void (*fn)(void *); void *arg; int used; };
/* GROWABLE (like crt0.c's atexit): a fixed cap silently DROPPED every
 * registration past it and returned 0=success, so the thread_local dtor never
 * ran. Slots freed at a thread's exit (used=0) are reused; only a genuinely full
 * table grows via realloc, and a real OOM is reported (ENOMEM), never swallowed.
 * Shared across threads -> every access is under tae_lock. */
static struct n9_tae *tae_tab;
static int tae_n = 0, tae_cap = 0;
static int tae_lock = 1;
int __cxa_thread_atexit(void (*fn)(void *), void *arg, void *dso){
	unsigned long pid = cur_pid();
	if(pid == 1) return __cxa_atexit(fn, arg, dso);   /* main: run at process exit */
	n9_semacquire(&tae_lock,1);
	int i;
	for(i=0;i<tae_n;i++) if(!tae_tab[i].used) break;   /* reuse a slot freed at some thread's exit */
	if(i == tae_n){                                    /* no free slot: append, growing if needed */
		if(tae_n == tae_cap){
			int ncap = tae_cap ? tae_cap*2 : 64;
			void *p = realloc(tae_tab, (unsigned long)ncap * sizeof *tae_tab);
			if(!p){ n9_semrelease(&tae_lock,1); return 12; }   /* ENOMEM: honest, not a silent drop */
			tae_tab = p; tae_cap = ncap;
		}
		tae_n++;
	}
	tae_tab[i].pid=pid; tae_tab[i].fn=fn; tae_tab[i].arg=arg; tae_tab[i].used=1;
	n9_semrelease(&tae_lock,1);
	return 0;
}
int __cxa_thread_atexit_impl(void (*fn)(void *), void *arg, void *dso){ return __cxa_thread_atexit(fn, arg, dso); }
static void run_thread_keys(void);   /* defined with the TSD arrays below */
static void run_thread_atexit(unsigned long pid){
	for(;;){   /* LIFO: highest-index entry for this pid first */
		void (*fn)(void *)=0; void *arg=0; int idx=-1;
		n9_semacquire(&tae_lock,1);
		for(int i=tae_n-1;i>=0;i--) if(tae_tab[i].used && tae_tab[i].pid==pid){ fn=tae_tab[i].fn; arg=tae_tab[i].arg; tae_tab[i].used=0; idx=i; break; }
		n9_semrelease(&tae_lock,1);
		if(idx<0) break;
		fn(arg);
	}
}

/* Shared thread-exit teardown, factored out of the trampoline so pthread_exit and
 * a normal return from the thread function run the IDENTICAL sequence and can't
 * drift. Runs ON the exiting thread's own stack and MUST be the last thing that
 * touches thread state before the proc terminates: once the joinsem is released
 * (or the detached park is done) the joiner is free to free(t->stack)/free(t), so
 * the caller must go straight to EXITS and touch nothing more. t->ret must already
 * be set by the caller. Contract: thread_local + set_value_at_thread_exit dtors
 * and POSIX TSD dtors run at thread exit, exactly once [basic.start.term]/[thread]. */
static void thread_teardown(n9_thread *t){
	run_thread_atexit(t->pid);   /* thread_local dtors + set_value_at_thread_exit */
	run_thread_keys();           /* POSIX TSD destructors (frees __thread_struct etc.) */
	/* S1: on Plan 9's contiguous heap an overflow usually corrupts SILENTLY (the
	 * memory below the stack is mapped) — check the canary now and abort the whole
	 * process loudly instead of letting the damage surface later. Never returns. */
	{ unsigned long *rz = (unsigned long *)t->stkbase;
	  if(rz[-1]!=STK_CANARY || rz[-2]!=STK_CANARY || rz[-3]!=STK_CANARY)
		cc9_stack_overflow_abort(t); }
	t->done = 1;
	{ unsigned long mypid = t->pid;
	  n9_semacquire(&th_lock,1);
	  for(int i=0;i<MAXTH;i++) if(th_tab[i].used && th_tab[i].pid==mypid){ th_tab[i].dead=1; break; }
	  n9_semrelease(&th_lock,1); }
	if(t->detached){
		/* nobody will join. Can't free our own running stack here — park it for a
		 * later create/join to reclaim. reap_slot makes the park single-owner. */
		if(reap_slot(t->pid)) dead_park(t->stack, t);
		return;
	}
	n9_semrelease(&t->joinsem, 1);    /* wake any joiner; touch NO stack state after this */
}

static void trampoline(void *p){
	n9_thread *t = (n9_thread *)p;
	cc9_fpmask();   /* this rfork child has fresh FP state — mask FP exceptions */
	{ extern void cc9_prof_arm(void); cc9_prof_arm(); }   /* CC9_PROF: alarms are per-proc, and a cc9 thread IS a proc */
	/* Register (tid, t, stack-range) BEFORE releasing the handoff the creator
	 * waits on, so by the time pthread_create returns the thread is findable AND
	 * cur_pid() resolves this thread — both before start() runs. The scalar range
	 * + used-set-last ordering is what makes the lock-free cur_pid scan safe. */
	unsigned long lo = t->stkbase, hi = lo + t->stksize;
	n9_semacquire(&th_lock,1);
	for(int i=0;i<MAXTH;i++) if(!th_tab[i].used){
		th_tab[i].pid=t->pid; th_tab[i].t=t; th_tab[i].stklo=lo; th_tab[i].stkhi=hi; th_tab[i].errno_v=0; th_tab[i].dead=0;
		((unsigned long *)lo)[-4] = (unsigned long)i;   /* TL1 stash: %rsp&~(al-1) finds this (read by self only) */
		/* Slots are recycled after join: drop the previous owner's emutls and TSD
		 * arrays or this thread would inherit ITS thread_locals / key values. (The
		 * emutls objects it pointed at leak, as they always have — no destructor
		 * rounds here; TSD values already went through run_thread_keys at exit.) */
		if(th_tab[i].etls) free(th_tab[i].etls);
		th_tab[i].etls=0; th_tab[i].etls_n=0;
		/* hand the dead thread's cached small blocks back to the heap, else they
		 * sit stranded in a bin nobody owns */
		if(th_tab[i].mcache){ extern void cc9_mcache_release(void *); cc9_mcache_release(th_tab[i].mcache); }
		th_tab[i].mcache=0;
		if(th_tab[i].tsd) free(th_tab[i].tsd);
		th_tab[i].tsd=0;
		if(i >= th_hi) th_hi = i+1;        /* publish before used: bounds the %rsp scans */
		__asm__ volatile("":::"memory");   /* release: publish range before used (pairs w/ cur_pid) */
		th_tab[i].used=1; break;
	}
	n9_semrelease(&th_lock,1);
	n9_semrelease(&handoff_sem, 1);   /* signal the rfork globals are consumed + registered */
	/* via a C++ shim that turns an escaped exception into std::terminate (cc9's
	 * unwinder can't unwind off the top of this C trampoline) — [thread.constr]. */
	extern void *cc9_thread_invoke(void *(*)(void *), void *);
	t->ret = cc9_thread_invoke(t->start, t->arg);
	thread_teardown(t);   /* dtors + TSD + canary + hand off to joiner / park (shared w/ pthread_exit) */
	/* return -> the n9_rfork_thread asm wrapper runs EXITS for this proc. */
}

/* RLIMIT_NPROC emulation: 9front has no per-process thread cap, but the suite's
 * thread_create_failure test sets the limit to 1 and expects creation to throw.
 * setrlimit (posix_llvm.c) routes the cap here; pthread_create honors it. */
static long cc9_nproc_limit = 0x7fffffff;
void cc9_set_nproc_limit(long n){ cc9_nproc_limit = n > 0 ? n : 1; }
long cc9_get_nproc_limit(void){ return cc9_nproc_limit; }

int pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*start)(void *), void *arg){
	dead_reap();   /* reclaim any parked detached-thread stacks (they're long dead now) */
	/* Record main's kernel pid for the S1 overflow abort (cc9_kill_threads can't
	 * reach main). th_slot()<0 == the caller runs on main's stack, so cc9_tos_pid()
	 * (no syscall) is main's pid; a nested create from a worker leaves it untouched. */
	if(!main_realpid && th_slot() < 0) main_realpid = cc9_tos_pid();
	if(cc9_nproc_limit < 0x7fffffff){
		int live = 1;   /* the main thread counts toward the limit */
		n9_semacquire(&th_lock,1);
		for(int i=0;i<MAXTH;i++) if(th_tab[i].used) live++;
		n9_semrelease(&th_lock,1);
		if(live >= cc9_nproc_limit) return 11;   /* EAGAIN: would exceed RLIMIT_NPROC */
	}
	n9_thread *t = malloc(sizeof *t);
	if(!t) return 11;
	/* Unique thread id, assigned by the parent (the rfork-return pid isn't
	 * visible to the child, and /dev/pid isn't readable here). Starts at 2 (main
	 * is 1). The child registers itself in the table keyed by this id. */
	static unsigned long tid_ctr = 1;
	static int tid_lock = 1;
	n9_semacquire(&tid_lock,1); unsigned long tid = ++tid_ctr; n9_semrelease(&tid_lock,1);
	t->start=start; t->arg=arg; t->ret=0; t->joinsem=0; t->done=0; t->detached=0; t->pid=tid; t->realpid=0;
	/* attr stacksize wins (POSIX); cc9_set_thread_stack is the legacy global knob
	 * for callers that can't pass an attr; STACKSIZE is the floor. */
	size_t ss = attr && attr->stacksize > STACKSIZE ? (size_t)attr->stacksize
	          : cc9_thread_stack > STACKSIZE ? (size_t)cc9_thread_stack : STACKSIZE;
	/* TL1: pow2-align the stack so th_slot recovers its base by masking %rsp.
	 * Over-allocate by hand rather than aligned_alloc: the redzone must sit
	 * BELOW the aligned base (aligned_alloc's own metadata lives there), and
	 * this way the thread still gets every byte of ss. The ~ss of padding is
	 * address space, not memory — those pages are never touched, so demand
	 * paging never commits them (only a recycled free block can hand back
	 * already-dirtied ones). */
	unsigned long al = STK_MINALIGN;
	while(al < ss && al < (1UL<<30)) al <<= 1;   /* cap: >1G stacks just use the scan */
	if(ss > ~0UL - al - STK_RED){ free(t); return 11; }
	char *stk = malloc(ss + al + STK_RED); if(!stk){ free(t); return 11; }
	unsigned long base = ((unsigned long)stk + STK_RED + al - 1) & ~(al - 1);
	unsigned long *rz = (unsigned long *)base;
	rz[-1] = rz[-2] = rz[-3] = STK_CANARY;   /* S1: an overflow smashes these first */
	rz[-4] = ~0UL;                           /* th_tab index: trampoline fills it in */
	rz[-5] = STK_STASH ^ base;               /* sentinel, base-bound */
	t->stack=stk; t->stkbase=base; t->stksize=ss;
	void *top = (void *)((base + ss) & ~15UL);
	n9_semacquire(&create_lock, 1);
	/* Monotonic probe bounds for th_slot, serialized by create_lock so a racing
	 * pair of creates can't lose an update. Published before the child exists,
	 * so its own th_slot always sees a floor<=base and a maxal>=al. */
	if(base < stk_floor) stk_floor = base;
	if(al > stk_maxal) stk_maxal = al;
	long pid = n9_rfork_thread(top, trampoline, t);
	if(pid < 0){   /* rfork failed: no child will ever release handoff_sem */
		n9_semrelease(&create_lock, 1); free(stk); free(t); return 11;
	}
	t->realpid = (unsigned long)pid;   /* the kernel pid — what /proc wants */
	n9_semacquire(&handoff_sem, 1);   /* wait until the child registered + copied the handoff */
	n9_semrelease(&create_lock, 1);
	*th = (pthread_t)tid;
	/* Honor PTHREAD_CREATE_DETACHED. Must come after the handoff above: the
	 * thread has to be registered before pthread_detach can find it. */
	if(attr && attr->detachstate == PTHREAD_CREATE_DETACHED)
		pthread_detach((pthread_t)tid);
	return 0;
}

int pthread_join(pthread_t pid, void **ret){
	n9_thread *t = find_thread(pid);
	if(!t) return 3;   /* ESRCH */
	n9_semacquire(&t->joinsem, 1);
	if(ret) *ret = t->ret;
	if(reap_slot(pid)){ free(t->stack); free(t); }
	return 0;
}

/* pthread_kill: on Plan 9 the only thing we can actually deliver to another
 * thread is a "kill" note (threads are rfork(RFPROC) procs). So:
 *   sig == 0        -> POSIX existence check, the common real use
 *   sig == SIGKILL  -> post "kill" to /proc/realpid/note
 *   anything else   -> EINVAL, rather than pretend we delivered a signal
 * The t->done guard is not optional: Plan 9 reuses pids, so a note aimed at a
 * finished thread can land on an unrelated proc (see cc9_kill_threads). */
int pthread_kill(pthread_t pid, int sig){
	n9_thread *t = find_thread(pid);
	if(!t) return 3;               /* ESRCH */
	if(sig == 0) return 0;
	if(sig != 9) return 22;        /* EINVAL: SIGKILL is all Plan 9 gives us */
	if(t->done || t->realpid == 0) return 3;   /* never note a possibly-reused pid */
	{
		char path[40]; int k=0;
		for(const char *p="/proc/"; *p; p++) path[k++]=*p;
		char num[20]; int n=0; unsigned long v=t->realpid;
		do { num[n++]='0'+(v%10); v/=10; } while(v);
		while(n>0) path[k++]=num[--n];
		for(const char *p="/note"; *p; p++) path[k++]=*p;
		path[k]=0;
		long fd = n9_open(path, 1 /*OWRITE*/);
		if(fd<0) return 3;
		n9_pwrite((int)fd, "kill", 4, -1);
		n9_close((int)fd);
	}
	return 0;
}

int pthread_detach(pthread_t pid){
	n9_thread *t=find_thread(pid); if(!t) return 3;
	t->detached=1;
	/* If it already finished, trampoline saw detached==0 and didn't reap, so park
	 * it here for deferred reclamation (reap_slot guarantees a single owner; never
	 * free now — the thread may still be on its stack mid-exit). */
	if(t->done && reap_slot(pid)) dead_park(t->stack, t);
	return 0;
}
pthread_t pthread_self(void){ return (pthread_t)cur_pid(); }
int pthread_equal(pthread_t a, pthread_t b){ return a==b; }
/* pthread_exit: end the CALLING thread, running the SAME teardown as a normal
 * return from the thread function (thread_teardown) so the two paths can't drift.
 * The old version ran NO destructors (skipped thread_local + TSD dtors) and then
 * spun in n9_sleep FOREVER on t->stack — leaking a live proc AND a use-after-free
 * (the joiner's free(t->stack) reclaimed the very stack these n9_sleep frames kept
 * running on). A worker now terminates its proc with n9_exits right after handing
 * off to the joiner — so the proc is (all but) dead before the joiner frees the
 * stack, the same narrow window the trampoline's asm EXITS already has, no worse.
 * pthread_exit from main keeps the process alive until every other thread finishes,
 * then exit(0) [support.runtime]/pthread_exit. */
void pthread_exit(void *ret){
	unsigned long pid = cur_pid();
	n9_thread *t = find_thread(pid);
	if(t){
		t->ret = ret;
		thread_teardown(t);   /* releases joinsem last; no stack use after it returns */
		n9_exits(0);          /* terminate this proc (nil status = normal exit) */
	}
	/* main has no th_tab slot and no malloc'd stack to free: block until no worker
	 * is still running, then end the process via exit(0) — NOT n9_exits — so crt0's
	 * atexit/__cxa_atexit chain + fini_array run. pthread_exit from main must behave
	 * like return-from-main/exit(0) [support.runtime], and __cxa_thread_atexit routes
	 * main's (pid==1) thread_local dtors to the PROCESS atexit, so skipping it would
	 * drop them. (Workers still use n9_exits: their finalizers already ran in teardown.) */
	extern void exit(int);
	for(;;){
		int live=0;
		n9_semacquire(&th_lock,1);
		for(int i=0;i<MAXTH;i++) if(th_tab[i].used && !th_tab[i].dead){ live=1; break; }
		n9_semrelease(&th_lock,1);
		if(!live) break;
		n9_sleep(50);
	}
	exit(0);
}

/* ---- mutex (binary semaphore + optional recursion) ---- */
/* NOTE: there is deliberately NO lazy "ensure sem!=0" init here. A prior version
 * resurrected sem=1 whenever it saw {sem,owner,count,kind} all-zero — but that state
 * also occurs TRANSIENTLY during a normal lock, in the window between n9_semacquire
 * (sem 1->0) and the m->owner=self store below. A second thread hitting that window
 * would re-set sem=1 and its own semacquire would succeed too, admitting two holders
 * -> lost updates under contention. Rust and cc9's PTHREAD_MUTEX_INITIALIZER/
 * pthread_mutex_init both set sem=1, so a valid unlocked mutex is never all-zero.
 * Raw zero-initialized (non-POSIX) mutexes are unsupported and will deadlock. */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a){ m->sem=0; m->held=0; m->owner=0; m->count=0; m->kind=a?a->kind:0; return 0; }

/* Futex-shaped lock/unlock. Uncontended = ONE atomic CAS, no syscall; m->sem is
 * only the sleep channel. Plan 9's semacquire/semrelease are syscalls (~1.4us)
 * even when the semaphore is free, so the old unconditional semacquire cost
 * ~3.2us per Rust std::sync::Mutex lock+unlock (measured on cirno) where Linux's
 * futex path is ~20ns — and Servo's layout is full of Arc<Mutex<..>>/rayon locks.
 * Same shape as n9libc's mlock(). Spin bound is deliberately < one syscall's cost.
 * The sem is a COUNTING semaphore, so a release racing a waiter's park can't be
 * lost: the token is still there when it acquires, and stale tokens just re-loop. */
static void mtx_acquire(pthread_mutex_t *m){
	if(__sync_bool_compare_and_swap(&m->held, 0, 1)) return;   /* uncontended: no syscall */
	/* Contended. Mark 2 ("held, someone may be waiting") and sleep until we win the
	 * xchg. Re-marking 2 on every retry is what keeps release honest: the unlocker
	 * only pays a syscall when it sees 2. */
	while(__sync_lock_test_and_set(&m->held, 2) != 0)
		n9_semacquire(&m->sem, 1);        /* stale tokens just re-loop */
}
static void mtx_release(pthread_mutex_t *m){
	if(__sync_lock_test_and_set(&m->held, 0) == 2)
		n9_semrelease(&m->sem, 1);        /* only pay the syscall if someone waited */
}
int pthread_mutex_destroy(pthread_mutex_t *m){ (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m){
	/* Only RECURSIVE/ERRORCHECK ever READ ->owner (here and in unlock); a NORMAL
	 * mutex — what Rust's std::sync::Mutex and most C++ locks are — used to pay a
	 * cur_pid() %rsp scan on every lock just to store a value nobody reads. */
	if(m->kind == PTHREAD_MUTEX_NORMAL){
		mtx_acquire(m);
		m->count=1; return 0;
	}
	unsigned long self=cur_pid();
	if(m->kind==PTHREAD_MUTEX_RECURSIVE && m->owner==self){ m->count++; return 0; }
	mtx_acquire(m);
	m->owner=self; m->count=1; return 0;
}
int pthread_mutex_trylock(pthread_mutex_t *m){
	if(m->kind == PTHREAD_MUTEX_NORMAL){
		if(!__sync_bool_compare_and_swap(&m->held, 0, 1)) return 16 /*EBUSY*/;
		m->count=1; return 0;
	}
	unsigned long self=cur_pid();
	if(m->kind==PTHREAD_MUTEX_RECURSIVE && m->owner==self){ m->count++; return 0; }
	if(!__sync_bool_compare_and_swap(&m->held, 0, 1)) return 16 /*EBUSY*/;
	m->owner=self; m->count=1; return 0;
}
int pthread_mutex_unlock(pthread_mutex_t *m){
	if((m->kind==PTHREAD_MUTEX_RECURSIVE || m->kind==PTHREAD_MUTEX_ERRORCHECK)
	   && m->owner != cur_pid()) return 1 /*EPERM*/;   /* only the owner may unlock */
	if(m->kind==PTHREAD_MUTEX_RECURSIVE && m->count>1){ m->count--; return 0; }
	m->owner=0; m->count=0; mtx_release(m); return 0;
}
int pthread_mutexattr_init(pthread_mutexattr_t *a){ a->kind=0; return 0; }
/* process-shared accepted for real: semaphore-word mutexes work cross-process
 * when placed in shared memory (shm9 segments) */
int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int v){ (void)a; (void)v; return 0; }
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *a, int *v){ (void)a; if(v) *v = 1; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *a){ (void)a; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int k){ a->kind=k; return 0; }

/* ---- condition variable: FIFO queue of per-waiter semaphores ----
 * The old design used one counting semaphore + a waiter count, which let a
 * NEWLY-arriving waiter steal the token a signal posted for an already-blocked
 * waiter (e.g. two threads waiting on one cv with opposite predicates — exactly
 * std::thread pool handshakes / Stockfish). Here each waiter enqueues a node
 * holding its OWN semaphore; signal dequeues the head and posts only that node's
 * sem, so a fresh waiter (appended at the tail) can't intercept it. c->lk is the
 * internal mutex-semaphore guarding the queue. */
extern long n9_tsemacquire(int *, long);
extern int clock_gettime(int, struct timespec *);
typedef struct cc9_cwaiter { int sem; int dq; struct cc9_cwaiter *next; } cc9_cwaiter;
/* The condvar's internal lock, futex-shaped — same reason as pthread_mutex: a
 * bare n9_semacquire/semrelease pair is TWO Plan 9 syscalls (~2.8us) even when
 * nobody contends, and pthread_cond_signal runs on EVERY Rust channel send
 * (notify_one), waiter or not. Profiling a dsl.sk load showed ~85% of on-CPU time
 * in semrelease/semacquire with free_u down at 5% — this is where it goes.
 *
 * The futex word is `pad`, NOT `lk`, deliberately: pad is 0 both in the old static
 * PTHREAD_COND_INITIALIZER {1,0,0,0} and in a zeroed/memset struct, so 0=free is
 * correct for BOTH init conventions (libc++'s condition_variable uses the static
 * initializer; other code calloc's). `lk` stays the sleep channel — its leftover
 * 1 is at most one spurious token, which the retry loop just re-checks. sizeof and
 * all field offsets are unchanged, so objects compiled against the old header
 * (cargo caches mozjs/angle and does not track these headers) still work. */
static void cond_lock(pthread_cond_t *c){
	if(__sync_bool_compare_and_swap(&c->pad, 0, 1)) return;   /* uncontended: no syscall */
	while(__sync_lock_test_and_set(&c->pad, 2) != 0)
		n9_semacquire(&c->lk, 1);
}
static void cond_unlock(pthread_cond_t *c){
	if(__sync_lock_test_and_set(&c->pad, 0) == 2)
		n9_semrelease(&c->lk, 1);      /* only if someone actually waited */
}

static void cond_enqueue(pthread_cond_t *c, cc9_cwaiter *w){
	w->sem=0; w->dq=0; w->next=0;
	cond_lock(c);
	if(c->tail) ((cc9_cwaiter*)c->tail)->next=w; else c->head=w;
	c->tail=w;
	cond_unlock(c);
}
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a){ (void)a; c->lk=0; c->pad=0; c->head=0; c->tail=0; return 0; }
int pthread_cond_destroy(pthread_cond_t *c){ (void)c; return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m){
	cc9_cwaiter w;
	cond_enqueue(c, &w);
	pthread_mutex_unlock(m);
	n9_semacquire(&w.sem, 1);             /* wait on my own semaphore */
	pthread_mutex_lock(m);
	return 0;
}
/* Real timed wait: deadline is an absolute CLOCK_REALTIME time (libc++/pthread
 * pass it that way). On timeout, de-queue self under c->lk; if a signal raced in
 * (dq set + sem posted), consume it and report success — race-free. */
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *ts){
	cc9_cwaiter w;
	cond_enqueue(c, &w);
	pthread_mutex_unlock(m);
	struct timespec now; clock_gettime(0 /*CLOCK_REALTIME*/, &now);
	long ms = (long)(ts->tv_sec - now.tv_sec)*1000 + (ts->tv_nsec - now.tv_nsec)/1000000;
	if(ms < 0) ms = 0;
	long r = n9_tsemacquire(&w.sem, ms);
	int rc = 0;
	if(r != 1){
		cond_lock(c);
		if(w.dq){                                  /* signal dequeued+posted us as we timed out */
			n9_tsemacquire(&w.sem, 0);             /* consume the token (non-blocking) */
			rc = 0;
		} else {                                   /* still queued: unlink self */
			cc9_cwaiter *p=(cc9_cwaiter*)c->head, *prev=0;
			while(p && p!=&w){ prev=p; p=p->next; }
			if(p==&w){ if(prev) prev->next=w.next; else c->head=w.next; if(c->tail==(void*)&w) c->tail=(void*)prev; }
			rc = 110 /*ETIMEDOUT*/;
		}
		cond_unlock(c);
	}
	pthread_mutex_lock(m);
	return rc;
}
int pthread_cond_signal(pthread_cond_t *c){
	cond_lock(c);
	cc9_cwaiter *w=(cc9_cwaiter*)c->head;
	if(w){ c->head=w->next; if(!c->head) c->tail=0; w->dq=1; n9_semrelease(&w->sem,1); }
	cond_unlock(c);
	return 0;
}
int pthread_cond_broadcast(pthread_cond_t *c){
	cond_lock(c);
	cc9_cwaiter *w=(cc9_cwaiter*)c->head;
	c->head=0; c->tail=0;
	while(w){ cc9_cwaiter *n=w->next; w->dq=1; n9_semrelease(&w->sem,1); w=n; }
	cond_unlock(c);
	return 0;
}

/* ---- once ---- */
/* 3-state once: 0=unstarted, 1=in-progress, 2=done. fn() runs WITHOUT the lock
 * held (so nested pthread_once on a different flag can't deadlock); concurrent
 * callers spin-yield until the initializer that set 1 finishes and sets 2. */
int pthread_once(pthread_once_t *o, void (*fn)(void)){
	static int once_lock = 1;
	for(;;){
		n9_semacquire(&once_lock,1);
		if(*o==2){ n9_semrelease(&once_lock,1); return 0; }
		if(*o==0){ *o=1; n9_semrelease(&once_lock,1); fn();
			n9_semacquire(&once_lock,1); *o=2; n9_semrelease(&once_lock,1); return 0; }
		n9_semrelease(&once_lock,1);   /* *o==1: another caller is running fn() */
		n9_sleep(0);
	}
}

/* ---- POSIX thread-specific data (pthread_{get,set}specific) ----
 * Upstream is `pthread_self()->tsd[key]` — one indexed load off the TCB, no lock.
 * cc9 used to key a 4096-entry GLOBAL table by (pid,key): a cur_pid() %rsp scan of
 * all MAXTH=1024 slots, then a scan of the table — plus a kernel semaphore on every
 * set (a semacquire/semrelease pair is ~2749 ns here) — and EAGAIN once 4096
 * (thread,key) pairs existed. Now it's what upstream does: th_slot() resolves the
 * calling thread and the array hangs off its th_tab slot, so both calls are an
 * indexed load. The array is private to the calling thread => no lock, ever.
 *
 * Flat void*[MAXKEYS] rather than a grown-on-demand array (what etls needs, since
 * emutls indices are unbounded): keys already can't exceed PTHREAD_KEYS_MAX=256, so
 * the whole array is 2K — smaller and simpler than growth code, and it makes the
 * bounds check a compile-time constant. Allocated lazily on first setspecific:
 * threads that never touch a key pay nothing. */
#define MAXKEYS 256
static int tls_lock = 1;                    /* guards next_key/key_dtor only */
static int next_key = 1;                    /* never reused: key 0 stays unhanded-out */
static void (*key_dtor[MAXKEYS])(void *);   /* TSD destructor per key (POSIX) */
static void **main_tsd;                     /* main has no th_tab slot (cf. main_etls) */

int pthread_key_create(pthread_key_t *k, void (*dtor)(void *)){
	n9_semacquire(&tls_lock,1);
	int key = next_key;
	if(key >= MAXKEYS){ n9_semrelease(&tls_lock,1); return 11; }   /* EAGAIN: PTHREAD_KEYS_MAX */
	next_key++; key_dtor[key]=dtor;
	n9_semrelease(&tls_lock,1);
	*k = key; return 0;
}
/* POSIX: key_delete runs no destructors and frees no values — the application owns
 * that storage. So retiring the destructor is the whole job; stale values left in
 * other threads' arrays are unreachable because next_key never re-hands-out a key
 * (and reaching into another live thread's array would race its slot being reused). */
int pthread_key_delete(pthread_key_t k){
	if(k <= 0 || k >= MAXKEYS) return 22;   /* EINVAL */
	n9_semacquire(&tls_lock,1); key_dtor[k]=0; n9_semrelease(&tls_lock,1);
	return 0;
}

static void **tsd_array(int alloc){
	int s = th_slot();
	void ***ap = s < 0 ? &main_tsd : &th_tab[s].tsd;
	if(!*ap && alloc){
		void **a = malloc(MAXKEYS * sizeof(void *));
		if(!a) return 0;
		for(int i=0;i<MAXKEYS;i++) a[i]=0;
		*ap = a;
	}
	return *ap;
}

void *pthread_getspecific(pthread_key_t k){
	void **a = tsd_array(0);
	if(!a || k <= 0 || k >= MAXKEYS) return 0;   /* unset key (or no array yet) reads NULL */
	return a[k];
}
int pthread_setspecific(pthread_key_t k, const void *v){
	if(k <= 0 || k >= MAXKEYS) return 22;        /* EINVAL: key_create hands out no such key */
	void **a = tsd_array(1);
	if(!a) return 12;                            /* ENOMEM */
	a[k] = (void *)v;                            /* private to this thread: no lock */
	return 0;
}

/* Run this thread's TSD destructors at its exit: for each key with a non-null value
 * and a registered destructor, null the value and call the destructor; repeat (a
 * destructor may set new values) for a few rounds. Without this,
 * __thread_specific_ptr-held data (e.g. libc++'s per-thread __thread_struct) leaks
 * on every std::thread — tripping the suite's new==delete leak checks.
 * Runs ON the exiting thread (from the trampoline), so th_slot() resolves it; the
 * array itself is freed when the th_tab slot is next claimed. */
static void run_thread_keys(void){
	void **a = tsd_array(0);
	if(!a) return;
	for(int round=0; round<4; round++){
		int any=0;
		for(int i=1;i<MAXKEYS;i++){
			void *v = a[i]; void (*d)(void *) = key_dtor[i];
			if(v && d){ a[i]=0; d(v); any=1; }
		}
		if(!any) break;
	}
}

/* Emulated TLS (-femulated-tls): the compiler turns each `thread_local` into a
 * call to __emutls_get_address(control), where control = {size, align, index,
 * init}. We return per-thread storage keyed by (pid, control) — the ELF %fs TLS
 * model doesn't work under rfork(RFMEM) (no per-thread %fs base), but this does. */
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
/* Ported 1:1 from compiler-rt lib/builtins/emutls.c, which is what -femulated-tls
 * actually calls. Upstream:
 *     uintptr_t index = emutls_get_index(control);       // control->index, set ONCE
 *     emutls_address_array *array = emutls_get_address_array(index--);
 *     if (array->data[index] == NULL)
 *       array->data[index] = emutls_allocate_object(control);
 *     return array->data[index];
 * i.e. an atomic load + a per-thread array index. No lock, no search.
 *
 * The control layout (upstream __emutls_control) is:
 *     c[0]=size  c[1]=align  c[2]=index ("data[index-1] is the object address")  c[3]=value/init
 *
 * The previous cc9 version IGNORED c[2] — the whole mechanism — and instead did
 * cur_pid() (scan of MAXTH slots) + a kernel semaphore + a linear scan of an
 * 8192-entry global table, on EVERY thread_local access: measured 21779 ns/access
 * (a plain call is 22 ns). It also died with "emutls table full" once 8192
 * (thread,var) pairs existed.
 *
 * The ONE thing that cannot be 1:1: upstream gets the per-thread array with
 * pthread_getspecific (one %fs load on Linux). cc9 threads are rfork(RFMEM) procs
 * with no per-thread %fs base, so we resolve the thread by %rsp range (the same
 * trick as cur_pid) and hang the array off its th_tab slot. Everything else —
 * the index, the array, the lazily allocated object — matches upstream.
 * ponytail: objects still leak at thread exit (as before — no destructor rounds);
 * add upstream's emutls_key destructor if that ever matters. */
static int emutls_lock = 1;
static unsigned long emutls_num_object = 0;      /* guarded by emutls_lock */
static void **main_etls; static unsigned long main_etls_n;

static unsigned long emutls_get_index(void *control){
	unsigned long *c = control;                   /* c[2] == index */
	unsigned long index = __atomic_load_n(&c[2], __ATOMIC_ACQUIRE);
	if(!index){
		n9_semacquire(&emutls_lock,1);
		index = c[2];
		if(!index){
			index = ++emutls_num_object;
			__atomic_store_n(&c[2], index, __ATOMIC_RELEASE);
		}
		n9_semrelease(&emutls_lock,1);
	}
	return index;
}

/* This thread's malloc-cache slot (n9libc's tcache), or 0 for main — main has no
 * th_tab slot and n9libc keeps its own. Same %rsp resolve as everything else. */
void **cc9_thread_mcache(void){
	int sl = th_slot();
	return sl < 0 ? 0 : &th_tab[sl].mcache;
}

/* upstream's emutls_getspecific(), by %rsp instead of %fs (th_slot). The array is
 * private to the calling thread, so nothing below needs a lock. */
static void ***emutls_array_slot(unsigned long **nslot){
	int s = th_slot();
	if(s < 0){ *nslot = &main_etls_n; return &main_etls; }
	*nslot = &th_tab[s].etls_n; return &th_tab[s].etls;
}

void *__emutls_get_address(void *control){
	unsigned long index = emutls_get_index(control);
	unsigned long *np; void ***ap = emutls_array_slot(&np);
	if(index > *np){                              /* grow (upstream: emutls_alloc_array) */
		unsigned long n = *np ? *np : 64;
		while(n < index) n *= 2;
		void **a = realloc(*ap, n * sizeof(void *));
		if(!a) n9_exits("cc9: emutls OOM\n");
		for(unsigned long k = *np; k < n; k++) a[k] = 0;
		*ap = a; *np = n;
	}
	void **arr = *ap;
	if(!arr[index-1]){                            /* upstream: emutls_allocate_object */
		size_t *c = control;
		size_t size = c[0];
		void *init = ((void **)control)[3];
		void *m = malloc(size ? size : 1);
		if(!m) n9_exits("cc9: emutls OOM\n");
		if(init) memcpy(m, init, size); else memset(m, 0, size);
		arr[index-1] = m;
	}
	return arr[index-1];
}

/* ---- reader/writer lock: real shared/exclusive (READER preference) ----
 * The old code made rdlock == mutex_lock, so concurrent readers SERIALIZED and a
 * thread that rdlocked twice self-deadlocked — POSIX requires N concurrent readers.
 * pthread_rwlock_t is a typedef of pthread_mutex_t (24 bytes, fixed by the ABI
 * header we can't grow), so the real state lives in a lazily-malloc'd control block
 * whose pointer we stash in the lock's `owner` field (0 = not built yet, which is
 * exactly what PTHREAD_RWLOCK_INITIALIZER and a calloc'd lock already are). The
 * block is a plain monitor built from cc9's OWN mutex+condvars, which already carry
 * the FIFO / no-lost-wakeup discipline (pthread_cond_* above).
 *
 * READER preference (glibc's PTHREAD_RWLOCK_PREFER_READER default): a rdlock blocks
 * ONLY while a writer holds, so readers never serialize and a recursive rdlock can't
 * self-deadlock. Mutual exclusion: `writer=1` is set only when readers==0, and
 * `readers++` only when writer==0, both under c->mtx — never simultaneously.
 * Residual limits, stated honestly:
 *   - writers can starve under a continuous stream of readers (the price of reader
 *     preference; acceptable for the read-mostly in-tree user, libunwind's FDE cache);
 *   - the control block is process-local heap, so a pshared rwlock placed in shm9
 *     does NOT work across processes (mutexes still do — this is a rwlock-only gap). */
typedef struct { pthread_mutex_t mtx; pthread_cond_t rok, wok; int readers; int writer; } cc9_rw;
static int rw_glock = 1;   /* guards the first-touch allocation of a lock's control block */

static cc9_rw *rw_ctl(pthread_rwlock_t *l){
	cc9_rw *c = (cc9_rw *)__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE);
	if(c) return c;                               /* fast path: already built */
	n9_semacquire(&rw_glock,1);
	c = (cc9_rw *)l->owner;                        /* re-check under the lock */
	if(!c){
		c = malloc(sizeof *c);
		if(c){
			pthread_mutex_init(&c->mtx, 0);        /* NORMAL: the fast futex path */
			pthread_cond_init(&c->rok, 0);
			pthread_cond_init(&c->wok, 0);
			c->readers = 0; c->writer = 0;
			__atomic_store_n(&l->owner, (unsigned long)c, __ATOMIC_RELEASE);
		}
	}
	n9_semrelease(&rw_glock,1);
	return c;
}
int pthread_rwlock_init(pthread_rwlock_t *l, const void *a){
	(void)a;   /* rwlockattr carries only pshared (meaningless under RFMEM) and is NOT
	              a mutexattr — the old code cast it to one and read a bogus ->kind. */
	/* POSIX-UB to re-init an already-initialized lock; we deliberately DON'T free an
	 * existing block (owner!=0), so re-init of a live lock leaks it — caller's bug. */
	l->sem=0; l->held=0; l->owner=0; l->count=0; l->kind=0; return 0;
}
int pthread_rwlock_destroy(pthread_rwlock_t *l){
	/* POSIX precondition: no thread holds/references the lock at destroy — so the
	 * unlocked read+free below is race-free by contract, not by luck. */
	cc9_rw *c = (cc9_rw *)l->owner;
	if(c){ free(c); l->owner=0; }   /* mutex/cond destroy are no-ops; just free the block */
	return 0;
}
int pthread_rwlock_rdlock(pthread_rwlock_t *l){
	cc9_rw *c = rw_ctl(l); if(!c) return 12;   /* ENOMEM */
	pthread_mutex_lock(&c->mtx);
	while(c->writer) pthread_cond_wait(&c->rok, &c->mtx);   /* reader pref: wait only for a writer */
	c->readers++;
	pthread_mutex_unlock(&c->mtx);
	return 0;
}
int pthread_rwlock_wrlock(pthread_rwlock_t *l){
	cc9_rw *c = rw_ctl(l); if(!c) return 12;
	pthread_mutex_lock(&c->mtx);
	while(c->writer || c->readers>0) pthread_cond_wait(&c->wok, &c->mtx);
	c->writer = 1;
	pthread_mutex_unlock(&c->mtx);
	return 0;
}
int pthread_rwlock_unlock(pthread_rwlock_t *l){
	cc9_rw *c = rw_ctl(l); if(!c) return 0;
	pthread_mutex_lock(&c->mtx);
	if(c->writer){                          /* writer releasing: wake all readers + one writer */
		c->writer = 0;
		pthread_cond_broadcast(&c->rok);    /* readers preferred; a woken writer re-checks & re-waits */
		pthread_cond_signal(&c->wok);       /* if no readers were waiting, a writer proceeds */
	} else if(c->readers > 0 && --c->readers == 0){
		pthread_cond_signal(&c->wok);       /* last reader out: a waiting writer may proceed */
	}
	pthread_mutex_unlock(&c->mtx);
	return 0;
}
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l){
	cc9_rw *c = rw_ctl(l); if(!c) return 12;
	int r = 16;   /* EBUSY */
	pthread_mutex_lock(&c->mtx);
	if(!c->writer){ c->readers++; r = 0; }
	pthread_mutex_unlock(&c->mtx);
	return r;
}
int pthread_rwlock_trywrlock(pthread_rwlock_t *l){
	cc9_rw *c = rw_ctl(l); if(!c) return 12;
	int r = 16;   /* EBUSY */
	pthread_mutex_lock(&c->mtx);
	if(!c->writer && c->readers==0){ c->writer = 1; r = 0; }
	pthread_mutex_unlock(&c->mtx);
	return r;
}

int sched_yield(void){ n9_sleep(0); return 0; }
int nanosleep(const struct timespec *req, struct timespec *rem){
	(void)rem;
	if(!req) return 0;
	long total_ms = req->tv_sec*1000 + req->tv_nsec/1000000;
	if(total_ms <= 0){ if(req->tv_nsec) n9_sleep(1); return 0; }
	/* Sleep against a wall-clock deadline so a note (e.g. SIGALRM from setitimer)
	 * that interrupts the SLEEP syscall doesn't cut the sleep short — std::thread
	 * sleep_for must honor the full duration across signals. */
	extern int clock_gettime(int, struct timespec*);
	struct timespec t; clock_gettime(0 /*CLOCK_REALTIME*/, &t);
	long long deadline = (long long)t.tv_sec*1000 + t.tv_nsec/1000000 + total_ms;
	for(;;){
		clock_gettime(0, &t);
		long long now = (long long)t.tv_sec*1000 + t.tv_nsec/1000000;
		long remms = (long)(deadline - now);
		if(remms <= 0) break;
		n9_sleep(remms);
	}
	return 0;
}

/* ---- POSIX unnamed semaphores (semaphore.h) — direct Plan 9 semaphores.
 * sem_t.v IS the kernel semaphore word; pshared is meaningless under
 * rfork(RFMEM) (all memory shared) and ignored. */
extern int *__n9_errno(void);
typedef struct { int v; } cc9_sem_t;
int sem_init(cc9_sem_t *s, int pshared, unsigned int value){ (void)pshared; s->v = (int)value; return 0; }
int sem_destroy(cc9_sem_t *s){ (void)s; return 0; }
int sem_post(cc9_sem_t *s){ n9_semrelease(&s->v, 1); return 0; }
int sem_wait(cc9_sem_t *s){ n9_semacquire(&s->v, 1); return 0; }
int sem_trywait(cc9_sem_t *s){
	if(n9_tsemacquire(&s->v, 0) == 1) return 0;
	*__n9_errno() = 11 /*EAGAIN*/; return -1;
}
int sem_timedwait(cc9_sem_t *s, const struct timespec *abs){
	struct timespec now; clock_gettime(0, &now);
	long ms = (long)(abs->tv_sec - now.tv_sec)*1000 + (abs->tv_nsec - now.tv_nsec)/1000000;
	if(ms < 0) ms = 0;
	if(n9_tsemacquire(&s->v, ms) == 1) return 0;
	*__n9_errno() = 110 /*ETIMEDOUT*/; return -1;
}
int sem_getvalue(cc9_sem_t *s, int *out){ *out = s->v; return 0; }

/* pthread_atfork — accepted, never invoked. cc9 fork() is rfork+exec-shaped
 * (libuv/nvim); nothing re-enters the runtime between fork and exec. */
int pthread_atfork(void (*prep)(void), void (*parent)(void), void (*child)(void)){
	(void)prep; (void)parent; (void)child; return 0;
}

/* attr/condattr are accepted-and-ignored (thread stacks are fixed-size heap
 * blocks; the only clock is /dev/bintime). rwlock try* live with the rwlock above. */
int pthread_attr_init(pthread_attr_t *a){ if(a){ a->detachstate = PTHREAD_CREATE_JOINABLE; a->stacksize = 0; } return 0; }
int pthread_attr_destroy(pthread_attr_t *a){ (void)a; return 0; }
/* detachstate is the one attr we actually honor — pthread_create applies it.
 * stacksize below is still a no-op (cc9_thread_stack is the real knob). */
int pthread_attr_setdetachstate(pthread_attr_t *a, int s){
	if(!a) return 22;   /* EINVAL */
	if(s != PTHREAD_CREATE_JOINABLE && s != PTHREAD_CREATE_DETACHED) return 22;
	a->detachstate = s; return 0;
}
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *s){
	if(!a || !s) return 22;
	*s = a->detachstate; return 0;
}
/* These used to be no-op stubs that ACCEPTED a stack size and threw it away, so
 * every thread silently got STACKSIZE — Rust's Builder::stack_size(8MB) (rayon /
 * stylo layout workers) got a success return and a 256 KB stack, then smashed its
 * return address recursing on deep DOM and faulted with pc pointing into the stack. */
int pthread_attr_setstacksize(pthread_attr_t *a, size_t s){ if(a) a->stacksize = s; return 0; }
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *s){
	*s = a && a->stacksize ? a->stacksize : (size_t)STACKSIZE;
	return 0;
}
int pthread_condattr_init(pthread_condattr_t *a){ (void)a; return 0; }
int pthread_condattr_destroy(pthread_condattr_t *a){ (void)a; return 0; }
int pthread_condattr_setclock(pthread_condattr_t *a, int c){ (void)a; (void)c; return 0; }
int pthread_setname_np(pthread_t t, const char *n){ (void)t; (void)n; return 0; }
int pthread_getname_np(pthread_t t, char *buf, size_t n){ (void)t; if(n) buf[0]=0; return 0; }
