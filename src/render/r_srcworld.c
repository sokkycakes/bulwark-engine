/* Bulwark engine - drawing Source BSP worlds: polygon faces and
 * displacements, RGBE lightmaps packed into atlas pages, and materials.
 * A material comes from a .mat file or image when the game has one;
 * otherwise a flat stand-in is made from the texture's reflectivity, with
 * a measuring grid for dev textures. Layouts: docs/spec/vbsp.md.
 * ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/fs.h"
#include "../common/vbsp.h"

#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>

#define LM_PAGE 1024

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

int VBSP_DispPoints(const uint8_t *info, const uint8_t *dverts, int numdverts, const float (*poly)[3], int npoly,
                    float (*out)[3], float (*flat)[3]);

/* --- lightmap atlas ------------------------------------------------------- */

static struct {
	uint8_t **pages;
	int num;
	int x, y, rowh;
} atlas;

static bool atlas_alloc(int w, int h, int *page, int *x, int *y)
{
	if (w > LM_PAGE || h > LM_PAGE)
		return false;
	if (!atlas.num || atlas.x + w > LM_PAGE) {
		atlas.x = 0;
		atlas.y += atlas.rowh;
		atlas.rowh = 0;
	}
	if (!atlas.num || atlas.y + h > LM_PAGE) {
		atlas.pages = Mem_Realloc(atlas.pages, sizeof(uint8_t *) * (atlas.num + 1));
		atlas.pages[atlas.num++] = Mem_Alloc(LM_PAGE * LM_PAGE * 3);
		atlas.x = atlas.y = atlas.rowh = 0;
	}
	*page = atlas.num - 1;
	*x = atlas.x;
	*y = atlas.y;
	atlas.x += w + 1;
	atlas.rowh = BW_MAX(atlas.rowh, h + 1);
	return true;
}

/* RGBE luxels are linear: c * 2^e / 255. Stored for the world shader, which
 * doubles lightmaps (r_lightmapscale), as half the gamma-encoded value. */
static uint8_t luxel_byte(float lin)
{
	float g = powf(BW_MAX(lin, 0.0f), 1.0f / 2.2f) * 0.5f;
	return (uint8_t)BW_MIN(g * 255.0f + 0.5f, 255.0f);
}

/* --- materials ---------------------------------------------------------------- */

static uint8_t srgb_byte(float lin) { return (uint8_t)BW_MIN(powf(BW_MAX(lin, 0.0f), 1.0f / 2.2f) * 255.0f + 0.5f, 255.0f); }

static bool name_has(const char *name, const char *part)
{
	size_t n = strlen(part);
	for (const char *s = name; *s; s++)
		if (!strncasecmp(s, part, n))
			return true;
	return false;
}

/* stands in for a material the game does not have: its average colour,
 * with Valve's dev textures given a measuring grid (lines every 16 and
 * 64 texels), grates and wire given holes and glass made see-through */
static material_t *standin_material(const char *name, const float *refl, int tw, int th)
{
	char mname[160];
	int w = BW_MIN(BW_MAX(tw, 16), 256), h = BW_MIN(BW_MAX(th, 16), 256);
	float sx = (float)tw / w, sy = (float)th / h;
	bool dev = name_has(name, "dev/") || name_has(name, "dev_"), grate = name_has(name, "grate") || name_has(name, "wire");
	bool glass = name_has(name, "glass");
	uint8_t base[3], *pix = Mem_Alloc((size_t)w * h * 4);
	image_t *img;
	material_t *m;
	snprintf(mname, sizeof(mname), "$source/%s", name);
	for (int k = 0; k < 3; k++)
		base[k] = srgb_byte(refl[k]);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint8_t *p = pix + ((size_t)y * w + x) * 4;
			int tx = (int)(x * sx), ty = (int)(y * sy);
			float shade = 1;
			if (dev) {
				if (tx % 64 == 0 || ty % 64 == 0)
					shade = 0.62f;
				else if (tx % 16 == 0 || ty % 16 == 0)
					shade = 0.85f;
			}
			for (int k = 0; k < 3; k++)
				p[k] = (uint8_t)(base[k] * shade);
			p[3] = 255;
			if (grate && (tx % 8) > 1 && (ty % 8) > 1)
				p[3] = 0;
			if (glass)
				p[3] = 90;
		}
	img = R_ImageFromPixels(mname, pix, w, h, 0);
	Mem_Free(pix);
	m = R_MaterialForImage(mname, img, 0);
	if (grate) {
		m->alphatest = true;
		m->twosided = true;
	} else if (glass) {
		m->blend = BLEND_ALPHA;
		m->src = GL_SRC_ALPHA;
		m->dst = GL_ONE_MINUS_SRC_ALPHA;
	}
	return m;
}

