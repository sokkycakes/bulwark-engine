/* Bulwark engine - models: Quake .mdl (alias) and IQM/VVM meshes, plus the
 * framegroups files that name animations. Layouts are in
 * docs/spec/models.md. ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/fs.h"
#include "../common/cmd.h"
#include "../common/skel.h"
#include "../common/studio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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
	a->groups = Skel_LoadFramegroups(mod->name, &a->numgroups);
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
	int skinref; /* a studio model's skin slot, for skin families */
	/* a studio model's body part and which of its sub-models this is (the
	 * geomset a custom skin selects), -1 for meshes always drawn */
	int geomgroup, geomvalue;
	char name[64]; /* the surface a custom skin's "replace" names */
} iqmmesh_t;

typedef struct iqm_s {
	rvert_t *verts;
	int numverts;
	uint32_t *indices;
	int numindexes;
	iqmmesh_t *meshes;
	int nummeshes;
	GLuint vao, vbo, ebo;
	framegroup_t *anims;  /* points into skel when there is one */
	int numanims;
	/* skinning: up to four joints per vertex */
	skelmodel_t *skel;
	uint8_t (*blendidx)[4];
	float (*blendw)[4];
	/* studio models: numfamilies x numskinref materials, picked by .skin */
	material_t **famskins;
	int numfamilies, numskinref;
} iqm_t;

enum { IQM_POSITION = 0, IQM_TEXCOORD = 1, IQM_NORMAL = 2, IQM_BLENDINDEXES = 4, IQM_BLENDWEIGHTS = 5, IQM_COLOR = 6 };
enum { IQM_UBYTE = 1, IQM_FLOAT = 7 };

static void finish_iqm(rmodel_t *mod, iqm_t *q);

static bool load_iqm(rmodel_t *mod, const uint8_t *d, size_t size)
{
	iqm_t *q;
	uint32_t num_text, ofs_text, num_meshes, ofs_meshes, num_va, num_verts, ofs_va, num_tris, ofs_tris;
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
	if ((uint64_t)ofs_text + num_text > size || (uint64_t)ofs_meshes + num_meshes * 24ull > size ||
	    (uint64_t)ofs_va + num_va * 20ull > size || (uint64_t)ofs_tris + num_tris * 12ull > size ||
	    num_verts > (1u << 24))
		return false;
	text = (const char *)d + ofs_text;
	q = Mem_Alloc(sizeof(*q));
	q->numverts = (int)num_verts;
	q->verts = Mem_Alloc(sizeof(rvert_t) * (num_verts + 1));
	for (uint32_t i = 0; i < num_verts; i++)
		memset(q->verts[i].color, 255, 4);
	q->skel = Skel_LoadModel(mod->name);
	if (q->skel) {
		q->blendidx = Mem_Alloc(sizeof(*q->blendidx) * (num_verts + 1));
		q->blendw = Mem_Alloc(sizeof(*q->blendw) * (num_verts + 1));
		for (uint32_t i = 0; i < num_verts; i++)
			q->blendw[i][0] = 1;
	}
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
			else if (type == IQM_BLENDINDEXES && q->skel && fmt == IQM_UBYTE && comps >= 4)
				for (int k = 0; k < 4; k++)
					q->blendidx[i][k] = src[k] < q->skel->numjoints ? src[k] : 0;
			else if (type == IQM_BLENDWEIGHTS && q->skel && (fmt == IQM_UBYTE || fmt == IQM_FLOAT) && comps >= 4)
				for (int k = 0; k < 4; k++)
					q->blendw[i][k] = fmt == IQM_UBYTE ? src[k] / 255.0f : rdf(src + k * 4);
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
		uint32_t mname = (uint32_t)rd32(me);
		const char *matname = mat < num_text ? text + mat : "";
		char path[192];
		q->meshes[m].geomgroup = q->meshes[m].geomvalue = -1;
		Str_Copy(q->meshes[m].name, mname < num_text ? text + mname : "", sizeof(q->meshes[m].name));
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
	finish_iqm(mod, q);
	return true;
}

/* bounds, the GL buffers and the animations of a mesh that is filled in */
static void finish_iqm(rmodel_t *mod, iqm_t *q)
{
	int num_verts = q->numverts;
	/* animations, as framegroups for frameforname and friends; a
	 * .framegroups file next to the model was already applied */
	if (q->skel) {
		q->anims = q->skel->anims;
		q->numanims = q->skel->numanims;
	}
	for (int k = 0; k < 3; k++) {
		mod->mins[k] = 1e30f;
		mod->maxs[k] = -1e30f;
	}
	for (int i = 0; i < num_verts; i++)
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
		qglBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(rvert_t) * num_verts), q->verts,
		              q->skel ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
		qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, q->ebo);
		qglBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(uint32_t) * q->numindexes), q->indices, GL_STATIC_DRAW);
		R_SetupVertexAttribs();
		qglBindVertexArray(0);
	}
	mod->iqm = q;
	mod->type = MOD_IQM;
	if (!q->skel) {
		/* a static mesh can still name poses of nothing, as some tools write */
		q->anims = Skel_LoadFramegroups(mod->name, &q->numanims);
	}
	mod->numframes = q->numanims;
}

/* --- GoldSrc studio models ---------------------------------------------------- */

/* "#WWWHHHname.bmp": a stand-in for the image models/texture/name.bmp
 * (with the '#WWWHHH' kept in the file name), which is WWW x HHH pixels.
 * Counter-Strike Online models carry a tiny placeholder under such names,
 * and their texture coordinates are in the external image's pixels */
static bool studio_extsize(const studio_texture_t *t, int *w, int *h)
{
	const char *n = t->name;
	if (n[0] != '#')
		return false;
	for (int i = 1; i <= 6; i++)
		if (n[i] < '0' || n[i] > '9')
			return false;
	*w = (n[1] - '0') * 100 + (n[2] - '0') * 10 + (n[3] - '0');
	*h = (n[4] - '0') * 100 + (n[5] - '0') * 10 + (n[6] - '0');
	return *w > 0 && *h > 0;
}

/* a studio texture: 8-bit pixels with their own palette. Flag 0x40
 * (masked) makes the last palette entry see-through, 0x20 is additive and
 * 0x04 fullbright (docs/spec/studio-mdl.md) */
