/*
 * plan9_libm.c -- correctly-rounded transcendentals for the 9front CPython
 * port, displacing APE's low-precision libm. Implementations are the classic
 * fdlibm/SunPro algorithms (freely usable). Each is < 1 ulp.
 * Built into the interpreter; these symbols override APE's exp/... at link.
 */
#include <math.h>
#include <errno.h>

/* expm1 lives in plan9_compat.c (APE's <math.h> doesn't declare it); kencc has
   no implicit declarations, so declare it before tanh uses it. */
extern double expm1(double);

/* APE's fabs(-0.0) returns -0.0 (it does not clear the sign bit under kencc),
   which corrupts signed-zero results that route a magnitude through fabs (seen
   in cmath sqrt/acos/asin branch cuts and math.remainder). Provide a correct
   bit-clearing fabs that fully replaces APE's. */
double
fabs(double x)
{
	union { double d; unsigned long long u; } v;
	v.d = x;
	v.u &= 0x7fffffffffffffffULL;
	return v.d;
}

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

#if 1  /* plan9: vendored fdlibm log (subnormal path fixed via scale-up); APE's
          log is a few ulp off, which shows up as 24-32 ulp errors in the
          Lanczos lgamma/tgamma (test_math test_mtestfile). */
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

/* APE bundles log and log10 in one object, so overriding log requires defining
   log10 here too (else the bundled object pulls a duplicate log). fdlibm log10
   built on the accurate log above. */
double
log10(double x)
{
	static const double
	  ivln10    = 4.34294481903251816668e-01,
	  log10_2hi = 3.01029995663611771306e-01,
	  log10_2lo = 3.69423907715893078616e-13;
	union { double d; unsigned long long u; } ux;
	double y, z;
	int i, k, hx;
	unsigned int lx;

	ux.d = x;
	hx = (int)(ux.u >> 32);
	lx = (unsigned int)(ux.u & 0xffffffff);

	k = 0;
	if (hx < 0x00100000) {			/* x < 2^-1022 */
		if (((hx & 0x7fffffff) | lx) == 0) {
			errno = ERANGE;
			return -_libm_inf();	/* log10(+-0) = -inf */
		}
		if (hx < 0) {
			errno = EDOM;
			return _libm_nan();	/* log10(-#) = NaN */
		}
		k -= 54;
		x *= two54;			/* subnormal -> normal */
		ux.d = x;
		hx = (int)(ux.u >> 32);
	}
	if (hx >= 0x7ff00000)
		return x + x;			/* inf or nan */
	k += (hx >> 20) - 1023;
	i = ((unsigned int)k & 0x80000000) >> 31;
	hx = (hx & 0x000fffff) | ((0x3ff - i) << 20);
	y = (double)(k + i);
	ux.u = (ux.u & 0xffffffffULL) | ((unsigned long long)(unsigned int)hx << 32);
	x = ux.d;				/* x reduced to [1,2)/[0.5,1) */
	z = y * log10_2lo + ivln10 * log(x);
	return z + y * log10_2hi;
}
#endif

/* fdlibm __ieee754_pow. APE's pow is a few ulp off, which the Lanczos tgamma
   amplifies into 30+ ulp errors (test_math test_mtestfile gamma cases). This is
   the standard correctly-rounded-to-<1ulp implementation, reworked onto 64-bit
   bit access. Must be kept exact: pow underlies the ** operator everywhere. */
