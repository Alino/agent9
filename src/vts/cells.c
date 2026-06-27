/*
 * vts cell buffer.
 *
 * Portable C, compiled both on Plan 9 (via 6c) and on macOS (via clang for
 * unit testing). The shim in compat.h maps Plan 9 idioms (Rune, print,
 * malloc-with-zero) to their libc equivalents when we're not on Plan 9.
 *
 * A Cell holds one screen position. A Buffer is a rows×cols grid of cells
 * plus a cursor and a scrollback ring.
 *
 * The parser (parser.c) calls into here via the cellbuf_* API; the 9P srv
 * (srv.c) reads diffs out via cellbuf_drain_dirty.
 */
#include "compat.h"
#include "cells.h"

/* Default 16-color palette indices (see wiki/concepts/vt100-parsing.md). */
#define DEFAULT_FG  7   /* white */
#define DEFAULT_BG  0   /* black */

void
cellbuf_init(Buffer *b, int rows, int cols)
{
	int i;

	b->rows = rows;
	b->cols = cols;
	b->ncells = rows * cols;
	b->cells = (Cell*)malloc(b->ncells * sizeof(Cell));
	b->dirty = (uchar*)malloc((b->ncells + 7) / 8);
	memset(b->cells, 0, b->ncells * sizeof(Cell));
	memset(b->dirty, 0, (b->ncells + 7) / 8);
	for(i = 0; i < b->ncells; i++){
		b->cells[i].rune = ' ';
		b->cells[i].fg = DEFAULT_FG;
		b->cells[i].bg = DEFAULT_BG;
		b->cells[i].attrs = 0;
	}
	b->cur_row = 0;
	b->cur_col = 0;
	b->cur_visible = 1;
	b->alt_screen = 0;
	b->wrap = 1;  /* DECAWM default: auto-wrap on (VT100 standard) */
	b->saved_row = 0;
	b->saved_col = 0;
	b->cur_fg = DEFAULT_FG;
	b->cur_bg = DEFAULT_BG;
	b->cur_attrs = 0;
	b->all_dirty = 1;	/* first paint = full repaint */

	/* Scrollback ring buffer */
	b->scroll_lines = (char*)malloc(SCROLL_MAX_LINES * SCROLL_LINE_BYTES);
	if(b->scroll_lines)
		memset(b->scroll_lines, 0, SCROLL_MAX_LINES * SCROLL_LINE_BYTES);
	b->scroll_head = 0;
	b->scroll_count = 0;
}

void
cellbuf_free(Buffer *b)
{
	free(b->cells);
	free(b->dirty);
	free(b->scroll_lines);
	b->cells = nil;
	b->dirty = nil;
	b->scroll_lines = nil;
}

/*
 * Resize the cell buffer in-place. Content from the old buffer that
 * fits is preserved (clamped to top-left). Cursor is clamped to the
 * new dimensions. all_dirty is set so a full repaint is emitted on
 * the next celldiff_encode call.
 */
void
cellbuf_resize(Buffer *b, int rows, int cols)
{
	int i, r, c;
	Cell *oldcells = b->cells;
	uchar *olddirty = b->dirty;
	int oldrows = b->rows;
	int oldcols = b->cols;

	if(rows == oldrows && cols == oldcols)
		return;
	if(rows < 1) rows = 1;
	if(cols < 1) cols = 1;

	b->rows = rows;
	b->cols = cols;
	b->ncells = rows * cols;
	b->cells = (Cell*)malloc(b->ncells * sizeof(Cell));
	b->dirty = (uchar*)malloc((b->ncells + 7) / 8);
	memset(b->dirty, 0, (b->ncells + 7) / 8);

	/* Init all cells to spaces with current attrs */
	for(i = 0; i < b->ncells; i++){
		b->cells[i].rune = ' ';
		b->cells[i].fg = b->cur_fg;
		b->cells[i].bg = b->cur_bg;
		b->cells[i].attrs = 0;
	}

	/* Copy old cells that fit (top-left aligned) */
	if(oldcells != nil){
		int copy_rows = rows < oldrows ? rows : oldrows;
		int copy_cols = cols < oldcols ? cols : oldcols;
		for(r = 0; r < copy_rows; r++){
			for(c = 0; c < copy_cols; c++){
				b->cells[r * cols + c] = oldcells[r * oldcols + c];
			}
		}
		free(oldcells);
		free(olddirty);
	}

	/* Clamp cursor */
	if(b->cur_row >= rows) b->cur_row = rows - 1;
	if(b->cur_col >= cols) b->cur_col = cols - 1;
	if(b->saved_row >= rows) b->saved_row = rows - 1;
	if(b->saved_col >= cols) b->saved_col = cols - 1;

	b->all_dirty = 1;
}

