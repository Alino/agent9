/* posix_llvm.c — the POSIX surface LLVM's Unix .inc files reference that cc9 didn't
 * already provide. Split by honesty:
 *   REAL  — backed by a Plan 9 syscall or a sane constant (getpagesize, getpid,
 *           getuid, sysconf, atoi, rand).
 *   STUB  — Plan 9 has no equivalent on any compiler compute path; returns a
 *           POSIX failure (-1/0) so callers take their error branch. fork/exec,
 *           record locks, signals, rlimits, sockets, mmap-of-files, passwd db.
 * None of the STUBs are reached when clang merely parses + emits an object. */
#include <stdint.h>
#include <stdarg.h>
#include <malloc.h>

typedef unsigned long n9size_t;
extern void *malloc(n9size_t);
extern void  free(void *);
extern void *memset(void *, int, n9size_t);
extern void *memcpy(void *, const void *, n9size_t);
extern n9size_t strlen(const char *);
extern long  n9_pread(int, void *, long, long long);
extern void  n9_exits(const char *);
extern int *__n9_errno(void);              /* per-thread errno (poll.c) */
#define errno (*__n9_errno())

/* ---- executable memory: an anonymous PROT_EXEC request (a JIT asking for W^X
 * memory: Mesa's rtasm vertex-fetch codegen, llvmpipe's shader JIT, ORC/MCJIT)
 * can't come from malloc — the kernel NX-enforces the heap. It must come from
 * segattach(SG_EXEC), which the wxallow kernel patch makes writable+executable.
 *
 * ONE pool, sub-allocated — NOT a segattach per call. Plan 9 gives a process a
 * small fixed segment table (NSEG), and text/data/bss/stack already use several;
 * a segment per JIT allocation exhausts it within a few shaders and segattach
 * starts failing ("LLVM ERROR: Unable to allocate section memory!"). Mesa's own
 * rtasm_execmem takes exactly this approach (one 10MB exec heap + a sub-allocator).
 * A range check against the pool also tells munmap to keep its hands off (this
 * memory is not heap and must never reach free()).
 * ponytail: bump allocator, no reuse — a long-running JIT that churns shaders can
 * exhaust the pool; add a free list (or segdetach the pool) if that shows up. */
extern void *n9_segattach(unsigned long attr, const char *cls, void *va, unsigned long len);
/* n9_open/n9_close are declared further down this file; cc9_have_wx below needs
 * them here. Signatures must match those (both return long). */
extern long n9_open(const char *, int);
extern long n9_close(int);
enum { SG_EXEC = 04000 };
#define EXEC_PGSZ  0x1000ul
#define EXEC_POOL_BYTES (64ul * 1024 * 1024)
/* A request at or above this gets its own segment instead of coming out of the
 * shared pool. SpiderMonkey's jit::ReserveProcessExecutableMemory asks for
 * MaxCodeBytesPerProcess (2044MB on 64-bit) in ONE call and then sub-allocates
 * it itself; that can't come from a 64MB pool, and it shouldn't — it wants to be
 * its own arena. Verified on 9front: segattach(SG_EXEC) serves 2044MB and is
 * demand-paged, so a reservation this big costs address space, not memory. */
#define EXEC_BIG_BYTES (8ul * 1024 * 1024)
/* Segments are a scarce, fixed-size per-process resource (NSEG), and text/data/
 * bss/stack already use several — hence one shared pool rather than a segattach
 * per call. Big requests are rare (SpiderMonkey makes exactly one), so a small
 * table is enough; exec_alloc fails rather than exhausting the segment table. */
#define EXEC_MAXBIG 4
static char *exec_pool, *exec_brk, *exec_end;
static struct { char *lo, *hi; } exec_big[EXEC_MAXBIG];
static int exec_nbig;

static int is_exec_map(void *p) {
	if (exec_pool && (char *)p >= exec_pool && (char *)p < exec_end) return 1;
	for (int i = 0; i < exec_nbig; i++)
		if ((char *)p >= exec_big[i].lo && (char *)p < exec_big[i].hi) return 1;
	return 0;
}

/* exec_alloc's state (exec_pool/exec_brk/exec_big/exec_nbig) is shared across all
 * cc9 threads (rfork(RFMEM)), and mmap(PROT_EXEC) is called concurrently — every
 * JIT thread allocates code memory. Unguarded, `r = exec_brk; exec_brk += sz;` is
 * a non-atomic read-modify-write: two threads hand back the SAME address, each
 * writes its code over the other's, and the loser jumps into clobbered or
 * never-written (demand-paged) pool memory — a fault with pc INSIDE the exec pool.
 * The `if (!exec_pool)` init raced the same way (double segattach, clobbered pool).
 * This is rare and cold (SpiderMonkey makes one big reservation), so a plain
 * semaphore is fine — no need for the malloc fast-path dance. */
extern void n9_semacquire(int *, int);   /* matches the decl further down this file */
extern void n9_semrelease(int *, int);
static int exec_lock = 1;

