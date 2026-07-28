/* shm9.c — cross-process anonymous shared memory over 9front named global
 * segments (segment(3), the #g kernel device). See include/sys/shm9.h for the
 * model. Built for Ladybird's Core::AnonymousBuffer (every bitmap that crosses
 * a browser process boundary), generic to any port.
 *
 * devsegment facts this leans on (9front /sys/src/9/port/devsegment.c):
 *   - create("#g/<name>", DMDIR) makes a named segment; writing
 *     "va 0x<addr> 0x<len>" to its ctl file fixes the VA and length ONCE.
 *   - segattach(attr, "<name>", 0, 0) attaches it; the kernel ignores the
 *     va/len arguments and uses the segment's own — so it appears at the SAME
 *     VA in every process. That makes cross-process VA collisions our problem:
 *     the address must be free in EVERY attacher, hence the private VA region
 *     below, far from anything brk/stack/exec-pool will ever hand out.
 *   - remove("#g/<name>") blocks new attaches; the memory lives until the
 *     last attacher exits/detaches.
 *   - the kernel caps named segments at ~100 SYSTEM-WIDE. Phase A (segment
 *     per buffer) fits bring-up; the wire format {name, offset, len} already
 *     carries an offset so a Phase B pool allocator (many buffers carved from
 *     few segments) changes no protocol. The cap failure is a loud create/ctl
 *     error, never corruption.
 *
 * VA allocation: base 0x0000300000000000 (48 TiB; USTKTOP is ~128 TiB, brk
 * heap sits near the bottom of the address space, the exec pool is kernel-
 * chosen). Each creating process owns a 1 GiB slab keyed on (pid & 0xFFFF) so
 * two live creators can't mint colliding VAs; within the slab a bump pointer
 * starts at a clock-derived page offset to de-correlate recycled pids.
 * ponytail: bump-only, no VA reuse within a creator — 1 GiB of segment VA per
 * process outlives any Phase A workload; the Phase B pool is the real fix. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/shm9.h>

extern long n9_open(const char *, int);
extern long n9_close(int);
extern long n9_create(const char *, int, unsigned long);
extern long n9_remove(const char *);
extern long n9_pread(int, void *, long, long long);
extern long n9_pwrite(int, const void *, long, long long);
extern long n9_fd2path(int, char *, int);
extern void *n9_segattach(unsigned long attr, const char *cls, void *va, unsigned long len);
extern long n9_segdetach(void *);
extern void n9_semacquire(int *, int);
extern void n9_semrelease(int *, int);
extern int cc9_errno_from_errstr(void);
extern int getpid(void);
extern long n9_errstr(char *, unsigned long);

/* CC9_SHM_TRACE diagnostic (off by default): one line per create/import/attach
 * to fd 2, so a browser run's stderr shows every segment name + VA + segattach
 * result. ponytail: pure diagnostic; drop once the #g attach bug is fixed. */
static void shm_trace(const char *op, const char *name, unsigned long va,
                      unsigned long len, long res, const char *err) {
	static int on = -1;
	if (on < 0) on = getenv("CC9_SHM_TRACE") ? 1 : 0;
	if (!on) return;
	char m[224];
	int n = snprintf(m, sizeof m, "SHM9 %-6s pid=%d name=%s va=0x%lx len=0x%lx res=%ld%s%s\n",
	                 op, getpid(), name ? name : "-", va, len, res,
	                 err && err[0] ? " err=" : "", err ? err : "");
	n9_pwrite(2, m, n, -1);
}

enum {
	OREAD   = 0,
	ORDWR   = 2,
};
#define DMDIR 0x80000000ul

#define SHM9_PREFIX "shm."
#define SHM9_BASE   0x0000300000000000ul
#define SHM9_SLAB   (1ul << 30)
#define SHM9_PAGE   4096ul

static int shm_lock = 1;        /* binary sem guarding all statics below */
static unsigned long slab_next; /* next VA within our slab (0 = uninitialized) */
static unsigned long slab_base; /* base of the slab slab_next lives in (set once, with slab_next) */
static unsigned seq;            /* per-process name counter */

/* per-process attach table: devsegment refuses a second attach of a segment
 * already mapped in this process, so double-mmap of the same buffer (dup'd fd,
 * two AnonymousBuffer views) must be satisfied from here with a refcount. */
typedef struct {
	char name[CC9_SHM_NAMELEN];
	unsigned long va, len;
	int refs;
	int pinned;          /* attached for slot-refcount access, not for an mmap */
	unsigned long lru;   /* tick of last use; 0 = never idle-reclaimable */
} shm_map;
#define SHM_MAXMAP 512
static shm_map maps[SHM_MAXMAP];
/* How many IDLE (refs==0) pools may stay attached. Detaching the moment a pool
 * falls to zero is what let a sibling thread yank a pool another sibling was
 * still reading (cc9 threads are rfork(RFMEM) procs sharing one address space,
 * and the refcount tracks mmap CALLS, not the raw pointers consumers hold).
 * Never detaching is the other extreme and retains every pool's touched pages
 * for the process lifetime -- a 256 MiB pool each, which exhausts a small box on
 * a heavy page. Keeping a few idle pools gives a wide grace window while
 * bounding retention. */
#define SHM_MAXIDLE 3
static unsigned long shm_tick;

/* ---- Phase B pool allocator ----
 * Plan 9 caps a process at NSEG (~12) attached segments (Proc.seg[]); one #g
 * segment per bitmap blows that in ~6 backing stores (see parity/deferrals.md).
 * So every buffer is sub-allocated from ONE big per-process pool segment
 * (#g/shmp.<pid>): the whole pool costs the creator AND each receiver exactly
 * ONE segattach regardless of buffer count. The {name,offset,len} wire triple
 * (offset was always 0 in Phase A) now carries the buffer's offset in the pool.
 *
 * A buffer's offset within the pool must survive two hazards, so it is recorded
 * BOTH ways and read back table-first, seek-second:
 *   - fd-number table (buf_*): reliable whenever export/mmap see the SAME fd
 *     create/import returned (the common case). But the IPC layer clones the fd
 *     (dup) before cc9_shm_export sees it, so the clone misses the table.
 *   - fd SEEK POSITION (shm_*_off): dup(2) shares the file offset, so it
 *     survives the clone. But not every kernel lets you seek a #g data file, and
 *     any read/write on the fd would move it — so it's the fallback, not primary.
 * The union is robust: the table catches the same-fd path (incl. kernels where
 * #g seek is a no-op), seek catches the dup'd-and-re-exported path. */
extern long n9_seek(long long *, int, long long, int);
static void shm_set_off(int fd, unsigned long off) {
	long long ret;
	n9_seek(&ret, fd, (long long)off, 0 /*SEEK_SET*/);
}
static unsigned long shm_seek_off(int fd) {
	long long ret = 0;
	if (n9_seek(&ret, fd, 0, 1 /*SEEK_CUR*/) < 0) return 0;
	return (unsigned long)ret;
}

