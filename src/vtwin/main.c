/*
 * vtwin — libdraw client for vts sessions.
 *
 * Mounts /srv/vts, opens a session's cells stream, polls for diffs,
 * renders each cell with libdraw into a window.
 *
 * Architecture:
 *   main thread: opens window, allocates color images, paints
 *   cellpoll proc: reads /sess/cells in a loop, posts frames to a channel
 *   kbdproc / mouseproc: standard libdraw input procs (eventually forward
 *                        keystrokes to /sess/cons)
 *
 * Phase 3 v1: cells rendering + keyboard forwarding. Scroll/mouse-based
 * scrollback comes later.
 *
 * NOTE: this is a STANDALONE binary. mxio integration (one taskbar entry
 * per vtwin) is a future step.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <mouse.h>
#include <keyboard.h>

/* Wire format constants — must match vts/celldiff.h. */
enum {
	CD_MAGIC = 0x76746331,
	CD_HEADER_BYTES = 22,
	CD_CELL_BYTES = 12,
	CD_MAXFRAME = 1024 * 1024,
};

/* WinXP Luna 16-color palette (same as vts100-parsing.md). */
static ulong palette_rgb[16] = {
	0x000000FF,  /* 0  black */
	0xCD3131FF,  /* 1  red */
	0x1E9C40FF,  /* 2  green */
	0xD8A800FF,  /* 3  yellow */
	0x1F5CB7FF,  /* 4  blue */
	0xB833A4FF,  /* 5  magenta */
	0x2CB8B8FF,  /* 6  cyan */
	0xCCCCCCFF,  /* 7  white */
	0x5E5E5EFF,  /* 8  bright black */
	0xF14848FF,  /* 9  bright red */
	0x2BD84DFF,  /* 10 bright green */
	0xFFD341FF,  /* 11 bright yellow */
	0x4283D8FF,  /* 12 bright blue */
	0xE054CBFF,  /* 13 bright magenta */
	0x4ADADAFF,  /* 14 bright cyan */
	0xFFFFFFFF,  /* 15 bright white */
};

Image *colors[16];     /* fg images */
Image *bgcolors[16];   /* bg images, indexed by palette */
Font *cellfont;
int cellw, cellh;      /* per-cell pixel dimensions */
int gridrows, gridcols;

/* Tracks last seen mouse button mask so the mouse event handler
 * can detect button-press EDGES (wheel ticks) vs continuous state.
 * Plan 9 mouse events fire continuously; we need edges for scroll. */
static int lastmbuttons = 0;

/* Text selection state.
 *
 * sel_active: 1 while user is dragging or selection is "alive" (not
 *   yet superseded by new output). Selection survives until either a
 *   new drag starts or the cell at that position changes.
 *
 * sel_anchor: where the drag started (cell row/col).
 * sel_end:    current end of selection (updates as user drags).
 *
 * Stored in cell coordinates so resize/scroll don't invalidate them
 * automatically. We DO clear on session ctl reset.
 *
 * Rectangular selection (not stream): pi9's UI is line-oriented but
 * vtwin doesn't model logical lines, so we treat selection as a
 * rectangular cell range. Like xterm with alt+drag (default).
 *
 * Future improvement: stream selection (everything from anchor to end
 * including line wraps) is nicer for prose. Defer until pi9 actually
 * wraps long lines visibly. */
static int sel_active = 0;
static int sel_anchor_row = -1, sel_anchor_col = -1;
static int sel_end_row = -1, sel_end_col = -1;

/* If non-zero, log every mouse event to /tmp/vt-mouse.log for
 * diagnosing input issues. Set from $vtmouselog env var at startup. */
static int mousedebug = 0;

/* If non-zero, log every keyboard rune to /tmp/vt-key.log for
 * discovering what keys/key-combos produce on plan9 (e.g. to
 * find out what Cmd+C sends, if anything). Set from $vtkeylog. */
static int keydebug = 0;

/* Forward declarations for selection helpers (defined below). */
static int cell_in_selection(int row, int col);
static void selection_to_snarf(void);
static void paste_from_snarf(void);
static char* selection_text(int *outlen);
char *session = "1";
int consfd = -1;

/*
 * mxio (our window manager) paints a WinXP-style titlebar in the top
 * Titlebar pixels of every window that's tall enough. The app's screen
 * rect covers the WHOLE window image — drawing into that area would
 * overpaint the titlebar.
 *
 * mxio's layout (see src/mxio/wind.c):
 *   Selborder = 2 px window border on all 4 sides
 *   Titlebar  = 22 px tall gradient strip just inside the top border
 *   Content   = everything below the titlebar
 *
 * So vtwin's drawing origin must be at:
 *   origin.x = screen.min.x + Selborder + 1   (1 extra for the wcontentrect inset)
 *   origin.y = screen.min.y + Selborder + Titlebar  (skip border + titlebar)
 *
 * mxio also requires the window to be taller than Titlebar+20 = 42px
 * before it actually paints a titlebar — at smaller sizes the whole
 * interior is treated as content (panels, launchers). We mirror that
 * threshold so vtwin doesn't reserve space for a titlebar that isn't
 * there.
 *
 * Flags:
 *   -B    force "stock rio" mode: no titlebar offset (use whole window)
 *   -T n  override titlebar height
 */