static material_t *studio_skin(const char *model, const studio_texture_t *t)
{
	char name[256], tex[72];
	uint8_t *rgba;
	material_t *m;
	bool masked = (t->flags & 0x40) != 0;
	Str_Copy(tex, t->name, sizeof(tex));
	if (strrchr(tex, '.'))
		*strrchr(tex, '.') = 0;
	snprintf(name, sizeof(name), "%s_%s", model, tex);
	/* a replacement image or material by that name wins */
	if (R_ImageExists(name) || FS_FileExists(va("%s.mat", name)))
		return R_FindMaterial(name, MATF_MODEL);
	{
		int ew, eh;
		char ext[96];
		snprintf(ext, sizeof(ext), "models/texture/%s", tex);
		if (studio_extsize(t, &ew, &eh) && R_ImageExists(ext))
			return R_FindMaterial(ext, MATF_MODEL);
	}
	rgba = Mem_Alloc((size_t)t->width * t->height * 4);
	for (int i = 0; i < t->width * t->height; i++) {
		uint8_t c = t->pixels[i];
		memcpy(rgba + i * 4, t->palette + c * 3, 3);
		rgba[i * 4 + 3] = masked && c == 255 ? 0 : 255;
	}
	m = R_MaterialForImage(name, R_ImageFromPixels(name, rgba, t->width, t->height, 0), MATF_MODEL);
	Mem_Free(rgba);
	if (masked)
		m->alphatest = true;
	if (t->flags & 0x20)
		m->blend = BLEND_ADD, m->src = GL_ONE, m->dst = GL_ONE;
	if (t->flags & 0x04)
		m->program = MP_UNLIT;
	return m;
}

/* one vertex of a triangle command, placed in the rest pose */
static void studio_vert(const studio_submodel_t *sm, const bonemat_t *rest, const studio_texture_t *t,
                        const uint8_t *cmd, rvert_t *v, uint8_t *bone)
{
	int vi = (int16_t)(cmd[0] | cmd[1] << 8), ni = (int16_t)(cmd[2] | cmd[3] << 8);
	int s = (int16_t)(cmd[4] | cmd[5] << 8), tt = (int16_t)(cmd[6] | cmd[7] << 8);
	float in[3];
	int b = sm->vertbone[vi], nb = sm->normbone[ni];
	for (int k = 0; k < 3; k++)
		in[k] = rdf(sm->verts + vi * 12 + k * 4);
	Bone_Transform(rest[b], in, v->xyz);
	for (int k = 0; k < 3; k++)
		in[k] = rdf(sm->norms + ni * 12 + k * 4);
	for (int r = 0; r < 3; r++)
		v->normal[r] = rest[nb][r * 4] * in[0] + rest[nb][r * 4 + 1] * in[1] + rest[nb][r * 4 + 2] * in[2];
	/* texture coordinates are in pixels */
	int tw = t ? t->width : 1, th = t ? t->height : 1;
	if (t)
		studio_extsize(t, &tw, &th);
	v->st[0] = t ? (float)s / tw : 0;
	v->st[1] = t ? (float)tt / th : 0;
	memset(v->color, 255, 4);
	*bone = (uint8_t)b;
}

/* every sub-model of every body part, in the rest pose and skinned to the
 * bones, with the sequences as animations (Skel_LoadModel). Each mesh knows
 * its body part and sub-model: the first sub-model draws unless a custom
 * skin's geomset picks another. */
static bool load_studio(rmodel_t *mod)
{
	const studio_model_t *st = Studio_Cached(mod->name);
	iqm_t *q;
	bonemat_t *rest;
	float *trs;
	int maxverts = 0, nv = 0, ni = 0, nm = 0, nskins;
	if (!st)
		return false;
	for (int b = 0; b < st->numbodyparts; b++)
		for (int s = 0; s < st->bodyparts[b].nummodels; s++) {
			const studio_submodel_t *sm = &st->bodyparts[b].models[s];
			for (int k = 0; k < sm->nummeshes; k++) {
				maxverts += Studio_CountTris(st, &sm->meshes[k], sm->numverts, sm->numnorms) * 3;
				nm++;
			}
		}
	q = Mem_Alloc(sizeof(*q));
	q->skel = Skel_LoadModel(mod->name);
	q->verts = Mem_Alloc(sizeof(rvert_t) * (maxverts + 1));
	q->indices = Mem_Alloc(sizeof(uint32_t) * (maxverts + 1));
	q->meshes = Mem_Alloc(sizeof(iqmmesh_t) * (nm + 1));
	if (q->skel) {
		q->blendidx = Mem_Alloc(sizeof(*q->blendidx) * (maxverts + 1));
		q->blendw = Mem_Alloc(sizeof(*q->blendw) * (maxverts + 1));
	}
	/* the rest pose, in model space */
	trs = Mem_Alloc(sizeof(float) * 10 * st->numbones);
	rest = Mem_Alloc(sizeof(bonemat_t) * st->numbones);
	Studio_RestPose(st, trs);
	for (int j = 0; j < st->numbones; j++) {
		bonemat_t rel;
		Bone_FromTRS(rel, trs + j * 10);
		if (st->bones[j].parent >= 0)
			Bone_Multiply(rest[j], rest[st->bones[j].parent], rel);
		else
			memcpy(rest[j], rel, sizeof(bonemat_t));
	}
	/* skins: family x slot */
	nskins = st->numskinref * BW_MAX(1, st->numskinfamilies);
	q->numskinref = st->numskinref;
	q->numfamilies = BW_MAX(1, st->numskinfamilies);
	q->famskins = Mem_Alloc(sizeof(material_t *) * (nskins + 1));
	{
		material_t **bytex = Mem_Alloc(sizeof(material_t *) * (st->numtextures + 1));
		for (int i = 0; i < st->numtextures; i++)
			bytex[i] = studio_skin(mod->name, &st->textures[i]);
		for (int i = 0; i < nskins; i++) {
			int t = st->skins ? st->skins[i] : i;
			q->famskins[i] = t >= 0 && t < st->numtextures ? bytex[t] : R_FindMaterial("$default", MATF_MODEL);
		}
		Mem_Free(bytex);
	}
	for (int b = 0; b < st->numbodyparts; b++)
	for (int s = 0; s < st->bodyparts[b].nummodels; s++) {
		const studio_submodel_t *sm = &st->bodyparts[b].models[s];
		for (int k = 0; k < sm->nummeshes; k++) {
			const studio_mesh_t *me = &sm->meshes[k];
			int slot = me->skinref, tex = st->skins && slot >= 0 && slot < st->numskinref ? st->skins[slot] : slot;
			const studio_texture_t *t = tex >= 0 && tex < st->numtextures ? &st->textures[tex] : NULL;
			const uint8_t *p = me->cmds;
			iqmmesh_t *out = &q->meshes[q->nummeshes++];
			out->skinref = slot;
			/* single sub-model parts always draw */
			out->geomgroup = st->bodyparts[b].nummodels > 1 ? b : -1;
			out->geomvalue = s;
			Str_Copy(out->name, t ? t->name : "", sizeof(out->name));
			out->material = slot >= 0 && slot < q->numskinref ? q->famskins[slot] : R_FindMaterial("$default", MATF_MODEL);
			out->firstindex = ni;
			/* strips (n > 0) and fans (n < 0) of n vertices, until 0 */
			for (;;) {
				int n = (int16_t)(p[0] | p[1] << 8), first = nv, fan = n < 0;
				p += 2;
				if (!n)
					break;
				n = abs(n);
				for (int i = 0; i < n; i++, p += 8) {
					uint8_t bone;
					studio_vert(sm, rest, t, p, &q->verts[nv], &bone);
					if (q->skel) {
						q->blendidx[nv][0] = bone;
						q->blendw[nv][0] = 1;
					}
					nv++;
				}
				for (int i = 2; i < n; i++) {
					uint32_t a, b2, c = (uint32_t)(first + i);
					if (fan)
						a = (uint32_t)first, b2 = (uint32_t)(first + i - 1);
					else if (i & 1)
						a = (uint32_t)(first + i - 1), b2 = (uint32_t)(first + i - 2);
					else
						a = (uint32_t)(first + i - 2), b2 = (uint32_t)(first + i - 1);
					q->indices[ni++] = a, q->indices[ni++] = b2, q->indices[ni++] = c;
				}
			}
			out->numindexes = ni - out->firstindex;
		}
	}
	q->numverts = nv;
	q->numindexes = ni;
	{
		/* face the triangles the way the IQM loader leaves its own: wound
		 * with their vertex normals, GL's front. Counted, so one odd mesh
		 * doesn't decide it */
		int with = 0, against = 0;
		for (int i = 0; i + 2 < ni; i += 3) {
			const rvert_t *a = &q->verts[q->indices[i]], *b = &q->verts[q->indices[i + 1]], *c = &q->verts[q->indices[i + 2]];
			float e1[3], e2[3], n[3];
			VectorSubtract(b->xyz, a->xyz, e1);
			VectorSubtract(c->xyz, a->xyz, e2);
			CrossProduct(e1, e2, n);
			if (DotProduct(n, a->normal) + DotProduct(n, b->normal) + DotProduct(n, c->normal) > 0)
				with++;
			else
				against++;
		}
		if (against > with)
			for (int i = 0; i + 2 < ni; i += 3) {
				uint32_t tmp = q->indices[i + 1];
				q->indices[i + 1] = q->indices[i + 2];
				q->indices[i + 2] = tmp;
			}
		Con_DPrintf("%s: studio model, %d triangles, %d bones, %d sequences\n", mod->name, ni / 3, st->numbones,
		            st->numseqs);
	}
	Mem_Free(trs);
	Mem_Free(rest);
	finish_iqm(mod, q);
	return true;
}

