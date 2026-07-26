/* srvfile_gate — a REGULAR FILE passed through /srv must still look like that
 * file to the receiver: same size from fstat, same bytes from pread at an offset.
 *
 * Why: cc9 has no SCM_RIGHTS, so LibIPC's plan9 transport passes descriptors by
 * posting them to /srv and having the peer reopen the entry. Ladybird leans on
 * that for three things that all mmap a RANGE of a received fd — the HTTP cache
 * body file, the freshly-cached body hand-off, and the JavaScript BYTECODE cache.
 * All three failed with "Invalid argument", which is MappedFile's bounds check
 * (size > file_size - offset) rejecting the mapping. That check only trips if the
 * receiver's fstat reports a SMALLER file than the sender's, so this gate asks
 * that question directly instead of inferring it from ladybird's logs.
 *
 * Run on 9front:  srvfile_gate   -> "srvfile_gate N/N PASS"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

extern long cc9_srv_post(const char *, int);
extern long cc9_srv_remove(const char *);

static int pass, total;
static void ck(int ok, const char *what)
{
	total++;
	printf("%d %s: %s\n", total, what, ok ? "PASS" : "FAIL");
	fflush(stdout);
	if (ok) pass++;
}

#define BODY 200000          /* comfortably past a page, like a real cache entry */
#define OFFSET 4096          /* cache entries put the body after a header */

int main(void)
{
	const char *path = "/tmp/srvfile_gate.dat";
	const char *srvname = "cc9srvfilegate";

	int w = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	if (w < 0) {
		printf("setup: create failed: %s\nsrvfile_gate 0/1 FAIL\n", strerror(errno));
		return 1;
	}
	static unsigned char buf[BODY];
	for (int i = 0; i < BODY; i++)
		buf[i] = (unsigned char)((i * 7 + (i >> 11)) & 0xff);
	if (write(w, buf, BODY) != BODY) {
		printf("setup: short write\nsrvfile_gate 0/1 FAIL\n");
		return 1;
	}
	close(w);

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		printf("setup: reopen failed\nsrvfile_gate 0/1 FAIL\n");
		return 1;
	}

	struct stat sender;
	int sender_ok = fstat(fd, &sender) == 0;
	ck(sender_ok && (long long)sender.st_size == BODY, "sender fstat reports the real size");

	(void)cc9_srv_remove(srvname);
	if (cc9_srv_post(srvname, fd) < 0) {
		printf("   cc9_srv_post: %s\n", strerror(errno));
		ck(0, "post the regular file to /srv");
		printf("srvfile_gate %d/%d FAIL\n", pass, total);
		return 1;
	}

	char srvpath[64];
	snprintf(srvpath, sizeof srvpath, "/srv/%s", srvname);
	int rfd = open(srvpath, O_RDONLY);
	(void)cc9_srv_remove(srvname);
	if (rfd < 0) {
		printf("   reopen /srv entry: %s\n", strerror(errno));
		ck(0, "receiver can reopen the /srv entry");
		printf("srvfile_gate %d/%d FAIL\n", pass, total);
		return 1;
	}
	ck(1, "receiver can reopen the /srv entry");

	/* THE question: does the reopened channel still know how big the file is?
	 * MappedFile refuses the mapping if it does not. */
	struct stat recv;
	int recv_ok = fstat(rfd, &recv) == 0;
	if (!recv_ok || (long long)recv.st_size != BODY)
		printf("   receiver size = %lld, expected %d\n",
		       recv_ok ? (long long)recv.st_size : -1LL, BODY);
	ck(recv_ok && (long long)recv.st_size == BODY, "receiver fstat reports the same size");

	/* And can it actually read a RANGE, which is what the mapping turns into. */
	static unsigned char got[4096];
	long r = pread(rfd, got, sizeof got, OFFSET);
	int bytes_ok = r == (long)sizeof got;
	if (bytes_ok)
		for (unsigned i = 0; i < sizeof got; i++)
			if (got[i] != buf[OFFSET + i]) { bytes_ok = 0; break; }
	if (r != (long)sizeof got)
		printf("   pread at %d returned %ld\n", OFFSET, r);
	ck(bytes_ok, "receiver preads the correct bytes at an offset");

	close(rfd);
	close(fd);
	unlink(path);

	printf("srvfile_gate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
