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
 * A buffer's offset within the pool is carried in the fd's SEEK POSITION, not
 * an fd-number-keyed table: the IPC layer clones the fd (dup) before
 * cc9_shm_export sees it, and dup(2) shares the file offset, so seek survives
 * the clone while an fd-number lookup would miss and fall back to offset 0
 * (the painter then maps the wrong pool region and reads zeros). AnonymousBuffer
 * only mmaps the fd (never read/write/lseeks it), so the position stays put. */
extern long n9_seek(long long *, int, long long, int);
static void shm_set_off(int fd, unsigned long off) {
	long long ret;
	n9_seek(&ret, fd, (long long)off, 0 /*SEEK_SET*/);
}
static unsigned long shm_get_off(int fd) {
	long long ret = 0;
	if (n9_seek(&ret, fd, 0, 1 /*SEEK_CUR*/) < 0) return 0;
	return (unsigned long)ret;
}

#define SHM9_POOL (256ul << 20)     /* 256 MiB per pool; demand-paged, so cheap */
static char pool_name[CC9_SHM_NAMELEN];  /* "" until the pool is created */
static unsigned long pool_off;           /* bump cursor within the pool */
static int pool_fd = -1;                 /* held open process-lifetime: keeps the #g dir + memory alive */

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

/* Lazily create this process's pool segment. Caller holds shm_lock. pool_fd is
 * held open forever so the #g dir + memory outlive every per-buffer fd. */
static int pool_ensure(void) {
	if (pool_name[0]) return 0;
	unsigned long va = va_alloc(SHM9_POOL);
	if (!va) return -1;
	char name[CC9_SHM_NAMELEN];
	int fd = -1;
	for (int tries = 0; tries < 8; tries++) {   /* recycled pid -> stale dir -> next seq */
		snprintf(name, sizeof name, "shmp.%d.%u", getpid(), seq++);
		fd = create_seg(name, va, SHM9_POOL);
		if (fd >= 0) break;
	}
	if (fd < 0) return -1;
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	pool_fd = fd;
	strcpy(pool_name, name);
	pool_off = 0;
	shm_trace("pool", pool_name, va, SHM9_POOL, fd, 0);
	return 0;
}

int cc9_shm_create(unsigned long size) {
	if (size == 0) { errno = EINVAL; return -1; }
	unsigned long len = page_round(size);
	if (len > SHM9_POOL) { errno = ENOMEM; return -1; }   /* ponytail: single 256M pool; multi-pool if a buffer needs more */

	n9_semacquire(&shm_lock, 1);
	if (pool_ensure() < 0) { n9_semrelease(&shm_lock, 1); return -1; }
	if (pool_off + len > SHM9_POOL) {                     /* ponytail: bump-only, no free-list; pool grows until proc exit */
		n9_semrelease(&shm_lock, 1); errno = ENOMEM; return -1;
	}
	unsigned long off = pool_off;
	pool_off += len;
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
	shm_set_off((int)fd, off);          /* carry the pool offset in the fd position */
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
	shm_set_off((int)fd, offset);       /* the buffer's offset in the sender's pool */
	shm_trace("import", name, offset, len, fd, 0);
	return (int)fd;
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
	n9_semacquire(&shm_lock, 1);
	if (off + page_round(len) > seglen) { n9_semrelease(&shm_lock, 1); errno = EINVAL; return (void *)-1; }

	/* One segattach per POOL (keyed by name, refcounted); every buffer in the
	 * pool shares it and returns base + its own offset. This is what keeps a
	 * process under the Plan 9 NSEG cap. */
	shm_map *free_slot = 0;
	for (int i = 0; i < SHM_MAXMAP; i++) {
		if (maps[i].refs && strcmp(maps[i].name, name) == 0) {
			maps[i].refs++;
			unsigned long base = maps[i].va;
			n9_semrelease(&shm_lock, 1);
			return (void *)(base + off);
		}
		if (!maps[i].refs && !free_slot) free_slot = &maps[i];
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
		n9_errstr(eb, sizeof eb);          /* swap back so the map below sees it */
		n9_semrelease(&shm_lock, 1);
		errno = cc9_errno_from_errstr();
		return (void *)-1;
	}
	shm_trace("attach", name, segva, seglen, (long)got, 0);
	strcpy(free_slot->name, name);
	free_slot->va = (unsigned long)got;
	free_slot->len = seglen;
	free_slot->refs = 1;
	n9_semrelease(&shm_lock, 1);
	return (void *)((unsigned long)got + off);
}

int cc9_shm_unmap(void *p, unsigned long len) {
	(void)len;
	unsigned long va = (unsigned long)p;
	n9_semacquire(&shm_lock, 1);
	for (int i = 0; i < SHM_MAXMAP; i++) {
		/* range-match: p is base + buffer offset, so find the pool it lands in */
		if (maps[i].refs && va >= maps[i].va && va < maps[i].va + maps[i].len) {
			if (--maps[i].refs == 0) {
				n9_segdetach((void *)maps[i].va);   /* detach at the segment base */
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
