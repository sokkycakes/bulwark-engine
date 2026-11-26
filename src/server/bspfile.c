/* Bulwark engine - BSP map loading and brush collision.
 * The Q3 lump directory and record sizes are listed in docs/spec/bsp.md.
 * ISC licence, see LICENSE. */
#include "bspfile.h"
#include "bsp_source.h"
#include "../common/fs.h"
#include "../common/ftew.h"

#include <math.h>

#define DIST_EPSILON 0.03125f
/* planetypes: not along one axis */
#define PLANE_ANY 3

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

enum {
	Q3_ENTITIES = 0, Q3_SHADERS = 1, Q3_PLANES = 2, Q3_NODES = 3, Q3_LEAFS = 4, Q3_LEAFFACES = 5, Q3_LEAFBRUSHES = 6,
	Q3_MODELS = 7, Q3_BRUSHES = 8, Q3_BRUSHSIDES = 9, Q3_VISIBILITY = 16, Q3_NUMLUMPS = 17
};
enum {
	Q1_ENTITIES = 0, Q1_PLANES = 1, Q1_TEXTURES = 2, Q1_VERTEXES = 3, Q1_VISIBILITY = 4, Q1_NODES = 5,
	Q1_TEXINFO = 6, Q1_FACES = 7, Q1_CLIPNODES = 9, Q1_LEAFS = 10, Q1_MARKSURFACES = 11, Q1_EDGES = 12,
	Q1_SURFEDGES = 13, Q1_MODELS = 14, Q1_NUMLUMPS = 15
};

/* Q1 leaf contents */
enum { Q1C_EMPTY = -1, Q1C_SOLID = -2, Q1C_WATER = -3, Q1C_SLIME = -4, Q1C_LAVA = -5, Q1C_SKY = -6, Q1C_CLIP = -8,
       Q1C_LADDER = -16 };

static int q1_contents(int c)
{
	switch (c) {
	case Q1C_SOLID: return CONTENTS_SOLID;
	case Q1C_WATER: return CONTENTS_WATER;
	case Q1C_SLIME: return CONTENTS_SLIME;
	case Q1C_LAVA: return CONTENTS_LAVA;
	case Q1C_SKY: return CONTENTS_SOLID | CONTENTS_SKY;
	case Q1C_CLIP: return CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP;
	case Q1C_LADDER: return CONTENTS_LADDER;
	default: return 0;
	}
}

int BSP_QuakeContentsToBits(int contents) { return q1_contents(contents); }

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
		cm->firstface = rd32(p + 24);
		cm->numfaces = rd32(p + 28);
	}

	/* the node tree and visibility; maps without them still load */
	if (lump_ok(size, l[Q3_NODES], 36) && lump_ok(size, l[Q3_LEAFS], 48) && lump_ok(size, l[Q3_LEAFFACES], 4) &&
	    lump_ok(size, l[Q3_LEAFBRUSHES], 4)) {
		m->numnodes = l[Q3_NODES].len / 36;
		m->nodes = Mem_Alloc(sizeof(cnode_t) * (m->numnodes + 1));
		m->numleafs = l[Q3_LEAFS].len / 48;
		m->leafs = Mem_Alloc(sizeof(cleaf_t) * (m->numleafs + 1));
		for (int i = 0; i < m->numnodes; i++) {
			const uint8_t *p = d + l[Q3_NODES].ofs + i * 36;
			cnode_t *n = &m->nodes[i];
			n->plane = rd32(p);
			n->children[0] = rd32(p + 4);
			n->children[1] = rd32(p + 8);
			for (int k = 0; k < 3; k++) {
				n->mins[k] = (float)rd32(p + 12 + k * 4);
				n->maxs[k] = (float)rd32(p + 24 + k * 4);
			}
			if (n->plane < 0 || n->plane >= m->numplanes)
				n->plane = 0;
			for (int c = 0; c < 2; c++)
				if (n->children[c] >= m->numnodes || -(n->children[c] + 1) >= m->numleafs)
					n->children[c] = -1;
		}
		m->numleaffaces = l[Q3_LEAFFACES].len / 4;
		m->leaffaces = Mem_Alloc(sizeof(int) * (m->numleaffaces + 1));
		for (int i = 0; i < m->numleaffaces; i++)
			m->leaffaces[i] = rd32(d + l[Q3_LEAFFACES].ofs + i * 4);
		m->numleafbrushes = l[Q3_LEAFBRUSHES].len / 4;
		m->leafbrushes = Mem_Alloc(sizeof(int) * (m->numleafbrushes + 1));
		for (int i = 0; i < m->numleafbrushes; i++)
			m->leafbrushes[i] = rd32(d + l[Q3_LEAFBRUSHES].ofs + i * 4);
		for (int i = 0; i < m->numleafs; i++) {
			const uint8_t *p = d + l[Q3_LEAFS].ofs + i * 48;
			cleaf_t *lf = &m->leafs[i];
			lf->cluster = rd32(p);
			lf->area = rd32(p + 4);
			for (int k = 0; k < 3; k++) {
				lf->mins[k] = (float)rd32(p + 8 + k * 4);
				lf->maxs[k] = (float)rd32(p + 20 + k * 4);
			}
			lf->firstface = rd32(p + 32);
			lf->numfaces = rd32(p + 36);
			lf->firstbrush = rd32(p + 40);
			lf->numbrushes = rd32(p + 44);
			if (lf->firstface < 0 || lf->numfaces < 0 || lf->firstface + lf->numfaces > m->numleaffaces)
				lf->numfaces = 0;
			if (lf->firstbrush < 0 || lf->numbrushes < 0 || lf->firstbrush + lf->numbrushes > m->numleafbrushes)
				lf->numbrushes = 0;
		}
		/* visibility: cluster count, bytes per row, then the rows as they are */
		if (lump_ok(size, l[Q3_VISIBILITY], 0) && l[Q3_VISIBILITY].len >= 8) {
			const uint8_t *v = d + l[Q3_VISIBILITY].ofs;
			int nc = rd32(v), rb = rd32(v + 4);
			if (nc > 0 && rb > 0 && rb >= (nc + 7) / 8 && 8 + (int64_t)nc * rb <= l[Q3_VISIBILITY].len) {
				m->numclusters = nc;
				m->clusterbytes = rb;
				m->vis = Mem_Alloc((size_t)nc * rb);
				memcpy(m->vis, v + 8, (size_t)nc * rb);
			}
		}
		for (int i = 0; i < m->numleafs; i++)
			if (m->leafs[i].cluster >= m->numclusters && m->vis)
				m->leafs[i].cluster = -1;
	}
	m->type = BSP_Q3;
	return true;
}

