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
#include "fexact_nds.h"

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
	const int32_t (*gfix)[4]; /* each general plane as h_side_general reads it, or NULL */
} htree_t;

static struct {
	int rule, numhulls, nummodels;
	htree_t t;
	int numnodes, numaxial, numgeneral, numgeneral0;
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
	int32_t (*gfix)[4], (*gfix0)[4];
	hull_fxnode_t *fxnodes;
	hull_fxplane_t *fxplanes;
	int *fxleafs;
} H;

/* the scene: brush entities and boxes, and the ones Hull_Focus kept */
typedef struct {
	hullbrush_t b;
	float amin[3], amax[3]; /* absmin/absmax as SV_LinkEdict makes them: origin + bounds -+ 1 */
	int32_t ib[6];          /* the same in 1/8 units, rounded outward, for Hull_Focus */
	bool turned;            /* angles not all 0: */
	float axis[3][3];       /* forward, left, up (sv_world.c brush_frame) */
} sbrush_t;
typedef struct {
	hullbox_t b;
	float amin[3], amax[3];
	int32_t ib[6];
} sbox_t;
static sbrush_t *brushes;
static int numbrushes, brushcap;
static pmtouch_t touches[HULL_MAX_TOUCH]; /* Hull_SetTouches */
static int numtouches;
static sbox_t boxes[HULL_MAX_BOXES];
static int numboxes;
static uint16_t candb[HULL_MAX_BRUSHES], candx[HULL_MAX_BOXES];
static int numcandb, numcandx;
static bool focused;
/* what a trace's result depends on besides its arguments: bumped whenever the hulls, the
 * scene or the boxes Hull_Focus leaves out change (Hull_Move's cache) */
static uint32_t scene_gen = 1;
/* Hull_Move's last results (see there) */
#define TC_N 32
typedef struct {
	uint32_t gen, key[14];
	uint64_t drop; /* the drop_mask it was worked out with */
	pmhit_t tr;
} tcentry_t;
static tcentry_t *tcache; /* TC_N, on the heap with the hulls (Hull_Load): none while a map loads */
static uint64_t drop_mask; /* the boxes left out (PM_FOCUS_DROPSTUCK) while focused */
static uint64_t pin_mask;  /* boxes PM_FOCUS_DROPSTUCK keeps (Hull_PinBoxes) */

static void unfocus(void)
{
	focused = false;
	drop_mask = 0;
}

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

/* The DS has no FPU, so float work is kept to what changes the result: comparisons are made
 * on the bits (fexact_nds.h), and a - b >= 0 is a >= b (a difference of floats is never
 * rounded to zero) */
/* the walk runs from the DS's ITCM (fetched without waits, no cache to miss) */
#if defined(__NDS__) && defined(ARM9)
#define HOT __attribute__((section(".itcm"), noinline))
#else
#define HOT
#endif

#define fbits fx_bits
#define fkey fx_key
#define plus0 fx_plus0

/* newlib's memset and memcmp go a byte at a time (2.4 us for 24 bytes in melonDS): small
 * fixed sizes are done by words here */
static inline bool same3(const float *a, const float *b)
{
	return fbits(a[0]) == fbits(b[0]) && fbits(a[1]) == fbits(b[1]) && fbits(a[2]) == fbits(b[2]);
}

static inline void copy3(float *d, const float *s)
{
	d[0] = s[0], d[1] = s[1], d[2] = s[2];
}

/* --- the walk (bspfile.c hull_contents, hull_trace) ---------------------------------------- */

/* the node's plane: the type (0-2 axial, 3 general) and the index into that type's array */
#define H_TYPE(wide, pl) ((wide) ? (pl) >> 30 : (pl) >> 14)
#define H_INDEX(wide, pl) ((wide) ? (pl) & 0x3fffffffu : (pl) & 0x3fffu)
#define H_PLANE(wide, t, num) ((wide) ? ((const hwnode_t *)(t)->nodes)[num].plane : ((const hnode_t *)(t)->nodes)[num].plane)
#define H_CHILD(wide, t, num, side) \
	((wide) ? ((const hwnode_t *)(t)->nodes)[num].children[side] : ((const hnode_t *)(t)->nodes)[num].children[side])

/* DotProduct(p, normal) - dist; for an axial plane p[type] - dist, the same bits */
static inline float h_dist(const htree_t *t, uint32_t pl, const float *p)
{
	uint32_t type = H_TYPE(t->wide, pl), i = H_INDEX(t->wide, pl);
	const float *g;
	if (type < 3)
		return p[type] - t->axial[i];
	g = t->general[i];
	return p[0] * g[0] + p[1] * g[1] + p[2] * g[2] - g[3];
}

static void h_normal(const htree_t *t, uint32_t pl, bool back, float *n)
{
	uint32_t type = H_TYPE(t->wide, pl), i = H_INDEX(t->wide, pl);
	for (int k = 0; k < 3; k++) {
		float v = type < 3 ? (k == (int)type ? 1.0f : 0.0f) : t->general[i][k];
		n[k] = back ? -v : v;
	}
}

