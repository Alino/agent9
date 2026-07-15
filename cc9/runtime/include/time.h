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
/* Linux "coarse" variants: same clocks here — Plan 9's bintime read is one
 * cached-fd pread either way; callers just want cheap. */
#define CLOCK_REALTIME_COARSE  CLOCK_REALTIME
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
typedef int clockid_t;
typedef void *locale_t;
#ifdef __cplusplus
extern "C" {
#endif
time_t time(time_t *); clock_t clock(void);
/* nanosleep lives in <time.h> per POSIX (it's implemented in pthread.c, which
 * is why it was only ever declared in <pthread.h> — hosted code that sleeps
 * without touching threads never saw it). */
int nanosleep(const struct timespec *, struct timespec *);
double difftime(time_t, time_t); time_t mktime(struct tm *);
time_t timegm(struct tm *);   /* mktime, UTC: Plan 9 wall time IS UTC */
void tzset(void);
char *asctime(const struct tm *); char *ctime(const time_t *);
struct tm *gmtime(const time_t *); struct tm *localtime(const time_t *);
struct tm *gmtime_r(const time_t *, struct tm *); struct tm *localtime_r(const time_t *, struct tm *);
char *asctime_r(const struct tm *, char *); char *ctime_r(const time_t *, char *);
size_t strftime(char *, size_t, const char *, const struct tm *);
int timespec_get(struct timespec *, int);
int clock_gettime(clockid_t, struct timespec *);
int clock_getres(clockid_t, struct timespec *);
size_t strftime_l(char *, size_t, const char *, const struct tm *, locale_t);
/* POSIX zone globals. Plan 9 keeps the zone in /env/timezone rather than a
 * libc global; these are populated by tzset() from it, and are the names hosted
 * code expects to read (SpiderMonkey's date code among them). */
extern char *tzname[2];
extern long timezone;
extern int daylight;
void tzset(void);
#ifdef __cplusplus
}
#endif
#endif
