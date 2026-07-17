/* dnsfetch_gate — isolate the ladybird9 remote-fetch hang.
 *
 * RequestServer hangs on any remote-host fetch (http AND https) with resolver
 * threads in Pread, while file:// and literal-IP LAN fetches work. This gate
 * replays the suspect layers one at a time, printing progress line by line:
 *   1. getaddrinfo("example.com","80")            — the /net/cs round-trip
 *   2. getaddrinfo A/AAAA style repeats            — LibDNS asks per-family
 *   3. blocking connect + HTTP/1.1 keep-alive GET  — read until headers+body
 *   4. same but Connection: close                  — read to EOF
 * Whichever line never prints is the hanging layer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void say(const char *msg) {
	printf("[%8.3f] %s\n", now_s(), msg);
	fflush(stdout);
}

static int fetch(const char *host, struct sockaddr *sa, socklen_t salen, const char *conn_hdr, int max_reads) {
	char msg[256];
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) { say("socket FAILED"); return -1; }
	say("socket ok; connecting...");
	if (connect(fd, sa, salen) != 0) { say("connect FAILED"); close(fd); return -1; }
	say("connect ok; sending GET...");
	char req[512];
	snprintf(req, sizeof req,
	         "GET / HTTP/1.1\r\nHost: %s\r\nUser-Agent: dnsfetch-gate\r\nAccept: */*\r\nConnection: %s\r\n\r\n",
	         host, conn_hdr);
	if (write(fd, req, strlen(req)) != (long)strlen(req)) { say("write FAILED"); close(fd); return -1; }
	say("request sent; reading...");
	char buf[4096];
	long total = 0;
	int reads = 0;
	long n;
	while ((n = read(fd, buf, sizeof buf)) > 0) {
		total += n;
		reads++;
		snprintf(msg, sizeof msg, "  read #%d: %ld bytes (total %ld)", reads, n, total);
		say(msg);
		if (reads == 1) {
			buf[n < 64 ? n : 64] = 0;
			char *nl = strchr(buf, '\r');
			if (nl) *nl = 0;
			snprintf(msg, sizeof msg, "  status line: %s", buf);
			say(msg);
		}
		if (max_reads && reads >= max_reads) { say("  (stopping early: keep-alive would block forever, as expected)"); break; }
	}
	if (n == 0) say("  EOF");
	if (n < 0) say("  read ERROR");
	snprintf(msg, sizeof msg, "fetch done: %ld bytes in %d reads", total, reads);
	say(msg);
	close(fd);
	return total > 0 ? 0 : -1;
}

int main(void) {
	char msg[256];
	const char *host = "example.com";

	say("STEP 1: getaddrinfo(example.com, 80)");
	struct addrinfo hints, *res = 0;
	memset(&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_STREAM;
	int rc = getaddrinfo(host, "80", &hints, &res);
	snprintf(msg, sizeof msg, "getaddrinfo rc=%d", rc);
	say(msg);
	if (rc != 0 || !res) { say("RESOLVE FAILED — this is the hang/fail layer"); return 1; }
	int n = 0;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next, n++) {
		struct sockaddr_in *sa = (struct sockaddr_in *)ai->ai_addr;
		snprintf(msg, sizeof msg, "  answer %d: %s", n, inet_ntoa(sa->sin_addr));
		say(msg);
	}

	say("STEP 2: repeat resolves (LibDNS does A+AAAA per name, repeatedly)");
	for (int i = 0; i < 3; i++) {
		struct addrinfo *r2 = 0;
		rc = getaddrinfo(host, "80", &hints, &r2);
		snprintf(msg, sizeof msg, "  repeat %d: rc=%d", i, rc);
		say(msg);
		if (r2) freeaddrinfo(r2);
	}

	say("STEP 3: HTTP/1.1 keep-alive GET (RequestServer's mode)");
	fetch(host, res->ai_addr, res->ai_addrlen, "keep-alive", 8);

	say("STEP 4: HTTP/1.1 Connection: close GET (read to EOF)");
	fetch(host, res->ai_addr, res->ai_addrlen, "close", 0);

	freeaddrinfo(res);
	say("ALL STEPS DONE");
	return 0;
}
