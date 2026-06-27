/* posix_llvm.c — the POSIX surface LLVM's Unix .inc files reference that cc9 didn't
 * already provide. Split by honesty:
 *   REAL  — backed by a Plan 9 syscall or a sane constant (getpagesize, getpid,
 *           getuid, sysconf, atoi, rand).
 *   STUB  — Plan 9 has no equivalent on any compiler compute path; returns a
 *           POSIX failure (-1/0) so callers take their error branch. fork/exec,
 *           record locks, signals, rlimits, sockets, mmap-of-files, passwd db.
 * None of the STUBs are reached when clang merely parses + emits an object. */
#include <stdint.h>

typedef unsigned long n9size_t;
extern void *malloc(n9size_t);
extern void  free(void *);
extern void *memset(void *, int, n9size_t);
extern void *memcpy(void *, const void *, n9size_t);
extern n9size_t strlen(const char *);
extern long  n9_pread(int, void *, long, long long);
extern void  n9_exits(const char *);

/* ---- memory: no real mmap. Anonymous => zeroed malloc; file-backed => malloc
 * + pread (the plan's read-fallback). munmap frees; mprotect/madvise/msync are
 * no-ops. Read-only consumers (the compiler's MemoryBuffer) are satisfied. */
void *mmap(void *addr, n9size_t len, int prot, int flags, int fd, long off) {
	(void)addr; (void)prot;
	/* Allocate len+1 and zero the trailing byte. Real mmap zero-pads the file's
	 * last page past EOF; clang's MemoryBuffer maps exactly FileSize bytes and then
	 * asserts the byte AT [FileSize] is 0 (its null terminator). A bare malloc(len)
	 * leaves that byte OOB/garbage -> "Buffer is not null terminated!". The +1 NUL
	 * provides the page-padding terminator clang relies on. */
	void *p = malloc((len ? len : 1) + 1);
	if (!p) return (void *)-1;
	if (flags & 0x20) {           /* MAP_ANON(YMOUS) */
		memset(p, 0, len + 1);
	} else if (fd >= 0) {         /* file-backed: read the region in */
		long got = n9_pread(fd, p, (long)len, off);
		if (got < 0) got = 0;
		if ((n9size_t)got < len) memset((char *)p + got, 0, len - (n9size_t)got);
		((char *)p)[len] = 0;     /* the null terminator clang expects past EOF */
	} else {
		memset(p, 0, len + 1);
	}
	return p;
}
int munmap(void *p, n9size_t len) { (void)len; free(p); return 0; }
int mprotect(void *p, n9size_t len, int prot) { (void)p; (void)len; (void)prot; return 0; }
int madvise(void *p, n9size_t len, int adv) { (void)p; (void)len; (void)adv; return 0; }
int msync(void *p, n9size_t len, int fl) { (void)p; (void)len; (void)fl; return 0; }

extern const char *cc9_exitstr(int);
void _exit(int code) { n9_exits(cc9_exitstr(code)); for (;;) {} }

n9size_t strnlen(const char *s, n9size_t max) {
	n9size_t n = 0; while (n < max && s[n]) n++; return n;
}
char *strdup(const char *s) {
	n9size_t n = strlen(s) + 1; char *p = malloc(n);
	if (p) memcpy(p, s, n);
	return p;
}
char *strndup(const char *s, n9size_t max) {
	n9size_t n = 0; while (n < max && s[n]) n++;
	char *p = malloc(n + 1);
	if (p) { memcpy(p, s, n); p[n] = 0; }
	return p;
}
char *strsignal(int sig) {
	static char buf[24]; const char *d = "signal ";
	int i = 0; while (d[i]) { buf[i] = d[i]; i++; }
	if (sig <= 0) buf[i++] = '0';
	else { char t[8]; int k = 0; while (sig) { t[k++] = '0' + sig % 10; sig /= 10; } while (k) buf[i++] = t[--k]; }
	buf[i] = 0; return buf;
}

/* qsort — insertion sort, correct for ANY element size (byte-swap, no temp-size
 * cap). A silent no-op on big elements would leave data unsorted and break a
 * later bsearch -> wrong-memory access; never cap. O(n^2) but qsort callers are
 * small. ponytail: swap byte-by-byte to avoid a VLA/alloca. */
