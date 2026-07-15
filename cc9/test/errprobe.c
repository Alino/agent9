/* what does 9front's errstr actually say, and does it name the file? */
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

extern const char *__n9_errstr_last(int *);

static void show(const char *what, const char *path) {
	errno = 0;
	int fd = open(path, O_RDONLY);
	int eno = 0;
	const char *s = __n9_errstr_last(&eno);
	printf("%-10s open(%s) -> fd=%d errno=%d errstr=\"%s\" (stashed eno=%d)\n",
	       what, path, fd, errno, s ? s : "(null)", eno);
}

int
main(void)
{
	show("alpha", "/n9gate-alpha-nonexistent");
	show("bravo", "/n9gate-bravo-nonexistent");
	show("permdir", "/adm/keys");   /* exists but not readable by glenda */
	return 0;
}
