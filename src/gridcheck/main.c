#include <u.h>
#include <libc.h>

/*
 * gridcheck — sample a single row from /dev/screen and look for non-black pixels.
 *
 * Usage: gridcheck Y         (read screen, sample row Y, print non-black runs)
 */

void
main(int argc, char *argv[])
{
	int targety;
	int fd, n, i, j;
	char hdr[5*12+1];
	int smnx, smny, smxx, smxy;
	char chan[13];
	uchar *buf;

	if(argc < 2){
		fprint(2, "usage: gridcheck <y>\n");
		exits("usage");
	}
	targety = atoi(argv[1]);

	fd = open("/dev/screen", OREAD);
	if(fd < 0){ fprint(2, "open: %r\n"); exits("screen"); }

	n = read(fd, hdr, 60);
	if(n != 60){ fprint(2, "hdr: %d\n", n); exits("hdr"); }
	memcpy(chan, hdr, 12); chan[12] = 0;
	for(i = 11; i >= 0 && chan[i] == ' '; i--) chan[i] = 0;
	smnx = atoi(hdr+12); smny = atoi(hdr+24);
	smxx = atoi(hdr+36); smxy = atoi(hdr+48);
	int w = smxx - smnx;
	int bpp = (strcmp(chan, "x8r8g8b8") == 0) ? 4 : 2;
	int bpr = w * bpp;
	buf = malloc(bpr);

	int skip = targety - smny;
	for(i = 0; i < skip; i++){
		n = read(fd, buf, bpr);
		if(n != bpr){ fprint(2, "skip read: %d\n", n); exits("skip"); }
	}
	n = read(fd, buf, bpr);
	if(n != bpr){ fprint(2, "row read: %d\n", n); exits("row"); }
	close(fd);

	/* Print all non-black non-white pixels in this row */
	print("row y=%d  w=%d chan=%s\n", targety, w, chan);
	int last_x = -2;
	int in_run = 0;
	for(j = 0; j < w; j++){
		int off = j * bpp;
		int R, G, B;
		if(bpp == 2){
			ushort v = buf[off] | (buf[off+1] << 8);
			R = ((v >> 11) & 0x1F) << 3;
			G = ((v >>  5) & 0x3F) << 2;
			B = ((v      ) & 0x1F) << 3;
		} else {
			B = buf[off]; G = buf[off+1]; R = buf[off+2];
		}
		int interesting = 0;
		if(R == G && G == B){
			/* True monochrome */
			interesting = (R < 32);  /* dark/black */
		} else {
			/* Non-monochrome — but RGB16 white (248,252,248) is
			 * still effectively white. Treat as monochrome if all
			 * channels are above 200. */
			if(R > 200 && G > 200 && B > 200)
				interesting = 0;
			else
				interesting = 1;
		}
		if(interesting){
			if(!in_run){
				print("  x=%d: (%d,%d,%d) ... ", j + smnx, R, G, B);
				in_run = 1;
			}
			last_x = j;
		} else {
			if(in_run){
				print("(end at x=%d)\n", last_x + smnx);
				in_run = 0;
			}
		}
	}
	if(in_run)
		print("(end at x=%d)\n", last_x + smnx);
	free(buf);
	exits(nil);
}
