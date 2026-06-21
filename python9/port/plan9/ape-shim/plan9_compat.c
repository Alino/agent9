/*
 * plan9_compat.c -- small POSIX functions APE's libc lacks, for the CPython
 * 9front port. Prototypes are declared in pyconfig.h so CPython sources see
 * them. Compiled into the interpreter (added to the object list).
 */
#ifndef _POSIX_SOURCE
#define _POSIX_SOURCE
#endif
#ifndef _BSD_EXTENSION
#define _BSD_EXTENSION
#endif
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>

/* plan9: APE's libm violates C99 for special values -- it sets errno (EDOM/
 * ERANGE) and returns wrong results for inf/nan (sqrt(nan)=0, exp(inf)=DBL_MAX,
 * ...). CPython's math module inspects errno and raises spuriously. The shims
 * below (and the ones APE lacks entirely) handle inf/nan/zero per C99 so
 * test_math's special-value assertions pass. Helpers for the IEEE specials: */
#define _LN2 0.69314718055994530942

static double
_qnan(void)
{
	union { unsigned long long u; double d; } v;
	v.u = 0x7ff8000000000000ULL;	/* quiet NaN */
	return v.d;
}

static double
_pinf(void)
{
	union { unsigned long long u; double d; } v;
	v.u = 0x7ff0000000000000ULL;	/* +inf */
	return v.d;
}

/* hwsqrt: exact hardware sqrt (SQRTSD), in hwsqrt.s -- APE's software sqrt
 * returns 0 (not NaN) for NaN input and sets EDOM. */
extern double hwsqrt(double);

double
sqrt(double x)
{
	if (isnan(x))
		return x;		/* sqrt(nan)=nan, no errno */
	if (x == 0.0)
		return x;		/* sqrt(+-0)=+-0 */
	if (x < 0.0) {
		errno = EDOM;
		return _qnan();
	}
	if (isinf(x))
		return x;		/* sqrt(+inf)=+inf */
	return hwsqrt(x);		/* finite positive: exact */
}

/* plan9: real +inf / quiet NaN by bit pattern -- APE's HUGE_VAL is finite, so
 * CPython's Py_HUGE_VAL/Py_NAN (and NAN) are broken without these. Referenced
 * from pyconfig.h's NAN/Py_NAN/Py_HUGE_VAL macros. */
double
_plan9_inf(void)
{
	union { unsigned long long u; double d; } v;
	v.u = 0x7ff0000000000000ULL;
	return v.d;
}

double
_plan9_nan(void)
{
	union { unsigned long long u; double d; } v;
	v.u = 0x7ff8000000000000ULL;
	return v.d;
}

/* C99 math functions APE's libm lacks (CPython 3.11 assumes they exist). */
double
copysign(double x, double y)
{
	/* copy the sign BIT of y onto x -- the only correct way to handle
	 * negative zero (copysign(4, -0.0) must be -4.0). */
	union { double d; unsigned long long u; } ux, uy;
	ux.d = x;
	uy.d = y;
	ux.u = (ux.u & 0x7fffffffffffffffULL) | (uy.u & 0x8000000000000000ULL);
	return ux.d;
}

double
round(double x)
{
	double t;

	/* C99 round(): round half away from zero. The naive floor(x+0.5) is
	   wrong for large integral x -- e.g. x = 4999999999999999.0 has no
	   fractional bits, but x+0.5 rounds up to 5e15. Values with magnitude
	   >= 2^52 are already integral, so return them unchanged; also preserve
	   the sign of zero. */
	if (isnan(x) || isinf(x) || x == 0.0)
		return x;
	if (x >= 0.0) {
		if (x >= 4503599627370496.0)		/* 2^52 */
			return x;
		t = floor(x + 0.5);
		if (t - x > 0.5)			/* x+0.5 rounded up past x */
			t -= 1.0;
		return t;
	}
	if (x <= -4503599627370496.0)
		return x;
	t = ceil(x - 0.5);
	if (x - t > 0.5)
		t += 1.0;
	return t;
}

