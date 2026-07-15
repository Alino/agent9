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
typedef struct { void *(*start)(void *); void *arg; void *ret; int joinsem; int done; int detached; void *stack; unsigned long stksize; unsigned long pid; unsigned long realpid; } n9_thread;

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
static struct { int used; int dead; unsigned long pid; n9_thread *t; unsigned long stklo, stkhi; int errno_v; } th_tab[MAXTH];
static int th_lock = 1;

/* Per-thread errno slot, resolved by %rsp stack-range like cur_pid (lock-free,
 * same TSO discipline). Weak-referenced from n9libc's __n9_errno so thread-free
 * links never pull pthread.o. Main thread (or a reaped slot) -> 0 = use the
 * global. A slot reused after join could absorb a stale write through a saved
 * errno pointer — errno is written-then-read immediately, so harmless. */
int *cc9_thread_errno_slot(void){
	unsigned long sp; __asm__ volatile("movq %%rsp,%0":"=r"(sp));
	for(int i=0;i<MAXTH;i++){
		if(!th_tab[i].used) continue;
		__asm__ volatile("":::"memory");
		if(sp>=th_tab[i].stklo && sp<th_tab[i].stkhi) return &th_tab[i].errno_v;
	}
	return 0;
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
	unsigned long sp; __asm__ volatile("movq %%rsp,%0":"=r"(sp));
	for(int i=0;i<MAXTH;i++){
		if(!th_tab[i].used) continue;
		__asm__ volatile("":::"memory");
		if(sp>=th_tab[i].stklo && sp<th_tab[i].stkhi){
			*lo = (void*)th_tab[i].stklo;
			*hi = (void*)th_tab[i].stkhi;
			return 0;
		}
	}
	*lo = __cc9_main_stack;
	*hi = __cc9_main_stack + (unsigned long)CC9_STACK_BYTES;
	return 0;
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
static void dead_reap(void){
	n9_semacquire(&dead_lock,1);
	for(int i=0;i<dead_n;i++){ free(dead_tab[i].stack); free(dead_tab[i].t); }
	dead_n=0;
	n9_semrelease(&dead_lock,1);
}

/* current "pid" = a stable per-thread id derived from which stack %rsp is in. */
static unsigned long cur_pid(void){
	unsigned long sp; __asm__ volatile("movq %%rsp,%0":"=r"(sp));
	for(int i=0;i<MAXTH;i++){
		if(!th_tab[i].used) continue;
		__asm__ volatile("":::"memory");   /* acquire: don't hoist range reads before used (pairs w/ trampoline) */
		if(sp>=th_tab[i].stklo && sp<th_tab[i].stkhi) return th_tab[i].pid;
	}
	return 1;   /* main thread */
}

/* Per-thread __cxa_thread_atexit: thread_local destructors + promise::
 * set_value_at_thread_exit must run when THIS thread exits, not at process exit.
 * Keyed by cur_pid(); the trampoline runs the current thread's list (LIFO) after
 * start() returns. Main thread (pid 1) routes to the process atexit, as before. */
extern int __cxa_atexit(void (*)(void *), void *, void *);
struct n9_tae { unsigned long pid; void (*fn)(void *); void *arg; int used; };
#define MAXTAE 4096
static struct n9_tae tae_tab[MAXTAE];
static int tae_lock = 1;
int __cxa_thread_atexit(void (*fn)(void *), void *arg, void *dso){
	unsigned long pid = cur_pid();
	if(pid == 1) return __cxa_atexit(fn, arg, dso);   /* main: run at process exit */
	n9_semacquire(&tae_lock,1);
	for(int i=0;i<MAXTAE;i++) if(!tae_tab[i].used){ tae_tab[i].pid=pid; tae_tab[i].fn=fn; tae_tab[i].arg=arg; tae_tab[i].used=1; break; }
	n9_semrelease(&tae_lock,1);
	return 0;
}
int __cxa_thread_atexit_impl(void (*fn)(void *), void *arg, void *dso){ return __cxa_thread_atexit(fn, arg, dso); }
static void run_thread_keys(unsigned long pid);   /* defined with the TLS table below */
static void run_thread_atexit(unsigned long pid){
	for(;;){   /* LIFO: highest-index entry for this pid first */
		void (*fn)(void *)=0; void *arg=0; int idx=-1;
		n9_semacquire(&tae_lock,1);
		for(int i=MAXTAE-1;i>=0;i--) if(tae_tab[i].used && tae_tab[i].pid==pid){ fn=tae_tab[i].fn; arg=tae_tab[i].arg; tae_tab[i].used=0; idx=i; break; }
		n9_semrelease(&tae_lock,1);
		if(idx<0) break;
		fn(arg);
	}
}

static void trampoline(void *p){
	n9_thread *t = (n9_thread *)p;
	cc9_fpmask();   /* this rfork child has fresh FP state — mask FP exceptions */
	/* Register (tid, t, stack-range) BEFORE releasing the handoff the creator
	 * waits on, so by the time pthread_create returns the thread is findable AND
	 * cur_pid() resolves this thread — both before start() runs. The scalar range
	 * + used-set-last ordering is what makes the lock-free cur_pid scan safe. */
	unsigned long lo = (unsigned long)t->stack, hi = lo + t->stksize;
	n9_semacquire(&th_lock,1);
	for(int i=0;i<MAXTH;i++) if(!th_tab[i].used){
		th_tab[i].pid=t->pid; th_tab[i].t=t; th_tab[i].stklo=lo; th_tab[i].stkhi=hi; th_tab[i].errno_v=0; th_tab[i].dead=0;
		__asm__ volatile("":::"memory");   /* release: publish range before used (pairs w/ cur_pid) */
		th_tab[i].used=1; break;
	}
	n9_semrelease(&th_lock,1);
	n9_semrelease(&handoff_sem, 1);   /* signal the rfork globals are consumed + registered */
	/* via a C++ shim that turns an escaped exception into std::terminate (cc9's
	 * unwinder can't unwind off the top of this C trampoline) — [thread.constr]. */
	extern void *cc9_thread_invoke(void *(*)(void *), void *);
	t->ret = cc9_thread_invoke(t->start, t->arg);
	run_thread_atexit(t->pid);   /* thread_local dtors + set_value_at_thread_exit */
	run_thread_keys(t->pid);     /* POSIX TSD destructors (frees __thread_struct etc.) */
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
	n9_semrelease(&t->joinsem, 1);    /* wake any joiner */
}

/* RLIMIT_NPROC emulation: 9front has no per-process thread cap, but the suite's
 * thread_create_failure test sets the limit to 1 and expects creation to throw.
 * setrlimit (posix_llvm.c) routes the cap here; pthread_create honors it. */
static long cc9_nproc_limit = 0x7fffffff;
void cc9_set_nproc_limit(long n){ cc9_nproc_limit = n > 0 ? n : 1; }
long cc9_get_nproc_limit(void){ return cc9_nproc_limit; }

int pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*start)(void *), void *arg){
	dead_reap();   /* reclaim any parked detached-thread stacks (they're long dead now) */
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
	size_t ss = cc9_thread_stack > STACKSIZE ? (size_t)cc9_thread_stack : STACKSIZE;
	char *stk = malloc(ss); if(!stk){ free(t); return 11; }
	t->stack=stk; t->stksize=ss;
	void *top = (void *)(((unsigned long)(stk+ss)) & ~15UL);
	n9_semacquire(&create_lock, 1);
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
void pthread_exit(void *ret){ unsigned long pid=cur_pid(); n9_thread *t=find_thread(pid); if(t){ t->ret=ret; t->done=1; n9_semrelease(&t->joinsem,1);} for(;;) n9_sleep(1000); }

/* ---- mutex (binary semaphore + optional recursion) ---- */
/* NOTE: there is deliberately NO lazy "ensure sem!=0" init here. A prior version
 * resurrected sem=1 whenever it saw {sem,owner,count,kind} all-zero — but that state
 * also occurs TRANSIENTLY during a normal lock, in the window between n9_semacquire
 * (sem 1->0) and the m->owner=self store below. A second thread hitting that window
 * would re-set sem=1 and its own semacquire would succeed too, admitting two holders
 * -> lost updates under contention. Rust and cc9's PTHREAD_MUTEX_INITIALIZER/
 * pthread_mutex_init both set sem=1, so a valid unlocked mutex is never all-zero.
 * Raw zero-initialized (non-POSIX) mutexes are unsupported and will deadlock. */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a){ m->sem=1; m->owner=0; m->count=0; m->kind=a?a->kind:0; return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m){ (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m){
	unsigned long self=cur_pid();
	if(m->kind==PTHREAD_MUTEX_RECURSIVE && m->owner==self){ m->count++; return 0; }
	n9_semacquire(&m->sem, 1);
	m->owner=self; m->count=1; return 0;
}
int pthread_mutex_trylock(pthread_mutex_t *m){
	unsigned long self=cur_pid();
	if(m->kind==PTHREAD_MUTEX_RECURSIVE && m->owner==self){ m->count++; return 0; }
	if(n9_semacquire(&m->sem, 0) < 0) return 16 /*EBUSY*/;
	m->owner=self; m->count=1; return 0;
}
int pthread_mutex_unlock(pthread_mutex_t *m){
	if((m->kind==PTHREAD_MUTEX_RECURSIVE || m->kind==PTHREAD_MUTEX_ERRORCHECK)
	   && m->owner != cur_pid()) return 1 /*EPERM*/;   /* only the owner may unlock */
	if(m->kind==PTHREAD_MUTEX_RECURSIVE && m->count>1){ m->count--; return 0; }
	m->owner=0; m->count=0; n9_semrelease(&m->sem, 1); return 0;
}
int pthread_mutexattr_init(pthread_mutexattr_t *a){ a->kind=0; return 0; }
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
static void cond_enqueue(pthread_cond_t *c, cc9_cwaiter *w){
	w->sem=0; w->dq=0; w->next=0;
	if(c->lk==0) c->lk=1;                 /* defensive: treat 0 as freshly-unlocked */
	n9_semacquire(&c->lk,1);
	if(c->tail) ((cc9_cwaiter*)c->tail)->next=w; else c->head=w;
	c->tail=w;
	n9_semrelease(&c->lk,1);
}
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a){ (void)a; c->lk=1; c->pad=0; c->head=0; c->tail=0; return 0; }
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
		n9_semacquire(&c->lk,1);
		if(w.dq){                                  /* signal dequeued+posted us as we timed out */
			n9_tsemacquire(&w.sem, 0);             /* consume the token (non-blocking) */
			rc = 0;
		} else {                                   /* still queued: unlink self */
			cc9_cwaiter *p=(cc9_cwaiter*)c->head, *prev=0;
			while(p && p!=&w){ prev=p; p=p->next; }
			if(p==&w){ if(prev) prev->next=w.next; else c->head=w.next; if(c->tail==(void*)&w) c->tail=(void*)prev; }
			rc = 110 /*ETIMEDOUT*/;
		}
		n9_semrelease(&c->lk,1);
	}
	pthread_mutex_lock(m);
	return rc;
}
int pthread_cond_signal(pthread_cond_t *c){
	if(c->lk==0) c->lk=1;
	n9_semacquire(&c->lk,1);
	cc9_cwaiter *w=(cc9_cwaiter*)c->head;
	if(w){ c->head=w->next; if(!c->head) c->tail=0; w->dq=1; n9_semrelease(&w->sem,1); }
	n9_semrelease(&c->lk,1);
	return 0;
}
int pthread_cond_broadcast(pthread_cond_t *c){
	if(c->lk==0) c->lk=1;
	n9_semacquire(&c->lk,1);
	cc9_cwaiter *w=(cc9_cwaiter*)c->head;
	c->head=0; c->tail=0;
	while(w){ cc9_cwaiter *n=w->next; w->dq=1; n9_semrelease(&w->sem,1); w=n; }
	n9_semrelease(&c->lk,1);
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

/* ---- TLS, keyed by pid (RFMEM shares memory, so no real per-thread storage) ---- */
#define MAXTLS 4096
#define MAXKEYS 256
static struct { int used; unsigned long pid; int key; void *val; } tls_tab[MAXTLS];
static int tls_lock = 1;
static int next_key = 1;
static void (*key_dtor[MAXKEYS])(void *);   /* TSD destructor per key (POSIX) */
int pthread_key_create(pthread_key_t *k, void (*dtor)(void *)){ n9_semacquire(&tls_lock,1); int key=next_key++; if(key>0&&key<MAXKEYS) key_dtor[key]=dtor; *k=key; n9_semrelease(&tls_lock,1); return 0; }
/* Run TSD destructors for a thread at its exit: for each key with a non-null
 * value and a registered destructor, null the value and call the destructor;
 * repeat (a destructor may set new values) for a few rounds, then free the
 * thread's slots. Without this, __thread_specific_ptr-held data (e.g. libc++'s
 * per-thread __thread_struct) leaks on every std::thread — tripping the suite's
 * new==delete leak checks. */
static void run_thread_keys(unsigned long pid){
	for(int round=0; round<4; round++){
		int any=0;
		for(int i=0;i<MAXTLS;i++){
			void *v=0; void (*d)(void*)=0;
			n9_semacquire(&tls_lock,1);
			if(tls_tab[i].used && tls_tab[i].pid==pid && tls_tab[i].val){
				int key=tls_tab[i].key;
				if(key>0 && key<MAXKEYS && key_dtor[key]){ v=tls_tab[i].val; d=key_dtor[key]; tls_tab[i].val=0; }
			}
			n9_semrelease(&tls_lock,1);
			if(d){ d(v); any=1; }
		}
		if(!any) break;
	}
	n9_semacquire(&tls_lock,1);
	for(int i=0;i<MAXTLS;i++) if(tls_tab[i].used && tls_tab[i].pid==pid) tls_tab[i].used=0;
	n9_semrelease(&tls_lock,1);
}
int pthread_key_delete(pthread_key_t k){ n9_semacquire(&tls_lock,1); for(int i=0;i<MAXTLS;i++) if(tls_tab[i].used && tls_tab[i].key==k) tls_tab[i].used=0; n9_semrelease(&tls_lock,1); return 0; }
void *pthread_getspecific(pthread_key_t k){ unsigned long pid=cur_pid(); for(int i=0;i<MAXTLS;i++) if(tls_tab[i].used && tls_tab[i].pid==pid && tls_tab[i].key==k) return tls_tab[i].val; return 0; }
int pthread_setspecific(pthread_key_t k, const void *v){
	unsigned long pid=cur_pid(); n9_semacquire(&tls_lock,1);
	for(int i=0;i<MAXTLS;i++) if(tls_tab[i].used && tls_tab[i].pid==pid && tls_tab[i].key==k){ tls_tab[i].val=(void*)v; n9_semrelease(&tls_lock,1); return 0; }
	for(int i=0;i<MAXTLS;i++) if(!tls_tab[i].used){ tls_tab[i].used=1; tls_tab[i].pid=pid; tls_tab[i].key=k; tls_tab[i].val=(void*)v; n9_semrelease(&tls_lock,1); return 0; }
	n9_semrelease(&tls_lock,1); return 11;
}

/* Emulated TLS (-femulated-tls): the compiler turns each `thread_local` into a
 * call to __emutls_get_address(control), where control = {size, align, index,
 * init}. We return per-thread storage keyed by (pid, control) — the ELF %fs TLS
 * model doesn't work under rfork(RFMEM) (no per-thread %fs base), but this does. */
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
#define MAXEMUTLS 8192
static struct { int used; unsigned long pid; void *ctrl; void *mem; } emutls_tab[MAXEMUTLS];
static int emutls_lock = 1;
void *__emutls_get_address(void *control){
	size_t *c = control;
	size_t size = c[0];
	void *init = ((void **)control)[3];
	unsigned long pid = cur_pid();
	/* Hold the lock across the whole find-or-create (malloc uses a DIFFERENT
	 * lock, so no deadlock). Dropping it between search and insert let a
	 * reentrant same-(pid,ctrl) call create a duplicate block (TOCTOU). */
	n9_semacquire(&emutls_lock,1);
	for(int i=0;i<MAXEMUTLS;i++)
		if(emutls_tab[i].used && emutls_tab[i].pid==pid && emutls_tab[i].ctrl==control){
			void *m=emutls_tab[i].mem; n9_semrelease(&emutls_lock,1); return m; }
	void *m = malloc(size ? size : 1);
	if(!m){ n9_semrelease(&emutls_lock,1); n9_exits("cc9: emutls OOM\n"); }
	if(init) memcpy(m, init, size); else memset(m, 0, size);
	int placed=0;
	for(int i=0;i<MAXEMUTLS;i++) if(!emutls_tab[i].used){ emutls_tab[i].used=1; emutls_tab[i].pid=pid; emutls_tab[i].ctrl=control; emutls_tab[i].mem=m; placed=1; break; }
	n9_semrelease(&emutls_lock,1);
	if(!placed) n9_exits("cc9: emutls table full\n");   /* would silently lose thread_local identity */
	return m;
}

/* rwlock = plain mutex (readers serialize; fine for libunwind's FDE cache). */
int pthread_rwlock_init(pthread_rwlock_t *l, const void *a){ return pthread_mutex_init(l,(const pthread_mutexattr_t*)a); }
int pthread_rwlock_destroy(pthread_rwlock_t *l){ return pthread_mutex_destroy(l); }
int pthread_rwlock_rdlock(pthread_rwlock_t *l){ return pthread_mutex_lock(l); }
int pthread_rwlock_wrlock(pthread_rwlock_t *l){ return pthread_mutex_lock(l); }
int pthread_rwlock_unlock(pthread_rwlock_t *l){ return pthread_mutex_unlock(l); }

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
 * blocks; the only clock is /dev/bintime). rwlock is a mutex, so try = trylock. */
int pthread_attr_init(pthread_attr_t *a){ if(a) a->detachstate = PTHREAD_CREATE_JOINABLE; return 0; }
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
int pthread_attr_setstacksize(pthread_attr_t *a, size_t s){ (void)a; (void)s; return 0; }
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *s){ (void)a; *s = 1<<20; return 0; }
int pthread_condattr_init(pthread_condattr_t *a){ (void)a; return 0; }
int pthread_condattr_destroy(pthread_condattr_t *a){ (void)a; return 0; }
int pthread_condattr_setclock(pthread_condattr_t *a, int c){ (void)a; (void)c; return 0; }
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l){ return pthread_mutex_trylock(l); }
int pthread_rwlock_trywrlock(pthread_rwlock_t *l){ return pthread_mutex_trylock(l); }
int pthread_setname_np(pthread_t t, const char *n){ (void)t; (void)n; return 0; }
int pthread_getname_np(pthread_t t, char *buf, size_t n){ (void)t; if(n) buf[0]=0; return 0; }
