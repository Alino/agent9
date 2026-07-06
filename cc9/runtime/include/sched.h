#ifndef _SCHED_H
#define _SCHED_H
#ifdef __cplusplus
extern "C" {
#endif
int sched_yield(void);
struct sched_param { int sched_priority; };
#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2
int sched_get_priority_min(int);
int sched_get_priority_max(int);
#ifdef __cplusplus
}
#endif
#endif
