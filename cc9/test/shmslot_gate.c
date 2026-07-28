/* shmslot_gate — a pool slot must NOT be recycled while another process still
 * references it.
 *
 * cc9 carves every anonymous shared buffer out of one big #g pool segment, so a
 * "buffer" is a byte range, not a kernel object: the kernel cannot tell us when
 * one goes unreferenced. The allocator used to decide that on its own, returning
 * a slot to its free-list the moment the CREATING process closed its fd — even
 * though the peer it had just exported the buffer to was still holding (and
 * about to map) exactly those pages. The next allocation then handed out the
 * same range, and two unrelated buffers silently shared memory.
 *
 * What that looked like: github.com's 404 page draws seven images. Every one of
 * them rendered as the SAME picture, each read through its own width — i.e. the
 * page appeared as horizontal streaks over one repeated image.
 *
 * This gate reproduces the mechanism directly, with no browser: export a buffer
 * to a child, drop the creator's fd, allocate again, and demand that the second
 * allocation does not land on the first buffer's bytes while the child still
 * holds it.
 *
 * Run on 9front:  shmslot_gate   -> "shmslot_gate N/N PASS"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/shm9.h>

static int pass, total;
static void ck(int ok, const char *what)
{
	total++;
	printf("%d %s: %s\n", total, what, ok ? "PASS" : "FAIL");
	fflush(stdout);
	if (ok) pass++;
}

#define BUFSZ (64 * 1024)
#define MARK_A 0xA5
#define MARK_B 0x5B

int main(void)
{
	char name[CC9_SHM_NAMELEN];
	unsigned long off = 0, len = 0;

	/* 1. Create a buffer, fill it with MARK_A, and export it (name+offset) the
	 *    way LibIPC ships one to a peer. */
	int fd = cc9_shm_create(BUFSZ);
	if (fd < 0) {
		printf("setup: cc9_shm_create: %s\nshmslot_gate 0/1 FAIL\n", strerror(errno));
		return 1;
	}
	unsigned char *a = mmap(0, BUFSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (a == MAP_FAILED) {
		printf("setup: mmap: %s\nshmslot_gate 0/1 FAIL\n", strerror(errno));
		return 1;
	}
	memset(a, MARK_A, BUFSZ);
	if (cc9_shm_export(fd, name, &off, &len) < 0) {
		printf("setup: cc9_shm_export: %s\nshmslot_gate 0/1 FAIL\n", strerror(errno));
		return 1;
	}
	ck(1, "create + export a pool buffer");

	/* 2. A DIFFERENT process imports it and keeps it, exactly as WebContent
	 *    holds a decoded bitmap after ImageDecoder has moved on. The pipes make
	 *    the ordering explicit rather than timing-dependent. */
	int to_child[2], to_parent[2];
	if (pipe(to_child) < 0 || pipe(to_parent) < 0) {
		printf("setup: pipe\nshmslot_gate %d/%d FAIL\n", pass, total);
		return 1;
	}
	pid_t kid = fork();
	if (kid == 0) {
		close(to_child[1]);
		close(to_parent[0]);
		int cfd = cc9_shm_import(name, off, BUFSZ);
		unsigned char *p = cfd < 0 ? MAP_FAILED
		                           : mmap(0, BUFSZ, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
		char ready = (p != MAP_FAILED && p[0] == MARK_A) ? 'y' : 'n';
		(void)write(to_parent[1], &ready, 1);   /* "I hold it, and it reads MARK_A" */
		char go;
		(void)read(to_child[0], &go, 1);        /* wait for the parent's second alloc */
		/* Still mine: nobody may have overwritten it. */
		char verdict = 'y';
		if (p == MAP_FAILED)
			verdict = 'n';
		else
			for (int i = 0; i < BUFSZ; i++)
				if (p[i] != MARK_A) { verdict = 'n'; break; }
		(void)write(to_parent[1], &verdict, 1);
		_exit(0);
	}
	close(to_child[0]);
	close(to_parent[1]);
	char ready = 0;
	(void)read(to_parent[0], &ready, 1);
	ck(ready == 'y', "a peer process imports and maps the buffer");

	/* 3. The creator is done with its own reference — but the peer is not. */
	munmap(a, BUFSZ);
	close(fd);

	/* 4. Allocate again, several times over, and write MARK_B everywhere. If any
	 *    allocation reuses the exported slot, the peer's pages change under it. */
	enum { NREALLOC = 4 };
	int fds[NREALLOC];
	unsigned char *ps[NREALLOC];
	int alloc_ok = 1;
	for (int i = 0; i < NREALLOC; i++) {
		fds[i] = cc9_shm_create(BUFSZ);
		if (fds[i] < 0) { alloc_ok = 0; break; }
		ps[i] = mmap(0, BUFSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
		if (ps[i] == MAP_FAILED) { alloc_ok = 0; break; }
		memset(ps[i], MARK_B, BUFSZ);
	}
	ck(alloc_ok, "the creator can keep allocating after closing its fd");

	char verdict = 0;
	(void)write(to_child[1], "g", 1);
	(void)read(to_parent[0], &verdict, 1);
	waitpid(kid, 0, 0);
	if (verdict != 'y')
		printf("   the peer's buffer was overwritten — a live slot was recycled\n");
	ck(verdict == 'y', "the peer's buffer still reads its own bytes");

	for (int i = 0; i < NREALLOC; i++)
		if (fds[i] >= 0) { munmap(ps[i], BUFSZ); close(fds[i]); }

	/* 5. The IN-FLIGHT window: a message names the buffer, and the sender drops
	 *    its last reference before the receiver has read that message. For that
	 *    stretch nobody holds the slot, yet it is still very much alive — this is
	 *    what SCM_RIGHTS covers on POSIX. Here the export itself must hold it.
	 *
	 *    Same shape as above but with the order reversed: export, release
	 *    everything, allocate over the top, and only THEN let the peer import. */
	char name2[CC9_SHM_NAMELEN];
	unsigned long off2 = 0, len2 = 0;
	int f2 = cc9_shm_create(BUFSZ);
	unsigned char *b = f2 < 0 ? MAP_FAILED
	                          : mmap(0, BUFSZ, PROT_READ | PROT_WRITE, MAP_SHARED, f2, 0);
	if (b == MAP_FAILED || cc9_shm_export(f2, name2, &off2, &len2) < 0) {
		ck(0, "create + export a second buffer");
	} else {
		memset(b, MARK_A, BUFSZ);
		ck(1, "create + export a second buffer");
		munmap(b, BUFSZ);
		close(f2);                        /* sender is done; the message is not read yet */

		for (int i = 0; i < NREALLOC; i++) {
			int nf = cc9_shm_create(BUFSZ);
			if (nf < 0) break;
			unsigned char *np = mmap(0, BUFSZ, PROT_READ | PROT_WRITE, MAP_SHARED, nf, 0);
			if (np == MAP_FAILED) { close(nf); break; }
			memset(np, MARK_B, BUFSZ);    /* would land on the in-flight buffer */
			munmap(np, BUFSZ);
			close(nf);
		}

		/* Only now does the receiver get to the message. */
		int ifd = cc9_shm_import(name2, off2, BUFSZ);
		unsigned char *ip = ifd < 0 ? MAP_FAILED
		                            : mmap(0, BUFSZ, PROT_READ | PROT_WRITE, MAP_SHARED, ifd, 0);
		int intact = ip != MAP_FAILED;
		if (intact)
			for (int i = 0; i < BUFSZ; i++)
				if (ip[i] != MARK_A) { intact = 0; break; }
		if (!intact)
			printf("   an in-flight buffer was reallocated before its receiver imported\n");
		ck(intact, "a buffer stays intact between export and the receiver's import");
		if (ip != MAP_FAILED) munmap(ip, BUFSZ);
		if (ifd >= 0) close(ifd);
	}

	/* 6. And once every reference is gone, slots must become reusable again —
	 *    the refcount must not turn into a permanent leak. */
	int again = cc9_shm_create(BUFSZ);
	ck(again >= 0, "allocation still works once every peer has exited");
	if (again >= 0)
		close(again);

	printf("shmslot_gate %d/%d %s\n", pass, total, pass == total ? "PASS" : "FAIL");
	return pass == total ? 0 : 1;
}
