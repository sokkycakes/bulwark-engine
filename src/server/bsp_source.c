/* Bulwark engine - Source BSP (VBSP, versions 19 to 21) for collision and
 * entities. Brushes keep their own planes, bevels included, so they feed
 * the same box sweeps as Q3 maps. Each model's brushes are gathered from
 * the leaves under its head node. Displacements and props that have no
 * brushes are turned into extra convex brushes. Layouts: docs/spec/vbsp.md.
 * ISC licence, see LICENSE. */
#include "bsp_source.h"

#include <math.h>

static int32_t rd32(const uint8_t *p) { return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }
static int16_t rds16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static uint16_t rdu16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static float rdf(const uint8_t *p)
{
	int32_t i = rd32(p);
	float f;
	memcpy(&f, &i, 4);
	return f;
}

/* --- hulls ------------------------------------------------------------- */

void SrcHull_Add(srchulls_t *hs, int model, int contents, const float (*pts)[3], int n)
{
	srchull_t *h;
	if (n < 4)
		return;
	if (hs->num == hs->cap) {
		hs->cap = hs->cap ? hs->cap * 2 : 64;
		hs->h = Mem_Realloc(hs->h, sizeof(srchull_t) * hs->cap);
	}
	h = &hs->h[hs->num++];
	h->model = model;
	h->contents = contents;
	h->npts = n;
	h->pts = Mem_Alloc(sizeof(float) * 3 * n);
	memcpy(h->pts, pts, sizeof(float) * 3 * n);
}

void SrcHull_Free(srchulls_t *hs)
{
	for (int i = 0; i < hs->num; i++)
		Mem_Free(hs->h[i].pts);
	Mem_Free(hs->h);
	memset(hs, 0, sizeof(*hs));
}

/* --- building the brush lists ------------------------------------------ */

typedef struct {
	cplane_t *planes;
	int numplanes, capplanes;
	cbrushside_t *sides;
	int numsides, capsides;
	cbrush_t *brushes;
	int numbrushes, capbrushes;
} build_t;

static int add_plane(build_t *b, const float *n, float dist)
{
	if (b->numplanes == b->capplanes) {
		b->capplanes = b->capplanes ? b->capplanes * 2 : 1024;
		b->planes = Mem_Realloc(b->planes, sizeof(cplane_t) * b->capplanes);
	}
	VectorCopy(n, b->planes[b->numplanes].normal);
	b->planes[b->numplanes].dist = dist;
	return b->numplanes++;
}

static void add_side(build_t *b, int plane, int shader)
{
	if (b->numsides == b->capsides) {
		b->capsides = b->capsides ? b->capsides * 2 : 4096;
		b->sides = Mem_Realloc(b->sides, sizeof(cbrushside_t) * b->capsides);
	}
	b->sides[b->numsides].plane = plane;
	b->sides[b->numsides].shader = shader;
	b->numsides++;
}

static cbrush_t *add_brush(build_t *b)
{
	if (b->numbrushes == b->capbrushes) {
		b->capbrushes = b->capbrushes ? b->capbrushes * 2 : 1024;
		b->brushes = Mem_Realloc(b->brushes, sizeof(cbrush_t) * b->capbrushes);
	}
	memset(&b->brushes[b->numbrushes], 0, sizeof(cbrush_t));
	return &b->brushes[b->numbrushes++];
}

#define HULL_EPS 0.01f
#define HULL_MAXPTS 256

typedef struct {
	float n[3], d;
} hplane_t;

static bool have_plane(const hplane_t *pl, int num, const float *n, float d)
{
	for (int i = 0; i < num; i++)
		if (DotProduct(pl[i].n, n) > 0.9999f && fabsf(pl[i].d - d) < 0.05f)
			return true;
	return false;
}

/* the plane through p with normal n bounds the points: all behind it */
static bool bounds_all(const float (*pts)[3], int n, const float *nrm, float d)
{
	for (int i = 0; i < n; i++)
		if (DotProduct(pts[i], nrm) - d > HULL_EPS)
			return false;
	return true;
}

/* A brush from the convex hull of a point set: every supporting plane
 * through three points, then the bevels a box sweep needs at the edges and
 * corners (axial planes at the bounds, and edge-by-axis planes), as the
 * compilers add them to real brushes. */