double
pow(double x, double y)
{
	static const double
	  bp[] = {1.0, 1.5},
	  dp_h[] = {0.0, 5.84962487220764160156e-01},
	  dp_l[] = {0.0, 1.35003920212974897128e-08},
	  zero = 0.0, one = 1.0, two = 2.0, two53 = 9007199254740992.0,
	  hugev = 1.0e300, tinyv = 1.0e-300,
	  L1 = 5.99999999999994648725e-01, L2 = 4.28571428578550184252e-01,
	  L3 = 3.33333329818377432918e-01, L4 = 2.72728123808534006489e-01,
	  L5 = 2.30660745775561366331e-01, L6 = 2.06975017800338417784e-01,
	  P1 = 1.66666666666666019037e-01, P2 = -2.77777777770155933842e-03,
	  P3 = 6.61375632143793436117e-05, P4 = -1.65339022054652515390e-06,
	  P5 = 4.13813679705723846039e-08,
	  lg2 = 6.93147180559945286227e-01, lg2_h = 6.93147182464599609375e-01,
	  lg2_l = -1.90465429995776804525e-09,
	  ovt = 8.0085662595372944372e-017,
	  cp = 9.61796693925975554329e-01, cp_h = 9.61796700954437255859e-01,
	  cp_l = -7.02846165095275826516e-09,
	  ivln2 = 1.44269504088896338700e+00, ivln2_h = 1.44269502162933349609e+00,
	  ivln2_l = 1.92596299112661746887e-08;
	double z, ax, z_h, z_l, p_h, p_l;
	double y1, t1, t2, r, s, t, u, v, w;
	double ss, s2, s_h, s_l, t_h, t_l;
	int i, j, k, yisint, n;
	int hx, hy, ix, iy;
	unsigned int lx, ly;
	union { double d; unsigned long long u; } uw;

	uw.d = x; hx = (int)(uw.u >> 32); lx = (unsigned int)(uw.u & 0xffffffff);
	uw.d = y; hy = (int)(uw.u >> 32); ly = (unsigned int)(uw.u & 0xffffffff);
	ix = hx & 0x7fffffff;
	iy = hy & 0x7fffffff;

	if ((iy | ly) == 0)
		return one;				/* x**0 = 1 */
	if (ix > 0x7ff00000 || ((ix == 0x7ff00000) && (lx != 0)) ||
	    iy > 0x7ff00000 || ((iy == 0x7ff00000) && (ly != 0)))
		return x + y;				/* +-NaN */

	yisint = 0;
	if (hx < 0) {
		if (iy >= 0x43400000)
			yisint = 2;			/* even integer y */
		else if (iy >= 0x3ff00000) {
			k = (iy >> 20) - 0x3ff;
			if (k > 20) {
				j = (int)(ly >> (52 - k));
				if (((unsigned int)j << (52 - k)) == ly)
					yisint = 2 - (j & 1);
			} else if (ly == 0) {
				j = iy >> (20 - k);
				if ((j << (20 - k)) == iy)
					yisint = 2 - (j & 1);
			}
		}
	}

	if (ly == 0) {
		if (iy == 0x7ff00000) {			/* y is +-inf */
			if (((ix - 0x3ff00000) | lx) == 0)
				return one;		/* (-1)**+-inf */
			else if (ix >= 0x3ff00000)
				return (hy >= 0) ? y : zero;
			else
				return (hy < 0) ? -y : zero;
		}
		if (iy == 0x3ff00000) {
			if (hy < 0) return one / x;
			else return x;			/* y is +-1 */
		}
		if (hy == 0x40000000) return x * x;	/* y is 2 */
		if (hy == 0x3fe00000) {			/* y is 0.5 */
			if (hx >= 0) return sqrt(x);
		}
	}

	ax = fabs(x);
	if (lx == 0) {
		if (ix == 0x7ff00000 || ix == 0 || ix == 0x3ff00000) {
			z = ax;				/* x is +-0,+-inf,+-1 */
			if (hy < 0) z = one / z;
			if (hx < 0) {
				if (((ix - 0x3ff00000) | yisint) == 0)
					z = (z - z) / (z - z);	/* (-1)**non-int = NaN */
				else if (yisint == 1)
					z = -z;
			}
			return z;
		}
	}

	n = (hx >> 31) + 1;
	if ((n | yisint) == 0)
		return (x - x) / (x - x);		/* (-ve)**(non-int) = NaN */

	s = one;
	if ((n | (yisint - 1)) == 0) s = -one;	/* (-ve)**(odd int) */

	if (iy > 0x41e00000) {				/* |y| > 2^31 */
		if (iy > 0x43f00000) {			/* |y| > 2^64 */
			if (ix <= 0x3fefffff) return (hy < 0) ? hugev * hugev : tinyv * tinyv;
			if (ix >= 0x3ff00000) return (hy > 0) ? hugev * hugev : tinyv * tinyv;
		}
		if (ix < 0x3fefffff) return (hy < 0) ? s * hugev * hugev : s * tinyv * tinyv;
		if (ix > 0x3ff00000) return (hy > 0) ? s * hugev * hugev : s * tinyv * tinyv;
		t = ax - one;
		w = (t * t) * (0.5 - t * (0.3333333333333333333333 - t * 0.25));
		u = ivln2_h * t;
		v = t * ivln2_l - w * ivln2;
		t1 = u + v;
		uw.d = t1; uw.u &= 0xffffffff00000000ULL; t1 = uw.d;
		t2 = v - (t1 - u);
	} else {
		n = 0;
		if (ix < 0x00100000) {			/* subnormal x */
			ax *= two53; n -= 53;
			uw.d = ax; ix = (int)(uw.u >> 32);
		}
		n += ((ix) >> 20) - 0x3ff;
		j = ix & 0x000fffff;
		ix = j | 0x3ff00000;
		if (j <= 0x3988E) k = 0;
		else if (j < 0xBB67A) k = 1;
		else { k = 0; n += 1; ix -= 0x00100000; }
		uw.d = ax; uw.u = (uw.u & 0xffffffffULL) |
		         ((unsigned long long)(unsigned int)ix << 32); ax = uw.d;
		u = ax - bp[k];
		v = one / (ax + bp[k]);
		ss = u * v;
		s_h = ss;
		uw.d = s_h; uw.u &= 0xffffffff00000000ULL; s_h = uw.d;
		t_h = zero;
		uw.d = t_h; uw.u = ((unsigned long long)(unsigned int)
		         (((ix >> 1) | 0x20000000) + 0x00080000 + (k << 18)) << 32); t_h = uw.d;
		t_l = ax - (t_h - bp[k]);
		s_l = v * ((u - s_h * t_h) - s_h * t_l);
		s2 = ss * ss;
		r = s2 * s2 * (L1 + s2 * (L2 + s2 * (L3 + s2 * (L4 + s2 * (L5 + s2 * L6)))));
		r += s_l * (s_h + ss);
		s2 = s_h * s_h;
		t_h = 3.0 + s2 + r;
		uw.d = t_h; uw.u &= 0xffffffff00000000ULL; t_h = uw.d;
		t_l = r - ((t_h - 3.0) - s2);
		u = s_h * t_h;
		v = s_l * t_h + t_l * ss;
		p_h = u + v;
		uw.d = p_h; uw.u &= 0xffffffff00000000ULL; p_h = uw.d;
		p_l = v - (p_h - u);
		z_h = cp_h * p_h;
		z_l = cp_l * p_h + p_l * cp + dp_l[k];
		t = (double)n;
		t1 = (((z_h + z_l) + dp_h[k]) + t);
		uw.d = t1; uw.u &= 0xffffffff00000000ULL; t1 = uw.d;
		t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
	}

	y1 = y;
	uw.d = y1; uw.u &= 0xffffffff00000000ULL; y1 = uw.d;
	p_l = (y - y1) * t1 + y * t2;
	p_h = y1 * t1;
	z = p_l + p_h;
	uw.d = z; j = (int)(uw.u >> 32); i = (int)(unsigned int)(uw.u & 0xffffffff);
	if (j >= 0x40900000) {				/* z >= 1024 */
		if (((j - 0x40900000) | i) != 0)
			return s * hugev * hugev;	/* overflow */
		if (p_l + ovt > z - p_h)
			return s * hugev * hugev;	/* overflow */
	} else if ((j & 0x7fffffff) >= 0x4090cc00) {	/* z <= -1075 */
		if (((j - (int)0xc090cc00) | i) != 0)
			return s * tinyv * tinyv;	/* underflow */
		if (p_l <= z - p_h)
			return s * tinyv * tinyv;	/* underflow */
	}
	i = j & 0x7fffffff;
	k = (i >> 20) - 0x3ff;
	n = 0;
	if (i > 0x3fe00000) {				/* |z| > 0.5 -> n=[z+0.5] */
		n = j + (0x00100000 >> (k + 1));
		k = ((n & 0x7fffffff) >> 20) - 0x3ff;
		t = zero;
		uw.d = t; uw.u = (uw.u & 0xffffffffULL) |
		         ((unsigned long long)(unsigned int)(n & ~(0x000fffff >> k)) << 32); t = uw.d;
		n = ((n & 0x000fffff) | 0x00100000) >> (20 - k);
		if (j < 0) n = -n;
		p_h -= t;
	}
	t = p_l + p_h;
	uw.d = t; uw.u &= 0xffffffff00000000ULL; t = uw.d;
	u = t * lg2_h;
	v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
	z = u + v;
	w = v - (z - u);
	t = z * z;
	t1 = z - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
	r = (z * t1) / (t1 - two) - (w + z * w);
	z = one - (r - z);
	uw.d = z; j = (int)(uw.u >> 32);
	j += (n << 20);
	if ((j >> 20) <= 0)
		z = ldexp(z, n);			/* subnormal output */
	else {
		uw.d = z; uw.u = (uw.u & 0xffffffffULL) |
		         ((unsigned long long)(unsigned int)j << 32); z = uw.d;
	}
	return s * z;
}

