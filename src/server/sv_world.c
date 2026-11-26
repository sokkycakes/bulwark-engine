/* Bulwark engine - server world: entity links, traces and contents.
 * Entities are found through a small area tree (below) rather than by
 * testing every slot.
 * ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/voxel.h"

#include <math.h>
#include <string.h>

/* The same code serves the client (CSQC traces and prediction) by pointing
 * W at the client's world; see SV_SetWorldContext. */
static bool sv_brushmodel(int mi, bspmap_t **bsp, int *sub)
{
	if (mi <= 0 || mi >= sv.nummodels || !sv.modelinfo[mi].bsp)
		return false;
	*bsp = sv.modelinfo[mi].bsp;
	*sub = sv.modelinfo[mi].submodel;
	return true;
}

static worldctx_t sv_worldctx = {NULL, NULL, sv_brushmodel, NULL, NULL};
static worldctx_t *W = &sv_worldctx;
static int last_voxelcell[4] = {0, 0, 0, -1};

const int *SV_LastVoxelCell(void) { return last_voxelcell; }

/* Every function other modules call starts with enter(), which brings the
 * server's context up to date; the rest only read W through cur(), which
 * the many field lookups (F, G, evec) make worth keeping to one load. */
static worldctx_t *enter(void)
{
	if (W == &sv_worldctx) {
		sv_worldctx.vm = sv.vm;
		sv_worldctx.world = sv.world;
		sv_worldctx.voxel = sv.voxel;
	}
	return W;
}

static inline worldctx_t *cur(void) { return W; }

bspmap_t *SV_CurrentWorld(void) { return enter()->world; }

worldctx_t *SV_SetWorldContext(worldctx_t *w)
{
	worldctx_t *old = W;
	W = w ? w : &sv_worldctx;
	return old == &sv_worldctx ? NULL : old;
}

#define F cur()->vm->fofs
#define G cur()->vm->gofs

static inline float *evec(int e, int ofs) { return ofs < 0 ? NULL : &QC_EdictFields(cur()->vm, e)[ofs].f; }

/* --- area tree ------------------------------------------------------------
 * Every entity slot is filed under the deepest node of a fixed tree of boxes
 * (the world's bounds halved along x or y, TREE_DEPTH times) that holds its
 * absmin..absmax whole. A trace, point or touch then looks only at the
 * nodes its own box reaches. SV_LinkEdict is what writes absmin and absmax,
 * so it refiles the entity; a check of every slot once per server frame and
 * per host frame catches the rest (slots cleared or reused, boxes restored
 * by a save or written by QC). Each candidate still gets every test it got
 * when all slots were scanned, in slot order, so results are unchanged.
 *
 * Coordinates are kept as keys that order as the floats do, so the tests
 * are integer compares (float compares are slow where SSE is emulated, as
 * in xemu). A NaN, which fails every float compare, becomes the key that
 * fails the one compare made on that side of a box, so it overlaps
 * everything there as before. */
#define TREE_DEPTH 5
#define TREE_NODES ((2 << TREE_DEPTH) - 1)

/* a float's bits; a union, as nxdk builds without builtin memcpy */
static inline uint32_t fbits(float f)
{
	union {
		float f;
		uint32_t u;
	} v;
	v.f = f;
	return v.u;
}

static inline uint32_t fkey(float f)
{
	uint32_t b = fbits(f), m;
	m = (uint32_t)((int32_t)b >> 31) | 0x80000000u;
	return (b ^ m) + (b == 0x80000000u); /* negatives flipped, and -0 == +0 */
}

static inline bool fnan(float f) { return f != f; }

/* a box's low side: a NaN there is never above anything */
static inline uint32_t fkey_lo(float f) { return fnan(f) ? 0 : fkey(f); }
/* a box's high side: a NaN there is never below anything */
static inline uint32_t fkey_hi(float f) { return fnan(f) ? 0xffffffffu : fkey(f); }

typedef struct {
	int axis; /* -1 for a leaf */
	float dist;
	uint32_t kdist;
	int children[2]; /* [0] above dist, [1] below */
	int head; /* first entity filed here, -1 for none */
} boxnode_t;

/* a slot's filed box and its neighbours in its node, together so a walk
 * down a node reads one 32-byte line per entity */
typedef struct {
	uint32_t key[6]; /* fkey_lo of the absmin and fkey_hi of the absmax it was filed with */
	int next, prev;
} areaslot_t;

typedef struct area_s {
	qcvm_t *vm;
	bspmap_t *world;
	int max_edicts;
	float wmins[3], wmaxs[3];
	double synced_time;
	int synced_frame;
	int numnodes;
	boxnode_t nodes[TREE_NODES];
	int *node; /* per slot; -1 until filed */
	areaslot_t *slot;
	qcval_t (*filed)[6]; /* per slot, the absmin and absmax it was filed with */
	uint32_t *bits; /* one per slot, all clear between queries */
	int *scratch; /* candidate lists; nested queries stack up in it */
	int scratch_top, scratch_size;
} area_t;

