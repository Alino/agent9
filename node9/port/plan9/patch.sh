#!/bin/bash
# node9 Phase 1 — host-side patcher: pristine quickjs-ng -> kencc/APE-portable tree.
set -e
cd /tmp/node9probe/src
rm -rf work
cp -r quickjs-master work

cat > work/node9_port.h <<'EOF'
#ifndef NODE9_PORT_H
#define NODE9_PORT_H
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
/* kencc: neutralize GCC attribute syntax (both spellings) */
#define __attribute__(x)
#define __attribute(x)
/* kencc/APE has no alloca. We emulate it with a frame-depth-ordered lazy-free stack:
   the C stack grows down and quickjs never lets an alloca pointer escape its frame, so
   before each allocation we free every buffer whose frame has been unwound past
   (saved mark address <= the current frame). Each of quickjs's 6 alloca sites lives in
   a distinct function with a single live alloca, so the (ptr,mark) records stay sorted
   by mark descending and reclamation is amortized O(1). This bounds live alloca memory
   to O(current stack depth) — matching real alloca — instead of leaking once per call,
   which is what let npm's hundreds of modules OOM under the old shim. Single-threaded
   only (node9 has no pthreads), which holds here. Routed through a wrapper because
   quickjs.c later poisons bare `malloc`. */
#define N9_ALLOCA_CAP 16384
static void *n9_alloca_ptr[N9_ALLOCA_CAP];
static char *n9_alloca_mark[N9_ALLOCA_CAP];
static int   n9_alloca_n;
static void *n9_alloca(unsigned long long n){
    char probe; char *cur = &probe; void *p;
    while(n9_alloca_n > 0 && n9_alloca_mark[n9_alloca_n-1] <= cur)
        free(n9_alloca_ptr[--n9_alloca_n]);
    p = malloc((unsigned long)n + 16);
    if(p && n9_alloca_n < N9_ALLOCA_CAP){
        n9_alloca_ptr[n9_alloca_n] = (void*)p;
        n9_alloca_mark[n9_alloca_n] = cur;
        n9_alloca_n++;
    }
    return p;
}
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
/* kencc miscompiles float relational comparisons against NaN (NaN > x returns true instead
   of false). Bit-based NaN test (no float compare) used to guard JS relational ops. */
static int n9_isnan(double x){ unsigned long long b; memcpy(&b,&x,8); return (((b>>52)&0x7ffULL)==0x7ffULL) && ((b&0xfffffffffffffULL)!=0ULL); }
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
/* real stack-overflow guard: address of a local tracks C stack depth (kencc has no
   __builtin_frame_address). One frame deeper than the caller — constant offset, fine. */
static void *n9_frame_addr(void){ volatile char c; return (void*)&c; }
#define __builtin_frame_address(n) n9_frame_addr()
#endif
EOF

# stub headers for POSIX features absent from APE (found via -I. for <...> includes).
# quickjs-libc.c's os.poll / os.dlopen degrade gracefully (return error) on Plan 9.
cat > work/poll.h <<'EOF'
#ifndef NODE9_POLL_H
#define NODE9_POLL_H
/* poll() implemented over APE's select() — APE multiplexes fds with Plan 9 helper
   procs internally, i.e. this IS the rfork-based native event loop, via a tested path. */
#include <sys/time.h>
#include <sys/select.h>
struct pollfd { int fd; short events; short revents; };
typedef unsigned long nfds_t;
#define POLLIN 1
#define POLLPRI 2
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
#define POLLNVAL 32
static int poll(struct pollfd *fds, nfds_t nfds, int timeout){
    fd_set rfds, wfds, efds;
    struct timeval tv, *ptv;
    int maxfd, n, ready;
    nfds_t i;
    FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&efds);
    maxfd = -1;
    for(i = 0; i < nfds; i++){
        fds[i].revents = 0;
        if(fds[i].fd < 0) continue;
        if(fds[i].events & POLLIN)  FD_SET(fds[i].fd, &rfds);
        if(fds[i].events & POLLOUT) FD_SET(fds[i].fd, &wfds);
        FD_SET(fds[i].fd, &efds);
        if(fds[i].fd > maxfd) maxfd = fds[i].fd;
    }
    ptv = 0;
    if(timeout >= 0){ tv.tv_sec = timeout/1000; tv.tv_usec = (timeout%1000)*1000; ptv = &tv; }
    n = select(maxfd+1, &rfds, &wfds, &efds, ptv);
    if(n <= 0) return n;
    ready = 0;
    for(i = 0; i < nfds; i++){
        short re = 0;
        if(fds[i].fd < 0) continue;
        if(FD_ISSET(fds[i].fd, &rfds)) re |= POLLIN;
        if(FD_ISSET(fds[i].fd, &wfds)) re |= POLLOUT;
        if(FD_ISSET(fds[i].fd, &efds)) re |= POLLERR;
        re &= (fds[i].events | POLLERR | POLLHUP | POLLNVAL);
        fds[i].revents = re;
        if(re) ready++;
    }
    return ready;
}
#endif
EOF
cat > work/dlfcn.h <<'EOF'
#ifndef NODE9_DLFCN_H
#define NODE9_DLFCN_H
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_DEFAULT ((void*)0)
static void *dlopen(const char *f, int m){ (void)f;(void)m; return 0; }
static void *dlsym(void *h, const char *s){ (void)h;(void)s; return 0; }
static int dlclose(void *h){ (void)h; return 0; }
static char *dlerror(void){ return (char*)"dlopen: unsupported on plan9"; }
#endif
EOF

