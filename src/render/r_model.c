/* Bulwark engine - models: Quake .mdl (alias) and IQM/VVM meshes, plus the
 * framegroups files that name animations. Layouts are in
 * docs/spec/models.md. ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int32_t rd32(const uint8_t *p) { return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }
static float rdf(const uint8_t *p)
{
	float f;
	int32_t i = rd32(p);
	memcpy(&f, &i, 4);
	return f;
}

/* --- framegroups ------------------------------------------------------------ */

typedef struct {
	int first, count;
	float fps;
	bool loop;
	char name[32];
} framegroup_t;

/* "first count fps loop // name" per line */
static framegroup_t *load_framegroups(const char *model, int *count)
{
	char path[160];
	size_t len;
	char *text, *p;
	framegroup_t *g = NULL;
	int n = 0, cap = 0;
	snprintf(path, sizeof(path), "%s.framegroups", model);
	text = FS_LoadFile(path, &len);
	*count = 0;
	if (!text)
		return NULL;
	for (p = text; *p;) {
		char line[256];
		size_t l = 0;
		int first, cnt, loop = 1;
		float fps;
		while (*p && *p != '\n' && l < sizeof(line) - 1)
			line[l++] = *p++;
		line[l] = 0;
		if (*p)
			p++;
		if (sscanf(line, "%d %d %f %d", &first, &cnt, &fps, &loop) >= 3) {
			char *c = strstr(line, "//");
			if (n == cap) {
				cap = cap ? cap * 2 : 32;
				g = Mem_Realloc(g, sizeof(*g) * cap);
			}
			memset(&g[n], 0, sizeof(g[n]));
			g[n].first = first;
			g[n].count = cnt > 0 ? cnt : 1;
			g[n].fps = fps > 0 ? fps : 10;
			g[n].loop = loop != 0;
			if (c) {
				c += 2;
				while (*c == ' ')
					c++;
				Str_Copy(g[n].name, c, sizeof(g[n].name));
			}
			n++;
		}
	}
	Mem_Free(text);
	*count = n;
	return g;
}

/* --- alias models (.mdl) ----------------------------------------------------- */

typedef struct {
	char name[16];
	int firstpose, numposes;
	float interval;
} aliasframe_t;

typedef struct alias_s {
	int numverts, numtris, numposes, numframes, numskins;
	float *poses;        /* numposes * numverts * 3 positions */
	float *normals;      /* numposes * numverts * 3 */
	float *st;           /* numverts * 2, after seam duplication */
	int *remap;          /* output vertex -> source vertex */
	int numoutverts;
	uint16_t *indices;   /* numtris * 3 */
	aliasframe_t *frames;
	material_t **skins;
	framegroup_t *groups;
	int numgroups;
} alias_t;

static uint8_t palette[768];
static bool palette_loaded;

static void load_palette(void)
{
	size_t len;
	uint8_t *p;
	int w, h;
	if (palette_loaded)
		return;
	palette_loaded = true;
	p = (uint8_t *)FS_LoadFile("gfx/palette.lmp", &len);
	if (p && len >= 768) {
		memcpy(palette, p, 768);
		Mem_Free(p);
		return;
	}
	Mem_Free(p);
	/* Nuclide ships the palette as a 16x16 image instead */
	p = R_LoadPixels("gfx/palette.tga", &w, &h);
	if (p && w * h >= 256) {
		for (int i = 0; i < 256; i++)
			memcpy(palette + i * 3, p + i * 4, 3);
	} else {
		for (int i = 0; i < 256; i++)
			palette[i * 3] = palette[i * 3 + 1] = palette[i * 3 + 2] = (uint8_t)i;
	}
	Mem_Free(p);
}