typedef struct {
	int fd;
	unsigned long off, len;
	int gen;
	unsigned long base;   /* VA this buffer's POOL is attached at, for slot_refs */
} shm_buf;   /* guarded by shm_lock */
#define SHM_MAXBUF 4096
static shm_buf bufs[SHM_MAXBUF];
static int bufs_live;   /* fast-path: skip the O(N) scan in close() when 0 */

/* ---- per-slot CROSS-PROCESS refcount ----
 *
 * A pool slot's lifetime is not the creator's business alone. POSIX keeps an
 * anonymous buffer alive while ANY process holds an fd or a mapping of it; here
 * the buffer is a sub-range of one shared segment, and the peer's fd refers to
 * the whole pool, so the kernel cannot tell us when a slot goes unreferenced.
 *
 * Without that, the free-list below recycled a slot the moment the CREATOR
 * closed its fd — while the receiver was still mapping it. The receiver's
 * pointer then aliased whatever was carved there next, and the two buffers
 * silently shared memory. That is what made github.com's 404 page render every
 * one of its images as the same picture at seven different strides (each <img>
 * read one buffer through its own width) — visible as horizontal streaks.
 *
 * So count references where all parties CAN see them: in the pool itself. The
 * first SHM9_HDR bytes of every pool are a u32 per pool page; a slot's count
 * lives at its first page's index. Every process that touches the slot has the
 * pool attached, so the counter is genuinely shared, and the updates are atomic.
 *
 * Counted: each fd (create, import) and each mmap. Released: each close of a
 * table fd, each munmap. A slot returns to the free-list only at zero.
 *
 * A count alone is not enough, because there is a moment when NOBODY holds the
 * buffer and it is still very much alive: after the sender has written the
 * message naming it and dropped its own fd, but before the receiver has read
 * that message and imported. SCM_RIGHTS keeps an fd alive in exactly that gap;
 * here nothing did, and the allocator handed the slot straight out again (seen
 * in a CC9_SHM_TRACE: the same offset created twice, then imported twice).
 * So export MARKS the slot in flight, and import clears the mark once it holds
 * its own reference. The mark is the counter's top bit, so "unreferenced" is
 * simply the whole word being zero.
 *
 * ponytail: a process that dies or execs without closing leaks its counts, and
 * a message that is never decoded leaks its in-flight mark, so those slots are
 * never reused — the pool fills and a new one is minted, exactly today's
 * behaviour. Leaking a slot is the safe direction; aliasing is not. */
#define SHM9_HDR (SHM9_POOL / SHM9_PAGE * sizeof(unsigned))   /* 256 KiB of a 256 MiB pool */
#define SLOT_INFLIGHT 0x80000000u

static unsigned *slot_refs(unsigned long base, unsigned long off) {
	if (!base) return 0;
	return (unsigned *)(base + (off / SHM9_PAGE) * sizeof(unsigned));
}
static void slot_ref(unsigned long base, unsigned long off) {
	unsigned *p = slot_refs(base, off);
	if (p) __atomic_fetch_add(p, 1u, __ATOMIC_SEQ_CST);
}
static void slot_unref(unsigned long base, unsigned long off) {
	unsigned *p = slot_refs(base, off);
	if (p && (__atomic_load_n(p, __ATOMIC_SEQ_CST) & ~SLOT_INFLIGHT) > 0)
		__atomic_fetch_sub(p, 1u, __ATOMIC_SEQ_CST);
}
static void slot_mark_inflight(unsigned long base, unsigned long off) {
	unsigned *p = slot_refs(base, off);
	if (p) __atomic_fetch_or(p, SLOT_INFLIGHT, __ATOMIC_SEQ_CST);
}
static void slot_clear_inflight(unsigned long base, unsigned long off) {
	unsigned *p = slot_refs(base, off);
	if (p) __atomic_fetch_and(p, ~SLOT_INFLIGHT, __ATOMIC_SEQ_CST);
}
/* Nonzero while ANYONE holds this slot — a reference or a message in flight. */
static unsigned slot_count(unsigned long base, unsigned long off) {
	unsigned *p = slot_refs(base, off);
	return p ? __atomic_load_n(p, __ATOMIC_SEQ_CST) : 0;
}
/* Attach a pool just to reach that header (defined with the mmap path below). */
static unsigned long pool_pin(const char *name);
static unsigned long pool_pin_locked(const char *name);

/* Per-pool free-list: freed [off,len) slots in the CURRENT pool, reused before
 * bumping pool_off. Without this the bump cursor climbs forever across a long
 * reused-process session (test-web never respawns the Compositor/WebContent, so
 * per-test canvas backing stores churned pool after 256 MiB pool until the 1 GiB
 * pid VA slab exhausted -> cc9_shm_create returned -1 -> a fatal VERIFY -> the
 * Compositor crashed ~100 tests in). First-fit + tail-split, no coalescing; the
 * "Phase B pool" the create/free comments below defer. All under shm_lock.
 *
 * An entry here is a CANDIDATE, not a free slot: it is only handed out once its
 * cross-process refcount above reads zero. */
#define SHM_FREEMAX 512
typedef struct { unsigned long off, len; } shm_free;
static shm_free freelist[SHM_FREEMAX];
static int free_n;
static int pool_gen;   /* bumped on every new-pool mint; each buffer carries its mint gen so a
                        * freed old-pool buffer is never reused as an offset into the new pool */

/* Record fd->off both in the table (primary) and the fd seek position
 * (dup-safe fallback). The table MUST be cleared on close (cc9_shm_forget_fd,
 * below) or a recycled fd number inherits a dead buffer's offset — the IPC
 * layer dup()s the buffer fd before export, and dup returns the lowest free
 * number, which readily lands on a just-closed shm fd. */
/* len/gen record what cc9_shm_forget_fd needs to return the slot to the pool
 * free-list: len = the buffer's page-rounded size, gen = the pool it was carved
 * from. Imports pass gen=-1 (NOT ours — never reclaimed into our pool). */
static void shm_set_off_both(int fd, unsigned long off, unsigned long len, int gen, unsigned long base) {
	shm_set_off(fd, off);                       /* seek: outside the lock (syscall) */
	n9_semacquire(&shm_lock, 1);
	shm_buf *b = 0, *slot = 0;
	for (int i = 0; i < SHM_MAXBUF; i++) {
		if (bufs[i].fd == fd + 1) { b = &bufs[i]; break; }   /* +1: 0 = empty */
		if (!bufs[i].fd && !slot) slot = &bufs[i];
	}
	if (!b && slot) { slot->fd = fd + 1; b = slot; bufs_live++; }
	if (b) { b->off = off; b->len = len; b->gen = gen; b->base = base; }
	n9_semrelease(&shm_lock, 1);
}
/* offset for an fd: table first (reliable), then the dup-shared seek position. */
static unsigned long shm_get_off(int fd) {
	n9_semacquire(&shm_lock, 1);
	unsigned long off = 0; int hit = 0;
	for (int i = 0; i < SHM_MAXBUF; i++)
		if (bufs[i].fd == fd + 1) { off = bufs[i].off; hit = 1; break; }
	n9_semrelease(&shm_lock, 1);
	return hit ? off : shm_seek_off(fd);
}
/* close(2) hook: forget this fd's pool offset so a later fd that recycles the
 * number can't read a dead buffer's offset. Safe (and cheap via bufs_live) to
 * call for every close, shm fd or not. */