#define MXIO_SELBORDER       2
#define MXIO_TITLEBAR_PX     22
#define MXIO_TITLE_THRESH    42   /* Dy(window) must exceed this for a titlebar */
static int titlebar_px_cfg = MXIO_TITLEBAR_PX;
static int titlebar_px = MXIO_TITLEBAR_PX;
static int side_inset  = MXIO_SELBORDER + 1;  /* matches wmk's Selborder+1 */
static int top_border  = MXIO_SELBORDER;       /* gap above titlebar */

/*
 * Decide whether the current window has a titlebar drawn by mxio.
 * Small windows (panels, launchers) don't get one — the whole interior
 * is content.
 */
static int
has_titlebar(void)
{
	if(titlebar_px_cfg == 0) return 0;
	if(screen == nil) return 0;
	return Dy(screen->r) > MXIO_TITLE_THRESH;
}

static Point
grid_origin(void)
{
	Point p = screen->r.min;
	p.x += side_inset;
	/* Skip top border + (titlebar if present) */
	p.y += top_border;
	if(has_titlebar())
		p.y += titlebar_px;
	else
		p.y += 1;  /* match wcontentrect's extra +1 inset */
	return p;
}

/* Cache of the last frame so we can repaint on window resize. */
typedef struct CellState CellState;
struct CellState {
	int rune;
	uchar fg, bg, attrs;
};
CellState *gridstate;   /* rows × cols */
int gridcur_row, gridcur_col, gridcur_vis;

Channel *frameschan;    /* chan(int): notifies main thread of new frame */
uchar lastframe[131072];
int lastframe_n;

static void
fatal(char *msg)
{
	fprint(2, "vtwin: %s: %r\n", msg);
	threadexitsall(msg);
}

static int
ldu16(uchar *p) { return p[0] | (p[1] << 8); }
static uint
ldu32(uchar *p) { return (uint)p[0] | ((uint)p[1] << 8) | ((uint)p[2] << 16) | ((uint)p[3] << 24); }

/* Draw one cell at (row, col). */
static void
draw_cell(int row, int col, CellState *c)
{
	Point origin = grid_origin();
	Point p;
	Rectangle r;
	Rune ru = c->rune ? c->rune : ' ';
	char buf[8];
	int fg = c->fg & 15;
	int bg = c->bg & 15;

	if(c->attrs & 4){ /* reverse */
		int t = fg; fg = bg; bg = t;
	}

	p = addpt(origin, Pt(col * cellw, row * cellh));
	r = Rect(p.x, p.y, p.x + cellw, p.y + cellh);

	/* Background */
	draw(screen, r, bgcolors[bg], nil, ZP);

	/* Foreground rune */
	if(ru != ' ' && ru != 0){
		int n = runetochar(buf, &ru);
		buf[n] = 0;
		string(screen, p, colors[fg], ZP, cellfont, buf);
	}

	/* Underline */
	if(c->attrs & 2){
		Rectangle ur = Rect(r.min.x, r.max.y - 2, r.max.x, r.max.y - 1);
		draw(screen, ur, colors[fg], nil, ZP);
	}

	/* Selection highlight: if this cell is inside [anchor..end],
	 * draw an inverted rect on top. We invert with display->black
	 * since the actual cell glyph is already drawn. SoverD blends
	 * for legibility. */
	if(sel_active && cell_in_selection(row, col)){
		draw(screen, r, display->white, nil, ZP);
		if(ru != ' ' && ru != 0){
			int sn = runetochar(buf, &ru);
			buf[sn] = 0;
			/* Re-draw the glyph in BG color (the cell's original
			 * background) over the white box. This gives the
			 * classic "selected text" look without us needing
			 * separate inverted color tables. */
			string(screen, p, display->black, ZP, cellfont, buf);
		}
	}
}

/* Selection helpers. cell_in_selection returns true if the given
 * cell row/col falls within [anchor..end]. Handles selections that
 * go in either direction (anchor below/above end). */
static int
cell_in_selection(int row, int col)
{
	int r0 = sel_anchor_row, r1 = sel_end_row;
	int c0 = sel_anchor_col, c1 = sel_end_col;
	if(r0 < 0 || r1 < 0) return 0;
	/* Normalize: r0/c0 = top-left, r1/c1 = bottom-right.
	 * For "line-major" stream selection (more natural for prose)
	 * we treat the selection as: every cell from
	 * (anchor) walking forward to (end) in reading order.
	 * Cell at (row, col) is selected if it's >= anchor and <= end
	 * in reading order. */
	int start_r, start_c, end_r, end_c;
	if(r0 < r1 || (r0 == r1 && c0 <= c1)){
		start_r = r0; start_c = c0;
		end_r = r1; end_c = c1;
	} else {
		start_r = r1; start_c = c1;
		end_r = r0; end_c = c0;
	}
	if(row < start_r || row > end_r) return 0;
	if(row == start_r && col < start_c) return 0;
	if(row == end_r && col > end_c) return 0;
	return 1;
}

/* Extract the selected text as UTF-8. Walks gridstate in reading
 * order (left to right, top to bottom) for cells inside the
 * selection. Returns a malloc'd buffer the caller must free.
 *
 * Trailing whitespace per row is trimmed (so selecting a multi-line
 * region doesn't include the right-pad of empty cells). */
