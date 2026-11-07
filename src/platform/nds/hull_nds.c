/* Bulwark engine - the map's clip hulls on the DS, for the link's movement prediction.
 * ISC licence, see LICENSE.
 *
 * The predicted player has to hit what the server's player hits, so this is the server's
 * own collision (src/server/bspfile.c q1_trace and hull_trace, src/server/sv_world.c SV_Move)
 * written again in plain C99, for the DS and for tools/nds/pmtest.c on the host. Each step
 * is done in the same float operations in the same order as there, so that the same map
 * gives the same bits.
 *
 * The hulls come from <map>.hul, which tools/nds/hlmap.py writes beside the map (the
 * converted map has no clip hulls), or from the map's own clipnodes lump. The .hul holds
 * them as they sit in RAM: the nodes a model's player hulls reach, renumbered, and their
 * planes, an axial plane (normal exactly +1 on one axis) as its distance alone. Hull 0, the
 * drawing tree, is world_nds.c's (see Hull_SetHull0); on the host Hull_Load can load it too.
 *
 * The .hul, little endian: "BWH1", u8 rule (1 Quake: hull 1; 2 Half-Life: hulls 1 and 3),
 * u8 numhulls, u16 flags (bit 0 WIDE), u32 numaxial, numgeneral, numnodes, nummodels; then
 * numaxial f32 dists, numgeneral f32 normal[3] + dist, numnodes nodes (u16 plane, s16
 * children[2]; WIDE: u32 plane, s32 children[2]; plane: the type in the top two bits, 0-2
 * axial on that axis, 3 general, then the index into that array; a child < 0 is Quake
 * contents), and nummodels x numhulls s32 heads. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hull_nds.h"

#define DIST_EPSILON 0.03125f
#define Q1C_SOLID (-2)
#define MOVE_TRIGGERS 16 /* sv_local.h; pmove never asks for it */
#define PART (8 * 1024)  /* largest read while loading */
/* the most of each a map or .hul may say it has: past these the sizes below would wrap on the
 * DS's 32-bit size_t and long (BSP limits are 32767 clipnodes and planes, 400-512 models) */
#define MAX_COUNT (1 << 20)
#define MAX_MODELS 65536

enum { L_PLANES = 1, L_NODES = 5, L_CLIPNODES = 9, L_LEAFS = 10, L_MODELS = 14, NUM_LUMPS = 15 };

/* nodes as the .hul and RAM hold them; plane: the type (0-2 axial, 3 general) in the top
 * two bits, the index into that type's array below */
typedef struct { uint16_t plane; int16_t children[2]; } hnode_t;
typedef struct { uint32_t plane; int32_t children[2]; } hwnode_t; /* WIDE */

typedef struct {
	const void *nodes;
	bool wide;
	const float *axial;
	const float (*general)[4];
} htree_t;

static struct {
	int rule, numhulls, nummodels;
	htree_t t;
	int numnodes, numaxial, numgeneral;
	void *nodes;
	float *axial;
	float (*general)[4];
	int32_t *heads;        /* nummodels x numhulls */
	float (*bounds)[6];    /* each model's mins, maxs (L_MODELS) */
	size_t bytes;
	const hull0_if *h0;    /* Hull_SetHull0's */
	/* the host's own hull 0, with HULL_WITH_HULL0 */
	hull0_if own0;
	htree_t t0;
	void *nodes0;
	float (*general0)[4];
	int32_t *heads0;
	hull_fxnode_t *fxnodes;
	hull_fxplane_t *fxplanes;
	int *fxleafs;
} H;

/* the scene: brush entities and boxes, and the ones Hull_Focus kept */
typedef struct {
	hullbrush_t b;
	float amin[3], amax[3]; /* absmin/absmax as SV_LinkEdict makes them: origin + bounds -+ 1 */
	int32_t ib[6];          /* the same in 1/8 units, rounded outward, for Hull_Focus */
} sbrush_t;
typedef struct {
	hullbox_t b;
	float amin[3], amax[3];
	int32_t ib[6];
} sbox_t;
static sbrush_t *brushes;
static int numbrushes, brushcap;
static sbox_t boxes[HULL_MAX_BOXES];
static int numboxes;
static uint16_t candb[HULL_MAX_BRUSHES], candx[HULL_MAX_BOXES];
static int numcandb, numcandx;
static bool focused;

