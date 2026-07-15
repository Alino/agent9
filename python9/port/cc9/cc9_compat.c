/* cc9_compat.c — the few libc gaps CPython needs that cc9's runtime doesn't
 * cover. Port-local; promote into cc9/runtime if another port wants them. */

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* CPython's bootstrap_hash falls back getrandom -> getentropy -> /dev/urandom;
 * Plan 9 has /dev/random only, so provide getentropy over it. */
int
getentropy(void *buf, unsigned long len)
{
	char *p = buf;
	int fd, n;

	if (len > 256) {
		errno = EIO;
		return -1;
	}
	fd = open("/dev/random", O_RDONLY);
	if (fd < 0)
		return -1;
	while (len > 0) {
		n = read(fd, p, len);
		if (n <= 0) {
			close(fd);
			errno = EIO;
			return -1;
		}
		p += n;
		len -= n;
	}
	close(fd);
	return 0;
}
