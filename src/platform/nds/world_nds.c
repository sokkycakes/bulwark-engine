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
#include "hull_nds.h"
#include "portals_nds.h"

#define BSPVERSION 29
/* the hot loops run from ITCM as ARM code (the rest of the file is Thumb) */
#define ARM_CODE __attribute__((target("arm")))
#define MAX_GPU_VERTS WORLD_MAX_VERTS
#define MAX_GPU_POLYS WORLD_MAX_POLYS
#define TEX_BUDGET (192 * 1024) /* VRAM banks A and B, less 64 KB for model skins (C and D hold captures) */
#define TEX_MAXDIM 256

enum { L_ENTITIES, L_PLANES, L_TEXTURES, L_VERTEXES, L_VISIBILITY, L_NODES, L_TEXINFO, L_FACES, L_LIGHTING,
       L_CLIPNODES, L_LEAFS, L_MARKSURFACES, L_EDGES, L_SURFEDGES, L_MODELS, NUM_LUMPS };

/* in the order the GPU takes them, so a vertex is three words: s t is
 * TEXCOORD's parameter, x y VTX_16's first and z its second (the GPU
 * ignores the top half, where the colour is) */
typedef struct {
	int16_t s, t;    /* t16: texels * 16 */
	int16_t x, y;    /* 1/8 map units */
	int16_t z;
	uint16_t color;  /* RGB15 */
} dvert_t;
_Static_assert(sizeof(dvert_t) == 12, "three words a vertex");

typedef struct {
	int32_t n[3]; /* 4.12 */
	int32_t dist; /* map units << 15, to compare with n . (1/8 units) */
} plane_t;

typedef struct {
	int plane;
	bool back;      /* the face looks along -normal */
	int8_t ptype;   /* the plane's axis when it is one (normal +x, +y or +z), else -1 */
	int firstvert;  /* into verts[], in strip order */
	int numverts;
	int tex;        /* -1: not drawn */
	int frame;
	int next;       /* texture chain */
	int alias;      /* merged into this face, or -1 */
} face_t;

typedef struct {
	int plane;
	int children[2]; /* >= 0 node, < 0 leaf -(c + 1) */
	int16_t mins[3], maxs[3];
	int16_t parent;
	int8_t ptype;    /* as face_t's */
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
	/* where a server (link play) says the brush entity is now: shown or not,
	 * and moved from where the map built it (doors, lifts, trains) */
	bool hidden, moved;
	int32_t org[3]; /* 1/8 units */
	int ang[3];     /* pitch yaw roll, libnds angles */
	int32_t home[3]; /* the entity's "origin" in the map: Half-Life builds models with an
	                    origin brush round 0 0 0 and puts them there */
	int headnode0;   /* the model's root in the drawing tree, for hull 0 traces */
} submodel_t;

static struct {
	bool loaded;
	uint8_t *entbuf, *markbuf, *visbuf; /* the lumps kept from the file */
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
	int16_t *vismodel; /* for brush entities' faces: which model */
	int numvis, numtreevis;
	pt_portals_t portals; /* none (numportals 0) if they couldn't be worked out */
	int16_t (*leafrect)[4]; /* this frame: the part of the screen each leaf is seen through */
	int *leafstamp, *queue;
	uint8_t *inqueue;
	int flood;
	bool flooded;           /* the last frame's leaves came from the portals */
	int portalleaves;       /* leaves the portals reached this frame */
	int portaltests, pvsleaves;
	int loadms;             /* portal building time, for the stats */
	bool haslit;            /* coloured light from a .lit */
	int modelgen;           /* counts changes to the brush entities' states (World_ModelState) */
	/* what the last World_Collect worked from: the same again gives the same list */
	struct {
		bool valid;
		int32_t eye[3];
		int yaw, pitch, roll, visframe, modelgen;
	} kept;
	int viewyaw, viewpitch, viewroll; /* this frame's (World_Begin) */
	int texfailed;          /* textures VRAM had no room for */
} w;

static inline uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static const uint8_t *lp[NUM_LUMPS]; /* the lumps read in while loading (see World_Load) */
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
	free(w.entbuf), free(w.markbuf), free(w.visbuf);
	free(w.planes);
	free(w.nodes);
	free(w.leafs);
	free(w.faces);
	free(w.verts);
	free(w.texs);
	free(w.models);
	free(w.leafvis);
	Pt_Free(&w.portals);
	free(w.leafrect), free(w.leafstamp), free(w.queue), free(w.inqueue);
	free(w.vislist);
	free(w.visdist);
	free(w.vismodel);
	memset(&w, 0, sizeof(w));
	Hull_Free();
}

bool World_Loaded(void) { return w.loaded; }
int World_TextureBytes(void) { return w.texbytes; }
bool World_HasLit(void) { return w.haslit; }
int World_TextureFails(void) { return w.texfailed; }

/* --- textures ---------------------------------------------------------------------------- */

static bool name_is(const char *name, const char *what) { return strncasecmp(name, what, 16) == 0; }

/* the size the mips really have: the header's, or for a Half-Life map
 * shrunk on the PC (tools/nds/hlmap.py) the one in the "DSz" tag after it;
 * the header keeps the original size, which texture coordinates use */
static void stored_size(const uint8_t *mt, int avail, int *pw, int *ph)
{
	*pw = (int)rd32(mt + 16), *ph = (int)rd32(mt + 20);
	if (avail >= 48 && rd32(mt + 24) == 48 && !memcmp(mt + 40, "DSz", 4))
		*pw = (uint16_t)rd16(mt + 44), *ph = (uint16_t)rd16(mt + 46);
}

static int plane_type(int p);
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
			int ofs = (int)rd32(lump + 4 + i * 4), tw, th, pw, ph;
			if (ofs <= 0 || ofs + 40 > len)
				continue;
			stored_size(lump + ofs, len - ofs, &pw, &ph);
			tw = pot_floor(pw >> level);
			th = pot_floor(ph >> level);
			total += BW_NDS_MIN(tw, TEX_MAXDIM) * BW_NDS_MIN(th, TEX_MAXDIM);
		}
		if (total <= TEX_BUDGET)
			break;
	}

	for (int i = 0; i < count; i++) {
		tex_t *t = &w.texs[i];
		int ofs = (int)rd32(lump + 4 + i * 4), sw, sh, tw, th, pw, ph;
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
		stored_size(mt, len - ofs, &pw, &ph);
		sw = pw >> level, sh = ph >> level;
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
				uint8_t c = src[(y * sh / th) * (pw >> level) + x * sw / tw];
				if (alpha)
					c = c == 255 ? 0 : c == 0 ? 255 : c;
				pix[y * tw + x] = c;
			}
		glGenTextures(1, &t->name);
		glBindTexture(GL_TEXTURE_2D, t->name);
		if (!glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB256, size_enum(tw), size_enum(th), 0,
		                  GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T | (alpha ? GL_TEXTURE_COLOR0_TRANSPARENT : 0), pix)) {
			w.texfailed++;
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

/* the light at a point of a face, from its lightmap(s), bilinear; samples
 * are stride bytes apart (1 for the map's own, 3 for a .lit), and chan picks
 * the byte. lightlen counts samples. */
static int sample_light(const uint8_t *light, int lightlen, int lightofs, const uint8_t *styles, int lw, int lh,
                        float u, float v, int stride, int chan)
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
		const uint8_t *m = light + (lightofs + s * lw * lh) * stride + chan;
		if (lightofs + (s + 1) * lw * lh > lightlen)
			break;
		total += (int)((m[(y0 * lw + x0) * stride] * (1 - fx) + m[(y0 * lw + x1) * stride] * fx) * (1 - fy) +
		               (m[(y1 * lw + x0) * stride] * (1 - fx) + m[(y1 * lw + x1) * stride] * fx) * fy);
	}
	return total;
}

/* --- merging ---------------------------------------------------------------------------
 *
 * The map compiler cuts walls into many small faces (along BSP splits, and
 * into pieces small enough for Quake's software surface cache). At load,
 * faces on the same plane with the same texture mapping are joined again
 * where they share a whole edge and the result stays convex, and vertices
 * left in the middle of a straight edge are dropped when the light there
 * is what the edge would give anyway. Each face's leaves still see it: a
 * face that was merged away points at the face that took it in (alias). */

