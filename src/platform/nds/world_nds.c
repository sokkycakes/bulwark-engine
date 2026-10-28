/* Bulwark engine - Quake maps (BSP version 29) on the DS's 3D hardware.
 * ISC licence, see LICENSE.
 *
 * Everything that can be worked out once is worked out at load time, in
 * floating point. After that, a frame only uses integer and fixed-point
 * maths, because the ARM9 has no FPU:
 *
 * - Vertices are 16-bit, in 1/8 map units. The GPU reads 4.12 fixed
 *   point, so one GPU unit is 512 map units and a map can reach +-4096.
 * - Textures are the map's own 8-bit pictures with the Quake palette
 *   (GL_RGB256), taken from a smaller mip level when the 384 KB of
 *   texture VRAM runs short.
 * - Lighting is baked into vertex colours from the lightmaps, because the
 *   GPU draws one texture per polygon.
 * - Each face is a single triangle strip, so a face costs as many GPU
 *   vertices as it has corners.
 * - The PVS and a frustum test on node boxes pick the faces. Faces turned
 *   away from the eye are dropped on the CPU. The tree is walked nearest
 *   first, so when a view holds more than the GPU takes in a frame (6144
 *   vertices, 2048 polygons), the faces left out are the farthest. */
#include <nds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "nds.h"

#define BSPVERSION 29
#define MAX_GPU_VERTS WORLD_MAX_VERTS
#define MAX_GPU_POLYS WORLD_MAX_POLYS
#define TEX_BUDGET (192 * 1024) /* VRAM banks A and B, less 64 KB for model skins (C and D hold captures) */
#define TEX_MAXDIM 256

enum { L_ENTITIES, L_PLANES, L_TEXTURES, L_VERTEXES, L_VISIBILITY, L_NODES, L_TEXINFO, L_FACES, L_LIGHTING,
       L_CLIPNODES, L_LEAFS, L_MARKSURFACES, L_EDGES, L_SURFEDGES, L_MODELS, NUM_LUMPS };

typedef struct {
	int16_t x, y, z; /* 1/8 map units */
	int16_t s, t;    /* t16: texels * 16 */
	uint16_t color;  /* RGB15 */
} dvert_t;

typedef struct {
	int32_t n[3]; /* 4.12 */
	int32_t dist; /* map units << 15, to compare with n . (1/8 units) */
} plane_t;

typedef struct {
	int plane;
	bool back;      /* the face looks along -normal */
	int firstvert;  /* into verts[], in strip order */
	int numverts;
	int tex;        /* -1: not drawn */
	int frame;
	int next;       /* texture chain */
} face_t;

typedef struct {
	int plane;
	int children[2]; /* >= 0 node, < 0 leaf -(c + 1) */
	int16_t mins[3], maxs[3];
	int parent;
	int visframe;
} node_t;

typedef struct {
	int contents;
	int visofs;
	int16_t mins[3], maxs[3];
	int firstmark, nummarks;
	int parent;
	int visframe;
} leaf_t;

typedef struct {
	int name;       /* GL texture name, 0 if missing */
	int w, h;       /* size of the picture in the map, at mip 0 */
	int tw, th;     /* size in VRAM */
	bool skip;
	bool fullbright;
	bool sky;
	int chain;      /* first face this frame, -1 none */
} tex_t;

typedef struct {
	int16_t mins[3], maxs[3];
	int firstface, numfaces;
} submodel_t;

static struct {
	bool loaded;
	uint8_t *file;
	size_t filesize;
	const char *entities;
	plane_t *planes;
	int numplanes;
	node_t *nodes;
	int numnodes;
	leaf_t *leafs;
	int numleafs, visleafs;
	const uint8_t *vis;
	int vissize;
	uint16_t *marks;
	int nummarks;
	face_t *faces;
	int numfaces;
	dvert_t *verts;
	int numverts;
	tex_t *texs;
	int numtexs;
	submodel_t *models;
	int nummodels;
	int texbytes;
	int visframe, frame;
	int lastleaf;
	uint8_t *leafvis; /* decompressed PVS for lastleaf */
	int *vislist;     /* this frame's faces: the tree's nearest first, then brush entities' */
	int32_t *visdist; /* for brush entities' faces: the squared distance to the entity (map units) */
	int numvis, numtreevis;
} w;

