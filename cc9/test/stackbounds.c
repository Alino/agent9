/* cc9_stack_bounds / pthread_get_stackaddr_np — does the reported stack range
 * actually contain the caller's stack? A conservative GC scans [sp, hi), so a
 * wrong answer here is silent heap corruption, not a crash. Checks the main
 * thread and a spawned thread, and that the two ranges are disjoint.
 *
 *   cc9 run cc9/test/stackbounds.c   ->  expects "stackbounds: ALL OK"
 */
#include <stdio.h>
#include <pthread.h>

static int fails;

static void check(const char *who) {
	void *lo = 0, *hi = 0;
	char probe;
	unsigned long sp = (unsigned long)&probe;

	if (cc9_stack_bounds(&lo, &hi) != 0) {
		printf("%s: cc9_stack_bounds failed\n", who); fails++; return;
	}
	if (!(sp >= (unsigned long)lo && sp < (unsigned long)hi)) {
		printf("%s: FAIL sp=%p not in [%p,%p)\n", who, (void *)sp, lo, hi);
		fails++; return;
	}
	if (pthread_get_stackaddr_np(pthread_self()) != hi) {
		printf("%s: FAIL stackaddr_np != hi\n", who); fails++; return;
	}
	printf("%s: ok sp=%p in [%p,%p) size=%luMB\n", who, (void *)sp, lo, hi,
	       ((unsigned long)hi - (unsigned long)lo) >> 20);
}

static void *lo_t, *hi_t;

static void *thread_main(void *a) {
	(void)a;
	check("thread");
	cc9_stack_bounds(&lo_t, &hi_t);
	return 0;
}

int main(void) {
	void *lo_m, *hi_m;
	pthread_t t;

	check("main");
	cc9_stack_bounds(&lo_m, &hi_m);

	if (pthread_create(&t, 0, thread_main, 0) != 0) {
		printf("pthread_create failed\n"); return 1;
	}
	pthread_join(t, 0);

	/* The two threads must not claim the same memory, or the GC would scan a
	 * live thread's frames as if they were another's. */
	if (lo_t < hi_m && lo_m < hi_t) {
		printf("FAIL main [%p,%p) overlaps thread [%p,%p)\n", lo_m, hi_m, lo_t, hi_t);
		fails++;
	} else {
		printf("disjoint: ok\n");
	}

	printf(fails ? "stackbounds: %d FAILURES\n" : "stackbounds: ALL OK\n", fails);
	return fails != 0;
}
