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
 * ctrl/alt/super-chorded base runes are emitted from 'k'/'K' state diffs (so
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

	EVCMD = 8,	/* chrome command (ladybird9 tabbed UI) */

	MODSHIFT = 1,
	MODCTL = 2,
	MODALT = 4,
	MODSUPER = 8,	/* Kmod4, the "windows" key — browser chords live here */

	MAXDOWN = 16,

	/* EVCMD subcommands (see alacritty9/PROTOCOL.md) */
	CMDBACK = 1, CMDFWD = 2, CMDRELOAD = 3, CMDSTOP = 4,
	CMDNEWTAB = 5, CMDSWITCH = 6, CMDCLOSE = 7, CMDNAV = 8,

	MAXTABS = 32,
	MAXADDR = 2048,
	PAD = 5,		/* inner padding for chrome rows */
};

static int evfd;	/* our end of the event pipe */
static int framefd;	/* our end of the frame pipe */
static QLock displock;	/* serializes draw ops vs getwindow */

/* ---- chrome (ladybird9 tabbed UI, drawn in a top strip) ----
 * Enabled the first time the app sends a GL9U/GL9L/GL9K record (alacritty9 and
 * dosbox9 never do, so they get chromeh==0 and the whole window is their frame).
 * State is shared by mouseproc/kbdproc/framereader; displock guards both the
 * state and the strip drawing (a state change always redraws the strip). */
static int haschrome;		/* 0 until the first chrome record */
static int chromeh;		/* strip height in px (0 == no chrome) */
static int rowh;		/* one chrome row (font->height + 2*PAD) */
static char curl[MAXADDR];	/* current page URL (from GL9U) */
static char addr[MAXADDR];	/* address-bar edit buffer */
static int addrn;		/* length of addr */
static int addrcur;		/* caret index into addr */
static int addrfocus;		/* address bar has keyboard focus */
static int addrsel;		/* whole address is selected (Super-L): next typed
				 * key replaces it, as in every other browser */
static int loading, canback, canfwd;	/* from GL9L */
static char tabtitle[MAXTABS][256];
static int ntabs, activetab;
static Image *cbg, *ctab, *cact, *ctext, *cfield;	/* chrome colors */

static void put32(uchar *b, ulong v);
static void drawchrome(void);
static void drawchrome_locked(void);
static void enablechrome(void);
static void chromemouse(int x, int y);
static int chromekey(Rune r);
static int chromeshortcut(Rune r);
static int ischromechord(Rune r);
static void focusaddr_locked(int sel);

/* Emit a chrome command. For CMDNAV the 16-byte record is followed by a
 * u32be length + UTF-8 text (the typed URL). */
static void
emitcmd(int sub, ulong tabidx, char *text)
{
	uchar rec[16], len[4];
	int n;

	memset(rec, 0, sizeof rec);
	rec[0] = EVCMD;
	rec[1] = sub;
	put32(rec + 4, tabidx);
	if(write(evfd, rec, 16) != 16)
		threadexitsall("event pipe");
	if(sub == CMDNAV){
		n = text ? strlen(text) : 0;
		put32(len, n);
		if(write(evfd, len, 4) != 4)
			threadexitsall("event pipe");
		if(n > 0 && write(evfd, text, n) != n)
			threadexitsall("event pipe");
	}
}

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
	return r == Kshift || r == Kctl || r == Kalt || r == Kmod4;
}

