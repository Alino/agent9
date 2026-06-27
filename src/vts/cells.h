/*
 * vts cell buffer — header.
 */
#ifndef VTS_CELLS_H
#define VTS_CELLS_H

#include "compat.h"

typedef struct Cell Cell;
struct Cell {
	int rune;       /* UTF-32 code point */
	uchar fg;       /* 0-15 palette index */
	uchar bg;       /* 0-15 palette index */
	uchar attrs;    /* bit0 bold, bit1 underline, bit2 reverse */
	uchar _pad;
};

/* Attribute bits */
enum {
	ATTR_BOLD      = 1 << 0,
	ATTR_UNDERLINE = 1 << 1,
	ATTR_REVERSE   = 1 << 2,
};

/* Scrollback ring buffer: stores the runes of rows that scrolled off
 * the top. Default 10000 lines, each up to 200 chars.
 * Newest at scroll_head; wraps around. */
enum {
	SCROLL_MAX_LINES = 10000,
	SCROLL_LINE_BYTES = 512,   /* enough for 80 cols worth of UTF-8 */
};

typedef struct Buffer Buffer;
struct Buffer {
	int rows;
	int cols;
	int ncells;       /* rows * cols */
	Cell *cells;      /* rows × cols grid, row-major */
	uchar *dirty;     /* bitmap, 1 bit per cell */
	int all_dirty;    /* if set, every cell is dirty (after full clear/init) */

	/* Cursor */
	int cur_row, cur_col;
	uchar cur_visible;

	/* Alternate-screen active (DECSET 1049). Mirrored to the cell-diff
	 * wire so clients can tell a full-screen app is running (vtwin uses
	 * it to layer ^C: kill the app first, exit only at the bare shell). */
	uchar alt_screen;

	/* Current rendition (applied to written cells) */
	uchar cur_fg, cur_bg, cur_attrs;

	/* Saved cursor + rendition (DECSC/DECRC) */
	int saved_row, saved_col;
	uchar saved_fg, saved_bg, saved_attrs;

	/* DECAWM (CSI ?7 h/l) — auto-wrap mode.
	 * When wrap=1 (default, VT100 standard), writing past the last
	 * column wraps to the next row and may scroll.
	 * When wrap=0, the cursor "sticks" at the last column: subsequent
	 * writes overwrite that cell rather than wrapping. Required for
	 * full-screen TUIs that paint into the last cell of the last
	 * row without wanting an unwanted scroll. */
	uchar wrap;

	/* Scrollback ring buffer. Each entry is a UTF-8 line (no newline).
	 * scroll_count is the total lines ever pushed (used to detect
	 * wraparound). scroll_head is the next slot to write. */
	char *scroll_lines;     /* SCROLL_MAX_LINES * SCROLL_LINE_BYTES */
	int   scroll_head;
	long  scroll_count;
};

void cellbuf_init(Buffer *b, int rows, int cols);
void cellbuf_free(Buffer *b);
void cellbuf_resize(Buffer *b, int rows, int cols);

void cellbuf_put(Buffer *b, int rune);
void cellbuf_advance(Buffer *b);
void cellbuf_newline(Buffer *b);
void cellbuf_index(Buffer *b);
void cellbuf_carriage_return(Buffer *b);
void cellbuf_backspace(Buffer *b);
void cellbuf_tab(Buffer *b);
void cellbuf_scroll(Buffer *b, int n);

void cellbuf_goto(Buffer *b, int row, int col);
void cellbuf_erase_line(Buffer *b, int mode);
void cellbuf_erase_display(Buffer *b, int mode);

void cellbuf_save_cursor(Buffer *b);
void cellbuf_restore_cursor(Buffer *b);
void cellbuf_set_attrs(Buffer *b, int fg, int bg, int attrs);

/* Engine bridge (engine.c): write one cell at an absolute position and mark
 * it dirty; push a pre-rendered UTF-8 line into the scrollback ring. */
void cellbuf_set(Buffer *b, int row, int col, int rune, int fg, int bg, int attrs);
void cellbuf_push_scroll(Buffer *b, const char *utf8line);

int cellbuf_dirty_count(Buffer *b);
int cellbuf_drain_dirty(Buffer *b, int *idxs, Cell *out, int maxcells);
Cell cellbuf_get(Buffer *b, int row, int col);

/* Scrollback access. Read `n` most-recent lines starting at offset
 * `from_end` from the end (0 = newest). Returns the number of lines
 * actually written into `out`. Each line is NUL-terminated; lines are
 * up to SCROLL_LINE_BYTES-1 chars. Caller provides:
 *   char buf[N * SCROLL_LINE_BYTES];
 */
int cellbuf_scrollback_lines(Buffer *b, int from_end, int n, char *out);
int cellbuf_scrollback_count(Buffer *b);   /* total lines retained */

#endif