static void *exec_alloc(n9size_t len) {
	n9size_t sz = (len + EXEC_PGSZ - 1) & ~(EXEC_PGSZ - 1);
	if (!sz) sz = EXEC_PGSZ;

	n9_semacquire(&exec_lock, 1);
	void *ret = (void *)-1;

	if (sz >= EXEC_BIG_BYTES) {          /* dedicated segment, own arena */
		if (exec_nbig < EXEC_MAXBIG) {
			void *p = n9_segattach(SG_EXEC, "memory", 0, sz);
			if (p != (void *)-1) {
				exec_big[exec_nbig].lo = (char *)p;
				exec_big[exec_nbig].hi = (char *)p + sz;
				exec_nbig++;
				ret = p;
			}
		}
		n9_semrelease(&exec_lock, 1);
		return ret;
	}

	if (!exec_pool) {
		void *p = n9_segattach(SG_EXEC, "memory", 0, EXEC_POOL_BYTES);
		if (p == (void *)-1) { n9_semrelease(&exec_lock, 1); return (void *)-1; }
		exec_pool = exec_brk = (char *)p;
		exec_end = exec_pool + EXEC_POOL_BYTES;
	}
	if (exec_brk + sz <= exec_end) {      /* else: pool exhausted */
		ret = exec_brk;
		exec_brk += sz;
	}
	n9_semrelease(&exec_lock, 1);
	return ret;
}

/* Is W^X available? Lets a caller choose a JIT at RUNTIME rather than at build
 * time, so one binary runs on both a patched and a stock kernel.
 *
 * This reads the `wxallow` gate out of /env. It deliberately does NOT probe by
 * executing: per the patch's truth table, with wxallow=0 segattach(SG_EXEC)
 * still SUCCEEDS and the kernel merely strips SG_EXEC, so the only way to
 * observe it is to call into the page — which faults. A detector whose negative
 * answer is "process dies" is not a detector.
 *
 * plan9.ini variables are published in /env, so:
 *   patched kernel + wxallow=1   -> 1  (JIT)
 *   patched kernel + wxallow=0   -> 0  (interpreter — gate deliberately off)
 *   stock kernel (no wxallow)    -> 0  (interpreter)
 * The one case this gets wrong is wxallow=1 in plan9.ini on a kernel WITHOUT the
 * patch: /env says yes, the kernel strips SG_EXEC anyway, and JIT code faults on
 * first call. That is operator error (asserting a capability the kernel lacks)
 * and it cannot be distinguished from the good case without executing.
 *
 * Cached: the answer cannot change within a process. */
int cc9_have_wx(void) {
	static int cached = -1;
	if (cached >= 0) return cached;
	cached = 0;
	long fd = n9_open("/env/wxallow", 0);
	if (fd >= 0) {
		char b[8];
		long n = n9_pread((int)fd, b, sizeof b - 1, 0);
		n9_close((int)fd);
		if (n > 0) {
			b[n] = 0;
			/* /env values are not NUL-terminated decimal strings by
			 * convention; accept a leading '1' and reject '0'. */
			for (long i = 0; i < n; i++) {
				if (b[i] == '1') { cached = 1; break; }
				if (b[i] == '0') break;
			}
		}
	}
	return cached;
}

