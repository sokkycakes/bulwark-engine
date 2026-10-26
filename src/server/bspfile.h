/* Bulwark engine - BSP map loading for collision and entity data.
 * Q3-family BSPs (IBSP v46 and the FBSP variant) load brush collision; Q1
 * family BSPs only provide entities and model bounds for now.
 * ISC licence, see LICENSE. */
#ifndef BW_BSPFILE_H
#define BW_BSPFILE_H

#include "../common/common.h"

/* Q3-style content bits, as used by trace_endcontentsi and hitcontentsmaski */
#define CONTENTS_SOLID 0x00000001
#define CONTENTS_LAVA 0x00000008
#define CONTENTS_SLIME 0x00000010
#define CONTENTS_WATER 0x00000020
#define CONTENTS_LADDER 0x00004000
#define CONTENTS_PLAYERCLIP 0x00010000
#define CONTENTS_MONSTERCLIP 0x00020000
#define CONTENTS_BODY 0x02000000
#define CONTENTS_CORPSE 0x04000000
#define CONTENTS_SKY 0x80000000u

typedef struct {
	float normal[3];
	float dist;
} cplane_t;

typedef struct {
	int plane;
	int shader;
} cbrushside_t;

typedef struct {
	int firstside, numsides;
	int contents;
	int shader;
	float mins[3], maxs[3];
} cbrush_t;

typedef struct {
	char name[64];
	int surfaceflags;
	int contents;
} cshader_t;

typedef struct {
	float mins[3], maxs[3];
	int firstbrush, numbrushes;
} cmodel_t;

typedef enum { BSP_NONE, BSP_Q1, BSP_Q3 } bsptype_t;

typedef struct {
	char name[128];
	bsptype_t type;
	char *entities;
	cplane_t *planes;
	int numplanes;
	cbrushside_t *brushsides;
	int numbrushsides;
	cbrush_t *brushes;
	int numbrushes;
	cshader_t *shaders;
	int numshaders;
	cmodel_t *models;
	int nummodels;
} bspmap_t;

bspmap_t *BSP_Load(const char *path);
void BSP_Free(bspmap_t *map);

typedef struct {
	float fraction;
	float endpos[3];
	float normal[3];
	float dist;
	bool startsolid, allsolid;
	int contents;     /* contents of what was hit */
	int surfaceflags;
	int shader;       /* -1 if none */
	int brush;        /* -1 if none */
	int brushside;
} bsptrace_t;

/* sweeps the box mins/maxs from start to end through one model's brushes,
 * offset by origin; only brushes whose contents intersect mask are hit */
void BSP_TraceModel(const bspmap_t *map, int model, const float *origin, const float *start,
                    const float *mins, const float *maxs, const float *end, int mask, bsptrace_t *tr);
int BSP_PointContents(const bspmap_t *map, int model, const float *origin, const float *p);

#endif
