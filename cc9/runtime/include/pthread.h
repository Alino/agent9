#ifndef _PTHREAD_H
#define _PTHREAD_H
#include <time.h>
/* cc9 pthreads over Plan 9 rfork(RFMEM) + semaphores. pthread_t is the Plan 9
 * pid of the thread (rfork return / read from /dev/pid). */
typedef unsigned long pthread_t;
typedef struct { int sem; unsigned long owner; int count; int kind; } pthread_mutex_t;
typedef struct { int sem; int waiters; int lk; } pthread_cond_t;
typedef int pthread_once_t;
typedef int pthread_key_t;
typedef struct { int kind; } pthread_mutexattr_t;
typedef struct { int unused; } pthread_condattr_t;
typedef struct { int unused; } pthread_attr_t;
typedef pthread_mutex_t pthread_rwlock_t;   /* a plain mutex (readers serialize) */
#define PTHREAD_RWLOCK_INITIALIZER {1,0,0,0}
#define PTHREAD_MUTEX_INITIALIZER {1,0,0,0}
#define PTHREAD_COND_INITIALIZER {0,0,1}
#define PTHREAD_ONCE_INIT 0
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT 0
#ifdef __cplusplus
extern "C" {
#endif
int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
int pthread_detach(pthread_t);
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