/* C99 math functions APE lacks (it has erf/erfc/log2/hypot but not these).
 * CPython 3.11's math module assumes they exist. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double
log1p(double x)
{
	double u;

	if (isnan(x))
		return x;
	if (x == 0.0)
		return x;		/* preserves -0.0 */
	if (x == -1.0) {
		errno = ERANGE;
		return -_pinf();	/* log1p(-1) = -inf */
	}
	if (x < -1.0) {
		errno = EDOM;
		return _qnan();
	}
	if (isinf(x))
		return x;		/* log1p(+inf) = +inf */
	/* log(1+x), accurate for small x */
	u = 1.0 + x;
	if (u == 1.0)
		return x;
	return log(u) * (x / (u - 1.0));
}

double
expm1(double x)
{
	double u;

	if (isnan(x))
		return x;
	if (isinf(x))
		return (x > 0.0) ? x : -1.0;	/* expm1(+inf)=+inf, expm1(-inf)=-1 */
	/* exp(x)-1, accurate for small x */
	u = exp(x);
	if (isinf(u)) {			/* exp(x) overflowed -> expm1 overflows */
		errno = ERANGE;
		return u;
	}
	if (u == 1.0)
		return x;
	if (u - 1.0 == -1.0)
		return -1.0;
	return (u - 1.0) * (x / log(u));
}

double
acosh(double x)
{
	double t;

	if (isnan(x))
		return x;
	if (x < 1.0) {
		errno = EDOM;
		return _qnan();
	}
	if (isinf(x))
		return x;		/* acosh(+inf) = +inf */
	if (x > 1.0e8)
		return log(x) + _LN2;	/* ~log(2x); avoids x*x overflow */
	t = x - 1.0;
	return log1p(t + sqrt(t * t + 2.0 * t));	/* sqrt(x^2-1)=sqrt(t^2+2t) */
}

double
asinh(double x)
{
	double a, r;

	if (isnan(x) || isinf(x) || x == 0.0)
		return x;
	a = fabs(x);
	if (a > 1.0e8)
		r = log(a) + _LN2;	/* ~log(2a); avoids a*a overflow */
	else
		r = log1p(a + a * a / (1.0 + sqrt(1.0 + a * a)));
	return (x < 0.0) ? -r : r;
}

double
atanh(double x)
{
	double a;

	if (isnan(x))
		return x;
	a = fabs(x);
	if (a > 1.0) {
		errno = EDOM;
		return _qnan();
	}
	if (a == 1.0) {
		errno = ERANGE;
		return (x < 0.0) ? -_pinf() : _pinf();
	}
	if (x == 0.0)
		return x;
	return 0.5 * log1p(2.0 * x / (1.0 - x));
}

double
cbrt(double x)
{
	double y;

	if (isnan(x) || isinf(x) || x == 0.0)
		return x;
	if (x < 0.0)
		y = -pow(-x, 1.0 / 3.0);
	else
		y = pow(x, 1.0 / 3.0);
	/* one Halley step to refine pow()'s imprecise cube root */
	y = y - (y * y * y - x) / (3.0 * y * y);
	return y;
}

double
exp2(double x)
{
	if (isnan(x))
		return x;
	if (isinf(x))
		return (x > 0.0) ? x : 0.0;	/* exp2(+inf)=+inf, exp2(-inf)=0 */
	return pow(2.0, x);	/* finite; overflow -> ERANGE (correct) */
}

double
trunc(double x)
{
	if (x < 0.0)
		return ceil(x);
	return floor(x);
}

/* Lanczos approximation (g=7, n=9) for the gamma family. */
static const double _lz_g = 7.0;
static const double _lz_c[9] = {
	0.99999999999980993, 676.5203681218851, -1259.1392167224028,
	771.32342877765313, -176.61502916214059, 12.507343278686905,
	-0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7
};

