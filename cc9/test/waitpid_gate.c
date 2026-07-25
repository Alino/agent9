/* waitpid_gate — a child's exit is reported no matter how the PREVIOUS child was
 * reaped, and a failed exec is still a child that exited.
 *
 * Why it matters: cc9 reaps children with a per-parent thread reading
 * /proc/<pid>/wait into a zombie table (await(2) blocks and only works in the
 * forking proc, which WNOHANG needs not to). node9's child_process hit a case
 * where the SECOND child's status never arrived: waitpid(pid, WNOHANG) answered
 * "still running" forever, so pi's shell tool waited for a process that had
 * already died. It only showed up when both children failed to exec (they die
 * within microseconds of the fork), which is exactly what pi does — `which bash`
 * then `sh -c ...` on a system that has neither.
 *
 * Run on 9front:  waitpid_gate  -> "waitpid_gate N/N PASS"
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

extern void cc9_wait_stats(int *zn, int *pending, int *running);
static void stats(const char *when)
{
	int zn = 0, pend = 0, run = 0;
	cc9_wait_stats(&zn, &pend, &run);
	printf("   [%s] collected=%d live-children=%d notifier=%d\n", when, zn, pend, run);
}

static int pass, total;
static void ck(int ok, const char *what)
{
	total++;
	printf("%d %s: %s\n", total, what, ok ? "PASS" : "FAIL");
	if (ok) pass++;
}

/* fork+exec a program that does not exist; the child exits 127 like a shell would */
static int spawn_missing(void)
{
	int pid = fork();
	if (pid == 0) {
		execvp("definitely-not-a-program-here", (char *const[]){ "definitely-not-a-program-here", 0 });
		_exit(127);
	}
	return pid;
}

static int spawn_true(void)
{
	int pid = fork();
	if (pid == 0) {
		execvp("/bin/cat", (char *const[]){ "/bin/cat", "/env/user", 0 });
		_exit(127);
	}
	return pid;
}

/* poll for up to ~4s, the way an event loop does */
static int poll_exit(int pid, int *st)
{
	for (int i = 0; i < 400; i++) {
		int r = waitpid(pid, st, WNOHANG);
		if (r == pid) return 1;
		if (r < 0) return 0;
		usleep(10000);
	}
	return 0;
}

int main(void)
{
	int st = 0;

	stats("start");
	int a = spawn_missing();
	stats("after fork a");
	ck(waitpid(a, &st, 0) == a && WIFEXITED(st) && WEXITSTATUS(st) == 127,
	   "blocking waitpid reports a failed exec as exit 127");

	stats("after wait a");
	int b = spawn_missing();
	stats("after fork b");
	ck(poll_exit(b, &st) && WIFEXITED(st) && WEXITSTATUS(st) == 127,
	   "WNOHANG poll sees the next failed-exec child after a blocking wait");

	stats("after poll b");
	int c = spawn_true();
	ck(poll_exit(c, &st) && WIFEXITED(st) && WEXITSTATUS(st) == 0,
	   "WNOHANG poll sees a normal child too");

	/* two children in flight at once, reaped by pid */
	int d = spawn_missing(), e = spawn_true();
	int gotd = poll_exit(d, &st), gote = poll_exit(e, &st);
	ck(gotd && gote, "two concurrent children are both reaped by pid");

	printf("waitpid_gate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