static int16_t rds16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static uint16_t rdu16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* the faces of a Q1 map with their polygons and texture names, for surface
 * queries; collision does not need them, so a map without them still loads */
static void load_q1_faces(bspmap_t *m, const uint8_t *d, const lump_t *l, bool b2, size_t size)
{
	int facesize = b2 ? 28 : 20, edgesize = b2 ? 8 : 4;
	int numverts, numedges, numsurfedges, numtexinfo, nummip, total = 0;
	if (!lump_ok(size, l[Q1_FACES], facesize) || !lump_ok(size, l[Q1_VERTEXES], 12) ||
	    !lump_ok(size, l[Q1_EDGES], edgesize) || !lump_ok(size, l[Q1_SURFEDGES], 4) ||
	    !lump_ok(size, l[Q1_TEXINFO], 40) || !lump_ok(size, l[Q1_TEXTURES], 0))
		return;
	numverts = l[Q1_VERTEXES].len / 12;
	numedges = l[Q1_EDGES].len / edgesize;
	numsurfedges = l[Q1_SURFEDGES].len / 4;
	numtexinfo = l[Q1_TEXINFO].len / 40;
	nummip = l[Q1_TEXTURES].len >= 4 ? rd32(d + l[Q1_TEXTURES].ofs) : 0;
	if (nummip < 0 || (size_t)nummip * 4 + 4 > (size_t)l[Q1_TEXTURES].len)
		nummip = 0;
	m->numshaders = nummip;
	m->shaders = Mem_Alloc(sizeof(cshader_t) * (nummip + 1));
	for (int i = 0; i < nummip; i++) {
		int ofs = rd32(d + l[Q1_TEXTURES].ofs + 4 + i * 4);
		if (ofs < 0 || ofs + 16 > l[Q1_TEXTURES].len)
			continue;
		memcpy(m->shaders[i].name, d + l[Q1_TEXTURES].ofs + ofs, 16);
		m->shaders[i].name[16] = 0;
	}
	m->numfaces = l[Q1_FACES].len / facesize;
	m->faces = Mem_Alloc(sizeof(cface_t) * (m->numfaces + 1));
	for (int i = 0; i < m->numfaces; i++) {
		const uint8_t *p = d + l[Q1_FACES].ofs + i * facesize;
		int n = b2 ? rd32(p + 12) : rdu16(p + 8);
		if (n > 0 && n < 4096)
			total += n;
	}
	m->facevertices = Mem_Alloc(sizeof(float) * 3 * (size_t)(total + 1));
	total = 0;
	for (int i = 0; i < m->numfaces; i++) {
		const uint8_t *p = d + l[Q1_FACES].ofs + i * facesize;
		cface_t *f = &m->faces[i];
		int plane = b2 ? rd32(p) : rdu16(p), side = b2 ? rd32(p + 4) : rdu16(p + 2);
		int first = rd32(b2 ? p + 8 : p + 4), n = b2 ? rd32(p + 12) : rdu16(p + 8);
		int ti = b2 ? rd32(p + 16) : rdu16(p + 10);
		f->plane = plane >= 0 && plane < m->numplanes ? plane : 0;
		f->back = side != 0;
		f->shader = -1;
		if (ti >= 0 && ti < numtexinfo) {
			int mip = rd32(d + l[Q1_TEXINFO].ofs + ti * 40 + 32);
			if (mip >= 0 && mip < nummip)
				f->shader = mip;
		}
		f->firstvert = total;
		for (int k = 0; k < 3; k++) {
			f->mins[k] = 1e30f;
			f->maxs[k] = -1e30f;
		}
		if (n <= 0 || n >= 4096 || first < 0 || first + n > numsurfedges)
			continue;
		for (int e = 0; e < n; e++) {
			int se = rd32(d + l[Q1_SURFEDGES].ofs + (first + e) * 4);
			int edge = se < 0 ? -se : se, v;
			const uint8_t *ep;
			float *out = m->facevertices + 3 * (size_t)total;
			if (edge >= numedges)
				break;
			ep = d + l[Q1_EDGES].ofs + edge * edgesize;
			if (b2)
				v = rd32(ep + (se < 0 ? 4 : 0));
			else
				v = rdu16(ep + (se < 0 ? 2 : 0));
			if (v < 0 || v >= numverts)
				break;
			for (int k = 0; k < 3; k++) {
				out[k] = rdf(d + l[Q1_VERTEXES].ofs + v * 12 + k * 4);
				f->mins[k] = BW_MIN(f->mins[k], out[k]);
				f->maxs[k] = BW_MAX(f->maxs[k], out[k]);
			}
			total++;
			f->numverts++;
		}
	}
}

/* Q1 BSP29 and BSP2: record sizes differ, BSP2 widening shorts to ints and
 * node and leaf bounds to floats. Layouts in docs/spec/bsp.md. */
