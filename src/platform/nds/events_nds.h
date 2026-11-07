/* Bulwark engine - the DS link's EVENT records (docs/spec/link.md): what the PC's
 * client shows from the server besides the world, shown the way Nuclide's client
 * code shows it. ISC licence, see LICENSE.
 *
 * View shakes (src/client/shake.qc), fades (src/client/fade.qc), damage arrows and
 * tint (coop/src/hud/jtf.qc Jtf_DrawFeedback), centerprints, prints and chat
 * (src/client/prints.qc, chat.qc), game_text and env_message (src/client/text.qc),
 * the co-op kill feed (coop/panorama hud.js) and pickup notices (valve/src/hud/hud.qc).
 * Text goes on its own layer in front of the HUD (BG2, set up by hud_nds.c), the rest on
 * the 3D view. */
#ifndef BW_EVENTS_NDS_H
#define BW_EVENTS_NDS_H

#include <stdbool.h>
#include <stdint.h>

#include "nds.h"

#include "hud_nds.h" /* the font, and the text's layer */

void Events_Reset(void);
/* the map changed (a WELCOME with another spawncount) */
void Events_MapChange(void);
/* an EVENT packet after the type byte (from the session on) */
void Events_Packet(const uint8_t *d, int len, uint32_t ms);
/* the last record taken in order, for INPUT's 'E' */
uint16_t Events_Ack(void);
/* once a frame, before the view is drawn: ms is the link's clock, health the player's */
void Events_Frame(uint32_t ms, int health);
/* the shake on the camera (origin in 1/8 units, roll) */
void Events_View(ndscam_t *view);
/* the fade and the damage feedback over the 3D scene (after everything else) */
void Events_Draw3D(const ndscam_t *view);
/* the text on the HUD layer: drawn in RAM, then put on the layer by Events_Commit2D
 * right after the wait for the vertical blank */
void Events_Draw2D(void);
void Events_Commit2D(void);

/* for the rig's checks: records taken, and what is up now */
typedef struct {
	uint32_t taken, gaps;
	uint16_t redraws;
	uint32_t lastdraw_ticks; /* the last redraw's time, tickGetCount() */
	uint8_t shaking, fading, damage, texts;
} evstats_t;
extern evstats_t Events_Stats;

#endif
