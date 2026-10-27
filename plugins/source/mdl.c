/* Bulwark Source formats plugin - studio models (MDL versions 44 to 49).
 * The .mdl holds the textures, skins and body parts, the .vvd the vertices
 * and the .vtx the triangles. The model is drawn in its reference pose: the
 * VVD's positions are already in model space there, so no bones are needed
 * for a still model. Layouts: docs/spec/source-formats.md.
 * ISC licence, see LICENSE. */
#include "source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VVD_VERTEX 48 /* weights 16, position 12, normal 12, texcoord 8 */

typedef struct {
	const unsigned char *d;
	size_t len;
} blob_t;

static int in(const blob_t *b, size_t ofs, size_t n) { return ofs <= b->len && n <= b->len - ofs; }

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
static const char *str_at(const blob_t *b, size_t ofs)
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

void *MDL_Convert(const char *path, const void *data, size_t len, size_t *outlen)
{
	blob_t m = {data, len}, v = {0}, x = {0};
	unsigned char *vvd = NULL, *vtx = NULL, *verts = NULL;
	size_t vlen = 0, xlen = 0;
	int nverts = 0, version, nmeshes = 0;
	tris_t t = {0};
	outmesh_t *meshes = NULL;
	out_t o = {0};
	*outlen = 0;
	if (len < 240 || memcmp(m.d, "IDST", 4))
		return NULL;
	version = rds32(m.d + 4);
	if (version < 44 || version > 49)
		return NULL; /* GoldSrc's version 10 is the engine's own */
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
	if (!vvd || !vtx || !vvd_vertices(&v, &verts, &nverts) || xlen < 36 || rds32(x.d) != 7) {
		srcprint("mdl: %s needs its .vvd and .dx90.vtx files\n", path);
		goto done;
	}
	{
		unsigned nparts = rd32(m.d + 232), partofs = rd32(m.d + 236);
		unsigned xparts = rd32(x.d + 28), xpartofs = rd32(x.d + 32);
		unsigned nskinref = rd32(m.d + 220), skinofs = rd32(m.d + 228);
		meshes = api->alloc(sizeof(outmesh_t) * 256);
		for (unsigned b = 0; b < nparts && b < xparts && b < 64; b++) {
			size_t bp = partofs + (size_t)b * 16, xb = xpartofs + (size_t)b * 8;
			size_t mm, xm, xl;
			unsigned nm;
			if (!in(&m, bp, 16) || !in(&x, xb, 8) || !rd32(m.d + bp + 4) || !rd32(x.d + xb))
				continue;
			/* the body part's first model: the default body */
			mm = bp + rd32(m.d + bp + 12);
			xm = xb + rd32(x.d + xb + 4);
			if (!in(&m, mm, 148) || !in(&x, xm, 8) || !rd32(x.d + xm))
				continue;
			xl = xm + rd32(x.d + xm + 4); /* LOD 0 */
			if (!in(&x, xl, 12))
				continue;
			nm = rd32(m.d + mm + 72);
			for (unsigned k = 0; k < nm && k < rd32(x.d + xl) && nmeshes < 256; k++) {
				size_t me = mm + rd32(m.d + mm + 76) + (size_t)k * 116;
				size_t xme = xl + rd32(x.d + xl + 4) + (size_t)k * 9;
				unsigned base;
				int skinref, tex;
				if (!in(&m, me, 116))
					break;
				base = rd32(m.d + mm + 84) / VVD_VERTEX + rd32(m.d + me + 12);
				skinref = rds32(m.d + me);
				tex = skinref;
				if (skinref >= 0 && (unsigned)skinref < nskinref && in(&m, skinofs + (size_t)skinref * 2, 2))
					tex = (int)rd16(m.d + skinofs + (size_t)skinref * 2);
				meshes[nmeshes].first = t.num / 3;
				mesh_tris(&x, xme, base, (unsigned)nverts, &t);
				meshes[nmeshes].count = t.num / 3 - meshes[nmeshes].first;
				material_for(&m, tex, meshes[nmeshes].material, sizeof(meshes[nmeshes].material));
				if (meshes[nmeshes].count)
					nmeshes++;
			}
		}
	}
	if (!nmeshes) {
		srcprint("mdl: %s has no triangles in LOD 0\n", path);
		goto done;
	}
	{
		/* header, text, meshes, vertex arrays, triangles */
		size_t textofs = 124, meshofs, vaofs, posofs, tcofs, nrmofs, triofs, textlen = 1;
		unsigned *names = api->alloc(sizeof(unsigned) * (size_t)nmeshes);
		for (int i = 0; i < nmeshes; i++) {
			names[i] = (unsigned)textlen;
			textlen += strlen(meshes[i].material) + 1;
		}
		textlen = (textlen + 3) & ~(size_t)3;
		meshofs = textofs + textlen;
		vaofs = meshofs + (size_t)nmeshes * 24;
		posofs = vaofs + 3 * 20;
		tcofs = posofs + (size_t)nverts * 12;
		nrmofs = tcofs + (size_t)nverts * 8;
		triofs = nrmofs + (size_t)nverts * 12;
		put(&o, "INTERQUAKEMODEL", 16);
		put32(&o, 2);
		for (int i = 0; i < 26; i++)
			put32(&o, 0);
		set32(&o, 20, (unsigned)(triofs + (size_t)t.num * 4)); /* filesize */
		set32(&o, 28, (unsigned)textlen);
		set32(&o, 32, (unsigned)textofs);
		set32(&o, 36, (unsigned)nmeshes);
		set32(&o, 40, (unsigned)meshofs);
		set32(&o, 44, 3);
		set32(&o, 48, (unsigned)nverts);
		set32(&o, 52, (unsigned)vaofs);
		set32(&o, 56, (unsigned)(t.num / 3));
		set32(&o, 60, (unsigned)triofs);
		put(&o, "", 1);
		for (int i = 0; i < nmeshes; i++)
			put(&o, meshes[i].material, strlen(meshes[i].material) + 1);
		while (o.len < meshofs)
			put(&o, "", 1);
		for (int i = 0; i < nmeshes; i++) {
			put32(&o, names[i]);
			put32(&o, names[i]);
			put32(&o, 0);
			put32(&o, (unsigned)nverts);
			put32(&o, (unsigned)meshes[i].first);
			put32(&o, (unsigned)meshes[i].count);
		}
		/* position, texcoord, normal: as floats */
		{
			const unsigned arrays[3][4] = {{0, 7, 3, 0}, {1, 7, 2, 0}, {2, 7, 3, 0}};
			const size_t ofs[3] = {posofs, tcofs, nrmofs};
			for (int a = 0; a < 3; a++) {
				put32(&o, arrays[a][0]);
				put32(&o, 0);
				put32(&o, arrays[a][1]);
				put32(&o, arrays[a][2]);
				put32(&o, (unsigned)ofs[a]);
			}
		}
		for (int i = 0; i < nverts; i++)
			for (int k = 0; k < 3; k++)
				putf(&o, rdf(verts + (size_t)i * VVD_VERTEX + 16 + k * 4));
		for (int i = 0; i < nverts; i++)
			for (int k = 0; k < 2; k++)
				putf(&o, rdf(verts + (size_t)i * VVD_VERTEX + 40 + k * 4));
		for (int i = 0; i < nverts; i++)
			for (int k = 0; k < 3; k++)
				putf(&o, rdf(verts + (size_t)i * VVD_VERTEX + 28 + k * 4));
		for (int i = 0; i < t.num; i++)
			put32(&o, t.idx[i]);
		api->free(names);
	}
	*outlen = o.len;
done:
	api->free(vvd);
	api->free(vtx);
	api->free(verts);
	api->free(t.idx);
	api->free(meshes);
	if (!*outlen) {
		api->free(o.d);
		return NULL;
	}
	return o.d;
}
