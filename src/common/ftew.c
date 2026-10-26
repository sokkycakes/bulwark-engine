/* Bulwark engine - FTEW v1 compiled worlds. ISC licence, see LICENSE.
 * Layout and limits: docs/spec/ftew.md. */
#include "ftew.h"

#include <math.h>
#include <stdio.h>

/* limits from the format notes */
#define MAX_FILE (128u * 1024u * 1024u)
#define MAX_ENTITIES (4u * 1024u * 1024u)
#define MAX_MATERIALS 4096
#define MAX_MESHES 4096
#define MAX_MESH_VERTS 65535
#define MAX_INSTANCES 16384
#define MAX_COLLIDERS 16384
#define MAX_MODELS 1024
#define MAX_TRIANGLES 50000
#define MAX_FLOAT 1e8f

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static float f32(const uint8_t *p)
{
	uint32_t u = u32(p);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

static bool floats_ok(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float f = f32(p + i * 4);
		if (!isfinite(f) || fabsf(f) > MAX_FLOAT)
			return false;
	}
	return true;
}

static bool fail(char *err, size_t errsize, const char *why)
{
	if (err && errsize)
		snprintf(err, errsize, "FTEW: %s", why);
	return false;
}

bool FTEW_IsFTEW(const void *data, size_t size) { return size >= 4 && !memcmp(data, "FTEW", 4); }

/* game-relative printable path, no quotes, backslashes, colons, semicolons or ".." */
static bool material_path_ok(const uint8_t *p, uint32_t n)
{
	if (!n || n >= 64 || p[0] == '/')
		return false;
	for (uint32_t i = 0; i < n; i++) {
		uint8_t c = p[i];
		if (c < 33 || c > 126 || c == '"' || c == '\\' || c == ':' || c == ';')
			return false;
		if (c == '.' && i + 1 < n && p[i + 1] == '.' && (i == 0 || p[i - 1] == '/'))
			return false;
	}
	return true;
}

