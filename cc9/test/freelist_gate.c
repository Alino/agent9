/* freelist_gate — proves cc9's shm9 pool free-list reclaims freed buffer slots
 * instead of bumping pool_off forever. This is the fix for the ladybird9
 * Compositor leak: test-web reuses ONE long-lived Compositor/WebContent across
 * ~300 tests, and per-test canvas backing stores churned 256 MiB pool after pool
 * until the 1 GiB pid VA slab exhausted -> cc9_shm_create returned -1 -> a fatal
 * VERIFY -> the Compositor crashed ~test 114 at ~553 MB (measured on cirno).
 *
 * Checks (prints "PASS n/n" or dies):
 *   1. direct reuse: create -> export off -> close -> create -> export off2;
 *      off2 == off1 (the freed slot was handed back, not bumped past).
 *   2. size-class reuse: a churn of same-size create/free stays at ONE offset.
 *   3. churn survival: 2000 x (create 1 MiB + close) all succeed. Without the
 *      free-list this exhausts the 1 GiB slab (~1024 x 1 MiB across 4 pools) and
 *      cc9_shm_create fails — the exact exhaustion that crashed the Compositor.
 *   4. control: 300 creates WITHOUT freeing DO climb the offset (proves the gate
 *      can see a regression — a no-op free-list would fail check 1/3, not this).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/shm9.h>

static int npass;
static void ok(const char *what) { npass++; fprintf(stderr, "  ok %d: %s\n", npass, what); }
static void die(const char *what) {
	fprintf(stderr, "FAIL: %s (errno %d %s)\n", what, errno, strerror(errno));
	exit(1);
}

#define MB (1024ul * 1024ul)

static unsigned long create_off(unsigned long size, int *out_fd) {
	int fd = cc9_shm_create(size);
	if (fd < 0) die("cc9_shm_create");
	char name[64];
	unsigned long off = 0, len = 0;
	if (cc9_shm_export(fd, name, &off, &len) < 0) die("cc9_shm_export");
	*out_fd = fd;
	return off;
}

int main(void) {
	/* 1. direct reuse: a freed 1 MiB slot is handed back to the next create. */
	int fd;
	unsigned long off1 = create_off(MB, &fd);
	close(fd);                                  /* -> cc9_shm_forget_fd -> free-list push */
	unsigned long off2 = create_off(MB, &fd);
	if (off2 != off1) { fprintf(stderr, "off1=%lu off2=%lu\n", off1, off2); die("freed slot not reused"); }
	close(fd);
	ok("direct reuse: freed slot returned at same offset");

	/* 2. size-class reuse: a long same-size churn never advances the offset. */
	unsigned long base = 0;
	for (int i = 0; i < 500; i++) {
		unsigned long off = create_off(MB, &fd);
		if (i == 0) base = off;
		else if (off != base) { fprintf(stderr, "iter %d off=%lu base=%lu\n", i, off, base); die("same-size churn drifted"); }
		close(fd);
	}
	ok("size-class reuse: 500 same-size create/free stayed at one offset");

	/* 3. churn survival: 2000 x (create + close) all succeed. Without the
	 * free-list the offset climbs 1 MiB/iter, mints a new pool every 256, and
	 * exhausts the 1 GiB slab around iter 1024 -> cc9_shm_create returns -1. */
	for (int i = 0; i < 2000; i++) {
		int f = cc9_shm_create(MB);
		if (f < 0) { fprintf(stderr, "create failed at churn iter %d\n", i); die("slab exhausted -> the leak is back"); }
		close(f);
	}
	ok("churn survival: 2000 create/close cycles, no slab exhaustion");

	/* 4. control: WITHOUT freeing, the offset MUST climb (else the gate is blind
	 * to a regression). Hold the fds so nothing is reclaimed. */
	int held[300];
	unsigned long first = 0, last = 0;
	for (int i = 0; i < 300; i++) {
		unsigned long off = create_off(MB, &held[i]);
		if (i == 0) first = off;
		last = off;
	}
	/* 300 x 1 MiB with no reuse spans past one 256 MiB pool; the offset either
	 * climbs within a pool or resets into a freshly minted pool — either way it
	 * must NOT be pinned at `first` like the reuse path is. */
	int climbed = 0;
	for (int i = 1; i < 300 && !climbed; i++)
		; /* offsets recorded via last; a single non-`first` sighting suffices */
	if (last == first) die("control: offset never moved without frees (gate is blind)");
	(void)climbed;
	for (int i = 0; i < 300; i++) close(held[i]);
	ok("control: no-free path climbs the offset (regression-visible)");

	fprintf(stderr, "PASS %d/%d\n", npass, npass);
	return 0;
}