static int
modbit(Rune r)
{
	switch(r){
	case Kshift: return MODSHIFT;
	case Kctl: return MODCTL;
	case Kalt: return MODALT;
	case Kmod4: return MODSUPER;
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
		fprint(2, "gl9win2 kbd: %s t=%lldms\n", buf, nsec()/1000000);
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
				}else if((mods & MODSUPER) && ischromechord(new[i])){
					/* ours (Super-L/T/R/W): swallow it entirely so the
					 * page never sees a stray keypress. Acted on from
					 * the 'c' message, which has the correct case. */
					nemit[i] = 0;
				}else if((mods & (MODCTL|MODALT|MODSUPER)) && !isspecial(new[i])){
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
		/* Browser chords (Super-L/T/R/W) first: they must work while the page
		 * has focus, which is the whole point of Super-L. */
		if(mods & MODSUPER){
			chromeshortcut(r);
			/* Super+anything is a command, never text: swallow it whether or
			 * not it mapped, so a mistyped chord can't type into the page or
			 * the address bar. The base rune already went out from 'k' for the
			 * chords we don't claim. */
			break;
		}
		/* When the chrome address bar has focus it consumes typed keys
		 * (printables, backspace, arrows, Enter, Esc) — the app never sees them.
		 * chromekey() returns 0 when the bar isn't focused, so the app gets them. */
		if(chromekey(r))
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
	if(haschrome)
		drawchrome_locked();	/* new window backing store — repaint the strip */
	qunlock(&displock);
	/* report the PAGE height (below the chrome strip) so the app lays out right */
	emit(EVRESIZE, 0, 0, w, h - chromeh);
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
		/* chrome strip owns its region; the app never sees those events. On a
		 * button-1 press there, act (tab/button/address bar). */
		if(haschrome && y < chromeh){
			if((b & 1) && !(ob & 1))
				chromemouse(x, y);
			ob = b;
			continue;
		}
		y -= chromeh;	/* page region: report page-relative y (chromeh==0 if none) */
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

/* ---- chrome (tabbed browser UI, ladybird9) ---- */

enum { TABW = 150, BTNW = 30 };

static Image*
solid(ulong rgba)
{
	Image *i = allocimage(display, Rect(0, 0, 1, 1), screen->chan, 1, rgba);
	return i != nil ? i : display->black;
}

/* displock held. Enable the chrome strip on the first chrome record + tell the
 * app its usable height shrank by chromeh (so it re-renders below the strip). */
static void
enablechrome(void)
{
	if(haschrome)
		return;
	rowh = font->height + 2*PAD;
	chromeh = 2*rowh;
	cbg = solid(0xECECECFF);
	ctab = solid(0xD4D4D4FF);
	cact = solid(0xFFFFFFFF);
	ctext = display->black;
	cfield = solid(0xFFFFFFFF);
	haschrome = 1;
	emit(EVRESIZE, 0, 0, Dx(screen->r), Dy(screen->r) - chromeh);
}

/* displock held. Draw the whole strip (tab row + toolbar + address bar). */
static void
drawchrome_locked(void)
{
	Point o;
	Rectangle r;
	int i, x0, winw, cx, ty;
	char *lbl, *s;

	if(!haschrome || screen == nil)
		return;
	o = screen->r.min;
	winw = Dx(screen->r);

	draw(screen, Rect(o.x, o.y, o.x+winw, o.y+chromeh), cbg, nil, ZP);

	/* tab row */
	for(i = 0; i < ntabs && i < MAXTABS; i++){
		x0 = i*TABW;
		if(x0 >= winw)
			break;
		r = Rect(o.x+x0, o.y, o.x+x0+TABW-2, o.y+rowh);
		draw(screen, r, i == activetab ? cact : ctab, nil, ZP);
		border(screen, r, 1, ctext, ZP);
		replclipr(screen, 0, insetrect(r, PAD));
		string(screen, Pt(r.min.x+PAD, r.min.y+PAD), ctext, ZP, font, tabtitle[i]);
		replclipr(screen, 0, screen->r);
		string(screen, Pt(r.max.x-11, r.min.y+PAD), ctext, ZP, font, "x");
	}
	/* + new-tab */
	x0 = ntabs*TABW;
	r = Rect(o.x+x0, o.y, o.x+x0+26, o.y+rowh);
	draw(screen, r, ctab, nil, ZP);
	border(screen, r, 1, ctext, ZP);
	string(screen, Pt(r.min.x+8, r.min.y+PAD), ctext, ZP, font, "+");

	/* toolbar: back / forward / reload-or-stop */
	for(i = 0; i < 3; i++){
		r = Rect(o.x+i*BTNW, o.y+rowh, o.x+(i+1)*BTNW-2, o.y+chromeh);
		draw(screen, r, ctab, nil, ZP);
		border(screen, r, 1, ctext, ZP);
		lbl = i == 0 ? "<" : i == 1 ? ">" : loading ? "x" : "r";
		string(screen, Pt(r.min.x+11, r.min.y+PAD), ctext, ZP, font, lbl);
	}

	/* Address field. The vertical inset is 2, NOT PAD: the row is only
	 * font->height + 2*PAD tall, so insetting the box by PAD left it exactly
	 * font->height, and the replclipr(insetrect(r,2)) below then clipped 4 more
	 * pixels off the drawable area — slicing the descenders, so "y" read as "u".
	 * At inset 2 the interior is font->height + 6, which fits a whole glyph
	 * (plus the 2px clip margin) with room to centre it. */
	r = Rect(o.x+3*BTNW+PAD, o.y+rowh+2, o.x+winw-PAD, o.y+chromeh-2);
	draw(screen, r, cfield, nil, ZP);
	border(screen, r, 1, ctext, ZP);
	s = addrfocus ? addr : curl;
	/* Centre the glyph box in the field interior rather than pinning it to the
	 * top, so ascenders and descenders get equal room. */
	ty = r.min.y + (Dy(r) - font->height) / 2;
	replclipr(screen, 0, insetrect(r, 2));
	if(addrfocus && addrsel && addrn > 0){
		/* Selected: text reversed out of a filled bar, and no caret — the
		 * whole field is the selection, so a caret would just be noise. */
		cx = r.min.x+4 + stringwidth(font, addr);
		draw(screen, Rect(r.min.x+4, r.min.y+2, cx, r.max.y-2), ctext, nil, ZP);
		string(screen, Pt(r.min.x+4, ty), cfield, ZP, font, addr);
	}else{
		string(screen, Pt(r.min.x+4, ty), ctext, ZP, font, s);
		if(addrfocus){
			cx = r.min.x+4 + stringnwidth(font, addr, addrcur);
			draw(screen, Rect(cx, r.min.y+2, cx+1, r.max.y-2), ctext, nil, ZP);
		}
	}
	replclipr(screen, 0, screen->r);
	flushimage(display, 1);
}

static void
drawchrome(void)
{
	qlock(&displock);
	drawchrome_locked();
	qunlock(&displock);
}

/* displock held. Give the address bar focus, loading it with the current URL.
 * sel=1 selects the whole thing (Ctrl-L), so typing replaces it; sel=0 just
 * places the caret at the end (mouse click). */
static void
focusaddr_locked(int sel)
{
	addrfocus = 1;
	strncpy(addr, curl, sizeof addr - 1);
	addr[sizeof addr - 1] = 0;
	addrn = strlen(addr);
	addrcur = addrn;
	addrsel = sel;
}

/* A button-1-down in the chrome strip (y < chromeh, window-relative). */
static void
chromemouse(int x, int y)
{
	int sub, i, focus;
	ulong idx;

	sub = 0;
	idx = 0;
	focus = 0;
	qlock(&displock);
	if(y < rowh){
		i = x / TABW;
		if(i >= 0 && i < ntabs){
			if(x >= (i+1)*TABW - 16){ sub = CMDCLOSE; idx = i; }
			else { sub = CMDSWITCH; idx = i; }
		}else if(x >= ntabs*TABW && x < ntabs*TABW + 26){
			sub = CMDNEWTAB;
		}
		addrfocus = 0;
	}else{
		if(x < BTNW) sub = CMDBACK;
		else if(x < 2*BTNW) sub = CMDFWD;
		else if(x < 3*BTNW) sub = loading ? CMDSTOP : CMDRELOAD;
		else focus = 1;
	}
	if(focus)
		focusaddr_locked(0);	/* click: caret only, keep what's there */
	drawchrome_locked();
	qunlock(&displock);
	if(sub)
		emitcmd(sub, idx, nil);
}

/* A key while the address bar has focus; returns 1 if consumed. */
static int
chromekey(Rune r)
{
	char tmp[UTFmax];
	int nb, nav;

	if(!addrfocus)
		return 0;
	nav = 0;
	qlock(&displock);
	/* A selected address behaves like every other text field: the next thing
	 * you type — printable, Backspace or Delete — replaces the whole lot. */
	if(addrsel && (r == Kbs || r == Kdel || (r >= 0x20 && r < 0xF000))){
		addrn = 0;
		addrcur = 0;
		addr[0] = 0;
		addrsel = 0;
		if(r == Kbs || r == Kdel){
			drawchrome_locked();
			qunlock(&displock);
			return 1;
		}
	}
	addrsel = 0;	/* any key reaching here collapses the selection */
	switch(r){
	case '\n':
	case '\r':
		addrfocus = 0;
		nav = 1;
		break;
	case Kesc:
		addrfocus = 0;
		break;
	/* Plan 9 line editing, which is what a 9front user's fingers expect. */
	case 0x01:	/* Ctrl-A: start of line */
		addrcur = 0;
		break;
	case 0x05:	/* Ctrl-E: end of line */
		addrcur = addrn;
		break;
	case 0x15:	/* Ctrl-U: clear the line */
		addrn = 0;
		addrcur = 0;
		addr[0] = 0;
		break;
	case Kbs:
		if(addrcur > 0){
			memmove(addr+addrcur-1, addr+addrcur, addrn-addrcur);
			addrcur--; addrn--; addr[addrn] = 0;
		}
		break;
	case Kdel:
		if(addrcur < addrn){
			memmove(addr+addrcur, addr+addrcur+1, addrn-addrcur-1);
			addrn--; addr[addrn] = 0;
		}
		break;
	case Kleft:
		if(addrcur > 0) addrcur--;
		break;
	case Kright:
		if(addrcur < addrn) addrcur++;
		break;
	default:
		if(r >= 0x20 && r < 0xF000){
			nb = runetochar(tmp, &r);
			if(addrn + nb < (int)sizeof addr - 1){
				memmove(addr+addrcur+nb, addr+addrcur, addrn-addrcur);
				memmove(addr+addrcur, tmp, nb);
				addrcur += nb; addrn += nb; addr[addrn] = 0;
			}
		}
		break;
	}
	drawchrome_locked();
	qunlock(&displock);
	if(nav)
		emitcmd(CMDNAV, 0, addr);
	return 1;
}

/* Browser-level chords, on SUPER (Kmod4) — the macOS Cmd layout. Caller checks
 * MODSUPER; this only maps the letter. Returns 1 if consumed.
 *
 * Super rather than Ctrl for two reasons. Ctrl chords arrive as control RUNES
 * (Ctrl-L is 0x0c), which collide head-on with Plan 9 line editing — Ctrl-W is
 * delete-word here, so binding it to close-tab would destroy a tab whenever a
 * 9front user rubbed out a word. Super carries the plain letter with only the
 * modifier bit set, so it collides with nothing and Super-W is free to mean
 * close-tab exactly as Cmd-W does. */
static int
ischromechord(Rune r)
{
	if(r >= 'A' && r <= 'Z')
		r += 'a' - 'A';
	return r == 'l' || r == 't' || r == 'r' || r == 'w';
}

static int
chromeshortcut(Rune r)
{
	if(r >= 'A' && r <= 'Z')
		r += 'a' - 'A';
	switch(r){
	case 'l':	/* Super-L: focus address bar, select all (upstream Ctrl-L/Cmd-L) */
		qlock(&displock);
		focusaddr_locked(1);
		drawchrome_locked();
		qunlock(&displock);
		return 1;
	case 't':	/* Super-T: new tab */
		emitcmd(CMDNEWTAB, 0, nil);
		return 1;
	case 'r':	/* Super-R: reload */
		emitcmd(CMDRELOAD, 0, nil);
		return 1;
	case 'w':	/* Super-W: close the active tab (the UI quits on the last one) */
		emitcmd(CMDCLOSE, activetab, nil);
		return 1;
	}
	return 0;
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
	int scroll;	/* GL9S: y=y0, h=y1, w=dy; pix=nil */
	int bgra;	/* GL9B: pixels are BGRA8888 (Plan9 ARGB32), not GL RGBA (ABGR32) */
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
	int full, bgra;
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
		if(memcmp(hdr, "GL9U", 4) == 0){	/* address bar URL */
			char *u;
			if(readn(framefd, hdr, 4) != 4)
				break;
			len = get32(hdr);
			u = malloc(len + 1);
			if(u == nil || readn(framefd, u, len) != (long)len){
				free(u);
				break;
			}
			u[len] = 0;
			qlock(&displock);
			enablechrome();
			strncpy(curl, u, sizeof curl - 1);
			curl[sizeof curl - 1] = 0;
			drawchrome_locked();
			qunlock(&displock);
			free(u);
			continue;
		}
		if(memcmp(hdr, "GL9L", 4) == 0){	/* loading + back/forward-enabled */
			if(readn(framefd, hdr, 4) != 4)
				break;
			qlock(&displock);
			enablechrome();
			loading = hdr[0];
			canback = hdr[1];
			canfwd = hdr[2];
			drawchrome_locked();
			qunlock(&displock);
			continue;
		}
		if(memcmp(hdr, "GL9K", 4) == 0){	/* tab list */
			uchar kb[8];
			ulong count, active, tl;
			char titles[MAXTABS][256];
			int i, nt, bad;
			if(readn(framefd, kb, 8) != 8)
				break;
			count = get32(kb);
			active = get32(kb + 4);
			nt = 0;
			bad = 0;
			for(i = 0; i < (int)count; i++){
				char *t;
				if(readn(framefd, hdr, 4) != 4){ bad = 1; break; }
				tl = get32(hdr);
				t = malloc(tl + 1);
				if(t == nil || readn(framefd, t, tl) != (long)tl){ free(t); bad = 1; break; }
				t[tl] = 0;
				if(nt < MAXTABS){
					strncpy(titles[nt], t, 255);
					titles[nt][255] = 0;
					nt++;
				}
				free(t);
			}
			if(bad)
				break;
			qlock(&displock);
			enablechrome();
			ntabs = nt;
			activetab = active < (ulong)nt ? (int)active : 0;
			for(i = 0; i < nt; i++)
				strcpy(tabtitle[i], titles[i]);
			drawchrome_locked();
			qunlock(&displock);
			continue;
		}
		x = y = 0;
		if(memcmp(hdr, "GL9S", 4) == 0){
			/* scroll: rows [y0,y1) moved up dy px; a pure blit */
			if(readn(framefd, hdr, 12) != 12)
				break;
			f = malloc(sizeof *f);
			if(f == nil)
				threadexitsall("malloc frame hdr");
			f->full = 0;
			f->scroll = 1;
			f->bgra = 0;
			f->x = 0;
			f->y = get32(hdr);
			f->h = get32(hdr + 4);
			f->w = get32(hdr + 8);
			f->pix = nil;
			enqueue(f);
			continue;
		}
		bgra = 0;
		if(memcmp(hdr, "GL9F", 4) == 0 || (bgra = memcmp(hdr, "GL9B", 4) == 0)){
			/* GL9B: full frame, but BGRA8888 (Ladybird/Skia) not GL RGBA */
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
		f->scroll = 0;
		f->bgra = bgra;
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
	ulong w, h, imchan;
	long n;
	Image *im;
	Point o;
	Frame *f, *fnext;
	Rectangle r;
	int y0, y1, dy;
	vlong lastns, blitns, t0, t1, t2;

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
	/* NB: don't be tempted to grow display->bufsize past what initdraw
	 * chose — the draw fd in a rio window namespace enforces its iounit
	 * and oversized flushes fail SILENTLY (blank window, no error).
	 * Full-frame loadimage costs ~70-135ms at 8K chunks on cirno; that's
	 * the price, and damage/scroll records avoid full frames anyway. */

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
	imchan = 0;
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
				fprint(2, "gl9win2 %s: %ludx%lud dt=%lldms t=%lldms skipped=%d\n",
					f->full ? "frame" : "damage",
					f->w, f->h, (now - lastns)/1000000,
					now/1000000, nskipped);
			lastns = now;
			nskipped = 0;
			blitns = now;
		}

		qlock(&displock);
		o = addpt(screen->r.min, Pt(0, chromeh));	/* page starts below the chrome strip */
		for(; f != nil; f = fnext){
			fnext = f->next;
			if(f->scroll){
				/* shift image + screen up: rows [y0,y1) by dy px.
				 * memdraw copies top-down, so an upward move over
				 * itself is overlap-safe. */
				y0 = f->y;
				y1 = f->h;
				dy = f->w;
				if(im != nil){
					if(y1 > Dy(im->r))
						y1 = Dy(im->r);
					if(y0 >= 0 && dy > 0 && y0 + dy < y1){
						r = Rect(0, y0, Dx(im->r), y1 - dy);
						draw(im, r, im, nil, Pt(0, y0 + dy));
						draw(screen, rectaddpt(r, o), screen, nil,
							addpt(Pt(0, y0 + dy), o));
					}
				}
				free(f);
				continue;
			}
			w = f->w;
			h = f->h;
			n = (long)w * h * 4;
			if(f->full){
				/* GL9B pixels are BGRA8888 == Plan 9 ARGB32; GL9F are GL
				 * RGBA == ABGR32. libdraw's draw() does the channel
				 * conversion to the screen for free — no per-frame repack. */
				ulong wantchan = f->bgra ? ARGB32 : ABGR32;
				/* reuse the draw image across same-size, same-format frames */
				if(im == nil || Dx(im->r) != (int)w || Dy(im->r) != (int)h || imchan != wantchan){
					if(im != nil)
						freeimage(im);
					im = allocimage(display, Rect(0, 0, w, h), wantchan, 0, DNofill);
					if(im == nil){
						qunlock(&displock);
						threadexitsall("allocimage");
					}
					imchan = wantchan;
				}
				t0 = kbddebug ? nsec() : 0;
				loadimage(im, im->r, f->pix, n);
				t1 = kbddebug ? nsec() : 0;
				/* letterbox undersized frames — but only the PAGE region, so the
				 * chrome strip (drawn above o.y) is never erased. */
				if((int)w < Dx(screen->r) || (int)h < Dy(screen->r) - chromeh)
					draw(screen, Rect(screen->r.min.x, o.y, screen->r.max.x, screen->r.max.y), display->black, nil, ZP);
				draw(screen, rectaddpt(im->r, o), im, nil, ZP);
				t2 = kbddebug ? nsec() : 0;
				if(kbddebug)
					fprint(2, "gl9win2 full: load=%lldms draw=%lldms\n",
						(t1-t0)/1000000, (t2-t1)/1000000);
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
