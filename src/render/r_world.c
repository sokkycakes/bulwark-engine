/* Bulwark engine - drawing BSP worlds: Q3-family draw surfaces (planar,
 * triangle soup, Bezier patches), lightmap pages and the light grid, and
 * culling by the potentially visible sets. Q1 worlds load in r_q1world.c.
 * Record layouts are in docs/spec/bsp.md. ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/fs.h"
#include "../common/ftew.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

rworld_t r_world;

#define SURF_SKY 0x4
#define SURF_NODRAW 0x80

enum { ST_BAD, ST_PLANAR, ST_PATCH, ST_TRISOUP, ST_FLARE };

static int32_t rd32(const uint8_t *p) { return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }
static float rdf(const uint8_t *p)
{
	float f;
	int32_t i = rd32(p);
	memcpy(&f, &i, 4);
	return f;
}

typedef struct {
	rvert_t *v;
	int num, cap;
} vertbuf_t;

typedef struct {
	uint32_t *i;
	int num, cap;
} idxbuf_t;

static int add_vert(vertbuf_t *b, const rvert_t *v)
{
	if (b->num == b->cap) {
		b->cap = b->cap ? b->cap * 2 : 4096;
		b->v = Mem_Realloc(b->v, sizeof(rvert_t) * b->cap);
	}
	b->v[b->num] = *v;
	return b->num++;
}

static void add_index(idxbuf_t *b, uint32_t i)
{
	if (b->num == b->cap) {
		b->cap = b->cap ? b->cap * 2 : 8192;
		b->i = Mem_Realloc(b->i, sizeof(uint32_t) * b->cap);
	}
	b->i[b->num++] = i;
}

static void lerp_vert(const rvert_t *a, const rvert_t *b, float t, rvert_t *out)
{
	for (int i = 0; i < 3; i++) {
		out->xyz[i] = a->xyz[i] + (b->xyz[i] - a->xyz[i]) * t;
		out->normal[i] = a->normal[i] + (b->normal[i] - a->normal[i]) * t;
	}
	for (int i = 0; i < 2; i++) {
		out->st[i] = a->st[i] + (b->st[i] - a->st[i]) * t;
		out->lm[i] = a->lm[i] + (b->lm[i] - a->lm[i]) * t;
	}
	for (int i = 0; i < 4; i++)
		out->color[i] = (uint8_t)(a->color[i] + (b->color[i] - a->color[i]) * t);
}

/* point on a quadratic Bezier through three control vertices */
static void bezier(const rvert_t *c0, const rvert_t *c1, const rvert_t *c2, float t, rvert_t *out)
{
	rvert_t a, b;
	lerp_vert(c0, c1, t, &a);
	lerp_vert(c1, c2, t, &b);
	lerp_vert(&a, &b, t, out);
}

/* Patches are grids of control points, width x height (both odd); each 3x3
 * block is one biquadratic piece, sampled at a fixed level here */
static void tessellate_patch(const rvert_t *ctrl, int w, int h, vertbuf_t *vb, idxbuf_t *ib)
{
	const int level = 8;
	for (int py = 0; py + 2 < h; py += 2)
		for (int px = 0; px + 2 < w; px += 2) {
			int base = vb->num;
			for (int j = 0; j <= level; j++) {
				float tv = (float)j / level;
				rvert_t col[3];
				for (int k = 0; k < 3; k++)
					bezier(&ctrl[(py + 0) * w + px + k], &ctrl[(py + 1) * w + px + k], &ctrl[(py + 2) * w + px + k],
					       tv, &col[k]);
				for (int i = 0; i <= level; i++) {
					rvert_t v;
					float len;
					bezier(&col[0], &col[1], &col[2], (float)i / level, &v);
					len = sqrtf(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
					if (len > 0)
						for (int n = 0; n < 3; n++)
							v.normal[n] /= len;
					add_vert(vb, &v);
				}
			}
			for (int j = 0; j < level; j++)
				for (int i = 0; i < level; i++) {
					uint32_t a = base + j * (level + 1) + i, b = a + 1, c = a + level + 1, d = c + 1;
					add_index(ib, a);
					add_index(ib, c);
					add_index(ib, b);
					add_index(ib, b);
					add_index(ib, c);
					add_index(ib, d);
				}
		}
}

static const char *entity_key(const char *ents, const char *key, char *out, size_t size)
{
	/* the first occurrence is worldspawn's */
	char pattern[80];
	const char *p;
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	out[0] = 0;
	p = ents ? strstr(ents, pattern) : NULL;
	if (p) {
		const char *end = strchr(ents, '}');
		if (end && p > end)
			return out;
		p += strlen(pattern);
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '"') {
			size_t n = 0;
			p++;
			while (*p && *p != '"' && n + 1 < size)
				out[n++] = *p++;
			out[n] = 0;
		}
	}
	return out;
}

static int surface_cmp(const void *a, const void *b)
{
	const rsurface_t *x = a, *y = b;
	if (x->material != y->material)
		return x->material < y->material ? -1 : 1;
	return x->lightmap - y->lightmap;
}

