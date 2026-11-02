/* Bulwark Source formats plugin - studio models (MDL versions 44 to 49).
 * The .mdl holds the textures, skins and body parts, the .vvd the vertices
 * and the .vtx the triangles. The model is drawn in its reference pose: the
 * VVD's positions are already in model space there, so no bones are needed
 * for a still model. Layouts: docs/spec/source-formats.md.
 * ISC licence, see LICENSE. */
#include "mdlskel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VVD_VERTEX 48 /* weights 16, position 12, normal 12, texcoord 8 */

typedef struct {
	unsigned char *d;
	size_t len, cap;
} out_t;

static void put(out_t *o, const void *p, size_t n)
{
	if (o->len + n > o->cap) {
		size_t cap = o->cap ? o->cap * 2 : 4096;
		unsigned char *nd;
		while (cap < o->len + n)
			cap *= 2;
		nd = api->alloc(cap);
		if (o->d)
			memcpy(nd, o->d, o->len);
		api->free(o->d);
		o->d = nd;
		o->cap = cap;
	}
	memcpy(o->d + o->len, p, n);
	o->len += n;
}

static void put32(out_t *o, unsigned v)
{
	unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
	put(o, b, 4);
}

static void putf(out_t *o, float f)
{
	union {
		float f;
		unsigned u;
	} c;
	c.f = f;
	put32(o, c.u);
}

static void set32(out_t *o, size_t ofs, unsigned v)
{
	o->d[ofs] = (unsigned char)v;
	o->d[ofs + 1] = (unsigned char)(v >> 8);
	o->d[ofs + 2] = (unsigned char)(v >> 16);
	o->d[ofs + 3] = (unsigned char)(v >> 24);
}

/* a NUL-terminated string at ofs, or "" */
const char *str_at(const blob_t *b, size_t ofs)
{
	if (!in(b, ofs, 1) || !memchr(b->d + ofs, 0, b->len - ofs))
		return "";
	return (const char *)b->d + ofs;
}

static void *load_sibling(const char *path, const char *ext, size_t *len)
{
	char p[300];
	size_t n = strlen(path);
	if (n > 4 && !strcasecmp(path + n - 4, ".mdl"))
		n -= 4;
	snprintf(p, sizeof(p), "%.*s%s", (int)n, path, ext);
	return api->load_file(p, len);
}

/* the LOD 0 vertices: the VVD's own list, or rebuilt from its fixup table */
static int vvd_vertices(const blob_t *v, unsigned char **verts, int *count)
{
	unsigned numfixups, fixupofs, dataofs, lod0;
	if (v->len < 64 || memcmp(v->d, "IDSV", 4) || rd32(v->d + 4) != 4)
		return 0;
	lod0 = rd32(v->d + 16);
	numfixups = rd32(v->d + 48);
	fixupofs = rd32(v->d + 52);
	dataofs = rd32(v->d + 56);
	if (lod0 > (1u << 20) || !in(v, dataofs, (size_t)lod0 * VVD_VERTEX))
		return 0;
	*verts = api->alloc((size_t)lod0 * VVD_VERTEX + 1);
	if (!numfixups) {
		memcpy(*verts, v->d + dataofs, (size_t)lod0 * VVD_VERTEX);
		*count = (int)lod0;
		return 1;
	}
	/* fixups: lod, first source vertex, count; those for LOD 0 and up are
	 * in LOD 0 */
	{
		unsigned n = 0;
		if (!in(v, fixupofs, (size_t)numfixups * 12)) {
			api->free(*verts);
			return 0;
		}
		for (unsigned i = 0; i < numfixups; i++) {
			const unsigned char *f = v->d + fixupofs + (size_t)i * 12;
			unsigned src = rd32(f + 4), num = rd32(f + 8);
			if (rds32(f) < 0)
				continue;
			if (n + num > lod0 || !in(v, dataofs + (size_t)src * VVD_VERTEX, (size_t)num * VVD_VERTEX))
				break;
			memcpy(*verts + (size_t)n * VVD_VERTEX, v->d + dataofs + (size_t)src * VVD_VERTEX, (size_t)num * VVD_VERTEX);
			n += num;
		}
		*count = (int)n;
	}
	return 1;
}

