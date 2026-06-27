#ifndef _SYS_TIME_H
#define _SYS_TIME_H
#include <sys/types.h>
#include <time.h>
struct timeval { long tv_sec; long tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
struct itimerval { struct timeval it_interval; struct timeval it_value; };
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2
#ifdef __cplusplus
extern "C" {
#endif
int gettimeofday(struct timeval *, void *);
int utimes(const char *, const struct timeval *);
int setitimer(int, const struct itimerval *, struct itimerval *);
#ifdef __cplusplus
}
#endif
#endif
