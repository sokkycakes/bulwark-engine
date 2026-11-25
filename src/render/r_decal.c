/* Bulwark engine - decals and world surface queries. The world's geometry is
 * kept on the CPU so decals can be clipped against it and the client game
 * can ask about the surface near a point (getsurface*). Decals come from
 * adddecal (rebuilt each frame by the game, so their geometry is cached)
 * and from particle effects of type decal (kept until they fade).
 * ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/cmd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static cvar_t *r_drawdecals, *r_decal_max;

/* --- the world's geometry ------------------------------------------------------ */

static struct {
	rvert_t *v;
	uint32_t *idx;
	int numverts, numidx;
	int *stamp; /* per surface, so a box query lists each surface once */
	int stampframe;
} wg;

void R_WorldKeepGeometry(const rvert_t *v, int numverts, const uint32_t *idx, int numidx)
{
	Mem_Free(wg.v);
	Mem_Free(wg.idx);
	Mem_Free(wg.stamp);
	memset(&wg, 0, sizeof(wg));
	wg.v = Mem_Alloc(sizeof(rvert_t) * (numverts + 1));
	wg.idx = Mem_Alloc(sizeof(uint32_t) * (numidx + 1));
	memcpy(wg.v, v, sizeof(rvert_t) * numverts);
	memcpy(wg.idx, idx, sizeof(uint32_t) * numidx);
	wg.numverts = numverts;
	wg.numidx = numidx;
}

static void free_decals(void);

void R_FreeWorldGeometry(void)
{
	Mem_Free(wg.v);
	Mem_Free(wg.idx);
	Mem_Free(wg.stamp);
	memset(&wg, 0, sizeof(wg));
	free_decals();
}

typedef struct {
	int *out;
	int num, max;
	const float *mins, *maxs;
} boxquery_t;

static bool box_overlap(const float *amins, const float *amaxs, const float *bmins, const float *bmaxs)
{
	for (int k = 0; k < 3; k++)
		if (amins[k] > bmaxs[k] || amaxs[k] < bmins[k])
			return false;
	return true;
}

static void add_surface(boxquery_t *q, int s)
{
	const rsurface_t *rs;
	if (s < 0 || s >= r_world.numsurfaces || wg.stamp[s] == wg.stampframe)
		return;
	wg.stamp[s] = wg.stampframe;
	rs = &r_world.surfaces[s];
	if (rs->numindexes && q->num < q->max && box_overlap(rs->mins, rs->maxs, q->mins, q->maxs))
		q->out[q->num++] = s;
}

/* down the node tree with a box, listing the faces of the leafs it touches */
static void box_nodes(boxquery_t *q, const bspmap_t *bsp, int num)
{
	int guard = 0;
	while (num >= 0 && guard++ < 65536) {
		const cnode_t *n = &bsp->nodes[num];
		const cplane_t *pl = &bsp->planes[n->plane];
		float dmin = -pl->dist, dmax = -pl->dist;
		for (int k = 0; k < 3; k++) {
			float a = pl->normal[k] * q->mins[k], b = pl->normal[k] * q->maxs[k];
			dmin += BW_MIN(a, b);
			dmax += BW_MAX(a, b);
		}
		if (dmin >= 0)
			num = n->children[0];
		else if (dmax < 0)
			num = n->children[1];
		else {
			box_nodes(q, bsp, n->children[0]);
			num = n->children[1];
		}
	}
	num = -(num + 1);
	if (num >= 0 && num < bsp->numleafs) {
		const cleaf_t *lf = &bsp->leafs[num];
		for (int i = 0; i < lf->numfaces; i++) {
			int f = bsp->leaffaces[lf->firstface + i];
			if (f >= 0 && f < r_world.numsurfaces)
				add_surface(q, r_world.surfmap[f]);
		}
	}
}

