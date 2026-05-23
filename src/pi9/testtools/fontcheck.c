/*
 * fontcheck.c — open a font, print its metrics, exit. Used to verify
 * vtwin can actually load a generated plan9 subfont.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>

void
main(int argc, char **argv)
{
	Font *f;

	if(argc < 2){
		fprint(2, "usage: fontcheck <font-path>\n");
		exits("usage");
	}
	if(initdraw(nil, nil, "fontcheck") < 0){
		fprint(2, "initdraw failed: %r\n");
		exits("initdraw");
	}
	f = openfont(display, argv[1]);
	if(f == nil){
		fprint(2, "openfont %s failed: %r\n", argv[1]);
		exits("openfont");
	}
	print("opened %s: height=%d ascent=%d width(M)=%d\n",
		argv[1], f->height, f->ascent, stringwidth(f, "M"));
	exits(nil);
}