static const char *va_vmt(const char *path)
{
	static char buf[420];
	snprintf(buf, sizeof(buf), "%s.vmt", path);
	return buf;
}

/* the material for a texture: the first of the model's material folders
 * that has it, as a path without the extension */
static void material_for(const blob_t *m, int tex, char *out, size_t size)
{
	unsigned numtex = rd32(m->d + 204), texofs = rd32(m->d + 208);
	unsigned numdirs = rd32(m->d + 212), dirofs = rd32(m->d + 216);
	char name[192];
	const unsigned char *t;
	out[0] = 0;
	if (tex < 0 || (unsigned)tex >= numtex || !in(m, texofs + (size_t)tex * 64, 64))
		return;
	t = m->d + texofs + (size_t)tex * 64;
	snprintf(name, sizeof(name), "%s", str_at(m, texofs + (size_t)tex * 64 + (size_t)rds32(t)));
	src_cleanpath(name);
	for (unsigned i = 0; i < numdirs && i < 32; i++) {
		char dir[192], path[400];
		size_t n;
		if (!in(m, dirofs + (size_t)i * 4, 4))
			break;
		snprintf(dir, sizeof(dir), "%s", str_at(m, rd32(m->d + dirofs + (size_t)i * 4)));
		src_cleanpath(dir);
		n = strlen(dir);
		snprintf(path, sizeof(path), "materials/%s%s%s", dir, n && dir[n - 1] != '/' ? "/" : "", name);
		if (!out[0] || api->file_exists(va_vmt(path)))
			snprintf(out, size, "%s", path);
		if (api->file_exists(va_vmt(path)))
			return;
	}
	if (!out[0])
		snprintf(out, size, "materials/%s", name);
}

/* --- triangles from the VTX ------------------------------------------------------- */

typedef struct {
	unsigned *idx; /* global vertex numbers, three a triangle */
	int num, cap;
} tris_t;

static void add_tri(tris_t *t, unsigned a, unsigned b, unsigned c)
{
	if (t->num + 3 > t->cap) {
		int cap = t->cap ? t->cap * 2 : 1024;
		unsigned *n = api->alloc(sizeof(unsigned) * (size_t)cap);
		if (t->idx)
			memcpy(n, t->idx, sizeof(unsigned) * (size_t)t->num);
		api->free(t->idx);
		t->idx = n;
		t->cap = cap;
	}
	t->idx[t->num++] = a;
	t->idx[t->num++] = b;
	t->idx[t->num++] = c;
}

/* whether strip groups of this size fit the file (later games added two
 * fields, making them 33 bytes and strips 35) */
static int groups_fit(const blob_t *x, size_t sg, unsigned n, size_t gsize)
{
	for (unsigned g = 0; g < n; g++) {
		size_t o = sg + (size_t)g * gsize;
		if (!in(x, o, 25))
			return 0;
		{
			unsigned nv = rd32(x->d + o), vo = rd32(x->d + o + 4), ni = rd32(x->d + o + 8), io = rd32(x->d + o + 12);
			if (!in(x, o + vo, (size_t)nv * 9) || !in(x, o + io, (size_t)ni * 2) || nv > 65536)
				return 0;
		}
	}
	return 1;
}