void cc9_shm_forget_fd(int fd) {
	if (bufs_live == 0) return;
	n9_semacquire(&shm_lock, 1);
	for (int i = 0; i < SHM_MAXBUF; i++)
		if (bufs[i].fd == fd + 1) {
			/* This fd's reference is gone, whichever side of the pool it was
			 * on — created here or imported from a peer. */
			slot_unref(bufs[i].base, bufs[i].off);
			/* Offer this buffer's slot to the current pool's free-list so its
			 * offset is reused instead of leaked. Only OUR pool buffers (gen>=0
			 * from create; imports carry gen=-1) belonging to the CURRENT pool
			 * (an already-superseded pool self-releases via its own attach
			 * refcount when its last buffer's fd closes). Recorded exactly once:
			 * only the create fd is in the table; the IPC layer's export dup and
			 * imported fds are either absent or gen=-1. The slot is a CANDIDATE
			 * from here: cc9_shm_create hands it out only once every other
			 * process has dropped it too (slot_count == 0). */
			if (bufs[i].gen == pool_gen && bufs[i].len && free_n < SHM_FREEMAX) {
				freelist[free_n].off = bufs[i].off;
				freelist[free_n].len = bufs[i].len;
				free_n++;
			}
			bufs[i].fd = 0; bufs_live--; break;
		}
	n9_semrelease(&shm_lock, 1);
}

#define SHM9_POOL (256ul << 20)     /* 256 MiB per pool; demand-paged, so cheap */
static char pool_name[CC9_SHM_NAMELEN];  /* "" until the current pool is created */
static unsigned long pool_off;           /* bump cursor within the current pool */
static unsigned long pool_base;          /* VA our own pool is attached at (slot refcounts live there) */
static int pool_fd = -1;                 /* held open process-lifetime: keeps the #g dir + memory alive */
static int pool_pid;                     /* pid that owns pool_*: detects a fork WITHOUT exec */

static unsigned long page_round(unsigned long n) {
	return (n + SHM9_PAGE - 1) & ~(SHM9_PAGE - 1);
}

/* "#g/<name>/data" -> name. Returns 0 if path is not a #g data file. */
static int path_to_name(const char *path, char *name) {
	if (strncmp(path, "#g/", 3) != 0) return 0;
	const char *p = path + 3, *slash = strchr(p, '/');
	if (!slash || strcmp(slash, "/data") != 0) return 0;
	unsigned long n = (unsigned long)(slash - p);
	if (n == 0 || n >= CC9_SHM_NAMELEN) return 0;
	memcpy(name, p, n);
	name[n] = 0;
	return 1;
}

/* Read a segment's ctl ("va 0x... 0x..." per segment(3)) -> va, len. */
static int read_ctl(const char *name, unsigned long *va, unsigned long *len) {
	char path[64], buf[96];
	snprintf(path, sizeof path, "#g/%s/ctl", name);
	long fd = n9_open(path, OREAD);
	if (fd < 0) return -1;
	long n = n9_pread((int)fd, buf, sizeof buf - 1, 0);
	n9_close((int)fd);
	if (n <= 0) return -1;
	buf[n] = 0;
	char *p = buf;
	while (*p && strncmp(p, "va ", 3) != 0) p++;
	if (!*p) return -1;
	p += 3;
	*va = strtoul(p, &p, 0);
	*len = strtoul(p, 0, 0);
	return (*va && *len) ? 0 : -1;
}

/* Fresh VA for a new segment of len bytes, from our pid-keyed slab.
 *
 * The slab BASE is cached (slab_base) the first time any thread allocates, and
 * every later allocation — including from rfork(RFMEM) SIBLING threads, which
 * share these statics but each have a DIFFERENT getpid() — reuses it. Without
 * this, siblings recomputed `slab` from their own pid while sharing the one
 * slab_next cursor, so the bound check compared a cursor in thread A's slab
 * against thread B's slab end. Under heavy concurrent allocation (a page
 * decoding many images across sibling threads) that handed out VAs that
 * overlapped an already-attached segment, and segattach failed "virtual memory
 * allocation failed" -> cc9_shm_create ENOENT -> the consumer's create_shareable
 * aborted. One process = one coherent slab, shared by all its threads.
 * Cross-PROCESS isolation is preserved: a different process's first allocator
 * has a different pid, hence a different base (the original design intent). */
static unsigned long va_alloc(unsigned long len) {
	if (slab_base == 0) {
		slab_base = SHM9_BASE + (((unsigned long)getpid() & 0xFFFFul) << 30);
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		/* start somewhere in the first quarter of the slab so recycled pids
		 * rarely mint the VA a dead predecessor's still-attached segment holds */
		slab_next = slab_base + (page_round((unsigned long)ts.tv_nsec) % (SHM9_SLAB / 4));
	}
	if (slab_next + len > slab_base + SHM9_SLAB) {
		errno = ENOMEM;          /* slab exhausted — loud, see ponytail above */
		return 0;
	}
	unsigned long va = slab_next;
	slab_next += len;
	return va;
}

/* Create #g/<name> fixed at <va> for <len> bytes; return a fd on its data file
 * (or -1, errno set, dir cleaned up). */
static int create_seg(const char *name, unsigned long va, unsigned long len) {
	char path[64];
	snprintf(path, sizeof path, "#g/%s", name);
	long dirfd = n9_create(path, OREAD, DMDIR | 0700);
	if (dirfd < 0) { errno = cc9_errno_from_errstr(); return -1; }
	n9_close((int)dirfd);

	snprintf(path, sizeof path, "#g/%s/ctl", name);
	long ctl = n9_open(path, ORDWR);
	if (ctl < 0) { errno = cc9_errno_from_errstr(); goto fail_rm; }
	char cmd[64];
	int cn = snprintf(cmd, sizeof cmd, "va 0x%lx 0x%lx", va, len);
	long w = n9_pwrite((int)ctl, cmd, cn, 0);
	n9_close((int)ctl);
	if (w != cn) { errno = cc9_errno_from_errstr(); goto fail_rm; }

	snprintf(path, sizeof path, "#g/%s/data", name);
	long fd = n9_open(path, ORDWR);
	if (fd < 0) { errno = cc9_errno_from_errstr(); goto fail_rm; }
	return (int)fd;

fail_rm:
	snprintf(path, sizeof path, "#g/%s", name);
	n9_remove(path);
	return -1;
}

