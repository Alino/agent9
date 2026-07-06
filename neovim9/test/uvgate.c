/* uvgate — G2 gate: real libuv running on the cc9 poll layer on 9front.
 * timer fires; pipe echo through the loop; uv_spawn(rc) captures stdout and
 * exit code; uv_async wakes the loop from another thread. */
#include <stdio.h>
#include <string.h>
#include <uv.h>

static int npass;
static void check(const char *name, int cond){
	printf(cond ? "PASS %s\n" : "FAIL %s\n", name);
	if(cond) npass++;
}

/* 1: timer */
static int timer_fired;
static void on_timer(uv_timer_t *t){ timer_fired = 1; uv_stop(t->loop); }

/* 2: pipe echo — write into one end, read callback on the other */
static char rbuf[64];
static int rlen;
static void alloc_cb(uv_handle_t *h, size_t sz, uv_buf_t *b){ (void)h; (void)sz; b->base = rbuf + rlen; b->len = sizeof rbuf - rlen; }
static void on_read(uv_stream_t *s, ssize_t n, const uv_buf_t *b){
	(void)b;
	if(n > 0) rlen += (int)n;
	if(rlen >= 5){ uv_read_stop(s); uv_stop(s->loop); }
}

/* 3: spawn */
static char sbuf[256];
static int slen;
static int64_t exit_status = -1;
static int exited;
static void salloc(uv_handle_t *h, size_t sz, uv_buf_t *b){ (void)h; (void)sz; b->base = sbuf + slen; b->len = sizeof sbuf - slen; }
static void sread(uv_stream_t *s, ssize_t n, const uv_buf_t *b){
	(void)b;
	if(n > 0) slen += (int)n;
	else uv_read_stop(s);
}
static void on_exit_cb(uv_process_t *p, int64_t status, int sig){
	(void)sig;
	exit_status = status; exited = 1;
	uv_close((uv_handle_t *)p, 0);
}

/* 4: async from a thread */
static int async_hit;
static void on_async(uv_async_t *a){ async_hit = 1; uv_stop(a->loop); }
static void thr_main(void *arg){ uv_sleep(100); uv_async_send((uv_async_t *)arg); }

int main(void){
	uv_loop_t *loop = uv_default_loop();
	check("loop-init", loop != 0);

	uv_timer_t tm;
	uv_timer_init(loop, &tm);
	uv_timer_start(&tm, on_timer, 80, 0);
	uint64_t t0 = uv_now(loop);
	uv_run(loop, UV_RUN_DEFAULT);
	check("timer", timer_fired && uv_now(loop) - t0 >= 60);
	uv_close((uv_handle_t *)&tm, 0);

	uv_pipe_t pr;
	uv_pipe_init(loop, &pr, 0);
	uv_file fds[2];
	check("uv_pipe", uv_pipe(fds, UV_NONBLOCK_PIPE, 0) == 0);
	uv_pipe_open(&pr, fds[0]);
	uv_read_start((uv_stream_t *)&pr, alloc_cb, on_read);
	uv_buf_t wb = uv_buf_init("hello", 5);
	uv_fs_t wreq;
	uv_fs_write(0, &wreq, fds[1], &wb, 1, -1, 0);   /* sync write on the write end */
	uv_run(loop, UV_RUN_DEFAULT);
	check("pipe-echo", rlen == 5 && memcmp(rbuf, "hello", 5) == 0);
	uv_close((uv_handle_t *)&pr, 0);

	uv_process_t proc;
	uv_pipe_t out;
	uv_pipe_init(loop, &out, 0);
	uv_stdio_container_t stdio[3];
	stdio[0].flags = UV_IGNORE;
	stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
	stdio[1].data.stream = (uv_stream_t *)&out;
	stdio[2].flags = UV_IGNORE;
	char *args[] = { "rc", "-c", "echo spawned; exit 'cc9exit=5'", 0 };
	uv_process_options_t opts;
	memset(&opts, 0, sizeof opts);
	opts.file = "/bin/rc";
	opts.args = args;
	opts.stdio_count = 3;
	opts.stdio = stdio;
	opts.exit_cb = on_exit_cb;
	int r = uv_spawn(loop, &proc, &opts);
	check("spawn", r == 0);
	if(r == 0){
		uv_read_start((uv_stream_t *)&out, salloc, sread);
		uv_run(loop, UV_RUN_DEFAULT);   /* runs until exit_cb + pipe EOF */
	}
	check("spawn-output", slen >= 7 && memcmp(sbuf, "spawned", 7) == 0);
	check("spawn-exit", exited && exit_status == 5);
	uv_close((uv_handle_t *)&out, 0);

	uv_async_t as;
	uv_async_init(loop, &as, on_async);
	uv_thread_t th;
	uv_thread_create(&th, thr_main, &as);
	uv_run(loop, UV_RUN_DEFAULT);
	uv_thread_join(&th);
	check("async", async_hit == 1);
	uv_close((uv_handle_t *)&as, 0);
	uv_run(loop, UV_RUN_DEFAULT);   /* drain closes */

	printf("uvgate: %d/8 passed on libuv %s\n", npass, uv_version_string());
	if(npass == 8) printf("UVGATE OK\n");
	return npass == 8 ? 0 : 1;
}
