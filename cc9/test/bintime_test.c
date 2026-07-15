/* bintime_test — the cached /dev/bintime fd behind clock_gettime/time/
 * gettimeofday.
 *
 * The bug this guards: n9_nsec caches its fd now, so it MUST pread at an
 * explicit offset 0. With the old offset -1 ("use the file offset") a cached
 * fd walks forward and every call after the first reads EOF -> time freezes at
 * 0. A single clock_gettime() call looks perfectly fine either way; only
 * repeated calls catch it.
 *
 *   cc9/host/cc9 run cc9/test/bintime_test.c
 */
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

int
main(void)
{
	struct timespec a, b;
	struct timeval tv;
	long t;
	int i, fails = 0;

	/* 1. a plain read is sane (post-2020 epoch seconds) */
	clock_gettime(CLOCK_REALTIME, &a);
	if (a.tv_sec < 1600000000L) {
		printf("FAIL clock_gettime sec=%ld — not a plausible epoch time\n", a.tv_sec);
		fails++;
	}

	/* 2. THE regression: repeated calls must keep returning live values.
	 *    With a cached fd + offset -1 this reads 0 from the 2nd call on. */
	for (i = 0; i < 1000; i++) {
		clock_gettime(CLOCK_REALTIME, &b);
		if (b.tv_sec < 1600000000L) {
			printf("FAIL clock_gettime went stale on call %d (sec=%ld)\n",
			       i, b.tv_sec);
			fails++;
			break;
		}
	}

	/* 3. and time must actually ADVANCE, never go backwards */
	if (b.tv_sec < a.tv_sec) {
		printf("FAIL time went backwards: %ld -> %ld\n", a.tv_sec, b.tv_sec);
		fails++;
	}

	/* 4. the other two spellings share n9_nsec — check they didn't freeze */
	gettimeofday(&tv, 0);
	if (tv.tv_sec < 1600000000L) {
		printf("FAIL gettimeofday sec=%ld\n", (long)tv.tv_sec);
		fails++;
	}
	t = time(0);
	if (t < 1600000000L) {
		printf("FAIL time() = %ld\n", t);
		fails++;
	}

	/* 5. it should be FAST now — that's the whole point of the cache. Time it
	 *    against the open+read+close-per-call shape it replaced, so the
	 *    speedup is a measured number and not a claim. */
	{
		struct timespec s, e;
		double cached_ms, uncached_ms;
		const int N = 20000;
		unsigned char buf[8];

		clock_gettime(CLOCK_REALTIME, &s);
		for (i = 0; i < N; i++)
			clock_gettime(CLOCK_REALTIME, &e);
		clock_gettime(CLOCK_REALTIME, &e);
		cached_ms = (e.tv_sec - s.tv_sec) * 1000.0 + (e.tv_nsec - s.tv_nsec) / 1e6;

		/* raw open/read/close — the exact shape n9_nsec used to have. Not
		 * fopen: stdio's buffer alloc would inflate the "before" number and
		 * flatter the cache. */
		clock_gettime(CLOCK_REALTIME, &s);
		for (i = 0; i < N; i++) {
			int fd = open("/dev/bintime", O_RDONLY);
			if (fd >= 0) { if (read(fd, buf, 8) != 8) fails++; close(fd); }
		}
		clock_gettime(CLOCK_REALTIME, &e);
		uncached_ms = (e.tv_sec - s.tv_sec) * 1000.0 + (e.tv_nsec - s.tv_nsec) / 1e6;

		printf("bintime: %d calls  cached=%.0fms (%.2fus)  open-per-call=%.0fms (%.2fus)  %.1fx\n",
		       N, cached_ms, cached_ms * 1000.0 / N,
		       uncached_ms, uncached_ms * 1000.0 / N,
		       cached_ms > 0 ? uncached_ms / cached_ms : 0.0);
	}

	if (fails == 0)
		printf("bintime: all ok\n");
	else
		printf("bintime: %d FAILURES\n", fails);
	return fails != 0;
}
