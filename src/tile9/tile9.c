/*
 * tile9 — micro tiling window manager for mxio/rio.
 *
 * Reimplements the Magnet-style yabai + Karabiner setup:
 *
 *	Ctrl+Alt+Left		left half	(yabai --grid 1:2:0:0:1:1)
 *	Ctrl+Alt+Right		right half	(          --grid 1:2:1:0:1:1)
 *	Ctrl+Alt+Up		top half	(          --grid 2:1:0:0:1:1)
 *	Ctrl+Alt+Down		bottom half	(          --grid 2:1:0:1:1:1)
 *	Ctrl+Alt+Enter		maximize	(          --grid 1:1:0:0:1:1)
 *	Alt+1..6		launch an app
 *
 * Karabiner's Fn+j/k/l/i arrow layer has no equivalent here (Plan 9 has no
 * Fn modifier), so j/k/l double as left/down/right in the chords above.
 * There is deliberately no 'i' for up: Ctrl+I is TAB, and mxio's Alt+Tab
 * handler consumes it before the tap ever sees it. Use Ctrl+Alt+Up.
 *
 * Global hotkeys come from /dev/kbdtap: rio hands us every keyboard message
 * and delivers only what we write back, so a chord we act on is swallowed.
 * Geometry is applied by writing "resize -r" to /dev/wsys/<id>/wctl; the
 * focused window id arrives on the tap itself as a 'z<id>' context message.
 */
#include <u.h>
#include <libc.h>
#include <keyboard.h>

enum {
	Pad	= 10,	/* yabai {top,bottom,left,right}_padding */
	Gap	= 10,	/* yabai window_gap */
	Nbuf	= 1024,
};

typedef struct Key Key;
struct Key {
	Rune	r;
	int	rows, cols, x, y, w, h;	/* yabai --grid rows:cols:x:y:w:h */
};

Key keys[] = {
	{ Kleft,	1,2, 0,0, 1,1 },
	{ 'j',		1,2, 0,0, 1,1 },
	{ Kright,	1,2, 1,0, 1,1 },
	{ 'l',		1,2, 1,0, 1,1 },
	{ Kup,		2,1, 0,0, 1,1 },
	{ Kdown,	2,1, 0,1, 1,1 },
	{ 'k',		2,1, 0,1, 1,1 },
	{ '\n',		1,1, 0,0, 1,1 },
	{ 0, }
};

/*
 * Alt+digit launchers. The Karabiner config opens Safari/Ghostty/Zed/
 * Things3/Rider; those don't exist here, so these are the nearest thing
 * the image ships. Override in $home/lib/tile9 with "<digit> <command>".
 */
char *apps[10] = {
	[1]	"window mothra",
	[2]	"window /bin/rc",
	[3]	"window acme",
	[4]	"window sam",
	[5]	"window new-pi9",
	[6]	"window stats -lmisce",
};

int	tapfd = -1;
int	curid = -1;
int	sx0, sy0, sx1, sy1;
int	debug;

void
dump(char *tag, char *m, int n)
{
	char *p;
	Rune r;

	fprint(2, "tile9: %s %c", tag, *m);
	for(p = m+1; *p != 0; ){
		p += chartorune(&r, p);
		fprint(2, " %.4ux", r);
	}
	fprint(2, " (n=%d)\n", n);
}

void
getscreen(void)
{
	char buf[61];
	int fd, n;

	/* ponytail: the fallback only matters if /dev/screen is unreadable */
	sx0 = 0, sy0 = 0, sx1 = 1024, sy1 = 768;
	if((fd = open("/dev/screen", OREAD)) < 0)
		return;
	n = readn(fd, buf, 60);
	close(fd);
	if(n < 60)
		return;
	buf[60] = 0;
	sx0 = atoi(buf+12);
	sy0 = atoi(buf+24);
	sx1 = atoi(buf+36);
	sy1 = atoi(buf+48);
}

/*
 * Place the focused window in cell (k->x,k->y,k->w,k->h) of a rows x cols
 * grid. `reserve` is the strip along the bottom of the screen to keep clear.
 */
void
tile(Key *k, int reserve, char *out, int nout)
{
	int wx0, wy0, wx1, wy1, cw, ch, x0, y0, x1, y1;

	wx0 = sx0 + Pad;
	wy0 = sy0 + Pad;
	wx1 = sx1 - Pad;
	wy1 = sy1 - reserve - Pad;

	cw = (wx1 - wx0 - (k->cols-1)*Gap) / k->cols;
	ch = (wy1 - wy0 - (k->rows-1)*Gap) / k->rows;

	x0 = wx0 + k->x * (cw + Gap);
	y0 = wy0 + k->y * (ch + Gap);
	x1 = x0 + k->w * cw + (k->w-1) * Gap;
	y1 = y0 + k->h * ch + (k->h-1) * Gap;
	/* the last cell absorbs the integer-division slack, as yabai does */
	if(k->x + k->w == k->cols)
		x1 = wx1;
	if(k->y + k->h == k->rows)
		y1 = wy1;

	snprint(out, nout, "resize -r %d %d %d %d\n", x0, y0, x1, y1);
}

