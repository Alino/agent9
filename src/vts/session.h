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
#include "parser.h"
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
	Parser parser;
	LineEditor editor;

	/* Cast as QLock; using opaque size from the include order. */
	QLock lock;

	int keyin_wfd;
	int shellout_rfd;
	int rc_pid;
	int rc_alive;

	uchar keyin_buf[4096];
	int keyin_buf_len;
};

void session_init(Session *s, char *name, int rows, int cols);
int  session_spawn_rc(Session *s);
int  session_feed_keystrokes(Session *s, uchar *bytes, int n);

#endif
