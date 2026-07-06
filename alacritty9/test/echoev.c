/* echoev — P2 gate child for gl9win2 (cc9 world, zero Rust, zero GL).
 *
 * Reads protocol event records on fd 0, logs each to stderr, and answers
 * every event with a solid-color GL9F frame on fd 1 at the current window
 * size. Key runes pick the color ('r' red, 'g' green, 'b' blue, anything
 * else grey), so a keystroke round trip is visible both in the window and
 * in a QMP screendump. See alacritty9/PROTOCOL.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned
get32(const unsigned char *b)
{
	return (unsigned)b[0] << 24 | (unsigned)b[1] << 16 | (unsigned)b[2] << 8 | b[3];
}

static void
put32(unsigned char *b, unsigned v)
{
	b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v;
}

static int
readn(int fd, void *buf, int n)
{
	char *p = buf;
	int got, total = 0;
	while (total < n) {
		got = read(fd, p + total, n - total);
		if (got <= 0) return total;
		total += got;
	}
	return total;
}

static void
frame(int w, int h, unsigned char r, unsigned char g, unsigned char b)
{
	static unsigned char *pix;
	static int cap;
	unsigned char hdr[12];
	long i, n = (long)w * h * 4;

	if (n > cap) {
		free(pix);
		pix = malloc(n);
		cap = pix ? n : 0;
		if (!pix) exit(1);
	}
	for (i = 0; i < n; i += 4) {
		pix[i] = r; pix[i+1] = g; pix[i+2] = b; pix[i+3] = 0xff;
	}
	memcpy(hdr, "GL9F", 4);
	put32(hdr + 4, w);
	put32(hdr + 8, h);
	write(1, hdr, 12);
	write(1, pix, n);
}

int
main(void)
{
	unsigned char rec[16];
	int w = 640, h = 480;
	unsigned char r = 0x30, g = 0x30, b = 0x30;
	unsigned type, state, mods, a, bb;

	for (;;) {
		if (readn(0, rec, 16) != 16) break;
		type = rec[0];
		state = rec[1];
		mods = rec[2] << 8 | rec[3];
		a = get32(rec + 4);
		bb = get32(rec + 8);
		fprintf(stderr, "echoev: type=%u state=%u mods=%u a=%u b=%u\n",
			type, state, mods, a, bb);
		switch (type) {
		case 5:	/* resize */
			w = a; h = bb;
			break;
		case 1:	/* key */
			if (state == 1) {
				switch (a) {
				case 'r': r = 0xff; g = 0; b = 0; break;
				case 'g': r = 0; g = 0xff; b = 0; break;
				case 'b': r = 0; g = 0; b = 0xff; break;
				default:  r = g = b = 0x80; break;
				}
			}
			break;
		case 7:	/* quit */
			fprintf(stderr, "echoev: quit\n");
			return 0;
		}
		frame(w, h, r, g, b);
	}
	return 0;
}
