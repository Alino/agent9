#ifndef _TIME_H
#define _TIME_H
#include <stddef.h>
typedef long time_t;
typedef long clock_t;
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
struct timespec { time_t tv_sec; long tv_nsec; };
#define CLOCKS_PER_SEC 1000000L
#ifdef __cplusplus
extern "C" {
#endif
time_t time(time_t *); clock_t clock(void);
double difftime(time_t, time_t); time_t mktime(struct tm *);
char *asctime(const struct tm *); char *ctime(const time_t *);
struct tm *gmtime(const time_t *); struct tm *localtime(const time_t *);
size_t strftime(char *, size_t, const char *, const struct tm *);
int timespec_get(struct timespec *, int);
#ifdef __cplusplus
}
#endif
#endif
