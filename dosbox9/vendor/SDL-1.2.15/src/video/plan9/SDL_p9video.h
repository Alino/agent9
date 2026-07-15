/*
    SDL 1.2 Plan 9 (9front) video backend — private data.
    See SDL_p9video.c for the design.
*/
#ifndef _SDL_p9video_h
#define _SDL_p9video_h

#include "SDL_config.h"
#include "SDL_mutex.h"
#include "SDL_thread.h"
#include "../SDL_sysvideo.h"

/* Hidden "this" pointer for the video functions. SDL_sysvideo.h spells it
 * `*_this` (so C++ TUs can include it); every backend's private header
 * re-defines it to plain `this`, and the backends are C-only. Match them. */
#undef _THIS
#define _THIS SDL_VideoDevice *this

/* gl9win2 event record: 16 bytes big-endian. alacritty9/PROTOCOL.md */
#define P9_RECORD_SIZE 16

#define P9_EV_KEY          1
#define P9_EV_MOUSE_MOVE   2
#define P9_EV_MOUSE_BUTTON 3
#define P9_EV_SCROLL       4
#define P9_EV_RESIZE       5
#define P9_EV_FOCUS        6
#define P9_EV_QUIT         7

#define P9_MOD_SHIFT (1<<0)
#define P9_MOD_CTRL  (1<<1)
#define P9_MOD_ALT   (1<<2)

/* Plan 9 key runes (/sys/include/keyboard.h), as validated by
 * alacritty9/vendor/winit/src/platform_impl/plan9/protocol.rs. */
#define P9_KBS     0x08
#define P9_KTAB    0x09
#define P9_KNL     0x0A
#define P9_KESC    0x1B
#define P9_KDEL    0x7F
#define P9_KDOWN   0x80   /* yes, really 0x80 on Plan 9 */
#define P9_KF1     0xF001
#define P9_KF12    0xF00C
#define P9_KHOME   0xF00D
#define P9_KUP     0xF00E
#define P9_KPGUP   0xF00F
#define P9_KLEFT   0xF011
#define P9_KRIGHT  0xF012
#define P9_KPGDOWN 0xF013
#define P9_KINS    0xF014
#define P9_KALT    0xF015
#define P9_KSHIFT  0xF016
#define P9_KCTL    0xF017
#define P9_KEND    0xF018

/* Event ring. The reader thread blocks on read(0); PumpEvents drains. Plan 9
 * has no usable non-blocking read on a pipe, so a blocking reader thread is
 * the idiom (cc9 has real pthreads). */
#define P9_RING 256

typedef struct {
	Uint8 buf[P9_RECORD_SIZE];
} P9Record;

struct SDL_PrivateVideoData {
	Uint8 *buffer;       /* the screen surface's pixels (any bpp) */
	SDL_Surface *rgba;   /* 32bpp RGBA staging; what we ship as GL9F/GL9D */
	Uint8 *scratch;      /* packs a damage rect's rows for one write() */
	int need_full;       /* gl9win2 needs a GL9F before any GL9D delta */
	int w, h;

	SDL_mutex *lock;
	SDL_Thread *reader;
	int quit;
	P9Record ring[P9_RING];
	int rhead, rtail;

	int mouse_x, mouse_y;
	Uint8 buttons;       /* SDL button state bitmask */
	int win_w, win_h;    /* last size gl9win2 told us */
};

#endif /* _SDL_p9video_h */
