/* netgate.c — gate test for the /net-backed BSD socket layer (net9.c).
 *
 * 1. loopback: socket/bind/listen + connect + accept + echo both ways
 * 2. getsockname/getpeername agree with the loopback endpoints
 * 3. UDP: bind + sendto/recvfrom round-trip via headers mode
 * 4. (QEMU) TCP to 10.0.2.2:8799 — HTTP GET against the host http.server
 * 5. getaddrinfo numeric fast path + /net/cs lookup (best-effort: PASS
 *    without cs is fine on a box with no ndb/cs running)
 *
 * Run on 9front:  netgate            -> "netgate N/N PASS"
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static int pass, fail;

static void ck(int ok, const char *what) {
	if (ok) { pass++; printf("ok  %s\n", what); }
	else    { fail++; printf("FAIL %s (errno %d %s)\n", what, errno, strerror(errno)); }
}

static void *server_thread(void *arg) {
	int lfd = *(int *)arg;
	struct sockaddr_in peer;
	socklen_t plen = sizeof peer;
	int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
	if (cfd < 0) return (void *)1;
	char buf[64];
	long n = recv(cfd, buf, sizeof buf, 0);
	if (n > 0) {
		/* echo back upper-cased first byte to prove both directions */
		buf[0] = (char)(buf[0] - 32);
		send(cfd, buf, (unsigned long)n, 0);
	}
	close(cfd);
	return 0;
}

int main(void) {
	/* --- 1+2: TCP loopback --- */
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	ck(lfd >= 0, "socket(listen)");
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(0);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	ck(bind(lfd, (struct sockaddr *)&a, sizeof a) == 0, "bind");
	ck(listen(lfd, 5) == 0, "listen(announce)");
	struct sockaddr_in local;
	socklen_t llen = sizeof local;
	ck(getsockname(lfd, (struct sockaddr *)&local, &llen) == 0, "getsockname");
	int port = ntohs(local.sin_port);
	printf("   listening on port %d\n", port);
	ck(port > 0, "announced port nonzero");

	pthread_t th;
	pthread_create(&th, 0, server_thread, &lfd);

	int cfd = socket(AF_INET, SOCK_STREAM, 0);
	ck(cfd >= 0, "socket(client)");
	struct sockaddr_in dst;
	memset(&dst, 0, sizeof dst);
	dst.sin_family = AF_INET;
	dst.sin_port = htons((unsigned short)port);
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	ck(connect(cfd, (struct sockaddr *)&dst, sizeof dst) == 0, "connect(loopback)");
	ck(send(cfd, "hello", 5, 0) == 5, "send");
	char rbuf[16];
	long rn = recv(cfd, rbuf, sizeof rbuf, 0);
	ck(rn == 5 && rbuf[0] == 'H', "recv echo (Hello)");
	struct sockaddr_in peer;
	socklen_t plen = sizeof peer;
	ck(getpeername(cfd, (struct sockaddr *)&peer, &plen) == 0 &&
	   ntohs(peer.sin_port) == port, "getpeername");
	close(cfd);
	pthread_join(th, 0);
	close(lfd);

	/* --- 3: UDP round-trip --- */
	int ufd = socket(AF_INET, SOCK_DGRAM, 0);
	ck(ufd >= 0, "socket(udp)");
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(0);
	ck(bind(ufd, (struct sockaddr *)&a, sizeof a) == 0, "bind(udp+headers)");
	struct sockaddr_in ua;
	socklen_t ualen = sizeof ua;
	ck(getsockname(ufd, (struct sockaddr *)&ua, &ualen) == 0, "getsockname(udp)");
	int uport = ntohs(ua.sin_port);
	printf("   udp port %d\n", uport);
	int u2 = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&dst, 0, sizeof dst);
	dst.sin_family = AF_INET;
	dst.sin_port = htons((unsigned short)uport);
	dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	long sn = sendto(u2, "ping", 4, 0, (struct sockaddr *)&dst, sizeof dst);
	ck(sn == 4, "sendto(udp)");
	struct sockaddr_in from;
	socklen_t flen = sizeof from;
	char ubuf[16];
	long un = recvfrom(ufd, ubuf, sizeof ubuf, 0, (struct sockaddr *)&from, &flen);
	ck(un == 4 && memcmp(ubuf, "ping", 4) == 0, "recvfrom(udp)");
	close(u2);
	close(ufd);

	/* --- 4: HTTP GET to the QEMU host (best-effort outside QEMU) --- */
	int hfd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&dst, 0, sizeof dst);
	dst.sin_family = AF_INET;
	dst.sin_port = htons(8799);
	inet_pton(AF_INET, "10.0.2.2", &dst.sin_addr);
	if (connect(hfd, (struct sockaddr *)&dst, sizeof dst) == 0) {
		const char *req = "HEAD /pycc9 HTTP/1.0\r\n\r\n";
		send(hfd, req, strlen(req), 0);
		char hbuf[256];
		long hn = recv(hfd, hbuf, sizeof hbuf - 1, 0);
		hbuf[hn > 0 ? hn : 0] = 0;
		ck(hn > 0 && strstr(hbuf, "200"), "HTTP HEAD to host (10.0.2.2:8799)");
	} else {
		printf("   (no 10.0.2.2 host server; skipping HTTP check)\n");
	}
	close(hfd);

	/* --- 5: getaddrinfo --- */
	struct addrinfo *ai = 0;
	int rc = getaddrinfo("127.0.0.1", "80", 0, &ai);
	ck(rc == 0 && ai && ai->ai_family == AF_INET &&
	   ntohs(((struct sockaddr_in *)ai->ai_addr)->sin_port) == 80,
	   "getaddrinfo numeric");
	freeaddrinfo(ai);
	rc = getaddrinfo("9front.org", "80", 0, &ai);
	if (rc == 0 && ai) {
		char ip[64];
		inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, ip, sizeof ip);
		printf("   /net/cs: 9front.org -> %s\n", ip);
		ck(1, "getaddrinfo via /net/cs");
		freeaddrinfo(ai);
	} else {
		printf("   (cs lookup failed rc=%d — ok if no ndb/cs or no dns)\n", rc);
	}

	printf("netgate %d/%d %s\n", pass, pass + fail, fail ? "FAIL" : "PASS");
	return fail != 0;
}
