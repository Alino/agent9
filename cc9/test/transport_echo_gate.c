/* transport_echo_gate — standalone C mirror of the ladybird9 TransportPlan9
 * wire protocol, run before any Ladybird code exists on the box:
 *   header {u32 payload_size, u32 att_size, u32 att_count}, then attachment
 *   records (kind u8: 0=Segment{namelen,name,offset u64,len u64},
 *   1=Srv{namelen,name}), then payload — over a cc9 socketpair.
 *
 * Checks:
 *   1. message + Segment attachment: child imports the named segment, maps,
 *      checksums, mutates; parent sees the mutation
 *   2. message + Srv attachment: child opens the posted pipe end, removes the
 *      /srv entry, and completes a nested echo through it
 *   3. DEADLOCK PROBE: parent and child each stream 8 MB at the other,
 *      full-duplex, O_NONBLOCK + poll — impossible without honest POLLOUT +
 *      write rings (two blocking writers on full pipes deadlock)
 *   4. 10k-message blocking ping-pong soak (ring wraparound, framing)
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/shm9.h>
#include <sys/wait.h>

extern int socketpair(int, int, int, int[2]);
extern long n9_create(const char *, int, unsigned long);
extern long n9_pwrite(int, const void *, long, long long);
extern long n9_close(int);
extern long n9_remove(const char *);

static int npass;
static void ok(const char *what) { npass++; fprintf(stderr, "  ok %d: %s\n", npass, what); }
static void die(const char *what) {
	fprintf(stderr, "FAIL(%d): %s (errno %d %s)\n", getpid(), what, errno, strerror(errno));
	exit(1);
}

/* ---- framing ---- */
typedef struct { unsigned payload, attsz, attn; } hdr;

static void put_all(int fd, const void *b, long n) {
	const char *p = b;
	while (n > 0) {
		long r = write(fd, p, n);
		if (r < 0) die("put_all");
		p += r; n -= r;
	}
}
static void get_all(int fd, void *b, long n) {
	char *p = b;
	while (n > 0) {
		long r = read(fd, p, n);
		if (r <= 0) die("get_all");
		p += r; n -= r;
	}
}

static void send_msg(int fd, const char *payload, const char *att, unsigned attsz) {
	hdr h = { (unsigned)strlen(payload), attsz, attsz ? 1u : 0u };
	put_all(fd, &h, sizeof h);
	if (attsz) put_all(fd, att, attsz);
	put_all(fd, payload, h.payload);
}
/* returns payload in pay (NUL-terminated); attachment record in att */
static unsigned recv_msg(int fd, char *pay, unsigned paymax, char *att, unsigned attmax) {
	hdr h;
	get_all(fd, &h, sizeof h);
	if (h.attsz > attmax || h.payload >= paymax) die("recv_msg sizes");
	if (h.attsz) get_all(fd, att, h.attsz);
	get_all(fd, pay, h.payload);
	pay[h.payload] = 0;
	return h.attsz;
}

/* Segment record: kind=0, namelen, name, offset u64, len u64 */
static unsigned mk_segment_att(char *att, const char *name, unsigned long off, unsigned long len) {
	unsigned k = 0;
	att[k++] = 0;
	att[k++] = (char)strlen(name);
	memcpy(att + k, name, strlen(name)); k += strlen(name);
	memcpy(att + k, &off, 8); k += 8;
	memcpy(att + k, &len, 8); k += 8;
	return k;
}
/* Srv record: kind=1, namelen, name */
static unsigned mk_srv_att(char *att, const char *name) {
	unsigned k = 0;
	att[k++] = 1;
	att[k++] = (char)strlen(name);
	memcpy(att + k, name, strlen(name)); k += strlen(name);
	return k;
}

#define SEGSZ (1024 * 1024ul)
#define PAT(i) ((unsigned char)(((i) * 13) + 7))

