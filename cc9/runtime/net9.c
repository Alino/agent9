/* net9.c — BSD sockets over Plan 9's /net (the dial(2)/announce(2) protocol).
 *
 * Replaces the historical ENOSYS stubs. Ported from the proven Rust
 * implementation in rust9 (std/sys/net/connection/plan9.rs):
 *
 *   TCP connect:  open /net/tcp/clone -> read conn N ->
 *                 write "connect ip!port" -> open /net/tcp/N/data
 *   TCP announce: write "announce local!port" -> open /net/tcp/N/listen
 *                 (open blocks; the returned fd is the NEW conn's ctl)
 *   resolve:      write "tcp!host!port" to /net/cs, read
 *                 "/net/tcp/clone ip!port" lines back
 *   UDP:          announce + "headers"; each datagram on the data file
 *                 carries a 52-byte Udphdr (raddr/laddr/ifcaddr/rport/lport)
 *
 * The BSD fd contract is kept with a dup2 shuffle: socket() hands out the
 * clone ctl fd; connect()/accept() dup2 the data fd onto that number so
 * read/write/poll on the caller's fd hit the byte stream, while the ctl fd
 * lives on in a side table (shutdown/getsockname need it).
 *
 * Honest limits: connect() is synchronous even on O_NONBLOCK sockets (a /net
 * connect can't be polled); an option /net has no knob for fails with
 * ENOPROTOOPT rather than being swallowed (see setsockopt). */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

extern long n9_open(const char *, int);
extern long n9_close(int);
extern long n9_pread(int, void *, long, long long);
extern long n9_pwrite(int, const void *, long, long long);
extern long n9_dup(int, int);
extern int cc9_errno_from_errstr(void);
extern int cc9_errno_from_errstr_or(int);
extern void n9_semacquire(int *, int);
extern void n9_semrelease(int *, int);

#define NS_MAX 64

typedef struct {
	int fd;          /* the caller's socket fd (-1 = free) */
	int ctl;         /* the /net ctl fd once connected/announced (-1 early: fd IS ctl) */
	int conn;        /* /net conn number */
	int type;        /* SOCK_STREAM / SOCK_DGRAM */
	int state;       /* 0 fresh, 1 bound, 2 announced, 3 connected, 4 udp-headers */
	int err;         /* latched connect errno, read-and-cleared by SO_ERROR */
	int ka_on;       /* SO_KEEPALIVE requested (pending until Established) */
	int ka_ms;       /* TCP_KEEPIDLE interval in ms, 0 = /net default */
	unsigned short bind_port;
	unsigned int bind_ip;    /* network order */
} ns_ent;

static ns_ent ns_tab[NS_MAX];
static int ns_inited;
/* ONE global lock over the whole ns_tab. Contract: held ONLY around structural
 * table ops (init/lookup/alloc/free), which are O(NS_MAX) scans with no blocking
 * I/O — NEVER across the /net ctl reads/writes in connect/accept/bind that sleep,
 * or all socket I/O would serialize. The known connect-vs-close race (a helper in
 * connect(fd) while another thread closes fd) is out of scope: ns_get hands back a
 * pointer that outlives the lock. ponytail: single lock; per-entry only if socket
 * churn ever shows contention. */
static int ns_lock = 1;

static void ns_init(void) {   /* caller holds ns_lock */
	if (ns_inited) return;
	for (int i = 0; i < NS_MAX; i++) ns_tab[i].fd = -1;
	ns_inited = 1;
}

/* lock-free scan; caller must hold ns_lock */
static ns_ent *ns_find(int fd) {
	if (!ns_inited) return 0;
	for (int i = 0; i < NS_MAX; i++)
		if (ns_tab[i].fd == fd) return &ns_tab[i];
	return 0;
}

static ns_ent *ns_get(int fd) {
	n9_semacquire(&ns_lock, 1);
	ns_ent *e = ns_find(fd);
	n9_semrelease(&ns_lock, 1);
	return e;
}

static ns_ent *ns_new(int fd) {
	n9_semacquire(&ns_lock, 1);   /* scan-for-free + claim must be atomic, else two
	                                 socket()s double-claim one slot */
	ns_init();
	for (int i = 0; i < NS_MAX; i++)
		if (ns_tab[i].fd < 0) {
			memset(&ns_tab[i], 0, sizeof ns_tab[i]);
			ns_tab[i].fd = fd;
			ns_tab[i].ctl = -1;
			n9_semrelease(&ns_lock, 1);
			return &ns_tab[i];
		}
	n9_semrelease(&ns_lock, 1);
	return 0;
}