static int area_build(area_t *a, int depth, const float *mins, const float *maxs)
{
	int n = a->numnodes++;
	boxnode_t *an = &a->nodes[n];
	an->head = -1;
	if (depth == TREE_DEPTH) {
		an->axis = -1;
		an->children[0] = an->children[1] = -1;
		return n;
	}
	an->axis = maxs[0] - mins[0] >= maxs[1] - mins[1] ? 0 : 1;
	an->dist = 0.5f * (mins[an->axis] + maxs[an->axis]);
	an->kdist = fkey(an->dist);
	if (fnan(an->dist)) {
		/* nothing is ever filed or looked for below a NaN split */
		an->axis = -1;
		an->children[0] = an->children[1] = -1;
		return n;
	}
	{
		float lo[3], hi[3];
		VectorCopy(mins, lo);
		VectorCopy(maxs, hi);
		lo[an->axis] = an->dist;
		an->children[0] = area_build(a, depth + 1, lo, maxs);
		hi[an->axis] = an->dist;
		an->children[1] = area_build(a, depth + 1, mins, hi);
	}
	return n;
}

static void area_unfile(area_t *a, int e)
{
	int n = a->node[e];
	if (n < 0)
		return;
	areaslot_t *s = &a->slot[e];
	if (s->prev >= 0)
		a->slot[s->prev].next = s->next;
	else
		a->nodes[n].head = s->next;
	if (s->next >= 0)
		a->slot[s->next].prev = s->prev;
	a->node[e] = -1;
}

/* Each node's list runs in slot order, so a query's hits come in a few
 * ascending runs that sort cheaply. */
static void area_file_keys(area_t *a, int e, const uint32_t *key)
{
	int n = 0, p, q;
	for (;;) {
		const boxnode_t *an = &a->nodes[n];
		if (an->axis < 0)
			break;
		if (key[an->axis] > an->kdist)
			n = an->children[0];
		else if (key[3 + an->axis] < an->kdist)
			n = an->children[1];
		else
			break; /* straddles the split (or NaN): stays here */
	}
	areaslot_t *s = &a->slot[e];
	if (a->node[e] != n) {
		area_unfile(a, e);
		for (p = -1, q = a->nodes[n].head; q >= 0 && q < e; q = a->slot[q].next)
			p = q;
		a->node[e] = n;
		s->prev = p;
		s->next = q;
		if (q >= 0)
			a->slot[q].prev = e;
		if (p >= 0)
			a->slot[p].next = e;
		else
			a->nodes[n].head = e;
	}
	for (int i = 0; i < 6; i++)
		s->key[i] = key[i];
}

static void box_keys(const float *amin, const float *amax, uint32_t *key)
{
	for (int i = 0; i < 3; i++) {
		key[i] = fkey_lo(amin[i]);
		key[3 + i] = fkey_hi(amax[i]);
	}
}

static void area_file(area_t *a, int e, const float *amin, const float *amax)
{
	uint32_t key[6];
	box_keys(amin, amax, key);
	area_file_keys(a, e, key);
	for (int i = 0; i < 3; i++) {
		a->filed[e][i].u = fbits(amin[i]);
		a->filed[e][3 + i].u = fbits(amax[i]);
	}
}

static void area_sync(area_t *a)
{
	qcvm_t *vm = a->vm;
	int fmin = vm->fofs.absmin, fmax = vm->fofs.absmax;
	for (int e = 1; e < vm->num_edicts; e++) {
		const qcval_t *f = QC_EdictFields(vm, e);
		const qcval_t *b = a->filed[e];
		/* bit for bit, so a NaN that stays put counts as unchanged */
		if (a->node[e] >= 0 && b[0].i == f[fmin].i && b[1].i == f[fmin + 1].i && b[2].i == f[fmin + 2].i &&
		    b[3].i == f[fmax].i && b[4].i == f[fmax + 1].i && b[5].i == f[fmax + 2].i)
			continue;
		area_file(a, e, &f[fmin].f, &f[fmax].f);
	}
}

static inline bool same_bits(const float *x, const float *y)
{
	return fbits(x[0]) == fbits(y[0]) && fbits(x[1]) == fbits(y[1]) && fbits(x[2]) == fbits(y[2]);
}

/* the current context's tree, made or resynced as needed; NULL when the
 * progs have no absmin/absmax, which leaves every slot to be scanned */
