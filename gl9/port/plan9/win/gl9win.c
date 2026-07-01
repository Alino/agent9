/* gl9win — the native (kencc/libdraw) half of gl9's presentation. A cc9 Mesa
 * process renders into an OSMesa buffer and writes framed RGBA to this program's
 * stdin (`glapp | gl9win`); we blit each frame into our rio window. This is the
 * two-process seam the cc9 ABI wall forces (cc9's System-V a.out can't link
 * kencc's libdraw) — the same pattern src/vtwin uses to draw vts frames.
 *
 * Frame: "GL9F" | u32be w | u32be h | w*h*4 bytes RGBA (OSMesa byte order). That
 * byte order (R,G,B,A) is exactly Plan 9 ABGR32 (a8b8g8r8, little-endian), so the
 * bytes load straight into the image with no repacking. */
#include <u.h>
#include <libc.h>
#include <draw.h>

static ulong
get32(uchar *b)
{
	return (ulong)b[0] << 24 | (ulong)b[1] << 16 | (ulong)b[2] << 8 | (ulong)b[3];
}

void
main(int, char **)
{
	uchar hdr[12], *pix;
	ulong w, h;
	long n;
	Image *im;
	Point o;

	if(initdraw(nil, nil, "gl9win") < 0)
		sysfatal("initdraw: %r");

	for(;;){
		if(readn(0, hdr, 12) != 12)
			break;			/* EOF: producer done */
		if(memcmp(hdr, "GL9F", 4) != 0)
			sysfatal("bad frame magic");
		w = get32(hdr + 4);
		h = get32(hdr + 8);
		n = (long)w * h * 4;
		if((pix = malloc(n)) == nil)
			sysfatal("malloc %ld: %r", n);
		if(readn(0, pix, n) != n){
			free(pix);
			break;
		}
		if((im = allocimage(display, Rect(0, 0, w, h), ABGR32, 0, DNofill)) == nil)
			sysfatal("allocimage: %r");
		loadimage(im, im->r, pix, n);
		/* clear the window, blit the frame centered */
		draw(screen, screen->r, display->black, nil, ZP);
		o = addpt(screen->r.min, Pt((Dx(screen->r) - (int)w) / 2,
					    (Dy(screen->r) - (int)h) / 2));
		draw(screen, rectaddpt(im->r, o), im, nil, ZP);
		flushimage(display, 1);
		freeimage(im);
		free(pix);
	}
	/* producer closed the stream: keep the last frame on screen briefly so it's
	 * observable (a real interactive gl9win would wait for a dismiss event). */
	sleep(60000);
	exits(nil);
}
