/* gl9win2 — interactive successor to gl9's gl9win (native kencc/libdraw).
 *
 * Owns a rio window and hosts one cc9-world GL app (Alacritty):
 *   gl9win2 cmd [args...]
 * spawns cmd with fd0 = event records (us -> app), fd1 = frame stream
 * (app -> us), fd2 passthrough. Protocol: alacritty9/PROTOCOL.md.
 *
 * Input: raw /dev/kbd (down/up + modifier tracking; cooked initkeyboard
 * can't give key-up or modifiers), /dev/mouse (moves/buttons/scroll and the
 * 'r' resize message). Frames: GL9F as in gl9win, plus GL9T window titles.
 *
 * Event emission rules (see kbdproc): plain typing and specials are emitted
 * from 'c' messages (correct case + auto-repeat); modifier keys and
 * ctrl/alt-chorded base runes are emitted from 'k'/'K' state diffs (so
 * Alacritty sees e.g. ctrl+shift+V as V + mods, not a control char).
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <keyboard.h>

enum {
	EVKEY = 1,
	EVMOVE = 2,
	EVBTN = 3,
	EVSCROLL = 4,
	EVRESIZE = 5,
	EVFOCUS = 6,
	EVQUIT = 7,

	MODSHIFT = 1,
	MODCTL = 2,
	MODALT = 4,

	MAXDOWN = 16,
};

static int evfd;	/* our end of the event pipe */
static int framefd;	/* our end of the frame pipe */
static QLock displock;	/* serializes draw ops vs getwindow */

static void
put32(uchar *b, ulong v)
{
	b[0] = v >> 24;
	b[1] = v >> 16;
	b[2] = v >> 8;
	b[3] = v;
}

static ulong
get32(uchar *b)
{
	return (ulong)b[0] << 24 | (ulong)b[1] << 16 | (ulong)b[2] << 8 | (ulong)b[3];
}

static void
emit(int type, int state, int mods, ulong a, ulong b)
{
	uchar rec[16];

	memset(rec, 0, sizeof rec);
	rec[0] = type;
	rec[1] = state;
	rec[2] = mods >> 8;
	rec[3] = mods;
	put32(rec + 4, a);
	put32(rec + 8, b);
	/* 16-byte pipe writes are atomic; procs can emit concurrently */
	if(write(evfd, rec, 16) != 16)
		threadexitsall("event pipe");
}

/* ---- keyboard ---- */

static int
ismod(Rune r)
{
	return r == Kshift || r == Kctl || r == Kalt;
}

static int
modbit(Rune r)
{
	switch(r){
	case Kshift: return MODSHIFT;
	case Kctl: return MODCTL;
	case Kalt: return MODALT;
	}
	return 0;
}

static int
isspecial(Rune r)
{
	return r >= KF || r == Kdown;	/* Kdown is 0x80, below KF */
}

static void
kbdproc(void*)
{
	static Rune down[MAXDOWN];
	static uchar emitted[MAXDOWN];
	Rune new[MAXDOWN], r;
	uchar nemit[MAXDOWN];
	char buf[256], *p;
	int fd, n, i, j, nnew, ndown, mods, found;

	if((fd = open("/dev/kbd", OREAD)) < 0)
		threadexitsall("open /dev/kbd");

	ndown = 0;
	mods = 0;
	for(;;){
		n = read(fd, buf, sizeof buf - 1);
		if(n <= 0)
			break;
		buf[n] = 0;
		switch(buf[0]){
		case 'k':	/* current down set, after a press */
		case 'K':	/* current down set, after a release */
			nnew = 0;
			for(p = buf + 1; *p != 0 && nnew < MAXDOWN; ){
				p += chartorune(&r, p);
				new[nnew++] = r;
			}
			/* recompute mods from the new state */
			mods = 0;
			for(i = 0; i < nnew; i++)
				mods |= modbit(new[i]);
			/* presses: in new, not in down */
			for(i = 0; i < nnew; i++){
				found = 0;
				for(j = 0; j < ndown; j++)
					if(down[j] == new[i])
						found = 1;
				nemit[i] = 0;
				if(!found){
					if(ismod(new[i])){
						emit(EVKEY, 1, mods, new[i], 0);
						nemit[i] = 1;
					}else if((mods & (MODCTL|MODALT)) && !isspecial(new[i])){
						/* chorded base rune: Alacritty gets V+ctl, not ^V */
						emit(EVKEY, 1, mods, new[i], 0);
						nemit[i] = 1;
					}
				}else{
					for(j = 0; j < ndown; j++)
						if(down[j] == new[i])
							nemit[i] = emitted[j];
				}
			}
			/* releases: in down, not in new */
			for(j = 0; j < ndown; j++){
				found = 0;
				for(i = 0; i < nnew; i++)
					if(down[j] == new[i])
						found = 1;
				if(!found && emitted[j])
					emit(EVKEY, 0, mods, down[j], 0);
			}
			memmove(down, new, nnew * sizeof(Rune));
			memmove(emitted, nemit, nnew);
			ndown = nnew;
			break;
		case 'c':	/* composed character (case + auto-repeat correct) */
			chartorune(&r, buf + 1);
			if(r == 0)
				break;
			if((mods & (MODCTL|MODALT)) && !isspecial(r))
				break;	/* chords already emitted from 'k' */
			emit(EVKEY, 1, mods, r, 0);
			emit(EVKEY, 0, mods, r, 0);
			break;
		}
	}
	/* /dev/kbd read fails when the window goes away */
	emit(EVQUIT, 0, 0, 0, 0);
	sleep(500);
	threadexitsall(nil);
}