static char*
selection_text(int *outlen)
{
	int start_r, start_c, end_r, end_c;
	int r0 = sel_anchor_row, r1 = sel_end_row;
	int c0 = sel_anchor_col, c1 = sel_end_col;
	if(r0 < 0 || r1 < 0){
		*outlen = 0;
		return strdup("");
	}
	if(r0 < r1 || (r0 == r1 && c0 <= c1)){
		start_r = r0; start_c = c0;
		end_r = r1; end_c = c1;
	} else {
		start_r = r1; start_c = c1;
		end_r = r0; end_c = c0;
	}
	if(start_r < 0) start_r = 0;
	if(end_r >= gridrows) end_r = gridrows - 1;

	/* Worst case: 4 bytes per rune * cells in range + newlines. */
	int maxn = (end_r - start_r + 1) * gridcols * 4 + (end_r - start_r + 1);
	char *buf = mallocz(maxn + 1, 1);
	char *p = buf;

	for(int row = start_r; row <= end_r; row++){
		int lo = (row == start_r) ? start_c : 0;
		int hi = (row == end_r) ? end_c : gridcols - 1;
		if(lo < 0) lo = 0;
		if(hi >= gridcols) hi = gridcols - 1;
		/* Find rightmost non-space cell in [lo..hi] for trimming. */
		int last = lo - 1;
		for(int col = lo; col <= hi; col++){
			Rune ru = gridstate[row * gridcols + col].rune;
			if(ru != 0 && ru != ' ') last = col;
		}
		for(int col = lo; col <= last; col++){
			Rune ru = gridstate[row * gridcols + col].rune;
			if(ru == 0) ru = ' ';
			p += runetochar(p, &ru);
		}
		if(row < end_r) *p++ = '\n';
	}
	*p = 0;
	*outlen = p - buf;
	return buf;
}

/* Write the current selection to /dev/snarf (plan9's clipboard).
 * No-op if no selection.
 *
 * Plan 9 convention: open /dev/snarf O_WRITE, write the bytes, close.
 * The snarf server (rio) handles the replace atomically. DO NOT use
 * OTRUNC — /dev/snarf is a device file, not a regular file, and
 * OTRUNC is undefined for it. Earlier versions had `OWRITE|OTRUNC`
 * which silently broke the open on at least some rio versions. */
static void
selection_to_snarf(void)
{
	int n;
	char *txt = selection_text(&n);

	/* Debug log so users can verify what got written (or didn't).
	 * Disabled by default; -K flag enables both this and the key log
	 * for diagnosing input issues. */
	if(keydebug){
		int lf = open("/tmp/vt-snarf.log", OWRITE);
		if(lf < 0)
			lf = create("/tmp/vt-snarf.log", OWRITE, 0666);
		if(lf >= 0){
			seek(lf, 0, 2);  /* append */
			fprint(lf, "selection_to_snarf: n=%d sel=(r%d,c%d)->(r%d,c%d) active=%d\n",
				n, sel_anchor_row, sel_anchor_col,
				sel_end_row, sel_end_col, sel_active);
			if(n > 0)
				fprint(lf, "  text: %.*s\n", n, txt);
			close(lf);
		}
	}

	if(n > 0){
		int sf = open("/dev/snarf", OWRITE);
		if(sf >= 0){
			write(sf, txt, n);
			close(sf);
		} else if(keydebug){
			int lf = open("/tmp/vt-snarf.log", OWRITE);
			if(lf < 0)
				lf = create("/tmp/vt-snarf.log", OWRITE, 0666);
			if(lf >= 0){
				seek(lf, 0, 2);
				fprint(lf, "  open /dev/snarf failed: %r\n");
				close(lf);
			}
		}
	}
	free(txt);
}

/* Read /dev/snarf and write its contents to consfd (the session's
 * keyboard input). This is how paste reaches the running terminal
 * app: it sees the snarf text as if the user had typed it.
 *
 * Truncates at 8KB so a giant paste doesn't lock things up. Apps
 * that need bigger paste would have to read /dev/snarf themselves. */
static void
paste_from_snarf(void)
{
	if(consfd < 0) return;
	int sf = open("/dev/snarf", OREAD);
	if(sf < 0) return;
	char buf[8192];
	int n = readn(sf, buf, sizeof buf);
	close(sf);
	if(n > 0){
		write(consfd, buf, n);
	}
}

static void
draw_cursor(void)
{
	Point origin = grid_origin();
	Rectangle r;
	Point p;
	if(!gridcur_vis) return;
	if(gridcur_row < 0 || gridcur_row >= gridrows) return;
	if(gridcur_col < 0 || gridcur_col >= gridcols) return;

	p = addpt(origin, Pt(gridcur_col * cellw, gridcur_row * cellh));
	r = Rect(p.x, p.y + cellh - 2, p.x + cellw, p.y + cellh);
	draw(screen, r, colors[7], nil, ZP);
}

/*
 * Recompute gridrows/gridcols from the current screen rectangle.
 * Returns 1 if dimensions changed, 0 otherwise.
 *
 * The grid origin sits just below the titlebar (see grid_origin()) and
 * the grid takes everything below it up to a small margin from the
 * bottom/right edges.
 */