typedef struct {
	int16_t x, y, z;
	int32_t s, t;   /* t16, not yet moved near zero */
	uint16_t color;
} mvert_t;

typedef struct {
	int first, num; /* into the mvert pool, fan order */
	int key;        /* model, for grouping */
	int texinfo;
} mface_t;

#define MERGE_MAXVERTS 32

bool World_MergeFaces = true;
int World_MergeLight = 2; /* of 31: how far a dropped vertex's light may be from what its edge gives */
static int merged_faces;

static bool same_pos(const mvert_t *a, const mvert_t *b) { return a->x == b->x && a->y == b->y && a->z == b->z; }

/* a joined polygon in out[], or 0 if a and b don't share an edge or the join isn't convex */
static int try_merge(const mvert_t *a, int na, const mvert_t *b, int nb, const float *normal, mvert_t *out)
{
	mvert_t tmp[2 * MERGE_MAXVERTS];
	int n = 0, i, j;
	for (i = 0; i < na; i++) {
		const mvert_t *a0 = &a[i], *a1 = &a[(i + 1) % na];
		for (j = 0; j < nb; j++)
			if (same_pos(&b[j], a1) && same_pos(&b[(j + 1) % nb], a0))
				goto found;
	}
	return 0;
found:
	/* a from the end of the shared edge round to its start, then b's other vertices */
	for (int k = 0; k < na; k++)
		tmp[n++] = a[(i + 1 + k) % na];
	for (int k = 2; k < nb; k++)
		tmp[n++] = b[(j + k) % nb];
	/* drop straight-through vertices whose light is what their edge gives
	 * anyway (one that isn't stays, as a corner of 180 degrees); any turn
	 * the wrong way means the join isn't convex */
	{
		int m = 0;
		for (int k = 0; k < n; k++) {
			const mvert_t *p = &tmp[(k + n - 1) % n], *v = &tmp[k], *q = &tmp[(k + 1) % n];
			float e1[3] = {v->x - p->x, v->y - p->y, v->z - p->z}, e2[3] = {q->x - v->x, q->y - v->y, q->z - v->z};
			float c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
			float turn = c[0] * normal[0] + c[1] * normal[1] + c[2] * normal[2];
			float l1 = sqrtf(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
			float l2 = sqrtf(e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2]);
			if (turn < -0.002f * l1 * l2)
				return 0;
			if (turn <= 0.002f * l1 * l2 && l1 + l2 > 0) {
				int want = (int)(((p->color & 31) * l2 + (q->color & 31) * l1) / (l1 + l2) + 0.5f);
				if (abs((v->color & 31) - want) <= World_MergeLight)
					continue;
			}
			out[m++] = *v;
		}
		n = m;
	}
	return n >= 3 && n <= MERGE_MAXVERTS ? n : 0;
}

static const mface_t *sort_faces;
static const face_t *sort_dfaces;
static int cmp_faces(const void *pa, const void *pb)
{
	int a = *(const int *)pa, b = *(const int *)pb;
	const mface_t *fa = &sort_faces[a], *fb = &sort_faces[b];
	if (fa->key != fb->key)
		return fa->key - fb->key;
	if (sort_dfaces[a].plane != sort_dfaces[b].plane)
		return sort_dfaces[a].plane - sort_dfaces[b].plane;
	if (sort_dfaces[a].back != sort_dfaces[b].back)
		return sort_dfaces[a].back - sort_dfaces[b].back;
	if (fa->texinfo != fb->texinfo)
		return fa->texinfo - fb->texinfo;
	return a - b;
}

/* after moving near zero by whole repeats, every coordinate has to fit t16 */
static bool fits_t16(const mvert_t *v, int n)
{
	int32_t smin = v[0].s, smax = v[0].s, tmin = v[0].t, tmax = v[0].t;
	for (int k = 1; k < n; k++) {
		smin = v[k].s < smin ? v[k].s : smin, smax = v[k].s > smax ? v[k].s : smax;
		tmin = v[k].t < tmin ? v[k].t : tmin, tmax = v[k].t > tmax ? v[k].t : tmax;
	}
	return smax - smin < 24000 && tmax - tmin < 24000;
}

static bool other_group(int a, int b, const mface_t *mf)
{
	return mf[a].key != mf[b].key || w.faces[a].plane != w.faces[b].plane || w.faces[a].back != w.faces[b].back ||
	       mf[a].texinfo != mf[b].texinfo;
}

static void merge_faces(mface_t *mf, mvert_t *pool, int *poolused, int poolsize)
{
	int *idx = malloc(sizeof(int) * (size_t)w.numfaces);
	if (!idx)
		return;
	for (int i = 0; i < w.numfaces; i++)
		idx[i] = i;
	sort_faces = mf, sort_dfaces = w.faces;
	qsort(idx, (size_t)w.numfaces, sizeof(int), cmp_faces);
	for (int g = 0; g < w.numfaces;) {
		int e = g + 1;
		while (e < w.numfaces && !other_group(idx[g], idx[e], mf))
			e++;
		for (int a = g; a < e; a++) {
			face_t *fa = &w.faces[idx[a]];
			mface_t *ma = &mf[idx[a]];
			float normal[3];
			bool again = true;
			if (fa->tex < 0 || fa->alias >= 0 || w.texs[fa->tex].sky)
				continue;
			/* the face's own winding gives which way is convex */
			normal[0] = normal[1] = normal[2] = 0;
			for (int k = 0; k < ma->num; k++) {
				const mvert_t *v = &pool[ma->first + k], *u = &pool[ma->first + (k + 1) % ma->num];
				normal[0] += (float)(v->y - u->y) * (v->z + u->z);
				normal[1] += (float)(v->z - u->z) * (v->x + u->x);
				normal[2] += (float)(v->x - u->x) * (v->y + u->y);
			}
			{
				float l = sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
				if (l <= 0)
					continue;
				normal[0] /= l, normal[1] /= l, normal[2] /= l;
			}
			while (again) {
				again = false;
				for (int b = g; b < e; b++) {
					face_t *fb = &w.faces[idx[b]];
					mface_t *mb = &mf[idx[b]];
					int n;
					if (b == a || fb->tex < 0 || fb->alias >= 0)
						continue;
					if (*poolused + ma->num + mb->num > poolsize)
						goto out;
					n = try_merge(&pool[ma->first], ma->num, &pool[mb->first], mb->num, normal, &pool[*poolused]);
					if (!n || !fits_t16(&pool[*poolused], n))
						continue;
					ma->first = *poolused, ma->num = n;
					*poolused += n;
					fb->alias = idx[a];
					merged_faces++;
					again = true;
				}
			}
		}
		g = e;
	}
out:
	free(idx);
}

/* coloured light for the map being loaded: a Quake-style .lit (QLIT,
 * version 1, RGB a sample), or NULL */
static const uint8_t *lit;
static int litsamples;

static int light_channel(int c)
{
	int v = c * 31 / 140;
	return v > 31 ? 31 : v < 2 ? 2 : v;
}