static void load_lightgrid(const uint8_t *d, size_t size, const int32_t *lumps, bool rbsp, const float *wmins,
                           const float *wmaxs, const char *ents)
{
	char buf[64];
	float gs[3] = {64, 64, 128};
	int total, ofs = lumps[15 * 2], len = lumps[15 * 2 + 1];
	if (entity_key(ents, "gridsize", buf, sizeof(buf))[0])
		sscanf(buf, "%f %f %f", &gs[0], &gs[1], &gs[2]);
	for (int i = 0; i < 3; i++) {
		float lo = gs[i] * ceilf(wmins[i] / gs[i]);
		float hi = gs[i] * floorf(wmaxs[i] / gs[i]);
		r_world.gridorigin[i] = lo;
		r_world.gridsize[i] = gs[i];
		r_world.gridbounds[i] = (int)((hi - lo) / gs[i]) + 1;
	}
	total = r_world.gridbounds[0] * r_world.gridbounds[1] * r_world.gridbounds[2];
	if (total <= 0 || len <= 0 || (size_t)ofs + len > size)
		return;
	/* the grid is stored as 8-byte points (IBSP) or, in RBSP/FBSP, 30-byte
	 * points with an index array in lump 17; both become 8 bytes here */
	r_world.lightgrid = Mem_Alloc((size_t)total * 8);
	if (!rbsp) {
		if (len / 8 < total) {
			Mem_Free(r_world.lightgrid);
			r_world.lightgrid = NULL;
			return;
		}
		memcpy(r_world.lightgrid, d + ofs, (size_t)total * 8);
	} else {
		int aofs = lumps[17 * 2], alen = lumps[17 * 2 + 1], npoints = len / 30;
		if (alen / 2 != total || (size_t)aofs + alen > size) {
			Con_DPrintf("light grid: %d indices for %d points, not using it\n", alen / 2, total);
			Mem_Free(r_world.lightgrid);
			r_world.lightgrid = NULL;
			return;
		}
		for (int i = 0; i < total; i++) {
			int idx = d[aofs + i * 2] | (d[aofs + i * 2 + 1] << 8);
			const uint8_t *p;
			uint8_t *o = r_world.lightgrid + i * 8;
			if (idx >= npoints) {
				memset(o, 0, 8);
				continue;
			}
			p = d + ofs + idx * 30;
			/* style 0's ambient and directed colours, then the direction */
			memcpy(o, p, 3);
			memcpy(o + 3, p + 12, 3);
			o[6] = p[28];
			o[7] = p[29];
		}
	}
}

void R_WorldUpload(const rvert_t *v, int numverts, const uint32_t *idx, int numidx)
{
	R_WorldKeepGeometry(v, numverts, idx, numidx);
	if (!R_Active() || !numverts || !numidx)
		return;
	qglGenVertexArrays(1, &r_world.vao);
	qglGenBuffers(1, &r_world.vbo);
	qglGenBuffers(1, &r_world.ebo);
	qglBindVertexArray(r_world.vao);
	qglBindBuffer(GL_ARRAY_BUFFER, r_world.vbo);
	qglBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(rvert_t) * numverts), v, GL_STATIC_DRAW);
	qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r_world.ebo);
	qglBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(uint32_t) * numidx), idx, GL_STATIC_DRAW);
	R_SetupVertexAttribs();
	qglBindVertexArray(0);
}

/* worldspawn keys both formats share, and the surface map for culling */
static void finish_world(bspmap_t *bsp)
{
	char buf[128];
	if (bsp && entity_key(bsp->entities, "fog", buf, sizeof(buf))[0]) {
		/* "fog" "density r g b" */
		float f[4] = {0};
		if (sscanf(buf, "%f %f %f %f", &f[3], &f[0], &f[1], &f[2]) == 4) {
			memcpy(r_world.fog, f, sizeof(f));
			r_world.hasfog = f[3] > 0;
		}
	}
	memset(r_world.waterfog, 0, sizeof(r_world.waterfog));
	memset(r_world.skyroomfog, 0, sizeof(r_world.skyroomfog));
	r_world.waterfog[1] = r_world.waterfog[2] = r_world.waterfog[3] = 0.3f;
	r_world.waterfog[4] = 1;
	memcpy(r_world.skyroomfog, r_world.waterfog, sizeof(r_world.skyroomfog));
	/* "density r g b [alpha depthbias]", as FTE's and Nuclide's worldspawn keys */
	{
		static const char *const keys[][2] = {{"waterfog", "_waterfog"}, {"skyroomfog", "_skyroomfog"}};
		for (int k = 0; k < 2; k++) {
			float *dst = k ? r_world.skyroomfog : r_world.waterfog;
			if (bsp && (entity_key(bsp->entities, keys[k][0], buf, sizeof(buf))[0] ||
			            entity_key(bsp->entities, keys[k][1], buf, sizeof(buf))[0]))
				sscanf(buf, "%f %f %f %f %f %f", &dst[0], &dst[1], &dst[2], &dst[3], &dst[4], &dst[5]);
		}
	}
	if (bsp && (entity_key(bsp->entities, "skyname", buf, sizeof(buf))[0] ||
	            entity_key(bsp->entities, "sky", buf, sizeof(buf))[0]) &&
	    Str_ICmp(buf, r_world.skyname)) {
		Str_Copy(r_world.skyname, buf, sizeof(r_world.skyname));
		r_world.skybox = NULL;
	}
	r_world.bsp = bsp;
	R_ToolsMapLoaded();
	r_world.surfmap = Mem_Alloc(sizeof(int) * (r_world.numsurfaces + 1));
	for (int i = 0; i < r_world.numsurfaces; i++)
		r_world.surfmap[i] = -1;
	for (int i = 0; i < r_world.numsurfaces; i++) {
		int o = r_world.surfaces[i].orig;
		if (o >= 0 && o < r_world.numsurfaces)
			r_world.surfmap[o] = i;
	}
	r_world.loaded = true;
}

/* --- FTEW worlds ----------------------------------------------------- */

/* FTEW (docs/spec/ftew.md) has shared meshes placed by instances, with
 * vertex colours and no lightmaps or normals. Each model gets one surface per
 * material; triangles get flat normals. A box around the world carries the
 * sky, since FTEW has no sky faces. */

static void ftew_upload(vertbuf_t *vb, idxbuf_t *ib)
{
	if (!R_Active() || !vb->num || !ib->num)
		return;
	qglGenVertexArrays(1, &r_world.vao);
	qglGenBuffers(1, &r_world.vbo);
	qglGenBuffers(1, &r_world.ebo);
	qglBindVertexArray(r_world.vao);
	qglBindBuffer(GL_ARRAY_BUFFER, r_world.vbo);
	qglBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(rvert_t) * vb->num), vb->v, GL_STATIC_DRAW);
	qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r_world.ebo);
	qglBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(uint32_t) * ib->num), ib->i, GL_STATIC_DRAW);
	R_SetupVertexAttribs();
	qglBindVertexArray(0);
}

