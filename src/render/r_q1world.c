/* Bulwark engine - drawing Quake 1 worlds (BSP29 and BSP2): faces built
 * from edges, textures from the map's own miptex lump, lightmaps packed
 * into atlas pages, and model lighting sampled from the lightmap under a
 * point. Record layouts are in docs/spec/bsp.md. ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum { L_ENTITIES, L_PLANES, L_TEXTURES, L_VERTEXES, L_VISIBILITY, L_NODES, L_TEXINFO, L_FACES, L_LIGHTING,
       L_CLIPNODES, L_LEAFS, L_MARKSURFACES, L_EDGES, L_SURFEDGES, L_MODELS, L_COUNT };

#define TEX_SPECIAL 1 /* texinfo flag: sky and liquids, no lightmap */
#define LM_PAGE 1024

static int32_t rd32(const uint8_t *p) { return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }
static int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static uint16_t rdu16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static float rdf(const uint8_t *p)
{
	float f;
	int32_t i = rd32(p);
	memcpy(&f, &i, 4);
	return f;
}

/* what model lighting needs of each face, kept after loading */
typedef struct {
	float vecs[2][4];
	int smin, tmin, w, h; /* lightmap extents in luxels */
	int page, x, y;       /* place in the atlas, page -1 for none */
} q1face_t;

typedef struct {
	int plane;
	int children[2];
	int firstface, numfaces;
} q1node_t;

static struct {
	q1face_t *faces;
	int numfaces;
	q1node_t *nodes;
	int numnodes;
	float (*planes)[4];
	int numplanes;
	uint8_t **pages; /* RGB copies of the lightmap pages */
	int numpages;
	int headnode;
} q1;

void R_FreeQ1World(void)
{
	for (int i = 0; i < q1.numpages; i++)
		Mem_Free(q1.pages[i]);
	Mem_Free(q1.pages);
	Mem_Free(q1.faces);
	Mem_Free(q1.nodes);
	Mem_Free(q1.planes);
	memset(&q1, 0, sizeof(q1));
}

/* --- lightmap atlas ---------------------------------------------------------- */

/* shelves: each page is filled in rows as tall as their tallest block */
static struct {
	int x, y, rowh;
} shelf;

static bool atlas_alloc(int w, int h, int *page, int *x, int *y)
{
	if (w > LM_PAGE || h > LM_PAGE)
		return false;
	if (!q1.numpages || shelf.x + w > LM_PAGE) {
		shelf.x = 0;
		shelf.y += shelf.rowh;
		shelf.rowh = 0;
	}
	if (!q1.numpages || shelf.y + h > LM_PAGE) {
		q1.pages = Mem_Realloc(q1.pages, sizeof(uint8_t *) * (q1.numpages + 1));
		q1.pages[q1.numpages++] = Mem_Alloc(LM_PAGE * LM_PAGE * 3);
		shelf.x = shelf.y = shelf.rowh = 0;
	}
	*page = q1.numpages - 1;
	*x = shelf.x;
	*y = shelf.y;
	shelf.x += w + 1; /* a gap keeps filtering from bleeding between faces */
	shelf.rowh = BW_MAX(shelf.rowh, h + 1);
	return true;
}

/* --- textures ------------------------------------------------------------------ */

typedef struct {
	material_t *mat;
	int width, height;
} q1tex_t;