/* fs.c close() calls this so a reused fd number can't hit a stale entry. Frees the
 * slot under the lock; closes the parked ctl OUTSIDE it (close is I/O). */
void cc9_net_onclose(int fd) {
	int ctl = -1;
	n9_semacquire(&ns_lock, 1);
	ns_ent *e = ns_find(fd);
	if (e) { ctl = e->ctl; e->fd = -1; }
	n9_semrelease(&ns_lock, 1);
	if (ctl >= 0) n9_close(ctl);
}

static const char *ns_proto(const ns_ent *e) {
	return e->type == SOCK_DGRAM ? "udp" : "tcp";
}

/* read the decimal conn number a fresh clone ctl reports */
static int read_conn_number(int fd) {
	char buf[64];
	long n = n9_pread(fd, buf, sizeof buf - 1, 0);
	if (n <= 0) return -1;
	buf[n] = 0;
	int num = -1;
	for (char *p = buf; *p; p++)
		if (*p >= '0' && *p <= '9') { num = atoi(p); break; }
	return num;
}

/* `dflt` is the errno for an errstr this build doesn't recognise. It must be per
 * call site: cc9_errno_from_errstr()'s own default is ENOENT, which is right for
 * a failed open and useless for a failed connect. */
static int ctl_write(int fd, const char *msg, int dflt) {
	long n = n9_pwrite(fd, msg, (long)strlen(msg), -1);
	if (n < 0) { errno = cc9_errno_from_errstr_or(dflt); return -1; }
	return 0;
}

/* The ctl fd: the caller's fd IS the clone ctl until connect()/bind() swaps the
 * data file onto that number and parks ctl aside. */
static int ns_ctl(const ns_ent *e, int fd) { return e->ctl >= 0 ? e->ctl : fd; }

/* 9front's tcpctl (sys/src/9/ip/tcp.c) takes "keepalive [ms]" — but tcpstartka
 * refuses unless the conn is Established, so a pre-connect request is held in
 * ka_on/ka_ms and issued by connect(). */
static int ka_apply(ns_ent *e, int fd) {
	char msg[32];
	if (e->ka_ms > 0) snprintf(msg, sizeof msg, "keepalive %d", e->ka_ms);
	else snprintf(msg, sizeof msg, "keepalive");
	return ctl_write(ns_ctl(e, fd), msg, EIO);
}

static void addr_to_str(const void *sa, char *out, unsigned long cap) {
	const struct sockaddr *s = sa;
	if (s->sa_family == AF_INET6) {
		const struct sockaddr_in6 *a = sa;
		char ip[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof ip);
		snprintf(out, cap, "%s!%d", ip, ntohs(a->sin6_port));
	} else {
		const struct sockaddr_in *a = sa;
		char ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
		snprintf(out, cap, "%s!%d", ip, ntohs(a->sin_port));
	}
}

/* parse "ip!port" into a sockaddr; returns length or 0 */
static socklen_t str_to_addr(const char *s, void *out) {
	char ip[64];
	const char *bang = strrchr(s, '!');
	if (!bang || (unsigned long)(bang - s) >= sizeof ip) return 0;
	memcpy(ip, s, bang - s);
	ip[bang - s] = 0;
	int port = atoi(bang + 1);
	struct in_addr a4;
	if (inet_pton(AF_INET, ip, &a4) == 1) {
		struct sockaddr_in *sa = out;
		memset(sa, 0, sizeof *sa);
		sa->sin_family = AF_INET;
		sa->sin_port = htons((unsigned short)port);
		sa->sin_addr = a4;
		return sizeof *sa;
	}
	struct in6_addr a6;
	if (inet_pton(AF_INET6, ip, &a6) == 1) {
		struct sockaddr_in6 *sa = out;
		memset(sa, 0, sizeof *sa);
		sa->sin6_family = AF_INET6;
		sa->sin6_port = htons((unsigned short)port);
		sa->sin6_addr = a6;
		return sizeof *sa;
	}
	return 0;
}