# inject port header at top of cutils.h AND quickjs.h (TUs include them in either order;
# quickjs-libc.c pulls quickjs.h first, so the shims must be present there too)
printf '#include "node9_port.h"\n' | cat - work/cutils.h > work/cutils.h.tmp && mv work/cutils.h.tmp work/cutils.h
printf '#include "node9_port.h"\n' | cat - work/quickjs.h > work/quickjs.h.tmp && mv work/quickjs.h.tmp work/quickjs.h

# cutils.h: C11 [static N] array param -> plain
sed -i '' 's/#define minimum_length(n) static n/#define minimum_length(n) n/' work/cutils.h

# quickjs.c: switch dispatch (no computed goto); hex float; enum compound literal
sed -i '' 's/#define DIRECT_DISPATCH  1/#define DIRECT_DISPATCH  0/' work/quickjs.c
sed -i '' 's/0x1p63/9223372036854775808.0/g' work/quickjs.c
sed -i '' 's/(JSAtomKindEnum){-1}/(JSAtomKindEnum)-1/' work/quickjs.c

# kencc codegen bug (non-LP64): for args[var_idx - ARGUMENT_VAR_OFFSET], kencc DISTRIBUTES
# the *sizeof multiply: args + var_idx*24 - ARGUMENT_VAR_OFFSET*24. With var_idx=0x20000000
# and sizeof(JSVarDef)=24, var_idx*24 == exactly 0x300000000 (spurious high bits, acid:
# DI=0x3004ea790 vs correct SI=0x4ea790) and the subtraction-back is botched -> wild write.
# A bitmask barrier forces the index subtraction to materialize before any address math.
sed -i '' 's/var_idx - ARGUMENT_VAR_OFFSET/((var_idx - ARGUMENT_VAR_OFFSET) \& 0x7fffffff)/g' work/quickjs.c

# kencc miscompiles float relational comparisons vs NaN (NaN > x => true, should be false).
# Guard the 4 JS relational ops in js_relational_slow with a bit-based NaN test (n9_isnan
# in node9_port.h). Without this, `undefined > 2000` etc. are true -> e.g. arborist realpath
# eloops on every npm install. (kencc only; mainline is IEEE-correct.)
sed -i '' 's/res = (d1 < d2);/res = (!n9_isnan(d1) \&\& !n9_isnan(d2) \&\& d1 < d2);/' work/quickjs.c
sed -i '' 's/res = (d1 <= d2);/res = (!n9_isnan(d1) \&\& !n9_isnan(d2) \&\& d1 <= d2);/' work/quickjs.c
sed -i '' 's/res = (d1 > d2);/res = (!n9_isnan(d1) \&\& !n9_isnan(d2) \&\& d1 > d2);/' work/quickjs.c
sed -i '' 's/res = (d1 >= d2);/res = (!n9_isnan(d1) \&\& !n9_isnan(d2) \&\& d1 >= d2);/' work/quickjs.c
# Same NaN miscompile in the EQUALITY paths: loose `==` (js_eq_slow) and strict `===`
# (js_strict_eq2 non-SAME_VALUE). NaN==NaN / NaN===NaN must be false (so `x !== x` NaN tests
# work). The SAME_VALUE/Object.is path already guards NaN via isnan() and is left alone; the
# guard keeps +0==-0 true (non-NaN passes through). Match end-of-line for the loose case so we
# don't touch the two commented `===`/SAME_VALUE_ZERO lines.
sed -i '' 's/res = (d1 == d2);$/res = (!n9_isnan(d1) \&\& !n9_isnan(d2) \&\& d1 == d2);/' work/quickjs.c
sed -i '' 's|res = (d1 == d2); /\* if NaN return false and +0 == -0 \*/|res = (!n9_isnan(d1) \&\& !n9_isnan(d2) \&\& d1 == d2);|' work/quickjs.c
# Array.prototype.sort(fn) and TypedArray.prototype.sort(fn) compute the comparator sign as
# `cmp = (val > 0) - (val < 0)` with raw C double compares (NOT js_relational_slow). A NaN from
# the user comparator under the kencc bug could yield a non-zero/inconsistent sign and violate
# rqsort's total-order invariant → potential OOB. Guard with n9_isnan (NaN sign = 0 = "equal",
# matching Node). The (double) cast makes this a harmless no-op for the int-valued sites.
sed -i '' 's/cmp = (val > 0) - (val < 0);/cmp = (n9_isnan((double)val) ? 0 : (val > 0) - (val < 0));/g' work/quickjs.c