static void hull_brush(build_t *b, const srchull_t *h)
{
	hplane_t *pl;
	int num = 0, cap = 64, n = BW_MIN(h->npts, HULL_MAXPTS);
	const float(*p)[3] = (const float(*)[3])h->pts;
	float mins[3] = {1e30f, 1e30f, 1e30f}, maxs[3] = {-1e30f, -1e30f, -1e30f};
	cbrush_t *br;

	pl = Mem_Alloc(sizeof(hplane_t) * cap);
	for (int i = 0; i < n; i++)
		for (int k = 0; k < 3; k++) {
			mins[k] = BW_MIN(mins[k], p[i][k]);
			maxs[k] = BW_MAX(maxs[k], p[i][k]);
		}
#define PUSH_PLANE(nn, dd)                                         \
	do {                                                           \
		if (!have_plane(pl, num, (nn), (dd))) {                    \
			if (num == cap)                                        \
				pl = Mem_Realloc(pl, sizeof(hplane_t) * (cap *= 2)); \
			VectorCopy((nn), pl[num].n);                           \
			pl[num].d = (dd);                                      \
			num++;                                                 \
		}                                                          \
	} while (0)
	/* axial planes first: brush bounds are read from them */
	for (int k = 0; k < 3; k++) {
		float nn[3] = {0, 0, 0};
		nn[k] = 1;
		PUSH_PLANE(nn, maxs[k]);
		nn[k] = -1;
		PUSH_PLANE(nn, -mins[k]);
	}
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++)
			for (int k = j + 1; k < n; k++) {
				float e1[3], e2[3], nn[3], d;
				VectorSubtract(p[j], p[i], e1);
				VectorSubtract(p[k], p[i], e2);
				CrossProduct(e1, e2, nn);
				if (VectorLength(nn) < 1e-4f)
					continue;
				VectorNormalize(nn);
				d = DotProduct(nn, p[i]);
				if (bounds_all(p, n, nn, d)) {
					PUSH_PLANE(nn, d);
				} else {
					VectorScale(nn, -1, nn);
					if (bounds_all(p, n, nn, -d))
						PUSH_PLANE(nn, -d);
				}
			}
	/* edge bevels: for each pair of points, planes holding the edge and an axis */
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++) {
			float e[3];
			VectorSubtract(p[j], p[i], e);
			if (VectorLength(e) < 0.5f)
				continue;
			for (int a = 0; a < 3; a++) {
				float ax[3] = {0, 0, 0}, nn[3], d;
				ax[a] = 1;
				CrossProduct(e, ax, nn);
				if (VectorLength(nn) < 1e-4f)
					continue;
				VectorNormalize(nn);
				for (int s = 0; s < 2; s++) {
					d = DotProduct(nn, p[i]);
					if (bounds_all(p, n, nn, d))
						PUSH_PLANE(nn, d);
					VectorScale(nn, -1, nn);
				}
			}
		}
#undef PUSH_PLANE
	br = add_brush(b);
	br->firstside = b->numsides;
	br->numsides = num;
	br->contents = h->contents;
	br->shader = -1;
	br->added = true;
	VectorCopy(mins, br->mins);
	VectorCopy(maxs, br->maxs);
	for (int i = 0; i < num; i++)
		add_side(b, add_plane(b, pl[i].n, pl[i].d), -1);
	Mem_Free(pl);
}

/* --- contents ------------------------------------------------------------ */

static int src_contents(int c)
{
	int out = 0;
	if (c & (SRC_CONTENTS_SOLID | SRC_CONTENTS_WINDOW | SRC_CONTENTS_GRATE))
		out |= CONTENTS_SOLID;
	if (c & SRC_CONTENTS_WATER)
		out |= CONTENTS_WATER;
	if (c & SRC_CONTENTS_SLIME)
		out |= CONTENTS_SLIME;
	if (c & SRC_CONTENTS_PLAYERCLIP)
		out |= CONTENTS_PLAYERCLIP;
	if (c & SRC_CONTENTS_MONSTERCLIP)
		out |= CONTENTS_MONSTERCLIP;
	if (c & SRC_CONTENTS_LADDER)
		out |= CONTENTS_LADDER;
	return out;
}

/* Q3 surface flags that the game reads from traces */
static int src_surfaceflags(int f)
{
	int out = 0;
	if (f & (SRC_SURF_SKY | SRC_SURF_SKY2D))
		out |= 0x4 | 0x10; /* SURF_SKY, SURF_NOIMPACT */
	if (f & SRC_SURF_NODRAW)
		out |= 0x80;
	return out;
}

