/* consread.c — what does a non-blocking read on a console report?
 *
 * nvim's UI client stops the TUI (and leaves the alternate screen up, cursor
 * blinking, nothing painted) when its input stream reports EOF or an error.
 * On a console with no typing, the honest answers are EAGAIN from read() and a
 * 0 return from poll(); EOF (0) or EIO instead would end the client instantly.
 *
 *   consread /tmp/consread.log </dev/cons
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

int
main(int argc, char **argv)
{
	FILE *log = fopen(argc > 1 ? argv[1] : "/tmp/consread.log", "w");
	char buf[256];
	if (!log) return 1;

	int fl = fcntl(0, F_GETFL);
	fprintf(log, "F_GETFL      %d\n", fl);
	fprintf(log, "F_SETFL      %d\n", fcntl(0, F_SETFL, fl | O_NONBLOCK));
	fflush(log);

	for (int i = 0; i < 3; i++) {
		errno = 0;
		long n = read(0, buf, sizeof buf);
		fprintf(log, "read#%d       n=%ld errno=%d%s\n", i, n, errno,
		        n == 0 ? "   <-- EOF: this is what kills the UI client" : "");
		fflush(log);

		struct pollfd p = { .fd = 0, .events = POLLIN };
		errno = 0;
		int r = poll(&p, 1, 400);
		fprintf(log, "poll#%d       r=%d revents=%#x errno=%d%s\n", i, r, p.revents, errno,
		        (p.revents & (POLLHUP | POLLERR)) ? "   <-- HUP/ERR" : "");
		fflush(log);
	}
	fclose(log);
	return 0;
}
