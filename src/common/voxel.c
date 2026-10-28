/* Bulwark engine - mutable voxel worlds. See voxel.h and docs/spec/voxel.md.
 * ISC licence, see LICENSE. */
#include "voxel.h"
#include "common.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CONTENTS_SOLID_BIT 1u
#define TRACE_EPSILON 0.03125f

/* --- life cycle ------------------------------------------------------------- */

static bool size_ok(const int size[3], float blocksize)
{
	uint64_t cells = 1;
	for (int i = 0; i < 3; i++) {
		if (size[i] < 1 || size[i] > VX_MAX_EDGE)
			return false;
		cells *= (uint64_t)size[i];
	}
	return cells <= VX_MAX_CELLS && blocksize >= 1 && blocksize <= 1024;
}

vxworld_t *Vox_Create(const int size[3], float blocksize, const float origin[3])
{
	vxworld_t *w;
	if (!size_ok(size, blocksize))
		return NULL;
	w = Mem_Alloc(sizeof(*w));
	for (int i = 0; i < 3; i++) {
		w->size[i] = size[i];
		w->origin[i] = origin ? origin[i] : 0;
		w->nchunks[i] = (size[i] + VX_CHUNK - 1) / VX_CHUNK;
	}
	w->blocksize = blocksize;
	w->chunks = Mem_Alloc(sizeof(vxchunk_t *) * (size_t)w->nchunks[0] * w->nchunks[1] * w->nchunks[2]);
	w->defs[0].defined = true;
	w->defs[0].shape = VXS_EMPTY;
	Str_Copy(w->defs[0].name, "EMPTY", sizeof(w->defs[0].name));
	return w;
}

static int numchunks(const vxworld_t *w) { return w->nchunks[0] * w->nchunks[1] * w->nchunks[2]; }

void Vox_Clear(vxworld_t *w)
{
	if (!w)
		return;
	for (int i = 0; i < numchunks(w); i++) {
		Mem_Free(w->chunks[i]);
		w->chunks[i] = NULL;
	}
	w->count = 0;
	w->revision++;
	w->anydirty = true;
}

void Vox_Free(vxworld_t *w)
{
	if (!w)
		return;
	Vox_Clear(w);
	Mem_Free(w->chunks);
	Mem_Free(w);
}

bool Vox_Define(vxworld_t *w, int type, const vxdef_t *def)
{
	if (!w || type <= 0 || type >= VX_MAX_TYPES || !def || def->shape >= VXS_COUNT)
		return false;
	w->defs[type] = *def;
	w->defs[type].defined = true;
	w->defs[type].name[sizeof(w->defs[type].name) - 1] = 0;
	w->defs[type].texture[sizeof(w->defs[type].texture) - 1] = 0;
	/* every chunk may now look different */
	for (int i = 0; i < numchunks(w); i++)
		if (w->chunks[i])
			w->chunks[i]->dirty = true;
	w->anydirty = true;
	w->revision++;
	return true;
}

int Vox_TypeForName(const vxworld_t *w, const char *name)
{
	if (!w || !name)
		return -1;
	for (int i = 0; i < VX_MAX_TYPES; i++)
		if (w->defs[i].defined && !Str_ICmp(w->defs[i].name, name))
			return i;
	return -1;
}

/* --- cells ------------------------------------------------------------------ */

bool Vox_InBounds(const vxworld_t *w, int x, int y, int z)
{
	return w && x >= 0 && y >= 0 && z >= 0 && x < w->size[0] && y < w->size[1] && z < w->size[2];
}

static vxchunk_t **chunk_slot(const vxworld_t *w, int cx, int cy, int cz)
{
	return &w->chunks[(cz * w->nchunks[1] + cy) * w->nchunks[0] + cx];
}

static int cell_index(int x, int y, int z)
{
	return ((z & (VX_CHUNK - 1)) * VX_CHUNK + (y & (VX_CHUNK - 1))) * VX_CHUNK + (x & (VX_CHUNK - 1));
}

