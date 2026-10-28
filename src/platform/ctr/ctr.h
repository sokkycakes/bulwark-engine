/* Bulwark engine - pieces of the 3DS platform layer that talk to each
 * other. ISC licence, see LICENSE. */
#ifndef BW_CTR_H
#define BW_CTR_H

#include <stdbool.h>

/* the top screen, as the renderer sees it: landscape, origin bottom left */
#define CTR_WIDTH 400
#define CTR_HEIGHT 240

/* gl_ctr.c: the renderer's GL subset on citro3d */
bool CTRGL_Init(void);
void CTRGL_Shutdown(void);
void *CTRGL_GetProc(const char *name);
void CTRGL_EndFrame(void); /* presents whatever was drawn since the last call */
void CTRGL_RegisterCvars(void);

/* sys_ctr.c */
void CTR_PrintMemory(const char *when);

#endif