static material_t *texture_material(const uint8_t *mt, size_t avail, int *w, int *h)
{
	char name[17];
	const uint8_t *pal = R_Palette();
	uint32_t ofs0;
	material_t *m;
	memcpy(name, mt, 16);
	name[16] = 0;
	for (char *c = name; *c; c++)
		*c = (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
	*w = rd32(mt + 16);
	*h = rd32(mt + 20);
	ofs0 = (uint32_t)rd32(mt + 24);
	if (*w <= 0 || *h <= 0 || *w > 4096 || *h > 4096)
		*w = *h = 64;
	/* a replacement texture or material on disk wins over the map's own pixels */
	if (R_MaterialFileExists(name) || R_ImageExists(name) || !ofs0 || ofs0 + (uint64_t)*w * *h > avail) {
		m = R_FindMaterial(name, 0);
	} else {
		const uint8_t *px = mt + ofs0;
		bool sky = !Str_NICmp(name, "sky", 3), fence = name[0] == '{';
		uint8_t *rgba = Mem_Alloc((size_t)*w * *h * 4), *glow = NULL;
		int pixels = *w * *h;
		char gname[64];
		for (int i = 0; i < pixels; i++) {
			int c = px[i];
			rgba[i * 4] = pal[c * 3];
			rgba[i * 4 + 1] = pal[c * 3 + 1];
			rgba[i * 4 + 2] = pal[c * 3 + 2];
			rgba[i * 4 + 3] = fence && c == 255 ? 0 : 255;
			/* the palette's last 32 colours glow in the dark */
			if (c >= 224 && c < 255 && !sky) {
				if (!glow)
					glow = Mem_Alloc((size_t)pixels * 4);
				memcpy(glow + i * 4, rgba + i * 4, 4);
			}
		}
		m = R_MaterialForImage(name, R_ImageFromPixels(name, rgba, *w, *h, 0), 0);
		if (glow) {
			snprintf(gname, sizeof(gname), "%s_luma", name);
			m->fullbright = R_ImageFromPixels(gname, glow, *w, *h, 0);
		}
		if (fence)
			m->alphatest = true;
		Mem_Free(rgba);
		Mem_Free(glow);
	}
	if (!Str_NICmp(name, "sky", 3))
		m->sky = true;
	else if (name[0] == '*' || name[0] == '!')
		m->program = MP_WATER;
	else if (!Str_NICmp(name, "clip", 4) || !Str_ICmp(name, "trigger"))
		m->nodraw = true;
	return m;
}

/* --- the world ------------------------------------------------------------------- */

typedef struct {
	rvert_t *v;
	int num, cap;
	uint32_t *i;
	int numi, capi;
} geom_t;

static void push_vert(geom_t *g, const rvert_t *v)
{
	if (g->num == g->cap) {
		g->cap = g->cap ? g->cap * 2 : 4096;
		g->v = Mem_Realloc(g->v, sizeof(rvert_t) * g->cap);
	}
	g->v[g->num++] = *v;
}

static void push_index(geom_t *g, uint32_t i)
{
	if (g->numi == g->capi) {
		g->capi = g->capi ? g->capi * 2 : 8192;
		g->i = Mem_Realloc(g->i, sizeof(uint32_t) * g->capi);
	}
	g->i[g->numi++] = i;
}

static int surface_cmp(const void *a, const void *b)
{
	const rsurface_t *x = a, *y = b;
	if (x->material != y->material)
		return x->material < y->material ? -1 : 1;
	return x->lightmap - y->lightmap;
}

bool R_LoadQ1World(bspmap_t *bsp, const char *path, const uint8_t *d, size_t size)
{
	struct {
		int32_t ofs, len;
	} l[L_COUNT];
	bool b2 = !memcmp(d, "BSP2", 4);
	int facesize = b2 ? 28 : 20, edgesize = b2 ? 8 : 4, nodesize = b2 ? 44 : 24;
	int numverts, numedges, numsurfedges, numtexinfo, numfaces, nummodels, numtex = 0;
	q1tex_t *texs = NULL;
	uint8_t *lit = NULL, *styled = NULL;
	size_t litlen = 0;
	geom_t g = {0};
	char litname[160];

	R_FreeQ1World();
	memset(&shelf, 0, sizeof(shelf));
	if (size < 4 + L_COUNT * 8)
		return false;
	for (int i = 0; i < L_COUNT; i++) {
		l[i].ofs = rd32(d + 4 + i * 8);
		l[i].len = rd32(d + 8 + i * 8);
		if (l[i].ofs < 0 || l[i].len < 0 || (size_t)l[i].ofs + l[i].len > size)
			return false;
	}
	numverts = l[L_VERTEXES].len / 12;
	numedges = l[L_EDGES].len / edgesize;
	numsurfedges = l[L_SURFEDGES].len / 4;
	numtexinfo = l[L_TEXINFO].len / 40;
	numfaces = l[L_FACES].len / facesize;
	nummodels = l[L_MODELS].len / 64;

	/* textures */
	if (l[L_TEXTURES].len >= 4) {
		const uint8_t *tl = d + l[L_TEXTURES].ofs;
		numtex = rd32(tl);
		if (numtex < 0 || 4 + (int64_t)numtex * 4 > l[L_TEXTURES].len)
			numtex = 0;
		texs = Mem_Alloc(sizeof(q1tex_t) * (numtex + 1));
		for (int i = 0; i < numtex; i++) {
			int32_t o = rd32(tl + 4 + i * 4);
			if (o < 0 || o + 40 > l[L_TEXTURES].len) {
				texs[i].mat = R_FindMaterial("$default", 0);
				texs[i].width = texs[i].height = 64;
				continue;
			}
			texs[i].mat = texture_material(tl + o, (size_t)(l[L_TEXTURES].len - o), &texs[i].width, &texs[i].height);
		}
	}

	/* coloured light from a .lit file next to the map: "QLIT", version 1, then RGB */
	{
		const char *dot = strrchr(path, '.');
		snprintf(litname, sizeof(litname), "%.*s.lit", dot ? (int)(dot - path) : (int)strlen(path), path);
		lit = (uint8_t *)FS_LoadFile(litname, &litlen);
		if (lit && (litlen < 8 || memcmp(lit, "QLIT", 4) || rd32(lit + 4) != 1 ||
		            litlen - 8 < (size_t)l[L_LIGHTING].len * 3)) {
			Mem_Free(lit);
			lit = NULL;
		}
	}

	q1.planes = Mem_Alloc(sizeof(float) * 4 * (l[L_PLANES].len / 20 + 1));
	q1.numplanes = l[L_PLANES].len / 20;
	for (int i = 0; i < q1.numplanes; i++)
		for (int k = 0; k < 4; k++)
			q1.planes[i][k] = rdf(d + l[L_PLANES].ofs + i * 20 + k * 4);

	q1.numfaces = numfaces;
	q1.faces = Mem_Alloc(sizeof(q1face_t) * (numfaces + 1));
	r_world.surfaces = Mem_Alloc(sizeof(rsurface_t) * (numfaces + 1));
	r_world.numsurfaces = numfaces;
	styled = Mem_Alloc(64 * 64 * 3);
	for (int f = 0; f < numfaces; f++) {
		const uint8_t *fp = d + l[L_FACES].ofs + f * facesize;
		int plane = b2 ? rd32(fp) : rdu16(fp), side = b2 ? rd32(fp + 4) : rd16(fp + 2);
		int firstedge = b2 ? rd32(fp + 8) : rd32(fp + 4), nedges = b2 ? rd32(fp + 12) : rd16(fp + 8);
		int ti = b2 ? rd32(fp + 16) : rd16(fp + 10);
		const uint8_t *styles = fp + (b2 ? 20 : 12);
		int lightofs = rd32(fp + (b2 ? 24 : 16));
		rsurface_t *rs = &r_world.surfaces[f];
		q1face_t *qf = &q1.faces[f];
		const uint8_t *tip;
		float normal[3], smin = 1e30f, smax = -1e30f, tmin = 1e30f, tmax = -1e30f;
		int miptex, tflags, base = g.num, tw = 64, th = 64;
		material_t *mat;
		rs->orig = f;
		rs->lightmap = -1;
		qf->page = -1;
		if (ti < 0 || ti >= numtexinfo || nedges < 3 || nedges > 256 || firstedge < 0 ||
		    firstedge + nedges > numsurfedges || plane < 0 || plane >= q1.numplanes) {
			rs->material = R_FindMaterial("$default", 0);
			continue;
		}
		tip = d + l[L_TEXINFO].ofs + ti * 40;
		for (int k = 0; k < 8; k++)
			qf->vecs[k / 4][k % 4] = rdf(tip + k * 4);
		miptex = rd32(tip + 32);
		tflags = rd32(tip + 36);
		if (miptex >= 0 && miptex < numtex) {
			mat = texs[miptex].mat;
			tw = texs[miptex].width;
			th = texs[miptex].height;
		} else {
			mat = R_FindMaterial("$default", 0);
		}
		rs->material = mat;
		rs->type = 1; /* planar */
		for (int k = 0; k < 3; k++)
			normal[k] = side ? -q1.planes[plane][k] : q1.planes[plane][k];
		/* the polygon, from its edges */
		for (int e = 0; e < nedges; e++) {
			int se = rd32(d + l[L_SURFEDGES].ofs + (firstedge + e) * 4), ei = se < 0 ? -se : se, vi;
			const uint8_t *ep;
			rvert_t v;
			float s, t;
			if (ei >= numedges)
				ei = 0;
			ep = d + l[L_EDGES].ofs + ei * edgesize;
			vi = b2 ? rd32(ep + (se < 0 ? 4 : 0)) : rdu16(ep + (se < 0 ? 2 : 0));
			if (vi < 0 || vi >= numverts)
				vi = 0;
			memset(&v, 0, sizeof(v));
			for (int k = 0; k < 3; k++)
				v.xyz[k] = rdf(d + l[L_VERTEXES].ofs + vi * 12 + k * 4);
			VectorCopy(normal, v.normal);
			memset(v.color, 255, 4);
			s = DotProduct(v.xyz, qf->vecs[0]) + qf->vecs[0][3];
			t = DotProduct(v.xyz, qf->vecs[1]) + qf->vecs[1][3];
			v.st[0] = s / tw;
			v.st[1] = t / th;
			v.lm[0] = s; /* turned into atlas coordinates below */
			v.lm[1] = t;
			smin = BW_MIN(smin, s), smax = BW_MAX(smax, s);
			tmin = BW_MIN(tmin, t), tmax = BW_MAX(tmax, t);
			push_vert(&g, &v);
		}
		/* lightmap: one luxel per 16 texels, corners included */
		qf->smin = (int)floorf(smin / 16);
		qf->tmin = (int)floorf(tmin / 16);
		qf->w = (int)ceilf(smax / 16) - qf->smin + 1;
		qf->h = (int)ceilf(tmax / 16) - qf->tmin + 1;
		if (!(tflags & TEX_SPECIAL) && lightofs >= 0 && qf->w <= 64 && qf->h <= 64 &&
		    atlas_alloc(qf->w, qf->h, &qf->page, &qf->x, &qf->y)) {
			int n = qf->w * qf->h;
			uint8_t *page = q1.pages[qf->page];
			memset(styled, 0, (size_t)n * 3);
			/* every style at its normal strength, added up */
			for (int st = 0; st < 4 && styles[st] != 255; st++) {
				size_t o = (size_t)lightofs + (size_t)st * n;
				if (o + n > (size_t)l[L_LIGHTING].len)
					break;
				for (int i = 0; i < n; i++)
					for (int c = 0; c < 3; c++) {
						int v = styled[i * 3 + c] + (lit ? lit[8 + (o + i) * 3 + c] : d[l[L_LIGHTING].ofs + o + i]);
						styled[i * 3 + c] = (uint8_t)BW_MIN(v, 255);
					}
			}
			for (int y = 0; y < qf->h; y++)
				memcpy(page + ((size_t)(qf->y + y) * LM_PAGE + qf->x) * 3, styled + (size_t)y * qf->w * 3,
				       (size_t)qf->w * 3);
			rs->lightmap = qf->page;
		} else if (!(tflags & TEX_SPECIAL) && lightofs < 0) {
			/* compiled without light: full bright */
			rs->lightmap = -1;
		}
		for (int k = base; k < g.num; k++) {
			rvert_t *v = &g.v[k];
			if (qf->page >= 0) {
				v->lm[0] = (qf->x + (v->lm[0] / 16 - qf->smin) + 0.5f) / LM_PAGE;
				v->lm[1] = (qf->y + (v->lm[1] / 16 - qf->tmin) + 0.5f) / LM_PAGE;
			} else {
				v->lm[0] = v->lm[1] = 0;
			}
		}
		/* a fan, wound for GL's counter-clockwise fronts */
		rs->firstindex = g.numi;
		for (int k = 2; k < nedges; k++) {
			push_index(&g, (uint32_t)base);
			push_index(&g, (uint32_t)(base + k));
			push_index(&g, (uint32_t)(base + k - 1));
		}
		rs->numindexes = g.numi - rs->firstindex;
		for (int k = 0; k < 3; k++) {
			rs->mins[k] = 1e30f;
			rs->maxs[k] = -1e30f;
		}
		for (int k = base; k < g.num; k++)
			for (int a = 0; a < 3; a++) {
				rs->mins[a] = BW_MIN(rs->mins[a], g.v[k].xyz[a]);
				rs->maxs[a] = BW_MAX(rs->maxs[a], g.v[k].xyz[a]);
			}
	}
	Mem_Free(styled);
	Mem_Free(lit);
	Mem_Free(texs);

	/* lightmap pages */
	r_world.lightmapsize = LM_PAGE;
	r_world.numlightmaps = q1.numpages;
	if (q1.numpages) {
		r_world.lightmaps = Mem_Alloc(sizeof(GLuint) * q1.numpages);
		if (R_Active()) {
			qglGenTextures(q1.numpages, r_world.lightmaps);
			qglPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			for (int i = 0; i < q1.numpages; i++) {
				GL_BindTexture(1, GL_TEXTURE_2D, r_world.lightmaps[i]);
				qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, LM_PAGE, LM_PAGE, 0, GL_RGB, GL_UNSIGNED_BYTE, q1.pages[i]);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}
		}
	}

	/* the node tree, for lighting models */
	q1.numnodes = l[L_NODES].len / nodesize;
	q1.nodes = Mem_Alloc(sizeof(q1node_t) * (q1.numnodes + 1));
	for (int i = 0; i < q1.numnodes; i++) {
		const uint8_t *np = d + l[L_NODES].ofs + i * nodesize;
		q1node_t *n = &q1.nodes[i];
		n->plane = rd32(np);
		for (int c = 0; c < 2; c++)
			n->children[c] = b2 ? rd32(np + 4 + c * 4) : rd16(np + 4 + c * 2);
		n->firstface = b2 ? rd32(np + 36) : rdu16(np + 20);
		n->numfaces = b2 ? rd32(np + 40) : rdu16(np + 22);
		if (n->plane < 0 || n->plane >= q1.numplanes)
			n->plane = 0;
		if (n->firstface < 0 || n->firstface + n->numfaces > numfaces)
			n->numfaces = 0;
	}
	q1.headnode = nummodels ? rd32(d + l[L_MODELS].ofs + 36) : 0;

	/* submodels */
	r_world.numsubmodels = nummodels;
	r_world.submodels = Mem_Alloc(sizeof(rmodel_t) * (nummodels + 1));
	for (int i = 0; i < nummodels; i++) {
		const uint8_t *mp = d + l[L_MODELS].ofs + i * 64;
		rmodel_t *rm = &r_world.submodels[i];
		snprintf(rm->name, sizeof(rm->name), "*%d", i);
		rm->type = MOD_BRUSH;
		for (int k = 0; k < 3; k++) {
			rm->mins[k] = rdf(mp + k * 4);
			rm->maxs[k] = rdf(mp + 12 + k * 4);
		}
		rm->firstsurface = rd32(mp + 56);
		rm->numsurfaces = rd32(mp + 60);
		if (rm->firstsurface < 0 || rm->numsurfaces < 0 || rm->firstsurface + rm->numsurfaces > numfaces)
			rm->numsurfaces = 0;
		rm->loaded = true;
		qsort(r_world.surfaces + rm->firstsurface, rm->numsurfaces, sizeof(rsurface_t), surface_cmp);
	}
	(void)bsp;
	R_WorldUpload(g.v, g.num, g.i, g.numi);
	Con_DPrintf("%s: %d faces, %d lightmap pages%s\n", path, numfaces, q1.numpages, lit ? " (coloured)" : "");
	Mem_Free(g.v);
	Mem_Free(g.i);
	return true;
}