static material_t *alias_skin(const char *model, int num, const uint8_t *pix, int w, int h)
{
	char name[192];
	uint8_t *rgba, *glow;
	bool anyglow = false;
	material_t *m;
	snprintf(name, sizeof(name), "%s_%d", model, num);
	/* a replacement image or material by that name wins */
	if (R_ImageExists(name) || FS_FileExists(va("%s.mat", name)))
		return R_FindMaterial(name, MATF_MODEL);
	load_palette();
	rgba = Mem_Alloc((size_t)w * h * 4);
	glow = Mem_Alloc((size_t)w * h * 4);
	for (int i = 0; i < w * h; i++) {
		uint8_t c = pix[i];
		memcpy(rgba + i * 4, palette + c * 3, 3);
		rgba[i * 4 + 3] = 255;
		/* the last 32 palette entries are fullbright in Quake's palette */
		if (c >= 224 && c != 255) {
			memcpy(glow + i * 4, palette + c * 3, 3);
			anyglow = true;
		} else {
			memset(glow + i * 4, 0, 3);
		}
		glow[i * 4 + 3] = 255;
	}
	m = R_MaterialForImage(name, R_ImageFromPixels(name, rgba, w, h, 0), MATF_MODEL);
	if (anyglow)
		m->fullbright = R_ImageFromPixels(va("%s_glow", name), glow, w, h, 0);
	Mem_Free(rgba);
	Mem_Free(glow);
	return m;
}

static void compute_normals(alias_t *a, const int *tris)
{
	for (int p = 0; p < a->numposes; p++) {
		float *pos = a->poses + (size_t)p * a->numverts * 3;
		float *nrm = a->normals + (size_t)p * a->numverts * 3;
		memset(nrm, 0, sizeof(float) * 3 * a->numverts);
		for (int t = 0; t < a->numtris; t++) {
			const int *v = tris + t * 3;
			float e1[3], e2[3], n[3];
			for (int k = 0; k < 3; k++) {
				e1[k] = pos[v[1] * 3 + k] - pos[v[0] * 3 + k];
				e2[k] = pos[v[2] * 3 + k] - pos[v[0] * 3 + k];
			}
			n[0] = e1[1] * e2[2] - e1[2] * e2[1];
			n[1] = e1[2] * e2[0] - e1[0] * e2[2];
			n[2] = e1[0] * e2[1] - e1[1] * e2[0];
			for (int j = 0; j < 3; j++)
				for (int k = 0; k < 3; k++)
					nrm[v[j] * 3 + k] += n[k];
		}
		for (int i = 0; i < a->numverts; i++) {
			float *n = nrm + i * 3, l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			if (l > 0)
				n[0] /= l, n[1] /= l, n[2] /= l;
		}
	}
}

