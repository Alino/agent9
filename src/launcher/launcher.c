/*
 * launcher — WinXP-style Start menu for xena-panel
 *
 * Pops up as a tall narrow window above the panel.  Lists items, click to run.
 * Exits after launching an item or losing focus.
 *
 * Spawned via `window -r MINX MINY MAXX MAXY launcher` by xena-panel.
 */

#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>

typedef struct Item Item;
struct Item {
	char     *label;
	char     *cmd;        /* full command to feed `rc -c` */
	Rectangle r;          /* drawn rect for hit-test */
};

/*
 * Menu items: label + shell command.
 * `window` is rio's spawn-new-window command.
 */
Item items[] = {
	{ "Pi9",         "window new-pi9",                       {0,0,0,0} },
	{ "Rc Shell",    "window /bin/rc",                       {0,0,0,0} },
	{ "Stats",       "window stats -lmisce",                 {0,0,0,0} },
	{ "Acme",        "window acme",                          {0,0,0,0} },
	{ "Faces",       "window faces -i",                      {0,0,0,0} },
	{ "Files (sam)", "window sam",                           {0,0,0,0} },
	{ "Clock",       "window clock",                         {0,0,0,0} },
	{ "Reboot",      "fshalt -r",                            {0,0,0,0} },
	{ "Halt",        "fshalt",                               {0,0,0,0} },
	{ nil,           nil,                                    {0,0,0,0} },
};

enum {
	ItemH    = 22,
	Padx     = 8,
	HeaderH  = 24,
};

Image *bg;        /* light blue background */
Image *hl;        /* highlight */
Image *header;    /* darker blue header */
Image *txtdark;
Image *txtlight;

Mousectl *mc;

int hoverIdx = -1;

void
initcolors(void)
{
	bg       = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xe6efffFF);
	hl       = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x3c5b91FF);
	header   = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x0a246aFF);
	txtdark  = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x202020FF);
	txtlight = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xffffffFF);
}

void
redraw(void)
{
	Rectangle r, hdr, ir;
	Point p;
	int i, n;

	r = screen->r;

	/* background fill */
	draw(screen, r, bg, nil, ZP);

	/* header strip with title */
	hdr = r;
	hdr.max.y = hdr.min.y + HeaderH;
	draw(screen, hdr, header, nil, ZP);
	p.x = hdr.min.x + Padx;
	p.y = hdr.min.y + (HeaderH - font->height) / 2;
	string(screen, p, txtlight, ZP, font, "  xena");

	/* items */
	n = 0;
	for(i = 0; items[i].label != nil; i++) n++;

	ir.min.x = r.min.x;
	ir.max.x = r.max.x;
	ir.min.y = hdr.max.y;
	ir.max.y = ir.min.y + ItemH;

	for(i = 0; i < n; i++){
		items[i].r = ir;
		if(i == hoverIdx)
			draw(screen, ir, hl, nil, ZP);
		else
			draw(screen, ir, bg, nil, ZP);

		p.x = ir.min.x + Padx;
		p.y = ir.min.y + (ItemH - font->height) / 2;
		string(screen, p, i == hoverIdx ? txtlight : txtdark, ZP, font,
		       items[i].label);

		ir = rectaddpt(ir, Pt(0, ItemH));
	}

	/* border */
	border(screen, r, 1, txtdark, ZP);
	flushimage(display, 1);
}

void
runcmd(const char *cmd)
{
	int pid;

	pid = fork();
	if(pid < 0){
		fprint(2, "launcher: fork: %r\n");
		return;
	}
	if(pid == 0){
		/* child — detach from parent, exec rc -c */
		rfork(RFNOTEG);
		execl("/bin/rc", "rc", "-c", cmd, nil);
		fprint(2, "launcher: exec /bin/rc -c '%s': %r\n", cmd);
		exits("exec failed");
	}
	/* parent — let child run, don't wait */
}

int
findItem(Point p)
{
	int i;
	for(i = 0; items[i].label != nil; i++)
		if(ptinrect(p, items[i].r))
			return i;
	return -1;
}

void
eresized(int new)
{
	if(new && getwindow(display, Refnone) < 0)
		sysfatal("launcher: cannot reattach: %r");
	redraw();
}

/* Watcher thread: blocks on /dev/wctl reads. Each read returns the current
 * window state; the read blocks until the state changes. As soon as we see
 * 'notcurrent' (lost focus), exit. This gives WinXP-style click-outside-
 * to-dismiss behaviour. */
void
focuswatcher(void *)
{
	int fd, n, sawcurrent;
	char buf[256];

	threadsetname("focuswatcher");
	sawcurrent = 0;
	for(;;){
		fd = open("/dev/wctl", OREAD);
		if(fd < 0) return;
		n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if(n <= 0){
			sleep(50);
			continue;
		}
		buf[n] = 0;
		/* wctl line has 'current' or 'notcurrent' as the 5th field.
		 * Don't react to the first read (still in startup); wait
		 * until we've seen the launcher get focus once. */
		if(strstr(buf, "notcurrent") != nil){
			if(sawcurrent)
				threadexitsall(nil);
		} else if(strstr(buf, "current") != nil) {
			sawcurrent = 1;
		}
		/* tiny sleep to avoid spinning if wctl returns immediately */
		sleep(50);
	}
}

void
threadmain(int argc, char **argv)
{
	USED(argc); USED(argv);

	if(initdraw(nil, nil, "launcher") < 0)
		sysfatal("initdraw: %r");

	mc = initmouse(nil, screen);
	if(mc == nil) sysfatal("initmouse: %r");

	initcolors();
	redraw();

	/* dismiss on focus loss */
	proccreate(focuswatcher, nil, 8*1024);

	{
		int prevbtns = 0;
		for(;;){
			Alt a[] = {
				{ mc->c,       nil, CHANRCV },
				{ mc->resizec, nil, CHANRCV },
				{ nil,         nil, CHANEND },
			};
			Mouse m;
			int n;

			a[0].v = &m;
			a[1].v = &n;

			switch(alt(a)){
			case 0:
				{
					int idx = findItem(m.xy);
					if(idx != hoverIdx){
						hoverIdx = idx;
						redraw();
					}
					/* button-1 press edge */
					if((m.buttons & 1) && !(prevbtns & 1)){
						if(idx >= 0){
							runcmd(items[idx].cmd);
							/* close ourselves */
							threadexitsall(nil);
						}
					}
					prevbtns = m.buttons;
				}
				break;
			case 1:
				eresized(1);
				break;
			}
		}
	}
}
