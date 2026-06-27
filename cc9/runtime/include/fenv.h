#ifndef _FENV_H
#define _FENV_H
/* x86 floating-point environment. Exception flags match the x87 status word /
 * MXCSR low bits; rounding macros are the x87 control-word values (MXCSR stores
 * them shifted left 3). libc++'s <cfenv> re-exports these. */
#ifdef __cplusplus
extern "C" {
#endif
#define FE_INVALID   0x01
#define FE_DENORM    0x02            /* x86 extension; not in FE_ALL_EXCEPT */
#define FE_DIVBYZERO 0x04
#define FE_OVERFLOW  0x08
#define FE_UNDERFLOW 0x10
#define FE_INEXACT   0x20
#define FE_ALL_EXCEPT (FE_INVALID|FE_DIVBYZERO|FE_OVERFLOW|FE_UNDERFLOW|FE_INEXACT)

#define FE_TONEAREST  0x000
#define FE_DOWNWARD   0x400
#define FE_UPWARD     0x800
#define FE_TOWARDZERO 0xc00

/* fenv_t MUST be byte-for-byte identical to openlibm's (openlibm_fenv_amd64.h):
 * a 28-byte x87 environment (as stored by fnstenv) followed by the 32-bit MXCSR
 * at offset 28. openlibm's math routines (nearbyint/fma/...) call cc9's extern
 * fegetenv/feholdexcept and inline their own fesetenv against this exact layout;
 * any mismatch makes them ldmxcsr garbage and unmask the SSE exceptions, so a
 * later denormal/inexact op traps mid-test. */
typedef struct {
	struct { unsigned int __control, __status, __tag; char __other[16]; } __x87;
	unsigned int __mxcsr;
} fenv_t;
typedef unsigned short fexcept_t;
#define FE_DFL_ENV ((const fenv_t *)-1)

int feclearexcept(int);
int fetestexcept(int);
int feraiseexcept(int);
int fegetexceptflag(fexcept_t *, int);
int fesetexceptflag(const fexcept_t *, int);
int fegetround(void);
int fesetround(int);
int fegetenv(fenv_t *);
int fesetenv(const fenv_t *);
int feholdexcept(fenv_t *);
int feupdateenv(const fenv_t *);
#ifdef __cplusplus
}
#endif
#endif