int socket(int af, int type, int proto) {
	(void)proto;
	if (af != AF_INET && af != AF_INET6) { errno = EAFNOSUPPORT; return -1; }
	type &= 0xf;   /* strip SOCK_NONBLOCK/SOCK_CLOEXEC */
	if (type != SOCK_STREAM && type != SOCK_DGRAM) { errno = EPROTONOSUPPORT; return -1; }
	char path[32];
	snprintf(path, sizeof path, "/net/%s/clone", type == SOCK_DGRAM ? "udp" : "tcp");
	long fd = n9_open(path, O_RDWR);
	if (fd < 0) { errno = cc9_errno_from_errstr(); return -1; }
	int conn = read_conn_number((int)fd);
	if (conn < 0) { n9_close((int)fd); errno = EIO; return -1; }
	ns_ent *e = ns_new((int)fd);
	if (!e) { n9_close((int)fd); errno = EMFILE; return -1; }
	e->conn = conn;
	e->type = type;
	return (int)fd;
}

/* swap the caller-visible fd over to the conn's data file, park ctl aside */
static int swap_in_data(ns_ent *e, int fd) {
	char path[48];
	snprintf(path, sizeof path, "/net/%s/%d/data", ns_proto(e), e->conn);
	long data = n9_open(path, O_RDWR);
	if (data < 0) { errno = cc9_errno_from_errstr(); return -1; }
	long ctl = n9_dup(fd, -1);
	if (ctl < 0) { n9_close((int)data); errno = EMFILE; return -1; }
	if (n9_dup((int)data, fd) < 0) {
		n9_close((int)data); n9_close((int)ctl);
		errno = EMFILE;
		return -1;
	}
	n9_close((int)data);
	e->ctl = (int)ctl;
	return 0;
}

int connect(int fd, const struct sockaddr *sa, socklen_t len) {
	(void)len;
	ns_ent *e = ns_get(fd);
	if (!e) { errno = ENOTSOCK; return -1; }
	if (e->state == 3) { errno = EISCONN; return -1; }
	char addr[80], msg[96];
	addr_to_str(sa, addr, sizeof addr);
	snprintf(msg, sizeof msg, "connect %s", addr);
	/* The ctl write is where the connect result arrives: connectctlmsg (devip.c)
	 * sleeps until the conv leaves Connecting, then errors with the conv's cerr
	 * ("connection refused", "connection timed out", an ICMP "host unreachable"
	 * …). Latch it — SO_ERROR is the only way a non-blocking caller can ever get
	 * it back, and reporting 0 there tells the app a refused connect CONNECTED. */
	if (ctl_write(fd, msg, ECONNREFUSED) < 0) { e->err = errno; return -1; }
	if (swap_in_data(e, fd) < 0) { e->err = errno; return -1; }
	e->state = 3;
	/* Now Established (the ctl write above waited for it), so a keepalive held
	 * by setsockopt applies here. It can only fail if the peer already tore the
	 * conn down, in which case keepalive is moot. */
	if (e->ka_on) ka_apply(e, fd);
	return 0;
}

int bind(int fd, const struct sockaddr *sa, socklen_t len) {
	(void)len;
	ns_ent *e = ns_get(fd);
	if (!e) { errno = ENOTSOCK; return -1; }
	const struct sockaddr_in *a = (const struct sockaddr_in *)sa;
	e->bind_port = ntohs(a->sin_port);
	e->bind_ip = (a->sin_family == AF_INET) ? a->sin_addr.s_addr : 0;
	e->state = 1;
	if (e->type == SOCK_DGRAM) {
		/* UDP binds for real now: announce + headers mode */
		char msg[64];
		if (e->bind_port)
			snprintf(msg, sizeof msg, "announce *!%d", e->bind_port);
		else
			snprintf(msg, sizeof msg, "announce *!0");
		if (ctl_write(fd, msg, EADDRINUSE) < 0) { errno = EADDRINUSE; return -1; }
		if (ctl_write(fd, "headers", EIO) < 0) return -1;
		if (swap_in_data(e, fd) < 0) return -1;
		e->state = 4;
	}
	return 0;
}

int listen(int fd, int backlog) {
	(void)backlog;
	ns_ent *e = ns_get(fd);
	if (!e) { errno = ENOTSOCK; return -1; }
	char msg[64];
	if (e->bind_port)
		snprintf(msg, sizeof msg, "announce *!%d", e->bind_port);
	else
		snprintf(msg, sizeof msg, "announce *!0");
	if (ctl_write(fd, msg, EADDRINUSE) < 0) { errno = EADDRINUSE; return -1; }
	e->state = 2;
	return 0;
}