/* the world model's surfaces whose bounds touch the box */
int R_SurfacesInBox(const float *mins, const float *maxs, int *out, int max)
{
	boxquery_t q = {out, 0, max, mins, maxs};
	const bspmap_t *bsp = r_world.bsp;
	if (!r_world.loaded || !wg.v || !r_world.numsubmodels)
		return 0;
	if (!wg.stamp)
		wg.stamp = Mem_Alloc(sizeof(int) * (r_world.numsurfaces + 1));
	wg.stampframe++;
	if (bsp && bsp == cl.world && bsp->numnodes && r_world.surfmap) {
		int root = bsp->type == BSP_Q1 && bsp->nummodels ? bsp->models[0].headnode[0] : 0;
		box_nodes(&q, bsp, root);
	} else {
		const rmodel_t *m = &r_world.submodels[0];
		for (int i = 0; i < m->numsurfaces; i++)
			add_surface(&q, m->firstsurface + i);
	}
	return q.num;
}

/* --- surface queries ------------------------------------------------------------- */

static void closest_on_triangle(const float *p, const float *a, const float *b, const float *c, float *out)
{
	/* by regions of the triangle: vertices, edges, then the inside */
	float ab[3], ac[3], ap[3], bp[3], cp[3], d1, d2, d3, d4, d5, d6, va, vb, vc, v, w, denom;
	for (int k = 0; k < 3; k++) {
		ab[k] = b[k] - a[k];
		ac[k] = c[k] - a[k];
		ap[k] = p[k] - a[k];
		bp[k] = p[k] - b[k];
		cp[k] = p[k] - c[k];
	}
	d1 = DotProduct(ab, ap);
	d2 = DotProduct(ac, ap);
	if (d1 <= 0 && d2 <= 0) {
		VectorCopy(a, out);
		return;
	}
	d3 = DotProduct(ab, bp);
	d4 = DotProduct(ac, bp);
	if (d3 >= 0 && d4 <= d3) {
		VectorCopy(b, out);
		return;
	}
	vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) {
		v = d1 / (d1 - d3);
		for (int k = 0; k < 3; k++)
			out[k] = a[k] + ab[k] * v;
		return;
	}
	d5 = DotProduct(ab, cp);
	d6 = DotProduct(ac, cp);
	if (d6 >= 0 && d5 <= d6) {
		VectorCopy(c, out);
		return;
	}
	vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) {
		w = d2 / (d2 - d6);
		for (int k = 0; k < 3; k++)
			out[k] = a[k] + ac[k] * w;
		return;
	}
	va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
		w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		for (int k = 0; k < 3; k++)
			out[k] = b[k] + (c[k] - b[k]) * w;
		return;
	}
	denom = 1 / (va + vb + vc);
	v = vb * denom;
	w = vc * denom;
	for (int k = 0; k < 3; k++)
		out[k] = a[k] + ab[k] * v + ac[k] * w;
}

static float surface_distance(int s, const float *p)
{
	const rsurface_t *rs = &r_world.surfaces[s];
	float best = 1e30f;
	for (int i = rs->firstindex; i + 2 < rs->firstindex + rs->numindexes; i += 3) {
		float c[3], d;
		closest_on_triangle(p, wg.v[wg.idx[i]].xyz, wg.v[wg.idx[i + 1]].xyz, wg.v[wg.idx[i + 2]].xyz, c);
		d = (c[0] - p[0]) * (c[0] - p[0]) + (c[1] - p[1]) * (c[1] - p[1]) + (c[2] - p[2]) * (c[2] - p[2]);
		best = BW_MIN(best, d);
	}
	return best;
}

/* nearest drawn surface of a model to a point in its space, as an index
 * into the model's surfaces; -1 when nothing is near */