static void surface_bounds(rsurface_t *rs, const vertbuf_t *vb, const idxbuf_t *ib)
{
	for (int k = 0; k < 3; k++) {
		rs->mins[k] = 1e30f;
		rs->maxs[k] = -1e30f;
	}
	for (int k = rs->firstindex; k < rs->firstindex + rs->numindexes; k++)
		for (int a = 0; a < 3; a++) {
			float v = vb->v[ib->i[k]].xyz[a];
			rs->mins[a] = BW_MIN(rs->mins[a], v);
			rs->maxs[a] = BW_MAX(rs->maxs[a], v);
		}
}

static void ftew_triangle(vertbuf_t *vb, idxbuf_t *ib, const rvert_t *v0, const rvert_t *v1, const rvert_t *v2)
{
	rvert_t v[3] = {*v0, *v1, *v2};
	float e1[3], e2[3], n[3];
	for (int k = 0; k < 3; k++) {
		e1[k] = v[1].xyz[k] - v[0].xyz[k];
		e2[k] = v[2].xyz[k] - v[0].xyz[k];
	}
	/* counter-clockwise for GL, so the normal is e1 x e2 */
	n[0] = e1[1] * e2[2] - e1[2] * e2[1];
	n[1] = e1[2] * e2[0] - e1[0] * e2[2];
	n[2] = e1[0] * e2[1] - e1[1] * e2[0];
	VectorNormalize(n);
	for (int k = 0; k < 3; k++) {
		VectorCopy(n, v[k].normal);
		add_index(ib, (uint32_t)add_vert(vb, &v[k]));
	}
}

