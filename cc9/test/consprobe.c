/* consprobe.c — report everything libuv's tty path asks about fd 0.
 *
 * uv_guess_handle() consults isatty() first and returns UV_TTY for a console,
 * which sends nvim down uv_tty_init() instead of the pipe path it uses under
 * alacritty9. uv_tty_init then needs fstat/ttyname_r/ptsname/fcntl/tcgetattr
 * to behave. Run this with fd 0 on the console under test:
 *
 *   consprobe </dev/cons >/tmp/probe.log
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/stat.h>

extern long n9_fd2path(int, char *, int);
extern char *ptsname(int);

int
main(void)
{
	char p[256];
	struct stat st;
	struct termios t;
	int r;

	if (n9_fd2path(0, p, sizeof p) < 0) strcpy(p, "(none)");
	printf("fd2path   %s\n", p);
	printf("isatty    %d\n", isatty(0));

	if (fstat(0, &st) != 0) {
		printf("fstat     FAILED errno=%d\n", errno);
	} else {
		const char *kind = S_ISREG(st.st_mode)  ? "S_IFREG (-> uv_guess_handle UV_FILE)"
		                 : S_ISCHR(st.st_mode)  ? "S_IFCHR"
		                 : S_ISFIFO(st.st_mode) ? "S_IFIFO (-> UV_NAMED_PIPE)"
		                 : S_ISDIR(st.st_mode)  ? "S_IFDIR" : "other";
		printf("fstat     mode=%o %s\n", (unsigned)st.st_mode, kind);
	}

	memset(p, 0, sizeof p);
	r = ttyname_r(0, p, sizeof p);
	printf("ttyname_r r=%d path=%s errno=%d\n", r, p[0] ? p : "(empty)", errno);

	{ char *n = ptsname(0); printf("ptsname   %s\n", n ? n : "(null)"); }

	errno = 0;
	r = fcntl(0, F_GETFL);
	printf("F_GETFL   r=%d errno=%d\n", r, errno);

	errno = 0;
	r = tcgetattr(0, &t);
	printf("tcgetattr r=%d errno=%d\n", r, errno);

	errno = 0;
	r = open("/dev/tty", O_RDONLY);
	printf("open/dev/tty r=%d errno=%d\n", r, errno);
	if (r >= 0) close(r);

	/* the reopen uv_tty_init attempts when ttyname_r succeeds */
	if (p[0]) {
		errno = 0;
		r = open(p, O_RDONLY);
		printf("reopen    %s r=%d errno=%d\n", p, r, errno);
		if (r >= 0) close(r);
	}
	return 0;
}