/* Ensure a current pool segment owned by THIS process. Caller holds shm_lock.
 * pool_fd is held open forever so the #g dir + memory outlive every per-buffer
 * fd. Keyed on pid so a fork WITHOUT exec (which inherits pool_* but execve's
 * cc9_shm_detach_all does NOT run) mints its own pool instead of scribbling the
 * parent's cursor onto the shared segment. (The inherited pool_fd is leaked in
 * that rare case — a bare fork that then allocates shm.) */
static int reaped_dead_pools;   /* one crash-cleanup pass per process */

static int pool_ensure(void) {
	/* Share the pool across rfork(RFMEM) siblings: a non-empty pool_name means a
	 * thread of THIS process (this file's statics + the #g memory are shared) already
	 * minted it, so reuse it — this is what collapses N per-thread pools into one and
	 * keeps the process under Plan 9's NSEG (~12) per-proc segment cap. A fork() child
	 * does NOT reach here with an inherited name: fork's child path calls
	 * cc9_shm_fork_child_reset(), which clears pool_name so the child mints its own.
	 * (Old gate keyed on pool_pid==getpid(), which forced every sibling to mint its
	 * own pool — the root of the NSEG exhaustion on heavy pages.) */
	if (pool_name[0]) return 0;
	/* First pool in this process: reap any pool whose creator crashed/was
	 * killed (dead pid, attached nowhere) so a previous run's leaked 256 MiB
	 * segments don't count against the ~100-entry #g cap. Self-healing — no
	 * caller hook, benefits every cc9 program that uses shm pools. */
	if (!reaped_dead_pools) { reaped_dead_pools = 1; cc9_shm_reap_dead("shmp."); }
	unsigned long va = va_alloc(SHM9_POOL);
	if (!va) return -1;
	char name[CC9_SHM_NAMELEN];
	int fd = -1;
	for (int reap = 0; reap < 2 && fd < 0; reap++) {
		for (int tries = 0; tries < 8; tries++) {   /* recycled pid -> stale dir -> next seq */
			snprintf(name, sizeof name, "shmp.%d.%u", getpid(), seq++);
			fd = create_seg(name, va, SHM9_POOL);
			if (fd >= 0) break;
		}
		/* create failed 8x — most likely the #g namespace is full. Reap dead
		 * pools once more and retry before giving up (the pressure case). */
		if (fd < 0 && reap == 0) cc9_shm_reap_dead("shmp.");
	}
	if (fd < 0) return -1;
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	pool_fd = fd;
	strcpy(pool_name, name);
	pool_off = SHM9_HDR;   /* the slot-refcount table lives in the first pages */
	pool_pid = getpid();
	/* New pool: bump the generation and drop the free-list (its slots were
	 * offsets into the pool we just left; those buffers self-release via their
	 * own refcount). Buffers minted from here carry this gen. */
	pool_gen++;
	free_n = 0;
	/* Attach our own pool now, not at the first mmap: cc9_shm_create has to read
	 * and write slot refcounts in the pool header, and a create can precede any
	 * mmap of this pool. Pinned, so idle-reclaim never detaches it under us. */
	pool_base = pool_pin_locked(pool_name);
	shm_trace("pool", pool_name, va, SHM9_POOL, fd, 0);
	return 0;
}

int cc9_shm_create(unsigned long size) {
	/* A 0-byte anonymous buffer is legitimate (e.g. an empty content-blocker
	 * list): Core::AnonymousBuffer requests the fd but never maps it. POSIX
	 * memfd/shm_open succeed at size 0; back it with one page so there is a real
	 * #g segment + fd to hand over IPC. */
	unsigned long len = page_round(size);
	if (len == 0) len = SHM9_PAGE;
	/* Bigger than a whole pool's USABLE space (the pool opens with the slot
	 * refcount table) is genuinely too big: minting a fresh pool would not help,
	 * and the bump path below assumes a fresh pool can always fit the request. */
	if (len > SHM9_POOL - SHM9_HDR) { errno = ENOMEM; return -1; }

	n9_semacquire(&shm_lock, 1);
	if (pool_ensure() < 0) { n9_semrelease(&shm_lock, 1); return -1; }
	unsigned long off;
	int reused = 0;
	/* Reuse a freed slot from the current pool first (first-fit). This is what
	 * bounds the pool to PEAK-live buffers instead of TOTAL-ever-allocated, so a
	 * long churny session (per-test canvas backing stores) stops filling pool_off
	 * and never mints pool #2 -> no VA-slab exhaustion -> no downstream crash.
	 *
	 * Only slots nobody else still references: our close put the slot here, but
	 * a peer we exported it to may still hold an fd or a mapping (see the
	 * slot-refcount comment above). Reusing one of those would alias two
	 * unrelated buffers onto the same memory. */
	for (int i = 0; i < free_n; i++) {
		if (freelist[i].len >= len && slot_count(pool_base, freelist[i].off) == 0) {
			off = freelist[i].off;
			unsigned long rem_off = off + len, rem_len = freelist[i].len - len;
			freelist[i] = freelist[--free_n];       /* swap-remove */
			if (rem_len && free_n < SHM_FREEMAX) {   /* keep the tail carve reusable */
				freelist[free_n].off = rem_off;
				freelist[free_n].len = rem_len;
				free_n++;
			}
			reused = 1;
			break;
		}
	}
	if (!reused) {
		if (pool_off + len > SHM9_POOL) {
			/* no free slot AND no bump space — mint a NEW pool. Rare now that the
			 * free-list bounds a churny session; a genuinely growing working set
			 * still attaches a few more 256M pools (bounded by the 1 GiB pid VA
			 * slab). The old pool stays alive via its live buffers' fds. */
			pool_name[0] = 0;
			if (pool_ensure() < 0) { n9_semrelease(&shm_lock, 1); return -1; }
		}
		off = pool_off;
		pool_off += len;
	}
	int cur_gen = pool_gen;
	char name[CC9_SHM_NAMELEN];
	strcpy(name, pool_name);
	unsigned long base = pool_base;
	n9_semrelease(&shm_lock, 1);

	/* A fresh fd per buffer (the AnonymousBuffer owns and closes it); its
	 * offset within the pool is recorded in the fd->buf table. */
	char path[64];
	snprintf(path, sizeof path, "#g/%s/data", name);
	long fd = n9_open(path, ORDWR);
	if (fd < 0) { errno = cc9_errno_from_errstr(); return -1; }
	fcntl((int)fd, F_SETFD, FD_CLOEXEC);
	/* This fd is the slot's first reference. Safe to take outside the lock: the
	 * slot is already ours (removed from the free-list / past the bump cursor),
	 * so no other allocation can pick it. */
	slot_ref(base, off);
	shm_set_off_both((int)fd, off, len, cur_gen, base);   /* len+gen so close() can reclaim the slot */
	shm_trace("create", name, off, len, fd, 0);
	return (int)fd;
}