/* ---- mouse ---- */

static void
sendresize(void)
{
	int w, h;

	qlock(&displock);
	if(getwindow(display, Refnone) < 0){
		qunlock(&displock);
		threadexitsall("getwindow");
	}
	w = Dx(screen->r);
	h = Dy(screen->r);
	qunlock(&displock);
	emit(EVRESIZE, 0, 0, w, h);
}

static void
mouseproc(void*)
{
	char buf[64];
	int fd, n, b, ob, x, y;

	if((fd = open("/dev/mouse", OREAD)) < 0)
		threadexitsall("open /dev/mouse");

	ob = 0;
	for(;;){
		n = read(fd, buf, sizeof buf - 1);
		if(n <= 0)
			break;
		buf[n] = 0;
		if(buf[0] == 'r'){
			sendresize();
			continue;
		}
		if(buf[0] != 'm')
			continue;
		x = atoi(buf + 1 + 0*12) - screen->r.min.x;
		y = atoi(buf + 1 + 1*12) - screen->r.min.y;
		b = atoi(buf + 1 + 2*12);
		emit(EVMOVE, 0, 0, x, y);
		if((b & 1) != (ob & 1))
			emit(EVBTN, (b & 1) != 0, 0, 1, 0);
		if((b & 2) != (ob & 2))
			emit(EVBTN, (b & 2) != 0, 0, 2, 0);
		if((b & 4) != (ob & 4))
			emit(EVBTN, (b & 4) != 0, 0, 3, 0);
		if((b & 8) && !(ob & 8))
			emit(EVSCROLL, 0, 0, (ulong)1, 0);
		if((b & 16) && !(ob & 16))
			emit(EVSCROLL, 0, 0, (ulong)-1, 0);
		ob = b;
	}
	emit(EVQUIT, 0, 0, 0, 0);
	sleep(500);
	threadexitsall(nil);
}

/* ---- child spawn ---- */

typedef struct Execargs Execargs;
struct Execargs {
	char **argv;
	int fd0;	/* child stdin: its end of the event pipe */
	int fd1;	/* child stdout: its end of the frame pipe */
};

static void
execproc(void *v)
{
	Execargs *e = v;

	dup(e->fd0, 0);
	dup(e->fd1, 1);
	close(e->fd0);
	close(e->fd1);
	close(evfd);
	close(framefd);
	procexec(nil, e->argv[0], e->argv);
	threadexitsall("exec failed");
}

/* ---- frames ---- */

static void
setlabel(char *s)
{
	int fd;

	if((fd = open("/dev/label", OWRITE)) < 0)
		return;
	write(fd, s, strlen(s));
	close(fd);
}

void
threadmain(int argc, char **argv)
{
	static Execargs e;
	int pev[2], pfr[2];
	uchar hdr[12], *pix;
	ulong w, h, len;
	long n;
	Image *im;
	Point o;
	char title[256];

	if(argc < 2){
		fprint(2, "usage: gl9win2 cmd [args...]\n");
		threadexitsall("usage");
	}

	if(initdraw(nil, nil, "gl9win2") < 0)
		threadexitsall("initdraw");

	if(pipe(pev) < 0 || pipe(pfr) < 0)
		threadexitsall("pipe");
	evfd = pev[0];
	framefd = pfr[0];

	e.argv = argv + 1;
	e.fd0 = pev[1];
	e.fd1 = pfr[1];
	procrfork(execproc, &e, 16*1024, RFFDG);
	close(pev[1]);
	close(pfr[1]);

	/* guaranteed first record: the initial window size */
	emit(EVRESIZE, 0, 0, Dx(screen->r), Dy(screen->r));
	emit(EVFOCUS, 1, 0, 0, 0);

	proccreate(kbdproc, nil, 32*1024);
	proccreate(mouseproc, nil, 32*1024);

	for(;;){
		if(readn(framefd, hdr, 4) != 4)
			break;	/* EOF: app exited */
		if(memcmp(hdr, "GL9T", 4) == 0){
			if(readn(framefd, hdr, 4) != 4)
				break;
			len = get32(hdr);
			if(len >= sizeof title){
				/* skip oversized title */
				pix = malloc(len);
				if(pix == nil || readn(framefd, pix, len) != len)
					break;
				free(pix);
				continue;
			}
			if(readn(framefd, title, len) != len)
				break;
			title[len] = 0;
			setlabel(title);
			continue;
		}
		if(memcmp(hdr, "GL9F", 4) != 0)
			threadexitsall("bad frame magic");
		if(readn(framefd, hdr, 8) != 8)
			break;
		w = get32(hdr);
		h = get32(hdr + 4);
		n = (long)w * h * 4;
		if((pix = malloc(n)) == nil)
			threadexitsall("malloc frame");
		if(readn(framefd, pix, n) != n){
			free(pix);
			break;
		}
		qlock(&displock);
		if((im = allocimage(display, Rect(0, 0, w, h), ABGR32, 0, DNofill)) == nil){
			qunlock(&displock);
			threadexitsall("allocimage");
		}
		loadimage(im, im->r, pix, n);
		if((int)w < Dx(screen->r) || (int)h < Dy(screen->r))
			draw(screen, screen->r, display->black, nil, ZP);
		o = screen->r.min;
		draw(screen, rectaddpt(im->r, o), im, nil, ZP);
		flushimage(display, 1);
		freeimage(im);
		qunlock(&displock);
		free(pix);
	}
	threadexitsall(nil);
}