static bool load_q1(bspmap_t *m, const uint8_t *d, size_t size)
{
	lump_t l[Q1_NUMLUMPS];
	bool b2 = !memcmp(d, "BSP2", 4);
	m->hl = rd32(d) == 30;
	int nodesize = b2 ? 44 : 24, leafsize = b2 ? 44 : 28, clipsize = b2 ? 12 : 8, marksize = b2 ? 4 : 2;
	int visleafs;
	if (size < 4 + Q1_NUMLUMPS * 8)
		return false;
	for (int i = 0; i < Q1_NUMLUMPS; i++) {
		l[i].ofs = rd32(d + 4 + i * 8);
		l[i].len = rd32(d + 8 + i * 8);
	}
	if (!lump_ok(size, l[Q1_ENTITIES], 0) || !lump_ok(size, l[Q1_MODELS], 64) || !lump_ok(size, l[Q1_PLANES], 20) ||
	    !lump_ok(size, l[Q1_NODES], nodesize) || !lump_ok(size, l[Q1_LEAFS], leafsize) ||
	    !lump_ok(size, l[Q1_CLIPNODES], clipsize) || !lump_ok(size, l[Q1_MARKSURFACES], marksize) ||
	    !lump_ok(size, l[Q1_VISIBILITY], 0))
		return false;
	m->bsp2 = b2;
	m->entities = Mem_Alloc((size_t)l[Q1_ENTITIES].len + 1);
	memcpy(m->entities, d + l[Q1_ENTITIES].ofs, (size_t)l[Q1_ENTITIES].len);

	m->numplanes = l[Q1_PLANES].len / 20;
	m->planes = Mem_Alloc(sizeof(cplane_t) * (m->numplanes + 1));
	for (int i = 0; i < m->numplanes; i++) {
		const uint8_t *p = d + l[Q1_PLANES].ofs + i * 20;
		for (int k = 0; k < 3; k++)
			m->planes[i].normal[k] = rdf(p + k * 4);
		m->planes[i].dist = rdf(p + 12);
	}
	/* twice over: the second half says PLANE_ANY for every plane */
	m->planetypes = Mem_Alloc(2 * (size_t)m->numplanes + 1);
	memset(m->planetypes + m->numplanes, PLANE_ANY, (size_t)m->numplanes);
	for (int i = 0; i < m->numplanes; i++) {
		const float *n = m->planes[i].normal;
		m->planetypes[i] = PLANE_ANY;
		for (int k = 0; k < 3; k++)
			if (n[k] == 1 && n[(k + 1) % 3] == 0 && n[(k + 2) % 3] == 0)
				m->planetypes[i] = (uint8_t)k;
	}

	m->nummodels = l[Q1_MODELS].len / 64;
	m->models = Mem_Alloc(sizeof(cmodel_t) * (m->nummodels + 1));
	for (int i = 0; i < m->nummodels; i++) {
		const uint8_t *p = d + l[Q1_MODELS].ofs + i * 64;
		cmodel_t *cm = &m->models[i];
		for (int k = 0; k < 3; k++) {
			cm->mins[k] = rdf(p + k * 4);
			cm->maxs[k] = rdf(p + 12 + k * 4);
		}
		for (int k = 0; k < 4; k++)
			cm->headnode[k] = rd32(p + 36 + k * 4);
		cm->firstface = rd32(p + 56);
		cm->numfaces = rd32(p + 60);
	}
	if (!m->nummodels)
		return false;
	visleafs = rd32(d + l[Q1_MODELS].ofs + 52);

	m->numleafs = l[Q1_LEAFS].len / leafsize;
	m->numleaffaces = l[Q1_MARKSURFACES].len / marksize;
	m->leaffaces = Mem_Alloc(sizeof(int) * (m->numleaffaces + 1));
	for (int i = 0; i < m->numleaffaces; i++) {
		const uint8_t *p = d + l[Q1_MARKSURFACES].ofs + i * marksize;
		m->leaffaces[i] = b2 ? rd32(p) : rdu16(p);
	}
	m->leafs = Mem_Alloc(sizeof(cleaf_t) * (m->numleafs + 1));
	visleafs = BW_MAX(0, BW_MIN(visleafs, m->numleafs - 1));
	m->numclusters = visleafs;
	m->clusterbytes = (visleafs + 7) / 8;
	if (visleafs && l[Q1_VISIBILITY].len)
		m->vis = Mem_Alloc((size_t)visleafs * m->clusterbytes + 1);
	for (int i = 0; i < m->numleafs; i++) {
		const uint8_t *p = d + l[Q1_LEAFS].ofs + i * leafsize;
		cleaf_t *lf = &m->leafs[i];
		int visofs = rd32(p + 4);
		lf->contents = q1_contents(rd32(p));
		for (int k = 0; k < 3; k++) {
			lf->mins[k] = b2 ? rdf(p + 8 + k * 4) : rds16(p + 8 + k * 2);
			lf->maxs[k] = b2 ? rdf(p + 20 + k * 4) : rds16(p + 14 + k * 2);
		}
		lf->firstface = b2 ? rd32(p + 32) : rdu16(p + 20);
		lf->numfaces = b2 ? rd32(p + 36) : rdu16(p + 22);
		if (lf->firstface < 0 || lf->numfaces < 0 || lf->firstface + lf->numfaces > m->numleaffaces)
			lf->numfaces = 0;
		/* leaf 0 is the shared solid leaf; the rest are clusters */
		lf->cluster = i >= 1 && i <= visleafs ? i - 1 : -1;
		if (m->vis && lf->cluster >= 0) {
			uint8_t *row = m->vis + (size_t)lf->cluster * m->clusterbytes;
			if (visofs < 0 || visofs >= l[Q1_VISIBILITY].len) {
				memset(row, 0xff, m->clusterbytes);
			} else {
				/* run-length coded: a zero byte is followed by a count of zero bytes */
				const uint8_t *v = d + l[Q1_VISIBILITY].ofs + visofs, *end = d + l[Q1_VISIBILITY].ofs + l[Q1_VISIBILITY].len;
				for (int o = 0; o < m->clusterbytes && v < end;) {
					if (*v) {
						row[o++] = *v++;
					} else if (v + 1 < end) {
						o += v[1];
						v += 2;
					} else {
						break;
					}
				}
			}
		}
	}

	m->numnodes = l[Q1_NODES].len / nodesize;
	m->nodes = Mem_Alloc(sizeof(cnode_t) * (m->numnodes + 1));
	m->hull0 = Mem_Alloc(sizeof(cclipnode_t) * (m->numnodes + 1));
	for (int i = 0; i < m->numnodes; i++) {
		const uint8_t *p = d + l[Q1_NODES].ofs + i * nodesize;
		cnode_t *n = &m->nodes[i];
		n->plane = rd32(p);
		if (n->plane < 0 || n->plane >= m->numplanes)
			n->plane = 0;
		for (int c = 0; c < 2; c++) {
			/* BSP29 children are shorts; a negative one is -(leaf + 1) */
			int ch = b2 ? rd32(p + 4 + c * 4) : rds16(p + 4 + c * 2);
			if (!b2 && ch >= 0 && ch >= m->numnodes)
				ch = (int)(uint16_t)ch - 65536; /* maps with more than 32767 leafs */
			if (ch >= m->numnodes || -(ch + 1) >= m->numleafs)
				ch = -1;
			n->children[c] = ch;
			m->hull0[i].children[c] = ch >= 0 ? ch : (-(ch + 1) == 0 ? Q1C_SOLID : 0);
		}
		for (int k = 0; k < 3; k++) {
			n->mins[k] = b2 ? rdf(p + 12 + k * 4) : rds16(p + 8 + k * 2);
			n->maxs[k] = b2 ? rdf(p + 24 + k * 4) : rds16(p + 14 + k * 2);
		}
		m->hull0[i].plane = n->plane;
	}
	/* hull 0 leaf children carry the Q1 contents of the leaf */
	for (int i = 0; i < m->numnodes; i++)
		for (int c = 0; c < 2; c++)
			if (m->nodes[i].children[c] < 0) {
				int leaf = -(m->nodes[i].children[c] + 1);
				int q1c = rd32(d + l[Q1_LEAFS].ofs + leaf * leafsize);
				m->hull0[i].children[c] = q1c < 0 ? q1c : Q1C_SOLID;
			}

	m->numclipnodes = l[Q1_CLIPNODES].len / clipsize;
	m->clipnodes = Mem_Alloc(sizeof(cclipnode_t) * (m->numclipnodes + 1));
	for (int i = 0; i < m->numclipnodes; i++) {
		const uint8_t *p = d + l[Q1_CLIPNODES].ofs + i * clipsize;
		cclipnode_t *c = &m->clipnodes[i];
		c->plane = rd32(p);
		if (c->plane < 0 || c->plane >= m->numplanes)
			c->plane = 0;
		for (int k = 0; k < 2; k++) {
			int ch = b2 ? rd32(p + 4 + k * 4) : rds16(p + 4 + k * 2);
			if (!b2 && ch >= m->numclipnodes)
				ch = (int)(uint16_t)ch - 65536;
			if (ch >= m->numclipnodes)
				ch = Q1C_SOLID;
			c->children[k] = ch;
		}
	}
	load_q1_faces(m, d, l, b2, size);
	m->type = BSP_Q1;
	return true;
}