static material_t *source_material(const char *name, const float *refl, int tw, int th, int flags)
{
	char path[192];
	material_t *m;
	snprintf(path, sizeof(path), "materials/%s", name);
	for (char *c = path; *c; c++)
		*c = (char)tolower((unsigned char)*c);
	if (R_MaterialFileExists(path) || R_ImageExists(path))
		m = R_FindMaterial(path, 0);
	else
		m = standin_material(name, refl, tw, th);
	if (flags & (SRC_SURF_SKY | SRC_SURF_SKY2D))
		m->sky = true;
	if (flags & (SRC_SURF_NODRAW | SRC_SURF_HINT | SRC_SURF_SKIP | SRC_SURF_TRIGGER))
		m->nodraw = true;
	return m;
}

/* --- geometry ------------------------------------------------------------------- */

typedef struct {
	rvert_t *v;
	int num, cap;
	uint32_t *i;
	int numi, capi;
} geom_t;

static void push_vert(geom_t *g, const rvert_t *v)
{
	if (g->num == g->cap)
		g->v = Mem_Realloc(g->v, sizeof(rvert_t) * (g->cap = g->cap ? g->cap * 2 : 8192));
	g->v[g->num++] = *v;
}

static void push_index(geom_t *g, uint32_t i)
{
	if (g->numi == g->capi)
		g->i = Mem_Realloc(g->i, sizeof(uint32_t) * (g->capi = g->capi ? g->capi * 2 : 16384));
	g->i[g->numi++] = i;
}

static int surface_cmp(const void *a, const void *b)
{
	const rsurface_t *x = a, *y = b;
	if (x->material != y->material)
		return x->material < y->material ? -1 : 1;
	return x->lightmap - y->lightmap;
}

typedef struct {
	float tv[2][4], lv[2][4];
	int flags, texdata;
} texinfo_t;