static int
cell_idx(Buffer *b, int row, int col)
{
	return row * b->cols + col;
}

static void
mark_dirty(Buffer *b, int row, int col)
{
	int i;
	if(row < 0 || row >= b->rows || col < 0 || col >= b->cols)
		return;
	i = cell_idx(b, row, col);
	b->dirty[i >> 3] |= (1 << (i & 7));
}

void
cellbuf_put(Buffer *b, int rune)
{
	Cell *c;
	if(b->cur_row < 0 || b->cur_row >= b->rows)
		return;
	if(b->cur_col < 0 || b->cur_col >= b->cols)
		return;
	c = &b->cells[cell_idx(b, b->cur_row, b->cur_col)];
	c->rune = rune;
	c->fg = b->cur_fg;
	c->bg = b->cur_bg;
	c->attrs = b->cur_attrs;
	mark_dirty(b, b->cur_row, b->cur_col);
}

/*
 * Cursor advance with wrap. After writing a printable rune, call this to
 * move the cursor one column right; at end of line, wrap to next row,
 * scrolling if needed.
 *
 * If DECAWM is off (b->wrap == 0), the cursor "sticks" at the rightmost
 * column. Subsequent writes overwrite that cell in place. xterm and
 * other VT100s do the same.
 */
void
cellbuf_advance(Buffer *b)
{
	b->cur_col++;
	if(b->cur_col >= b->cols){
		if(b->wrap){
			b->cur_col = 0;
			cellbuf_newline(b);
		} else {
			/* Stay glued to last column. */
			b->cur_col = b->cols - 1;
		}
	}
}

void
cellbuf_newline(Buffer *b)
{
	/* Plan 9 convention: LF means "newline" (move to start of next row),
	 * not VT100's "index" (move down, keep column). rc and friends emit
	 * bare \n with no preceding \r and expect a fresh column. ESC D (IND)
	 * uses cellbuf_index() for strict VT100 down-only semantics. */
	b->cur_col = 0;
	b->cur_row++;
	if(b->cur_row >= b->rows){
		b->cur_row = b->rows - 1;
		cellbuf_scroll(b, 1);
	}
}

void
cellbuf_index(Buffer *b)
{
	/* VT100 IND: cursor down one row, column unchanged. */
	b->cur_row++;
	if(b->cur_row >= b->rows){
		b->cur_row = b->rows - 1;
		cellbuf_scroll(b, 1);
	}
}

void
cellbuf_carriage_return(Buffer *b)
{
	b->cur_col = 0;
}

void
cellbuf_backspace(Buffer *b)
{
	if(b->cur_col > 0)
		b->cur_col--;
}

void
cellbuf_tab(Buffer *b)
{
	int next = (b->cur_col + 8) & ~7;
	if(next >= b->cols)
		next = b->cols - 1;
	b->cur_col = next;
}

/*
 * UTF-8 encode a single code point. Writes up to 4 bytes; returns count.
 */
static int
utf8_encode(int rune, char *out)
{
	if(rune < 0x80){
		out[0] = rune;
		return 1;
	} else if(rune < 0x800){
		out[0] = 0xC0 | (rune >> 6);
		out[1] = 0x80 | (rune & 0x3F);
		return 2;
	} else if(rune < 0x10000){
		out[0] = 0xE0 | (rune >> 12);
		out[1] = 0x80 | ((rune >> 6) & 0x3F);
		out[2] = 0x80 | (rune & 0x3F);
		return 3;
	} else {
		out[0] = 0xF0 | (rune >> 18);
		out[1] = 0x80 | ((rune >> 12) & 0x3F);
		out[2] = 0x80 | ((rune >> 6) & 0x3F);
		out[3] = 0x80 | (rune & 0x3F);
		return 4;
	}
}

/* Push one buffer row into the scrollback ring as a UTF-8 string. */
static void
scrollback_push_row(Buffer *b, int row)
{
	char *line;
	int c, n, p, lastnonspace;
	Cell cell;

	if(b->scroll_lines == nil)
		return;
	line = b->scroll_lines + b->scroll_head * SCROLL_LINE_BYTES;

	/* Find rightmost non-space cell so we don't store trailing blanks. */
	lastnonspace = -1;
	for(c = 0; c < b->cols; c++){
		cell = b->cells[row * b->cols + c];
		if(cell.rune != ' ' && cell.rune != 0)
			lastnonspace = c;
	}

	p = 0;
	for(c = 0; c <= lastnonspace && p < SCROLL_LINE_BYTES - 5; c++){
		cell = b->cells[row * b->cols + c];
		n = utf8_encode(cell.rune ? cell.rune : ' ', line + p);
		p += n;
	}
	line[p] = 0;

	b->scroll_head = (b->scroll_head + 1) % SCROLL_MAX_LINES;
	b->scroll_count++;
}