static area_t *area_get(void)
{
	worldctx_t *w = cur();
	qcvm_t *vm = w->vm;
	area_t *a = w->area;
	float wmins[3] = {-4096, -4096, -4096}, wmaxs[3] = {4096, 4096, 4096};
	int frame = w == &sv_worldctx ? sv.frames : 0;
	if (!vm || vm->fofs.absmin < 0 || vm->fofs.absmax < 0)
		return NULL;
	if (w->world && w->world->nummodels > 0) {
		VectorCopy(w->world->models[0].mins, wmins);
		VectorCopy(w->world->models[0].maxs, wmaxs);
	}
	if (!a || a->vm != vm || a->world != w->world || a->max_edicts != vm->max_edicts ||
	    !same_bits(a->wmins, wmins) || !same_bits(a->wmaxs, wmaxs)) {
		if (a && a->scratch_top)
			return NULL; /* remade under a running query: scan this one */
		if (!a || a->max_edicts != vm->max_edicts) {
			if (a) {
				Mem_Free(a->node);
				Mem_Free(a->slot);
				Mem_Free(a->filed);
				Mem_Free(a->scratch);
				Mem_Free(a->bits);
				Mem_Free(a);
			}
			a = w->area = Mem_Alloc(sizeof(*a));
			a->max_edicts = vm->max_edicts;
			a->node = Mem_Alloc(sizeof(int) * (size_t)a->max_edicts);
			a->slot = Mem_Alloc(sizeof(*a->slot) * (size_t)a->max_edicts);
			a->filed = Mem_Alloc(sizeof(*a->filed) * (size_t)a->max_edicts);
			a->scratch_size = a->max_edicts * 4;
			a->scratch = Mem_Alloc(sizeof(int) * (size_t)a->scratch_size);
			a->bits = Mem_Alloc(sizeof(uint32_t) * (size_t)((a->max_edicts + 31) >> 5));
		}
		a->vm = vm;
		a->world = w->world;
		VectorCopy(wmins, a->wmins);
		VectorCopy(wmaxs, a->wmaxs);
		a->numnodes = 0;
		area_build(a, 0, wmins, wmaxs);
		for (int e = 0; e < a->max_edicts; e++)
			a->node[e] = -1;
		a->synced_time = -1;
	}
	if (a->synced_time != host_time || a->synced_frame != frame) {
		a->synced_time = host_time;
		a->synced_frame = frame;
		area_sync(a);
	}
	return a;
}

typedef struct {
	area_t *a; /* NULL: every slot */
	int *ids;
	int n;
} arealist_t;

/* The slots whose filed boxes may meet lo..hi, in slot order unless
 * !sorted (for callers where order cannot matter). With no tree (or no
 * room) it lists every slot, as the scan used to. Release with area_done,
 * innermost first. */
static void area_query(arealist_t *l, const float *lo, const float *hi, bool sorted)
{
	area_t *a = area_get();
	int stack[TREE_NODES], sp = 0, n = 0, room, *ids;
	uint32_t klo[3], khi[3];
	l->a = NULL;
	l->n = 0;
	l->ids = NULL;
	for (int i = 0; i < 3 && a; i++)
		if (lo[i] != lo[i] || hi[i] != hi[i])
			a = NULL; /* NaN boxes overlap everything */
	if (!a)
		return;
	/* any filed slot can be listed, even one past num_edicts that a reused
	   tree still holds, so room is counted against every slot */
	room = a->scratch_size - a->scratch_top;
	if (room < a->max_edicts)
		return;
	ids = a->scratch + a->scratch_top;
	for (int i = 0; i < 3; i++) {
		klo[i] = fkey(lo[i]);
		khi[i] = fkey(hi[i]);
	}
	stack[sp++] = 0;
	while (sp) {
		const boxnode_t *an = &a->nodes[stack[--sp]];
		for (int e = an->head; e >= 0;) {
			const areaslot_t *s = &a->slot[e];
			const uint32_t *k = s->key;
			if (!(k[0] > khi[0] || k[1] > khi[1] || k[2] > khi[2] || k[3] < klo[0] || k[4] < klo[1] || k[5] < klo[2]))
				ids[n++] = e;
			e = s->next;
		}
		if (an->axis < 0)
			continue;
		if (khi[an->axis] > an->kdist)
			stack[sp++] = an->children[0];
		if (klo[an->axis] < an->kdist)
			stack[sp++] = an->children[1];
	}
	if (n > 1 && sorted) {
		if (n <= 32) {
			for (int i = 1; i < n; i++) {
				int v = ids[i], j = i;
				while (j > 0 && ids[j - 1] > v) {
					ids[j] = ids[j - 1];
					j--;
				}
				ids[j] = v;
			}
		} else {
			/* long lists (a trace across the map) sort through a bitmap */
			int words = 0;
			for (int i = 0; i < n; i++) {
				a->bits[ids[i] >> 5] |= 1u << (ids[i] & 31);
				words = BW_MAX(words, (ids[i] >> 5) + 1);
			}
			n = 0;
			for (int w = 0; w < words; w++) {
				uint32_t b = a->bits[w];
				a->bits[w] = 0;
				while (b) {
					ids[n++] = (w << 5) + __builtin_ctz(b);
					b &= b - 1;
				}
			}
		}
	}
	a->scratch_top += n;
	l->a = a;
	l->ids = ids;
	l->n = n;
}

/* e's absmin and absmax are still the bits it was filed with, so the query
 * that listed it already made the same overlap test on them */
static inline bool still_filed(const area_t *a, int e, const float *amin, const float *amax)
{
	const qcval_t *f = a ? a->filed[e] : NULL;
	if (!f)
		return false;
	return !((f[0].u ^ fbits(amin[0])) | (f[1].u ^ fbits(amin[1])) | (f[2].u ^ fbits(amin[2])) |
	         (f[3].u ^ fbits(amax[0])) | (f[4].u ^ fbits(amax[1])) | (f[5].u ^ fbits(amax[2])));
}