int accept4(int fd, struct sockaddr *sa, socklen_t *salen, int flags) {
	extern int accept(int, struct sockaddr *, socklen_t *);
	extern int fcntl(int, int, ...);
	int nfd = accept(fd, sa, salen);
	if (nfd >= 0 && flags) {
		if (flags & 0x1000 /*SOCK_NONBLOCK == O_NONBLOCK here*/)
			fcntl(nfd, 4 /*F_SETFL*/, 0x1000);
		if (flags & 0x2000 /*SOCK_CLOEXEC*/)
			fcntl(nfd, 2 /*F_SETFD*/, 1 /*FD_CLOEXEC*/);
	}
	return nfd;
}

int accept(int fd, struct sockaddr *sa, socklen_t *salen) {
	ns_ent *e = ns_get(fd);
	if (!e || e->state != 2) { errno = e ? EINVAL : ENOTSOCK; return -1; }
	char path[48];
	snprintf(path, sizeof path, "/net/%s/%d/listen", ns_proto(e), e->conn);
	long lfd = n9_open(path, O_RDWR);   /* blocks until a caller arrives */
	if (lfd < 0) { errno = cc9_errno_from_errstr(); return -1; }
	int newconn = read_conn_number((int)lfd);
	if (newconn < 0) { n9_close((int)lfd); errno = EIO; return -1; }
	snprintf(path, sizeof path, "/net/%s/%d/data", ns_proto(e), newconn);
	long data = n9_open(path, O_RDWR);
	if (data < 0) { n9_close((int)lfd); errno = cc9_errno_from_errstr(); return -1; }
	ns_ent *ne = ns_new((int)data);
	if (!ne) { n9_close((int)lfd); n9_close((int)data); errno = EMFILE; return -1; }
	ne->conn = newconn;
	ne->type = e->type;
	ne->state = 3;
	ne->ctl = (int)lfd;   /* the listen fd IS the new conn's ctl */
	if (sa && salen) {
		getpeername((int)data, sa, salen);
	}
	return (int)data;
}

static int read_endpoint(ns_ent *e, const char *which, void *sa, socklen_t *salen) {
	char path[48], buf[128];
	snprintf(path, sizeof path, "/net/%s/%d/%s", ns_proto(e), e->conn, which);
	long fd = n9_open(path, O_RDONLY);
	if (fd < 0) { errno = cc9_errno_from_errstr(); return -1; }
	long n = n9_pread((int)fd, buf, sizeof buf - 1, 0);
	n9_close((int)fd);
	if (n <= 0) { errno = EIO; return -1; }
	buf[n] = 0;
	char *sp = strchr(buf, ' ');
	if (sp) *sp = 0;
	char *nl = strchr(buf, '\n');
	if (nl) *nl = 0;
	struct sockaddr_storage tmp;
	socklen_t l = str_to_addr(buf, &tmp);
	if (!l) { errno = EIO; return -1; }
	if (sa && salen) {
		socklen_t c = *salen < l ? *salen : l;
		memcpy(sa, &tmp, c);
		*salen = l;
	}
	return 0;
}

int getsockname(int fd, struct sockaddr *sa, socklen_t *salen) {
	ns_ent *e = ns_get(fd);
	if (!e) { errno = ENOTSOCK; return -1; }
	if (e->state < 2) {   /* not announced yet: report the bind() intent */
		struct sockaddr_in a;
		memset(&a, 0, sizeof a);
		a.sin_family = AF_INET;
		a.sin_port = htons(e->bind_port);
		a.sin_addr.s_addr = e->bind_ip;
		socklen_t c = *salen < sizeof a ? *salen : (socklen_t)sizeof a;
		memcpy(sa, &a, c);
		*salen = sizeof a;
		return 0;
	}
	return read_endpoint(e, "local", sa, salen);
}

int getpeername(int fd, struct sockaddr *sa, socklen_t *salen) {
	ns_ent *e = ns_get(fd);
	if (!e) { errno = ENOTSOCK; return -1; }
	if (e->state != 3) { errno = ENOTCONN; return -1; }
	return read_endpoint(e, "remote", sa, salen);
}

int shutdown(int fd, int how) {
	ns_ent *e = ns_get(fd);
	if (!e) { errno = ENOTSOCK; return -1; }
	/* /net has no half-close: the only teardown is "hangup", which closes BOTH
	 * directions. So (mirroring rust9's net overlay):
	 *  - SHUT_RD: no-op. Hanging up here would also kill the WRITE side and break
	 *    send-then-read-response / TLS close_notify. (Trade-off: shutdown(RD) can no
	 *    longer unblock another thread's blocking read; use a timeout / close.)
	 *  - SHUT_WR / SHUT_RDWR: hangup. Lossy for SHUT_WR (it also stops reads), but
	 *    it's the only way to signal EOF to the peer on /net. */
	if (how == SHUT_RD) return 0;
	if (e->ctl >= 0) n9_pwrite(e->ctl, "hangup", 6, -1);
	return 0;
}

