/*
 * vts-attach — connect a dumb TTY (SSH, drawterm dumb console) to a vts
 * session.
 *
 * Two halves:
 *   - reader: mount /srv/vts, open the session's cells stream, decode
 *     each frame into VT100 escape sequences, write to stdout
 *   - writer: read stdin (raw keystrokes from the remote terminal),
 *     write each chunk to the session's cons file
 *
 * Phase 4.1: reader only (no stdin handling yet). Useful for "tail -f"
 * style viewing of a session from SSH.
 *
 * Phase 4.2 will add the writer half + tty raw mode.
 */
#include <u.h>
#include <libc.h>

/* Wire format constants — must match celldiff.h in vts. */
enum {
	CD_MAGIC = 0x76746331,
	CD_HEADER_BYTES = 22,
	CD_CELL_BYTES = 12,
	CD_MAXFRAME = 1024 * 1024,
};

/* ANSI escape helpers */
static int
emit(const char *s)
{
	return write(1, s, strlen(s));
}

static void
emitf(char *fmt, ...)
{
	char buf[256];
	va_list a;
	va_start(a, fmt);
	vseprint(buf, buf+sizeof buf, fmt, a);
	va_end(a);
	write(1, buf, strlen(buf));
}

/* Read exactly n bytes, returning -1 on EOF/error. */
static int
readall(int fd, uchar *buf, int n)
{
	int got = 0, r;
	while(got < n){
		r = read(fd, buf+got, n-got);
		if(r <= 0) return -1;
		got += r;
	}
	return got;
}

/* Decoder state — remember the last SGR emitted so we don't redundantly
 * issue the same escape codes. */
static int last_fg = -1, last_bg = -1, last_attrs = -1;

static int
ansi_fg(int idx)
{
	if(idx < 8) return 30 + idx;
	return 90 + (idx - 8);
}

static int
ansi_bg(int idx)
{
	if(idx < 8) return 40 + idx;
	return 100 + (idx - 8);
}

static void
emit_sgr(int fg, int bg, int attrs)
{
	if(fg == last_fg && bg == last_bg && attrs == last_attrs)
		return;
	/* Reset then apply — simple and idempotent. */
	emitf("\x1b[0;%d;%dm", ansi_fg(fg), ansi_bg(bg));
	if(attrs & 1) emit("\x1b[1m");  /* bold */
	if(attrs & 2) emit("\x1b[4m");  /* underline */
	if(attrs & 4) emit("\x1b[7m");  /* reverse */
	last_fg = fg;
	last_bg = bg;
	last_attrs = attrs;
}

/* Decode u16/u32 little-endian. */
static int
ldu16(uchar *p)
{
	return p[0] | (p[1] << 8);
}

static uint
ldu32(uchar *p)
{
	return (uint)p[0] | ((uint)p[1] << 8) | ((uint)p[2] << 16) | ((uint)p[3] << 24);
}

/* Emit a UTF-8 sequence for a code point. */
static void
emit_rune(int r)
{
	uchar buf[5];
	int n;

	if(r < 0x80){
		buf[0] = r;
		n = 1;
	} else if(r < 0x800){
		buf[0] = 0xC0 | (r >> 6);
		buf[1] = 0x80 | (r & 0x3F);
		n = 2;
	} else if(r < 0x10000){
		buf[0] = 0xE0 | (r >> 12);
		buf[1] = 0x80 | ((r >> 6) & 0x3F);
		buf[2] = 0x80 | (r & 0x3F);
		n = 3;
	} else {
		buf[0] = 0xF0 | (r >> 18);
		buf[1] = 0x80 | ((r >> 12) & 0x3F);
		buf[2] = 0x80 | ((r >> 6) & 0x3F);
		buf[3] = 0x80 | (r & 0x3F);
		n = 4;
	}
	write(1, buf, n);
}

/*
 * Decode one frame. Returns 0 on success, -1 on protocol error.
 *
 * Strategy: issue ONE large read which lib9p delivers to our daemon as a
 * single Tread (capped at msize). The daemon's encoder fills as much as
 * fits — typically header + ~600 cells for an 8KB msize. Parse the buffer
 * locally.
 */
