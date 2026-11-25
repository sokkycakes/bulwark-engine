/* Bulwark engine - BSP map loading for collision and entity data.
 * Q3-family BSPs (IBSP v46, RBSP and FBSP) collide with brushes; Q1-family
 * BSPs (BSP29 and BSP2) with their clipping hulls. Both carry the node tree
 * and the potentially visible sets.
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
	bool added; /* made by the loader (props, displacements), not in the file */
} cbrush_t;

typedef struct {
	char name[64];
	int surfaceflags;
	int contents;
} cshader_t;

typedef struct {
	float mins[3], maxs[3];
	int firstbrush, numbrushes;
	/* Q1: the root of each clipping hull (0 the node tree, 1 and 2 player
	 * and large hulls) and the faces drawn for the model */
	int headnode[4];
	int firstface, numfaces;
} cmodel_t;

/* Q1 faces, kept for surface queries (getsurfacenearpoint) on the server */
typedef struct {
	int plane;
	bool back;            /* the face looks along the plane's negative normal */
	int firstvert, numverts; /* into facevertices */
	int shader;           /* the miptex, into shaders */
	float mins[3], maxs[3];
} cface_t;

/* children: >= 0 a node, < 0 the leaf -(child + 1) */
typedef struct {
	int plane;
	int children[2];
	float mins[3], maxs[3];
} cnode_t;

typedef struct {
	int contents;      /* Q3 content bits */
	int cluster;       /* row of the visibility data, -1 for none */
	int area;
	float mins[3], maxs[3];
	int firstface, numfaces;   /* into leaffaces */
	int firstbrush, numbrushes; /* into leafbrushes (Q3) */
} cleaf_t;

/* Q1 clipping hull nodes: children >= 0 a node, < 0 Q1 contents */
typedef struct {
	int plane;
	int children[2];
} cclipnode_t;

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
	cnode_t *nodes;
	int numnodes;
	cleaf_t *leafs;
	int numleafs;
	int *leaffaces;
	int numleaffaces;
	int *leafbrushes;
	int numleafbrushes;
	/* one uncompressed row of numclusters bits per cluster; NULL sees all */
	uint8_t *vis;
	int numclusters, clusterbytes;
	/* Q1 hulls: hull 0 is the node tree turned into clip nodes */
	cclipnode_t *clipnodes, *hull0;
	int numclipnodes;
	cface_t *faces;
	int numfaces;
	float *facevertices; /* xyz triples */
	bool bsp2;
	/* GoldSrc BSP30: four hulls, sized for Half-Life players */
	bool hl;
	/* Source BSP (VBSP): loaded into the Q3 structures, see bsp_source.c */
	bool source;
} bspmap_t;

bspmap_t *BSP_Load(const char *path);
/* the surface of a model nearest a point (model-relative), -1 for none:
 * a Q1 face of the model, or a brush side for Q3-family maps */
int BSP_SurfaceNearPoint(const bspmap_t *map, int model, const float *p);
const char *BSP_SurfaceTexture(const bspmap_t *map, int model, int surface);
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
/* a Quake CONTENT_ value (-2 solid, -16 ladder, ...) as CONTENTS_ bits */
int BSP_QuakeContentsToBits(int contents);
void BSP_TraceModel(const bspmap_t *map, int model, const float *origin, const float *start,
                    const float *mins, const float *maxs, const float *end, int mask, bsptrace_t *tr);
int BSP_PointContents(const bspmap_t *map, int model, const float *origin, const float *p);

/* visibility: the leaf holding p, its cluster's potentially visible set
 * (NULL when everything is visible) and a point-to-point check */
int BSP_PointLeaf(const bspmap_t *map, const float *p);
const uint8_t *BSP_ClusterPVS(const bspmap_t *map, int cluster);
bool BSP_CheckPVS(const bspmap_t *map, const float *a, const float *b);
/* the same, with the leaf of a already found (BSP_PointLeaf; -1 sees all) */
bool BSP_CheckPVSLeaf(const bspmap_t *map, int la, const float *b);

#endif