# APE intptr_t/uintptr_t are 32-bit `long` — they TRUNCATE 64-bit pointers everywhere
# quickjs uses them for pointer tagging/arithmetic. Force them to 64-bit (int64_t/uint64_t).
# (s/intptr_t/int64_t/ also fixes uintptr_t -> uint64_t since uintptr_t = "u"+"intptr_t".)
for f in quickjs.c cutils.h libregexp.c libunicode.c dtoa.c quickjs-libc.c; do
  sed -i '' 's/intptr_t/int64_t/g' work/$f
done

# kencc rejects `return <void-expr>;` (C99-legal). Drop the `return ` keyword for
# calls to void-returning functions (void fn falls off the end fine). Grow this
# list as each recompile surfaces the next void callee.
VOID_CALLS="js_free_cstring"
for f in $VOID_CALLS; do
  sed -i '' "s/return $f(/$f(/g" work/quickjs.c
done

# quickjs.h: kencc rejects compound literals on UNIONS. JS_MKPTR/JS_MKVAL/JS_NAN use
# (JSValueUnion){ .ptr=... } — rewrite the 3 as static-inline helpers (struct JSValue,
# JS_NAN_BOXING=0). Required because APE intptr_t is 32-bit so NaN-boxing truncates ptrs.
perl -i -pe 's|^#define JS_MKPTR\(tag, p\).*|static inline JSValue JS_MKPTR(int64_t tag, void *p){ JSValue v; v.u.ptr = p; v.tag = tag; return v; }|' work/quickjs.h
perl -i -pe 's|^#define JS_MKVAL\(tag, val\).*|static inline JSValue JS_MKVAL(int64_t tag, int32_t val){ JSValue v; v.u.ptr = 0; v.u.int32 = val; v.tag = tag; return v; }|' work/quickjs.h
perl -i -pe 's|^#define JS_NAN .*|static inline JSValue js__nan(void){ JSValue v; v.u.float64 = NAN; v.tag = JS_TAG_FLOAT64; return v; }\n#define JS_NAN js__nan()|' work/quickjs.h

# quickjs-libc.c: APE struct stat has no st_blocks nor nanosecond st_atim/st_mtim/st_ctim.
# Use the seconds-based fields (st_atime etc., which APE has) and zero st_blocks.
perl -i -pe 's/JS_NewInt64\(ctx, st\.st_blocks\)/JS_NewInt64(ctx, (int64_t)0)/' work/quickjs-libc.c
perl -i -pe 's/timespec_to_ms\(&st\.st_atim\)/(int64_t)st.st_atime * 1000/' work/quickjs-libc.c
perl -i -pe 's/timespec_to_ms\(&st\.st_mtim\)/(int64_t)st.st_mtime * 1000/' work/quickjs-libc.c
perl -i -pe 's/timespec_to_ms\(&st\.st_ctim\)/(int64_t)st.st_ctime * 1000/' work/quickjs-libc.c

# package
cd work && tar czf ../qjs-patched.tar.gz . && cd ..
echo "patched tar: $(ls -lh /tmp/node9probe/src/qjs-patched.tar.gz | awk '{print $5}')"
echo "sanity checks:"
grep -c '0x1p63' work/quickjs.c | sed 's/^/  remaining 0x1p63: /'
grep -m1 'DIRECT_DISPATCH  0' work/quickjs.c >/dev/null && echo "  DIRECT_DISPATCH patched: yes"
grep -m1 'JSAtomKindEnum)-1' work/quickjs.c >/dev/null && echo "  enum literal patched: yes"
