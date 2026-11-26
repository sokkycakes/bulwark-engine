/* Bulwark engine - pieces of the Xbox platform layer that talk to each
 * other. ISC licence, see LICENSE. */
#ifndef BW_XBOX_H
#define BW_XBOX_H

#include <stdbool.h>
#include <stddef.h>

/* gl_xbox.c: the renderer's GL subset on the NV2A, through pbkit */
void XBGL_RegisterCvars(void);
bool XBGL_Init(void);
void XBGL_Shutdown(void);
void *XBGL_GetProc(const char *name);
void XBGL_EndFrame(void); /* presents whatever was drawn since the last call */
int XBGL_Width(void);
int XBGL_Height(void);
size_t XBGL_GpuMemoryUsed(void); /* bytes of contiguous memory the GPU objects hold */
void XBGL_PrintGpuMemory(void);

/* sys_xbox.c */
void XBX_PrintMemory(const char *when);
size_t XBX_FreeMemory(void);
size_t XBX_TotalMemory(void);

#endif