static void area_done(arealist_t *l)
{
	if (l->a)
		l->a->scratch_top -= l->n;
}

/* the k'th slot of a list, or past the end: -1 */
static inline int area_at(const arealist_t *l, int k, int num_edicts)
{
	if (!l->a)
		return k + 1 < num_edicts ? k + 1 : -1;
	return k < l->n ? l->ids[k] : -1;
}

void SV_LinkEdict(int e, bool touch_triggers)
{
	enter();
	float *org = evec(e, F.origin), *mins = evec(e, F.mins), *maxs = evec(e, F.maxs);
	float *amin = evec(e, F.absmin), *amax = evec(e, F.absmax), *size = evec(e, F.size);
	if (e <= 0 || !org || !mins || !maxs)
		return;
	const float *ang = evec(e, F.angles);
	float radius = 0;
	/* a turned brush entity can reach anywhere within its farthest corner */
	if (ang && (ang[0] != 0 || ang[1] != 0 || ang[2] != 0) &&
	    (int)QC_EFloat(cur()->vm, e, F.solid) == SOLID_BSP) {
		for (int i = 0; i < 3; i++) {
			float c = BW_MAX(fabsf(mins[i]), fabsf(maxs[i]));
			radius += c * c;
		}
		radius = sqrtf(radius);
	}
	for (int i = 0; i < 3; i++) {
		if (size)
			size[i] = maxs[i] - mins[i];
		if (amin)
			amin[i] = org[i] + (radius > 0 ? -radius : mins[i]) - 1;
		if (amax)
			amax[i] = org[i] + (radius > 0 ? radius : maxs[i]) + 1;
	}
	if (amin && amax && cur()->area && cur()->area->vm == cur()->vm && e < cur()->area->max_edicts)
		area_file(cur()->area, e, amin, amax);
	if (touch_triggers && QC_EFloat(cur()->vm, e, F.solid) != SOLID_NOT)
		SV_TouchTriggers(e);
}

void SV_UnlinkEdict(int e) { (void)e; }

int SV_ContentsToQuake(int bits)
{
	if (bits & CONTENTS_SOLID)
		return CONTENT_SOLID;
	if (bits & CONTENTS_LAVA)
		return CONTENT_LAVA;
	if (bits & CONTENTS_SLIME)
		return CONTENT_SLIME;
	if (bits & CONTENTS_WATER)
		return CONTENT_WATER;
	if (bits & CONTENTS_SKY)
		return CONTENT_SKY;
	if (bits & CONTENTS_LADDER)
		return CONTENT_LADDER;
	return CONTENT_EMPTY;
}

typedef struct {
	bspmap_t *bsp;
	int submodel;
} brushref_t;

static bool ent_brush_model(int e, brushref_t *out)
{
	int mi = (int)QC_EFloat(cur()->vm, e, F.modelindex);
	if (!cur()->brushmodel(mi, &out->bsp, &out->submodel) || out->bsp->type == BSP_NONE)
		return false;
	return true;
}

/* A rotated brush entity is tested in its own frame: points are moved there
 * (relative to the origin, then onto the model's forward, left and up axes),
 * and hit normals are turned back into the world. The box being moved keeps
 * its world alignment, so against a turned model it is an approximation, but
 * a close one for the small boxes of players and monsters. */
typedef struct {
	bool rotated;
	float origin[3];
	float axis[3][3]; /* forward, left, up */
} brushframe_t;

static void brush_frame(int e, brushframe_t *f)
{
	const float *org = evec(e, F.origin), *ang = evec(e, F.angles);
	float right[3];
	VectorCopy(org, f->origin);
	f->rotated = ang && (ang[0] != 0 || ang[1] != 0 || ang[2] != 0);
	if (!f->rotated)
		return;
	AngleVectors(ang, f->axis[0], right, f->axis[2]);
	for (int i = 0; i < 3; i++)
		f->axis[1][i] = -right[i];
}

/* world point to the model's space */
static void frame_to_local(const brushframe_t *f, const float *p, float *out)
{
	float d[3];
	for (int i = 0; i < 3; i++)
		d[i] = p[i] - f->origin[i];
	if (!f->rotated) {
		VectorCopy(d, out);
		return;
	}
	for (int i = 0; i < 3; i++)
		out[i] = d[0] * f->axis[i][0] + d[1] * f->axis[i][1] + d[2] * f->axis[i][2];
}

/* direction in the model's space back to the world */
static void frame_dir_to_world(const brushframe_t *f, const float *v, float *out)
{
	if (!f->rotated) {
		VectorCopy(v, out);
		return;
	}
	for (int i = 0; i < 3; i++)
		out[i] = v[0] * f->axis[0][i] + v[1] * f->axis[1][i] + v[2] * f->axis[2][i];
}

