/* conswrite.c — do non-blocking writes reach a console?
 *
 * libuv's uv_tty_init() sets O_NONBLOCK on the tty fd. In cc9 that flips the fd
 * onto poll.c's write ring (fs.c:169 -> cc9_poll_write), drained by a writer
 * thread. nvim's TUI emits its init sequence and then goes silent on a 9P
 * console, which is what that ring would look like if it never drained.
 *
 * Writes markers to fd 1 (point it at the console under test) and logs what
 * each write returned to argv[1]; the caller compares the console's text
 * against the log.
 *
 *   conswrite /tmp/conswrite.log </dev/cons >/dev/cons
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

static FILE *log;

static void mark(const char *what, const char *s) {
	errno = 0;
	long n = write(1, s, strlen(s));
	fprintf(log, "%-14s wrote=%ld errno=%d\n", what, n, errno);
	fflush(log);
}

int
main(int argc, char **argv)
{
	log = fopen(argc > 1 ? argv[1] : "/tmp/conswrite.log", "w");
	if (!log) return 1;

	mark("blocking", "A-BLOCKING\n");

	int fl = fcntl(1, F_GETFL);
	int r = fcntl(1, F_SETFL, fl | O_NONBLOCK);
	fprintf(log, "F_SETFL O_NONBLOCK r=%d (old flags %d) errno=%d\n", r, fl, errno);
	fflush(log);

	mark("nonblock-1", "B-NONBLOCK\n");
	sleep(1);                      /* let a drain thread run */
	mark("nonblock-2", "C-NONBLOCK\n");
	sleep(2);

	/* and a big one: the ring is finite, a full screen repaint is ~KBs */
	{
		char big[8192];
		memset(big, 'x', sizeof big);
		big[0] = 'D'; big[sizeof big - 1] = '\n';
		errno = 0;
		long n = write(1, big, sizeof big);
		fprintf(log, "%-14s wrote=%ld errno=%d\n", "nonblock-big", n, errno);
		fflush(log);
	}
	sleep(2);

	r = fcntl(1, F_SETFL, fl);     /* back to blocking */
	fprintf(log, "restore r=%d\n", r);
	mark("blocking-2", "E-BLOCKING\n");
	fclose(log);
	return 0;
}
