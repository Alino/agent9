/* memperf_gate — the mem* family must be word-at-a-time, and CORRECT.
 *
 * These are the hottest functions in any cc9 program (a browser runs them for
 * every Skia blit, bitmap clear and string copy). They were byte-at-a-time
 * loops compiled at -O0, which is a first-order cost on this hardware.
 *
 * Correctness first — a fast memcpy that gets an unaligned tail or an
 * overlapping memmove wrong is worse than a slow one — then a throughput floor
 * that a byte loop cannot meet.
 *
 * Run on 9front:  memperf_gate   -> "memperf_gate N/N PASS"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int pass, total;
static void ck(int ok, const char *what)
{
	total++;
	printf("%d %s: %s\n", total, what, ok ? "PASS" : "FAIL");
	fflush(stdout);
	if (ok) pass++;
}

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#define BUFSZ (4u << 20)          /* 4 MiB: a browser frame is ~2 MiB */
/* A byte loop at -O0 on cirno runs well under 100 MB/s. Word-at-a-time clears
 * this comfortably; the floor is deliberately loose so it gates the ALGORITHM,
 * not the exact clock speed of one box. */
#define FLOOR_MB_PER_S 250

static unsigned char *a, *b;

static int throughput_ok(const char *name, long ms, unsigned long bytes)
{
	if (ms <= 0)
		ms = 1;
	unsigned long mbps = (bytes / 1024 / 1024) * 1000 / (unsigned long)ms;
	printf("   %s: %lu MB/s\n", name, mbps);
	return mbps >= FLOOR_MB_PER_S;
}

int main(void)
{
	a = malloc(BUFSZ);
	b = malloc(BUFSZ);
	if (!a || !b) {
		printf("setup: malloc failed\nmemperf_gate 0/1 FAIL\n");
		return 1;
	}
	for (unsigned i = 0; i < BUFSZ; i++)
		a[i] = (unsigned char)(i * 31 + (i >> 13));

	/* ---- correctness: every length through the word/tail boundary ---- */
	int copy_ok = 1, cmp_ok = 1, set_ok = 1, chr_ok = 1;
	for (unsigned n = 0; n <= 64 && copy_ok; n++) {
		for (unsigned off = 0; off < 9; off++) {   /* unaligned starts too */
			memset(b, 0xAA, 128);
			memcpy(b + off, a + off, n);
			if (n && memcmp(b + off, a + off, n) != 0) { copy_ok = 0; break; }
			if (b[off + n] != 0xAA) { copy_ok = 0; break; }  /* no overrun */
		}
	}
	ck(copy_ok, "memcpy is exact for every length/offset across the word boundary");

	for (unsigned n = 1; n <= 64 && cmp_ok; n++) {
		for (unsigned pos = 0; pos < n; pos++) {
			memcpy(b, a, n + 1);
			b[pos] = (unsigned char)(a[pos] ^ 0xFF);
			int r = memcmp(a, b, n);
			int want = (int)a[pos] - (int)b[pos];
			if ((r < 0) != (want < 0) || (r > 0) != (want > 0)) { cmp_ok = 0; break; }
			if (memcmp(a, a, n) != 0) { cmp_ok = 0; break; }
		}
	}
	ck(cmp_ok, "memcmp finds the first differing byte with the right sign");

	for (unsigned n = 0; n <= 64 && set_ok; n++) {
		memset(b, 0xAA, 128);
		memset(b, 0x5A, n);
		for (unsigned i = 0; i < n; i++)
			if (b[i] != 0x5A) { set_ok = 0; break; }
		if (b[n] != 0xAA) set_ok = 0;
	}
	ck(set_ok, "memset fills exactly n bytes");

	/* memmove overlap, both directions — the case a fast memcpy gets wrong */
	int move_ok = 1;
	for (unsigned shift = 1; shift <= 17 && move_ok; shift++) {
		memcpy(b, a, 1024);
		memmove(b + shift, b, 512);              /* forward overlap */
		if (memcmp(b + shift, a, 512) != 0) move_ok = 0;
		memcpy(b, a, 1024);
		memmove(b, b + shift, 512);              /* backward overlap */
		if (memcmp(b, a + shift, 512) != 0) move_ok = 0;
	}
	ck(move_ok, "memmove is correct for overlap in both directions");

	for (unsigned n = 1; n <= 64 && chr_ok; n++) {
		memset(b, 0x11, n + 1);
		for (unsigned pos = 0; pos < n; pos++) {
			b[pos] = 0x77;
			if (memchr(b, 0x77, n) != b + pos) { chr_ok = 0; break; }
			b[pos] = 0x11;
		}
		if (memchr(b, 0x77, n) != 0) chr_ok = 0;
	}
	ck(chr_ok, "memchr finds the first match and only within n");

	/* ---- throughput floor ---- */
	long t0 = now_ms();
	for (int i = 0; i < 8; i++)
		memcpy(b, a, BUFSZ);
	int fast_copy = throughput_ok("memcpy", now_ms() - t0, 8UL * BUFSZ);

	t0 = now_ms();
	for (int i = 0; i < 8; i++)
		memset(b, i, BUFSZ);
	int fast_set = throughput_ok("memset", now_ms() - t0, 8UL * BUFSZ);

	memcpy(b, a, BUFSZ);
	t0 = now_ms();
	volatile int sink = 0;
	for (int i = 0; i < 8; i++)
		sink += memcmp(a, b, BUFSZ);
	(void)sink;
	int fast_cmp = throughput_ok("memcmp", now_ms() - t0, 8UL * BUFSZ);

	ck(fast_copy, "memcpy clears the throughput floor");
	ck(fast_set, "memset clears the throughput floor");
	ck(fast_cmp, "memcmp clears the throughput floor");

	free(a);
	free(b);
	printf("memperf_gate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
