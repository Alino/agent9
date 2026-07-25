/* ttygate.c — isatty() on a real console.
 *
 * isatty() used to be "fd is 0-2 AND $TERM is set". A terminal emulator
 * (alacritty9/vts/9term) talks to its child over pipes and sets $TERM, so that
 * heuristic is all we have there — but a BARE CONSOLE session sets no $TERM and
 * doesn't need a heuristic: its fd 0 really is /dev/cons. Calling the real
 * console not-a-terminal made nvim's TUI refuse to start: over `drawterm -G` it
 * hung forever having written zero bytes (parent in Semacquire, children in
 * Pread). fs.c now also asks the kernel (fd2path == "/dev/cons").
 *
 * Build/run on 9front (cc9). The environment decides the answer, so say which
 * one this is:
 *   ttygate cons    # from a rio window / drawterm -G / the physical console
 *   ttygate pipe    # stdin is a pipe or a file (listen1, `ttygate pipe </x`)
 * Expects "ttygate N/N PASS". Run the cons case with $TERM cleared — with TERM
 * set the old heuristic answers too and the new arm is never exercised. And do
 * NOT redirect its output to capture it: that makes fd1 a file, which is not a
 * console and correctly fails. Read the exit status instead (rc: `$status`).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

extern long n9_fd2path(int, char *, int);

static int pass, total;

static void ok(const char *what, int cond, const char *detail) {
	total++;
	printf("%d %s: %s %s\n", total, what, cond ? "PASS" : "FAIL", detail ? detail : "");
	if (cond) pass++;
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";
	int want_cons = strcmp(mode, "cons") == 0;
	char p[128];
	int i;

	if (!want_cons && strcmp(mode, "pipe") != 0) {
		printf("usage: ttygate cons|pipe\n");
		return 2;
	}
	printf("TERM=%s\n", getenv("TERM") ? getenv("TERM") : "(unset)");
	for (i = 0; i < 3; i++) {
		if (n9_fd2path(i, p, sizeof p) < 0) strcpy(p, "(no path)");
		printf("fd%d %s isatty=%d\n", i, p, isatty(i));
	}

	if (want_cons) {
		/* the regression: a console with no $TERM must still be a tty */
		ok("cons-is-tty", isatty(0) == 1, "fd0 = /dev/cons");
		ok("cons-fd1", isatty(1) == 1, 0);
	} else {
		/* and the ceiling stays honest: a pipe with no $TERM is not one */
		ok("pipe-not-tty", getenv("TERM") != 0 || isatty(0) == 0, "no TERM, no tty");
	}
	/* out-of-range fds are never ttys, console session or not */
	ok("fd-range", isatty(-1) == 0 && isatty(9) == 0, 0);

	printf("ttygate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
