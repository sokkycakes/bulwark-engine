/* Bulwark engine - window, GL context and input events. ISC licence, see LICENSE. */
#ifndef BW_VID_H
#define BW_VID_H

#include "../common/common.h"

bool VID_Init(int width, int height, bool fullscreen);
void VID_Shutdown(void);
bool VID_Active(void);
bool VID_PumpEvents(void);     /* false when the window was closed */
void VID_GetSize(int *w, int *h);
void VID_Swap(void);
void *VID_GetProcAddress(const char *name);
void VID_SetMouseGrab(bool grab); /* relative mouse for looking around */
void VID_SetCaption(const char *text);
void VID_GetMousePos(float *x, float *y);
void VID_SetMousePos(float x, float y);

/* game controllers, numbered from 0 in the order they were connected */
void VID_Rumble(int dev, float low, float high, float seconds);
void VID_RumbleTriggers(int dev, float left, float right, float seconds);
void VID_SetControllerLED(int dev, const float *rgb);

#endif