/* ---- the full-duplex nonblocking pump (deadlock probe) ---- */
static void pump(int fd, unsigned long total, const char *tag) {
	if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) die("pump O_NONBLOCK");
	unsigned long sent = 0, rcvd = 0;
	char chunk[32768];
	while (sent < total || rcvd < total) {
		struct pollfd pf = { fd, 0, 0 };
		if (rcvd < total) pf.events |= POLLIN;
		if (sent < total) pf.events |= POLLOUT;
		int pr = poll(&pf, 1, 15000);
		if (pr < 0) die("pump poll");
		if (pr == 0) {
			fprintf(stderr, "%s: STALL sent=%lu rcvd=%lu\n", tag, sent, rcvd);
			die("pump 15s stall — DEADLOCK");
		}
		if ((pf.revents & POLLIN) && rcvd < total) {
			long r = read(fd, chunk, sizeof chunk);
			if (r < 0 && errno != EAGAIN) die("pump read");
			for (long i = 0; i < r; i++)
				if ((unsigned char)chunk[i] != PAT(rcvd + (unsigned long)i))
					die("pump data corruption");
			if (r > 0) rcvd += (unsigned long)r;
		}
		if ((pf.revents & POLLOUT) && sent < total) {
			long want = (long)(total - sent);
			if (want > (long)sizeof chunk) want = sizeof chunk;
			for (long i = 0; i < want; i++)
				chunk[i] = (char)PAT(sent + (unsigned long)i);
			long r = write(fd, chunk, want);
			if (r < 0 && errno != EAGAIN) die("pump write");
			if (r > 0) sent += (unsigned long)r;
		}
		if (pf.revents & POLLERR) die("pump POLLERR");
	}
}

#define PUMP_BYTES (8ul * 1024 * 1024)
#define SOAK_N 10000

static int child_main(int fd) {
	char pay[256], att[256];

	/* 1: Segment attachment */
	unsigned attsz = recv_msg(fd, pay, sizeof pay, att, sizeof att);
	if (strcmp(pay, "bitmap") != 0 || attsz == 0 || att[0] != 0) die("child: seg msg");
	char name[CC9_SHM_NAMELEN];
	unsigned nl = (unsigned char)att[1];
	memcpy(name, att + 2, nl); name[nl] = 0;
	unsigned long off, len;
	memcpy(&off, att + 2 + nl, 8);
	memcpy(&len, att + 2 + nl + 8, 8);
	int sfd = cc9_shm_import(name, off, len);
	if (sfd < 0) die("child: import");
	unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
	if (p == MAP_FAILED) die("child: map");
	for (unsigned long i = 0; i < len; i++)
		if (p[i] != PAT(i)) die("child: segment content");
	p[0] ^= 0xFF;                              /* mutate: parent checks */
	send_msg(fd, "bitmap-ok", 0, 0);
	munmap(p, len); close(sfd);

	/* 2: Srv attachment carrying a live pipe */
	attsz = recv_msg(fd, pay, sizeof pay, att, sizeof att);
	if (strcmp(pay, "newclient") != 0 || attsz == 0 || att[0] != 1) die("child: srv msg");
	nl = (unsigned char)att[1];
	memcpy(name, att + 2, nl); name[nl] = 0;
	char path[64];
	snprintf(path, sizeof path, "/srv/%s", name);
	int nfd = open(path, O_RDWR);
	if (nfd < 0) die("child: open srv");
	n9_remove(path);                           /* receiver-removes protocol */
	char nb[32];
	long r = read(nfd, nb, sizeof nb);         /* nested echo */
	if (r <= 0) die("child: nested read");
	put_all(nfd, nb, r);
	close(nfd);

	/* 3: deadlock probe */
	send_msg(fd, "pump-ready", 0, 0);
	pump(fd, PUMP_BYTES, "child");

	/* 4: soak — echo everything back, blocking mode */
	fcntl(fd, F_SETFL, 0);
	for (int i = 0; i < SOAK_N; i++) {
		unsigned seq;
		char body[64];
		get_all(fd, &seq, sizeof seq);
		get_all(fd, body, 48);
		if (seq != (unsigned)i) die("child: soak seq");
		put_all(fd, &seq, sizeof seq);
	}
	return 0;
}