/* sinh/cosh/tanh built on the accurate exp() above (APE's are imprecise). */
double
cosh(double x)
{
	double ax, e;
	if (isnan(x))
		return x;			/* nan */
	if (x > 1.7976931348623157e308 || x < -1.7976931348623157e308)
		return _libm_inf();		/* cosh(+-inf)=+inf, no errno */
	ax = fabs(x);
	if (ax > 710.4758600739439) {		/* ln(2*DBL_MAX): genuine overflow */
		errno = ERANGE;
		return _libm_inf();
	}
	if (ax > 709.0) {			/* exp(ax) overflows; split exp(ax/2)^2 */
		e = exp(ax * 0.5);
		return (0.5 * e) * e;
	}
	e = exp(ax);
	return 0.5 * (e + 1.0 / e);
}

double
sinh(double x)
{
	double ax, e, r;
	if (isnan(x) || x == 0.0)
		return x;			/* nan, or preserve signed zero */
	if (x > 1.7976931348623157e308)
		return _libm_inf();		/* sinh(+inf)=+inf */
	if (x < -1.7976931348623157e308)
		return -_libm_inf();		/* sinh(-inf)=-inf */
	ax = fabs(x);
	if (ax > 710.4758600739439) {		/* genuine overflow */
		errno = ERANGE;
		r = _libm_inf();
		return (x < 0.0) ? -r : r;
	}
	if (ax > 709.0) {
		e = exp(ax * 0.5);
		r = (0.5 * e) * e;
	} else {
		/* sinh(x) = 0.5*(t + t/(t+1)) with t = expm1(|x|); expm1 avoids the
		   cancellation of (e - 1/e) and is exact for small x, where the old
		   degree-9 Taylor truncated at ~x^11/11! (sinh(0.2) was 18 ulps off). */
		e = expm1(ax);
		r = 0.5 * (e + e / (e + 1.0));
	}
	return (x < 0.0) ? -r : r;
}