double
tgamma(double x)
{
	double a, t;
	int i;

	if (x < 0.5)
		return M_PI / (sin(M_PI * x) * tgamma(1.0 - x));
	x -= 1.0;
	a = _lz_c[0];
	t = x + _lz_g + 0.5;
	for (i = 1; i < 9; i++)
		a += _lz_c[i] / (x + (double)i);
	return sqrt(2.0 * M_PI) * pow(t, x + 0.5) * exp(-t) * a;
}

double
lgamma(double x)
{
	double a, t;
	int i;

	if (x < 0.5)
		return log(M_PI / fabs(sin(M_PI * x))) - lgamma(1.0 - x);
	x -= 1.0;
	a = _lz_c[0];
	t = x + _lz_g + 0.5;
	for (i = 1; i < 9; i++)
		a += _lz_c[i] / (x + (double)i);
	return 0.5 * log(2.0 * M_PI) + (x + 0.5) * log(t) - t + log(a);
}

/* clock_gettime via APE gettimeofday (microsecond resolution). All clk_id
 * values map to wall-clock; good enough for a first boot. */
#include <sys/time.h>
#include <time.h>
int
clock_gettime(int clk_id, struct timespec *tp)
{
	struct timeval tv;

	(void)clk_id;
	if (gettimeofday(&tv, (struct timezone *)0) != 0)
		return -1;
	tp->tv_sec = tv.tv_sec;
	tp->tv_nsec = (long)tv.tv_usec * 1000;
	return 0;
}

int
clock_getres(int clk_id, struct timespec *res)
{
	(void)clk_id;
	res->tv_sec = 0;
	res->tv_nsec = 1000;	/* gettimeofday gives microsecond resolution */
	return 0;
}

/* Note: APE's libap provides localtime_r/gmtime_r (just undeclared in its
 * <time.h>); we only declare them in pyconfig.h, not redefine them here. */

/*
 * setenv via Plan 9's /env. Plan 9 environment variables are files under
 * /env, so writing the value there is the native equivalent. APE has no
 * setenv(); CPython uses it for locale coercion.
 */
/*
 * environ[] maintenance. APE builds the child's /env from the parent's
 * environ[] at execve() time, so writing only to /env (below) is *not* enough:
 * a subprocess spawned without an explicit env would get a child /env rebuilt
 * from the STALE environ[], dropping the new variable. So mirror every change
 * into environ[] too. (Memory for replaced/removed entries is intentionally
 * leaked -- safe and simple, and matches glibc's setenv.)
 */
extern char **environ;

static int
env_array_set(const char *name, const char *value)
{
	size_t nl = strlen(name), vl = strlen(value);
	char *entry, **ne;
	int n;

	entry = malloc(nl + vl + 2);
	if (entry == 0)
		return -1;
	memcpy(entry, name, nl);
	entry[nl] = '=';
	memcpy(entry + nl + 1, value, vl + 1);

	for (n = 0; environ && environ[n]; n++) {
		if (strncmp(environ[n], name, nl) == 0 && environ[n][nl] == '=') {
			environ[n] = entry;		/* replace in place */
			return 0;
		}
	}
	ne = malloc((n + 2) * sizeof(char *));
	if (ne == 0) {
		free(entry);
		return -1;
	}
	memcpy(ne, environ, n * sizeof(char *));
	ne[n] = entry;
	ne[n + 1] = 0;
	environ = ne;
	return 0;
}

static void
env_array_unset(const char *name)
{
	size_t nl = strlen(name);
	int i, j;

	if (environ == 0)
		return;
	for (i = j = 0; environ[i]; i++) {
		if (strncmp(environ[i], name, nl) == 0 && environ[i][nl] == '=')
			continue;		/* drop it */
		environ[j++] = environ[i];
	}
	environ[j] = 0;
}