static int
recalc_grid(void)
{
	Point origin = grid_origin();
	int avail_w = screen->r.max.x - origin.x;
	int avail_h = screen->r.max.y - origin.y;
	int newcols = avail_w / cellw;
	int newrows = avail_h / cellh;
	int i;

	if(newcols < 1) newcols = 1;
	if(newrows < 1) newrows = 1;
	if(newcols == gridcols && newrows == gridrows && gridstate != nil)
		return 0;
	gridcols = newcols;
	gridrows = newrows;
	free(gridstate);
	gridstate = mallocz(gridrows * gridcols * sizeof(CellState), 1);
	for(i = 0; i < gridrows * gridcols; i++){
		gridstate[i].rune = ' ';
		gridstate[i].fg = 7;
		gridstate[i].bg = 0;
	}
	return 1;
}

/*
 * Tell vts to resize the session's cell buffer to match our grid.
 * Writes "size R C\n" to the session's ctl file.
 */
static void
notify_vts_size(void)
{
	char path[128];
	char buf[64];
	int fd, n;
	snprint(path, sizeof path, "/n/vts/%s/ctl", session);
	fd = open(path, OWRITE);
	if(fd < 0) return;
	n = snprint(buf, sizeof buf, "size %d %d\n", gridrows, gridcols);
	write(fd, buf, n);
	close(fd);
}

/*
 * Tell vts to mark every cell dirty so the next cells read returns a
 * full repaint. Used when vtwin's display state is suspect — at
 * startup, on focus gain, after a window resize.
 */
static void
notify_vts_redraw(void)
{
	char path[128];
	int fd;
	snprint(path, sizeof path, "/n/vts/%s/ctl", session);
	fd = open(path, OWRITE);
	if(fd < 0) return;
	write(fd, "redraw\n", 7);
	close(fd);
}

static void
repaint_all(void)
{
	int r, c;
	Point origin = grid_origin();
	Rectangle paintrect;

	/*
	 * Fill the ENTIRE area below the titlebar (and inside the border)
	 * with black. mxio's initial wmk paints the content area white
	 * (cols[BACK]); we want our cells area to be black, including the
	 * margins between the grid's bottom/right and the window edge.
	 *
	 * IMPORTANT: do NOT touch the top `titlebar_px` pixels — those
	 * belong to mxio's gradient titlebar.
	 */
	paintrect.min.x = screen->r.min.x + side_inset;
	paintrect.min.y = origin.y;
	paintrect.max.x = screen->r.max.x - side_inset;
	paintrect.max.y = screen->r.max.y - side_inset;
	if(paintrect.min.x < screen->r.min.x) paintrect.min.x = screen->r.min.x;
	if(paintrect.max.x > screen->r.max.x) paintrect.max.x = screen->r.max.x;
	if(paintrect.max.y > screen->r.max.y) paintrect.max.y = screen->r.max.y;
	if(paintrect.max.x > paintrect.min.x && paintrect.max.y > paintrect.min.y)
		draw(screen, paintrect, bgcolors[0], nil, ZP);

	for(r = 0; r < gridrows; r++)
		for(c = 0; c < gridcols; c++)
			draw_cell(r, c, &gridstate[r * gridcols + c]);
	draw_cursor();
	flushimage(display, 1);
}

/*
 * Process a single cells frame buffer of `n` bytes. Updates gridstate
 * and draws changed cells.
 */
static void
process_frame(uchar *buf, int n)
{
	uint magic;
	int rows, cols, ncells, cur_row, cur_col, vis, i;
	uchar *p;
	int prev_cur_row, prev_cur_col;
	static int frame_n = 0;
	static int dbgfd = -1;
	if(dbgfd == -1){
		dbgfd = open("/tmp/vtwin-render.log", OWRITE|OTRUNC|OCEXEC);
		if(dbgfd < 0) dbgfd = create("/tmp/vtwin-render.log", OWRITE, 0666);
	}
	frame_n++;

	if(n < CD_HEADER_BYTES) return;
	magic = ldu32(buf);
	if(magic != CD_MAGIC){
		fprint(2, "vtwin: bad magic 0x%08ux\n", magic);
		return;
	}
	rows = ldu16(buf + 6);
	cols = ldu16(buf + 8);
	ncells = ldu32(buf + 10);
	cur_row = ldu16(buf + 14);
	cur_col = ldu16(buf + 16);
	vis = buf[18];

	if(dbgfd >= 0 && (frame_n < 5 || ncells > 0))
		fprint(dbgfd, "f%d: rows=%d cols=%d ncells=%d cur=(%d,%d) cellw=%d cellh=%d sR=(%d,%d)-(%d,%d)\n",
			frame_n, rows, cols, ncells, cur_row, cur_col,
			cellw, cellh,
			screen->r.min.x, screen->r.min.y,
			screen->r.max.x, screen->r.max.y);

	/* If grid dims changed, reallocate */
	if(rows != gridrows || cols != gridcols){
		free(gridstate);
		gridstate = mallocz(rows * cols * sizeof(CellState), 1);
		gridrows = rows;
		gridcols = cols;
		for(i = 0; i < rows * cols; i++){
			gridstate[i].rune = ' ';
			gridstate[i].fg = 7;
			gridstate[i].bg = 0;
		}
		repaint_all();
	}

	/* Erase the old cursor cell (redraw without cursor) */
	prev_cur_row = gridcur_row;
	prev_cur_col = gridcur_col;
	if(gridcur_vis && prev_cur_row >= 0 && prev_cur_row < gridrows &&
	   prev_cur_col >= 0 && prev_cur_col < gridcols){
		draw_cell(prev_cur_row, prev_cur_col,
			&gridstate[prev_cur_row * gridcols + prev_cur_col]);
	}

	p = buf + CD_HEADER_BYTES;
	for(i = 0; i < ncells; i++){
		int row, col;
		if((p - buf) + CD_CELL_BYTES > n) break;
		row = ldu16(p + 0);
		col = ldu16(p + 2);
		if(row >= 0 && row < gridrows && col >= 0 && col < gridcols){
			CellState *c = &gridstate[row * gridcols + col];
			c->rune = (int)ldu32(p + 4);
			c->fg = p[8];
			c->bg = p[9];
			c->attrs = p[10];
			draw_cell(row, col, c);
		}
		p += CD_CELL_BYTES;
	}

	gridcur_row = cur_row;
	gridcur_col = cur_col;
	gridcur_vis = vis;
	draw_cursor();

	flushimage(display, 1);
}