static inline uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static inline float rdf(const uint8_t *p)
{
	uint32_t u = rd32(p);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

static int pot_floor(int v)
{
	int p = 8;
	while (p * 2 <= v && p < 1024)
		p *= 2;
	return p;
}

static int size_enum(int v)
{
	int e = 0;
	while ((8 << e) < v)
		e++;
	return e; /* TEXTURE_SIZE_8 = 0, ... */
}

void World_Free(void)
{
	if (w.texs) {
		for (int i = 0; i < w.numtexs; i++)
			if (w.texs[i].name)
				glDeleteTextures(1, &w.texs[i].name);
	}
	free(w.file);
	free(w.planes);
	free(w.nodes);
	free(w.leafs);
	free(w.faces);
	free(w.verts);
	free(w.texs);
	free(w.models);
	free(w.leafvis);
	free(w.vislist);
	free(w.visdist);
	memset(&w, 0, sizeof(w));
}

bool World_Loaded(void) { return w.loaded; }
int World_TextureBytes(void) { return w.texbytes; }

/* --- textures ---------------------------------------------------------------------------- */

static bool name_is(const char *name, const char *what) { return strncasecmp(name, what, 16) == 0; }

static void load_textures(const uint8_t *lump, int len, const uint16_t *palette)
{
	static uint16_t pal[256], palswap[256];
	int count, level = 0, palnorm = 0, palalpha = 0;
	if (len < 4) {
		w.numtexs = 0;
		return;
	}
	count = (int)rd32(lump);
	w.numtexs = count;
	w.texs = calloc((size_t)count, sizeof(tex_t));
	for (int i = 0; i < 256; i++)
		pal[i] = palette ? palette[i] : RGB15(i >> 3, i >> 3, i >> 3);
	memcpy(palswap, pal, sizeof(pal));
	palswap[0] = pal[255], palswap[255] = pal[0];

	/* the smallest mip level at which everything fits in texture VRAM */
	for (level = 0; level < 3; level++) {
		int total = 0;
		for (int i = 0; i < count; i++) {
			int ofs = (int)rd32(lump + 4 + i * 4), tw, th;
			if (ofs <= 0 || ofs + 40 > len)
				continue;
			tw = pot_floor((int)rd32(lump + ofs + 16) >> level);
			th = pot_floor((int)rd32(lump + ofs + 20) >> level);
			total += BW_NDS_MIN(tw, TEX_MAXDIM) * BW_NDS_MIN(th, TEX_MAXDIM);
		}
		if (total <= TEX_BUDGET)
			break;
	}

	for (int i = 0; i < count; i++) {
		tex_t *t = &w.texs[i];
		int ofs = (int)rd32(lump + 4 + i * 4), sw, sh, tw, th;
		const uint8_t *mt, *src;
		char name[17];
		uint8_t *pix;
		bool alpha;
		t->chain = -1;
		if (ofs <= 0 || ofs + 40 > len) {
			t->skip = true;
			continue;
		}
		mt = lump + ofs;
		memcpy(name, mt, 16);
		name[16] = 0;
		t->w = (int)rd32(mt + 16);
		t->h = (int)rd32(mt + 20);
		t->skip = name_is(name, "skip") || name_is(name, "trigger") || name_is(name, "clip") ||
		          name_is(name, "hint") || name_is(name, "origin") || name_is(name, "*waterskip");
		t->sky = strncasecmp(name, "sky", 3) == 0;
		t->fullbright = name[0] == '*' || t->sky;
		alpha = name[0] == '{';
		if (t->skip || t->w <= 0 || t->h <= 0)
			continue;
		sw = t->w >> level, sh = t->h >> level;
		if (sw < 1 || sh < 1 || !rd32(mt + 24 + level * 4) ||
		    ofs + (int)rd32(mt + 24 + level * 4) + sw * sh > len)
			continue; /* the picture lives in a WAD the map doesn't carry */
		src = mt + rd32(mt + 24 + level * 4);
		if (t->sky)
			sw /= 2; /* the back layer: the left half */
		tw = BW_NDS_MIN(pot_floor(sw), TEX_MAXDIM), th = BW_NDS_MIN(pot_floor(sh), TEX_MAXDIM);
		pix = malloc((size_t)tw * th);
		if (!pix)
			continue;
		for (int y = 0; y < th; y++)
			for (int x = 0; x < tw; x++) {
				uint8_t c = src[(y * sh / th) * (t->w >> level) + x * sw / tw];
				if (alpha)
					c = c == 255 ? 0 : c == 0 ? 255 : c;
				pix[y * tw + x] = c;
			}
		glGenTextures(1, &t->name);
		glBindTexture(GL_TEXTURE_2D, t->name);
		if (!glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB256, size_enum(tw), size_enum(th), 0,
		                  GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T | (alpha ? GL_TEXTURE_COLOR0_TRANSPARENT : 0), pix)) {
			glDeleteTextures(1, &t->name);
			t->name = 0;
			free(pix);
			continue;
		}
		free(pix);
		/* one palette for opaque pictures, one with 0 and 255 swapped for
		 * the see-through ones (the GPU makes index 0 clear) */
		if (alpha) {
			if (palalpha)
				glAssignColorTable(GL_TEXTURE_2D, palalpha);
			else
				glColorTableEXT(GL_TEXTURE_2D, 0, 256, 0, 0, palswap), palalpha = t->name;
		} else {
			if (palnorm)
				glAssignColorTable(GL_TEXTURE_2D, palnorm);
			else
				glColorTableEXT(GL_TEXTURE_2D, 0, 256, 0, 0, pal), palnorm = t->name;
		}
		t->tw = tw, t->th = th;
		/* texture coordinates are worked out against the mip-0 size */
		w.texbytes += tw * th;
	}
}