/*
 * Scroll the buffer up by n rows. Top n rows are pushed to scrollback,
 * bottom n rows become blank.
 */
void
cellbuf_scroll(Buffer *b, int n)
{
	int r, c, i;
	Cell blank;

	if(n <= 0 || n > b->rows)
		return;

	/* Push scrolled-off rows into scrollback */
	for(r = 0; r < n; r++)
		scrollback_push_row(b, r);

	memmove(b->cells,
		b->cells + n * b->cols,
		(b->rows - n) * b->cols * sizeof(Cell));

	blank.rune = ' ';
	blank.fg = DEFAULT_FG;
	blank.bg = DEFAULT_BG;
	blank.attrs = 0;
	for(r = b->rows - n; r < b->rows; r++)
		for(c = 0; c < b->cols; c++)
			b->cells[cell_idx(b, r, c)] = blank;

	/* Whole screen content shifted — every cell dirty. */
	for(i = 0; i < b->ncells; i++)
		b->dirty[i >> 3] |= (1 << (i & 7));
}

/*
 * Cursor positioning. 0-indexed internally; the parser converts from
 * 1-indexed VT100 coords.
 */
void
cellbuf_goto(Buffer *b, int row, int col)
{
	if(row < 0) row = 0;
	if(row >= b->rows) row = b->rows - 1;
	if(col < 0) col = 0;
	if(col >= b->cols) col = b->cols - 1;
	b->cur_row = row;
	b->cur_col = col;
}

/*
 * Erase functions. Mode: 0=below/right, 1=above/left, 2=all.
 */
static void
clear_range(Buffer *b, int from, int to)
{
	int i;
	Cell blank;
	blank.rune = ' ';
	blank.fg = b->cur_fg;
	blank.bg = b->cur_bg;
	blank.attrs = 0;
	for(i = from; i < to && i < b->ncells; i++){
		b->cells[i] = blank;
		b->dirty[i >> 3] |= (1 << (i & 7));
	}
}

void
cellbuf_erase_line(Buffer *b, int mode)
{
	int row_start = b->cur_row * b->cols;
	int row_end = row_start + b->cols;
	int cur = row_start + b->cur_col;

	switch(mode){
	case 0:	/* right of cursor (inclusive) */
		clear_range(b, cur, row_end);
		break;
	case 1:	/* left of cursor (inclusive) */
		clear_range(b, row_start, cur + 1);
		break;
	case 2:	/* whole line */
		clear_range(b, row_start, row_end);
		break;
	}
}

void
cellbuf_erase_display(Buffer *b, int mode)
{
	int cur = cell_idx(b, b->cur_row, b->cur_col);
	switch(mode){
	case 0:	/* from cursor to end */
		clear_range(b, cur, b->ncells);
		break;
	case 1:	/* from start to cursor */
		clear_range(b, 0, cur + 1);
		break;
	case 2:	/* whole screen */
		clear_range(b, 0, b->ncells);
		break;
	}
}

void
cellbuf_save_cursor(Buffer *b)
{
	b->saved_row = b->cur_row;
	b->saved_col = b->cur_col;
	b->saved_fg = b->cur_fg;
	b->saved_bg = b->cur_bg;
	b->saved_attrs = b->cur_attrs;
}

void
cellbuf_restore_cursor(Buffer *b)
{
	b->cur_row = b->saved_row;
	b->cur_col = b->saved_col;
	b->cur_fg = b->saved_fg;
	b->cur_bg = b->saved_bg;
	b->cur_attrs = b->saved_attrs;
}

void
cellbuf_set_attrs(Buffer *b, int fg, int bg, int attrs)
{
	b->cur_fg = fg;
	b->cur_bg = bg;
	b->cur_attrs = attrs;
}

/*
 * Engine bridge: write one cell at an absolute (row,col) and mark it dirty.
 * Used by engine.c's damage callback — libvterm owns the screen model, this
 * just mirrors changed cells into our wire buffer.
 */