static bool load_alias(rmodel_t *mod, const uint8_t *d, size_t size)
{
	alias_t *a;
	const uint8_t *p;
	float scale[3], origin[3];
	int skinw, skinh, numverts, numtris, numframes, numskins;
	int *onseam, *s, *t, *tris, *front;
	int posecap = 0;

#define NEED(n)                                                                                                \
	if ((size_t)(p - d) + (size_t)(n) > size)                                                                  \
		goto bad;
	if (size < 84 || rd32(d + 4) != 6)
		return false;
	for (int i = 0; i < 3; i++) {
		scale[i] = rdf(d + 8 + i * 4);
		origin[i] = rdf(d + 20 + i * 4);
	}
	numskins = rd32(d + 48);
	skinw = rd32(d + 52);
	skinh = rd32(d + 56);
	numverts = rd32(d + 60);
	numtris = rd32(d + 64);
	numframes = rd32(d + 68);
	if (numskins < 0 || skinw <= 0 || skinh <= 0 || skinw * skinh > 4096 * 4096 || numverts <= 0 ||
	    numverts > 65535 || numtris <= 0 || numframes <= 0)
		return false;

	a = Mem_Alloc(sizeof(*a));
	a->numverts = numverts;
	a->numtris = numtris;
	a->numskins = numskins;
	a->skins = Mem_Alloc(sizeof(material_t *) * (numskins + 1));
	p = d + 84;
	for (int i = 0; i < numskins; i++) {
		int group;
		NEED(4);
		group = rd32(p);
		p += 4;
		if (group == 0) {
			NEED((size_t)skinw * skinh);
			a->skins[i] = alias_skin(mod->name, i, p, skinw, skinh);
			p += (size_t)skinw * skinh;
		} else {
			/* animated skin group: keep the first picture */
			int nb;
			NEED(4);
			nb = rd32(p);
			p += 4;
			if (nb <= 0)
				goto bad;
			NEED((size_t)nb * 4 + (size_t)nb * skinw * skinh);
			p += nb * 4;
			a->skins[i] = alias_skin(mod->name, i, p, skinw, skinh);
			p += (size_t)nb * skinw * skinh;
		}
	}

	onseam = Mem_Alloc(sizeof(int) * numverts * 3);
	s = onseam + numverts;
	t = s + numverts;
	NEED((size_t)numverts * 12);
	for (int i = 0; i < numverts; i++) {
		onseam[i] = rd32(p);
		s[i] = rd32(p + 4);
		t[i] = rd32(p + 8);
		p += 12;
	}
	tris = Mem_Alloc(sizeof(int) * numtris * 4);
	front = tris + numtris * 3;
	NEED((size_t)numtris * 16);
	for (int i = 0; i < numtris; i++) {
		front[i] = rd32(p);
		for (int k = 0; k < 3; k++) {
			tris[i * 3 + k] = rd32(p + 4 + k * 4);
			if (tris[i * 3 + k] < 0 || tris[i * 3 + k] >= numverts)
				tris[i * 3 + k] = 0;
		}
		p += 16;
	}

	/* vertices on the seam use the back half of the skin when the triangle
	 * faces backwards, so those get a second output vertex */
	a->remap = Mem_Alloc(sizeof(int) * numverts * 2);
	a->st = Mem_Alloc(sizeof(float) * numverts * 4);
	a->indices = Mem_Alloc(sizeof(uint16_t) * numtris * 3);
	{
		int *backcopy = Mem_Alloc(sizeof(int) * numverts);
		for (int i = 0; i < numverts; i++) {
			a->remap[i] = i;
			a->st[i * 2] = (s[i] + 0.5f) / skinw;
			a->st[i * 2 + 1] = (t[i] + 0.5f) / skinh;
			backcopy[i] = -1;
		}
		a->numoutverts = numverts;
		for (int i = 0; i < numtris; i++)
			for (int k = 0; k < 3; k++) {
				int v = tris[i * 3 + k], out = v;
				if (!front[i] && onseam[v]) {
					if (backcopy[v] < 0) {
						backcopy[v] = a->numoutverts++;
						a->remap[backcopy[v]] = v;
						a->st[backcopy[v] * 2] = (s[v] + skinw / 2 + 0.5f) / skinw;
						a->st[backcopy[v] * 2 + 1] = (t[v] + 0.5f) / skinh;
					}
					out = backcopy[v];
				}
				/* Quake's triangles wind the other way round from GL's front faces */
				a->indices[i * 3 + (2 - k)] = (uint16_t)out;
			}
		Mem_Free(backcopy);
	}

	/* frames: single poses or groups of poses with their own intervals */
	a->frames = Mem_Alloc(sizeof(aliasframe_t) * numframes);
	a->numframes = numframes;
	for (int i = 0; i < numframes; i++) {
		int type, nb = 1;
		NEED(4);
		type = rd32(p);
		p += 4;
		if (type != 0) {
			NEED(12);
			nb = rd32(p);
			if (nb <= 0 || nb > 1024)
				goto bad;
			p += 12; /* count, bbox min and max */
			NEED((size_t)nb * 4);
			a->frames[i].interval = rdf(p);
			p += nb * 4;
		} else {
			a->frames[i].interval = 0.1f;
		}
		a->frames[i].firstpose = a->numposes;
		a->frames[i].numposes = nb;
		for (int k = 0; k < nb; k++) {
			float *pose;
			NEED(8 + 16 + (size_t)numverts * 4);
			if (k == 0)
				memcpy(a->frames[i].name, p + 8, 15);
			if (a->numposes == posecap) {
				posecap = posecap ? posecap * 2 : 64;
				a->poses = Mem_Realloc(a->poses, sizeof(float) * 3 * numverts * posecap);
			}
			pose = a->poses + (size_t)a->numposes * numverts * 3;
			p += 24;
			for (int v = 0; v < numverts; v++) {
				for (int c = 0; c < 3; c++)
					pose[v * 3 + c] = p[c] * scale[c] + origin[c];
				p += 4;
			}
			a->numposes++;
		}
	}
	a->normals = Mem_Alloc(sizeof(float) * 3 * numverts * a->numposes);
	compute_normals(a, tris);
	Mem_Free(onseam);
	Mem_Free(tris);

	/* bounds over every pose */
	for (int k = 0; k < 3; k++) {
		mod->mins[k] = 1e30f;
		mod->maxs[k] = -1e30f;
	}
	for (size_t i = 0; i < (size_t)a->numposes * numverts; i++)
		for (int k = 0; k < 3; k++) {
			float v = a->poses[i * 3 + k];
			if (v < mod->mins[k])
				mod->mins[k] = v;
			if (v > mod->maxs[k])
				mod->maxs[k] = v;
		}
	a->groups = load_framegroups(mod->name, &a->numgroups);
	mod->alias = a;
	mod->type = MOD_ALIAS;
	mod->numframes = a->numgroups ? a->numgroups : a->numframes;
	return true;
#undef NEED
bad:
	Con_Printf("%s: truncated or corrupt model\n", mod->name);
	return false;
}

