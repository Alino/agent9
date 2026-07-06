/* pollgate — exercises the cc9 readiness layer before libuv sits on it:
 * poll timeout accuracy, pipe readiness via reader thread, nonblocking
 * EAGAIN reads, fork+execv+waitpid, WNOHANG, SIGCHLD from the reaper. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static int npass;
static void check(const char *name, int cond){
	printf(cond ? "PASS %s\n" : "FAIL %s\n", name);
	if(cond) npass++;
}
static long now_ms(void){
	struct timespec ts; clock_gettime(0, &ts);
	return ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

static int wfd;
static void *late_writer(void *a){
	(void)a;
	struct timespec t = {0, 150*1000*1000};
	nanosleep(&t, 0);
	write(wfd, "ping", 4);
	return 0;
}

static volatile int got_chld;
static void on_chld(int s){ (void)s; got_chld = 1; }

int main(void){
	/* 1: poll pure timeout */
	long t0 = now_ms();
	int rv = poll(0, 0, 120);
	long dt = now_ms() - t0;
	check("timeout", rv == 0 && dt >= 100 && dt < 600);

	/* 2: pipe becomes readable while poll blocks */
	int p[2];
	check("pipe2", pipe2(p, O_CLOEXEC) == 0);
	wfd = p[1];
	pthread_t th; pthread_create(&th, 0, late_writer, 0);
	struct pollfd pf = { p[0], POLLIN, 0 };
	t0 = now_ms();
	rv = poll(&pf, 1, 5000);
	dt = now_ms() - t0;
	check("poll-in", rv == 1 && (pf.revents & POLLIN) && dt < 3000);
	char buf[16];
	long n = read(p[0], buf, sizeof buf);
	check("read-buffered", n == 4 && memcmp(buf, "ping", 4) == 0);
	pthread_join(th, 0);

	/* 3: nonblocking read on drained pipe -> EAGAIN */
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	errno = 0;
	n = read(p[0], buf, sizeof buf);
	check("eagain", n == -1 && errno == EAGAIN);

	/* 4: EOF after writer closes */
	write(p[1], "z", 1);
	struct timespec t = {0, 120*1000*1000}; nanosleep(&t, 0);
	read(p[0], buf, sizeof buf);
	close(p[1]);
	rv = poll(&pf, 1, 3000);
	check("poll-hup", rv == 1 && (pf.revents & POLLHUP));
	n = read(p[0], buf, sizeof buf);
	check("read-eof", n == 0);
	close(p[0]);

	/* 5: fork + execv + waitpid + SIGCHLD */
	signal(SIGCHLD, on_chld);
	int pid = fork();
	if(pid == 0){
		char *argv[] = { "rc", "-c", "exit 'cc9exit=7'", 0 };
		execv("/bin/rc", argv);
		_exit(99);
	}
	check("fork", pid > 0);
	int st = -1;
	int wp = waitpid(pid, &st, 0);
	check("waitpid", wp == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7);
	check("sigchld", got_chld == 1);

	/* 6: WNOHANG on a slow child, then reap */
	got_chld = 0;
	pid = fork();
	if(pid == 0){
		char *argv[] = { "rc", "-c", "sleep 1", 0 };
		execv("/bin/rc", argv);
		_exit(99);
	}
	rv = waitpid(pid, &st, WNOHANG);
	check("wnohang-running", rv == 0);
	wp = waitpid(pid, &st, 0);
	check("wnohang-reaped", wp == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);

	printf("pollgate: %d/12 passed\n", npass);
	if(npass == 12) printf("POLLGATE OK\n");
	return npass == 12 ? 0 : 1;
}
