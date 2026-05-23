#include <u.h>
#include <libc.h>
#include <draw.h>

/*
 * titlecheck — read pixels along the titlebar of a named window and
 * verify the WinXP gradient is fully painted.
 *
 * Usage: titlecheck <label>
 *
 * Finds the window by label (via /mnt/wsys/wsys/<id>/label), reads its
 * wctl rect, then samples pixels at (rect.min.x + 250, rect.min.y + N)
 * for N in 0..30. Prints each pixel's RGB and reports whether the
 * gradient is the expected 22+ non-monochrome rows.
 */

typedef struct Wininfo Wininfo;
struct Wininfo {
	int id;
	char label[64];
	int minx, miny, maxx, maxy;
	int current;
	int visible;
};

int
findwin(char *label, Wininfo *wi)
{
	int fd, n, i;
	char path[256], buf[256];
	Dir *d;
	long ndirs;

	fd = open("/mnt/wsys/wsys", OREAD);
	if(fd < 0) return -1;
	ndirs = dirreadall(fd, &d);
	close(fd);

	for(i = 0; i < ndirs; i++){
		if(!(d[i].mode & DMDIR)) continue;
		snprint(path, sizeof path, "/mnt/wsys/wsys/%s/label", d[i].name);
		fd = open(path, OREAD);
		if(fd < 0) continue;
		n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if(n <= 0) continue;
		buf[n] = 0;
		/* strip trailing newline/space */
		while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ' || buf[n-1] == 0)){
			buf[--n] = 0;
		}
		if(strcmp(buf, label) != 0) continue;

		/* read wctl */
		strncpy(wi->label, buf, sizeof wi->label - 1);
		wi->id = atoi(d[i].name);
		snprint(path, sizeof path, "/mnt/wsys/wsys/%s/wctl", d[i].name);
		fd = open(path, OREAD);
		if(fd < 0) return -1;
		n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if(n <= 0){
			fprint(2, "wctl read returned %d (no pending state — try focusing first)\n", n);
			return -1;
		}
		buf[n] = 0;
		/* parse: " minx miny maxx maxy current|notcurrent visible|hidden" */
		char *fields[8];
		int nf = tokenize(buf, fields, 8);
		if(nf < 6){
			fprint(2, "couldn't parse wctl (%d fields): %s\n", nf, buf);
			return -1;
		}
		wi->minx = atoi(fields[0]);
		wi->miny = atoi(fields[1]);
		wi->maxx = atoi(fields[2]);
		wi->maxy = atoi(fields[3]);
		wi->current = (strcmp(fields[4], "current") == 0);
		wi->visible = (strcmp(fields[5], "visible") == 0);
		free(d);
		return 0;
	}
	free(d);
	return -1;
}

void
main(int argc, char *argv[])
{
	Wininfo wi;
	int i, fd, n, nrows;
	uchar *buf;
	int w, h;
	char *label;

	if(argc < 2){
		fprint(2, "usage: titlecheck <label>\n");
		exits("usage");
	}
	label = argv[1];

	if(findwin(label, &wi) < 0){
		fprint(2, "titlecheck: window %q not found (try focusing it first)\n", label);
		exits("not found");
	}

	print("found window: id=%d label=%s rect=(%d,%d)-(%d,%d) %s %s\n",
		wi.id, wi.label, wi.minx, wi.miny, wi.maxx, wi.maxy,
		wi.current ? "current" : "notcurrent",
		wi.visible ? "visible" : "hidden");

	/* Now read /dev/screen image data.
	 * Plan 9 screen image format: ascii header (5 fields, 12 chars each)
	 * followed by row data.
	 *   chan  minx  miny  maxx  maxy  (12 chars each, space-separated)
	 * Pixel format depends on chan. RGB16 = 2 bytes per pixel little-endian.
	 */
	fd = open("/dev/screen", OREAD);
	if(fd < 0){
		fprint(2, "titlecheck: open /dev/screen: %r\n");
		exits("screen");
	}
	char hdr[5*12 + 1];
	n = read(fd, hdr, 5*12);
	if(n != 5*12){
		fprint(2, "titlecheck: short hdr read %d\n", n);
		exits("hdr");
	}
	hdr[5*12] = 0;
	char chan[13];
	int smnx, smny, smxx, smxy;
	memcpy(chan, hdr, 12); chan[12] = 0;
	/* strip trailing whitespace */
	for(i = 11; i >= 0 && (chan[i] == ' ' || chan[i] == 0); i--) chan[i] = 0;
	smnx = atoi(hdr + 12);
	smny = atoi(hdr + 24);
	smxx = atoi(hdr + 36);
	smxy = atoi(hdr + 48);
	print("screen: chan=%q rect=(%d,%d)-(%d,%d)\n", chan, smnx, smny, smxx, smxy);

	w = smxx - smnx;
	h = smxy - smny;

	/* Allocate buffer for the rows we care about: 32 rows starting at miny */
	int sample_y_start = wi.miny - 2;  /* a few before in case of border */
	int sample_y_end = wi.miny + 32;
	int sample_rows = sample_y_end - sample_y_start;

	/* We need to read until sample_y_end. /dev/screen is sequential, no
	 * seek. Read whole rows until we get to our target. */
	int bytes_per_pixel = 2;  /* RGB16 */
	if(strcmp(chan, "x8r8g8b8") == 0) bytes_per_pixel = 4;
	else if(strcmp(chan, "r5g6b5") == 0) bytes_per_pixel = 2;
	/* Don't bother with other formats */

	int bytes_per_row = w * bytes_per_pixel;
	int sample_col = (wi.minx + wi.maxx) / 2;  /* middle of window */

	/* Read+discard until sample_y_start, then keep sample_rows */
	int skip_rows = sample_y_start - smny;
	buf = malloc(bytes_per_row);
	if(buf == nil){ fprint(2, "malloc\n"); exits("malloc"); }
	for(i = 0; i < skip_rows; i++){
		n = read(fd, buf, bytes_per_row);
		if(n != bytes_per_row){
			fprint(2, "skip read short %d\n", n);
			break;
		}
	}

	print("\ntitlebar pixel scan (col=%d):\n", sample_col);
	nrows = 0;  /* count rows that have non-monochrome pixel = gradient */
	for(i = 0; i < sample_rows; i++){
		n = read(fd, buf, bytes_per_row);
		if(n != bytes_per_row){
			fprint(2, "scan read short %d (%r)\n", n);
			break;
		}
		int rel_x = sample_col - smnx;
		int off = rel_x * bytes_per_pixel;
		int abs_y = sample_y_start + i;
		int R, G, B;
		if(bytes_per_pixel == 2){
			/* RGB16: 5R 6G 5B little-endian word */
			ushort v = buf[off] | (buf[off+1] << 8);
			R = ((v >> 11) & 0x1F) << 3;
			G = ((v >> 5)  & 0x3F) << 2;
			B = ((v      ) & 0x1F) << 3;
		} else {
			/* XRGB32: B G R X */
			B = buf[off];
			G = buf[off+1];
			R = buf[off+2];
		}
		char *marker = "";
		if(R == G && G == B){
			marker = (R == 0xFF || R == 0) ? "" : "(gray)";
		} else if(B > R && B > 100){
			marker = "<- gradient";
			nrows++;
		}
		print("  y=%d: RGB=(%d,%d,%d) %s\n", abs_y, R, G, B, marker);
	}
	close(fd);
	free(buf);
	print("\nresult: %d gradient rows (expected ~22 for a focused titlebar)\n", nrows);
	exits(nil);
}