/*
 * Triggered after a short delay (so mxio's wmk has fully completed),
 * writes 'current' to /dev/wctl. This causes mxio to dispatch a
 * Topped ctl message to vtwin's winctl thread, which calls wrepaint
 * → wborder → wdrawtitlebar → flushimage. That path runs in the
 * proper draw-thread context inside mxio so all 22 gradient rows
 * commit cleanly — fixing the "squished 2-pixel titlebar" bug.
 */
static void
titlebar_kicker(void *arg)
{
	int wfd;
	USED(arg);
	sleep(200);
	wfd = open("/dev/wctl", OWRITE);
	if(wfd >= 0){
		write(wfd, "current\n", 8);
		close(wfd);
	}
	threadexits(nil);
}

/*
 * Cell poll proc — own kernel proc so blocking reads don't stall main.
 * Reads /n/vts/<sess>/cells repeatedly, processing each diff frame.
 */
static void
cellpoll(void *arg)
{
	char *path = (char*)arg;
	int fd;
	/* Static because 128KB on a 32KB proc stack would smash it. */
	static uchar buf[131072];
	int n;
	int frame_count = 0;
	int dbgfd = open("/tmp/vtwin.log", OWRITE|OTRUNC|OCEXEC);
	if(dbgfd < 0)
		dbgfd = create("/tmp/vtwin.log", OWRITE, 0666);
	if(dbgfd >= 0)
		fprint(dbgfd, "vtwin: cellpoll started, path=%s\n", path);

	for(;;){
		fd = open(path, OREAD);
		if(fd < 0){
			if(dbgfd >= 0) fprint(dbgfd, "open failed: %r\n");
			sleep(500);
			continue;
		}
		n = read(fd, buf, sizeof buf);
		close(fd);
		if(n >= CD_HEADER_BYTES){
			int ncells = ldu32(buf + 10);
			frame_count++;
			if(dbgfd >= 0 && (frame_count < 5 || ncells > 0))
				fprint(dbgfd, "frame %d: n=%d ncells=%d\n", frame_count, n, ncells);
			memmove(lastframe, buf, n);
			lastframe_n = n;
			sendul(frameschan, (ulong)n);
		} else if(n > 0 && dbgfd >= 0){
			fprint(dbgfd, "short read: %d\n", n);
		}
		sleep(40);
	}
}

/*
 * Keyboard proc: read /dev/cons-like events, forward to /sess/cons.
 * For now, just read directly from the keyboard channel via libdraw.
 */
static Channel *kbdc;

static void
kbdthread(void *arg)
{
	Rune r;
	char buf[8];
	int n;
	USED(arg);
	for(;;){
		r = (Rune)(uintptr)recvp(kbdc);
		if(consfd < 0) continue;
		n = runetochar(buf, &r);
		write(consfd, buf, n);
	}
}

static int
init_kbd_thread(void)
{
	Keyboardctl *kc;
	kc = initkeyboard(nil);
	if(kc == nil) return -1;

	threadcreate((void(*)(void*))kbdthread, kc, 16*1024);

	/* Spawn a relay proc that puts keyboard runes onto kbdc */
	/* Actually simpler: do this inline in the main loop using alt() */
	return 0;
}

