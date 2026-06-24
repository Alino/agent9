/*
 * vts 9P file server.
 *
 * Posts /srv/vts. Tree layout:
 *
 *   /                directory
 *   /ctl             daemon control (read: status; write: new/kill/list)
 *   /<sess>/         per session
 *   /<sess>/ctl      per-session control
 *   /<sess>/cons     bidirectional shell I/O
 *   /<sess>/cells    binary cell-diff stream
 *
 * Phase 6 adds multi-session via /ctl commands.
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
#include "celldiff.h"
#include "session.h"

Session *gsessions[MAXSESS];
int nsessions = 0;

/* Flag set by main.c via -n (no rc) for testing. */
int vts_spawn_rc_at_start = 1;

enum {
	Faux_root_ctl = 1,
	Faux_sess_ctl,
	Faux_sess_cons,
	Faux_sess_cells,
	Faux_sess_scroll,
};

/* Find session by name. Returns NULL if not found. */
static Session*
session_by_name(const char *name)
{
	int i;
	for(i = 0; i < nsessions; i++){
		if(gsessions[i] && strcmp(gsessions[i]->name, name) == 0)
			return gsessions[i];
	}
	return nil;
}

/* Get the session for a per-session file (cons, cells, ctl).
 * The file's parent's name is the session name. */
static Session*
sessof(File *f)
{
	File *parent;
	if(f == nil)
		return nil;
	parent = f->parent;
	if(parent == nil)
		return nil;
	return session_by_name(parent->name);
}

static char Estatus[1024];
static Srv vts_srv;