/* --- BSP files drawn as models ------------------------------------------------ */

/* Quake's item boxes (maps/b_bh25.bsp and the like) are small BSP maps used
 * as models. The first model's faces become one static mesh per texture,
 * lit like any model; their lightmaps aren't used. Q1 BSP29, BSP2 and
 * GoldSrc BSP30; record layouts as in docs/spec/bsp.md. */
static bool load_bspmodel(rmodel_t *mod, const uint8_t *d, size_t size)
{
	enum { L_TEXTURES = 2, L_VERTEXES = 3, L_TEXINFO = 6, L_FACES = 7, L_EDGES = 12, L_SURFEDGES = 13,
	       L_MODELS = 14, L_COUNT = 15 };
	struct {
		int32_t ofs, len;
	} l[L_COUNT];
	bool b2 = !memcmp(d, "BSP2", 4), hl = rd32(d) == 30;
	int facesize = b2 ? 28 : 20, edgesize = b2 ? 8 : 4;
	int numtex = 0, firstface, numfaces, numverts, numedges, numsurfedges, numtexinfo, nv = 0, ni = 0;
	material_t **mats = NULL;
	int (*sizes)[2] = NULL;
	iqm_t *q;
	if (size < 4 + L_COUNT * 8)
		return false;
	for (int i = 0; i < L_COUNT; i++) {
		l[i].ofs = rd32(d + 4 + i * 8);
		l[i].len = rd32(d + 8 + i * 8);
		if (l[i].ofs < 0 || l[i].len < 0 || (size_t)l[i].ofs + l[i].len > size)
			return false;
	}
	if (l[L_MODELS].len < 64)
		return false;
	firstface = rd32(d + l[L_MODELS].ofs + 56);
	numfaces = rd32(d + l[L_MODELS].ofs + 60);
	numverts = l[L_VERTEXES].len / 12;
	numedges = l[L_EDGES].len / edgesize;
	numsurfedges = l[L_SURFEDGES].len / 4;
	numtexinfo = l[L_TEXINFO].len / 40;
	if (firstface < 0 || numfaces < 0 || (int64_t)firstface + numfaces > l[L_FACES].len / facesize)
		return false;
	if (l[L_TEXTURES].len >= 4) {
		const uint8_t *tl = d + l[L_TEXTURES].ofs;
		numtex = rd32(tl);
		if (numtex < 0 || 4 + (int64_t)numtex * 4 > l[L_TEXTURES].len)
			numtex = 0;
		mats = Mem_Alloc(sizeof(*mats) * (numtex + 1));
		sizes = Mem_Alloc(sizeof(*sizes) * (numtex + 1));
		for (int i = 0; i < numtex; i++) {
			int32_t o = rd32(tl + 4 + i * 4);
			sizes[i][0] = sizes[i][1] = 64;
			mats[i] = o < 0 || o + 40 > l[L_TEXTURES].len
			              ? R_FindMaterial("$default", MATF_MODEL)
			              : R_Q1TextureMaterial(tl + o, (size_t)(l[L_TEXTURES].len - o), &sizes[i][0], &sizes[i][1], hl);
		}
	}
	q = Mem_Alloc(sizeof(*q));
	/* a face of n edges is a fan of n - 2 triangles */
	for (int f = 0; f < numfaces; f++) {
		const uint8_t *fp = d + l[L_FACES].ofs + (firstface + f) * facesize;
		int n = b2 ? rd32(fp + 12) : rd16(fp + 8);
		if (n >= 3 && n <= 256) {
			nv += n;
			ni += (n - 2) * 3;
		}
	}
	q->verts = Mem_Alloc(sizeof(rvert_t) * (nv + 1));
	q->indices = Mem_Alloc(sizeof(uint32_t) * (ni + 1));
	q->meshes = Mem_Alloc(sizeof(iqmmesh_t) * (numtex + 1));
	/* one mesh per texture, faces in texture order */
	for (int t = -1; t < numtex; t++) {
		int first = q->numindexes;
		for (int f = 0; f < numfaces; f++) {
			const uint8_t *fp = d + l[L_FACES].ofs + (firstface + f) * facesize;
			int firstedge = b2 ? rd32(fp + 8) : rd32(fp + 4), n = b2 ? rd32(fp + 12) : rd16(fp + 8);
			int ti = b2 ? rd32(fp + 16) : rd16(fp + 10), side = b2 ? rd32(fp + 4) : rd16(fp + 2);
			int plane = b2 ? rd32(fp) : rdu16(fp);
			const uint8_t *tip;
			float vecs[2][4], normal[3] = {0, 0, 1};
			int miptex, base = q->numverts;
			if (n < 3 || n > 256 || ti < 0 || ti >= numtexinfo || firstedge < 0 || firstedge + n > numsurfedges)
				continue;
			tip = d + l[L_TEXINFO].ofs + ti * 40;
			miptex = rd32(tip + 32);
			if (miptex < 0 || miptex >= numtex)
				miptex = -1;
			if (miptex != t)
				continue;
			for (int k = 0; k < 8; k++)
				vecs[k / 4][k % 4] = rdf(tip + k * 4);
			if (plane >= 0 && (int64_t)(plane + 1) * 20 <= rd32(d + 4 + 1 * 8 + 4)) {
				const uint8_t *pp = d + rd32(d + 4 + 1 * 8) + plane * 20;
				for (int k = 0; k < 3; k++)
					normal[k] = rdf(pp + k * 4) * (side ? -1.0f : 1.0f);
			}
			for (int e = 0; e < n; e++) {
				int se = rd32(d + l[L_SURFEDGES].ofs + (firstedge + e) * 4), edge = se < 0 ? -se : se, v;
				rvert_t *out = &q->verts[q->numverts];
				const uint8_t *ep;
				if (edge >= numedges)
					break;
				ep = d + l[L_EDGES].ofs + edge * edgesize;
				v = b2 ? rd32(ep + (se < 0 ? 4 : 0)) : rdu16(ep + (se < 0 ? 2 : 0));
				if (v < 0 || v >= numverts)
					break;
				for (int k = 0; k < 3; k++) {
					out->xyz[k] = rdf(d + l[L_VERTEXES].ofs + v * 12 + k * 4);
					out->normal[k] = normal[k];
				}
				out->st[0] = (DotProduct(out->xyz, vecs[0]) + vecs[0][3]) / (t >= 0 ? sizes[t][0] : 64);
				out->st[1] = (DotProduct(out->xyz, vecs[1]) + vecs[1][3]) / (t >= 0 ? sizes[t][1] : 64);
				memset(out->color, 255, 4);
				q->numverts++;
			}
			/* a fan, wound for GL's counter-clockwise fronts as the world is */
			for (int k = 2; k < q->numverts - base; k++) {
				q->indices[q->numindexes++] = (uint32_t)base;
				q->indices[q->numindexes++] = (uint32_t)(base + k);
				q->indices[q->numindexes++] = (uint32_t)(base + k - 1);
			}
		}
		if (q->numindexes > first) {
			iqmmesh_t *m = &q->meshes[q->nummeshes++];
			m->material = t >= 0 ? mats[t] : R_FindMaterial("$default", MATF_MODEL);
			m->firstindex = first;
			m->numindexes = q->numindexes - first;
			m->skinref = -1;
			m->geomgroup = m->geomvalue = -1;
			if (t >= 0)
				Str_Copy(m->name, m->material->name, sizeof(m->name));
		}
	}
	Mem_Free(mats);
	Mem_Free(sizes);
	finish_iqm(mod, q);
	Con_DPrintf("%s: BSP model, %d triangles\n", mod->name, q->numindexes / 3);
	return true;
}

