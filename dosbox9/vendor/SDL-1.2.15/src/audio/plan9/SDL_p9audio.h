/*
    SDL 1.2 Plan 9 (9front) audio backend — private data.
*/
#ifndef _SDL_p9audio_h
#define _SDL_p9audio_h

#include "SDL_config.h"
#include "../SDL_sysaudio.h"

/* Hidden "this" pointer for the audio device */
#define _THIS SDL_AudioDevice *this

struct SDL_PrivateAudioData {
	int audio_fd;        /* /dev/audio, or -1 when there's no audio device */
	Uint8 *mixbuf;
	Uint32 mixlen;
	Uint32 silence_ms;   /* pacing when audio_fd < 0 */
};

#endif /* _SDL_p9audio_h */