/* A void map (maps/<name>.vxmap) is just an entity lump in a text file: a
 * world with no brushes, used by games that build their level at run time
 * out of voxels (docs/spec/voxel.md). It has one model so the world entity
 * and model index 1 exist, with bounds that cover the voxel range. */
static bool is_void_map(const char *path)
{
	size_t n = strlen(path);
	return n > 6 && !Str_ICmp(path + n - 6, ".vxmap");
}

static void load_void(bspmap_t *m, const uint8_t *d, size_t size)
{
	m->type = BSP_NONE;
	m->entities = Mem_Alloc(size + 1);
	memcpy(m->entities, d, size);
	m->nummodels = 1;
	m->models = Mem_Alloc(sizeof(cmodel_t));
	for (int i = 0; i < 3; i++) {
		m->models[0].mins[i] = -32768;
		m->models[0].maxs[i] = 32768;
	}
}

/* --- FTEW worlds ----------------------------------------------------- */

/* FTEW (docs/spec/ftew.md) stores convex colliders as planes, which are
 * brushes already, and marks mesh instances collidable. Each collidable
 * triangle becomes a thin brush: its face, a back face, three edge planes
 * and axial bevels so box sweeps do not catch on its corners. */

#define FTEW_TRI_THICKNESS 2.0f

typedef struct {
	bspmap_t *m;
	int maxplanes, maxsides, maxbrushes;
} brushbuild_t;

static int add_plane(brushbuild_t *bb, const float *n, float d)
{
	bspmap_t *m = bb->m;
	if (m->numplanes == bb->maxplanes) {
		bb->maxplanes = bb->maxplanes ? bb->maxplanes * 2 : 1024;
		m->planes = Mem_Realloc(m->planes, sizeof(cplane_t) * (size_t)bb->maxplanes);
	}
	VectorCopy(n, m->planes[m->numplanes].normal);
	m->planes[m->numplanes].dist = d;
	return m->numplanes++;
}

static void add_side(brushbuild_t *bb, const float *n, float d)
{
	bspmap_t *m = bb->m;
	if (m->numbrushsides == bb->maxsides) {
		bb->maxsides = bb->maxsides ? bb->maxsides * 2 : 1024;
		m->brushsides = Mem_Realloc(m->brushsides, sizeof(cbrushside_t) * (size_t)bb->maxsides);
	}
	m->brushsides[m->numbrushsides].plane = add_plane(bb, n, d);
	m->brushsides[m->numbrushsides].shader = 0;
	m->numbrushsides++;
}

static cbrush_t *begin_brush(brushbuild_t *bb)
{
	bspmap_t *m = bb->m;
	cbrush_t *b;
	if (m->numbrushes == bb->maxbrushes) {
		bb->maxbrushes = bb->maxbrushes ? bb->maxbrushes * 2 : 256;
		m->brushes = Mem_Realloc(m->brushes, sizeof(cbrush_t) * (size_t)bb->maxbrushes);
	}
	b = &m->brushes[m->numbrushes++];
	memset(b, 0, sizeof(*b));
	b->firstside = m->numbrushsides;
	b->contents = CONTENTS_SOLID;
	return b;
}

