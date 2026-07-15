/* malloc_usable_size — must be >= the requested size and must never overlap the
 * next block. SpiderMonkey feeds this into GC pressure accounting, so a wrong
 * answer is silent mistiming, not a crash. Two shapes to get right: a plain
 * malloc block, and an aligned_alloc block (which hides the real base at ap[-2]
 * behind a magic at ap[-1]).
 *
 *   cc9 run cc9/test/usablesize.c   ->  expects "usablesize: ALL OK"
 */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

static int fails;

static void chk_plain(size_t n) {
	char *p = malloc(n);
	size_t u;
	if (!p) { printf("malloc(%lu) failed\n", (unsigned long)n); fails++; return; }
	u = malloc_usable_size(p);
	if (u < n) {
		printf("plain %lu: FAIL usable %lu < requested\n",
		       (unsigned long)n, (unsigned long)u);
		fails++;
	} else {
		/* Writing the full usable span must not corrupt the heap; a later
		 * malloc/free round-trip would trip over it if it did. */
		memset(p, 0xa5, u);
	}
	free(p);
}

static void chk_aligned(size_t al, size_t n) {
	char *p = aligned_alloc(al, n);
	size_t u;
	if (!p) { printf("aligned_alloc(%lu,%lu) failed\n",
	                 (unsigned long)al, (unsigned long)n); fails++; return; }
	if ((unsigned long)p & (al - 1)) {
		printf("aligned %lu: FAIL not aligned\n", (unsigned long)al);
		fails++; free(p); return;
	}
	u = malloc_usable_size(p);
	if (u < n) {
		printf("aligned %lu/%lu: FAIL usable %lu < requested\n",
		       (unsigned long)al, (unsigned long)n, (unsigned long)u);
		fails++;
	} else {
		memset(p, 0x5a, u);
	}
	free(p);
}

int main(void) {
	size_t sizes[] = {1, 7, 8, 15, 16, 17, 64, 1000, 4096, 100000};
	size_t aligns[] = {32, 64, 4096};
	int i, j;

	if (malloc_usable_size(0) != 0) { printf("NULL: FAIL\n"); fails++; }

	for (i = 0; i < (int)(sizeof sizes / sizeof sizes[0]); i++)
		chk_plain(sizes[i]);
	for (i = 0; i < (int)(sizeof aligns / sizeof aligns[0]); i++)
		for (j = 0; j < (int)(sizeof sizes / sizeof sizes[0]); j++)
			chk_aligned(aligns[i], sizes[j]);

	/* The heap must still work after all that writing. */
	{
		void *a = malloc(1234), *b = malloc(5678);
		if (!a || !b) { printf("post-check malloc failed\n"); fails++; }
		free(a); free(b);
	}

	printf(fails ? "usablesize: %d FAILURES\n" : "usablesize: ALL OK\n", fails);
	return fails != 0;
}