static bool load_faces(const uint8_t *d, const uint32_t *ofs, const uint32_t *len)
{
	const uint8_t *vx = lp[L_VERTEXES], *ed = lp[L_EDGES], *se = lp[L_SURFEDGES];
	const uint8_t *ti = lp[L_TEXINFO], *fc = lp[L_FACES], *light = lp[L_LIGHTING];
	int numvx = (int)(len[L_VERTEXES] / 12), numedges = (int)(len[L_EDGES] / 4);
	int numse = (int)(len[L_SURFEDGES] / 4), numti = (int)(len[L_TEXINFO] / 40);
	w.numfaces = (int)(len[L_FACES] / 20);
	w.faces = calloc((size_t)w.numfaces, sizeof(face_t));
	w.vislist = malloc(sizeof(int) * (size_t)(w.numfaces + 1));
	w.visdist = malloc(sizeof(int32_t) * (size_t)(w.numfaces + 1));
	w.vismodel = malloc(sizeof(int16_t) * (size_t)(w.numfaces + 1));
	int poolsize = numse + (World_MergeFaces ? numse / 2 : 0), poolused = 0;
	mvert_t *pool = malloc(sizeof(mvert_t) * (size_t)poolsize);
	mface_t *mf = calloc((size_t)w.numfaces, sizeof(mface_t));
	const uint8_t *mo = lp[L_MODELS];
	int nummodels = (int)(len[L_MODELS] / 64);
	if (!w.faces || !w.vislist || !w.visdist || !w.vismodel || !pool || !mf) {
		free(pool), free(mf);
		return false;
	}
	/* which brush model each face belongs to: faces merge only within one */
	for (int m = 0; m < nummodels; m++) {
		int first = (int)rd32(mo + m * 64 + 56), num = (int)rd32(mo + m * 64 + 60);
		for (int i = first; i < first + num && i < w.numfaces; i++)
			if (i >= 0)
				mf[i].key = m;
	}
	merged_faces = 0;
	for (int i = 0; i < w.numfaces; i++) {
		const uint8_t *f = fc + i * 20;
		face_t *face = &w.faces[i];
		int first = (int)rd32(f + 4), num = rd16(f + 8), texinfo = rd16(f + 10), lightofs = (int)rd32(f + 16);
		const uint8_t *t;
		float vecs[2][4], smin = 1e9f, smax = -1e9f, tmin = 1e9f, tmax = -1e9f, sscale, tscale;
		float pos[64][3], st[64][2];
		int lw, lh, bmins, bmint, texnum;
		tex_t *tex;
		face->plane = rd16(f);
		face->back = rd16(f + 2) != 0;
		face->ptype = (int8_t)plane_type(face->plane);
		face->frame = -1;
		face->next = -1;
		face->tex = -1;
		face->alias = -1;
		mf[i].texinfo = texinfo;
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
				goto bad;
			v = e >= 0 ? (uint16_t)rd16(ed + e * 4) : (uint16_t)rd16(ed + (-e) * 4 + 2);
			if (v >= numvx)
				goto bad;
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
		sscale = (float)tex->tw * (tex->sky ? 2 : 1) / tex->w;
		tscale = (float)tex->th / tex->h;
		face->tex = texnum;
		mf[i].first = poolused, mf[i].num = num;
		for (int k = 0; k < num; k++) {
			mvert_t *mv = &pool[poolused++];
			int l;
			mv->x = (int16_t)BW_NDS_CLAMP(lrintf(pos[k][0] * 8), -32767, 32767);
			mv->y = (int16_t)BW_NDS_CLAMP(lrintf(pos[k][1] * 8), -32767, 32767);
			mv->z = (int16_t)BW_NDS_CLAMP(lrintf(pos[k][2] * 8), -32767, 32767);
			mv->s = lrintf(st[k][0] * sscale * 16);
			mv->t = lrintf(st[k][1] * tscale * 16);
			if (tex->fullbright) {
				mv->color = light_color(255);
			} else if (lit) {
				int rgb[3];
				for (int c = 0; c < 3; c++)
					rgb[c] = light_channel(sample_light(lit, litsamples, lightofs, f + 12, lw, lh, st[k][0] / 16 - bmins,
					                                    st[k][1] / 16 - bmint, 3, c));
				mv->color = RGB15(rgb[0], rgb[1], rgb[2]);
			} else {
				l = sample_light(light, (int)len[L_LIGHTING], lightofs, f + 12, lw, lh, st[k][0] / 16 - bmins,
				                 st[k][1] / 16 - bmint, 1, 0);
				mv->color = light_color(l);
			}
		}
	}
	if (World_MergeFaces)
		merge_faces(mf, pool, &poolused, poolsize);
	/* out to the GPU's layout: strip order, texture coordinates moved near zero
	 * (t16 only reaches +-2048 texels) by whole repeats of the texture */
	w.verts = malloc(sizeof(dvert_t) * (size_t)numse);
	if (!w.verts) {
		free(pool), free(mf);
		return false;
	}
	for (int i = 0; i < w.numfaces; i++) {
		face_t *face = &w.faces[i];
		const mvert_t *mv = &pool[mf[i].first];
		int num = mf[i].num, order[64];
		int32_t smin = INT32_MAX, smax = INT32_MIN, tmin = INT32_MAX, tmax = INT32_MIN, sbase, tbase, sper, tper;
		face->firstvert = w.numverts;
		face->numverts = 0;
		if (face->tex < 0 || face->alias >= 0 || num < 3)
			continue;
		sper = w.texs[face->tex].tw * 16 * (w.texs[face->tex].sky ? 2 : 1);
		tper = w.texs[face->tex].th * 16;
		for (int k = 0; k < num; k++) {
			smin = mv[k].s < smin ? mv[k].s : smin, smax = mv[k].s > smax ? mv[k].s : smax;
			tmin = mv[k].t < tmin ? mv[k].t : tmin, tmax = mv[k].t > tmax ? mv[k].t : tmax;
		}
		sbase = (int32_t)floorf((smin + smax) * 0.5f / sper) * sper;
		tbase = (int32_t)floorf((tmin + tmax) * 0.5f / tper) * tper;
		/* fan order 0 1 2 ... n-1 to strip order 0 1 n-1 2 n-2 3 ... */
		for (int k = 0, lo = 1, hi = num - 1; k < num; k++) {
			if (k == 0)
				order[k] = 0;
			else if (k & 1)
				order[k] = lo++;
			else
				order[k] = hi--;
		}
		face->numverts = num;
		for (int k = 0; k < num; k++) {
			const mvert_t *v = &mv[order[k]];
			dvert_t *dv = &w.verts[w.numverts++];
			dv->x = v->x, dv->y = v->y, dv->z = v->z;
			dv->s = (int16_t)BW_NDS_CLAMP(v->s - sbase, -32767, 32767);
			dv->t = (int16_t)BW_NDS_CLAMP(v->t - tbase, -32767, 32767);
			dv->color = v->color;
		}
	}
	free(pool), free(mf);
	return true;
bad:
	free(pool), free(mf);
	return false;
}

int World_MergedFaces(void) { return merged_faces; }

int World_TotalPolys(void)
{
	int p = 0;
	for (int i = 0; i < w.numfaces; i++)
		if (w.faces[i].tex >= 0 && w.faces[i].numverts >= 3)
			p += (w.faces[i].numverts - 1) / 2;
	return p;
}

/* --- tree and visibility ---------------------------------------------------------------- */

/* the axis a plane's normal is (exactly +x, +y or +z), so the eye's side is one
 * compare: n . eye - dist with n = 4096 on that axis; -1 for any other */
static int plane_type(int p)
{
	const plane_t *pl;
	if (p < 0 || p >= w.numplanes)
		return -1;
	pl = &w.planes[p];
	for (int c = 0; c < 3; c++)
		if (pl->n[c] == 4096 && !pl->n[(c + 1) % 3] && !pl->n[(c + 2) % 3])
			return c;
	return -1;
}

