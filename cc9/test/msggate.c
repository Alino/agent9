/* msggate.c — sendmsg/recvmsg over /net (net9.c).
 *
 * These were ENOSYS stubs until now, which meant socket2's vectored paths
 * compiled and then failed at runtime. They are real now: the msghdr iovec array
 * maps onto readv/writev, and msg_name onto sendto/recvfrom. Ancillary data
 * (SCM_RIGHTS) is refused with EOPNOTSUPP — Plan 9 passes channels via /srv, not
 * inside a connection — and that refusal is part of the contract, so it is
 * tested too.
 *
 * Build/run on 9front (cc9):  msggate [ip]
 * With no argument it takes this machine's own IPv4 from /net/ipselftab, so
 * `cc9 run test/msggate.c` just works on any box; pass an address to override.
 * (127.0.0.1 is NOT a usable default — it does not route on every 9front.)
 * Expects "msggate N/N PASS".
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int pass, total;

static void ok(const char *what, int cond, const char *detail) {
	total++;
	if (cond) { pass++; printf("%d %s: PASS %s\n", total, what, detail ? detail : ""); }
	else      { printf("%d %s: FAIL %s\n", total, what, detail ? detail : ""); }
}

static const char *g_ip;
static int g_port;

/* Echo server: accept one connection, bounce one message back. */
static void *srv(void *arg) {
	int ln = *(int *)arg;
	int c = accept(ln, 0, 0);
	if (c < 0) return 0;
	char buf[256];
	long n = read(c, buf, sizeof buf);
	if (n > 0) write(c, buf, n);
	close(c);
	return 0;
}

static int listen_somewhere(int *port_out) {
	int ln = socket(AF_INET, SOCK_STREAM, 0);
	if (ln < 0) return -1;
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(g_port);
	a.sin_addr.s_addr = inet_addr(g_ip);
	if (bind(ln, (struct sockaddr *)&a, sizeof a) < 0) { close(ln); return -1; }
	if (listen(ln, 4) < 0) { close(ln); return -1; }
	*port_out = g_port;
	return ln;
}

/* 1+2: sendmsg with a 3-part iovec must arrive coalesced; recvmsg must scatter
 *      it back into a 2-part iovec. This is exactly what socket2 does. */
static void test_stream_vectored(void) {
	int port, ln = listen_somewhere(&port);
	if (ln < 0) { ok("stream setup", 0, "listen failed"); return; }
	pthread_t t;
	pthread_create(&t, 0, srv, &ln);

	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = inet_addr(g_ip);
	if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
		ok("stream connect", 0, strerror(errno));
		close(s); close(ln); pthread_join(t, 0); return;
	}

	struct iovec out[3];
	out[0].iov_base = (void *)"hello"; out[0].iov_len = 5;
	out[1].iov_base = (void *)" ";     out[1].iov_len = 1;
	out[2].iov_base = (void *)"plan9"; out[2].iov_len = 5;
	struct msghdr mh;
	memset(&mh, 0, sizeof mh);
	mh.msg_iov = out; mh.msg_iovlen = 3;
	long w = sendmsg(s, &mh, 0);
	ok("sendmsg vectored", w == 11, w == 11 ? "11 bytes" : strerror(errno));

	char p1[6], p2[16];
	memset(p1, 0, sizeof p1); memset(p2, 0, sizeof p2);
	struct iovec in[2];
	in[0].iov_base = p1; in[0].iov_len = 5;
	in[1].iov_base = p2; in[1].iov_len = 6;
	struct msghdr rh;
	memset(&rh, 0, sizeof rh);
	rh.msg_iov = in; rh.msg_iovlen = 2;
	long r = recvmsg(s, &rh, 0);
	int good = r == 11 && memcmp(p1, "hello", 5) == 0 && memcmp(p2, " plan9", 6) == 0;
	char d[64];
	snprintf(d, sizeof d, "r=%ld p1=%.5s p2=%.6s", r, p1, p2);
	ok("recvmsg scattered", good, d);

	close(s); close(ln); pthread_join(t, 0);
}

/* 3: ancillary data must be REFUSED, not silently dropped. A dropped SCM_RIGHTS
 *    looks like a lost fd at the far end — far worse than an error. */
static void test_cmsg_refused(void) {
	int s = socket(AF_INET, SOCK_STREAM, 0);
	char ctl[32];
	struct iovec iov;
	iov.iov_base = (void *)"x"; iov.iov_len = 1;
	struct msghdr mh;
	memset(&mh, 0, sizeof mh);
	mh.msg_iov = &iov; mh.msg_iovlen = 1;
	mh.msg_control = ctl; mh.msg_controllen = sizeof ctl;
	errno = 0;
	long r = sendmsg(s, &mh, 0);
	ok("cmsg refused (EOPNOTSUPP)", r < 0 && errno == EOPNOTSUPP,
	   r < 0 ? strerror(errno) : "returned success!");
	close(s);
}

/* 4: recvmsg must zero msg_controllen so callers do not parse stale ancillary. */
static void test_controllen_cleared(void) {
	int port, ln = listen_somewhere(&port);
	if (ln < 0) { ok("controllen setup", 0, "listen failed"); return; }
	pthread_t t;
	pthread_create(&t, 0, srv, &ln);

	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = inet_addr(g_ip);
	if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
		ok("controllen connect", 0, strerror(errno));
		close(s); close(ln); pthread_join(t, 0); return;
	}
	write(s, "z", 1);

	char buf[8], ctl[32];
	struct iovec iov;
	iov.iov_base = buf; iov.iov_len = sizeof buf;
	struct msghdr rh;
	memset(&rh, 0, sizeof rh);
	rh.msg_iov = &iov; rh.msg_iovlen = 1;
	rh.msg_control = ctl; rh.msg_controllen = sizeof ctl;   /* stale on entry */
	long r = recvmsg(s, &rh, 0);
	ok("recvmsg clears controllen", r == 1 && rh.msg_controllen == 0, 0);

	close(s); close(ln); pthread_join(t, 0);
}

/* This machine's own IPv4, from /net/ipselftab — the lines look like
 *   10.0.2.15    01   4u
 * and we want a "4u" (IPv4, unicast) entry that is not loopback. Hardcoding an
 * address instead ties the gate to one particular box. */
static char self_ip[64];

static const char *
find_self_ip(void)
{
	char buf[4096];
	int fd = open("/net/ipselftab", 0);
	if (fd < 0) return 0;
	long n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0) return 0;
	buf[n] = 0;

	for (char *p = buf; *p; ) {
		char *eol = strchr(p, '\n');
		if (eol) *eol = 0;
		/* address is the first field; flags the third */
		char ip[64];
		int i = 0;
		while (p[i] && p[i] != ' ' && p[i] != '\t' && i < (int)sizeof ip - 1) { ip[i] = p[i]; i++; }
		ip[i] = 0;
		if (strstr(p, "4u") && strcmp(ip, "127.0.0.1") != 0 && strchr(ip, '.')) {
			strcpy(self_ip, ip);
			return self_ip;
		}
		if (!eol) break;
		p = eol + 1;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc > 1) {
		g_ip = argv[1];
	} else {
		g_ip = find_self_ip();
		if (!g_ip) { printf("msggate: no IPv4 in /net/ipselftab; pass an address\n"); return 1; }
	}
	printf("msggate: using %s\n", g_ip);
	g_port = 9210;

	test_stream_vectored();
	g_port++;
	test_cmsg_refused();
	g_port++;
	test_controllen_cleared();

	printf("msggate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