/* --- displacements ------------------------------------------------------- */

/* The face's corners, starting from the one at the displacement's start
 * position; then the grid of (2^power + 1)^2 points, row by row from
 * corner 0 towards corner 1, each row running from the 0-1 edge to the
 * 3-2 edge, pushed out by the vertex's offset. */
int VBSP_DispPoints(const uint8_t *info, const uint8_t *dverts, int numdverts, const float (*poly)[3], int npoly,
                    float (*out)[3], float (*flat)[3]);

int VBSP_DispPoints(const uint8_t *info, const uint8_t *dverts, int numdverts, const float (*poly)[3], int npoly,
                    float (*out)[3], float (*flat)[3])
{
	float start[3], c[4][3];
	int power = rd32(info + 20), first = rd32(info + 12), size, best = 0;
	float bestd = 1e30f;
	if (npoly != 4 || power < 2 || power > 4)
		return 0;
	size = (1 << power) + 1;
	if (first < 0 || first + size * size > numdverts)
		return 0;
	for (int k = 0; k < 3; k++)
		start[k] = rdf(info + k * 4);
	for (int i = 0; i < 4; i++) {
		float d[3];
		VectorSubtract(poly[i], start, d);
		if (DotProduct(d, d) < bestd) {
			bestd = DotProduct(d, d);
			best = i;
		}
	}
	for (int i = 0; i < 4; i++)
		VectorCopy(poly[(best + i) & 3], c[i]);
	for (int r = 0; r < size; r++) {
		float t = (float)r / (size - 1), a[3], b[3];
		for (int k = 0; k < 3; k++) {
			a[k] = c[0][k] + (c[1][k] - c[0][k]) * t;
			b[k] = c[3][k] + (c[2][k] - c[3][k]) * t;
		}
		for (int col = 0; col < size; col++) {
			float s = (float)col / (size - 1);
			int i = r * size + col;
			const uint8_t *dv = dverts + (size_t)(first + i) * 20;
			float dist = rdf(dv + 12);
			for (int k = 0; k < 3; k++) {
				float base = a[k] + (b[k] - a[k]) * s;
				if (flat)
					flat[i][k] = base;
				out[i][k] = base + rdf(dv + k * 4) * dist;
			}
		}
	}
	return size;
}

/* the polygon of a face, up to max points */
static int face_poly(const uint8_t *face, const uint8_t *verts, size_t nverts, const uint8_t *edges, size_t nedges,
                     const uint8_t *surfedges, size_t nsurfedges, float (*out)[3], int max)
{
	int firstedge = rd32(face + 4), num = rds16(face + 8);
	if (firstedge < 0 || num < 3 || num > max || (size_t)firstedge + num > nsurfedges)
		return 0;
	for (int e = 0; e < num; e++) {
		int se = rd32(surfedges + (size_t)(firstedge + e) * 4), ei = se < 0 ? -se : se, vi;
		if ((size_t)ei >= nedges)
			return 0;
		vi = rdu16(edges + (size_t)ei * 4 + (se < 0 ? 2 : 0));
		if ((size_t)vi >= nverts)
			return 0;
		for (int k = 0; k < 3; k++)
			out[e][k] = rdf(verts + (size_t)vi * 12 + k * 4);
	}
	return num;
}

#define DISP_FLAG_NO_HULL_COLL 0x4
#define DISP_DEPTH 8.0f

