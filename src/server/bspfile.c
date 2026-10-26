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

enum {
	Q3_ENTITIES = 0, Q3_SHADERS = 1, Q3_PLANES = 2, Q3_NODES = 3, Q3_LEAFS = 4, Q3_LEAFFACES = 5, Q3_LEAFBRUSHES = 6,
	Q3_MODELS = 7, Q3_BRUSHES = 8, Q3_BRUSHSIDES = 9, Q3_VISIBILITY = 16, Q3_NUMLUMPS = 17
};
enum {
	Q1_ENTITIES = 0, Q1_PLANES = 1, Q1_VISIBILITY = 4, Q1_NODES = 5, Q1_CLIPNODES = 9, Q1_LEAFS = 10,
	Q1_MARKSURFACES = 11, Q1_MODELS = 14, Q1_NUMLUMPS = 15
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
	Mem_Free(m->nodes);
	Mem_Free(m->leafs);
	Mem_Free(m->leaffaces);
	Mem_Free(m->leafbrushes);
	Mem_Free(m->vis);
	Mem_Free(m->clipnodes);
	Mem_Free(m->hull0);
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

/* --- Q1 hull collision -------------------------------------------------- */

/* the player and large hulls are the world grown by these boxes */
static const float hull_mins[3][3] = {{0, 0, 0}, {-16, -16, -24}, {-32, -32, -24}};
/* GoldSrc: standing player, large, crouching player */
static const float hl_hull_mins[4][3] = {{0, 0, 0}, {-16, -16, -36}, {-32, -32, -32}, {-16, -16, -18}};

static int hull_contents(const cclipnode_t *nodes, const cplane_t *planes, int num, const float *p)
{
	int guard = 0;
	while (num >= 0 && guard++ < 65536) {
		const cplane_t *pl = &planes[nodes[num].plane];
		num = nodes[num].children[DotProduct(p, pl->normal) - pl->dist < 0];
	}
	return num;
}

typedef struct {
	const cclipnode_t *nodes;
	const cplane_t *planes;
	int head;
	bsptrace_t *tr;
	bool allsolid, startsolid;
	int depth;
} hullwork_t;

/* sweeps p1-p2 (fractions f1-f2 of the whole move) down the hull; false
 * once something was hit, which ends the walk */
static bool hull_trace(hullwork_t *w, int num, float f1, float f2, const float *p1, const float *p2)
{
	const cplane_t *pl;
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
	pl = &w->planes[w->nodes[num].plane];
	t1 = DotProduct(p1, pl->normal) - pl->dist;
	t2 = DotProduct(p2, pl->normal) - pl->dist;
	if (t1 >= 0 && t2 >= 0)
		return hull_trace(w, w->nodes[num].children[0], f1, f2, p1, p2);
	if (t1 < 0 && t2 < 0)
		return hull_trace(w, w->nodes[num].children[1], f1, f2, p1, p2);
	/* the crossing, kept a hair on the near side */
	frac = t1 < 0 ? (t1 + DIST_EPSILON) / (t1 - t2) : (t1 - DIST_EPSILON) / (t1 - t2);
	frac = BW_MAX(0, BW_MIN(frac, 1));
	side = t1 < 0;
	midf = f1 + (f2 - f1) * frac;
	for (int k = 0; k < 3; k++)
		mid[k] = p1[k] + (p2[k] - p1[k]) * frac;
	if (!hull_trace(w, w->nodes[num].children[side], f1, midf, p1, mid))
		return false;
	if (hull_contents(w->nodes, w->planes, w->nodes[num].children[side ^ 1], mid) != Q1C_SOLID)
		return hull_trace(w, w->nodes[num].children[side ^ 1], midf, f2, mid, p2);
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
	while (hull_contents(w->nodes, w->planes, w->head, mid) == Q1C_SOLID) {
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
		return q1_contents(hull_contents(map->hull0, map->planes, map->models[model].headnode[0], lp));
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
	int la = BSP_PointLeaf(map, a), lb = BSP_PointLeaf(map, b);
	const uint8_t *row;
	int cb;
	if (la < 0 || lb < 0)
		return true;
	cb = map->leafs[lb].cluster;
	row = BSP_ClusterPVS(map, map->leafs[la].cluster);
	if (!row || cb < 0)
		return map->leafs[la].cluster >= 0 || !map->vis; /* outside the world sees nothing */
	return (row[cb >> 3] >> (cb & 7)) & 1;
}
