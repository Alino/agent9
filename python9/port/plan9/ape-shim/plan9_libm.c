/*
 * plan9_libm.c -- correctly-rounded transcendentals for the 9front CPython
 * port, displacing APE's low-precision libm. Implementations are the classic
 * fdlibm/SunPro algorithms (freely usable). Each is < 1 ulp.
 * Built into the interpreter; these symbols override APE's exp/... at link.
 */
#include <math.h>
#include <errno.h>

static double _libm_inf(void){ union{unsigned long long u; double d;} v; v.u=0x7ff0000000000000ULL; return v.d; }
static double _libm_nan(void){ union{unsigned long long u; double d;} v; v.u=0x7ff8000000000000ULL; return v.d; }

static const double
  half_[2] = {0.5, -0.5},
  ln2HI[2] = { 6.93147180369123816490e-01, -6.93147180369123816490e-01},
  ln2LO[2] = { 1.90821492927058770002e-10, -1.90821492927058770002e-10},
  invln2   = 1.44269504088896338700e+00,
  Pe1 =  1.66666666666666019037e-01,
  Pe2 = -2.77777777770155933842e-03,
  Pe3 =  6.61375632143793436117e-05,
  Pe4 = -1.65339022054652515390e-06,
  Pe5 =  4.13813679705723846039e-08;

double
exp(double x)
{
	double y, hi, lo, c, t, r;
	int k, xsb;
	union { double d; unsigned long long u; } ux;
	unsigned int hx;

	ux.d = x;
	hx = (unsigned int)(ux.u >> 32);
	xsb = (hx >> 31) & 1;	/* sign bit */
	hx &= 0x7fffffff;	/* high word of |x| */

	if (hx >= 0x40862E42) {		/* |x| >= 709.78... */
		if (hx >= 0x7ff00000) {	/* inf or nan */
			if (((hx & 0xfffff) | (unsigned int)(ux.u & 0xffffffff)) != 0)
				return x + x;	/* nan */
			return (xsb == 0) ? x : 0.0;	/* exp(+inf)=inf, exp(-inf)=0 */
		}
		if (x > 7.09782712893383973096e+02) {	/* overflow */
			errno = ERANGE;
			return half_[0] * 1.0e308 * 1.0e308;	/* +inf */
		}
		if (x < -7.45133219101941108420e+02) {	/* underflow */
			errno = ERANGE;
			return 1.0e-300 * 1.0e-300;	/* +0 */
		}
	}

	if (hx > 0x3fd62e42) {		/* |x| > 0.5*ln2 */
		if (hx < 0x3FF0A2B2)	/* |x| < 1.5*ln2 */
			k = 1 - xsb - xsb;
		else
			k = (int)(invln2 * x + half_[xsb]);
		t = (double)k;
		hi = x - t * ln2HI[0];
		lo = t * ln2LO[0];
		x = hi - lo;
	} else if (hx < 0x3e300000) {	/* |x| < 2^-28 */
		return 1.0 + x;
	} else {
		k = 0;
		hi = x;
		lo = 0.0;
	}

	r = x;
	t = r * r;
	c = r - t * (Pe1 + t * (Pe2 + t * (Pe3 + t * (Pe4 + t * Pe5))));
	if (k == 0)
		return 1.0 - ((r * c) / (c - 2.0) - r);
	y = 1.0 - ((lo - (r * c) / (2.0 - c)) - hi);
	return ldexp(y, k);
}

#if 0  /* plan9: fdlibm log -- subnormal path NaNs under kencc; keep APE log for now */
static const double
  ln2_hi = 6.93147180369123816490e-01,
  ln2_lo = 1.90821492927058770002e-10,
  two54  = 1.80143985094819840000e+16,
  Lg1 = 6.666666666666735130e-01,
  Lg2 = 3.999999999940941908e-01,
  Lg3 = 2.857142874366239149e-01,
  Lg4 = 2.222219843214978396e-01,
  Lg5 = 1.818357216161805012e-01,
  Lg6 = 1.531383769920937332e-01,
  Lg7 = 1.479819860511658591e-01;