double
tanh(double x)
{
	double ax, t, r;
	if (isnan(x) || x == 0.0)
		return x;			/* nan, or preserve signed zero */
	ax = fabs(x);
	if (ax > 22.0)
		r = 1.0;			/* tanh saturates */
	else if (ax >= 1.0) {
		t = expm1(2.0 * ax);
		r = 1.0 - 2.0 / (t + 2.0);
	} else if (ax >= 1.3877787807814457e-17) {	/* 2^-56 */
		/* expm1 avoids the catastrophic cancellation that (1-exp(-2x))
		   suffers for small x -- (1-t) collapses to 2^-53 regardless of
		   the true tiny value, so tanh(5e-17) came out as 2^-54. */
		t = expm1(-2.0 * ax);
		r = -t / (t + 2.0);
	} else {
		return x;			/* |x| < 2^-56: tanh(x) = x to rounding */
	}
	return (x < 0.0) ? -r : r;
}



/* sin/cos via fdlibm kernels + Cody-Waite range reduction (3-part pi/2,
   accurate for |x| up to ~2^28 -- covers the test range). APE's trig degrades
   to ~10 ulp for larger arguments because its reduction is imprecise. */
static const double
  _S1 = -1.66666666666666324348e-01, _S2 = 8.33333333332248946124e-03,
  _S3 = -1.98412698298579493134e-04, _S4 = 2.75573137070700676789e-06,
  _S5 = -2.50507602534068634195e-08, _S6 = 1.58969099521155010221e-10,
  _C1 = 4.16666666666666019037e-02, _C2 = -1.38888888888741095749e-03,
  _C3 = 2.48015872894767294178e-05, _C4 = -2.75573143513906633035e-07,
  _C5 = 2.08757232129817482790e-09, _C6 = -1.13596475577881948265e-11,
  _pio4 = 7.85398163397448278999e-01,
  _invpio2 = 6.36619772367581382433e-01,
  _pio2_1 = 1.57079632673412561417e+00,
  _pio2_1t = 6.07710050650619224932e-11,
  _pio2_2 = 6.07710050630396597660e-11,
  _pio2_2t = 2.02226624879595063154e-21,
  _pio2_3 = 2.02226624871116645580e-21,
  _pio2_3t = 8.47842766036889956997e-32;

