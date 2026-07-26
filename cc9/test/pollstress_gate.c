/* pollstress_gate — hammer the poll layer's cross-thread paths hard enough that
 * a compiler-cached shared field shows up as a HANG or a WRONG BYTE COUNT.
 *
 * The poll layer emulates readiness with a reader thread (and, for non-blocking
 * fds, a writer thread) per fd, sharing plain struct fields with the app thread.
 * At -O0 every access is a fresh load, so the sharing "works". At -O2 the
 * compiler may hoist such a load out of a wait loop and spin forever, or cache a
 * ring index across a call it can see through. This gate exists so enabling -O2
 * on poll.c cannot silently regress into a deadlock that only reproduces under
 * load on the real box.
 *
 * What it stresses:
 *   1. many concurrent fds, each with its own reader thread, all draining at once
 *      (slot churn + per-fd ring state),
 *   2. fds closed and reopened while readers are still blocked in pread (the
 *      lingering-reader / slot-reuse path),
 *   3. a full ring with a slow consumer, forcing the reader to park on p->space
 *      and be woken (the wait loop most at risk of a hoisted load),
 *   4. exact byte accounting end to end — a torn head/tail shows up as a short
 *      or duplicated stream, not just as slowness.
 *
 * Every phase has a WATCHDOG: an alarm that fails the gate instead of hanging
 * the test runner forever, because "hangs" is exactly the failure mode here.
 *
 * Run on 9front:  pollstress_gate   -> "pollstress_gate N/N PASS"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

static int pass, total;
static void ck(int ok, const char *what)
{
	total++;
	printf("%d %s: %s\n", total, what, ok ? "PASS" : "FAIL");
	fflush(stdout);
	if (ok) pass++;
}

/* A hang is the headline failure mode, so never hang: every phase arms this. */
static volatile int g_phase_done;
static void watchdog_fired(int sig)
{
	(void)sig;
	if (!g_phase_done) {
		printf("WATCHDOG: phase did not finish — this is the -O2 hang signature\n");
		printf("pollstress_gate %d/%d FAIL\n", pass, total + 1);
		fflush(stdout);
		_exit(1);
	}
}

static void arm_watchdog(unsigned seconds)
{
	g_phase_done = 0;
	signal(SIGALRM, watchdog_fired);
	alarm(seconds);
}

static void disarm_watchdog(void)
{
	g_phase_done = 1;
	alarm(0);
}

static unsigned char byte_at(unsigned i) { return (unsigned char)((i * 37u + (i >> 9)) & 0xff); }

/* ---- 1+4: N concurrent pipes, exact byte accounting ------------------------ */

#define NPIPE 16
#define PER_PIPE (192u * 1024)      /* > the 64 KiB initial ring, so readers park */

struct feeder {
	int fd;
	unsigned n;
};

static void *feeder_main(void *arg)
{
	struct feeder *f = arg;
	unsigned char buf[4096];
	for (unsigned off = 0; off < f->n;) {
		unsigned chunk = f->n - off < sizeof buf ? f->n - off : (unsigned)sizeof buf;
		for (unsigned i = 0; i < chunk; i++)
			buf[i] = byte_at(off + i);
		unsigned done = 0;
		while (done < chunk) {
			long w = write(f->fd, buf + done, chunk - done);
			if (w <= 0)
				return (void *)1;
			done += (unsigned)w;
		}
		off += chunk;
	}
	close(f->fd);
	return 0;
}

