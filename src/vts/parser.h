/*
 * vts VT100 parser — header.
 *
 * State machine ported from Paul Williams's DEC ANSI parser.
 * See wiki/concepts/vt100-parsing.md for the subset we support.
 *
 * The parser holds a small amount of state (current state, parameter
 * accumulator). It feeds output into a Buffer (cells.h) via the cellbuf_*
 * API.
 *
 * Inputs are bytes (uchar). UTF-8 decoding happens in the parser — we
 * collect continuation bytes until we have a full code point, then call
 * cellbuf_put with the resulting rune.
 */
#ifndef VTS_PARSER_H
#define VTS_PARSER_H

#include "compat.h"
#include "cells.h"

enum {
	PMAXPARAMS = 16,    /* max CSI params */
	PMAXINTERM = 4,     /* max intermediate bytes per CSI */
	PMAXOSC    = 256,   /* max OSC string length we'll buffer */
};

typedef struct Parser Parser;
struct Parser {
	int state;
	Buffer *buf;

	/* CSI parameter accumulation */
	int params[PMAXPARAMS];
	int has_param[PMAXPARAMS];   /* 1 if explicitly set (vs default) */
	int nparams;
	int csi_private;              /* '?' prefix flag */
	uchar interm[PMAXINTERM];
	int nintern;

	/* OSC accumulation */
	uchar osc[PMAXOSC];
	int osc_len;
	int osc_param;
	int osc_in_string;            /* 1 once we're past the ;text part */

	/* UTF-8 decoding */
	int utf_state;                /* bytes still needed to complete the rune */
	int utf_partial;
};

/* Parser states */
enum {
	STATE_GROUND,
	STATE_ESCAPE,
	STATE_ESCAPE_INTERM,
	STATE_CSI_ENTRY,
	STATE_CSI_PARAM,
	STATE_CSI_INTERM,
	STATE_CSI_IGNORE,
	STATE_OSC_STRING,
	STATE_SOS_PM_APC_STRING,
};

void parser_init(Parser *p, Buffer *b);
void parser_feed(Parser *p, const uchar *bytes, int n);

/* Convenience for tests/strings: feeds a null-terminated string */
void parser_feed_str(Parser *p, const char *s);

#endif
