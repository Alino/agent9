#ifndef _SYS_TIME_H
#define _SYS_TIME_H
#include <sys/types.h>
#include <time.h>
struct timeval { long tv_sec; long tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
#ifdef __cplusplus
extern "C" {
#endif
int gettimeofday(struct timeval *, void *);
int utimes(const char *, const struct timeval *);
#ifdef __cplusplus
}
#endif
#endif