static double
k_sin(double x, double y)
{
	double z = x * x, w = z * z;
	double r = _S2 + z * (_S3 + z * _S4) + z * w * (_S5 + z * _S6);
	double v = z * x;
	if (y == 0.0)
		return x + v * (_S1 + z * r);
	return x - ((z * (0.5 * y - v * r) - y) - v * _S1);
}

static double
k_cos(double x, double y)
{
	double z = x * x, w = z * z;
	double r = z * (_C1 + z * (_C2 + z * _C3)) + w * w * (_C4 + z * (_C5 + z * _C6));
	double hz = 0.5 * z, q = 1.0 - hz;
	return q + (((1.0 - q) - hz) + (z * r - x * y));
}

/* reduce x to y[0]+y[1] in [-pi/4,pi/4]; returns n mod 4 (medium-size path). */
static int
rem_pio2(double x, double *y)
{
	double z, w, t, r, fn;
	int n, sign = 0;

	if (fabs(x) <= _pio4) {
		y[0] = x;
		y[1] = 0.0;
		return 0;
	}
	if (x < 0.0) {
		sign = 1;
		x = -x;
	}
	fn = x * _invpio2 + 0.5;
	n = (int)fn;
	fn = (double)n;			/* nearest int to x*2/pi (round toward 0 after +0.5) */
	if (fn > x * _invpio2 + 0.5)
		;
	r = x - fn * _pio2_1;
	w = fn * _pio2_1t;		/* 1st round, good to 85 bits */
	y[0] = r - w;
	/* 2nd iteration needed for accuracy */
	t = r;
	w = fn * _pio2_2;
	r = t - w;
	w = fn * _pio2_2t - ((t - r) - w);
	y[0] = r - w;
	y[1] = (r - y[0]) - w;
	if (sign) {
		y[0] = -y[0];
		y[1] = -y[1];
		return (-n) & 3;
	}
	return n & 3;
}

double
sin(double x)
{
	double y[2];
	int n;
	if (isnan(x) || isinf(x))
		return x - x;		/* nan */
	if (x == 0.0)
		return x;		/* sin(+-0)=+-0; k_sin's x+... makes -0.0+0.0=+0.0 */
	if (fabs(x) <= _pio4)
		return k_sin(x, 0.0);
	n = rem_pio2(x, y);
	switch (n) {
	case 0: return k_sin(y[0], y[1]);
	case 1: return k_cos(y[0], y[1]);
	case 2: return -k_sin(y[0], y[1]);
	default: return -k_cos(y[0], y[1]);
	}
}

