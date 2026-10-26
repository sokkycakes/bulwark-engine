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
/* the same for other streams: 0 is the raw stream, 1 on are voices */
#define S_MAX_STREAMS 18
void S_StreamSamples(int stream, const int16_t *frames, int count, int rate, float volume);
void S_StreamClear(int stream);
int S_StreamQueued(int stream);
/* scales game sounds and music (not the streams), for voice ducking */
void S_SetDucking(float scale);

/* the media player's playlist: track i (NULL past the end), the one playing (-1 none) */
const char *S_MediaTrack(int i);
int S_MediaCurrent(void);

/* the microphone (the device layer): mono 16-bit samples at the asked rate */
int S_MicDevices(char names[][128], int max);
bool S_MicOpen(const char *device, int rate); /* NULL or "" for the default */
int S_MicRead(int16_t *out, int max);         /* samples read */
void S_MicClose(void);

/* video capture: while on, the device plays silence and the capture mixes
 * exactly the frames it needs with S_CaptureMix */
void S_SetCapture(bool on);
void S_CaptureMix(int16_t *out, int frames);
int S_Rate(void);

#endif