int R_SurfaceNearPoint(const rmodel_t *m, const float *p)
{
	int best = -1;
	float bestd = 1e30f;
	if (!m || m->type != MOD_BRUSH || !wg.v)
		return -1;
	if (m == &r_world.submodels[0]) {
		static int list[1024];
		for (float r = 4; r <= 256 && best < 0; r *= 4) {
			float mins[3] = {p[0] - r, p[1] - r, p[2] - r}, maxs[3] = {p[0] + r, p[1] + r, p[2] + r};
			int n = R_SurfacesInBox(mins, maxs, list, (int)BW_ARRAY_COUNT(list));
			for (int i = 0; i < n; i++) {
				float d;
				if (r_world.surfaces[list[i]].material->sky && n > 1)
					d = surface_distance(list[i], p) + 1; /* prefer a real wall at a corner */
				else
					d = surface_distance(list[i], p);
				if (d < bestd && d <= r * r) {
					bestd = d;
					best = list[i] - m->firstsurface;
				}
			}
		}
		return best;
	}
	for (int i = 0; i < m->numsurfaces; i++) {
		float d = surface_distance(m->firstsurface + i, p);
		if (d < bestd) {
			bestd = d;
			best = i;
		}
	}
	return best;
}

const rsurface_t *R_ModelSurface(const rmodel_t *m, int s)
{
	if (!m || m->type != MOD_BRUSH || !wg.v || s < 0 || s >= m->numsurfaces)
		return NULL;
	return &r_world.surfaces[m->firstsurface + s];
}

/* the distinct vertices of a surface, in index order */
int R_SurfacePoints(const rsurface_t *rs, const rvert_t **out, int max)
{
	int n = 0;
	for (int i = rs->firstindex; i < rs->firstindex + rs->numindexes && n < max; i++) {
		const rvert_t *v = &wg.v[wg.idx[i]];
		bool seen = false;
		for (int k = 0; k < n && !seen; k++)
			seen = out[k] == v;
		if (!seen)
			out[n++] = v;
	}
	return n;
}

const rvert_t *R_SurfaceTriangle(const rsurface_t *rs, int tri, int corner)
{
	int i = rs->firstindex + tri * 3 + corner;
	if (tri < 0 || tri * 3 + 2 >= rs->numindexes)
		return NULL;
	return &wg.v[wg.idx[i]];
}

/* texture axes of a surface: how s and t change across it, from one triangle */
void R_SurfaceAxes(const rsurface_t *rs, float *saxis, float *taxis, float *normal)
{
	const rvert_t *a, *b, *c;
	float e1[3], e2[3], ds1, ds2, dt1, dt2, det;
	VectorClear(saxis);
	VectorClear(taxis);
	VectorClear(normal);
	if (rs->numindexes < 3)
		return;
	a = &wg.v[wg.idx[rs->firstindex]];
	b = &wg.v[wg.idx[rs->firstindex + 1]];
	c = &wg.v[wg.idx[rs->firstindex + 2]];
	VectorCopy(a->normal, normal);
	for (int k = 0; k < 3; k++) {
		e1[k] = b->xyz[k] - a->xyz[k];
		e2[k] = c->xyz[k] - a->xyz[k];
	}
	ds1 = b->st[0] - a->st[0], ds2 = c->st[0] - a->st[0];
	dt1 = b->st[1] - a->st[1], dt2 = c->st[1] - a->st[1];
	det = ds1 * dt2 - ds2 * dt1;
	if (fabsf(det) < 1e-8f)
		return;
	for (int k = 0; k < 3; k++) {
		saxis[k] = (e1[k] * dt2 - e2[k] * dt1) / det;
		taxis[k] = (e2[k] * ds1 - e1[k] * ds2) / det;
	}
	VectorNormalize(saxis);
	VectorNormalize(taxis);
}

/* --- clipping decals ------------------------------------------------------------- */

#define MAX_CLIP_POINTS 24

typedef struct {
	float org[3], s[3], t[3], n[3]; /* s, t and n are unit vectors */
	float hs, ht, depth;
	bool bothsides;
	float tc[4]; /* s1 t1 s2 t2 of the picture */
	rvert_t *v;
	int num, cap;
} dbuild_t;

