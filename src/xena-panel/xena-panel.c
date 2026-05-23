/*
 * xena-panel — WinXP-style taskbar for mxio (Plan 9)
 *
 * Bottom strip (30px) containing:
 *   [Start]   [Win1] [Win2] ...   12:34
 *
 * Reads /dev/wsys/N/{label,wctl,winid} to enumerate windows.
 * Resizes itself via /dev/wctl ("resize -r x y X Y").
 *
 * v0.1: clock + window list (read-only, no clicks yet).
 */

#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>

enum {
	PanelH      = 30,           /* panel height */
	StartBtnW   = 60,           /* Start button width */
	ClockW      = 70,           /* clock area width */
	WinBtnMaxW  = 160,          /* max width per window button */
	WinBtnMinW  = 40,
	BtnPad      = 4,
	Refresh     = 500,          /* ms between refresh */
	MaxWindows  = 32,
};

typedef struct WinEntry WinEntry;
struct WinEntry {
	int       id;
	char      label[64];
	int       current;
	Rectangle btnr;          /* button bounds (panel-local) for click hit-test */
};

Image  *panelbg;        /* gray gradient (WinXP taskbar) */
Image  *startbg;        /* green-ish gradient (Start button) */
Image  *startbgactive;
Image  *winbtnbg;       /* unselected window button */
Image  *winbtncur;      /* selected (current) window button */
Image  *txtcol;         /* white text on gradient */
Image  *txtdark;        /* dark text */
Font   *bigfont;

WinEntry  wins[MaxWindows];
int       nwins;
Rectangle startbtnr;    /* Start button bounds */

Mousectl *mc;
Keyboardctl *kc;

/* Write `current -id N` to /dev/wctl to bring window N to front.
 * Using -id explicitly avoids any ambiguity about which window the
 * 9P fid is tied to. */
void
focuswin(int id)
{
	int fd, n;
	char cmd[64];
	int dfd;
	char dbg[128];

	dfd = open("/tmp/xp.log", OWRITE);
	if(dfd < 0) dfd = create("/tmp/xp.log", OWRITE, 0644);
	if(dfd >= 0) seek(dfd, 0, 2);

	fd = open("/dev/wctl", OWRITE);
	if(fd < 0){
		fprint(2, "xena-panel: focuswin open: %r\n");
		if(dfd >= 0){
			n = snprint(dbg, sizeof dbg, "    focuswin: open /dev/wctl FAILED: %r\n");
			write(dfd, dbg, n);
			close(dfd);
		}
		return;
	}
	snprint(cmd, sizeof cmd, "current -id %d\n", id);
	n = write(fd, cmd, strlen(cmd));
	if(dfd >= 0){
		int e = snprint(dbg, sizeof dbg, "    focuswin: wrote '%s' n=%d errstr=%r\n",
		                cmd, n);
		write(dfd, dbg, e);
		close(dfd);
	}
	if(n < 0)
		fprint(2, "xena-panel: focuswin write: %r\n");
	close(fd);
}

/* Find an open launcher window by scanning /dev/wsys/N/label entries.
 * Returns the window id (>= 1) if found, or -1 if not. */
int
findlauncher(void)
{
	int dirfd, n, i, id, lfd, ln;
	Dir *d;
	char path[128], buf[64];

	dirfd = open("/dev/wsys", OREAD);
	if(dirfd < 0)
		return -1;
	n = dirreadall(dirfd, &d);
	close(dirfd);
	if(n <= 0){
		if(n == 0) free(d);
		return -1;
	}

	for(i = 0; i < n; i++){
		id = atoi(d[i].name);
		if(id <= 0 && d[i].name[0] != '0') continue;
		snprint(path, sizeof path, "/dev/wsys/%d/label", id);
		lfd = open(path, OREAD);
		if(lfd < 0) continue;
		ln = read(lfd, buf, sizeof buf - 1);
		close(lfd);
		if(ln <= 0) continue;
		buf[ln] = 0;
		/* labels often have trailing whitespace */
		while(ln > 0 && (buf[ln-1] == '\n' || buf[ln-1] == ' '))
			buf[--ln] = 0;
		if(strcmp(buf, "launcher") == 0){
			free(d);
			return id;
		}
	}
	free(d);
	return -1;
}

/* Close window id by writing 'delete' to its wctl. */
void
closewin(int id)
{
	int fd;
	char path[64];
	snprint(path, sizeof path, "/dev/wsys/%d/wctl", id);
	fd = open(path, OWRITE);
	if(fd < 0) return;
	write(fd, "delete\n", 7);
	close(fd);
}

