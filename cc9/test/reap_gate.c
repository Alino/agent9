/* reap_gate — cc9_shm_reap_dead() reclaims a crashed process's pool.
 *
 * The shm pool (#g/shmp.<pid>.<seq>) is reclaimed by cc9_shm_sweep on graceful
 * teardown, but a KILLED/crashed process leaks its 256 MiB segment until the
 * ~100-entry #g cap wedges the box (the exact fragility seen running ladybird
 * headless in a loop). cc9_shm_reap_dead is the startup crash-cleanup.
 *
 *   1. child creates a pool buffer, prints its #g name, then _exits WITHOUT
 *      detaching (simulating a kill/crash) — the segment leaks.
 *   2. parent confirms #g/<name> exists (leaked), calls cc9_shm_reap_dead,
 *      confirms it's gone, and confirms a LIVE process's pool is NOT reaped.
 *
 * Run on 9front:  reap_gate   -> "reap_gate N/N PASS"
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/shm9.h>

extern long n9_open(const char *, int);
extern long n9_close(int);
extern int cc9_shm_reap_dead(const char *);

static int pass, total;
static void ck(int ok, const char *what) {
	total++;
	printf("%d %s: %s\n", total, what, ok ? "PASS" : "FAIL");
	if (ok) pass++;
}

static int g_exists(const char *name) {
	char path[64];
	snprintf(path, sizeof path, "#g/%s/ctl", name);
	long fd = n9_open(path, O_RDONLY);
	if (fd < 0) return 0;
	n9_close((int)fd);
	return 1;
}

int main(void)
{
	/* A pipe to carry the child's leaked pool name back to the parent. */
	int pfd[2];
	if (pipe(pfd) != 0) { printf("pipe failed\n"); return 1; }

	pid_t kid = fork();
	if (kid == 0) {
		close(pfd[0]);
		int fd = cc9_shm_create(4096);
		char name[CC9_SHM_NAMELEN] = {0};
		unsigned long off = 0, len = 0;
		cc9_shm_export(fd, name, &off, &len);   /* name = "shmp.<pid>.<seq>" */
		/* truncate at the pool dir: export gives the buffer's segment name,
		 * which IS the pool name for a Phase B pool. */
		write(pfd[1], name, strlen(name) + 1);
		close(pfd[1]);
		_exit(0);   /* CRASH SIMULATION: no detach, no sweep — leak the segment */
	}
	close(pfd[1]);
	char name[CC9_SHM_NAMELEN] = {0};
	read(pfd[0], name, sizeof name - 1);
	close(pfd[0]);
	int st;
	waitpid(kid, &st, 0);   /* reap the zombie so /proc/<kid> disappears */

	printf("child leaked pool: %s\n", name);
	ck(name[0] != 0, "child reported a pool name");
	ck(g_exists(name), "leaked pool exists after child died");

	int n = cc9_shm_reap_dead("shmp.");
	printf("reaped %d dead pool(s)\n", n);
	ck(n >= 1, "reap removed at least one dead pool");
	ck(!g_exists(name), "leaked pool is gone after reap");

	/* Live-owner safety: our own pool must survive a reap. */
	int myfd = cc9_shm_create(4096);
	char myname[CC9_SHM_NAMELEN] = {0};
	unsigned long o2 = 0, l2 = 0;
	cc9_shm_export(myfd, myname, &o2, &l2);
	cc9_shm_reap_dead("shmp.");
	ck(g_exists(myname), "live process's own pool survives reap");

	printf("reap_gate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
