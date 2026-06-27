/*
 * vts terminal engine — libvterm bridge.
 *
 * libvterm owns the VT state machine and the authoritative screen. We wire
 * its screen callbacks to copy changed cells into our Buffer (cells.h), so
 * the existing cell-diff wire format (celldiff.c) and 9P server (srv.c) need
 * no changes. See engine.h and wiki/concepts/vt-architecture.md.
 *
 * Plan 9 only — libvterm.a is built from src/libvterm (native 6c).
 */
#include "compat.h"      /* pulls <u.h>/<libc.h>; defines nil before vterm.h */
#include <vterm.h>
#include "cells.h"
#include "engine.h"

/* Our wire is a 16-colour palette index (cells.h). These match cells.c. */
enum { DEF_FG = 7, DEF_BG = 0 };

/* Map an RGB triple to the nearest ANSI-16 index (classic xterm scheme:
 * one bit per channel over a mid threshold, +8 when clearly bright). */
static int
rgb_to_16(int r, int g, int b)
{
	int v = 0, mx;
	if(r > 0x55) v |= 1;   /* red   -> bit0 */
	if(g > 0x55) v |= 2;   /* green -> bit1 */
	if(b > 0x55) v |= 4;   /* blue  -> bit2 */
	mx = r; if(g > mx) mx = g; if(b > mx) mx = b;
	if(mx > 0xaa) v += 8;  /* bright variant */
	return v;
}

/* Map a libvterm 256-colour index to ANSI-16. 0-15 pass through; 16-231 are
 * the 6x6x6 cube; 232-255 are the grayscale ramp. */
static int
idx256_to_16(int idx)
{
	int r, g, b, l;
	if(idx < 16)
		return idx;
	if(idx < 232){
		idx -= 16;
		r = (idx / 36) % 6;
		g = (idx / 6) % 6;
		b = idx % 6;
		return rgb_to_16(r*51, g*51, b*51);
	}
	l = (idx - 232) * 10 + 8;
	return rgb_to_16(l, l, l);
}

static int
color_to_idx(VTermColor *c, int isfg)
{
	if(VTERM_COLOR_IS_DEFAULT_FG(c))
		return DEF_FG;
	if(VTERM_COLOR_IS_DEFAULT_BG(c))
		return DEF_BG;
	if(VTERM_COLOR_IS_INDEXED(c))
		return idx256_to_16(c->indexed.idx);
	if(VTERM_COLOR_IS_RGB(c))
		return rgb_to_16(c->rgb.red, c->rgb.green, c->rgb.blue);
	return isfg ? DEF_FG : DEF_BG;
}

static int
attrs_to_bits(VTermScreenCellAttrs *a)
{
	int v = 0;
	if(a->bold)      v |= ATTR_BOLD;
	if(a->underline) v |= ATTR_UNDERLINE;   /* underline is 0/1/2; any -> on */
	if(a->reverse)   v |= ATTR_REVERSE;
	return v;
}

/* --- libvterm screen callbacks ------------------------------------------ */

static int
cb_damage(VTermRect rect, void *user)
{
	Engine *e = user;
	VTermScreen *vs = e->vs;
	VTermPos pos;
	VTermScreenCell cell;
	int rune;

	for(pos.row = rect.start_row; pos.row < rect.end_row; pos.row++){
		for(pos.col = rect.start_col; pos.col < rect.end_col; pos.col++){
			if(!vterm_screen_get_cell(vs, pos, &cell))
				continue;
			rune = cell.chars[0];
			/* 0 = blank, (uint32_t)-1 = right half of a wide glyph */
			if(rune == 0 || rune == (int)0xffffffff)
				rune = ' ';
			cellbuf_set(e->buf, pos.row, pos.col, rune,
				color_to_idx(&cell.fg, 1),
				color_to_idx(&cell.bg, 0),
				attrs_to_bits(&cell.attrs));
		}
	}
	return 1;
}

static int
cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
	Engine *e = user;
	USED(oldpos);
	e->buf->cur_row = pos.row;
	e->buf->cur_col = pos.col;
	e->buf->cur_visible = visible ? 1 : 0;
	return 1;
}