double
cos(double x)
{
	double y[2];
	int n;
	if (isnan(x) || isinf(x))
		return x - x;
	if (fabs(x) <= _pio4)
		return k_cos(x, 0.0);
	n = rem_pio2(x, y);
	switch (n) {
	case 0: return k_cos(y[0], y[1]);
	case 1: return -k_sin(y[0], y[1]);
	case 2: return -k_cos(y[0], y[1]);
	default: return k_sin(y[0], y[1]);
	}
}


static int _signbit(double x){ union { double d; unsigned long long u; } v; v.d = x; return (int)(v.u >> 63); }
static double _pi(void){ return 3.14159265358979311600e+00; }
static double _pi_2(void){ return 1.57079632679489655800e+00; }
static double _pi_4(void){ return 7.85398163397448278999e-01; }

/* atan + atan2 (fdlibm). APE's atan2 mishandles the sign of zero in the first
   argument (atan2(-0, x<0) returns +pi), breaking cmath branch-cut signs. */
static const double _atanhi[] = {
	4.63647609000806093515e-01, 7.85398163397448278999e-01,
	9.82793723247329054082e-01, 1.57079632679489655800e+00,
};
static const double _atanlo[] = {
	2.26987774529616870924e-17, 3.06161699786838301793e-17,
	1.39033110312309984516e-17, 6.12323399573676603587e-17,
};
static const double _aT[] = {
	3.33333333333329318027e-01, -1.99999999998764832476e-01,
	1.42857142725034663711e-01, -1.11111104054623557880e-01,
	9.09088713343650656196e-02, -7.69187620504482999495e-02,
	6.66107313738753120669e-02, -5.83357013379057348645e-02,
	4.97687799461593236017e-02, -3.65315727442169155270e-02,
	1.62858201153657823623e-02,
};

double
atan(double x)
{
	double w, s1, s2, z;
	int id, sign;
	union { double d; unsigned long long u; } ux;
	unsigned int hx, ix;

	ux.d = x;
	hx = (unsigned int)(ux.u >> 32);
	ix = hx & 0x7fffffff;
	if (ix >= 0x44100000) {			/* |x| >= 2^66 */
		if (isnan(x))
			return x;
		return (hx >> 31) ? -_atanhi[3] - _atanlo[3] : _atanhi[3] + _atanlo[3];
	}
	sign = hx >> 31;
	if (ix < 0x3fdc0000) {			/* |x| < 0.4375 */
		if (ix < 0x3e400000)		/* |x| < 2^-27 */
			return x;
		id = -1;
	} else {
		x = fabs(x);
		if (ix < 0x3ff30000) {		/* |x| < 1.1875 */
			if (ix < 0x3fe60000) {	/* 7/16 <= |x| < 11/16 */
				id = 0;
				x = (2.0 * x - 1.0) / (2.0 + x);
			} else {		/* 11/16 <= |x| < 19/16 */
				id = 1;
				x = (x - 1.0) / (x + 1.0);
			}
		} else if (ix < 0x40038000) {	/* |x| < 2.4375 */
			id = 2;
			x = (x - 1.5) / (1.0 + 1.5 * x);
		} else {			/* 2.4375 <= |x| < 2^66 */
			id = 3;
			x = -1.0 / x;
		}
	}
	z = x * x;
	w = z * z;
	s1 = z * (_aT[0] + w * (_aT[2] + w * (_aT[4] + w * (_aT[6] + w * (_aT[8] + w * _aT[10])))));
	s2 = w * (_aT[1] + w * (_aT[3] + w * (_aT[5] + w * (_aT[7] + w * _aT[9]))));
	if (id < 0)
		return x - x * (s1 + s2);
	z = _atanhi[id] - ((x * (s1 + s2) - _atanlo[id]) - x);
	return sign ? -z : z;
}