int cc9_shm_export(int fd, char *name, unsigned long *offset, unsigned long *len) {
	char path[128];
	if (n9_fd2path(fd, path, sizeof path) < 0 || !path_to_name(path, name)) {
		errno = EBADF;
		return -1;
	}
	*offset = shm_get_off(fd);           /* dup-safe: reads the shared file offset */
	*len = 0;                            /* Phase B: unused on the wire (receiver's mmap size drives it) */
	/* From here the buffer is IN FLIGHT: its name+offset are about to be written
	 * into a message, and the sender is free to drop its own reference the
	 * moment the write returns. Nothing else holds the slot until the receiver
	 * imports, so mark it — otherwise the very next allocation carves the buffer
	 * the peer is still on its way to read. */
	slot_mark_inflight(pool_pin(name), *offset);
	return 0;
}

int cc9_shm_import(const char *name, unsigned long offset, unsigned long len) {
	if (strlen(name) >= CC9_SHM_NAMELEN) { errno = ENAMETOOLONG; return -1; }
	char path[64];
	snprintf(path, sizeof path, "#g/%s/data", name);
	long fd = n9_open(path, ORDWR);
	if (fd < 0) { errno = cc9_errno_from_errstr(); shm_trace("import", name, offset, len, -1, "open-failed"); return -1; }
	fcntl((int)fd, F_SETFD, FD_CLOEXEC);
	/* Take a reference the EXPORTER can see, before it possibly closes its own:
	 * the slot must not be recycled under us between here and our mmap. This is
	 * the whole point of putting the count inside the pool. */
	unsigned long base = pool_pin(name);
	slot_ref(base, offset);
	/* Our own reference is in place, so the message that carried this buffer has
	 * been consumed: release the in-flight mark export took. Order matters —
	 * ref first, then clear, or the slot is briefly unreferenced again. */
	slot_clear_inflight(base, offset);
	shm_set_off_both((int)fd, offset, 0, -1, base);  /* gen=-1: imported, NOT carved from our pool -> never reclaimed */
	shm_trace("import", name, offset, len, fd, 0);
	return (int)fd;
}

/* Is a Shared segment mapped at exactly [va, va+len) in THIS process?
 *
 * The kernel's answer, not maps[]'s. Used to tell "this pool is already
 * attached and I lost the bookkeeping" (adoptable) from "unrelated memory is in
 * the way" (a real failure). Exact-range match on purpose: a partial or
 * differently-sized overlap is NOT our segment.
 *
 * /proc/<pid>/segment lines are "<class> <start-hex> <end-hex> <ref>", e.g.
 *   Shared    46bd8e255000 46bd9e255000    9
 * Only reached on the overlap path, so the read costs nothing in the normal case. */
static int va_matches_shared_segment(unsigned long va, unsigned long len) {
	char path[48], buf[4096];
	snprintf(path, sizeof path, "/proc/%d/segment", getpid());
	long fd = n9_open(path, OREAD);
	if (fd < 0) return 0;                    /* can't confirm -> don't adopt */
	long n = n9_pread((int)fd, buf, sizeof buf - 1, 0);
	n9_close((int)fd);
	if (n <= 0) return 0;
	buf[n] = 0;
	for (char *p = buf; *p; ) {
		char *eol = p;
		while (*eol && *eol != '\n') eol++;
		char saved = *eol;
		*eol = 0;
		if (strncmp(p, "Shared", 6) == 0) {
			char *q = p + 6;
			while (*q == ' ' || *q == '\t') q++;
			char *end = q;
			unsigned long start = strtoul(q, &end, 16);
			if (end != q) {
				q = end;
				while (*q == ' ' || *q == '\t') q++;
				char *end2 = q;
				unsigned long stop = strtoul(q, &end2, 16);
				if (end2 != q && start == va && stop == va + len) {
					*eol = saved;
					return 1;
				}
			}
		}
		*eol = saved;
		p = *eol ? eol + 1 : eol;
	}
	return 0;
}

/* Attach the pool named `name` in THIS process — or find it already attached —
 * and return its base VA (0 on failure, errno set). Caller holds shm_lock.
 *
 * mode POOL_MAP: an mmap. Bumps the attach refcount, as every mmap always did.
 * mode POOL_PIN: the caller only needs the pool ADDRESSABLE, to read or update
 *   a slot refcount that lives in the pool header. Pinning takes one permanent
 *   attach reference so idle-reclaim can never detach the pool out from under a
 *   counter update; the pool is one this process is about to use anyway (its own
 *   pool, or a peer's it just imported a buffer from), so nothing extra is
 *   retained in practice.
 *
 * `need` (0 to skip) is the end offset the caller intends to use: it is checked
 * against the pool's real length BEFORE any reference is taken, so a bogus
 * request fails without leaving an attach reference behind. */
