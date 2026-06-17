/*
 * plan9_compat.c -- small POSIX functions APE's libc lacks, for the CPython
 * 9front port. Prototypes are declared in pyconfig.h so CPython sources see
 * them. Compiled into the interpreter (added to the object list).
 */
#define _POSIX_SOURCE
#define _BSD_EXTENSION
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

/*
 * setenv via Plan 9's /env. Plan 9 environment variables are files under
 * /env, so writing the value there is the native equivalent. APE has no
 * setenv(); CPython uses it for locale coercion.
 */
int
setenv(const char *name, const char *value, int overwrite)
{
	char path[1024];
	int fd, n;

	if (name == 0 || name[0] == 0 || strchr(name, '='))
		return -1;
	if (strlen(name) > sizeof(path) - 6)
		return -1;
	strcpy(path, "/env/");
	strcat(path, name);

	if (!overwrite) {
		fd = open(path, O_RDONLY);
		if (fd >= 0) {		/* already set, leave it */
			close(fd);
			return 0;
		}
	}
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	n = (int)strlen(value);
	if (write(fd, value, n) != n) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

int
unsetenv(const char *name)
{
	char path[1024];

	if (name == 0 || name[0] == 0 || strchr(name, '='))
		return -1;
	if (strlen(name) > sizeof(path) - 6)
		return -1;
	strcpy(path, "/env/");
	strcat(path, name);
	remove(path);
	return 0;
}
