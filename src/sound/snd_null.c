/* Bulwark engine - no sound device. ISC licence, see LICENSE. */
#include "snd_local.h"

bool SNDDMA_Init(int *rate)
{
	(void)rate;
	return false;
}

void SNDDMA_Shutdown(void) {}
void SNDDMA_Lock(void) {}
void SNDDMA_Unlock(void) {}

int S_MicDevices(char names[][128], int max)
{
	(void)names;
	(void)max;
	return 0;
}

bool S_MicOpen(const char *device, int rate)
{
	(void)device;
	(void)rate;
	return false;
}

int S_MicRead(int16_t *out, int max)
{
	(void)out;
	(void)max;
	return 0;
}

void S_MicClose(void) {}
