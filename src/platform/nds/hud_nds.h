/* Bulwark engine - the DS link's HUD on the top screen: the co-op HUD the PC
 * draws (Nuclide coop/src/hud/jtf.qc and its Panorama layout, with the
 * Half-Life crosshair and Nuclide's nameplates and target ID), from HUDINFO
 * (docs/spec/link.md). ISC licence, see LICENSE. */
#ifndef BW_HUD_NDS_H
#define BW_HUD_NDS_H
#include "nds.h"

/* another entity as the link draws it this frame: a player (slot >= 0) or a
 * friendly NPC the server listed; the box from the bottom middle up */
typedef struct {
	uint16_t ent;
	int8_t slot;          /* the player's slot, -1 for another entity */
	int16_t org[3];       /* the bottom middle, map units */
	int16_t half, height; /* map units */
} hudent_t;

/* what the link knows itself */
typedef struct {
	int health, armor, frags; /* VIEW and STATS */
	const char *weapon;       /* STATS: the class without weapon_, "" none (a title for a server without HUDINFO) */
	bool haveweapon;          /* the weapon block or WEAPONINFO names one */
	int clip, clipsize, reserve; /* the predicted weapon's (Weapon_Ammo), as the PC's HUD reads them */
	const char *usekey, *firekey; /* the DS buttons for Use and attack */
	const char *diag1, *diag2;    /* the diagnostics line and the map's load error, "" none */
	uint32_t srvms;               /* the server's clock (ENTS time) as the link runs it */
	bool srvclock;                /* srvms runs (an ENTS came) */
} hudin_t;

void Hud_Init(void);  /* BG1 on the top screen, and the font */
void Hud_Done(void);
void Hud_Packet(const uint8_t *d, int len); /* HUDINFO after the type byte (from the session on) */
uint16_t Hud_Version(void);                 /* the HUDINFO version we have, for INPUT */
/* once a frame: view is the camera drawn this frame */
void Hud_Frame(const ndscam_t *view, const hudent_t *ents, int nents, const hudin_t *in);
#endif
