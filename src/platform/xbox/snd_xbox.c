/* Bulwark engine - Xbox sound through the AC'97 codec (nxdk's XAudio).
 * ISC licence, see LICENSE.
 *
 * The codec plays 16-bit stereo at 48 kHz from buffers the CPU hands it.
 * Its completion callback runs at DPC level, where mixing (floating point,
 * locks, long work) is not allowed, so it only queues the next buffer that
 * is ready and wakes a mixing thread, which refills the one just played.
 *
 * The engine mixes at 24 kHz (snd_xbox_rate 48000 mixes at the codec's
 * rate) and the thread doubles it to 48 kHz by linear interpolation: half
 * the mixing work, and the game's samples are mostly 22 or 11 kHz anyway. */
#include <hal/audio.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "../../common/cvar.h"
#include "../../sound/snd_local.h"

#define OUT_RATE 48000
#define NUMBUFS 4
#define OUTFRAMES 1024 /* 21 ms at 48 kHz each; four of them is 85 ms of latency */

static int16_t *bufs[NUMBUFS];         /* contiguous memory the codec reads */
static int16_t *silence;
static int16_t mixbuf[OUTFRAMES * 2]; /* the engine's mix, before resampling */
static int mixrate;
static volatile LONG played;  /* buffers the codec has finished */
static volatile LONG queued;  /* buffers handed to the codec */
static volatile LONG filled;  /* buffers the mixer has filled */
static KEVENT wake;
static CRITICAL_SECTION lock;
static HANDLE thread;
static volatile bool running;
static int16_t last[2]; /* the previous mixed frame, for interpolation across buffers */

/* DPC level: no floating point, no waits */
static void codec_done(void *dev, void *data)
{
	(void)dev, (void)data;
	played++;
	if (queued < filled) {
		int16_t *b = bufs[queued % NUMBUFS];
		XAudioProvideSamples((unsigned char *)b, OUTFRAMES * 4, 0);
		queued++;
	} else {
		/* the mixer fell behind: play silence rather than stop */
		XAudioProvideSamples((unsigned char *)silence, OUTFRAMES * 4, 0);
	}
	KeSetEvent(&wake, 0, FALSE);
}

static void mix_into(int16_t *out)
{
	EnterCriticalSection(&lock);
	if (mixrate == OUT_RATE) {
		S_PaintAudio(out, OUTFRAMES);
		LeaveCriticalSection(&lock);
		return;
	}
	S_PaintAudio(mixbuf, OUTFRAMES / 2);
	LeaveCriticalSection(&lock);
	/* 2x: each mixed frame, then the midpoint to the next */
	for (int i = 0; i < OUTFRAMES / 2; i++) {
		int16_t l = mixbuf[i * 2], r = mixbuf[i * 2 + 1];
		out[i * 4 + 0] = (int16_t)((last[0] + l) >> 1);
		out[i * 4 + 1] = (int16_t)((last[1] + r) >> 1);
		out[i * 4 + 2] = l;
		out[i * 4 + 3] = r;
		last[0] = l, last[1] = r;
	}
}

static DWORD WINAPI mixer_main(LPVOID p)
{
	(void)p;
	while (running) {
		/* keep every buffer the codec isn't playing filled */
		while (running && filled - played < NUMBUFS) {
			int16_t *b = bufs[filled % NUMBUFS];
			mix_into(b);
			filled++;
		}
		LARGE_INTEGER timeout;
		timeout.QuadPart = -100000; /* 10 ms */
		KeWaitForSingleObject(&wake, Executive, KernelMode, FALSE, &timeout);
	}
	return 0;
}

bool SNDDMA_Init(int *rate)
{
	cvar_t *r = Cvar_Get("snd_xbox_rate", "24000", CVAR_ARCHIVE, "Rate the sound mixer runs at: 24000 or 48000 (Xbox)");
	mixrate = r->ivalue == OUT_RATE ? OUT_RATE : OUT_RATE / 2;
	for (int i = 0; i < NUMBUFS; i++) {
		bufs[i] = MmAllocateContiguousMemoryEx(OUTFRAMES * 4, 0, 0x03FFAFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
		if (!bufs[i]) {
			Con_Printf("sound: no memory for the codec's buffers\n");
			return false;
		}
		memset(bufs[i], 0, OUTFRAMES * 4);
	}
	silence = MmAllocateContiguousMemoryEx(OUTFRAMES * 4, 0, 0x03FFAFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
	if (!silence)
		return false;
	memset(silence, 0, OUTFRAMES * 4);
	InitializeCriticalSection(&lock);
	KeInitializeEvent(&wake, SynchronizationEvent, FALSE);
	played = queued = filled = 0;
	running = true;
	/* two buffers of silence start the codec; the mixer fills the rest */
	XAudioInit(16, 2, codec_done, NULL);
	filled = 2;
	for (int i = 0; i < 2; i++) {
		XAudioProvideSamples((unsigned char *)bufs[i], OUTFRAMES * 4, 0);
		queued++;
	}
	thread = CreateThread(NULL, 64 * 1024, mixer_main, NULL, 0, NULL);
	if (!thread) {
		running = false;
		return false;
	}
	/* above the game's thread, so a long frame doesn't starve the codec */
	SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
	XAudioPlay();
	*rate = mixrate;
	Con_Printf("sound: AC'97 at %d Hz, mixing at %d Hz\n", OUT_RATE, mixrate);
	return true;
}

void SNDDMA_Shutdown(void)
{
	if (!running)
		return;
	running = false;
	KeSetEvent(&wake, 0, FALSE);
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
	XAudioPause();
}

void SNDDMA_Lock(void)
{
	if (running)
		EnterCriticalSection(&lock);
}

void SNDDMA_Unlock(void)
{
	if (running)
		LeaveCriticalSection(&lock);
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
