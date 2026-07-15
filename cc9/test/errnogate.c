/* errnogate.c — errno/errstr fidelity: the right error, and YOUR error.
 *
 * Plan 9 reports errors as text, so every POSIX call here has to map that text to
 * an errno. Hand-rolling one instead is the recurring bug in this runtime:
 *   - mkdir  said EACCES for everything-but-EEXIST  -> broke every mkdir -p
 *   - open   said ENOENT for every non-O_CREAT fail -> a permission denial, a
 *            non-directory component and "is a directory" were indistinguishable
 *   - unlink said ENOENT for everything             -> ENOTEMPTY vanished, which
 *            is exactly what remove_dir_all keys its recursion off
 * A wrong errno is worse than a vague one: callers BRANCH on it.
 *
 * The errstr text is also per-thread here, like errno. It used to be one global
 * buffer, guarded only by "compare the stashed errno" — which fails the moment two
 * threads take an ENOENT at once, and then a caller prints another thread's error.
 * Servo hit exactly that: a missing font directory reported as
 * "file does not exist: '.../reg.sqlite-wal'". Test 6 is that race, on purpose.
 *
 * Build/run on 9front (cc9):  errnogate
 * Expects "errnogate N/N PASS".
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>

extern const char *__n9_errstr_last(int *);

static int pass, total;

static void ok(const char *what, int cond, const char *detail) {
	total++;
	printf("%d %s: %s %s\n", total, what, cond ? "PASS" : "FAIL", detail ? detail : "");
	if (cond) pass++;
}

static char base[64];

/* 1: a plain missing file is ENOENT (the easy case that always worked). */
static void test_missing(void) {
	char p[128];
	snprintf(p, sizeof p, "%s/nope", base);
	errno = 0;
	int fd = open(p, O_RDONLY);
	ok("open missing -> ENOENT", fd < 0 && errno == ENOENT, strerror(errno));
	if (fd >= 0) close(fd);
}

/* 2: opening a DIRECTORY for writing is not "file not found". Plan 9 says
 *    "is a directory"; the old open() flattened it to ENOENT. */
static void test_dir_for_write(void) {
	errno = 0;
	int fd = open(base, O_WRONLY);
	int e = errno;
	char d[64];
	snprintf(d, sizeof d, "errno=%d (%s)", e, strerror(e));
	ok("open dir O_WRONLY -> not ENOENT", fd < 0 && e != ENOENT, d);
	if (fd >= 0) close(fd);
}

/* 3: a path whose parent is a FILE is ENOTDIR, not ENOENT. */
static void test_notdir(void) {
	char f[128], p[192];
	snprintf(f, sizeof f, "%s/afile", base);
	int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd >= 0) { write(fd, "x", 1); close(fd); }
	snprintf(p, sizeof p, "%s/under", f);   /* afile/under — afile is not a dir */
	errno = 0;
	fd = open(p, O_RDONLY);
	char d[64];
	snprintf(d, sizeof d, "errno=%d (%s)", errno, strerror(errno));
	ok("open under a file -> ENOTDIR", fd < 0 && errno == ENOTDIR, d);
	if (fd >= 0) close(fd);
}

/* 4: rmdir/unlink of a NON-EMPTY directory is ENOTEMPTY. This is the one
 *    remove_dir_all needs; ENOENT here makes it think the job is already done. */
static void test_notempty(void) {
	char d[128], inner[192];
	snprintf(d, sizeof d, "%s/full", base);
	mkdir(d, 0777);
	snprintf(inner, sizeof inner, "%s/child", d);
	int fd = open(inner, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd >= 0) close(fd);
	errno = 0;
	int r = rmdir(d);
	int eno = 0;
	const char *s = __n9_errstr_last(&eno);
	char det[224];
	snprintf(det, sizeof det, "errno=%d (%s) errstr=\"%s\"", errno, strerror(errno), s ? s : "");
	ok("rmdir non-empty -> ENOTEMPTY", r < 0 && errno == ENOTEMPTY, det);
	remove(inner); rmdir(d);
}

/* 5: the errstr is actually captured — a failure must leave a real message, not
 *    an empty string (open used to never stash one at all). */
static void test_errstr_present(void) {
	char p[128];
	snprintf(p, sizeof p, "%s/nope-again", base);
	errno = 0;
	int fd = open(p, O_RDONLY);
	(void)fd;
	int eno = 0;
	const char *s = __n9_errstr_last(&eno);
	char d[192];
	snprintf(d, sizeof d, "errstr=\"%s\"", s ? s : "(null)");
	ok("failure stashes a real errstr", s && s[0] != 0, d);
}

/* 6: THE RACE. Two threads failing at once must each see THEIR OWN errstr.
 *    With one process-global buffer this fails within a few iterations. */
#define ITERS 300
static int race_bad;      /* a thread saw the other's path in its errstr */
static int race_checked;

static void *racer(void *arg) {
	const char *tag = arg;
	char p[128];
	snprintf(p, sizeof p, "%s/%s-missing", base, tag);
	const char *other = strcmp(tag, "alpha") == 0 ? "bravo" : "alpha";
	for (int i = 0; i < ITERS; i++) {
		errno = 0;
		int fd = open(p, O_RDONLY);
		if (fd >= 0) { close(fd); continue; }
		int eno = 0;
		const char *s = __n9_errstr_last(&eno);
		if (!s || !s[0]) continue;          /* nothing stashed: not this test's business */
		race_checked++;
		/* Our own path must be the one named. Seeing the other thread's tag is
		 * the exact cross-talk this test exists to catch. */
		if (strstr(s, other) != 0) race_bad++;
	}
	return 0;
}

static void test_errstr_per_thread(void) {
	pthread_t a, b;
	pthread_create(&a, 0, racer, (void *)"alpha");
	pthread_create(&b, 0, racer, (void *)"bravo");
	pthread_join(a, 0);
	pthread_join(b, 0);
	char d[96];
	snprintf(d, sizeof d, "%d checked, %d saw another thread's errstr", race_checked, race_bad);
	ok("errstr is per-thread under contention", race_bad == 0, d);
}

int
main(void)
{
	snprintf(base, sizeof base, "/tmp/errnogate.%d", (int)getpid());
	if (mkdir(base, 0777) < 0) { printf("setup: mkdir %s failed: %s\n", base, strerror(errno)); return 1; }

	test_missing();
	test_dir_for_write();
	test_notdir();
	test_notempty();
	test_errstr_present();
	test_errstr_per_thread();

	{ char f[128]; snprintf(f, sizeof f, "%s/afile", base); remove(f); }
	rmdir(base);

	printf("errnogate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