/* one mesh's triangles; base is the global number of its first vertex */
static void mesh_tris(const blob_t *x, size_t mesh, unsigned base, unsigned nverts, tris_t *t)
{
	unsigned ngroups;
	size_t sg, gsize;
	if (!in(x, mesh, 9))
		return;
	ngroups = rd32(x->d + mesh);
	sg = mesh + rd32(x->d + mesh + 4);
	if (ngroups > 64)
		return;
	gsize = groups_fit(x, sg, ngroups, 25) ? 25 : groups_fit(x, sg, ngroups, 33) ? 33 : 0;
	if (!gsize)
		return;
	for (unsigned g = 0; g < ngroups; g++) {
		size_t o = sg + (size_t)g * gsize;
		unsigned nv = rd32(x->d + o), vo = rd32(x->d + o + 4), ni = rd32(x->d + o + 8), io = rd32(x->d + o + 12);
		unsigned nstrips = rd32(x->d + o + 16), so = rd32(x->d + o + 20);
		size_t ssize = gsize == 25 ? 27 : 35;
		const unsigned char *verts = x->d + o + vo, *idx = x->d + o + io;
#define VERT(i) (base + rd16(verts + (size_t)rd16(idx + (size_t)(i)*2) * 9 + 4))
		for (unsigned s = 0; s < nstrips && s < 4096; s++) {
			size_t st = o + so + (size_t)s * ssize;
			unsigned sni, sio, flags;
			if (!in(x, st, 19))
				break;
			sni = rd32(x->d + st);
			sio = rd32(x->d + st + 4);
			flags = x->d[st + 18];
			if ((size_t)sio + sni > ni)
				break;
			if (flags & 2) { /* a triangle strip */
				for (unsigned i = 0; i + 2 < sni; i++) {
					unsigned a = sio + i, b = sio + i + 1, c = sio + i + 2;
					for (unsigned k = 0; k < 3; k++)
						if (rd16(idx + (size_t)(sio + i + k) * 2) >= nv)
							goto next;
					if (i & 1)
						add_tri(t, VERT(a), VERT(c), VERT(b));
					else
						add_tri(t, VERT(a), VERT(b), VERT(c));
				next:;
				}
			} else {
				for (unsigned i = 0; i + 2 < sni; i += 3) {
					if (rd16(idx + (size_t)(sio + i) * 2) >= nv || rd16(idx + (size_t)(sio + i + 1) * 2) >= nv ||
					    rd16(idx + (size_t)(sio + i + 2) * 2) >= nv)
						continue;
					add_tri(t, VERT(sio + i), VERT(sio + i + 1), VERT(sio + i + 2));
				}
			}
		}
#undef VERT
	}
	/* drop triangles that point past the model's vertices */
	for (int i = 0; i < t->num;) {
		if (t->idx[i] >= nverts || t->idx[i + 1] >= nverts || t->idx[i + 2] >= nverts) {
			memmove(t->idx + i, t->idx + i + 3, sizeof(unsigned) * (size_t)(t->num - i - 3));
			t->num -= 3;
		} else
			i += 3;
	}
}


/* --- the IQM ----------------------------------------------------------------------- */

typedef struct {
	char material[400];
	int first, count; /* triangles */
} outmesh_t;

typedef struct {
	float pos[3], tc[2], nrm[3];
	unsigned char bi[4], bw[4];
} overt_t;

typedef struct {
	overt_t *v;
	int num;
	tris_t t;
	outmesh_t *meshes;
	int nummeshes;
} body_t;

/* the model's own mesh, posed by the skeleton: the VVD's bind-pose
 * positions go to bone space (pose-to-bone) and back out through the rest
 * pose built from the bones, so the two always agree */
