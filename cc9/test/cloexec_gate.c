/* cloexec_gate — O_CLOEXEC on open(2) must actually survive to exec(2).
 *
 * cc9's open() parsed O_ACCMODE/O_TRUNC/O_CREAT/O_EXCL/O_APPEND/O_DIRECTORY and
 * silently dropped O_CLOEXEC, so an fd opened close-on-exec was inherited by
 * EVERY spawned child. That is not a cosmetic leak: SQLite opens its database
 * files with O_CLOEXEC deliberately, and ladybird's WebContent/RequestServer
 * were inheriting the UI process's writable History.db and Ladybird.db handles.
 * This port runs SQLite on the lock-free "unix-none" VFS, so several processes
 * ended up holding writable handles to a database exactly one of them thinks it
 * owns -- and a persistent profile wedged.
 *
 * The gate re-execs itself: the parent opens one fd WITH O_CLOEXEC and one
 * WITHOUT, passes both numbers in argv, and the child reports which are still
 * open. Only a real exec can test this -- fork alone keeps both.
 *
 * Run on 9front:  cloexec_gate   -> "cloexec_gate N/N PASS"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int pass, total;
static void ck(int ok, const char *what)
{
	total++;
	printf("%d %s: %s\n", total, what, ok ? "PASS" : "FAIL");
	fflush(stdout);
	if (ok) pass++;
}

static int fd_is_open(int fd)
{
	struct stat st;
	return fstat(fd, &st) == 0;
}

int main(int argc, char **argv)
{
	/* ---- child arm: report which of the two inherited fds survived ---- */
	if (argc == 4 && strcmp(argv[1], "--child") == 0) {
		int cloexec_fd = atoi(argv[2]);
		int plain_fd = atoi(argv[3]);
		printf("%d %d\n", fd_is_open(cloexec_fd), fd_is_open(plain_fd));
		fflush(stdout);
		return 0;
	}

	const char *path = "/tmp/cloexec_gate.tmp";
	int seed = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	if (seed < 0) {
		printf("setup: cannot create %s: %s\ncloexec_gate 0/1 FAIL\n", path, strerror(errno));
		return 1;
	}
	(void)write(seed, "x", 1);
	close(seed);

	int cloexec_fd = open(path, O_RDONLY | O_CLOEXEC);
	int plain_fd = open(path, O_RDONLY);
	if (cloexec_fd < 0 || plain_fd < 0) {
		printf("setup: reopen failed\ncloexec_gate 0/1 FAIL\n");
		return 1;
	}

	/* In THIS process both must still be usable -- O_CLOEXEC closes at exec,
	 * not at open. Getting this wrong would break the flag's whole point. */
	ck(fd_is_open(cloexec_fd), "O_CLOEXEC fd is usable before exec");

	char a2[16], a3[16];
	snprintf(a2, sizeof a2, "%d", cloexec_fd);
	snprintf(a3, sizeof a3, "%d", plain_fd);

	int fds[2];
	if (pipe(fds) != 0) {
		printf("setup: pipe failed\ncloexec_gate %d/%d FAIL\n", pass, total + 1);
		return 1;
	}

	pid_t kid = fork();
	if (kid < 0) {
		printf("setup: fork failed\ncloexec_gate %d/%d FAIL\n", pass, total + 1);
		return 1;
	}
	if (kid == 0) {
		close(fds[0]);
		dup2(fds[1], 1);
		if (fds[1] != 1)
			close(fds[1]);
		char *args[5] = { argv[0], (char *)"--child", a2, a3, 0 };
		execv(argv[0], args);
		_exit(127);
	}
	close(fds[1]);

	char buf[64];
	int n = 0, r;
	while (n < (int)sizeof buf - 1 && (r = (int)read(fds[0], buf + n, sizeof buf - 1 - n)) > 0)
		n += r;
	buf[n > 0 ? n : 0] = 0;
	close(fds[0]);
	int status = 0;
	waitpid(kid, &status, 0);

	int child_saw_cloexec = -1, child_saw_plain = -1;
	if (sscanf(buf, "%d %d", &child_saw_cloexec, &child_saw_plain) != 2) {
		printf("   child said: '%s'\n", buf);
		ck(0, "child reported inherited fd state");
		printf("cloexec_gate %d/%d FAIL\n", pass, total);
		return 1;
	}

	/* The control: a plain fd MUST be inherited. If this fails the test is
	 * wrong (or exec is losing every fd), not the CLOEXEC handling. */
	ck(child_saw_plain == 1, "a plain fd IS inherited across exec (control)");
	ck(child_saw_cloexec == 0, "an O_CLOEXEC fd is NOT inherited across exec");

	close(cloexec_fd);
	close(plain_fd);
	unlink(path);

	printf("cloexec_gate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
