/* pollfork_gate — poll() keeps reporting a pipe readable after the process forks.
 *
 * This is the exact path an event loop uses: poll() on a pipe, read when it says
 * readable, and somewhere in between spawn a child. cc9 serves poll() from a reader
 * THREAD per fd filling a ring, and fork() here is rfork(RFPROC|RFFDG|RFENVG) — the
 * child gets a copy of the fd group while that thread is mid-read. If the fork leaves
 * the ring not waking the reactor, everything written afterwards sits in the pipe
 * unread: a program keeps running but never sees another keystroke. That is what
 * happened to pi after it ran a shell tool.
 *
 * Run on 9front:  pollfork_gate  -> "pollfork_gate N/N PASS"
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>

extern int execv(const char *, char *const *);

static int pass, total;
static void ck(int ok, const char *what)
{
	total++;
	printf("%d %s: %s\n", total, what, ok ? "PASS" : "FAIL");
	if (ok) pass++;
}

/* poll for up to ~3s, then read whatever arrived */
static int poll_read(int fd, char *buf, int len)
{
	struct pollfd p = { fd, POLLIN, 0 };
	int r = poll(&p, 1, 3000);
	if (r <= 0)
		return -1;
	return (int)read(fd, buf, len);
}

int main(void)
{
	int fds[2];
	char buf[64];

	if (pipe(fds) != 0) { printf("pipe failed\npollfork_gate 0/1 FAIL\n"); return 1; }

	write(fds[1], "aaa", 3);
	ck(poll_read(fds[0], buf, sizeof buf) == 3, "poll+read before any fork");

	int pid = fork();
	if (pid == 0) {
		char *av[] = { "cat", "/env/user", 0 };
		execv("/bin/cat", av);
		_exit(127);
	}
	int st = 0;
	waitpid(pid, &st, 0);

	write(fds[1], "bbbb", 4);
	ck(poll_read(fds[0], buf, sizeof buf) == 4, "poll+read after fork+exec");

	/* and again, since pi survives one tool call and dies on the next input */
	write(fds[1], "ccccc", 5);
	ck(poll_read(fds[0], buf, sizeof buf) == 5, "poll+read still works on the next write");

	/* A child that never execs — it just exits through the runtime's own exit path. */
	pid = fork();
	if (pid == 0)
		_exit(3);
	waitpid(pid, &st, 0);
	write(fds[1], "ee", 2);
	ck(poll_read(fds[0], buf, sizeof buf) == 2, "poll+read survives a child that only _exit()s");

	/* And the case pi hits on every tool call: the exec FAILS (no `which` here), so
	 * the child falls through to _exit(127). */
	pid = fork();
	if (pid == 0) {
		char *av[] = { "definitely-not-here", 0 };
		execv("/bin/definitely-not-here", av);
		_exit(127);
	}
	waitpid(pid, &st, 0);
	ck(WIFEXITED(st) && WEXITSTATUS(st) == 127, "a failed exec reports 127");

	write(fds[1], "dd", 2);
	ck(poll_read(fds[0], buf, sizeof buf) == 2, "poll+read survives a FAILED exec");

	printf("pollfork_gate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
