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
void S_ForEachSfx(void (*fn)(const sfx_t *sfx, void *ctx), void *ctx);

/* --- distance (docs/spec/sound.md) ---------------------------------------- */

/* the dB sound level a QC attenuation names; 0 for one heard everywhere */
float S_SoundLevel(float attenuation);
/* 0..1 gain at a distance */
float S_DistanceGain(float attenuation, float dist);

/* --- music --------------------------------------------------------------- */

bool S_OpenMusic(const char *path, bool loop);
void S_CloseMusic(void);
/* adds the music into a stereo float buffer at the device rate */
void S_MixMusic(float *out, int frames, int rate, float volume);
bool S_MusicActive(void);
double S_MusicTime(void);
void S_SeekMusic(double seconds);

/* --- room effects (snd_dsp.c) ---------------------------------------------- */

void S_InitDSPCvars(void);
void S_InitDSP(int rate);
void S_ShutdownDSP(void);
/* the listener moved: picks the room effect for there (main thread) */
void S_UpdateDSP(const float *origin);
void S_SetReverbSlot(int slot);
float S_DSPVolume(void);
/* runs the room effect over mixed stereo frames (mixer thread) */
void S_ApplyDSP(float *buf, int frames, float volume);
void S_BI_SetupReverb(qcvm_t *vm);

/* --- device -------------------------------------------------------------- */

/* opens the device; it then calls S_PaintAudio from its own thread */
bool SNDDMA_Init(int *rate);
void SNDDMA_Shutdown(void);
void SNDDMA_Lock(void);
void SNDDMA_Unlock(void);

/* fills interleaved stereo frames; called by the device with the lock held */
void S_PaintAudio(int16_t *out, int frames);

#endif
