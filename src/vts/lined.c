/*
 * vts line editor implementation. See lined.h.
 */
#include "compat.h"
#include "lined.h"

void
lined_init(LineEditor *e)
{
	memset(e, 0, sizeof *e);
	e->enabled = 1;
}

void
lined_set_enabled(LineEditor *e, int on)
{
	e->enabled = on;
}

void
lined_push_history(LineEditor *e, const char *line)
{
	int len;
	if(line == nil) return;
	len = strlen(line);
	if(len == 0) return;
	if(len >= LINEED_HIST_LINE) len = LINEED_HIST_LINE - 1;
	memmove(e->history[e->hist_head], line, len);
	e->history[e->hist_head][len] = 0;
	e->hist_head = (e->hist_head + 1) % LINEED_MAXHIST;
	if(e->hist_count < LINEED_MAXHIST)
		e->hist_count++;
}

/* Index 0 = newest entry, 1 = second newest, etc. */
static const char*
hist_get(LineEditor *e, int idx)
{
	int slot;
	if(idx <= 0 || idx > e->hist_count)
		return nil;
	slot = (e->hist_head - idx + LINEED_MAXHIST) % LINEED_MAXHIST;
	return e->history[slot];
}

static void
load_history_into_buf(LineEditor *e, int pos)
{
	const char *line;
	int len;
	if(pos == 0){
		e->len = 0;
		e->cursor = 0;
		return;
	}
	line = hist_get(e, pos);
	if(line == nil) return;
	len = strlen(line);
	if(len >= LINEED_MAXBUF) len = LINEED_MAXBUF - 1;
	memmove(e->buf, line, len);
	e->len = len;
	e->cursor = len;
}

/*
 * Build a redraw sequence: CR + erase-to-EOL + buffer + cursor positioning.
 * Returns bytes written. Conservative — assumes the prompt is on the
 * current line and we're editing after it; caller may need to prepend
 * a prompt restore.
 */
static int
build_redraw(LineEditor *e, uchar *out, int max)
{
	int p = 0;
	int i;
	int back;

	if(max < 16) return 0;
	/* CR */
	out[p++] = '\r';
	/* erase to end-of-line: ESC [ K */
	out[p++] = 0x1B; out[p++] = '['; out[p++] = 'K';
	/* buffer content */
	for(i = 0; i < e->len && p < max - 16; i++)
		out[p++] = e->buf[i];
	/* Move cursor back if not at end */
	back = e->len - e->cursor;
	if(back > 0 && p < max - 8){
		/* ESC [ <n> D */
		char tmp[16];
		int n = snprint(tmp, sizeof tmp, "\x1b[%dD", back);
		if(p + n < max){
			memmove(out + p, tmp, n);
			p += n;
		}
	}
	return p;
}

/*
 * Process a single byte.
 */