/* each displacement triangle becomes a prism reaching DISP_DEPTH below it */
static void disp_hulls(const vbsp_t *v, srchulls_t *hs)
{
	size_t ninfo, nverts, nedges, nsurf, nfaces, ndv, len;
	uint8_t *info = VBSP_Lump(v, VL_DISPINFO, &ninfo), *verts = VBSP_Lump(v, VL_VERTEXES, &nverts);
	uint8_t *edges = VBSP_Lump(v, VL_EDGES, &nedges), *surf = VBSP_Lump(v, VL_SURFEDGES, &nsurf);
	uint8_t *faces = VBSP_Lump(v, VL_FACES, &nfaces), *dv = VBSP_Lump(v, VL_DISP_VERTS, &ndv);
	(void)len;
	if (!faces)
		faces = VBSP_Lump(v, VL_FACES_HDR, &nfaces);
	if (!info || !verts || !edges || !surf || !faces || !dv)
		goto done;
	for (size_t i = 0; i < ninfo / 176; i++) {
		const uint8_t *di = info + i * 176;
		int mintess = rd32(di + 24), face = rdu16(di + 36), size;
		float poly[4][3], pts[17 * 17][3], fn[3], e1[3], e2[3];
		/* with the top bit set, minimum tesselation holds the Hammer flags */
		if (mintess < 0 && ((uint32_t)mintess & DISP_FLAG_NO_HULL_COLL))
			continue;
		if ((size_t)face >= nfaces / 56 ||
		    face_poly(faces + (size_t)face * 56, verts, nverts / 12, edges, nedges / 4, surf, nsurf / 4, poly, 4) != 4)
			continue;
		size = VBSP_DispPoints(di, dv, (int)(ndv / 20), (const float(*)[3])poly, 4, pts, NULL);
		if (!size)
			continue;
		VectorSubtract(poly[1], poly[0], e1);
		VectorSubtract(poly[2], poly[0], e2);
		CrossProduct(e2, e1, fn); /* faces wind clockwise seen from the front */
		VectorNormalize(fn);
		for (int r = 0; r + 1 < size; r++)
			for (int c = 0; c + 1 < size; c++) {
				int a = r * size + c, b = a + 1, cc = a + size, d = cc + 1;
				int tri[2][3];
				/* the diagonal alternates like a checkerboard */
				if (a % 2) {
					int t0[3] = {a, cc, b}, t1[3] = {b, cc, d};
					memcpy(tri[0], t0, sizeof(t0));
					memcpy(tri[1], t1, sizeof(t1));
				} else {
					int t0[3] = {a, cc, d}, t1[3] = {a, d, b};
					memcpy(tri[0], t0, sizeof(t0));
					memcpy(tri[1], t1, sizeof(t1));
				}
				for (int t = 0; t < 2; t++) {
					float prism[6][3];
					for (int k = 0; k < 3; k++) {
						VectorCopy(pts[tri[t][k]], prism[k]);
						VectorMA(pts[tri[t][k]], -DISP_DEPTH, fn, prism[k + 3]);
					}
					SrcHull_Add(hs, 0, CONTENTS_SOLID, (const float(*)[3])prism, 6);
				}
			}
	}
done:
	Mem_Free(info);
	Mem_Free(verts);
	Mem_Free(edges);
	Mem_Free(surf);
	Mem_Free(faces);
	Mem_Free(dv);
}

/* --- the map --------------------------------------------------------------- */

static void brush_bounds_from_sides(const build_t *b, cbrush_t *br)
{
	for (int i = 0; i < 3; i++) {
		br->mins[i] = -99999;
		br->maxs[i] = 99999;
	}
	for (int s = 0; s < br->numsides; s++) {
		const cplane_t *p = &b->planes[b->sides[br->firstside + s].plane];
		for (int i = 0; i < 3; i++) {
			if (p->normal[i] == 1 && p->normal[(i + 1) % 3] == 0 && p->normal[(i + 2) % 3] == 0)
				br->maxs[i] = BW_MIN(br->maxs[i], p->dist);
			else if (p->normal[i] == -1 && p->normal[(i + 1) % 3] == 0 && p->normal[(i + 2) % 3] == 0)
				br->mins[i] = BW_MAX(br->mins[i], -p->dist);
		}
	}
}

/* every brush in the leaves under a node, once */
static void gather_brushes(const bspmap_t *m, int node, uint8_t *seen, int *list, int *num, int depth)
{
	if (depth > 4096)
		return;
	if (node < 0) {
		int li = -(node + 1);
		const cleaf_t *lf;
		if (li >= m->numleafs)
			return;
		lf = &m->leafs[li];
		for (int i = 0; i < lf->numbrushes; i++) {
			int b = m->leafbrushes[lf->firstbrush + i];
			if (b >= 0 && b < m->numbrushes && !seen[b]) {
				seen[b] = 1;
				list[(*num)++] = b;
			}
		}
		return;
	}
	if (node >= m->numnodes)
		return;
	gather_brushes(m, m->nodes[node].children[0], seen, list, num, depth + 1);
	gather_brushes(m, m->nodes[node].children[1], seen, list, num, depth + 1);
}

