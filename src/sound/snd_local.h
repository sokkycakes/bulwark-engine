/* Bulwark engine - sound internals. ISC licence, see LICENSE.
 *
 * Laid out like Source's split, written fresh: a sample cache, a software
 * mixer with entity channels and spatialisation, a streamed music channel,
 * and a small device layer (SDL, or none) that pulls mixed frames. */
#ifndef BW_SND_LOCAL_H
#define BW_SND_LOCAL_H

#include "sound.h"

/* --- samples ------------------------------------------------------------- */

typedef struct sfx_s {
	char name[128];
	int16_t *data;     /* interleaved frames, NULL until loaded */
	int frames;
	int channels;      /* 1 or 2 */
	int rate;
	int loopstart;     /* frame to loop back to, -1 for none (a WAV cue point) */
	bool loaded, failed;
	struct sfx_s *next;
} sfx_t;

sfx_t *S_FindSfx(const char *name, bool load);
bool S_LoadSfx(sfx_t *sfx);
void S_FreeAllSfx(void);

/* --- music --------------------------------------------------------------- */

bool S_OpenMusic(const char *path, bool loop);
void S_CloseMusic(void);
/* adds the music into a stereo float buffer at the device rate */
void S_MixMusic(float *out, int frames, int rate, float volume);

/* --- device -------------------------------------------------------------- */

/* opens the device; it then calls S_PaintAudio from its own thread */
bool SNDDMA_Init(int *rate);
void SNDDMA_Shutdown(void);
void SNDDMA_Lock(void);
void SNDDMA_Unlock(void);

/* fills interleaved stereo frames; called by the device with the lock held */
void S_PaintAudio(int16_t *out, int frames);

#endif