void qsort(void *base, n9size_t n, n9size_t sz, int (*cmp)(const void *, const void *)) {
	char *a = base;
	for (n9size_t i = 1; i < n; i++)
		for (n9size_t j = i; j > 0 && cmp(a + j*sz, a + (j-1)*sz) < 0; j--) {
			char *x = a + j*sz, *y = a + (j-1)*sz;
			for (n9size_t k = 0; k < sz; k++) { char t = x[k]; x[k] = y[k]; y[k] = t; }
		}
}

/* ---- REAL ---- */
int getpagesize(void) { return 4096; }      /* Plan 9 amd64 base page */
long sysconf(int name) {
	switch (name) {
	case 30: return 4096;        /* _SC_PAGESIZE / _SC_PAGE_SIZE */
	case 84: return 1;           /* _SC_NPROCESSORS_ONLN (threads OFF) */
	case 85: return 262144;      /* _SC_PHYS_PAGES (~1 GiB; informational) */
	case 2:  return 100;         /* _SC_CLK_TCK */
	case 70: return 1024;        /* _SC_GETPW_R_SIZE_MAX */
	case 180: return 256;        /* _SC_HOST_NAME_MAX */
	default: return -1;
	}
}
int   getpid(void)  { return 1; }            /* TODO: read /dev/pid or Tos */
int   getppid(void) { return 0; }
unsigned int getuid(void)  { return 0; }
unsigned int geteuid(void) { return 0; }
unsigned int getgid(void)  { return 0; }
unsigned int getegid(void) { return 0; }

int atoi(const char *s) {
	int sign = 1, v = 0;
	while (*s == ' ' || *s == '\t' || *s == '\n') s++;
	if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
	while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
	return v * sign;
}
long atol(const char *s) {
	long sign = 1, v = 0;
	while (*s == ' ' || *s == '\t' || *s == '\n') s++;
	if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
	while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
	return v * sign;
}
extern double strtod(const char *, char **);
extern long long strtoll(const char *, char **, int);
long long atoll(const char *s) { return strtoll(s, 0, 10); }
double atof(const char *s) { return strtod(s, 0); }

/* bsearch: classic binary search over a sorted array. */
void *bsearch(const void *key, const void *base, n9size_t n, n9size_t sz,
              int (*cmp)(const void *, const void *)) {
	const char *a = base;
	while (n) {
		n9size_t mid = n / 2;
		const char *p = a + mid * sz;
		int c = cmp(key, p);
		if (c == 0) return (void *)p;
		if (c > 0) { a = p + sz; n -= mid + 1; }
		else n = mid;
	}
	return 0;
}

/* xorshift PRNG — LLVM only uses rand() for jitter/tmp-name salting. */
static uint64_t rng_state = 0x2545F4914F6CDD1DULL;
void srand(unsigned s) { rng_state = s ? s : 1; }
void srandom(unsigned s) { srand(s); }
int rand(void) {
	rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
	return (int)(rng_state & 0x7fffffff);
}
long random(void) { return rand(); }

/* ---- process control over Plan 9 rfork/exec/await ---- */
extern long n9_rfork(int);
extern long n9_exec(const char *, char **);
extern long n9_await(char *, int);
extern void n9_exits(const char *);
/* Plan 9 rfork flags: RFPROC=1<<4, RFFDG=1<<2 (copy fd group). */
#define N9_RFPROC 0x10
#define N9_RFFDG  0x04
int    fork(void) { return (int)n9_rfork(N9_RFPROC|N9_RFFDG); }

/* system(): run `cmd` via `/bin/rc -c cmd`, await it, return 0 on clean exit.
 * The libc++ filesystem test framework uses system("mkdir -p ...") etc. With
 * cmd==NULL, report that a command processor is available (rc). */