static void load_tree(const uint8_t *d, const uint32_t *ofs, const uint32_t *len)
{
	const uint8_t *p = lp[L_PLANES], *n = lp[L_NODES], *l = lp[L_LEAFS], *m = w.markbuf;
	w.numplanes = (int)(len[L_PLANES] / 20);
	w.planes = malloc(sizeof(plane_t) * (size_t)w.numplanes);
	for (int i = 0; i < w.numplanes; i++) {
		for (int c = 0; c < 3; c++)
			w.planes[i].n[c] = (int32_t)lrintf(rdf(p + i * 20 + c * 4) * 4096);
		w.planes[i].dist = (int32_t)lrintf(rdf(p + i * 20 + 12) * 32768);
	}
	w.numnodes = (int)(len[L_NODES] / 24);
	if (w.numnodes > 32767)
		w.numnodes = 0; /* children are 16-bit: no such map */
	w.nodes = malloc(sizeof(node_t) * (size_t)(w.numnodes ? w.numnodes : 1));
	for (int i = 0; i < w.numnodes; i++) {
		const uint8_t *q = n + i * 24;
		node_t *nd = &w.nodes[i];
		nd->plane = (int)rd32(q);
		nd->children[0] = rd16(q + 4);
		nd->children[1] = rd16(q + 6);
		for (int c = 0; c < 3; c++)
			nd->mins[c] = rd16(q + 8 + c * 2), nd->maxs[c] = rd16(q + 14 + c * 2);
		nd->parent = -1;
		nd->ptype = (int8_t)plane_type(nd->plane);
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
				w.nodes[c].parent = (int16_t)i;
			else if (c < 0 && -(c + 1) < w.numleafs)
				w.leafs[-(c + 1)].parent = i;
		}
	w.nummarks = (int)(len[L_MARKSURFACES] / 2);
	w.marks = (uint16_t *)(uintptr_t)m; /* its own allocation, so aligned */
	w.vis = w.visbuf;
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

static void mark_leaf(int leaf)
{
	leaf_t *lf = &w.leafs[leaf];
	if (lf->visframe == w.visframe)
		return;
	lf->visframe = w.visframe;
	for (int n = lf->parent; n >= 0 && w.nodes[n].visframe != w.visframe; n = w.nodes[n].parent)
		w.nodes[n].visframe = w.visframe;
}

static inline bool in_pvs(int leaf) { return leaf > 0 && leaf <= w.visleafs && (w.leafvis[(leaf - 1) >> 3] & (1 << ((leaf - 1) & 7))); }

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


static void build_portals(const uint8_t *d, const uint32_t *ofs, const uint32_t *len);
static void place_models(void);
static void resolve_marks(void);
static void load_hulls(const char *path);

/* --- loading ----------------------------------------------------------------------------- */

/* the map's lumps, read one at a time so a big map never sits in RAM whole:
 * each is freed once what it holds has been turned into the DS's own form */
static uint8_t *lbuf[NUM_LUMPS];

static bool get_lump(const char *path, int i, const uint32_t *ofs, const uint32_t *len)
{
	if (!lbuf[i])
		lbuf[i] = Fs_LoadRange(path, (long)ofs[i], (long)len[i], NULL);
	lp[i] = lbuf[i];
	return lbuf[i] != NULL;
}

static void drop_lump(int i)
{
	free(lbuf[i]);
	lbuf[i] = NULL, lp[i] = NULL;
}

static void drop_lumps(void)
{
	for (int i = 0; i < NUM_LUMPS; i++)
		drop_lump(i);
}

static bool world_load(const char *path, const uint16_t *palette, char *err, int errsize, bool uselit, bool *triedlit)
{
	uint32_t ofs[NUM_LUMPS], len[NUM_LUMPS];
	size_t hsize;
	uint8_t *head;
	World_Free();
	head = Fs_LoadRange(path, 0, 4 + NUM_LUMPS * 8, &hsize);
	if (!head) {
		snprintf(err, (size_t)errsize, "can't read %s", path);
		return false;
	}
	if (hsize < 4 + NUM_LUMPS * 8 || rd32(head) != BSPVERSION) {
		if (hsize >= 4 && rd32(head) == 30)
			snprintf(err, (size_t)errsize, "a Half-Life map: convert it with tools/nds/hlmap.py first");
		else
			snprintf(err, (size_t)errsize, "not a Quake map (BSP version %d)", hsize >= 4 ? (int)rd32(head) : -1);
		free(head);
		return false;
	}
	for (int i = 0; i < NUM_LUMPS; i++)
		ofs[i] = rd32(head + 4 + i * 8), len[i] = rd32(head + 8 + i * 8);
	free(head);

	/* kept for as long as the map is: the entities (text), the marks and the PVS */
	if (!get_lump(path, L_ENTITIES, ofs, len) || !get_lump(path, L_MARKSURFACES, ofs, len) ||
	    !get_lump(path, L_VISIBILITY, ofs, len)) {
		snprintf(err, (size_t)errsize, "can't read %s (cut short, or not enough memory)", path);
		drop_lumps();
		return false;
	}
	w.entbuf = lbuf[L_ENTITIES], w.markbuf = lbuf[L_MARKSURFACES], w.visbuf = lbuf[L_VISIBILITY];
	lbuf[L_ENTITIES] = lbuf[L_MARKSURFACES] = lbuf[L_VISIBILITY] = NULL; /* now w's to free */
	w.entities = (const char *)w.entbuf;
	if (len[L_ENTITIES])
		w.entbuf[len[L_ENTITIES] - 1] = 0;
	else
		w.entbuf[0] = 0;

	/* a Half-Life map made for the DS (tools/nds/hlmap.py) brings its own
	 * palette and coloured light, beside it as <map>.pal and <map>.lit */
	{
		static uint16_t mappal[256];
		char side[300];
		size_t n;
		uint8_t *b;
		bool ok;
		snprintf(side, sizeof(side), "%.*s.pal", (int)(strlen(path) > 4 ? strlen(path) - 4 : strlen(path)), path);
		if ((b = Fs_Load(side, &n)) && n >= 768) {
			for (int i = 0; i < 256; i++)
				mappal[i] = RGB15(b[i * 3] >> 3, b[i * 3 + 1] >> 3, b[i * 3 + 2] >> 3);
			palette = mappal;
		}
		free(b);
		if (get_lump(path, L_TEXTURES, ofs, len))
			load_textures(lp[L_TEXTURES], (int)len[L_TEXTURES], palette);
		drop_lump(L_TEXTURES);
		ok = get_lump(path, L_PLANES, ofs, len) && get_lump(path, L_NODES, ofs, len) && get_lump(path, L_LEAFS, ofs, len);
		if (ok)
			load_tree(NULL, ofs, len);
		drop_lump(L_NODES), drop_lump(L_LEAFS);
		memcpy(side + strlen(side) - 4, ".lit", 4);
		b = uselit ? Fs_Load(side, &n) : NULL;
		*triedlit = b != NULL;
		if (b && n >= 8 && !memcmp(b, "QLIT", 4) && rd32(b + 4) == 1)
			lit = b + 8, litsamples = (int)((n - 8) / 3);
		w.haslit = lit != NULL;
		ok = ok && get_lump(path, L_VERTEXES, ofs, len) && get_lump(path, L_EDGES, ofs, len) &&
		     get_lump(path, L_SURFEDGES, ofs, len) && get_lump(path, L_TEXINFO, ofs, len) &&
		     get_lump(path, L_FACES, ofs, len) && get_lump(path, L_MODELS, ofs, len) &&
		     (lit || get_lump(path, L_LIGHTING, ofs, len));
		if (ok)
			ok = load_faces(NULL, ofs, len);
		free(b);
		lit = NULL;
		drop_lump(L_VERTEXES), drop_lump(L_EDGES), drop_lump(L_SURFEDGES), drop_lump(L_TEXINFO);
		drop_lump(L_FACES), drop_lump(L_LIGHTING);
		if (!ok) {
			snprintf(err, (size_t)errsize, "bad faces, or out of memory");
			drop_lumps();
			World_Free();
			return false;
		}
	}
	w.nummodels = (int)(len[L_MODELS] / 64);
	w.models = calloc((size_t)(w.nummodels ? w.nummodels : 1), sizeof(submodel_t));
	for (int i = 0; i < w.nummodels; i++) {
		const uint8_t *q = lp[L_MODELS] + i * 64;
		for (int c = 0; c < 3; c++) {
			w.models[i].mins[c] = (int16_t)BW_NDS_CLAMP(lrintf(rdf(q + c * 4)), -32767, 32767);
			w.models[i].maxs[c] = (int16_t)BW_NDS_CLAMP(lrintf(rdf(q + 12 + c * 4)), -32767, 32767);
		}
		w.models[i].headnode0 = (int)rd32(q + 36);
		w.models[i].firstface = (int)rd32(q + 56);
		w.models[i].numfaces = (int)rd32(q + 60);
		if (i == 0)
			w.visleafs = BW_NDS_MIN((int)rd32(q + 52), w.numleafs - 1);
	}
	drop_lump(L_MODELS);
	resolve_marks();
	place_models();
	build_portals(NULL, ofs, len);
	drop_lumps();
	w.loaded = true;
	load_hulls(path);
	return true;
}

/* --- clip hulls for the link's movement prediction (hull_nds.c) ------------------------- */

#define HULL_RESERVE (256 * 1024) /* left free for what loads after the map, the studio models: they come
                                    * first, a map that only just fits goes without prediction */
static char hullerr[64];

const char *World_HullError(void) { return hullerr; }

static long hull_read(void *ctx, const char *path, long ofs, long len, void *dst)
{
	size_t n = 0;
	uint8_t *b = Fs_LoadRange(path, ofs, len, &n);
	if (!b)
		return -1;
	n = n < (size_t)len ? n : (size_t)len;
	memcpy(dst, b, n);
	free(b);
	return (long)n;
}

/* hull 0 is the drawing tree, in the renderer's fixed point (load_tree), placed at origin */
static void world_hull0(void *ctx, int model, const float origin[3], const float start[3], const float end[3],
                        hulltrace_t *tr)
{
	if (model < 0 || model >= w.nummodels || !w.numnodes || !w.numleafs) {
		memset(tr, 0, sizeof(*tr));
		tr->fraction = 1;
		memcpy(tr->endpos, end, sizeof(tr->endpos));
		return;
	}
	Hull_Trace0Fixed((const hull_fxnode_t *)(const void *)w.nodes, (int)sizeof(node_t),
	                 (const hull_fxplane_t *)(const void *)w.planes, (int)sizeof(plane_t), &w.leafs[0].contents,
	                 (int)sizeof(leaf_t), w.models[model].headnode0, origin, start, end, tr);
}

static const hull0_if world_hull0_if = {NULL, world_hull0};

/* a point trace through the world alone (hull 0), as far as it gets: the HUD's
 * nameplates and target ID look for a wall between the eye and a teammate */
float World_TraceFraction(const float start[3], const float end[3])
{
	static const float zero[3] = {0, 0, 0};
	hulltrace_t tr;
	world_hull0(NULL, 0, zero, start, end, &tr);
	return tr.startsolid ? 0.0f : tr.fraction;
}
_Static_assert(offsetof(node_t, plane) == offsetof(hull_fxnode_t, plane) &&
               offsetof(node_t, children) == offsetof(hull_fxnode_t, children) && offsetof(leaf_t, contents) == 0 &&
               sizeof(plane_t) == sizeof(hull_fxplane_t), "the tree as Hull_Trace0Fixed reads it");

/* after the map, so the two loads' peaks don't stack; without them (no .hul, or no room) the
 * map still loads and the link shows the server's view as before */
static void load_hulls(const char *path)
{
	hullerr[0] = 0;
	if (Hull_Load(path, 0, hull_read, NULL, HULL_RESERVE, hullerr, sizeof(hullerr)))
		Hull_SetHull0(&world_hull0_if);
}

/* a big Half-Life map may not fit with its coloured light (three bytes a
 * sample where the BSP has one), so then it loads again with the grey, and
 * after that without joining faces (which needs room for the joined ones) */
bool World_Load(const char *path, const uint16_t *palette, char *err, int errsize)
{
	bool triedlit = false, merge = World_MergeFaces, ok;
	if (world_load(path, palette, err, errsize, true, &triedlit))
		return true;
	if (triedlit && world_load(path, palette, err, errsize, false, &triedlit))
		return true;
	if (!merge)
		return false;
	World_MergeFaces = false;
	ok = world_load(path, palette, err, errsize, false, &triedlit);
	World_MergeFaces = merge;
	return ok;
}

/* brush entities with an "origin": where the map has them to begin with */
/* each leaf's marks as the tree walk wants them: a face merged away becomes
 * the face that took it in, faces that aren't drawn go, and each face once.
 * In place: a leaf's list only gets shorter (World_LightAt reads them too) */
static void resolve_marks(void)
{
	for (int l = 0; l < w.numleafs; l++) {
		leaf_t *lf = &w.leafs[l];
		int out = lf->firstmark, end = BW_NDS_MIN(lf->firstmark + lf->nummarks, w.nummarks);
		for (int m = lf->firstmark; m < end; m++) {
			int i = w.marks[m], k;
			if (i >= w.numfaces)
				continue;
			if (w.faces[i].alias >= 0)
				i = w.faces[i].alias;
			if (w.faces[i].tex < 0)
				continue;
			for (k = lf->firstmark; k < out && w.marks[k] != i; k++)
				;
			if (k == out)
				w.marks[out++] = (uint16_t)i;
		}
		lf->nummarks = BW_NDS_MAX(out - lf->firstmark, 0);
	}
}

static void place_models(void)
{
	const char *e = w.entities;
	while (e && (e = strchr(e, '{'))) {
		const char *end = strchr(e, '}'), *m = strstr(e, "\"model\""), *o = strstr(e, "\"origin\"");
		int sm;
		float org[3];
		if (!end)
			break;
		if (m && o && m < end && o < end && sscanf(m + 7, " \"*%d\"", &sm) == 1 && sm > 0 && sm < w.nummodels &&
		    sscanf(o + 8, " \"%f %f %f\"", &org[0], &org[1], &org[2]) == 3) {
			for (int c = 0; c < 3; c++)
				w.models[sm].home[c] = (int32_t)lrintf(org[c] * 8);
			World_ModelState(sm, true, NULL, NULL);
		}
		e = end + 1;
	}
}

/* places to look round from, for the bench: where players start, the
 * level-change landmarks, then the AI's path nodes (Half-Life maps have them
 * all over the walkable floor) taken evenly; never one inside a wall */
int World_Spots(ndscam_t *out, int max)
{
	static const struct {
		const char *classname;
		int up; /* from the entity's origin to the eye */
	} kinds[] = {{"info_player_start", 22}, {"info_player_coop", 22}, {"info_player_deathmatch", 22},
	             {"info_landmark", 16}, {"info_node", 48}};
	int n = 0;
	if (!w.loaded || max <= 0)
		return 0;
	for (int k = 0; k < 5 && n < max; k++) {
		int total = 0, seen = 0, room = max - n;
		char key[40];
		snprintf(key, sizeof(key), "\"%s\"", kinds[k].classname);
		for (const char *p = w.entities; (p = strstr(p, key)); p++)
			total++;
		for (const char *p = w.entities; n < max && (p = strstr(p, key)); p++, seen++) {
			const char *start = p, *end = strchr(p, '}'), *o, *a;
			float org[3], angle = 0;
			int32_t eye[3];
			int leaf;
			/* nodes: every so many, to spread the picks over the map */
			if (k == 4 && total > room && seen * room / total == (seen + 1) * room / total)
				continue;
			while (start > w.entities && *start != '{')
				start--;
			o = strstr(start, "\"origin\"");
			a = strstr(start, "\"angle\"");
			if (!o || (end && o > end) || sscanf(o + 8, " \"%f %f %f\"", &org[0], &org[1], &org[2]) != 3)
				continue;
			if (a && (!end || a < end))
				sscanf(a + 7, " \"%f\"", &angle);
			for (int c = 0; c < 3; c++)
				eye[c] = (int32_t)lrintf((org[c] + (c == 2 ? kinds[k].up : 0)) * 8);
			leaf = point_leaf(eye);
			if (leaf <= 0 || w.leafs[leaf].contents == -2)
				continue; /* outside the map, or in a wall */
			memcpy(out[n].org, eye, sizeof(eye));
			out[n].yaw = (int)(angle * 32768 / 360), out[n].pitch = 0;
			n++;
		}
	}
	return n;
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
	else if (World_Spots(cam, 1))
		return; /* a Half-Life map past the first: a landmark or a path node */
	for (int c = 0; c < 3; c++)
		cam->org[c] = (int32_t)lrintf(org[c] * 8);
	cam->yaw = (int)(angle * 32768 / 360);
	cam->pitch = 0;
}

/* --- drawing ----------------------------------------------------------------------------- */

typedef struct {
	int32_t n[3]; /* 4.12, pointing into the view */
} fplane_t;

/* in DTCM: the tree walk reads them for every node */
DTCM_BSS static fplane_t frustum[4];
DTCM_BSS static int32_t eye[3];
static int32_t vfwd[3], vright[3], vup[3]; /* the view's axes in map space (4.12) */

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

/* box_visible for the tree walk: only the planes in *clip (a bit each) are
 * tested, and those the box lies wholly inside are dropped from it, so the
 * boxes inside it (a node's children) skip them. The same answer as
 * box_visible, as a child's box lies inside its parent's */
static inline bool box_clip(const int16_t *mins, const int16_t *maxs, int *clip)
{
	int c = *clip;
	for (int i = 0; i < 4; i++) {
		const int32_t *n;
		int32_t px, py, pz, nx, ny, nz;
		if (!(c & (1 << i)))
			continue;
		n = frustum[i].n;
		if (n[0] >= 0)
			px = maxs[0], nx = mins[0];
		else
			px = mins[0], nx = maxs[0];
		if (n[1] >= 0)
			py = maxs[1], ny = mins[1];
		else
			py = mins[1], ny = maxs[1];
		if (n[2] >= 0)
			pz = maxs[2], nz = mins[2];
		else
			pz = mins[2], nz = maxs[2];
		if (n[0] * (px * 8 - eye[0]) + n[1] * (py * 8 - eye[1]) + n[2] * (pz * 8 - eye[2]) < 0)
			return false;
		if (n[0] * (nx * 8 - eye[0]) + n[1] * (ny * 8 - eye[1]) + n[2] * (nz * 8 - eye[2]) >= 0)
			c &= ~(1 << i);
	}
	*clip = c;
	return true;
}

/* what the tree walk reads for every node and face, in DTCM, so it isn't
 * read again through w after each call (World_Collect fills it) */
DTCM_BSS static struct {
	node_t *nodes;
	leaf_t *leafs;
	const plane_t *planes;
	face_t *faces;
	const uint16_t *marks;
	int *vislist;
	int numnodes, visframe, frame, numvis;
} wk;

/* the eye's side of a plane: n . eye - dist, one compare for an axial plane */
static inline int32_t plane_dist(const plane_t *pl, int ptype)
{
	if (ptype >= 0)
		return (eye[ptype] << 12) - pl->dist;
	return pl->n[0] * eye[0] + pl->n[1] * eye[1] + pl->n[2] * eye[2] - pl->dist;
}

/* a leaf's face (marks already resolved: drawn, not merged away) */
static inline void add_leafface(int i)
{
	face_t *f = &wk.faces[i];
	int32_t d;
	if (f->frame == wk.frame)
		return;
	f->frame = wk.frame;
	d = plane_dist(&wk.planes[f->plane], f->ptype);
	if (f->back ? d > 0 : d < 0)
		return; /* turned away */
	wk.vislist[wk.numvis++] = i;
}

/* a brush entity's face; returns whether it went in the list */
static inline bool add_face(int i)
{
	face_t *f = &w.faces[i];
	int32_t d;
	if (f->alias >= 0)
		i = f->alias, f = &w.faces[i];
	if (f->frame == w.frame || f->tex < 0)
		return false;
	f->frame = w.frame;
	d = plane_dist(&w.planes[f->plane], f->ptype);
	if (f->back ? d > 0 : d < 0)
		return false; /* turned away */
	w.vislist[w.numvis++] = i;
	return true;
}

/* clip: the frustum planes the node's parent wasn't wholly inside (box_clip) */
ITCM_CODE ARM_CODE static void walk(int n, int clip)
{
	while (n >= 0) {
		const node_t *nd = &wk.nodes[n];
		int near;
		if (nd->visframe != wk.visframe || (clip && !box_clip(nd->mins, nd->maxs, &clip)))
			return;
		/* the eye's side first, so faces come out nearest first */
		near = plane_dist(&wk.planes[nd->plane], nd->ptype) < 0;
		walk(nd->children[near], clip);
		n = nd->children[!near];
		if (n >= wk.numnodes)
			return;
	}
	{
		const leaf_t *lf = &wk.leafs[-(n + 1)];
		const uint16_t *mk, *end;
		if (-(n + 1) == 0 || lf->visframe != wk.visframe || (clip && !box_clip(lf->mins, lf->maxs, &clip)))
			return;
		/* resolve_marks kept every list inside the lump */
		for (mk = wk.marks + lf->firstmark, end = mk + lf->nummarks; mk < end; mk++)
			add_leafface(*mk);
	}
}

static void setup_frustum(const ndscam_t *cam)
{
	/* the view's axes in map space (4.12) */
	int sy = sinLerp(cam->yaw), cy = cosLerp(cam->yaw), sp = sinLerp(cam->pitch), cp = cosLerp(cam->pitch);
	int32_t fwd[3] = {cp * cy >> 12, cp * sy >> 12, sp};
	int32_t right[3] = {sy, -cy, 0};
	int32_t up[3] = {-sp * cy >> 12, -sp * sy >> 12, cp};
	memcpy(vfwd, fwd, sizeof(vfwd)), memcpy(vright, right, sizeof(vright)), memcpy(vup, up, sizeof(vup));
	/* half angles: 35 degrees up and down; 4:3 makes about 43 across.
	 * Each side plane leans in from the eye: n = fwd * sin(a) +- side * cos(a) */
	int sv = sinLerp(35 * 32768 / 360), cv = cosLerp(35 * 32768 / 360);
	int sh = sinLerp(44 * 32768 / 360), ch = cosLerp(44 * 32768 / 360);
	if (cam->roll & 0xffff) {
		/* a rolled view (a shake) turns the screen's rectangle about fwd: the planes take
		 * the box around it, tan h' = tan h cos r + tan v sin r, tan v' = tan h sin r + tan v cos r */
		float r = fabsf((float)(int16_t)cam->roll) * (2 * (float)M_PI / 32768.0f);
		float th = (float)sh / ch, tv = (float)sv / cv, cr = cosf(r), sr = sinf(r);
		float h2 = th * cr + tv * sr, v2 = th * sr + tv * cr, nh = sqrtf(1 + h2 * h2), nv = sqrtf(1 + v2 * v2);
		sh = (int)(4096 * h2 / nh), ch = (int)(4096 / nh), sv = (int)(4096 * v2 / nv), cv = (int)(4096 / nv);
	}
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

/* the tree's faces from..to into their textures' chains, in what's left of the budget */
ITCM_CODE ARM_CODE static void chain_range(int from, int to, int *verts, int *polys, int maxverts, int maxpolys, ndsstats_t *st)
{
	for (int k = from; k < to; k++)
		chain_face(w.vislist[k], verts, polys, maxverts, maxpolys, st);
}

ITCM_CODE ARM_CODE static void draw_chains(void)
{
	uint32_t colour = ~0u; /* the GPU keeps the last COLOR: send it only when it changes */
	for (int t = 0; t < w.numtexs; t++) {
		tex_t *tex = &w.texs[t];
		int i = tex->chain;
		if (i < 0)
			continue;
		glBindTexture(GL_TEXTURE_2D, tex->name);
		for (; i >= 0; i = w.faces[i].next) {
			const face_t *f = &w.faces[i];
			const uint32_t *v = (const uint32_t *)&w.verts[f->firstvert];
			int n = f->numverts;
			GFX_BEGIN = GL_QUAD_STRIP;
			for (int k = 0; k < n; k++, v += 3) {
				uint32_t st = v[0], xy = v[1], zc = v[2];
				if (zc >> 16 != colour)
					GFX_COLOR = colour = zc >> 16;
				GFX_TEX_COORD = st;
				GFX_VERTEX16 = xy;
				GFX_VERTEX16 = zc;
			}
			if (n & 1) {
				GFX_VERTEX16 = v[-2];
				GFX_VERTEX16 = v[-1];
			}
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
	glRotateZi(cam->roll);
	glRotateXi(-cam->pitch);
	glRotateYi(-cam->yaw);
	glMultMatrix4x4(&axes);
	glTranslatef32(-cam->org[0], -cam->org[1], -cam->org[2]);
}

/* --- portals ------------------------------------------------------------------------------
 *
 * Each frame, out from the eye's leaf through the portals (portals_nds.c),
 * keeping for every leaf reached the part of the screen it is seen through.
 * A portal is followed only where it shows inside that part, so leaves the
 * PVS lets through but walls hide from here are never reached. Screen
 * parts are boxes in the frustum's terms: +-4096 is the edge of the view. */

int World_Portals = WORLD_PORTALS_AUTO;

#define PT_NEAR 8      /* 1 map unit, in 1/8 units */
#define PT_KX 4241     /* 1 / tan(44 degrees), the frustum's sides, in 4.12 */
#define PT_KY 5850     /* 1 / tan(35 degrees), its top and bottom */
#define PT_EDGE 4096

/* r: in, the box the portal is seen through; out, the part of it the portal covers */
static bool portal_rect(const pt_portal_t *p, int16_t *r)
{
	int32_t v[32][3], c[40][3];
	int n = 0, m = 0, x0 = 32767, y0 = 32767, x1 = -32767, y1 = -32767;
	for (int k = 0; k < p->num && n < 32; k++, n++) {
		const int16_t *pt = w.portals.points[p->first + k];
		int32_t rel[3] = {pt[0] - eye[0], pt[1] - eye[1], pt[2] - eye[2]};
		v[n][0] = (rel[0] * vright[0] + rel[1] * vright[1] + rel[2] * vright[2]) >> 12;
		v[n][1] = (rel[0] * vup[0] + rel[1] * vup[1] + rel[2] * vup[2]) >> 12;
		v[n][2] = (rel[0] * vfwd[0] + rel[1] * vfwd[1] + rel[2] * vfwd[2]) >> 12;
	}
	/* the part in front of the eye */
	for (int k = 0; k < n && m < 38; k++) {
		const int32_t *a = v[k], *b = v[(k + 1) % n];
		if (a[2] >= PT_NEAR)
			memcpy(c[m++], a, sizeof(c[0]));
		if ((a[2] >= PT_NEAR) != (b[2] >= PT_NEAR)) {
			int32_t t = div32((PT_NEAR - a[2]) << 12, b[2] - a[2]); /* 4.12, 0..1 */
			c[m][0] = a[0] + (int32_t)(((int64_t)(b[0] - a[0]) * t) >> 12);
			c[m][1] = a[1] + (int32_t)(((int64_t)(b[1] - a[1]) * t) >> 12);
			c[m][2] = PT_NEAR;
			m++;
		}
	}
	if (m < 3)
		return false;
	for (int k = 0; k < m; k++) {
		/* the hardware divider; |c| < 2^17 and the factors < 2^13, so no overflow */
		int32_t sx = div32(c[k][0] * PT_KX, c[k][2]), sy = div32(c[k][1] * PT_KY, c[k][2]);
		x0 = sx < x0 ? sx : x0, x1 = sx > x1 ? sx : x1;
		y0 = sy < y0 ? sy : y0, y1 = sy > y1 ? sy : y1;
	}
	x0 = x0 > r[0] ? x0 : r[0], y0 = y0 > r[1] ? y0 : r[1];
	x1 = x1 < r[2] ? x1 : r[2], y1 = y1 < r[3] ? y1 : r[3];
	if (x0 >= x1 || y0 >= y1)
		return false;
	r[0] = (int16_t)x0, r[1] = (int16_t)y0, r[2] = (int16_t)x1, r[3] = (int16_t)y1;
	return true;
}

static void flood_portals(int start)
{
	int head = 0, tail = 0, cap = w.portals.numleafs;
	w.visframe++;
	w.flood++;
	w.portalleaves = 0;
	w.portaltests = 0;
	w.leafrect[start][0] = -PT_EDGE, w.leafrect[start][1] = -PT_EDGE;
	w.leafrect[start][2] = PT_EDGE, w.leafrect[start][3] = PT_EDGE;
	w.leafstamp[start] = w.flood;
	w.queue[tail++] = start, w.inqueue[start] = 1;
	while (head != tail) {
		int leaf = w.queue[head];
		head = head + 1 == cap ? 0 : head + 1;
		w.inqueue[leaf] = 0;
		for (int i = w.portals.leaffirst[leaf]; i < w.portals.leaffirst[leaf + 1]; i++) {
			const pt_portal_t *p = &w.portals.portals[w.portals.leafportals[i]];
			const plane_t *pl = &w.planes[p->plane];
			int other = p->leaf[0] == leaf ? p->leaf[1] : p->leaf[0];
			int32_t d = pl->n[0] * eye[0] + pl->n[1] * eye[1] + pl->n[2] * eye[2] - pl->dist;
			int16_t r[4];
			/* only looking out of this leaf through the portal, not back in */
			if (p->leaf[0] == leaf ? d < -32768 : d > 32768)
				continue;
			if (!in_pvs(other))
				continue;
			memcpy(r, w.leafrect[leaf], sizeof(r));
			w.portaltests++;
			if (!portal_rect(p, r))
				continue;
			if (w.leafstamp[other] == w.flood) {
				int16_t *o = w.leafrect[other];
				if (r[0] >= o[0] && r[1] >= o[1] && r[2] <= o[2] && r[3] <= o[3])
					continue; /* nothing new */
				o[0] = r[0] < o[0] ? r[0] : o[0], o[1] = r[1] < o[1] ? r[1] : o[1];
				o[2] = r[2] > o[2] ? r[2] : o[2], o[3] = r[3] > o[3] ? r[3] : o[3];
			} else {
				memcpy(w.leafrect[other], r, sizeof(r));
				w.leafstamp[other] = w.flood;
			}
			if (!w.inqueue[other]) {
				w.queue[tail] = other, w.inqueue[other] = 1;
				tail = tail + 1 == cap ? 0 : tail + 1;
			}
		}
	}
	w.pvsleaves = 0;
	for (int l = 1; l <= w.visleafs; l++) {
		w.pvsleaves += in_pvs(l);
		if (w.leafstamp[l] == w.flood)
			mark_leaf(l), w.portalleaves++;
		else if (w.portals.leaffirst[l] == w.portals.leaffirst[l + 1] && in_pvs(l))
			mark_leaf(l); /* no portals were found for it: the PVS decides */
	}
	mark_leaf(start);
}

static void build_portals(const uint8_t *d, const uint32_t *ofs, const uint32_t *len)
{
	int np = (int)(len[L_PLANES] / 20);
	pt_plane_t *pl = malloc(sizeof(pt_plane_t) * (size_t)np);
	pt_node_t *nd = malloc(sizeof(pt_node_t) * (size_t)(w.numnodes ? w.numnodes : 1));
	int *contents = malloc(sizeof(int) * (size_t)w.numleafs);
	float mins[3], maxs[3];
	int start = Nds_Vblanks();
	/* auto: only for maps compiled without a PVS, where they pay; with one,
	 * Quake's PVS plus the frustum test leave the portals little to remove
	 * for a lot of work (docs/platforms/nds.md) */
	bool want = World_Portals == WORLD_PORTALS_ON || (World_Portals == WORLD_PORTALS_AUTO && !w.vissize);
	if (want && pl && nd && contents && w.numnodes && w.nummodels) {
		for (int i = 0; i < np; i++) {
			for (int c = 0; c < 3; c++)
				pl[i].n[c] = rdf(lp[L_PLANES] + i * 20 + c * 4);
			pl[i].dist = rdf(lp[L_PLANES] + i * 20 + 12);
		}
		for (int i = 0; i < w.numnodes; i++) {
			nd[i].plane = w.nodes[i].plane;
			nd[i].children[0] = w.nodes[i].children[0], nd[i].children[1] = w.nodes[i].children[1];
		}
		for (int i = 0; i < w.numleafs; i++)
			contents[i] = w.leafs[i].contents;
		for (int c = 0; c < 3; c++)
			mins[c] = w.models[0].mins[c], maxs[c] = w.models[0].maxs[c];
		if (Pt_Build(&w.portals, pl, nd, w.numnodes, contents, w.numleafs, mins, maxs) && w.portals.overflow)
			Pt_Free(&w.portals);
	}
	free(pl), free(nd), free(contents);
	w.leafrect = malloc(sizeof(w.leafrect[0]) * (size_t)w.numleafs);
	w.leafstamp = calloc((size_t)w.numleafs, sizeof(int));
	w.queue = malloc(sizeof(int) * (size_t)w.numleafs);
	w.inqueue = calloc((size_t)w.numleafs, 1);
	if (!w.leafrect || !w.leafstamp || !w.queue || !w.inqueue)
		Pt_Free(&w.portals);
	w.loadms = (Nds_Vblanks() - start) * 1000 / 60;
}

int World_PortalCount(void) { return w.portals.numportals; }
int World_PortalLeaves(void) { return w.flooded ? w.portalleaves : -1; }
int World_PortalLoadMs(void) { return w.loadms; }
int World_PortalTests(void) { return w.portaltests; }
int World_PvsLeaves(void) { return w.pvsleaves; }

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
	setup_frustum(cam);
	w.viewyaw = cam->yaw, w.viewpitch = cam->pitch, w.viewroll = cam->roll;
	if (World_Portals != WORLD_PORTALS_OFF && w.portals.numportals && leaf > 0 && leaf <= w.visleafs &&
	    w.portals.leaffirst[leaf] != w.portals.leaffirst[leaf + 1]) {
		if (leaf != w.lastleaf)
			mark_visible(leaf); /* for its PVS */
		flood_portals(leaf);
		w.flooded = true;
	} else if (leaf != w.lastleaf || w.flooded) {
		mark_visible(leaf);
		w.flooded = false;
	}
	w.lastleaf = leaf;
	st->leaf = leaf;
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

/* --- brush entities a server moves ------------------------------------------------------ */

void World_ResetModels(void)
{
	for (int m = 1; m < w.nummodels; m++)
		World_ModelState(m, true, NULL, NULL);
}

void World_ModelState(int m, bool shown, const int32_t *org, const int *ang)
{
	submodel_t *sm;
	if (m <= 0 || m >= w.nummodels)
		return;
	sm = &w.models[m];
	if (!org)
		org = sm->home, ang = NULL;
	{
		bool moved = org[0] || org[1] || org[2] || (ang && (ang[0] || ang[1] || ang[2]));
		int a[3];
		for (int c = 0; c < 3; c++)
			a[c] = ang ? (int16_t)ang[c] : 0;
		if (sm->hidden == !shown && sm->moved == moved && !memcmp(sm->org, org, sizeof(sm->org)) &&
		    !memcmp(sm->ang, a, sizeof(a)))
			return; /* as it was: World_Collect may keep its list */
		w.modelgen++;
		sm->hidden = !shown;
		sm->moved = moved;
		memcpy(sm->org, org, sizeof(sm->org));
		memcpy(sm->ang, a, sizeof(sm->ang));
	}
}

int World_NumModels(void) { return w.nummodels; }

/* rotate v about axis (0 x, 1 y, 2 z) by a, the way glRotate does */
static void rotate(int32_t *v, int axis, int a)
{
	int i = (axis + 1) % 3, j = (axis + 2) % 3;
	int32_t s = sinLerp((int16_t)a), c = cosLerp((int16_t)a);
	int32_t x = v[i], y = v[j];
	v[i] = (int32_t)(((int64_t)c * x - (int64_t)s * y) >> 12);
	v[j] = (int32_t)(((int64_t)s * x + (int64_t)c * y) >> 12);
}

/* a point (1/8 units) from the map's terms into the moved model's */
static void to_model(const submodel_t *sm, const int32_t *p, int32_t *out)
{
	for (int c = 0; c < 3; c++)
		out[c] = p[c] - sm->org[c];
	if (sm->ang[0] || sm->ang[1] || sm->ang[2]) {
		rotate(out, 2, -sm->ang[1]);
		rotate(out, 1, -sm->ang[0]);
		rotate(out, 0, -sm->ang[2]);
	}
}

/* the moved model's box in the map (map units); turned ones get a box round every turn */
static void model_box(const submodel_t *sm, int16_t *mins, int16_t *maxs)
{
	int32_t r = 0;
	bool turned = sm->ang[0] || sm->ang[1] || sm->ang[2];
	for (int c = 0; c < 3; c++) {
		int32_t o = sm->org[c] / 8;
		if (turned)
			r += BW_NDS_MAX(-sm->mins[c], sm->maxs[c]);
		else {
			mins[c] = (int16_t)BW_NDS_CLAMP(sm->mins[c] + o, -32767, 32767);
			maxs[c] = (int16_t)BW_NDS_CLAMP(sm->maxs[c] + o, -32767, 32767);
		}
	}
	if (turned)
		for (int c = 0; c < 3; c++) {
			int32_t o = sm->org[c] / 8;
			mins[c] = (int16_t)BW_NDS_CLAMP(o - r, -32767, 32767);
			maxs[c] = (int16_t)BW_NDS_CLAMP(o + r, -32767, 32767);
		}
}

void World_Collect(void)
{
	if (!w.loaded) {
		w.numvis = 0;
		return;
	}
	/* standing still, looking the same way, with the same PVS: the tree's
	 * faces are what they were (the walk takes most of a frame on a big map) */
	if (w.kept.valid && w.kept.visframe == w.visframe && w.kept.yaw == w.viewyaw && w.kept.pitch == w.viewpitch && w.kept.roll == w.viewroll &&
	    !memcmp(w.kept.eye, eye, sizeof(eye))) {
		if (w.kept.modelgen == w.modelgen)
			return; /* and the brush entities' too */
		w.numvis = w.numtreevis;
	} else if (w.numnodes) {
		w.numvis = 0;
		wk.nodes = w.nodes, wk.leafs = w.leafs, wk.planes = w.planes, wk.faces = w.faces;
		wk.marks = w.marks, wk.vislist = w.vislist;
		wk.numnodes = w.numnodes, wk.visframe = w.visframe, wk.frame = w.frame, wk.numvis = w.numvis;
		walk(0, 15);
		w.numvis = wk.numvis;
	} else
		w.numvis = 0;
	w.numtreevis = w.numvis;
	w.kept.valid = true, w.kept.visframe = w.visframe, w.kept.modelgen = w.modelgen;
	w.kept.yaw = w.viewyaw, w.kept.pitch = w.viewpitch, w.kept.roll = w.viewroll;
	memcpy(w.kept.eye, eye, sizeof(eye));
	/* doors, lifts and other brush entities: all their faces, by box */
	for (int m = 1; m < w.nummodels; m++) {
		const submodel_t *sm = &w.models[m];
		int16_t mins[3], maxs[3];
		int32_t d = 0, saved[3];
		if (sm->hidden)
			continue;
		if (sm->moved)
			model_box(sm, mins, maxs);
		else
			memcpy(mins, sm->mins, sizeof(mins)), memcpy(maxs, sm->maxs, sizeof(maxs));
		if (!box_visible(mins, maxs))
			continue;
		for (int c = 0; c < 3; c++) {
			int32_t v = (mins[c] + maxs[c]) / 2 - eye[c] / 8;
			d += v * v;
		}
		/* faces turned away are dropped from where the eye is in the model's terms */
		memcpy(saved, eye, sizeof(saved));
		if (sm->moved)
			to_model(sm, saved, eye);
		for (int i = sm->firstface, end = BW_NDS_MIN(sm->firstface + sm->numfaces, w.numfaces); i < end; i++)
			if (add_face(i))
				w.visdist[w.numvis - 1] = d, w.vismodel[w.numvis - 1] = (int16_t)m;
		memcpy(eye, saved, sizeof(saved));
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
	PROF_T(pt);
	if (!w.loaded)
		return;
	chain_range(from, BW_NDS_MIN(to, w.numtreevis), &verts, &polys, maxverts, maxpolys, st);
	for (int k = w.numtreevis; k < w.numvis; k++)
		if ((w.visdist[k] < split2) == nearside && !w.models[w.vismodel[k]].moved)
			chain_face(w.vislist[k], &verts, &polys, maxverts, maxpolys, st);
	PROF_LAP(PROF_CHAIN, pt);
	draw_chains();
	/* the moved ones, each in its own place: a model's faces are together in the list */
	for (int k = w.numtreevis; k < w.numvis;) {
		int m = w.vismodel[k], end = k;
		const submodel_t *sm = &w.models[m];
		while (end < w.numvis && w.vismodel[end] == m)
			end++;
		if (sm->moved && (w.visdist[k] < split2) == nearside) {
			for (int i = k; i < end; i++)
				chain_face(w.vislist[i], &verts, &polys, maxverts, maxpolys, st);
			glPushMatrix();
			glTranslatef32(sm->org[0], sm->org[1], sm->org[2]);
			if (sm->ang[1])
				glRotateZi(sm->ang[1]);
			if (sm->ang[0])
				glRotateYi(sm->ang[0]);
			if (sm->ang[2])
				glRotateXi(sm->ang[2]);
			draw_chains();
			glPopMatrix(1);
		}
		k = end;
	}
	PROF_LAP(PROF_SUBMIT, pt);
	st->verts += verts, st->polys += polys;
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
			int32_t nz, z = 0;
			if (f->alias >= 0)
				f = &w.faces[f->alias];
			nz = w.planes[f->plane].n[2];
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
