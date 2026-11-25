/* Bulwark engine - the controller slots SDL's pads and Steam Input's
 * controllers share (vid_sdl.c, in_steam.c). ISC licence, see LICENSE. */
#ifndef BW_IN_PADS_H
#define BW_IN_PADS_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_PADS 4

/* a free slot for a Steam Input controller (its handle), or -1 */
int Pad_ClaimSteam(uint64_t handle);
void Pad_Release(int slot);
/* the slot a player just used: its prompts become in_padglyphs */
void Pad_Touched(int slot);

/* in_steam.c */
void INS_Init(const char *exedir);
void INS_Shutdown(void);
void INS_Frame(void);
/* true while Steam Input drives the controllers: SDL's view of the same
 * pads (Steam's virtual gamepad, or the device itself) is then ignored */
bool INS_Active(void);
uint64_t INS_SlotHandle(int slot); /* 0 when the slot isn't Steam Input's */
int INS_SlotSet(int slot);         /* the slot's glyph set (si_set_t), -1 if not Steam's */
void INS_Rumble(int slot, float low, float high, float seconds);
void INS_LED(int slot, const float *rgb);
/* the prompt for a key or command on a Steam Input controller slot */
bool INS_Glyph(int slot, const char *what, char *path, size_t pathsize, char *label, size_t labelsize);

#endif