bool FTEW_Parse(const void *data, size_t size, ftew_t *w, char *err, size_t errsize)
{
	static const char tags[5][5] = {"MATL", "MESH", "INST", "COLL", "ENTS"};
	const uint8_t *b = data;
	const uint8_t *sec[5];
	uint32_t len[5], count[5], off = FTEW_HEADER_SIZE;
	size_t triangles = 0;
	const uint8_t *p, *end;

	memset(w, 0, sizeof(*w));
	if (size < FTEW_HEADER_SIZE || size > MAX_FILE || !FTEW_IsFTEW(b, size))
		return fail(err, errsize, "bad header or size");
	if (u32(b + 4) != 1)
		return fail(err, errsize, "unsupported version (expected 1)");
	if (u32(b + 8) != size)
		return fail(err, errsize, "file length does not match header");
	w->nummodels = u32(b + 12);
	if (!w->nummodels || w->nummodels > MAX_MODELS)
		return fail(err, errsize, "bad model count");
	/* five sections, in order, back to back from byte 96 to the end */
	for (int i = 0; i < 5; i++) {
		const uint8_t *d = b + 16 + i * 16;
		if (memcmp(d, tags[i], 4) || u32(d + 4) != off || u32(d + 8) > size - off)
			return fail(err, errsize, "bad section directory");
		sec[i] = b + off;
		len[i] = u32(d + 8);
		count[i] = u32(d + 12);
		off += len[i];
	}
	if (off != size)
		return fail(err, errsize, "sections do not fill the file");
	if (count[0] > MAX_MATERIALS || count[1] > MAX_MESHES || count[2] > MAX_INSTANCES || count[3] > MAX_COLLIDERS)
		return fail(err, errsize, "too many records");

	/* MATL: length-prefixed paths */
	w->nummaterials = (int)count[0];
	w->materials = Mem_Alloc(sizeof(*w->materials) * (count[0] + 1));
	p = sec[0];
	end = p + len[0];
	for (uint32_t i = 0; i < count[0]; i++) {
		uint32_t n;
		if (end - p < 4)
			goto truncated;
		n = u32(p);
		p += 4;
		if (n > (size_t)(end - p) || !material_path_ok(p, n)) {
			FTEW_Free(w);
			return fail(err, errsize, "bad material path");
		}
		memcpy(w->materials[i], p, n);
		w->materials[i][n] = 0;
		p += n;
	}
	if (p != end)
		goto mismatch;

	/* MESH: material, vertex count, index count, vertices, indices */
	w->nummeshes = (int)count[1];
	w->meshes = Mem_Alloc(sizeof(ftew_mesh_t) * (count[1] + 1));
	p = sec[1];
	end = p + len[1];
	for (uint32_t i = 0; i < count[1]; i++) {
		ftew_mesh_t *m = &w->meshes[i];
		if (end - p < 12)
			goto truncated;
		m->material = u32(p);
		m->numverts = u32(p + 4);
		m->numindexes = u32(p + 8);
		p += 12;
		if (m->material >= count[0] || m->numverts < 3 || m->numverts > MAX_MESH_VERTS || !m->numindexes ||
		    m->numindexes % 3 || m->numindexes > MAX_TRIANGLES * 3 ||
		    (size_t)m->numverts * FTEW_VERTEX_SIZE + (size_t)m->numindexes * 4 > (size_t)(end - p)) {
			FTEW_Free(w);
			return fail(err, errsize, "bad mesh");
		}
		m->verts = p;
		if (!floats_ok(p, (size_t)m->numverts * 9)) {
			FTEW_Free(w);
			return fail(err, errsize, "non-finite mesh vertex");
		}
		p += (size_t)m->numverts * FTEW_VERTEX_SIZE;
		m->indexes = p;
		for (uint32_t k = 0; k < m->numindexes; k++)
			if (u32(p + k * 4) >= m->numverts) {
				FTEW_Free(w);
				return fail(err, errsize, "mesh index out of range");
			}
		p += (size_t)m->numindexes * 4;
	}
	if (p != end)
		goto mismatch;

	/* INST: mesh, model, flags, three basis columns, translation */
	if (len[2] != (size_t)count[2] * FTEW_INSTANCE_SIZE)
		goto mismatch;
	w->numinstances = (int)count[2];
	w->instances = Mem_Alloc(sizeof(ftew_instance_t) * (count[2] + 1));
	for (uint32_t i = 0; i < count[2]; i++) {
		const uint8_t *r = sec[2] + (size_t)i * FTEW_INSTANCE_SIZE;
		ftew_instance_t *in = &w->instances[i];
		double m[9], det;
		in->mesh = u32(r);
		in->model = u32(r + 4);
		in->flags = u32(r + 8);
		if (in->mesh >= count[1] || in->model >= w->nummodels || !(in->flags & 3) || (in->flags & ~3u) ||
		    !floats_ok(r + 12, 12)) {
			FTEW_Free(w);
			return fail(err, errsize, "bad instance");
		}
		for (int c = 0; c < 3; c++) {
			double l;
			for (int k = 0; k < 3; k++) {
				in->axis[c][k] = f32(r + 12 + (c * 3 + k) * 4);
				m[c * 3 + k] = in->axis[c][k];
			}
			/* singular check on normalised columns, so small unit scales pass */
			l = sqrt(m[c * 3] * m[c * 3] + m[c * 3 + 1] * m[c * 3 + 1] + m[c * 3 + 2] * m[c * 3 + 2]);
			if (!isfinite(l) || l == 0) {
				FTEW_Free(w);
				return fail(err, errsize, "singular instance transform");
			}
			for (int k = 0; k < 3; k++)
				m[c * 3 + k] /= l;
		}
		det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[3] * (m[1] * m[8] - m[2] * m[7]) + m[6] * (m[1] * m[5] - m[2] * m[4]);
		if (!isfinite(det) || fabs(det) < 1e-6) {
			FTEW_Free(w);
			return fail(err, errsize, "singular instance transform");
		}
		for (int k = 0; k < 3; k++)
			in->origin[k] = f32(r + 48 + k * 4);
		triangles += w->meshes[in->mesh].numindexes / 3;
		if (triangles > MAX_TRIANGLES) {
			FTEW_Free(w);
			return fail(err, errsize, "more than 50000 triangles after instancing");
		}
	}

	/* COLL: model, contents, mins, maxs, plane count, planes */
	w->numcolliders = (int)count[3];
	w->colliders = Mem_Alloc(sizeof(ftew_collider_t) * (count[3] + 1));
	p = sec[3];
	end = p + len[3];
	for (uint32_t i = 0; i < count[3]; i++) {
		ftew_collider_t *c = &w->colliders[i];
		if (end - p < 36)
			goto truncated;
		c->model = u32(p);
		c->contents = u32(p + 4);
		c->numplanes = u32(p + 32);
		if (c->model >= w->nummodels || c->contents != 1 || c->numplanes < 4 || c->numplanes > 64 || !floats_ok(p + 8, 6) ||
		    (size_t)c->numplanes * 16 > (size_t)(end - p - 36)) {
			FTEW_Free(w);
			return fail(err, errsize, "bad convex collider");
		}
		for (int k = 0; k < 3; k++) {
			c->mins[k] = f32(p + 8 + k * 4);
			c->maxs[k] = f32(p + 20 + k * 4);
			if (c->mins[k] >= c->maxs[k]) {
				FTEW_Free(w);
				return fail(err, errsize, "empty collider bounds");
			}
		}
		p += 36;
		c->planes = p;
		if (!floats_ok(p, (size_t)c->numplanes * 4)) {
			FTEW_Free(w);
			return fail(err, errsize, "non-finite collider plane");
		}
		for (uint32_t k = 0; k < c->numplanes; k++) {
			float n[3], d;
			FTEW_Plane(c, k, n, &d);
			if (fabsf(DotProduct(n, n) - 1) > 0.002f) {
				FTEW_Free(w);
				return fail(err, errsize, "collider plane normal is not normalised");
			}
		}
		p += (size_t)c->numplanes * 16;
	}
	if (p != end)
		goto mismatch;

	/* ENTS: one NUL-terminated entity lump */
	if (!len[4] || len[4] > MAX_ENTITIES || count[4] != 1 || sec[4][len[4] - 1] != 0 || memchr(sec[4], 0, len[4] - 1)) {
		FTEW_Free(w);
		return fail(err, errsize, "bad entity section");
	}
	w->entities = (const char *)sec[4];
	if (err && errsize)
		err[0] = 0;
	return true;

truncated:
	FTEW_Free(w);
	return fail(err, errsize, "truncated section");
mismatch:
	FTEW_Free(w);
	return fail(err, errsize, "section length does not match its records");
}