int
setenv(const char *name, const char *value, int overwrite)
{
	char path[1024];
	int fd, n;

	if (name == 0 || name[0] == 0 || strchr(name, '='))
		return -1;
	if (strlen(name) > sizeof(path) - 6)
		return -1;
	strcpy(path, "/env/");
	strcat(path, name);

	if (!overwrite) {
		fd = open(path, O_RDONLY);
		if (fd >= 0) {		/* already set, leave it */
			close(fd);
			return 0;
		}
	}
	if (env_array_set(name, value) != 0)
		return -1;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	n = (int)strlen(value);
	if (write(fd, value, n) != n) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/* getentropy via Plan 9's /dev/random (APE has no getentropy). */
int
getentropy(void *buf, unsigned long len)
{
	char *p = (char *)buf;
	int fd;
	long n;

	fd = open("/dev/random", O_RDONLY);
	if (fd < 0)
		return -1;
	while (len > 0) {
		n = read(fd, p, len);
		if (n <= 0) {
			close(fd);
			return -1;
		}
		p += n;
		len -= n;
	}
	close(fd);
	return 0;
}

int
unsetenv(const char *name)
{
	char path[1024];

	if (name == 0 || name[0] == 0 || strchr(name, '='))
		return -1;
	if (strlen(name) > sizeof(path) - 6)
		return -1;
	strcpy(path, "/env/");
	strcat(path, name);
	env_array_unset(name);
	remove(path);
	return 0;
}

/*
 * fmod -- APE's libm fmod (/sys/src/ape/lib/ap/math/fmod.c) loops forever when
 * either operand is NaN or Inf (its reduction loop's comparisons are all false
 * for NaN, so it never terminates). CPython's math.fmod feeds it inf/nan in
 * test_math, which hung the interpreter indefinitely (our timeout watchdog is a
 * no-op without threads). Provide a correct C99/IEEE fmod: handle the special
 * cases, then reduce finite values with exact binary long division (every
 * subtraction is in the Sterbenz-exact range, and doubling/halving by 2 is
 * exact), so the result is bit-accurate.
 */
double
fmod(double x, double y)
{
	union { unsigned long long u; double d; } qnan;
	double ax, ay, r, ys;

	qnan.u = 0x7ff8000000000000ULL;	/* quiet NaN */

	if (isnan(x) || isnan(y) || isinf(x) || y == 0.0)
		return qnan.d;
	if (isinf(y) || x == 0.0)
		return x;

	ax = fabs(x);
	ay = fabs(y);
	if (ax < ay)
		return x;
	if (ax == ay)
		return (x < 0.0) ? -0.0 : 0.0;

	/* ys = largest ay*2^k <= ax */
	ys = ay;
	while (ys <= ax * 0.5)
		ys += ys;		/* exact doubling */
	r = ax;
	for (;;) {
		if (r >= ys)
			r -= ys;	/* exact: ys <= r < 2*ys (Sterbenz) */
		if (ys == ay)
			break;
		ys *= 0.5;		/* exact halving */
	}
	return (x < 0.0) ? -r : r;
}

/* hstrerror: APE has no h_errno string table. _socket links against it. */
const char *
hstrerror(int err)
{
	switch (err) {
	case 1: return "Unknown host";
	case 2: return "Host name lookup failure";
	case 3: return "Unknown server error";
	case 4: return "No address associated with name";
	default: return "Unknown resolver error";
	}
}

double
nextafter(double x, double y)
{
	union { double d; unsigned long long u; } v;
	/* nan first: kencc compiles `x == y` as true for nan operands, so the
	   == short-circuit below would wrongly return y for a nan argument. */
	if (isnan(x) || isnan(y))
		return x + y;
	if (x == y)
		return y;
	if (x == 0.0) {
		v.u = 1;
		return y > 0.0 ? v.d : -v.d;
	}
	v.d = x;
	if ((y > x) == (x > 0.0))
		v.u++;
	else
		v.u--;
	return v.d;
}
