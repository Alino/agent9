/* posix_llvm.c — the POSIX surface LLVM's Unix .inc files reference that cc9 didn't
 * already provide. Split by honesty:
 *   REAL  — backed by a Plan 9 syscall or a sane constant (getpagesize, getpid,
 *           getuid, sysconf, atoi, rand).
 *   STUB  — Plan 9 has no equivalent on any compiler compute path; returns a
 *           POSIX failure (-1/0) so callers take their error branch. fork/exec,
 *           record locks, signals, rlimits, sockets, mmap-of-files, passwd db.
 * None of the STUBs are reached when clang merely parses + emits an object. */
#include <stdint.h>
#include <malloc.h>

typedef unsigned long n9size_t;
extern void *malloc(n9size_t);
extern void  free(void *);
extern void *memset(void *, int, n9size_t);
extern void *memcpy(void *, const void *, n9size_t);
extern n9size_t strlen(const char *);
extern long  n9_pread(int, void *, long, long long);
extern void  n9_exits(const char *);

/* ---- executable memory: an anonymous PROT_EXEC request (a JIT asking for W^X
 * memory, e.g. Mesa's rtasm vertex-fetch codegen) can't come from malloc — the
 * kernel NX-enforces the heap. Route it to segattach(SG_EXEC), which the wxallow
 * kernel patch makes writable+executable. These blocks must NOT be free()d in
 * munmap (they aren't heap), so record them in a tiny table.
 * ponytail: fixed 64-slot table + global (Mesa allocs ONE 10MB exec heap, never
 * frees it); grow to a real allocator / add segdetach if a JIT churns exec maps. */
extern void *n9_segattach(unsigned long attr, const char *cls, void *va, unsigned long len);
enum { SG_EXEC = 04000 };
#define EXEC_PGSZ 0x1000ul
static void *exec_tab[64];
static int   exec_n;
static int is_exec_map(void *p) {
	for (int i = 0; i < exec_n; i++) if (exec_tab[i] == p) return 1;
	return 0;
}

