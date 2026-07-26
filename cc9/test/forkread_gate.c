/* forkread_gate — a thread blocked reading a pipe keeps working across fork+exec.
 *
 * cc9's poll layer runs one reader THREAD per polled fd, and fork() here is
 * rfork(RFPROC|RFFDG|RFENVG): the child gets a COPY of the fd table while that
 * thread sits in a read on the parent's copy. If the fork disturbs the blocked
 * read — or the child's pre-exec fd cleanup reaches back — the thread never wakes
 * again, bytes pile up unread in the pipe, and a program looks frozen at its input
 * while everything else in it keeps running. That is the shape of a node9 bug where
 * pi stopped seeing keystrokes after running a shell tool.
 *
 * Run on 9front:  forkread_gate  -> "forkread_gate N/N PASS"
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>

extern int execv(const char *, char *const *);

static int pass, total;
static void ck(int ok, const char *what)
{
	total++;
	printf("%d %s: %s\n", total, what, ok ? "PASS" : "FAIL");
	if (ok) pass++;
}

static int fds[2];
static volatile int got_before, got_after, forked;

static void *reader(void *a)
{
	char buf[64];
	(void)a;
	for (;;) {
		long n = read(fds[0], buf, sizeof buf);
		if (n <= 0)
			return 0;
		if (forked)
			got_after += (int)n;
		else
			got_before += (int)n;
	}
}

int main(void)
{
	if (pipe(fds) != 0) { printf("pipe failed\nforkread_gate 0/1 FAIL\n"); return 1; }

	pthread_t t;
	pthread_create(&t, 0, reader, 0);

	/* the thread is now blocked in read() */
	write(fds[1], "aaa", 3);
	usleep(300000);
	ck(got_before == 3, "reader thread receives before the fork");

	/* fork+exec, exactly what a child_process spawn does */
	forked = 1;
	int pid = fork();
	if (pid == 0) {
		{ char *av[] = { "cat", "/env/user", 0 }; execv("/bin/cat", av); }
		_exit(127);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	usleep(200000);

	/* and now the part that matters: does the blocked thread still get bytes? */
	write(fds[1], "bbbb", 4);
	usleep(500000);
	ck(got_after == 4, "reader thread still receives after fork+exec");

	printf("forkread_gate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