/* --- cache ------------------------------------------------------------------ */

#define MAX_RMODELS 2048
static rmodel_t *models[MAX_RMODELS];
static int nummodels;

/* --- sprites (.spr) ----------------------------------------------------------- */

/* GoldSrc sprites (IDSP version 2; the header is described in r_image.c).
 * Each frame becomes its own material, named "model:N" as spriteframe names
 * them, with frame groups counting each member. The frame's origin is the
 * offset of its top left corner from the sprite's centre. Orientation types:
 * 0 parallel upright, 1 facing upright, 2 parallel, 3 oriented, 4 parallel
 * oriented (DESIGN: the usual meanings of these Quake sprite types). */
typedef struct {
	float left, right, top, bottom;
	material_t *material;
} spriteframe_t;

typedef struct sprite_s {
	int type, format, numframes;
	spriteframe_t *frames;
} sprite_t;

static bool sprite_frame(rmodel_t *mod, sprite_t *s, const uint8_t *d, size_t size, size_t *ofs)
{
	spriteframe_t *f;
	int32_t ox, oy, w, h;
	if (*ofs + 16 > size)
		return false;
	ox = rd32(d + *ofs), oy = rd32(d + *ofs + 4), w = rd32(d + *ofs + 8), h = rd32(d + *ofs + 12);
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || *ofs + 16 + (size_t)w * h > size)
		return false;
	*ofs += 16 + (size_t)w * h;
	f = &s->frames[s->numframes];
	f->left = (float)ox;
	f->right = (float)(ox + w);
	f->top = (float)oy;
	f->bottom = (float)(oy - h);
	f->material = R_FindMaterial(s->numframes ? va("%s:%d", mod->name, s->numframes) : mod->name, MATF_MODEL);
	s->numframes++;
	return true;
}

static bool load_sprite(rmodel_t *mod, const uint8_t *d, size_t size)
{
	sprite_t *s;
	size_t ofs;
	int npal, count, total = 0;
	if (size < 42 || rd32(d + 4) != 2)
		return false;
	count = rd32(d + 28);
	npal = d[40] | (d[41] << 8);
	if (count <= 0 || count > 4096 || npal <= 0 || npal > 256 || 42 + (size_t)npal * 3 > size)
		return false;
	/* count the frames, groups included, before making them */
	ofs = 42 + (size_t)npal * 3;
	for (int i = 0; i < count; i++) {
		int n = 1;
		if (ofs + 4 > size)
			return false;
		if (rd32(d + ofs) != 0) {
			if (ofs + 8 > size)
				return false;
			n = rd32(d + ofs + 4);
			if (n <= 0 || n > 1024)
				return false;
			ofs += 4 + (size_t)n * 4;
		}
		ofs += 4;
		for (int g = 0; g < n; g++) {
			int32_t w, h;
			if (ofs + 16 > size)
				return false;
			w = rd32(d + ofs + 8), h = rd32(d + ofs + 12);
			if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
				return false;
			ofs += 16 + (size_t)w * h;
		}
		total += n;
	}
	s = Mem_Alloc(sizeof(*s));
	s->type = rd32(d + 8);
	s->format = rd32(d + 12);
	s->frames = Mem_Alloc(sizeof(spriteframe_t) * total);
	ofs = 42 + (size_t)npal * 3;
	for (int i = 0; i < count; i++) {
		int n = 1;
		if (rd32(d + ofs) != 0) {
			n = rd32(d + ofs + 4);
			ofs += 4 + (size_t)n * 4;
		}
		ofs += 4;
		for (int g = 0; g < n; g++)
			if (!sprite_frame(mod, s, d, size, &ofs)) {
				Mem_Free(s->frames);
				Mem_Free(s);
				return false;
			}
	}
	for (int i = 0; i < s->numframes; i++) {
		const spriteframe_t *f = &s->frames[i];
		float r = BW_MAX(BW_MAX(fabsf(f->left), fabsf(f->right)), BW_MAX(fabsf(f->top), fabsf(f->bottom)));
		for (int k = 0; k < 3; k++)
			mod->mins[k] = BW_MIN(mod->mins[k], -r), mod->maxs[k] = BW_MAX(mod->maxs[k], r);
	}
	mod->sprite = s;
	mod->numframes = s->numframes;
	mod->type = MOD_SPRITE;
	return true;
}