static void trace_brush_entity(int e, const brushref_t *m, const float *start, const float *mins, const float *maxs,
                               const float *end, int mask, bsptrace_t *bt)
{
	float zero[3] = {0, 0, 0}, ls[3], le[3], n[3];
	brushframe_t f;
	brush_frame(e, &f);
	if (!f.rotated) {
		BSP_TraceModel(m->bsp, m->submodel, f.origin, start, mins, maxs, end, mask, bt);
		return;
	}
	frame_to_local(&f, start, ls);
	frame_to_local(&f, end, le);
	BSP_TraceModel(m->bsp, m->submodel, zero, ls, mins, maxs, le, mask, bt);
	frame_dir_to_world(&f, bt->normal, n);
	VectorCopy(n, bt->normal);
	for (int i = 0; i < 3; i++)
		bt->endpos[i] = start[i] + (end[i] - start[i]) * bt->fraction;
	bt->dist = n[0] * bt->endpos[0] + n[1] * bt->endpos[1] + n[2] * bt->endpos[2];
}

static int brush_entity_contents(int e, const brushref_t *m, const float *p)
{
	float zero[3] = {0, 0, 0}, lp[3];
	brushframe_t f;
	brush_frame(e, &f);
	frame_to_local(&f, p, lp);
	return BSP_PointContents(m->bsp, m->submodel, zero, lp);
}

/* A brush entity with a negative .skin takes that Quake content value for all
 * of its brushes, as in FTE: a func_ladder sets CONTENT_LADDER, which leaves
 * it climbable but not solid. Returns 0 for brush entities without one. */
static int skin_contents(int e)
{
	int skin = F.skin >= 0 ? (int)QC_EFloat(cur()->vm, e, F.skin) : 0;
	return skin < 0 ? BSP_QuakeContentsToBits(skin) : 0;
}

/* e's box, where e is now, overlaps the brushes of the brush entity pusher */
bool SV_InsideBrushEntity(int pusher, int e)
{
	enter();
	brushref_t m;
	bsptrace_t bt;
	float *org = evec(e, F.origin), *mins = evec(e, F.mins), *maxs = evec(e, F.maxs);
	if (!org || !mins || !maxs || !ent_brush_model(pusher, &m))
		return false;
	trace_brush_entity(pusher, &m, org, mins, maxs, org,
	                   CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP, &bt);
	return bt.startsolid;
}

static int world_contents(const float *p)
{
	int bits = cur()->world ? BSP_WorldContents(cur()->world, p) : 0;
	if (cur()->voxel)
		bits |= (int)Vox_PointContents(cur()->voxel, p);
	return bits;
}

/* brush entities such as func_water add their contents too. With boxtest
 * the list is a larger query's, and only the slots a query of p alone
 * would list are looked at, by the same test on their filed boxes. */
static int entity_contents(const arealist_t *l, const float *p, bool boxtest)
{
	int bits = 0;
	uint32_t kp[3];
	for (int i = 0; i < 3 && boxtest; i++)
		kp[i] = fkey(p[i]); /* p is no NaN: it lies in the query's box */
	for (int k = 0, e; (e = area_at(l, k, cur()->vm->num_edicts)) >= 0; k++) {
		brushref_t m;
		int solid;
		if (boxtest) {
			const uint32_t *b = l->a->slot[e].key;
			if (b[0] > kp[0] || b[1] > kp[1] || b[2] > kp[2] || b[3] < kp[0] || b[4] < kp[1] || b[5] < kp[2])
				continue;
		}
		if (e >= cur()->vm->num_edicts || cur()->vm->edicts[e].free)
			continue;
		solid = (int)QC_EFloat(cur()->vm, e, F.solid);
		if (solid != SOLID_BSP && solid != SOLID_BSPTRIGGER)
			continue;
		if (F.absmin >= 0) {
			const float *amin = evec(e, F.absmin), *amax = evec(e, F.absmax);
			if (p[0] < amin[0] || p[1] < amin[1] || p[2] < amin[2] ||
			    p[0] > amax[0] || p[1] > amax[1] || p[2] > amax[2])
				continue;
		}
		if (!ent_brush_model(e, &m))
			continue;
		{
			int c = brush_entity_contents(e, &m, p), skinbits = skin_contents(e);
			bits |= skinbits && c ? skinbits : c;
		}
	}
	return bits;
}

int SV_PointContentsBits(const float *p)
{
	enter();
	int bits = world_contents(p);
	arealist_t l;
	area_query(&l, p, p, false);
	bits |= entity_contents(&l, p, false);
	area_done(&l);
	return bits;
}

/* sweeps a box against an axis aligned box (another entity's bounds) */
static void trace_aabb(const float *start, const float *mins, const float *maxs, const float *end,
                       const float *bmin, const float *bmax, svtrace_t *tr, int ent, int contents, bool trigger)
{
	float enter = -1, leave = 1, n[3] = {0, 0, 0};
	bool startout = false;
	for (int i = 0; i < 3; i++) {
		for (int side = 0; side < 2; side++) {
			/* plane facing out of the box along +i (side 0) or -i (side 1) */
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
			/* starting inside a trigger volume is noted but doesn't stop
			   the trace: a +use line from inside a big trigger still has
			   to reach the button in front of it */
			tr->startsolid = true;
			if (tr->ent < 0)
				tr->ent = ent;
			return;
		}
		if (!tr->startsolid || tr->ent < 0) {
			tr->startsolid = tr->allsolid = true;
			tr->fraction = 0;
			tr->ent = ent;
			tr->contents = contents;
		}
		return;
	}
	if (enter < leave && enter > -1 && enter < tr->fraction) {
		tr->fraction = enter < 0 ? 0 : enter;
		VectorCopy(n, tr->normal);
		tr->dist = 0;
		tr->ent = ent;
		tr->contents = contents;
		tr->surfaceflags = 0;
		tr->surfacename = "";
		tr->brush = tr->brushside = -1;
	}
}

