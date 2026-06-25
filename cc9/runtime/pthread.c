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

#include <pthread.h>

#define STACKSIZE (256*1024)

static unsigned long cur_pid(void){
	char b[16]; int fd=(int)n9_open("/dev/pid", 0); if(fd<0) return 0;
	long n=n9_pread(fd, b, 15, -1); n9_close(fd); if(n<=0) return 0;
	unsigned long p=0; for(int i=0;i<n;i++){ if(b[i]>='0'&&b[i]<='9') p=p*10+(b[i]-'0'); else break; } return p;
}

typedef struct { void *(*start)(void *); void *arg; void *ret; int joinsem; int done; int detached; void *stack; unsigned long pid; } n9_thread;

static int create_lock = 1;       /* serializes the rfork handoff */
static int handoff_sem = 0;       /* child signals it has consumed the handoff */

#define MAXTH 1024
static struct { int used; unsigned long pid; n9_thread *t; } th_tab[MAXTH];
static int th_lock = 1;
static n9_thread main_thread;

static n9_thread *find_thread(unsigned long pid){
	for(int i=0;i<MAXTH;i++) if(th_tab[i].used && th_tab[i].pid==pid) return th_tab[i].t;
	return 0;
}

static void trampoline(void *p){
	n9_thread *t = (n9_thread *)p;
	n9_semrelease(&handoff_sem, 1);   /* signal the rfork globals are consumed */
	t->ret = t->start(t->arg);
	t->done = 1;
	n9_semrelease(&t->joinsem, 1);    /* wake any joiner */
}

int pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*start)(void *), void *arg){
	(void)attr;
	n9_thread *t = malloc(sizeof *t);
	if(!t) return 11;
	t->start=start; t->arg=arg; t->ret=0; t->joinsem=0; t->done=0; t->detached=0; t->pid=0;
	char *stk = malloc(STACKSIZE); if(!stk){ free(t); return 11; }
	t->stack=stk;
	void *top = (void *)(((unsigned long)(stk+STACKSIZE)) & ~15UL);
	n9_semacquire(&create_lock, 1);
	long pid = n9_rfork_thread(top, trampoline, t);
	n9_semacquire(&handoff_sem, 1);   /* wait until the child copied the handoff */
	n9_semrelease(&create_lock, 1);
	if(pid < 0){ free(stk); free(t); return 11; }
	/* Register from the parent (synchronously, before returning) so a join that
	 * races right after create always finds the thread — the child registering
	 * itself would race the join. */
	t->pid = (unsigned long)pid;
	n9_semacquire(&th_lock,1);
	for(int i=0;i<MAXTH;i++) if(!th_tab[i].used){ th_tab[i].used=1; th_tab[i].pid=(unsigned long)pid; th_tab[i].t=t; break; }
	n9_semrelease(&th_lock,1);
	*th = (pthread_t)pid;
	return 0;
}

int pthread_join(pthread_t pid, void **ret){
	n9_thread *t = find_thread(pid);
	if(!t) return 3;   /* ESRCH */
	n9_semacquire(&t->joinsem, 1);
	if(ret) *ret = t->ret;
	n9_semacquire(&th_lock,1);
	for(int i=0;i<MAXTH;i++) if(th_tab[i].used && th_tab[i].pid==pid){ th_tab[i].used=0; break; }
	n9_semrelease(&th_lock,1);
	free(t->stack); free(t);
	return 0;
}

int pthread_detach(pthread_t pid){ n9_thread *t=find_thread(pid); if(t) t->detached=1; return 0; }
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
int pthread_once(pthread_once_t *o, void (*fn)(void)){
	static int once_lock = 1;
	n9_semacquire(&once_lock,1);
	if(*o==0){ *o=1; n9_semrelease(&once_lock,1); fn(); }
	else n9_semrelease(&once_lock,1);
	return 0;
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

int sched_yield(void){ n9_sleep(0); return 0; }
int nanosleep(const struct timespec *req, struct timespec *rem){ (void)rem; if(req) n9_sleep(req->tv_sec*1000 + req->tv_nsec/1000000); return 0; }
