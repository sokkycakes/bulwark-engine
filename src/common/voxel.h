/* Bulwark engine - mutable voxel worlds (the "Studio" block grid).
 *
 * A world is a box of cells, each holding a block type and a rotation. Block
 * types are defined at run time (by the game's QC) with a collision shape,
 * content bits and a look; the engine does not know any game's block list.
 * The same code runs on the server and on the client, which keeps a mirror
 * fed by svc_voxel messages. Design notes and file layout:
 * docs/spec/voxel.md. ISC licence, see LICENSE. */
#ifndef BW_VOXEL_H
#define BW_VOXEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VX_CHUNK 16                    /* cells per chunk edge */
#define VX_CHUNKCELLS (VX_CHUNK * VX_CHUNK * VX_CHUNK)
#define VX_MAX_TYPES 4096              /* block type ids 0..4095, 0 is empty */
#define VX_MAX_EDGE 1024               /* cells per world edge */
#define VX_MAX_CELLS (64u * 1024u * 1024u)
#define VX_FILE_VERSION 1

/* collision shapes; the comments give the solid part of a unit cell for
 * rotation 0 (rotations turn it about +Z by 90 degrees each) */
typedef enum {
	VXS_EMPTY,     /* nothing */
	VXS_BOX,       /* the whole cell */
	VXS_SLOPE,     /* wedge: z <= x, rising towards +X */
	VXS_RSLOPE,    /* upside-down wedge: z >= 1 - x */
	VXS_SLAB_LOW,  /* z <= 0.5 */
	VXS_SLAB_HIGH, /* z >= 0.5 */
	VXS_THIN,      /* panel: x >= 0.875 (fences, doors, hatches) */
	VXS_FIELD,     /* the whole cell, but with no solid bit by default
	                  (water, ladders, trigger volumes) */
	VXS_COUNT
} vxshape_t;

typedef struct {
	bool defined;
	uint8_t shape;       /* vxshape_t */
	uint32_t contents;   /* Q3 content bits (CONTENTS_SOLID, _WATER, _LADDER...) */
	uint8_t color[4];    /* tint and alpha; alpha < 255 draws translucent */
	uint8_t flags;       /* VXF_* */
	char name[48];       /* stable name used in save files */
	char texture[96];    /* image for all faces, "" for a flat colour */
} vxdef_t;

#define VXF_NODRAW 1     /* collides but is not drawn (clip, triggers in play) */
#define VXF_FULLBRIGHT 2

typedef struct {
	uint16_t type[VX_CHUNKCELLS];
	uint8_t rot[VX_CHUNKCELLS];
	int count;           /* non-empty cells */
	bool dirty;          /* changed since the renderer last built it */
} vxchunk_t;

typedef struct vxworld_s {
	int size[3];         /* in cells */
	float blocksize;     /* world units per cell edge */
	float origin[3];     /* world position of cell (0,0,0)'s minimum corner */
	int nchunks[3];
	vxchunk_t **chunks;  /* NULL entries are empty */
	vxdef_t defs[VX_MAX_TYPES];
	int count;           /* non-empty cells in the world */
	uint32_t revision;   /* bumped on every change */
	bool anydirty;
} vxworld_t;

/* life cycle; Vox_Create returns NULL when the size is out of range */
vxworld_t *Vox_Create(const int size[3], float blocksize, const float origin[3]);
void Vox_Free(vxworld_t *w);
void Vox_Clear(vxworld_t *w); /* empties every cell, keeps the type table */

bool Vox_Define(vxworld_t *w, int type, const vxdef_t *def);
int Vox_TypeForName(const vxworld_t *w, const char *name); /* -1 if none */

/* cells; out of range reads give type 0 */
bool Vox_InBounds(const vxworld_t *w, int x, int y, int z);
int Vox_Get(const vxworld_t *w, int x, int y, int z, int *rot);
/* false when out of range or the type is not defined (0 always works) */
bool Vox_Set(vxworld_t *w, int x, int y, int z, int type, int rot);

/* world <-> cell coordinates */
void Vox_CellForPoint(const vxworld_t *w, const float p[3], int out[3]);
void Vox_CellMins(const vxworld_t *w, int x, int y, int z, float out[3]);

/* collision. Sweeps the box mins/maxs from start to end against solid cells
 * whose contents meet mask. Fills a trace the way BSP traces do. */
typedef struct {
	float fraction;
	float endpos[3];
	float normal[3];
	float dist;
	bool startsolid, allsolid;
	int contents;
	int cell[3];         /* the cell hit, valid when fraction < 1 or startsolid */
	int type;            /* its block type */
} vxtrace_t;

void Vox_Trace(const vxworld_t *w, const float start[3], const float mins[3], const float maxs[3],
               const float end[3], uint32_t mask, vxtrace_t *tr);
uint32_t Vox_PointContents(const vxworld_t *w, const float p[3]);
/* true when the box at origin overlaps a cell whose contents meet mask */
bool Vox_BoxTouches(const vxworld_t *w, const float origin[3], const float mins[3], const float maxs[3],
                    uint32_t mask);

/* the planes of a shape in a unit cell: normal and distance, solid where
 * dot(n, p) <= d for every plane. Returns the plane count. */
int Vox_ShapePlanes(int shape, int rot, float planes[][4]);

/* --- serialisation --------------------------------------------------------- */

/* one chunk as a run-length stream (used on the wire and in files). Returns
 * the bytes written, 0 if out did not have room. */
size_t Vox_EncodeChunk(const vxworld_t *w, int cx, int cy, int cz, uint8_t *out, size_t outsize);
/* decodes into the chunk at cx,cy,cz; false on a malformed stream or an
 * undefined type, leaving the chunk unchanged */
bool Vox_DecodeChunk(vxworld_t *w, int cx, int cy, int cz, const uint8_t *in, size_t len);

/* a whole world, with its type table (by name) and an opaque metadata block
 * the game keeps with it (entity settings, links, room options). The caller
 * frees *out with Mem_Free. */
bool Vox_SaveToMemory(const vxworld_t *w, const uint8_t *meta, size_t metalen, uint8_t **out, size_t *outlen);
/* Builds a new world from a file image. Block names are matched against
 * defs (the current game's table); unknown names become type 0 and are
 * counted in *unknown. On failure returns NULL and puts a reason in err. */
vxworld_t *Vox_LoadFromMemory(const uint8_t *data, size_t len, const vxworld_t *defs, uint8_t **meta,
                              size_t *metalen, int *unknown, char *err, size_t errsize);

uint32_t Vox_CRC32(const void *data, size_t len);

#endif