void FTEW_Free(ftew_t *w)
{
	Mem_Free(w->materials);
	Mem_Free(w->meshes);
	Mem_Free(w->instances);
	Mem_Free(w->colliders);
	memset(w, 0, sizeof(*w));
}

void FTEW_Vertex(const ftew_mesh_t *m, uint32_t i, float xyz[3], float st[2], float rgba[4])
{
	const uint8_t *v = m->verts + (size_t)i * FTEW_VERTEX_SIZE;
	for (int k = 0; k < 3; k++)
		xyz[k] = f32(v + k * 4);
	if (st) {
		st[0] = f32(v + 12);
		st[1] = f32(v + 16);
	}
	if (rgba)
		for (int k = 0; k < 4; k++)
			rgba[k] = f32(v + 20 + k * 4);
}

uint32_t FTEW_Index(const ftew_mesh_t *m, uint32_t i) { return u32(m->indexes + (size_t)i * 4); }

void FTEW_Plane(const ftew_collider_t *c, uint32_t i, float normal[3], float *dist)
{
	const uint8_t *p = c->planes + (size_t)i * 16;
	for (int k = 0; k < 3; k++)
		normal[k] = f32(p + k * 4);
	*dist = f32(p + 12);
}

void FTEW_Transform(const ftew_instance_t *in, const float p[3], float out[3])
{
	float r[3];
	for (int k = 0; k < 3; k++)
		r[k] = in->axis[0][k] * p[0] + in->axis[1][k] * p[1] + in->axis[2][k] * p[2] + in->origin[k];
	VectorCopy(r, out);
}

bool FTEW_Mirrored(const ftew_instance_t *in)
{
	const float(*a)[3] = in->axis;
	float det = a[0][0] * (a[1][1] * a[2][2] - a[2][1] * a[1][2]) - a[1][0] * (a[0][1] * a[2][2] - a[2][1] * a[0][2]) +
	            a[2][0] * (a[0][1] * a[1][2] - a[1][1] * a[0][2]);
	return det < 0;
}