/* --- IQM / VVM ------------------------------------------------------------ */

typedef struct {
	material_t *material;
	int firstindex, numindexes;
} iqmmesh_t;

typedef struct iqm_s {
	rvert_t *verts;
	int numverts;
	uint32_t *indices;
	int numindexes;
	iqmmesh_t *meshes;
	int nummeshes;
	GLuint vao, vbo, ebo;
	framegroup_t *anims;
	int numanims;
} iqm_t;

enum { IQM_POSITION = 0, IQM_TEXCOORD = 1, IQM_NORMAL = 2, IQM_COLOR = 6 };
enum { IQM_UBYTE = 1, IQM_FLOAT = 7 };

static bool load_iqm(rmodel_t *mod, const uint8_t *d, size_t size)
{
	iqm_t *q;
	uint32_t num_text, ofs_text, num_meshes, ofs_meshes, num_va, num_verts, ofs_va, num_tris, ofs_tris;
	uint32_t num_anims, ofs_anims;
	const char *text;
	if (size < 124 || memcmp(d, "INTERQUAKEMODEL", 16) || rd32(d + 16) != 2)
		return false;
	num_text = (uint32_t)rd32(d + 28);
	ofs_text = (uint32_t)rd32(d + 32);
	num_meshes = (uint32_t)rd32(d + 36);
	ofs_meshes = (uint32_t)rd32(d + 40);
	num_va = (uint32_t)rd32(d + 44);
	num_verts = (uint32_t)rd32(d + 48);
	ofs_va = (uint32_t)rd32(d + 52);
	num_tris = (uint32_t)rd32(d + 56);
	ofs_tris = (uint32_t)rd32(d + 60);
	num_anims = (uint32_t)rd32(d + 88);
	ofs_anims = (uint32_t)rd32(d + 92);
	if ((uint64_t)ofs_text + num_text > size || (uint64_t)ofs_meshes + num_meshes * 24ull > size ||
	    (uint64_t)ofs_va + num_va * 20ull > size || (uint64_t)ofs_tris + num_tris * 12ull > size ||
	    (uint64_t)ofs_anims + num_anims * 20ull > size || num_verts > (1u << 24))
		return false;
	text = (const char *)d + ofs_text;
	q = Mem_Alloc(sizeof(*q));
	q->numverts = (int)num_verts;
	q->verts = Mem_Alloc(sizeof(rvert_t) * (num_verts + 1));
	for (uint32_t i = 0; i < num_verts; i++)
		memset(q->verts[i].color, 255, 4);
	for (uint32_t a = 0; a < num_va; a++) {
		const uint8_t *va = d + ofs_va + a * 20;
		uint32_t type = (uint32_t)rd32(va), fmt = (uint32_t)rd32(va + 8), comps = (uint32_t)rd32(va + 12);
		uint32_t ofs = (uint32_t)rd32(va + 16);
		size_t elem = fmt == IQM_FLOAT ? 4 : 1;
		if ((uint64_t)ofs + (uint64_t)num_verts * comps * elem > size)
			continue;
		for (uint32_t i = 0; i < num_verts; i++) {
			const uint8_t *src = d + ofs + (size_t)i * comps * elem;
			rvert_t *v = &q->verts[i];
			if (type == IQM_POSITION && fmt == IQM_FLOAT && comps >= 3)
				for (int k = 0; k < 3; k++)
					v->xyz[k] = rdf(src + k * 4);
			else if (type == IQM_TEXCOORD && fmt == IQM_FLOAT && comps >= 2)
				v->st[0] = rdf(src), v->st[1] = rdf(src + 4);
			else if (type == IQM_NORMAL && fmt == IQM_FLOAT && comps >= 3)
				for (int k = 0; k < 3; k++)
					v->normal[k] = rdf(src + k * 4);
			else if (type == IQM_COLOR && fmt == IQM_UBYTE && comps >= 4)
				memcpy(v->color, src, 4);
		}
	}
	q->numindexes = (int)num_tris * 3;
	q->indices = Mem_Alloc(sizeof(uint32_t) * (q->numindexes + 1));
	for (uint32_t i = 0; i < num_tris * 3; i++) {
		uint32_t v = (uint32_t)rd32(d + ofs_tris + i * 4);
		q->indices[i] = v < num_verts ? v : 0;
	}
	/* IQM triangles wind the opposite way to GL's default front face */
	for (int i = 0; i + 2 < q->numindexes; i += 3) {
		uint32_t tmp = q->indices[i + 1];
		q->indices[i + 1] = q->indices[i + 2];
		q->indices[i + 2] = tmp;
	}
	q->nummeshes = (int)num_meshes;
	q->meshes = Mem_Alloc(sizeof(iqmmesh_t) * (num_meshes + 1));
	for (uint32_t m = 0; m < num_meshes; m++) {
		const uint8_t *me = d + ofs_meshes + m * 24;
		uint32_t mat = (uint32_t)rd32(me + 4), ft = (uint32_t)rd32(me + 16), nt = (uint32_t)rd32(me + 20);
		const char *matname = mat < num_text ? text + mat : "";
		char path[192];
		/* a bare material name is looked up next to the model */
		if (!strchr(matname, '/')) {
			const char *slash = strrchr(mod->name, '/');
			snprintf(path, sizeof(path), "%.*s%s", slash ? (int)(slash - mod->name + 1) : 0, mod->name, matname);
			matname = path;
		}
		q->meshes[m].material = R_FindMaterial(matname, MATF_MODEL);
		if (ft > num_tris)
			ft = num_tris;
		if (ft + nt > num_tris)
			nt = num_tris - ft;
		q->meshes[m].firstindex = (int)ft * 3;
		q->meshes[m].numindexes = (int)nt * 3;
	}
	/* animations, as framegroups for frameforname and friends */
	q->numanims = (int)num_anims;
	q->anims = Mem_Alloc(sizeof(framegroup_t) * (num_anims + 1));
	for (uint32_t i = 0; i < num_anims; i++) {
		const uint8_t *an = d + ofs_anims + i * 20;
		uint32_t name = (uint32_t)rd32(an);
		q->anims[i].first = rd32(an + 4);
		q->anims[i].count = rd32(an + 8);
		q->anims[i].fps = rdf(an + 12);
		q->anims[i].loop = (rd32(an + 16) & 1) != 0;
		Str_Copy(q->anims[i].name, name < num_text ? text + name : "", sizeof(q->anims[i].name));
	}
	for (int k = 0; k < 3; k++) {
		mod->mins[k] = 1e30f;
		mod->maxs[k] = -1e30f;
	}
	for (uint32_t i = 0; i < num_verts; i++)
		for (int k = 0; k < 3; k++) {
			if (q->verts[i].xyz[k] < mod->mins[k])
				mod->mins[k] = q->verts[i].xyz[k];
			if (q->verts[i].xyz[k] > mod->maxs[k])
				mod->maxs[k] = q->verts[i].xyz[k];
		}
	if (!num_verts)
		memset(mod->mins, 0, sizeof(mod->mins)), memset(mod->maxs, 0, sizeof(mod->maxs));
	if (R_Active() && num_verts) {
		qglGenVertexArrays(1, &q->vao);
		qglGenBuffers(1, &q->vbo);
		qglGenBuffers(1, &q->ebo);
		qglBindVertexArray(q->vao);
		qglBindBuffer(GL_ARRAY_BUFFER, q->vbo);
		qglBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(rvert_t) * num_verts), q->verts, GL_STATIC_DRAW);
		qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, q->ebo);
		qglBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(uint32_t) * q->numindexes), q->indices, GL_STATIC_DRAW);
		R_SetupVertexAttribs();
		qglBindVertexArray(0);
	}
	mod->iqm = q;
	mod->type = MOD_IQM;
	{
		int ng;
		framegroup_t *g = load_framegroups(mod->name, &ng);
		if (g) {
			Mem_Free(q->anims);
			q->anims = g;
			q->numanims = ng;
		}
	}
	mod->numframes = q->numanims;
	return true;
}