void *mmap(void *addr, n9size_t len, int prot, int flags, int fd, long off) {
	(void)addr;
	if ((prot & 4) && (flags & 0x20)) {   /* PROT_EXEC + MAP_ANON: JIT memory */
		n9size_t sz = (len + EXEC_PGSZ - 1) & ~(EXEC_PGSZ - 1);
		void *p = n9_segattach(SG_EXEC, "memory", 0, sz ? sz : EXEC_PGSZ);
		if (p == (void *)-1) return (void *)-1;
		if (exec_n < (int)(sizeof exec_tab / sizeof exec_tab[0]))
			exec_tab[exec_n++] = p;
		return p;
	}
	(void)prot;
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
int munmap(void *p, n9size_t len) { (void)len; if (is_exec_map(p)) return 0; free(p); return 0; }
/* POSIX shm — 9front has no shm_open; the JIT's in-process mapper never calls
 * these (only the cross-process SharedMemoryMapper does). Fail cleanly. */
int shm_open(const char *n, int f, unsigned m) { (void)n; (void)f; (void)m; return -1; }
int shm_unlink(const char *n) { (void)n; return -1; }
int sigaltstack(const void *a, void *b) { (void)a; (void)b; return 0; }
struct mallinfo  mallinfo(void)  { struct mallinfo  m; memset(&m, 0, sizeof m); return m; }
struct mallinfo2 mallinfo2(void) { struct mallinfo2 m; memset(&m, 0, sizeof m); return m; }
extern int rand(void);
unsigned arc4random(void) { return ((unsigned)rand() << 16) ^ (unsigned)rand(); }
unsigned arc4random_uniform(unsigned u) { return u ? arc4random() % u : 0; }
void arc4random_buf(void *b, unsigned long n) { unsigned char *p = b; for (unsigned long i = 0; i < n; i++) p[i] = (unsigned char)rand(); }
/* JIT unwind-frame registration: RuntimeDyld calls these to tell the unwinder
 * about JIT'd code's .eh_frame. Our JIT'd code never throws -> no-op is safe. */
void __register_frame(const void *fde) { (void)fde; }
void __deregister_frame(const void *fde) { (void)fde; }
int backtrace(void **b, int n) { (void)b; (void)n; return 0; }
char **backtrace_symbols(void *const *b, int n) { (void)b; (void)n; return 0; }
void backtrace_symbols_fd(void *const *b, int n, int fd) { (void)b; (void)n; (void)fd; }
int posix_spawn(int *pid, const char *p, const void *fa, const void *at, char *const av[], char *const ev[]) { (void)pid;(void)p;(void)fa;(void)at;(void)av;(void)ev; return -1; }
int posix_spawnp(int *pid, const char *p, const void *fa, const void *at, char *const av[], char *const ev[]) { (void)pid;(void)p;(void)fa;(void)at;(void)av;(void)ev; return -1; }
int posix_spawn_file_actions_init(void *a) { (void)a; return 0; }
int posix_spawn_file_actions_destroy(void *a) { (void)a; return 0; }
int posix_spawn_file_actions_adddup2(void *a, int x, int y) { (void)a;(void)x;(void)y; return 0; }
int posix_spawn_file_actions_addopen(void *a, int x, const char *p, int f, unsigned m) { (void)a;(void)x;(void)p;(void)f;(void)m; return 0; }
unsigned long getauxval(unsigned long t) { return t == 6 ? 4096 : 0; } /* AT_PAGESZ */
int mprotect(void *p, n9size_t len, int prot) { (void)p; (void)len; (void)prot; return 0; }
int madvise(void *p, n9size_t len, int adv) { (void)p; (void)len; (void)adv; return 0; }
int msync(void *p, n9size_t len, int fl) { (void)p; (void)len; (void)fl; return 0; }

extern const char *cc9_exitstr(int);
/* _exit skips atexit (POSIX) but must still kill worker threads — Plan 9
 * doesn't reparent orphans and a leaked reader steals the shell's stdin. */
void _exit(int code) {
	extern void cc9_kill_threads(void) __attribute__((weak));
	if (cc9_kill_threads) cc9_kill_threads();
	n9_exits(cc9_exitstr(code));
	for (;;) {}
}

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
extern unsigned cc9_tos_pid(void);           /* Plan 9 _tos.pid, captured in crt0 */
int   getpid(void)  { unsigned p = cc9_tos_pid(); return p ? (int)p : 1; }
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

/* xorshift PRNG — LLVM only uses rand() for jitter/tmp-name salting. Default seed
 * is fixed on purpose: C requires rand() with no prior srand() to reproduce the
 * srand(1) sequence (C11 7.22.2.2), so callers wanting per-run randomness must
 * srand() themselves (the Plan 9 source is /dev/random, already used in fs.c). */
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
/* Plan 9 rfork flags: RFPROC=1<<4, RFFDG=1<<2 (copy fd group), RFENVG=1<<1
 * (copy env group — gives fork() POSIX private-environ semantics; execve
 * then writes envp into the child's own /env). */
#define N9_RFPROC 0x10
#define N9_RFFDG  0x04
#define N9_RFENVG 0x02
static void cc9_reap_forked(int pid);      /* below: child bookkeeping + reaper */
static void cc9_reap_child_reset(void);
int waitpid(int, int *, int);
int    fork(void) {
	int pid = (int)n9_rfork(N9_RFPROC|N9_RFFDG|N9_RFENVG);
	if (pid > 0) cc9_reap_forked(pid);
	else if (pid == 0) cc9_reap_child_reset();   /* copied tables describe the parent's kids */
	return pid;
}

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
	/* route through fork()/waitpid so the reaper bookkeeping stays coherent
	 * (a bare await here would race the reaper thread for the waitmsg). */
	int pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		char *argv[4]; argv[0] = "rc"; argv[1] = "-c"; argv[2] = (char *)cmd; argv[3] = 0;
		n9_exec("/bin/rc", argv);
		n9_exits("exec");                 /* only reached if exec fails */
	}
	int st = 0;
	if (waitpid(pid, &st, 0) < 0) return -1;
	return st;
}

