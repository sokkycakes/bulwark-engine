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
#define BW_NDS_MAX(a, b) ((a) > (b) ? (a) : (b))
#define BW_NDS_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* world_nds.c */
typedef struct {
	int32_t org[3]; /* eye position in 1/8 map units */
	int yaw, pitch; /* libnds angles (32768 to a turn); pitch up is positive */
	int roll;       /* the view leaning, positive to the right */
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
/* world_nds.c: the clip hulls for the link's movement prediction (hull_nds.c) */
const char *World_HullError(void); /* why the last map has none, "" if it has */
void World_StartPosition(ndscam_t *cam);
int World_Spots(ndscam_t *out, int max); /* where to look round from: starts, landmarks, path nodes */
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
int World_TextureFails(void); /* textures of this map VRAM had no room for */
bool World_HasLit(void); /* the map came with coloured light (.lit) */
float World_TraceFraction(const float start[3], const float end[3]); /* a point through hull 0, the world alone */
/* brush entities as a server has them (link play): shown or not, and moved
 * to org (1/8 units) and turned by ang (pitch yaw roll, libnds angles); org
 * NULL puts it back where the map built it */
void World_ModelState(int model, bool shown, const int32_t *org, const int *ang);
void World_ResetModels(void);
int World_NumModels(void);
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
/* models the server names, for linked play (link_nds.c): Ents_Model loads
 * a path such as models/scientist.mdl once (-1 if it can't), Ents_Clear
 * empties the list each frame and Ents_AddLinked puts one in it, playing
 * sequence seq (mapped to its baked clip) ms into it; Ents_SetTime gives
 * the clock a walk or run the server doesn't play goes by. Ents_LerpFrames
 * blends between the baked frames (default on) */
int Ents_Model(const char *path);
void Ents_Clear(void);
void Ents_Counts(int *loaded, int *failed);
/* light -1: look it up; returns it. alpha 0-255 (255 opaque); dead: a body,
 * which plays a death clip of the model's when seq maps to none */
int Ents_AddLinked(int mdl, const int16_t *org, int yaw, int seq, uint32_t ms, int speed, int light, int alpha, bool dead);
void Ents_SetViewModel(int mdl, int seq, uint32_t ms, int light); /* the weapon in hand, -1 for none; light -1: a default */
void Ents_SetTime(uint32_t ms);
extern bool Ents_LerpFrames;
extern int32_t Ents_ViewOfs[3]; /* the view model moved by this (1/8 units: forward, left, up), for bob */
extern int Ents_ViewAng[3];     /* and turned by this (libnds angles): pitch (nose down), yaw (left), roll (right) */
/* view models other than Half-Life's own (Counter-Strike Online's and the like) are moved by this
 * as well (1/8 units: forward, left, up; link.txt vm_forward, vm_right, vm_up): they are built to
 * sit higher and further right */
extern int32_t Ents_ViewShift[3];
bool Ents_ViewShifted(void); /* the view model in hand takes Ents_ViewShift */
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
uint8_t *Fs_LoadRange(const char *name, long pos, long len, size_t *size); /* part of one, the same way */
long Fs_Size(const char *name); /* -1: not there */
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

/* frame profiling: compiled in only with BULWARK_NDS_PROF (the rig's measurement
 * builds define it); otherwise every macro is empty. PROF_T(t) starts a lap in a
 * local, PROF_LAP(i, t) adds the time since to section i and starts the next */
enum {
	PROF_BEGIN,   /* World_Begin: PVS or portal flood */
	PROF_SELECT,  /* Ents_Select */
	PROF_COLLECT, /* World_Collect: the tree walk */
	PROF_ENTS,    /* the models sent (Ents_DrawPass) */
	PROF_WAIT1,   /* waiting on the geometry engine before the world */
	PROF_CHAIN,   /* the world's faces into texture chains */
	PROF_SUBMIT,  /* the world's faces sent */
	PROF_REST,    /* things, markers, effects */
	PROF_WAIT2,   /* waiting on the geometry engine at the end */
	PROF_N
};
#ifdef BULWARK_NDS_PROF
extern uint32_t Nds_Prof[PROF_N], Nds_ProfMax[PROF_N], Nds_ProfFrame[PROF_N];
uint64_t tickGetCount(void);
#define PROF_T(t) uint32_t t = (uint32_t)tickGetCount()
#define PROF_LAP(i, t) do { uint32_t n_ = (uint32_t)tickGetCount(); Nds_ProfFrame[i] += n_ - (t); (t) = n_; } while (0)
extern uint32_t Nds_ProfCount[8];
#define PROF_COUNT(i, n) (Nds_ProfCount[i] += (n))
#else
#define PROF_COUNT(i, n) ((void)0)
#define PROF_T(t) do { } while (0)
#define PROF_LAP(i, t) do { } while (0)
#endif

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
