#ifndef _MATH_H
#define _MATH_H
/* cc9 <math.h> — declares the full libm surface provided by libcc9m.a
 * (openlibm). The breadth matters: libc++'s <cmath> does `using ::fn` for each
 * of these, dropping any it can't find, so an absent declaration makes std::fn
 * silently unavailable (the original <complex> "atan2l unresolved" failure). */
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))
#define math_errhandling 0
#define FP_ILOGB0 (-2147483647-1)
#define FP_ILOGBNAN 2147483647
#define MATH_ERRNO 1
#define MATH_ERREXCEPT 2

/* C99 7.12: the evaluation-format types. FLT_EVAL_METHOD == 0 on x86_64 with
 * SSE math (cc9 always builds +sse,+sse2), so each maps to itself. libc++'s
 * <cmath> does `using ::float_t`, so their absence breaks std::float_t. */
#define __FLT_EVAL_METHOD__DEFINED 1
typedef float float_t;
typedef double double_t;

/* X/Open math constants. Not ISO C, but enough hosted code assumes <math.h>
 * provides them (SpiderMonkey, Mesa, OpenSSL) that leaving them out just moves
 * the same #define block into every port's shim. */
#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

/* classification — implemented as compiler builtins (no libm call needed) */
#define fpclassify(x) __builtin_fpclassify(FP_NAN,FP_INFINITE,FP_NORMAL,FP_SUBNORMAL,FP_ZERO,x)
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define isnormal(x)   __builtin_isnormal(x)
#define signbit(x)    __builtin_signbit(x)
#define isgreater(x,y)      __builtin_isgreater(x,y)
#define isgreaterequal(x,y) __builtin_isgreaterequal(x,y)
#define isless(x,y)         __builtin_isless(x,y)
#define islessequal(x,y)    __builtin_islessequal(x,y)
#define islessgreater(x,y)  __builtin_islessgreater(x,y)
#define isunordered(x,y)    __builtin_isunordered(x,y)

#ifdef __cplusplus
extern "C" {
#endif
/* The triad: double, float (f), long double (l). */
#define _CC9_M3(N) double N(double); float N##f(float); long double N##l(long double)
#define _CC9_M3_2(N) double N(double,double); float N##f(float,float); long double N##l(long double,long double)
_CC9_M3(sin); _CC9_M3(cos); _CC9_M3(tan);
_CC9_M3(asin); _CC9_M3(acos); _CC9_M3(atan);
_CC9_M3(sinh); _CC9_M3(cosh); _CC9_M3(tanh);
_CC9_M3(asinh); _CC9_M3(acosh); _CC9_M3(atanh);
_CC9_M3(exp); _CC9_M3(exp2); _CC9_M3(expm1);
_CC9_M3(log); _CC9_M3(log2); _CC9_M3(log10); _CC9_M3(log1p); _CC9_M3(logb);
_CC9_M3(sqrt); _CC9_M3(cbrt); _CC9_M3(fabs);
_CC9_M3(ceil); _CC9_M3(floor); _CC9_M3(trunc); _CC9_M3(round); _CC9_M3(nearbyint); _CC9_M3(rint);
_CC9_M3(erf); _CC9_M3(erfc); _CC9_M3(tgamma); _CC9_M3(lgamma);
double nan(const char *); float nanf(const char *); long double nanl(const char *);
_CC9_M3_2(atan2); _CC9_M3_2(pow); _CC9_M3_2(hypot); _CC9_M3_2(fmod);
_CC9_M3_2(copysign); _CC9_M3_2(nextafter); _CC9_M3_2(fdim); _CC9_M3_2(fmax); _CC9_M3_2(fmin);
double scalbn(double,int); float scalbnf(float,int); long double scalbnl(long double,int);
double scalbln(double,long); float scalblnf(float,long); long double scalblnl(long double,long);
double ldexp(double,int); float ldexpf(float,int); long double ldexpl(long double,int);
double frexp(double,int*); float frexpf(float,int*); long double frexpl(long double,int*);
double modf(double,double*); float modff(float,float*); long double modfl(long double,long double*);
double fma(double,double,double); float fmaf(float,float,float); long double fmal(long double,long double,long double);
double remainder(double,double); float remainderf(float,float); long double remainderl(long double,long double);
double remquo(double,double,int*); float remquof(float,float,int*); long double remquol(long double,long double,int*);
double nexttoward(double,long double); float nexttowardf(float,long double); long double nexttowardl(long double,long double);
int ilogb(double); int ilogbf(float); int ilogbl(long double);
long lround(double); long lroundf(float); long lroundl(long double);
long long llround(double); long long llroundf(float); long long llroundl(long double);
long lrint(double); long lrintf(float); long lrintl(long double);
long long llrint(double); long long llrintf(float); long long llrintl(long double);
double j0(double),j1(double),y0(double),y1(double);
int abs(int);
#undef _CC9_M3
#undef _CC9_M3_2
#ifdef __cplusplus
}
#endif
#endif
