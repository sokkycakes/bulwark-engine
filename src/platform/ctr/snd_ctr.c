/* Bulwark engine - 3DS sound through the DSP (NDSP). ISC licence, see LICENSE.
 *
 * One stereo channel cycles through a few wave buffers; NDSP's frame
 * callback (its own thread) refills the ones it has finished with the
 * engine's mix. The mixer runs at 22050 Hz to spare the CPU. NDSP needs
 * the DSP firmware dump (sdmc:/3ds/dspfirm.cdc) on real hardware; without
 * it the game runs silent. No microphone. */
#include <3ds.h>
#include <string.h>

#include "../../sound/snd_local.h"

#define RATE 22050
#define NUMBUFS 4
#define BUFFRAMES 1024 /* about 46ms each */

static ndspWaveBuf wbufs[NUMBUFS];
static int16_t *samples;
static LightLock lock;
static bool running;

static void fill(ndspWaveBuf *wb)
{
	LightLock_Lock(&lock);
	S_PaintAudio(wb->data_pcm16, BUFFRAMES);
	LightLock_Unlock(&lock);
	DSP_FlushDataCache(wb->data_pcm16, BUFFRAMES * 4);
	ndspChnWaveBufAdd(0, wb);
}

static void frame_callback(void *data)
{
	(void)data;
	if (!running)
		return;
	for (int i = 0; i < NUMBUFS; i++)
		if (wbufs[i].status == NDSP_WBUF_DONE)
			fill(&wbufs[i]);
}

bool SNDDMA_Init(int *rate)
{
	float mix[12] = {1.0f, 1.0f};
	if (R_FAILED(ndspInit())) {
		Con_Printf("sound: the DSP didn't start (on hardware it needs sdmc:/3ds/dspfirm.cdc)\n");
		return false;
	}
	samples = linearAlloc(sizeof(int16_t) * 2 * BUFFRAMES * NUMBUFS);
	if (!samples) {
		ndspExit();
		return false;
	}
	memset(samples, 0, sizeof(int16_t) * 2 * BUFFRAMES * NUMBUFS);
	LightLock_Init(&lock);
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnReset(0);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetRate(0, RATE);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	ndspChnSetMix(0, mix);
	memset(wbufs, 0, sizeof(wbufs));
	for (int i = 0; i < NUMBUFS; i++) {
		wbufs[i].data_pcm16 = samples + i * BUFFRAMES * 2;
		wbufs[i].nsamples = BUFFRAMES;
	}
	*rate = RATE;
	running = true;
	/* start with silence queued; the callback takes over from there */
	for (int i = 0; i < NUMBUFS; i++) {
		DSP_FlushDataCache(wbufs[i].data_pcm16, BUFFRAMES * 4);
		ndspChnWaveBufAdd(0, &wbufs[i]);
	}
	ndspSetCallback(frame_callback, NULL);
	Con_Printf("sound: NDSP at %d Hz\n", RATE);
	return true;
}

void SNDDMA_Shutdown(void)
{
	if (!running)
		return;
	running = false;
	ndspSetCallback(NULL, NULL);
	ndspChnReset(0);
	ndspExit();
	linearFree(samples);
	samples = NULL;
}

void SNDDMA_Lock(void)
{
	if (running)
		LightLock_Lock(&lock);
}

void SNDDMA_Unlock(void)
{
	if (running)
		LightLock_Unlock(&lock);
}

int S_MicDevices(char names[][128], int max)
{
	(void)names, (void)max;
	return 0;
}

bool S_MicOpen(const char *device, int rate)
{
	(void)device, (void)rate;
	return false;
}

int S_MicRead(int16_t *out, int max)
{
	(void)out, (void)max;
	return 0;
}

void S_MicClose(void) {}
