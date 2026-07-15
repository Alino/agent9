/*
    SDL 1.2 video backend for Plan 9 / 9front, via the gl9win2 window server.

    Why there is no libdraw here: cc9 compiles to a System-V-ABI Plan 9 a.out,
    which cannot link kencc's libdraw. So we don't draw — gl9win2 (native
    kencc, owns the rio window, from alacritty9/win/) hosts us and we speak its
    protocol over inherited fds. See alacritty9/PROTOCOL.md.

        fd 0  events  win -> app   16-byte big-endian records
        fd 1  frames  app -> win   "GL9F" | u32be w | u32be h | w*h*4 RGBA
        fd 2  stderr passthrough

    Run as:  gl9win2 /bin/dosbox

    Frames: GL9F's RGBA byte order is exactly Plan 9 ABGR32 in memory, so a
    32bpp SDL surface with R=0x0000FF/G=0x00FF00/B=0xFF0000 masks ships with no
    repack. DOSBox may still ask for an 8bpp palettized mode, so we always
    stage through a 32bpp RGBA surface and let SDL's own blitters do the
    palette->RGB conversion (that correctness is why we vendored SDL at all).

    Events: Plan 9 has no usable non-blocking read on a pipe, so a reader
    thread blocks on read(0) and fills a ring that PumpEvents drains.
*/
#include "SDL_config.h"

#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"

#include "SDL_p9video.h"

#define P9VID_DRIVER_NAME "plan9"

/* GL9F ships RGBA bytes == little-endian 0xAABBGGRR. */
#define P9_RMASK 0x000000FF
#define P9_GMASK 0x0000FF00
#define P9_BMASK 0x00FF0000

static int P9_VideoInit(_THIS, SDL_PixelFormat *vformat);
static SDL_Rect **P9_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags);
static SDL_Surface *P9_SetVideoMode(_THIS, SDL_Surface *current, int width,
                                    int height, int bpp, Uint32 flags);
static int P9_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors);
static void P9_VideoQuit(_THIS);
static int P9_AllocHWSurface(_THIS, SDL_Surface *surface);
static int P9_LockHWSurface(_THIS, SDL_Surface *surface);
static void P9_UnlockHWSurface(_THIS, SDL_Surface *surface);
static void P9_FreeHWSurface(_THIS, SDL_Surface *surface);
static void P9_UpdateRects(_THIS, int numrects, SDL_Rect *rects);
static void P9_PumpEvents(_THIS);
static void P9_InitOSKeymap(_THIS);
static void P9_SetCaption(_THIS, const char *title, const char *icon);

/* ---- wire helpers ---- */

static void
put32(Uint8 *b, Uint32 v)
{
	b[0] = (Uint8)(v >> 24);
	b[1] = (Uint8)(v >> 16);
	b[2] = (Uint8)(v >> 8);
	b[3] = (Uint8)v;
}

static Uint32
get32(const Uint8 *b)
{
	return (Uint32)b[0] << 24 | (Uint32)b[1] << 16 |
	       (Uint32)b[2] << 8 | (Uint32)b[3];
}

