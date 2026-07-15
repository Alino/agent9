/* clonepath.c — what does fd2path(2) report for a freshly-cloned /net fd?
 *
 * rust9's std rebuilds a TcpStream from a bare fd by asking fd2path which /net
 * connection directory the fd belongs to. That only works if a socket() fd (an
 * open of /net/tcp/clone) reports a path with the connection number in it.
 * If it instead reports the path as walked ("/net/tcp/clone"), there is no
 * number to parse and from_raw_fd cannot work before connect.
 *
 * Build/run on 9front (cc9): clonepath
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

extern long n9_open(const char *, int);
extern long n9_fd2path(int, char *, int);

int
main(void)
{
	char p[128];

	/* 1: raw open of the clone file */
	long fd = n9_open("/net/tcp/clone", 2 /*ORDWR*/);
	if (fd < 0) { printf("clone open FAILED\n"); return 1; }
	memset(p, 0, sizeof p);
	if (n9_fd2path((int)fd, p, sizeof p) < 0) printf("raw clone: fd2path FAILED\n");
	else                                      printf("raw clone fd2path: %s\n", p);
	close((int)fd);

	/* 2: through the socket(2) shim, which is what mio's new_socket calls */
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) { printf("socket() FAILED\n"); return 1; }
	memset(p, 0, sizeof p);
	if (n9_fd2path(s, p, sizeof p) < 0) printf("socket(): fd2path FAILED\n");
	else                                printf("socket() fd2path: %s\n", p);
	close(s);

	return 0;
}