/* a sprite's frame for particles: its material, and its rectangle about the
 * sprite's centre as left, right, top, bottom; NULL for a bad frame */
int R_SpriteFrameCount(const rmodel_t *m)
{
	return (m && m->type == MOD_SPRITE && m->sprite) ? m->sprite->numframes : 0;
}

material_t *R_SpriteFrame(const rmodel_t *m, int frame, float *rect)
{
	const spriteframe_t *f;
	if (!m || m->type != MOD_SPRITE || !m->sprite || m->sprite->numframes <= 0)
		return NULL;
	frame = BW_MAX(0, BW_MIN(m->sprite->numframes - 1, frame));
	f = &m->sprite->frames[frame];
	rect[0] = f->left, rect[1] = f->right, rect[2] = f->top, rect[3] = f->bottom;
	return f->material;
}

/* sprites face the camera, always in the translucent pass. Additive sprites
 * (texture format 1, EF_ADDITIVE or RF_ADDITIVE) add onto the scene */
static void draw_sprite(rentity_t *e, const float *viewproj)
{
	const glprog_t *p = &r_progs[PROG_UNLIT];
	const sprite_t *s = e->model->sprite;
	const spriteframe_t *f = &s->frames[e->frame >= 0 && e->frame < s->numframes ? e->frame : 0];
	material_t *mat = e->forceshader ? e->forceshader : f->material;
	float right[3], up[3], c[4], rgba[4], sc = e->scale;
	bool additive = s->format == 1 || (e->effects & 32) || (e->renderflags & 8);
	GLenum src = GL_SRC_ALPHA, dst = additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA;
	rvert_t v[4];
	static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
	if (!mat->diffuse)
		return;
	switch (s->type) {
	case 0: /* parallel upright: turns about the vertical, square to the view */
	case 1: /* facing upright: turns about the vertical towards the viewer */
		up[0] = up[1] = 0, up[2] = 1;
		if (s->type == 0)
			right[0] = r_scene.right[0], right[1] = r_scene.right[1];
		else
			right[0] = e->origin[1] - r_scene.origin[1], right[1] = r_scene.origin[0] - e->origin[0];
		right[2] = 0;
		if (VectorNormalize(right) < 1e-6f)
			VectorCopy(r_scene.right, right);
		break;
	case 3: /* oriented: the entity's own axes */
		for (int k = 0; k < 3; k++)
			right[k] = -e->axis[1][k], up[k] = e->axis[2][k];
		break;
	case 4: { /* parallel, turned by the entity's roll */
		float a = e->angles[2] * (float)M_PI / 180, cs = cosf(a), sn = sinf(a);
		for (int k = 0; k < 3; k++) {
			right[k] = r_scene.right[k] * cs + r_scene.up[k] * sn;
			up[k] = r_scene.up[k] * cs - r_scene.right[k] * sn;
		}
		break;
	}
	default:
		VectorCopy(r_scene.right, right);
		VectorCopy(r_scene.up, up);
	}
	for (int k = 0; k < 3; k++) {
		float l = f->left * sc, r = f->right * sc, t = f->top * sc, b = f->bottom * sc;
		v[0].xyz[k] = e->origin[k] + right[k] * l + up[k] * b;
		v[1].xyz[k] = e->origin[k] + right[k] * l + up[k] * t;
		v[2].xyz[k] = e->origin[k] + right[k] * r + up[k] * t;
		v[3].xyz[k] = e->origin[k] + right[k] * r + up[k] * b;
	}
	for (int i = 0; i < 4; i++) {
		v[i].st[0] = i < 2 ? 0.0f : 1.0f;
		v[i].st[1] = (i == 1 || i == 2) ? 0.0f : 1.0f;
		memset(v[i].normal, 0, sizeof(v[i].normal));
		memset(v[i].color, 255, 4);
	}
	rgba[0] = e->colormod[0], rgba[1] = e->colormod[1], rgba[2] = e->colormod[2], rgba[3] = e->alpha;
	if (!rgba[0] && !rgba[1] && !rgba[2])
		rgba[0] = rgba[1] = rgba[2] = 1;
	if (e->forceshader && mat->fromfile && mat->blend != BLEND_OPAQUE) {
		int b;
		R_PickBlend(mat, BLEND_ALPHA, src, dst, &b, &src, &dst);
	}
	R_BlendColor(src, rgba, c);
	R_UseProgram(PROG_UNLIT);
	qglUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, viewproj);
	qglUniform4f(p->u_color, c[0], c[1], c[2], c[3]);
	qglUniform4f(p->u_tcmod, 0, 0, 1, 1);
	qglUniform1f(p->u_alphatest, s->format == 3 ? 0.5f : 0.0f);
	GL_BindTexture(0, GL_TEXTURE_2D, mat->diffuse->tex);
	GL_SetBlend(BLEND_CUSTOM, src, dst);
	GL_SetDepth(true, false);
	GL_SetCull(false);
	R_StreamDraw(GL_TRIANGLES, v, 4, idx, 6);
	GL_SetCull(true);
	GL_SetDepth(true, true);
}

