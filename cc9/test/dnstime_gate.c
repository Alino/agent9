/* dnstime_gate — how long does cc9's getaddrinfo actually take, per family?
 *
 * Why this exists: ladybird's RequestServer resolves through LibDNS, but LibDNS
 * only uses its own socket when a DNS server is configured. The default is
 * "system DNS", which leaves it unconfigured, so EVERY lookup falls back to
 * getaddrinfo on a thread pool — and it asks for A and AAAA. On a box with no
 * IPv6 the AAAA half can stall, and youtube.com pulls from a dozen hostnames, so
 * a per-hostname stall shows up directly as page load time.
 *
 * Prints milliseconds for AF_INET, AF_INET6 and AF_UNSPEC per hostname. It is a
 * measurement, not a pass/fail gate — except for the one bound that matters:
 * no single lookup should take longer than DNS_BUDGET_MS.
 *
 * Run on 9front:  dnstime_gate
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>

/* A lookup slower than this is a page-load problem on its own: youtube.com
 * touches ~10 distinct hosts, so 3 s each is half a minute of nothing. */
#define DNS_BUDGET_MS 3000

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long timed_lookup(const char *host, int family, int *out_count)
{
	struct addrinfo hints, *res = 0;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;

	long t0 = now_ms();
	int rc = getaddrinfo(host, "443", &hints, &res);
	long dt = now_ms() - t0;

	int n = 0;
	if (rc == 0)
		for (struct addrinfo *a = res; a; a = a->ai_next)
			n++;
	if (res)
		freeaddrinfo(res);
	*out_count = (rc == 0) ? n : -1;
	return dt;
}

int main(void)
{
	static const char *hosts[] = {
		"www.youtube.com",
		"i.ytimg.com",
		"fonts.gstatic.com",
		"rr1---sn-2gb7snez.googlevideo.com",
		/* A host that does not exist. youtube hands the player decoy media URLs
		 * pointing at hostnames like this, and every one of them occupies a
		 * LibDNS thread-pool worker for however long the failure takes. With only
		 * 4 workers (and two jobs per lookup, A and AAAA), a handful of slow NXDOMAINs
		 * stalls DNS for the whole browser. */
		"rr1---sn-ab5sznzd.googlevideo.com",
		"nonexistent-host-9front-test.googlevideo.com",
		0
	};

	long worst = 0;
	const char *worst_what = "none";

	printf("%-38s %10s %10s %10s\n", "host", "A(ms)", "AAAA(ms)", "UNSPEC(ms)");
	for (int i = 0; hosts[i]; i++) {
		int c4, c6, cu;
		long t4 = timed_lookup(hosts[i], AF_INET, &c4);
		long t6 = timed_lookup(hosts[i], AF_INET6, &c6);
		long tu = timed_lookup(hosts[i], AF_UNSPEC, &cu);

		printf("%-38s %6ld/%-3d %6ld/%-3d %6ld/%-3d\n",
		       hosts[i], t4, c4, t6, c6, tu, cu);

		long m = t4 > t6 ? t4 : t6;
		if (tu > m) m = tu;
		if (m > worst) { worst = m; worst_what = hosts[i]; }
	}

	printf("\n(count -1 = lookup failed)\n");
	printf("slowest single lookup: %ld ms (%s), budget %d ms\n",
	       worst, worst_what, DNS_BUDGET_MS);
	printf("dnstime_gate %s\n", worst <= DNS_BUDGET_MS ? "PASS" : "FAIL");
	return worst <= DNS_BUDGET_MS ? 0 : 1;
}