/* ---- STUB (POSIX failure; not on a compile path) ---- */
extern long n9_pipe(int *);
int    pipe(int fds[2]) { return (int)n9_pipe(fds); }
int    dup3(int o, int n, int f) { (void)o; (void)n; (void)f; return -1; }
int    setsid(void) { return -1; }
int    getsid(int p) { (void)p; return -1; }
/* ---- exec over Plan 9 exec(2) ----
 * execve: envp lands in the child's private /env (fork uses RFENVG), fds
 * marked FD_CLOEXEC (poll.c table) are closed — libuv's spawn error-pipe
 * protocol depends on close-on-exec actually happening. */
extern int *__n9_errno(void);
#define errno (*__n9_errno())
extern int cc9_poll_cloexec(int);
extern long n9_create(const char *, int, unsigned int);
extern long n9_close(int);
extern long n9_pwrite(int, const void *, long, long long);
int    execve(const char *p, char *const a[], char *const e[]) {
	if (e) {
		for (int i = 0; e[i]; i++) {
			const char *eq = e[i]; while (*eq && *eq != '=') eq++;
			if (!*eq) continue;
			char name[128]; int n = 0;
			const char *s = e[i];
			while (s < eq && n < (int)sizeof name - 6) name[n++] = *s++;
			name[n] = 0;
			char path[160]; char *d = path;
			const char *pre = "/env/"; while (*pre) *d++ = *pre++;
			for (int j = 0; j <= n; j++) *d++ = name[j];
			int fd = (int)n9_create(path, 1 /*OWRITE*/, 0666);
			if (fd >= 0) {
				const char *v = eq + 1; long vl = 0; while (v[vl]) vl++;
				n9_pwrite(fd, v, vl, -1);
				n9_close(fd);
			}
		}
	}
	for (int fd = 3; fd < 64; fd++)
		if (cc9_poll_cloexec(fd)) n9_close(fd);
	n9_exec(p, (char **)a);
	errno = 2 /*ENOENT*/;
	return -1;
}
int    execv(const char *p, char *const a[]) { return execve(p, a, 0); }
int    execvp(const char *p, char *const a[]) {
	for (const char *s = p; *s; s++)
		if (*s == '/') return execve(p, a, 0);
	char b[256]; char *d = b;
	const char *pre = "/bin/"; while (*pre) *d++ = *pre++;
	const char *s = p; while (*s && d < b + sizeof b - 1) *d++ = *s++;
	*d = 0;
	return execve(b, a, 0);
}
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
/* decode one waitmsg ("pid utime stime rtime 'status'") -> pid, *s */
static int cc9_wait_decode(const char *w, int *s) {
	long wp = 0; const char *p = w; while (*p >= '0' && *p <= '9') wp = wp*10 + (*p++ - '0');
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

/* ---- async child reaping (libuv needs waitpid(WNOHANG) + SIGCHLD) ----
 * await(2) only works in the proc that forked, and it BLOCKS — useless for
 * WNOHANG and for a reaper pthread (a different proc). But the forking proc's
 * /proc/<pid>/wait FILE is readable by any same-user proc. So: fork() records
 * the child under its forking proc; per forking proc a reaper thread blocks
 * reading that wait file, parses waitmsgs into a zombie table, raise()s
 * SIGCHLD (libuv's handler is a self-pipe write — thread-agnostic). waitpid()
 * only consumes the table. Processes that never fork() keep the legacy
 * direct-await path. ponytail: max 4 forking procs, 32 zombies — nvim forks
 * from one loop thread; bump if a real workload outgrows it. */
extern void n9_semacquire(int *, int);
extern void n9_semrelease(int *, int);
extern long n9_tsemacquire(int *, long);
extern long n9_open(const char *, int);
extern long n9_pread(int, void *, long, long long);
extern int raise(int);
typedef unsigned long cc9_pthread_t;
extern int pthread_create(cc9_pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_detach(cc9_pthread_t);

static int zlock = 1;
static struct { int pid; int status; } ztab[32];
static int znum;
static int reap_sem;                      /* released once per new zombie */
static struct { int parent; int outstanding; int kick; int running; } rtab[4];

static void *cc9_reaper(void *arg) {
	int slot = (int)(long)arg;
	char path[32], w[256];
	/* "/proc/<pid>/wait" */
	{ char *d = path; const char *p = "/proc/"; while (*p) *d++ = *p++;
	  int v = rtab[slot].parent; char num[16]; int n = 0;
	  do { num[n++] = '0' + v % 10; v /= 10; } while (v);
	  while (n) *d++ = num[--n];
	  p = "/wait"; while (*p) *d++ = *p++; *d = 0; }
	for (;;) {
		while (rtab[slot].outstanding == 0)
			n9_semacquire(&rtab[slot].kick, 1);
		int fd = (int)n9_open(path, 0 /*OREAD*/);
		if (fd < 0) { rtab[slot].outstanding = 0; continue; }
		long n = n9_pread(fd, w, sizeof w - 1, -1);
		n9_close(fd);
		if (n <= 0) continue;                     /* interrupted note etc: retry */
		w[n] = 0;
		int st = 0, pid = cc9_wait_decode(w, &st);
		n9_semacquire(&zlock, 1);
		if (znum < (int)(sizeof ztab / sizeof ztab[0])) { ztab[znum].pid = pid; ztab[znum].status = st; znum++; }
		rtab[slot].outstanding--;
		n9_semrelease(&zlock, 1);
		n9_semrelease(&reap_sem, 1);
		raise(17 /*SIGCHLD*/);
	}
	return 0;
}

static void cc9_reap_forked(int childpid) {
	(void)childpid;
	int me = getpid();
	n9_semacquire(&zlock, 1);
	int slot = -1;
	for (int i = 0; i < 4; i++) if (rtab[i].running && rtab[i].parent == me) { slot = i; break; }
	if (slot < 0)
		for (int i = 0; i < 4; i++) if (!rtab[i].running) { slot = i; break; }
	if (slot >= 0) {
		if (!rtab[slot].running) {
			rtab[slot].parent = me; rtab[slot].outstanding = 0; rtab[slot].kick = 0;
			cc9_pthread_t t;
			if (pthread_create(&t, 0, cc9_reaper, (void *)(long)slot) == 0) {
				pthread_detach(t);
				rtab[slot].running = 1;
			}
		}
		if (rtab[slot].running) {
			rtab[slot].outstanding++;
			n9_semrelease(&rtab[slot].kick, 1);
		}
	}
	n9_semrelease(&zlock, 1);
}

static void cc9_reap_child_reset(void) {
	zlock = 1; znum = 0; reap_sem = 0;
	for (int i = 0; i < 4; i++) { rtab[i].running = 0; rtab[i].outstanding = 0; rtab[i].kick = 0; }
}

static int cc9_reaping(void) {
	for (int i = 0; i < 4; i++) if (rtab[i].running) return 1;
	return 0;
}

int    waitpid(int pid, int *s, int o) {
	if (!cc9_reaping()) {                          /* legacy: never fork()ed */
		char w[256];
		for (;;) {
			long n = n9_await(w, sizeof w - 1);
			if (n < 0) return -1;
			w[n] = 0;
			int st = 0, wp = cc9_wait_decode(w, &st);
			if (pid > 0 && wp != pid) continue;
			if (s) *s = st;
			return wp;
		}
	}
	for (;;) {
		n9_semacquire(&zlock, 1);
		int found = -1;
		for (int i = 0; i < znum; i++)
			if (pid <= 0 || ztab[i].pid == pid) { found = i; break; }
		int rp = 0;
		if (found >= 0) {
			rp = ztab[found].pid;
			if (s) *s = ztab[found].status;
			ztab[found] = ztab[znum - 1]; znum--;
		}
		int pending = 0;
		for (int i = 0; i < 4; i++) pending += rtab[i].outstanding;
		n9_semrelease(&zlock, 1);
		if (found >= 0) return rp;
		if (pending == 0) { errno = 10 /*ECHILD*/; return -1; }
		if (o & 1 /*WNOHANG*/) return 0;
		n9_tsemacquire(&reap_sem, 200);            /* woken per zombie; re-scan */
	}
}
int    wait4(int p, int *s, int o, void *r) { (void)r; return waitpid(p, s, o); }
int    wait(int *s) { return waitpid(-1, s, 0); }
/* kill over /proc: SIGKILL writes "kill" to ctl (forced); sig 0 probes for
 * existence; anything else posts an "interrupt" note. */
int    kill(int p, int s) {
	char path[40], *d = path;
	const char *pre = "/proc/"; while (*pre) *d++ = *pre++;
	{ int v = p; char num[16]; int n = 0;
	  do { num[n++] = '0' + v % 10; v /= 10; } while (v > 0);
	  while (n) *d++ = num[--n]; }
	const char *suf = (s == 9 /*SIGKILL*/) ? "/ctl" : (s == 0) ? "/status" : "/note";
	while (*suf) *d++ = *suf++;
	*d = 0;
	int fd = (int)n9_open(path, s == 0 ? 0 /*OREAD*/ : 1 /*OWRITE*/);
	if (fd < 0) { errno = 3 /*ESRCH*/; return -1; }
	int r = 0;
	if (s == 9)      r = n9_pwrite(fd, "kill", 4, -1) == 4 ? 0 : -1;
	else if (s != 0) r = n9_pwrite(fd, "interrupt", 9, -1) == 9 ? 0 : -1;
	n9_close(fd);
	return r;
}
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
extern long n9_sleep(long);
unsigned int sleep(unsigned int sec) { n9_sleep((long)sec * 1000); return 0; }
/* ioctl moved to fs.c (real TIOCGWINSZ over /env/LINES + /env/COLS) */
/* fcntl moved to poll.c (real O_NONBLOCK/FD_CLOEXEC; locks still "succeed") */
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

double difftime(long t1, long t0) { return (double)(t1 - t0); }

/* mktime — days-from-civil (Hinnant), the exact inverse of gmtime_r above.
 * local == UTC on Plan 9 (no tz db), so no offset. Normalizes the fields
 * (wday/yday, field overflow) through gmtime_r like mktime must. */
long mktime(struct cc9_tm *tm) {
	long y = tm->tm_year + 1900, m = tm->tm_mon + 1;
	long d = tm->tm_mday;
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (unsigned)((153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1);
	unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
	long days = era * 146097 + (long)doe - 719468;
	long t = days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
	gmtime_r(&t, tm);
	return t;
}

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

/* sockets: REAL now — over /net in net9.c (socket/bind/listen/accept/connect/
 * send/recv/sendto/recvfrom/shutdown/get·setsockopt/getsock·peername). */

/* dynamic loading: static binaries only. dlopen(NULL) = a handle to the main
 * program (standard semantics) — LLVM's MCJIT opens it at startup for symbol
 * search and treats a null return as a hard failure. Named libraries still fail
 * (can't load .so on 9front). dlsym always misses -> the JIT falls back to its
 * explicitly-registered symbols (all a static-linked JIT needs). */
void *dlopen(const char *p, int m) { (void)m; return p ? (void *)0 : (void *)0x1; }
int   dlclose(void *h) { (void)h; return 0; }
void *dlsym(void *h, const char *n) { (void)h; (void)n; return 0; }
char *dlerror(void) { return (char *)"cc9: no dynamic loading"; }
int   dladdr(const void *a, void *info) { (void)a; (void)info; return 0; }

/* ---- sockets: socketpair is REAL (Plan 9 pipes are full-duplex, exactly a
 * SOCK_STREAM pair — backs libuv's spawn-stdio and uv_pipe(2)); the rest of
 * the BSD socket surface is honest ENOSYS (networking on Plan 9 is /net). */
int socketpair(int af, int type, int proto, int sv[2]) {
	(void)af; (void)proto;
	extern int pipe2(int[2], int);
	return pipe2(sv, type & (0x1000|0x2000) /*SOCK_NONBLOCK|SOCK_CLOEXEC*/);
}
long sendmsg(int a, const void *m, int f) { (void)a;(void)m;(void)f; errno = 88; return -1; }
long recvmsg(int a, void *m, int f) { (void)a;(void)m;(void)f; errno = 88; return -1; }

/* ---- decorative termios (see include/termios.h) ---- */
struct cc9_termios { unsigned int i, o, c, l; unsigned char cc[32]; unsigned int is, os; };
int tcgetattr(int fd, struct cc9_termios *t) {
	(void)fd;
	memset(t, 0, sizeof *t);
	t->i = 0400 /*ICRNL*/; t->o = 0005 /*OPOST|ONLCR*/;
	t->c = 0260 /*CS8|CREAD*/; t->l = 0173 /*ISIG|ICANON|ECHO...*/;
	return 0;
}
int tcsetattr(int fd, int act, const struct cc9_termios *t) { (void)fd; (void)act; (void)t; return 0; }
void cfmakeraw(struct cc9_termios *t) { t->i = 0; t->o = 0; t->l = 0; }
int tcflush(int fd, int q) { (void)fd; (void)q; return 0; }
int tcdrain(int fd) { (void)fd; return 0; }

/* resolver: REAL now — /net/cs in net9.c */
unsigned int if_nametoindex(const char *n) { (void)n; return 0; }
char *if_indextoname(unsigned int i, char *buf) { (void)i; (void)buf; return 0; }
void *getgrnam(const char *n) { (void)n; return 0; }
void *getgrgid(unsigned g) { (void)g; return 0; }
int setgroups(int n, const unsigned *g) { (void)n; (void)g; return 0; }
int pthread_sigmask(int how, const unsigned long *set, unsigned long *old) { return sigprocmask(how, set, old); }
struct cc9_sched_param { int sched_priority; };
int sched_get_priority_min(int p) { (void)p; return 0; }
int sched_get_priority_max(int p) { (void)p; return 0; }
int pthread_getschedparam(unsigned long t, int *pol, struct cc9_sched_param *sp) { (void)t; if (pol) *pol = 0; if (sp) sp->sched_priority = 0; return 0; }
int pthread_setschedparam(unsigned long t, int pol, const struct cc9_sched_param *sp) { (void)t; (void)pol; (void)sp; return 0; }
int getpriority(int which, unsigned int who) { (void)which; (void)who; return 0; }
int setpriority(int which, unsigned int who, int prio) { (void)which; (void)who; (void)prio; return 0; }
int getgrgid_r(unsigned g, void *grp, char *buf, unsigned long n, void **res) { (void)g; (void)grp; (void)buf; (void)n; if (res) *res = 0; return 0; }
int setuid(unsigned u) { (void)u; return 0; }
int setgid(unsigned g) { (void)g; return 0; }
int getifaddrs(void **out) { if (out) *out = 0; errno = 38 /*ENOSYS*/; return -1; }
void freeifaddrs(void *l) { (void)l; }
char *ptsname(int fd) { (void)fd; return 0; }   /* no ptys on Plan 9 */
int ttyname_r(int fd, char *buf, unsigned long n) {
	/* MUST fail: the "tty" is an anonymous pipe from the terminal emulator.
	 * A name here makes libuv's uv_tty_init reopen it (losing the pipe) —
	 * failing sends it down the keep-the-fd blocking-writes fallback. */
	(void)fd; (void)buf; (void)n;
	return 38 /*ENOSYS*/;
}
const unsigned char in6addr_any[16];   /* matches struct in6_addr layout */
void *getprotobyname(const char *n) { (void)n; return 0; }
void *getprotobynumber(int p) { (void)p; return 0; }
int openpty(int *m, int *s, char *name, const void *t, const void *w) { (void)m;(void)s;(void)name;(void)t;(void)w; errno = 38; return -1; }
int forkpty(int *m, char *name, const void *t, const void *w) { (void)m;(void)name;(void)t;(void)w; errno = 38; return -1; }
int login_tty(int fd) { (void)fd; errno = 38; return -1; }
void tzset(void) {}   /* no tz db: local == UTC */
int cfsetispeed(void *t, unsigned s) { (void)t; (void)s; return 0; }
int cfsetospeed(void *t, unsigned s) { (void)t; (void)s; return 0; }
int killpg(int pgrp, int sig) { extern int kill(int, int); return kill(pgrp, sig); }