int
lined_feed_byte(LineEditor *e, uchar byte,
	uchar **out_buf, int *out_len,
	uchar *redraw_buf, int redraw_max, int *redraw_len)
{
	int rl = 0;

	if(!e->enabled)
		return LINEED_PASSTHROUGH;

	/* Escape sequence parsing for arrows */
	if(e->esc_state == 1){
		if(byte == '['){
			e->esc_state = 2;
			return LINEED_CONSUMED;
		}
		e->esc_state = 0;
		/* fall through */
	} else if(e->esc_state == 2){
		e->esc_state = 0;
		switch(byte){
		case 'A': /* Up — history back */
			if(e->hist_pos < e->hist_count){
				e->hist_pos++;
				load_history_into_buf(e, e->hist_pos);
				if(redraw_buf)
					rl = build_redraw(e, redraw_buf, redraw_max);
			}
			if(redraw_len) *redraw_len = rl;
			return LINEED_CONSUMED;
		case 'B': /* Down — history forward */
			if(e->hist_pos > 0){
				e->hist_pos--;
				load_history_into_buf(e, e->hist_pos);
				if(redraw_buf)
					rl = build_redraw(e, redraw_buf, redraw_max);
			}
			if(redraw_len) *redraw_len = rl;
			return LINEED_CONSUMED;
		case 'C': /* Right */
			if(e->cursor < e->len){
				e->cursor++;
				if(redraw_buf && redraw_max >= 3){
					redraw_buf[0] = 0x1B;
					redraw_buf[1] = '[';
					redraw_buf[2] = 'C';
					rl = 3;
				}
			}
			if(redraw_len) *redraw_len = rl;
			return LINEED_CONSUMED;
		case 'D': /* Left */
			if(e->cursor > 0){
				e->cursor--;
				if(redraw_buf && redraw_max >= 3){
					redraw_buf[0] = 0x1B;
					redraw_buf[1] = '[';
					redraw_buf[2] = 'D';
					rl = 3;
				}
			}
			if(redraw_len) *redraw_len = rl;
			return LINEED_CONSUMED;
		case 'H': /* Home */
			e->cursor = 0;
			if(redraw_buf)
				rl = build_redraw(e, redraw_buf, redraw_max);
			if(redraw_len) *redraw_len = rl;
			return LINEED_CONSUMED;
		case 'F': /* End */
			e->cursor = e->len;
			if(redraw_buf)
				rl = build_redraw(e, redraw_buf, redraw_max);
			if(redraw_len) *redraw_len = rl;
			return LINEED_CONSUMED;
		}
		/* Unknown CSI — drop it */
		return LINEED_CONSUMED;
	}

	switch(byte){
	case 0x1B: /* ESC — start escape sequence */
		e->esc_state = 1;
		return LINEED_CONSUMED;

	case '\r':
	case '\n': /* Enter: complete the line */
		if(out_buf) *out_buf = e->buf;
		if(out_len) *out_len = e->len;
		/* Push to history */
		{
			char line[LINEED_HIST_LINE];
			int n = e->len;
			if(n >= LINEED_HIST_LINE) n = LINEED_HIST_LINE - 1;
			memmove(line, e->buf, n);
			line[n] = 0;
			if(n > 0) lined_push_history(e, line);
		}
		/* Echo the newline in redraw so user sees the prompt advance */
		if(redraw_buf && redraw_max >= 2){
			redraw_buf[0] = '\r';
			redraw_buf[1] = '\n';
			if(redraw_len) *redraw_len = 2;
		} else if(redraw_len) {
			*redraw_len = 0;
		}
		e->len = 0;
		e->cursor = 0;
		e->hist_pos = 0;
		return LINEED_COMPLETE;

	case 0x08: /* ^H: backspace */
	case 0x7F: /* DEL */
		if(e->cursor > 0){
			memmove(e->buf + e->cursor - 1, e->buf + e->cursor,
				e->len - e->cursor);
			e->cursor--;
			e->len--;
			if(redraw_buf)
				rl = build_redraw(e, redraw_buf, redraw_max);
		}
		if(redraw_len) *redraw_len = rl;
		e->hist_pos = 0;
		return LINEED_CONSUMED;

	case 0x01: /* ^A: home */
		e->cursor = 0;
		if(redraw_buf)
			rl = build_redraw(e, redraw_buf, redraw_max);
		if(redraw_len) *redraw_len = rl;
		return LINEED_CONSUMED;

	case 0x05: /* ^E: end */
		e->cursor = e->len;
		if(redraw_buf)
			rl = build_redraw(e, redraw_buf, redraw_max);
		if(redraw_len) *redraw_len = rl;
		return LINEED_CONSUMED;

	case 0x15: /* ^U: clear line */
		e->len = 0;
		e->cursor = 0;
		if(redraw_buf)
			rl = build_redraw(e, redraw_buf, redraw_max);
		if(redraw_len) *redraw_len = rl;
		e->hist_pos = 0;
		return LINEED_CONSUMED;

	case 0x17: /* ^W: delete word backwards */
		while(e->cursor > 0 && e->buf[e->cursor - 1] == ' '){
			memmove(e->buf + e->cursor - 1, e->buf + e->cursor,
				e->len - e->cursor);
			e->cursor--;
			e->len--;
		}
		while(e->cursor > 0 && e->buf[e->cursor - 1] != ' '){
			memmove(e->buf + e->cursor - 1, e->buf + e->cursor,
				e->len - e->cursor);
			e->cursor--;
			e->len--;
		}
		if(redraw_buf)
			rl = build_redraw(e, redraw_buf, redraw_max);
		if(redraw_len) *redraw_len = rl;
		e->hist_pos = 0;
		return LINEED_CONSUMED;
	}

	/* Printable byte (and other unhandled). Insert at cursor. */
	if(byte >= 0x20 && byte < 0x7F){
		int append_only = (e->cursor == e->len);
		if(e->len < LINEED_MAXBUF - 1){
			memmove(e->buf + e->cursor + 1, e->buf + e->cursor,
				e->len - e->cursor);
			e->buf[e->cursor] = byte;
			e->cursor++;
			e->len++;
		}
		if(redraw_buf){
			if(append_only){
				/* Fast path: cursor was at end, just emit the byte.
				 * Avoids the CR + erase-to-EOL dance in build_redraw
				 * which would wipe out the rc prompt sitting to the
				 * left of the editing region. */
				if(redraw_max >= 1){
					redraw_buf[0] = byte;
					rl = 1;
				}
			} else {
				rl = build_redraw(e, redraw_buf, redraw_max);
			}
		}
		if(redraw_len) *redraw_len = rl;
		e->hist_pos = 0;
		return LINEED_CONSUMED;
	}

	/* Unknown control char — pass through to rc. */
	return LINEED_PASSTHROUGH;
}