/*
 * One read of /dev/wsys/<name>/<file>, NUL-terminated. A second read of a
 * wctl blocks forever and the hung reader makes later opens fail "file in
 * use", so this never reads twice and always closes.
 */
int
readwin(char *name, char *file, char *buf, int nbuf)
{
	char path[64];
	int fd, n;

	snprint(path, sizeof path, "/dev/wsys/%s/%s", name, file);
	if((fd = open(path, OREAD)) < 0)
		return -1;
	n = read(fd, buf, nbuf-1);
	close(fd);
	if(n <= 0)
		return -1;
	buf[n] = 0;
	return 0;
}

/*
 * rio only announces the focused window on the tap when the focus CHANGES,
 * and it remembers the last window it announced across tap opens — so a
 * freshly started tile9 can wait forever for a 'z' that never comes.
 * Fall back to the same scan the taskbar does.
 */
int
findcurrent(void)
{
	char buf[256];
	Dir *d;
	int dirfd, i, n, id;

	if((dirfd = open("/dev/wsys", OREAD)) < 0)
		return -1;
	id = -1;
	while(id < 0 && (n = dirread(dirfd, &d)) > 0){
		for(i = 0; i < n; i++){
			if(readwin(d[i].name, "wctl", buf, sizeof buf) < 0)
				continue;
			if(strstr(buf, "current") != nil && strstr(buf, "notcurrent") == nil){
				id = atoi(d[i].name);
				break;
			}
		}
		free(d);
	}
	close(dirfd);
	return id;
}

/*
 * Height of the strip along the bottom of the screen that the taskbar
 * occupies, or 0 when there is no taskbar. Discovered rather than hardcoded:
 * tile9 runs both on the agent9 desktop and on a stock rio, and a fixed
 * reserve leaves a dead strip on the latter. Rechecked per keystroke because
 * riostart starts tile9 before it creates the panel window.
 */
int
panelreserve(void)
{
	char buf[256];
	Dir *d;
	int dirfd, i, n, res;

	res = 0;
	if((dirfd = open("/dev/wsys", OREAD)) < 0)
		return 0;
	while(res == 0 && (n = dirread(dirfd, &d)) > 0){
		for(i = 0; i < n; i++){
			if(readwin(d[i].name, "label", buf, sizeof buf) < 0)
				continue;
			if(strcmp(buf, "xena-panel") != 0)
				continue;
			if(readwin(d[i].name, "wctl", buf, sizeof buf) < 0)
				continue;
			/* wctl is four 12-column fields: x0 y0 x1 y1 */
			if(atoi(buf+36) >= sy1)		/* sits on the bottom edge */
				res = sy1 - atoi(buf+12);
			break;
		}
		free(d);
	}
	close(dirfd);
	return res;
}

void
resizecur(Key *k)
{
	char cmd[64], path[64];
	int fd;

	if(curid < 0)
		curid = findcurrent();
	if(curid < 0)
		return;
	tile(k, panelreserve(), cmd, sizeof cmd);
	snprint(path, sizeof path, "/dev/wsys/%d/wctl", curid);
	if((fd = open(path, OWRITE)) < 0){	/* window went away — re-scan once */
		if((curid = findcurrent()) < 0)
			return;
		snprint(path, sizeof path, "/dev/wsys/%d/wctl", curid);
		if((fd = open(path, OWRITE)) < 0)
			return;
	}
	if(debug)
		fprint(2, "tile9: %s <- %s", path, cmd);
	write(fd, cmd, strlen(cmd));
	close(fd);
}

void
runcmd(char *cmd)
{
	switch(rfork(RFPROC|RFFDG|RFNOTEG|RFNOWAIT)){
	case -1:
		return;
	case 0:
		execl("/bin/rc", "rc", "-c", cmd, nil);
		exits("exec");
	}
}

/* $home/lib/tile9: one "<digit> <rc command>" per line. */
void
loadapps(void)
{
	char path[128], *b, *p, *e, *nl;
	int fd, n, d;

	snprint(path, sizeof path, "%s/lib/tile9", getenv("home"));
	if((fd = open(path, OREAD)) < 0)
		return;
	if((b = malloc(8192)) == nil){
		close(fd);
		return;
	}
	n = readn(fd, b, 8191);
	close(fd);
	if(n <= 0){
		free(b);
		return;
	}
	b[n] = 0;
	for(p = b; p != nil && *p != 0; p = nl){
		if((nl = strchr(p, '\n')) != nil)
			*nl++ = 0;
		while(*p == ' ' || *p == '\t')
			p++;
		if(*p < '0' || *p > '9')
			continue;
		d = *p++ - '0';
		while(*p == ' ' || *p == '\t')
			p++;
		for(e = p+strlen(p); e > p && (e[-1] == ' ' || e[-1] == '\t'); e--)
			e[-1] = 0;
		if(*p != 0)
			apps[d] = strdup(p);
	}
	free(b);
}

