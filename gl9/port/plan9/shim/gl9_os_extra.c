/* gl9_os_extra.c — the true runtime gaps between cc9's freestanding libc and
 * what Mesa needs, once the Linux/glibc feature macros are scrubbed. Kept as
 * small as possible (the "ported:" comment spirit). Compile-time gaps are in
 * gl9_pre.h; this file is real code linked into the gl9 archive. */
#include "gl9_pre.h"

/* --- pthread barrier over cc9's mutex+cond (see gl9_pre.h for the type). ------
 * Correct N-thread barrier: a phase counter flips when the last arriver shows up
 * and broadcasts; earlier arrivers wait for the phase to change. softpipe runs
 * single-threaded for now, so this rarely fires, but it must link. */
int
pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count)
{
	(void)a;
	if (count == 0)
		return 22;			/* EINVAL */
	pthread_mutex_init(&b->mtx, 0);
	pthread_cond_init(&b->cond, 0);
	b->count = count;
	b->waiting = 0;
	b->phase = 0;
	return 0;
}

int
pthread_barrier_destroy(pthread_barrier_t *b)
{
	pthread_cond_destroy(&b->cond);
	pthread_mutex_destroy(&b->mtx);
	return 0;
}

int
pthread_barrier_wait(pthread_barrier_t *b)
{
	unsigned ph;

	pthread_mutex_lock(&b->mtx);
	ph = b->phase;
	if (++b->waiting == b->count) {
		b->waiting = 0;
		b->phase++;
		pthread_cond_broadcast(&b->cond);
		pthread_mutex_unlock(&b->mtx);
		return PTHREAD_BARRIER_SERIAL_THREAD;
	}
	while (ph == b->phase)
		pthread_cond_wait(&b->cond, &b->mtx);
	pthread_mutex_unlock(&b->mtx);
	return 0;
}

/* --- libc gaps cc9 doesn't provide -------------------------------------- */

#include <stdarg.h>
extern void *malloc(unsigned long);
extern void *memset(void *, int, unsigned long);

/* sprintf/strcasecmp/strncasecmp/strtok_r/pthread_sigmask used to be defined
 * here; cc9's runtime grew them (neovim9 port), so they're gone to avoid
 * duplicate symbols. (this file is compiled -fno-builtin so clang doesn't
 * intercept the str* defs below.) */

char *
stpcpy(char *d, const char *s)
{
	while ((*d = *s)) { d++; s++; }
	return d;
}

char *
strchrnul(const char *s, int c)
{
	while (*s && *s != (char)c)
		s++;
	return (char *)s;
}

/* open_memstream over cc9's fmemopen. ponytail: fixed 1 MB buffer (calloc'd, so
 * text output stays NUL-terminated); *sizep is not updated live. Enough for
 * nir_print's debug-to-string path (the only user in the softpipe set); grow via
 * a cookie stream if a real workload needs exact sizes or huge dumps. */
FILE *
open_memstream(char **bufp, unsigned long *sizep)
{
	enum { CAP = 1u << 20 };
	char *buf = malloc(CAP);
	if (!buf)
		return 0;
	memset(buf, 0, CAP);
	*bufp = buf;
	*sizep = 0;
	return fmemopen(buf, CAP, "w");
}

/* --- pthread gaps: single-threaded softpipe, so signals/timeouts are moot -- */
int
pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *t)
{
	(void)t;
	return pthread_mutex_lock(m);	/* ignore the deadline; just block */
}

int
pthread_getcpuclockid(pthread_t th, int *clk)
{
	(void)th;
	if (clk)
		*clk = 2;		/* CLOCK_PROCESS_CPUTIME_ID-ish; unused */
	return 0;
}
