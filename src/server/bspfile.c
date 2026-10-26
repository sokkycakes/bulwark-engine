/* Bulwark engine - BSP map loading and brush collision.
 * The Q3 lump directory and record sizes are listed in docs/spec/bsp.md.
 * ISC licence, see LICENSE. */
#include "bspfile.h"
#include "../common/fs.h"

#include <math.h>

#define DIST_EPSILON 0.03125f

static int32_t rd32(const uint8_t *p) { return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }
static float rdf(const uint8_t *p)
{
	int32_t i = rd32(p);
	float f;
	memcpy(&f, &i, 4);
	return f;
}

typedef struct {
	int32_t ofs, len;
} lump_t;

static bool lump_ok(size_t size, lump_t l, int recsize)
{
	return l.ofs >= 0 && l.len >= 0 && (size_t)l.ofs + (size_t)l.len <= size && (recsize <= 0 || l.len % recsize == 0);
}

enum { Q3_ENTITIES = 0, Q3_SHADERS = 1, Q3_PLANES = 2, Q3_MODELS = 7, Q3_BRUSHES = 8, Q3_BRUSHSIDES = 9, Q3_NUMLUMPS = 17 };
enum { Q1_ENTITIES = 0, Q1_MODELS = 14, Q1_NUMLUMPS = 15 };

static void brush_bounds(bspmap_t *m, cbrush_t *b)
{
	/* axial sides give the bounds; Q3 compilers always emit the six axial planes first */
	for (int i = 0; i < 3; i++) {
		b->mins[i] = -99999;
		b->maxs[i] = 99999;
	}
	for (int s = 0; s < b->numsides; s++) {
		cplane_t *p = &m->planes[m->brushsides[b->firstside + s].plane];
		for (int i = 0; i < 3; i++) {
			if (p->normal[i] == 1 && p->normal[(i + 1) % 3] == 0 && p->normal[(i + 2) % 3] == 0)
				b->maxs[i] = BW_MIN(b->maxs[i], p->dist);
			else if (p->normal[i] == -1 && p->normal[(i + 1) % 3] == 0 && p->normal[(i + 2) % 3] == 0)
				b->mins[i] = BW_MAX(b->mins[i], -p->dist);
		}
	}
}

