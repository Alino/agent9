#ifndef _MATH_H
#define _MATH_H
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
#ifdef __cplusplus
extern "C" {
#endif
double fabs(double); float fabsf(float); long double fabsl(long double);
double sqrt(double); float sqrtf(float); long double sqrtl(long double);
double sin(double),cos(double),tan(double),exp(double),log(double),log2(double),log10(double);
double pow(double,double),fmod(double,double),atan2(double,double),hypot(double,double);
double ceil(double),floor(double),trunc(double),round(double),nearbyint(double),rint(double);
double copysign(double,double),scalbn(double,int),frexp(double,int*),ldexp(double,int),modf(double,double*);
float sinf(float),cosf(float),powf(float,float),floorf(float),ceilf(float),copysignf(float,float);
double nextafter(double,double); float nextafterf(float,float);
int abs(int);
#ifdef __cplusplus
}
#endif
#endif