static int mesh_body(const char *path, const blob_t *m, const sbone_t *b, int nb, float (*rest)[12], body_t *out)
{
	blob_t v = {0}, x = {0};
	unsigned char *vvd = NULL, *vtx = NULL, *verts = NULL;
	size_t vlen = 0, xlen = 0;
	int nverts = 0, ok = 0;
	float (*skin)[12] = NULL;
	vvd = load_sibling(path, ".vvd", &vlen);
	vtx = load_sibling(path, ".dx90.vtx", &xlen);
	if (!vtx)
		vtx = load_sibling(path, ".dx80.vtx", &xlen);
	if (!vtx)
		vtx = load_sibling(path, ".sw.vtx", &xlen);
	if (!vtx)
		vtx = load_sibling(path, ".vtx", &xlen);
	v.d = vvd, v.len = vlen;
	x.d = vtx, x.len = xlen;
	if (!vvd || !vtx || !vvd_vertices(&v, &verts, &nverts) || xlen < 36 || rds32(x.d) != 7)
		goto done;
	{
		unsigned nparts = rd32(m->d + 232), partofs = rd32(m->d + 236);
		unsigned xparts = rd32(x.d + 28), xpartofs = rd32(x.d + 32);
		unsigned nskinref = rd32(m->d + 220), skinofs = rd32(m->d + 228);
		out->meshes = api->alloc(sizeof(outmesh_t) * 256);
		for (unsigned bp_i = 0; bp_i < nparts && bp_i < xparts && bp_i < 64; bp_i++) {
			size_t bp = partofs + (size_t)bp_i * 16, xb = xpartofs + (size_t)bp_i * 8;
			size_t mm, xm, xl;
			unsigned nm;
			if (!in(m, bp, 16) || !in(&x, xb, 8) || !rd32(m->d + bp + 4) || !rd32(x.d + xb))
				continue;
			/* the body part's first model: the default body */
			mm = bp + rd32(m->d + bp + 12);
			xm = xb + rd32(x.d + xb + 4);
			if (!in(m, mm, 148) || !in(&x, xm, 8) || !rd32(x.d + xm))
				continue;
			xl = xm + rd32(x.d + xm + 4); /* LOD 0 */
			if (!in(&x, xl, 12))
				continue;
			nm = rd32(m->d + mm + 72);
			for (unsigned k = 0; k < nm && k < rd32(x.d + xl) && out->nummeshes < 256; k++) {
				size_t me = mm + rd32(m->d + mm + 76) + (size_t)k * 116;
				size_t xme = xl + rd32(x.d + xl + 4) + (size_t)k * 9;
				outmesh_t *om = &out->meshes[out->nummeshes];
				unsigned base;
				int skinref, tex;
				if (!in(m, me, 116))
					break;
				base = rd32(m->d + mm + 84) / VVD_VERTEX + rd32(m->d + me + 12);
				skinref = rds32(m->d + me);
				tex = skinref;
				if (skinref >= 0 && (unsigned)skinref < nskinref && in(m, skinofs + (size_t)skinref * 2, 2))
					tex = (int)rd16(m->d + skinofs + (size_t)skinref * 2);
				om->first = out->t.num / 3;
				mesh_tris(&x, xme, base, (unsigned)nverts, &out->t);
				om->count = out->t.num / 3 - om->first;
				material_for(m, tex, om->material, sizeof(om->material));
				if (om->count)
					out->nummeshes++;
			}
		}
	}
	if (!out->nummeshes)
		goto done;
	skin = api->alloc(sizeof(*skin) * (size_t)nb);
	for (int i = 0; i < nb; i++)
		mat_mul(skin[i], rest[i], b[i].posetobone);
	out->v = api->alloc(sizeof(overt_t) * (size_t)(nverts + 1));
	out->num = nverts;
	for (int i = 0; i < nverts; i++) {
		const unsigned char *s = verts + (size_t)i * VVD_VERTEX;
		overt_t *o = &out->v[i];
		float p[3], n[3], wsum = 0;
		int nw = s[15] > 3 ? 3 : s[15], wi[3];
		for (int k = 0; k < 3; k++)
			p[k] = rdf(s + 16 + k * 4), n[k] = rdf(s + 28 + k * 4);
		o->tc[0] = rdf(s + 40), o->tc[1] = rdf(s + 44);
		for (int k = 0; k < nw; k++) {
			float w = rdf(s + k * 4);
			int bone = s[12 + k];
			float tp[3], tn[3];
			if (bone >= nb || !(w > 0))
				continue;
			mat_point(skin[bone], p, tp);
			mat_dir(skin[bone], n, tn);
			for (int c = 0; c < 3; c++)
				o->pos[c] += tp[c] * w, o->nrm[c] += tn[c] * w;
			wi[k] = (int)(w * 255 + 0.5f);
			o->bi[k] = (unsigned char)bone;
			o->bw[k] = (unsigned char)(wi[k] > 255 ? 255 : wi[k]);
			wsum += w;
		}
		if (wsum > 0) {
			for (int c = 0; c < 3; c++)
				o->pos[c] /= wsum, o->nrm[c] /= wsum;
		} else {
			memcpy(o->pos, p, sizeof(p));
			memcpy(o->nrm, n, sizeof(n));
			o->bi[0] = 0, o->bw[0] = 255;
		}
		/* weights that add up to 255 */
		{
			int sum = o->bw[0] + o->bw[1] + o->bw[2], big = 0;
			for (int k = 1; k < 3; k++)
				if (o->bw[k] > o->bw[big])
					big = k;
			if (sum && sum != 255)
				o->bw[big] = (unsigned char)(o->bw[big] + 255 - sum);
		}
	}
	ok = 1;
done:
	api->free(vvd);
	api->free(vtx);
	api->free(verts);
	api->free(skin);
	if (!ok) {
		api->free(out->meshes);
		api->free(out->t.idx);
		memset(out, 0, sizeof(*out));
	}
	return ok;
}