/* IBSP brush sides are {plane, shader}; RBSP/FBSP append a drawsurface index */
static bool load_q3(bspmap_t *m, const uint8_t *d, size_t size)
{
	lump_t l[Q3_NUMLUMPS];
	int sidesize = memcmp(d, "IBSP", 4) ? 12 : 8;
	if (size < 8 + Q3_NUMLUMPS * 8)
		return false;
	for (int i = 0; i < Q3_NUMLUMPS; i++) {
		l[i].ofs = rd32(d + 8 + i * 8);
		l[i].len = rd32(d + 12 + i * 8);
	}
	if (!lump_ok(size, l[Q3_ENTITIES], 0) || !lump_ok(size, l[Q3_SHADERS], 72) || !lump_ok(size, l[Q3_PLANES], 16) ||
	    !lump_ok(size, l[Q3_MODELS], 40) || !lump_ok(size, l[Q3_BRUSHES], 12) || !lump_ok(size, l[Q3_BRUSHSIDES], sidesize))
		return false;

	m->entities = Mem_Alloc((size_t)l[Q3_ENTITIES].len + 1);
	memcpy(m->entities, d + l[Q3_ENTITIES].ofs, (size_t)l[Q3_ENTITIES].len);

	m->numshaders = l[Q3_SHADERS].len / 72;
	m->shaders = Mem_Alloc(sizeof(cshader_t) * (m->numshaders + 1));
	for (int i = 0; i < m->numshaders; i++) {
		const uint8_t *p = d + l[Q3_SHADERS].ofs + i * 72;
		memcpy(m->shaders[i].name, p, 63);
		m->shaders[i].surfaceflags = rd32(p + 64);
		m->shaders[i].contents = rd32(p + 68);
	}

	m->numplanes = l[Q3_PLANES].len / 16;
	m->planes = Mem_Alloc(sizeof(cplane_t) * (m->numplanes + 1));
	for (int i = 0; i < m->numplanes; i++) {
		const uint8_t *p = d + l[Q3_PLANES].ofs + i * 16;
		for (int k = 0; k < 3; k++)
			m->planes[i].normal[k] = rdf(p + k * 4);
		m->planes[i].dist = rdf(p + 12);
	}

	m->numbrushsides = l[Q3_BRUSHSIDES].len / sidesize;
	m->brushsides = Mem_Alloc(sizeof(cbrushside_t) * (m->numbrushsides + 1));
	for (int i = 0; i < m->numbrushsides; i++) {
		const uint8_t *p = d + l[Q3_BRUSHSIDES].ofs + i * sidesize;
		m->brushsides[i].plane = rd32(p);
		m->brushsides[i].shader = rd32(p + 4);
		if (m->brushsides[i].plane < 0 || m->brushsides[i].plane >= m->numplanes)
			return false;
	}

	m->numbrushes = l[Q3_BRUSHES].len / 12;
	m->brushes = Mem_Alloc(sizeof(cbrush_t) * (m->numbrushes + 1));
	for (int i = 0; i < m->numbrushes; i++) {
		const uint8_t *p = d + l[Q3_BRUSHES].ofs + i * 12;
		cbrush_t *b = &m->brushes[i];
		b->firstside = rd32(p);
		b->numsides = rd32(p + 4);
		b->shader = rd32(p + 8);
		if (b->firstside < 0 || b->numsides < 0 || b->firstside + b->numsides > m->numbrushsides)
			return false;
		b->contents = (b->shader >= 0 && b->shader < m->numshaders) ? m->shaders[b->shader].contents : CONTENTS_SOLID;
		brush_bounds(m, b);
	}

	m->nummodels = l[Q3_MODELS].len / 40;
	m->models = Mem_Alloc(sizeof(cmodel_t) * (m->nummodels + 1));
	for (int i = 0; i < m->nummodels; i++) {
		const uint8_t *p = d + l[Q3_MODELS].ofs + i * 40;
		cmodel_t *cm = &m->models[i];
		for (int k = 0; k < 3; k++) {
			cm->mins[k] = rdf(p + k * 4);
			cm->maxs[k] = rdf(p + 12 + k * 4);
		}
		cm->firstbrush = rd32(p + 32);
		cm->numbrushes = rd32(p + 36);
		if (cm->firstbrush < 0 || cm->numbrushes < 0 || cm->firstbrush + cm->numbrushes > m->numbrushes)
			return false;
	}
	m->type = BSP_Q3;
	return true;
}

static bool load_q1(bspmap_t *m, const uint8_t *d, size_t size)
{
	lump_t l[Q1_NUMLUMPS];
	if (size < 4 + Q1_NUMLUMPS * 8)
		return false;
	for (int i = 0; i < Q1_NUMLUMPS; i++) {
		l[i].ofs = rd32(d + 4 + i * 8);
		l[i].len = rd32(d + 8 + i * 8);
	}
	if (!lump_ok(size, l[Q1_ENTITIES], 0) || !lump_ok(size, l[Q1_MODELS], 64))
		return false;
	m->entities = Mem_Alloc((size_t)l[Q1_ENTITIES].len + 1);
	memcpy(m->entities, d + l[Q1_ENTITIES].ofs, (size_t)l[Q1_ENTITIES].len);
	m->nummodels = l[Q1_MODELS].len / 64;
	m->models = Mem_Alloc(sizeof(cmodel_t) * (m->nummodels + 1));
	for (int i = 0; i < m->nummodels; i++) {
		const uint8_t *p = d + l[Q1_MODELS].ofs + i * 64;
		for (int k = 0; k < 3; k++) {
			m->models[i].mins[k] = rdf(p + k * 4);
			m->models[i].maxs[k] = rdf(p + 12 + k * 4);
		}
	}
	/* TODO: Q1 clipping hulls; Q1 maps currently have no world collision */
	m->type = BSP_Q1;
	return true;
}