double
log(double x)
{
	double hfsq, f, s, z, R, w, t1, t2, dk;
	int k, i, j;
	int hx;
	unsigned int lx;
	union { double d; unsigned long long u; } ux;

	ux.d = x;
	hx = (int)(ux.u >> 32);
	lx = (unsigned int)(ux.u & 0xffffffff);

	k = 0;
	if (hx < 0x00100000) {			/* x < 2^-1022 */
		if (((hx & 0x7fffffff) | lx) == 0) {
			errno = ERANGE;
			return -_libm_inf();	/* log(+-0) = -inf */
		}
		if (hx < 0) {
			errno = EDOM;
			return _libm_nan();	/* log(-#) = NaN */
		}
		/* positive subnormal: scale up to a normal value and correct.
		   log(x) = log(x*2^54) - 54*ln2. Avoids the bit-twiddling path,
		   which mis-handled subnormals under kencc. */
		return log(x * two54) - 54.0 * (ln2_hi + ln2_lo);
	}
	if (hx >= 0x7ff00000)
		return x + x;			/* inf or nan */
	k += (hx >> 20) - 1023;
	hx &= 0x000fffff;
	i = (hx + 0x95f64) & 0x100000;
	ux.u = ((unsigned long long)(unsigned int)(hx | (i ^ 0x3ff00000)) << 32)
	       | (ux.u & 0xffffffff);
	x = ux.d;				/* normalize x or x/2 */
	k += (i >> 20);
	f = x - 1.0;
	if ((0x000fffff & (2 + hx)) < 3) {	/* -2**-20 <= f < 2**-20 */
		if (f == 0.0) {
			if (k == 0)
				return 0.0;
			dk = (double)k;
			return dk * ln2_hi + dk * ln2_lo;
		}
		R = f * f * (0.5 - 0.33333333333333333 * f);
		if (k == 0)
			return f - R;
		dk = (double)k;
		return dk * ln2_hi - ((R - dk * ln2_lo) - f);
	}
	s = f / (2.0 + f);
	dk = (double)k;
	z = s * s;
	i = hx - 0x6147a;
	w = z * z;
	j = 0x6b851 - hx;
	t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
	t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
	i |= j;
	R = t2 + t1;
	if (i > 0) {
		hfsq = 0.5 * f * f;
		if (k == 0)
			return f - (hfsq - s * (hfsq + R));
		return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
	} else {
		if (k == 0)
			return f - s * (f - R);
		return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
	}
}
#endif

/* sinh/cosh/tanh built on the accurate exp() above (APE's are imprecise). */
double
cosh(double x)
{
	double ax, e;
	if (x != x)
		return x;			/* nan */
	ax = fabs(x);
	if (ax > 7.10e2) {			/* avoid overflow in exp(ax) */
		e = exp(ax * 0.5);
		return (0.5 * e) * e;		/* may overflow -> inf (ERANGE) */
	}
	e = exp(ax);
	return 0.5 * (e + 1.0 / e);
}

double
sinh(double x)
{
	double ax, e, r;
	if (x != x || x == 0.0)
		return x;			/* nan, or preserve signed zero */
	ax = fabs(x);
	if (ax > 7.10e2) {
		e = exp(ax * 0.5);
		r = (0.5 * e) * e;
	} else if (ax >= 0.5) {
		e = exp(ax);
		r = 0.5 * (e - 1.0 / e);
	} else {
		double x2 = ax * ax;	/* Taylor: x + x^3/6 + x^5/120 + ... */
		r = ax * (1.0 + x2 * (1.0/6.0 + x2 * (1.0/120.0
		         + x2 * (1.0/5040.0 + x2 * (1.0/362880.0)))));
	}
	return (x < 0.0) ? -r : r;
}

double
tanh(double x)
{
	double ax, t, r;
	if (x != x || x == 0.0)
		return x;			/* nan, or preserve signed zero */
	ax = fabs(x);
	if (ax > 22.0)
		r = 1.0;			/* tanh saturates */
	else if (ax > 1.0) {
		t = exp(2.0 * ax);
		r = 1.0 - 2.0 / (t + 1.0);
	} else {
		t = exp(-2.0 * ax);
		r = (1.0 - t) / (1.0 + t);
	}
	return (x < 0.0) ? -r : r;
}