/* MSG_* on a /net read. /net has neither a peek nor a per-read non-block knob:
 *  - MSG_PEEK:     EOPNOTSUPP. A /net read CONSUMES the bytes; faking a peek would
 *                  silently eat the caller's stream, so refuse honestly instead.
 *  - MSG_WAITALL:  loop until the whole buffer is filled (or EOF/error).
 *  - MSG_DONTWAIT: no true one-shot non-block on /net; the read stays BLOCKING —
 *                  documented, not pretended. Other MSG_* bits are ignored. */
static long net_read_flags(int fd, void *buf, unsigned long n, int flags) {
	if (flags & MSG_PEEK) { errno = EOPNOTSUPP; return -1; }
	if ((flags & MSG_WAITALL) && n) {
		unsigned long got = 0;
		while (got < n) {
			long r = read(fd, (char *)buf + got, n - got);
			if (r < 0) return got ? (long)got : -1;   /* report progress; errno on retry */
			if (r == 0) break;                         /* EOF: short count */
			got += (unsigned long)r;
		}
		return (long)got;
	}
	return read(fd, buf, n);
}

long send(int fd, const void *buf, unsigned long n, int flags) {
	/* /net has no urgent/out-of-band channel, so MSG_OOB can't be honored — refuse
	 * rather than sending it inline as normal data. MSG_NOSIGNAL (Plan 9 write just
	 * errors, no SIGPIPE) and MSG_MORE (a coalescing hint) are safe to ignore;
	 * MSG_DONTWAIT stays blocking (see net_read_flags). */
	if (flags & MSG_OOB) { errno = EOPNOTSUPP; return -1; }
	return write(fd, buf, n);
}

long recv(int fd, void *buf, unsigned long n, int flags) {
	return net_read_flags(fd, buf, n, flags);
}

/* ---- socket options ---------------------------------------------------------
 *
 * What /net's ctl files actually accept, read off the 9front kernel source:
 *
 *   devip.c ipwrite/Qctl:  connect announce bind ttl tos ignoreadvice
 *                          addmulti delmulti|remmulti, then the proto's own ctl
 *   tcp.c   tcpctl:        close hangup keepalive [ms] checksum   — and nothing
 *                          else; anything unlisted errors "unknown control
 *                          request", so writing it would fail, not silently work.
 *
 * There is NO "nodelay" verb, because 9front's TCP has no Nagle to switch off:
 * tcpoutput() sends whatever the write queue holds. TCP_NODELAY=1 is therefore
 * already the truth on this stack — accepting it states a fact rather than
 * hiding one. Every other unlisted option fails with ENOPROTOOPT: an option
 * accepted and thrown away (the pthread_attr_setstacksize mistake) turns into a
 * silent misbehaviour thousands of lines from the setsockopt call. */
int setsockopt(int fd, int level, int opt, const void *val, socklen_t len) {
	ns_ent *e = ns_get(fd);
	if (!e) { errno = ENOTSOCK; return -1; }
	int v = (val && len >= 4) ? *(const int *)val : 0;
	char msg[32];

	if (level == SOL_SOCKET) switch (opt) {
	case SO_KEEPALIVE:
		if (!v) {
			/* tcpctl can start the ka timer but has no verb to stop it. */
			if (e->ka_on && e->state == 3) { errno = ENOPROTOOPT; return -1; }
			e->ka_on = 0;
			return 0;
		}
		e->ka_on = 1;
		return e->state == 3 ? ka_apply(e, fd) : 0;   /* else connect() applies it */
	case SO_REUSEADDR:
	case SO_REUSEPORT:
		/* No knob, and none needed: /net decides port reuse itself in
		 * Fsstdannounce — there is no state for this flag to change, so
		 * accepting it discards nothing. Servers set it unconditionally. */
		return 0;
	case SO_BROADCAST:
		/* Likewise: /net/udp needs no per-socket permission to broadcast. */
		return 0;
	}

	if (level == IPPROTO_TCP) switch (opt) {
	case TCP_NODELAY:
		if (v) return 0;   /* already true: no Nagle in 9front's TCP */
		errno = ENOPROTOOPT;   /* and it can't be turned back on */
		return -1;
	case TCP_KEEPIDLE:
		e->ka_ms = v * 1000;   /* tcpstartka takes ms; ignores < MSPTICK */
		return (e->ka_on && e->state == 3) ? ka_apply(e, fd) : 0;
	}

	/* devip's generic per-conv knobs, on any proto, in any state. */
	if ((level == IPPROTO_IP && opt == IP_TTL) ||
	    (level == IPPROTO_IPV6 && opt == IPV6_UNICAST_HOPS)) {
		snprintf(msg, sizeof msg, "ttl %d", v);
		return ctl_write(ns_ctl(e, fd), msg, EINVAL);
	}
	if (level == IPPROTO_IP && opt == IP_TOS) {
		snprintf(msg, sizeof msg, "tos %d", v);
		return ctl_write(ns_ctl(e, fd), msg, EINVAL);
	}

	errno = ENOPROTOOPT;
	return -1;
}