bspmap_t *BSP_Load(const char *path)
{
	size_t size;
	uint8_t *d = (uint8_t *)FS_LoadFile(path, &size);
	bspmap_t *m;
	bool ok = false;
	if (!d)
		return NULL;
	m = Mem_Alloc(sizeof(*m));
	Str_Copy(m->name, path, sizeof(m->name));
	if (size >= 8 && (!memcmp(d, "IBSP", 4) || !memcmp(d, "FBSP", 4) || !memcmp(d, "RBSP", 4)))
		ok = load_q3(m, d, size);
	else if (size >= 4) {
		int32_t v = rd32(d);
		if (v == 29 || v == 30 || !memcmp(d, "BSP2", 4))
			ok = load_q1(m, d, size);
	}
	Mem_Free(d);
	if (!ok) {
		Con_Printf("%s: unsupported or corrupt BSP\n", path);
		BSP_Free(m);
		return NULL;
	}
	return m;
}

void BSP_Free(bspmap_t *m)
{
	if (!m)
		return;
	Mem_Free(m->entities);
	Mem_Free(m->planes);
	Mem_Free(m->brushsides);
	Mem_Free(m->brushes);
	Mem_Free(m->shaders);
	Mem_Free(m->models);
	Mem_Free(m);
}

/* --- collision ------------------------------------------------------- */

typedef struct {
	float start[3], end[3];
	float mins[3], maxs[3];
	float tmins[3], tmaxs[3]; /* bounds of the whole sweep */
	bool ispoint;
	int mask;
	bsptrace_t *tr;
} tracework_t;

/* distance from the plane to the box corner that touches it first */
static float box_offset(const float *n, const float *mins, const float *maxs)
{
	float d = 0;
	for (int i = 0; i < 3; i++)
		d += n[i] * (n[i] < 0 ? maxs[i] : mins[i]);
	return d;
}

static void trace_brush(const bspmap_t *m, int bnum, tracework_t *w)
{
	const cbrush_t *b = &m->brushes[bnum];
	float enterfrac = -1, leavefrac = 1;
	const cplane_t *hitplane = NULL;
	int hitside = -1;
	bool startout = false, getout = false;

	if (!b->numsides)
		return;
	for (int s = 0; s < b->numsides; s++) {
		const cbrushside_t *side = &m->brushsides[b->firstside + s];
		const cplane_t *p = &m->planes[side->plane];
		float dist = p->dist - box_offset(p->normal, w->mins, w->maxs);
		float d1 = DotProduct(w->start, p->normal) - dist;
		float d2 = DotProduct(w->end, p->normal) - dist;
		if (d2 > 0)
			getout = true;
		if (d1 > 0)
			startout = true;
		/* completely in front of one face: the whole sweep misses this brush */
		if (d1 > 0 && (d2 >= DIST_EPSILON || d2 >= d1))
			return;
		if (d1 <= 0 && d2 <= 0)
			continue;
		if (d1 > d2) {
			float f = (d1 - DIST_EPSILON) / (d1 - d2);
			if (f < 0)
				f = 0;
			if (f > enterfrac) {
				enterfrac = f;
				hitplane = p;
				hitside = b->firstside + s;
			}
		} else {
			float f = (d1 + DIST_EPSILON) / (d1 - d2);
			if (f > 1)
				f = 1;
			if (f < leavefrac)
				leavefrac = f;
		}
	}
	if (!startout) {
		w->tr->startsolid = true;
		if (!getout) {
			w->tr->allsolid = true;
			w->tr->fraction = 0;
		}
		w->tr->contents = b->contents;
		w->tr->brush = bnum;
		return;
	}
	if (enterfrac < leavefrac && enterfrac > -1 && enterfrac < w->tr->fraction) {
		if (enterfrac < 0)
			enterfrac = 0;
		w->tr->fraction = enterfrac;
		VectorCopy(hitplane->normal, w->tr->normal);
		w->tr->dist = hitplane->dist;
		w->tr->contents = b->contents;
		w->tr->brush = bnum;
		w->tr->brushside = hitside;
		{
			int sh = m->brushsides[hitside].shader;
			if (sh < 0 || sh >= m->numshaders)
				sh = b->shader;
			w->tr->shader = sh;
			w->tr->surfaceflags = (sh >= 0 && sh < m->numshaders) ? m->shaders[sh].surfaceflags : 0;
		}
	}
}

