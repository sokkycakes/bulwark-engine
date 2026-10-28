/* Bulwark engine - Source BSP loading, shared between the map loader and
 * the prop collision builder. ISC licence, see LICENSE. */
#ifndef BW_BSP_SOURCE_H
#define BW_BSP_SOURCE_H

#include "bspfile.h"
#include "../common/vbsp.h"

/* a convex solid given by its corner points, in the space of the brush
 * model it belongs to (model 0 is the world) */
typedef struct {
	int model;
	int contents;
	int npts;
	float (*pts)[3];
} srchull_t;

typedef struct {
	srchull_t *h;
	int num, cap;
} srchulls_t;

void SrcHull_Add(srchulls_t *hs, int model, int contents, const float (*pts)[3], int n);
void SrcHull_Free(srchulls_t *hs);

/* most points a .phy piece contributes to one brush */
#define HULL_MAXPTS_PHY 256

/* collision for static props and prop entities whose models are not drawn:
 * the model's .phy convex pieces when the file is there, otherwise boxes
 * (see docs/spec/vbsp.md, "Prop collision") */
void SrcProps_Hulls(const vbsp_t *v, bspmap_t *m, srchulls_t *out);

bool BSP_LoadSource(bspmap_t *m, const uint8_t *d, size_t size);

#endif
