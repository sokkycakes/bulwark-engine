/* Bulwark engine - sound interface. ISC licence, see LICENSE. */
#ifndef BW_SOUND_H
#define BW_SOUND_H

#include "../common/common.h"
#include "../qcvm/qcvm.h"

extern const qc_builtin_t s_builtins[];

void S_Init(bool headless);
void S_Shutdown(void);
bool S_PrecacheSound(const char *name);
void S_StartSound(int ent, int channel, const char *name, const float *origin, float volume, float attenuation,
                  float pitch, int flags);
void S_StopAllSounds(void);
void S_Update(const float *origin, const float *forward, const float *right, const float *up);

/* a stream of samples from elsewhere (video soundtracks): interleaved
 * stereo at its own rate, mixed in as it is queued */
void S_RawSamples(const int16_t *frames, int count, int rate, float volume);
void S_RawClear(void);
int S_RawQueued(void); /* frames not yet played, at the device rate */

/* video capture: while on, the device plays silence and the capture mixes
 * exactly the frames it needs with S_CaptureMix */
void S_SetCapture(bool on);
void S_CaptureMix(int16_t *out, int frames);
int S_Rate(void);

#endif
