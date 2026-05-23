/*
 * vts session management — Phase 1.2 + 1.3
 *
 * For each session, vts:
 *   1) creates two pipes (keyin and shellout)
 *   2) uses procrfork (libthread-aware fork) to spawn rc with stdin=keyin_read,
 *      stdout/stderr=shellout_write
 *   3) starts a proc (separate kernel-scheduled process) that drains shellout
 *      into the parser
 *
 * Using proccreate (not threadcreate) for the reader because reads from a
 * pipe are blocking syscalls that pin the kernel proc; we need the 9P srv
 * worker to keep handling requests in parallel.
 */
#include "compat.h"
#include <auth.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#include "dat.h"
#include "fns.h"
#include "cells.h"
#include "parser.h"
#include "session.h"

/* Reader proc: blocks on shellout pipe, feeds parser, keeps doing it
 * forever or until rc dies. */
static void
shellout_reader(void *arg)
{
	Session *s = (Session*)arg;
	uchar buf[4096];
	long n;

	fprint(2, "vts: reader proc started, fd=%d\n", s->shellout_rfd);
	for(;;){
		n = read(s->shellout_rfd, buf, sizeof buf);
		fprint(2, "vts: reader read n=%ld\n", n);
		if(n <= 0){
			s->rc_alive = 0;
			fprint(2, "vts: rc died (read returned %ld)\n", n);
			break;
		}
		qlock(&s->lock);
		parser_feed(&s->parser, buf, (int)n);
		qunlock(&s->lock);
	}
	threadexits(nil);
}

int
session_spawn_rc(Session *s)
{
	int pkeyin[2];
	int pshellout[2];
	int pid;

	if(pipe(pkeyin) < 0){
		fprint(2, "vts: pipe(keyin): %r\n");
		return -1;
	}
	if(pipe(pshellout) < 0){
		fprint(2, "vts: pipe(shellout): %r\n");
		close(pkeyin[0]); close(pkeyin[1]);
		return -1;
	}

	fprint(2, "vts: pipes ready: keyin=%d,%d shellout=%d,%d\n",
		pkeyin[0], pkeyin[1], pshellout[0], pshellout[1]);

	switch(pid = rfork(RFPROC|RFFDG|RFNOTEG|RFENVG)){
	case -1:
		fprint(2, "vts: rfork: %r\n");
		close(pkeyin[0]); close(pkeyin[1]);
		close(pshellout[0]); close(pshellout[1]);
		return -1;

	case 0:
		/* Child. */
		dup(pkeyin[0], 0);
		dup(pshellout[1], 1);
		dup(pshellout[1], 2);
		/* Close ALL inherited fds except 0,1,2 (we just wrote those).
		 * Without this, rc inherits the 9P srv connection fd and
		 * mxio/shell relay file descriptors, which can confuse lib9p
		 * and prevent the srv from being torn down on rc exit. */
		{
			int i;
			for(i = 3; i < 100; i++)
				close(i);
		}
		putenv("vts", s->name);
		putenv("prompt", "vts% \t");
		execl("/bin/rc", "rc", "-i", nil);
		fprint(2, "vts: exec rc: %r\n");
		exits("exec");
	}

	/* Parent. */
	s->rc_pid = pid;
	s->rc_alive = 1;
	close(pkeyin[0]);
	close(pshellout[1]);
	s->keyin_wfd = pkeyin[1];
	s->shellout_rfd = pshellout[0];

	fprint(2, "vts: rc forked pid=%d, starting reader proc\n", pid);

	/* proccreate, not threadcreate: read() is a blocking syscall, so
	 * we need a separate kernel process to avoid blocking the srv. */
	proccreate(shellout_reader, s, 32*1024);

	return 0;
}

int
session_feed_keystrokes(Session *s, uchar *bytes, int n)
{
	if(!s->rc_alive || s->keyin_wfd < 0)
		return -1;
	return write(s->keyin_wfd, bytes, n);
}

void
session_init(Session *s, char *name, int rows, int cols)
{
	s->name = strdup(name);
	cellbuf_init(&s->buf, rows, cols);
	parser_init(&s->parser, &s->buf);
	lined_init(&s->editor);
	/* Default: line editor enabled. mxio's cell-grid terminal (vtwin)
	 * is the only client we ship; it expects local echo + line editing
	 * because rc/9front isn't a tty and won't echo or cook input itself.
	 * vts-attach for headless remote can disable via "edit off\n" on
	 * /sess/ctl. */
	lined_set_enabled(&s->editor, 1);
	s->keyin_wfd = -1;
	s->shellout_rfd = -1;
	s->rc_pid = -1;
	s->rc_alive = 0;
	s->keyin_buf_len = 0;
}