/* --- cache ------------------------------------------------------------------ */

#define MAX_RMODELS 2048
static rmodel_t *models[MAX_RMODELS];
static int nummodels;

static void free_model(rmodel_t *m)
{
	if (m->alias) {
		alias_t *a = m->alias;
		Mem_Free(a->poses);
		Mem_Free(a->normals);
		Mem_Free(a->st);
		Mem_Free(a->remap);
		Mem_Free(a->indices);
		Mem_Free(a->frames);
		Mem_Free(a->skins);
		Mem_Free(a->groups);
		Mem_Free(a);
	}
	if (m->iqm) {
		iqm_t *q = m->iqm;
		if (R_Active() && q->vao) {
			qglDeleteVertexArrays(1, &q->vao);
			qglDeleteBuffers(1, &q->vbo);
			qglDeleteBuffers(1, &q->ebo);
		}
		Mem_Free(q->verts);
		Mem_Free(q->indices);
		Mem_Free(q->meshes);
		Mem_Free(q->anims);
		Mem_Free(q);
	}
	Mem_Free(m);
}

void R_FreeModels(void)
{
	for (int i = 0; i < nummodels; i++)
		free_model(models[i]);
	nummodels = 0;
}

rmodel_t *R_LoadModel(const char *name)
{
	rmodel_t *m;
	uint8_t *d;
	size_t size;
	if (!name || !*name)
		return NULL;
	if (name[0] == '*') {
		int n = atoi(name + 1);
		return r_world.loaded && n >= 0 && n < r_world.numsubmodels ? &r_world.submodels[n] : NULL;
	}
	for (int i = 0; i < nummodels; i++)
		if (!Str_ICmp(models[i]->name, name))
			return models[i];
	if (nummodels >= MAX_RMODELS)
		return NULL;
	m = Mem_Alloc(sizeof(*m));
	Str_Copy(m->name, name, sizeof(m->name));
	models[nummodels++] = m;
	d = (uint8_t *)FS_LoadFile(name, &size);
	if (!d) {
		m->failed = true;
		Con_DPrintf("model %s not found\n", name);
		return m;
	}
	if (size >= 4 && !memcmp(d, "IDPO", 4))
		m->loaded = load_alias(m, d, size);
	else if (size >= 16 && !memcmp(d, "INTERQUAKEMODEL", 16))
		m->loaded = load_iqm(m, d, size);
	else
		Con_DPrintf("model %s: format not supported yet\n", name);
	m->failed = !m->loaded;
	{
		float r = 0;
		for (int k = 0; k < 3; k++)
			r += BW_MAX(fabsf(m->mins[k]), fabsf(m->maxs[k])) * BW_MAX(fabsf(m->mins[k]), fabsf(m->maxs[k]));
		m->radius = sqrtf(r);
	}
	Mem_Free(d);
	return m;
}

