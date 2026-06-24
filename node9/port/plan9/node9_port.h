#ifndef NODE9_PORT_H
#define NODE9_PORT_H
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
/* kencc: neutralize GCC attribute syntax (both spellings) */
#define __attribute__(x)
#define __attribute(x)
/* kencc/APE has no alloca: SMOKE shim (LEAKS) — replace with heap+free in Phase 1
   proper. Routed through a wrapper because quickjs.c later poisons `malloc`. */
static void *n9_alloca(unsigned long n){ return malloc(n); }
#define alloca(n) n9_alloca(n)
/* C99 math missing from APE */
#define scalbn(x,n) ldexp((x),(n))
static double n9_inf(void){ unsigned long long b=0x7ff0000000000000ULL; double d; memcpy(&d,&b,8); return d; }
static double n9_nan(void){ unsigned long long b=0x7ff8000000000000ULL; double d; memcpy(&d,&b,8); return d; }
static double n9_copysign(double x,double y){ unsigned long long bx,by; memcpy(&bx,&x,8); memcpy(&by,&y,8); bx=(bx&0x7fffffffffffffffULL)|(by&0x8000000000000000ULL); memcpy(&x,&bx,8); return x; }
#define copysign(x,y) n9_copysign((x),(y))
#ifndef INFINITY
#define INFINITY n9_inf()
#endif
#ifndef NAN
#define NAN n9_nan()
#endif
/* C99 math functions absent from APE (kencc). SMOKE-grade accuracy; tighten later.
   Present in APE (do NOT shim): isnan isinf cosh sinh tanh log2 hypot fmin fmod. */
static double n9_round(double x){ return x<0 ? ceil(x-0.5) : floor(x+0.5); }
static double n9_trunc(double x){ return x<0 ? ceil(x) : floor(x); }
static double n9_rint(double x){ return floor(x+0.5); }
static int n9_isfinite(double x){ unsigned long long b; memcpy(&b,&x,8); return ((b>>52)&0x7ffULL)!=0x7ffULL; }
static int n9_signbit(double x){ unsigned long long b; memcpy(&b,&x,8); return (int)(b>>63); }
static double n9_acosh(double x){ return log(x+sqrt(x*x-1.0)); }
static double n9_asinh(double x){ return log(x+sqrt(x*x+1.0)); }
static double n9_atanh(double x){ return 0.5*log((1.0+x)/(1.0-x)); }
static double n9_expm1(double x){ return exp(x)-1.0; }
static double n9_log1p(double x){ return log(1.0+x); }
static double n9_exp2(double x){ return pow(2.0,x); }
static double n9_cbrt(double x){ double r=pow(fabs(x),1.0/3.0); return x<0?-r:r; }
static double n9_fmax(double x,double y){ return x>=y?x:y; }
static double n9_remainder(double x,double y){ return x-n9_rint(x/y)*y; }
#define round(x)     n9_round(x)
#define trunc(x)     n9_trunc(x)
#define rint(x)      n9_rint(x)
#define nearbyint(x) n9_rint(x)
#define lrint(x)     ((long)n9_rint(x))
#define isfinite(x)  n9_isfinite(x)
#define signbit(x)   n9_signbit(x)
#define acosh(x)     n9_acosh(x)
#define asinh(x)     n9_asinh(x)
#define atanh(x)     n9_atanh(x)
#define expm1(x)     n9_expm1(x)
#define log1p(x)     n9_log1p(x)
#define exp2(x)      n9_exp2(x)
#define cbrt(x)      n9_cbrt(x)
#define fmax(x,y)    n9_fmax((x),(y))
#define remainder(x,y) n9_remainder((x),(y))
/* APE has localtime/gmtime but not the _r reentrant forms */
static struct tm *n9_localtime_r(const long *t, struct tm *r){ *r=*localtime((const time_t*)t); return r; }
static struct tm *n9_gmtime_r(const long *t, struct tm *r){ *r=*gmtime((const time_t*)t); return r; }
#define localtime_r(t,r) n9_localtime_r((const long*)(t),(r))
#define gmtime_r(t,r)    n9_gmtime_r((const long*)(t),(r))
/* POSIX bits absent from APE, needed by quickjs-libc.c (harmless in other TUs) */
typedef void (*sighandler_t)(int);
static char *n9_realpath(const char *p, char *r){ if(r){ strcpy(r,p); return r; } return strdup(p); }
#define realpath(p,r) n9_realpath((p),(r))
/* js_once: cutils.h only defines it under JS_HAVE_THREADS (we disabled threads), but
   quickjs-libc.c's os.exec uses it at file scope. Provide a threadless guard. */
typedef int js_once_t;
#ifndef JS_ONCE_INIT
#define JS_ONCE_INIT 0
#endif
static void js_once(js_once_t *g, void (*cb)(void)){ if(!*g){ *g=1; cb(); } }
/* env: APE lacks setenv/unsetenv and doesn't declare environ for generic platforms.
   SMOKE: env mutation is a no-op for now (os.setenv won't persist). */
extern char **environ;
static int n9_setenv(const char *n, const char *v, int o){ (void)n;(void)v;(void)o; return 0; }
static int n9_unsetenv(const char *n){ (void)n; return 0; }
#define setenv(n,v,o) n9_setenv((n),(v),(o))
#define unsetenv(n)   n9_unsetenv(n)
/* tty window size (os.ttyGetWinSize) — not in APE; the ioctl fails at runtime */
#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#endif
#ifndef RUSAGE_SELF
#define RUSAGE_SELF 0
#endif
/* more APE-absent POSIX (os.* features degrade): getrusage, mkstemp, mkdtemp */
static int n9_getrusage(int who, void *r){ (void)who;(void)r; return -1; }
#define getrusage(w,r) n9_getrusage((int)(w),(void*)(r))
static int n9_mkstemp(char *t){ (void)t; return -1; }
static char *n9_mkdtemp(char *t){ (void)t; return 0; }
#define mkstemp(t) n9_mkstemp(t)
#define mkdtemp(t) n9_mkdtemp(t)
static int n9_utimes(const char *p, const void *t){ (void)p;(void)t; return -1; }
#define utimes(p,t) n9_utimes((p),(const void*)(t))
static int n9_setgroups(long n, const void *g){ (void)n;(void)g; return -1; }
#define setgroups(n,g) n9_setgroups((long)(n),(const void*)(g))
/* kencc has no GCC builtins */
static int n9_clz32(unsigned x){ int n=0; if(!x)return 32; while(!(x&0x80000000U)){x<<=1;n++;} return n; }
static int n9_clz64(unsigned long long x){ int n=0; if(!x)return 64; while(!(x&0x8000000000000000ULL)){x<<=1;n++;} return n; }
static int n9_ctz32(unsigned x){ int n=0; if(!x)return 32; while(!(x&1U)){x>>=1;n++;} return n; }
static int n9_ctz64(unsigned long long x){ int n=0; if(!x)return 64; while(!(x&1ULL)){x>>=1;n++;} return n; }
#define __builtin_clz(x)   n9_clz32(x)
#define __builtin_clzll(x) n9_clz64(x)
#define __builtin_ctz(x)   n9_ctz32(x)
#define __builtin_ctzll(x) n9_ctz64(x)
#define __builtin_expect(x,c) (x)
#define __builtin_frame_address(n) ((void*)0)
#endif