static void ftew_sky_box(vertbuf_t *vb, idxbuf_t *ib, const float *mins, const float *maxs)
{
	/* inward-facing; drawn without culling, so winding does not matter */
	static const int quads[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
	rvert_t c[8];
	int base;
	memset(c, 0, sizeof(c));
	for (int i = 0; i < 8; i++) {
		for (int k = 0; k < 3; k++) {
			float pad = (maxs[k] - mins[k]) * 0.5f + 1024;
			c[i].xyz[k] = (i >> (2 - k)) & 1 ? maxs[k] + pad : mins[k] - pad;
		}
		memset(c[i].color, 255, 4);
	}
	base = vb->num;
	for (int i = 0; i < 8; i++)
		add_vert(vb, &c[i]);
	for (int f = 0; f < 6; f++) {
		add_index(ib, base + quads[f][0]);
		add_index(ib, base + quads[f][1]);
		add_index(ib, base + quads[f][2]);
		add_index(ib, base + quads[f][0]);
		add_index(ib, base + quads[f][2]);
		add_index(ib, base + quads[f][3]);
	}
}

static bool load_ftew_world(bspmap_t *bsp, const char *path, const uint8_t *d, size_t size)
{
	ftew_t w;
	char err[128], buf[128];
	vertbuf_t vb = {0};
	idxbuf_t ib = {0};
	material_t **mats;
	int maxsurfs, ns = 0;

	if (!FTEW_Parse(d, size, &w, err, sizeof(err))) {
		Con_Printf("%s: %s\n", path, err);
		return false;
	}
	mats = Mem_Alloc(sizeof(material_t *) * (size_t)(w.nummaterials + 1));
	for (int i = 0; i < w.nummaterials; i++)
		mats[i] = R_FindMaterial(w.materials[i], 0);
	maxsurfs = (int)w.nummodels * (w.nummaterials + 1) + 1;
	r_world.surfaces = Mem_Alloc(sizeof(rsurface_t) * (size_t)maxsurfs);
	r_world.numsubmodels = (int)w.nummodels;
	r_world.submodels = Mem_Alloc(sizeof(rmodel_t) * (w.nummodels + 1));

	for (uint32_t mi = 0; mi < w.nummodels; mi++) {
		rmodel_t *rm = &r_world.submodels[mi];
		snprintf(rm->name, sizeof(rm->name), "*%u", mi);
		rm->type = MOD_BRUSH;
		rm->firstsurface = ns;
		for (int mat = 0; mat < w.nummaterials; mat++) {
			rsurface_t *rs = &r_world.surfaces[ns];
			rs->material = mats[mat];
			rs->lightmap = -1; /* vertex lit */
			rs->type = ST_TRISOUP;
			rs->firstindex = ib.num;
			for (int i = 0; i < w.numinstances; i++) {
				const ftew_instance_t *in = &w.instances[i];
				const ftew_mesh_t *mesh = &w.meshes[in->mesh];
				bool flip = FTEW_Mirrored(in);
				if (in->model != mi || mesh->material != (uint32_t)mat || !(in->flags & FTEW_INST_VISIBLE))
					continue;
				for (uint32_t t = 0; t + 2 < mesh->numindexes; t += 3) {
					rvert_t v[3];
					memset(v, 0, sizeof(v));
					for (int k = 0; k < 3; k++) {
						float rgba[4];
						FTEW_Vertex(mesh, FTEW_Index(mesh, t + k), v[k].xyz, v[k].st, rgba);
						FTEW_Transform(in, v[k].xyz, v[k].xyz);
						for (int c = 0; c < 4; c++) {
							float f = rgba[c] < 0 ? 0 : rgba[c] > 1 ? 1 : rgba[c];
							v[k].color[c] = (uint8_t)(f * 255 + 0.5f);
						}
					}
					/* clockwise front faces, as in Q3 maps: swap for GL unless mirrored */
					if (flip)
						ftew_triangle(&vb, &ib, &v[0], &v[1], &v[2]);
					else
						ftew_triangle(&vb, &ib, &v[0], &v[2], &v[1]);
				}
			}
			rs->numindexes = ib.num - rs->firstindex;
			if (rs->numindexes) {
				surface_bounds(rs, &vb, &ib);
				ns++;
			}
		}
		rm->numsurfaces = ns - rm->firstsurface;
		if (bsp && (int)mi < bsp->nummodels) {
			VectorCopy(bsp->models[mi].mins, rm->mins);
			VectorCopy(bsp->models[mi].maxs, rm->maxs);
		}
		rm->loaded = true;
	}

	if (r_world.numsubmodels > 0) {
		rmodel_t *world = &r_world.submodels[0];
		rsurface_t *rs = &r_world.surfaces[ns];
		float mins[3], maxs[3];
		for (int k = 0; k < 3; k++) {
			mins[k] = 1e30f;
			maxs[k] = -1e30f;
		}
		for (int i = world->firstsurface; i < world->firstsurface + world->numsurfaces; i++)
			for (int k = 0; k < 3; k++) {
				mins[k] = BW_MIN(mins[k], r_world.surfaces[i].mins[k]);
				maxs[k] = BW_MAX(maxs[k], r_world.surfaces[i].maxs[k]);
			}
		if (mins[0] <= maxs[0]) {
			/* the world's last surface; nothing after it belongs to model 0 */
			rs->material = R_FindMaterial("ftew/sky", 0);
			rs->material->sky = true;
			rs->lightmap = -1;
			rs->firstindex = ib.num;
			ftew_sky_box(&vb, &ib, mins, maxs);
			rs->numindexes = ib.num - rs->firstindex;
			surface_bounds(rs, &vb, &ib);
			if (r_world.numsubmodels == 1) {
				ns++;
				world->numsurfaces++;
			} else {
				/* keep model ranges contiguous: move the sky in after model 0 */
				rsurface_t sky = *rs;
				memmove(&r_world.surfaces[world->numsurfaces + 1], &r_world.surfaces[world->numsurfaces],
				        sizeof(rsurface_t) * (size_t)(ns - world->numsurfaces));
				r_world.surfaces[world->numsurfaces] = sky;
				ns++;
				world->numsurfaces++;
				for (int m = 1; m < r_world.numsubmodels; m++)
					r_world.submodels[m].firstsurface++;
			}
		}
	}
	r_world.numsurfaces = ns;
	for (int m = 0; m < r_world.numsubmodels; m++) {
		rmodel_t *rm = &r_world.submodels[m];
		qsort(r_world.surfaces + rm->firstsurface, rm->numsurfaces, sizeof(rsurface_t), surface_cmp);
	}

	if (bsp && entity_key(bsp->entities, "skyname", buf, sizeof(buf))[0] && Str_ICmp(buf, r_world.skyname)) {
		Str_Copy(r_world.skyname, buf, sizeof(r_world.skyname));
		r_world.skybox = NULL;
	}
	ftew_upload(&vb, &ib);
	Con_DPrintf("%s: FTEW world, %d surfaces, %d triangles\n", path, ns, ib.num / 3);
	Mem_Free(vb.v);
	Mem_Free(ib.i);
	Mem_Free(mats);
	FTEW_Free(&w);
	return true;
}

void R_LoadWorld(bspmap_t *bsp, const char *path)
{
	size_t size;
	uint8_t *d;
	int32_t lumps[18 * 2];
	bool rbsp, fbsp;
	int vertsize, surfsize, nshaders, nverts, nsurfs, nmodels, nindexes;
	material_t **mats;
	rvert_t *fileverts;
	vertbuf_t vb = {0};
	idxbuf_t ib = {0};
	int lmsize, npages;
	bool oddlm = false;

	R_FreeWorld();
	Str_Copy(r_world.name, path, sizeof(r_world.name));
	d = (uint8_t *)FS_LoadFile(path, &size);
	if (d && FTEW_IsFTEW(d, size)) {
		r_world.loaded = load_ftew_world(bsp, path, d, size);
		Mem_Free(d);
		return;
	}
	if (!d || size < 8 + 17 * 8)
		goto fail;
	if (rd32(d) == 29 || rd32(d) == 30 || !memcmp(d, "BSP2", 4)) {
		if (!R_LoadQ1World(bsp, path, d, size))
			goto fail;
		Mem_Free(d);
		finish_world(bsp);
		return;
	}
	if (!memcmp(d, "VBSP", 4)) {
		if (!R_LoadSourceWorld(bsp, path, d, size))
			goto fail;
		Mem_Free(d);
		finish_world(bsp);
		return;
	}
	rbsp = !memcmp(d, "RBSP", 4) || !memcmp(d, "FBSP", 4);
	fbsp = !memcmp(d, "FBSP", 4);
	if (memcmp(d, "IBSP", 4) && !rbsp) {
		Con_Printf("%s: not a map format that can be drawn\n", path);
		goto fail;
	}
	memset(lumps, 0, sizeof(lumps));
	for (int i = 0; i < (rbsp ? 18 : 17); i++) {
		lumps[i * 2] = rd32(d + 8 + i * 8);
		lumps[i * 2 + 1] = rd32(d + 12 + i * 8);
		if (lumps[i * 2] < 0 || lumps[i * 2 + 1] < 0 || (size_t)lumps[i * 2] + lumps[i * 2 + 1] > size)
			goto fail;
	}
	vertsize = rbsp ? 80 : 44;
	surfsize = rbsp ? 148 : 104;
	nshaders = lumps[1 * 2 + 1] / 72;
	nverts = lumps[10 * 2 + 1] / vertsize;
	nindexes = lumps[11 * 2 + 1] / 4;
	nsurfs = lumps[13 * 2 + 1] / surfsize;
	nmodels = lumps[7 * 2 + 1] / 40;

	mats = Mem_Alloc(sizeof(material_t *) * (nshaders + 1));
	for (int i = 0; i < nshaders; i++) {
		const uint8_t *s = d + lumps[1 * 2] + i * 72;
		char name[65];
		int flags = rd32(s + 64);
		memcpy(name, s, 64);
		name[64] = 0;
		mats[i] = R_FindMaterial(name, 0);
		if (flags & SURF_SKY)
			mats[i]->sky = true;
		if (flags & SURF_NODRAW)
			mats[i]->nodraw = true;
	}

	fileverts = Mem_Alloc(sizeof(rvert_t) * (nverts + 1));
	for (int i = 0; i < nverts; i++) {
		const uint8_t *v = d + lumps[10 * 2] + i * vertsize;
		rvert_t *o = &fileverts[i];
		for (int k = 0; k < 3; k++)
			o->xyz[k] = rdf(v + k * 4);
		o->st[0] = rdf(v + 12);
		o->st[1] = rdf(v + 16);
		o->lm[0] = rdf(v + 20);
		o->lm[1] = rdf(v + 24);
		if (rbsp) {
			/* lightmap coordinates for four styles, then the normal, then four colours */
			for (int k = 0; k < 3; k++)
				o->normal[k] = rdf(v + 52 + k * 4);
			memcpy(o->color, v + 64, 4);
		} else {
			for (int k = 0; k < 3; k++)
				o->normal[k] = rdf(v + 28 + k * 4);
			memcpy(o->color, v + 40, 4);
		}
	}

	r_world.surfaces = Mem_Alloc(sizeof(rsurface_t) * (nsurfs + 1));
	r_world.numsurfaces = nsurfs;
	for (int i = 0; i < nsurfs; i++) {
		const uint8_t *s = d + lumps[13 * 2] + i * surfsize;
		rsurface_t *rs = &r_world.surfaces[i];
		int shader = rd32(s), type = rd32(s + 8), firstvert = rd32(s + 12), numverts = rd32(s + 16);
		int firstindex = rd32(s + 20), numindexes = rd32(s + 24);
		int lm = rbsp ? rd32(s + 36) : rd32(s + 28);
		int pw = rd32(s + surfsize - 8), ph = rd32(s + surfsize - 4);
		rs->material = shader >= 0 && shader < nshaders ? mats[shader] : R_FindMaterial("$default", 0);
		rs->type = type;
		rs->orig = i;
		rs->lightmap = lm >= 0 ? lm : -1;
		if (lm >= 0 && (lm & 1))
			oddlm = true;
		rs->firstindex = ib.num;
		if (firstvert < 0 || numverts < 0 || firstvert + numverts > nverts)
			type = ST_BAD;
		if (type == ST_PLANAR || type == ST_TRISOUP) {
			int base = vb.num;
			if (firstindex < 0 || firstindex + numindexes > nindexes)
				numindexes = 0;
			for (int k = 0; k < numverts; k++)
				add_vert(&vb, &fileverts[firstvert + k]);
			/* Q3 winds clockwise seen from the front; GL culls by counter-clockwise */
			for (int k = 0; k + 2 < numindexes; k += 3) {
				const uint8_t *ip = d + lumps[11 * 2] + (size_t)(firstindex + k) * 4;
				int a = rd32(ip), b = rd32(ip + 4), c = rd32(ip + 8);
				if (a < 0 || b < 0 || c < 0 || a >= numverts || b >= numverts || c >= numverts)
					continue;
				add_index(&ib, base + a);
				add_index(&ib, base + c);
				add_index(&ib, base + b);
			}
		} else if (type == ST_PATCH && pw >= 3 && ph >= 3 && (pw & 1) && (ph & 1) && pw * ph <= numverts) {
			tessellate_patch(&fileverts[firstvert], pw, ph, &vb, &ib);
		}
		rs->numindexes = ib.num - rs->firstindex;
		for (int k = 0; k < 3; k++) {
			rs->mins[k] = 1e30f;
			rs->maxs[k] = -1e30f;
		}
		for (int k = rs->firstindex; k < ib.num; k++)
			for (int a = 0; a < 3; a++) {
				float v = vb.v[ib.i[k]].xyz[a];
				if (v < rs->mins[a])
					rs->mins[a] = v;
				if (v > rs->maxs[a])
					rs->maxs[a] = v;
			}
	}

	/* lightmap pages: 128x128 in IBSP/RBSP, 512x512 in FBSP. Surfaces name
	 * pages directly; in maps lit with deluxemaps the pages alternate light,
	 * direction, so surfaces only use even pages (docs/spec/bsp.md). Every
	 * page is uploaded, which keeps the indices as they are. */
	lmsize = fbsp ? 512 : 128;
	npages = lumps[14 * 2 + 1] / (lmsize * lmsize * 3);
	r_world.deluxe = npages >= 2 && (npages % 2) == 0 && !oddlm;
	r_world.lightmapsize = lmsize;
	r_world.numlightmaps = npages;
	if (r_world.numlightmaps > 0) {
		r_world.lightmaps = Mem_Alloc(sizeof(GLuint) * r_world.numlightmaps);
		if (R_Active()) {
			qglGenTextures(r_world.numlightmaps, r_world.lightmaps);
			qglPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			for (int i = 0; i < r_world.numlightmaps; i++) {
				const uint8_t *page = d + lumps[14 * 2] + (size_t)i * lmsize * lmsize * 3;
				GL_BindTexture(1, GL_TEXTURE_2D, r_world.lightmaps[i]);
				qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, lmsize, lmsize, 0, GL_RGB, GL_UNSIGNED_BYTE, page);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}
		}
	}
	for (int i = 0; i < nsurfs; i++)
		if (r_world.surfaces[i].lightmap >= r_world.numlightmaps)
			r_world.surfaces[i].lightmap = -1;

	/* submodels */
	r_world.numsubmodels = nmodels;
	r_world.submodels = Mem_Alloc(sizeof(rmodel_t) * (nmodels + 1));
	for (int i = 0; i < nmodels; i++) {
		const uint8_t *m = d + lumps[7 * 2] + i * 40;
		rmodel_t *rm = &r_world.submodels[i];
		snprintf(rm->name, sizeof(rm->name), "*%d", i);
		rm->type = MOD_BRUSH;
		for (int k = 0; k < 3; k++) {
			rm->mins[k] = rdf(m + k * 4);
			rm->maxs[k] = rdf(m + 12 + k * 4);
		}
		rm->firstsurface = rd32(m + 24);
		rm->numsurfaces = rd32(m + 28);
		if (rm->firstsurface < 0 || rm->firstsurface + rm->numsurfaces > nsurfs)
			rm->numsurfaces = 0;
		rm->loaded = true;
		/* fewer state changes: sort each model's surfaces by material */
		qsort(r_world.surfaces + rm->firstsurface, rm->numsurfaces, sizeof(rsurface_t), surface_cmp);
	}

	if (nmodels > 0)
		load_lightgrid(d, size, lumps, rbsp, r_world.submodels[0].mins, r_world.submodels[0].maxs, bsp ? bsp->entities : NULL);
	R_WorldUpload(vb.v, vb.num, ib.i, ib.num);
	Con_DPrintf("%s: %d surfaces, %d vertices, %d triangles, %d lightmaps%s\n", path, nsurfs, vb.num, ib.num / 3,
	            r_world.numlightmaps, r_world.deluxe ? " (with deluxemaps)" : "");
	Mem_Free(vb.v);
	Mem_Free(ib.i);
	Mem_Free(fileverts);
	Mem_Free(mats);
	Mem_Free(d);
	finish_world(bsp);
	return;

fail:
	Mem_Free(d);
	r_world.loaded = false;
}

