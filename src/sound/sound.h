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

#endif
