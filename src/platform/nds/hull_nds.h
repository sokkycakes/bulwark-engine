/* Bulwark engine - the map's clip hulls on the DS, for the link's movement prediction.
 * ISC licence, see LICENSE. Plain C99 without libnds (tools/nds/pmtest.c uses it on the host). */
#ifndef BW_HULL_NDS_H
#define BW_HULL_NDS_H
#include <stddef.h>
#include "pmove_nds.h"

enum { HULL_NONE, HULL_Q1, HULL_HL };       /* which hull a box uses: bspfile.c:1044-1048 */
/* Hull_Load: also load hull 0 from the BSP (host); with HULL_HULL0_FIXED, hull 0 is the DS's
 * fixed-point copy of the drawing tree (world_nds.c's planes) walked by Hull_Trace0Fixed */
enum { HULL_WITH_HULL0 = 1, HULL_HULL0_FIXED = 2 };
#define HULL_MAX_BRUSHES 512
#define HULL_MAX_BOXES 64

/* reads len bytes at ofs of path into dst; returns the bytes read (< len: short or error) */
typedef long (*hull_read_fn)(void *ctx, const char *path, long ofs, long len, void *dst);

typedef struct {
	float fraction, endpos[3], normal[3];
	bool startsolid, allsolid;
	bool hit;                   /* fraction < 1 or startsolid: bspfile.c:1067-1068 CONTENTS_SOLID */
} hulltrace_t;

/* hull 0 (the drawing tree): point traces through model m placed at origin. Q1 rules:
 * only leaf 0 and CONTENTS_SOLID (-2) leaves stop it (hull_trace, bspfile.c:978-1037) */
typedef struct {
	void *ctx;
	void (*trace)(void *ctx, int model, const float origin[3], const float start[3],
	              const float end[3], hulltrace_t *tr);
} hull0_if;

/* the DS's fixed-point drawing tree, as world_nds.c holds it (4.12 normals, dist x 32768):
 * the walk lives here so pmtest -dshull0 runs the same code */
typedef struct { int32_t n[3], dist; } hull_fxplane_t;
typedef struct { int plane; int children[2]; } hull_fxnode_t; /* < 0: leaf -(c + 1) */
void Hull_Trace0Fixed(const hull_fxnode_t *nodes, int nodestride, const hull_fxplane_t *planes,
                      int planestride, const int *leafcontents, int leafstride, int headnode,
                      const float origin[3], const float start[3], const float end[3],
                      hulltrace_t *tr);

/* reads the .hul header first; loads only if free heap afterwards >= Hull_Bytes + reserve */
bool Hull_Load(const char *bsppath, int flags, hull_read_fn read, void *ctx, size_t reserve,
               char *err, int errsize);
void Hull_Free(void);
int Hull_Rule(void);              /* HULL_NONE when nothing loaded */
size_t Hull_Bytes(void);          /* RAM the hulls hold */
int Hull_NumModels(void);
void Hull_ModelBounds(int model, float mins[3], float maxs[3]); /* from L_MODELS */
void Hull_SetHull0(const hull0_if *h); /* the DS: world_nds.c's; NULL: Hull_Load's own if any */

/* bspfile.c BSP_TraceModel for Q1 maps (q1_trace, :1039-1071): picks the hull by box size,
 * lines the hull's box up with mins, translation only */
void Hull_TraceModel(int model, const float origin[3], const float start[3], const float mins[3],
                     const float maxs[3], const float end[3], hulltrace_t *tr);

/* the scene the predicted player moves in, set by link_nds.c from BRUSHINFO and PSTATE.
 * Both lists sorted by edict (the caller sorts); copied */
typedef struct { uint16_t edict, model; uint8_t solid; int8_t skin; float origin[3]; } hullbrush_t;
typedef struct { uint16_t edict; float mins[3], maxs[3]; uint8_t solid; } hullbox_t; /* absolute */
void Hull_SetBrushes(const hullbrush_t *b, int n);
void Hull_PlaceBrush(int i, const float origin[3]); /* entry i of the last Hull_SetBrushes list, moved */
void Hull_SetBoxes(const hullbox_t *b, int n);

/* SV_Move against world + brushes + boxes: the pmtrace_if.move contract */
void Hull_Move(void *ctx, const float start[3], const float mins[3], const float maxs[3],
               const float end[3], int type, int32_t mask, pmhit_t *tr);
/* the pmtrace_if.focus contract: candidate lists for the next traces */
uint32_t Hull_Focus(void *ctx, const float org[3], const float mins[3], const float maxs[3],
                    float radius, int flags);
extern const pmtrace_if Hull_Pmove;                  /* { NULL, Hull_Move, Hull_Focus } */
#endif