/* v * 2^sh truncated, on the float's bits, for |v| <= lim * 2^-sh (lim <= 2^30); false
 * when it is bigger */
static inline bool h_fix(float v, int sh, int32_t lim, int32_t *out)
{
	uint32_t u = fbits(v), e = (u >> 23) & 0xff, m = (u & 0x7fffffu) | 0x800000u;
	int k = (int)e - 150 + sh; /* v * 2^sh = m * 2^k */
	int32_t r;
	if (k > 7)
		return false;
	r = k >= 0 ? (int32_t)(m << k) : k > -32 ? (int32_t)(m >> -k) : 0;
	if (e == 0)
		r = 0; /* subnormal: below any precision used here */
	if (r > lim)
		return false;
	*out = u & 0x80000000u ? -r : r;
	return true;
}

/* a point of the walk, with what the side tests read worked out once: each coordinate's
 * float key (fkey), and the point in 1/65536 units for h_side_general (q_ok false when a
 * coordinate is 16384 or more away) */
typedef struct {
	float p[3];
	int32_t k[3], q[3];
	uint32_t qsum; /* |q0| + |q1| + |q2| */
	bool q_ok;
} hpt_t;

static inline void hpt_set(hpt_t *a)
{
	a->q_ok = true;
	a->qsum = 0;
	for (int i = 0; i < 3; i++) {
		a->k[i] = fkey(a->p[i]);
		if (!h_fix(a->p[i], 16, 16383 << 16, &a->q[i]))
			a->q_ok = false;
		a->qsum += (uint32_t)(a->q[i] < 0 ? -a->q[i] : a->q[i]);
	}
}

/* the sign of DotProduct(p, n) - d without the float work, when it can be told: the float
 * result F is within 2^-21 (|p0| + |p1| + |p2| + |d|) of the exact value (5 roundings of at
 * most 2^-24 of that each, |n| <= 1), and the value worked out here in 1/2^46 units is
 * within 2^-13 of it (p and d to 1/65536, n to 2^-30, |p|, |d| < 16384); further from 0
 * than both, F has its sign and is not 0. Returns 1 back, 0 front, -1 can't tell. */
/* the plane's normal in 2^-30 units and its distance in 1/65536 units, for h_side_general;
 * f[0] INT32_MIN when they don't fit */
static void h_gfix(const float *g, int32_t *f)
{
	if (!h_fix(g[0], 30, 1 << 30, &f[0]) || !h_fix(g[1], 30, 1 << 30, &f[1]) ||
	    !h_fix(g[2], 30, 1 << 30, &f[2]) || !h_fix(g[3], 16, 16383 << 16, &f[3]))
		f[0] = INT32_MIN;
}

static inline __attribute__((always_inline)) int h_side_general(const int32_t *f, const hpt_t *a)
{
	int32_t d = f[3];
	int64_t e, lim;
	if (!a->q_ok || f[0] == INT32_MIN)
		return -1;
	e = (int64_t)a->q[0] * f[0] + (int64_t)a->q[1] * f[1] + (int64_t)a->q[2] * f[2] - ((int64_t)d << 30);
	lim = ((int64_t)(a->qsum + (uint32_t)(d < 0 ? -d : d)) << 10) + ((int64_t)1 << 34); /* the sum < 2^32 */
	return e > lim ? 0 : e < -lim ? 1 : -1;
}

/* h_dist(...) < 0: an axial plane by the keys (p - dist < 0 is p < dist, a difference of
 * floats is never rounded to 0) */
static inline __attribute__((always_inline)) int h_back(const htree_t *t, bool wide, uint32_t pl, const hpt_t *a)
{
	uint32_t type = H_TYPE(wide, pl), i = H_INDEX(wide, pl);
	int side;
	if (type < 3)
		return a->k[type] < fkey(t->axial[i]);
	if (t->gfix) {
		side = h_side_general(t->gfix[i], a);
	} else {
		int32_t f[4];
		h_gfix(t->general[i], f);
		side = h_side_general(f, a);
	}
#ifdef HULL_SELFCHECK /* pmtest built with it: the shortcut must agree with the float test */
	if (side >= 0 && side != fx_lt(h_dist(t, pl, a->p), 0))
		abort();
#endif
	return side >= 0 ? side : fx_lt(h_dist(t, pl, a->p), 0);
}

static inline __attribute__((always_inline)) int h_contents_g(const htree_t *t, bool wide, int num, const hpt_t *a)
{
	int guard = 0;
	while (num >= 0 && guard++ < 65536)
		num = H_CHILD(wide, t, num, h_back(t, wide, H_PLANE(wide, t, num), a));
	return num;
}

typedef struct {
	const htree_t *t;
	int head;
	hulltrace_t *tr;
	bool allsolid, startsolid;
	int depth;
} hwork_t;

/* p1 + (p2 - p1) * frac for each axis; an axis where both ends are the same bits gives
 * p1 + (+0), with no float work */