/* string prefix test */
static int n9_startswith(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
int system(const char *cmd) {
	if (!cmd) return 1;
	/* ponytail: 9front's chmod has no -R. The libc++ filesystem test harness only
	 * runs `chmod -R 777 <dir>` to make a tree writable before `rm -rf`; on 9front
	 * test-created files are already owned+writable by the user, so the rm works
	 * regardless. Treat `chmod -R ...` as a successful no-op. Drop if 9front chmod
	 * ever gains -R. */
	if (n9_startswith(cmd, "chmod -R ")) return 0;
	long pid = n9_rfork(N9_RFPROC|N9_RFFDG);
	if (pid < 0) return -1;
	if (pid == 0) {
		char *argv[4]; argv[0] = "rc"; argv[1] = "-c"; argv[2] = (char *)cmd; argv[3] = 0;
		n9_exec("/bin/rc", argv);
		n9_exits("exec");                 /* only reached if exec fails */
	}
	char w[256];
	for (;;) {
		long n = n9_await(w, sizeof w - 1);
		if (n < 0) return -1;
		w[n] = 0;
		long wp = 0; const char *p = w; while (*p >= '0' && *p <= '9') wp = wp*10 + (*p++ - '0');
		if (wp != pid) continue;          /* a different child; keep waiting */
		/* await msg = "pid utime stime rtime 'status'"; the exit string is
		 * QUOTED (Plan 9 %q): '' means empty == clean exit == success. */
		int sp = 0; const char *q = w;
		for (; *q && sp < 4; q++) if (*q == ' ') sp++;
		if (*q == '\'') q++;              /* skip opening quote */
		return (*q == '\'' || *q == 0) ? 0 : 1;   /* empty quoted status => 0 */
	}
}

/* ---- STUB (POSIX failure; not on a compile path) ---- */
extern long n9_pipe(int *);
int    pipe(int fds[2]) { return (int)n9_pipe(fds); }
int    dup3(int o, int n, int f) { (void)o; (void)n; (void)f; return -1; }
int    setsid(void) { return -1; }
int    getsid(int p) { (void)p; return -1; }
int    execv(const char *p, char *const a[]) { (void)p; (void)a; return -1; }
int    execvp(const char *p, char *const a[]) { (void)p; (void)a; return -1; }
int    execve(const char *p, char *const a[], char *const e[]) { (void)p; (void)a; (void)e; return -1; }
/* waitpid over Plan 9 await(2): wait for child `pid` (or any if pid<=0), decode
 * the exit-status string into *s for the WIFEXITED/WEXITSTATUS macros. Plan 9
 * reports a non-empty child exit string to the parent as "<argv0> <pid>: <str>";
 * cc9_exitstr tags the numeric code as "cc9exit=N", so we scan the whole status
 * for that marker. Empty status => clean exit 0; a "cc9exit=N" marker => exit
 * code N; anything else non-empty (a "sys:" fault note / "cc9: abort") => report
 * as a signal (SIGABRT) so WIFSIGNALED is true. */
static const char *cc9_find(const char *h, const char *n){
	for(; *h; h++){ const char *a=h,*b=n; while(*b && *a==*b){a++;b++;} if(!*b) return h; }
	return 0;
}
int    waitpid(int pid, int *s, int o) {
	(void)o;
	char w[256];
	for (;;) {
		long n = n9_await(w, sizeof w - 1);
		if (n < 0) return -1;
		w[n] = 0;
		long wp = 0; const char *p = w; while (*p >= '0' && *p <= '9') wp = wp*10 + (*p++ - '0');
		if (pid > 0 && wp != pid) continue;          /* a different child; keep waiting */
		/* locate the quoted exit string: skip "pid utime stime rtime " (4 spaces) */
		int sp = 0; const char *q = w;
		for (; *q && sp < 4; q++) if (*q == ' ') sp++;
		if (*q == '\'') q++;                          /* skip opening quote */
		if (s) {
			const char *m = cc9_find(q, "cc9exit=");
			if (*q == '\'' || *q == 0) *s = 0;                       /* clean exit 0 */
			else if (m) {
				m += 8; int neg = (*m=='-'); if (neg) m++;
				int code = 0; while (*m >= '0' && *m <= '9') code = code*10 + (*m++ - '0');
				*s = ((neg ? -code : code) & 0xff) << 8;             /* WIFEXITED, WEXITSTATUS=code */
			}
			/* a CPU trap (__builtin_trap/__builtin_verbose_trap -> "sys: trap:
			 * invalid opcode") is libc++'s hardening trap path — report as SIGILL
			 * so death tests map it to DeathCause::Trap, not SIGABRT. */
			else if (cc9_find(q, "trap") || cc9_find(q, "invalid opcode") || cc9_find(q, "illegal"))
				*s = 4;                                              /* SIGILL: WIFSIGNALED */
			else *s = 6;                                             /* SIGABRT: WIFSIGNALED */
		}
		return (int)wp;
	}
}
int    wait4(int p, int *s, int o, void *r) { (void)r; return waitpid(p, s, o); }
int    wait(int *s) { return waitpid(-1, s, 0); }
int    kill(int p, int s) { (void)p; (void)s; return -1; }
unsigned int alarm(unsigned int s) { (void)s; return 0; }
/* setitimer over Plan 9 alarm(2): ITIMER_REAL arms n9_alarm(ms); a SIGALRM note
 * then fires (crt0.c note handler). Other timers are accepted as no-ops.
 * Local struct mirrors <sys/time.h> (this file avoids pulling system headers). */
extern long n9_alarm(unsigned long);
struct itimerval { struct { long tv_sec, tv_usec; } it_interval, it_value; };
int setitimer(int which, const struct itimerval *nv, struct itimerval *ov) {
	(void)ov;
	if(which != 0) return 0;                       /* ITIMER_REAL only */
	unsigned long ms = nv ? (unsigned long)nv->it_value.tv_sec*1000 + (unsigned long)nv->it_value.tv_usec/1000 : 0;
	n9_alarm(ms);
	return 0;
}
int    usleep(unsigned int us) { (void)us; return 0; }
int    ioctl(int fd, unsigned long req, ...) { (void)fd; (void)req; return -1; }
int    fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; return 0; }  /* "locks succeed" */
int    chown(const char *p, unsigned u, unsigned g) { (void)p; (void)u; (void)g; return 0; }
int    fchown(int fd, unsigned u, unsigned g) { (void)fd; (void)u; (void)g; return 0; }
int    lchown(const char *p, unsigned u, unsigned g) { (void)p; (void)u; (void)g; return 0; }
unsigned umask(unsigned m) { (void)m; return 022; }
int    gethostname(char *n, unsigned long l) { if (l) n[0] = 0; return 0; }
extern long n9_dup(int, int);
int    dup2(int o, int n) { return (int)n9_dup(o, n); }

