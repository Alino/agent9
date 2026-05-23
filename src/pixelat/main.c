#include <u.h>
#include <libc.h>

/* pixelat <x> <y>  — print the RGB of one pixel from /dev/screen */
void
main(int argc, char *argv[])
{
	int tx, ty, i, fd, n;
	char hdr[60+1];
	int smnx, smny, smxx, smxy;
	char chan[13];
	uchar *buf;

	if(argc < 3){ fprint(2, "usage: pixelat <x> <y>\n"); exits("u"); }
	tx = atoi(argv[1]);
	ty = atoi(argv[2]);

	fd = open("/dev/screen", OREAD);
	if(fd < 0){ fprint(2, "open: %r\n"); exits("o"); }
	if(read(fd, hdr, 60) != 60) exits("hdr");
	memcpy(chan, hdr, 12); chan[12]=0;
	for(i = 11; i >= 0 && chan[i] == ' '; i--) chan[i] = 0;
	smnx = atoi(hdr+12); smny = atoi(hdr+24);
	smxx = atoi(hdr+36); smxy = atoi(hdr+48);
	USED(smxy);
	int w = smxx - smnx;
	int bpp = (strcmp(chan, "x8r8g8b8") == 0) ? 4 : 2;
	int bpr = w * bpp;
	buf = malloc(bpr);
	int skip = ty - smny;
	for(i = 0; i < skip; i++){
		if(read(fd, buf, bpr) != bpr){ fprint(2, "skip\n"); exits("s"); }
	}
	if(read(fd, buf, bpr) != bpr){ fprint(2, "row\n"); exits("r"); }
	close(fd);
	int off = (tx - smnx) * bpp;
	int R, G, B;
	if(bpp == 2){
		ushort v = buf[off] | (buf[off+1] << 8);
		R = ((v >> 11) & 0x1F) << 3;
		G = ((v >>  5) & 0x3F) << 2;
		B = ((v      ) & 0x1F) << 3;
	} else {
		B = buf[off]; G = buf[off+1]; R = buf[off+2];
	}
	print("(%d,%d) = RGB(%d,%d,%d) chan=%s\n", tx, ty, R, G, B, chan);
	exits(nil);
}
