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
	int split, treefaces, farpolys;          /* two passes: nearest faces in the near pass, of how many */
	int entsflicker;                        /* models taking turns (Ents_Flicker) */
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
/* World_End in parts, for drawing the view in two passes (multipass_nds.c):
 * Collect walks the tree; Split says how many of the nearest faces fit a
 * budget, counting the models (nearest first) that stand nearer than each,
 * and how far away (squared, map units) the near pass ends; DrawPass
 * draws a range of them, plus the brush entities on that side of split2 */
void World_Collect(void);
int World_Split(int maxverts, int maxpolys, const int32_t *mdist2, const int *mpolys, int nummodels, int32_t *dist2);
int World_NumTreeFaces(void);
int World_Cost(int from, int to, int32_t split2, bool nearside, int *verts); /* polygons DrawPass would send */
void World_DrawPass(int from, int to, int32_t split2, bool nearside, ndsstats_t *stats, int maxverts, int maxpolys);
#define WORLD_MAX_VERTS 6000
#define WORLD_MAX_POLYS 2000
const char *World_Entities(void);
int World_LightAt(const int16_t *org); /* 0..31, from the floor below */
int World_TextureBytes(void);
extern bool World_MergeFaces; /* join coplanar faces at load (default on) */
int World_MergedFaces(void);  /* faces the last load joined into others */
extern int World_MergeLight;  /* light steps (of 31) a dropped vertex may be off */
int World_TotalPolys(void);   /* GPU polygons the whole map would take */
enum { WORLD_PORTALS_OFF, WORLD_PORTALS_ON, WORLD_PORTALS_AUTO };
extern int World_Portals;     /* follow portals out from the eye rather than draw the whole PVS; auto (the
                                 default) builds them only for maps without a PVS. Read at load. */
int World_PortalCount(void);  /* worked out at load; 0 if that failed */
int World_PortalLeaves(void); /* leaves reached this frame, or -1 if the PVS was used */
int World_PortalLoadMs(void);
int World_PortalTests(void);  /* portals looked through this frame */
int World_PvsLeaves(void);    /* leaves the PVS allows from the eye's leaf */
int Nds_Vblanks(void);        /* screen refreshes since start-up (main_nds.c) */

/* models_nds.c: Quake models (.mdl) on the map's entities, plus test crowds */
void Ents_Load(const uint16_t *palette);                       /* from the loaded map's entities */
int Ents_AddCrowd(const ndscam_t *cam, const char *model, int count); /* a grid in front of cam */
void Ents_Free(void);
void Ents_Draw(const ndscam_t *cam, ndsstats_t *stats);        /* world and models together */
/* Ents_Draw in parts, for two passes: Select culls and sorts after World_Begin;
 * DrawPass draws the models on one side of split2 (squared map units) */
void Ents_Select(const ndscam_t *cam);
int Ents_Costs(int32_t *dist2, int *polys, int max, int *viewpolys); /* nearest first, estimated */
/* flicker: -1 leaves out the models past the budget; otherwise it is the
 * GPU polygons the world leaves for models, and those that don't fit take turns */
void Ents_DrawPass(const ndscam_t *cam, int32_t split2, bool nearside, bool withviewmodel, int flicker, ndsstats_t *stats);
int Ents_FlickerBudget(int worldpolys, int worldverts); /* -1 when Ents_Flicker is off */
extern bool Ents_Flicker; /* models that don't fit take turns, NES style, rather than being left out */
void Gpu_Counts(int *polys, int *verts); /* what the GPU has stored so far this frame */

/* multipass_nds.c: the view in two passes, a far one captured to VRAM and a
 * near one drawn over it, for twice the GPU's polygons at 30 fps */
void Mp_Init(void);                  /* keeps VRAM C and D out of texture use */
void Mp_Frame(const ndscam_t *cam, ndsstats_t *stats); /* two refreshes */
void Mp_Stop(void);                  /* back to drawing the 3D straight to the screen */

/* fs_nds.c: loose files in the base folders, then pak0.pak ... pak9.pak */
void Fs_Init(void);
int Fs_NumPaks(void);
uint8_t *Fs_Load(const char *name, size_t *size); /* malloc'd, with a 0 after the end */
void Fs_List(const char *dir, const char *ext, void (*fn)(const char *name, void *user), void *user);

/* skel_nds.c: skeletons cut down on the PC (tools/nds/skelreduce), posed on
 * the CPU, a box per hitbox for now */
typedef struct skel_s skel_t;
typedef struct {
	int32_t m[12]; /* the GPU's 4x3: the bone's axes (4.12) as rows, then its origin (1/8 units) */
} skmat_t;
skel_t *Skel_Load(const char *path, char *err, int errsize);
void Skel_Free(skel_t *s);
int Skel_NumBones(const skel_t *s);
int Skel_NumClips(const skel_t *s);
int Skel_FileBytes(const skel_t *s);
const char *Skel_ClipName(const skel_t *s, int clip);
int Skel_ClipFrames(const skel_t *s, int clip);
void Skel_Pose(const skel_t *s, int clip, int32_t time, skmat_t *out); /* time in seconds, 20.12 */
void Skel_Draw(const skel_t *s, const skmat_t *pose, const int32_t *org, int yaw);
void Skel_Bench(const char *args, char *result, int resultsize); /* "<file> <copies> [clip]" */
void Skel_View(const char *args);                                /* "<file> <copies>", until START */

/* main_nds.c */
void Nds_PrintResults(void); /* the bench results so far, on the bottom screen */
extern const char *const nds_basedirs[];
#define NDS_NUMBASEDIRS 4
void Nds_Init3D(void);
void Nds_InitConsole(void);   /* the bottom screen's text, in VRAM H */
void Nds_ResetTextures(void); /* glResetTextures, keeping VRAM C and D out of it */

/* link_nds.c: join a game on a PC over WiFi (docs/spec/link.md) */
void Link_Run(const uint16_t *palette);
bool Link_AutoJoin(void); /* link.txt says autojoin=1 */

#endif