int getsockopt(int fd, int level, int opt, void *val, socklen_t *len) {
	ns_ent *e = ns_get(fd);
	if (!e) { errno = ENOTSOCK; return -1; }
	if (!val || !len || *len < 4) { errno = EINVAL; return -1; }
	int v;

	if (level == IPPROTO_TCP && opt == TCP_NODELAY)
		v = 1;                       /* 9front's TCP has no Nagle; see setsockopt */
	else if (level != SOL_SOCKET) { errno = ENOPROTOOPT; return -1; }
	else if (opt == SO_ERROR) {
		/* The result of a non-blocking connect, and the ONLY route back to it:
		 * connect() -> poll writable -> getsockopt(SO_ERROR) is what Rust's
		 * TcpStream, libuv and asyncio all do. POSIX says read-and-clear. */
		v = e->err;
		e->err = 0;
	}
	else if (opt == SO_TYPE)      v = e->type;
	else if (opt == SO_KEEPALIVE) v = e->ka_on;
	else { errno = ENOPROTOOPT; return -1; }

	memcpy(val, &v, 4);
	*len = 4;
	return 0;
}

/* ---- UDP datagrams: 52-byte Udphdr in headers mode ---- */

#define UDPHDR 52

long sendto(int fd, const void *buf, unsigned long n, int flags,
            const void *sa0, socklen_t salen) {
	const struct sockaddr *sa = sa0;
	(void)flags; (void)salen;
	ns_ent *e = ns_get(fd);
	if (!e) { errno = ENOTSOCK; return -1; }
	if (e->state == 3 || !sa) return write(fd, buf, n);
	if (e->type != SOCK_DGRAM) { errno = EDESTADDRREQ; return -1; }
	if (e->state != 4) {   /* implicit bind, as BSD does */
		struct sockaddr_in any;
		memset(&any, 0, sizeof any);
		any.sin_family = AF_INET;
		if (bind(fd, (struct sockaddr *)&any, sizeof any) < 0) return -1;
	}
	const struct sockaddr_in *a = (const struct sockaddr_in *)sa;
	unsigned char pkt[UDPHDR + 65536];
	if (n > sizeof pkt - UDPHDR) { errno = EMSGSIZE; return -1; }
	memset(pkt, 0, UDPHDR);
	/* v4-mapped v6 form, as /net writes it */
	memset(pkt + 10, 0xff, 2);
	memcpy(pkt + 12, &a->sin_addr.s_addr, 4);
	pkt[48] = (unsigned char)(ntohs(a->sin_port) >> 8);   /* rport: after 3x16B addrs */
	pkt[49] = (unsigned char)(ntohs(a->sin_port) & 0xff);
	memcpy(pkt + UDPHDR, buf, n);
	long w = write(fd, pkt, UDPHDR + n);
	return w < 0 ? w : w - UDPHDR;
}

long recvfrom(int fd, void *buf, unsigned long n, int flags,
              void *sa0, socklen_t *salen) {
	void *sa = sa0;
	ns_ent *e = ns_get(fd);
	if (!e) { errno = ENOTSOCK; return -1; }
	if (flags & MSG_PEEK) { errno = EOPNOTSUPP; return -1; }   /* /net reads consume; no peek */
	if (e->state == 3 || (!sa && e->state != 4)) return net_read_flags(fd, buf, n, flags);
	if (e->state != 4) { errno = ENOTCONN; return -1; }
	unsigned char pkt[UDPHDR + 65536];
	unsigned long want = n > sizeof pkt - UDPHDR ? sizeof pkt : n + UDPHDR;
	long r = read(fd, pkt, want);
	if (r < 0) return r;
	if (r < UDPHDR) { errno = EIO; return -1; }
	long payload = r - UDPHDR;
	memcpy(buf, pkt + UDPHDR, (unsigned long)payload);
	if (sa && salen) {
		struct sockaddr_in a;
		memset(&a, 0, sizeof a);
		a.sin_family = AF_INET;
		memcpy(&a.sin_addr.s_addr, pkt + 12, 4);   /* v4-mapped tail */
		a.sin_port = htons((unsigned short)((pkt[48] << 8) | pkt[49]));
		socklen_t c = *salen < sizeof a ? *salen : (socklen_t)sizeof a;
		memcpy(sa, &a, c);
		*salen = sizeof a;
	}
	return payload;
}