/* one box per bone: hitboxes where the model has them, else a stick to the
 * bone's child. The colours come from the engine's "$bonepalette":
 * hit groups 0 to 7, then grey for bones without a hitbox */
static void box_body(const boxes_t *bx, float (*rest)[12], body_t *out)
{
	/* corners of each face, outward normal first */
	static const int faces[6][5] = {
	    {0, 0, 2, 6, 4}, {1, 1, 5, 7, 3}, {2, 0, 4, 5, 1}, {3, 2, 3, 7, 6}, {4, 0, 1, 3, 2}, {5, 4, 6, 7, 5}};
	static const float normals[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
	out->v = api->alloc(sizeof(overt_t) * (size_t)(bx->num * 24 + 1));
	out->meshes = api->alloc(sizeof(outmesh_t));
	for (int i = 0; i < bx->num; i++) {
		const sbox_t *s = &bx->b[i];
		float c[8][3];
		for (int k = 0; k < 8; k++) {
			float p[3] = {k & 1 ? s->maxs[0] : s->mins[0], k & 2 ? s->maxs[1] : s->mins[1], k & 4 ? s->maxs[2] : s->mins[2]};
			mat_point(rest[s->bone], p, c[k]);
		}
		for (int f = 0; f < 6; f++) {
			int base = out->num;
			float n[3], e1[3], e2[3], cr[3];
			mat_dir(rest[s->bone], normals[f], n);
			for (int k = 0; k < 4; k++) {
				overt_t *o = &out->v[out->num++];
				memcpy(o->pos, c[faces[f][1 + k]], sizeof(float) * 3);
				memcpy(o->nrm, n, sizeof(n));
				o->tc[0] = ((float)s->color + 0.5f) / 16.0f;
				o->tc[1] = 0.5f;
				o->bi[0] = (unsigned char)s->bone;
				o->bw[0] = 255;
			}
			/* IQM order runs against the normal (the loader turns it for GL) */
			for (int k = 0; k < 3; k++)
				e1[k] = out->v[base + 1].pos[k] - out->v[base].pos[k], e2[k] = out->v[base + 2].pos[k] - out->v[base].pos[k];
			cr[0] = e1[1] * e2[2] - e1[2] * e2[1];
			cr[1] = e1[2] * e2[0] - e1[0] * e2[2];
			cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
			if (cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2] > 0) {
				add_tri(&out->t, (unsigned)base, (unsigned)base + 2, (unsigned)base + 1);
				add_tri(&out->t, (unsigned)base, (unsigned)base + 3, (unsigned)base + 2);
			} else {
				add_tri(&out->t, (unsigned)base, (unsigned)base + 1, (unsigned)base + 2);
				add_tri(&out->t, (unsigned)base, (unsigned)base + 2, (unsigned)base + 3);
			}
		}
	}
	snprintf(out->meshes[0].material, sizeof(out->meshes[0].material), "$bonepalette");
	out->meshes[0].first = 0;
	out->meshes[0].count = out->t.num / 3;
	out->nummeshes = 1;
}

