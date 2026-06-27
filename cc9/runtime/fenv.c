/* cc9 <fenv.h> implementation over the x86 MXCSR (SSE float/double) and x87
 * control/status words (long double). FP exceptions stay masked (cc9 runs with
 * a masked FP environment — see crt0 fpmask), so feraiseexcept only sets status
 * bits, it never traps. */
#include <fenv.h>

static unsigned int getmxcsr(void){ unsigned int v; __asm__ __volatile__("stmxcsr %0":"=m"(v)); return v; }
static void setmxcsr(unsigned int v){ __asm__ __volatile__("ldmxcsr %0"::"m"(v)); }
static unsigned short getcw(void){ unsigned short v; __asm__ __volatile__("fnstcw %0":"=m"(v)); return v; }
static void setcw(unsigned short v){ __asm__ __volatile__("fldcw %0"::"m"(v)); }
static unsigned short getsw(void){ unsigned short v; __asm__ __volatile__("fnstsw %0":"=m"(v)); return v; }

int feclearexcept(int e){
	e &= FE_ALL_EXCEPT;
	setmxcsr(getmxcsr() & ~(unsigned)e);
	/* read-modify-write the x87 status via the environment */
	struct { unsigned int cw, sw, tag, ip, cs, dp, ds; } env;
	__asm__ __volatile__("fnstenv %0":"=m"(env));
	env.sw &= ~(unsigned)e;
	__asm__ __volatile__("fldenv %0"::"m"(env));
	return 0;
}
int fetestexcept(int e){
	e &= FE_ALL_EXCEPT;
	return (int)((getmxcsr() | getsw()) & (unsigned)e);
}
int feraiseexcept(int e){
	e &= FE_ALL_EXCEPT;
	setmxcsr(getmxcsr() | (unsigned)e);
	return 0;
}
int fegetexceptflag(fexcept_t *f, int e){ *f = (fexcept_t)((getmxcsr()|getsw()) & (unsigned)(e & FE_ALL_EXCEPT)); return 0; }
int fesetexceptflag(const fexcept_t *f, int e){ feclearexcept(e); feraiseexcept(*f & e); return 0; }
int fegetround(void){ return (int)(((getmxcsr()>>13)&3)<<10); }
int fesetround(int r){
	unsigned mode = ((unsigned)r>>10)&3;
	setmxcsr((getmxcsr() & ~(3u<<13)) | (mode<<13));
	setcw((unsigned short)((getcw() & ~(3u<<10)) | (mode<<10)));
	return 0;
}
/* These four touch the fenv_t struct, so they must use the openlibm 28-byte-x87
 * + MXCSR@28 layout (see fenv.h). fnstenv/fldenv save/load the whole x87 env. */
int fegetenv(fenv_t *e){
	/* fnstenv stores the x87 env but masks all x87 exceptions as a side effect;
	 * reload the saved control word so this is a non-destructive read. */
	__asm__ __volatile__("fnstenv %0":"=m"(e->__x87));
	setcw((unsigned short)e->__x87.__control);
	e->__mxcsr = getmxcsr();
	return 0;
}
int fesetenv(const fenv_t *e){
	if(e == FE_DFL_ENV){ __asm__ __volatile__("fninit"); setcw(0x037f); setmxcsr(0x1f80); return 0; }
	__asm__ __volatile__("fldenv %0"::"m"(e->__x87)
		: "st","st(1)","st(2)","st(3)","st(4)","st(5)","st(6)","st(7)");
	setmxcsr(e->__mxcsr);
	return 0;
}
int feholdexcept(fenv_t *e){
	unsigned int mx = getmxcsr();
	__asm__ __volatile__("fnstenv %0":"=m"(e->__x87));   /* masks all x87 as a side effect */
	__asm__ __volatile__("fnclex");
	e->__mxcsr = mx;
	setmxcsr((mx & ~0x3fu) | 0x1f80u);   /* clear SSE status, mask all 6 SSE exceptions */
	return 0;
}
int feupdateenv(const fenv_t *e){
	unsigned int mx = getmxcsr(); unsigned short sw = getsw();
	fesetenv(e);
	feraiseexcept((int)((sw | mx) & FE_ALL_EXCEPT));
	return 0;
}