/* A short write on a pipe is normal — loop. Returns 0 on success. */
static int
writen(int fd, const void *buf, size_t n)
{
	const char *p = (const char *)buf;
	while (n > 0) {
		long w = write(fd, p, n);
		if (w <= 0)
			return -1;
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static int
readn(int fd, void *buf, size_t n)
{
	char *p = (char *)buf;
	while (n > 0) {
		long r = read(fd, p, n);
		if (r <= 0)
			return -1;
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

/* ---- keyboard ---- */

/* US-layout shifted punctuation -> the unshifted key. DOSBox's mapper wants
 * the physical key (SDLK_1), not the produced character ('!'); it tracks shift
 * separately from the Kshift key events gl9win2 sends. */
static int
unshift_punct(int r)
{
	switch (r) {
	case '!': return '1';
	case '@': return '2';
	case '#': return '3';
	case '$': return '4';
	case '%': return '5';
	case '^': return '6';
	case '&': return '7';
	case '*': return '8';
	case '(': return '9';
	case ')': return '0';
	case '_': return '-';
	case '+': return '=';
	case '{': return '[';
	case '}': return ']';
	case '|': return '\\';
	case ':': return ';';
	case '"': return '\'';
	case '<': return ',';
	case '>': return '.';
	case '?': return '/';
	case '~': return '`';
	}
	return r;
}

/* Plan 9 rune -> SDLK_*. DOSBox maps SDLK_* to DOS scancodes itself
 * (usescancodes defaults false), so we never need real hardware scancodes. */
static SDLKey
rune_to_sdlk(Uint32 r)
{
	if (r >= P9_KF1 && r <= P9_KF12)
		return (SDLKey)(SDLK_F1 + (r - P9_KF1));

	switch (r) {
	case P9_KBS:     return SDLK_BACKSPACE;
	case P9_KTAB:    return SDLK_TAB;
	case P9_KNL:     return SDLK_RETURN;
	case P9_KESC:    return SDLK_ESCAPE;
	case P9_KDEL:    return SDLK_DELETE;
	case P9_KDOWN:   return SDLK_DOWN;
	case P9_KUP:     return SDLK_UP;
	case P9_KLEFT:   return SDLK_LEFT;
	case P9_KRIGHT:  return SDLK_RIGHT;
	case P9_KHOME:   return SDLK_HOME;
	case P9_KEND:    return SDLK_END;
	case P9_KPGUP:   return SDLK_PAGEUP;
	case P9_KPGDOWN: return SDLK_PAGEDOWN;
	case P9_KINS:    return SDLK_INSERT;
	case P9_KSHIFT:  return SDLK_LSHIFT;
	case P9_KCTL:    return SDLK_LCTRL;
	case P9_KALT:    return SDLK_LALT;
	}

	if (r < 0x80) {
		int c = (int)r;
		if (c >= 'A' && c <= 'Z')       /* SDLK_a..z are the lowercase runes */
			c = c - 'A' + 'a';
		else
			c = unshift_punct(c);
		if (c >= ' ' && c < 0x7F)
			return (SDLKey)c;
	}
	return SDLK_UNKNOWN;
}

/* ---- event reader thread ---- */

static int
P9_ReaderThread(void *data)
{
	SDL_VideoDevice *this = (SDL_VideoDevice *)data;
	P9Record rec;

	for (;;) {
		if (readn(0, rec.buf, P9_RECORD_SIZE) < 0)
			break;			/* gl9win2 closed: window gone */
		SDL_mutexP(this->hidden->lock);
		if (this->hidden->quit) {
			SDL_mutexV(this->hidden->lock);
			break;
		}
		{
			int next = (this->hidden->rhead + 1) % P9_RING;
			if (next != this->hidden->rtail) {	/* drop when full */
				this->hidden->ring[this->hidden->rhead] = rec;
				this->hidden->rhead = next;
			}
		}
		SDL_mutexV(this->hidden->lock);
	}

	/* Stream closed. Synthesize a quit so DOSBox exits rather than hangs. */
	SDL_mutexP(this->hidden->lock);
	{
		P9Record q;
		int next = (this->hidden->rhead + 1) % P9_RING;
		SDL_memset(q.buf, 0, sizeof q.buf);
		q.buf[0] = P9_EV_QUIT;
		if (next != this->hidden->rtail) {
			this->hidden->ring[this->hidden->rhead] = q;
			this->hidden->rhead = next;
		}
	}
	SDL_mutexV(this->hidden->lock);
	return 0;
}

static void
P9_HandleRecord(_THIS, P9Record *rec)
{
	Uint8 type = rec->buf[0];
	Uint8 state = rec->buf[1];
	Uint32 a = get32(rec->buf + 4);
	Uint32 b = get32(rec->buf + 8);
	SDL_keysym keysym;

	switch (type) {
	case P9_EV_KEY:
		SDL_memset(&keysym, 0, sizeof keysym);
		keysym.scancode = 0;	/* Plan 9 gives runes, not scancodes */
		keysym.sym = rune_to_sdlk(a);
		keysym.mod = KMOD_NONE;	/* SDL derives this from the modifier keys */
		keysym.unicode = (a < 0xF000) ? (Uint16)a : 0;
		if (keysym.sym != SDLK_UNKNOWN)
			SDL_PrivateKeyboard(state ? SDL_PRESSED : SDL_RELEASED, &keysym);
		break;

	case P9_EV_MOUSE_MOVE:
		this->hidden->mouse_x = (int)a;
		this->hidden->mouse_y = (int)b;
		SDL_PrivateMouseMotion(0, 0, (Sint16)a, (Sint16)b);
		break;

	case P9_EV_MOUSE_BUTTON:
		SDL_PrivateMouseButton(state ? SDL_PRESSED : SDL_RELEASED,
		                       (Uint8)a,
		                       (Sint16)this->hidden->mouse_x,
		                       (Sint16)this->hidden->mouse_y);
		break;

	case P9_EV_RESIZE:
		/* gl9win2 centers our frame in the rio window, so a window resize
		 * needs no action here — DOSBox picks its own emulated resolution. */
		this->hidden->win_w = (int)a;
		this->hidden->win_h = (int)b;
		break;

	case P9_EV_QUIT:
		SDL_PrivateQuit();
		break;

	case P9_EV_SCROLL:
	case P9_EV_FOCUS:
	default:
		break;
	}
}

static void
P9_PumpEvents(_THIS)
{
	for (;;) {
		P9Record rec;
		SDL_mutexP(this->hidden->lock);
		if (this->hidden->rtail == this->hidden->rhead) {
			SDL_mutexV(this->hidden->lock);
			break;
		}
		rec = this->hidden->ring[this->hidden->rtail];
		this->hidden->rtail = (this->hidden->rtail + 1) % P9_RING;
		SDL_mutexV(this->hidden->lock);
		P9_HandleRecord(this, &rec);
	}
}

static void
P9_InitOSKeymap(_THIS)
{
	/* rune_to_sdlk is a pure function — no table to build. */
}

/* ---- bootstrap ---- */

static int
P9_Available(void)
{
	return 1;
}

static void
P9_DeleteDevice(SDL_VideoDevice *device)
{
	SDL_free(device->hidden);
	SDL_free(device);
}

static SDL_VideoDevice *
P9_CreateDevice(int devindex)
{
	SDL_VideoDevice *device;

	device = (SDL_VideoDevice *)SDL_malloc(sizeof(SDL_VideoDevice));
	if (device) {
		SDL_memset(device, 0, (sizeof *device));
		device->hidden = (struct SDL_PrivateVideoData *)
		    SDL_malloc((sizeof *device->hidden));
	}
	if ((device == NULL) || (device->hidden == NULL)) {
		SDL_OutOfMemory();
		if (device)
			SDL_free(device);
		return 0;
	}
	SDL_memset(device->hidden, 0, (sizeof *device->hidden));

	device->VideoInit = P9_VideoInit;
	device->ListModes = P9_ListModes;
	device->SetVideoMode = P9_SetVideoMode;
	device->CreateYUVOverlay = NULL;
	device->SetColors = P9_SetColors;
	device->UpdateRects = P9_UpdateRects;
	device->VideoQuit = P9_VideoQuit;
	device->AllocHWSurface = P9_AllocHWSurface;
	device->CheckHWBlit = NULL;
	device->FillHWRect = NULL;
	device->SetHWColorKey = NULL;
	device->SetHWAlpha = NULL;
	device->LockHWSurface = P9_LockHWSurface;
	device->UnlockHWSurface = P9_UnlockHWSurface;
	device->FlipHWSurface = NULL;
	device->FreeHWSurface = P9_FreeHWSurface;
	device->SetCaption = P9_SetCaption;
	device->SetIcon = NULL;
	device->IconifyWindow = NULL;
	device->GrabInput = NULL;
	device->GetWMInfo = NULL;
	device->InitOSKeymap = P9_InitOSKeymap;
	device->PumpEvents = P9_PumpEvents;
	device->free = P9_DeleteDevice;

	return device;
}

VideoBootStrap P9_bootstrap = {
	P9VID_DRIVER_NAME, "Plan 9 gl9win2 video driver",
	P9_Available, P9_CreateDevice
};

/* ---- driver ---- */

static int
P9_VideoInit(_THIS, SDL_PixelFormat *vformat)
{
	this->hidden->lock = SDL_CreateMutex();
	if (this->hidden->lock == NULL) {
		SDL_SetError("p9: couldn't create event mutex");
		return -1;
	}
	this->hidden->rhead = this->hidden->rtail = 0;
	this->hidden->quit = 0;
	this->hidden->win_w = 640;
	this->hidden->win_h = 480;

	this->hidden->reader = SDL_CreateThread(P9_ReaderThread, this);
	if (this->hidden->reader == NULL) {
		SDL_SetError("p9: couldn't start event reader thread");
		return -1;
	}

	/* Report 32bpp RGBA as the desktop format so DOSBox picks the path that
	 * needs no conversion on our side. */
	vformat->BitsPerPixel = 32;
	vformat->BytesPerPixel = 4;
	vformat->Rmask = P9_RMASK;
	vformat->Gmask = P9_GMASK;
	vformat->Bmask = P9_BMASK;
	vformat->Amask = 0;
	return 0;
}

static SDL_Rect **
P9_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags)
{
	return (SDL_Rect **)-1;		/* any size is fine: gl9win2 centers us */
}

static SDL_Surface *
P9_SetVideoMode(_THIS, SDL_Surface *current, int width, int height, int bpp,
                Uint32 flags)
{
	if (this->hidden->buffer) {
		SDL_free(this->hidden->buffer);
		this->hidden->buffer = NULL;
	}
	if (this->hidden->rgba) {
		SDL_FreeSurface(this->hidden->rgba);
		this->hidden->rgba = NULL;
	}
	if (this->hidden->scratch) {
		SDL_free(this->hidden->scratch);
		this->hidden->scratch = NULL;
	}

	this->hidden->buffer = (Uint8 *)SDL_malloc((size_t)width * height * ((bpp + 7) / 8));
	if (!this->hidden->buffer) {
		SDL_SetError("p9: couldn't allocate buffer for requested mode");
		return NULL;
	}
	SDL_memset(this->hidden->buffer, 0, (size_t)width * height * ((bpp + 7) / 8));

	if (!SDL_ReallocFormat(current, bpp,
	                       bpp == 8 ? 0 : P9_RMASK,
	                       bpp == 8 ? 0 : P9_GMASK,
	                       bpp == 8 ? 0 : P9_BMASK, 0)) {
		SDL_free(this->hidden->buffer);
		this->hidden->buffer = NULL;
		SDL_SetError("p9: couldn't allocate new pixel format");
		return NULL;
	}

	current->flags = (flags & SDL_FULLSCREEN) | SDL_SWSURFACE;
	this->hidden->w = current->w = width;
	this->hidden->h = current->h = height;
	current->pitch = current->w * ((bpp + 7) / 8);
	current->pixels = this->hidden->buffer;

	/* Staging surface we actually ship. For a 32bpp mode this makes the blit
	 * a straight copy; for 8bpp it's where SDL applies the palette. */
	this->hidden->rgba = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 32,
	                                          P9_RMASK, P9_GMASK, P9_BMASK, 0);
	if (!this->hidden->rgba) {
		SDL_SetError("p9: couldn't allocate RGBA staging surface");
		return NULL;
	}

	/* Worst-case damage rect is the whole screen. */
	this->hidden->scratch = (Uint8 *)SDL_malloc((size_t)width * height * 4);
	if (!this->hidden->scratch) {
		SDL_SetError("p9: couldn't allocate damage scratch buffer");
		return NULL;
	}

	/* The frame size just changed, so gl9win2's Image is stale (or absent):
	 * the next update must be a full GL9F before any delta. */
	this->hidden->need_full = 1;

	return current;
}

static int
P9_AllocHWSurface(_THIS, SDL_Surface *surface)
{
	return -1;			/* software surfaces only */
}

static void
P9_FreeHWSurface(_THIS, SDL_Surface *surface)
{
}

static int
P9_LockHWSurface(_THIS, SDL_Surface *surface)
{
	return 0;
}

static void
P9_UnlockHWSurface(_THIS, SDL_Surface *surface)
{
}

/* Ship the whole staging surface as a GL9F full frame. gl9win2 needs one of
 * these before any GL9D delta (it allocates its Image from the frame size), so
 * this is also the "first update after a mode change" path. */
static void
P9_SendFull(_THIS)
{
	Uint8 hdr[12];
	SDL_memcpy(hdr, "GL9F", 4);
	put32(hdr + 4, (Uint32)this->hidden->w);
	put32(hdr + 8, (Uint32)this->hidden->h);
	if (writen(1, hdr, sizeof hdr) < 0)
		return;			/* window gone; reader thread will post QUIT */
	writen(1, this->hidden->rgba->pixels,
	       (size_t)this->hidden->w * this->hidden->h * 4);
}

/* Ship one dirty rect as a GL9D delta: "GL9D" | x | y | w | h | w*h*4 RGBA,
 * rows packed at the RECT's stride (not the surface's). DOSBox's typical
 * update is a 16x16 cursor blink — 1KB here versus a 1MB full frame, and
 * gl9win2 blits it as a small rect instead of reloading the whole image. */
static void
P9_SendDamage(_THIS, SDL_Rect *r)
{
	SDL_Surface *rgba = this->hidden->rgba;
	Uint8 hdr[20];
	int row;
	size_t rowbytes = (size_t)r->w * 4;
	Uint8 *scratch = this->hidden->scratch;

	SDL_memcpy(hdr, "GL9D", 4);
	put32(hdr + 4, (Uint32)r->x);
	put32(hdr + 8, (Uint32)r->y);
	put32(hdr + 12, (Uint32)r->w);
	put32(hdr + 16, (Uint32)r->h);
	if (writen(1, hdr, sizeof hdr) < 0)
		return;

	/* Pack the sub-rect into one buffer: a write() per row would be h
	 * syscalls and h pipe round-trips. */
	for (row = 0; row < r->h; row++)
		SDL_memcpy(scratch + row * rowbytes,
		           (Uint8 *)rgba->pixels + (size_t)(r->y + row) * rgba->pitch
		               + (size_t)r->x * 4,
		           rowbytes);
	writen(1, scratch, rowbytes * r->h);
}

static void
P9_UpdateRects(_THIS, int numrects, SDL_Rect *rects)
{
	SDL_Surface *screen = this->screen;
	SDL_Surface *rgba = this->hidden->rgba;

	if (!rgba || !screen || numrects == 0)
		return;

	/* P9_VIDEO_DEBUG=1 to dump geometry per update (stderr is the fd 2
	 * passthrough; stdout would corrupt the frame stream). */
	{
		static int dbg = -1;
		if (dbg < 0)
			dbg = SDL_getenv("P9_VIDEO_DEBUG") ? 1 : 0;
		if (dbg) {
			int i;
			fprintf(stderr, "p9: up n=%d screen=%dx%d bpp=%d pitch=%d "
			        "| rgba=%dx%d pitch=%d | send=%dx%d | shadow=%d |",
			        numrects, screen->w, screen->h,
			        screen->format->BitsPerPixel, screen->pitch,
			        rgba->w, rgba->h, rgba->pitch,
			        this->hidden->w, this->hidden->h,
			        (int)(screen != SDL_VideoSurface));
			for (i = 0; i < numrects && i < 20; i++)
				fprintf(stderr, " %d,%d,%dx%d", rects[i].x, rects[i].y,
				        rects[i].w, rects[i].h);
			fprintf(stderr, "\n");
		}
	}

	if (screen->format->BitsPerPixel == 32) {
		SDL_memcpy(rgba->pixels, screen->pixels,
		           (size_t)screen->pitch * screen->h);
	} else {
		/* SDL's blitters do palette->RGB for us. */
		SDL_Rect all;
		all.x = 0; all.y = 0;
		all.w = (Uint16)screen->w; all.h = (Uint16)screen->h;
		SDL_LowerBlit(screen, &all, rgba, &all);
	}

	/* Force the 4th byte opaque before shipping. gl9win2's Image is ABGR32 —
	 * a format WITH an alpha channel — so Plan 9's draw(2) computes
	 *
	 *     dst = src + dst*(1 - src.alpha)
	 *
	 * from that byte. Our masks declare no alpha, so SDL leaves it 0 and every
	 * draw became an ADDITIVE blend instead of a replace. GL9F hid this because
	 * gl9win2 blacks the window first (src + 0 == src); a GL9D draws straight
	 * onto live pixels, so old text stayed visible under new and colours crept
	 * brighter with each blend. alacritty9 never hit it: OSMesa already writes
	 * alpha 255. The protocol says RGBA, so sending 0 was our bug, not
	 * gl9win2's. One pass over a buffer we just memcpy'd anyway. */
	{
		Uint32 *p = (Uint32 *)rgba->pixels;
		size_t i, n = (size_t)rgba->w * rgba->h;
		for (i = 0; i < n; i++)
			p[i] |= 0xFF000000u;
	}

	/* P9_DUMP_FRAME=N: write frame N to /tmp/d9/frame.ppm. Proves whether a
	 * rendering artifact is in OUR frame or in the window server. */
	{
		static int want = -2, seen = 0;
		if (want == -2) {
			const char *e = SDL_getenv("P9_DUMP_FRAME");
			want = e ? SDL_atoi(e) : -1;
		}
		if (want >= 0 && ++seen == want) {
			FILE *f = fopen("/tmp/d9/frame.ppm", "wb");
			if (f) {
				int x, y, W = this->hidden->w, H = this->hidden->h;
				Uint8 *p = (Uint8 *)rgba->pixels;
				fprintf(f, "P6\n%d %d\n255\n", W, H);
				for (y = 0; y < H; y++)
					for (x = 0; x < W; x++) {
						Uint8 *q = p + y * rgba->pitch + x * 4;
						fputc(q[0], f); fputc(q[1], f); fputc(q[2], f);
					}
				fclose(f);
				fprintf(stderr, "p9: dumped frame %d (%dx%d)\n", seen, W, H);
			}
		}
	}

	/* GL9D deltas are the default: a blinking cursor is ~1KB here instead of a
	 * 1MB frame plus a whole-window reload.
	 *
	 * DOSBox's rects are trustworthy. They come from GFX_EndUpdate's
	 * changedLines run-lengths, and a from-scratch diff of consecutive frames
	 * (ignoring the rects entirely) was measured to pick exactly the same rows
	 * — e.g. both say "13 rows at y=354" on bmenace's blinking status box. The
	 * artifact that used to be blamed on this path was the alpha blend fixed
	 * above, not lost damage.
	 *
	 * P9_FULLFRAMES=1 forces full frames. Keep it: it is how the alpha bug was
	 * bisected (full frames were correct only because gl9win2 blacks the window
	 * first, which is exactly what made the delta path look guilty). */
	{
		int i, area = 0;
		static int fullframes = -1;

		if (fullframes < 0)
			fullframes = SDL_getenv("P9_FULLFRAMES") ? 1 : 0;

		for (i = 0; i < numrects; i++)
			area += rects[i].w * rects[i].h;

		if (fullframes || this->hidden->need_full || numrects > 16 ||
		    area * 3 >= this->hidden->w * this->hidden->h) {
			P9_SendFull(this);
			this->hidden->need_full = 0;
		} else {
			for (i = 0; i < numrects; i++) {
				SDL_Rect r = rects[i];
				/* Clip: DOSBox can hand us a rect past the surface edge,
				 * and gl9win2 silently DROPS an out-of-range GL9D (it
				 * assumes a stale pre-resize rect) — that would leave the
				 * change permanently unpainted. */
				if (r.x < 0) { r.w += r.x; r.x = 0; }
				if (r.y < 0) { r.h += r.y; r.y = 0; }
				if (r.x + r.w > this->hidden->w)
					r.w = this->hidden->w - r.x;
				if (r.y + r.h > this->hidden->h)
					r.h = this->hidden->h - r.y;
				if (r.w <= 0 || r.h <= 0)
					continue;
				P9_SendDamage(this, &r);
			}
		}
	}
}

static int
P9_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors)
{
	/* SDL keeps the palette on screen->format->palette and our blit in
	 * UpdateRects reads it, so there is nothing device-side to program. */
	return 1;
}

static void
P9_SetCaption(_THIS, const char *title, const char *icon)
{
	/* GL9T: "GL9T" | u32be len | len bytes of UTF-8 title. */
	Uint8 hdr[8];
	size_t n;

	if (!title)
		return;
	n = SDL_strlen(title);
	SDL_memcpy(hdr, "GL9T", 4);
	put32(hdr + 4, (Uint32)n);
	if (writen(1, hdr, sizeof hdr) < 0)
		return;
	writen(1, title, n);
}

static void
P9_VideoQuit(_THIS)
{
	if (this->hidden->lock) {
		SDL_mutexP(this->hidden->lock);
		this->hidden->quit = 1;
		SDL_mutexV(this->hidden->lock);
	}
	/* The reader thread is blocked in read(0) and only unblocks when gl9win2
	 * closes the pipe. Don't join it — we'd hang. It dies with the process.
	 * ponytail: leaked thread at exit. Fine: exit is next. */

	if (this->hidden->rgba) {
		SDL_FreeSurface(this->hidden->rgba);
		this->hidden->rgba = NULL;
	}
	if (this->screen && this->screen->pixels) {
		SDL_free(this->screen->pixels);
		this->screen->pixels = NULL;
		this->hidden->buffer = NULL;
	}
}
