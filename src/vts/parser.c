/*
 * vts VT100 parser.
 *
 * Implements the Paul Williams VT500-series state machine, subset documented
 * in wiki/concepts/vt100-parsing.md. Drives a Buffer via cellbuf_*.
 *
 * Phase 2.2: printable + C0 control characters + ESC dispatches.
 * Phase 2.3 (next): CSI sequences (SGR, cursor, erase).
 *
 * Test driver: test/test_parser.c
 */
#include "compat.h"
#include "parser.h"
#include "cells.h"

static void reset_params(Parser *p);
static void csi_dispatch(Parser *p, uchar final);
static void esc_dispatch(Parser *p, uchar final);
static void osc_dispatch(Parser *p);
static void put_rune(Parser *p, int rune);

void
parser_init(Parser *p, Buffer *b)
{
	p->state = STATE_GROUND;
	p->buf = b;
	p->nparams = 0;
	p->csi_private = 0;
	p->nintern = 0;
	p->osc_len = 0;
	p->osc_param = 0;
	p->osc_in_string = 0;
	p->utf_state = 0;
	p->utf_partial = 0;
	reset_params(p);
}

static void
reset_params(Parser *p)
{
	int i;
	for(i = 0; i < PMAXPARAMS; i++){
		p->params[i] = 0;
		p->has_param[i] = 0;
	}
	p->nparams = 0;
	p->csi_private = 0;
	p->nintern = 0;
}

/*
 * UTF-8 decoder. Feed one byte; if the rune is complete, returns the rune;
 * else returns -1 (still accumulating). Returns -2 on invalid sequence.
 */
static int
utf8_step(Parser *p, uchar b)
{
	int r;

	if(p->utf_state == 0){
		if((b & 0x80) == 0)
			return b;                  /* ASCII */
		if((b & 0xE0) == 0xC0){
			p->utf_partial = b & 0x1F;
			p->utf_state = 1;
			return -1;
		}
		if((b & 0xF0) == 0xE0){
			p->utf_partial = b & 0x0F;
			p->utf_state = 2;
			return -1;
		}
		if((b & 0xF8) == 0xF0){
			p->utf_partial = b & 0x07;
			p->utf_state = 3;
			return -1;
		}
		/* Invalid leading byte — treat as Latin-1 fallback */
		return b;
	}

	if((b & 0xC0) != 0x80){
		/* Expected continuation, got something else. Reset, treat as new. */
		p->utf_state = 0;
		p->utf_partial = 0;
		if((b & 0x80) == 0)
			return b;
		return -2;
	}

	p->utf_partial = (p->utf_partial << 6) | (b & 0x3F);
	p->utf_state--;
	if(p->utf_state == 0){
		r = p->utf_partial;
		p->utf_partial = 0;
		return r;
	}
	return -1;
}

static void
put_rune(Parser *p, int rune)
{
	cellbuf_put(p->buf, rune);
	cellbuf_advance(p->buf);
}

/*
 * Handle a C0 control character in Ground state. Returns 1 if it was a
 * control, 0 if the caller should treat it as printable.
 */
static int
handle_c0(Parser *p, uchar b)
{
	switch(b){
	case 0x07: /* BEL */
		/* TODO: visual bell hook for mxio client */
		return 1;
	case 0x08: /* BS */
		cellbuf_backspace(p->buf);
		return 1;
	case 0x09: /* HT */
		cellbuf_tab(p->buf);
		return 1;
	case 0x0A: /* LF */
	case 0x0B: /* VT */
	case 0x0C: /* FF */
		cellbuf_newline(p->buf);
		return 1;
	case 0x0D: /* CR */
		cellbuf_carriage_return(p->buf);
		return 1;
	case 0x1B: /* ESC */
		p->state = STATE_ESCAPE;
		reset_params(p);
		return 1;
	case 0x7F: /* DEL */
		return 1;
	}
	if(b < 0x20)
		return 1;  /* drop other C0 controls */
	return 0;
}

/*
 * Parse one byte. The state machine dispatches based on (state, byte).
 * This is the Williams parser, simplified to our subset.
 */