bool BSP_LoadSource(bspmap_t *m, const uint8_t *d, size_t size)
{
	vbsp_t v;
	size_t len, ntexinfo, ntexdata, nst, nsd;
	uint8_t *lump, *texinfo = NULL, *texdata = NULL, *strtab = NULL, *strdata = NULL;
	int *headnodes = NULL, *order = NULL, nfilebrushes;
	uint8_t *seen = NULL;
	build_t b = {0};
	srchulls_t hulls = {0};
	cbrush_t *filebrushes = NULL;
	bool ok = false;

	if (!VBSP_Open(&v, d, size) || v.version < 19 || v.version > 21)
		return false;

	lump = VBSP_Lump(&v, VL_ENTITIES, &len);
	m->entities = Mem_Alloc(len + 1);
	if (lump)
		memcpy(m->entities, lump, len);
	Mem_Free(lump);
	/* newer maps separate the fields of an output with ESC instead of
	   commas; game code splits on commas (docs/spec/vbsp.md) */
	for (size_t i = 0; i < len; i++)
		if (m->entities[i] == 0x1b)
			m->entities[i] = ',';

	/* shaders: one per texinfo, named after its texdata's material */
	texinfo = VBSP_Lump(&v, VL_TEXINFO, &ntexinfo);
	texdata = VBSP_Lump(&v, VL_TEXDATA, &ntexdata);
	strtab = VBSP_Lump(&v, VL_TEXDATA_STRING_TABLE, &nst);
	strdata = VBSP_Lump(&v, VL_TEXDATA_STRING_DATA, &nsd);
	m->numshaders = (int)(ntexinfo / 72);
	m->shaders = Mem_Alloc(sizeof(cshader_t) * (m->numshaders + 1));
	for (int i = 0; i < m->numshaders; i++) {
		const uint8_t *ti = texinfo + (size_t)i * 72;
		int flags = rd32(ti + 64), td = rd32(ti + 68);
		m->shaders[i].surfaceflags = src_surfaceflags(flags);
		if (td >= 0 && (size_t)td < ntexdata / 32 && strtab && strdata) {
			int si = rd32(texdata + (size_t)td * 32 + 12);
			if (si >= 0 && (size_t)si < nst / 4) {
				int so = rd32(strtab + (size_t)si * 4);
				if (so >= 0 && (size_t)so < nsd)
					Str_Copy(m->shaders[i].name, (const char *)strdata + so,
					         BW_MIN(sizeof(m->shaders[i].name), nsd - so + 1));
			}
		}
	}

	/* planes: normal, dist, type */
	lump = VBSP_Lump(&v, VL_PLANES, &len);
	if (!lump)
		goto fail;
	for (size_t i = 0; i < len / 20; i++) {
		float n[3];
		for (int k = 0; k < 3; k++)
			n[k] = rdf(lump + i * 20 + k * 4);
		add_plane(&b, n, rdf(lump + i * 20 + 12));
	}
	Mem_Free(lump);

	/* brush sides: plane ushort, texinfo short, dispinfo short, bevel short */
	lump = VBSP_Lump(&v, VL_BRUSHSIDES, &len);
	for (size_t i = 0; lump && i < len / 8; i++) {
		int plane = rdu16(lump + i * 8), ti = rds16(lump + i * 8 + 2);
		if (plane >= b.numplanes)
			plane = 0;
		add_side(&b, plane, ti >= 0 && ti < m->numshaders ? ti : -1);
	}
	Mem_Free(lump);

	/* brushes: first side, side count, contents */
	lump = VBSP_Lump(&v, VL_BRUSHES, &len);
	nfilebrushes = (int)(len / 12);
	filebrushes = Mem_Alloc(sizeof(cbrush_t) * (nfilebrushes + 1));
	for (int i = 0; i < nfilebrushes; i++) {
		cbrush_t *br = &filebrushes[i];
		br->firstside = rd32(lump + i * 12);
		br->numsides = rd32(lump + i * 12 + 4);
		br->contents = src_contents(rd32(lump + i * 12 + 8));
		br->shader = br->numsides > 0 && br->firstside >= 0 && br->firstside < b.numsides ? b.sides[br->firstside].shader : -1;
		if (br->firstside < 0 || br->numsides < 0 || br->firstside + br->numsides > b.numsides)
			br->numsides = 0;
		brush_bounds_from_sides(&b, br);
	}
	Mem_Free(lump);
	/* the tree walk below indexes the file's brushes */
	m->brushes = filebrushes;
	m->numbrushes = nfilebrushes;

	/* nodes: plane, children[2], short bounds[6], first face, face count, area, pad */
	lump = VBSP_Lump(&v, VL_NODES, &len);
	m->numnodes = (int)(len / 32);
	m->nodes = Mem_Alloc(sizeof(cnode_t) * (m->numnodes + 1));
	for (int i = 0; i < m->numnodes; i++) {
		const uint8_t *p = lump + (size_t)i * 32;
		cnode_t *n = &m->nodes[i];
		n->plane = rd32(p);
		n->children[0] = rd32(p + 4);
		n->children[1] = rd32(p + 8);
		for (int k = 0; k < 3; k++) {
			n->mins[k] = rds16(p + 12 + k * 2);
			n->maxs[k] = rds16(p + 18 + k * 2);
		}
		if (n->plane < 0 || n->plane >= b.numplanes)
			n->plane = 0;
	}
	Mem_Free(lump);

	/* leaves, version 1 (32 bytes; version 0 adds 24 bytes of ambient light):
	 * contents, cluster short, area:9 flags:7 short, short bounds[6], first
	 * leaf face, leaf face count, first leaf brush, leaf brush count, water id */
	lump = VBSP_Lump(&v, VL_LEAFS, &len);
	{
		int lsize = v.lumps[VL_LEAFS].version == 0 ? 56 : 32;
		m->numleafs = (int)(len / lsize);
		m->leafs = Mem_Alloc(sizeof(cleaf_t) * (m->numleafs + 1));
		for (int i = 0; i < m->numleafs; i++) {
			const uint8_t *p = lump + (size_t)i * lsize;
			cleaf_t *lf = &m->leafs[i];
			lf->contents = src_contents(rd32(p));
			lf->cluster = rds16(p + 4);
			lf->area = rdu16(p + 6) & 0x1ff;
			for (int k = 0; k < 3; k++) {
				lf->mins[k] = rds16(p + 8 + k * 2);
				lf->maxs[k] = rds16(p + 14 + k * 2);
			}
			lf->firstface = rdu16(p + 20);
			lf->numfaces = rdu16(p + 22);
			lf->firstbrush = rdu16(p + 24);
			lf->numbrushes = rdu16(p + 26);
		}
	}
	Mem_Free(lump);
	for (int i = 0; i < m->numnodes; i++)
		for (int c = 0; c < 2; c++)
			if (m->nodes[i].children[c] >= m->numnodes || -(m->nodes[i].children[c] + 1) >= m->numleafs)
				m->nodes[i].children[c] = -1;

	lump = VBSP_Lump(&v, VL_LEAFFACES, &len);
	m->numleaffaces = (int)(len / 2);
	m->leaffaces = Mem_Alloc(sizeof(int) * (m->numleaffaces + 1));
	for (int i = 0; i < m->numleaffaces; i++)
		m->leaffaces[i] = rdu16(lump + (size_t)i * 2);
	Mem_Free(lump);
	lump = VBSP_Lump(&v, VL_LEAFBRUSHES, &len);
	m->numleafbrushes = (int)(len / 2);
	m->leafbrushes = Mem_Alloc(sizeof(int) * (m->numleafbrushes + 1));
	for (int i = 0; i < m->numleafbrushes; i++)
		m->leafbrushes[i] = rdu16(lump + (size_t)i * 2);
	Mem_Free(lump);
	for (int i = 0; i < m->numleafs; i++) {
		cleaf_t *lf = &m->leafs[i];
		if (lf->firstface + lf->numfaces > m->numleaffaces)
			lf->numfaces = 0;
		if (lf->firstbrush + lf->numbrushes > m->numleafbrushes)
			lf->numbrushes = 0;
	}

	/* visibility: cluster count, then a PVS and a PAS offset per cluster;
	 * rows are run-length coded like Quake's (a zero byte, then a count) */
	lump = VBSP_Lump(&v, VL_VISIBILITY, &len);
	if (lump && len >= 4) {
		int nc = rd32(lump);
		if (nc > 0 && nc < 65536 && 4 + (size_t)nc * 8 <= len) {
			int rb = (nc + 7) / 8;
			m->numclusters = nc;
			m->clusterbytes = rb;
			m->vis = Mem_Alloc((size_t)nc * rb);
			for (int c = 0; c < nc; c++) {
				int ofs = rd32(lump + 4 + c * 8);
				uint8_t *row = m->vis + (size_t)c * rb;
				int o = 0;
				if (ofs < 0 || (size_t)ofs >= len) {
					memset(row, 0xff, rb);
					continue;
				}
				while (o < rb && (size_t)ofs < len) {
					if (lump[ofs]) {
						row[o++] = lump[ofs++];
						continue;
					}
					if ((size_t)ofs + 1 >= len)
						break;
					o += lump[ofs + 1];
					ofs += 2;
				}
			}
		}
	}
	Mem_Free(lump);
	for (int i = 0; i < m->numleafs; i++)
		if (m->leafs[i].cluster >= m->numclusters)
			m->leafs[i].cluster = -1;

	/* models: mins, maxs, origin, head node, first face, face count */
	lump = VBSP_Lump(&v, VL_MODELS, &len);
	m->nummodels = (int)(len / 48);
	if (!m->nummodels)
		goto fail;
	m->models = Mem_Alloc(sizeof(cmodel_t) * (m->nummodels + 1));
	headnodes = Mem_Alloc(sizeof(int) * (m->nummodels + 1));
	for (int i = 0; i < m->nummodels; i++) {
		const uint8_t *p = lump + (size_t)i * 48;
		cmodel_t *cm = &m->models[i];
		for (int k = 0; k < 3; k++) {
			cm->mins[k] = rdf(p + k * 4);
			cm->maxs[k] = rdf(p + 12 + k * 4);
		}
		headnodes[i] = rd32(p + 36);
		cm->headnode[0] = headnodes[i];
		cm->firstface = rd32(p + 40);
		cm->numfaces = rd32(p + 44);
	}
	Mem_Free(lump);

	/* extra solids: displacements, then props */
	disp_hulls(&v, &hulls);
	SrcProps_Hulls(&v, m, &hulls);

	/* the final brush list, model by model: the file's brushes found under
	 * the model's head node, then the hulls that move with it */
	seen = Mem_Alloc((size_t)nfilebrushes + 1);
	order = Mem_Alloc(sizeof(int) * (nfilebrushes + 1));
	{
		build_t out = b;
		out.brushes = NULL;
		out.numbrushes = out.capbrushes = 0;
		for (int mi = 0; mi < m->nummodels; mi++) {
			int n = 0;
			gather_brushes(m, headnodes[mi], seen, order, &n, 0);
			m->models[mi].firstbrush = out.numbrushes;
			for (int i = 0; i < n; i++) {
				cbrush_t *br;
				if (!filebrushes[order[i]].contents || !filebrushes[order[i]].numsides)
					continue;
				br = add_brush(&out);
				*br = filebrushes[order[i]];
			}
			for (int h = 0; h < hulls.num; h++)
				if (hulls.h[h].model == mi)
					hull_brush(&out, &hulls.h[h]);
			m->models[mi].numbrushes = out.numbrushes - m->models[mi].firstbrush;
		}
		b = out;
	}
	m->brushes = b.brushes;
	m->numbrushes = b.numbrushes;
	m->planes = b.planes;
	m->numplanes = b.numplanes;
	m->brushsides = b.sides;
	m->numbrushsides = b.numsides;
	b.brushes = NULL;
	b.planes = NULL;
	b.sides = NULL;
	Mem_Free(filebrushes);
	filebrushes = NULL;
	m->type = BSP_Q3;
	m->source = true;
	ok = true;
	Con_DPrintf("%s: Source BSP v%d, %d models, %d brushes (%d added for displacements and props)\n", m->name,
	            v.version, m->nummodels, m->numbrushes, hulls.num);

fail:
	if (!ok) {
		if (m->brushes == filebrushes)
			m->brushes = NULL;
		Mem_Free(filebrushes);
		Mem_Free(b.brushes);
		Mem_Free(b.planes);
		Mem_Free(b.sides);
	}
	SrcHull_Free(&hulls);
	Mem_Free(seen);
	Mem_Free(order);
	Mem_Free(headnodes);
	Mem_Free(texinfo);
	Mem_Free(texdata);
	Mem_Free(strtab);
	Mem_Free(strdata);
	return ok;
}