enum { POOL_MAP = 0, POOL_PIN = 1 };
static unsigned long pool_attach_locked(const char *name, int mode, unsigned long need)
{
	/* One segattach per POOL (keyed by name, refcounted); every buffer in the
	 * pool shares it and returns base + its own offset. This is what keeps a
	 * process under the Plan 9 NSEG cap.
	 *
	 * The already-attached case is the common one — every mmap, export and
	 * import of a pool this process has seen before — so answer it from maps[]
	 * alone. read_ctl below is three syscalls (open/pread/close of the segment's
	 * ctl file) and is only needed to attach a pool for the first time; an
	 * attached entry already knows the length. */
	shm_map *free_slot = 0;
	for (int i = 0; i < SHM_MAXMAP; i++) {
		/* A named entry is ATTACHED, whatever its refcount: unmap never
		 * detaches (see cc9_shm_unmap), so only execve's detach_all clears a
		 * name. Liveness is therefore name[0], NOT refs — matching on refs
		 * would miss an attached pool sitting at zero and segattach it a
		 * second time, which the kernel refuses with "segments overlap". */
		if (maps[i].name[0] && strcmp(maps[i].name, name) == 0) {
			if (need > maps[i].len) { errno = EINVAL; return 0; }
			if (mode == POOL_PIN) {
				if (!maps[i].pinned) { maps[i].pinned = 1; maps[i].refs++; }
			} else {
				maps[i].refs++;
				maps[i].lru = ++shm_tick;
			}
			return maps[i].va;
		}
		if (!maps[i].name[0] && !free_slot) free_slot = &maps[i];
	}
	if (!free_slot) {
		errno = ENOMEM;
		return 0;
	}
	/* Not attached yet: now the segment's own VA/length are needed, both to
	 * bound-check the request and to recognise an already-mapped range below. */
	unsigned long segva, seglen;
	if (read_ctl(name, &segva, &seglen) < 0) { errno = EBADF; return 0; }
	if (need > seglen) { errno = EINVAL; return 0; }
	/* attr 0, not SG_CEXEC: the kernel ignores attach attrs for #g segments
	 * (verified on 9front — inherited attaches survive exec either way).
	 * Exec-clean semantics come from cc9_shm_detach_all() in execve. */
	void *got = n9_segattach(0, name, 0, 0);
	if ((long)got < 0 || got == 0) {
		char eb[96]; eb[0] = 0;
		n9_errstr(eb, sizeof eb);          /* read + CLEAR kernel errstr */
		shm_trace("attach", name, segva, seglen, (long)got, eb);
		/* "segments overlap" means SOMETHING already occupies the fixed VA this
		 * segment must live at. Usually that something IS this very segment:
		 * maps[] is only a mirror of the kernel's state and can fall out of step
		 * with it — a mapping inherited across fork+exec (the kernel keeps #g
		 * attaches; see execve's cc9_shm_detach_all), or an entry dropped while
		 * the mapping survived. The kernel is the authority, so ask it: if a
		 * segment really is mapped at exactly [segva, segva+seglen), adopt it
		 * instead of failing. Re-attaching is impossible and failing is
		 * permanent — every later map of that pool would fail the same way,
		 * which is how one lost entry turned into an undecodable IPC message.
		 * If the range does NOT match, the overlap is a genuine conflict with
		 * unrelated memory and must still fail: handing back a pointer into
		 * someone else's segment would be far worse than an error. */
		if (va_matches_shared_segment(segva, seglen)) {
			shm_trace("adopt", name, segva, seglen, 0, 0);
			got = (void *)segva;
			goto adopted;
		}
		n9_errstr(eb, sizeof eb);          /* swap back so the map below sees it */
		errno = cc9_errno_from_errstr();
		return 0;
	}
adopted:
	shm_trace("attach", name, segva, seglen, (long)got, 0);
	strcpy(free_slot->name, name);
	free_slot->va = (unsigned long)got;
	free_slot->len = seglen;
	free_slot->refs = 1;                        /* this mmap, or the pin */
	free_slot->pinned = (mode == POOL_PIN);
	free_slot->lru = ++shm_tick;
	return (unsigned long)got;
}

/* Attach a pool purely to reach its slot-refcount header. Takes shm_lock. */
static unsigned long pool_pin(const char *name)
{
	n9_semacquire(&shm_lock, 1);
	unsigned long base = pool_attach_locked(name, POOL_PIN, 0);
	n9_semrelease(&shm_lock, 1);
	return base;
}
/* Same, for a caller that already holds shm_lock. */
static unsigned long pool_pin_locked(const char *name)
{
	return pool_attach_locked(name, POOL_PIN, 0);
}

void *cc9_shm_try_map(int fd, unsigned long len, int *handled) {
	char path[128], name[CC9_SHM_NAMELEN];
	*handled = 0;
	if (n9_fd2path(fd, path, sizeof path) < 0 || !path_to_name(path, name))
		return 0;                /* not ours: mmap falls through to pread-copy */
	*handled = 1;

	unsigned long off = shm_get_off(fd);  /* pool offset, from the fd position */
	/* The offset is the fragile part of the pool scheme (fd table OR seek
	 * position; the IPC layer dups the fd, so neither alone is sufficient). A
	 * wrong offset hands the caller the pool BASE instead of its buffer, which
	 * reads as truncated/garbage image data and can fault past the end. */
	shm_trace("map", name, off, len, (long)fd, 0);
	n9_semacquire(&shm_lock, 1);
	unsigned long base = pool_attach_locked(name, POOL_MAP, off + page_round(len));
	n9_semrelease(&shm_lock, 1);
	if (!base) return (void *)-1;
	/* This mapping is a reference to the slot, not just to the pool: the
	 * creator may close its fd while we still hold these pages. */
	slot_ref(base, off);
	return (void *)(base + off);
}

/* Lazy fault-attach — the fix for the cross-sibling #g read fault.
 *
 * cc9 threads are rfork(RFMEM) procs: they share one address space but each has
 * its OWN kernel segment table, and Plan 9 does NOT propagate a #g segattach
 * across siblings. So a pool buffer that thread A mmap'd (and attached in A's
 * table) is only a raw pointer to thread B — B never attached the segment, and
 * reading it faults. maps[] is a static shared across the RFMEM siblings, so it
 * records the pool B is missing; the note handler hands us the fault addr and we
 * attach that pool HERE, in the faulting proc, then signal a retry. segattach
 * ignores its va/len args for a #g segment and maps it at the segment's own
 * fixed VA (see top-of-file), so the pointer B already holds becomes valid and
 * the re-executed load reads the right bytes.
 *
 * The concrete victim was web fonts: Skia/FreeType (FT_Stream_ReadULong, in
 * sfnt_init_face) reads a font AnonymousBuffer that convert_to_ttf built and
 * memcpy'd on a different WebContent thread — youtube's web fonts faulted a
 * WebContent proc, which cascaded to the whole box.
 *
 * Lockless by design: the note handler runs in the faulting proc and must NEVER
 * block on shm_lock — the proc may have been holding it when it faulted, which
 * would self-deadlock. A torn maps[] read at worst fails to match (the fault
 * stays fatal, as before) or names a stale pool (segattach fails, also fatal) —
 * never worse than the crash it replaces. Returns 1 only on a FRESH successful
 * attach, so the same addr can't fault-attach twice (a second try hits "segments
 * overlap" -> 0 -> the process dies); that bounds retries to one per addr. */
int cc9_shm_fault_attach(unsigned long addr) {
	for (int i = 0; i < SHM_MAXMAP; i++) {
		unsigned long va = maps[i].va, len = maps[i].len;
		if (!maps[i].name[0] || !va || addr < va || addr >= va + len)
			continue;
		char name[CC9_SHM_NAMELEN];
		strncpy(name, maps[i].name, sizeof name);
		name[sizeof name - 1] = 0;
		void *got = n9_segattach(0, name, 0, 0);
		if ((long)got >= 0 && got != 0) {
			shm_trace("faultatt", name, va, len, (long)got, 0);
			return 1;   /* freshly attached in this proc -> the retried load succeeds */
		}
		char eb[96]; eb[0] = 0;
		n9_errstr(eb, sizeof eb);   /* clear errstr; "segments overlap" = already here, so a retry won't help */
		shm_trace("faultatt", name, va, len, (long)got, eb);
		return 0;   /* couldn't newly attach -> let the fault stay fatal (no retry loop) */
	}
	return 0;   /* addr not in any known pool -> a genuine fault, die */
}