static void
step(Parser *p, uchar b)
{
	int rune;

	switch(p->state){

	case STATE_GROUND:
		/* In Ground, ESC and C0 are immediate; everything else goes
		 * through UTF-8 decoder and lands in the cell buffer. */
		if(b == 0x1B){
			p->state = STATE_ESCAPE;
			reset_params(p);
			return;
		}
		if(b < 0x20 || b == 0x7F){
			handle_c0(p, b);
			return;
		}
		rune = utf8_step(p, b);
		if(rune >= 0)
			put_rune(p, rune);
		return;

	case STATE_ESCAPE:
		/* After ESC, decide: another ESC ('[' = CSI, ']' = OSC, etc.) */
		if(b == 0x5B){           /* '[' — CSI */
			p->state = STATE_CSI_ENTRY;
			reset_params(p);
			return;
		}
		if(b == 0x5D){           /* ']' — OSC */
			p->state = STATE_OSC_STRING;
			p->osc_len = 0;
			p->osc_param = 0;
			p->osc_in_string = 0;
			return;
		}
		if(b == 0x50 || b == 0x58 || b == 0x5E || b == 0x5F){
			/* DCS / SOS / PM / APC — swallow until ST */
			p->state = STATE_SOS_PM_APC_STRING;
			return;
		}
		if(b >= 0x20 && b <= 0x2F){
			p->state = STATE_ESCAPE_INTERM;
			return;
		}
		if(b >= 0x30 && b <= 0x7E){
			/* ESC final byte: dispatch. */
			esc_dispatch(p, b);
			p->state = STATE_GROUND;
			return;
		}
		if(b == 0x1B){
			/* ESC ESC — restart */
			reset_params(p);
			return;
		}
		/* Anything else: back to ground */
		p->state = STATE_GROUND;
		return;

	case STATE_ESCAPE_INTERM:
		if(b >= 0x30 && b <= 0x7E){
			esc_dispatch(p, b);
			p->state = STATE_GROUND;
			return;
		}
		if(b == 0x1B){
			p->state = STATE_ESCAPE;
			reset_params(p);
			return;
		}
		return;

	case STATE_CSI_ENTRY:
		if(b == 0x3F){           /* '?' — private mode marker */
			p->csi_private = 1;
			p->state = STATE_CSI_PARAM;
			return;
		}
		if(b >= 0x30 && b <= 0x39){  /* digit */
			p->params[0] = b - 0x30;
			p->has_param[0] = 1;
			p->nparams = 1;
			p->state = STATE_CSI_PARAM;
			return;
		}
		if(b == 0x3B){           /* ';' — empty first param */
			p->nparams = 2;
			p->has_param[0] = 0;
			p->state = STATE_CSI_PARAM;
			return;
		}
		if(b >= 0x40 && b <= 0x7E){
			/* Immediate final, no params */
			csi_dispatch(p, b);
			p->state = STATE_GROUND;
			return;
		}
		if(b >= 0x20 && b <= 0x2F){
			if(p->nintern < PMAXINTERM)
				p->interm[p->nintern++] = b;
			p->state = STATE_CSI_INTERM;
			return;
		}
		p->state = STATE_CSI_IGNORE;
		return;

	case STATE_CSI_PARAM:
		if(b >= 0x30 && b <= 0x39){
			if(p->nparams == 0) p->nparams = 1;
			p->params[p->nparams - 1] = p->params[p->nparams - 1] * 10 + (b - 0x30);
			p->has_param[p->nparams - 1] = 1;
			return;
		}
		if(b == 0x3B){
			if(p->nparams < PMAXPARAMS){
				p->nparams++;
				p->params[p->nparams - 1] = 0;
				p->has_param[p->nparams - 1] = 0;
			}
			return;
		}
		if(b >= 0x20 && b <= 0x2F){
			if(p->nintern < PMAXINTERM)
				p->interm[p->nintern++] = b;
			p->state = STATE_CSI_INTERM;
			return;
		}
		if(b >= 0x40 && b <= 0x7E){
			csi_dispatch(p, b);
			p->state = STATE_GROUND;
			return;
		}
		p->state = STATE_CSI_IGNORE;
		return;

	case STATE_CSI_INTERM:
		if(b >= 0x20 && b <= 0x2F){
			if(p->nintern < PMAXINTERM)
				p->interm[p->nintern++] = b;
			return;
		}
		if(b >= 0x40 && b <= 0x7E){
			csi_dispatch(p, b);
			p->state = STATE_GROUND;
			return;
		}
		p->state = STATE_CSI_IGNORE;
		return;

	case STATE_CSI_IGNORE:
		if(b >= 0x40 && b <= 0x7E)
			p->state = STATE_GROUND;
		return;

	case STATE_OSC_STRING:
		if(b == 0x07){           /* BEL terminator */
			osc_dispatch(p);
			p->state = STATE_GROUND;
			return;
		}
		if(b == 0x1B){
			/* ESC \\ (ST) — peek for backslash */
			/* Simplest impl: end on ESC alone, which we then re-enter as
			 * Escape state. The backslash arriving next will be dispatched
			 * harmlessly. To be safer, we treat ESC inside OSC as end. */
			osc_dispatch(p);
			p->state = STATE_ESCAPE;
			reset_params(p);
			return;
		}
		if(p->osc_in_string){
			if(p->osc_len < PMAXOSC - 1)
				p->osc[p->osc_len++] = b;
		} else {
			if(b >= 0x30 && b <= 0x39){
				p->osc_param = p->osc_param * 10 + (b - 0x30);
				return;
			}
			if(b == 0x3B){
				p->osc_in_string = 1;
				return;
			}
			/* Anything else — start collecting the string */
			p->osc_in_string = 1;
			if(p->osc_len < PMAXOSC - 1)
				p->osc[p->osc_len++] = b;
		}
		return;

	case STATE_SOS_PM_APC_STRING:
		if(b == 0x07 || b == 0x1B){
			p->state = (b == 0x1B) ? STATE_ESCAPE : STATE_GROUND;
			if(b == 0x1B) reset_params(p);
			return;
		}
		return;
	}
}