static void free_model(rmodel_t *m)
{
	if (m->sprite) {
		Mem_Free(m->sprite->frames);
		Mem_Free(m->sprite);
	}
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
		if (!q->skel)
			Mem_Free(q->anims);
		Mem_Free(q->blendidx);
		Mem_Free(q->blendw);
		Mem_Free(q->famskins);
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
	/* the map itself, named by its file (a game asking for world.model) */
	if (r_world.loaded && r_world.numsubmodels && !Str_ICmp(name, r_world.name))
		return &r_world.submodels[0];
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
	else if (size >= 8 && !memcmp(d, "IDST", 4) && d[4] == 10)
		m->loaded = load_studio(m);
	else if (size >= 4 && !memcmp(d, "IDSP", 4))
		m->loaded = load_sprite(m, d, size);
	else if (size >= 4 && (rd32(d) == 29 || rd32(d) == 30 || !memcmp(d, "BSP2", 4)))
		m->loaded = load_bspmodel(m, d, size);
	else {
		/* a plugin may turn it into an IQM (Source .mdl) */
		size_t qlen;
		uint8_t *q = Plug_ConvertModel(name, d, size, &qlen);
		if (q && qlen >= 16 && !memcmp(q, "INTERQUAKEMODEL", 16))
			m->loaded = load_iqm(m, q, qlen);
		else
			Con_DPrintf("model %s: format not supported yet\n", name);
		Mem_Free(q);
	}
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

/* poses a skeletal IQM for e and uploads the skinned vertices: the bones
 * come from the entity's skeleton object when it has one, otherwise from
 * its frame fields */
static void skin_iqm(iqm_t *q, rentity_t *e)
{
	static bonemat_t *bones, *skin;
	static int maxbones;
	static rvert_t *out;
	static int maxverts;
	skelmodel_t *m = q->skel, *sm;
	const float(*sb)[12];
	bool absolute;
	int n = m->numjoints;
	if (n > maxbones) {
		maxbones = n;
		bones = Mem_Realloc(bones, sizeof(bonemat_t) * n);
		skin = Mem_Realloc(skin, sizeof(bonemat_t) * n);
	}
	if (q->numverts > maxverts) {
		maxverts = q->numverts;
		out = Mem_Realloc(out, sizeof(rvert_t) * maxverts);
	}
	if (e->skeletonindex && e->vm && QC_SkelBones(e->vm, e->skeletonindex, &sm, &sb, &absolute)) {
		int k = BW_MIN(n, sm->numjoints);
		Skel_SampleRelative(m, -1, 0, bones); /* any bones the object lacks */
		if (absolute) {
			Skel_ToAbsolute(m, bones, bones);
			memcpy(bones, sb, sizeof(bonemat_t) * k);
		} else {
			memcpy(bones, sb, sizeof(bonemat_t) * k);
			Skel_ToAbsolute(m, bones, bones);
		}
	} else {
		Skel_EntityPose(m, e->frame, e->frame1time, e->frame2, e->frame2time, e->lerpfrac, bones);
		Skel_ToAbsolute(m, bones, bones);
	}
	for (int j = 0; j < n; j++)
		Bone_Multiply(skin[j], bones[j], m->invbind[j]);
	for (int i = 0; i < q->numverts; i++) {
		const rvert_t *v = &q->verts[i];
		rvert_t *o = &out[i];
		bonemat_t mat;
		const float *w = q->blendw[i];
		const uint8_t *b = q->blendidx[i];
		*o = *v;
		for (int k = 0; k < 12; k++)
			mat[k] = skin[b[0]][k] * w[0] + skin[b[1]][k] * w[1] + skin[b[2]][k] * w[2] + skin[b[3]][k] * w[3];
		Bone_Transform(mat, v->xyz, o->xyz);
		for (int r = 0; r < 3; r++)
			o->normal[r] = mat[r * 4] * v->normal[0] + mat[r * 4 + 1] * v->normal[1] + mat[r * 4 + 2] * v->normal[2];
		VectorNormalize(o->normal);
	}
	qglBindBuffer(GL_ARRAY_BUFFER, q->vbo);
	qglBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(sizeof(rvert_t) * q->numverts), out);
}

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