/* Toggle the launcher: if open, close it; otherwise spawn it.
 *
 * Originally this exec'd /bin/window which forwards to a new wctl entry.
 * That fork-exec chain was failing silently inside the panel's namespace
 * (no launcher process ever appeared, no error visible). Instead, post
 * the wctl 'new' command directly so we control the result. */
void
spawnlauncher(void)
{
	int fd, n;
	char cmd[256];
	int screenH = 768;
	int menuH   = 220;
	int menuW   = 200;
	int miny    = screenH - PanelH - menuH;
	int maxy    = screenH - PanelH;
	int dfd, existing;
	char dbg[256];

	dfd = open("/tmp/xp.log", OWRITE);
	if(dfd < 0) dfd = create("/tmp/xp.log", OWRITE, 0644);
	if(dfd >= 0) seek(dfd, 0, 2);

	existing = findlauncher();
	if(existing > 0){
		if(dfd >= 0){
			n = snprint(dbg, sizeof dbg, "    spawnlauncher: closing existing id=%d\n", existing);
			write(dfd, dbg, n);
			close(dfd);
		}
		closewin(existing);
		return;
	}

	fd = open("/dev/wctl", OWRITE);
	if(fd < 0){
		if(dfd >= 0){
			n = snprint(dbg, sizeof dbg, "    spawnlauncher: open wctl FAILED: %r\n");
			write(dfd, dbg, n);
			close(dfd);
		}
		return;
	}
	n = snprint(cmd, sizeof cmd,
		"new -pid 0 -r %d %d %d %d /bin/launcher\n",
		0, miny, menuW, maxy);
	n = write(fd, cmd, strlen(cmd));
	close(fd);

	if(dfd >= 0){
		int e = snprint(dbg, sizeof dbg, "    spawnlauncher: wrote '%s' n=%d errstr=%r\n", cmd, n);
		write(dfd, dbg, e);
		close(dfd);
	}
}

/* Handle a left-click at point p (screen coords) */
void
panelclick(Point p)
{
	int i, fd, n;
	char dbg[256];

	/* debug log */
	fd = open("/tmp/xp.log", OWRITE);
	if(fd < 0) fd = create("/tmp/xp.log", OWRITE, 0644);
	if(fd >= 0) seek(fd, 0, 2);

	if(fd >= 0){
		n = snprint(dbg, sizeof dbg, "click (%d,%d) nwins=%d\n", p.x, p.y, nwins);
		write(fd, dbg, n);
	}

	if(ptinrect(p, startbtnr)){
		if(fd >= 0){ write(fd, "  -> START\n", 11); close(fd); }
		spawnlauncher();
		return;
	}
	for(i = 0; i < nwins; i++){
		if(fd >= 0){
			n = snprint(dbg, sizeof dbg, "  wins[%d] id=%d label=%s btnr=(%d,%d-%d,%d)\n",
			            i, wins[i].id, wins[i].label,
			            wins[i].btnr.min.x, wins[i].btnr.min.y,
			            wins[i].btnr.max.x, wins[i].btnr.max.y);
			write(fd, dbg, n);
		}
		if(ptinrect(p, wins[i].btnr)){
			if(fd >= 0){
				n = snprint(dbg, sizeof dbg, "  MATCH -> focuswin(%d)\n", wins[i].id);
				write(fd, dbg, n);
				close(fd);
			}
			focuswin(wins[i].id);
			return;
		}
	}
	if(fd >= 0){ write(fd, "  NO MATCH\n", 11); close(fd); }
}


void
initcolors(void)
{
	/* WinXP taskbar: blue-gray gradient
	 * We approximate with a single mid-tone */
	panelbg       = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x3c5b91FF);
	startbg       = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x4d8c4cFF); /* green */
	startbgactive = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x65b463FF);
	winbtnbg      = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x6080b8FF); /* lighter blue */
	winbtncur     = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xa6cbf0FF); /* highlight */
	txtcol        = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xffffffFF);
	txtdark       = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x202020FF);
}

/*
 * Resize our own window to a strip at the bottom of the screen.
 * Writes to /dev/wctl:
 *    resize -r minx miny maxx maxy
 */
