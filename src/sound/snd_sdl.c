/* Bulwark engine - the SDL2 sound device. ISC licence, see LICENSE. */
#include "snd_local.h"

#include <SDL.h>
#include <stdio.h>

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

/* --- microphone ---------------------------------------------------------- */

#define MIC_FRAMES 48000 /* a second of mono samples */
static SDL_AudioDeviceID mic;
static int16_t micbuf[MIC_FRAMES];
static unsigned mic_head, mic_tail;

static void mic_callback(void *userdata, Uint8 *stream, int len)
{
	(void)userdata;
	const int16_t *in = (const int16_t *)stream;
	for (int i = 0; i < len / 2; i++) {
		if (mic_head - mic_tail >= MIC_FRAMES)
			mic_tail++; /* nobody read for a second: drop the oldest */
		micbuf[mic_head++ % MIC_FRAMES] = in[i];
	}
}

int S_MicDevices(char names[][128], int max)
{
	int n = 0;
	if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		return 0;
	for (int i = 0; i < SDL_GetNumAudioDevices(1) && n < max; i++) {
		const char *name = SDL_GetAudioDeviceName(i, 1);
		if (name)
			snprintf(names[n++], 128, "%s", name);
	}
	return n;
}

bool S_MicOpen(const char *device, int rate)
{
	SDL_AudioSpec want, have;
	S_MicClose();
	if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		return false;
	SDL_zero(want);
	want.freq = rate;
	want.format = AUDIO_S16SYS;
	want.channels = 1;
	want.samples = 480;
	want.callback = mic_callback;
	mic = SDL_OpenAudioDevice(device && device[0] ? device : NULL, 1, &want, &have, 0);
	if (!mic) {
		Con_Printf("^&C0microphone: %s\n", SDL_GetError());
		return false;
	}
	mic_head = mic_tail = 0;
	SDL_PauseAudioDevice(mic, 0);
	return true;
}

int S_MicRead(int16_t *out, int max)
{
	int n = 0;
	if (!mic)
		return 0;
	SDL_LockAudioDevice(mic);
	while (n < max && mic_tail != mic_head)
		out[n++] = micbuf[mic_tail++ % MIC_FRAMES];
	SDL_UnlockAudioDevice(mic);
	return n;
}

void S_MicClose(void)
{
	if (mic)
		SDL_CloseAudioDevice(mic);
	mic = 0;
}
