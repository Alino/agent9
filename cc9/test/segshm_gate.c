/* segshm_gate — proves cc9's shm9 layer (named #g global segments) gives REAL
 * cross-process shared memory across fork+exec, the substrate Ladybird's
 * Core::AnonymousBuffer needs. Runs on 9front. Prints "PASS n/n" or dies.
 *
 * Re-execs itself as the child (argv[1] = "child <name>") like the msggate
 * convention, so the sharing crosses a full exec boundary — the same topology
 * as browser helper processes (rfork alone would share via RFMEM and prove
 * nothing).
 *
 * Checks:
 *   1. create + mmap(MAP_SHARED) + fill pattern
 *   2. export -> wire triple {name, offset, len}
 *   3. child (fork+exec): import by name, mmap, verify pattern, MUTATE, exit
 *   4. parent sees the child's mutation (true sharing, not a copy)
 *   5. double-mmap of the same fd in one process -> same VA, refcounted unmap
 *   6. munmap -> segdetach -> re-import + re-map still sees the data
 *   7. remove-while-attached: name gone, contents persist for attachers
 *   8. exec-clean semantics: the child could attach at all ONLY because
 *      execve detached the fork-inherited mapping (kernel ignores SG_CEXEC
 *      for #g attaches; a leaked inherit fails "segments overlap" — the
 *      first bug this gate caught)
 *   9. /proc/<pid>/segment format probe (prints the raw line for the sweeper)
 *  10. cap probe: create-until-error, report the system-wide ceiling, free all
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/shm9.h>
#include <sys/wait.h>

static int npass;
static void ok(const char *what) { npass++; fprintf(stderr, "  ok %d: %s\n", npass, what); }
static void die(const char *what) {
	fprintf(stderr, "FAIL: %s (errno %d %s)\n", what, errno, strerror(errno));
	exit(1);
}

#define SEGSZ (1024 * 1024ul)
#define PAT(i) ((unsigned char)(((i) * 131) ^ ((i) >> 8)))

static void fill(unsigned char *p, unsigned long n, int salt) {
	for (unsigned long i = 0; i < n; i++) p[i] = PAT(i) ^ (unsigned char)salt;
}
static int check(const unsigned char *p, unsigned long n, int salt) {
	for (unsigned long i = 0; i < n; i++)
		if (p[i] != (unsigned char)(PAT(i) ^ (unsigned char)salt)) return 0;
	return 1;
}

static void print_own_segments(const char *tag) {
	char path[64], buf[4096];
	snprintf(path, sizeof path, "/proc/%d/segment", getpid());
	int fd = open(path, O_RDONLY);
	if (fd < 0) { fprintf(stderr, "  (no %s)\n", path); return; }
	long n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n > 0) { buf[n] = 0; fprintf(stderr, "-- %s %s:\n%s", tag, path, buf); }
}

static int child_main(const char *name) {
	int fd = cc9_shm_import(name, 0, SEGSZ);
	if (fd < 0) die("child: import");
	unsigned char *p = mmap(0, SEGSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) die("child: mmap");
	if (!check(p, SEGSZ, 0x00)) die("child: pattern from parent");
	fill(p, SEGSZ, 0x5A);                 /* mutate: parent must see this */
	print_own_segments("child");
	munmap(p, SEGSZ);
	close(fd);
	return 0;
}

