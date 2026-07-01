#ifndef GL9_PRE_H
#define GL9_PRE_H
/* gl9 compile shim — force-included (-include) into every Mesa TU built for
 * 9front with cc9. It fills the handful of gaps between cc9's freestanding libc
 * and what Mesa's *portable* fallback paths assume once the Linux/glibc feature
 * macros are scrubbed (host/harvest.py). One place for all compile-time target
 * quirks; runtime gaps live in gl9_os_extra.c. Keep this tiny. */

/* amd64 is little-endian. With HAVE_ENDIAN_H scrubbed and clang's bare
 * x86_64-unknown-none target defining no OS macro (__linux__/__APPLE__/…),
 * util/u_endian.h sets neither UTIL_ARCH_* and #errors. Assert the known truth
 * here — this header is included before u_endian.h runs its final check. */
#ifndef UTIL_ARCH_LITTLE_ENDIAN
#define UTIL_ARCH_LITTLE_ENDIAN 1
#endif
#ifndef UTIL_ARCH_BIG_ENDIAN
#define UTIL_ARCH_BIG_ENDIAN 0
#endif

/* cc9's <assert.h> predates the C11 static_assert macro; map to the keyword.
 * C++ already has static_assert as a keyword. */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

/* cc9's <math.h> omits the POSIX/BSD math constants; Mesa (nir_builtin_builder.h,
 * glsl_to_nir, …) uses them directly. Define the standard set (guarded). */
#ifndef M_PI
#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.434294481903251827651
#define M_LN2      0.693147180559945309417
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.785398163397448309616
#define M_1_PI     0.318309886183790671538
#define M_2_PI     0.636619772367581343076
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.707106781186547524401
#endif

/* cc9 pthreads have no barriers, but util/u_thread.h declares util_barrier over
 * pthread_barrier_t (in both C and C++ TUs). Provide the type + prototypes here;
 * the impl (over cc9 mutex+cond) is in gl9_os_extra.c. Guarded so a future cc9
 * that ships real barriers wins. */
#ifndef GL9_HAVE_PTHREAD_BARRIER
#define GL9_HAVE_PTHREAD_BARRIER 1
#include <pthread.h>
#define PTHREAD_BARRIER_SERIAL_THREAD 1
typedef struct {
	pthread_mutex_t mtx;
	pthread_cond_t cond;
	unsigned count, waiting, phase;
} pthread_barrier_t;
typedef struct { int unused; } pthread_barrierattr_t;
#ifdef __cplusplus
extern "C" {
#endif
int pthread_barrier_init(pthread_barrier_t *, const pthread_barrierattr_t *, unsigned);
int pthread_barrier_destroy(pthread_barrier_t *);
int pthread_barrier_wait(pthread_barrier_t *);
#ifdef __cplusplus
}
#endif
#endif /* GL9_HAVE_PTHREAD_BARRIER */

/* clang has real stack alloca; cc9 doesn't declare it. Mesa calls alloca()
 * directly in a few spots (nir_functions, shaderapi, the bison/flex parsers). */
#ifndef alloca
#define alloca __builtin_alloca
#endif

/* errno values Mesa references that cc9's <errno.h> omits (standard Linux
 * numbers, matching cc9's mapping; guarded so cc9 wins if it adds them). Used
 * only in comparisons/returns on paths 9front never takes, but must be defined. */
#ifndef E2BIG
#define E2BIG        7
#endif
#ifndef EDEADLK
#define EDEADLK     35
#endif
#ifndef EBADMSG
#define EBADMSG     74
#endif
#ifndef ENOTSOCK
#define ENOTSOCK    88
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT 97
#endif
#ifndef EADDRINUSE
#define EADDRINUSE  98
#endif
#ifndef EADDRNOTAVAIL
#define EADDRNOTAVAIL 99
#endif
#ifndef ECONNABORTED
#define ECONNABORTED 103
#endif
#ifndef ECONNRESET
#define ECONNRESET  104
#endif
#ifndef EISCONN
#define EISCONN    106
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED 111
#endif
#ifndef EALREADY
#define EALREADY   114
#endif
#ifndef SIGSYS
#define SIGSYS      31
#endif

/* cc9's <time.h> has TIME_UTC but not the TIME_MONOTONIC base os_time passes to
 * timespec_get; cc9 reads /dev/bintime regardless of base, so alias it. */
#ifndef TIME_MONOTONIC
#define TIME_MONOTONIC 1
#endif

/* blake3: cc9 can't assemble the hand-written SIMD .S (CET/TLS relocations); the
 * .S are excluded from the build, so force blake3's portable C dispatch. */
#ifndef BLAKE3_NO_SSE2
#define BLAKE3_NO_SSE2 1
#define BLAKE3_NO_SSE41 1
#define BLAKE3_NO_AVX2 1
#define BLAKE3_NO_AVX512 1
#endif

/* libc / pthread surface cc9 lacks; implemented in gl9_os_extra.c. Pull the
 * headers the prototypes reference first (idempotent): FILE, sigset_t, timespec. */
#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>   /* mkdir — cc9 declares+implements it, but dd_draw.c et al.
                         * don't pull this header on the bare target. */
#ifdef __cplusplus
extern "C" {
#endif
char *strchrnul(const char *, int);
int   strcasecmp(const char *, const char *);
int   strncasecmp(const char *, const char *, unsigned long);
char *strtok_r(char *, const char *, char **);
FILE *open_memstream(char **, unsigned long *);
int   pthread_sigmask(int, const sigset_t *, sigset_t *);
int   pthread_mutex_timedlock(pthread_mutex_t *, const struct timespec *);
int   pthread_getcpuclockid(pthread_t, int *);
#ifdef __cplusplus
}
#endif

#endif /* GL9_PRE_H */
