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
static unsigned seq;            /* per-process name counter */

/* per-process attach table: devsegment refuses a second attach of a segment
 * already mapped in this process, so double-mmap of the same buffer (dup'd fd,
 * two AnonymousBuffer views) must be satisfied from here with a refcount. */
typedef struct {
	char name[CC9_SHM_NAMELEN];
	unsigned long va, len;
	int refs;
	/* Adopted (see cc9_shm_try_map): the mapping already existed and we could
	 * not know how many users it had, so refs is a LOWER BOUND, not the truth.
	 * Detaching on refs==0 would then pull the segment out from under a user we
	 * never counted — a dangling pointer into image data. Pinned entries are
	 * therefore never detached by cc9_shm_unmap; only cc9_shm_detach_all (which
	 * runs at execve, where the whole address space is about to be replaced)
	 * tears them down. Cost is VA retention, which is exactly what we can
	 * afford — the alternative is a use-after-detach fault. */
	unsigned char pinned;
} shm_map;
#define SHM_MAXMAP 512
static shm_map maps[SHM_MAXMAP];

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

typedef struct { int fd; unsigned long off, len; int gen; } shm_buf;   /* guarded by shm_lock */
#define SHM_MAXBUF 4096
static shm_buf bufs[SHM_MAXBUF];
static int bufs_live;   /* fast-path: skip the O(N) scan in close() when 0 */