int Vox_Get(const vxworld_t *w, int x, int y, int z, int *rot)
{
	vxchunk_t *c;
	int i;
	if (rot)
		*rot = 0;
	if (!Vox_InBounds(w, x, y, z))
		return 0;
	c = *chunk_slot(w, x / VX_CHUNK, y / VX_CHUNK, z / VX_CHUNK);
	if (!c)
		return 0;
	i = cell_index(x, y, z);
	if (rot)
		*rot = c->rot[i];
	return c->type[i];
}

static void mark_neighbours_dirty(vxworld_t *w, int x, int y, int z)
{
	/* a face on a chunk edge depends on the cell across it */
	static const int d[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	for (int k = 0; k < 6; k++) {
		int nx = x + d[k][0], ny = y + d[k][1], nz = z + d[k][2];
		vxchunk_t *c;
		if (!Vox_InBounds(w, nx, ny, nz))
			continue;
		c = *chunk_slot(w, nx / VX_CHUNK, ny / VX_CHUNK, nz / VX_CHUNK);
		if (c)
			c->dirty = true;
	}
}

bool Vox_Set(vxworld_t *w, int x, int y, int z, int type, int rot)
{
	vxchunk_t **slot, *c;
	int i;
	if (!Vox_InBounds(w, x, y, z) || type < 0 || type >= VX_MAX_TYPES || !w->defs[type].defined)
		return false;
	rot &= 3;
	slot = chunk_slot(w, x / VX_CHUNK, y / VX_CHUNK, z / VX_CHUNK);
	c = *slot;
	if (!c) {
		if (!type)
			return true;
		c = *slot = Mem_Alloc(sizeof(vxchunk_t));
	}
	i = cell_index(x, y, z);
	if (c->type[i] == type && c->rot[i] == (type ? rot : 0))
		return true;
	if (c->type[i] && !type) {
		c->count--;
		w->count--;
	} else if (!c->type[i] && type) {
		c->count++;
		w->count++;
	}
	c->type[i] = (uint16_t)type;
	c->rot[i] = (uint8_t)(type ? rot : 0);
	c->dirty = true;
	w->anydirty = true;
	w->revision++;
	mark_neighbours_dirty(w, x, y, z);
	if (!c->count) {
		/* keep memory proportional to what is built; the renderer notices
		 * the missing chunk through anydirty */
		Mem_Free(c);
		*slot = NULL;
	}
	return true;
}

void Vox_CellForPoint(const vxworld_t *w, const float p[3], int out[3])
{
	for (int i = 0; i < 3; i++)
		out[i] = (int)floorf((p[i] - w->origin[i]) / w->blocksize);
}

void Vox_CellMins(const vxworld_t *w, int x, int y, int z, float out[3])
{
	out[0] = w->origin[0] + x * w->blocksize;
	out[1] = w->origin[1] + y * w->blocksize;
	out[2] = w->origin[2] + z * w->blocksize;
}

/* --- shapes ----------------------------------------------------------------- */

int Vox_ShapePlanes(int shape, int rot, float planes[][4])
{
	static const float box[6][4] = {{1, 0, 0, 1}, {-1, 0, 0, 0}, {0, 1, 0, 1}, {0, -1, 0, 0}, {0, 0, 1, 1}, {0, 0, -1, 0}};
	const float r2 = 0.70710678f;
	int n = 6;
	if (shape == VXS_EMPTY || shape >= VXS_COUNT)
		return 0;
	memcpy(planes, box, sizeof(box));
	switch (shape) {
	case VXS_SLOPE: /* z - x <= 0 */
		planes[n][0] = -r2, planes[n][1] = 0, planes[n][2] = r2, planes[n][3] = 0;
		n++;
		break;
	case VXS_RSLOPE: /* -x - z <= -1 */
		planes[n][0] = -r2, planes[n][1] = 0, planes[n][2] = -r2, planes[n][3] = -r2;
		n++;
		break;
	case VXS_SLAB_LOW:
		planes[4][3] = 0.5f;
		break;
	case VXS_SLAB_HIGH:
		planes[5][3] = -0.5f;
		break;
	case VXS_THIN:
		planes[1][3] = -0.875f;
		break;
	default:
		break;
	}
	/* turn about the cell's vertical centre line: (x, y) -> (1 - y, x) per
	 * step, which turns normals by +90 degrees about +Z */
	for (int k = 0; k < (rot & 3); k++) {
		for (int p = 0; p < n; p++) {
			float nx = planes[p][0], ny = planes[p][1];
			float cdot_old = nx * 0.5f + ny * 0.5f;
			float rx = -ny, ry = nx;
			float cdot_new = rx * 0.5f + ry * 0.5f;
			planes[p][0] = rx;
			planes[p][1] = ry;
			planes[p][3] += cdot_new - cdot_old;
		}
	}
	return n;
}

/* --- collision ------------------------------------------------------------- */

typedef struct {
	float enter;      /* best entry fraction so far */
	float normal[3];
	float dist;
	bool startsolid, allsolid;
	int cell[3];
	int type;
	uint32_t contents;
	bool hit;
} cliptrace_t;

/* clips the swept box against one cell's convex shape (planes in world space) */
static void clip_cell(const float planes[][4], int n, const float *start, const float *mins, const float *maxs,
                      const float *end, cliptrace_t *ct, const int cell[3], int type, uint32_t contents)
{
	float enter = -1, leave = 1, hitn[3] = {0, 0, 0}, hitd = 0;
	bool startout = false, endout = false;
	for (int p = 0; p < n; p++) {
		const float *pl = planes[p];
		float ofs[3], dist, d1, d2;
		for (int i = 0; i < 3; i++)
			ofs[i] = pl[i] < 0 ? maxs[i] : mins[i];
		dist = pl[3] - (pl[0] * ofs[0] + pl[1] * ofs[1] + pl[2] * ofs[2]);
		d1 = pl[0] * start[0] + pl[1] * start[1] + pl[2] * start[2] - dist;
		d2 = pl[0] * end[0] + pl[1] * end[1] + pl[2] * end[2] - dist;
		if (d1 > 0)
			startout = true;
		if (d2 > 0)
			endout = true;
		/* entirely in front of this plane: no contact */
		if (d1 > 0 && (d2 >= TRACE_EPSILON || d2 >= d1))
			return;
		if (d1 <= 0 && d2 <= 0)
			continue;
		if (d1 > d2) {
			float f = (d1 - TRACE_EPSILON) / (d1 - d2);
			if (f > enter) {
				enter = f;
				hitn[0] = pl[0], hitn[1] = pl[1], hitn[2] = pl[2];
				hitd = dist;
			}
		} else {
			float f = (d1 + TRACE_EPSILON) / (d1 - d2);
			if (f < leave)
				leave = f;
		}
	}
	if (!startout) {
		if (!ct->startsolid) {
			ct->startsolid = true;
			ct->cell[0] = cell[0], ct->cell[1] = cell[1], ct->cell[2] = cell[2];
			ct->type = type;
			ct->contents = contents;
		}
		if (!endout)
			ct->allsolid = true;
		ct->enter = 0;
		ct->hit = true;
		return;
	}
	if (enter < leave && enter > -1 && enter < ct->enter) {
		ct->enter = enter < 0 ? 0 : enter;
		ct->normal[0] = hitn[0], ct->normal[1] = hitn[1], ct->normal[2] = hitn[2];
		ct->dist = hitd;
		ct->cell[0] = cell[0], ct->cell[1] = cell[1], ct->cell[2] = cell[2];
		ct->type = type;
		ct->contents = contents;
		ct->hit = true;
	}
}

static int cell_planes_world(const vxworld_t *w, int x, int y, int z, int shape, int rot, float planes[8][4])
{
	float m[3];
	int n = Vox_ShapePlanes(shape, rot, planes);
	Vox_CellMins(w, x, y, z, m);
	for (int p = 0; p < n; p++)
		planes[p][3] = planes[p][3] * w->blocksize + planes[p][0] * m[0] + planes[p][1] * m[1] + planes[p][2] * m[2];
	return n;
}

static void cell_range(const vxworld_t *w, const float lo[3], const float hi[3], int a[3], int b[3])
{
	for (int i = 0; i < 3; i++) {
		a[i] = (int)floorf((lo[i] - w->origin[i]) / w->blocksize);
		b[i] = (int)floorf((hi[i] - w->origin[i]) / w->blocksize);
		if (a[i] < 0)
			a[i] = 0;
		if (b[i] > w->size[i] - 1)
			b[i] = w->size[i] - 1;
	}
}

static void trace_segment(const vxworld_t *w, const float *s, const float *mins, const float *maxs, const float *e,
                          uint32_t mask, cliptrace_t *ct)
{
	float lo[3], hi[3], planes[8][4];
	int a[3], b[3];
	for (int i = 0; i < 3; i++) {
		lo[i] = BW_MIN(s[i], e[i]) + mins[i] - 1;
		hi[i] = BW_MAX(s[i], e[i]) + maxs[i] + 1;
	}
	cell_range(w, lo, hi, a, b);
	for (int z = a[2]; z <= b[2]; z++)
		for (int y = a[1]; y <= b[1]; y++)
			for (int x = a[0]; x <= b[0]; x++) {
				int rot, type = Vox_Get(w, x, y, z, &rot), n;
				const vxdef_t *d;
				int cell[3] = {x, y, z};
				if (!type)
					continue;
				d = &w->defs[type];
				if (d->shape == VXS_EMPTY || !(d->contents & mask))
					continue;
				n = cell_planes_world(w, x, y, z, d->shape, rot, planes);
				clip_cell(planes, n, s, mins, maxs, e, ct, cell, type, d->contents);
			}
}

void Vox_Trace(const vxworld_t *w, const float start[3], const float mins[3], const float maxs[3],
               const float end[3], uint32_t mask, vxtrace_t *tr)
{
	float zero[3] = {0, 0, 0}, len = 0, step;
	int nseg = 1;
	memset(tr, 0, sizeof(*tr));
	tr->fraction = 1;
	if (!mins)
		mins = zero;
	if (!maxs)
		maxs = zero;
	for (int i = 0; i < 3; i++)
		len += (end[i] - start[i]) * (end[i] - start[i]);
	len = sqrtf(len);
	/* long sweeps go in pieces so each piece only visits nearby cells, and
	 * the first piece with a hit holds the nearest one */
	step = 2 * w->blocksize;
	if (len > step)
		nseg = (int)ceilf(len / step);
	if (nseg > 100000)
		nseg = 100000;
	for (int k = 0; k < nseg; k++) {
		float s[3], e[3];
		cliptrace_t ct;
		memset(&ct, 0, sizeof(ct));
		ct.enter = 1;
		for (int i = 0; i < 3; i++) {
			s[i] = start[i] + (end[i] - start[i]) * k / nseg;
			e[i] = start[i] + (end[i] - start[i]) * (k + 1) / nseg;
		}
		trace_segment(w, s, mins, maxs, e, mask, &ct);
		if (!ct.hit)
			continue;
		if (ct.startsolid) {
			tr->startsolid = true;
			tr->allsolid = ct.allsolid && nseg == 1;
			tr->fraction = (float)k / nseg;
		} else {
			tr->fraction = (k + ct.enter) / nseg;
		}
		memcpy(tr->normal, ct.normal, sizeof(tr->normal));
		tr->dist = ct.dist;
		memcpy(tr->cell, ct.cell, sizeof(tr->cell));
		tr->type = ct.type;
		tr->contents = (int)ct.contents;
		break;
	}
	for (int i = 0; i < 3; i++)
		tr->endpos[i] = start[i] + (end[i] - start[i]) * tr->fraction;
}

static bool point_in_cell(const vxworld_t *w, int x, int y, int z, int shape, int rot, const float p[3])
{
	float planes[8][4];
	int n = cell_planes_world(w, x, y, z, shape, rot, planes);
	for (int k = 0; k < n; k++)
		if (planes[k][0] * p[0] + planes[k][1] * p[1] + planes[k][2] * p[2] > planes[k][3])
			return false;
	return n > 0;
}

uint32_t Vox_PointContents(const vxworld_t *w, const float p[3])
{
	int c[3], rot, type;
	if (!w)
		return 0;
	Vox_CellForPoint(w, p, c);
	type = Vox_Get(w, c[0], c[1], c[2], &rot);
	if (!type || !point_in_cell(w, c[0], c[1], c[2], w->defs[type].shape, rot, p))
		return 0;
	return w->defs[type].contents;
}

bool Vox_BoxTouches(const vxworld_t *w, const float origin[3], const float mins[3], const float maxs[3],
                    uint32_t mask)
{
	vxtrace_t tr;
	if (!w)
		return false;
	Vox_Trace(w, origin, mins, maxs, origin, mask, &tr);
	return tr.startsolid;
}

/* --- serialisation ------------------------------------------------------- */

uint32_t Vox_CRC32(const void *data, size_t len)
{
	/* reflected CRC-32 (polynomial 0xEDB88320), as used by zip and PNG */
	static uint32_t table[256];
	static bool init;
	const uint8_t *p = data;
	uint32_t crc = 0xFFFFFFFFu;
	if (!init) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		init = true;
	}
	while (len--)
		crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

typedef struct {
	uint8_t *data;
	size_t len, cap;
	bool fixed, overflow;
} wbuf_t;

static void wb_put(wbuf_t *b, const void *d, size_t n)
{
	if (b->len + n > b->cap) {
		if (b->fixed) {
			b->overflow = true;
			return;
		}
		while (b->len + n > b->cap)
			b->cap = b->cap ? b->cap * 2 : 4096;
		b->data = Mem_Realloc(b->data, b->cap);
	}
	memcpy(b->data + b->len, d, n);
	b->len += n;
}

static void wb_u8(wbuf_t *b, unsigned v)
{
	uint8_t c = (uint8_t)v;
	wb_put(b, &c, 1);
}

static void wb_u16(wbuf_t *b, unsigned v)
{
	uint8_t c[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
	wb_put(b, c, 2);
}

static void wb_u32(wbuf_t *b, uint32_t v)
{
	uint8_t c[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
	wb_put(b, c, 4);
}

static void wb_f32(wbuf_t *b, float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	wb_u32(b, u);
}

static void wb_var(wbuf_t *b, uint32_t v)
{
	while (v >= 0x80) {
		wb_u8(b, (v & 0x7F) | 0x80);
		v >>= 7;
	}
	wb_u8(b, v);
}

typedef struct {
	const uint8_t *data;
	size_t len, pos;
	bool bad;
} rbuf_t;

static bool rb_get(rbuf_t *r, void *out, size_t n)
{
	if (r->bad || r->pos + n > r->len) {
		r->bad = true;
		memset(out, 0, n);
		return false;
	}
	memcpy(out, r->data + r->pos, n);
	r->pos += n;
	return true;
}

static unsigned rb_u8(rbuf_t *r)
{
	uint8_t c;
	rb_get(r, &c, 1);
	return c;
}

static unsigned rb_u16(rbuf_t *r)
{
	uint8_t c[2];
	rb_get(r, c, 2);
	return c[0] | (c[1] << 8);
}

static uint32_t rb_u32(rbuf_t *r)
{
	uint8_t c[4];
	rb_get(r, c, 4);
	return c[0] | (c[1] << 8) | (c[2] << 16) | ((uint32_t)c[3] << 24);
}

static float rb_f32(rbuf_t *r)
{
	uint32_t u = rb_u32(r);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

static uint32_t rb_var(rbuf_t *r)
{
	uint32_t v = 0;
	for (int shift = 0; shift < 35; shift += 7) {
		unsigned c = rb_u8(r);
		v |= (uint32_t)(c & 0x7F) << shift;
		if (!(c & 0x80))
			return v;
	}
	r->bad = true;
	return 0;
}

static void encode_chunk(const vxchunk_t *c, wbuf_t *b)
{
	int i = 0;
	if (!c) {
		wb_var(b, VX_CHUNKCELLS);
		wb_u16(b, 0);
		wb_u8(b, 0);
		return;
	}
	while (i < VX_CHUNKCELLS) {
		int j = i + 1;
		while (j < VX_CHUNKCELLS && c->type[j] == c->type[i] && c->rot[j] == c->rot[i])
			j++;
		wb_var(b, (uint32_t)(j - i));
		wb_u16(b, c->type[i]);
		wb_u8(b, c->rot[i]);
		i = j;
	}
}

size_t Vox_EncodeChunk(const vxworld_t *w, int cx, int cy, int cz, uint8_t *out, size_t outsize)
{
	wbuf_t b = {out, 0, outsize, true, false};
	if (!w || cx < 0 || cy < 0 || cz < 0 || cx >= w->nchunks[0] || cy >= w->nchunks[1] || cz >= w->nchunks[2])
		return 0;
	encode_chunk(*chunk_slot(w, cx, cy, cz), &b);
	return b.overflow ? 0 : b.len;
}

/* remap: file type id -> world type id (NULL: ids are used as they are) */
static bool decode_into(vxworld_t *w, int cx, int cy, int cz, rbuf_t *r, const int *remap, int nremap, int *unknown)
{
	static uint16_t types[VX_CHUNKCELLS];
	static uint8_t rots[VX_CHUNKCELLS];
	int i = 0, count = 0;
	vxchunk_t **slot;
	while (i < VX_CHUNKCELLS) {
		uint32_t run = rb_var(r);
		unsigned type = rb_u16(r), rot = rb_u8(r);
		if (r->bad || run == 0 || run > (uint32_t)(VX_CHUNKCELLS - i) || rot > 3)
			return false;
		if (remap) {
			if (type >= (unsigned)nremap)
				return false;
			if (remap[type] < 0) {
				if (unknown)
					*unknown += (int)run;
				type = 0;
			} else {
				type = (unsigned)remap[type];
			}
		} else if (type >= VX_MAX_TYPES || !w->defs[type].defined) {
			return false;
		}
		for (uint32_t k = 0; k < run; k++, i++) {
			types[i] = (uint16_t)type;
			rots[i] = (uint8_t)(type ? rot : 0);
		}
		if (type)
			count += (int)run;
	}
	/* cells outside the world (the far side of an edge chunk) stay empty */
	for (int z = 0; z < VX_CHUNK; z++)
		for (int y = 0; y < VX_CHUNK; y++)
			for (int x = 0; x < VX_CHUNK; x++) {
				int idx = (z * VX_CHUNK + y) * VX_CHUNK + x;
				if (!Vox_InBounds(w, cx * VX_CHUNK + x, cy * VX_CHUNK + y, cz * VX_CHUNK + z) && types[idx]) {
					types[idx] = 0;
					rots[idx] = 0;
					count--;
				}
			}
	slot = chunk_slot(w, cx, cy, cz);
	if (*slot) {
		w->count -= (*slot)->count;
		if (!count) {
			Mem_Free(*slot);
			*slot = NULL;
		}
	} else if (count) {
		*slot = Mem_Alloc(sizeof(vxchunk_t));
	}
	if (*slot) {
		memcpy((*slot)->type, types, sizeof(types));
		memcpy((*slot)->rot, rots, sizeof(rots));
		(*slot)->count = count;
		(*slot)->dirty = true;
	}
	w->count += count;
	w->revision++;
	w->anydirty = true;
	/* neighbouring chunks share faces with this one */
	for (int d = 0; d < 6; d++) {
		int n[3] = {cx, cy, cz};
		n[d / 2] += (d & 1) ? -1 : 1;
		if (n[0] < 0 || n[1] < 0 || n[2] < 0 || n[0] >= w->nchunks[0] || n[1] >= w->nchunks[1] ||
		    n[2] >= w->nchunks[2])
			continue;
		if (*chunk_slot(w, n[0], n[1], n[2]))
			(*chunk_slot(w, n[0], n[1], n[2]))->dirty = true;
	}
	return true;
}

bool Vox_DecodeChunk(vxworld_t *w, int cx, int cy, int cz, const uint8_t *in, size_t len)
{
	rbuf_t r = {in, len, 0, false};
	if (!w || cx < 0 || cy < 0 || cz < 0 || cx >= w->nchunks[0] || cy >= w->nchunks[1] || cz >= w->nchunks[2])
		return false;
	return decode_into(w, cx, cy, cz, &r, NULL, 0, NULL) && r.pos == len;
}

bool Vox_SaveToMemory(const vxworld_t *w, const uint8_t *meta, size_t metalen, uint8_t **out, size_t *outlen)
{
	wbuf_t b = {0};
	int ntypes = 0, nonempty = 0;
	if (!w || !out || !outlen)
		return false;
	wb_put(&b, "BWVX", 4);
	wb_u32(&b, VX_FILE_VERSION);
	for (int i = 0; i < 3; i++)
		wb_u32(&b, (uint32_t)w->size[i]);
	wb_f32(&b, w->blocksize);
	for (int i = 0; i < 3; i++)
		wb_f32(&b, w->origin[i]);
	for (int i = 1; i < VX_MAX_TYPES; i++)
		if (w->defs[i].defined)
			ntypes++;
	wb_u32(&b, (uint32_t)ntypes);
	for (int i = 1; i < VX_MAX_TYPES; i++) {
		size_t n;
		if (!w->defs[i].defined)
			continue;
		n = strlen(w->defs[i].name);
		wb_u16(&b, (unsigned)i);
		wb_u8(&b, (unsigned)n);
		wb_put(&b, w->defs[i].name, n);
	}
	for (int i = 0; i < numchunks(w); i++)
		if (w->chunks[i])
			nonempty++;
	wb_u32(&b, (uint32_t)nonempty);
	for (int cz = 0; cz < w->nchunks[2]; cz++)
		for (int cy = 0; cy < w->nchunks[1]; cy++)
			for (int cx = 0; cx < w->nchunks[0]; cx++) {
				const vxchunk_t *c = *chunk_slot(w, cx, cy, cz);
				size_t lenpos;
				uint32_t clen;
				if (!c)
					continue;
				wb_u16(&b, (unsigned)cx);
				wb_u16(&b, (unsigned)cy);
				wb_u16(&b, (unsigned)cz);
				lenpos = b.len;
				wb_u32(&b, 0);
				encode_chunk(c, &b);
				clen = (uint32_t)(b.len - lenpos - 4);
				b.data[lenpos] = (uint8_t)clen;
				b.data[lenpos + 1] = (uint8_t)(clen >> 8);
				b.data[lenpos + 2] = (uint8_t)(clen >> 16);
				b.data[lenpos + 3] = (uint8_t)(clen >> 24);
			}
	wb_u32(&b, (uint32_t)metalen);
	if (metalen)
		wb_put(&b, meta, metalen);
	wb_u32(&b, Vox_CRC32(b.data, b.len));
	*out = b.data;
	*outlen = b.len;
	return true;
}

#define FAIL(...)                                                                                                      \
	do {                                                                                                               \
		if (err)                                                                                                       \
			snprintf(err, errsize, __VA_ARGS__);                                                                       \
		goto fail;                                                                                                     \
	} while (0)

vxworld_t *Vox_LoadFromMemory(const uint8_t *data, size_t len, const vxworld_t *defs, uint8_t **meta,
                              size_t *metalen, int *unknown, char *err, size_t errsize)
{
	rbuf_t r = {data, 0, 0, false};
	vxworld_t *w = NULL;
	int size[3], *remap = NULL;
	float bs, org[3];
	uint32_t version, ntypes, nchunks, mlen, crc;
	bool *seen = NULL;
	if (meta)
		*meta = NULL;
	if (metalen)
		*metalen = 0;
	if (unknown)
		*unknown = 0;
	if (!data || len < 8 + 4)
		FAIL("file too short");
	crc = (uint32_t)data[len - 4] | ((uint32_t)data[len - 3] << 8) | ((uint32_t)data[len - 2] << 16) |
	      ((uint32_t)data[len - 1] << 24);
	if (memcmp(data, "BWVX", 4))
		FAIL("not a voxel world file");
	if (Vox_CRC32(data, len - 4) != crc)
		FAIL("checksum mismatch (damaged file)");
	r.len = len - 4;
	r.pos = 4;
	version = rb_u32(&r);
	if (version != VX_FILE_VERSION)
		FAIL("unsupported version %u", version);
	for (int i = 0; i < 3; i++)
		size[i] = (int)rb_u32(&r);
	bs = rb_f32(&r);
	for (int i = 0; i < 3; i++)
		org[i] = rb_f32(&r);
	if (r.bad || !size_ok(size, bs) || !isfinite(org[0]) || !isfinite(org[1]) || !isfinite(org[2]))
		FAIL("bad world size");
	w = Vox_Create(size, bs, org);
	if (defs)
		memcpy(w->defs, defs->defs, sizeof(w->defs));
	ntypes = rb_u32(&r);
	if (r.bad || ntypes >= VX_MAX_TYPES)
		FAIL("bad type table");
	remap = Mem_Alloc(sizeof(int) * VX_MAX_TYPES);
	seen = Mem_Alloc(sizeof(bool) * VX_MAX_TYPES);
	for (int i = 0; i < VX_MAX_TYPES; i++)
		remap[i] = -1;
	remap[0] = 0;
	for (uint32_t i = 0; i < ntypes; i++) {
		unsigned id = rb_u16(&r), n = rb_u8(&r);
		char name[256];
		int t;
		if (r.bad || id == 0 || id >= VX_MAX_TYPES || seen[id] || n >= sizeof(name))
			FAIL("bad type entry %u", i);
		rb_get(&r, name, n);
		name[n] = 0;
		seen[id] = true;
		t = Vox_TypeForName(w, name);
		remap[id] = t > 0 ? t : -1;
		if (!defs)
			remap[id] = -1;
	}
	nchunks = rb_u32(&r);
	if (r.bad || nchunks > (uint32_t)numchunks(w))
		FAIL("bad chunk count");
	for (uint32_t i = 0; i < nchunks; i++) {
		unsigned cx = rb_u16(&r), cy = rb_u16(&r), cz = rb_u16(&r);
		uint32_t clen = rb_u32(&r);
		rbuf_t cr;
		if (r.bad || cx >= (unsigned)w->nchunks[0] || cy >= (unsigned)w->nchunks[1] || cz >= (unsigned)w->nchunks[2] ||
		    clen > r.len - r.pos)
			FAIL("bad chunk header %u", i);
		cr.data = r.data + r.pos;
		cr.len = clen;
		cr.pos = 0;
		cr.bad = false;
		if (*chunk_slot(w, (int)cx, (int)cy, (int)cz))
			FAIL("chunk %u,%u,%u stored twice", cx, cy, cz);
		if (!decode_into(w, (int)cx, (int)cy, (int)cz, &cr, remap, VX_MAX_TYPES, unknown) || cr.pos != clen)
			FAIL("bad chunk data at %u,%u,%u", cx, cy, cz);
		r.pos += clen;
	}
	mlen = rb_u32(&r);
	if (r.bad || mlen > r.len - r.pos)
		FAIL("bad metadata length");
	if (meta && mlen) {
		*meta = Mem_Alloc(mlen + 1);
		memcpy(*meta, r.data + r.pos, mlen);
	}
	if (metalen)
		*metalen = mlen;
	r.pos += mlen;
	if (r.pos != r.len)
		FAIL("%u trailing bytes", (unsigned)(r.len - r.pos));
	Mem_Free(remap);
	Mem_Free(seen);
	return w;
fail:
	Mem_Free(remap);
	Mem_Free(seen);
	Vox_Free(w);
	if (meta) {
		Mem_Free(*meta);
		*meta = NULL;
	}
	return NULL;
}