/* --- faces ------------------------------------------------------------------------------- */

static uint16_t light_color(int l)
{
	/* lightmaps are stored at about half brightness: 128 and up is full */
	int c = l * 31 / 140;
	if (c > 31)
		c = 31;
	if (c < 2)
		c = 2;
	return RGB15(c, c, c);
}

/* the light at a point of a face, from its lightmap(s), bilinear */
static int sample_light(const uint8_t *light, int lightlen, int lightofs, const uint8_t *styles, int lw, int lh,
                        float u, float v)
{
	int x0, y0, x1, y1, total = 0;
	float fx, fy;
	if (lightofs < 0)
		return 255;
	if (u < 0)
		u = 0;
	if (v < 0)
		v = 0;
	if (u > lw - 1)
		u = (float)(lw - 1);
	if (v > lh - 1)
		v = (float)(lh - 1);
	x0 = (int)u, y0 = (int)v;
	x1 = x0 + 1 < lw ? x0 + 1 : x0;
	y1 = y0 + 1 < lh ? y0 + 1 : y0;
	fx = u - x0, fy = v - y0;
	for (int s = 0; s < 4 && styles[s] != 255; s++) {
		const uint8_t *m = light + lightofs + s * lw * lh;
		if (lightofs + (s + 1) * lw * lh > lightlen)
			break;
		total += (int)((m[y0 * lw + x0] * (1 - fx) + m[y0 * lw + x1] * fx) * (1 - fy) +
		               (m[y1 * lw + x0] * (1 - fx) + m[y1 * lw + x1] * fx) * fy);
	}
	return total;
}

static bool load_faces(const uint8_t *d, const uint32_t *ofs, const uint32_t *len)
{
	const uint8_t *vx = d + ofs[L_VERTEXES], *ed = d + ofs[L_EDGES], *se = d + ofs[L_SURFEDGES];
	const uint8_t *ti = d + ofs[L_TEXINFO], *fc = d + ofs[L_FACES], *light = d + ofs[L_LIGHTING];
	int numvx = (int)(len[L_VERTEXES] / 12), numedges = (int)(len[L_EDGES] / 4);
	int numse = (int)(len[L_SURFEDGES] / 4), numti = (int)(len[L_TEXINFO] / 40);
	w.numfaces = (int)(len[L_FACES] / 20);
	w.faces = calloc((size_t)w.numfaces, sizeof(face_t));
	w.vislist = malloc(sizeof(int) * (size_t)(w.numfaces + 1));
	w.visdist = malloc(sizeof(int32_t) * (size_t)(w.numfaces + 1));
	w.verts = malloc(sizeof(dvert_t) * (size_t)numse);
	if (!w.faces || !w.verts || !w.vislist || !w.visdist)
		return false;
	for (int i = 0; i < w.numfaces; i++) {
		const uint8_t *f = fc + i * 20;
		face_t *face = &w.faces[i];
		int first = (int)rd32(f + 4), num = rd16(f + 8), texinfo = rd16(f + 10), lightofs = (int)rd32(f + 16);
		const uint8_t *t;
		float vecs[2][4], smin = 1e9f, smax = -1e9f, tmin = 1e9f, tmax = -1e9f, sbase, tbase, sscale, tscale;
		float pos[64][3], st[64][2];
		int order[64], lw, lh, bmins, bmint, texnum;
		tex_t *tex;
		face->plane = rd16(f);
		face->back = rd16(f + 2) != 0;
		face->frame = -1;
		face->next = -1;
		face->tex = -1;
		if (num < 3 || num > 64 || first < 0 || first + num > numse || texinfo < 0 || texinfo >= numti)
			continue;
		t = ti + texinfo * 40;
		for (int a = 0; a < 2; a++)
			for (int b = 0; b < 4; b++)
				vecs[a][b] = rdf(t + (a * 4 + b) * 4);
		texnum = (int)rd32(t + 32);
		if (texnum < 0 || texnum >= w.numtexs || w.texs[texnum].skip || !w.texs[texnum].name)
			continue;
		tex = &w.texs[texnum];
		for (int k = 0; k < num; k++) {
			int e = (int)rd32(se + (first + k) * 4), v;
			if (e >= 0 ? e >= numedges : -e >= numedges)
				return false;
			v = e >= 0 ? (uint16_t)rd16(ed + e * 4) : (uint16_t)rd16(ed + (-e) * 4 + 2);
			if (v >= numvx)
				return false;
			for (int c = 0; c < 3; c++)
				pos[k][c] = rdf(vx + v * 12 + c * 4);
			for (int a = 0; a < 2; a++)
				st[k][a] = pos[k][0] * vecs[a][0] + pos[k][1] * vecs[a][1] + pos[k][2] * vecs[a][2] + vecs[a][3];
			smin = fminf(smin, st[k][0]), smax = fmaxf(smax, st[k][0]);
			tmin = fminf(tmin, st[k][1]), tmax = fmaxf(tmax, st[k][1]);
		}
		bmins = (int)floorf(smin / 16), bmint = (int)floorf(tmin / 16);
		lw = (int)ceilf(smax / 16) - bmins + 1;
		lh = (int)ceilf(tmax / 16) - bmint + 1;
		/* keep texture coordinates near zero: t16 only reaches +-2048 texels */
		sbase = floorf((smin + smax) * 0.5f / tex->w) * tex->w;
		tbase = floorf((tmin + tmax) * 0.5f / tex->h) * tex->h;
		/* the sky keeps only its back layer, the left half of the picture */
		sscale = (float)tex->tw * (tex->sky ? 2 : 1) / tex->w;
		tscale = (float)tex->th / tex->h;
		/* fan order 0 1 2 ... n-1 to strip order 0 1 n-1 2 n-2 3 ... */
		for (int k = 0, lo = 1, hi = num - 1; k < num; k++) {
			if (k == 0)
				order[k] = 0;
			else if (k & 1)
				order[k] = lo++;
			else
				order[k] = hi--;
		}
		face->firstvert = w.numverts;
		face->numverts = num;
		face->tex = texnum;
		for (int k = 0; k < num; k++) {
			int o = order[k], l;
			dvert_t *dv = &w.verts[w.numverts++];
			float s = (st[o][0] - sbase) * sscale * 16, tt = (st[o][1] - tbase) * tscale * 16;
			dv->x = (int16_t)BW_NDS_CLAMP(lrintf(pos[o][0] * 8), -32767, 32767);
			dv->y = (int16_t)BW_NDS_CLAMP(lrintf(pos[o][1] * 8), -32767, 32767);
			dv->z = (int16_t)BW_NDS_CLAMP(lrintf(pos[o][2] * 8), -32767, 32767);
			dv->s = (int16_t)BW_NDS_CLAMP(lrintf(s), -32767, 32767);
			dv->t = (int16_t)BW_NDS_CLAMP(lrintf(tt), -32767, 32767);
			if (tex->fullbright)
				l = 255;
			else
				l = sample_light(light, (int)len[L_LIGHTING], lightofs, f + 12, lw, lh, st[o][0] / 16 - bmins,
				                 st[o][1] / 16 - bmint);
			dv->color = light_color(l);
		}
	}
	return true;
}

