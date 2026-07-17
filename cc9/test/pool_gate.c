/* pool_gate — replicate Ladybird's Threading::ThreadPool on cc9 pthreads.
 *
 * RequestServer's DNS hangs with jobs submitted to a 4-worker pool never
 * completing. The pool is: workers block in pthread_cond_wait on a queue;
 * submit() enqueues + pthread_cond_signal. Workers are created with an
 * 8 MiB stack via pthread_attr_setstacksize — which cc9 only recently
 * started honoring (be284d7). This gate replays exactly that:
 *   1. create 4 workers with 8 MiB attr stacks (does creation succeed? do
 *      the threads actually run?)
 *   2. submit 8 jobs; each worker cond_waits, dequeues, runs
 *   3. main waits (condvar, reverse direction) for all 8 completions
 * PASS = all jobs complete. A hang or missing worker = the RequestServer bug.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define WORKERS 4
#define JOBS 8

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;
static int queue[JOBS], q_head, q_tail;
static int done_count;
static int started_workers;

static void *worker(void *arg)
{
	long id = (long)arg;
	pthread_mutex_lock(&mtx);
	started_workers++;
	printf("worker %ld started (stack var at %p)\n", id, (void *)&id);
	fflush(stdout);
	pthread_mutex_unlock(&mtx);

	for (;;) {
		pthread_mutex_lock(&mtx);
		while (q_head == q_tail)
			pthread_cond_wait(&work_cv, &mtx);
		int job = queue[q_head++ % JOBS];
		pthread_mutex_unlock(&mtx);

		/* the "work": a getaddrinfo-sized delay */
		usleep(50000);

		pthread_mutex_lock(&mtx);
		done_count++;
		printf("worker %ld finished job %d (done=%d)\n", id, job, done_count);
		fflush(stdout);
		pthread_cond_signal(&done_cv);
		pthread_mutex_unlock(&mtx);
	}
	return 0;
}

int main(void)
{
	pthread_t tids[WORKERS];
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	int rc = pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
	printf("setstacksize(8MiB) rc=%d\n", rc);

	for (long i = 0; i < WORKERS; i++) {
		rc = pthread_create(&tids[i], &attr, worker, (void *)i);
		printf("create worker %ld rc=%d\n", i, rc);
		fflush(stdout);
		if (rc)
			return 1;
	}

	/* give workers a beat to start, then verify they exist */
	usleep(300000);
	pthread_mutex_lock(&mtx);
	printf("started_workers=%d (want %d)\n", started_workers, WORKERS);
	fflush(stdout);
	pthread_mutex_unlock(&mtx);

	/* submit like ThreadPool::submit: lock, enqueue, signal (one at a time) */
	for (int j = 0; j < JOBS; j++) {
		pthread_mutex_lock(&mtx);
		queue[q_tail++ % JOBS] = j;
		pthread_cond_signal(&work_cv);
		pthread_mutex_unlock(&mtx);
	}
	printf("submitted %d jobs; waiting for completion...\n", JOBS);
	fflush(stdout);

	/* wait with a watchdog: if 10s passes without all done, report the hang */
	pthread_mutex_lock(&mtx);
	int spins = 0;
	while (done_count < JOBS) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 1;
		pthread_cond_timedwait(&done_cv, &mtx, &ts);
		if (++spins > 10) {
			printf("POOL_GATE FAIL: only %d/%d jobs done after 10s — the RequestServer hang, reproduced\n", done_count, JOBS);
			fflush(stdout);
			return 1;
		}
	}
	pthread_mutex_unlock(&mtx);
	printf("POOL_GATE PASS: %d/%d jobs done\n", done_count, JOBS);
	fflush(stdout);
	return 0;
}
