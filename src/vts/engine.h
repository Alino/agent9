/*
 * vts terminal engine — header.
 *
 * Thin bridge between libvterm (the VT100/ANSI state machine + screen model)
 * and the vts Buffer (cells.h). Replaces the old hand-written parser.c:
 * libvterm now owns parsing and the authoritative screen grid; its damage /
 * movecursor / settermprop / sb_pushline callbacks write into our Buffer so
 * celldiff.c / srv.c keep working unchanged.
 *
 * engine_feed() is a drop-in for parser_feed().
 */
#ifndef VTS_ENGINE_H
#define VTS_ENGINE_H

#include "compat.h"
#include "cells.h"

/* libvterm wants to send bytes back to the host (DA/DSR/cursor reports etc.).
 * vts routes those to the shell's stdin. The sink is supplied at init. */
typedef void (*EngineOut)(void *ctx, const uchar *bytes, int n);

typedef struct Engine Engine;
struct Engine {
	void *vt;        /* VTerm*        (opaque here to keep vterm.h out of session.h) */
	void *vs;        /* VTermScreen*  */
	Buffer *buf;
	EngineOut out;
	void *outctx;
};

void engine_init(Engine *e, Buffer *b, int rows, int cols, EngineOut out, void *outctx);
void engine_free(Engine *e);
void engine_feed(Engine *e, const uchar *bytes, int n);
void engine_resize(Engine *e, int rows, int cols);

#endif
