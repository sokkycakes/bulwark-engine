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