/* --- tree and visibility ---------------------------------------------------------------- */

static void load_tree(const uint8_t *d, const uint32_t *ofs, const uint32_t *len)
{
	const uint8_t *p = d + ofs[L_PLANES], *n = d + ofs[L_NODES], *l = d + ofs[L_LEAFS], *m = d + ofs[L_MARKSURFACES];
	w.numplanes = (int)(len[L_PLANES] / 20);
	w.planes = malloc(sizeof(plane_t) * (size_t)w.numplanes);
	for (int i = 0; i < w.numplanes; i++) {
		for (int c = 0; c < 3; c++)
			w.planes[i].n[c] = (int32_t)lrintf(rdf(p + i * 20 + c * 4) * 4096);
		w.planes[i].dist = (int32_t)lrintf(rdf(p + i * 20 + 12) * 32768);
	}
	w.numnodes = (int)(len[L_NODES] / 24);
	w.nodes = malloc(sizeof(node_t) * (size_t)w.numnodes);
	for (int i = 0; i < w.numnodes; i++) {
		const uint8_t *q = n + i * 24;
		node_t *nd = &w.nodes[i];
		nd->plane = (int)rd32(q);
		nd->children[0] = rd16(q + 4);
		nd->children[1] = rd16(q + 6);
		for (int c = 0; c < 3; c++)
			nd->mins[c] = rd16(q + 8 + c * 2), nd->maxs[c] = rd16(q + 14 + c * 2);
		nd->parent = -1;
		nd->visframe = 0;
	}
	w.numleafs = (int)(len[L_LEAFS] / 28);
	w.leafs = malloc(sizeof(leaf_t) * (size_t)w.numleafs);
	for (int i = 0; i < w.numleafs; i++) {
		const uint8_t *q = l + i * 28;
		leaf_t *lf = &w.leafs[i];
		lf->contents = (int)rd32(q);
		lf->visofs = (int)rd32(q + 4);
		for (int c = 0; c < 3; c++)
			lf->mins[c] = rd16(q + 8 + c * 2), lf->maxs[c] = rd16(q + 14 + c * 2);
		lf->firstmark = (uint16_t)rd16(q + 20);
		lf->nummarks = (uint16_t)rd16(q + 22);
		lf->parent = -1;
		lf->visframe = 0;
	}
	for (int i = 0; i < w.numnodes; i++)
		for (int s = 0; s < 2; s++) {
			int c = w.nodes[i].children[s];
			if (c >= 0 && c < w.numnodes)
				w.nodes[c].parent = i;
			else if (c < 0 && -(c + 1) < w.numleafs)
				w.leafs[-(c + 1)].parent = i;
		}
	w.nummarks = (int)(len[L_MARKSURFACES] / 2);
	w.marks = (uint16_t *)(uintptr_t)m; /* the file is 4-byte aligned and the lump 2-byte aligned */
	w.vis = d + ofs[L_VISIBILITY];
	w.vissize = (int)len[L_VISIBILITY];
	w.leafvis = malloc((size_t)(w.numleafs + 7) / 8 + 1);
	w.lastleaf = -2;
}

