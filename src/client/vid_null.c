/* Bulwark engine - no window: for builds without SDL2. ISC licence, see LICENSE. */
#include "vid.h"

bool VID_Init(int width, int height, bool fullscreen) { return false; }
void VID_Shutdown(void) {}
bool VID_Active(void) { return false; }
bool VID_PumpEvents(void) { return true; }
void VID_GetSize(int *w, int *h) { *w = 640, *h = 480; }
void VID_Swap(void) {}
void *VID_GetProcAddress(const char *name) { return NULL; }
void VID_SetMouseGrab(bool grab) {}
void VID_SetCaption(const char *text) {}
void VID_GetMousePos(float *x, float *y) { *x = *y = 0; }
void VID_SetMousePos(float x, float y) {}
void VID_Rumble(int dev, float low, float high, float seconds) {}
void VID_RumbleTriggers(int dev, float left, float right, float seconds) {}
void VID_SetControllerLED(int dev, const float *rgb) {}

void VID_ApplyMode(void) {}
