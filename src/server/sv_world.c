/* Bulwark engine - server world: entity links, traces and contents.
 * Entities are tested by brute force for now; an area grid can come later.
 * ISC licence, see LICENSE. */
#include "sv_local.h"

#include <math.h>

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

static worldctx_t sv_worldctx = {NULL, NULL, sv_brushmodel};
static worldctx_t *W = &sv_worldctx;

static inline worldctx_t *cur(void)
{
	if (W == &sv_worldctx) {
		sv_worldctx.vm = sv.vm;
		sv_worldctx.world = sv.world;
	}
	return W;
}

worldctx_t *SV_SetWorldContext(worldctx_t *w)
{
	worldctx_t *old = W;
	W = w ? w : &sv_worldctx;
	return old == &sv_worldctx ? NULL : old;
}

#define F cur()->vm->fofs
#define G cur()->vm->gofs

static float *evec(int e, int ofs) { return ofs < 0 ? NULL : &QC_EdictFields(cur()->vm, e)[ofs].f; }

void SV_LinkEdict(int e, bool touch_triggers)
{
	float *org = evec(e, F.origin), *mins = evec(e, F.mins), *maxs = evec(e, F.maxs);
	float *amin = evec(e, F.absmin), *amax = evec(e, F.absmax), *size = evec(e, F.size);
	if (e <= 0 || !org || !mins || !maxs)
		return;
	for (int i = 0; i < 3; i++) {
		if (size)
			size[i] = maxs[i] - mins[i];
		if (amin)
			amin[i] = org[i] + mins[i] - 1;
		if (amax)
			amax[i] = org[i] + maxs[i] + 1;
	}
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

int SV_PointContentsBits(const float *p)
{
	float zero[3] = {0, 0, 0};
	int bits = cur()->world ? BSP_PointContents(cur()->world, 0, zero, p) : 0;
	/* brush entities such as func_water add their contents too */
	for (int e = 1; e < cur()->vm->num_edicts; e++) {
		brushref_t m;
		int solid;
		if (cur()->vm->edicts[e].free)
			continue;
		solid = (int)QC_EFloat(cur()->vm, e, F.solid);
		if (solid != SOLID_BSP && solid != SOLID_BSPTRIGGER)
			continue;
		if (!ent_brush_model(e, &m))
			continue;
		bits |= BSP_PointContents(m.bsp, m.submodel, evec(e, F.origin), p);
	}
	return bits;
}

/* sweeps a box against an axis aligned box (another entity's bounds) */
static void trace_aabb(const float *start, const float *mins, const float *maxs, const float *end,
                       const float *bmin, const float *bmax, svtrace_t *tr, int ent, int contents)
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

void SV_Move(const float *start, const float *mins, const float *maxs, const float *end, int type, int passent, svtrace_t *tr)
{
	float zero[3] = {0, 0, 0}, tmin[3], tmax[3];
	int mask = default_mask(passent);
	int passowner = passent > 0 ? QC_EInt(cur()->vm, passent, F.owner) : 0;
	int passhit = (passent > 0 && F.dimension_hit >= 0) ? (int)QC_EFloat(cur()->vm, passent, F.dimension_hit) : 255;
	bsptrace_t bt;

	memset(tr, 0, sizeof(*tr));
	tr->fraction = 1;
	tr->ent = -1;
	tr->surfacename = "";
	tr->brush = tr->brushside = -1;
	if (type & MOVE_EVERYTHING)
		mask = -1;

	if (cur()->world) {
		BSP_TraceModel(cur()->world, 0, zero, start, mins, maxs, end, mask, &bt);
		merge_bsp(tr, &bt, cur()->world, 0);
	}

	for (int i = 0; i < 3; i++) {
		tmin[i] = BW_MIN(start[i], end[i]) + mins[i] - 1;
		tmax[i] = BW_MAX(start[i], end[i]) + maxs[i] + 1;
	}
	for (int e = 1; e < cur()->vm->num_edicts && tr->fraction > 0; e++) {
		int solid, owner;
		float *amin, *amax;
		brushref_t m;
		if (cur()->vm->edicts[e].free || e == passent)
			continue;
		solid = (int)QC_EFloat(cur()->vm, e, F.solid);
		if (solid == SOLID_NOT)
			continue;
		if ((solid == SOLID_TRIGGER || solid == SOLID_BSPTRIGGER) && !(type & (MOVE_TRIGGERS | MOVE_EVERYTHING)))
			continue;
		if ((type & MOVE_NOMONSTERS) && solid != SOLID_BSP)
			continue;
		owner = QC_EInt(cur()->vm, e, F.owner);
		if (passent > 0 && (owner == passent || (passowner && passowner == e)))
			continue;
		if (F.dimension_solid >= 0 && !((int)QC_EFloat(cur()->vm, e, F.dimension_solid) & passhit))
			continue;
		amin = evec(e, F.absmin);
		amax = evec(e, F.absmax);
		if (amin && amax && (amin[0] > tmax[0] || amin[1] > tmax[1] || amin[2] > tmax[2] ||
		                     amax[0] < tmin[0] || amax[1] < tmin[1] || amax[2] < tmin[2]))
			continue;
		if ((solid == SOLID_BSP || solid == SOLID_BSPTRIGGER) && ent_brush_model(e, &m)) {
			int emask = (solid == SOLID_BSPTRIGGER) ? -1 : mask;
			BSP_TraceModel(m.bsp, m.submodel, evec(e, F.origin), start, mins, maxs, end, emask, &bt);
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
			trace_aabb(start, mins, maxs, end, bmin, bmax, tr, e, c);
		}
	}
	for (int i = 0; i < 3; i++)
		tr->endpos[i] = start[i] + (end[i] - start[i]) * tr->fraction;
	if (tr->ent < 0)
		tr->ent = 0;
	tr->inopen = !tr->startsolid;
	tr->inwater = (SV_PointContentsBits(tr->endpos) & (CONTENTS_WATER | CONTENTS_SLIME | CONTENTS_LAVA)) != 0;
}

void SV_SetTraceGlobals(const svtrace_t *tr)
{
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
	qcvm_t *vm = cur()->vm;
	float *amin = evec(e, F.absmin), *amax = evec(e, F.absmax);
	int oldself = QC_GInt(vm, G.self), oldother = QC_GInt(vm, G.other);
	if (!amin || !amax)
		return;
	for (int t = 1; t < vm->num_edicts; t++) {
		int solid;
		float *tmin, *tmax;
		func_t touch;
		if (t == e || vm->edicts[t].free)
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
		if (vm->edicts[e].free)
			break;
	}
	QC_SetGInt(vm, G.self, oldself);
	QC_SetGInt(vm, G.other, oldother);
}

bool SV_DropToFloor(int e)
{
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