/* Per-pool free-list: freed [off,len) slots in the CURRENT pool, reused before
 * bumping pool_off. Without this the bump cursor climbs forever across a long
 * reused-process session (test-web never respawns the Compositor/WebContent, so
 * per-test canvas backing stores churned pool after 256 MiB pool until the 1 GiB
 * pid VA slab exhausted -> cc9_shm_create returned -1 -> a fatal VERIFY -> the
 * Compositor crashed ~100 tests in). First-fit + tail-split, no coalescing; the
 * "Phase B pool" the create/free comments below defer. All under shm_lock. */
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
static void shm_set_off_both(int fd, unsigned long off, unsigned long len, int gen) {
	shm_set_off(fd, off);                       /* seek: outside the lock (syscall) */
	n9_semacquire(&shm_lock, 1);
	shm_buf *b = 0, *slot = 0;
	for (int i = 0; i < SHM_MAXBUF; i++) {
		if (bufs[i].fd == fd + 1) { b = &bufs[i]; break; }   /* +1: 0 = empty */
		if (!bufs[i].fd && !slot) slot = &bufs[i];
	}
	if (!b && slot) { slot->fd = fd + 1; b = slot; bufs_live++; }
	if (b) { b->off = off; b->len = len; b->gen = gen; }
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
			/* Return this buffer's slot to the current pool's free-list so its
			 * offset is reused instead of leaked. Only OUR pool buffers (gen>=0
			 * from create; imports carry gen=-1) belonging to the CURRENT pool
			 * (an already-superseded pool self-releases via its own attach
			 * refcount when its last buffer's fd closes). Recorded exactly once:
			 * only the create fd is in the table; the IPC layer's export dup and
			 * imported fds are either absent or gen=-1. */
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

/* Fresh VA for a new segment of len bytes, from our pid-keyed slab. */
static unsigned long va_alloc(unsigned long len) {
	unsigned long slab = SHM9_BASE + (((unsigned long)getpid() & 0xFFFFul) << 30);
	if (slab_next == 0) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		/* start somewhere in the first quarter of the slab so recycled pids
		 * rarely mint the VA a dead predecessor's still-attached segment holds */
		slab_next = slab + (page_round((unsigned long)ts.tv_nsec) % (SHM9_SLAB / 4));
	}
	if (slab_next + len > slab + SHM9_SLAB) {
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
	if (pool_name[0] && pool_pid == getpid()) return 0;
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
	pool_off = 0;
	pool_pid = getpid();
	/* New pool: bump the generation and drop the free-list (its slots were
	 * offsets into the pool we just left; those buffers self-release via their
	 * own refcount). Buffers minted from here carry this gen. */
	pool_gen++;
	free_n = 0;
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
	if (len > SHM9_POOL) { errno = ENOMEM; return -1; }   /* one buffer bigger than a whole pool: genuinely too big */

	n9_semacquire(&shm_lock, 1);
	if (pool_ensure() < 0) { n9_semrelease(&shm_lock, 1); return -1; }
	unsigned long off;
	int reused = 0;
	/* Reuse a freed slot from the current pool first (first-fit). This is what
	 * bounds the pool to PEAK-live buffers instead of TOTAL-ever-allocated, so a
	 * long churny session (per-test canvas backing stores) stops filling pool_off
	 * and never mints pool #2 -> no VA-slab exhaustion -> no downstream crash. */
	for (int i = 0; i < free_n; i++) {
		if (freelist[i].len >= len) {
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
	n9_semrelease(&shm_lock, 1);

	/* A fresh fd per buffer (the AnonymousBuffer owns and closes it); its
	 * offset within the pool is recorded in the fd->buf table. */
	char path[64];
	snprintf(path, sizeof path, "#g/%s/data", name);
	long fd = n9_open(path, ORDWR);
	if (fd < 0) { errno = cc9_errno_from_errstr(); return -1; }
	fcntl((int)fd, F_SETFD, FD_CLOEXEC);
	shm_set_off_both((int)fd, off, len, cur_gen);   /* len+gen so close() can reclaim the slot */
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
	return 0;
}

int cc9_shm_import(const char *name, unsigned long offset, unsigned long len) {
	if (strlen(name) >= CC9_SHM_NAMELEN) { errno = ENAMETOOLONG; return -1; }
	char path[64];
	snprintf(path, sizeof path, "#g/%s/data", name);
	long fd = n9_open(path, ORDWR);
	if (fd < 0) { errno = cc9_errno_from_errstr(); shm_trace("import", name, offset, len, -1, "open-failed"); return -1; }
	fcntl((int)fd, F_SETFD, FD_CLOEXEC);
	shm_set_off_both((int)fd, offset, 0, -1);  /* gen=-1: imported, NOT carved from our pool -> never reclaimed */
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

void *cc9_shm_try_map(int fd, unsigned long len, int *handled) {
	char path[128], name[CC9_SHM_NAMELEN];
	*handled = 0;
	if (n9_fd2path(fd, path, sizeof path) < 0 || !path_to_name(path, name))
		return 0;                /* not ours: mmap falls through to pread-copy */
	*handled = 1;

	unsigned long segva, seglen;
	if (read_ctl(name, &segva, &seglen) < 0) { errno = EBADF; return (void *)-1; }

	unsigned long off = shm_get_off(fd);  /* pool offset, from the fd position */
	/* The offset is the fragile part of the pool scheme (fd table OR seek
	 * position; the IPC layer dups the fd, so neither alone is sufficient). A
	 * wrong offset hands the caller the pool BASE instead of its buffer, which
	 * reads as truncated/garbage image data and can fault past the end. */
	shm_trace("map", name, off, len, (long)fd, 0);
	n9_semacquire(&shm_lock, 1);
	if (off + page_round(len) > seglen) { n9_semrelease(&shm_lock, 1); errno = EINVAL; return (void *)-1; }

	/* One segattach per POOL (keyed by name, refcounted); every buffer in the
	 * pool shares it and returns base + its own offset. This is what keeps a
	 * process under the Plan 9 NSEG cap. */
	shm_map *free_slot = 0;
	int pin = 0;
	for (int i = 0; i < SHM_MAXMAP; i++) {
		/* Match on refs OR pinned: a pinned entry that has fallen to refs==0 is
		 * still ATTACHED (that is what pinned means), so it must be re-adopted
		 * from the table rather than segattach'd again. Missing it here would
		 * overlap, adopt, and burn a fresh slot on every map until the table
		 * ran out. */
		if ((maps[i].refs || maps[i].pinned) && maps[i].name[0]
		    && strcmp(maps[i].name, name) == 0) {
			maps[i].refs++;
			unsigned long base = maps[i].va;
			n9_semrelease(&shm_lock, 1);
			return (void *)(base + off);
		}
		if (!maps[i].refs && !maps[i].pinned && !free_slot) free_slot = &maps[i];
	}
	if (!free_slot) {
		n9_semrelease(&shm_lock, 1);
		errno = ENOMEM;
		return (void *)-1;
	}
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
			pin = 1;
			goto adopted;
		}
		n9_errstr(eb, sizeof eb);          /* swap back so the map below sees it */
		n9_semrelease(&shm_lock, 1);
		errno = cc9_errno_from_errstr();
		return (void *)-1;
	}
adopted:
	shm_trace("attach", name, segva, seglen, (long)got, 0);
	strcpy(free_slot->name, name);
	free_slot->va = (unsigned long)got;
	free_slot->len = seglen;
	free_slot->refs = 1;
	free_slot->pinned = (unsigned char)pin;
	n9_semrelease(&shm_lock, 1);
	return (void *)((unsigned long)got + off);
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
		if ((maps[i].refs || maps[i].pinned)
		    && va >= maps[i].va && va < maps[i].va + maps[i].len) {
			if (maps[i].refs > 0 && --maps[i].refs == 0 && !maps[i].pinned) {
				long dr = n9_segdetach((void *)maps[i].va);   /* at the segment base */
				char eb[96]; eb[0] = 0;
				if (dr < 0) n9_errstr(eb, sizeof eb);
				shm_trace("detach", maps[i].name, maps[i].va, maps[i].len, dr, eb);
				maps[i].name[0] = 0;
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
		if (maps[i].refs) {
			n9_segdetach((void *)maps[i].va);
			maps[i].refs = 0;
			maps[i].name[0] = 0;
		}
	}
	n9_semrelease(&shm_lock, 1);
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
