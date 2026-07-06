#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H
#include <sys/types.h>
#include <sys/time.h>
typedef unsigned long rlim_t;
#define RLIM_INFINITY (~0ul)
#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_NPROC   6
#define RLIMIT_NOFILE  7
#define RLIMIT_AS      9
#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2
#ifdef __cplusplus
extern "C" {
#endif
int getpriority(int, unsigned int);
int setpriority(int, unsigned int, int);
#ifdef __cplusplus
}
#endif
#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
struct rlimit { rlim_t rlim_cur, rlim_max; };
struct rusage {
  struct timeval ru_utime, ru_stime;
  long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt,
       ru_nswap, ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv,
       ru_nsignals, ru_nvcsw, ru_nivcsw;
};
#ifdef __cplusplus
extern "C" {
#endif
int getrlimit(int, struct rlimit *);
int setrlimit(int, const struct rlimit *);
int getrusage(int, struct rusage *);
#ifdef __cplusplus
}
#endif
#endif