static void merge_bsp(svtrace_t *tr, const bsptrace_t *bt, const bspmap_t *map, int ent)
{
	if (bt->allsolid || bt->startsolid) {
		tr->startsolid = true;
		if (bt->allsolid)
			tr->allsolid = true;
		if (tr->ent < 0 || bt->fraction < tr->fraction) {
			tr->ent = ent;
			tr->contents = bt->contents;
		}
	}
	if (bt->fraction < tr->fraction) {
		tr->fraction = bt->fraction;
		VectorCopy(bt->normal, tr->normal);
		tr->dist = bt->dist;
		tr->ent = ent;
		tr->contents = bt->contents;
		tr->surfaceflags = bt->surfaceflags;
		tr->surfacename = (bt->shader >= 0 && bt->shader < map->numshaders) ? map->shaders[bt->shader].name : "";
		tr->brush = bt->brush;
		tr->brushside = bt->brushside;
	}
}

static int default_mask(int passent)
{
	int mask = passent > 0 ? QC_EInt(cur()->vm, passent, F.hitcontentsmaski) : 0;
	if (mask)
		return mask;
	mask = CONTENTS_SOLID | CONTENTS_BODY;
	if (passent > 0 && ((int)QC_EFloat(cur()->vm, passent, F.flags) & FL_CLIENT))
		mask |= CONTENTS_PLAYERCLIP;
	return mask;
}

static int ent_contents(int e, int solid)
{
	if (solid == SOLID_CORPSE)
		return CONTENTS_CORPSE;
	if (solid == SOLID_BBOX || solid == SOLID_SLIDEBOX)
		return CONTENTS_BODY;
	return CONTENTS_SOLID;
}