static int
cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
	Engine *e = user;
	if(prop == VTERM_PROP_CURSORVISIBLE)
		e->buf->cur_visible = val->boolean ? 1 : 0;
	else if(prop == VTERM_PROP_ALTSCREEN)
		e->buf->alt_screen = val->boolean ? 1 : 0;
	return 1;
}

/* UTF-8 encode one rune; returns byte count (1..4). */
static int
u8enc(int rune, char *out)
{
	if(rune < 0x80){ out[0] = rune; return 1; }
	if(rune < 0x800){
		out[0] = 0xC0 | (rune >> 6);
		out[1] = 0x80 | (rune & 0x3F);
		return 2;
	}
	if(rune < 0x10000){
		out[0] = 0xE0 | (rune >> 12);
		out[1] = 0x80 | ((rune >> 6) & 0x3F);
		out[2] = 0x80 | (rune & 0x3F);
		return 3;
	}
	out[0] = 0xF0 | (rune >> 18);
	out[1] = 0x80 | ((rune >> 12) & 0x3F);
	out[2] = 0x80 | ((rune >> 6) & 0x3F);
	out[3] = 0x80 | (rune & 0x3F);
	return 4;
}

static int
cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
	Engine *e = user;
	char line[SCROLL_LINE_BYTES];
	int c, p, last, rune;

	/* Trim trailing blanks. */
	last = -1;
	for(c = 0; c < cols; c++){
		rune = cells[c].chars[0];
		if(rune != 0 && rune != ' ' && rune != (int)0xffffffff)
			last = c;
	}
	p = 0;
	for(c = 0; c <= last && p < SCROLL_LINE_BYTES - 5; c++){
		rune = cells[c].chars[0];
		if(rune == 0 || rune == (int)0xffffffff)
			rune = ' ';
		p += u8enc(rune, line + p);
	}
	line[p] = 0;
	cellbuf_push_scroll(e->buf, line);
	return 1;
}

/* vterm wants to send bytes to the host (terminal replies). Route to sink. */
static void
cb_output(const char *s, size_t len, void *user)
{
	Engine *e = user;
	if(e->out && len > 0)
		e->out(e->outctx, (const uchar*)s, (int)len);
}

static VTermScreenCallbacks engine_cbs;   /* filled in engine_init (no compound literals) */

void
engine_init(Engine *e, Buffer *b, int rows, int cols, EngineOut out, void *outctx)
{
	VTerm *vt;
	VTermScreen *vs;

	e->buf = b;
	e->out = out;
	e->outctx = outctx;

	vt = vterm_new(rows, cols);
	vterm_set_utf8(vt, 1);
	vterm_output_set_callback(vt, cb_output, e);

	vs = vterm_obtain_screen(vt);

	/* Publish vt/vs BEFORE reset: vterm_screen_reset() fires cb_damage
	 * synchronously, which dereferences e->vs. */
	e->vt = vt;
	e->vs = vs;

	engine_cbs.damage      = cb_damage;
	engine_cbs.moverect    = nil;       /* nil -> libvterm falls back to damage */
	engine_cbs.movecursor  = cb_movecursor;
	engine_cbs.settermprop = cb_settermprop;
	engine_cbs.bell        = nil;
	engine_cbs.resize      = nil;       /* we drive sizing via engine_resize */
	engine_cbs.sb_pushline = cb_sb_pushline;
	engine_cbs.sb_popline  = nil;
	engine_cbs.sb_clear    = nil;
	vterm_screen_set_callbacks(vs, &engine_cbs, e);

	/* Give full-screen apps (vi, pi9, …) a real alternate screen: on exit
	 * the primary buffer (shell) is restored instead of left scrolled. */
	vterm_screen_enable_altscreen(vs, 1);

	vterm_screen_reset(vs, 1);
}

void
engine_free(Engine *e)
{
	if(e->vt){
		vterm_free((VTerm*)e->vt);
		e->vt = nil;
		e->vs = nil;
	}
}

void
engine_feed(Engine *e, const uchar *bytes, int n)
{
	if(e->vt && n > 0)
		vterm_input_write((VTerm*)e->vt, (const char*)bytes, n);
}

void
engine_resize(Engine *e, int rows, int cols)
{
	if(e->vt)
		vterm_set_size((VTerm*)e->vt, rows, cols);
}