Key*
chord(char *held)
{
	Key *k;

	if(utfrune(held, Kctl) == nil || utfrune(held, Kalt) == nil)
		return nil;
	for(k = keys; k->r != 0; k++)
		if(utfrune(held, k->r) != nil)
			return k;
	return nil;
}

/* Alt+digit with no other modifier, and something bound to it. 0 if not. */
int
applaunch(char *held)
{
	Rune r;

	if(utfrune(held, Kalt) == nil || utfrune(held, Kctl) != nil)
		return 0;
	for(r = '1'; r <= '9'; r++)
		if(utfrune(held, r) != nil && apps[r-'0'] != nil)
			return r - '0';
	return 0;
}

void
selftest(void)
{
	struct {
		Rune	r;
		int	reserve;
		char	*want;
	} t[] = {
		/* agent9 desktop: 30px taskbar along the bottom */
		{ Kleft,  30, "resize -r 10 10 507 728\n" },
		{ Kright, 30, "resize -r 517 10 1014 728\n" },
		{ Kup,    30, "resize -r 10 10 1014 364\n" },
		{ Kdown,  30, "resize -r 10 374 1014 728\n" },
		{ '\n',   30, "resize -r 10 10 1014 728\n" },
		/* stock rio: no taskbar, tiles must reach the bottom padding */
		{ Kleft,   0, "resize -r 10 10 507 758\n" },
		{ Kdown,   0, "resize -r 10 389 1014 758\n" },
		{ '\n',    0, "resize -r 10 10 1014 758\n" },
	};
	char got[64], held[8];
	Rune alt = Kalt;
	Key *k;
	int i;

	sx0 = 0, sy0 = 0, sx1 = 1024, sy1 = 768;
	for(i = 0; i < nelem(t); i++){
		for(k = keys; k->r != t[i].r; k++)
			assert(k->r != 0);
		tile(k, t[i].reserve, got, sizeof got);
		if(strcmp(got, t[i].want) != 0)
			sysfatal("grid %C reserve %d: got %q want %q", t[i].r, t[i].reserve, got, t[i].want);
	}
	if(chord("\1\2") != nil)		/* keys held, but not ctl+alt */
		sysfatal("chord matched without ctl+alt");
	if(applaunch("2") != 0)			/* digit held, but not alt */
		sysfatal("applaunch matched without alt");
	held[runetochar(held, &alt)] = 0;
	strcat(held, "2");
	if(applaunch(held) != 2)
		sysfatal("applaunch missed a bound Alt+digit");
	held[runetochar(held, &alt)] = 0;
	strcat(held, "7");			/* nothing bound to Alt+7 */
	if(applaunch(held) != 0)
		sysfatal("applaunch fired on an unbound Alt+digit");
	print("tile9: %d grid cases ok\n", i);
}

void
usage(void)
{
	fprint(2, "usage: tile9 [-dt]\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	char buf[Nbuf], *p;
	Rune fired;
	Key *k;
	int n, d, pending;

	ARGBEGIN{
	case 'd':
		debug++;
		break;
	case 't':
		selftest();
		exits(nil);
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	loadapps();
	getscreen();
	if((tapfd = open("/dev/kbdtap", ORDWR|OCEXEC)) < 0)
		sysfatal("open /dev/kbdtap: %r");

	fired = 0;
	pending = 0;
	while((n = read(tapfd, buf, sizeof buf - 1)) > 0){
		buf[n] = 0;
		p = buf;
		if(debug)
			dump("tap", p, n);
		switch(*p){
		case 'z':			/* focus changed; rio drops these on write */
			curid = atoi(p+1);
			continue;
		case 'k':
		case 'K':
			pending = 0;
			if(*p == 'K')
				fired = 0;
			else if((k = chord(p+1)) != nil){
				if(k->r != fired){
					fired = k->r;
					pending = 1;
					resizecur(k);
				}
			} else if((d = applaunch(p+1)) != 0){
				if(d + '0' != fired){
					fired = d + '0';
					pending = 1;
					runcmd(apps[d]);
				}
			} else
				fired = 0;
			break;
		case 'c':
			/*
			 * Drop the character the chord we just acted on produces.
			 * It cannot be recognised by its rune: under Ctrl the key
			 * arrives control-mapped (Ctrl+K is 0x0B, an arrow is
			 * 0xF800), nothing like the rune in the key-state message.
			 * rio always sends 'k' before 'c', so "we just fired" is
			 * the reliable test. Stays armed for autorepeat and is
			 * disarmed by the next key-state message.
			 */
			if(pending)
				continue;
			break;
		}
		if(write(tapfd, p, n) != n)	/* n already covers the trailing NUL */
			break;
	}
	exits(nil);
}