void BSP_TraceModel(const bspmap_t *map, int model, const float *origin, const float *start,
                    const float *mins, const float *maxs, const float *end, int mask, bsptrace_t *tr)
{
	tracework_t w;
	const cmodel_t *cm;
	memset(tr, 0, sizeof(*tr));
	tr->fraction = 1;
	tr->shader = tr->brush = tr->brushside = -1;
	VectorCopy(end, tr->endpos);
	if (!map || map->type != BSP_Q3 || model < 0 || model >= map->nummodels)
		return;
	cm = &map->models[model];
	for (int i = 0; i < 3; i++) {
		/* work in the model's space: entity models are only translated */
		w.start[i] = start[i] - origin[i];
		w.end[i] = end[i] - origin[i];
		w.mins[i] = mins[i];
		w.maxs[i] = maxs[i];
		w.tmins[i] = BW_MIN(w.start[i], w.end[i]) + mins[i] - 1;
		w.tmaxs[i] = BW_MAX(w.start[i], w.end[i]) + maxs[i] + 1;
	}
	w.mask = mask;
	w.tr = tr;
	for (int i = 0; i < cm->numbrushes; i++) {
		int bn = cm->firstbrush + i;
		const cbrush_t *b = &map->brushes[bn];
		if (!(b->contents & mask))
			continue;
		if (b->mins[0] > w.tmaxs[0] || b->mins[1] > w.tmaxs[1] || b->mins[2] > w.tmaxs[2] ||
		    b->maxs[0] < w.tmins[0] || b->maxs[1] < w.tmins[1] || b->maxs[2] < w.tmins[2])
			continue;
		trace_brush(map, bn, &w);
		if (tr->allsolid)
			break;
	}
	for (int i = 0; i < 3; i++)
		tr->endpos[i] = start[i] + (end[i] - start[i]) * tr->fraction;
}

int BSP_PointContents(const bspmap_t *map, int model, const float *origin, const float *p)
{
	const cmodel_t *cm;
	int contents = 0;
	float lp[3];
	if (!map || map->type != BSP_Q3 || model < 0 || model >= map->nummodels)
		return 0;
	cm = &map->models[model];
	for (int i = 0; i < 3; i++)
		lp[i] = p[i] - origin[i];
	for (int i = 0; i < cm->numbrushes; i++) {
		const cbrush_t *b = &map->brushes[cm->firstbrush + i];
		bool inside = true;
		if (lp[0] < b->mins[0] || lp[1] < b->mins[1] || lp[2] < b->mins[2] || lp[0] > b->maxs[0] ||
		    lp[1] > b->maxs[1] || lp[2] > b->maxs[2])
			continue;
		for (int s = 0; s < b->numsides && inside; s++) {
			const cplane_t *pl = &map->planes[map->brushsides[b->firstside + s].plane];
			if (DotProduct(lp, pl->normal) - pl->dist > 0)
				inside = false;
		}
		if (inside)
			contents |= b->contents;
	}
	return contents;
}
