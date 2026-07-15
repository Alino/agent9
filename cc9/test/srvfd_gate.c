/* srvfd_gate — proves Plan 9's /srv gives cc9 genuine fd passing between
 * unrelated processes: post an open fd under a name, another process opens the
 * name and holds a NEW reference to the SAME channel (shared offset — exactly
 * SCM_RIGHTS open-file-description semantics). This is the carrier for
 * Ladybird TransportPlan9's `Srv` attachments. Prints "PASS n/n".
 *
 * Checks:
 *   1. post a socketpair end to /srv/<name>; the LOCAL fd is then closed —
 *      the /srv entry alone must keep the channel alive (srv(3): "an entry
 *      holds a reference ... even if no process has the file open")
 *   2. fork+exec child: opens the name, removes the entry (receiver-removes,
 *      the TransportPlan9 protocol), bidirectional echo works
 *   3. EOF propagates: child close -> parent read returns 0
 *   4. the removed /srv entry is really gone
 *   5. regular-file fd via /srv shares the file offset with the poster
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

extern long n9_create(const char *, int, unsigned long);
extern long n9_pwrite(int, const void *, long, long long);
extern long n9_close(int);
extern long n9_remove(const char *);
extern int socketpair(int, int, int, int[2]);

static int npass;
static void ok(const char *what) { npass++; fprintf(stderr, "  ok %d: %s\n", npass, what); }
static void die(const char *what) {
	fprintf(stderr, "FAIL: %s (errno %d %s)\n", what, errno, strerror(errno));
	exit(1);
}

/* Post fd under /srv/<name>. srv(3): create the entry, write the decimal fd. */
static void srv_post(const char *name, int fd) {
	char path[64], num[16];
	snprintf(path, sizeof path, "/srv/%s", name);
	long sfd = n9_create(path, 1 /*OWRITE*/, 0600);
	if (sfd < 0) die("srv create");
	int n = snprintf(num, sizeof num, "%d", fd);
	if (n9_pwrite((int)sfd, num, n, -1) != n) die("srv write fd");
	n9_close((int)sfd);
}

static int child_main(const char *name) {
	char path[64], buf[64];
	snprintf(path, sizeof path, "/srv/%s", name);
	int fd = open(path, O_RDWR);
	if (fd < 0) die("child: open /srv entry");
	if (n9_remove(path) < 0) die("child: remove /srv entry");
	if (write(fd, "hello-from-child", 16) != 16) die("child: write");
	long r = read(fd, buf, sizeof buf);
	if (r != 17 || memcmp(buf, "hello-from-parent", 17) != 0) die("child: reply");
	close(fd);                            /* -> parent must see EOF */
	return 0;
}

int main(int argc, char **argv) {
	if (argc == 3 && strcmp(argv[1], "child") == 0)
		return child_main(argv[2]);

	char name[32], path[64], buf[64];

	/* 1: post one end of a socketpair, close it locally */
	int sp[2];
	if (socketpair(1, 1, 0, sp) < 0) die("socketpair");
	snprintf(name, sizeof name, "sfd.%d", getpid());
	srv_post(name, sp[1]);
	close(sp[1]);                          /* /srv holds the only reference now */
	ok("posted socketpair end to /srv, closed local fd");

	/* 2: child gets the channel purely via /srv */
	int kid = fork();
	if (kid < 0) die("fork");
	if (kid == 0) {
		char *cargv[] = { argv[0], "child", name, 0 };
		execv(argv[0], cargv);
		_exit(127);
	}
	long r = read(sp[0], buf, sizeof buf);
	if (r != 16 || memcmp(buf, "hello-from-child", 16) != 0) die("greeting");
	if (write(sp[0], "hello-from-parent", 17) != 17) die("reply");
	ok("bidirectional echo through the passed fd");

	/* 3: EOF propagation */
	r = read(sp[0], buf, sizeof buf);
	if (r != 0) die("expected EOF after child close");
	int st;
	if (waitpid(kid, &st, 0) != kid || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
		die("child failed");
	ok("EOF propagated on child close");
	close(sp[0]);

	/* 4: entry removed by the receiver */
	snprintf(path, sizeof path, "/srv/%s", name);
	if (open(path, O_RDWR) >= 0) die("/srv entry still present");
	ok("receiver removed the /srv entry");

	/* 5: regular file, shared offset */
	int ffd = open("/tmp/srvfd_gate.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (ffd < 0) die("data file");
	if (write(ffd, "0123456789", 10) != 10) die("data write");
	lseek(ffd, 0, SEEK_SET);
	snprintf(name, sizeof name, "sfo.%d", getpid());
	srv_post(name, ffd);
	if (read(ffd, buf, 2) != 2) die("read via original");     /* offset -> 2 */
	snprintf(path, sizeof path, "/srv/%s", name);
	int ffd2 = open(path, O_RDWR);
	if (ffd2 < 0) die("open posted file fd");
	if (read(ffd2, buf, 2) != 2) die("read via /srv fd");
	if (memcmp(buf, "23", 2) != 0) die("offset NOT shared (got wrong bytes)");
	ok("regular-file fd via /srv shares the offset");
	n9_remove(path);
	close(ffd); close(ffd2);
	unlink("/tmp/srvfd_gate.dat");

	printf("PASS %d/%d\n", npass, npass);
	return 0;
}