static int point_leaf(const int32_t *org)
{
	int n = 0;
	if (!w.numnodes)
		return 0;
	while (n >= 0) {
		const node_t *nd = &w.nodes[n];
		const plane_t *pl = &w.planes[nd->plane];
		int32_t d = pl->n[0] * org[0] + pl->n[1] * org[1] + pl->n[2] * org[2] - pl->dist;
		n = nd->children[d < 0];
		if (n >= w.numnodes)
			return 0;
	}
	return -(n + 1);
}

static void mark_visible(int leaf)
{
	const uint8_t *in;
	int bytes = (w.visleafs + 7) / 8;
	w.visframe++;
	if (leaf <= 0 || w.leafs[leaf].visofs < 0 || w.leafs[leaf].visofs >= w.vissize) {
		memset(w.leafvis, 0xff, (size_t)bytes); /* outside the map or no PVS: everything */
	} else {
		uint8_t *out = w.leafvis;
		in = w.vis + w.leafs[leaf].visofs;
		while (out < w.leafvis + bytes && in < w.vis + w.vissize) {
			if (*in) {
				*out++ = *in++;
				continue;
			}
			for (int c = in + 1 < w.vis + w.vissize ? in[1] : 0; c > 0 && out < w.leafvis + bytes; c--)
				*out++ = 0;
			in += 2;
		}
		while (out < w.leafvis + bytes)
			*out++ = 0;
	}
	for (int i = 0; i < w.visleafs; i++) {
		if (!(w.leafvis[i >> 3] & (1 << (i & 7))))
			continue;
		leaf_t *lf = &w.leafs[i + 1];
		if (lf->visframe == w.visframe)
			continue;
		lf->visframe = w.visframe;
		for (int n = lf->parent; n >= 0 && w.nodes[n].visframe != w.visframe; n = w.nodes[n].parent)
			w.nodes[n].visframe = w.visframe;
	}
	/* the eye's own leaf, even when the PVS leaves it out */
	if (leaf > 0) {
		w.leafs[leaf].visframe = w.visframe;
		for (int n = w.leafs[leaf].parent; n >= 0 && w.nodes[n].visframe != w.visframe; n = w.nodes[n].parent)
			w.nodes[n].visframe = w.visframe;
	}
}

/* --- loading ----------------------------------------------------------------------------- */

bool World_Load(const char *path, const uint16_t *palette, char *err, int errsize)
{
	size_t fsize;
	long size;
	uint32_t ofs[NUM_LUMPS], len[NUM_LUMPS];
	World_Free();
	w.file = Fs_Load(path, &fsize); /* with a spare byte for a 0 */
	if (!w.file) {
		snprintf(err, (size_t)errsize, "can't read %s (missing, or not enough memory)", path);
		return false;
	}
	size = (long)fsize;
	w.file[size] = 0;
	w.filesize = (size_t)size;
	if (size < 4 + NUM_LUMPS * 8 || rd32(w.file) != BSPVERSION) {
		snprintf(err, (size_t)errsize, "not a Quake map (BSP version %d)", size >= 4 ? (int)rd32(w.file) : -1);
		World_Free();
		return false;
	}
	for (int i = 0; i < NUM_LUMPS; i++) {
		ofs[i] = rd32(w.file + 4 + i * 8);
		len[i] = rd32(w.file + 8 + i * 8);
		if (ofs[i] > (uint32_t)size || len[i] > (uint32_t)size - ofs[i]) {
			snprintf(err, (size_t)errsize, "lump %d runs past the end", i);
			World_Free();
			return false;
		}
	}
	/* the entities are text: make sure they end */
	w.entities = (const char *)w.file + ofs[L_ENTITIES];
	if (len[L_ENTITIES])
		w.file[ofs[L_ENTITIES] + len[L_ENTITIES] - 1] = 0;

	load_textures(w.file + ofs[L_TEXTURES], (int)len[L_TEXTURES], palette);
	load_tree(w.file, ofs, len);
	if (!load_faces(w.file, ofs, len)) {
		snprintf(err, (size_t)errsize, "bad faces, or out of memory");
		World_Free();
		return false;
	}
	w.nummodels = (int)(len[L_MODELS] / 64);
	w.models = malloc(sizeof(submodel_t) * (size_t)(w.nummodels ? w.nummodels : 1));
	for (int i = 0; i < w.nummodels; i++) {
		const uint8_t *q = w.file + ofs[L_MODELS] + i * 64;
		for (int c = 0; c < 3; c++) {
			w.models[i].mins[c] = (int16_t)BW_NDS_CLAMP(lrintf(rdf(q + c * 4)), -32767, 32767);
			w.models[i].maxs[c] = (int16_t)BW_NDS_CLAMP(lrintf(rdf(q + 12 + c * 4)), -32767, 32767);
		}
		w.models[i].firstface = (int)rd32(q + 56);
		w.models[i].numfaces = (int)rd32(q + 60);
		if (i == 0)
			w.visleafs = BW_NDS_MIN((int)rd32(q + 52), w.numleafs - 1);
	}
	/* the lighting and pictures have been used up; the rest of the file
	 * (planes, vis, marks) is still read from */
	w.loaded = true;
	return true;
}