static void cross(const float *a, const float *b, float *out)
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static void add_triangle_brush(brushbuild_t *bb, const float *a, const float *b, const float *c)
{
	const float *v[3] = {a, b, c};
	float e1[3], e2[3], n[3], back[3], d;
	cbrush_t *br;
	for (int k = 0; k < 3; k++) {
		e1[k] = c[k] - a[k];
		e2[k] = b[k] - a[k];
	}
	/* clockwise front faces: the front normal is (c - a) x (b - a) */
	cross(e1, e2, n);
	if (VectorNormalize(n) < 1e-4f)
		return; /* degenerate */
	br = begin_brush(bb);
	d = DotProduct(n, a);
	add_side(bb, n, d);
	for (int k = 0; k < 3; k++)
		back[k] = -n[k];
	add_side(bb, back, -(d - FTEW_TRI_THICKNESS));
	for (int i = 0; i < 3; i++) {
		const float *p0 = v[i], *p1 = v[(i + 1) % 3], *p2 = v[(i + 2) % 3];
		float edge[3], en[3];
		for (int k = 0; k < 3; k++)
			edge[k] = p1[k] - p0[k];
		cross(edge, n, en);
		if (VectorNormalize(en) < 1e-6f)
			continue;
		if (DotProduct(en, p2) > DotProduct(en, p0)) /* face away from the opposite corner */
			for (int k = 0; k < 3; k++)
				en[k] = -en[k];
		add_side(bb, en, DotProduct(en, p0));
	}
	for (int k = 0; k < 3; k++) {
		br->mins[k] = BW_MIN(BW_MIN(a[k], b[k]), c[k]);
		br->maxs[k] = BW_MAX(BW_MAX(a[k], b[k]), c[k]);
		/* the back face sits behind the triangle */
		if (n[k] > 0)
			br->mins[k] -= n[k] * FTEW_TRI_THICKNESS;
		else
			br->maxs[k] -= n[k] * FTEW_TRI_THICKNESS;
	}
	for (int k = 0; k < 3; k++) {
		float ax[3] = {0, 0, 0};
		ax[k] = 1;
		add_side(bb, ax, br->maxs[k]);
		ax[k] = -1;
		add_side(bb, ax, -br->mins[k]);
	}
	br->numsides = bb->m->numbrushsides - br->firstside;
}

static void add_collider_brush(brushbuild_t *bb, const ftew_collider_t *c)
{
	cbrush_t *br = begin_brush(bb);
	for (uint32_t i = 0; i < c->numplanes; i++) {
		float n[3], d;
		FTEW_Plane(c, i, n, &d);
		add_side(bb, n, d);
	}
	VectorCopy(c->mins, br->mins);
	VectorCopy(c->maxs, br->maxs);
	br->numsides = bb->m->numbrushsides - br->firstside;
}

static bool load_ftew(bspmap_t *m, const uint8_t *d, size_t size)
{
	ftew_t w;
	char err[128];
	brushbuild_t bb = {m, 0, 0, 0};
	if (!FTEW_Parse(d, size, &w, err, sizeof(err))) {
		Con_Printf("%s: %s\n", m->name, err);
		return false;
	}
	m->entities = Mem_Strdup(w.entities);
	m->numshaders = 1;
	m->shaders = Mem_Alloc(sizeof(cshader_t) * 2);
	Str_Copy(m->shaders[0].name, "ftew/solid", sizeof(m->shaders[0].name));
	m->shaders[0].contents = CONTENTS_SOLID;
	m->nummodels = (int)w.nummodels;
	m->models = Mem_Alloc(sizeof(cmodel_t) * (w.nummodels + 1));

	/* brushes grouped by model, so each model is one range */
	for (uint32_t mi = 0; mi < w.nummodels; mi++) {
		cmodel_t *cm = &m->models[mi];
		cm->firstbrush = m->numbrushes;
		for (int i = 0; i < w.numcolliders; i++)
			if (w.colliders[i].model == mi)
				add_collider_brush(&bb, &w.colliders[i]);
		for (int i = 0; i < w.numinstances; i++) {
			const ftew_instance_t *in = &w.instances[i];
			const ftew_mesh_t *mesh;
			if (in->model != mi || !(in->flags & FTEW_INST_COLLIDE))
				continue;
			mesh = &w.meshes[in->mesh];
			for (uint32_t t = 0; t + 2 < mesh->numindexes; t += 3) {
				float p[3][3];
				for (int k = 0; k < 3; k++) {
					FTEW_Vertex(mesh, FTEW_Index(mesh, t + k), p[k], NULL, NULL);
					FTEW_Transform(in, p[k], p[k]);
				}
				if (FTEW_Mirrored(in))
					add_triangle_brush(&bb, p[0], p[2], p[1]);
				else
					add_triangle_brush(&bb, p[0], p[1], p[2]);
			}
		}
		cm->numbrushes = m->numbrushes - cm->firstbrush;
		/* bounds: collision and visible geometry */
		for (int k = 0; k < 3; k++) {
			cm->mins[k] = 1e30f;
			cm->maxs[k] = -1e30f;
		}
		for (int i = cm->firstbrush; i < m->numbrushes; i++)
			for (int k = 0; k < 3; k++) {
				cm->mins[k] = BW_MIN(cm->mins[k], m->brushes[i].mins[k]);
				cm->maxs[k] = BW_MAX(cm->maxs[k], m->brushes[i].maxs[k]);
			}
		for (int i = 0; i < w.numinstances; i++) {
			const ftew_instance_t *in = &w.instances[i];
			const ftew_mesh_t *mesh = &w.meshes[in->mesh];
			if (in->model != mi)
				continue;
			for (uint32_t v = 0; v < mesh->numverts; v++) {
				float p[3];
				FTEW_Vertex(mesh, v, p, NULL, NULL);
				FTEW_Transform(in, p, p);
				for (int k = 0; k < 3; k++) {
					cm->mins[k] = BW_MIN(cm->mins[k], p[k]);
					cm->maxs[k] = BW_MAX(cm->maxs[k], p[k]);
				}
			}
		}
		if (cm->mins[0] > cm->maxs[0])
			for (int k = 0; k < 3; k++)
				cm->mins[k] = cm->maxs[k] = 0;
	}
	FTEW_Free(&w);
	m->type = BSP_Q3; /* brush collision, like Q3-family maps */
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
	if (is_void_map(path)) {
		load_void(m, d, size);
		Mem_Free(d);
		return m;
	}
	if (size >= 8 && (!memcmp(d, "IBSP", 4) || !memcmp(d, "FBSP", 4) || !memcmp(d, "RBSP", 4)))
		ok = load_q3(m, d, size);
	else if (size >= 8 && !memcmp(d, "VBSP", 4))
		ok = BSP_LoadSource(m, d, size);
	else if (FTEW_IsFTEW(d, size))
		ok = load_ftew(m, d, size);
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
	Mem_Free(m->planetypes);
	Mem_Free(m->brushsides);
	Mem_Free(m->brushes);
	Mem_Free(m->shaders);
	Mem_Free(m->models);
	Mem_Free(m->nodes);
	Mem_Free(m->leafs);
	Mem_Free(m->leaffaces);
	Mem_Free(m->leafbrushes);
	Mem_Free(m->vis);
	Mem_Free(m->clipnodes);
	Mem_Free(m->hull0);
	Mem_Free(m->faces);
	Mem_Free(m->facevertices);
	Mem_Free(m->cache);
	Mem_Free(m);
}

