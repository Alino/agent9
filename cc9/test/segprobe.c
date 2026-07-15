/* segprobe — pinpoint why a fork+exec'd child can't segattach a #g segment
 * its parent created and attached. Prints raw kernel errstr at each step. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/shm9.h>
#include <sys/wait.h>

extern long n9_fd2path(int, char *, int);
extern long n9_errstr(char *, int);
extern void *n9_segattach(unsigned long attr, const char *cls, void *va, unsigned long len);

static void estr(const char *tag) {
	char e[128];
	e[0] = 0;
	n9_errstr(e, sizeof e);
	fprintf(stderr, "%s: errstr=[%s]\n", tag, e);
	n9_errstr(e, sizeof e);   /* swap back so the next read isn't clobbered */
}

int main(int argc, char **argv) {
	if (argc == 3 && strcmp(argv[1], "child") == 0) {
		const char *name = argv[2];
		fprintf(stderr, "child: pid=%d name=[%s]\n", getpid(), name);
		int fd = cc9_shm_import(name, 0, 0);
		fprintf(stderr, "child: import fd=%d\n", fd);
		if (fd < 0) { estr("child import"); return 1; }
		char path[128];
		long pr = n9_fd2path(fd, path, sizeof path);
		fprintf(stderr, "child: fd2path ret=%ld path=[%s]\n", pr, path);
		void *va = n9_segattach(0, name, 0, 0);          /* no SG_CEXEC: bare */
		fprintf(stderr, "child: segattach(0) -> %p\n", va);
		if ((long)va <= 0) estr("child segattach bare");
		if ((long)va <= 0) {
			va = n9_segattach(0100 /*SG_CEXEC*/, name, 0, 0);
			fprintf(stderr, "child: segattach(SG_CEXEC) -> %p\n", va);
			if ((long)va <= 0) estr("child segattach cexec");
		}
		if ((long)va > 0) {
			fprintf(stderr, "child: first bytes: %02x %02x\n",
			        ((unsigned char *)va)[0], ((unsigned char *)va)[1]);
			return 0;
		}
		return 1;
	}

	int fd = cc9_shm_create(1 << 20);
	fprintf(stderr, "parent: create fd=%d\n", fd);
	if (fd < 0) { estr("create"); return 1; }
	char name[CC9_SHM_NAMELEN];
	unsigned long off, len;
	if (cc9_shm_export(fd, name, &off, &len) < 0) { estr("export"); return 1; }
	fprintf(stderr, "parent: name=[%s] len=0x%lx\n", name, len);
	unsigned char *p = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	fprintf(stderr, "parent: mmap -> %p\n", (void *)p);
	if (p == MAP_FAILED) { estr("parent mmap"); return 1; }
	p[0] = 0xAB; p[1] = 0xCD;

	int kid = fork();
	if (kid == 0) {
		char *cargv[] = { argv[0], "child", name, 0 };
		execv(argv[0], cargv);
		_exit(127);
	}
	int st;
	waitpid(kid, &st, 0);
	fprintf(stderr, "parent: child status=%d\n", st);
	printf("PROBE-DONE\n");
	return 0;
}