int main(int argc, char **argv) {
	if (argc == 3 && strcmp(argv[1], "child") == 0)
		return child_main(argv[2]);

	/* 1: create + map + fill */
	int fd = cc9_shm_create(SEGSZ);
	if (fd < 0) die("create");
	unsigned char *p = mmap(0, SEGSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) die("mmap");
	fill(p, SEGSZ, 0x00);
	ok("create + mmap(MAP_SHARED) + fill");

	/* 2: export */
	char name[CC9_SHM_NAMELEN];
	unsigned long off, len;
	if (cc9_shm_export(fd, name, &off, &len) < 0) die("export");
	/* Phase B pool model (cc9 702e3b9): the first buffer sits at pool offset 0,
	 * and len is receiver-driven (export leaves it 0 on the wire). */
	if (off != 0) die("export triple");
	ok("export wire triple");

	/* 9 (early, format probe for the sweeper): our own segment table */
	print_own_segments("parent");
	fprintf(stderr, "-- shm VA: %p\n", (void *)p);
	ok("/proc segment format probe printed");

	/* 3+4: child across fork+exec mutates; we see it */
	int kid = fork();
	if (kid < 0) die("fork");
	if (kid == 0) {
		char *cargv[] = { argv[0], "child", name, 0 };
		execv(argv[0], cargv);
		_exit(127);
	}
	int st;
	if (waitpid(kid, &st, 0) != kid || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
		die("child failed");
	if (!check(p, SEGSZ, 0x5A)) die("child mutation not visible: NOT shared");
	ok("child (fork+exec) saw pattern and mutated it — TRUE sharing");

	/* 5: second mmap of the same fd -> same VA (attach table refcount) */
	unsigned char *p2 = mmap(0, SEGSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p2 != p) die("double-map returned different VA");
	munmap(p2, SEGSZ);                    /* refs 2 -> 1; must NOT detach */
	if (!check(p, SEGSZ, 0x5A)) die("early detach on refcounted unmap");
	ok("double-map same VA, refcounted unmap");

	/* 6: full unmap -> re-import -> data persists (name still present) */
	munmap(p, SEGSZ);
	close(fd);
	fd = cc9_shm_import(name, 0, SEGSZ);
	if (fd < 0) die("re-import");
	p = mmap(0, SEGSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) die("re-map");
	if (!check(p, SEGSZ, 0x5A)) die("data lost across detach/re-attach");
	ok("detach -> re-import -> data persists");

	/* 8: exec-clean — the child's successful attach in step 3 is itself the
	 * proof: had execve leaked the fork-inherited mapping, its segattach
	 * would have failed "segments overlap". */
	ok("execve detached inherited mappings (child attach succeeded)");

	/* 9: pool capacity probe. Phase B (cc9 702e3b9) sub-allocates every buffer
	 * out of ONE named pool segment, so this no longer probes the kernel NSEG
	 * cap (which Phase B exists to dodge) — it proves the pool bump-allocator
	 * hands out many independent buffers without hitting NSEG. Run it BEFORE the
	 * destructive remove below (all these buffers live in `name`'s pool). */
	int fds[256], nbuf = 0;
	for (; nbuf < 256; nbuf++) {
		fds[nbuf] = cc9_shm_create(4096);
		if (fds[nbuf] < 0) break;
	}
	fprintf(stderr, "-- pool probe: %d buffers from the pool (errno %d)\n", nbuf, errno);
	for (int i = 0; i < nbuf; i++)
		close(fds[i]);                    /* just close: they share the pool name */
	if (nbuf < 8) die("pool probe: fewer than 8 buffers available");
	ok("pool capacity probe (all closed)");

	/* 7 (last — destroys the pool): remove the pool name while attached. Contents
	 * must persist for existing attachers; new imports must be refused. */
	char gpath[64];
	snprintf(gpath, sizeof gpath, "#g/%s", name);
	extern long n9_remove(const char *);
	if (n9_remove(gpath) < 0) die("remove name");
	if (!check(p, SEGSZ, 0x5A)) die("contents died with the name");
	if (cc9_shm_import(name, 0, SEGSZ) >= 0) die("import after remove should fail");
	ok("remove-while-attached: contents persist, new attaches blocked");
	munmap(p, SEGSZ);
	close(fd);

	/* sweep smoke: run it; with grace 0 it may remove leftovers from crashed
	 * prior runs — must not remove anything attached. */
	cc9_shm_sweep("shm.", 0);
	ok("sweep smoke");

	printf("PASS %d/%d\n", npass, npass);
	return 0;
}
