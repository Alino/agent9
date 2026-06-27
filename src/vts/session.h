/*
 * vts session — shared types and functions.
 *
 * A Session owns one shell process and its cell buffer + parser.
 *
 * Plan 9 only.
 *
 * Note: we do NOT #include <thread.h> here, because the .c files that
 * include this header also include <thread.h> directly. Plan 9 system
 * headers have no include guards. We forward-declare what we need.
 */
#ifndef VTS_SESSION_H
#define VTS_SESSION_H

#include "compat.h"

#include "cells.h"
#include "engine.h"
#include "lined.h"

/* QLock from thread.h. Forward declaration is enough since we only use
 * it as a struct field (sizeof must be known when including session.h,
 * so we duplicate the field layout). Safer alternative: include the
 * header before this file. We rely on session.c and srv.c including
 * <thread.h> BEFORE this file. */
typedef struct QLock VTQLock;

enum { MAXSESS = 64 };

typedef struct Session Session;
struct Session {
	char *name;
	Buffer buf;
	Engine engine;
	LineEditor editor;

	/* Cast as QLock; using opaque size from the include order. */
	QLock lock;

	int keyin_wfd;
	int shellout_rfd;
	int rc_pid;
	int rc_alive;

	uchar keyin_buf[4096];
	int keyin_buf_len;

	/* lib9p File* handles owned by this session, kept so teardown can
	 * removefile() each exactly once (the only correct way to balance
	 * createfile's reference). Stored as void* to avoid pulling <9p.h>
	 * into this header. Index 0 = session dir, 1..4 = ctl/cons/cells/
	 * scroll. */
	void *files[5];

	/* Teardown handshake. kill_session and the reader proc both race
	 * to be the last to touch a killed session; "last one out frees"
	 * avoids both the use-after-free (freeing while the reader is mid
	 * read/parse) and the leak (never freeing). Both flags are written
	 * under `lock`; whoever sets its flag second sees the other's and
	 * calls session_free. onheap=0 for the static first session, whose
	 * struct must not be free()d. */
	int   killed;
	int   reader_done;
	uchar onheap;
};

void session_init(Session *s, char *name, int rows, int cols);
int  session_spawn_rc(Session *s);
int  session_feed_keystrokes(Session *s, uchar *bytes, int n);
void session_free(Session *s);

#endif