/* where info_player_start stands, or the middle of the map */
void World_StartPosition(ndscam_t *cam)
{
	const char *e = w.entities, *p;
	float org[3] = {0, 0, 0}, angle = 0;
	bool found = false;
	if (w.nummodels > 0)
		for (int c = 0; c < 3; c++)
			org[c] = (w.models[0].mins[c] + w.models[0].maxs[c]) * 0.5f;
	while (e && (p = strstr(e, "info_player_start"))) {
		const char *start = p, *end = strchr(p, '}'), *o, *a;
		while (start > w.entities && *start != '{')
			start--;
		o = strstr(start, "\"origin\"");
		a = strstr(start, "\"angle\"");
		if (o && (!end || o < end) && sscanf(o + 8, " \"%f %f %f\"", &org[0], &org[1], &org[2]) == 3)
			found = true;
		if (a && (!end || a < end))
			sscanf(a + 7, " \"%f\"", &angle);
		if (found)
			break;
		e = p + 1;
	}
	if (found)
		org[2] += 22; /* eye height */
	for (int c = 0; c < 3; c++)
		cam->org[c] = (int32_t)lrintf(org[c] * 8);
	cam->yaw = (int)(angle * 32768 / 360);
	cam->pitch = 0;
}

/* --- drawing ----------------------------------------------------------------------------- */

typedef struct {
	int32_t n[3]; /* 4.12, pointing into the view */
} fplane_t;

static fplane_t frustum[4];
static int32_t eye[3];

/* false if the box (map units) is wholly outside the view */
static bool box_visible(const int16_t *mins, const int16_t *maxs)
{
	for (int i = 0; i < 4; i++) {
		const int32_t *n = frustum[i].n;
		int32_t x = (n[0] >= 0 ? maxs[0] : mins[0]) * 8 - eye[0];
		int32_t y = (n[1] >= 0 ? maxs[1] : mins[1]) * 8 - eye[1];
		int32_t z = (n[2] >= 0 ? maxs[2] : mins[2]) * 8 - eye[2];
		if (n[0] * x + n[1] * y + n[2] * z < 0)
			return false;
	}
	return true;
}

static inline void add_face(int i)
{
	face_t *f = &w.faces[i];
	const plane_t *pl;
	int32_t d;
	if (f->frame == w.frame || f->tex < 0)
		return;
	f->frame = w.frame;
	pl = &w.planes[f->plane];
	d = pl->n[0] * eye[0] + pl->n[1] * eye[1] + pl->n[2] * eye[2] - pl->dist;
	if (f->back ? d > 0 : d < 0)
		return; /* turned away */
	w.vislist[w.numvis++] = i;
}

static void walk(int n)
{
	while (n >= 0) {
		node_t *nd = &w.nodes[n];
		const plane_t *pl = &w.planes[nd->plane];
		int near;
		if (nd->visframe != w.visframe || !box_visible(nd->mins, nd->maxs))
			return;
		/* the eye's side first, so faces come out nearest first */
		near = pl->n[0] * eye[0] + pl->n[1] * eye[1] + pl->n[2] * eye[2] - pl->dist < 0;
		walk(nd->children[near]);
		n = nd->children[!near];
		if (n >= w.numnodes)
			return;
	}
	{
		leaf_t *lf = &w.leafs[-(n + 1)];
		if (-(n + 1) == 0 || lf->visframe != w.visframe || !box_visible(lf->mins, lf->maxs))
			return;
		for (int m = 0; m < lf->nummarks; m++) {
			int idx = lf->firstmark + m;
			if (idx < w.nummarks)
				add_face(w.marks[idx]);
		}
	}
}

