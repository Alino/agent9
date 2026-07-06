#ifndef _TIME_H
#define _TIME_H
#include <stddef.h>
typedef long time_t;
typedef long clock_t;
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
struct timespec { time_t tv_sec; long tv_nsec; };
#define CLOCKS_PER_SEC 1000000L
#define TIME_UTC 1
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
typedef int clockid_t;
typedef void *locale_t;
#ifdef __cplusplus
extern "C" {
#endif
time_t time(time_t *); clock_t clock(void);
double difftime(time_t, time_t); time_t mktime(struct tm *);
void tzset(void);
char *asctime(const struct tm *); char *ctime(const time_t *);
struct tm *gmtime(const time_t *); struct tm *localtime(const time_t *);
struct tm *gmtime_r(const time_t *, struct tm *); struct tm *localtime_r(const time_t *, struct tm *);
char *asctime_r(const struct tm *, char *); char *ctime_r(const time_t *, char *);
size_t strftime(char *, size_t, const char *, const struct tm *);
int timespec_get(struct timespec *, int);
int clock_gettime(clockid_t, struct timespec *);
size_t strftime_l(char *, size_t, const char *, const struct tm *, locale_t);
#ifdef __cplusplus
}
#endif
#endif