int main(int argc, char **argv) {
	if (argc == 3 && strcmp(argv[1], "child") == 0)
		return child_main(atoi(argv[2]));

	int sp[2];
	if (socketpair(1, 1, 0, sp) < 0) die("socketpair");

	int kid = fork();
	if (kid < 0) die("fork");
	if (kid == 0) {
		char fdstr[16], *cargv[4];
		snprintf(fdstr, sizeof fdstr, "%d", sp[1]);
		cargv[0] = argv[0]; cargv[1] = "child"; cargv[2] = fdstr; cargv[3] = 0;
		close(sp[0]);
		execv(argv[0], cargv);
		_exit(127);
	}
	close(sp[1]);
	int fd = sp[0];
	char pay[256], att[256];

	/* 1: Segment attachment round-trip */
	int sfd = cc9_shm_create(SEGSZ);
	if (sfd < 0) die("shm create");
	unsigned char *p = mmap(0, SEGSZ, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
	if (p == MAP_FAILED) die("shm map");
	for (unsigned long i = 0; i < SEGSZ; i++) p[i] = PAT(i);
	char name[CC9_SHM_NAMELEN];
	unsigned long off, len;
	if (cc9_shm_export(sfd, name, &off, &len) < 0) die("export");
	unsigned attsz = mk_segment_att(att, name, off, len);
	send_msg(fd, "bitmap", att, attsz);
	recv_msg(fd, pay, sizeof pay, att, sizeof att);
	if (strcmp(pay, "bitmap-ok") != 0) die("bitmap ack");
	if (p[0] != (unsigned char)(PAT(0) ^ 0xFF)) die("child mutation not shared");
	ok("Segment attachment: import+map+checksum+mutate round-trip");
	munmap(p, SEGSZ); close(sfd);

	/* 2: Srv attachment */
	int np[2];
	if (socketpair(1, 1, 0, np) < 0) die("nested socketpair");
	char sname[32], spath[64], num[16];
	snprintf(sname, sizeof sname, "lbx.%d", getpid());
	snprintf(spath, sizeof spath, "/srv/%s", sname);
	long svfd = n9_create(spath, 1, 0600);
	if (svfd < 0) die("srv create");
	int n = snprintf(num, sizeof num, "%d", np[1]);
	if (n9_pwrite((int)svfd, num, n, -1) != n) die("srv post");
	n9_close((int)svfd);
	close(np[1]);
	attsz = mk_srv_att(att, sname);
	send_msg(fd, "newclient", att, attsz);
	put_all(np[0], "nested-ping", 11);
	char nb[32];
	get_all(np[0], nb, 11);
	if (memcmp(nb, "nested-ping", 11) != 0) die("nested echo");
	ok("Srv attachment: fd passed, entry removed, nested echo");
	close(np[0]);

	/* 3: deadlock probe — 8 MB each way simultaneously */
	recv_msg(fd, pay, sizeof pay, att, sizeof att);
	if (strcmp(pay, "pump-ready") != 0) die("pump handshake");
	pump(fd, PUMP_BYTES, "parent");
	ok("deadlock probe: 8 MB full-duplex, honest POLLOUT, no stall");

	/* 4: soak */
	fcntl(fd, F_SETFL, 0);
	for (unsigned i = 0; i < SOAK_N; i++) {
		char body[48];
		memset(body, (int)('a' + i % 26), sizeof body);
		put_all(fd, &i, sizeof i);
		put_all(fd, body, sizeof body);
		unsigned echo;
		get_all(fd, &echo, sizeof echo);
		if (echo != i) die("soak echo");
	}
	ok("10k-message framed soak (ring wraparound)");

	int st;
	if (waitpid(kid, &st, 0) != kid || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
		die("child exit");
	ok("child exited clean");

	printf("PASS %d/%d\n", npass, npass);
	return 0;
}