static void h_lerp(const hpt_t *a, const hpt_t *b, float frac, hpt_t *mid)
{
	for (int k = 0; k < 3; k++)
		mid->p[k] = fbits(a->p[k]) == fbits(b->p[k]) ? plus0(a->p[k]) : a->p[k] + (b->p[k] - a->p[k]) * frac;
	hpt_set(mid);
}

static bool h_trace_n(hwork_t *w, int num, float f1, float f2, const hpt_t *a, const hpt_t *b);
static bool h_trace_w(hwork_t *w, int num, float f1, float f2, const hpt_t *a, const hpt_t *b);

/* hull_trace, for narrow or wide nodes (two copies, each with its own node layout) */
static inline __attribute__((always_inline)) bool h_trace_g(hwork_t *w, bool wide, int num, float f1, float f2,
                                                             const hpt_t *a, const hpt_t *b)
{
	const htree_t *t = w->t;
	uint32_t pl;
	float t1, t2, frac, midf;
	hpt_t mid;
	int side;
	for (;;) {
		if (num < 0) {
			if (num != Q1C_SOLID)
				w->allsolid = false;
			else
				w->startsolid = true;
			return true;
		}
		if (++w->depth > 4096)
			return true;
		pl = H_PLANE(wide, t, num);
		{
			/* which side each end is on, before working out how far: both on one side
			 * goes down that side (bspfile.c's tail call, as a loop) */
			int b1 = h_back(t, wide, pl, a), b2 = b == a ? b1 : h_back(t, wide, pl, b);
			if (b1 != b2)
				break;
			num = H_CHILD(wide, t, num, b1);
		}
	}
	t1 = h_dist(t, pl, a->p);
	t2 = h_dist(t, pl, b->p);
	side = fx_lt(t1, 0);
	frac = side ? (t1 + DIST_EPSILON) / (t1 - t2) : (t1 - DIST_EPSILON) / (t1 - t2);
	frac = fx_lt(frac, 1) ? frac : 1;
	frac = fx_gt(0, frac) ? 0 : frac;
	midf = f1 + (f2 - f1) * frac;
	h_lerp(a, b, frac, &mid);
	if (!(wide ? h_trace_w : h_trace_n)(w, H_CHILD(wide, t, num, side), f1, midf, a, &mid))
		return false;
	if (h_contents_g(t, wide, H_CHILD(wide, t, num, side ^ 1), &mid) != Q1C_SOLID)
		return (wide ? h_trace_w : h_trace_n)(w, H_CHILD(wide, t, num, side ^ 1), midf, f2, &mid, b);
	if (w->allsolid)
		return false;
	h_normal(t, pl, side != 0, w->tr->normal);
	while (h_contents_g(t, wide, w->head, &mid) == Q1C_SOLID) {
		frac -= 0.1f;
		if (fx_lt(frac, 0)) {
			w->tr->fraction = f1;
			copy3(w->tr->endpos, a->p);
			return false;
		}
		midf = f1 + (f2 - f1) * frac;
		h_lerp(a, b, frac, &mid);
	}
	w->tr->fraction = midf;
	copy3(w->tr->endpos, mid.p);
	return false;
}

HOT static bool h_trace_n(hwork_t *w, int num, float f1, float f2, const hpt_t *a, const hpt_t *b)
{
	return h_trace_g(w, false, num, f1, f2, a, b);
}

HOT static bool h_trace_w(hwork_t *w, int num, float f1, float f2, const hpt_t *a, const hpt_t *b)
{
	return h_trace_g(w, true, num, f1, f2, a, b);
}

static void trace_clear(const float *end, hulltrace_t *tr)
{
	tr->fraction = 1;
	copy3(tr->endpos, end);
	tr->normal[0] = tr->normal[1] = tr->normal[2] = 0;
	tr->startsolid = tr->allsolid = tr->hit = false;
}