/* an alias model's vertices for the entity's frames, lerped */
static rvert_t *alias_verts(const rentity_t *e, alias_t *a)
{
	static rvert_t *buf;
	static int bufsize;
	int p1 = alias_pose(a, e->frame, e->frame1time), p2 = alias_pose(a, e->frame2, e->frame2time);
	float lerp = e->lerpfrac;
	const float *pa = a->poses + (size_t)p1 * a->numverts * 3, *pb = a->poses + (size_t)p2 * a->numverts * 3;
	const float *na = a->normals + (size_t)p1 * a->numverts * 3, *nb = a->normals + (size_t)p2 * a->numverts * 3;
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
	return buf;
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
	bool viewmodel = (e->renderflags & (1 | 4)) != 0; /* RF_VIEWMODEL or RF_DEPTHHACK */
	if (!m || !m->loaded)
		return;
	Mat4_FromAxis(model, e->axis, e->origin, e->scale);
	Mat4_Multiply(mvp, viewmodel ? r_scene.vmviewproj : r_scene.viewproj, model);

	if (m->type == MOD_BRUSH) {
		bool trans = e->alpha < 1 || (e->renderflags & 8);
		/* an entity wearing the map itself (modelindex 1): the scene draws the
		 * world once; drawing it again here would also redo its visibility
		 * with this entity's transform */
		if (m == &r_world.submodels[0])
			return;
		if (trans == translucentpass || translucentpass)
			R_DrawWorldModel(m, mvp, model, e->alpha, translucentpass);
		return;
	}
	if (m->type == MOD_SPRITE) {
		if (translucentpass) {
			if (depthhack)
				qglDepthRange(0, 0.3);
			draw_sprite(e, viewmodel ? r_scene.vmviewproj : r_scene.viewproj);
			if (depthhack)
				qglDepthRange(0, 1);
		}
		return;
	}
	if ((e->alpha < 1 || (e->renderflags & 8)) != translucentpass)
		return;
	if (depthhack)
		qglDepthRange(0, 0.3);
	if (e->mirrored)
		qglCullFace(GL_FRONT);
	R_UseProgram(PROG_MODEL);
	qglUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, mvp);
	qglUniformMatrix4fv(p->u_model, 1, GL_FALSE, model);
	qglUniform4f(p->u_color, 1, 1, 1, e->alpha);
	qglUniform3f(p->u_vieworg, r_scene.origin[0], r_scene.origin[1], r_scene.origin[2]);
	qglUniform4f(p->u_fog, r_viewfog[0], r_viewfog[1], r_viewfog[2], r_viewfog[3]);
	set_model_lighting(p, e);

	if (m->type == MOD_ALIAS) {
		alias_t *a = m->alias;
		rvert_t *buf = alias_verts(e, a);
		material_t *skin;
		skin = e->forceshader ? e->forceshader
		       : a->numskins ? a->skins[e->skin >= 0 && e->skin < a->numskins ? e->skin : 0]
		                     : R_FindMaterial("$default", MATF_MODEL);
		bind_material(p, skin, e->alpha, translucentpass);
		R_StreamDraw(GL_TRIANGLES, buf, a->numoutverts, a->indices, a->numtris * 3);
	} else if (m->type == MOD_IQM && m->iqm->vao) {
		iqm_t *q = m->iqm;
		if (q->skel)
			skin_iqm(q, e);
		qglBindVertexArray(q->vao);
		for (int i = 0; i < q->nummeshes; i++) {
			material_t *mat = e->forceshader ? e->forceshader : q->meshes[i].material;
			if (q->meshes[i].geomgroup >= 0 &&
			    R_SkinGeomset(e->customskin, q->meshes[i].geomgroup) != q->meshes[i].geomvalue)
				continue; /* another sub-model of this body part */
			if (!e->forceshader && q->famskins && e->skin > 0 && e->skin < q->numfamilies &&
			    q->meshes[i].skinref >= 0 && q->meshes[i].skinref < q->numskinref)
				mat = q->famskins[e->skin * q->numskinref + q->meshes[i].skinref]; /* a skin family */
			if (!e->forceshader && e->customskin) {
				material_t *r = R_SkinReplace(e->customskin, q->meshes[i].name);
				if (r)
					mat = r;
			}
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
	if (e->mirrored)
		qglCullFace(GL_BACK);
	GL_SetCull(true);
}

/* --- outlines --------------------------------------------------------------- */

/* RF_OUTLINE: Counter-Strike Online's teammate outline (cl_outline_tdm).
 * CSO's hw.dll (read in the CSNZ reconstruction: FUN_023035d0, FUN_02303190)
 * puts the model in the stencil and then draws its wireframe with 4.5 to 7
 * pixel lines where the stencil is clear, through walls; only the outer half
 * of the lines shows. Wide lines aren't available everywhere and look ragged,
 * so Bulwark draws the same band in screen space instead: the model goes
 * into a mask texture, and every pixel outside it within half CSO's line
 * width (7 - d^2 / 10000 pixels, at least 4.5, d the distance from the view)
 * gets the outline, smoothed at the outer edge. CSO's alpha is kept: 0.2 +
 * d^2 / 100000, at most 0.8, and at most (z - (origin z + mins z)) /
 * (maxs z - mins z) of the nearest model pixel, so it fades out towards the
 * bottom of the entity's box. View models go into the mask too and hide
 * outlines, as they do in CSO, where outlines are drawn with their models
 * before the view model. No normals, lines or stencil are involved. */
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_SCISSOR_BOX
#define GL_SCISSOR_BOX 0x0C10
#endif
static cvar_t *r_outline, *r_outline_width;
static GLuint outline_fb, outline_tex;
static int outline_w, outline_h;

static void outline_geometry(rentity_t *e)
{
	rmodel_t *m = e->model;
	if (m->type == MOD_ALIAS) {
		alias_t *a = m->alias;
		R_StreamDraw(GL_TRIANGLES, alias_verts(e, a), a->numoutverts, a->indices, a->numtris * 3);
	} else if (m->type == MOD_IQM && m->iqm->vao) {
		iqm_t *q = m->iqm;
		qglBindVertexArray(q->vao);
		for (int i = 0; i < q->nummeshes; i++) {
			if (q->meshes[i].geomgroup >= 0 &&
			    R_SkinGeomset(e->customskin, q->meshes[i].geomgroup) != q->meshes[i].geomvalue)
				continue;
			if (!q->meshes[i].numindexes || q->meshes[i].material->nodraw)
				continue;
			qglDrawElements(GL_TRIANGLES, q->meshes[i].numindexes, GL_UNSIGNED_INT,
			                (void *)(sizeof(uint32_t) * (size_t)q->meshes[i].firstindex));
		}
		qglBindVertexArray(0);
	}
}

static bool outline_target(void)
{
	if (!qglFramebufferTexture2D_opt)
		return false;
	if (outline_fb && outline_w == r_pwidth && outline_h == r_pheight)
		return true;
	if (!outline_fb) {
		qglGenFramebuffers(1, &outline_fb);
		qglGenTextures(1, &outline_tex);
	}
	GL_BindTexture(0, GL_TEXTURE_2D, outline_tex);
	qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, r_pwidth, r_pheight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	qglBindFramebuffer(GL_FRAMEBUFFER, outline_fb);
	qglFramebufferTexture2D_opt(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outline_tex, 0);
	outline_w = r_pwidth, outline_h = r_pheight;
	return true;
}

/* the entity's box on screen, in physical pixels, widened by pad; false if
 * it is all behind the view. Partly behind: the whole box given */
static bool outline_rect(const rentity_t *e, const int *box, int pad, int *rect)
{
	float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
	const float *vp = r_scene.viewproj;
	int behind = 0;
	for (int c = 0; c < 8; c++) {
		float p[3], clip[4];
		for (int k = 0; k < 3; k++)
			p[k] = e->origin[k] + ((c >> k) & 1 ? e->maxs[k] : e->mins[k]);
		for (int r = 0; r < 4; r++)
			clip[r] = vp[r] * p[0] + vp[4 + r] * p[1] + vp[8 + r] * p[2] + vp[12 + r];
		if (clip[3] <= 0.01f) {
			behind++;
			continue;
		}
		for (int k = 0; k < 2; k++) {
			float ndc = clip[k] / clip[3];
			lo[k] = BW_MIN(lo[k], ndc), hi[k] = BW_MAX(hi[k], ndc);
		}
	}
	if (behind == 8)
		return false;
	rect[0] = box[0], rect[1] = box[1], rect[2] = box[0] + box[2], rect[3] = box[1] + box[3];
	if (!behind) {
		int x0 = box[0] + (int)((lo[0] * 0.5f + 0.5f) * box[2]) - pad, x1 = box[0] + (int)((hi[0] * 0.5f + 0.5f) * box[2]) + pad;
		int y0 = box[1] + (int)((lo[1] * 0.5f + 0.5f) * box[3]) - pad, y1 = box[1] + (int)((hi[1] * 0.5f + 0.5f) * box[3]) + pad;
		rect[0] = BW_MAX(rect[0], x0), rect[1] = BW_MAX(rect[1], y0);
		rect[2] = BW_MIN(rect[2], x1), rect[3] = BW_MIN(rect[3], y1);
	}
	return rect[2] > rect[0] && rect[3] > rect[1];
}

void R_DrawOutlines(void)
{
	const glprog_t *p = &r_progs[PROG_OUTLINE], *p2 = &r_progs[PROG_OUTLINE2];
	static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	static rvert_t quad[4];
	static const uint16_t quadidx[6] = {0, 1, 2, 0, 2, 3};
	bool any = false;
	GLint box[4], savedfb;
	if (!r_outline) {
		r_outline = Cvar_Get("r_outline", "1", CVAR_ARCHIVE, "Draw outlines on entities that ask for one (RF_OUTLINE)");
		r_outline_width = Cvar_Get("r_outline_width", "1", CVAR_ARCHIVE, "Scales the width of outlines (1 is CSO's)");
	}
	if (!r_outline->ivalue || !p->prog || !p2->prog)
		return;
	if (!quad[1].xyz[0]) {
		quad[0].xyz[0] = quad[3].xyz[0] = quad[0].xyz[1] = quad[1].xyz[1] = -1;
		quad[1].xyz[0] = quad[2].xyz[0] = quad[2].xyz[1] = quad[3].xyz[1] = 1;
	}
	for (int i = 0; i < r_scene.numents && !any; i++)
		any = (r_scene.ents[i].renderflags & RF_OUTLINE) != 0;
	if (!any)
		return;
	qglGetIntegerv(GL_SCISSOR_BOX, box);
	qglGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedfb);
	if (!outline_target()) {
		qglBindFramebuffer(GL_FRAMEBUFFER, (GLuint)savedfb);
		return;
	}
	GL_SetCull(false);
	GL_SetDepth(false, false);
	for (int i = 0; i < r_scene.numents; i++) {
		rentity_t *e = &r_scene.ents[i];
		rmodel_t *m = e->model;
		float model[16], mvp[16], d2 = 0, width, alpha;
		int rect[4];
		if (!(e->renderflags & RF_OUTLINE) || (e->renderflags & (1 | 4)) || !m || !m->loaded ||
		    (m->type != MOD_ALIAS && m->type != MOD_IQM))
			continue;
		for (int k = 0; k < 3; k++)
			d2 += (e->origin[k] - r_scene.origin[k]) * (e->origin[k] - r_scene.origin[k]);
		width = 7 - d2 * 0.0001f;
		if (width < 4.5f)
			width = 4.5f;
		width *= 0.5f * r_outline_width->value; /* the outer half of CSO's lines */
		alpha = d2 * 1e-5f + 0.2f;
		if (alpha > 0.8f)
			alpha = 0.8f;
		if (!outline_rect(e, box, (int)ceilf(width) + 2, rect))
			continue;

		/* the mask */
		qglBindFramebuffer(GL_FRAMEBUFFER, outline_fb);
		{
			/* wider than the band, which looks this far past its own edge */
			int pad = (int)ceilf(width) + 1;
			int x0 = BW_MAX(rect[0] - pad, 0), y0 = BW_MAX(rect[1] - pad, 0);
			int x1 = BW_MIN(rect[2] + pad, outline_w), y1 = BW_MIN(rect[3] + pad, outline_h);
			qglScissor(x0, y0, x1 - x0, y1 - y0);
		}
		qglClearColor(0, 0, 0, 0);
		qglClear(GL_COLOR_BUFFER_BIT);
		GL_SetBlend(BLEND_OPAQUE, 0, 0);
		R_UseProgram(PROG_OUTLINE);
		qglUniform4f(p->u_outline_z, 0, 0, 0, 0);
		qglUniform4f(p->u_color, 0, 0, 1, 1); /* view models: hide the outline */
		for (int j = 0; j < r_scene.numents; j++) {
			rentity_t *v = &r_scene.ents[j];
			float vmodel[16], vmvp[16];
			if (!(v->renderflags & (1 | 4)) || !v->model || !v->model->loaded ||
			    (v->model->type != MOD_ALIAS && v->model->type != MOD_IQM))
				continue;
			Mat4_FromAxis(vmodel, v->axis, v->origin, v->scale);
			Mat4_Multiply(vmvp, r_scene.vmviewproj, vmodel);
			qglUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, vmvp);
			qglUniformMatrix4fv(p->u_model, 1, GL_FALSE, vmodel);
			if (v->model->type == MOD_IQM && v->model->iqm->skel)
				skin_iqm(v->model->iqm, v);
			outline_geometry(v);
		}
		Mat4_FromAxis(model, e->axis, e->origin, e->scale);
		Mat4_Multiply(mvp, r_scene.viewproj, model);
		qglUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, mvp);
		qglUniformMatrix4fv(p->u_model, 1, GL_FALSE, model);
		qglUniform4f(p->u_color, 1, 0, 0, 1);
		qglUniform4f(p->u_outline_z, e->origin[2] + e->mins[2], e->maxs[2] - e->mins[2], 0, 0);
		if (m->type == MOD_IQM && m->iqm->skel)
			skin_iqm(m->iqm, e); /* the buffer holds whichever entity used the model last */
		outline_geometry(e);

		/* the band around it, onto the scene */
		qglBindFramebuffer(GL_FRAMEBUFFER, (GLuint)savedfb);
		qglScissor(rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]);
		R_UseProgram(PROG_OUTLINE2);
		GL_BindTexture(0, GL_TEXTURE_2D, outline_tex);
		GL_SetBlend(BLEND_ALPHA, 0, 0);
		qglUniformMatrix4fv(p2->u_mvp, 1, GL_FALSE, ident);
		qglUniform4f(p2->u_color, e->outline[0], e->outline[1], e->outline[2], alpha);
		qglUniform4f(p2->u_outline, width, 0, 0, 0);
		R_StreamDraw(GL_TRIANGLES, quad, 4, quadidx, 6);
	}
	qglBindFramebuffer(GL_FRAMEBUFFER, (GLuint)savedfb);
	qglScissor(box[0], box[1], box[2], box[3]);
	GL_BindTexture(0, GL_TEXTURE_2D, 0);
	GL_SetBlend(BLEND_OPAQUE, 0, 0);
	GL_SetDepth(true, true);
	GL_SetCull(true);
}

int R_NumModels(void) { return nummodels; }
rmodel_t *R_ModelAt(int i) { return i >= 0 && i < nummodels ? models[i] : NULL; }

/* roughly what a model holds in memory, and its vertex and triangle counts */
size_t R_ModelMemory(const rmodel_t *m, int *verts, int *tris)
{
	size_t bytes = sizeof(*m);
	*verts = *tris = 0;
	if (m->alias) {
		const alias_t *a = m->alias;
		bytes += (size_t)a->numposes * (size_t)a->numverts * 6 * sizeof(float) + (size_t)a->numoutverts * (2 * sizeof(float) + sizeof(int)) +
		         (size_t)a->numtris * 3 * sizeof(uint16_t) + (size_t)a->numframes * sizeof(aliasframe_t);
		*verts = a->numverts;
		*tris = a->numtris;
	}
	if (m->type == MOD_BRUSH || (!m->alias && !m->iqm && m->numsurfaces)) {
		for (int i = 0; i < m->numsurfaces; i++)
			*tris += r_world.surfaces[m->firstsurface + i].numindexes / 3;
		bytes += (size_t)*tris * 3 * sizeof(uint32_t) + (size_t)m->numsurfaces * sizeof(rsurface_t);
	}
	return bytes;
}
