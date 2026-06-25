/* cc9 complex-arithmetic builtins — clang lowers C99 _Complex *, / to these
 * (__mul?c3 / __div?c3); std::complex<T> operator*,/ go through them too.
 * Standard compiler-rt / C99 Annex G algorithm: Smith's method + inf/nan
 * recovery. Uses libm (libcc9m / openlibm) for logb/scalbn/fabs/fmax/copysign;
 * isnan/isinf/isfinite are the type-generic builtins from <math.h>. */
#include <math.h>

#define MULC3(NAME, T, FMAX, FABS, LOGB, SCALBN, COPYSIGN, INF)               \
T _Complex NAME(T a, T b, T c, T d) {                                          \
    T ac = a * c, bd = b * d, ad = a * d, bc = b * c;                          \
    T _Complex z;                                                             \
    __real__ z = ac - bd;                                                      \
    __imag__ z = ad + bc;                                                      \
    if (isnan(__real__ z) && isnan(__imag__ z)) {                             \
        int recalc = 0;                                                        \
        if (isinf(a) || isinf(b)) {                                            \
            a = COPYSIGN(isinf(a) ? (T)1 : (T)0, a);                          \
            b = COPYSIGN(isinf(b) ? (T)1 : (T)0, b);                          \
            if (isnan(c)) c = COPYSIGN((T)0, c);                              \
            if (isnan(d)) d = COPYSIGN((T)0, d);                              \
            recalc = 1; }                                                      \
        if (isinf(c) || isinf(d)) {                                            \
            c = COPYSIGN(isinf(c) ? (T)1 : (T)0, c);                          \
            d = COPYSIGN(isinf(d) ? (T)1 : (T)0, d);                          \
            if (isnan(a)) a = COPYSIGN((T)0, a);                              \
            if (isnan(b)) b = COPYSIGN((T)0, b);                              \
            recalc = 1; }                                                      \
        if (!recalc && (isinf(ac)||isinf(bd)||isinf(ad)||isinf(bc))) {         \
            if (isnan(a)) a = COPYSIGN((T)0, a);                              \
            if (isnan(b)) b = COPYSIGN((T)0, b);                              \
            if (isnan(c)) c = COPYSIGN((T)0, c);                              \
            if (isnan(d)) d = COPYSIGN((T)0, d);                              \
            recalc = 1; }                                                      \
        if (recalc) {                                                          \
            __real__ z = INF * (a * c - b * d);                               \
            __imag__ z = INF * (a * d + b * c); }                            \
    }                                                                          \
    return z;                                                                  \
}

#define DIVC3(NAME, T, FMAX, FABS, LOGB, SCALBN, COPYSIGN, INF)               \
T _Complex NAME(T a, T b, T c, T d) {                                          \
    int ilogbw = 0;                                                            \
    T logbw = LOGB(FMAX(FABS(c), FABS(d)));                                    \
    if (isfinite(logbw)) {                                                     \
        ilogbw = (int)logbw;                                                   \
        c = SCALBN(c, -ilogbw);                                                \
        d = SCALBN(d, -ilogbw); }                                             \
    T denom = c * c + d * d;                                                   \
    T _Complex z;                                                             \
    __real__ z = SCALBN((a * c + b * d) / denom, -ilogbw);                    \
    __imag__ z = SCALBN((b * c - a * d) / denom, -ilogbw);                    \
    if (isnan(__real__ z) && isnan(__imag__ z)) {                             \
        if ((denom == (T)0) && (!isnan(a) || !isnan(b))) {                    \
            __real__ z = COPYSIGN(INF, c) * a;                               \
            __imag__ z = COPYSIGN(INF, c) * b;                              \
        } else if ((isinf(a) || isinf(b)) && isfinite(c) && isfinite(d)) {    \
            a = COPYSIGN(isinf(a) ? (T)1 : (T)0, a);                          \
            b = COPYSIGN(isinf(b) ? (T)1 : (T)0, b);                          \
            __real__ z = INF * (a * c + b * d);                              \
            __imag__ z = INF * (b * c - a * d);                             \
        } else if (isinf(logbw) && logbw > (T)0 && isfinite(a) && isfinite(b)) {\
            c = COPYSIGN(isinf(c) ? (T)1 : (T)0, c);                          \
            d = COPYSIGN(isinf(d) ? (T)1 : (T)0, d);                          \
            __real__ z = (T)0 * (a * c + b * d);                             \
            __imag__ z = (T)0 * (b * c - a * d); }                          \
    }                                                                          \
    return z;                                                                  \
}

MULC3(__mulsc3, float,       fmaxf, fabsf, logbf, scalbnf, copysignf, __builtin_inff())
MULC3(__muldc3, double,      fmax,  fabs,  logb,  scalbn,  copysign,  __builtin_inf())
MULC3(__mulxc3, long double, fmaxl, fabsl, logbl, scalbnl, copysignl, __builtin_infl())
DIVC3(__divsc3, float,       fmaxf, fabsf, logbf, scalbnf, copysignf, __builtin_inff())
DIVC3(__divdc3, double,      fmax,  fabs,  logb,  scalbn,  copysign,  __builtin_inf())
DIVC3(__divxc3, long double, fmaxl, fabsl, logbl, scalbnl, copysignl, __builtin_infl())