/* the end of q1_trace (bspfile.c:1058-1070) once the hull and its offset are known */
HOT static void tree_trace(const htree_t *t, int head, const float *off, const float *start, const float *end,
                           hulltrace_t *tr)
{
	hwork_t w;
	hpt_t p1, p2;
	bool point = same3(start, end);
	w.t = t, w.head = head, w.tr = tr, w.allsolid = true, w.startsolid = false, w.depth = 0;
	for (int k = 0; k < 3; k++) {
		/* x - (+0) is x */
		p1.p[k] = fbits(off[k]) ? start[k] - off[k] : start[k];
		p2.p[k] = fbits(off[k]) ? end[k] - off[k] : end[k];
	}
	hpt_set(&p1);
	if (!point)
		hpt_set(&p2);
	tr->fraction = 1;
	copy3(tr->endpos, p2.p);
	if (t->wide)
		h_trace_w(&w, head, 0, 1, &p1, point ? &p1 : &p2);
	else
		h_trace_n(&w, head, 0, 1, &p1, point ? &p1 : &p2);
	tr->startsolid = w.startsolid;
	tr->allsolid = w.allsolid;
	if (w.allsolid) {
		tr->fraction = 0;
		copy3(tr->endpos, p1.p);
	}
	tr->hit = fx_lt(tr->fraction, 1) || w.startsolid;
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

HOT static int fx_contents(const fxwork_t *w, int num, const int32_t *p)
{
	int guard = 0;
	while (num >= 0 && guard++ < 65536) {
		const int *nd = fx_node(w, num);
		num = nd[1 + (fx_dist(w, nd[0], p) < 0)];
	}
	return num < 0 ? fx_leaf(w, num) : num; /* as hull_contents: a node number is not solid */
}

HOT static bool fx_trace(fxwork_t *w, int num, float f1, float f2, const float *p1, const float *p2, const int32_t *q1,
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
	frac = fx_lt(frac, 1) ? frac : 1;
	frac = fx_gt(0, frac) ? 0 : frac;
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
		if (fx_lt(frac, 0)) {
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
	w.nodes = (const uint8_t *)nodes, w.planes = (const uint8_t *)planes, w.leafs = (const uint8_t *)leafcontents;
	w.ns = nodestride, w.ps = planestride, w.ls = leafstride, w.head = headnode;
	w.tr = tr, w.allsolid = true, w.startsolid = false, w.depth = 0;
	for (int k = 0; k < 3; k++) {
		off[k] = plus0(origin[k]); /* (hm - mins) + origin, both 0 for hull 0 */
		p1[k] = fbits(off[k]) ? start[k] - off[k] : start[k];
		p2[k] = fbits(off[k]) ? end[k] - off[k] : end[k];
		q1[k] = fx_pos(p1[k]);
		q2[k] = fx_pos(p2[k]);
	}
	memcpy(tr->endpos, p2, sizeof(p2));
	if (q1[0] == q2[0] && q1[1] == q2[1] && q1[2] == q2[2] && same3(p1, p2))
		fx_trace(&w, headnode, 0, 1, p1, p1, q1, q1); /* a point: one test a node */
	else
		fx_trace(&w, headnode, 0, 1, p1, p2, q1, q2);
	tr->startsolid = w.startsolid;
	tr->allsolid = w.allsolid;
	if (w.allsolid) {
		tr->fraction = 0;
		memcpy(tr->endpos, p1, sizeof(p1));
	}
	tr->hit = fx_lt(tr->fraction, 1) || w.startsolid;
	for (int k = 0; k < 3; k++)
		tr->endpos[k] = fbits(off[k]) ? tr->endpos[k] + off[k] : plus0(tr->endpos[k]);
}

/* --- models ------------------------------------------------------------------------------- */

static const float q1_hull1[3] = {-16, -16, -24};
static const float hl_hull1[3] = {-16, -16, -36}, hl_hull3[3] = {-16, -16, -18};

/* the hull a box uses and its offset from the box (hm - mins), worked out once per box: the
 * player's box changes only when it crouches */
static struct {
	uint32_t key[6]; /* mins, maxs bits */
	int rule, hull, slot;
	const float *hm;
	bool same[3];    /* hm[k] and mins[k] the same bits: the offset is the origin's */
} hsel;

static void select_hull(const float *mins, const float *maxs)
{
	uint32_t key[6];
	float size;
	for (int k = 0; k < 3; k++)
		key[k] = fbits(mins[k]), key[3 + k] = fbits(maxs[k]);
	if (hsel.rule == H.rule && key[0] == hsel.key[0] && key[1] == hsel.key[1] && key[2] == hsel.key[2] &&
	    key[3] == hsel.key[3] && key[4] == hsel.key[4] && key[5] == hsel.key[5])
		return;
	for (int k = 0; k < 6; k++)
		hsel.key[k] = key[k];
	hsel.rule = H.rule;
	size = maxs[0] - mins[0];
	if (H.rule == HULL_HL)
		hsel.hull = size <= 8 ? 0 : size <= 36 ? (maxs[2] - mins[2] <= 36 ? 3 : 1) : 2;
	else
		hsel.hull = size < 3 ? 0 : size <= 32 ? 1 : 2;
	/* hull 2 (wider than the player) is not kept: the player's box is 32 wide */
	hsel.slot = hsel.hull == 3 ? 1 : 0;
	hsel.hm = H.rule == HULL_HL ? (hsel.hull == 3 ? hl_hull3 : hl_hull1) : q1_hull1;
	for (int k = 0; k < 3; k++)
		hsel.same[k] = fbits(hsel.hm[k]) == fbits(mins[k]);
}

void Hull_TraceModel(int model, const float origin[3], const float start[3], const float mins[3],
                     const float maxs[3], const float end[3], hulltrace_t *tr)
{
	float off[3];
	trace_clear(end, tr);
	if (H.rule == HULL_NONE || model < 0 || model >= H.nummodels)
		return;
	select_hull(mins, maxs);
	if (hsel.hull == 0) {
		const hull0_if *h = H.h0 ? H.h0 : H.own0.trace ? &H.own0 : NULL;
		if (h)
			h->trace(h->ctx, model, origin, start, end, tr);
		return;
	}
	for (int k = 0; k < 3; k++)
		off[k] = hsel.same[k] ? plus0(origin[k]) : hsel.hm[k] - mins[k] + origin[k];
	tree_trace(&H.t, H.heads[model * H.numhulls + hsel.slot], off, start, end, tr);
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
void Hull_SetHull0(const hull0_if *h) { H.h0 = h, scene_gen++; }

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
	free(H.nodes0), free(H.general0), free(H.heads0), free(H.gfix), free(H.gfix0);
	free(H.fxnodes), free(H.fxplanes), free(H.fxleafs);
	free(tcache);
	tcache = NULL;
	memset(&H, 0, sizeof(H));
	memset(&hsel, 0, sizeof(hsel));
	free(brushes);
	brushes = NULL;
	numbrushes = brushcap = numboxes = numcandb = numcandx = numtouches = 0;
	focused = false;
	drop_mask = 0, scene_gen++;
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

/* every node a kept head reaches, every plane they use, and the heads, in range; and each
 * child after its node (BSP compilers write clipnodes depth first and hlmap.py keeps their
 * order: all 103 Half-Life maps' do), so no walk can go round a loop and a .hul written
 * wrong can't hang the trace */
static bool check_hulls(void)
{
	for (int i = 0; i < H.numnodes; i++) {
		uint32_t pl = H_PLANE(H.t.wide, &H.t, i), type = H.t.wide ? pl >> 30 : pl >> 14;
		uint32_t idx = H.t.wide ? pl & 0x3fffffffu : pl & 0x3fffu;
		if (idx >= (uint32_t)(type < 3 ? H.numaxial : H.numgeneral))
			return false;
		for (int s = 0; s < 2; s++) {
			int c = H_CHILD(H.t.wide, &H.t, i, s);
			if (c >= H.numnodes || (c >= 0 && c <= i))
				return false;
		}
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
		H.numgeneral0 = np;
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

/* the general planes in fixed point, if that leaves the reserve free (the walk works them
 * out on each test otherwise) */
static int32_t (*make_gfix(htree_t *t, int n, size_t reserve))[4]
{
	int32_t (*f)[4] = n > 0 ? malloc(16 * (size_t)n) : NULL;
	void *probe = f && reserve ? malloc(reserve) : NULL;
	if (f && reserve && !probe) {
		free(f);
		return NULL;
	}
	free(probe);
	for (int i = 0; f && i < n; i++)
		h_gfix(t->general[i], f[i]);
	t->gfix = (const int32_t (*)[4])f;
	return f;
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
	H.gfix = make_gfix(&H.t, H.numgeneral, reserve);
	if (H.t0.nodes)
		H.gfix0 = make_gfix(&H.t0, H.numgeneral0, reserve);
	if (!tcache) /* gen 0: nothing kept (scene_gen starts at 1); without it every trace is worked out */
		tcache = calloc(TC_N, sizeof(*tcache));
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

/* sv_world.c brush_frame (:120-131) with common.c AngleVectors (:320-341), in the same float
 * operations: sinf and cosf are glibc's (pmove_nds.c's port), as on the server */
static void brush_axes(const float *ang, float axis[3][3])
{
	const float d2r = (float)(3.14159265358979323846 / 180.0);
	float sy, cy, sp, cp, sr, cr;
	PM_SinCos(ang[1] * d2r, &sy, &cy);
	PM_SinCos(ang[0] * d2r, &sp, &cp);
	PM_SinCos(ang[2] * d2r, &sr, &cr);
	axis[0][0] = cp * cy;
	axis[0][1] = cp * sy;
	axis[0][2] = -sp;
	axis[1][0] = -(-sr * sp * cy + cr * sy); /* left: the negated right */
	axis[1][1] = -(-sr * sp * sy - cr * cy);
	axis[1][2] = -(-sr * cp);
	axis[2][0] = cr * sp * cy + sr * sy;
	axis[2][1] = cr * sp * sy - sr * cy;
	axis[2][2] = cr * cp;
}

/* a brush at origin and angles: its absmin/absmax (SV_LinkEdict, sv_world.c:50-75: a turned
 * SOLID_BSP one reaches its farthest corner's distance every way), and the same in 1/8 units */
static void place_brush(sbrush_t *s, const float origin[3], const float angles[3])
{
	float mins[3], maxs[3], radius = 0;
	memcpy(s->b.origin, origin, sizeof(s->b.origin));
	memcpy(s->b.angles, angles, sizeof(s->b.angles));
	Hull_ModelBounds(s->b.model, mins, maxs);
	s->turned = fx_nonzero(angles[0]) || fx_nonzero(angles[1]) || fx_nonzero(angles[2]);
	if (s->turned) {
		brush_axes(angles, s->axis);
		if (s->b.solid == PM_SOLID_BSP) {
			for (int k = 0; k < 3; k++) {
				float c = fabsf(mins[k]) > fabsf(maxs[k]) ? fabsf(mins[k]) : fabsf(maxs[k]);
				radius += c * c;
			}
			radius = sqrtf(radius);
		}
	}
	for (int k = 0; k < 3; k++) {
		s->amin[k] = origin[k] + (fx_gt(radius, 0) ? -radius : mins[k]) - 1;
		s->amax[k] = origin[k] + (fx_gt(radius, 0) ? radius : maxs[k]) + 1;
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
		const hullbrush_t *o = &brushes[i].b;
		if (i >= numbrushes || o->edict != b[i].edict || o->model != b[i].model || o->solid != b[i].solid ||
		    o->skin != b[i].skin || !same3(o->origin, b[i].origin) || !same3(o->angles, b[i].angles)) {
			brushes[i].b = b[i];
			place_brush(&brushes[i], b[i].origin, b[i].angles);
			scene_gen++;
		}
	}
	if (numbrushes != n)
		scene_gen++;
	numbrushes = n;
	unfocus();
}

void Hull_PlaceBrush(int i, const float origin[3], const float angles[3])
{
	if (i < 0 || i >= numbrushes)
		return;
	if (!same3(brushes[i].b.origin, origin) || !same3(brushes[i].b.angles, angles)) {
		place_brush(&brushes[i], origin, angles);
		scene_gen++;
	}
	unfocus();
}

void Hull_SetTouches(const pmtouch_t *t, int n)
{
	numtouches = n < HULL_MAX_TOUCH ? n : HULL_MAX_TOUCH;
	memcpy(touches, t, sizeof(*t) * (size_t)numtouches);
}

const pmtouch_t *Hull_TouchOf(void *ctx, int ent)
{
	(void)ctx;
	for (int i = 0; i < numtouches; i++)
		if (touches[i].edict == ent)
			return touches[i].kind == PM_TOUCH_CONVEYOR ? &touches[i] : NULL;
	return NULL;
}

/* SV_TouchTriggers (sv_world.c:493-525): a trigger whose absmin..absmax meets the player's */
int Hull_Triggers(void *ctx, const float absmin[3], const float absmax[3], const pmtouch_t **out, int max)
{
	int n = 0;
	(void)ctx;
	for (int i = 0; i < numtouches && n < max; i++) {
		const pmtouch_t *t = &touches[i];
		if (t->kind != PM_TOUCH_PUSH || t->absmin[0] > absmax[0] || t->absmin[1] > absmax[1] ||
		    t->absmin[2] > absmax[2] || t->absmax[0] < absmin[0] || t->absmax[1] < absmin[1] ||
		    t->absmax[2] < absmin[2])
			continue;
		out[n++] = t;
	}
	return n;
}

void Hull_BrushBox(int i, const float absmin[3], const float absmax[3])
{
	sbrush_t *s;
	if (i < 0 || i >= numbrushes)
		return;
	s = &brushes[i];
	if (same3(s->amin, absmin) && same3(s->amax, absmax))
		return;
	for (int k = 0; k < 3; k++) {
		s->amin[k] = absmin[k];
		s->amax[k] = absmax[k];
		s->ib[k] = fx8_down(s->amin[k]);
		s->ib[3 + k] = fx8_up(s->amax[k]);
	}
	scene_gen++;
	unfocus();
}

void Hull_SetBoxes(const hullbox_t *b, int n)
{
	n = n < 0 ? 0 : n > HULL_MAX_BOXES ? HULL_MAX_BOXES : n;
	for (int i = 0; i < n; i++) {
		sbox_t *s = &boxes[i];
		if (i < numboxes && s->b.edict == b[i].edict && s->b.solid == b[i].solid && same3(s->b.mins, b[i].mins) &&
		    same3(s->b.maxs, b[i].maxs))
			continue;
		scene_gen++;
		s->b = b[i];
		for (int k = 0; k < 3; k++) {
			s->amin[k] = b[i].mins[k] - 1;
			s->amax[k] = b[i].maxs[k] + 1;
			s->ib[k] = fx8_down(s->amin[k]);
			s->ib[3 + k] = fx8_up(s->amax[k]);
		}
	}
	if (numboxes != n)
		scene_gen++;
	numboxes = n;
	pin_mask = 0;
	unfocus();
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

static uint32_t skin_bits(int skin);

/* the boxes the server's player overlaps where the server has it: there the server's own
 * traces start solid in them (trace_aabb, sv_world.c:283-292) and the player stays put, so
 * PM_FOCUS_DROPSTUCK leaves them in (on the Half-Life campaign: a player spawned inside
 * another body, c2a3e; the bot walking into a scientist, c1a1b) */
void Hull_PinBoxes(const float org[3], const float mins[3], const float maxs[3])
{
	pin_mask = 0;
	for (int i = 0; i < numboxes; i++)
		if (box_overlaps(org, mins, maxs, boxes[i].b.mins, boxes[i].b.maxs))
			pin_mask |= (uint64_t)1 << i;
	unfocus();
}

uint32_t Hull_Focus(void *ctx, const float org[3], const float mins[3], const float maxs[3],
                    float radius, int flags)
{
	int32_t r[6];
	uint32_t near = 0;
	uint64_t drop = 0;
	for (int k = 0; k < 3; k++) {
		r[k] = fx8_down(org[k] - radius);
		r[3 + k] = fx8_up(org[k] + radius);
	}
	numcandb = numcandx = 0;
	for (int i = 0; i < numbrushes; i++)
		if (ib_meets(brushes[i].ib, r)) {
			candb[numcandb++] = (uint16_t)i;
			near |= skin_bits(brushes[i].b.skin);
		}
	for (int i = 0; i < numboxes; i++) {
		if (!ib_meets(boxes[i].ib, r))
			continue;
		if ((flags & PM_FOCUS_DROPSTUCK) && !(pin_mask >> i & 1) &&
		    box_overlaps(org, mins, maxs, boxes[i].b.mins, boxes[i].b.maxs)) {
			drop |= (uint64_t)1 << i;
			continue;
		}
		candx[numcandx++] = (uint16_t)i;
	}
	/* a focus without PM_FOCUS_DROPSTUCK (the weapon's traces) between two with it
	 * changes this back and forth: it is part of Hull_Move's key, not of scene_gen,
	 * so that doesn't empty the trace cache */
	drop_mask = drop;
	focused = true;
	return near;
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
		if (tr->ent < 0 || fx_lt(bt->fraction, tr->fraction)) {
			set_ent(tr, who, ent, solid, amin, amax);
			tr->contents = contents;
		}
	}
	if (fx_lt(bt->fraction, tr->fraction)) {
		tr->fraction = bt->fraction;
		memcpy(tr->normal, bt->normal, sizeof(tr->normal));
		set_ent(tr, who, ent, solid, amin, amax);
		tr->contents = contents;
	}
}

/* sv_world.c trace_aabb (:242-300); sign * x is x or its negation, exactly */
static void trace_aabb(const float *start, const float *mins, const float *maxs, const float *end, const float *bmin,
                       const float *bmax, pmhit_t *tr, who_t *who, const sbox_t *box, uint32_t contents, bool trigger)
{
	float enter = -1, leave = 1, n[3] = {0, 0, 0};
	bool startout = false;
	for (int i = 0; i < 3; i++) {
		for (int side = 0; side < 2; side++) {
			float dist = side == 0 ? bmax[i] - mins[i] : -(bmin[i] - maxs[i]);
			float d1 = (side == 0 ? start[i] : -start[i]) - dist, d2 = (side == 0 ? end[i] : -end[i]) - dist;
			bool out1 = fx_gt(d1, 0);
			if (out1)
				startout = true;
			if (out1 && (fx_ge(d2, 0.03125f) || fx_ge(d2, d1)))
				return;
			if (fx_le(d1, 0) && fx_le(d2, 0))
				continue;
			if (fx_gt(d1, d2)) {
				float f = (d1 - 0.03125f) / (d1 - d2);
				if (fx_gt(f, enter)) {
					enter = f;
					n[0] = n[1] = n[2] = 0;
					n[i] = side == 0 ? 1.0f : -1.0f;
				}
			} else {
				float f = (d1 + 0.03125f) / (d1 - d2);
				if (fx_lt(f, leave))
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
	if (fx_lt(enter, leave) && fx_gt(enter, -1) && fx_lt(enter, tr->fraction)) {
		tr->fraction = fx_lt(enter, 0) ? 0 : enter;
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
	if (s->turned) {
		/* sv_world.c trace_brush_entity (:157-176): start and end into the model's frame
		 * (frame_to_local), the hit's normal back into the world (frame_dir_to_world) */
		static const float zero[3] = {0, 0, 0};
		float ls[3], le[3], n[3], d[3], e[3];
		for (int k = 0; k < 3; k++)
			d[k] = start[k] - s->b.origin[k], e[k] = end[k] - s->b.origin[k];
		for (int k = 0; k < 3; k++) {
			ls[k] = d[0] * s->axis[k][0] + d[1] * s->axis[k][1] + d[2] * s->axis[k][2];
			le[k] = e[0] * s->axis[k][0] + e[1] * s->axis[k][1] + e[2] * s->axis[k][2];
		}
		Hull_TraceModel(s->b.model, zero, ls, mins, maxs, le, &bt);
		for (int k = 0; k < 3; k++)
			n[k] = bt.normal[0] * s->axis[0][k] + bt.normal[1] * s->axis[1][k] + bt.normal[2] * s->axis[2][k];
		memcpy(bt.normal, n, sizeof(n));
		for (int k = 0; k < 3; k++)
			bt.endpos[k] = start[k] + (end[k] - start[k]) * bt.fraction;
	} else
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

static HOT void hull_move(const float start[3], const float mins[3], const float maxs[3], const float end[3],
                          int type, int32_t mask, pmhit_t *tr)
{
	static const float zero[3] = {0, 0, 0};
	float tmin[3], tmax[3];
	hulltrace_t bt;
	who_t who = {PM_SOLID_BSP, zero, zero};
	uint32_t m = mask ? (uint32_t)mask : PM_CB_SOLID | PM_CB_BODY | PM_CB_PLAYERCLIP;
	int nb = focused ? numcandb : numbrushes, nx = focused ? numcandx : numboxes, ib = 0, ix = 0;
	bool have_t = false;
	tr->fraction = 1;
	tr->normal[0] = tr->normal[1] = tr->normal[2] = 0;
	tr->startsolid = tr->allsolid = false;
	tr->contents = 0;
	tr->ent = -1;
	if (type & PM_MOVE_EVERYTHING)
		m = 0xffffffffu;

	Hull_TraceModel(0, zero, start, mins, maxs, end, &bt);
	merge_bsp(tr, &who, &bt, bt.hit ? PM_CB_SOLID : 0, 0, PM_SOLID_BSP, zero, zero);

	/* brushes and boxes as one list in edict order, until something leaves fraction 0; the
	 * box around the move only once there is one to cull */
	while ((ib < nb || ix < nx) && fx_gt(tr->fraction, 0)) {
		const sbrush_t *b = ib < nb ? &brushes[focused ? candb[ib] : ib] : NULL;
		const sbox_t *x = ix < nx ? &boxes[focused ? candx[ix] : ix] : NULL;
		if (!have_t) {
			for (int i = 0; i < 3; i++) {
				tmin[i] = (fx_lt(start[i], end[i]) ? start[i] : end[i]) + mins[i] - 1;
				tmax[i] = (fx_gt(start[i], end[i]) ? start[i] : end[i]) + maxs[i] + 1;
			}
			have_t = true;
		}
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
	/* start + (end - start) * fraction; an axis that doesn't move, or fraction 0, needs no
	 * float work: start + (+-0), the zero's sign that of end - start (fraction is +0) */
	{
		uint32_t fb = fbits(tr->fraction);
		for (int i = 0; i < 3; i++) {
			if (fbits(start[i]) == fbits(end[i]))
				tr->endpos[i] = plus0(start[i]);
			else if (fb == 0)
				tr->endpos[i] = fx_lt(end[i], start[i]) ? start[i] : plus0(start[i]);
			else
				tr->endpos[i] = start[i] + (end[i] - start[i]) * tr->fraction;
		}
	}
	if (tr->ent <= 0) {
		tr->ent = 0;
		who.solid = PM_SOLID_BSP, who.amin = who.amax = zero;
	}
	tr->entsolid = who.solid;
	memcpy(tr->entmins, who.amin, sizeof(tr->entmins));
	memcpy(tr->entmaxs, who.amax, sizeof(tr->entmaxs));
}

/* The same moves come again: from one command to the next while standing or sliding along
 * the same wall, and when a replay from a server state that matched the prediction asks
 * what the first run asked (27% / 40% / 59% of the traces repeat one of the last 64 in the
 * coop_arena / e0m1 / dev replays of pmtest). The last results are kept, keyed on every
 * argument's bits and on scene_gen, which changes with anything else the result reads: a
 * result found here is the one hull_move gave for the same question about the same world. */
#ifdef HULL_SELFCHECK
long tc_checked;
#endif

HOT void Hull_Move(void *ctx, const float start[3], const float mins[3], const float maxs[3],
               const float end[3], int type, int32_t mask, pmhit_t *tr)
{
	uint32_t key[14], h = 0;
	int i;
	(void)ctx;
	for (i = 0; i < 3; i++) {
		key[i] = fbits(start[i]), key[3 + i] = fbits(mins[i]);
		key[6 + i] = fbits(maxs[i]), key[9 + i] = fbits(end[i]);
	}
	key[12] = (uint32_t)type, key[13] = (uint32_t)mask;
	for (i = 0; i < 14; i++)
		h = (h ^ key[i]) * 0x9e3779b1u;
	h >>= 27; /* TC_N 32 */
	if (tcache && tcache[h].gen == scene_gen && tcache[h].drop == drop_mask) {
		for (i = 0; i < 14 && tcache[h].key[i] == key[i]; i++)
			;
		if (i == 14) {
			*tr = tcache[h].tr;
#ifdef HULL_SELFCHECK /* the kept result must be what the trace gives now */
			{
				pmhit_t f;
				hull_move(start, mins, maxs, end, type, mask, &f);
				if (fbits(f.fraction) != fbits(tr->fraction) || !same3(f.endpos, tr->endpos) ||
				    !same3(f.normal, tr->normal) || f.startsolid != tr->startsolid || f.allsolid != tr->allsolid ||
				    f.contents != tr->contents || f.ent != tr->ent || f.entsolid != tr->entsolid ||
				    !same3(f.entmins, tr->entmins) || !same3(f.entmaxs, tr->entmaxs))
					abort();
				tc_checked++;
			}
#endif
			return;
		}
	}
	hull_move(start, mins, maxs, end, type, mask, tr);
	if (!tcache)
		return;
	tcache[h].gen = scene_gen, tcache[h].drop = drop_mask;
	for (i = 0; i < 14; i++)
		tcache[h].key[i] = key[i];
	tcache[h].tr = *tr;
}