bool R_LoadSourceWorld(bspmap_t *bsp, const char *path, const uint8_t *d, size_t size)
{
	vbsp_t v;
	size_t nverts, nedges, nsurf, nfaces, nti, ntd, nst, nsd, nlight, ninfo, ndv, nmodels;
	uint8_t *verts, *edges, *surf, *faces, *ti, *td, *st, *sd, *light, *info, *dv, *models;
	texinfo_t *tinfo = NULL;
	material_t **mats = NULL;
	int numtd, numti, numfaces;
	geom_t g = {0};
	uint8_t *styled = NULL;
	bool ok = false;

	memset(&atlas, 0, sizeof(atlas));
	if (!VBSP_Open(&v, d, size))
		return false;
	verts = VBSP_Lump(&v, VL_VERTEXES, &nverts);
	edges = VBSP_Lump(&v, VL_EDGES, &nedges);
	surf = VBSP_Lump(&v, VL_SURFEDGES, &nsurf);
	faces = VBSP_Lump(&v, VL_FACES, &nfaces);
	light = VBSP_Lump(&v, VL_LIGHTING, &nlight);
	if (!faces || !light) {
		/* compiled HDR only */
		Mem_Free(faces);
		Mem_Free(light);
		faces = VBSP_Lump(&v, VL_FACES_HDR, &nfaces);
		light = VBSP_Lump(&v, VL_LIGHTING_HDR, &nlight);
	}
	ti = VBSP_Lump(&v, VL_TEXINFO, &nti);
	td = VBSP_Lump(&v, VL_TEXDATA, &ntd);
	st = VBSP_Lump(&v, VL_TEXDATA_STRING_TABLE, &nst);
	sd = VBSP_Lump(&v, VL_TEXDATA_STRING_DATA, &nsd);
	info = VBSP_Lump(&v, VL_DISPINFO, &ninfo);
	dv = VBSP_Lump(&v, VL_DISP_VERTS, &ndv);
	models = VBSP_Lump(&v, VL_MODELS, &nmodels);
	if (!verts || !edges || !surf || !faces || !ti || !models)
		goto done;

	/* materials, one per texdata: reflectivity[3], name index, width, height */
	numtd = (int)(ntd / 32);
	mats = Mem_Alloc(sizeof(material_t *) * (numtd + 1));
	for (int i = 0; i < numtd; i++) {
		const uint8_t *p = td + (size_t)i * 32;
		float refl[3] = {rdf(p), rdf(p + 4), rdf(p + 8)};
		int si = rd32(p + 12), w = rd32(p + 16), h = rd32(p + 20);
		char name[128] = "$default";
		int flags = 0;
		if (si >= 0 && (size_t)si < nst / 4) {
			int so = rd32(st + (size_t)si * 4);
			if (so >= 0 && (size_t)so < nsd)
				Str_Copy(name, (const char *)sd + so, BW_MIN(sizeof(name), nsd - so + 1));
		}
		/* sky and tool flags live on texinfo; they are set below */
		mats[i] = source_material(name, refl, w, h, flags);
	}

	/* texinfo: texture vecs[2][4], lightmap vecs[2][4], flags, texdata */
	numti = (int)(nti / 72);
	tinfo = Mem_Alloc(sizeof(texinfo_t) * (numti + 1));
	for (int i = 0; i < numti; i++) {
		const uint8_t *p = ti + (size_t)i * 72;
		for (int k = 0; k < 8; k++) {
			tinfo[i].tv[k / 4][k % 4] = rdf(p + k * 4);
			tinfo[i].lv[k / 4][k % 4] = rdf(p + 32 + k * 4);
		}
		tinfo[i].flags = rd32(p + 64);
		tinfo[i].texdata = rd32(p + 68);
		if (tinfo[i].texdata >= 0 && tinfo[i].texdata < numtd) {
			material_t *m = mats[tinfo[i].texdata];
			if (tinfo[i].flags & (SRC_SURF_SKY | SRC_SURF_SKY2D))
				m->sky = true;
			if (tinfo[i].flags & (SRC_SURF_NODRAW | SRC_SURF_HINT | SRC_SURF_SKIP | SRC_SURF_TRIGGER))
				m->nodraw = true;
		}
	}

	numfaces = (int)(nfaces / 56);
	r_world.surfaces = Mem_Alloc(sizeof(rsurface_t) * (numfaces + 1));
	r_world.numsurfaces = numfaces;
	styled = Mem_Alloc(40 * 40 * 3);
	for (int f = 0; f < numfaces; f++) {
		/* plane ushort, side, on node, first edge, edge count short, texinfo
		 * short, dispinfo short, fog volume short, styles[4], light offset,
		 * area, lightmap mins in luxels int[2], lightmap size int[2], ... */
		const uint8_t *fp = faces + (size_t)f * 56;
		int tii = rds16(fp + 10), disp = rds16(fp + 12), lightofs = rd32(fp + 20);
		int lmins[2] = {rd32(fp + 28), rd32(fp + 32)}, lsize[2] = {rd32(fp + 36), rd32(fp + 40)};
		const uint8_t *styles = fp + 16;
		int firstedge = rd32(fp + 4), nedge = rds16(fp + 8);
		rsurface_t *rs = &r_world.surfaces[f];
		const texinfo_t *t;
		float poly[64][3], normal[3] = {0, 0, 0};
		int npoly = 0, page = -1, ax = 0, ay = 0, base = g.num, tw = 64, th = 64;
		rs->orig = f;
		rs->lightmap = -1;
		rs->type = 1;
		rs->material = R_FindMaterial("$default", 0);
		rs->firstindex = g.numi;
		if (tii < 0 || tii >= numti || nedge < 3 || nedge > 64 || firstedge < 0 || (size_t)firstedge + nedge > nsurf / 4)
			continue;
		t = &tinfo[tii];
		if (t->texdata >= 0 && t->texdata < numtd) {
			rs->material = mats[t->texdata];
			tw = BW_MAX(rd32(td + (size_t)t->texdata * 32 + 16), 1);
			th = BW_MAX(rd32(td + (size_t)t->texdata * 32 + 20), 1);
		}
		for (int e = 0; e < nedge; e++) {
			int se = rd32(surf + (size_t)(firstedge + e) * 4), ei = se < 0 ? -se : se, vi;
			if ((size_t)ei >= nedges / 4)
				ei = 0;
			vi = rdu16(edges + (size_t)ei * 4 + (se < 0 ? 2 : 0));
			if ((size_t)vi >= nverts / 12)
				vi = 0;
			for (int k = 0; k < 3; k++)
				poly[npoly][k] = rdf(verts + (size_t)vi * 12 + k * 4);
			npoly++;
		}
		{
			/* faces wind clockwise seen from the front */
			float e1[3], e2[3];
			for (int k = 2; k < npoly && VectorLength(normal) < 1e-3f; k++) {
				VectorSubtract(poly[k - 1], poly[0], e1);
				VectorSubtract(poly[k], poly[0], e2);
				CrossProduct(e2, e1, normal);
			}
			VectorNormalize(normal);
		}

		/* the lightmap: (size + 1) luxels a side for each style, four maps
		 * per style when bump mapped (the first is the flat one) */
		if (lightofs >= 0 && light && !(t->flags & (SRC_SURF_NOLIGHT | SRC_SURF_SKY | SRC_SURF_SKY2D)) &&
		    lsize[0] >= 0 && lsize[1] >= 0 && lsize[0] < 40 && lsize[1] < 40) {
			int w = lsize[0] + 1, h = lsize[1] + 1, n = w * h, maps = (t->flags & SRC_SURF_BUMPLIGHT) ? 4 : 1;
			memset(styled, 0, (size_t)n * 3);
			for (int s = 0; s < 4 && styles[s] != 255; s++) {
				size_t o = (size_t)lightofs + (size_t)s * maps * n * 4;
				if (o + (size_t)n * 4 > nlight)
					break;
				for (int i = 0; i < n; i++) {
					const uint8_t *px = light + o + (size_t)i * 4;
					float scale = ldexpf(1.0f / 255.0f, (int8_t)px[3]);
					for (int c = 0; c < 3; c++)
						styled[i * 3 + c] = (uint8_t)BW_MIN(styled[i * 3 + c] + luxel_byte(px[c] * scale), 255);
				}
			}
			if (atlas_alloc(w, h, &page, &ax, &ay)) {
				for (int y = 0; y < h; y++)
					memcpy(atlas.pages[page] + ((size_t)(ay + y) * LM_PAGE + ax) * 3, styled + (size_t)y * w * 3,
					       (size_t)w * 3);
				rs->lightmap = page;
			}
		}

#define SETUP_VERT(vtx, pos, flatpos)                                                                  \
	do {                                                                                               \
		float s_, t_;                                                                                  \
		memset(&(vtx), 0, sizeof(vtx));                                                                \
		VectorCopy((pos), (vtx).xyz);                                                                  \
		VectorCopy(normal, (vtx).normal);                                                              \
		memset((vtx).color, 255, 4);                                                                   \
		(vtx).st[0] = (DotProduct((pos), t->tv[0]) + t->tv[0][3]) / tw;                                \
		(vtx).st[1] = (DotProduct((pos), t->tv[1]) + t->tv[1][3]) / th;                                \
		s_ = DotProduct((flatpos), t->lv[0]) + t->lv[0][3] - lmins[0];                                 \
		t_ = DotProduct((flatpos), t->lv[1]) + t->lv[1][3] - lmins[1];                                 \
		if (page >= 0) {                                                                               \
			(vtx).lm[0] = (ax + BW_MAX(0.0f, BW_MIN(s_, (float)lsize[0])) + 0.5f) / LM_PAGE;           \
			(vtx).lm[1] = (ay + BW_MAX(0.0f, BW_MIN(t_, (float)lsize[1])) + 0.5f) / LM_PAGE;           \
		}                                                                                              \
	} while (0)

		if (disp >= 0 && info && dv && (size_t)disp < ninfo / 176) {
			float pts[17 * 17][3], flat[17 * 17][3];
			int n = VBSP_DispPoints(info + (size_t)disp * 176, dv, (int)(ndv / 20), (const float(*)[3])poly, npoly, pts, flat);
			for (int i = 0; i < n * n; i++) {
				rvert_t vx;
				SETUP_VERT(vx, pts[i], flat[i]);
				push_vert(&g, &vx);
			}
			for (int r = 0; r + 1 < n; r++)
				for (int c = 0; c + 1 < n; c++) {
					int a = base + r * n + c, b = a + 1, cc = a + n, dd = cc + 1;
					int tri[2][3];
					if ((r * n + c) % 2) {
						int t0[3] = {a, cc, b}, t1[3] = {b, cc, dd};
						memcpy(tri[0], t0, sizeof(t0));
						memcpy(tri[1], t1, sizeof(t1));
					} else {
						int t0[3] = {a, cc, dd}, t1[3] = {a, dd, b};
						memcpy(tri[0], t0, sizeof(t0));
						memcpy(tri[1], t1, sizeof(t1));
					}
					for (int k = 0; k < 2; k++) {
						/* counter-clockwise from the face's front, for GL */
						float e1[3], e2[3], tn[3];
						VectorSubtract(g.v[tri[k][1]].xyz, g.v[tri[k][0]].xyz, e1);
						VectorSubtract(g.v[tri[k][2]].xyz, g.v[tri[k][0]].xyz, e2);
						CrossProduct(e1, e2, tn);
						push_index(&g, (uint32_t)tri[k][0]);
						if (DotProduct(tn, normal) >= 0) {
							push_index(&g, (uint32_t)tri[k][1]);
							push_index(&g, (uint32_t)tri[k][2]);
						} else {
							push_index(&g, (uint32_t)tri[k][2]);
							push_index(&g, (uint32_t)tri[k][1]);
						}
					}
				}
			/* displacements are not in the leaf face lists */
			rs->novis = true;
		} else if (disp < 0) {
			for (int i = 0; i < npoly; i++) {
				rvert_t vx;
				SETUP_VERT(vx, poly[i], poly[i]);
				push_vert(&g, &vx);
			}
			for (int k = 2; k < npoly; k++) {
				push_index(&g, (uint32_t)base);
				push_index(&g, (uint32_t)(base + k));
				push_index(&g, (uint32_t)(base + k - 1));
			}
		}
#undef SETUP_VERT
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

	/* lightmap pages */
	r_world.lightmapsize = LM_PAGE;
	r_world.numlightmaps = atlas.num;
	if (atlas.num) {
		r_world.lightmaps = Mem_Alloc(sizeof(GLuint) * atlas.num);
		if (R_Active()) {
			qglGenTextures(atlas.num, r_world.lightmaps);
			qglPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			for (int i = 0; i < atlas.num; i++) {
				GL_BindTexture(1, GL_TEXTURE_2D, r_world.lightmaps[i]);
				qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, LM_PAGE, LM_PAGE, 0, GL_RGB, GL_UNSIGNED_BYTE, atlas.pages[i]);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}
		}
	}

	/* models: mins, maxs, origin, head node, first face, face count */
	r_world.numsubmodels = (int)(nmodels / 48);
	r_world.submodels = Mem_Alloc(sizeof(rmodel_t) * (r_world.numsubmodels + 1));
	for (int i = 0; i < r_world.numsubmodels; i++) {
		const uint8_t *mp = models + (size_t)i * 48;
		rmodel_t *rm = &r_world.submodels[i];
		snprintf(rm->name, sizeof(rm->name), "*%d", i);
		rm->type = MOD_BRUSH;
		for (int k = 0; k < 3; k++) {
			rm->mins[k] = rdf(mp + k * 4);
			rm->maxs[k] = rdf(mp + 12 + k * 4);
		}
		rm->firstsurface = rd32(mp + 40);
		rm->numsurfaces = rd32(mp + 44);
		if (rm->firstsurface < 0 || rm->numsurfaces < 0 || rm->firstsurface + rm->numsurfaces > numfaces)
			rm->numsurfaces = 0;
		rm->loaded = true;
		qsort(r_world.surfaces + rm->firstsurface, rm->numsurfaces, sizeof(rsurface_t), surface_cmp);
	}
	(void)bsp;
	R_WorldUpload(g.v, g.num, g.i, g.numi);
	Con_DPrintf("%s: Source BSP, %d faces, %d lightmap pages\n", path, numfaces, atlas.num);
	ok = true;

done:
	for (int i = 0; i < atlas.num; i++)
		Mem_Free(atlas.pages[i]);
	Mem_Free(atlas.pages);
	memset(&atlas, 0, sizeof(atlas));
	Mem_Free(g.v);
	Mem_Free(g.i);
	Mem_Free(styled);
	Mem_Free(tinfo);
	Mem_Free(mats);
	Mem_Free(verts);
	Mem_Free(edges);
	Mem_Free(surf);
	Mem_Free(faces);
	Mem_Free(light);
	Mem_Free(ti);
	Mem_Free(td);
	Mem_Free(st);
	Mem_Free(sd);
	Mem_Free(info);
	Mem_Free(dv);
	Mem_Free(models);
	return ok;
}