/* --- surface queries ------------------------------------------------- */

/* squared distance from p to a convex polygon */
static float polygon_distance(const float *verts, int n, const float *normal, const float *p)
{
	float d = DotProduct(p, normal) - DotProduct(verts, normal), best = 1e30f;
	int pos = 0, neg = 0;
	/* inside when p is on the same side of every edge, whichever way the
	 * polygon winds */
	for (int i = 0; i < n; i++) {
		const float *a = verts + i * 3, *b = verts + ((i + 1) % n) * 3;
		float e[3], c[3], ap[3], s;
		VectorSubtract(b, a, e);
		VectorSubtract(p, a, ap);
		CrossProduct(e, normal, c);
		s = DotProduct(ap, c);
		pos += s > 0;
		neg += s < 0;
	}
	if (!pos || !neg)
		return d * d;
	for (int i = 0; i < n; i++) {
		const float *a = verts + i * 3, *b = verts + ((i + 1) % n) * 3;
		float e[3], ap[3], q[3], t, len = 0, dd;
		VectorSubtract(b, a, e);
		VectorSubtract(p, a, ap);
		len = DotProduct(e, e);
		t = len > 0 ? DotProduct(ap, e) / len : 0;
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		for (int k = 0; k < 3; k++)
			q[k] = a[k] + e[k] * t - p[k];
		dd = DotProduct(q, q);
		if (dd < best)
			best = dd;
	}
	return best;
}

int BSP_SurfaceNearPoint(const bspmap_t *map, int model, const float *p)
{
	const cmodel_t *cm;
	int best = -1;
	float bestd = 1e30f;
	if (!map || model < 0 || model >= map->nummodels)
		return -1;
	cm = &map->models[model];
	if (map->type == BSP_Q1) {
		const float pad = 64;
		if (!map->faces)
			return -1;
		for (int i = 0; i < cm->numfaces; i++) {
			int fi = cm->firstface + i;
			const cface_t *f;
			float n[3], d;
			if (fi < 0 || fi >= map->numfaces)
				break;
			f = &map->faces[fi];
			if (f->numverts < 3 || p[0] < f->mins[0] - pad || p[0] > f->maxs[0] + pad || p[1] < f->mins[1] - pad ||
			    p[1] > f->maxs[1] + pad || p[2] < f->mins[2] - pad || p[2] > f->maxs[2] + pad)
				continue;
			VectorCopy(map->planes[f->plane].normal, n);
			if (f->back)
				VectorScale(n, -1, n);
			d = polygon_distance(map->facevertices + 3 * (size_t)f->firstvert, f->numverts, n, p);
			if (d < bestd) {
				bestd = d;
				best = i;
			}
		}
		return best;
	}
	/* brush maps: the side of a nearby brush that the point is closest to */
	for (int i = 0; i < cm->numbrushes; i++) {
		const cbrush_t *b = &map->brushes[cm->firstbrush + i];
		const float pad = 16;
		int side = -1;
		float outmost = -1e30f;
		if (p[0] < b->mins[0] - pad || p[0] > b->maxs[0] + pad || p[1] < b->mins[1] - pad ||
		    p[1] > b->maxs[1] + pad || p[2] < b->mins[2] - pad || p[2] > b->maxs[2] + pad)
			continue;
		for (int s = 0; s < b->numsides; s++) {
			const cplane_t *pl = &map->planes[map->brushsides[b->firstside + s].plane];
			float d = DotProduct(p, pl->normal) - pl->dist;
			if (d > outmost) {
				outmost = d;
				side = b->firstside + s;
			}
		}
		if (side >= 0 && outmost <= pad && fabsf(outmost) < bestd) {
			bestd = fabsf(outmost);
			best = side;
		}
	}
	return best;
}