/* --- lighting models ----------------------------------------------------------- */

/* follows the segment start-end down the tree; at the first face it crosses,
 * reads the lightmap there */
static int light_recurse(int num, const float *start, const float *end, float *rgb)
{
	const q1node_t *n;
	const float *pl;
	float d1, d2, frac, mid[3];
	int side, r;
	if (num < 0 || num >= q1.numnodes)
		return 0; /* a leaf: nothing hit on this branch */
	n = &q1.nodes[num];
	pl = q1.planes[n->plane];
	d1 = DotProduct(start, pl) - pl[3];
	d2 = DotProduct(end, pl) - pl[3];
	side = d1 < 0;
	if ((d2 < 0) == side)
		return light_recurse(n->children[side], start, end, rgb);
	frac = d1 / (d1 - d2);
	for (int k = 0; k < 3; k++)
		mid[k] = start[k] + (end[k] - start[k]) * frac;
	if ((r = light_recurse(n->children[side], start, mid, rgb)) != 0)
		return r;
	for (int i = 0; i < n->numfaces; i++) {
		const q1face_t *f = &q1.faces[n->firstface + i];
		float s, t;
		int ls, lt;
		const uint8_t *px;
		if (f->page < 0)
			continue;
		s = (DotProduct(mid, f->vecs[0]) + f->vecs[0][3]) / 16 - f->smin;
		t = (DotProduct(mid, f->vecs[1]) + f->vecs[1][3]) / 16 - f->tmin;
		if (s < 0 || t < 0 || s > f->w - 1 || t > f->h - 1)
			continue;
		ls = (int)(s + 0.5f);
		lt = (int)(t + 0.5f);
		px = q1.pages[f->page] + ((size_t)(f->y + lt) * LM_PAGE + f->x + ls) * 3;
		for (int k = 0; k < 3; k++)
			rgb[k] = px[k] / 255.0f;
		return 1;
	}
	return light_recurse(n->children[!side], mid, end, rgb);
}

bool R_Q1LightPoint(const float *p, float *ambient, float *diffuse, float *dir)
{
	float end[3] = {p[0], p[1], p[2] - 2048}, rgb[3] = {0, 0, 0};
	float scale = r_lightmapscale ? r_lightmapscale->value : 2;
	if (!q1.nodes || !light_recurse(q1.headnode, p, end, rgb))
		return false;
	/* no direction is stored: mostly ambient, a little from above */
	for (int k = 0; k < 3; k++) {
		ambient[k] = rgb[k] * scale * 0.7f;
		diffuse[k] = rgb[k] * scale * 0.3f;
	}
	dir[0] = 0.3f, dir[1] = 0.2f, dir[2] = 0.93f;
	return true;
}