/* keeps the part of the polygon where dot(p - org, axis) * sign <= limit */
static int clip_side(float (*in)[3], int n, const float *org, const float *axis, float sign, float limit,
                     float (*out)[3])
{
	int o = 0;
	for (int i = 0; i < n; i++) {
		const float *a = in[i], *b = in[(i + 1) % n];
		float da = ((a[0] - org[0]) * axis[0] + (a[1] - org[1]) * axis[1] + (a[2] - org[2]) * axis[2]) * sign - limit;
		float db = ((b[0] - org[0]) * axis[0] + (b[1] - org[1]) * axis[1] + (b[2] - org[2]) * axis[2]) * sign - limit;
		if (da <= 0 && o < MAX_CLIP_POINTS)
			VectorCopy(a, out[o++]);
		if ((da < 0) != (db < 0) && o < MAX_CLIP_POINTS) {
			float f = da / (da - db);
			for (int k = 0; k < 3; k++)
				out[o][k] = a[k] + (b[k] - a[k]) * f;
			o++;
		}
	}
	return o;
}

static void push_decal_vert(dbuild_t *d, const float *p)
{
	rvert_t *v;
	float ds, dt;
	if (d->num == d->cap) {
		d->cap = d->cap ? d->cap * 2 : 64;
		d->v = Mem_Realloc(d->v, sizeof(rvert_t) * d->cap);
	}
	v = &d->v[d->num++];
	memset(v, 0, sizeof(*v));
	VectorCopy(p, v->xyz);
	VectorCopy(d->n, v->normal);
	ds = (p[0] - d->org[0]) * d->s[0] + (p[1] - d->org[1]) * d->s[1] + (p[2] - d->org[2]) * d->s[2];
	dt = (p[0] - d->org[0]) * d->t[0] + (p[1] - d->org[1]) * d->t[1] + (p[2] - d->org[2]) * d->t[2];
	ds = ds / (2 * d->hs) + 0.5f;
	dt = dt / (2 * d->ht) + 0.5f;
	v->st[0] = d->tc[0] + (d->tc[2] - d->tc[0]) * ds;
	v->st[1] = d->tc[1] + (d->tc[3] - d->tc[1]) * dt;
	memset(v->color, 255, 4);
}

static void clip_triangle(dbuild_t *d, const rvert_t *a, const rvert_t *b, const rvert_t *c)
{
	float bufa[MAX_CLIP_POINTS][3], bufb[MAX_CLIP_POINTS][3], fn[3];
	float (*in)[3] = bufa, (*out)[3] = bufb, (*tmp)[3];
	int n = 3;
	float facing;
	/* the vertex normals say which way the surface faces */
	for (int k = 0; k < 3; k++)
		fn[k] = a->normal[k] + b->normal[k] + c->normal[k];
	if (VectorNormalize(fn) < 1e-6f)
		return;
	facing = DotProduct(fn, d->n);
	if (d->bothsides ? fabsf(facing) < 0.1f : facing < 0.1f)
		return;
	VectorCopy(a->xyz, in[0]);
	VectorCopy(b->xyz, in[1]);
	VectorCopy(c->xyz, in[2]);
	{
		const float *axes[3] = {d->s, d->t, d->n};
		const float limits[3] = {d->hs, d->ht, d->depth};
		for (int ax = 0; ax < 3 && n >= 3; ax++)
			for (int sign = -1; sign <= 1 && n >= 3; sign += 2) {
				n = clip_side(in, n, d->org, axes[ax], (float)sign, limits[ax], out);
				tmp = in, in = out, out = tmp;
			}
	}
	for (int i = 1; i + 1 < n; i++) {
		push_decal_vert(d, in[0]);
		push_decal_vert(d, in[i]);
		push_decal_vert(d, in[i + 1]);
	}
}

