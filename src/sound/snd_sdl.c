/* Bulwark engine - the SDL2 sound device. ISC licence, see LICENSE. */
#include "snd_local.h"

#include <SDL.h>

static SDL_AudioDeviceID dev;

static void callback(void *userdata, Uint8 *stream, int len)
{
	(void)userdata;
	/* SDL holds the device lock while it calls us */
	S_PaintAudio((int16_t *)stream, len / 4);
}

bool SNDDMA_Init(int *rate)
{
	SDL_AudioSpec want, have;
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		Con_Printf("SDL audio: %s\n", SDL_GetError());
		return false;
	}
	SDL_zero(want);
	want.freq = 44100;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024; /* about 23ms at 44.1kHz */
	want.callback = callback;
	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (!dev) {
		Con_Printf("SDL audio: %s\n", SDL_GetError());
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return false;
	}
	*rate = have.freq;
	Con_Printf("sound: %s at %d Hz\n", SDL_GetCurrentAudioDriver(), have.freq);
	SDL_PauseAudioDevice(dev, 0);
	return true;
}

void SNDDMA_Shutdown(void)
{
	if (dev)
		SDL_CloseAudioDevice(dev);
	dev = 0;
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void SNDDMA_Lock(void)
{
	if (dev)
		SDL_LockAudioDevice(dev);
}

void SNDDMA_Unlock(void)
{
	if (dev)
		SDL_UnlockAudioDevice(dev);
}