static int
decode_frame(int fd)
{
	uchar buf[16384];
	int n, rows, cols, ncells, cur_row, cur_col, vis, i;
	uint magic;
	uchar *cell;

	n = read(fd, buf, sizeof buf);
	if(n < CD_HEADER_BYTES){
		fprint(2, "vts-attach: short read %d\n", n);
		return -1;
	}

	magic = ldu32(buf + 0);
	if(magic != CD_MAGIC){
		fprint(2, "vts-attach: bad magic 0x%08ux\n", magic);
		return -1;
	}

	rows = ldu16(buf + 6);
	cols = ldu16(buf + 8);
	ncells = ldu32(buf + 10);
	cur_row = ldu16(buf + 14);
	cur_col = ldu16(buf + 16);
	vis = buf[18];

	USED(rows); USED(cols);

	cell = buf + CD_HEADER_BYTES;
	for(i = 0; i < ncells; i++){
		int row, col, fg, bg, attrs;
		uint rune;

		if((cell - buf) + CD_CELL_BYTES > n){
			fprint(2, "vts-attach: truncated cell %d\n", i);
			break;
		}
		row = ldu16(cell + 0);
		col = ldu16(cell + 2);
		rune = ldu32(cell + 4);
		fg = cell[8];
		bg = cell[9];
		attrs = cell[10];
		cell += CD_CELL_BYTES;

		emitf("\x1b[%d;%dH", row + 1, col + 1);
		emit_sgr(fg, bg, attrs);
		emit_rune(rune);
	}

	emitf("\x1b[%d;%dH", cur_row + 1, cur_col + 1);
	if(vis)
		emit("\x1b[?25h");
	else
		emit("\x1b[?25l");

	return 0;
}

void
usage(void)
{
	fprint(2, "usage: vts-attach [-r] [session]\n");
	fprint(2, "    -r   read-only (don't forward stdin to cons)\n");
	exits("usage");
}

/* Writer side: fork a proc that reads stdin and writes each chunk to cons. */
static int cons_fd = -1;

static void
stdin_writer(void)
{
	uchar buf[1024];
	int n;
	for(;;){
		n = read(0, buf, sizeof buf);
		if(n <= 0) break;
		if(cons_fd >= 0)
			write(cons_fd, buf, n);
	}
}

void
main(int argc, char **argv)
{
	char *sess = "1";
	char cells_path[256], cons_path[256];
	int fd;
	int read_only = 0;
	int ctlfd;

	ARGBEGIN{
	case 'r':
		read_only = 1;
		break;
	}ARGEND;

	if(argc > 0)
		sess = argv[0];

	/* Mount /srv/vts onto /n/vts. */
	{
		int sfd = open("/srv/vts", ORDWR);
		if(sfd < 0){
			fprint(2, "vts-attach: open /srv/vts: %r\n");
			exits("srv");
		}
		if(mount(sfd, -1, "/n/vts", MREPL, "") < 0){
			fprint(2, "vts-attach: mount: %r (continuing if /n/vts already mounted)\n");
		}
	}

	snprint(cells_path, sizeof cells_path, "/n/vts/%s/cells", sess);
	snprint(cons_path, sizeof cons_path, "/n/vts/%s/cons", sess);

	/* Switch our /dev/cons (the dumb pipe to the user's terminal) to
	 * raw mode so keystrokes deliver byte-by-byte. */
	if(!read_only){
		ctlfd = open("/dev/consctl", OWRITE);
		if(ctlfd >= 0){
			fprint(ctlfd, "rawon\n");
			/* don't close — closing resets raw mode */
		} else {
			fprint(2, "vts-attach: warning: can't open /dev/consctl: %r\n");
		}

		cons_fd = open(cons_path, OWRITE);
		if(cons_fd < 0){
			fprint(2, "vts-attach: open cons: %r\n");
			exits("cons");
		}

		/* Spawn writer proc */
		switch(rfork(RFPROC|RFMEM|RFNOWAIT)){
		case -1:
			fprint(2, "vts-attach: rfork writer: %r\n");
			break;
		case 0:
			stdin_writer();
			exits(nil);
		}
	}

	emit("\x1b[2J\x1b[H\x1b[?25l");

	for(;;){
		fd = open(cells_path, OREAD);
		if(fd < 0){
			fprint(2, "vts-attach: cannot open %s: %r\n", cells_path);
			exits("open");
		}
		if(decode_frame(fd) < 0){
			close(fd);
			break;
		}
		close(fd);
		sleep(50);
	}
	exits(nil);
}