void R_FreeWorld(void)
{
	R_FreeStaticProps();
	if (R_Active()) {
		if (r_world.vao)
			qglDeleteVertexArrays(1, &r_world.vao);
		if (r_world.vbo)
			qglDeleteBuffers(1, &r_world.vbo);
		if (r_world.ebo)
			qglDeleteBuffers(1, &r_world.ebo);
		if (r_world.numlightmaps && r_world.lightmaps)
			GL_DeleteTextures(r_world.numlightmaps, r_world.lightmaps);
	}
	Mem_Free(r_world.surfaces);
	Mem_Free(r_world.lightmaps);
	Mem_Free(r_world.submodels);
	Mem_Free(r_world.lightgrid);
	Mem_Free(r_world.surfmap);
	R_FreeQ1World();
	R_FreeWorldGeometry();
	{
		image_t *sky = r_world.skybox;
		char skyname[64];
		Str_Copy(skyname, r_world.skyname, sizeof(skyname));
		memset(&r_world, 0, sizeof(r_world));
		/* the sky command may come before the map: keep it */
		r_world.skybox = sky;
		Str_Copy(r_world.skyname, skyname, sizeof(r_world.skyname));
	}
}

/* --- lighting models ------------------------------------------------------- */

void R_LightPoint(const float *p, float *ambient, float *diffuse, float *dir)
{
	int c[3];
	const uint8_t *g;
	float lat, lng, scale = r_lightmapscale ? r_lightmapscale->value : 2, vox[3];
	/* inside a voxel world, its block and sky light (r_voxel.c); DESIGN:
	   CSO's one colour is split 7:3 between ambient and a light from above */
	if (R_VoxelLightPoint(p, vox)) {
		for (int i = 0; i < 3; i++) {
			ambient[i] = vox[i] * 0.7f;
			diffuse[i] = vox[i] * 0.3f;
		}
		dir[0] = 0.3f, dir[1] = 0.3f, dir[2] = 0.9f;
		return;
	}
	if (!r_world.lightgrid && R_Q1LightPoint(p, ambient, diffuse, dir))
		return;
	if (!r_world.lightgrid) {
		ambient[0] = ambient[1] = ambient[2] = 0.6f;
		diffuse[0] = diffuse[1] = diffuse[2] = 0.4f;
		dir[0] = 0.3f, dir[1] = 0.3f, dir[2] = 0.9f;
		return;
	}
	for (int i = 0; i < 3; i++) {
		c[i] = (int)floorf((p[i] - r_world.gridorigin[i]) / r_world.gridsize[i] + 0.5f);
		if (c[i] < 0)
			c[i] = 0;
		if (c[i] >= r_world.gridbounds[i])
			c[i] = r_world.gridbounds[i] - 1;
	}
	g = r_world.lightgrid + 8 * (c[0] + c[1] * r_world.gridbounds[0] + c[2] * r_world.gridbounds[0] * r_world.gridbounds[1]);
	for (int i = 0; i < 3; i++) {
		ambient[i] = g[i] / 255.0f * scale;
		diffuse[i] = g[3 + i] / 255.0f * scale;
	}
	/* direction as latitude/longitude bytes, 256 steps per turn */
	lat = g[7] * (float)M_PI * 2 / 256;
	lng = g[6] * (float)M_PI * 2 / 256;
	dir[0] = cosf(lat) * sinf(lng);
	dir[1] = sinf(lat) * sinf(lng);
	dir[2] = cosf(lng);
}