static void setup_frustum(const ndscam_t *cam)
{
	/* the view's axes in map space (4.12) */
	int sy = sinLerp(cam->yaw), cy = cosLerp(cam->yaw), sp = sinLerp(cam->pitch), cp = cosLerp(cam->pitch);
	int32_t fwd[3] = {cp * cy >> 12, cp * sy >> 12, sp};
	int32_t right[3] = {sy, -cy, 0};
	int32_t up[3] = {-sp * cy >> 12, -sp * sy >> 12, cp};
	/* half angles: 35 degrees up and down; 4:3 makes about 43 across.
	 * Each side plane leans in from the eye: n = fwd * sin(a) +- side * cos(a) */
	const int sv = sinLerp(35 * 32768 / 360), cv = cosLerp(35 * 32768 / 360);
	const int sh = sinLerp(44 * 32768 / 360), ch = cosLerp(44 * 32768 / 360);
	for (int c = 0; c < 3; c++) {
		frustum[0].n[c] = (fwd[c] * sh + right[c] * ch) >> 12; /* left edge: normal points right */
		frustum[1].n[c] = (fwd[c] * sh - right[c] * ch) >> 12;
		frustum[2].n[c] = (fwd[c] * sv + up[c] * cv) >> 12;
		frustum[3].n[c] = (fwd[c] * sv - up[c] * cv) >> 12;
	}
}

/* faces go to the GPU as quad strips: a quad is one of the 2048 polygons,
 * as a triangle is, so an n-sided face costs (n - 1) / 2 polygons rather
 * than n - 2. An odd face repeats its last vertex, closing with a triangle. */
#define FACE_POLYS(f) (((f)->numverts - 1) / 2)
#define FACE_VERTS(f) ((f)->numverts + ((f)->numverts & 1))

/* adds a face to its texture's chain if it fits what's left of the budget */
static inline bool chain_face(int i, int *verts, int *polys, int maxverts, int maxpolys, ndsstats_t *st)
{
	face_t *f = &w.faces[i];
	if (*verts + FACE_VERTS(f) > maxverts || *polys + FACE_POLYS(f) > maxpolys) {
		st->skipped++;
		return false;
	}
	*verts += FACE_VERTS(f);
	*polys += FACE_POLYS(f);
	st->faces++;
	f->next = w.texs[f->tex].chain;
	w.texs[f->tex].chain = i;
	return true;
}

ITCM_CODE static void draw_chains(void)
{
	for (int t = 0; t < w.numtexs; t++) {
		tex_t *tex = &w.texs[t];
		int i = tex->chain;
		if (i < 0)
			continue;
		glBindTexture(GL_TEXTURE_2D, tex->name);
		for (; i >= 0; i = w.faces[i].next) {
			const face_t *f = &w.faces[i];
			const dvert_t *v = &w.verts[f->firstvert];
			glBegin(GL_QUAD_STRIP);
			for (int k = 0; k < f->numverts; k++, v++) {
				glColor(v->color);
				glTexCoord2t16(v->s, v->t);
				glVertex3v16(v->x, v->y, v->z);
			}
			if (f->numverts & 1) {
				v--;
				glVertex3v16(v->x, v->y, v->z);
			}
			glEnd();
		}
		tex->chain = -1;
	}
}

void World_SetView(const ndscam_t *cam)
{
	/* map axes to the GPU's: map x (forward) is -z, map y (left) is -x, map z (up) is y */
	static const m4x4 axes = {{0, 0, -4096, 0, -4096, 0, 0, 0, 0, 4096, 0, 0, 0, 0, 0, 4096}};
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glRotateXi(-cam->pitch);
	glRotateYi(-cam->yaw);
	glMultMatrix4x4(&axes);
	glTranslatef32(-cam->org[0], -cam->org[1], -cam->org[2]);
}

void World_Begin(const ndscam_t *cam, ndsstats_t *st)
{
	int leaf;
	memset(st, 0, sizeof(*st));
	World_SetView(cam);
	if (!w.loaded)
		return;
	w.frame++;
	memcpy(eye, cam->org, sizeof(eye));
	leaf = point_leaf(eye);
	if (leaf != w.lastleaf) {
		mark_visible(leaf);
		w.lastleaf = leaf;
	}
	st->leaf = leaf;
	setup_frustum(cam);
}

bool World_BoxVisible(const int16_t *mins, const int16_t *maxs)
{
	int32_t mid[3];
	int leaf;
	if (!w.loaded)
		return true;
	if (!box_visible(mins, maxs))
		return false;
	for (int c = 0; c < 3; c++)
		mid[c] = (mins[c] + maxs[c]) * 4; /* the middle, in 1/8 units */
	leaf = point_leaf(mid);
	return leaf <= 0 || w.leafs[leaf].visframe == w.visframe;
}

void World_Collect(void)
{
	w.numvis = 0;
	if (!w.loaded)
		return;
	if (w.numnodes)
		walk(0);
	w.numtreevis = w.numvis;
	/* doors, lifts and other brush entities: all their faces, by box */
	for (int m = 1; m < w.nummodels; m++) {
		const submodel_t *sm = &w.models[m];
		int32_t d = 0;
		if (!box_visible(sm->mins, sm->maxs))
			continue;
		for (int c = 0; c < 3; c++) {
			int32_t v = (sm->mins[c] + sm->maxs[c]) / 2 - eye[c] / 8;
			d += v * v;
		}
		for (int i = 0; i < sm->numfaces; i++)
			if (sm->firstface + i < w.numfaces) {
				int before = w.numvis;
				add_face(sm->firstface + i);
				if (w.numvis > before)
					w.visdist[before] = d;
			}
	}
}