int R_ModelNumFrames(rmodel_t *m) { return m ? m->numframes : 0; }

/* --- frame queries for the client game -------------------------------------- */

static framegroup_t *model_groups(rmodel_t *m, int *count)
{
	*count = 0;
	if (!m)
		return NULL;
	if (m->alias && m->alias->groups) {
		*count = m->alias->numgroups;
		return m->alias->groups;
	}
	if (m->iqm) {
		*count = m->iqm->numanims;
		return m->iqm->anims;
	}
	return NULL;
}

int R_FrameForName(rmodel_t *m, const char *name)
{
	int n;
	framegroup_t *g = model_groups(m, &n);
	for (int i = 0; i < n; i++)
		if (!Str_ICmp(g[i].name, name))
			return i;
	if (m && m->alias && !g)
		for (int i = 0; i < m->alias->numframes; i++)
			if (!Str_ICmp(m->alias->frames[i].name, name))
				return i;
	return -1;
}

const char *R_FrameToName(rmodel_t *m, int frame)
{
	int n;
	framegroup_t *g = model_groups(m, &n);
	if (g && frame >= 0 && frame < n)
		return g[frame].name;
	if (m && m->alias && !g && frame >= 0 && frame < m->alias->numframes)
		return m->alias->frames[frame].name;
	return "";
}