/* --- drawing ----------------------------------------------------------------- */

static bool cull_box(const float *planes, const float *mins, const float *maxs)
{
	for (int p = 0; p < 4; p++) {
		const float *pl = planes + p * 4;
		float x = pl[0] > 0 ? maxs[0] : mins[0];
		float y = pl[1] > 0 ? maxs[1] : mins[1];
		float z = pl[2] > 0 ? maxs[2] : mins[2];
		if (pl[0] * x + pl[1] * y + pl[2] * z + pl[3] < 0)
			return true;
	}
	return false;
}

/* left, right, bottom, top planes of a clip matrix (rows combined) */
static void frustum_planes(const float *m, float *planes)
{
	for (int i = 0; i < 4; i++) {
		int row = i / 2;
		float sign = (i & 1) ? -1.0f : 1.0f;
		for (int k = 0; k < 4; k++)
			planes[i * 4 + k] = m[k * 4 + 3] + sign * m[k * 4 + row];
	}
}

static void set_lights(const glprog_t *p, const float *model)
{
	float pos[8 * 4], col[8 * 3];
	int n = 0;
	(void)model;
	for (int i = 0; i < r_scene.numdlights && n < 8; i++) {
		const rdlight_t *l = &r_scene.dlights[i];
		pos[n * 4 + 0] = l->origin[0];
		pos[n * 4 + 1] = l->origin[1];
		pos[n * 4 + 2] = l->origin[2];
		pos[n * 4 + 3] = l->radius;
		col[n * 3 + 0] = l->color[0];
		col[n * 3 + 1] = l->color[1];
		col[n * 3 + 2] = l->color[2];
		n++;
	}
	qglUniform1i(p->u_numdlights, n);
	if (n) {
		qglUniform4fv(p->u_dlights, n, pos);
		qglUniform3fv(p->u_dlightcolors, n, col);
	}
}

