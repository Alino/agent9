/* pollring_gate — the poll layer's read ring GROWS on demand instead of being
 * allocated at a fixed size per fd.
 *
 * Why it matters: cc9 has no non-blocking I/O, so poll() is emulated with a
 * reader thread per fd filling a ring. A large ring lets that thread absorb a
 * whole multi-MB body while a slow event-loop consumer catches up (without it,
 * a 10 MB script stalls behind Plan 9's fixed 256 KiB pipe buffer). But the ring
 * is PER FD: pinning every fd at 8 MiB reserved ~1 GiB across ladybird's ~64
 * curl connections and blew RLIMIT_AS mid-page. So CC9_POLL_RING is a CEILING —
 * rings start at 64 KiB and double only on fds that actually back up.
 *
 * The gate: push more bytes through a pipe than the initial ring and the kernel
 * pipe buffer can jointly hold, WITHOUT draining, then drain and verify every
 * byte arrives in order. If growth is broken the writer wedges (or bytes are
 * mangled at the wrap, since growing re-linearizes the ring).
 *
 * Run on 9front:  CC9_POLL_RING=8388608 pollring_gate  -> "pollring_gate N/N PASS"
 * Run with no CC9_POLL_RING and it still passes: the ring simply never grows and
 * the reader parks on a full ring until the drain starts, which is the old
 * behavior and equally correct.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

static int pass, total;
static void ck(int ok, const char *what)
{
	total++;
	printf("%d %s: %s\n", total, what, ok ? "PASS" : "FAIL");
	if (ok) pass++;
}

/* 3 MiB: comfortably past the 64 KiB initial ring + Plan 9's 256 KiB pipe
 * buffer, so the ring must grow (or the reader must park) to hold it. */
#define TOTAL (3u << 20)
#define CHUNK 4096u

/* Byte i of the stream is a function of i, so a drop, a duplicate, or a bad
 * wrap during a grow all show up as a mismatch at a known offset. */
static unsigned char byte_at(unsigned i) { return (unsigned char)((i * 31u + (i >> 8)) & 0xff); }

/* Second gate: the ring must still be drainable AFTER the reader thread is gone.
 * The reader exits as soon as the kernel gives it EOF, which routinely happens with
 * the tail of the transfer still in the ring. read() has to keep taking bytes from
 * the ring then — routing it back to the kernel returns 0 and silently drops them
 * (a 220 KiB HTTP body arrived as 4 KiB in node9). Blocking fd on purpose: that is
 * the socket case, and the non-blocking gate above never exercises it. */
#define EOFN 8192u
static void eof_drain_case(void)
{
	int fds[2];
	if (pipe(fds) != 0) { ck(0, "pipe for the eof-drain gate"); return; }
	int rd = fds[0], wr = fds[1];

	pid_t kid = fork();
	if (kid < 0) { ck(0, "fork for the eof-drain gate"); return; }
	if (kid == 0) {
		close(rd);
		unsigned char b[512];
		for (unsigned off = 0; off < EOFN; ) {
			unsigned n = EOFN - off < sizeof b ? EOFN - off : (unsigned)sizeof b;
			for (unsigned i = 0; i < n; i++) b[i] = byte_at(off + i);
			unsigned done = 0;
			while (done < n) {
				long w = write(wr, b + done, n - done);
				if (w <= 0) _exit(1);
				done += (unsigned)w;
			}
			off += n;
		}
		close(wr);                       /* EOF right behind the data */
		_exit(0);
	}
	close(wr);

	struct pollfd pfd = { rd, POLLIN, 0 };
	(void)poll(&pfd, 1, 5000);           /* starts the reader thread, as an event loop would */
	sleep(2);                            /* it reads everything, hits EOF, and exits */

	unsigned got = 0, bad = 0;
	unsigned char buf[1024];
	for (;;) {
		long r = read(rd, buf, sizeof buf);
		if (r <= 0) break;
		for (long i = 0; i < r; i++)
			if (buf[i] != byte_at(got + (unsigned)i)) bad++;
		got += (unsigned)r;
	}
	close(rd);

	if (got != EOFN) printf("   short after eof: got %u of %u bytes\n", got, EOFN);
	ck(got == EOFN && bad == 0, "ring still drains after the reader thread hit EOF");
}