int World_Split(int maxverts, int maxpolys, const int32_t *mdist2, const int *mpolys, int nummodels, int32_t *dist2)
{
	int verts = 0, polys = 0, k = 0, m = 0;
	*dist2 = INT32_MAX;
	/* room for every brush entity, whichever pass it lands in */
	for (int i = w.numtreevis; i < w.numvis; i++)
		verts += FACE_VERTS(&w.faces[w.vislist[i]]), polys += FACE_POLYS(&w.faces[w.vislist[i]]);
	for (; k < w.numtreevis; k++) {
		const face_t *f = &w.faces[w.vislist[k]];
		const dvert_t *v = &w.verts[f->firstvert];
		int32_t dx = (v->x - eye[0]) / 8, dy = (v->y - eye[1]) / 8, dz = (v->z - eye[2]) / 8;
		int32_t d = dx * dx + dy * dy + dz * dz;
		/* the models nearer than this face come first */
		while (m < nummodels && mdist2[m] < d) {
			if (polys + mpolys[m] > maxpolys || verts + mpolys[m] * 3 > maxverts)
				break;
			polys += mpolys[m], verts += mpolys[m] * 3, m++;
		}
		if ((m < nummodels && mdist2[m] < d) || verts + FACE_VERTS(f) > maxverts ||
		    polys + FACE_POLYS(f) > maxpolys) {
			*dist2 = m < nummodels && mdist2[m] < d ? mdist2[m] : d;
			break;
		}
		verts += FACE_VERTS(f), polys += FACE_POLYS(f);
	}
	return k;
}

int World_NumTreeFaces(void) { return w.numtreevis; }

int World_Cost(int from, int to, int32_t split2, bool nearside, int *verts)
{
	int v = 0, p = 0;
	for (int k = from; k < to && k < w.numtreevis; k++)
		v += FACE_VERTS(&w.faces[w.vislist[k]]), p += FACE_POLYS(&w.faces[w.vislist[k]]);
	for (int k = w.numtreevis; k < w.numvis; k++)
		if ((w.visdist[k] < split2) == nearside)
			v += FACE_VERTS(&w.faces[w.vislist[k]]), p += FACE_POLYS(&w.faces[w.vislist[k]]);
	*verts = v;
	return p;
}

void World_DrawPass(int from, int to, int32_t split2, bool nearside, ndsstats_t *st, int maxverts, int maxpolys)
{
	int verts = 0, polys = 0;
	if (!w.loaded)
		return;
	for (int k = from; k < to && k < w.numtreevis; k++)
		chain_face(w.vislist[k], &verts, &polys, maxverts, maxpolys, st);
	for (int k = w.numtreevis; k < w.numvis; k++)
		if ((w.visdist[k] < split2) == nearside)
			chain_face(w.vislist[k], &verts, &polys, maxverts, maxpolys, st);
	st->verts += verts, st->polys += polys;
	draw_chains();
}

void World_End(ndsstats_t *st, int reserve_verts, int reserve_polys)
{
	World_Collect();
	World_DrawPass(0, w.numtreevis, INT32_MAX, true, st, MAX_GPU_VERTS - reserve_verts, MAX_GPU_POLYS - reserve_polys);
}

void World_Draw(const ndscam_t *cam, ndsstats_t *st)
{
	World_Begin(cam, st);
	World_End(st, 0, 0);
}

const char *World_Entities(void) { return w.loaded ? w.entities : NULL; }

/* about how bright the floor under a point (map units) is, 0..31, from the
 * baked vertex colours of the nearest upward face below it */
int World_LightAt(const int16_t *org)
{
	int best = -1;
	int32_t bestdz = 0x7fffffff;
	if (!w.loaded)
		return 24;
	for (int drop = 0; drop <= 128 && best < 0; drop += 64) {
		int32_t p[3] = {org[0] * 8, org[1] * 8, (org[2] - drop) * 8};
		int leaf = point_leaf(p);
		const leaf_t *lf;
		if (leaf <= 0)
			continue;
		lf = &w.leafs[leaf];
		for (int m = 0; m < lf->nummarks && lf->firstmark + m < w.nummarks; m++) {
			const face_t *f = &w.faces[w.marks[lf->firstmark + m]];
			int32_t nz = w.planes[f->plane].n[2], z = 0;
			if (f->tex < 0 || (f->back ? -nz : nz) < 2800 || !f->numverts)
				continue;
			for (int k = 0; k < f->numverts; k++)
				z += w.verts[f->firstvert + k].z;
			z /= f->numverts;
			if (z <= org[2] * 8 && org[2] * 8 - z < bestdz)
				bestdz = org[2] * 8 - z, best = (int)(f - w.faces);
		}
	}
	if (best < 0)
		return 24;
	{
		const face_t *f = &w.faces[best];
		int sum = 0;
		for (int k = 0; k < f->numverts; k++)
			sum += w.verts[f->firstvert + k].color & 31;
		return sum / f->numverts;
	}
}