/*
 * ESC final dispatches (no CSI).
 */
static void
esc_dispatch(Parser *p, uchar final)
{
	Buffer *b = p->buf;
	switch(final){
	case 'c': /* RIS — full reset */
		cellbuf_erase_display(b, 2);
		cellbuf_goto(b, 0, 0);
		cellbuf_set_attrs(b, 7, 0, 0);
		return;
	case 'D': /* IND — index (down, keep column) */
		cellbuf_index(b);
		return;
	case 'E': /* NEL — next line */
		cellbuf_carriage_return(b);
		cellbuf_newline(b);
		return;
	case 'M': /* RI — reverse index */
		if(b->cur_row > 0)
			b->cur_row--;
		else {
			/* TODO: reverse scroll. Not implemented in v1. */
		}
		return;
	case '7': /* DECSC */
		cellbuf_save_cursor(b);
		return;
	case '8': /* DECRC */
		cellbuf_restore_cursor(b);
		return;
	}
}

/*
 * CSI dispatch. Phase 2.2 only handles cursor-movement final bytes,
 * minimal subset to make the state machine wiring testable; full SGR
 * etc. lands in Phase 2.3.
 */
static int param_or(Parser *p, int idx, int def);

/*
 * CSI dispatch (Phase 2.3): SGR, cursor moves, erase, mode set/reset.
 */
