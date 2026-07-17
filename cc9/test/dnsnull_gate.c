/* dnsnull_gate — the EXACT getaddrinfo calls Ladybird's resolve_host makes:
 * serv=NULL, hints.ai_family = AF_INET then AF_INET6 (two pool workers).
 * cc9 turns serv=NULL into port "0" -> /net/cs query "tcp!host!0".
 * Does that return, error, or hang?
 */
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void try_family(const char *label, int family)
{
	struct addrinfo hints, *res = 0;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	printf("[%8.3f] %s: getaddrinfo(example.com, NULL) start\n", now_s(), label);
	fflush(stdout);
	int rc = getaddrinfo("example.com", 0, &hints, &res);
	printf("[%8.3f] %s: rc=%d (%s)\n", now_s(), label, rc, rc ? gai_strerror(rc) : "ok");
	fflush(stdout);
	int n = 0;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next, n++) {
		if (ai->ai_family == AF_INET) {
			struct sockaddr_in *sa = (struct sockaddr_in *)ai->ai_addr;
			printf("  answer %d: family=AF_INET %s\n", n, inet_ntoa(sa->sin_addr));
		} else {
			printf("  answer %d: family=%d (not AF_INET)\n", n, ai->ai_family);
		}
	}
	fflush(stdout);
	if (res)
		freeaddrinfo(res);
}

int main(void)
{
	try_family("AF_INET ", AF_INET);
	try_family("AF_INET6", AF_INET6);
	try_family("AF_UNSPEC", AF_UNSPEC);
	printf("DNSNULL_GATE DONE\n");
	fflush(stdout);
	return 0;
}