/* shm_lock held. Keep at most SHM_MAXIDLE idle (refs==0) pools attached,
 * detaching the least recently used beyond that.
 *
 * The two extremes are both wrong. Detaching the instant a pool hits zero let a
 * sibling thread pull a pool out from under another sibling mid-decode -- the
 * refcount counts mmap CALLS, not the raw pointers consumers still hold, and
 * cc9 threads share one address space. Never detaching pins every pool's touched
 * pages for the process lifetime (256 MiB apiece), which exhausts a small box on
 * a heavy page. A few idle pools give a wide grace window at bounded cost.
 *
 * ponytail: a grace window, not a proof. The real fix is refcounting BUFFERS
 * rather than pools, so a pointer keeps its own pool alive; until then
 * SHM_MAXIDLE is the knob, and raising it trades memory for safety. */
static void shm_reclaim_idle_locked(void) {
	for (;;) {
		int idle = 0, victim = -1;
		for (int i = 0; i < SHM_MAXMAP; i++) {
			if (!maps[i].name[0] || maps[i].refs) continue;
			idle++;
			if (victim < 0 || maps[i].lru < maps[victim].lru) victim = i;
		}
		if (idle <= SHM_MAXIDLE || victim < 0) return;
		long dr = n9_segdetach((void *)maps[victim].va);
		char eb[96]; eb[0] = 0;
		if (dr < 0) n9_errstr(eb, sizeof eb);
		shm_trace("evict", maps[victim].name, maps[victim].va, maps[victim].len, dr, eb);
		maps[victim].name[0] = 0;
		maps[victim].pinned = 0;
		maps[victim].lru = 0;
	}
}

int cc9_shm_unmap(void *p, unsigned long len) {
	(void)len;
	unsigned long va = (unsigned long)p;
	n9_semacquire(&shm_lock, 1);
	for (int i = 0; i < SHM_MAXMAP; i++) {
		/* range-match: p is base + buffer offset, so find the pool it lands in.
		 * A PINNED entry must be claimed even at refs==0: it is still attached,
		 * and returning 0 here would send munmap down its fallback, which calls
		 * free() on the pointer — handing a shared-segment address to the heap
		 * allocator. Claim the range, and only decrement a refcount that is
		 * actually positive. */
		if (maps[i].name[0]
		    && va >= maps[i].va && va < maps[i].va + maps[i].len) {
			/* Decrement, but NEVER detach here. The refcount counts mmap calls;
			 * it does not track the raw pointers consumers keep INTO the pool.
			 * cc9 threads are rfork(RFMEM) procs sharing one address space and
			 * one maps[], so a sibling that finishes with its buffer can drive
			 * the pool's count to zero while another sibling is still reading a
			 * different buffer in the same pool — and a detach then unmaps it
			 * under the reader. Observed exactly that: ImageDecoder attached
			 * shmp.<peer>.0, a sibling detached it, and the next read faulted at
			 * the segment base (BMPImageDecoderPlugin::sniff), alongside
			 * "truncated input" from decoders reading memory that had gone away.
			 *
			 * Keeping the attach also removes the per-buffer attach/detach thrash
			 * the creator side showed. Teardown is cc9_shm_detach_all() at execve,
			 * where the address space is replaced anyway.
			 *
			 * ponytail: mappings are now per-POOL and live for the process, so
			 * the ceiling is Plan 9's NSEG (~12 segments/proc) against the number
			 * of DISTINCT pools a process touches — one per peer plus its own,
			 * so ~6 here. If that ever binds, the failure is a loud ENOMEM from
			 * segattach, never corruption; the fix would be refcounting the
			 * buffers rather than the pool. */
			/* Drop this mapping's reference to the SLOT it points into, so the
			 * creator can eventually recycle that slot. Distinct from the pool
			 * attach refcount below, which only decides when a pool may be
			 * detached. */
			slot_unref(maps[i].va, va - maps[i].va);
			if (maps[i].refs > 0)
				maps[i].refs--;
			if (maps[i].refs == 0) {
				maps[i].lru = ++shm_tick;
				shm_reclaim_idle_locked();
			}
			n9_semrelease(&shm_lock, 1);
			return 1;
		}
	}
	n9_semrelease(&shm_lock, 1);
	return 0;
}

/* ---- /srv fd passing (TransportPlan9's Srv attachments) ----
 * srv(3): create /srv/<name>, write the decimal fd number into it; the entry
 * holds a reference to the open file itself. Any process that opens the
 * entry acquires the same channel (shared offset) — SCM_RIGHTS semantics. */
long cc9_srv_post(const char *name, int fd) {
	char path[64], num[16];
	snprintf(path, sizeof path, "/srv/%s", name);
	long sfd = n9_create(path, 1 /*OWRITE*/, 0600);
	if (sfd < 0) { errno = cc9_errno_from_errstr(); return -1; }
	int n = snprintf(num, sizeof num, "%d", fd);
	long w = n9_pwrite((int)sfd, num, n, -1);
	n9_close((int)sfd);
	if (w != n) { errno = cc9_errno_from_errstr(); n9_remove(path); return -1; }
	return 0;
}
long cc9_srv_remove(const char *name) {
	char path[64];
	snprintf(path, sizeof path, "/srv/%s", name);
	return n9_remove(path);
}

/* Called by execve just before n9_exec — userspace SG_CEXEC (see there).
 * After fork the child owns a copy of the attach table describing exactly the
 * inherited attaches, so detaching every entry leaves the exec image clean. */
void cc9_shm_detach_all(void) {
	n9_semacquire(&shm_lock, 1);
	for (int i = 0; i < SHM_MAXMAP; i++) {
		/* name[0], NOT refs: an entry is attached regardless of refcount now
		 * that unmap never detaches. Keying on refs here would leave those
		 * mappings attached across the exec, and the new image — starting with
		 * a zeroed maps[] in BSS — would segattach them again and get
		 * "segments overlap" forever. That is exactly the desync this function
		 * exists to prevent. */
		if (maps[i].name[0]) {
			n9_segdetach((void *)maps[i].va);
			maps[i].refs = 0;
			maps[i].pinned = 0;
			maps[i].name[0] = 0;
		}
	}
	n9_semrelease(&shm_lock, 1);
}

/* Fork-child shm reset — the counterpart that makes pool-sharing safe.
 *
 * rfork(RFMEM) SIBLING threads share one pool (see pool_ensure): they share this
 * file's statics AND the #g pool memory, so one pool serves the whole process and
 * each proc holds ONE pool segment instead of one-per-thread — which is what keeps
 * a heavy page's WebContent under Plan 9's NSEG (~12) per-proc segment cap.
 *
 * A fork() child is different: it is a COW COPY of our address space, not a sibling.
 * It inherits pool_name/pool_off/pool_fd but its writes to the (global, shared) #g
 * pool memory would race the parent's carving of the SAME bytes — both advance their
 * own copy of pool_off into one physical pool. So the child must NOT keep the pool.
 * fork() calls this from its child path (cc9_reap_child_reset), BEFORE the child can
 * allocate shm; it detaches the inherited attachments and wipes all pool/slab/buffer
 * bookkeeping so the child mints a fresh pool keyed on its own pid. rfork siblings
 * never come through fork(), so they are untouched and keep sharing. The child is
 * single-threaded here, so no lock is needed — reset the sem to free in case it was
 * inherited held, then reset. */