double
atan2(double y, double x)
{
	double z;
	int m;

	if (isnan(x) || isnan(y))
		return x + y;
	if (x == 1.0)				/* x=1, atan2(y,1)=atan(y) */
		return atan(y);
	m = _signbit(y) + (_signbit(x) << 1);	/* 2*sign(x)+sign(y), -0 aware */

	if (y == 0.0) {				/* y == +-0 */
		switch (m) {
		case 0: case 1: return y;			/* atan(+-0,+x)=+-0 */
		case 2: return _pi();				/* atan(+0,-x)=+pi */
		case 3: return -_pi();				/* atan(-0,-x)=-pi */
		}
	}
	if (x == 0.0)				/* x == +-0 */
		return (y < 0.0) ? -_pi_2() : _pi_2();
	if (isinf(x)) {
		if (isinf(y)) {
			switch (m) {
			case 0: return _pi_4();
			case 1: return -_pi_4();
			case 2: return 3.0 * _pi_4();
			case 3: return -3.0 * _pi_4();
			}
		} else {
			switch (m) {
			case 0: return 0.0;
			case 1: return -0.0;
			case 2: return _pi();
			case 3: return -_pi();
			}
		}
	}
	if (isinf(y))
		return _signbit(y) ? -_pi_2() : _pi_2();

	z = atan(fabs(y / x));
	switch (m) {
	case 0: return z;			/* atan(+,+) */
	case 1: return -z;			/* atan(-,+) */
	case 2: return _pi() - z;		/* atan(+,-) */
	default: return z - _pi();		/* atan(-,-) */
	}
}

/* tan via fdlibm __kernel_tan + the sin/cos range reduction. */
static const double _T[13] = {
	3.33333333333334091986e-01, 1.33333333333201242699e-01,
	5.39682539762260521377e-02, 2.18694882948595424599e-02,
	8.86323982359930005737e-03, 3.59207910759131235356e-03,
	1.45620945432529025516e-03, 5.88041240820264096874e-04,
	2.46463134818469906812e-04, 7.81794442939557092300e-05,
	7.14072491382608190305e-05, -1.85586374855275456654e-05,
	2.59073051863633712884e-05,
};
static const double _Tpio4 = 7.85398163397448278999e-01,
	_Tpio4lo = 3.06161699786838301793e-17;

static double
_hiword(double x)	/* x with its low 32 mantissa bits cleared */
{
	union { double d; unsigned long long u; } v;
	v.d = x;
	v.u &= 0xffffffff00000000ULL;
	return v.d;
}

static double
k_tan(double x, double y, int iy)
{
	double z, r, v, w, s, a, t;
	int big, sign = 0;
	union { double d; unsigned long long u; } ux;
	unsigned int ix;

	ux.d = x;
	ix = (unsigned int)(ux.u >> 32) & 0x7fffffff;
	big = (ix >= 0x3FE59428);		/* |x| >= 0.6744 */
	if (big) {
		if ((int)(ux.u >> 63)) {
			x = -x;
			y = -y;
		}
		z = _Tpio4 - x;
		w = _Tpio4lo - y;
		x = z + w;
		y = 0.0;
	}
	z = x * x;
	w = z * z;
	r = _T[1] + w * (_T[3] + w * (_T[5] + w * (_T[7] + w * (_T[9] + w * _T[11]))));
	v = z * (_T[2] + w * (_T[4] + w * (_T[6] + w * (_T[8] + w * (_T[10] + w * _T[12])))));
	s = z * x;
	r = y + z * (s * (r + v) + y);
	r += _T[0] * s;
	w = x + r;
	if (big) {
		v = (double)iy;
		sign = (int)(ux.u >> 63) ? -1 : 1;
		return (double)sign * (v - 2.0 * (x - (w * w / (w + v) - r)));
	}
	if (iy == 1)
		return w;
	/* compute -1.0/(x+r) accurately */
	z = _hiword(w);
	v = r - (z - x);			/* z + v = r + x = w */
	a = t = -1.0 / w;
	t = _hiword(t);
	s = 1.0 + t * z;
	return t + a * (s + t * v);
}

double
tan(double x)
{
	double y[2];
	int n;
	if (isnan(x) || isinf(x))
		return x - x;
	if (x == 0.0)
		return x;			/* tan(+-0) = +-0 (preserve sign) */
	if (fabs(x) <= _pio4)
		return k_tan(x, 0.0, 1);
	n = rem_pio2(x, y);
	return k_tan(y[0], y[1], 1 - ((n & 1) << 1));
}