void
threadmain(int argc, char **argv)
{
	int i;
	char cells_path[256], cons_path[256];
	Keyboardctl *kbd;
	Mousectl *mctl;
	Rune r;
	char *fontflag = nil; /* -f <path> overrides $font + default */

	ARGBEGIN{
	case 'B':
		/* Stock rio mode: no WinXP titlebar to leave room for. */
		titlebar_px_cfg = 0;
		titlebar_px = 0;
		side_inset = 0;
		top_border = 0;
		break;
	case 'T':
		titlebar_px_cfg = atoi(ARGF());
		titlebar_px = titlebar_px_cfg;
		break;
	case 'f':
		/* Explicit font path. Takes precedence over $font and the
		 * libdraw default. Lets `new-pi9 -f <path>` propagate a
		 * font choice to vtwin without futzing with rio's env. */
		fontflag = ARGF();
		break;
	case 'K':
		/* -K: enable keyboard rune logging to /tmp/vt-key.log.
		 * Same effect as $vtkeylog=1 but reliable: env doesn't
		 * propagate through `window`, but CLI flags do. */
		keydebug = 1;
		break;
	case 'M':
		/* -M: enable mouse event logging to /tmp/vt-mouse.log. */
		mousedebug = 1;
		break;
	}ARGEND;

	if(argc > 0)
		session = argv[0];

	if(initdraw(nil, nil, "vtwin") < 0)
		fatal("initdraw");

	/* Claim /dev/mouse and /dev/cons EARLY so we fail fast if we're being
	 * launched into a namespace that already has them (e.g. user typed
	 * `vtwin` inside an existing vtwin instead of using `window vtwin`
	 * from the start menu, which would create a fresh namespace). Bail
	 * before we allocate window images, mount vts, or spawn cellpoll —
	 * otherwise mxio leaves a white orphan window on the screen until the
	 * next redraw. */
	mctl = initmouse(nil, screen);
	if(mctl == nil)
		fatal("init mouse (already taken? try `window vtwin` instead of `vtwin`)");
	kbd = initkeyboard(nil);
	if(kbd == nil)
		fatal("init keyboard");
	/* Note: libdraw initkeyboard() already opens /dev/consctl and writes
	 * "rawon", so each keystroke is delivered individually via kbd->c —
	 * no need for a separate consctl write here. */

	/* libdraw's getwindow() will white-fill our client image (covering
	 * mxio's gradient titlebar). titlebar_kicker() runs on a short
	 * delay and writes "current" to /dev/wctl so mxio re-paints the
	 * titlebar in its own draw-thread context. We also re-trigger the
	 * same path on every Aresize below, so drag/resize keep working. */

	/* If non-zero, log every mouse event to /tmp/vt-mouse.log for
	 * diagnosing input issues. Set from $vtmouselog env var at startup. */
	{
		char *md = getenv("vtmouselog");
		if(md != nil && *md != '\0' && *md != '0'){
			mousedebug = 1;
		}
		if(md != nil) free(md);
	}

	/* Same for keyboard. Set $vtkeylog=1 to discover what Cmd+C and
	 * other unusual key combos produce on your specific plan9/QEMU
	 * setup — we may be able to wire them once we know the rune. */
	{
		char *kd = getenv("vtkeylog");
		if(kd != nil && *kd != '\0' && *kd != '0'){
			keydebug = 1;
		}
		if(kd != nil) free(kd);
	}

	/* Font selection. Precedence (highest first):
	 *   1. -f <path>      CLI flag (Phase 4: lets new-pi9 specify)
	 *   2. $font          env var — rio/9term/acme honour the same one
	 *   3. defaultfont    libdraw's built-in (whatever initdraw picked)
	 * fontsrv-served TTFs (e.g. /n/font/Inconsolata/16a/font) work
	 * transparently — they are just subfont paths to libdraw. See
	 * wiki/concepts/vtwin-typography.md. */
	cellfont = nil;
	if(fontflag != nil && *fontflag != '\0'){
		cellfont = openfont(display, fontflag);
		if(cellfont == nil)
			fprint(2, "vtwin: openfont -f %s: %r (falling back)\n", fontflag);
	}
	if(cellfont == nil){
		char *fn = getenv("font");
		if(fn != nil && *fn != '\0'){
			cellfont = openfont(display, fn);
			if(cellfont == nil)
				fprint(2, "vtwin: openfont $font=%s: %r (falling back to defaultfont)\n", fn);
			free(fn);
		}
	}
	if(cellfont == nil)
		cellfont = display->defaultfont;
	cellw = stringwidth(cellfont, "M");
	cellh = cellfont->height;
	if(cellw < 6) cellw = 8;
	if(cellh < 10) cellh = 14;

	/* Allocate color images. */
	for(i = 0; i < 16; i++){
		colors[i] = allocimage(display, Rect(0,0,1,1), screen->chan, 1, palette_rgb[i]);
		bgcolors[i] = colors[i];
		if(colors[i] == nil) fatal("allocimage");
	}

	/* Mount vts */
	{
		int sfd = open("/srv/vts", ORDWR);
		if(sfd < 0) fatal("open /srv/vts");
		if(mount(sfd, -1, "/n/vts", MREPL, "") < 0){
			/* may already be mounted; that's fine */
		}
	}

	snprint(cells_path, sizeof cells_path, "/n/vts/%s/cells", session);
	snprint(cons_path, sizeof cons_path, "/n/vts/%s/cons", session);

	consfd = open(cons_path, OWRITE);
	if(consfd < 0)
		fprint(2, "vtwin: warning: can't open cons: %r\n");

	/* Initialize grid state with current screen dimensions. */
	gridstate = nil;
	gridrows = gridcols = 0;
	recalc_grid();
	gridcur_vis = 1;
	notify_vts_size();
	notify_vts_redraw();  /* force vts to re-emit all cells */
	repaint_all();

	frameschan = chancreate(sizeof(ulong), 1);

	/* Spawn cellpoll in a separate proc */
	proccreate(cellpoll, cells_path, 32*1024);

	/* Kick mxio to repaint our titlebar after a delay. Runs concurrently
	 * with the main alt loop. See titlebar_kicker() for rationale. */
	proccreate(titlebar_kicker, nil, 16*1024);

	enum { Akbd, Aframes, Aresize, Amouse, Aend };
	Alt alts[Aend + 1] = {
		[Akbd]    = { kbd->c, &r, CHANRCV },
		[Aframes] = { frameschan, nil, CHANRCV },
		[Aresize] = { mctl->resizec, nil, CHANRCV },
		[Amouse]  = { mctl->c, nil, CHANRCV },
		[Aend]    = { nil, nil, CHANEND },
	};

	for(;;){
		switch(alt(alts)){
		case Akbd:
			if(consfd >= 0){
				/* Optional debug: log every received rune so we can
				 * discover what key combos like Cmd+C produce on
				 * plan9 (depends on QEMU keymap and rio passthrough).
				 * Enable with $vtkeylog=1. */
				if(keydebug){
					int lf = open("/tmp/vt-key.log", OWRITE);
					if(lf < 0)
						lf = create("/tmp/vt-key.log", OWRITE, 0666);
					if(lf >= 0){
						seek(lf, 0, 2);  /* append */
						fprint(lf, "rune=0x%04x dec=%d", r, r);
						if(r >= 0x20 && r < 0x7f) fprint(lf, " char='%C'", r);
						fprint(lf, "\n");
						close(lf);
					}
				}

				/* Intercept selection/clipboard keys BEFORE writing
				 * to cons (don't want pi9 to see them as input).
				 *
				 * Kins (Insert): paste from /dev/snarf. Standard
				 *   xterm/Windows convention. Works on plan9 if you
				 *   have an Insert key (Macbooks don't).
				 *
				 * KF|2 (F2): copy current selection to /dev/snarf.
				 *   Universal — works on every keyboard.
				 *
				 * 0x16 (^V): paste. On Mac+VNC, Cmd+V arrives here
				 *   because the VNC client maps Cmd → Ctrl. This
				 *   is the most user-friendly Mac path. ^V isn't
				 *   bound to anything in pi9/bubbletea so hijacking
				 *   is safe.
				 *
				 * 0x03 (^C): copy IF a selection is active; otherwise
				 *   pass through to pi9 as the normal SIGINT-style
				 *   interrupt (pi9's status bar says "ctrl-c to quit").
				 *   This gives Mac users Cmd+C → copy when text is
				 *   selected, while preserving the quit shortcut
				 *   when nothing is selected.
				 *
				 * 0x2229 (∩) / 0x221A (√): Mac Option+C/V symbols.
				 *   Kept in case some VNC clients DO pass Option
				 *   through unmapped, in which case these arrive
				 *   instead of nothing.
				 *
				 * Enable -K flag to discover what other key combos
				 * produce on your specific keyboard/VNC client. */
				if(r == Kins){
					paste_from_snarf();
					break;
				}
				if(r == (KF|2)){
					selection_to_snarf();
					break;
				}
				if(r == (KF|3)){
					/* F3: paste from /dev/snarf. Macbook-friendly
					 * alternative to Insert (which Macbooks don't
					 * have). F2 = copy, F3 = paste — adjacent keys. */
					paste_from_snarf();
					break;
				}
				if(r == 0x16){  /* ^V / Mac Cmd+V */
					paste_from_snarf();
					break;
				}
				if(r == 0x03 && sel_active && sel_anchor_row >= 0 &&
				   !(sel_anchor_row == sel_end_row && sel_anchor_col == sel_end_col)){
					/* ^C / Mac Cmd+C with a NON-EMPTY selection:
					 * copy. A "selection" of one cell (anchor ==
					 * end) is treated as no selection, so single
					 * left-clicks don't break the Cmd+C quit shortcut. */
					selection_to_snarf();
					break;
				}
				if(r == 0x2229){  /* ∩ = Mac Option+C (rare) */
					selection_to_snarf();
					break;
				}
				if(r == 0x221A){  /* √ = Mac Option+V (rare) */
					paste_from_snarf();
					break;
				}

				/* Plan 9 sends arrow keys + special keys as runes
				 * in the 0xF000-0xF8FF private-use space (Kup, Kdown,
				 * Kleft, Kright, Khome, Kend, Kpgup, Kpgdown, Kdel,
				 * etc — see <keyboard.h>). Terminal apps that speak
				 * xterm sequences (bubbletea, ncurses, vim) won't
				 * recognize the raw runes — they expect ANSI CSI
				 * escapes like ESC [ A.
				 *
				 * Translate here before writing to cons so any
				 * terminal app inside vts gets xterm-compatible
				 * input. Falls through to runetochar for normal
				 * printable runes. */
				char *seq = nil;
				switch(r){
				case Kup:     seq = "\x1b[A"; break;
				case Kdown:   seq = "\x1b[B"; break;
				case Kright:  seq = "\x1b[C"; break;
				case Kleft:   seq = "\x1b[D"; break;
				case Khome:   seq = "\x1b[H"; break;
				case Kend:    seq = "\x1b[F"; break;
				case Kpgup:   seq = "\x1b[5~"; break;
				case Kpgdown: seq = "\x1b[6~"; break;
				case Kdel:    seq = "\x1b[3~"; break;
				/* Kins handled above as paste; don't forward */
				/* F2 handled above as copy; F3 as paste */
				case KF|1:    seq = "\x1bOP"; break;
				case KF|4:    seq = "\x1bOS"; break;
				case KF|5:    seq = "\x1b[15~"; break;
				case KF|6:    seq = "\x1b[17~"; break;
				case KF|7:    seq = "\x1b[18~"; break;
				case KF|8:    seq = "\x1b[19~"; break;
				case KF|9:    seq = "\x1b[20~"; break;
				case KF|10:   seq = "\x1b[21~"; break;
				case KF|11:   seq = "\x1b[23~"; break;
				case KF|12:   seq = "\x1b[24~"; break;
				}
				if(seq != nil){
					write(consfd, seq, strlen(seq));
				} else {
					char buf[8];
					int n = runetochar(buf, &r);
					write(consfd, buf, n);
				}
			}
			break;
		case Aframes:
			/* Use the stashed buffer */
			process_frame(lastframe, lastframe_n);
			break;
		case Aresize:
			/* libdraw's getwindow() unconditionally allocates a new
			 * client sub-image filled with DWhite (see gengetwindow
			 * in /sys/src/libdraw/init.c). That sub-image's rect is
			 * insetrect(master, Borderwidth=4), which OVERLAPS the
			 * 22px titlebar zone — so any titlebar gradient mxio just
			 * painted onto the master gets wiped to white the moment
			 * we call getwindow. We can't avoid the wipe (libdraw
			 * hardcodes the fill colour), so we instead trigger mxio
			 * to repaint the titlebar AFTER getwindow has done its
			 * damage. Writing "current" to /dev/wctl dispatches a
			 * Repaint ctl message in mxio, which lands in the proper
			 * draw-thread context and re-paints border + titlebar
			 * via wborder/wdrawtitlebar. Same mechanism as
			 * titlebar_kicker(), just invoked on every resize. */
			if(getwindow(display, Refbackup) < 0) fatal("getwindow");
			if(recalc_grid())
				notify_vts_size();
			notify_vts_redraw();  /* whatever changed, re-emit all */
			repaint_all();
			{
				int wfd = open("/dev/wctl", OWRITE);
				if(wfd >= 0){
					write(wfd, "current\n", 8);
					close(wfd);
				}
			}
			break;
		case Amouse:
			/* Mouse handler: covers wheel scroll, button-1 drag for
			 * text selection (with copy-to-snarf on release), and
			 * button-2 click for paste-from-snarf. This is the
			 * standard plan9 terminal convention.
			 *
			 * Wheel: rio reports wheel-up as button 8, wheel-down
			 * as button 16 in mctl->buttons. Translate to xterm SGR
			 * mouse sequences so terminal apps (pi9, vim) can react.
			 *
			 * Button 1 (left): pressed → start selection at cell.
			 *   While held: extend to current cell, redraw with
			 *   highlight.
			 *   Released: extract selected text → /dev/snarf.
			 *
			 * Button 2 (middle): click → paste /dev/snarf into
			 *   consfd. App sees pasted text as keystrokes.
			 *
			 * Plan 9 doesn't fire a separate mouse event PER click —
			 * mctl->Mouse is updated continuously. We compare against
			 * lastmbuttons to detect EDGES (press vs release vs
			 * still-held). */
			{
				int btn = mctl->buttons;
				int delta_press = btn & ~lastmbuttons;
				int delta_release = lastmbuttons & ~btn;
				Point mo = grid_origin();
				int col = (mctl->xy.x - mo.x) / cellw;
				int row = (mctl->xy.y - mo.y) / cellh;

				/* Optional debug: set $vtmouselog=1 in env to write
				 * every mouse event to /tmp/vt-mouse.log. Useful for
				 * diagnosing real-mouse vs QMP-simulated input.
				 * Disabled by default — overhead is tiny but writes
				 * accumulate. */
				if(mousedebug){
					int lf = open("/tmp/vt-mouse.log", OWRITE);
					if(lf < 0)
						lf = create("/tmp/vt-mouse.log", OWRITE, 0666);
					if(lf >= 0){
						seek(lf, 0, 2);  /* append */
						fprint(lf, "btn=0x%x last=0x%x press=0x%x rel=0x%x xy=(%d,%d) cell=(%d,%d)\n",
							btn, lastmbuttons, delta_press, delta_release,
							mctl->xy.x, mctl->xy.y, col, row);
						close(lf);
					}
				}

				/* Clamp to grid bounds — clicks outside the cell
				 * area get clamped to the edge. */
				if(col < 0) col = 0;
				if(row < 0) row = 0;
				if(col >= gridcols) col = gridcols - 1;
				if(row >= gridrows) row = gridrows - 1;

				/* Wheel ticks — cell coords 1-based for xterm. */
				if(delta_press & 8){     /* wheel up */
					char buf[32];
					int n = snprint(buf, sizeof buf, "\x1b[<64;%d;%dM", col+1, row+1);
					if(consfd >= 0) write(consfd, buf, n);
				}
				if(delta_press & 16){    /* wheel down */
					char buf[32];
					int n = snprint(buf, sizeof buf, "\x1b[<65;%d;%dM", col+1, row+1);
					if(consfd >= 0) write(consfd, buf, n);
				}

				/* Button 1 press: start selection */
				if(delta_press & 1){
					sel_active = 1;
					sel_anchor_row = row;
					sel_anchor_col = col;
					sel_end_row = row;
					sel_end_col = col;
					repaint_all();
				}
				/* Button 1 held + moved: extend selection */
				else if((btn & 1) && (sel_end_row != row || sel_end_col != col)){
					sel_end_row = row;
					sel_end_col = col;
					repaint_all();
				}
				/* Button 1 released: copy selection to snarf */
				if(delta_release & 1){
					selection_to_snarf();
					/* Keep selection visible after release — like rio.
					 * It clears when next output arrives at those cells
					 * (we don't track that; it stays until next drag). */
				}

				/* Button 2 click: paste */
				if(delta_press & 2){
					paste_from_snarf();
				}

				lastmbuttons = btn;
			}
			break;
		}
	}
}