void
moveself(void)
{
	int fd, n;
	char buf[256];
	int screenW, screenH;
	Rectangle r;

	/* read /dev/screen to discover screen size — fallback to assumption */
	screenW = 1024;
	screenH = 768;

	fd = open("/dev/screen", OREAD);
	if(fd >= 0){
		/* /dev/screen starts with a 5*12 byte header containing chan + rect */
		char hdr[5*12+1];
		n = read(fd, hdr, sizeof hdr - 1);
		close(fd);
		if(n >= 5*12){
			hdr[5*12] = 0;
			/* fields are 12-char ascii decimals: chan, r.min.x, r.min.y, r.max.x, r.max.y */
			char field[13];
			memcpy(field, hdr+12, 12); field[12] = 0;
			/* skip — chan field is non-numeric format. just use defaults */
		}
	}

	r.min.x = 0;
	r.max.x = screenW;
	r.min.y = screenH - PanelH;
	r.max.y = screenH;

	fd = open("/dev/wctl", OWRITE);
	if(fd < 0){
		fprint(2, "xena-panel: can't open /dev/wctl: %r\n");
		return;
	}
	n = snprint(buf, sizeof buf, "resize -r %d %d %d %d\n",
	            r.min.x, r.min.y, r.max.x, r.max.y);
	write(fd, buf, n);
	close(fd);
}

/*
 * Enumerate windows from /dev/wsys.
 * For each /dev/wsys/N: read label, wctl (for current state).
 * Skip ourselves.
 */
int
readwins(void)
{
	int fd, dirfd, n, i;
	Dir *d;
	char path[128], buf[256];
	int myid;

	/* find own winid */
	myid = -1;
	fd = open("/dev/winid", OREAD);
	if(fd >= 0){
		n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if(n > 0){
			buf[n] = 0;
			myid = atoi(buf);
		}
	}

	dirfd = open("/dev/wsys", OREAD);
	if(dirfd < 0){
		fprint(2, "xena-panel: can't open /dev/wsys: %r\n");
		return 0;
	}

	nwins = 0;
	while(nwins < MaxWindows && (n = dirread(dirfd, &d)) > 0){
		for(i = 0; i < n && nwins < MaxWindows; i++){
			int id = atoi(d[i].name);
			char label[64];
			int m;
			if(id == myid) continue;          /* skip ourselves */
			if(id == 0 && d[i].name[0] != '0') continue;

			/* read label first so we can skip chrome windows */
			label[0] = 0;
			snprint(path, sizeof path, "/dev/wsys/%d/label", id);
			fd = open(path, OREAD);
			if(fd >= 0){
				m = read(fd, label, sizeof label - 1);
				close(fd);
				if(m > 0){
					label[m] = 0;
					if(m > 0 && label[m-1] == '\n')
						label[m-1] = 0;
				}
			}
			/* chrome windows are not user windows — skip in taskbar */
			if(strcmp(label, "xena-panel") == 0) continue;
			if(strcmp(label, "launcher")   == 0) continue;

			wins[nwins].id = id;
			wins[nwins].current = 0;
			strecpy(wins[nwins].label,
			        wins[nwins].label + sizeof wins[nwins].label,
			        label);

			/* read wctl */
			snprint(path, sizeof path, "/dev/wsys/%d/wctl", id);
			fd = open(path, OREAD);
			if(fd >= 0){
				m = read(fd, buf, sizeof buf - 1);
				close(fd);
				if(m > 0){
					buf[m] = 0;
					if(strstr(buf, "current") != nil
					   && strstr(buf, "notcurrent") == nil)
						wins[nwins].current = 1;
				}
			}

			nwins++;
		}
		free(d);
	}
	close(dirfd);
	return nwins;
}

void
drawclock(Rectangle r)
{
	char buf[16];
	Tm *t;
	long now;
	Point p;
	int w;

	now = time(0);
	t = localtime(now);
	snprint(buf, sizeof buf, "%02d:%02d", t->hour, t->min);

	w = stringwidth(font, buf);
	p.x = r.min.x + (Dx(r) - w) / 2;
	p.y = r.min.y + (Dy(r) - font->height) / 2;
	string(screen, p, txtcol, ZP, font, buf);
}

void
drawstartbtn(Rectangle r, int active)
{
	Point p;
	int w;
	const char *txt = "Start";

	draw(screen, r, active ? startbgactive : startbg, nil, ZP);
	/* simple border */
	border(screen, r, 1, txtdark, ZP);

	w = stringwidth(font, txt);
	p.x = r.min.x + (Dx(r) - w) / 2;
	p.y = r.min.y + (Dy(r) - font->height) / 2;
	string(screen, p, txtcol, ZP, font, txt);
}