float R_FrameDuration(rmodel_t *m, int frame)
{
	int n;
	framegroup_t *g = model_groups(m, &n);
	if (g && frame >= 0 && frame < n)
		return g[frame].count / g[frame].fps;
	if (m && m->alias && frame >= 0 && frame < m->alias->numframes)
		return m->alias->frames[frame].numposes * m->alias->frames[frame].interval;
	return 0;
}

/* --- drawing ---------------------------------------------------------------- */

/* the pose an animation frame shows at a time into it */
static int alias_pose(alias_t *a, int frame, float t)
{
	if (a->groups) {
		framegroup_t *g;
		int step;
		if (frame < 0 || frame >= a->numgroups)
			frame = 0;
		g = &a->groups[frame];
		step = (int)floorf(t * g->fps);
		step = g->loop ? ((step % g->count) + g->count) % g->count : BW_MAX(0, BW_MIN(step, g->count - 1));
		return BW_MIN(g->first + step, a->numposes - 1);
	}
	if (frame < 0 || frame >= a->numframes)
		frame = 0;
	{
		aliasframe_t *f = &a->frames[frame];
		int step = f->numposes > 1 && f->interval > 0 ? (int)floorf(t / f->interval) % f->numposes : 0;
		return f->firstpose + (step < 0 ? 0 : step);
	}
}

static void set_model_lighting(const glprog_t *p, rentity_t *e)
{
	float amb[3], dif[3], dir[3], pos[8 * 4], col[8 * 3];
	float c[3] = {e->origin[0], e->origin[1], e->origin[2]};
	int n = 0;
	if (e->model)
		for (int k = 0; k < 3; k++)
			c[k] += (e->model->mins[k] + e->model->maxs[k]) * 0.5f;
	if ((e->renderflags & 1) || r_fullbright->ivalue) { /* RF_VIEWMODEL lights from the view position */
		R_LightPoint(r_scene.origin, amb, dif, dir);
	} else {
		R_LightPoint(c, amb, dif, dir);
	}
	if (r_fullbright->ivalue) {
		amb[0] = amb[1] = amb[2] = 1;
		dif[0] = dif[1] = dif[2] = 0;
	}
	/* keep models from going black in unlit corners */
	for (int k = 0; k < 3; k++)
		amb[k] = BW_MAX(amb[k], 0.1f);
	qglUniform3f(p->u_light_ambient, amb[0] * e->colormod[0], amb[1] * e->colormod[1], amb[2] * e->colormod[2]);
	qglUniform3f(p->u_light_diffuse, dif[0] * e->colormod[0], dif[1] * e->colormod[1], dif[2] * e->colormod[2]);
	qglUniform3f(p->u_light_dir, dir[0], dir[1], dir[2]);
	for (int i = 0; i < r_scene.numdlights && n < 8; i++) {
		const rdlight_t *l = &r_scene.dlights[i];
		pos[n * 4 + 0] = l->origin[0];
		pos[n * 4 + 1] = l->origin[1];
		pos[n * 4 + 2] = l->origin[2];
		pos[n * 4 + 3] = l->radius;
		memcpy(col + n * 3, l->color, sizeof(float) * 3);
		n++;
	}
	qglUniform1i(p->u_numdlights, n);
	if (n) {
		qglUniform4fv(p->u_dlights, n, pos);
		qglUniform3fv(p->u_dlightcolors, n, col);
	}
}

static void bind_material(const glprog_t *p, material_t *mat, float alpha, bool translucent)
{
	GL_BindTexture(0, GL_TEXTURE_2D, mat->diffuse->tex);
	if (mat->fullbright) {
		GL_BindTexture(2, GL_TEXTURE_2D, mat->fullbright->tex);
		qglUniform1f(p->u_usefullbright, 1);
	} else {
		qglUniform1f(p->u_usefullbright, 0);
	}
	qglUniform1f(p->u_alphatest, mat->alphatest ? 0.5f : 0.0f);
	if (translucent)
		GL_SetBlend(mat->blend == BLEND_OPAQUE ? BLEND_ALPHA : mat->blend, mat->src, mat->dst);
	else
		GL_SetBlend(BLEND_OPAQUE, 0, 0);
	GL_SetDepth(true, !translucent);
	GL_SetCull(!mat->twosided);
	(void)alpha;
}