static void
csi_dispatch(Parser *p, uchar final)
{
	Buffer *b = p->buf;
	int n, m, i, code;
	int fg, bg, attrs;

	switch(final){

	case 'A':	/* CUU — cursor up */
		n = param_or(p, 0, 1);
		cellbuf_goto(b, b->cur_row - n, b->cur_col);
		return;

	case 'B':	/* CUD — cursor down */
		n = param_or(p, 0, 1);
		cellbuf_goto(b, b->cur_row + n, b->cur_col);
		return;

	case 'C':	/* CUF — cursor right */
		n = param_or(p, 0, 1);
		cellbuf_goto(b, b->cur_row, b->cur_col + n);
		return;

	case 'D':	/* CUB — cursor left */
		n = param_or(p, 0, 1);
		cellbuf_goto(b, b->cur_row, b->cur_col - n);
		return;

	case 'H':	/* CUP — cursor position */
	case 'f':	/* HVP — same */
		n = param_or(p, 0, 1);   /* row, 1-indexed */
		m = param_or(p, 1, 1);   /* col, 1-indexed */
		cellbuf_goto(b, n - 1, m - 1);
		return;

	case 'J':	/* ED — erase display */
		cellbuf_erase_display(b, param_or(p, 0, 0));
		return;

	case 'K':	/* EL — erase line */
		cellbuf_erase_line(b, param_or(p, 0, 0));
		return;

	case 'm':	/* SGR */
		if(p->nparams == 0){
			cellbuf_set_attrs(b, 7, 0, 0);
			return;
		}
		fg = b->cur_fg;
		bg = b->cur_bg;
		attrs = b->cur_attrs;
		for(i = 0; i < p->nparams; i++){
			code = p->has_param[i] ? p->params[i] : 0;
			if(code == 0){
				fg = 7; bg = 0; attrs = 0;
			} else if(code == 1){
				attrs |= ATTR_BOLD;
			} else if(code == 4){
				attrs |= ATTR_UNDERLINE;
			} else if(code == 7){
				attrs |= ATTR_REVERSE;
			} else if(code == 22){
				attrs &= ~ATTR_BOLD;
			} else if(code == 24){
				attrs &= ~ATTR_UNDERLINE;
			} else if(code == 27){
				attrs &= ~ATTR_REVERSE;
			} else if(code >= 30 && code <= 37){
				fg = code - 30;
			} else if(code == 38){
				/* 256 or truecolor foreground:
				 *   38;5;N or 38;2;R;G;B
				 * Consume those params, quantize to 16-color. */
				if(i + 1 < p->nparams && p->params[i+1] == 5 &&
				   i + 2 < p->nparams){
					fg = p->params[i+2] & 0x0F;
					i += 2;
				} else if(i + 1 < p->nparams && p->params[i+1] == 2 &&
				          i + 4 < p->nparams){
					/* Crude truecolor quantization: pick brightest channel */
					int r = p->params[i+2];
					int g = p->params[i+3];
					int bl = p->params[i+4];
					if(r > g && r > bl) fg = (r > 128) ? 9 : 1;
					else if(g > bl) fg = (g > 128) ? 10 : 2;
					else fg = (bl > 128) ? 12 : 4;
					i += 4;
				}
			} else if(code == 39){
				fg = 7;
			} else if(code >= 40 && code <= 47){
				bg = code - 40;
			} else if(code == 48){
				if(i + 1 < p->nparams && p->params[i+1] == 5 &&
				   i + 2 < p->nparams){
					bg = p->params[i+2] & 0x0F;
					i += 2;
				} else if(i + 1 < p->nparams && p->params[i+1] == 2 &&
				          i + 4 < p->nparams){
					int r = p->params[i+2];
					int g = p->params[i+3];
					int bl = p->params[i+4];
					if(r > g && r > bl) bg = (r > 128) ? 9 : 1;
					else if(g > bl) bg = (g > 128) ? 10 : 2;
					else bg = (bl > 128) ? 12 : 4;
					i += 4;
				}
			} else if(code == 49){
				bg = 0;
			} else if(code >= 90 && code <= 97){
				fg = code - 90 + 8;
			} else if(code >= 100 && code <= 107){
				bg = code - 100 + 8;
			}
			/* Unknown codes silently ignored. */
		}
		cellbuf_set_attrs(b, fg, bg, attrs);
		return;

	case 'h':	/* SM — set mode */
		if(p->csi_private){
			/* DECSET — private modes */
			for(i = 0; i < p->nparams; i++){
				int code = p->params[i];
				if(code == 7)
					b->wrap = 1;        /* DECAWM on — auto-wrap */
				else if(code == 25)
					b->cur_visible = 1;
				else if(code == 1049){
					/* Alt screen — we just save cursor and clear */
					cellbuf_save_cursor(b);
					cellbuf_erase_display(b, 2);
					cellbuf_goto(b, 0, 0);
				}
				/* others ignored */
			}
		}
		return;

	case 'l':	/* RM — reset mode */
		if(p->csi_private){
			for(i = 0; i < p->nparams; i++){
				int code = p->params[i];
				if(code == 7)
					b->wrap = 0;        /* DECAWM off — cursor sticks at last col */
				else if(code == 25)
					b->cur_visible = 0;
				else if(code == 1049){
					cellbuf_restore_cursor(b);
				}
			}
		}
		return;

	case 's':	/* SCP — save cursor */
		cellbuf_save_cursor(b);
		return;

	case 'u':	/* RCP — restore cursor */
		cellbuf_restore_cursor(b);
		return;
	}
}

static int
param_or(Parser *p, int idx, int def)
{
	if(idx >= p->nparams || !p->has_param[idx])
		return def;
	return p->params[idx];
}

static void
osc_dispatch(Parser *p)
{
	/* Phase 2.3 will hook this to set window title. */
#ifndef HOST
	USED(p);
#else
	(void)p;
#endif
}

void
parser_feed(Parser *p, const uchar *bytes, int n)
{
	int i;
	for(i = 0; i < n; i++)
		step(p, bytes[i]);
}

void
parser_feed_str(Parser *p, const char *s)
{
	parser_feed(p, (const uchar*)s, (int)strlen(s));
}
