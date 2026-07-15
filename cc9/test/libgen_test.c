/* libgen_test — the POSIX dirname/basename table. The edge cases (trailing
 * slashes, all-slashes, no-slash) are the entire reason these functions are
 * hard; this is the table they were written against.
 *
 *   cc9/host/cc9 run cc9/test/libgen_test.c    -> "libgen: N/N ok"
 */
#include <libgen.h>
#include <stdio.h>
#include <string.h>

static int fails;

static void
ck(const char *in, const char *wantdir, const char *wantbase)
{
	char a[256], b[256];
	const char *gd, *gb;

	/* dirname/basename may modify their argument — give each its own copy. */
	strcpy(a, in);
	strcpy(b, in);
	gd = dirname(a);
	gb = basename(b);
	if (strcmp(gd, wantdir) != 0) {
		printf("FAIL dirname(\"%s\") = \"%s\", want \"%s\"\n", in, gd, wantdir);
		fails++;
	}
	if (strcmp(gb, wantbase) != 0) {
		printf("FAIL basename(\"%s\") = \"%s\", want \"%s\"\n", in, gb, wantbase);
		fails++;
	}
}

int
main(void)
{
	/* The table from POSIX / the dirname(3) and basename(3) man pages. */
	ck("/usr/lib",   "/usr", "lib");
	ck("/usr/lib/",  "/usr", "lib");
	ck("/usr/",      "/",    "usr");
	ck("usr",        ".",    "usr");
	ck("usr/",       ".",    "usr");
	ck("/",          "/",    "/");
	ck("//",         "/",    "/");
	ck(".",          ".",    ".");
	ck("..",         ".",    "..");
	ck("a/b",        "a",    "b");
	ck("a/b/c",      "a/b",  "c");
	ck("/a",         "/",    "a");
	ck("///a///b",   "///a", "b");
	ck("",           ".",    ".");

	if (fails == 0)
		printf("libgen: all ok\n");
	else
		printf("libgen: %d FAILURES\n", fails);
	return fails != 0;
}