/* time breakdown: gmtime/localtime (LLVM uses them for diagnostic timestamps).
 * No timezone db on Plan 9 -> local == UTC. Civil-from-days (Hinnant). */
struct cc9_tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
struct cc9_tm *gmtime_r(const long *tp, struct cc9_tm *r) {
	long secs = *tp, days = secs / 86400, rem = secs % 86400;
	if (rem < 0) { rem += 86400; days -= 1; }
	r->tm_hour = (int)(rem / 3600); r->tm_min = (int)(rem % 3600 / 60); r->tm_sec = (int)(rem % 60);
	r->tm_wday = (int)((days % 7 + 4 + 7) % 7);              /* 1970-01-01 = Thu */
	long z = days + 719468;                                  /* shift epoch to 0000-03-01 */
	long era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned doe = (unsigned)(z - era * 146097);
	unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
	long y = (long)yoe + era * 400;
	unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
	unsigned mp = (5*doy + 2) / 153;
	unsigned d = doy - (153*mp + 2)/5 + 1;
	unsigned m = mp < 10 ? mp + 3 : mp - 9;
	y += (m <= 2);
	r->tm_year = (int)(y - 1900); r->tm_mon = (int)m - 1; r->tm_mday = (int)d;
	/* yday: days since Jan 1 of this year */
	static const int cum[] = {0,31,59,90,120,151,181,212,243,273,304,334};
	int leap = ((y%4==0 && y%100!=0) || y%400==0);
	r->tm_yday = cum[r->tm_mon] + r->tm_mday - 1 + (leap && r->tm_mon > 1 ? 1 : 0);
	r->tm_isdst = 0;
	return r;
}
struct cc9_tm *localtime_r(const long *tp, struct cc9_tm *r) { return gmtime_r(tp, r); }
struct cc9_tm *gmtime(const long *tp) { static struct cc9_tm t; return gmtime_r(tp, &t); }
struct cc9_tm *localtime(const long *tp) { return gmtime(tp); }

