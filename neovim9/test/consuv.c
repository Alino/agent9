/* consuv — does libuv's stream layer stay alive on a Plan 9 console?
 *
 * nvim's UI client puts stdin through uv_pipe_open()+uv_read_start() (nvim
 * treats UV_TTY as a pipe on non-Windows, event/stream.c:71). On a console it
 * emits its init sequence, tears the TUI down and exits — cleanly, with no
 * error logged, which is what uv_run() returning early looks like.
 *
 * This is that path and nothing else: open fd 0 as a uv pipe, start reading,
 * run the loop with a 3s timer. Expected on a healthy stream with nobody
 * typing: the loop stays in uv_run until the timer fires, no read callback.
 * If uv_run returns immediately, the stream never became an active handle —
 * and that is exactly why nvim quits.
 *
 *   consuv </dev/cons >/tmp/consuv.log
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <uv.h>

static uv_loop_t loop;
static uv_pipe_t in;
static uv_timer_t timer;
static int read_cbs, eof_seen, err_seen;
static uint64_t t_start;

static void alloc_cb(uv_handle_t *h, size_t sz, uv_buf_t *b)
{
	static char buf[256];
	(void)h; (void)sz;
	b->base = buf; b->len = sizeof buf;
}

static void on_read(uv_stream_t *s, ssize_t n, const uv_buf_t *b)
{
	(void)s; (void)b;
	read_cbs++;
	if (n == UV_EOF) { eof_seen = 1; printf("read_cb: UV_EOF after %llums\n",
	                                        (unsigned long long)(uv_now(&loop) - t_start)); }
	else if (n < 0)  { err_seen = 1;  printf("read_cb: error %s after %llums\n", uv_strerror((int)n),
	                                        (unsigned long long)(uv_now(&loop) - t_start)); }
	else printf("read_cb: %ld bytes\n", (long)n);
}

static void on_timer(uv_timer_t *t) { printf("timer fired at 3s\n"); uv_stop(t->loop); }

int
main(void)
{
	int r;
	uv_loop_init(&loop);
	t_start = uv_now(&loop);

	printf("uv_guess_handle(0) = %d (UV_TTY=%d UV_NAMED_PIPE=%d UV_FILE=%d)\n",
	       (int)uv_guess_handle(0), (int)UV_TTY, (int)UV_NAMED_PIPE, (int)UV_FILE);

	r = uv_pipe_init(&loop, &in, 0);
	printf("uv_pipe_init  = %d %s\n", r, r ? uv_strerror(r) : "");
	r = uv_pipe_open(&in, 0);
	printf("uv_pipe_open  = %d %s\n", r, r ? uv_strerror(r) : "");
	r = uv_read_start((uv_stream_t *)&in, alloc_cb, on_read);
	printf("uv_read_start = %d %s\n", r, r ? uv_strerror(r) : "");
	printf("is_active     = %d  has_ref = %d\n",
	       uv_is_active((uv_handle_t *)&in), uv_has_ref((uv_handle_t *)&in));

	uv_timer_init(&loop, &timer);
	uv_timer_start(&timer, on_timer, 3000, 0);

	uint64_t before = uv_now(&loop);
	uv_run(&loop, UV_RUN_DEFAULT);
	uint64_t elapsed = uv_now(&loop) - before;

	printf("uv_run returned after %llums (read_cbs=%d eof=%d err=%d)\n",
	       (unsigned long long)elapsed, read_cbs, eof_seen, err_seen);
	printf("%s\n", elapsed >= 2500 && !eof_seen && !err_seen
	       ? "consuv PASS  loop stayed alive"
	       : "consuv FAIL  stream died early — this is nvim's blank screen");
	return 0;
}