/* Fourth gate: the poll TABLE must not be the thing that runs out first.
 * ensure() reports a full table as EMFILE from fcntl/ioctl — "Too many open
 * files" while the OS is nowhere near its own limit. Ladybird's RequestServer
 * hit that on a youtube watch page (a socket plus a pipe pair per in-flight
 * request) and every later request then failed to create its response pipe.
 * 300 non-blocking pipe ends is well past the old 256-slot table and well under
 * what Plan 9 itself allows. */
#define NPIPE 150
static void table_capacity_case(void)
{
	static int rd[NPIPE], wr[NPIPE];
	int made = 0, setfl_fail = 0;

	for (int i = 0; i < NPIPE; i++) {
		int fds[2];
		if (pipe(fds) != 0)
			break;                       /* a real OS fd limit, not the table */
		rd[made] = fds[0]; wr[made] = fds[1];
		/* Both ends non-blocking: each claims its own poll-table slot. */
		if (fcntl(rd[made], F_SETFL, O_NONBLOCK) != 0) setfl_fail++;
		if (fcntl(wr[made], F_SETFL, O_NONBLOCK) != 0) setfl_fail++;
		made++;
	}

	if (setfl_fail)
		printf("   %d of %d F_SETFL calls failed (errno %d = %s)\n",
		       setfl_fail, made * 2, errno, strerror(errno));
	if (made < NPIPE)
		printf("   only %d of %d pipes created\n", made, NPIPE);

	for (int i = 0; i < made; i++) { close(rd[i]); close(wr[i]); }

	ck(setfl_fail == 0, "poll table holds 300 non-blocking fds without EMFILE");
}


/* Fifth gate: select() must not write past the caller's fd_set.
 *
 * fd_set is FD_SETSIZE(512) bits = 8 words = 64 bytes, but select() used to cap
 * nfds at PFD_MAX*8 -- a count of fds THIS LAYER can track, unrelated to the size
 * of the object the caller handed over. The result-clearing loops run
 * (nfds+63)/64 words, so a caller passing nfds=1024 (libuv and CPython both do)
 * wrote 128 bytes into 64, and raising PFD_MAX to 1024 made the worst case 1 KiB.
 * Deterministic, and identical at -O0 and -O2: a wrong value, not a hang. */
static void select_bound_case(void)
{
	struct { fd_set r; unsigned long canary[8]; } x;
	int fds[2];
	if (pipe(fds) != 0) { ck(0, "pipe for the select-bound gate"); return; }
	FD_ZERO(&x.r);
	for (int i = 0; i < 8; i++)
		x.canary[i] = 0xA5A5A5A5A5A5A5A5UL;
	FD_SET(fds[0], &x.r);
	struct timeval tv = { 0, 1000 };
	(void)select(1024, &x.r, 0, 0, &tv);      /* nfds > FD_SETSIZE, as real callers pass */
	int smashed = 0;
	for (int i = 0; i < 8; i++)
		if (x.canary[i] != 0xA5A5A5A5A5A5A5A5UL)
			smashed++;
	close(fds[0]);
	close(fds[1]);
	if (smashed)
		printf("   %d canary words past the fd_set were overwritten\n", smashed);
	ck(smashed == 0, "select() does not write past the caller's fd_set");
}

/* Sixth gate: the read ring size must stay a POWER OF TWO.
 *
 * head/tail are absolute 32-bit counters indexed as `head % bufsz`, which is only
 * continuous across the 2^32 wrap when bufsz divides 2^32. At any other size the
 * index jumps at the wrap and the ring silently desyncs -- a long-lived streaming
 * fd corrupts its own data after 4 GiB. Growth doubles, so the invariant only
 * needs enforcing on the CC9_POLL_RING env value.
 *
 * Observed through FIONREAD (cc9_poll_pending -> ring_avail), which saturates at
 * exactly bufsz once the reader parks on a full ring -- no duplicated logic.
 * Run with CC9_POLL_RING=100000: unfixed reports 100000 and FAILS. */