static void move(const float *start, const float *mins, const float *maxs, const float *end, int type, int passent,
                 svtrace_t *tr, bool water)
{
	float tmin[3], tmax[3];
	int mask = default_mask(passent);
	int passowner = passent > 0 ? QC_EInt(cur()->vm, passent, F.owner) : 0;
	int passhit = (passent > 0 && F.dimension_hit >= 0) ? (int)QC_EFloat(cur()->vm, passent, F.dimension_hit) : 255;
	bsptrace_t bt;

	memset(tr, 0, sizeof(*tr));
	tr->fraction = 1;
	tr->ent = -1;
	tr->surfacename = "";
	tr->brush = tr->brushside = -1;
	int onlyent = -1; /* MOVE_OTHERONLY: the one entity to test, 0 for the world */
	if (type & MOVE_EVERYTHING)
		mask = -1;
	if (type & MOVE_OTHERONLY) {
		/* fteextensions: collide against only the 'other' global's entity,
		   ignoring owner, SOLID_NOT and dimension rules but keeping
		   contents and bsp/box tests. other = world leaves only the map. */
		onlyent = G.other >= 0 ? QC_GInt(cur()->vm, G.other) : 0;
	}

	tr->voxelcell[3] = -1;
	if (cur()->world && onlyent <= 0) {
		BSP_TraceWorld(cur()->world, start, mins, maxs, end, mask, &bt);
		merge_bsp(tr, &bt, cur()->world, 0);
	}
	if (cur()->voxel && onlyent <= 0) {
		vxtrace_t vt;
		Vox_Trace(cur()->voxel, start, mins, maxs, end, (uint32_t)mask, &vt);
		if (vt.startsolid && !tr->startsolid) {
			tr->startsolid = true;
			tr->ent = 0;
			tr->contents = vt.contents;
		}
		if (vt.allsolid)
			tr->allsolid = true;
		if (vt.fraction < tr->fraction || (vt.startsolid && vt.fraction <= tr->fraction)) {
			tr->fraction = vt.fraction;
			VectorCopy(vt.normal, tr->normal);
			tr->dist = vt.dist;
			tr->ent = 0;
			tr->contents = vt.contents;
			tr->surfaceflags = 0;
			tr->surfacename = "";
			tr->brush = tr->brushside = -1;
			for (int i = 0; i < 3; i++)
				tr->voxelcell[i] = vt.cell[i];
			tr->voxelcell[3] = vt.type;
		}
	}

	for (int i = 0; i < 3; i++) {
		tmin[i] = BW_MIN(start[i], end[i]) + mins[i] - 1;
		tmax[i] = BW_MAX(start[i], end[i]) + maxs[i] + 1;
	}
	arealist_t l = {NULL, NULL, 0};
	if (onlyent < 0)
		area_query(&l, tmin, tmax, true);
	for (int k = 0, e; tr->fraction > 0 && (e = area_at(&l, k, cur()->vm->num_edicts)) >= 0; k++) {
		int solid, owner;
		float *amin, *amax;
		brushref_t m;
		if (onlyent >= 0 && e != onlyent)
			continue;
		if (e >= cur()->vm->num_edicts)
			continue;
		if (cur()->vm->edicts[e].free || (e == passent && onlyent < 0))
			continue;
		solid = (int)QC_EFloat(cur()->vm, e, F.solid);
		if (solid == SOLID_NOT && onlyent < 0)
			continue;
		if ((solid == SOLID_TRIGGER || solid == SOLID_BSPTRIGGER) && !(type & (MOVE_TRIGGERS | MOVE_EVERYTHING)))
			continue;
		if ((type & MOVE_NOMONSTERS) && solid != SOLID_BSP)
			continue;
		owner = QC_EInt(cur()->vm, e, F.owner);
		if (onlyent < 0 && passent > 0 && (owner == passent || (passowner && passowner == e)))
			continue;
		if (onlyent < 0 && F.dimension_solid >= 0 && !((int)QC_EFloat(cur()->vm, e, F.dimension_solid) & passhit))
			continue;
		amin = evec(e, F.absmin);
		amax = evec(e, F.absmax);
		if (amin && amax && !still_filed(l.a, e, amin, amax) &&
		    (amin[0] > tmax[0] || amin[1] > tmax[1] || amin[2] > tmax[2] || amax[0] < tmin[0] ||
		     amax[1] < tmin[1] || amax[2] < tmin[2]))
			continue;
		if ((solid == SOLID_BSP || solid == SOLID_BSPTRIGGER) && ent_brush_model(e, &m)) {
			int emask = (solid == SOLID_BSPTRIGGER) ? -1 : mask;
			int skinbits = skin_contents(e);
			if (skinbits) {
				/* every brush counts as the skin's contents */
				if (!(skinbits & mask) && solid != SOLID_BSPTRIGGER)
					continue;
				emask = -1;
			}
			trace_brush_entity(e, &m, start, mins, maxs, end, emask, &bt);
			if (skinbits)
				bt.contents = skinbits;
			if (solid == SOLID_BSPTRIGGER && bt.startsolid) {
				/* same as a box trigger: note it, keep tracing */
				tr->startsolid = true;
				if (tr->ent < 0)
					tr->ent = e;
				continue;
			}
			merge_bsp(tr, &bt, m.bsp, e);
		} else {
			float bmin[3], bmax[3], *org = evec(e, F.origin), *emin = evec(e, F.mins), *emax = evec(e, F.maxs);
			int c = ent_contents(e, solid);
			if (!(c & mask) && solid != SOLID_TRIGGER)
				continue;
			for (int i = 0; i < 3; i++) {
				bmin[i] = org[i] + emin[i];
				bmax[i] = org[i] + emax[i];
			}
			trace_aabb(start, mins, maxs, end, bmin, bmax, tr, e, c, solid == SOLID_TRIGGER);
		}
	}
	for (int i = 0; i < 3; i++)
		tr->endpos[i] = start[i] + (end[i] - start[i]) * tr->fraction;
	tr->inwater = false;
	if (water && G.trace_inwater >= 0) {
		/* the contents where the trace stopped. Nothing has moved since the
		   query above, so when the end lies in its box the slots it listed
		   hold every one a point query there would list */
		const float *p = tr->endpos;
		int bits;
		if (l.a && p[0] >= tmin[0] && p[1] >= tmin[1] && p[2] >= tmin[2] && p[0] <= tmax[0] && p[1] <= tmax[1] &&
		    p[2] <= tmax[2])
			bits = world_contents(p) | entity_contents(&l, p, true);
		else
			bits = SV_PointContentsBits(p);
		tr->inwater = (bits & (CONTENTS_WATER | CONTENTS_SLIME | CONTENTS_LAVA)) != 0;
	}
	area_done(&l);
	if (tr->ent > 0)
		tr->voxelcell[3] = -1;
	if (tr->ent < 0)
		tr->ent = 0;
	memcpy(last_voxelcell, tr->voxelcell, sizeof(tr->voxelcell));
	tr->inopen = !tr->startsolid;
}

void SV_Move(const float *start, const float *mins, const float *maxs, const float *end, int type, int passent, svtrace_t *tr)
{
	enter();
	move(start, mins, maxs, end, type, passent, tr, false);
}

void SV_MoveWater(const float *start, const float *mins, const float *maxs, const float *end, int type, int passent,
                  svtrace_t *tr)
{
	enter();
	move(start, mins, maxs, end, type, passent, tr, true);
}

