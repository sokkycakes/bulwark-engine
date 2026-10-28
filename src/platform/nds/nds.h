/* Bulwark engine - the Nintendo DS map viewer. ISC licence, see LICENSE.
 *
 * The DS has 4 MB of RAM and no FPU: the full engine (Nuclide's progs alone
 * are 3.9 MB) can't run there. This build is a separate, small program that
 * loads a Quake-format map (BSP version 29) and flies through it on the DS's
 * 3D hardware (docs/platforms/nds.md). */
#ifndef BW_NDS_H
#define BW_NDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BW_NDS_MIN(a, b) ((a) < (b) ? (a) : (b))
#define BW_NDS_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* world_nds.c */
typedef struct {
	int32_t org[3]; /* eye position in 1/8 map units */
	int yaw, pitch; /* libnds angles (32768 to a turn); pitch up is positive */
} ndscam_t;

typedef struct {
	int faces, verts, polys, leaf, skipped; /* the world's share */
	int ents, entpolys, entsdropped;        /* models */
	int hwpolys, hwverts;                   /* what the GPU actually stored (after culling) */
} ndsstats_t;

/* palette: 256 RGB15 colours, or NULL for a grey ramp */
bool World_Load(const char *path, const uint16_t *palette, char *err, int errsize);
void World_Free(void);
bool World_Loaded(void);
void World_StartPosition(ndscam_t *cam);
void World_Draw(const ndscam_t *cam, ndsstats_t *stats);
void World_SetView(const ndscam_t *cam); /* the modelview World_Draw uses, for drawing more */
/* World_Draw in two halves, for drawing models in the same GPU budget:
 * Begin sets up the view and visibility, End draws the world in what's left */
void World_Begin(const ndscam_t *cam, ndsstats_t *stats);
bool World_BoxVisible(const int16_t *mins, const int16_t *maxs); /* map units; after Begin */
void World_End(ndsstats_t *stats, int reserve_verts, int reserve_polys);
const char *World_Entities(void);
int World_LightAt(const int16_t *org); /* 0..31, from the floor below */
int World_TextureBytes(void);

/* models_nds.c: Quake models (.mdl) on the map's entities, plus test crowds */
void Ents_Load(const uint16_t *palette);                       /* from the loaded map's entities */
int Ents_AddCrowd(const ndscam_t *cam, const char *model, int count); /* a grid in front of cam */
void Ents_Free(void);
void Ents_Draw(const ndscam_t *cam, ndsstats_t *stats);        /* world and models together */

/* fs_nds.c: loose files in the base folders, then pak0.pak ... pak9.pak */
void Fs_Init(void);
int Fs_NumPaks(void);
uint8_t *Fs_Load(const char *name, size_t *size); /* malloc'd, with a 0 after the end */
void Fs_List(const char *dir, const char *ext, void (*fn)(const char *name, void *user), void *user);

/* main_nds.c */
extern const char *const nds_basedirs[];
#define NDS_NUMBASEDIRS 4
void Nds_Init3D(void);

/* link_nds.c: join a game on a PC over WiFi (docs/spec/link.md) */
void Link_Run(const uint16_t *palette);
bool Link_AutoJoin(void); /* link.txt says autojoin=1 */

#endif