static int concurrent_drain_case(void)
{
	int rd[NPIPE];
	pthread_t th[NPIPE];
	static struct feeder fe[NPIPE];
	unsigned got[NPIPE];
	int bad = 0;

	for (int i = 0; i < NPIPE; i++) {
		int fds[2];
		if (pipe(fds) != 0) {
			printf("   pipe %d failed: %s\n", i, strerror(errno));
			return 0;
		}
		rd[i] = fds[0];
		got[i] = 0;
		fcntl(rd[i], F_SETFL, O_NONBLOCK);   /* route through the poll layer */
		fe[i].fd = fds[1];
		fe[i].n = PER_PIPE;
		if (pthread_create(&th[i], 0, feeder_main, &fe[i]) != 0) {
			printf("   pthread_create %d failed\n", i);
			return 0;
		}
	}

	/* Deliberately slow, interleaved consumer: every reader fills its ring and
	 * must park on p->space, then be woken. That wait loop is the one a hoisted
	 * load turns into an infinite spin. */
	unsigned char buf[2048];
	int live = NPIPE;
	while (live > 0) {
		for (int i = 0; i < NPIPE; i++) {
			if (rd[i] < 0)
				continue;
			long r = read(rd[i], buf, sizeof buf);
			if (r > 0) {
				for (long k = 0; k < r; k++)
					if (buf[k] != byte_at(got[i] + (unsigned)k))
						bad++;
				got[i] += (unsigned)r;
				continue;
			}
			if (r == 0) {
				close(rd[i]);
				rd[i] = -1;
				live--;
				continue;
			}
			if (errno != EAGAIN) {
				close(rd[i]);
				rd[i] = -1;
				live--;
			}
		}
		struct pollfd pfd[NPIPE];
		int np = 0;
		for (int i = 0; i < NPIPE; i++)
			if (rd[i] >= 0) { pfd[np].fd = rd[i]; pfd[np].events = POLLIN; pfd[np].revents = 0; np++; }
		if (np)
			poll(pfd, np, 5000);
	}

	for (int i = 0; i < NPIPE; i++)
		pthread_join(th[i], 0);

	int short_count = 0;
	for (int i = 0; i < NPIPE; i++)
		if (got[i] != PER_PIPE) {
			short_count++;
			printf("   pipe %d: got %u of %u\n", i, got[i], PER_PIPE);
		}
	if (bad)
		printf("   %d corrupt bytes\n", bad);
	return short_count == 0 && bad == 0;
}

/* ---- 2: churn — close fds while readers are parked, then reuse the numbers -- */

static int slot_churn_case(void)
{
	for (int round = 0; round < 40; round++) {
		int fds[2];
		if (pipe(fds) != 0)
			return 0;
		fcntl(fds[0], F_SETFL, O_NONBLOCK);
		/* Touch it so the poll layer claims a slot and spawns a reader... */
		unsigned char c;
		(void)read(fds[0], &c, 1);
		struct pollfd pfd = { fds[0], POLLIN, 0 };
		(void)poll(&pfd, 1, 0);
		/* ...leave a byte pending so the reader is mid-flight, then close both.
		 * The reader can still be blocked in pread; the slot must be reclaimed
		 * and the fd NUMBERS reused by the next round without cross-talk. */
		(void)write(fds[1], "x", 1);
		close(fds[0]);
		close(fds[1]);
	}
	return 1;
}

int main(void)
{
	arm_watchdog(120);
	int drained = concurrent_drain_case();
	disarm_watchdog();
	ck(drained, "16 concurrent fds drain byte-exact with parked readers");

	arm_watchdog(60);
	int churned = slot_churn_case();
	disarm_watchdog();
	ck(churned, "close/reopen churn with in-flight readers does not wedge");

	/* A blocking fd whose writer closes mid-stream: the ring must still hand
	 * back everything buffered after the reader thread has exited (the
	 * cc9_poll_owned-after-EOF path). */
	arm_watchdog(60);
	int fds[2];
	int eof_ok = 0;
	if (pipe(fds) == 0) {
		unsigned char blob[32768];
		for (unsigned i = 0; i < sizeof blob; i++)
			blob[i] = byte_at(i);
		pthread_t t;
		static struct feeder f;
		f.fd = fds[1];
		f.n = sizeof blob;
		if (pthread_create(&t, 0, feeder_main, &f) == 0) {
			struct pollfd pfd = { fds[0], POLLIN, 0 };
			(void)poll(&pfd, 1, 5000);
			sleep(2);                       /* reader hits EOF with data queued */
			unsigned got = 0;
			int bad = 0;
			unsigned char buf[1024];
			for (;;) {
				long r = read(fds[0], buf, sizeof buf);
				if (r <= 0)
					break;
				for (long k = 0; k < r; k++)
					if (buf[k] != byte_at(got + (unsigned)k))
						bad++;
				got += (unsigned)r;
			}
			pthread_join(t, 0);
			close(fds[0]);
			if (got != sizeof blob)
				printf("   after-eof drain: got %u of %u\n", got, (unsigned)sizeof blob);
			eof_ok = got == sizeof blob && bad == 0;
		}
	}
	disarm_watchdog();
	ck(eof_ok, "ring still drains fully after the reader thread hit EOF");

	printf("pollstress_gate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
