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

typedef struct { void *(*start)(void *); void *arg; void *ret; int joinsem; int done; int detached; void *stack; unsigned long pid; } n9_thread;

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
static struct { int used; unsigned long pid; n9_thread *t; unsigned long stklo, stkhi; } th_tab[MAXTH];
static int th_lock = 1;

static n9_thread *find_thread(unsigned long pid){
	n9_thread *t=0;
	n9_semacquire(&th_lock,1);
	for(int i=0;i<MAXTH;i++) if(th_tab[i].used && th_tab[i].pid==pid){ t=th_tab[i].t; break; }
	n9_semrelease(&th_lock,1);
	return t;
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

static void trampoline(void *p){
	n9_thread *t = (n9_thread *)p;
	cc9_fpmask();   /* this rfork child has fresh FP state — mask FP exceptions */
	/* Register (tid, t, stack-range) BEFORE releasing the handoff the creator
	 * waits on, so by the time pthread_create returns the thread is findable AND
	 * cur_pid() resolves this thread — both before start() runs. The scalar range
	 * + used-set-last ordering is what makes the lock-free cur_pid scan safe. */
	unsigned long lo = (unsigned long)t->stack, hi = lo + STACKSIZE;
	n9_semacquire(&th_lock,1);
	for(int i=0;i<MAXTH;i++) if(!th_tab[i].used){
		th_tab[i].pid=t->pid; th_tab[i].t=t; th_tab[i].stklo=lo; th_tab[i].stkhi=hi;
		__asm__ volatile("":::"memory");   /* release: publish range before used (pairs w/ cur_pid) */
		th_tab[i].used=1; break;
	}
	n9_semrelease(&th_lock,1);
	n9_semrelease(&handoff_sem, 1);   /* signal the rfork globals are consumed + registered */
	t->ret = t->start(t->arg);
	t->done = 1;
	if(t->detached){
		/* nobody will join. Can't free our own running stack here — park it for a
		 * later create/join to reclaim. reap_slot makes the park single-owner. */
		if(reap_slot(t->pid)) dead_park(t->stack, t);
		return;
	}
	n9_semrelease(&t->joinsem, 1);    /* wake any joiner */
}

int pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*start)(void *), void *arg){
	(void)attr;
	dead_reap();   /* reclaim any parked detached-thread stacks (they're long dead now) */
	n9_thread *t = malloc(sizeof *t);
	if(!t) return 11;
	/* Unique thread id, assigned by the parent (the rfork-return pid isn't
	 * visible to the child, and /dev/pid isn't readable here). Starts at 2 (main
	 * is 1). The child registers itself in the table keyed by this id. */
	static unsigned long tid_ctr = 1;
	static int tid_lock = 1;
	n9_semacquire(&tid_lock,1); unsigned long tid = ++tid_ctr; n9_semrelease(&tid_lock,1);
	t->start=start; t->arg=arg; t->ret=0; t->joinsem=0; t->done=0; t->detached=0; t->pid=tid;
	char *stk = malloc(STACKSIZE); if(!stk){ free(t); return 11; }
	t->stack=stk;
	void *top = (void *)(((unsigned long)(stk+STACKSIZE)) & ~15UL);
	n9_semacquire(&create_lock, 1);
	long pid = n9_rfork_thread(top, trampoline, t);
	if(pid < 0){   /* rfork failed: no child will ever release handoff_sem */
		n9_semrelease(&create_lock, 1); free(stk); free(t); return 11;
	}
	n9_semacquire(&handoff_sem, 1);   /* wait until the child registered + copied the handoff */
	n9_semrelease(&create_lock, 1);
	*th = (pthread_t)tid;
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
static void mtx_ensure(pthread_mutex_t *m){ if(m->sem==0 && m->owner==0 && m->count==0 && m->kind==0) m->sem=1; }
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a){ m->sem=1; m->owner=0; m->count=0; m->kind=a?a->kind:0; return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m){ (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m){
	mtx_ensure(m);
	unsigned long self=cur_pid();
	if(m->kind==PTHREAD_MUTEX_RECURSIVE && m->owner==self){ m->count++; return 0; }
	n9_semacquire(&m->sem, 1);
	m->owner=self; m->count=1; return 0;
}
int pthread_mutex_trylock(pthread_mutex_t *m){
	mtx_ensure(m);
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

/* ---- condition variable (semaphore + waiter count under a spin-free lock) ---- */
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a){ (void)a; c->sem=0; c->waiters=0; c->lk=1; return 0; }
int pthread_cond_destroy(pthread_cond_t *c){ (void)c; return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m){
	if(c->lk==0 && c->waiters==0) c->lk=1;     /* lazy init for PTHREAD_COND_INITIALIZER */
	n9_semacquire(&c->lk,1); c->waiters++; n9_semrelease(&c->lk,1);
	pthread_mutex_unlock(m);
	n9_semacquire(&c->sem, 1);
	pthread_mutex_lock(m);
	return 0;
}
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *ts){ (void)ts; return pthread_cond_wait(c,m); }
int pthread_cond_signal(pthread_cond_t *c){
	if(c->lk==0) c->lk=1;
	n9_semacquire(&c->lk,1); if(c->waiters>0){ c->waiters--; n9_semrelease(&c->sem,1);} n9_semrelease(&c->lk,1); return 0;
}
int pthread_cond_broadcast(pthread_cond_t *c){
	if(c->lk==0) c->lk=1;
	n9_semacquire(&c->lk,1); while(c->waiters>0){ c->waiters--; n9_semrelease(&c->sem,1);} n9_semrelease(&c->lk,1); return 0;
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
static struct { int used; unsigned long pid; int key; void *val; } tls_tab[MAXTLS];
static int tls_lock = 1;
static int next_key = 1;
int pthread_key_create(pthread_key_t *k, void (*dtor)(void *)){ (void)dtor; n9_semacquire(&tls_lock,1); *k=next_key++; n9_semrelease(&tls_lock,1); return 0; }
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
int nanosleep(const struct timespec *req, struct timespec *rem){ (void)rem; if(req) n9_sleep(req->tv_sec*1000 + req->tv_nsec/1000000); return 0; }
