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

	printf("pollring_gate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