/* ---- resolver: /net/cs, with a numeric fast path ---- */

static struct addrinfo *ai_one(const char *ipport, int socktype) {
	struct sockaddr_storage tmp;
	socklen_t l = str_to_addr(ipport, &tmp);
	if (!l) return 0;
	struct addrinfo *ai = calloc(1, sizeof *ai + sizeof tmp);
	if (!ai) return 0;
	ai->ai_addr = (struct sockaddr *)(ai + 1);
	memcpy(ai->ai_addr, &tmp, l);
	ai->ai_addrlen = l;
	ai->ai_family = ((struct sockaddr *)&tmp)->sa_family;
	ai->ai_socktype = socktype ? socktype : SOCK_STREAM;
	ai->ai_protocol = ai->ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
	return ai;
}

int getaddrinfo(const char *host, const char *serv,
                const struct addrinfo *hints, struct addrinfo **res) {
	int socktype = hints ? hints->ai_socktype : 0;
	const char *proto = socktype == SOCK_DGRAM ? "udp" : "tcp";
	const char *port = serv && *serv ? serv : "0";
	char buf[192];
	*res = 0;

	if (!host || !*host)
		host = (hints && (hints->ai_flags & AI_PASSIVE)) ? "*" : "127.0.0.1";

	/* numeric ip + numeric port: no cs round-trip */
	struct sockaddr_storage tmp;
	snprintf(buf, sizeof buf, "%s!%s", host, port);
	if (port[0] >= '0' && port[0] <= '9' && str_to_addr(buf, &tmp)) {
		*res = ai_one(buf, socktype);
		return *res ? 0 : EAI_MEMORY;
	}

	long cs = n9_open("/net/cs", O_RDWR);
	if (cs < 0) return EAI_FAIL;
	snprintf(buf, sizeof buf, "%s!%s!%s", proto, host, port);
	if (n9_pwrite((int)cs, buf, (long)strlen(buf), -1) < 0) {
		n9_close((int)cs);
		return EAI_NONAME;
	}
	struct addrinfo *head = 0, **tail = &head;
	long n;
	long long off = 0;
	char line[192];
	while ((n = n9_pread((int)cs, line, sizeof line - 1, off)) > 0) {
		off += n;
		line[n] = 0;
		/* "/net/tcp/clone 1.2.3.4!80" */
		char *sp = strchr(line, ' ');
		if (!sp) continue;
		char *nl = strchr(sp + 1, '\n');
		if (nl) *nl = 0;
		struct addrinfo *ai = ai_one(sp + 1, socktype);
		if (ai) { *tail = ai; tail = &ai->ai_next; }
	}
	n9_close((int)cs);
	if (!head) return EAI_NONAME;
	*res = head;
	return 0;
}

void freeaddrinfo(struct addrinfo *ai) {
	while (ai) {
		struct addrinfo *next = ai->ai_next;
		free(ai);
		ai = next;
	}
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host,
                socklen_t hostlen, char *serv, socklen_t servlen, int flags) {
	(void)salen; (void)flags;
	char addr[80];
	addr_to_str(sa, addr, sizeof addr);
	char *bang = strrchr(addr, '!');
	if (!bang) return EAI_FAIL;
	*bang = 0;
	if (host && hostlen) snprintf(host, hostlen, "%s", addr);
	if (serv && servlen) snprintf(serv, servlen, "%s", bang + 1);
	return 0;
}

const char *gai_strerror(int e) {
	switch (e) {
	case EAI_NONAME: return "name or service not known";
	case EAI_FAIL:   return "non-recoverable resolver failure (/net/cs)";
	case EAI_MEMORY: return "out of memory";
	}
	return "resolver error";
}