static void
fsread(Req *r)
{
	int aux = (uintptr)r->fid->file->aux;
	Session *s;
	long off, n;
	int i, p;

	switch(aux){
	case Faux_root_ctl:
		p = snprint(Estatus, sizeof Estatus,
			"vts %s; sessions=%d\n", VTBUILD, nsessions);
		for(i = 0; i < nsessions && p < (int)sizeof Estatus - 100; i++){
			Session *ss = gsessions[i];
			if(!ss) continue;
			p += snprint(Estatus + p, sizeof Estatus - p,
				"  %s: %dx%d cur=%d,%d rc=%d alive=%d\n",
				ss->name, ss->buf.rows, ss->buf.cols,
				ss->buf.cur_row, ss->buf.cur_col,
				ss->rc_pid, ss->rc_alive);
		}
		readstr(r, Estatus);
		respond(r, nil);
		return;

	case Faux_sess_ctl:
		s = sessof(r->fid->file);
		if(s == nil){
			respond(r, "no session");
			return;
		}
		snprint(Estatus, sizeof Estatus,
			"session %s; size=%dx%d; cursor=%d,%d; visible=%d; rc_alive=%d\n",
			s->name, s->buf.rows, s->buf.cols,
			s->buf.cur_row, s->buf.cur_col,
			(int)s->buf.cur_visible,
			s->rc_alive);
		readstr(r, Estatus);
		respond(r, nil);
		return;

	case Faux_sess_cons:
		s = sessof(r->fid->file);
		if(s == nil){
			respond(r, "no session");
			return;
		}
		off = r->ifcall.offset;
		if(off >= s->keyin_buf_len){
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		n = s->keyin_buf_len - off;
		if(n > (long)r->ifcall.count)
			n = r->ifcall.count;
		memmove(r->ofcall.data, s->keyin_buf + off, n);
		r->ofcall.count = n;
		respond(r, nil);
		return;

	case Faux_sess_cells:
		s = sessof(r->fid->file);
		if(s == nil){
			respond(r, "no session");
			return;
		}
		if(r->ifcall.offset != 0){
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		qlock(&s->lock);
		{
			int max = r->ifcall.count;
			int written = celldiff_encode(&s->buf,
				(uchar*)r->ofcall.data, max);
			r->ofcall.count = written;
		}
		qunlock(&s->lock);
		respond(r, nil);
		return;

	case Faux_sess_scroll:
		s = sessof(r->fid->file);
		if(s == nil){
			respond(r, "no session");
			return;
		}
		/* Read scrollback as UTF-8 lines separated by '\n'.
		 * Offset-aware: caller can seek into the stream. */
		qlock(&s->lock);
		{
			int total = cellbuf_scrollback_count(&s->buf);
			long off = r->ifcall.offset;
			long want = r->ifcall.count;
			long produced = 0;
			char *out = (char*)r->ofcall.data;
			int i;

			/* Render lines into a temporary aware of where we are
			 * in the byte stream. We iterate one line at a time. */
			long pos = 0;
			char *linebuf = (char*)malloc(SCROLL_LINE_BYTES);
			for(i = 0; i < total && produced < want; i++){
				cellbuf_scrollback_lines(&s->buf, i, 1, linebuf);
				int llen = strlen(linebuf);
				/* This line spans bytes [pos, pos+llen+1) where +1 is '\n' */
				long line_end = pos + llen + 1;
				if(line_end <= off){
					pos = line_end;
					continue;
				}
				/* Some part of this line is in our window. */
				long start_in_line = (off > pos) ? off - pos : 0;
				long to_copy = (long)(llen + 1) - start_in_line;
				if(to_copy > want - produced) to_copy = want - produced;
				const char *src;
				if(start_in_line < llen){
					src = linebuf + start_in_line;
					long cn = llen - start_in_line;
					if(cn > to_copy) cn = to_copy;
					memmove(out + produced, src, cn);
					produced += cn;
					to_copy -= cn;
				}
				if(to_copy > 0 && produced < want){
					out[produced++] = '\n';
				}
				pos = line_end;
			}
			free(linebuf);
			r->ofcall.count = produced;
		}
		qunlock(&s->lock);
		respond(r, nil);
		return;
	}

	respond(r, "vts: unknown file");
}

/* Forward decl */
static int create_session(const char *name, int spawn_rc);
static int kill_session(const char *name);

static void
fswrite(Req *r)
{
	int aux = (uintptr)r->fid->file->aux;
	Session *s;
	long n;
	char cmd[256];
	char *argv[8];
	int argc;

	n = r->ifcall.count;

	switch(aux){
	case Faux_root_ctl:
		/* Phase 6: parse commands.
		 *   new <name>      create session
		 *   kill <name>     destroy session  */
		if(n >= (long)sizeof cmd){
			respond(r, "command too long");
			return;
		}
		memmove(cmd, r->ifcall.data, n);
		cmd[n] = 0;
		/* strip trailing newline */
		while(n > 0 && (cmd[n-1] == '\n' || cmd[n-1] == '\r')){
			cmd[--n] = 0;
		}
		argc = tokenize(cmd, argv, nelem(argv));
		if(argc == 0){
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
			return;
		}
		if(strcmp(argv[0], "new") == 0 && argc >= 2){
			if(create_session(argv[1], 1) < 0){
				respond(r, "create_session failed");
				return;
			}
		} else if(strcmp(argv[0], "kill") == 0 && argc >= 2){
			if(kill_session(argv[1]) < 0){
				respond(r, "kill_session failed");
				return;
			}
		} else {
			respond(r, "unknown command");
			return;
		}
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;

	case Faux_sess_ctl:
		/* Per-session control commands:
		 *   edit on    enable in-process line editor
		 *   edit off   disable
		 *   size R C   resize the cell buffer (rows, cols)
		 */
		s = sessof(r->fid->file);
		if(s != nil && n > 0){
			char cmd[64];
			int cn = n < (long)sizeof cmd - 1 ? n : (long)sizeof cmd - 1;
			memmove(cmd, r->ifcall.data, cn);
			cmd[cn] = 0;
			while(cn > 0 && (cmd[cn-1] == '\n' || cmd[cn-1] == '\r'))
				cmd[--cn] = 0;
			if(strcmp(cmd, "edit on") == 0)
				lined_set_enabled(&s->editor, 1);
			else if(strcmp(cmd, "edit off") == 0)
				lined_set_enabled(&s->editor, 0);
			else if(strcmp(cmd, "redraw") == 0){
				/* Force re-emit of every cell on the next cells
				 * read. Used by vtwin when it (re)gains focus or
				 * suspects its display is stale. */
				qlock(&s->lock);
				s->buf.all_dirty = 1;
				qunlock(&s->lock);
			}
			else if(strncmp(cmd, "size ", 5) == 0){
				char *fields[3];
				char tmp[64];
				strncpy(tmp, cmd, sizeof tmp - 1);
				tmp[sizeof tmp - 1] = 0;
				int nf = tokenize(tmp, fields, 3);
				if(nf == 3){
					int newrows = atoi(fields[1]);
					int newcols = atoi(fields[2]);
					if(newrows > 0 && newcols > 0 &&
					   newrows < 1000 && newcols < 1000){
						qlock(&s->lock);
						cellbuf_resize(&s->buf, newrows, newcols);
						qunlock(&s->lock);
					}
				}
			}
		}
		r->ofcall.count = n;
		respond(r, nil);
		return;

	case Faux_sess_cons:
		s = sessof(r->fid->file);
		if(s == nil){
			respond(r, "no session");
			return;
		}
		/*
		 * If line editor is enabled, intercept the byte stream:
		 *   - run each byte through lined_feed_byte
		 *   - on LINEED_COMPLETE: flush completed line + '\n' to rc
		 *   - on LINEED_CONSUMED: emit any redraw bytes to the cell buffer
		 *   - on LINEED_PASSTHROUGH: forward raw
		 * If editor disabled, write straight through.
		 */
		if(s->editor.enabled){
			long i;
			for(i = 0; i < n; i++){
				uchar *line_out;
				int line_len;
				uchar redraw[256];
				int rl = 0;
				int rc = lined_feed_byte(&s->editor,
					((uchar*)r->ifcall.data)[i],
					&line_out, &line_len,
					redraw, sizeof redraw, &rl);
				if(rl > 0){
					qlock(&s->lock);
					parser_feed(&s->parser, redraw, rl);
					qunlock(&s->lock);
				}
				if(rc == LINEED_COMPLETE){
					if(s->rc_alive){
						session_feed_keystrokes(s, line_out, line_len);
						uchar nl = '\n';
						session_feed_keystrokes(s, &nl, 1);
					} else {
						qlock(&s->lock);
						parser_feed(&s->parser, line_out, line_len);
						parser_feed(&s->parser, (uchar*)"\r\n", 2);
						qunlock(&s->lock);
					}
				} else if(rc == LINEED_PASSTHROUGH){
					if(s->rc_alive){
						uchar b = ((uchar*)r->ifcall.data)[i];
						session_feed_keystrokes(s, &b, 1);
					}
				}
			}
		} else if(s->rc_alive){
			session_feed_keystrokes(s, (uchar*)r->ifcall.data, n);
		} else {
			qlock(&s->lock);
			parser_feed(&s->parser, (uchar*)r->ifcall.data, n);
			qunlock(&s->lock);
		}
		/* Record recent input for read-back via fsread cons. Bound
		 * against keyin_buf: a single write larger than the buffer
		 * (e.g. a big paste from vtwin) must NOT memmove past its end
		 * — that overflows the Session struct and corrupts the heap,
		 * faulting later. Keep only the most recent bytes. */
		if(n >= (long)sizeof s->keyin_buf){
			memmove(s->keyin_buf,
				(uchar*)r->ifcall.data + (n - (long)sizeof s->keyin_buf),
				sizeof s->keyin_buf);
			s->keyin_buf_len = sizeof s->keyin_buf;
		} else {
			if(s->keyin_buf_len + n > (int)sizeof s->keyin_buf)
				s->keyin_buf_len = 0;
			memmove(s->keyin_buf + s->keyin_buf_len, r->ifcall.data, n);
			s->keyin_buf_len += n;
		}
		r->ofcall.count = n;
		respond(r, nil);
		return;

	case Faux_sess_cells:
		respond(r, "cells is read-only");
		return;
	}

	respond(r, "vts: unknown file");
}

/* Add a session's files to the tree. */
static int
add_session_files(Session *s)
{
	File *sess_dir;
	void *axp;

	sess_dir = createfile(vts_srv.tree->root, s->name, "vts", DMDIR|0555, nil);
	if(sess_dir == nil){
		fprint(2, "vts: createfile %s failed: %r\n", s->name);
		return -1;
	}

	/* Keep every File* createfile hands back: each carries one
	 * reference that teardown must release with exactly one removefile.
	 * (The previous code discarded these and re-derived them with
	 * walkfile at teardown — which takes an EXTRA ref and, combined
	 * with child-removal dropping the parent's ref, corrupted lib9p's
	 * accounting and faulted freefile() after a handful of sessions.) */
	s->files[0] = sess_dir;

	axp = (void*)(uintptr)Faux_sess_ctl;
	s->files[1] = createfile(sess_dir, "ctl", "vts", 0666, axp);

	axp = (void*)(uintptr)Faux_sess_cons;
	s->files[2] = createfile(sess_dir, "cons", "vts", 0666, axp);

	axp = (void*)(uintptr)Faux_sess_cells;
	s->files[3] = createfile(sess_dir, "cells", "vts", 0444, axp);

	axp = (void*)(uintptr)Faux_sess_scroll;
	s->files[4] = createfile(sess_dir, "scroll", "vts", 0444, axp);

	return 0;
}

static int
remove_session_files(Session *s)
{
	int i;

	if(s->files[0] == nil)
		return -1;
	/* Children first, then the directory. removefile unlinks the file
	 * and drops the createfile reference; if a client still has the
	 * file open, lib9p keeps the (now unlinked) File alive until the
	 * last fid clunks, so this is safe even mid-use. No walkfile. */
	for(i = 1; i < 5; i++){
		if(s->files[i] != nil){
			removefile((File*)s->files[i]);
			s->files[i] = nil;
		}
	}
	removefile((File*)s->files[0]);
	s->files[0] = nil;
	return 0;
}

static void
spawn_rc_proc(void *arg)
{
	Session *s = (Session*)arg;
	if(session_spawn_rc(s) < 0)
		fprint(2, "vts: warning: could not spawn rc for %s\n", s->name);
	threadexits(nil);
}

static int
create_session(const char *name, int spawn_rc)
{
	Session *s;
	int slot;

	if(session_by_name(name) != nil)
		return -1;
	for(slot = 0; slot < MAXSESS; slot++)
		if(gsessions[slot] == nil) break;
	if(slot >= MAXSESS)
		return -1;

	s = (Session*)malloc(sizeof(Session));
	if(s == nil)
		return -1;
	memset(s, 0, sizeof(Session));
	session_init(s, (char*)name, 24, 80);
	s->onheap = 1;	/* heap-allocated; teardown may free(s) */
	gsessions[slot] = s;
	if(slot >= nsessions) nsessions = slot + 1;

	if(add_session_files(s) < 0){
		session_free(s);
		gsessions[slot] = nil;
		return -1;
	}

	if(spawn_rc){
		proccreate(spawn_rc_proc, s, 32*1024);
	}

	fprint(2, "vts: created session %s (slot %d)\n", name, slot);
	return 0;
}

static int
kill_session(const char *name)
{
	int i;
	Session *s = nil;
	for(i = 0; i < nsessions; i++){
		if(gsessions[i] && strcmp(gsessions[i]->name, name) == 0){
			s = gsessions[i];
			gsessions[i] = nil;
			break;
		}
	}
	if(s == nil) return -1;

	if(s->rc_alive && s->rc_pid > 0){
		char path[64];
		int fd;
		snprint(path, sizeof path, "/proc/%d/note", s->rc_pid);
		fd = open(path, OWRITE);
		if(fd >= 0){
			fprint(fd, "kill");
			close(fd);
		}
	}

	/* Reclaim the keyin write end. (The reader proc closes the shellout
	 * read end when rc's death makes its read return EOF.) Killing rc
	 * via the note above also makes that happen. Marking rc_alive=0 +
	 * keyin_wfd=-1 keeps any late session_feed_keystrokes a no-op. */
	s->rc_alive = 0;
	if(s->keyin_wfd >= 0){
		close(s->keyin_wfd);
		s->keyin_wfd = -1;
	}

	remove_session_files(s);
	fprint(2, "vts: killed session %s\n", name);

	/* Teardown handshake (see session.c). The reader proc may still be
	 * unwinding out of its blocking read on this session. Mark it killed;
	 * if the reader has already finished, we're the last user and free
	 * now. Otherwise the reader frees when it exits and sees `killed`.
	 * gsessions[i] is already nil and the files are removed, so no 9P
	 * request can reach `s` after this point. */
	{
		int reader_done;
		qlock(&s->lock);
		s->killed = 1;
		reader_done = s->reader_done;
		qunlock(&s->lock);
		if(reader_done)
			session_free(s);
	}
	return 0;
}

static void
fsstart(Srv *srv)
{
	Session *s;
	USED(srv);
	if(nsessions == 0)
		return;
	s = gsessions[0];
	if(vts_spawn_rc_at_start && !s->rc_alive){
		proccreate(spawn_rc_proc, s, 32*1024);
	}
}

static Srv vts_srv = {
	.read = fsread,
	.write = fswrite,
	.start = fsstart,
};

static Session sess1_storage;

void
srvinit(int spawn_rc)
{
	File *root;
	void *axp;

	vts_spawn_rc_at_start = spawn_rc;

	/* Initialize the FIRST session statically. Additional sessions
	 * are heap-allocated by create_session. */
	session_init(&sess1_storage, "1", 24, 80);
	gsessions[0] = &sess1_storage;
	nsessions = 1;

	vts_srv.tree = alloctree("vts", "vts", DMDIR|0555, nil);
	root = vts_srv.tree->root;

	axp = (void*)(uintptr)Faux_root_ctl;
	createfile(root, "ctl", "vts", 0666, axp);

	add_session_files(&sess1_storage);
}

void
srvstart(void)
{
	threadpostsrv(&vts_srv, "vts");
	print("vts: posted at /srv/vts\n");
}