void
cellbuf_set(Buffer *b, int row, int col, int rune, int fg, int bg, int attrs)
{
	Cell *c;
	if(row < 0 || row >= b->rows || col < 0 || col >= b->cols)
		return;
	c = &b->cells[cell_idx(b, row, col)];
	c->rune = rune;
	c->fg = (uchar)fg;
	c->bg = (uchar)bg;
	c->attrs = (uchar)attrs;
	mark_dirty(b, row, col);
}

/*
 * Engine bridge: push a pre-rendered, NUL-terminated UTF-8 line into the
 * scrollback ring (libvterm's sb_pushline callback).
 */
void
cellbuf_push_scroll(Buffer *b, const char *utf8line)
{
	char *line;
	int n;

	if(b->scroll_lines == nil)
		return;
	line = b->scroll_lines + b->scroll_head * SCROLL_LINE_BYTES;
	n = strlen(utf8line);
	if(n > SCROLL_LINE_BYTES - 1)
		n = SCROLL_LINE_BYTES - 1;
	memmove(line, utf8line, n);
	line[n] = 0;
	b->scroll_head = (b->scroll_head + 1) % SCROLL_MAX_LINES;
	b->scroll_count++;
}

int
cellbuf_dirty_count(Buffer *b)
{
	int i, n = 0;
	if(b->all_dirty)
		return b->ncells;
	for(i = 0; i < b->ncells; i++)
		if(b->dirty[i >> 3] & (1 << (i & 7)))
			n++;
	return n;
}

/* Drain dirty cells. Caller passes an array; we fill it with (idx, Cell)
 * pairs up to maxcells. Returns the number written. After draining, the
 * dirty bitmap is cleared. */
int
cellbuf_drain_dirty(Buffer *b, int *idxs, Cell *out, int maxcells)
{
	int i, n = 0;
	if(b->all_dirty){
		for(i = 0; i < b->ncells && n < maxcells; i++){
			idxs[n] = i;
			out[n] = b->cells[i];
			n++;
		}
		b->all_dirty = 0;
	} else {
		for(i = 0; i < b->ncells && n < maxcells; i++){
			if(b->dirty[i >> 3] & (1 << (i & 7))){
				idxs[n] = i;
				out[n] = b->cells[i];
				n++;
			}
		}
	}
	memset(b->dirty, 0, (b->ncells + 7) / 8);
	return n;
}

Cell
cellbuf_get(Buffer *b, int row, int col)
{
	Cell empty;
	if(row < 0 || row >= b->rows || col < 0 || col >= b->cols){
		empty.rune = 0;
		empty.fg = 0;
		empty.bg = 0;
		empty.attrs = 0;
		return empty;
	}
	return b->cells[cell_idx(b, row, col)];
}

/*
 * Scrollback access. Returns the number of lines actually written.
 * `out` is filled with concatenated NUL-terminated lines; the caller
 * walks them.
 *
 * Lines are returned OLDEST-FIRST. `from_end` of 0 means start at the
 * very oldest retained line. Pass `cellbuf_scrollback_count(b)` to get
 * the count first, then allocate `count * SCROLL_LINE_BYTES`.
 *
 * Simpler API: pass from_end=0, n=count, out=count*SCROLL_LINE_BYTES.
 */
int
cellbuf_scrollback_count(Buffer *b)
{
	if(b->scroll_lines == nil) return 0;
	return b->scroll_count < SCROLL_MAX_LINES ?
		(int)b->scroll_count : SCROLL_MAX_LINES;
}

int
cellbuf_scrollback_lines(Buffer *b, int from_end, int n, char *out)
{
	int total, start, i;

	if(b->scroll_lines == nil) return 0;
	total = cellbuf_scrollback_count(b);
	if(from_end >= total) return 0;
	if(from_end + n > total) n = total - from_end;

	/* The OLDEST line lives at:
	 *   scroll_head     if buffer is full (overwriting started)
	 *   0               if not yet full
	 * Actually: oldest = (scroll_head - total + SCROLL_MAX_LINES) % SCROLL_MAX_LINES
	 *           but when total < SCROLL_MAX, oldest is at 0.
	 */
	if(b->scroll_count < SCROLL_MAX_LINES){
		start = from_end;
	} else {
		start = (b->scroll_head + from_end) % SCROLL_MAX_LINES;
	}

	for(i = 0; i < n; i++){
		int idx = (start + i) % SCROLL_MAX_LINES;
		const char *src = b->scroll_lines + idx * SCROLL_LINE_BYTES;
		memmove(out + i * SCROLL_LINE_BYTES, src, SCROLL_LINE_BYTES);
	}
	return n;
}