const pmtrace_if Hull_Pmove = {NULL, Hull_Move, Hull_Focus};

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int16_t rds16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static float rdf(const uint8_t *p)
{
	uint32_t u = rd32(p);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

/* The DS has no FPU, so float work is kept to what changes the result. Two exact shortcuts:
 * a float comparison made on the bits (fkey orders them as the floats, -0 with +0), and
 * a - b >= 0 being a >= b (a difference of floats is never rounded to zero) */
static inline uint32_t fbits(float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}

static inline int32_t fkey(float f)
{
	uint32_t u = fbits(f);
	return u & 0x80000000u ? -(int32_t)(u & 0x7fffffffu) : (int32_t)u;
}

/* +0 + x: x, but -0 becomes +0 */
static inline float plus0(float x) { return fbits(x) == 0x80000000u ? 0.0f : x; }

/* --- the walk (bspfile.c hull_contents, hull_trace) ---------------------------------------- */

static inline uint32_t h_plane(const htree_t *t, int num)
{
	return t->wide ? ((const hwnode_t *)t->nodes)[num].plane : ((const hnode_t *)t->nodes)[num].plane;
}

static inline int h_child(const htree_t *t, int num, int side)
{
	return t->wide ? ((const hwnode_t *)t->nodes)[num].children[side] : ((const hnode_t *)t->nodes)[num].children[side];
}

/* DotProduct(p, normal) - dist; for an axial plane p[type] - dist, the same bits */
static inline float h_dist(const htree_t *t, uint32_t pl, const float *p)
{
	uint32_t type = t->wide ? pl >> 30 : pl >> 14, i = t->wide ? pl & 0x3fffffffu : pl & 0x3fffu;
	const float *g;
	if (type < 3)
		return p[type] - t->axial[i];
	g = t->general[i];
	return p[0] * g[0] + p[1] * g[1] + p[2] * g[2] - g[3];
}

static void h_normal(const htree_t *t, uint32_t pl, bool back, float *n)
{
	uint32_t type = t->wide ? pl >> 30 : pl >> 14, i = t->wide ? pl & 0x3fffffffu : pl & 0x3fffu;
	for (int k = 0; k < 3; k++) {
		float v = type < 3 ? (k == (int)type ? 1.0f : 0.0f) : t->general[i][k];
		n[k] = back ? -v : v;
	}
}

/* h_dist(...) < 0 */
static inline int h_back(const htree_t *t, uint32_t pl, const float *p)
{
	uint32_t type = t->wide ? pl >> 30 : pl >> 14, i = t->wide ? pl & 0x3fffffffu : pl & 0x3fffu;
	if (type < 3)
		return fkey(p[type]) < fkey(t->axial[i]);
	return h_dist(t, pl, p) < 0;
}

static int h_contents(const htree_t *t, int num, const float *p)
{
	int guard = 0;
	while (num >= 0 && guard++ < 65536)
		num = h_child(t, num, h_back(t, h_plane(t, num), p));
	return num;
}

typedef struct {
	const htree_t *t;
	int head;
	hulltrace_t *tr;
	bool allsolid, startsolid;
	int depth;
} hwork_t;

static bool h_trace(hwork_t *w, int num, float f1, float f2, const float *p1, const float *p2)
{
	uint32_t pl;
	float t1, t2, frac, midf, mid[3];
	int side;
	if (num < 0) {
		if (num != Q1C_SOLID)
			w->allsolid = false;
		else
			w->startsolid = true;
		return true;
	}
	if (++w->depth > 4096)
		return true;
	pl = h_plane(w->t, num);
	{
		/* which side each end is on, before working out how far */
		int b1 = h_back(w->t, pl, p1), b2 = p2 == p1 ? b1 : h_back(w->t, pl, p2);
		if (b1 == b2)
			return h_trace(w, h_child(w->t, num, b1), f1, f2, p1, p2);
	}
	t1 = h_dist(w->t, pl, p1);
	t2 = h_dist(w->t, pl, p2);
	frac = t1 < 0 ? (t1 + DIST_EPSILON) / (t1 - t2) : (t1 - DIST_EPSILON) / (t1 - t2);
	frac = frac < 1 ? frac : 1;
	frac = 0 > frac ? 0 : frac;
	side = t1 < 0;
	midf = f1 + (f2 - f1) * frac;
	for (int k = 0; k < 3; k++)
		mid[k] = p1[k] + (p2[k] - p1[k]) * frac;
	if (!h_trace(w, h_child(w->t, num, side), f1, midf, p1, mid))
		return false;
	if (h_contents(w->t, h_child(w->t, num, side ^ 1), mid) != Q1C_SOLID)
		return h_trace(w, h_child(w->t, num, side ^ 1), midf, f2, mid, p2);
	if (w->allsolid)
		return false;
	h_normal(w->t, pl, side != 0, w->tr->normal);
	while (h_contents(w->t, w->head, mid) == Q1C_SOLID) {
		frac -= 0.1f;
		if (frac < 0) {
			w->tr->fraction = f1;
			memcpy(w->tr->endpos, p1, sizeof(float) * 3);
			return false;
		}
		midf = f1 + (f2 - f1) * frac;
		for (int k = 0; k < 3; k++)
			mid[k] = p1[k] + (p2[k] - p1[k]) * frac;
	}
	w->tr->fraction = midf;
	memcpy(w->tr->endpos, mid, sizeof(float) * 3);
	return false;
}

static void trace_clear(const float *end, hulltrace_t *tr)
{
	memset(tr, 0, sizeof(*tr));
	tr->fraction = 1;
	memcpy(tr->endpos, end, sizeof(float) * 3);
}

/* the end of q1_trace (bspfile.c:1058-1070) once the hull and its offset are known */
static void tree_trace(const htree_t *t, int head, const float *off, const float *start, const float *end,
                       hulltrace_t *tr)
{
	hwork_t w;
	float p1[3], p2[3];
	bool point = !memcmp(start, end, sizeof(p1));
	memset(&w, 0, sizeof(w));
	w.t = t, w.head = head, w.tr = tr, w.allsolid = true;
	for (int k = 0; k < 3; k++) {
		/* x - (+0) is x */
		p1[k] = fbits(off[k]) ? start[k] - off[k] : start[k];
		p2[k] = fbits(off[k]) ? end[k] - off[k] : end[k];
	}
	tr->fraction = 1;
	memcpy(tr->endpos, p2, sizeof(p2));
	h_trace(&w, head, 0, 1, p1, point ? p1 : p2);
	tr->startsolid = w.startsolid;
	tr->allsolid = w.allsolid;
	if (w.allsolid) {
		tr->fraction = 0;
		memcpy(tr->endpos, p1, sizeof(p1));
	}
	tr->hit = tr->fraction < 1 || w.startsolid;
	for (int k = 0; k < 3; k++)
		tr->endpos[k] = fbits(off[k]) ? tr->endpos[k] + off[k] : plus0(tr->endpos[k]);
}

/* --- hull 0 in fixed point (world_nds.c's tree) ------------------------------------------- */

/* positions in 1/65536 units, which holds any float of 128 or more exactly, so an axial
 * plane's test is the server's to the bit there; n . p - dist comes out in units x 2^28 */
typedef struct {
	const uint8_t *nodes, *planes, *leafs;
	int ns, ps, ls, head;
	hulltrace_t *tr;
	bool allsolid, startsolid;
	int depth;
} fxwork_t;

/* lrintf(v * 65536) for v clamped to +-32767, on the float's bits */
static inline int32_t fx_pos(float v)
{
	uint32_t u = fbits(v), e = (u >> 23) & 0xff, m = (u & 0x7fffffu) | 0x800000u, r;
	int sh = (int)e - 134; /* v * 65536 = m * 2^sh */
	if (e == 0)
		return 0;
	if (sh >= 0) {
		r = sh > 7 ? 0x7fff0000u : (uint32_t)((uint64_t)m << sh) > 0x7fff0000u ? 0x7fff0000u : m << sh;
	} else if (sh < -24) {
		r = 0;
	} else {
		uint32_t rem = m & ((1u << -sh) - 1), half = 1u << (-sh - 1);
		r = m >> -sh;
		if (rem > half || (rem == half && (r & 1)))
			r++;
	}
	return u & 0x80000000u ? -(int32_t)r : (int32_t)r;
}

static inline const int *fx_node(const fxwork_t *w, int num) { return (const int *)(w->nodes + (size_t)num * w->ns); }

static inline int64_t fx_dist(const fxwork_t *w, int plane, const int32_t *p)
{
	const int32_t *pl = (const int32_t *)(w->planes + (size_t)plane * w->ps);
	return (int64_t)pl[0] * p[0] + (int64_t)pl[1] * p[1] + (int64_t)pl[2] * p[2] - (int64_t)pl[3] * 8192;
}

/* a leaf child as bspfile.c's hull 0 has it (load_q1 :434-441): the leaf's contents, or solid */
static inline int fx_leaf(const fxwork_t *w, int c)
{
	int q = *(const int *)(w->leafs + (size_t)(-(c + 1)) * w->ls);
	return q < 0 ? q : Q1C_SOLID;
}

static int fx_contents(const fxwork_t *w, int num, const int32_t *p)
{
	int guard = 0;
	while (num >= 0 && guard++ < 65536) {
		const int *nd = fx_node(w, num);
		num = nd[1 + (fx_dist(w, nd[0], p) < 0)];
	}
	return num < 0 ? fx_leaf(w, num) : num; /* as hull_contents: a node number is not solid */
}

static bool fx_trace(fxwork_t *w, int num, float f1, float f2, const float *p1, const float *p2, const int32_t *q1,
                     const int32_t *q2)
{
	const int *nd;
	int64_t t1, t2;
	float ft1, ft2, frac, midf, mid[3];
	int32_t qm[3];
	int side;
	if (num < 0) {
		if (fx_leaf(w, num) != Q1C_SOLID)
			w->allsolid = false;
		else
			w->startsolid = true;
		return true;
	}
	if (++w->depth > 4096)
		return true;
	nd = fx_node(w, num);
	t1 = fx_dist(w, nd[0], q1);
	t2 = q2 == q1 ? t1 : fx_dist(w, nd[0], q2);
	if (t1 >= 0 && t2 >= 0)
		return fx_trace(w, nd[1], f1, f2, p1, p2, q1, q2);
	if (t1 < 0 && t2 < 0)
		return fx_trace(w, nd[2], f1, f2, p1, p2, q1, q2);
	ft1 = (float)t1 * (1.0f / 268435456.0f);
	ft2 = (float)t2 * (1.0f / 268435456.0f);
	frac = t1 < 0 ? (ft1 + DIST_EPSILON) / (ft1 - ft2) : (ft1 - DIST_EPSILON) / (ft1 - ft2);
	frac = frac < 1 ? frac : 1;
	frac = 0 > frac ? 0 : frac;
	side = t1 < 0;
	midf = f1 + (f2 - f1) * frac;
	for (int k = 0; k < 3; k++) {
		mid[k] = p1[k] + (p2[k] - p1[k]) * frac;
		qm[k] = fx_pos(mid[k]);
	}
	if (!fx_trace(w, nd[1 + side], f1, midf, p1, mid, q1, qm))
		return false;
	if (fx_contents(w, nd[1 + (side ^ 1)], qm) != Q1C_SOLID)
		return fx_trace(w, nd[1 + (side ^ 1)], midf, f2, mid, p2, qm, q2);
	if (w->allsolid)
		return false;
	{
		const int32_t *pl = (const int32_t *)(w->planes + (size_t)nd[0] * w->ps);
		for (int k = 0; k < 3; k++)
			w->tr->normal[k] = side ? -((float)pl[k] * (1.0f / 4096.0f)) : (float)pl[k] * (1.0f / 4096.0f);
	}
	while (fx_contents(w, w->head, qm) == Q1C_SOLID) {
		frac -= 0.1f;
		if (frac < 0) {
			w->tr->fraction = f1;
			memcpy(w->tr->endpos, p1, sizeof(float) * 3);
			return false;
		}
		midf = f1 + (f2 - f1) * frac;
		for (int k = 0; k < 3; k++) {
			mid[k] = p1[k] + (p2[k] - p1[k]) * frac;
			qm[k] = fx_pos(mid[k]);
		}
	}
	w->tr->fraction = midf;
	memcpy(w->tr->endpos, mid, sizeof(mid));
	return false;
}

void Hull_Trace0Fixed(const hull_fxnode_t *nodes, int nodestride, const hull_fxplane_t *planes,
                      int planestride, const int *leafcontents, int leafstride, int headnode,
                      const float origin[3], const float start[3], const float end[3],
                      hulltrace_t *tr)
{
	fxwork_t w;
	float off[3], p1[3], p2[3];
	int32_t q1[3], q2[3];
	trace_clear(end, tr);
	if (!nodes || !planes || !leafcontents)
		return;
	memset(&w, 0, sizeof(w));
	w.nodes = (const uint8_t *)nodes, w.planes = (const uint8_t *)planes, w.leafs = (const uint8_t *)leafcontents;
	w.ns = nodestride, w.ps = planestride, w.ls = leafstride, w.head = headnode;
	w.tr = tr, w.allsolid = true;
	for (int k = 0; k < 3; k++) {
		off[k] = plus0(origin[k]); /* (hm - mins) + origin, both 0 for hull 0 */
		p1[k] = fbits(off[k]) ? start[k] - off[k] : start[k];
		p2[k] = fbits(off[k]) ? end[k] - off[k] : end[k];
		q1[k] = fx_pos(p1[k]);
		q2[k] = fx_pos(p2[k]);
	}
	memcpy(tr->endpos, p2, sizeof(p2));
	if (!memcmp(q1, q2, sizeof(q1)) && !memcmp(p1, p2, sizeof(p1)))
		fx_trace(&w, headnode, 0, 1, p1, p1, q1, q1); /* a point: one test a node */
	else
		fx_trace(&w, headnode, 0, 1, p1, p2, q1, q2);
	tr->startsolid = w.startsolid;
	tr->allsolid = w.allsolid;
	if (w.allsolid) {
		tr->fraction = 0;
		memcpy(tr->endpos, p1, sizeof(p1));
	}
	tr->hit = tr->fraction < 1 || w.startsolid;
	for (int k = 0; k < 3; k++)
		tr->endpos[k] = fbits(off[k]) ? tr->endpos[k] + off[k] : plus0(tr->endpos[k]);
}

/* --- models ------------------------------------------------------------------------------- */

static const float q1_hull1[3] = {-16, -16, -24};
static const float hl_hull1[3] = {-16, -16, -36}, hl_hull3[3] = {-16, -16, -18};

void Hull_TraceModel(int model, const float origin[3], const float start[3], const float mins[3],
                     const float maxs[3], const float end[3], hulltrace_t *tr)
{
	float size = maxs[0] - mins[0], off[3];
	const float *hm;
	int hull, slot = 0;
	trace_clear(end, tr);
	if (H.rule == HULL_NONE || model < 0 || model >= H.nummodels)
		return;
	if (H.rule == HULL_HL)
		hull = size <= 8 ? 0 : size <= 36 ? (maxs[2] - mins[2] <= 36 ? 3 : 1) : 2;
	else
		hull = size < 3 ? 0 : size <= 32 ? 1 : 2;
	if (hull == 0) {
		const hull0_if *h = H.h0 ? H.h0 : H.own0.trace ? &H.own0 : NULL;
		if (h)
			h->trace(h->ctx, model, origin, start, end, tr);
		return;
	}
	/* hull 2 (wider than the player) is not kept: the player's box is 32 wide */
	if (hull == 3)
		slot = 1;
	hm = H.rule == HULL_HL ? (hull == 3 ? hl_hull3 : hl_hull1) : q1_hull1;
	for (int k = 0; k < 3; k++)
		off[k] = fbits(hm[k]) == fbits(mins[k]) ? plus0(origin[k]) : hm[k] - mins[k] + origin[k];
	tree_trace(&H.t, H.heads[model * H.numhulls + slot], off, start, end, tr);
}

static void own_hull0(void *ctx, int model, const float origin[3], const float start[3], const float end[3],
                      hulltrace_t *tr)
{
	float off[3];
	trace_clear(end, tr);
	if (model < 0 || model >= H.nummodels)
		return;
	if (H.fxnodes) {
		Hull_Trace0Fixed(H.fxnodes, sizeof(hull_fxnode_t), H.fxplanes, sizeof(hull_fxplane_t), H.fxleafs,
		                 sizeof(int), H.heads0[model], origin, start, end, tr);
		return;
	}
	for (int k = 0; k < 3; k++)
		off[k] = plus0(origin[k]);
	tree_trace(&H.t0, H.heads0[model], off, start, end, tr);
}

int Hull_Rule(void) { return H.rule; }
size_t Hull_Bytes(void) { return H.bytes; }
int Hull_NumModels(void) { return H.nummodels; }
void Hull_SetHull0(const hull0_if *h) { H.h0 = h; }

void Hull_ModelBounds(int model, float mins[3], float maxs[3])
{
	for (int k = 0; k < 3; k++) {
		mins[k] = model >= 0 && model < H.nummodels ? H.bounds[model][k] : 0;
		maxs[k] = model >= 0 && model < H.nummodels ? H.bounds[model][3 + k] : 0;
	}
}

/* --- loading ------------------------------------------------------------------------------ */

void Hull_Free(void)
{
	free(H.nodes), free(H.axial), free(H.general), free(H.heads), free(H.bounds);
	free(H.nodes0), free(H.general0), free(H.heads0);
	free(H.fxnodes), free(H.fxplanes), free(H.fxleafs);
	memset(&H, 0, sizeof(H));
	free(brushes);
	brushes = NULL;
	numbrushes = brushcap = numboxes = numcandb = numcandx = 0;
	focused = false;
}

static bool read_parts(hull_read_fn read, void *ctx, const char *path, long ofs, long len, void *dst)
{
	uint8_t *d = dst;
	while (len > 0) {
		long n = len < PART ? len : PART;
		if (read(ctx, path, ofs, n, d) != n)
			return false;
		ofs += n, d += n, len -= n;
	}
	return true;
}

static void *alloc_all(size_t n) { return malloc(n ? n : 1); }

static bool fail(char *err, int errsize, const char *msg)
{
	snprintf(err, (size_t)errsize, "hull: %s", msg);
	Hull_Free();
	return false;
}

/* axial when the normal is exactly +1 on one axis and +0 on the others: then the plane's
 * p[type] - dist and its normal are the same bits as the general form's */
static int axial_type(const float *n)
{
	uint32_t b[3];
	memcpy(b, n, sizeof(b));
	for (int k = 0; k < 3; k++)
		if (b[k] == 0x3f800000u && b[(k + 1) % 3] == 0 && b[(k + 2) % 3] == 0)
			return k;
	return 3;
}

/* the RAM the final arrays take, and room for the reserve after them (one try at it) */
static bool alloc_hulls(size_t reserve, char *err, int errsize)
{
	size_t ns = H.t.wide ? sizeof(hwnode_t) : sizeof(hnode_t);
	void *probe;
	H.bytes = ns * (size_t)H.numnodes + 4 * (size_t)H.numaxial + 16 * (size_t)H.numgeneral +
	          4 * (size_t)H.numhulls * (size_t)H.nummodels + 24 * (size_t)H.nummodels;
	H.nodes = alloc_all(ns * (size_t)H.numnodes);
	H.axial = alloc_all(4 * (size_t)H.numaxial);
	H.general = alloc_all(16 * (size_t)H.numgeneral);
	H.heads = alloc_all(4 * (size_t)H.numhulls * (size_t)H.nummodels);
	H.bounds = alloc_all(24 * (size_t)H.nummodels);
	probe = reserve ? malloc(reserve) : NULL;
	if (!H.nodes || !H.axial || !H.general || !H.heads || !H.bounds || (reserve && !probe)) {
		char msg[48];
		free(probe);
		snprintf(msg, sizeof(msg), "no RAM %d", (int)((H.bytes + reserve + 1023) / 1024));
		return fail(err, errsize, msg);
	}
	free(probe);
	H.t.nodes = H.nodes, H.t.axial = H.axial, H.t.general = (const float (*)[4])H.general;
	return true;
}

/* every node a kept head reaches, every plane they use, and the heads, in range */
static bool check_hulls(void)
{
	for (int i = 0; i < H.numnodes; i++) {
		uint32_t pl = h_plane(&H.t, i), type = H.t.wide ? pl >> 30 : pl >> 14;
		uint32_t idx = H.t.wide ? pl & 0x3fffffffu : pl & 0x3fffu;
		if (idx >= (uint32_t)(type < 3 ? H.numaxial : H.numgeneral))
			return false;
		for (int s = 0; s < 2; s++)
			if (h_child(&H.t, i, s) >= H.numnodes)
				return false;
	}
	for (int i = 0; i < H.numhulls * H.nummodels; i++)
		if (H.heads[i] >= H.numnodes)
			return false;
	return true;
}

static bool load_hul(const char *hulpath, const uint8_t *hd, hull_read_fn read, void *ctx, int nummodels,
                     size_t reserve, char *err, int errsize)
{
	long ofs = 24;
	int rule = hd[4], numhulls = hd[5], flags = hd[6] | hd[7] << 8;
	uint32_t na = rd32(hd + 8), ng = rd32(hd + 12), nn = rd32(hd + 16);
	uint8_t past;
	/* narrow nodes hold 14-bit plane indices and 16-bit children */
	if ((rule != HULL_Q1 && rule != HULL_HL) || numhulls != (rule == HULL_HL ? 2 : 1) || (int)rd32(hd + 20) != nummodels ||
	    na > MAX_COUNT || ng > MAX_COUNT || nn > MAX_COUNT || (!(flags & 1) && (na > 0x4000 || ng > 0x4000 || nn > 0x8000)))
		return fail(err, errsize, "bad .hul (made for another map, or another version)");
	H.numaxial = (int)na, H.numgeneral = (int)ng, H.numnodes = (int)nn;
	H.rule = rule, H.numhulls = numhulls, H.nummodels = nummodels, H.t.wide = (flags & 1) != 0;
	if (!alloc_hulls(reserve, err, errsize))
		return false;
	if (!read_parts(read, ctx, hulpath, ofs, 4L * H.numaxial, H.axial))
		return fail(err, errsize, ".hul cut short");
	ofs += 4L * H.numaxial;
	if (!read_parts(read, ctx, hulpath, ofs, 16L * H.numgeneral, H.general))
		return fail(err, errsize, ".hul cut short");
	ofs += 16L * H.numgeneral;
	{
		long ns = H.t.wide ? (long)sizeof(hwnode_t) : (long)sizeof(hnode_t);
		if (!read_parts(read, ctx, hulpath, ofs, ns * H.numnodes, H.nodes))
			return fail(err, errsize, ".hul cut short");
		ofs += ns * H.numnodes;
	}
	if (!read_parts(read, ctx, hulpath, ofs, 4L * H.numhulls * H.nummodels, H.heads))
		return fail(err, errsize, ".hul cut short");
	ofs += 4L * H.numhulls * H.nummodels;
	if (read(ctx, hulpath, ofs, 1, &past) > 0) /* hlmap.py writes nothing after the heads */
		return fail(err, errsize, "bad .hul (longer than it says)");
	if (!check_hulls())
		return fail(err, errsize, "bad .hul (an index out of range)");
	return true;
}

/* the map's own clip hulls (a Quake map, or a Half-Life original on the host), pruned to what
 * the kept hulls of the models reach: what hlmap.py --hul-only writes, built here */
static bool load_clipnodes(const char *path, const uint32_t *lo, const uint32_t *ll, int hl, hull_read_fn read,
                           void *ctx, size_t reserve, char *err, int errsize)
{
	static const int hulls[2][2] = {{1, 1}, {1, 3}};
	int ncl = (int)(ll[L_CLIPNODES] / 8), numplanes = (int)(ll[L_PLANES] / 20), nm = H.nummodels, nh = hl ? 2 : 1;
	uint8_t *cl = NULL, *mo = NULL, *part = NULL;
	int32_t *map = NULL, *stack = NULL, *pmap = NULL, *heads = NULL;
	int nn = 0, na = 0, ng = 0, sp;
	bool ok = false;
	if (!ncl)
		return fail(err, errsize, "no clip hulls (a converted map needs its .hul from hlmap.py)");
	if (ncl > MAX_COUNT || numplanes > MAX_COUNT)
		return fail(err, errsize, "bad map (too many clipnodes or planes)");
	cl = malloc((size_t)ncl * 8);
	mo = malloc((size_t)nm * 64);
	map = malloc(sizeof(int32_t) * (size_t)ncl);
	stack = malloc(sizeof(int32_t) * (size_t)ncl * 2 + 8);
	pmap = malloc(sizeof(int32_t) * (size_t)(numplanes ? numplanes : 1));
	heads = malloc(sizeof(int32_t) * (size_t)nm * nh);
	part = malloc(400 * 20);
	if (!cl || !mo || !map || !stack || !pmap || !heads || !part) {
		snprintf(err, (size_t)errsize, "hull: no RAM %d", (int)(((size_t)ncl * 24 + (size_t)nm * 72) / 1024));
		goto done;
	}
	if (!read_parts(read, ctx, path, (long)lo[L_CLIPNODES], (long)ncl * 8, cl) ||
	    !read_parts(read, ctx, path, (long)lo[L_MODELS], (long)nm * 64, mo)) {
		snprintf(err, (size_t)errsize, "hull: %s cut short", path);
		goto done;
	}
	/* children as bspfile.c reads them (load_q1 :443-458): a child past the end is taken as an
	 * unsigned short minus 65536, which is never -2: as good as empty for a trace */
	for (int i = 0; i < ncl; i++) {
		int32_t pl = (int32_t)rd32(cl + i * 8);
		map[i] = -1;
		if (pl < 0 || pl >= numplanes)
			pl = 0;
		memcpy(cl + i * 8, &pl, 4);
		for (int k = 0; k < 2; k++) {
			int ch = rds16(cl + i * 8 + 4 + k * 2);
			if (ch >= ncl)
				ch = -1;
			cl[i * 8 + 4 + k * 2] = (uint8_t)(ch & 0xff), cl[i * 8 + 5 + k * 2] = (uint8_t)((ch >> 8) & 0xff);
		}
	}
	sp = 0;
	for (int m = 0; m < nm; m++)
		for (int h = 0; h < nh; h++) {
			int32_t head = (int32_t)rd32(mo + m * 64 + 36 + hulls[hl][h] * 4);
			if (head >= ncl)
				head = -1;
			heads[m * nh + h] = head;
			if (head >= 0)
				stack[sp++] = head;
			while (sp > 0) {
				int n = stack[--sp];
				if (map[n] >= 0)
					continue;
				map[n] = 0;
				for (int k = 0; k < 2; k++) {
					int ch = rds16(cl + n * 8 + 4 + k * 2);
					if (ch >= 0 && map[ch] < 0 && sp < ncl * 2)
						stack[sp++] = ch;
				}
			}
		}
	for (int i = 0; i < ncl; i++)
		if (map[i] >= 0)
			map[i] = nn++;
	for (int p = 0; p < numplanes; p++)
		pmap[p] = -1;
	for (int i = 0; i < ncl; i++)
		if (map[i] >= 0)
			pmap[rd32(cl + i * 8)] = 0;
	/* the planes they use, read a part at a time and sorted into the two arrays */
	{
		for (int p0 = 0; p0 < numplanes; p0 += 400) {
			int n = numplanes - p0 < 400 ? numplanes - p0 : 400;
			if (!read_parts(read, ctx, path, (long)lo[L_PLANES] + p0 * 20L, n * 20L, part)) {
				snprintf(err, (size_t)errsize, "hull: %s cut short", path);
				goto done;
			}
			for (int i = 0; i < n; i++)
				if (pmap[p0 + i] >= 0) {
					float nrm[3];
					for (int k = 0; k < 3; k++)
						nrm[k] = rdf(part + i * 20 + k * 4);
					pmap[p0 + i] = axial_type(nrm) < 3 ? na++ : ng++;
				}
		}
		H.numnodes = nn, H.numaxial = na, H.numgeneral = ng, H.numhulls = nh, H.rule = hl ? HULL_HL : HULL_Q1;
		H.t.wide = na > 0x3fff || ng > 0x3fff || nn > 0x7fff;
		free(stack), stack = NULL;
		if (!alloc_hulls(reserve, err, errsize))
			goto done;
		for (int p0 = 0; p0 < numplanes; p0 += 400) {
			int n = numplanes - p0 < 400 ? numplanes - p0 : 400;
			if (!read_parts(read, ctx, path, (long)lo[L_PLANES] + p0 * 20L, n * 20L, part)) {
				fail(err, errsize, "map cut short");
				goto done;
			}
			for (int i = 0; i < n; i++)
				if (pmap[p0 + i] >= 0) {
					float f[4];
					int type;
					for (int k = 0; k < 4; k++)
						f[k] = rdf(part + i * 20 + k * 4);
					type = axial_type(f);
					if (type < 3)
						H.axial[pmap[p0 + i]] = f[3];
					else
						memcpy(H.general[pmap[p0 + i]], f, sizeof(f));
					pmap[p0 + i] = (int32_t)((uint32_t)pmap[p0 + i] | (uint32_t)type << 30); /* the type goes with it */
				}
		}
	}
	for (int i = 0; i < ncl; i++) {
		int32_t pm, c[2];
		uint32_t type, idx, pl;
		if (map[i] < 0)
			continue;
		pm = pmap[rd32(cl + i * 8)];
		type = (uint32_t)pm >> 30, idx = (uint32_t)pm & 0x3fffffffu;
		pl = H.t.wide ? type << 30 | idx : type << 14 | idx;
		for (int k = 0; k < 2; k++) {
			c[k] = rds16(cl + i * 8 + 4 + k * 2);
			if (c[k] >= 0)
				c[k] = map[c[k]];
		}
		if (H.t.wide) {
			hwnode_t *d = &((hwnode_t *)H.nodes)[map[i]];
			d->plane = pl, d->children[0] = c[0], d->children[1] = c[1];
		} else {
			hnode_t *d = &((hnode_t *)H.nodes)[map[i]];
			d->plane = (uint16_t)pl, d->children[0] = (int16_t)c[0], d->children[1] = (int16_t)c[1];
		}
	}
	for (int i = 0; i < nm * nh; i++)
		H.heads[i] = heads[i] >= 0 ? map[heads[i]] : heads[i];
	ok = true;
done:
	free(cl), free(mo), free(map), free(stack), free(pmap), free(heads), free(part);
	if (!ok)
		Hull_Free();
	return ok;
}

/* hull 0 for the host: the drawing tree as bspfile.c turns it into clip nodes (load_q1
 * :409-441), in float, or (HULL_HULL0_FIXED) as world_nds.c holds it (:708-745) */
static bool load_hull0(const char *path, const uint32_t *lo, const uint32_t *ll, int flags, hull_read_fn read,
                       void *ctx, char *err, int errsize)
{
	int np = (int)(ll[L_PLANES] / 20), nn = (int)(ll[L_NODES] / 24), nl = (int)(ll[L_LEAFS] / 28);
	uint8_t *pl = malloc((size_t)np * 20 + 1), *nd = malloc((size_t)nn * 24 + 1), *lf = malloc((size_t)nl * 28 + 1);
	uint8_t mo[64];
	bool ok = pl && nd && lf && read_parts(read, ctx, path, (long)lo[L_PLANES], np * 20L, pl) &&
	          read_parts(read, ctx, path, (long)lo[L_NODES], nn * 24L, nd) &&
	          read_parts(read, ctx, path, (long)lo[L_LEAFS], nl * 28L, lf);
	H.heads0 = malloc(sizeof(int32_t) * (size_t)H.nummodels + 4);
	ok = ok && H.heads0;
	for (int m = 0; ok && m < H.nummodels; m++) {
		ok = read_parts(read, ctx, path, (long)lo[L_MODELS] + m * 64L, 64, mo);
		H.heads0[m] = ok ? (int32_t)rd32(mo + 36) : 0;
	}
	if (ok && (flags & HULL_HULL0_FIXED)) {
		H.fxplanes = malloc(sizeof(hull_fxplane_t) * (size_t)np + 1);
		H.fxnodes = malloc(sizeof(hull_fxnode_t) * (size_t)nn + 1);
		H.fxleafs = malloc(sizeof(int) * (size_t)nl + 1);
		ok = H.fxplanes && H.fxnodes && H.fxleafs;
		for (int i = 0; ok && i < np; i++) {
			for (int c = 0; c < 3; c++)
				H.fxplanes[i].n[c] = (int32_t)lrintf(rdf(pl + i * 20 + c * 4) * 4096);
			H.fxplanes[i].dist = (int32_t)lrintf(rdf(pl + i * 20 + 12) * 32768);
		}
		for (int i = 0; ok && i < nn; i++) {
			H.fxnodes[i].plane = (int)rd32(nd + i * 24);
			H.fxnodes[i].children[0] = rds16(nd + i * 24 + 4);
			H.fxnodes[i].children[1] = rds16(nd + i * 24 + 6);
		}
		for (int i = 0; ok && i < nl; i++)
			H.fxleafs[i] = (int)rd32(lf + i * 28);
	} else if (ok) {
		hwnode_t *n0 = malloc(sizeof(hwnode_t) * (size_t)nn + 1);
		H.nodes0 = n0;
		H.general0 = malloc(16 * (size_t)np + 1);
		ok = n0 && H.general0;
		for (int i = 0; ok && i < np; i++)
			for (int k = 0; k < 4; k++)
				H.general0[i][k] = rdf(pl + i * 20 + k * 4);
		for (int i = 0; ok && i < nn; i++) {
			int p = (int)rd32(nd + i * 24);
			n0[i].plane = 3u << 30 | (uint32_t)(p < 0 || p >= np ? 0 : p);
			for (int c = 0; c < 2; c++) {
				int ch = rds16(nd + i * 24 + 4 + c * 2);
				if (ch >= 0 && ch >= nn)
					ch = (int)(uint16_t)ch - 65536;
				if (ch >= nn || -(ch + 1) >= nl)
					ch = -1;
				if (ch < 0) {
					int q1c = (int)rd32(lf + (-(ch + 1)) * 28);
					ch = q1c < 0 ? q1c : Q1C_SOLID;
				}
				n0[i].children[c] = ch;
			}
		}
		H.t0.nodes = n0, H.t0.wide = true, H.t0.axial = NULL, H.t0.general = (const float (*)[4])H.general0;
	}
	free(pl), free(nd), free(lf);
	if (!ok)
		return fail(err, errsize, "can't load hull 0");
	H.own0.ctx = NULL, H.own0.trace = own_hull0;
	return true;
}

bool Hull_Load(const char *bsppath, int flags, hull_read_fn read, void *ctx, size_t reserve,
               char *err, int errsize)
{
	uint8_t head[4 + NUM_LUMPS * 8], hd[24];
	uint32_t lo[NUM_LUMPS], ll[NUM_LUMPS], ver;
	char hulpath[300];
	size_t n = strlen(bsppath);
	const char *dot = strrchr(bsppath, '.');
	Hull_Free();
	if (err && errsize > 0)
		err[0] = 0;
	if (read(ctx, bsppath, 0, sizeof(head), head) != (long)sizeof(head))
		return fail(err, errsize, "can't read the map");
	ver = rd32(head);
	if (ver != 29 && ver != 30)
		return fail(err, errsize, "not a BSP 29 or 30 map");
	for (int i = 0; i < NUM_LUMPS; i++)
		lo[i] = rd32(head + 4 + i * 8), ll[i] = rd32(head + 8 + i * 8);
	H.nummodels = (int)(ll[L_MODELS] / 64);
	if (H.nummodels <= 0)
		return fail(err, errsize, "no models");
	if (H.nummodels > MAX_MODELS)
		return fail(err, errsize, "bad map (too many models)");
	if (dot && !strchr(dot, '/'))
		n = (size_t)(dot - bsppath);
	snprintf(hulpath, sizeof(hulpath), "%.*s.hul", (int)n, bsppath);
	if (read(ctx, hulpath, 0, sizeof(hd), hd) == (long)sizeof(hd) && !memcmp(hd, "BWH1", 4)) {
		if (!load_hul(hulpath, hd, read, ctx, H.nummodels, reserve, err, errsize))
			return false;
	} else if (!load_clipnodes(bsppath, lo, ll, ver == 30, read, ctx, reserve, err, errsize)) {
		return false;
	}
	{
		/* the bounds, 64 models a read */
		uint8_t *mo = malloc(64 * 64);
		bool ok = mo != NULL;
		for (int m0 = 0; ok && m0 < H.nummodels; m0 += 64) {
			int n = H.nummodels - m0 < 64 ? H.nummodels - m0 : 64;
			ok = read_parts(read, ctx, bsppath, (long)lo[L_MODELS] + m0 * 64L, n * 64L, mo);
			for (int m = 0; ok && m < n; m++)
				for (int k = 0; k < 6; k++)
					H.bounds[m0 + m][k] = rdf(mo + m * 64 + k * 4);
		}
		free(mo);
		if (!ok)
			return fail(err, errsize, "map cut short, or no RAM");
	}
	if ((flags & (HULL_WITH_HULL0 | HULL_HULL0_FIXED)) && !load_hull0(bsppath, lo, ll, flags, read, ctx, err, errsize))
		return false;
	return true;
}

/* --- the scene and SV_Move (sv_world.c:348-471) ------------------------------------------- */

static int32_t fx8_down(float v)
{
	v = floorf(v * 8.0f) - 1.0f;
	return v < -1e9f ? -1000000000 : v > 1e9f ? 1000000000 : (int32_t)v;
}

static int32_t fx8_up(float v)
{
	v = ceilf(v * 8.0f) + 1.0f;
	return v < -1e9f ? -1000000000 : v > 1e9f ? 1000000000 : (int32_t)v;
}

/* a brush at origin: its absmin/absmax, and the same in 1/8 units */
static void place_brush(sbrush_t *s, const float origin[3])
{
	float mins[3], maxs[3];
	memcpy(s->b.origin, origin, sizeof(s->b.origin));
	Hull_ModelBounds(s->b.model, mins, maxs);
	for (int k = 0; k < 3; k++) {
		s->amin[k] = origin[k] + mins[k] - 1;
		s->amax[k] = origin[k] + maxs[k] + 1;
		s->ib[k] = fx8_down(s->amin[k]);
		s->ib[3 + k] = fx8_up(s->amax[k]);
	}
}

void Hull_SetBrushes(const hullbrush_t *b, int n)
{
	n = n < 0 ? 0 : n > HULL_MAX_BRUSHES ? HULL_MAX_BRUSHES : n;
	if (n > brushcap) {
		sbrush_t *nb = realloc(brushes, sizeof(sbrush_t) * (size_t)n);
		if (!nb)
			n = brushcap;
		else
			brushes = nb, brushcap = n;
	}
	for (int i = 0; i < n; i++) {
		brushes[i].b = b[i];
		place_brush(&brushes[i], b[i].origin);
	}
	numbrushes = n;
	focused = false;
}

void Hull_PlaceBrush(int i, const float origin[3])
{
	if (i < 0 || i >= numbrushes)
		return;
	place_brush(&brushes[i], origin);
	focused = false;
}

void Hull_SetBoxes(const hullbox_t *b, int n)
{
	n = n < 0 ? 0 : n > HULL_MAX_BOXES ? HULL_MAX_BOXES : n;
	for (int i = 0; i < n; i++) {
		sbox_t *s = &boxes[i];
		s->b = b[i];
		for (int k = 0; k < 3; k++) {
			s->amin[k] = b[i].mins[k] - 1;
			s->amax[k] = b[i].maxs[k] + 1;
			s->ib[k] = fx8_down(s->amin[k]);
			s->ib[3 + k] = fx8_up(s->amax[k]);
		}
	}
	numboxes = n;
	focused = false;
}

static bool ib_meets(const int32_t *a, const int32_t *b)
{
	return a[0] <= b[3] && a[1] <= b[4] && a[2] <= b[5] && a[3] >= b[0] && a[4] >= b[1] && a[5] >= b[2];
}

/* trace_aabb's "starts outside" (sv_world.c:247-257) for a box at org */
static bool box_overlaps(const float *org, const float *mins, const float *maxs, const float *bmin, const float *bmax)
{
	for (int i = 0; i < 3; i++)
		for (int side = 0; side < 2; side++) {
			float dist = side == 0 ? bmax[i] - mins[i] : -(bmin[i] - maxs[i]);
			float sign = side == 0 ? 1.0f : -1.0f;
			if (sign * org[i] - dist > 0)
				return false;
		}
	return true;
}

void Hull_Focus(void *ctx, const float org[3], const float mins[3], const float maxs[3],
                float radius, int flags)
{
	int32_t r[6];
	for (int k = 0; k < 3; k++) {
		r[k] = fx8_down(org[k] - radius);
		r[3 + k] = fx8_up(org[k] + radius);
	}
	numcandb = numcandx = 0;
	for (int i = 0; i < numbrushes; i++)
		if (ib_meets(brushes[i].ib, r))
			candb[numcandb++] = (uint16_t)i;
	for (int i = 0; i < numboxes; i++) {
		if (!ib_meets(boxes[i].ib, r))
			continue;
		if ((flags & PM_FOCUS_DROPSTUCK) && box_overlaps(org, mins, maxs, boxes[i].b.mins, boxes[i].b.maxs))
			continue;
		candx[numcandx++] = (uint16_t)i;
	}
	focused = true;
}

/* bspfile.c q1_contents for a brush entity's negative .skin (sv_world.c skin_contents) */
static uint32_t skin_bits(int skin)
{
	switch (skin) {
	case -2: return PM_CB_SOLID;
	case -3: return PM_CB_WATER;
	case -4: return PM_CB_SLIME;
	case -5: return PM_CB_LAVA;
	case -6: return PM_CB_SOLID | PM_CB_SKY;
	case -8: return PM_CB_PLAYERCLIP | 0x00020000u; /* CONTENTS_MONSTERCLIP */
	case -16: return PM_CB_LADDER;
	default: return 0;
	}
}

/* what trace_ent is, for pmhit_t's entsolid and entmins/entmaxs */
typedef struct {
	int solid;
	const float *amin, *amax;
} who_t;

static void set_ent(pmhit_t *tr, who_t *who, int ent, int solid, const float *amin, const float *amax)
{
	tr->ent = ent;
	who->solid = solid, who->amin = amin, who->amax = amax;
}

/* sv_world.c merge_bsp (:304-326) */
static void merge_bsp(pmhit_t *tr, who_t *who, const hulltrace_t *bt, uint32_t contents, int ent, int solid,
                      const float *amin, const float *amax)
{
	if (bt->allsolid || bt->startsolid) {
		tr->startsolid = true;
		if (bt->allsolid)
			tr->allsolid = true;
		if (tr->ent < 0 || bt->fraction < tr->fraction) {
			set_ent(tr, who, ent, solid, amin, amax);
			tr->contents = contents;
		}
	}
	if (bt->fraction < tr->fraction) {
		tr->fraction = bt->fraction;
		memcpy(tr->normal, bt->normal, sizeof(tr->normal));
		set_ent(tr, who, ent, solid, amin, amax);
		tr->contents = contents;
	}
}

/* sv_world.c trace_aabb (:242-300) */
static void trace_aabb(const float *start, const float *mins, const float *maxs, const float *end, const float *bmin,
                       const float *bmax, pmhit_t *tr, who_t *who, const sbox_t *box, uint32_t contents, bool trigger)
{
	float enter = -1, leave = 1, n[3] = {0, 0, 0};
	bool startout = false;
	for (int i = 0; i < 3; i++) {
		for (int side = 0; side < 2; side++) {
			float dist = side == 0 ? bmax[i] - mins[i] : -(bmin[i] - maxs[i]);
			float sign = side == 0 ? 1.0f : -1.0f;
			float d1 = sign * start[i] - dist, d2 = sign * end[i] - dist;
			if (d1 > 0)
				startout = true;
			if (d1 > 0 && (d2 >= 0.03125f || d2 >= d1))
				return;
			if (d1 <= 0 && d2 <= 0)
				continue;
			if (d1 > d2) {
				float f = (d1 - 0.03125f) / (d1 - d2);
				if (f > enter) {
					enter = f;
					n[0] = n[1] = n[2] = 0;
					n[i] = sign;
				}
			} else {
				float f = (d1 + 0.03125f) / (d1 - d2);
				if (f < leave)
					leave = f;
			}
		}
	}
	if (!startout) {
		if (trigger) {
			tr->startsolid = true;
			if (tr->ent < 0)
				set_ent(tr, who, box->b.edict, box->b.solid, box->amin, box->amax);
			return;
		}
		if (!tr->startsolid || tr->ent < 0) {
			tr->startsolid = tr->allsolid = true;
			tr->fraction = 0;
			set_ent(tr, who, box->b.edict, box->b.solid, box->amin, box->amax);
			tr->contents = contents;
		}
		return;
	}
	if (enter < leave && enter > -1 && enter < tr->fraction) {
		tr->fraction = enter < 0 ? 0 : enter;
		memcpy(tr->normal, n, sizeof(n));
		set_ent(tr, who, box->b.edict, box->b.solid, box->amin, box->amax);
		tr->contents = contents;
	}
}

static bool culled(const float *amin, const float *amax, const float *tmin, const float *tmax)
{
	for (int k = 0; k < 3; k++)
		if (fkey(amin[k]) > fkey(tmax[k]) || fkey(amax[k]) < fkey(tmin[k]))
			return true;
	return false;
}

static void move_brush(const sbrush_t *s, const float *start, const float *mins, const float *maxs, const float *end,
                       int type, uint32_t mask, pmhit_t *tr, who_t *who)
{
	hulltrace_t bt;
	uint32_t skinbits, contents;
	int solid = s->b.solid;
	if (solid != PM_SOLID_BSP && solid != 22)
		return;
	if (solid == 22 && !(type & (MOVE_TRIGGERS | PM_MOVE_EVERYTHING)))
		return;
	if ((type & PM_MOVE_NOMONSTERS) && solid != PM_SOLID_BSP)
		return;
	skinbits = skin_bits(s->b.skin);
	if (skinbits && !(skinbits & mask) && solid != 22)
		return;
	/* the Q1 trace never reads the mask, so emask (sv_world.c:431-437) changes nothing */
	Hull_TraceModel(s->b.model, s->b.origin, start, mins, maxs, end, &bt);
	contents = skinbits ? skinbits : bt.hit ? PM_CB_SOLID : 0;
	if (solid == 22 && bt.startsolid) {
		tr->startsolid = true;
		if (tr->ent < 0)
			set_ent(tr, who, s->b.edict, solid, s->amin, s->amax);
		return;
	}
	merge_bsp(tr, who, &bt, contents, s->b.edict, solid, s->amin, s->amax);
}

static void move_box(const sbox_t *s, const float *start, const float *mins, const float *maxs, const float *end,
                     int type, uint32_t mask, pmhit_t *tr, who_t *who)
{
	int solid = s->b.solid;
	uint32_t c = solid == 5 ? 0x04000000u : solid == PM_SOLID_BBOX || solid == PM_SOLID_SLIDEBOX ? PM_CB_BODY : PM_CB_SOLID;
	if (solid == PM_SOLID_NOT)
		return;
	if (solid == 1 && !(type & (MOVE_TRIGGERS | PM_MOVE_EVERYTHING)))
		return;
	if ((type & PM_MOVE_NOMONSTERS) && solid != PM_SOLID_BSP)
		return;
	if (!(c & mask) && solid != 1)
		return;
	trace_aabb(start, mins, maxs, end, s->b.mins, s->b.maxs, tr, who, s, c, solid == 1);
}

void Hull_Move(void *ctx, const float start[3], const float mins[3], const float maxs[3],
               const float end[3], int type, int32_t mask, pmhit_t *tr)
{
	static const float zero[3] = {0, 0, 0};
	float tmin[3], tmax[3];
	hulltrace_t bt;
	who_t who = {PM_SOLID_BSP, zero, zero};
	uint32_t m = mask ? (uint32_t)mask : PM_CB_SOLID | PM_CB_BODY | PM_CB_PLAYERCLIP;
	int nb = focused ? numcandb : numbrushes, nx = focused ? numcandx : numboxes, ib = 0, ix = 0;
	memset(tr, 0, sizeof(*tr));
	tr->fraction = 1;
	tr->ent = -1;
	if (type & PM_MOVE_EVERYTHING)
		m = 0xffffffffu;

	Hull_TraceModel(0, zero, start, mins, maxs, end, &bt);
	merge_bsp(tr, &who, &bt, bt.hit ? PM_CB_SOLID : 0, 0, PM_SOLID_BSP, zero, zero);

	for (int i = 0; i < 3; i++) {
		tmin[i] = (start[i] < end[i] ? start[i] : end[i]) + mins[i] - 1;
		tmax[i] = (start[i] > end[i] ? start[i] : end[i]) + maxs[i] + 1;
	}
	/* brushes and boxes as one list in edict order, until something leaves fraction 0 */
	while ((ib < nb || ix < nx) && tr->fraction > 0) {
		const sbrush_t *b = ib < nb ? &brushes[focused ? candb[ib] : ib] : NULL;
		const sbox_t *x = ix < nx ? &boxes[focused ? candx[ix] : ix] : NULL;
		if (b && (!x || b->b.edict <= x->b.edict)) {
			ib++;
			if (!culled(b->amin, b->amax, tmin, tmax))
				move_brush(b, start, mins, maxs, end, type, m, tr, &who);
		} else {
			ix++;
			if (!culled(x->amin, x->amax, tmin, tmax))
				move_box(x, start, mins, maxs, end, type, m, tr, &who);
		}
	}
	for (int i = 0; i < 3; i++)
		tr->endpos[i] = start[i] + (end[i] - start[i]) * tr->fraction;
	if (tr->ent <= 0) {
		tr->ent = 0;
		who.solid = PM_SOLID_BSP, who.amin = who.amax = zero;
	}
	tr->entsolid = who.solid;
	memcpy(tr->entmins, who.amin, sizeof(tr->entmins));
	memcpy(tr->entmaxs, who.amax, sizeof(tr->entmaxs));
}