void cc9_shm_fork_child_reset(void) {
	shm_lock = 1;
	for (int i = 0; i < SHM_MAXMAP; i++) {
		if (maps[i].name[0]) {
			n9_segdetach((void *)maps[i].va);
			maps[i].name[0] = 0;
			maps[i].refs = 0;
			maps[i].pinned = 0;
			maps[i].lru = 0;
		}
	}
	if (pool_fd >= 0) n9_close(pool_fd);
	pool_fd = -1;
	pool_name[0] = 0;
	pool_off = 0;
	pool_base = 0;
	pool_pid = 0;
	pool_gen++;              /* invalidate any inherited buffer's pool generation */
	slab_base = 0;
	slab_next = 0;           /* child picks a fresh slab keyed on its own pid */
	free_n = 0;
	for (int i = 0; i < SHM_MAXBUF; i++) bufs[i].fd = 0;
	bufs_live = 0;
	reaped_dead_pools = 0;
}

/* ---- sweep: GC segment names whose memory is attached in no process ----
 *
 * Liveness check: /proc/<pid>/segment lists each attached segment with its
 * start address (segments(3)); a named segment attached ANYWHERE shows its
 * fixed VA in someone's file. Our VAs live in a private region no other
 * mapping uses, so a hex-substring match on the VA is unambiguous. */

static int va_attached_anywhere(unsigned long va) {
	char hex[24], line[4096];
	snprintf(hex, sizeof hex, "%lx", va);
	DIR *d = opendir("/proc");
	if (!d) return 1;            /* can't tell: treat as live (never remove blind) */
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
		char path[64];
		snprintf(path, sizeof path, "/proc/%s/segment", e->d_name);
		long fd = n9_open(path, OREAD);
		if (fd < 0) continue;
		long n = n9_pread((int)fd, line, sizeof line - 1, 0);
		n9_close((int)fd);
		if (n <= 0) continue;
		line[n] = 0;
		if (strstr(line, hex)) { closedir(d); return 1; }
	}
	closedir(d);
	return 0;
}

/* first-seen-unattached bookkeeping so a buffer in flight (created, sent, not
 * yet attached by the receiver) survives the sweep: only names observed
 * unattached across the whole grace window are removed. */
typedef struct { char name[CC9_SHM_NAMELEN]; long first; } stale_ent;
#define SHM_MAXSTALE 256
static stale_ent stale[SHM_MAXSTALE];

void cc9_shm_sweep(const char *prefix, int grace_seconds) {
	long now = time(0);
	unsigned long plen = strlen(prefix);
	DIR *d = opendir("#g");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, prefix, plen) != 0) continue;
		unsigned long va, len;
		char path[64];
		if (read_ctl(e->d_name, &va, &len) < 0 || va_attached_anywhere(va)) {
			/* live (or unreadable): forget any stale record */
			for (int i = 0; i < SHM_MAXSTALE; i++)
				if (stale[i].first && strcmp(stale[i].name, e->d_name) == 0)
					stale[i].first = 0;
			continue;
		}
		stale_ent *ent = 0, *slot = 0;
		for (int i = 0; i < SHM_MAXSTALE; i++) {
			if (stale[i].first && strcmp(stale[i].name, e->d_name) == 0) ent = &stale[i];
			if (!stale[i].first && !slot) slot = &stale[i];
		}
		if (!ent) {
			if (slot) {
				strcpy(slot->name, e->d_name);
				slot->first = now;
			}
			continue;            /* first sighting: start the clock */
		}
		if (now - ent->first >= grace_seconds) {
			snprintf(path, sizeof path, "#g/%s", ent->name);
			n9_remove(path);
			ent->first = 0;
		}
	}
	closedir(d);
}

/* ---- reap: startup crash-cleanup of pools whose owner PID is dead ----
 *
 * cc9_shm_sweep's grace window exists for an in-flight buffer (created + sent,
 * receiver not yet attached) — it must not be reaped just for being unattached
 * for an instant. A crash is different: the pool name is "<prefix><pid>.<seq>"
 * and if /proc/<pid> is gone the creator is DEAD, so there is no future
 * attach coming. Combined with "attached in no live process right now", that's
 * unambiguously reclaimable — no grace, no second pass. Called once at process
 * startup so a killed/crashed browser's leaked 256 MiB pool doesn't survive to
 * exhaust the ~100-entry #g cap. Returns the number reaped.
 *
 * ponytail: single startup pass; the periodic cc9_shm_sweep still covers the
 * graceful-teardown and in-flight cases. */
static int pid_alive(long pid) {
	char path[32];
	snprintf(path, sizeof path, "/proc/%ld/status", pid);
	long fd = n9_open(path, OREAD);
	if (fd < 0) return 0;   /* no proc dir -> dead (or never existed) */
	n9_close((int)fd);
	return 1;
}

int cc9_shm_reap_dead(const char *prefix) {
	unsigned long plen = strlen(prefix);
	DIR *d = opendir("#g");
	if (!d) return 0;
	int reaped = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, prefix, plen) != 0) continue;
		char path[64];
		/* An empty/unreadable ctl means the segment has NO backing memory: its
		 * last attacher already left and only the #g name lingers (a zombie).
		 * That is unambiguously reclaimable regardless of the owner pid — which
		 * may even read "alive" because it was RECYCLED to an unrelated process.
		 * A live pool always has a valid ctl (va written before first use), so
		 * this never removes one in use; create_seg's tiny make-dir/write-va
		 * window self-heals via its retry loop. This was the leak that wedged
		 * back-to-back runs: zombies here used to be skipped ("unreadable: don't
		 * touch") and piled up until they broke the next run's shm. */
		unsigned long va, len;
		if (read_ctl(e->d_name, &va, &len) < 0) {
			snprintf(path, sizeof path, "#g/%s", e->d_name);
			if (n9_remove(path) == 0) reaped++;
			continue;
		}
		/* Valid ctl (real backing): only reap when the creator is DEAD and no
		 * live process still maps it (the A->B->C forwarding case keeps it). */
		const char *p = e->d_name + plen;
		long pid = 0;
		int any = 0;
		for (; *p >= '0' && *p <= '9'; p++) { pid = pid * 10 + (*p - '0'); any = 1; }
		if (!any || pid_alive(pid)) continue;     /* owner still running: leave it */
		if (va_attached_anywhere(va)) continue;   /* a receiver still maps it: keep */
		snprintf(path, sizeof path, "#g/%s", e->d_name);
		if (n9_remove(path) == 0) reaped++;
	}
	closedir(d);
	return reaped;
}