const char *BSP_SurfaceTexture(const bspmap_t *map, int model, int surface)
{
	int shader = -1;
	if (!map || surface < 0)
		return "";
	if (map->type == BSP_Q1) {
		if (model < 0 || model >= map->nummodels)
			return "";
		surface += map->models[model].firstface;
		if (surface < map->numfaces)
			shader = map->faces[surface].shader;
	} else if (surface < map->numbrushsides) {
		shader = map->brushsides[surface].shader;
	}
	return shader >= 0 && shader < map->numshaders ? map->shaders[shader].name : "";
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

/* --- Q1 hull collision -------------------------------------------------- */

/* the player and large hulls are the world grown by these boxes */
static const float hull_mins[3][3] = {{0, 0, 0}, {-16, -16, -24}, {-32, -32, -24}};
/* GoldSrc: standing player, large, crouching player */
static const float hl_hull_mins[4][3] = {{0, 0, 0}, {-16, -16, -36}, {-32, -32, -32}, {-16, -16, -18}};

/* A plane whose normal is exactly +x, +y or +z (most of a Q1 map's) is
 * tested on that one coordinate. For finite points that is bit for bit the
 * dot product: the other terms are exact zeros, and the sign a zero result
 * may differ in never changes a comparison or a division below. Points
 * beyond PLANE_FAST_LIMIT (or NaN) take the dot product, as inf * 0 would. */
#define PLANE_FAST_LIMIT 1e30f

static bool fast_point(const float *p)
{
	return fabsf(p[0]) <= PLANE_FAST_LIMIT && fabsf(p[1]) <= PLANE_FAST_LIMIT && fabsf(p[2]) <= PLANE_FAST_LIMIT;
}

static const uint8_t *plane_types(const bspmap_t *map, bool fast)
{
	return fast ? map->planetypes : map->planetypes + map->numplanes;
}

static inline float plane_diff(const cplane_t *pl, int type, const float *p)
{
	return type < PLANE_ANY ? p[type] - pl->dist : DotProduct(p, pl->normal) - pl->dist;
}

/* types: the map's planetypes, or plane_types' second half to take the dot
 * product everywhere */
static int hull_contents(const cclipnode_t *nodes, const cplane_t *planes, const uint8_t *types, int num,
                         const float *p)
{
	int guard = 0;
	while (num >= 0 && guard++ < 65536) {
		int pn = nodes[num].plane;
		num = nodes[num].children[plane_diff(&planes[pn], types[pn], p) < 0];
	}
	return num;
}

typedef struct {
	const cclipnode_t *nodes;
	const cplane_t *planes;
	const uint8_t *types; /* see hull_contents */
	int head;
	bsptrace_t *tr;
	bool allsolid, startsolid;
	int depth;
} hullwork_t;

/* sweeps p1-p2 (fractions f1-f2 of the whole move) down the hull; false
 * once something was hit, which ends the walk. Going down one side, and on
 * to the far side of a crossing, loop rather than recurse. */
static bool hull_trace(hullwork_t *w, int num, float f1, float f2, const float *p1, const float *p2)
{
	const cplane_t *pl;
	float t1, t2, frac, midf, mid[3], from[3];
	int side, type, pn;
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
		pn = w->nodes[num].plane;
		pl = &w->planes[pn];
		type = w->types[pn];
		t1 = plane_diff(pl, type, p1);
		t2 = plane_diff(pl, type, p2);
		if (t1 >= 0 && t2 >= 0) {
			num = w->nodes[num].children[0];
			continue;
		}
		if (t1 < 0 && t2 < 0) {
			num = w->nodes[num].children[1];
			continue;
		}
		/* the crossing, kept a hair on the near side */
		frac = t1 < 0 ? (t1 + DIST_EPSILON) / (t1 - t2) : (t1 - DIST_EPSILON) / (t1 - t2);
		frac = BW_MAX(0, BW_MIN(frac, 1));
		side = t1 < 0;
		midf = f1 + (f2 - f1) * frac;
		for (int k = 0; k < 3; k++)
			mid[k] = p1[k] + (p2[k] - p1[k]) * frac;
		if (!hull_trace(w, w->nodes[num].children[side], f1, midf, p1, mid))
			return false;
		if (hull_contents(w->nodes, w->planes, w->types, w->nodes[num].children[side ^ 1], mid) != Q1C_SOLID) {
			num = w->nodes[num].children[side ^ 1];
			f1 = midf;
			VectorCopy(mid, from);
			p1 = from;
			continue;
		}
		break;
	}
	if (w->allsolid)
		return false; /* never got out of the solid */
	/* hit the plane going into the far side */
	if (!side) {
		VectorCopy(pl->normal, w->tr->normal);
		w->tr->dist = pl->dist;
	} else {
		for (int k = 0; k < 3; k++)
			w->tr->normal[k] = -pl->normal[k];
		w->tr->dist = -pl->dist;
	}
	/* rounding can leave mid inside: step back towards p1 until it isn't */
	while (hull_contents(w->nodes, w->planes, w->types, w->head, mid) == Q1C_SOLID) {
		frac -= 0.1f;
		if (frac < 0) {
			w->tr->fraction = f1;
			VectorCopy(p1, w->tr->endpos);
			return false;
		}
		midf = f1 + (f2 - f1) * frac;
		for (int k = 0; k < 3; k++)
			mid[k] = p1[k] + (p2[k] - p1[k]) * frac;
	}
	w->tr->fraction = midf;
	VectorCopy(mid, w->tr->endpos);
	return false;
}