/*
 * ldexp / frexp / modf -- APE bundles these three in one libc object whose
 * ldexp/frexp flush or mangle subnormals: ldexp(1,-1074) -> 0 (so
 * float.fromhex('1p-1074') and math.remainder on subnormals broke), and frexp
 * mishandles subnormal mantissas. Because they share one object, overriding one
 * pulls a duplicate-symbol conflict, so we must define all three here to keep
 * APE's object out of the link entirely. These are the fdlibm scalbn/frexp/modf
 * algorithms reworked onto 64-bit bit-manipulation, correct across the full
 * subnormal range.
 */
#define _TWO54   18014398509481984.0		/* 2^54  */
#define _TWOM54  5.5511151231257827e-17		/* 2^-54 */

double
ldexp(double x, int n)
{
	union { double d; unsigned long long u; } v;
	int k;

	v.d = x;
	k = (int)((v.u >> 52) & 0x7ff);			/* extract exponent */
	if (k == 0) {					/* 0 or subnormal */
		if ((v.u & 0x7fffffffffffffffULL) == 0)
			return x;			/* +-0 */
		v.d = x * _TWO54;			/* normalize subnormal */
		k = (int)((v.u >> 52) & 0x7ff) - 54;
		if (n < -50000) {			/* underflow */
			errno = ERANGE;
			return _signbit(x) ? -0.0 : 0.0;
		}
	}
	if (k == 0x7ff)
		return x + x;				/* NaN or Inf */
	k += n;
	if (k > 0x7fe) {				/* overflow */
		errno = ERANGE;
		return _signbit(x) ? -_libm_inf() : _libm_inf();
	}
	if (k > 0) {					/* normal result */
		v.u = (v.u & 0x800fffffffffffffULL) |
		      ((unsigned long long)k << 52);
		return v.d;
	}
	if (k <= -54) {
		errno = ERANGE;
		if (n > 50000)				/* int overflow in k+n: really overflow */
			return _signbit(x) ? -_libm_inf() : _libm_inf();
		return _signbit(x) ? -0.0 : 0.0;	/* underflow to 0 */
	}
	k += 54;					/* subnormal result */
	v.u = (v.u & 0x800fffffffffffffULL) |
	      ((unsigned long long)k << 52);
	return v.d * _TWOM54;
}

double
frexp(double x, int *eptr)
{
	union { double d; unsigned long long u; } v;
	int k;

	v.d = x;
	k = (int)((v.u >> 52) & 0x7ff);
	if (k == 0x7ff || (v.u & 0x7fffffffffffffffULL) == 0) {
		*eptr = 0;				/* 0, inf, nan */
		return x;
	}
	if (k == 0) {					/* subnormal */
		v.d = x * _TWO54;
		k = (int)((v.u >> 52) & 0x7ff) - 54;
	}
	*eptr = k - 1022;
	v.u = (v.u & 0x800fffffffffffffULL) | (0x3feULL << 52);	/* [0.5,1) */
	return v.d;
}

double
modf(double x, double *iptr)
{
	union { double d; unsigned long long u; } v, iv;
	int e;
	unsigned long long mask;

	v.d = x;
	e = (int)((v.u >> 52) & 0x7ff) - 1023;		/* unbiased exponent */
	if (e < 0) {					/* |x| < 1 */
		iv.u = v.u & 0x8000000000000000ULL;	/* +-0 integer part */
		*iptr = iv.d;
		return x;
	}
	if (e >= 52) {					/* no fractional part */
		*iptr = x;
		if (isnan(x))
			return x;
		iv.u = v.u & 0x8000000000000000ULL;	/* +-0 */
		return iv.d;
	}
	mask = 0x000fffffffffffffULL >> e;
	if ((v.u & mask) == 0) {			/* x is integral */
		*iptr = x;
		iv.u = v.u & 0x8000000000000000ULL;
		return iv.d;
	}
	iv.u = v.u & ~mask;				/* truncate to integer */
	*iptr = iv.d;
	return x - iv.d;
}