/* clips the decal's box against the world: a triangle list in d->v */
static void build_decal(dbuild_t *d)
{
	static int list[2048];
	float mins[3], maxs[3];
	int n;
	for (int k = 0; k < 3; k++) {
		float e = fabsf(d->s[k]) * d->hs + fabsf(d->t[k]) * d->ht + fabsf(d->n[k]) * d->depth;
		mins[k] = d->org[k] - e;
		maxs[k] = d->org[k] + e;
	}
	n = R_SurfacesInBox(mins, maxs, list, (int)BW_ARRAY_COUNT(list));
	for (int i = 0; i < n; i++) {
		const rsurface_t *rs = &r_world.surfaces[list[i]];
		const material_t *m = rs->material;
		if (m->sky || m->nodraw || m->program == MP_WATER || m->blend != BLEND_OPAQUE)
			continue;
		for (int k = rs->firstindex; k + 2 < rs->firstindex + rs->numindexes; k += 3)
			clip_triangle(d, &wg.v[wg.idx[k]], &wg.v[wg.idx[k + 1]], &wg.v[wg.idx[k + 2]]);
	}
}

/* --- kept decals (particle effects) ----------------------------------------------- */

typedef struct {
	material_t *mat;
	int blend;
	GLenum src, dst;
	rvert_t *v;
	int num;
	float rgba[4], alphadelta;
	double born, die;
} decal_t;

#ifdef BW_LOWMEM
#define MAX_DECALS 1024 /* r_decal_max's default */
#else
#define MAX_DECALS 4096
#endif
static decal_t decals[MAX_DECALS];
static int decalhead, numdecals; /* a ring: the oldest goes first */

/* temporary decals from adddecal: their geometry is cached by placement */
typedef struct {
	material_t *mat;
	float key[9];
	rvert_t *v;
	int num;
	int lastframe;
} tdecal_t;

#define MAX_TEMP_CACHE 512
#define MAX_TEMP_DECALS 1024
static tdecal_t tcache[MAX_TEMP_CACHE];
static struct {
	int cache;
	float rgba[4];
} tframe[MAX_TEMP_DECALS];
static int numtframe, tframecount;

static void free_decals(void)
{
	for (int i = 0; i < MAX_DECALS; i++)
		Mem_Free(decals[i].v);
	memset(decals, 0, sizeof(decals));
	decalhead = numdecals = 0;
	for (int i = 0; i < MAX_TEMP_CACHE; i++)
		Mem_Free(tcache[i].v);
	memset(tcache, 0, sizeof(tcache));
	numtframe = 0;
}

void R_ClearDecals(void) { free_decals(); }

/* perpendicular axes for a plane normal, turned by an angle in degrees */
static void plane_axes(const float *n, float angle, float *s, float *t)
{
	float up[3] = {0, 0, 1}, a = angle * (float)M_PI / 180, cs = cosf(a), sn = sinf(a), s0[3], t0[3];
	if (fabsf(n[2]) > 0.9f)
		up[0] = 1, up[2] = 0;
	CrossProduct(up, n, s0);
	VectorNormalize(s0);
	CrossProduct(n, s0, t0);
	for (int k = 0; k < 3; k++) {
		s[k] = s0[k] * cs + t0[k] * sn;
		t[k] = t0[k] * cs - s0[k] * sn;
	}
}

void R_SpawnDecal(material_t *mat, int blend, GLenum src, GLenum dst, const float *org, const float *normal,
                  float radius, float angle, const float *tc, const float *rgba, float alphadelta, float life)
{
	dbuild_t d;
	decal_t *dc;
	int max = r_decal_max ? BW_MAX(1, BW_MIN(MAX_DECALS, r_decal_max->ivalue)) : MAX_DECALS;
	float n[3];
	if (!mat || !r_world.loaded || radius <= 0)
		return;
	VectorCopy(normal, n);
	if (VectorNormalize(n) < 1e-6f)
		return;
	memset(&d, 0, sizeof(d));
	VectorCopy(org, d.org);
	VectorCopy(n, d.n);
	plane_axes(n, angle, d.s, d.t);
	d.hs = d.ht = radius;
	d.depth = BW_MAX(4.0f, radius);
	memcpy(d.tc, tc, sizeof(d.tc));
	build_decal(&d);
	if (!d.num) {
		Mem_Free(d.v);
		return;
	}
	/* the oldest makes room */
	while (numdecals >= max) {
		decal_t *old = &decals[(decalhead - numdecals + MAX_DECALS * 2) % MAX_DECALS];
		Mem_Free(old->v);
		old->v = NULL;
		numdecals--;
	}
	dc = &decals[decalhead];
	Mem_Free(dc->v);
	decalhead = (decalhead + 1) % MAX_DECALS;
	numdecals++;
	dc->mat = mat;
	dc->blend = blend;
	dc->src = src;
	dc->dst = dst;
	dc->v = d.v;
	dc->num = d.num;
	memcpy(dc->rgba, rgba, sizeof(dc->rgba));
	dc->alphadelta = alphadelta;
	dc->born = r_time;
	dc->die = r_time + life;
}