void SV_SetTraceGlobals(const svtrace_t *tr)
{
	enter();
	qcvm_t *vm = cur()->vm;
	QC_SetGFloat(vm, G.trace_allsolid, tr->allsolid);
	QC_SetGFloat(vm, G.trace_startsolid, tr->startsolid);
	QC_SetGFloat(vm, G.trace_fraction, tr->fraction);
	QC_SetGVec(vm, G.trace_endpos, tr->endpos);
	QC_SetGVec(vm, G.trace_plane_normal, tr->normal);
	QC_SetGFloat(vm, G.trace_plane_dist, tr->dist);
	QC_SetGInt(vm, G.trace_ent, tr->ent);
	QC_SetGFloat(vm, G.trace_inopen, tr->inopen);
	QC_SetGFloat(vm, G.trace_inwater, tr->inwater);
	QC_SetGInt(vm, G.trace_endcontentsi, tr->contents);
	QC_SetGInt(vm, G.trace_surfaceflagsi, tr->surfaceflags);
	if (G.trace_surfacename >= 0)
		vm->globals[G.trace_surfacename].i = tr->surfacename && *tr->surfacename ? QC_NewString(vm, tr->surfacename) : 0;
	QC_SetGInt(vm, G.trace_brush_id, tr->brush + 1);
	QC_SetGInt(vm, G.trace_brush_faceid, tr->brushside + 1);
}

void SV_TouchTriggers(int e)
{
	enter();
	qcvm_t *vm = cur()->vm;
	float *amin = evec(e, F.absmin), *amax = evec(e, F.absmax);
	int oldself = QC_GInt(vm, G.self), oldother = QC_GInt(vm, G.other);
	arealist_t l;
	if (!amin || !amax)
		return;
	area_query(&l, amin, amax, true);
	for (int k = 0, t; (t = area_at(&l, k, vm->num_edicts)) >= 0; k++) {
		int solid;
		float *tmin, *tmax;
		func_t touch;
		if (t == e || t >= vm->num_edicts || vm->edicts[t].free)
			continue;
		solid = (int)QC_EFloat(vm, t, F.solid);
		if (solid != SOLID_TRIGGER && solid != SOLID_BSPTRIGGER)
			continue;
		touch = QC_EInt(vm, t, F.touch);
		if (!touch)
			continue;
		tmin = evec(t, F.absmin);
		tmax = evec(t, F.absmax);
		if (tmin[0] > amax[0] || tmin[1] > amax[1] || tmin[2] > amax[2] || tmax[0] < amin[0] ||
		    tmax[1] < amin[1] || tmax[2] < amin[2])
			continue;
		QC_SetGInt(vm, G.self, t);
		QC_SetGInt(vm, G.other, e);
		QC_Execute(vm, touch);
		enter(); /* as the touch's own world calls left it */
		if (vm->edicts[e].free)
			break;
		/* the touch may have moved e or any trigger: look again from here
		   on, as the scan of every slot saw them where they are now */
		if (l.a) {
			area_done(&l);
			area_query(&l, amin, amax, true);
			if (l.a) {
				for (k = 0; k < l.n && l.ids[k] <= t; k++)
					;
				k--;
			} else
				k = t - 1; /* scanning every slot: carry on after t */
		}
	}
	area_done(&l);
	QC_SetGInt(vm, G.self, oldself);
	QC_SetGInt(vm, G.other, oldother);
}

bool SV_DropToFloor(int e)
{
	enter();
	float *org = evec(e, F.origin), end[3];
	svtrace_t tr;
	if (!org)
		return false;
	VectorCopy(org, end);
	end[2] -= 256;
	SV_Move(org, evec(e, F.mins), evec(e, F.maxs), end, MOVE_NORMAL, e, &tr);
	if (tr.fraction == 1 || tr.allsolid)
		return false;
	VectorCopy(tr.endpos, org);
	SV_LinkEdict(e, false);
	QC_SetEFloat(cur()->vm, e, F.flags, (float)((int)QC_EFloat(cur()->vm, e, F.flags) | FL_ONGROUND));
	QC_SetEInt(cur()->vm, e, F.groundentity, tr.ent);
	return true;
}

/* getsurfacenearpoint(ent, point) and getsurfacetexture(ent, surface) on the
 * collision model, so the server game can tell what it hit or stands on
 * (footsteps, impact effects) without a renderer. Entity 0 is the world. */
static bool surface_model(int e, brushref_t *m, float *origin)
{
	VectorClear(origin);
	if (e <= 0) {
		m->bsp = cur()->world;
		m->submodel = 0;
		return m->bsp != NULL;
	}
	if (!ent_brush_model(e, m))
		return false;
	VectorCopy(evec(e, F.origin), origin);
	return true;
}

void W_bi_getsurfacenearpoint(qcvm_t *vm)
{
	enter();
	brushref_t m;
	float o[3], p[3];
	const float *in = QC_PVEC(vm, 1);
	if (!surface_model(QC_PENT(vm, 0), &m, o)) {
		QC_ReturnFloat(vm, -1);
		return;
	}
	for (int k = 0; k < 3; k++)
		p[k] = in[k] - o[k];
	QC_ReturnFloat(vm, (float)BSP_SurfaceNearPoint(m.bsp, m.submodel, p));
}

void W_bi_getsurfacetexture(qcvm_t *vm)
{
	enter();
	brushref_t m;
	float o[3];
	if (!surface_model(QC_PENT(vm, 0), &m, o)) {
		QC_ReturnString(vm, "");
		return;
	}
	QC_ReturnString(vm, BSP_SurfaceTexture(m.bsp, m.submodel, (int)QC_PFLOAT(vm, 1)));
}