/* marks the surfaces in leafs the view's cluster can see and the frustum
 * holds; false when there is nothing to cull with, so everything draws */
static bool mark_visible(const float *planes)
{
	bspmap_t *bsp = r_world.bsp;
	const uint8_t *row;
	int leaf;
	if (!bsp || bsp != cl.world || !bsp->vis || !r_world.surfmap || r_novis->ivalue)
		return false;
	leaf = BSP_PointLeaf(bsp, r_scene.origin);
	r_world.viewcluster = leaf >= 0 ? bsp->leafs[leaf].cluster : -1;
	/* outside the map (noclip): no cluster to see from */
	if (r_world.viewcluster < 0 || !(row = BSP_ClusterPVS(bsp, r_world.viewcluster)))
		return false;
	r_world.visframe++;
	for (int i = 0; i < bsp->numleafs; i++) {
		const cleaf_t *lf = &bsp->leafs[i];
		int c = lf->cluster;
		if (c < 0 || c >= bsp->numclusters || !((row[c >> 3] >> (c & 7)) & 1))
			continue;
		if (cull_box(planes, lf->mins, lf->maxs)) {
			r_stats.culled += lf->numfaces;
			continue;
		}
		for (int k = 0; k < lf->numfaces; k++) {
			int f = bsp->leaffaces[lf->firstface + k];
			if (f >= 0 && f < r_world.numsurfaces && r_world.surfmap[f] >= 0)
				r_world.surfaces[r_world.surfmap[f]].visframe = r_world.visframe;
		}
	}
	return true;
}

/* surfaces drawn with the same state go to GL in one call */
#define WORLD_BATCH 256
static GLsizei batchcount[WORLD_BATCH];
static const void *batchofs[WORLD_BATCH];
static int batchn;

static void flush_surfaces(void)
{
	if (batchn == 1 || (batchn && !qglMultiDrawElements_opt))
		for (int i = 0; i < batchn; i++)
			qglDrawElements(GL_TRIANGLES, batchcount[i], GL_UNSIGNED_INT, batchofs[i]);
	else if (batchn)
		qglMultiDrawElements_opt(GL_TRIANGLES, batchcount, GL_UNSIGNED_INT, batchofs, batchn);
	batchn = 0;
}

static void batch_surface(const rsurface_t *s)
{
	if (batchn == WORLD_BATCH)
		flush_surfaces();
	batchcount[batchn] = s->numindexes;
	batchofs[batchn++] = (const void *)(sizeof(uint32_t) * (size_t)s->firstindex);
}

void R_DrawWorldModel(rmodel_t *m, const float *mvp, const float *model, float alpha, bool translucentpass)
{
	const glprog_t *p = &r_progs[PROG_WORLD];
	material_t *cur = NULL;
	int curlm = -2;
	float planes[16];
	bool isworld = m == &r_world.submodels[0], usevis = false;
	if (!r_world.loaded || !r_world.vao || !m)
		return;
	frustum_planes(mvp, planes);
	if (isworld)
		usevis = mark_visible(planes);
	R_UseProgram(PROG_WORLD);
	qglUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, mvp);
	qglUniformMatrix4fv(p->u_model, 1, GL_FALSE, model);
	qglUniform3f(p->u_vieworg, r_scene.origin[0], r_scene.origin[1], r_scene.origin[2]);
	qglUniform4f(p->u_fog, r_viewfog[0], r_viewfog[1], r_viewfog[2], r_viewfog[3]);
	qglUniform1f(p->u_usefullbright, 0);
	set_lights(p, model);
	qglBindVertexArray(r_world.vao);
	GL_SetCull(true);
	for (int i = 0; i < m->numsurfaces; i++) {
		rsurface_t *s = &r_world.surfaces[m->firstsurface + i];
		material_t *mat = s->material;
		bool trans = mat->blend != BLEND_OPAQUE || alpha < 1 || mat->program == MP_WATER;
		if (!s->numindexes || mat->nodraw || mat->sky || trans != translucentpass)
			continue;
		if (usevis && !s->novis && s->visframe != r_world.visframe) {
			r_stats.culled++;
			continue;
		}
		if (isworld && cull_box(planes, s->mins, s->maxs)) {
			r_stats.culled++;
			continue;
		}
		r_stats.worldsurfs++;
		if (mat != cur || s->lightmap != curlm)
			flush_surfaces();
		if (mat != cur) {
			float a = alpha * (mat->program == MP_WATER && mat->blend == BLEND_OPAQUE ? 0.6f : 1.0f);
			cur = mat;
			GL_BindTexture(0, GL_TEXTURE_2D, mat->diffuse->tex);
			if (mat->fullbright) {
				GL_BindTexture(2, GL_TEXTURE_2D, mat->fullbright->tex);
				qglUniform1f(p->u_usefullbright, 1);
			} else {
				qglUniform1f(p->u_usefullbright, 0);
			}
			qglUniform4f(p->u_color, 1, 1, 1, a);
			qglUniform1f(p->u_alphatest, mat->alphatest ? 0.5f : 0.0f);
			if (trans)
				GL_SetBlend(mat->blend == BLEND_OPAQUE ? BLEND_ALPHA : mat->blend, mat->src, mat->dst);
			else
				GL_SetBlend(BLEND_OPAQUE, 0, 0);
			GL_SetDepth(true, !trans);
			GL_SetCull(!mat->twosided);
			curlm = -2;
		}
		if (s->lightmap != curlm) {
			curlm = s->lightmap;
			if (r_fullbright->ivalue || mat->program == MP_UNLIT || mat->program == MP_WATER) {
				GL_BindTexture(1, GL_TEXTURE_2D, r_whiteimage->tex);
				qglUniform1f(p->u_lmscale, 1);
			} else if (curlm >= 0) {
				GL_BindTexture(1, GL_TEXTURE_2D, r_world.lightmaps[curlm]);
				qglUniform1f(p->u_lmscale, r_lightmapscale->value);
			} else {
				/* vertex lit */
				qglUniform1f(p->u_lmscale, -1);
			}
		}
		batch_surface(s);
	}
	flush_surfaces();
	qglBindVertexArray(0);
	GL_SetCull(true);
}