void
drawwinbtn(Rectangle r, WinEntry *w)
{
	Point p;
	int txtw, maxw;
	char shortbuf[40];

	draw(screen, r, w->current ? winbtncur : winbtnbg, nil, ZP);
	border(screen, r, 1, txtdark, ZP);

	/* truncate label */
	maxw = Dx(r) - 8;
	strncpy(shortbuf, w->label, sizeof shortbuf - 1);
	shortbuf[sizeof shortbuf - 1] = 0;
	while(stringwidth(font, shortbuf) > maxw && strlen(shortbuf) > 1)
		shortbuf[strlen(shortbuf) - 1] = 0;

	txtw = stringwidth(font, shortbuf);
	p.x = r.min.x + (Dx(r) - txtw) / 2;
	p.y = r.min.y + (Dy(r) - font->height) / 2;
	string(screen, p, w->current ? txtdark : txtcol, ZP, font, shortbuf);
}

void
redraw(void)
{
	Rectangle pr, startr, clockr, listr;
	int btnw, x, i;

	pr = screen->r;

	/* background */
	draw(screen, pr, panelbg, nil, ZP);

	/* divide into sections: start | win-list | clock */
	startr = pr;
	startr.max.x = startr.min.x + StartBtnW;
	startr = insetrect(startr, BtnPad / 2);
	startbtnr = startr;             /* remember for hit-test */

	clockr = pr;
	clockr.min.x = clockr.max.x - ClockW;

	listr = pr;
	listr.min.x = startr.max.x + BtnPad;
	listr.max.x = clockr.min.x - BtnPad;

	drawstartbtn(startr, 0);
	drawclock(clockr);

	/* window list: equal-width buttons, capped at WinBtnMaxW */
	if(nwins > 0 && Dx(listr) > 20){
		btnw = (Dx(listr) - (nwins - 1) * BtnPad) / nwins;
		if(btnw > WinBtnMaxW) btnw = WinBtnMaxW;
		if(btnw < WinBtnMinW) btnw = WinBtnMinW;

		x = listr.min.x;
		for(i = 0; i < nwins; i++){
			Rectangle wr = Rect(x, listr.min.y + 1, x + btnw, listr.max.y - 1);
			if(wr.max.x > listr.max.x) wr.max.x = listr.max.x;
			wins[i].btnr = wr;     /* remember for hit-test */
			drawwinbtn(wr, &wins[i]);
			x += btnw + BtnPad;
			if(x >= listr.max.x) break;
		}
		/* zero out remaining entries' btnr so they don't accidentally match */
		for(; i < nwins; i++)
			wins[i].btnr = ZR;
	}

	flushimage(display, 1);
}

void
eresized(int new)
{
	if(new && getwindow(display, Refnone) < 0)
		sysfatal("xena-panel: cannot reattach: %r");
	redraw();
}

/* every 'Refresh' ms: re-read window list, re-render */
void
ticker(void *)
{
	for(;;){
		sleep(Refresh);
		readwins();
		redraw();
	}
}

void
threadmain(int argc, char **argv)
{
	USED(argc); USED(argv);

	if(initdraw(nil, nil, "xena-panel") < 0)
		sysfatal("initdraw: %r");

	initcolors();
	moveself();              /* resize self to bottom strip */
	/* re-get the screen image since we moved */
	getwindow(display, Refnone);

	/* initmouse/initkeyboard AFTER moveself so they bind to the
	 * resized window, not the original size. */
	mc = initmouse(nil, screen);
	if(mc == nil) sysfatal("initmouse: %r");

	kc = initkeyboard(nil);
	if(kc == nil) sysfatal("initkeyboard: %r");

	readwins();
	redraw();

	/* background tick thread */
	proccreate(ticker, nil, 8*1024);

	/* event loop — handle mouse for click-to-focus */
	{
		int prevbtns = 0;
		for(;;){
			Alt a[] = {
				{ mc->c,       nil, CHANRCV },
				{ mc->resizec, nil, CHANRCV },
				{ kc->c,       nil, CHANRCV },
				{ nil,         nil, CHANEND },
			};
			Mouse m;
			int k, n;

			a[0].v = &m;
			a[1].v = &n;
			a[2].v = &k;

			switch(alt(a)){
			case 0:
				/* fresh button-1 press (edge-triggered) */
				if((m.buttons & 1) && !(prevbtns & 1)){
					panelclick(m.xy);
				}
				prevbtns = m.buttons;
				break;
			case 1:
				eresized(1);
				break;
			case 2:
				/* keyboard — drop */
				break;
			}
		}
	}
}