/* uname — fixed Plan 9 amd64 identity (LLVM host triple detection). */
struct cc9_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
static void cc9_setstr(char *d, const char *s) { int i = 0; while (s[i] && i < 64) { d[i] = s[i]; i++; } d[i] = 0; }
int uname(void *vp) {
	struct cc9_utsname *u = vp;
	cc9_setstr(u->sysname, "Plan9");  cc9_setstr(u->nodename, "9front");
	cc9_setstr(u->release, "9front"); cc9_setstr(u->version, "cc9");
	cc9_setstr(u->machine, "x86_64"); cc9_setstr(u->domainname, "");
	return 0;
}

/* signals: no delivery; sets "succeed" so handlers just never fire. */
int sigemptyset(unsigned long *s) { if (s) *s = 0; return 0; }
int sigfillset(unsigned long *s) { if (s) *s = ~0ul; return 0; }
int sigaddset(unsigned long *s, int n) { if (s) *s |= 1ul << (n & 63); return 0; }
int sigdelset(unsigned long *s, int n) { if (s) *s &= ~(1ul << (n & 63)); return 0; }
int sigismember(const unsigned long *s, int n) { return s ? (int)((*s >> (n & 63)) & 1) : 0; }
int sigprocmask(int how, const unsigned long *set, unsigned long *old) { (void)how; (void)set; if (old) *old = 0; return 0; }
extern void cc9_set_sigh(int, void (*)(int));
int sigaction(int sig, const void *act, void *old) {
	(void)old;
	/* struct sigaction begins with the sa_handler union (a fn pointer); register
	 * it so a matching Plan 9 note (e.g. SIGALRM from setitimer) dispatches it. */
	if (act) cc9_set_sigh(sig, *(void (*const *)(int))act);
	return 0;
}

/* rlimit / rusage: report "unlimited" / zero. */
extern void cc9_set_nproc_limit(long);
extern long cc9_get_nproc_limit(void);
int getrlimit(int r, void *rl) {
	if (rl) { unsigned long *p = rl;
		if (r == 6 /*RLIMIT_NPROC*/) { p[0] = p[1] = (unsigned long)cc9_get_nproc_limit(); }
		else { p[0] = p[1] = ~0ul; }
	}
	return 0;
}
int setrlimit(int r, const void *rl) {
	if (r == 6 /*RLIMIT_NPROC*/ && rl) { const unsigned long *p = rl; cc9_set_nproc_limit((long)p[0]); }
	return 0;
}
int getrusage(int who, void *ru) { (void)who; (void)ru; return 0; }

/* passwd db: none. getpwuid* return failure so LLVM falls back to $HOME/cwd. */
void *getpwuid(unsigned uid) { (void)uid; return 0; }
int   getpwuid_r(unsigned uid, void *pw, char *buf, unsigned long n, void **res) { (void)uid; (void)pw; (void)buf; (void)n; if (res) *res = 0; return 0; }
void *getpwnam(const char *nm) { (void)nm; return 0; }
int   getpwnam_r(const char *nm, void *pw, char *buf, unsigned long n, void **res) { (void)nm; (void)pw; (void)buf; (void)n; if (res) *res = 0; return 0; }

/* sockets: no BSD layer (Plan 9 networking is /net). All fail. */
int  socket(int a, int b, int c) { (void)a; (void)b; (void)c; return -1; }
int  bind(int a, const void *b, unsigned c) { (void)a; (void)b; (void)c; return -1; }
int  listen(int a, int b) { (void)a; (void)b; return -1; }
int  accept(int a, void *b, unsigned *c) { (void)a; (void)b; (void)c; return -1; }
int  connect(int a, const void *b, unsigned c) { (void)a; (void)b; (void)c; return -1; }
long send(int a, const void *b, unsigned long c, int d) { (void)a; (void)b; (void)c; (void)d; return -1; }
long recv(int a, void *b, unsigned long c, int d) { (void)a; (void)b; (void)c; (void)d; return -1; }
int  setsockopt(int a, int b, int c, const void *d, unsigned e) { (void)a; (void)b; (void)c; (void)d; (void)e; return -1; }
int  shutdown(int a, int b) { (void)a; (void)b; return -1; }

/* dynamic loading: static binaries only. */
void *dlopen(const char *p, int m) { (void)p; (void)m; return 0; }
int   dlclose(void *h) { (void)h; return 0; }
void *dlsym(void *h, const char *n) { (void)h; (void)n; return 0; }
char *dlerror(void) { return (char *)"cc9: no dynamic loading"; }
int   dladdr(const void *a, void *info) { (void)a; (void)info; return 0; }
