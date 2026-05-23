/*
 * vts line editor — Phase 5
 *
 * In-line editor that sits between the terminal client (mxio/vts-attach)
 * and rc's stdin. When "edit on", keystrokes are processed locally:
 *
 *   printable chars     -> appended to buffer at cursor
 *   ^H / 0x7f           -> backspace
 *   Left/Right arrow    -> move cursor
 *   Up/Down arrow       -> history scroll
 *   Home/End            -> cursor to start/end
 *   ^A / ^E             -> same as Home/End (emacs)
 *   ^U                  -> clear line
 *   ^W                  -> delete word backwards
 *   Enter (CR/LF)       -> flush the buffer as one line to rc + push history
 *
 * History: ring buffer of N most recent submitted lines, accessed
 * with Up/Down arrows. Up scrolls back, Down forward. Modifying the
 * current line cancels history navigation.
 *
 * The editor renders ITS state by writing to the cell buffer via the
 * parser — so both mxio and vts-attach see the editing happen.
 *
 * Portable C (compiles with macOS clang + Plan 9 6c).
 */
#ifndef VTS_LINEDIT_H
#define VTS_LINEDIT_H

#include "compat.h"

enum {
	LINEED_MAXBUF = 4096,
	LINEED_MAXHIST = 200,
	LINEED_HIST_LINE = 256,
};

/* Result codes from lined_feed_byte */
enum {
	LINEED_CONSUMED = 0,    /* byte processed, no flush */
	LINEED_COMPLETE = 1,    /* line ready in `out` (Enter was pressed) */
	LINEED_PASSTHROUGH = 2, /* editor declined, caller should forward byte */
};

typedef struct LineEditor LineEditor;
struct LineEditor {
	int enabled;               /* if 0, all bytes pass through */

	uchar buf[LINEED_MAXBUF];
	int   len;                 /* current bytes in buf */
	int   cursor;              /* cursor position in buf */

	/* History (ring buffer) */
	char  history[LINEED_MAXHIST][LINEED_HIST_LINE];
	int   hist_head;           /* next slot to write */
	int   hist_count;          /* total entries pushed (clamped to MAXHIST) */
	int   hist_pos;            /* 0 = current line, 1..N = N steps back */

	/* Escape state for arrow keys: ESC [ X */
	int   esc_state;           /* 0 none, 1 saw ESC, 2 saw ESC [ */
};

void lined_init(LineEditor *e);
void lined_set_enabled(LineEditor *e, int on);

/*
 * Feed one byte. Returns one of LINEED_CONSUMED / COMPLETE / PASSTHROUGH.
 * On COMPLETE, the line (without trailing newline) is in `*out_buf` /
 * `*out_len`. Caller should typically forward `"<line>\n"` to rc.
 *
 * `redraw_buf` / `redraw_len` (if non-nil) get a VT100 redraw sequence
 * that the caller should write to the cell buffer/screen to update the
 * displayed editing line (clear, redraw, cursor reposition).
 */
int lined_feed_byte(LineEditor *e, uchar byte,
	uchar **out_buf, int *out_len,
	uchar *redraw_buf, int redraw_max, int *redraw_len);

void lined_push_history(LineEditor *e, const char *line);

#endif