/* adddecal(shader, origin, up, side, rgb, alpha); see the note inside on
 * how up and side are read */
void R_AddDecal(material_t *mat, const float *org, const float *up, const float *side, const float *rgb, float alpha)
{
	int found = -1, oldest = 0;
	float key[9];
	if (!mat || numtframe >= MAX_TEMP_DECALS || !r_world.loaded)
		return;
	VectorCopy(org, key);
	VectorCopy(up, key + 3);
	VectorCopy(side, key + 6);
	for (int i = 0; i < MAX_TEMP_CACHE; i++) {
		tdecal_t *t = &tcache[i];
		if (t->mat == mat && !memcmp(t->key, key, sizeof(key))) {
			found = i;
			break;
		}
		if (t->lastframe < tcache[oldest].lastframe)
			oldest = i;
	}
	if (found < 0) {
		dbuild_t d;
		tdecal_t *t = &tcache[oldest];
		float ln = VectorLength(up), lt = VectorLength(side);
		if (ln < 1e-6f || lt < 1e-6f)
			return;
		memset(&d, 0, sizeof(d));
		VectorCopy(org, d.org);
		/* up is along the surface normal (either way) over the width; side
		 * is the picture's downward axis over its height, as Nuclide's
		 * decals and sprays pass them; the picture's right is side x up */
		for (int k = 0; k < 3; k++) {
			d.n[k] = up[k] / ln;
			d.t[k] = side[k] / lt;
		}
		CrossProduct(d.t, d.n, d.s);
		if (VectorNormalize(d.s) < 1e-6f)
			return;
		d.hs = 0.5f / ln;
		d.ht = 0.5f / lt;
		d.depth = BW_MAX(4.0f, BW_MIN(d.hs, d.ht));
		d.bothsides = true;
		d.tc[0] = 0, d.tc[1] = 0, d.tc[2] = 1, d.tc[3] = 1;
		build_decal(&d);
		Con_DPrintf("decal %s at %.0f %.0f %.0f: %d triangles\n", mat->name, org[0], org[1], org[2], d.num / 3);
		Mem_Free(t->v);
		t->mat = mat;
		memcpy(t->key, key, sizeof(key));
		t->v = d.v;
		t->num = d.num;
		found = oldest;
	}
	tcache[found].lastframe = tframecount;
	tframe[numtframe].cache = found;
	VectorCopy(rgb, tframe[numtframe].rgba);
	tframe[numtframe].rgba[3] = alpha;
	numtframe++;
}

void R_ClearTempDecals(void)
{
	numtframe = 0;
	tframecount++;
}

/* --- drawing -------------------------------------------------------------------- */

/* the material's own blend wins; otherwise the effect's */
void R_PickBlend(const material_t *m, int blend, GLenum src, GLenum dst, int *ob, GLenum *os, GLenum *od)
{
	if (m->fromfile && m->blend != BLEND_OPAQUE) {
		*ob = m->blend, *os = m->src, *od = m->dst;
		if (m->blend == BLEND_ALPHA)
			*os = GL_SRC_ALPHA, *od = GL_ONE_MINUS_SRC_ALPHA;
		else if (m->blend == BLEND_ADD)
			*os = GL_SRC_ALPHA, *od = GL_ONE;
		else if (m->blend == BLEND_FILTER)
			*os = GL_DST_COLOR, *od = GL_ZERO;
		*ob = BLEND_CUSTOM;
		return;
	}
	*ob = blend, *os = src, *od = dst;
}

