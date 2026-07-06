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

int kbddebug;

static Rune down[MAXDOWN];
static uchar emitted[MAXDOWN];
static int ndown, mods;

/* Handle ONE kbd message (NUL-terminated). A read on /dev/kbd can carry
 * several queued messages back to back (fast typing) — the caller loops. */
static void
kbdmsg(char *buf)
{
	Rune new[MAXDOWN], r;
	uchar nemit[MAXDOWN];
	char *p;
	int i, j, nnew, found;

	if(kbddebug)
		fprint(2, "gl9win2 kbd: %s\n", buf);
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
		/* Suppress only the control BYTES of chords already emitted from
		 * 'k' — never printable runes, so a missed modifier release can't
		 * silently eat typing. */
		if((mods & (MODCTL|MODALT)) && !isspecial(r) && (r < 0x20 || r == 0x7f))
			break;
		emit(EVKEY, 1, mods, r, 0);
		emit(EVKEY, 0, mods, r, 0);
		break;
	}
}

static void
kbdproc(void*)
{
	char buf[512], *p, *e;
	int fd, n;

	if((fd = open("/dev/kbd", OREAD)) < 0)
		threadexitsall("open /dev/kbd");

	for(;;){
		n = read(fd, buf, sizeof buf - 1);
		if(n <= 0)
			break;
		buf[n] = 0;
		/* one read may hold SEVERAL NUL-terminated messages — walk all;
		 * dropping the tail loses key releases and wedges the mod state */
		e = buf + n;
		for(p = buf; p < e; p += strlen(p) + 1)
			if(*p != 0)
				kbdmsg(p);
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

/* Handoff between the pipe reader and the blitter. The reader drains the
 * frame pipe at full speed so the app's swap write never queues behind slow
 * blits (queued full frames ARE input lag). Two record kinds:
 *   GL9F full frames — a new full frame supersedes everything queued;
 *   GL9D damage rects — deltas against the current image, applied in order
 *   (cheap: a keystroke is a few rows, and small blits are what a real
 *   framebuffer is fast at).
 */
typedef struct Frame Frame;
struct Frame {
	int full;
	ulong x, y, w, h;
	uchar *pix;
	Frame *next;
};

static QLock framelock;
static Frame *qhead, *qtail;	/* undrawn records, in arrival order */
static Channel *framec;		/* capacity 1: "work is pending" */
static int nskipped;

static void
enqueue(Frame *f)
{
	Frame *g, *next;

	qlock(&framelock);
	if(f->full){
		/* a full frame supersedes everything before it */
		for(g = qhead; g != nil; g = next){
			next = g->next;
			free(g->pix);
			free(g);
			nskipped++;
		}
		qhead = qtail = nil;
	}
	f->next = nil;
	if(qtail == nil)
		qhead = f;
	else
		qtail->next = f;
	qtail = f;
	qunlock(&framelock);
	nbsendul(framec, 1);
}

static void
framereader(void*)
{
	uchar hdr[16], *pix;
	ulong w, h, x, y, len;
	long n;
	Frame *f;
	int full;
	char title[256];

	for(;;){
		if(readn(framefd, hdr, 4) != 4)
			break;	/* EOF: app exited */
		if(memcmp(hdr, "GL9T", 4) == 0){
			if(readn(framefd, hdr, 4) != 4)
				break;
			len = get32(hdr);
			if(len >= sizeof title){
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
		x = y = 0;
		if(memcmp(hdr, "GL9F", 4) == 0){
			full = 1;
			if(readn(framefd, hdr, 8) != 8)
				break;
			w = get32(hdr);
			h = get32(hdr + 4);
		}else if(memcmp(hdr, "GL9D", 4) == 0){
			full = 0;
			if(readn(framefd, hdr, 16) != 16)
				break;
			x = get32(hdr);
			y = get32(hdr + 4);
			w = get32(hdr + 8);
			h = get32(hdr + 12);
		}else
			threadexitsall("bad frame magic");
		n = (long)w * h * 4;
		if((pix = malloc(n)) == nil)
			threadexitsall("malloc frame");
		if(readn(framefd, pix, n) != n){
			free(pix);
			break;
		}
		f = malloc(sizeof *f);
		if(f == nil)
			threadexitsall("malloc frame hdr");
		f->full = full;
		f->x = x;
		f->y = y;
		f->w = w;
		f->h = h;
		f->pix = pix;
		enqueue(f);
	}
	sendul(framec, 0);	/* EOF marker */
	threadexits(nil);
}

void
threadmain(int argc, char **argv)
{
	static Execargs e;
	int pev[2], pfr[2];
	ulong w, h;
	long n;
	Image *im;
	Point o;
	Frame *f, *fnext;
	Rectangle r;
	vlong lastns, blitns;

	if(argc >= 2 && strcmp(argv[1], "-d") == 0){
		kbddebug = 1;
		argv++;
		argc--;
	}
	if(argc < 2){
		fprint(2, "usage: gl9win2 [-d] cmd [args...]\n");
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

	framec = chancreate(sizeof(ulong), 1);
	proccreate(kbdproc, nil, 32*1024);
	proccreate(mouseproc, nil, 32*1024);
	proccreate(framereader, nil, 32*1024);

	im = nil;
	lastns = 0;
	for(;;){
		if(recvul(framec) == 0)
			break;	/* reader hit EOF */
		qlock(&framelock);
		f = qhead;
		qhead = qtail = nil;
		qunlock(&framelock);
		if(f == nil)
			continue;

		blitns = 0;
		if(kbddebug){
			vlong now = nsec();
			if(lastns != 0)
				fprint(2, "gl9win2 %s: %ludx%lud dt=%lldms skipped=%d\n",
					f->full ? "frame" : "damage",
					f->w, f->h, (now - lastns)/1000000, nskipped);
			lastns = now;
			nskipped = 0;
			blitns = now;
		}

		qlock(&displock);
		o = screen->r.min;
		for(; f != nil; f = fnext){
			fnext = f->next;
			w = f->w;
			h = f->h;
			n = (long)w * h * 4;
			if(f->full){
				/* reuse the draw image across same-size frames */
				if(im == nil || Dx(im->r) != (int)w || Dy(im->r) != (int)h){
					if(im != nil)
						freeimage(im);
					im = allocimage(display, Rect(0, 0, w, h), ABGR32, 0, DNofill);
					if(im == nil){
						qunlock(&displock);
						threadexitsall("allocimage");
					}
				}
				loadimage(im, im->r, f->pix, n);
				if((int)w < Dx(screen->r) || (int)h < Dy(screen->r))
					draw(screen, screen->r, display->black, nil, ZP);
				draw(screen, rectaddpt(im->r, o), im, nil, ZP);
			}else if(im != nil
			     && (int)(f->x + w) <= Dx(im->r) && (int)(f->y + h) <= Dy(im->r)){
				/* delta against the current image; a stale rect
				 * (pre-resize) is dropped — the app follows any
				 * resize with a fully-damaged frame */
				r = Rect(f->x, f->y, f->x + w, f->y + h);
				loadimage(im, r, f->pix, n);
				draw(screen, rectaddpt(r, o), im, nil, r.min);
			}
			free(f->pix);
			free(f);
		}
		flushimage(display, 1);
		qunlock(&displock);
		if(kbddebug && blitns != 0)
			fprint(2, "gl9win2 blit: %lldms\n", (nsec() - blitns)/1000000);
	}
	threadexitsall(nil);
}
