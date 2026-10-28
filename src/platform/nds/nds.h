/* Bulwark engine - the Nintendo DS map viewer. ISC licence, see LICENSE.
 *
 * The DS has 4 MB of RAM and no FPU: the full engine (Nuclide's progs alone
 * are 3.9 MB) can't run there. This build is a separate, small program that
 * loads a Quake-format map (BSP version 29) and flies through it on the DS's
 * 3D hardware (docs/platforms/nds.md). */
#ifndef BW_NDS_H
#define BW_NDS_H

#include <stdbool.h>
#include <stdint.h>

#define BW_NDS_MIN(a, b) ((a) < (b) ? (a) : (b))
#define BW_NDS_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* world_nds.c */
typedef struct {
	int32_t org[3]; /* eye position in 1/8 map units */
	int yaw, pitch; /* libnds angles (32768 to a turn); pitch up is positive */
} ndscam_t;

typedef struct {
	int faces, verts, polys, leaf, skipped;
} ndsstats_t;

/* palette: 256 RGB15 colours, or NULL for a grey ramp */
bool World_Load(const char *path, const uint16_t *palette, char *err, int errsize);
void World_Free(void);
bool World_Loaded(void);
void World_StartPosition(ndscam_t *cam);
void World_Draw(const ndscam_t *cam, ndsstats_t *stats);
void World_SetView(const ndscam_t *cam); /* the modelview World_Draw uses, for drawing more */
int World_TextureBytes(void);

/* main_nds.c */
extern const char *const nds_basedirs[];
#define NDS_NUMBASEDIRS 4
void Nds_Init3D(void);

/* link_nds.c: join a game on a PC over WiFi (docs/spec/link.md) */
void Link_Run(const uint16_t *palette);
bool Link_AutoJoin(void); /* link.txt says autojoin=1 */

#endif