/* colours fade by alpha in blends that ignore the source's alpha */
void R_BlendColor(GLenum src, const float *rgba, float *out)
{
	bool premul = src == GL_ZERO || src == GL_ONE || src == GL_DST_COLOR || src == GL_SRC_COLOR;
	for (int k = 0; k < 3; k++)
		out[k] = premul ? rgba[k] * rgba[3] : rgba[k];
	out[3] = rgba[3];
}

static void draw_one(material_t *mat, int blend, GLenum src, GLenum dst, const rvert_t *v, int num, const float *rgba)
{
	const glprog_t *p = &r_progs[PROG_UNLIT];
	int b;
	GLenum s, d;
	float c[4];
	if (num < 3 || mat->nodraw)
		return;
	R_PickBlend(mat, blend, src, dst, &b, &s, &d);
	if (b == BLEND_OPAQUE)
		b = BLEND_ALPHA, s = GL_SRC_ALPHA, d = GL_ONE_MINUS_SRC_ALPHA;
	R_BlendColor(s, rgba, c);
	GL_BindTexture(0, GL_TEXTURE_2D, mat->diffuse->tex);
	GL_SetBlend(BLEND_CUSTOM, s, d);
	qglUniform4f(p->u_color, c[0], c[1], c[2], c[3]);
	R_StreamDraw(GL_TRIANGLES, v, num, NULL, 0);
}

void R_DrawDecals(void)
{
	const glprog_t *p = &r_progs[PROG_UNLIT];
	if (!r_drawdecals->ivalue || (!numdecals && !numtframe))
		return;
	R_UseProgram(PROG_UNLIT);
	qglUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, r_scene.viewproj);
	qglUniform4f(p->u_tcmod, 0, 0, 1, 1);
	qglUniform1f(p->u_alphatest, 0);
	GL_SetDepth(true, false);
	GL_SetCull(false);
	qglEnable(GL_POLYGON_OFFSET_FILL);
	qglPolygonOffset(-1, -2);
	for (int i = 0; i < numdecals; i++) {
		decal_t *dc = &decals[(decalhead - numdecals + i + MAX_DECALS * 2) % MAX_DECALS];
		float rgba[4], left = (float)(dc->die - r_time);
		if (!dc->v)
			continue;
		memcpy(rgba, dc->rgba, sizeof(rgba));
		rgba[3] -= dc->alphadelta * (float)(r_time - dc->born);
		/* the last second fades out */
		if (left < 1)
			rgba[3] *= BW_MAX(0.0f, left);
		if (rgba[3] <= 0)
			continue;
		draw_one(dc->mat, dc->blend, dc->src, dc->dst, dc->v, dc->num, rgba);
	}
	for (int i = 0; i < numtframe; i++) {
		tdecal_t *t = &tcache[tframe[i].cache];
		draw_one(t->mat, BLEND_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, t->v, t->num, tframe[i].rgba);
	}
	qglDisable(GL_POLYGON_OFFSET_FILL);
	GL_SetCull(true);
	GL_SetDepth(true, true);
}

/* drops kept decals whose time is up */
void R_UpdateDecals(void)
{
	while (numdecals) {
		decal_t *old = &decals[(decalhead - numdecals + MAX_DECALS * 2) % MAX_DECALS];
		if (old->v && old->die > r_time)
			break;
		Mem_Free(old->v);
		old->v = NULL;
		numdecals--;
	}
}

static void decal_count_f(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	Con_Printf("%d decals kept, %d cached for adddecal\n", numdecals, MAX_TEMP_CACHE);
}

void R_InitDecals(void)
{
	r_drawdecals = Cvar_Get("r_drawdecals", "1", CVAR_ARCHIVE, "Draw decals and sprays");
	r_decal_max = Cvar_Get("r_decal_max", "1024", CVAR_ARCHIVE, "Decals kept from effects before the oldest goes");
	Cmd_Add("r_decalinfo", decal_count_f);
}
