#ifndef _PTHREAD_H
#define _PTHREAD_H
#include <sched.h>
#include <time.h>
/* cc9 pthreads over Plan 9 rfork(RFMEM) + semaphores. pthread_t is the Plan 9
 * pid of the thread (rfork return / read from /dev/pid). */
typedef unsigned long pthread_t;
/* `held` is the futex word (Drepper's 3-state: 0=free, 1=held, 2=held+waiters);
 * `sem` is ONLY the sleep channel for real contention. An uncontended lock is one
 * atomic CAS, no syscall — Plan 9's semacquire/semrelease are syscalls (~1.4us)
 * even on a free semaphore, which cost ~3.2us per std::sync::Mutex lock+unlock
 * (measured) vs ~20ns for Linux's futex.
 *
 * `held` deliberately occupies the 4 bytes of PADDING that already sat between
 * sem and owner: sizeof and the offsets of owner/count/kind are UNCHANGED, so
 * C/C++ objects compiled against the old header (cargo caches mozjs/angle build
 * output and does not track these headers) keep working. Their static
 * PTHREAD_MUTEX_INITIALIZER leaves sem=1 rather than 0 — harmless: it's one
 * spurious token on the sleep channel, which the acquire loop just re-checks. */
typedef struct { int sem; int held; unsigned long owner; int count; int kind; } pthread_mutex_t;
/* FIFO waiter queue: each waiter blocks on its OWN semaphore (a node on its
 * stack), so a signal targets a specific already-queued waiter and a newly
 * arriving waiter (appended at the tail) cannot steal it. lk = internal
 * mutex-semaphore (1=free). */
typedef struct { int lk; int pad; void *head; void *tail; } pthread_cond_t;
typedef int pthread_once_t;
typedef int pthread_key_t;
typedef struct { int kind; } pthread_mutexattr_t;
typedef struct { int unused; } pthread_condattr_t;
typedef struct { int detachstate; unsigned long stacksize; } pthread_attr_t;
typedef pthread_mutex_t pthread_rwlock_t;   /* a plain mutex (readers serialize) */
#define PTHREAD_RWLOCK_INITIALIZER {0,0,0,0,0}
#define PTHREAD_MUTEX_INITIALIZER {0,0,0,0,0}
#define PTHREAD_COND_INITIALIZER {1,0,0,0}
#define PTHREAD_ONCE_INIT 0
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT 0
/* JOINABLE must be 0: a zeroed/uninitialized attr then means "joinable",
 * matching both POSIX's default and what pthread_create does with no attr. */
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
#ifdef __cplusplus
extern "C" {
#endif
int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
int pthread_detach(pthread_t);
int pthread_kill(pthread_t, int);   /* sig 0 = exists?, 9 = kill; else EINVAL */
int pthread_attr_setdetachstate(pthread_attr_t *, int);
int pthread_attr_getdetachstate(const pthread_attr_t *, int *);
pthread_t pthread_self(void);
int pthread_equal(pthread_t, pthread_t);
void pthread_exit(void *);
int pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int pthread_mutex_destroy(pthread_mutex_t *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_trylock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_mutexattr_init(pthread_mutexattr_t *);
int pthread_mutexattr_destroy(pthread_mutexattr_t *);
/* Process-shared: cc9 mutexes are Plan 9 semaphores on the mutex word, which
 * work across processes when the mutex lives in shared memory (shm9) — so
 * accepting the attribute is honest, not a stub. */
#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED 1
int pthread_mutexattr_setpshared(pthread_mutexattr_t *, int);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *, int *);
int pthread_mutexattr_settype(pthread_mutexattr_t *, int);
int pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
int pthread_cond_destroy(pthread_cond_t *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int pthread_cond_signal(pthread_cond_t *);
int pthread_cond_broadcast(pthread_cond_t *);
int pthread_once(pthread_once_t *, void (*)(void));
int pthread_key_create(pthread_key_t *, void (*)(void *));
int pthread_key_delete(pthread_key_t);
void *pthread_getspecific(pthread_key_t);
int pthread_setspecific(pthread_key_t, const void *);
int pthread_rwlock_init(pthread_rwlock_t *, const void *);
int pthread_rwlock_destroy(pthread_rwlock_t *);
int pthread_rwlock_rdlock(pthread_rwlock_t *);
int pthread_rwlock_wrlock(pthread_rwlock_t *);
int pthread_rwlock_unlock(pthread_rwlock_t *);
int sched_yield(void);
int nanosleep(const struct timespec *, struct timespec *);
#ifdef __cplusplus
}
#endif
#endif
#ifdef __cplusplus
extern "C" {
#endif
int pthread_atfork(void (*)(void), void (*)(void), void (*)(void));
int pthread_attr_init(pthread_attr_t *);
int pthread_attr_destroy(pthread_attr_t *);
int pthread_attr_setstacksize(pthread_attr_t *, size_t);
int pthread_attr_getstacksize(const pthread_attr_t *, size_t *);
/* Stack bounds of the CALLING thread (hi = base, stacks grow down). Wanted by
 * conservative GCs to know the root-scan range. cc9_stack_bounds is the native
 * spelling; pthread_get_stackaddr_np is the Darwin one and ignores its argument
 * (self only). Both answer for the main thread too. */
int cc9_stack_bounds(void **lo, void **hi);
void *pthread_get_stackaddr_np(pthread_t);
int pthread_condattr_init(pthread_condattr_t *);
int pthread_condattr_destroy(pthread_condattr_t *);
int pthread_condattr_setclock(pthread_condattr_t *, int);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *);
int pthread_rwlock_trywrlock(pthread_rwlock_t *);
int pthread_setname_np(pthread_t, const char *);
int pthread_getname_np(pthread_t, char *, size_t);
int pthread_sigmask(int, const unsigned long *, unsigned long *);
int pthread_getschedparam(pthread_t, int *, struct sched_param *);
int pthread_setschedparam(pthread_t, int, const struct sched_param *);
#ifdef __cplusplus
}
#endif