static void q1_trace(const bspmap_t *map, int model, const float *origin, const float *start, const float *mins,
                     const float *maxs, const float *end, bsptrace_t *tr)
{
	const cmodel_t *cm = &map->models[model];
	hullwork_t w;
	float off[3], p1[3], p2[3], size = maxs[0] - mins[0];
	int hull = size < 3 ? 0 : size <= 32 ? 1 : 2;
	const float (*hm)[3] = hull_mins;
	if (map->hl) {
		hull = size <= 8 ? 0 : size <= 36 ? (maxs[2] - mins[2] <= 36 ? 3 : 1) : 2;
		hm = hl_hull_mins;
	}
	memset(&w, 0, sizeof(w));
	w.nodes = hull ? map->clipnodes : map->hull0;
	w.planes = map->planes;
	w.head = cm->headnode[hull];
	w.tr = tr;
	w.allsolid = true;
	if (!w.nodes || (hull ? map->numclipnodes : map->numnodes) == 0)
		return;
	/* the hull's box stands in for the mover's; line up their bottoms */
	for (int k = 0; k < 3; k++) {
		off[k] = (hull ? hm[hull][k] : 0) - (hull ? mins[k] : 0) + origin[k];
		p1[k] = start[k] - off[k];
		p2[k] = end[k] - off[k];
	}
	tr->fraction = 1;
	VectorCopy(p2, tr->endpos);
	w.types = plane_types(map, fast_point(p1) && fast_point(p2));
	hull_trace(&w, w.head, 0, 1, p1, p2);
	tr->startsolid = w.startsolid;
	tr->allsolid = w.allsolid;
	if (w.allsolid) {
		tr->fraction = 0;
		VectorCopy(p1, tr->endpos);
	}
	if (tr->fraction < 1 || w.startsolid)
		tr->contents = CONTENTS_SOLID;
	for (int k = 0; k < 3; k++)
		tr->endpos[k] += off[k];
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
	if (!map || model < 0 || model >= map->nummodels)
		return;
	if (map->type == BSP_Q1) {
		q1_trace(map, model, origin, start, mins, maxs, end, tr);
		return;
	}
	if (map->type != BSP_Q3)
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
	if (map && map->type == BSP_Q1 && model >= 0 && model < map->nummodels) {
		for (int i = 0; i < 3; i++)
			lp[i] = p[i] - origin[i];
		return q1_contents(hull_contents(map->hull0, map->planes, plane_types(map, fast_point(lp)),
		                                 map->models[model].headnode[0], lp));
	}
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

/* --- remembered world queries ----------------------------------------- */

/* Direct mapped, keyed on the raw bits of every input, so -0 and +0 or two
 * NaNs are different keys and a hit is always the answer the query would
 * give. Most repeats come a tick or more apart, so the tables hold a few
 * ticks' worth. */
#ifdef BW_LOWMEM
#define TRACE_CACHE_BITS 11 /* 2048 * 112 bytes */
#define POINT_CACHE_BITS 11
#else
#define TRACE_CACHE_BITS 13
#define POINT_CACHE_BITS 13
#endif

typedef struct {
	uint32_t key[13]; /* start, end, mins, maxs, mask */
	uint32_t valid;
	bsptrace_t tr;
} tracecache_t;

typedef struct {
	uint32_t key[3];
	int32_t contents;
	uint32_t valid;
} pointcache_t;

struct bspcache_s {
	tracecache_t traces[1 << TRACE_CACHE_BITS];
	pointcache_t points[1 << POINT_CACHE_BITS];
};

static struct bspcache_s *world_cache(bspmap_t *map)
{
	if (!map->cache)
		map->cache = Mem_Alloc(sizeof(*map->cache)); /* zeroed: nothing valid */
	return map->cache;
}

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

static uint32_t key_hash(const uint32_t *k, int n)
{
	uint32_t h = 0;
	for (int i = 0; i < n; i++)
		h = (h ^ k[i]) * 0x9e3779b1u;
	return h ^ (h >> 16);
}

void BSP_TraceWorld(bspmap_t *map, const float *start, const float *mins, const float *maxs, const float *end,
                    int mask, bsptrace_t *tr)
{
	static const float zero[3] = {0, 0, 0};
	uint32_t key[13];
	tracecache_t *c;
	if (!map) {
		BSP_TraceModel(map, 0, zero, start, mins, maxs, end, mask, tr);
		return;
	}
	for (int k = 0; k < 3; k++) {
		key[k] = fbits(start[k]);
		key[3 + k] = fbits(end[k]);
		key[6 + k] = fbits(mins[k]);
		key[9 + k] = fbits(maxs[k]);
	}
	key[12] = (uint32_t)mask;
	c = &world_cache(map)->traces[key_hash(key, 13) & ((1u << TRACE_CACHE_BITS) - 1)];
	if (c->valid) {
		int i = 0;
		while (i < 13 && c->key[i] == key[i])
			i++;
		if (i == 13) {
			*tr = c->tr;
			return;
		}
	}
	BSP_TraceModel(map, 0, zero, start, mins, maxs, end, mask, tr);
	for (int k = 0; k < 13; k++)
		c->key[k] = key[k];
	c->valid = 1;
	c->tr = *tr;
}

int BSP_WorldContents(bspmap_t *map, const float *p)
{
	static const float zero[3] = {0, 0, 0};
	uint32_t key[3];
	pointcache_t *c;
	if (!map)
		return 0;
	key[0] = fbits(p[0]);
	key[1] = fbits(p[1]);
	key[2] = fbits(p[2]);
	c = &world_cache(map)->points[key_hash(key, 3) & ((1u << POINT_CACHE_BITS) - 1)];
	if (c->valid && c->key[0] == key[0] && c->key[1] == key[1] && c->key[2] == key[2])
		return c->contents;
	c->contents = BSP_PointContents(map, 0, zero, p);
	c->key[0] = key[0];
	c->key[1] = key[1];
	c->key[2] = key[2];
	c->valid = 1;
	return c->contents;
}

/* --- visibility ------------------------------------------------------- */

int BSP_PointLeaf(const bspmap_t *map, const float *p)
{
	int num = 0, guard = 0;
	if (!map || !map->numnodes || !map->numleafs)
		return -1;
	if (map->type == BSP_Q1 && map->nummodels)
		num = map->models[0].headnode[0];
	while (num >= 0 && guard++ < 65536) {
		const cnode_t *n = &map->nodes[num];
		const cplane_t *pl = &map->planes[n->plane];
		num = n->children[DotProduct(p, pl->normal) - pl->dist < 0];
	}
	num = -(num + 1);
	return num >= 0 && num < map->numleafs ? num : -1;
}

const uint8_t *BSP_ClusterPVS(const bspmap_t *map, int cluster)
{
	if (!map || !map->vis || cluster < 0 || cluster >= map->numclusters)
		return NULL;
	return map->vis + (size_t)cluster * map->clusterbytes;
}

bool BSP_CheckPVS(const bspmap_t *map, const float *a, const float *b)
{
	return BSP_CheckPVSLeaf(map, BSP_PointLeaf(map, a), b);
}

bool BSP_CheckPVSLeaf(const bspmap_t *map, int la, const float *b)
{
	int lb;
	const uint8_t *row;
	int cb;
	if (la < 0)
		return true;
	lb = BSP_PointLeaf(map, b);
	if (lb < 0)
		return true;
	cb = map->leafs[lb].cluster;
	row = BSP_ClusterPVS(map, map->leafs[la].cluster);
	if (!row || cb < 0)
		return map->leafs[la].cluster >= 0 || !map->vis; /* outside the world sees nothing */
	return (row[cb >> 3] >> (cb & 7)) & 1;
}