static void ring_pow2_case(void)
{
	int fds[2];
	if (pipe(fds) != 0) { ck(0, "pipe for the ring-size gate"); return; }
	int rd = fds[0], wr = fds[1];
	if (fcntl(rd, F_SETFL, O_NONBLOCK) != 0) { ck(0, "F_SETFL for the ring-size gate"); return; }

	pid_t kid = fork();
	if (kid < 0) { ck(0, "fork for the ring-size gate"); return; }
	if (kid == 0) {
		close(rd);
		unsigned char b[4096];
		memset(b, 0x5A, sizeof b);
		for (int i = 0; i < 512; i++)          /* 2 MiB: saturates any ring here */
			if (write(wr, b, sizeof b) <= 0)
				break;
		close(wr);
		_exit(0);
	}
	close(wr);

	unsigned char probe[1];
	(void)read(rd, probe, 0);
	struct pollfd pfd = { rd, POLLIN, 0 };
	(void)poll(&pfd, 1, 0);
	sleep(3);                                   /* reader fills and parks */

	int fill = 0;
	(void)ioctl(rd, FIONREAD, &fill);
	if (fill >= 65536) {
		if ((fill & (fill - 1)) != 0)
			printf("   ring saturated at %d, which is not a power of two\n", fill);
		ck((fill & (fill - 1)) == 0, "read ring size is a power of two");
	} else {
		printf("   ring not saturated (%d); size check skipped\n", fill);
		ck(1, "read ring size is a power of two (not saturated, skipped)");
	}

	/* Drain so the child can finish, then reap. */
	unsigned char buf[4096];
	while (read(rd, buf, sizeof buf) > 0)
		;
	close(rd);
	waitpid(kid, 0, 0);
}

int main(void)
{
	int fds[2];
	if (pipe(fds) != 0) {
		printf("pipe: %s\npollring_gate 0/1 FAIL\n", strerror(errno));
		return 1;
	}
	int rd = fds[0], wr = fds[1];

	/* Non-blocking read end: this is what routes the fd through the poll
	 * layer's reader thread and its ring in the first place. */
	if (fcntl(rd, F_SETFL, O_NONBLOCK) != 0)
		printf("warning: F_SETFL O_NONBLOCK failed: %s\n", strerror(errno));

	pid_t kid = fork();
	if (kid < 0) {
		printf("fork: %s\npollring_gate 0/1 FAIL\n", strerror(errno));
		return 1;
	}
	if (kid == 0) {
		close(rd);
		unsigned char buf[CHUNK];
		for (unsigned off = 0; off < TOTAL; ) {
			unsigned n = TOTAL - off < CHUNK ? TOTAL - off : CHUNK;
			for (unsigned i = 0; i < n; i++)
				buf[i] = byte_at(off + i);
			unsigned done = 0;
			while (done < n) {
				long w = write(wr, buf + done, n - done);
				if (w <= 0) _exit(1);
				done += (unsigned)w;
			}
			off += n;
		}
		close(wr);
		_exit(0);
	}
	close(wr);

	/* Touch the fd once so the poll layer spawns its reader thread NOW. Without
	 * this the ring does not exist yet during the sleep below, only the kernel
	 * pipe buffer fills, and the backlog this gate is about never forms. */
	{
		unsigned char probe[1];
		(void)read(rd, probe, 0);
		struct pollfd pfd = { rd, POLLIN, 0 };
		(void)poll(&pfd, 1, 0);
	}

	/* Now let the writer run ahead while nothing drains, so the ring itself has
	 * to absorb the backlog — the ladybird case: RequestServer streams a body
	 * while WebContent's event loop is busy elsewhere. */
	sleep(3);

	unsigned got = 0, bad = 0, first_bad = 0;
	unsigned char buf[CHUNK];
	for (;;) {
		long r = read(rd, buf, sizeof buf);
		if (r > 0) {
			for (long i = 0; i < r; i++)
				if (buf[i] != byte_at(got + (unsigned)i)) {
					if (!bad) first_bad = got + (unsigned)i;
					bad++;
				}
			got += (unsigned)r;
			continue;
		}
		if (r == 0)
			break;                       /* writer closed and ring drained */
		if (errno == EAGAIN) {
			struct pollfd pfd = { rd, POLLIN, 0 };
			if (poll(&pfd, 1, 10000) <= 0) break;   /* 10s with no data = wedged */
			continue;
		}
		break;
	}
	close(rd);

	if (got != TOTAL)
		printf("   short: got %u of %u bytes\n", got, TOTAL);
	if (bad)
		printf("   corrupt: %u mismatched bytes, first at offset %u\n", bad, first_bad);

	ck(got == TOTAL, "whole stream arrives without draining first");
	ck(bad == 0, "bytes are intact and in order across the grow");

	eof_drain_case();
	table_capacity_case();
	select_bound_case();
	ring_pow2_case();

	printf("pollring_gate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