/* keeps each animation's quaternions on one side, so frames blend the
 * short way round */
static void quat_continuity(anims_t *a, const sbone_t *b, int nb)
{
	for (int i = 0; i < a->num; i++)
		for (int j = 0; j < nb; j++) {
			const float *prev = b[j].quat;
			for (int f = 0; f < a->a[i].count; f++) {
				float *q = a->frames + ((size_t)(a->a[i].first + f) * nb + j) * 10 + 3;
				if (q[0] * prev[0] + q[1] * prev[1] + q[2] * prev[2] + q[3] * prev[3] < 0)
					for (int k = 0; k < 4; k++)
						q[k] = -q[k];
				prev = q;
			}
		}
}

static size_t text_add(out_t *t, const char *s)
{
	size_t at = t->len;
	put(t, s, strlen(s) + 1);
	return at;
}

void *MDL_Convert(const char *path, const void *data, size_t len, size_t *outlen)
{
	blob_t m = {data, len};
	sbone_t *bones = NULL;
	float (*rest)[12] = NULL;
	int nb, version, boxes;
	body_t body = {0};
	boxes_t bx = {0};
	anims_t an = {0};
	out_t o = {0}, text = {0};
	*outlen = 0;
	if (len < 408 || memcmp(m.d, "IDST", 4))
		return NULL;
	version = rds32(m.d + 4);
	if (version < 44 || version > 49)
		return NULL; /* GoldSrc's version 10 is the engine's own */
	nb = mdl_bones(&m, &bones);
	if (!nb) {
		srcprint("mdl: %s has no bones\n", path);
		return NULL;
	}
	rest = api->alloc(sizeof(*rest) * (size_t)nb);
	mdl_restpose(bones, nb, rest);
	boxes = atoi(api->cvar_string("src_boneboxes"));
	if (boxes || !mesh_body(path, &m, bones, nb, rest, &body)) {
		if (!boxes)
			srcprint("mdl: %s has no .vvd/.dx90.vtx, drawing a box per bone\n", path);
		mdl_boxes(&m, bones, nb, &bx);
		box_body(&bx, rest, &body);
	}
	mdl_anims(path, &m, bones, nb, &an);
	if (an.unsupported)
		srcprint("mdl: %s: %d animations use a frame-based layout not read yet\n", path, an.unsupported);
	quat_continuity(&an, bones, nb);
	{
		size_t *mnames = api->alloc(sizeof(size_t) * (size_t)(body.nummeshes + 1));
		size_t *jnames = api->alloc(sizeof(size_t) * (size_t)nb);
		size_t *anames = api->alloc(sizeof(size_t) * (size_t)(an.num + 1));
		size_t meshofs, vaofs, posofs, tcofs, nrmofs, biofs, bwofs, triofs, jointofs, poseofs, animofs, frameofs, end;
		int nv = body.num, nchan = 0, jointcount = nb;
		unsigned *mask = api->alloc(sizeof(unsigned) * (size_t)nb);
		float *lo = api->alloc(sizeof(float) * 10 * (size_t)nb), *hi = api->alloc(sizeof(float) * 10 * (size_t)nb);
		put(&text, "", 1);
		for (int i = 0; i < body.nummeshes; i++)
			mnames[i] = text_add(&text, body.meshes[i].material);
		for (int j = 0; j < nb; j++)
			jnames[j] = text_add(&text, bones[j].name);
		for (int i = 0; i < an.num; i++)
			anames[i] = text_add(&text, an.a[i].name);
		while (text.len & 3)
			put(&text, "", 1);
		/* the channels each joint animates, and their ranges */
		for (int j = 0; j < nb; j++) {
			for (int c = 0; c < 10; c++) {
				float v0 = an.numframes ? an.frames[(size_t)j * 10 + c] : 0;
				lo[j * 10 + c] = hi[j * 10 + c] = v0;
			}
			for (int f = 0; f < an.numframes; f++)
				for (int c = 0; c < 10; c++) {
					float v = an.frames[((size_t)f * nb + j) * 10 + c];
					if (v < lo[j * 10 + c])
						lo[j * 10 + c] = v;
					if (v > hi[j * 10 + c])
						hi[j * 10 + c] = v;
				}
			for (int c = 0; c < 10; c++)
				if (an.numframes && hi[j * 10 + c] - lo[j * 10 + c] > 1e-6f) {
					mask[j] |= 1u << c;
					nchan++;
				}
		}
		/* a still model with one bone (a static prop) stays a plain mesh,
		 * which the engine draws without skinning it every frame */
		if (nb == 1 && !nchan) {
			jointcount = 0;
			an.num = an.numframes = 0;
		}
		meshofs = 124 + text.len;
		vaofs = meshofs + (size_t)body.nummeshes * 24;
		posofs = vaofs + (jointcount ? 5 : 3) * 20;
		tcofs = posofs + (size_t)nv * 12;
		nrmofs = tcofs + (size_t)nv * 8;
		biofs = nrmofs + (size_t)nv * 12;
		bwofs = biofs + (jointcount ? (size_t)nv * 4 : 0);
		triofs = bwofs + (jointcount ? (size_t)nv * 4 : 0);
		jointofs = triofs + (size_t)body.t.num * 4;
		poseofs = jointofs + (size_t)jointcount * 48;
		animofs = poseofs + (an.numframes ? (size_t)nb * 88 : 0);
		frameofs = animofs + (size_t)an.num * 20;
		end = frameofs + (size_t)an.numframes * (size_t)nchan * 2;
		put(&o, "INTERQUAKEMODEL", 16);
		put32(&o, 2);
		for (int i = 0; i < 26; i++)
			put32(&o, 0);
		set32(&o, 20, (unsigned)end); /* filesize */
		set32(&o, 28, (unsigned)text.len);
		set32(&o, 32, 124);
		set32(&o, 36, (unsigned)body.nummeshes);
		set32(&o, 40, (unsigned)meshofs);
		set32(&o, 44, jointcount ? 5 : 3);
		set32(&o, 48, (unsigned)nv);
		set32(&o, 52, (unsigned)vaofs);
		set32(&o, 56, (unsigned)(body.t.num / 3));
		set32(&o, 60, (unsigned)triofs);
		set32(&o, 68, (unsigned)jointcount);
		set32(&o, 72, (unsigned)jointofs);
		set32(&o, 76, an.numframes ? (unsigned)nb : 0);
		set32(&o, 80, (unsigned)poseofs);
		set32(&o, 84, (unsigned)an.num);
		set32(&o, 88, (unsigned)animofs);
		set32(&o, 92, (unsigned)an.numframes);
		set32(&o, 96, (unsigned)nchan);
		set32(&o, 100, (unsigned)frameofs);
		put(&o, text.d, text.len);
		for (int i = 0; i < body.nummeshes; i++) {
			put32(&o, (unsigned)mnames[i]);
			put32(&o, (unsigned)mnames[i]);
			put32(&o, 0);
			put32(&o, (unsigned)nv);
			put32(&o, (unsigned)body.meshes[i].first);
			put32(&o, (unsigned)body.meshes[i].count);
		}
		{
			/* type, flags, format, size, offset */
			const unsigned arrays[5][4] = {{0, 7, 3, 0}, {1, 7, 2, 0}, {2, 7, 3, 0}, {4, 1, 4, 0}, {5, 1, 4, 0}};
			const size_t ofs[5] = {posofs, tcofs, nrmofs, biofs, bwofs};
			for (int a = 0; a < (jointcount ? 5 : 3); a++) {
				put32(&o, arrays[a][0]);
				put32(&o, 0);
				put32(&o, arrays[a][1]);
				put32(&o, arrays[a][2]);
				put32(&o, (unsigned)ofs[a]);
			}
		}
		for (int i = 0; i < nv; i++)
			for (int k = 0; k < 3; k++)
				putf(&o, body.v[i].pos[k]);
		for (int i = 0; i < nv; i++)
			for (int k = 0; k < 2; k++)
				putf(&o, body.v[i].tc[k]);
		for (int i = 0; i < nv; i++)
			for (int k = 0; k < 3; k++)
				putf(&o, body.v[i].nrm[k]);
		for (int i = 0; jointcount && i < nv; i++)
			put(&o, body.v[i].bi, 4);
		for (int i = 0; jointcount && i < nv; i++)
			put(&o, body.v[i].bw, 4);
		for (int i = 0; i < body.t.num; i++)
			put32(&o, body.t.idx[i]);
		for (int j = 0; j < jointcount; j++) {
			put32(&o, (unsigned)jnames[j]);
			put32(&o, (unsigned)bones[j].parent);
			for (int k = 0; k < 3; k++)
				putf(&o, bones[j].pos[k]);
			for (int k = 0; k < 4; k++)
				putf(&o, bones[j].quat[k]);
			for (int k = 0; k < 3; k++)
				putf(&o, 1);
		}
		if (an.numframes)
			for (int j = 0; j < nb; j++) {
				put32(&o, (unsigned)bones[j].parent);
				put32(&o, mask[j]);
				for (int c = 0; c < 10; c++)
					putf(&o, lo[j * 10 + c]);
				for (int c = 0; c < 10; c++)
					putf(&o, (mask[j] >> c & 1) ? (hi[j * 10 + c] - lo[j * 10 + c]) / 65535.0f : 0);
			}
		for (int i = 0; i < an.num; i++) {
			put32(&o, (unsigned)anames[i]);
			put32(&o, (unsigned)an.a[i].first);
			put32(&o, (unsigned)an.a[i].count);
			putf(&o, an.a[i].fps);
			put32(&o, an.a[i].loop ? 1 : 0);
		}
		for (int f = 0; f < an.numframes; f++)
			for (int j = 0; j < nb; j++)
				for (int c = 0; c < 10; c++)
					if (mask[j] >> c & 1) {
						float range = hi[j * 10 + c] - lo[j * 10 + c];
						float v = (an.frames[((size_t)f * nb + j) * 10 + c] - lo[j * 10 + c]) / range * 65535.0f + 0.5f;
						unsigned u = v < 0 ? 0 : v > 65535 ? 65535 : (unsigned)v;
						unsigned char b2[2] = {(unsigned char)u, (unsigned char)(u >> 8)};
						put(&o, b2, 2);
					}
		api->free(mnames);
		api->free(jnames);
		api->free(anames);
		api->free(mask);
		api->free(lo);
		api->free(hi);
		if (o.len == end)
			*outlen = o.len;
		else
			srcprint("mdl: %s: internal size mismatch (%zu vs %zu)\n", path, o.len, end);
	}
	api->free(text.d);
	api->free(bones);
	api->free(rest);
	api->free(body.v);
	api->free(body.t.idx);
	api->free(body.meshes);
	api->free(bx.b);
	api->free(an.a);
	api->free(an.frames);
	if (!*outlen) {
		api->free(o.d);
		return NULL;
	}
	return o.d;
}