/* the skybox named by worldspawn or the sky command, loaded on first use */
static void load_skybox(void)
{
	if (!r_world.skybox && r_world.skyname[0]) {
		/* worldspawn and the sky command name either a path or a set in
		 * env/, gfx/env/ where GoldSrc keeps them, or Source's folder */
		char env[96];
		snprintf(env, sizeof(env), "env/%s", r_world.skyname);
		r_world.skybox = R_LoadCubemap(r_world.skyname);
		if (!r_world.skybox)
			r_world.skybox = R_LoadCubemap(env);
		if (!r_world.skybox) {
			snprintf(env, sizeof(env), "gfx/env/%s", r_world.skyname);
			r_world.skybox = R_LoadCubemap(env);
		}
		if (!r_world.skybox) {
			/* Source keeps its sets as textures under materials/skybox/,
			 * faces named as GoldSrc's (sky_day01_01rt.vtf) */
			snprintf(env, sizeof(env), "materials/skybox/%s", r_world.skyname);
			r_world.skybox = R_LoadCubemap(env);
		}
	}
}

/* a world with no sky surfaces (a voxel world, as CSO Studio's): the
 * skybox drawn behind everything, a cube around the view with depth off,
 * before the blocks. CSO sets the sky with sv_skyname (hw.dll
 * FUN_0224dfd0); the sky command does it here. Nothing is drawn without a
 * sky name, as before. */
void R_DrawBackgroundSky(const float *mvp)
{
	const glprog_t *p = &r_progs[PROG_SKY];
	static const uint16_t idx[36] = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
	                                 1, 5, 6, 1, 6, 2, 2, 6, 7, 2, 7, 3, 3, 7, 4, 3, 4, 0};
	rvert_t v[8];
	const float r = 64;
	if (!r_world.skyname[0])
		return;
	load_skybox();
	if (!r_world.skybox)
		return;
	memset(v, 0, sizeof(v));
	for (int i = 0; i < 8; i++) {
		v[i].xyz[0] = r_scene.origin[0] + ((i == 1 || i == 2 || i == 5 || i == 6) ? r : -r);
		v[i].xyz[1] = r_scene.origin[1] + ((i == 2 || i == 3 || i == 6 || i == 7) ? r : -r);
		v[i].xyz[2] = r_scene.origin[2] + (i >= 4 ? r : -r);
	}
	R_UseProgram(PROG_SKY);
	qglUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, mvp);
	qglUniform3f(p->u_vieworg, r_scene.origin[0], r_scene.origin[1], r_scene.origin[2]);
	qglUniform4f(p->u_color, 1, 1, 1, 1);
	GL_BindTexture(3, GL_TEXTURE_CUBE_MAP, r_world.skybox->tex);
	GL_SetBlend(BLEND_OPAQUE, 0, 0);
	GL_SetDepth(false, false);
	GL_SetCull(false);
	R_StreamDraw(GL_TRIANGLES, v, 8, idx, 36);
	qglBindVertexArray(0);
	GL_SetDepth(true, true);
	GL_SetCull(true);
}

/* sky surfaces: drawn after the opaque world, looking up the skybox */
void R_DrawSky(const float *mvp)
{
	const glprog_t *p = &r_progs[PROG_SKY];
	rmodel_t *m;
	bool any = false;
	if (!r_world.loaded || !r_world.vao || !r_world.numsubmodels)
		return;
	load_skybox();
	m = &r_world.submodels[0];
	R_UseProgram(PROG_SKY);
	qglUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, mvp);
	qglUniform3f(p->u_vieworg, r_scene.origin[0], r_scene.origin[1], r_scene.origin[2]);
	qglBindVertexArray(r_world.vao);
	GL_SetBlend(BLEND_OPAQUE, 0, 0);
	GL_SetDepth(true, true);
	GL_SetCull(false);
	for (int i = 0; i < m->numsurfaces; i++) {
		rsurface_t *s = &r_world.surfaces[m->firstsurface + i];
		image_t *cube;
		if (!s->material->sky || !s->numindexes)
			continue;
		cube = s->material->skybox ? s->material->skybox : r_world.skybox;
		if (!any || cube) {
			if (cube) {
				GL_BindTexture(3, GL_TEXTURE_CUBE_MAP, cube->tex);
				qglUniform4f(p->u_color, 1, 1, 1, 1);
			} else {
				GL_BindTexture(3, GL_TEXTURE_CUBE_MAP, r_whitecube->tex);
				qglUniform4f(p->u_color, 0.3f, 0.4f, 0.55f, 1);
			}
			any = true;
		}
		qglDrawElements(GL_TRIANGLES, s->numindexes, GL_UNSIGNED_INT, (void *)(sizeof(uint32_t) * (size_t)s->firstindex));
	}
	qglBindVertexArray(0);
	GL_SetCull(true);
}