void *mmap(void *addr, n9size_t len, int prot, int flags, int fd, long off) {
	(void)addr;
	if ((prot & 4) && (flags & 0x20))     /* PROT_EXEC + MAP_ANON: JIT memory */
		return exec_alloc(len);
	if ((flags & 0x1) && fd >= 0) {       /* MAP_SHARED of a #g data fd: real
		 * cross-process shared memory over a named segment (shm9.c). Any
		 * other fd keeps the historical pread-copy behavior below. */
		extern void *cc9_shm_try_map(int, n9size_t, int *);
		int handled;
		void *p = cc9_shm_try_map(fd, len, &handled);
		if (handled) return p ? p : (void *)-1;
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
int munmap(void *p, n9size_t len) {
	extern int cc9_shm_unmap(void *, n9size_t);
	if (is_exec_map(p)) return 0;
	if (cc9_shm_unmap(p, len)) return 0;   /* named-segment mapping: detached */
	free(p);
	return 0;
}
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
/* JIT unwind-frame registration (__register_frame/__deregister_frame) now
 * comes from libunwind's UnwindLevel1-gcc-ext.o (build-runtime.sh) — the real
 * thing, not the old no-op stubs (JIT'd code still never throws; Rust's
 * panic machinery wanted the rest of that TU's entry points). */
int backtrace(void **b, int n) { (void)b; (void)n; return 0; }
char **backtrace_symbols(void *const *b, int n) { (void)b; (void)n; return 0; }
void backtrace_symbols_fd(void *const *b, int n, int fd) { (void)b; (void)n; (void)fd; }
/* ---- real posix_spawn: fork + file_actions + execve -----------------------
 * The header type is { void *__acts } pointing at this growable vector.
 * Applied IN ORDER in the child between fork and exec. attrs are ignored.
 * Child-side rules (the parent's pthreads don't exist here):
 *   - dup2 lands via n9_dup, then the target's poll-table slot is FORGOTTEN
 *     (cc9_poll_forget) so no stale O_CLOEXEC mark can close it at exec —
 *     POSIX says a dup2 file action clears CLOEXEC on the result.
 *   - close/open likewise bypass fs.c close() (its ring flush would wait on
 *     threads that don't exist in the child). */
typedef struct {
	int op;                     /* 0=dup2 1=close 2=open */
	int fd, newfd;
	char path[192];
	int oflag; unsigned mode;
} cc9_spawn_act;
typedef struct { int n, cap; cc9_spawn_act *v; } cc9_spawn_acts;

extern void cc9_poll_forget(int);
extern int open(const char *, int, ...);
extern int fork(void);
extern int execve(const char *, char *const[], char *const[]);
extern void _exit(int);

static cc9_spawn_act *spawn_act_new(void **slot) {
	cc9_spawn_acts *a = *slot;
	if (!a) {
		a = malloc(sizeof *a);
		if (!a) return 0;
		a->n = 0; a->cap = 8;
		a->v = malloc(a->cap * sizeof *a->v);
		if (!a->v) { free(a); return 0; }
		*slot = a;
	}
	if (a->n == a->cap) {
		cc9_spawn_act *nv = malloc(2 * a->cap * sizeof *nv);
		if (!nv) return 0;
		memcpy(nv, a->v, a->n * sizeof *nv);
		free(a->v); a->v = nv; a->cap *= 2;
	}
	return &a->v[a->n++];
}

int posix_spawn_file_actions_init(void **fa) { *fa = 0; return 0; }
int posix_spawn_file_actions_destroy(void **fa) {
	cc9_spawn_acts *a = *fa;
	if (a) { free(a->v); free(a); *fa = 0; }
	return 0;
}
int posix_spawn_file_actions_adddup2(void **fa, int fd, int newfd) {
	cc9_spawn_act *x = spawn_act_new(fa);
	if (!x) return 12 /*ENOMEM*/;
	x->op = 0; x->fd = fd; x->newfd = newfd;
	return 0;
}
int posix_spawn_file_actions_addclose(void **fa, int fd) {
	cc9_spawn_act *x = spawn_act_new(fa);
	if (!x) return 12;
	x->op = 1; x->fd = fd;
	return 0;
}
int posix_spawn_file_actions_addopen(void **fa, int fd, const char *path, int oflag, unsigned mode) {
	cc9_spawn_act *x = spawn_act_new(fa);
	if (!x) return 12;
	unsigned long i = 0;
	while (path[i] && i < sizeof x->path - 1) { x->path[i] = path[i]; i++; }
	x->path[i] = 0;
	x->op = 2; x->fd = fd; x->oflag = oflag; x->mode = mode;
	return 0;
}

static int spawn_apply(const cc9_spawn_acts *a) {
	if (!a) return 0;
	extern long n9_dup(int, int);
	for (int i = 0; i < a->n; i++) {
		const cc9_spawn_act *x = &a->v[i];
		if (x->op == 0) {
			if (n9_dup(x->fd, x->newfd) < 0) return -1;
			cc9_poll_forget(x->newfd);
		} else if (x->op == 1) {
			n9_close(x->fd);
			cc9_poll_forget(x->fd);
		} else {
			int got = open(x->path, x->oflag, x->mode);
			if (got < 0) return -1;
			if (got != x->fd) {
				if (n9_dup(got, x->fd) < 0) return -1;
				n9_close(got);
			}
			cc9_poll_forget(x->fd);
		}
	}
	return 0;
}

static int spawn_common(int *pid, const char *path, const void *fa,
                        char *const av[], char *const ev[]) {
	int kid = fork();
	if (kid < 0) return 11 /*EAGAIN*/;
	if (kid == 0) {
		if (spawn_apply(fa ? *(cc9_spawn_acts *const *)fa : 0) == 0)
			execve(path, av, ev);
		_exit(127);              /* POSIX-sanctioned late failure */
	}
	if (pid) *pid = kid;
	return 0;
}

int posix_spawn(int *pid, const char *p, const void *fa, const void *at, char *const av[], char *const ev[]) {
	(void)at;
	return spawn_common(pid, p, fa, av, ev);
}
int posix_spawnp(int *pid, const char *p, const void *fa, const void *at, char *const av[], char *const ev[]) {
	(void)at;
	for (const char *s = p; *s; s++)
		if (*s == '/') return spawn_common(pid, p, fa, av, ev);
	char b[256]; char *d = b;
	const char *pre = "/bin/"; while (*pre) *d++ = *pre++;
	const char *s = p; while (*s && d < b + sizeof b - 1) *d++ = *s++;
	*d = 0;
	return spawn_common(pid, b, fa, av, ev);
}
unsigned long getauxval(unsigned long t) { return t == 6 ? 4096 : 0; } /* AT_PAGESZ */
/* mprotect — Plan 9 has no per-page protection call: a segment's permissions are
 * fixed at segattach(2) time and nothing can change them afterwards. So the ONLY
 * honest answers are "you already have that" and "-1".
 *
 * Two callers, opposite consequences:
 *   W^X/JIT (LLVM's SectionMemoryManager, SpiderMonkey): mmap'd the region with
 *     PROT_EXEC, so it came from exec_alloc and is already RWX. A later
 *     mprotect(RX) only wants to DROP write as hardening; we can't, but the code
 *     still runs and behavior is unchanged. Return 0 — the justified case.
 *   Guard page (Rust std installs one below every thread stack, musl/glibc
 *     pthreads do the same): mprotect(p, n, PROT_NONE) expecting the next stack
 *     overflow to fault cleanly. Returning 0 here was a LIE with teeth — no guard
 *     exists, so an overflow silently scribbles over the adjacent heap instead of
 *     trapping. -1/ENOSYS tells std there is no guard; it has a fallback path,
 *     it has none for a phantom guard.
 * PROT_EXEC on memory that did NOT come from exec_alloc can't be granted either
 * (the kernel NX-enforces the heap) — jumping there faults, so fail and let a JIT
 * pick its interpreter. */
int mprotect(void *p, n9size_t len, int prot) {
	(void)len;
	if (prot == 0) { errno = 38 /*ENOSYS*/; return -1; }        /* PROT_NONE: no guard pages */
	if ((prot & 4 /*PROT_EXEC*/) && !is_exec_map(p)) { errno = 38; return -1; }
	return 0;   /* R/W (and X within the exec pool) are already true of the pages */
}
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

/* memmem — GNU extension (Mesa's brw_eu_validate uses it). Naive search: the
 * needles here are tiny and it runs at shader-compile time, not per-draw. */
void *memmem(const void *h, n9size_t hl, const void *n, n9size_t nl) {
	const unsigned char *hp = h, *np = n;
	if (nl == 0) return (void *)hp;
	if (hl < nl) return 0;
	for (n9size_t i = 0; i + nl <= hl; i++) {
		n9size_t j = 0;
		while (j < nl && hp[i+j] == np[j]) j++;
		if (j == nl) return (void *)(hp + i);
	}
	return 0;
}
int mkfifoat(int fd, const char *p, unsigned m) { (void)fd; (void)p; (void)m; return -1; }

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

/* ---- qsort: musl's smoothsort, ported 1:1 -------------------------------------
 * src/stdlib/qsort.c + src/stdlib/qsort_nr.c (MIT, Valentin Ochs 2011, integrated
 * by Rich Felker). Was an insertion sort here, justified by "qsort callers are
 * small" — false: SQLite sorts result sets and Servo sorts CSS rules / font
 * fallbacks / glyph runs. At n=1e5 the O(n^2) didn't fail, it HUNG (minutes).
 * Smoothsort is worst-case O(n log n), near-O(n) when mostly sorted, and — the
 * reason it's the right upstream to copy — strictly in-place: no alloca, no VLA,
 * no malloc, and it handles ANY element width (cycle() moves bytes through a
 * fixed 256-byte buffer). Exactly the constraint the old comment used to justify
 * insertion sort, already solved upstream.
 * Plan 9 divergences: musl's `ntz` comes from atomic.h (a_ctz_l) -> __builtin_ctzl
 * here; size_t -> n9size_t; weak_alias dropped (qsort_r is defined outright). */
#define ntz(x) __builtin_ctzl((unsigned long)(x))
typedef int (*cc9_cmpfun)(const void *, const void *, void *);

/* returns index of first bit set, excluding the low bit assumed to always
 * be set, starting from low bit of p[0] up through high bit of p[1] */
static inline int pntz(n9size_t p[2]) {
	if (p[0] != 1) return ntz(p[0] - 1);
	if (p[1]) return 8*sizeof(n9size_t) + ntz(p[1]);
	return 0;
}

static void cycle(n9size_t width, unsigned char *ar[], int n) {
	unsigned char tmp[256];
	n9size_t l;
	int i;

	if (n < 2) return;

	ar[n] = tmp;
	while (width) {
		l = sizeof(tmp) < width ? sizeof(tmp) : width;
		memcpy(ar[n], ar[0], l);
		for (i = 0; i < n; i++) {
			memcpy(ar[i], ar[i + 1], l);
			ar[i] += l;
		}
		width -= l;
	}
}

/* shl() and shr() need n > 0 */
static inline void shl(n9size_t p[2], int n) {
	if (n >= (int)(8 * sizeof(n9size_t))) {
		n -= 8 * sizeof(n9size_t);
		p[1] = p[0];
		p[0] = 0;
		if (!n) return;
	}
	p[1] <<= n;
	p[1] |= p[0] >> (sizeof(n9size_t) * 8 - n);
	p[0] <<= n;
}

static inline void shr(n9size_t p[2], int n) {
	if (n >= (int)(8 * sizeof(n9size_t))) {
		n -= 8 * sizeof(n9size_t);
		p[0] = p[1];
		p[1] = 0;
		if (!n) return;
	}
	p[0] >>= n;
	p[0] |= p[1] << (sizeof(n9size_t) * 8 - n);
	p[1] >>= n;
}

/* power-of-two length for working array so that we can mask indices and
 * not depend on any invariant of the algorithm for spatial memory safety.
 * the original size was just 14*sizeof(size_t)+1 */
#define AR_LEN  (16 * sizeof(n9size_t))
#define AR_MASK (AR_LEN - 1)

static void sift(unsigned char *head, n9size_t width, cc9_cmpfun cmp, void *arg, int pshift, n9size_t lp[]) {
	unsigned char *rt, *lf;
	unsigned char *ar[AR_LEN];
	int i = 1;

	ar[0] = head;
	while (pshift > 1) {
		rt = head - width;
		lf = head - width - lp[pshift - 2];

		if (cmp(ar[0], lf, arg) >= 0 && cmp(ar[0], rt, arg) >= 0) break;
		if (cmp(lf, rt, arg) >= 0) {
			ar[i++ & AR_MASK] = lf;
			head = lf;
			pshift -= 1;
		} else {
			ar[i++ & AR_MASK] = rt;
			head = rt;
			pshift -= 2;
		}
	}
	cycle(width, ar, i & AR_MASK);
}

static void trinkle(unsigned char *head, n9size_t width, cc9_cmpfun cmp, void *arg, n9size_t pp[2], int pshift, int trusty, n9size_t lp[]) {
	unsigned char *stepson, *rt, *lf;
	n9size_t p[2];
	unsigned char *ar[AR_LEN];
	int i = 1;
	int trail;

	p[0] = pp[0];
	p[1] = pp[1];

	ar[0] = head;
	while (p[0] != 1 || p[1] != 0) {
		stepson = head - lp[pshift];
		if (cmp(stepson, ar[0], arg) <= 0) break;
		if (!trusty && pshift > 1) {
			rt = head - width;
			lf = head - width - lp[pshift - 2];
			if (cmp(rt, stepson, arg) >= 0 || cmp(lf, stepson, arg) >= 0) break;
		}

		ar[i++ & AR_MASK] = stepson;
		head = stepson;
		trail = pntz(p);
		shr(p, trail);
		pshift += trail;
		trusty = 0;
	}
	if (!trusty) {
		cycle(width, ar, i & AR_MASK);
		sift(head, width, cmp, arg, pshift, lp);
	}
}

void __qsort_r(void *base, n9size_t nel, n9size_t width, cc9_cmpfun cmp, void *arg) {
	n9size_t lp[12*sizeof(n9size_t)];
	n9size_t i, size = width * nel;
	unsigned char *head, *high;
	n9size_t p[2] = {1, 0};
	int pshift = 1;
	int trail;

	if (!size) return;

	head = base;
	high = head + size - width;

	/* Precompute Leonardo numbers, scaled by element width */
	for (lp[0]=lp[1]=width, i=2; (lp[i]=lp[i-2]+lp[i-1]+width) < size; i++);

	while (head < high) {
		if ((p[0] & 3) == 3) {
			sift(head, width, cmp, arg, pshift, lp);
			shr(p, 2);
			pshift += 2;
		} else {
			if (lp[pshift - 1] >= (n9size_t)(high - head)) {
				trinkle(head, width, cmp, arg, p, pshift, 0, lp);
			} else {
				sift(head, width, cmp, arg, pshift, lp);
			}

			if (pshift == 1) {
				shl(p, 1);
				pshift = 0;
			} else {
				shl(p, pshift - 1);
				pshift = 1;
			}
		}

		p[0] |= 1;
		head += width;
	}

	trinkle(head, width, cmp, arg, p, pshift, 0, lp);

	while (pshift != 1 || p[0] != 1 || p[1] != 0) {
		if (pshift <= 1) {
			trail = pntz(p);
			shr(p, trail);
			pshift += trail;
		} else {
			shl(p, 2);
			pshift -= 2;
			p[0] ^= 7;
			shr(p, 1);
			trinkle(head - lp[pshift] - width, width, cmp, arg, p, pshift + 1, 1, lp);
			shl(p, 1);
			p[0] |= 1;
			trinkle(head - width, width, cmp, arg, p, pshift, 1, lp);
		}
		head -= width;
	}
}
void qsort_r(void *base, n9size_t nel, n9size_t width, cc9_cmpfun cmp, void *arg) {
	__qsort_r(base, nel, width, cmp, arg);
}
/* qsort_nr.c: the plain-qsort ABI is cmp(const void*, const void*) — NO arg —
 * so it goes through musl's wrapper, with the fn pointer smuggled in as `arg`. */
static int cc9_qsort_wrapper_cmp(const void *v1, const void *v2, void *cmp) {
	return ((int (*)(const void *, const void *))cmp)(v1, v2);
}
void qsort(void *base, n9size_t nel, n9size_t width, int (*cmp)(const void *, const void *)) {
	__qsort_r(base, nel, width, cc9_qsort_wrapper_cmp, (void *)cmp);
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
/* errno is hoisted to the top of this file (mprotect needs it). */
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
	{
		extern void cc9_poll_close_cloexec(void);
		cc9_poll_close_cloexec();
	}
	/* Userspace SG_CEXEC: the kernel ignores SG_CEXEC on #g named-segment
	 * attaches (verified: fork+exec children inherit the mapping and a fresh
	 * segattach fails "segments overlap"), so shm mappings are detached here,
	 * where CLOEXEC fds already die. A failed exec leaves them detached —
	 * acceptable: spawn paths _exit on exec failure. */
	{
		extern void cc9_shm_detach_all(void);
		cc9_shm_detach_all();
	}
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
/* --- libgen.h: POSIX path splitting ---------------------------------------
 * Both may modify the argument and return a pointer into it. The edge cases
 * (trailing slashes, all-slashes, no-slash) are the whole job; see
 * cc9/test/libgen_test.c for the table these were written against. */
char *dirname(char *s) {
	static char dot[] = ".", root[] = "/";
	char *e, *p;
	if (!s || !*s) return dot;
	e = s; while (*e) e++; e--;                  /* last char */
	while (e > s && *e == '/') e--;              /* strip trailing slashes */
	p = e;
	while (p > s && *p != '/') p--;              /* last slash at or before e */
	if (*p != '/') return dot;                   /* no slash: "usr" -> "." */
	while (p > s && *p == '/') p--;              /* collapse the run of slashes */
	if (p == s && *p == '/') return root;        /* "/usr" -> "/" */
	p[1] = 0;
	return s;
}
char *basename(char *s) {
	static char dot[] = ".", root[] = "/";
	char *e, *p;
	if (!s || !*s) return dot;
	e = s; while (*e) e++; e--;
	while (e > s && *e == '/') e--;
	if (e == s && *e == '/') return root;        /* "/" or "//" -> "/" */
	e[1] = 0;                                    /* chop trailing slashes */
	p = e;
	while (p > s && p[-1] != '/') p--;
	return p;
}

/* execlp: the varargs spelling of execvp. Collect argv off the stack (NULL
 * terminated) and hand it over. EXECL_MAXARG is a cap, not a POSIX limit —
 * overflowing it is a caller bug, so fail loudly rather than truncate. */
#define EXECL_MAXARG 64
int    execlp(const char *p, const char *a0, ...) {
	char *argv[EXECL_MAXARG];
	int n = 0;
	va_list ap;
	argv[n++] = (char *)a0;
	va_start(ap, a0);
	while (n < EXECL_MAXARG) {
		char *x = va_arg(ap, char *);
		argv[n++] = x;
		if (x == 0) break;
	}
	va_end(ap);
	if (n >= EXECL_MAXARG && argv[n-1] != 0) { errno = 7 /*E2BIG*/; return -1; }
	return execvp(p, argv);
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
static struct { int pid; int status; } ztab[128];
static int znum;
static int reap_sem;                      /* released once per new zombie */
static struct { int parent; int outstanding; int kick; int running; } rtab[8];

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
extern long n9_alarm(unsigned long);
/* alarm(2) POSIX: arm a SIGALRM in `s` seconds, return the seconds left on the
 * previous alarm. Used to discard `s` and return 0 — i.e. the alarm never fired
 * and the caller's timeout never happened — while setitimer right below already
 * did the real work. Same Plan 9 alarm(2) underneath (ms, returns the previous
 * remaining ms); alarm(0) cancels, in both worlds. Divergence: POSIX rounds the
 * return UP to whole seconds, so a sub-second remainder reports as 1, never 0
 * ("no alarm was pending"). */
unsigned int alarm(unsigned int s) {
	long prev = n9_alarm((unsigned long)s * 1000);
	if (prev <= 0) return 0;
	return (unsigned int)((prev + 999) / 1000);
}
/* setitimer over Plan 9 alarm(2): ITIMER_REAL arms n9_alarm(ms); a SIGALRM note
 * then fires (crt0.c note handler). Other timers are accepted as no-ops.
 * Local struct mirrors <sys/time.h> (this file avoids pulling system headers). */
struct itimerval { struct { long tv_sec, tv_usec; } it_interval, it_value; };
int setitimer(int which, const struct itimerval *nv, struct itimerval *ov) {
	(void)ov;
	if(which != 0) return 0;                       /* ITIMER_REAL only */
	unsigned long ms = nv ? (unsigned long)nv->it_value.tv_sec*1000 + (unsigned long)nv->it_value.tv_usec/1000 : 0;
	n9_alarm(ms);
	return 0;
}
extern long n9_sleep(long);
int    usleep(unsigned int us) { if (us) n9_sleep((us + 999) / 1000); return 0; }  /* ms floor: Plan 9 sleep(2) granularity */
unsigned int sleep(unsigned int sec) { n9_sleep((long)sec * 1000); return 0; }
/* ioctl moved to fs.c (real TIOCGWINSZ over /env/LINES + /env/COLS) */
/* fcntl moved to poll.c (real O_NONBLOCK/FD_CLOEXEC; locks still "succeed") */
/* chown family: Plan 9 has no chown. A file's uid is set at create(2) from the
 * creating process and only the host owner (`eve`, on the file server) may wstat
 * it — a normal process cannot, ever. Returning 0 told installers and tar/rsync
 * that ownership had been fixed when nothing happened. EPERM is the truth, and
 * it's exactly what these callers get on Unix as non-root, so their fallback
 * (warn, or ignore) is already written. */
int    chown(const char *p, unsigned u, unsigned g) { (void)p; (void)u; (void)g; errno = 1 /*EPERM*/; return -1; }
int    fchown(int fd, unsigned u, unsigned g) { (void)fd; (void)u; (void)g; errno = 1; return -1; }
int    lchown(const char *p, unsigned u, unsigned g) { (void)p; (void)u; (void)g; errno = 1; return -1; }

/* umask: Plan 9 has no umask — create(2) permissions are masked by the parent
 * directory's bits instead, in the file server, out of our reach. umask()'s
 * signature has no way to say "unsupported" (it cannot fail), so the one thing we
 * can be honest about is the VALUE: return the caller's own previous mask.
 * Fabricating 022 broke the standard save/restore idiom outright —
 *   old = umask(0); ... ; umask(old);
 * left the process at 022, a value it had never been set to.
 * ponytail: advisory — stored and round-tripped, NOT applied. cc9's create path
 * lives in fs.c; if a caller ever depends on the mask actually masking, fs.c's
 * open/create should &= ~cc9_umask_get() on the perm argument. */
static unsigned cc9_umask_val = 022;   /* the conventional startup default */
unsigned cc9_umask_get(void) { return cc9_umask_val; }
unsigned umask(unsigned m) { unsigned old = cc9_umask_val; cc9_umask_val = m & 0777; return old; }

/* gethostname: /dev/sysname holds the host's name (no trailing newline). Used to
 * write "" and report success, so every caller believed it ran on a nameless
 * host. POSIX: ENAMETOOLONG when the name doesn't fit. */
int    gethostname(char *n, unsigned long l) {
	if (!n || !l) { errno = 22 /*EINVAL*/; return -1; }
	char b[64];
	long fd = n9_open("/dev/sysname", 0 /*OREAD*/);
	if (fd < 0) { errno = 2 /*ENOENT*/; return -1; }
	long got = n9_pread((int)fd, b, (long)sizeof b - 1, 0);
	n9_close((int)fd);
	if (got < 0) { errno = 5 /*EIO*/; return -1; }
	while (got > 0 && (b[got-1] == '\n' || b[got-1] == ' ')) got--;
	if ((unsigned long)got >= l) { errno = 36 /*ENAMETOOLONG*/; return -1; }
	memcpy(n, b, (n9size_t)got);
	n[got] = 0;
	return 0;
}
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
/* timegm — identical here: local == UTC on Plan 9 (see mktime). */
long timegm(struct cc9_tm *tm) { return mktime(tm); }

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
void setpwent(void) {}
void *getpwent(void) { return 0; }     /* empty db: iteration sees nothing */
void endpwent(void) {}
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
/* sendmsg/recvmsg moved to net9.c and are REAL now (vectored I/O over /net;
 * only ancillary data — SCM_RIGHTS — is refused, with EOPNOTSUPP). */

/* ---- termios over /dev/consctl (see include/termios.h) ----------------------
 * Was decorative: cfmakeraw cleared ECHO in the caller's struct and tcsetattr
 * threw the struct away and returned success. The canonical caller is a PASSWORD
 * PROMPT (getpass(3), Python's getpass.getpass, ssh, sudo): read the termios,
 * clear ECHO, tcsetattr, read the line. Succeeding while doing nothing meant the
 * password echoed to the screen with the program believing echo was off — the
 * exact failure mode a lie-stub is built to produce, in the worst place.
 *
 * Plan 9 has a real knob: /dev/consctl takes "rawon"/"rawoff" (cons(3)). Raw is
 * the console's single combined ECHO+ICANON off switch — there is no separate
 * echo bit and no per-c_cc programming — so this maps ECHO|ICANON onto it and
 * nothing else. The rest of the struct is stored and handed back verbatim.
 *
 * PLAN 9 QUIRK: raw is a property of the OPEN consctl FILE, not of the console —
 * the kernel restores cooked mode when the last consctl fd closes. So the fd is
 * opened once and deliberately kept open for the life of the process; a
 * close-after-write would silently undo the very setting we just made. */
struct cc9_termios { unsigned int i, o, c, l; unsigned char cc[32]; unsigned int is, os; };
extern int isatty(int);
static long cc9_consctl_fd = -1;
static struct cc9_termios cc9_tio;
static int cc9_tio_valid;

static int cc9_consctl(const char *cmd) {
	if (cc9_consctl_fd < 0) cc9_consctl_fd = n9_open("/dev/consctl", 1 /*OWRITE*/);
	if (cc9_consctl_fd < 0) return -1;      /* no console here (a pipe from the
	                                         * terminal emulator has no consctl) */
	long n = (long)strlen(cmd);
	return n9_pwrite((int)cc9_consctl_fd, cmd, n, -1LL) == n ? 0 : -1;
}

int tcgetattr(int fd, struct cc9_termios *t) {
	if (!t) { errno = 22 /*EINVAL*/; return -1; }
	if (!isatty(fd)) { errno = 25 /*ENOTTY*/; return -1; }
	if (cc9_tio_valid) { *t = cc9_tio; return 0; }   /* report what we actually set */
	memset(t, 0, sizeof *t);
	/* the honest default: a fresh Plan 9 console is cooked — it echoes and does
	 * line editing (backspace/^U/^D) and ^ | delete post notes. */
	t->i = 0400 /*ICRNL*/; t->o = 0005 /*OPOST|ONLCR*/;
	t->c = 0260 /*CS8|CREAD*/; t->l = 0173 /*ISIG|ICANON|ECHO...*/;
	return 0;
}
int tcsetattr(int fd, int act, const struct cc9_termios *t) {
	(void)act;   /* TCSANOW/TCSADRAIN/TCSAFLUSH: Plan 9 has no output queue to
	              * drain and no input queue we may flush behind cons's back. */
	if (!t) { errno = 22 /*EINVAL*/; return -1; }
	if (!isatty(fd)) { errno = 25 /*ENOTTY*/; return -1; }
	/* raw == neither echo nor line discipline. Either bit cleared means the caller
	 * wants cons out of the way; only both set is genuinely cooked. */
	int raw = !(t->l & 0010 /*ECHO*/) || !(t->l & 0002 /*ICANON*/);
	if (cc9_consctl(raw ? "rawon" : "rawoff") < 0) { errno = 25 /*ENOTTY*/; return -1; }
	cc9_tio = *t; cc9_tio_valid = 1;
	return 0;
}
/* cfmakeraw — musl src/termios/cfmakeraw.c, 1:1. It only edits the caller's
 * struct (no syscall); tcsetattr above is what makes it true. */
void cfmakeraw(struct cc9_termios *t) {
	t->i &= ~(0001|0002|0010|0040|0100|0200|0400|02000);  /* IGNBRK BRKINT PARMRK ISTRIP INLCR IGNCR ICRNL IXON */
	t->o &= ~0001;                                        /* OPOST */
	t->l &= ~(0010|0100|0002|0001|0100000);               /* ECHO ECHONL ICANON ISIG IEXTEN */
	t->c &= ~(0060|0400);                                 /* CSIZE PARENB */
	t->c |= 0060;                                         /* CS8 */
	t->cc[6 /*VMIN*/] = 1;
	t->cc[5 /*VTIME*/] = 0;
}
int tcflush(int fd, int q) { (void)fd; (void)q; return 0; }
int tcdrain(int fd) { (void)fd; return 0; }

/* resolver: REAL now — /net/cs in net9.c */
unsigned int if_nametoindex(const char *n) { (void)n; return 0; }
char *if_indextoname(unsigned int i, char *buf) { (void)i; (void)buf; return 0; }
void *getgrnam(const char *n) { (void)n; return 0; }
void *getgrgid(unsigned g) { (void)g; return 0; }
int setgroups(int n, const unsigned *g) { (void)n; (void)g; return 0; }
int pthread_sigmask(int how, const unsigned long *set, unsigned long *old) { return sigprocmask(how, set, old); }
/* ---- scheduling priority over /proc/<pid>/ctl -------------------------------
 * REAL: Plan 9 proc(3) accepts "pri N" on the ctl file, N in 0..19 (Nrq run
 * queues; higher = more favoured, same direction as POSIX). cc9's pthread_t IS
 * the Plan 9 pid (include/pthread.h), so a thread's priority is directly
 * addressable — no tid->pid table needed.
 * Divergences: Plan 9 has ONE scheduler, so the POSIX policy argument has no
 * meaning and is ignored (rather than rejected — SCHED_OTHER is what you get);
 * and the kernel refuses N > PriNormal(10) unless you are eve, which surfaces
 * here as EPERM. Priority cannot be READ back (ctl is write-only and /proc's
 * status file doesn't carry it), so pthread_getschedparam fails honestly. */
struct cc9_sched_param { int sched_priority; };
static int cc9_proc_ctl(int pid, const char *cmd) {
	char path[40], *d = path;
	const char *pre = "/proc/"; while (*pre) *d++ = *pre++;
	{ int v = pid; char num[16]; int n = 0;
	  do { num[n++] = '0' + v % 10; v /= 10; } while (v > 0);
	  while (n) *d++ = num[--n]; }
	const char *suf = "/ctl"; while (*suf) *d++ = *suf++;
	*d = 0;
	int fd = (int)n9_open(path, 1 /*OWRITE*/);
	if (fd < 0) return -1;
	long n = (long)strlen(cmd);
	int r = n9_pwrite(fd, cmd, n, -1LL) == n ? 0 : -1;
	n9_close(fd);
	return r;
}
int sched_get_priority_min(int p) { (void)p; return 0; }
int sched_get_priority_max(int p) { (void)p; return 19; }   /* Nrq-1 */
int pthread_getschedparam(unsigned long t, int *pol, struct cc9_sched_param *sp) {
	(void)t; (void)pol; (void)sp;
	return 38 /*ENOSYS*/;   /* Plan 9 exposes no way to read a proc's priority */
}
int pthread_setschedparam(unsigned long t, int pol, const struct cc9_sched_param *sp) {
	(void)pol;
	if (!sp || sp->sched_priority < 0 || sp->sched_priority > 19) return 22 /*EINVAL*/;
	char cmd[12], *d = cmd;
	*d++ = 'p'; *d++ = 'r'; *d++ = 'i'; *d++ = ' ';
	{ int v = sp->sched_priority; char num[4]; int n = 0;
	  do { num[n++] = '0' + v % 10; v /= 10; } while (v > 0);
	  while (n) *d++ = num[--n]; }
	*d = 0;
	return cc9_proc_ctl((int)t, cmd) == 0 ? 0 : 1 /*EPERM: pri > PriNormal needs eve*/;
}
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
/* -fstack-protector support. The compiler emits a read of __stack_chk_guard
 * into each protected frame and compares on return, calling __stack_chk_fail on
 * a mismatch. Both symbols are the SSP ABI and must exist for any TU built with
 * -fstack-protector* to link (SpiderMonkey builds with -fstack-protector-strong
 * by default).
 *
 * The guard is seeded from /dev/random by __cc9_ssp_init, called from __cc9_run
 * before .init_array and before main — it must be set before any protected
 * frame is entered, or the epilogue would compare against a value the prologue
 * never saw. The initial value is a non-zero fallback for the window before
 * that (and if /dev/random can't be read); it contains a NUL byte on purpose,
 * to stop string-based overflows from writing the canary intact. */
unsigned long __stack_chk_guard = 0x00000aff0d0a0000UL;

/* n9_open/n9_pread/n9_pwrite/n9_close are already declared at file scope above. */
void __cc9_ssp_init(void) {
	unsigned long v;
	long fd = n9_open("/dev/random", 0);
	if (fd >= 0) {
		if (n9_pread((int)fd, &v, (long)sizeof v, -1LL) == (long)sizeof v && v != 0)
			__stack_chk_guard = v;
		n9_close((int)fd);
	}
}

void __stack_chk_fail(void) {
	static const char m[] = "cc9: stack smashing detected\n";
	extern void abort(void);
	n9_pwrite(2, m, sizeof m - 1, -1LL);
	abort();
}

void tzset(void) {}   /* no tz db: local == UTC */
/* The POSIX zone globals that go with the no-op tzset above. Kept consistent
 * with it: local == UTC, so no offset and no DST. Plan 9's real zone lives in
 * /env/timezone; wiring these to it is a separate job. */
static char tzname_utc[] = "UTC";
char *tzname[2] = { tzname_utc, tzname_utc };
long timezone = 0;
int daylight = 0;
int cfsetispeed(void *t, unsigned s) { (void)t; (void)s; return 0; }
int cfsetospeed(void *t, unsigned s) { (void)t; (void)s; return 0; }
int killpg(int pgrp, int sig) { extern int kill(int, int); return kill(pgrp, sig); }
