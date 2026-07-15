/* mkdirgate.c — mkdir(2) errno mapping.
 *
 * mkdir used to report EACCES for every failure that wasn't "already exists".
 * That silently breaks every mkdir -p implementation: Rust's create_dir_all (and
 * the shell's mkdir -p) only recurse to build a missing parent when mkdir reports
 * ENOENT. Told "permission denied" instead, they give up — which is how Servo
 * failed to create $home/.config/servo/default on a box where glenda plainly
 * could write to $home.
 *
 * Build/run on 9front (cc9):  mkdirgate
 * Expects "mkdirgate N/N PASS".
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

static int pass, total;

static void ok(const char *what, int cond, const char *detail) {
	total++;
	if (cond) { pass++; printf("%d %s: PASS %s\n", total, what, detail ? detail : ""); }
	else      { printf("%d %s: FAIL %s\n", total, what, detail ? detail : ""); }
}

int
main(void)
{
	char base[64], sub[128], deep[192];
	snprintf(base, sizeof base, "/tmp/mkdirgate.%d", (int)getpid());
	snprintf(sub,  sizeof sub,  "%s/a", base);
	snprintf(deep, sizeof deep, "%s/b/c", base);   /* parent b does not exist */

	/* 1: a plain mkdir works. */
	ok("mkdir", mkdir(base, 0777) == 0, "");

	/* 2: THE REGRESSION. Missing intermediate component must be ENOENT, not
	 *    EACCES — this is the errno mkdir -p keys off of. */
	errno = 0;
	int r = mkdir(deep, 0777);
	ok("missing parent -> ENOENT", r < 0 && errno == ENOENT,
	   r == 0 ? "unexpectedly succeeded" : strerror(errno));

	/* 3: existing directory must still be EEXIST. */
	ok("mkdir sub", mkdir(sub, 0777) == 0, "");
	errno = 0;
	r = mkdir(sub, 0777);
	ok("existing -> EEXIST", r < 0 && errno == EEXIST,
	   r == 0 ? "unexpectedly succeeded" : strerror(errno));

	/* 5: mkdir over an existing regular FILE. Plan 9's create(2) truncates an
	 *    existing name, so this must be shown not to eat the file's contents —
	 *    silent data loss would be far worse than a wrong errno. */
	char f[128];
	snprintf(f, sizeof f, "%s/afile", base);
	FILE *fp = fopen(f, "w");
	if (fp) { fputs("precious", fp); fclose(fp); }
	errno = 0;
	r = mkdir(f, 0777);
	ok("mkdir over file -> EEXIST", r < 0 && errno == EEXIST,
	   r == 0 ? "unexpectedly succeeded" : strerror(errno));
	char rb[32];
	memset(rb, 0, sizeof rb);
	fp = fopen(f, "r");
	if (fp) { if (!fgets(rb, sizeof rb, fp)) rb[0] = 0; fclose(fp); }
	ok("file survives mkdir", strcmp(rb, "precious") == 0,
	   rb[0] ? rb : "file was TRUNCATED/DESTROYED");
	remove(f);

	/* 6: the create_dir_all shape end-to-end — build the chain the way a
	 *    mkdir -p does, driven by the errno above. */
	char b[128];
	snprintf(b, sizeof b, "%s/b", base);
	int built = mkdir(b, 0777) == 0 && mkdir(deep, 0777) == 0;
	ok("parent-then-child chain", built, built ? "" : strerror(errno));

	rmdir(deep); rmdir(b); rmdir(sub); rmdir(base);

	printf("mkdirgate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
