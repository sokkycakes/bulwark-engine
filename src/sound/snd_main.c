/* Bulwark engine - sound. ISC licence, see LICENSE. */
#include "sound.h"

void S_Init(bool headless) {}
void S_Shutdown(void) {}
bool S_PrecacheSound(const char *name) { return true; }
void S_StartSound(int ent, int channel, const char *name, const float *origin, float volume, float attenuation,
                  float pitch, int flags) {}
void S_StopAllSounds(void) {}
void S_Update(const float *origin, const float *forward, const float *right, const float *up) {}

const qc_builtin_t s_builtins[] = {{NULL, NULL}};