void R_DrawModelEntity(rentity_t *e, bool translucentpass)
{
	rmodel_t *m = e->model;
	float model[16], mvp[16];
	const glprog_t *p = &r_progs[PROG_MODEL];
	bool depthhack = (e->renderflags & 4) != 0; /* RF_DEPTHHACK */
	if (!m || !m->loaded)
		return;
	Mat4_FromAxis(model, e->axis, e->origin, e->scale);
	Mat4_Multiply(mvp, r_scene.viewproj, model);

	if (m->type == MOD_BRUSH) {
		bool trans = e->alpha < 1 || (e->renderflags & 8);
		if (trans == translucentpass || translucentpass)
			R_DrawWorldModel(m, mvp, model, e->alpha, translucentpass);
		return;
	}
	if ((e->alpha < 1 || (e->renderflags & 8)) != translucentpass)
		return;
	if (depthhack)
		qglDepthRange(0, 0.3);
	R_UseProgram(PROG_MODEL);
	qglUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, mvp);
	qglUniformMatrix4fv(p->u_model, 1, GL_FALSE, model);
	qglUniform4f(p->u_color, 1, 1, 1, e->alpha);
	qglUniform3f(p->u_vieworg, r_scene.origin[0], r_scene.origin[1], r_scene.origin[2]);
	qglUniform4f(p->u_fog, r_world.fog[0], r_world.fog[1], r_world.fog[2], r_world.hasfog ? r_world.fog[3] : 0);
	set_model_lighting(p, e);

	if (m->type == MOD_ALIAS) {
		alias_t *a = m->alias;
		static rvert_t *buf;
		static int bufsize;
		int p1 = alias_pose(a, e->frame, e->frame1time), p2 = alias_pose(a, e->frame2, e->frame2time);
		float lerp = e->lerpfrac;
		const float *pa = a->poses + (size_t)p1 * a->numverts * 3, *pb = a->poses + (size_t)p2 * a->numverts * 3;
		const float *na = a->normals + (size_t)p1 * a->numverts * 3, *nb = a->normals + (size_t)p2 * a->numverts * 3;
		material_t *skin;
		if (a->numoutverts > bufsize) {
			bufsize = a->numoutverts;
			buf = Mem_Realloc(buf, sizeof(rvert_t) * bufsize);
		}
		/* lerpfrac is how far towards frame2 the entity is */
		for (int i = 0; i < a->numoutverts; i++) {
			int v = a->remap[i];
			rvert_t *o = &buf[i];
			for (int k = 0; k < 3; k++) {
				o->xyz[k] = pa[v * 3 + k] + (pb[v * 3 + k] - pa[v * 3 + k]) * lerp;
				o->normal[k] = na[v * 3 + k] + (nb[v * 3 + k] - na[v * 3 + k]) * lerp;
			}
			o->st[0] = a->st[i * 2];
			o->st[1] = a->st[i * 2 + 1];
			memset(o->color, 255, 4);
		}
		skin = e->forceshader ? e->forceshader
		       : a->numskins ? a->skins[e->skin >= 0 && e->skin < a->numskins ? e->skin : 0]
		                     : R_FindMaterial("$default", MATF_MODEL);
		bind_material(p, skin, e->alpha, translucentpass);
		R_StreamDraw(GL_TRIANGLES, buf, a->numoutverts, a->indices, a->numtris * 3);
	} else if (m->type == MOD_IQM && m->iqm->vao) {
		iqm_t *q = m->iqm;
		qglBindVertexArray(q->vao);
		for (int i = 0; i < q->nummeshes; i++) {
			material_t *mat = e->forceshader ? e->forceshader : q->meshes[i].material;
			if (!q->meshes[i].numindexes || mat->nodraw)
				continue;
			bind_material(p, mat, e->alpha, translucentpass);
			qglDrawElements(GL_TRIANGLES, q->meshes[i].numindexes, GL_UNSIGNED_INT,
			                (void *)(sizeof(uint32_t) * (size_t)q->meshes[i].firstindex));
		}
		qglBindVertexArray(0);
	}
	if (depthhack)
		qglDepthRange(0, 1);
	GL_SetCull(true);
}
