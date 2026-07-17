/* wake_gate — can a pipe write from thread B wake a poll() in thread A?
 *
 * Ladybird's EventLoop wake: worker thread writes 0i32 to the loop's wake
 * pipe; the loop thread polls the read end with POLLIN. If this doesn't wake
 * under cc9's poll layer, every cross-thread deferred_invoke is lost — which
 * is exactly the RequestServer DNS hang shape (completion posted from a
 * ThreadPool worker never runs on the main loop).
 *
 * Three rounds:
 *   1. write BEFORE poll (data already pending — the easy case)
 *   2. poll first, write 1s later from another thread (the real case)
 *   3. repeat round 2 (ring/reader-thread state after a prior wake)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>

static int fds[2];

static void *late_writer(void *arg)
{
	(void)arg;
	usleep(1000000);
	int v = 0;
	long n = write(fds[1], &v, sizeof v);
	printf("  writer: wrote %ld bytes (1s after poll started)\n", n);
	fflush(stdout);
	return 0;
}

static int poll_round(const char *name, int spawn_writer)
{
	pthread_t tid;
	if (spawn_writer)
		pthread_create(&tid, 0, late_writer, 0);

	struct pollfd p = { .fd = fds[0], .events = POLLIN, .revents = 0 };
	int rc = poll(&p, 1, 8000);
	printf("%s: poll rc=%d revents=0x%x -> %s\n", name, rc, p.revents,
	       rc == 1 && (p.revents & POLLIN) ? "WOKE" : "TIMEOUT/FAIL");
	fflush(stdout);

	if (rc == 1 && (p.revents & POLLIN)) {
		int v;
		long n = read(fds[0], &v, sizeof v);
		printf("  drained %ld bytes\n", n);
		fflush(stdout);
	}
	if (spawn_writer)
		pthread_join(tid, 0);
	return rc == 1;
}

int main(void)
{
	if (pipe(fds) != 0) { printf("pipe FAILED\n"); return 1; }
	/* upstream uses pipe2(O_CLOEXEC); CLOEXEC doesn't affect wake semantics */

	int v = 0;
	write(fds[1], &v, sizeof v);
	int r1 = poll_round("round 1 (write-before-poll)", 0);

	int r2 = poll_round("round 2 (write-during-poll, other thread)", 1);
	int r3 = poll_round("round 3 (repeat)", 1);

	int pass = r1 && r2 && r3;
	printf("WAKE_GATE %s\n", pass ? "PASS 3/3" : "FAIL");
	fflush(stdout);
	return pass ? 0 : 1;
}