/* ---- sendmsg / recvmsg -----------------------------------------------------
 *
 * These were ENOSYS stubs in posix_llvm.c (Plan 9 has no cmsg, so the original
 * shim refused outright). But socket2 — and therefore anything above it — uses
 * them for ordinary VECTORED I/O, which /net does support: the msghdr's iovec
 * array maps straight onto readv/writev, and msg_name onto sendto/recvfrom.
 *
 * What genuinely cannot work is ancillary data (msg_control): SCM_RIGHTS fd
 * passing has no /net equivalent — Plan 9 passes channels via /srv, not over a
 * connection. A caller asking for that gets EOPNOTSUPP rather than a silent
 * drop, which would look like a lost fd at the far end.
 */

extern long readv(int, const struct iovec *, int);
extern long writev(int, const struct iovec *, int);

/* Total bytes across an iovec array, or -1 on overflow. */
static long iov_total(const struct iovec *iov, int n) {
	unsigned long t = 0;
	for (int i = 0; i < n; i++) {
		unsigned long l = iov[i].iov_len;
		if (t + l < t) return -1;
		t += l;
	}
	return (long)t;
}

long sendmsg(int fd, const struct msghdr *m, int flags) {
	if (!m) { errno = EINVAL; return -1; }
	if (m->msg_control && m->msg_controllen) {
		/* Ancillary data (SCM_RIGHTS et al). Plan 9 passes channels through
		 * /srv, not inside a connection; there is nothing to translate to. */
		errno = EOPNOTSUPP;
		return -1;
	}
	const struct iovec *iov = m->msg_iov;
	int niov = m->msg_iovlen;
	if (niov < 0 || (niov > 0 && !iov)) { errno = EINVAL; return -1; }

	ns_ent *e = ns_get(fd);
	if (!e) { errno = ENOTSOCK; return -1; }

	/* Unconnected datagram with a destination: sendto needs one flat buffer,
	 * because the /net header must precede the payload in a single write. */
	if (m->msg_name && m->msg_namelen && e->type == SOCK_DGRAM && e->state != 3) {
		long total = iov_total(iov, niov);
		if (total < 0) { errno = EINVAL; return -1; }
		char *tmp = malloc((unsigned long)total ? (unsigned long)total : 1);
		if (!tmp) { errno = ENOMEM; return -1; }
		long off = 0;
		for (int i = 0; i < niov; i++) {
			memcpy(tmp + off, iov[i].iov_base, iov[i].iov_len);
			off += (long)iov[i].iov_len;
		}
		long r = sendto(fd, tmp, (unsigned long)total, flags,
		                m->msg_name, m->msg_namelen);
		free(tmp);
		return r;
	}

	/* Connected (or stream): the iovec array is exactly writev's argument. */
	return writev(fd, iov, niov);
}

long recvmsg(int fd, struct msghdr *m, int flags) {
	if (!m) { errno = EINVAL; return -1; }
	struct iovec *iov = m->msg_iov;
	int niov = m->msg_iovlen;
	if (niov < 0 || (niov > 0 && !iov)) { errno = EINVAL; return -1; }

	/* No ancillary data is ever produced; say so rather than leave the
	 * caller's controllen holding a stale value it might parse. */
	m->msg_controllen = 0;
	m->msg_flags = 0;

	ns_ent *e = ns_get(fd);
	if (!e) { errno = ENOTSOCK; return -1; }

	/* Datagram where the caller wants the sender's address: recvfrom strips the
	 * /net Udphdr for us, so read flat and scatter afterwards. */
	if (m->msg_name && m->msg_namelen && e->type == SOCK_DGRAM && e->state != 3) {
		long total = iov_total(iov, niov);
		if (total < 0) { errno = EINVAL; return -1; }
		char *tmp = malloc((unsigned long)total ? (unsigned long)total : 1);
		if (!tmp) { errno = ENOMEM; return -1; }
		socklen_t nl = m->msg_namelen;
		long r = recvfrom(fd, tmp, (unsigned long)total, flags, m->msg_name, &nl);
		if (r < 0) { free(tmp); return -1; }
		m->msg_namelen = nl;
		long off = 0, left = r;
		for (int i = 0; i < niov && left > 0; i++) {
			unsigned long take = iov[i].iov_len < (unsigned long)left
			                   ? iov[i].iov_len : (unsigned long)left;
			memcpy(iov[i].iov_base, tmp + off, take);
			off += (long)take;
			left -= (long)take;
		}
		free(tmp);
		return r;
	}

	if (m->msg_name) m->msg_namelen = 0;
	return readv(fd, iov, niov);
}
