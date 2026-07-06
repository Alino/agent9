#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H
/* POSIX unnamed semaphores over Plan 9 semacquire/semrelease (pthread.c). */
#include <time.h>
typedef struct { int v; } sem_t;
#ifdef __cplusplus
extern "C" {
#endif
int sem_init(sem_t *, int, unsigned int);
int sem_destroy(sem_t *);
int sem_post(sem_t *);
int sem_wait(sem_t *);
int sem_trywait(sem_t *);
int sem_timedwait(sem_t *, const struct timespec *);
int sem_getvalue(sem_t *, int *);
#ifdef __cplusplus
}
#endif
#endif
