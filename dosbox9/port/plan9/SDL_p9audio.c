/*
    SDL 1.2 audio backend for Plan 9 / 9front.

    /dev/audio takes raw 16-bit signed little-endian stereo at 44100Hz and the
    write blocks until there's buffer space — which is exactly the pacing SDL's
    audio thread wants, so this is the classic /dev/dsp-shaped driver: SDL
    calls WaitAudio, GetAudioBuf, (app fills), PlayAudio, and our blocking
    write does the rest.

    We force the hardware format and let SDL's SDL_audiocvt convert whatever
    DOSBox asks for. If /dev/audio can't be opened (no audio device — QEMU
    often has none) we degrade to paced silence rather than failing: DOSBox
    still runs, just mute. A missing sound card shouldn't stop a game booting.
*/
#include "SDL_config.h"

#include <fcntl.h>
#include <unistd.h>

#include "SDL_audio.h"
#include "SDL_timer.h"
#include "../SDL_audiomem.h"
#include "../SDL_audio_c.h"
#include "SDL_p9audio.h"

#define P9AUD_DRIVER_NAME "plan9"

/* /dev/audio is fixed-format on Plan 9. */
#define P9_FREQ     44100
#define P9_CHANNELS 2

static int P9AUD_OpenAudio(_THIS, SDL_AudioSpec *spec);
static void P9AUD_WaitAudio(_THIS);
static void P9AUD_PlayAudio(_THIS);
static Uint8 *P9AUD_GetAudioBuf(_THIS);
static void P9AUD_CloseAudio(_THIS);

static int
P9AUD_Available(void)
{
	return 1;
}

static void
P9AUD_DeleteDevice(SDL_AudioDevice *device)
{
	SDL_free(device->hidden);
	SDL_free(device);
}

static SDL_AudioDevice *
P9AUD_CreateDevice(int devindex)
{
	SDL_AudioDevice *this;

	this = (SDL_AudioDevice *)SDL_malloc(sizeof(SDL_AudioDevice));
	if (this) {
		SDL_memset(this, 0, (sizeof *this));
		this->hidden = (struct SDL_PrivateAudioData *)
		    SDL_malloc((sizeof *this->hidden));
	}
	if ((this == NULL) || (this->hidden == NULL)) {
		SDL_OutOfMemory();
		if (this)
			SDL_free(this);
		return 0;
	}
	SDL_memset(this->hidden, 0, (sizeof *this->hidden));
	this->hidden->audio_fd = -1;

	this->OpenAudio = P9AUD_OpenAudio;
	this->WaitAudio = P9AUD_WaitAudio;
	this->PlayAudio = P9AUD_PlayAudio;
	this->GetAudioBuf = P9AUD_GetAudioBuf;
	this->CloseAudio = P9AUD_CloseAudio;
	this->free = P9AUD_DeleteDevice;

	return this;
}

AudioBootStrap P9AUD_bootstrap = {
	P9AUD_DRIVER_NAME, "Plan 9 /dev/audio",
	P9AUD_Available, P9AUD_CreateDevice
};

static int
P9AUD_OpenAudio(_THIS, SDL_AudioSpec *spec)
{
	/* Force the only format /dev/audio speaks; SDL converts to it. */
	spec->freq = P9_FREQ;
	spec->format = AUDIO_S16LSB;
	spec->channels = P9_CHANNELS;
	SDL_CalculateAudioSpec(spec);

	this->hidden->mixlen = spec->size;
	this->hidden->mixbuf = (Uint8 *)SDL_AllocAudioMem(this->hidden->mixlen);
	if (this->hidden->mixbuf == NULL)
		return -1;
	SDL_memset(this->hidden->mixbuf, spec->silence, spec->size);

	/* ms of audio one buffer represents — the pacing when we have no device. */
	this->hidden->silence_ms = (spec->samples * 1000) / spec->freq;
	if (this->hidden->silence_ms == 0)
		this->hidden->silence_ms = 1;

	this->hidden->audio_fd = open("/dev/audio", O_WRONLY);
	/* audio_fd < 0 is not an error: run mute. */

	return 0;
}

static void
P9AUD_WaitAudio(_THIS)
{
	/* The blocking write in PlayAudio is the wait. */
}

static void
P9AUD_PlayAudio(_THIS)
{
	Uint8 *p = this->hidden->mixbuf;
	int n = (int)this->hidden->mixlen;

	if (this->hidden->audio_fd < 0) {
		/* No device: burn the same wall-clock the write would have, so the
		 * emulator's timing doesn't run away. */
		SDL_Delay(this->hidden->silence_ms);
		return;
	}

	while (n > 0) {
		long w = write(this->hidden->audio_fd, p, n);
		if (w <= 0) {
			/* Device went away — fall back to pacing rather than spinning. */
			close(this->hidden->audio_fd);
			this->hidden->audio_fd = -1;
			SDL_Delay(this->hidden->silence_ms);
			return;
		}
		p += w;
		n -= (int)w;
	}
}

static Uint8 *
P9AUD_GetAudioBuf(_THIS)
{
	return this->hidden->mixbuf;
}

static void
P9AUD_CloseAudio(_THIS)
{
	if (this->hidden->mixbuf != NULL) {
		SDL_FreeAudioMem(this->hidden->mixbuf);
		this->hidden->mixbuf = NULL;
	}
	if (this->hidden->audio_fd >= 0) {
		close(this->hidden->audio_fd);
		this->hidden->audio_fd = -1;
	}
}
