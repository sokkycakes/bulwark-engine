/* Bulwark Source formats plugin - studio model skeletons: bones, hitboxes
 * and animations (MDL versions 44 to 49, with .ani animation blocks and
 * included animation models), turned into IQM joints and frames, plus the
 * "bone box" body: one box per bone in place of the mesh.
 * Layouts: docs/spec/source-formats.md, "Skeleton and animation".
 * ISC licence, see LICENSE. */
#include "mdlskel.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* --- small maths ---------------------------------------------------------- */

void mat_from_pq(float m[12], const float p[3], const float q[4])
{
	float x = q[0], y = q[1], z = q[2], w = q[3];
	float l = x * x + y * y + z * z + w * w;
	if (l > 0 && fabsf(l - 1) > 1e-6f) {
		l = 1 / sqrtf(l);
		x *= l, y *= l, z *= l, w *= l;
	}
	m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y - z * w), m[2] = 2 * (x * z + y * w), m[3] = p[0];
	m[4] = 2 * (x * y + z * w), m[5] = 1 - 2 * (x * x + z * z), m[6] = 2 * (y * z - x * w), m[7] = p[1];
	m[8] = 2 * (x * z - y * w), m[9] = 2 * (y * z + x * w), m[10] = 1 - 2 * (x * x + y * y), m[11] = p[2];
}

void mat_mul(float o[12], const float a[12], const float b[12])
{
	float r[12];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++)
			r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
		r[i * 4 + 3] += a[i * 4 + 3];
	}
	memcpy(o, r, sizeof(r));
}

void mat_point(const float m[12], const float p[3], float o[3])
{
	float r[3];
	for (int i = 0; i < 3; i++)
		r[i] = m[i * 4] * p[0] + m[i * 4 + 1] * p[1] + m[i * 4 + 2] * p[2] + m[i * 4 + 3];
	memcpy(o, r, sizeof(r));
}

void mat_dir(const float m[12], const float p[3], float o[3])
{
	float r[3];
	for (int i = 0; i < 3; i++)
		r[i] = m[i * 4] * p[0] + m[i * 4 + 1] * p[1] + m[i * 4 + 2] * p[2];
	memcpy(o, r, sizeof(r));
}

/* radian Euler angles (x, y, z) to a quaternion (x, y, z, w): a turn about
 * x, then y, then z */
static void euler_quat(const float a[3], float q[4])
{
	float sr = sinf(a[0] * 0.5f), cr = cosf(a[0] * 0.5f);
	float sp = sinf(a[1] * 0.5f), cp = cosf(a[1] * 0.5f);
	float sy = sinf(a[2] * 0.5f), cy = cosf(a[2] * 0.5f);
	q[0] = sr * cp * cy - cr * sp * sy;
	q[1] = cr * sp * cy + sr * cp * sy;
	q[2] = cr * cp * sy - sr * sp * cy;
	q[3] = cr * cp * cy + sr * sp * sy;
}

static void quat_mul(const float a[4], const float b[4], float o[4])
{
	float r[4];
	r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	memcpy(o, r, sizeof(r));
}

static float half_float(unsigned h)
{
	unsigned s = (h >> 15) & 1, e = (h >> 10) & 31, f = h & 1023;
	float v;
	if (!e)
		v = ldexpf((float)f, -24);
	else if (e == 31)
		v = f ? NAN : INFINITY;
	else
		v = ldexpf((float)(f | 1024), (int)e - 25);
	return s ? -v : v;
}

/* --- bones ----------------------------------------------------------------- */

#define BONE_SIZE 216

int mdl_bones(const blob_t *m, sbone_t **out)
{
	unsigned n = rd32(m->d + 156), ofs = rd32(m->d + 160);
	sbone_t *b;
	*out = NULL;
	if (!n || n > 255 || !in(m, ofs, (size_t)n * BONE_SIZE))
		return 0;
	b = api->alloc(sizeof(sbone_t) * n);
	for (unsigned i = 0; i < n; i++) {
		const unsigned char *p = m->d + ofs + (size_t)i * BONE_SIZE;
		snprintf(b[i].name, sizeof(b[i].name), "%s", str_at(m, ofs + (size_t)i * BONE_SIZE + (size_t)rds32(p)));
		b[i].parent = rds32(p + 4);
		if (b[i].parent >= (int)i)
			b[i].parent = -1;
		for (int k = 0; k < 3; k++) {
			b[i].pos[k] = rdf(p + 32 + k * 4);
			b[i].rot[k] = rdf(p + 60 + k * 4);
			b[i].posscale[k] = rdf(p + 72 + k * 4);
			b[i].rotscale[k] = rdf(p + 84 + k * 4);
		}
		for (int k = 0; k < 4; k++)
			b[i].quat[k] = rdf(p + 44 + k * 4);
		for (int k = 0; k < 12; k++)
			b[i].posetobone[k] = rdf(p + 96 + k * 4);
	}
	*out = b;
	return (int)n;
}

/* model-space matrices of the bones' rest pose */
void mdl_restpose(const sbone_t *b, int n, float (*abs)[12])
{
	for (int i = 0; i < n; i++) {
		float rel[12];
		mat_from_pq(rel, b[i].pos, b[i].quat);
		if (b[i].parent >= 0)
			mat_mul(abs[i], abs[b[i].parent], rel);
		else
			memcpy(abs[i], rel, sizeof(rel));
	}
}

/* --- bone boxes ------------------------------------------------------------- */

static void box_add(boxes_t *bx, int bone, int color, const float mins[3], const float maxs[3])
{
	if (bx->num == bx->cap) {
		int cap = bx->cap ? bx->cap * 2 : 64;
		sbox_t *n = api->alloc(sizeof(sbox_t) * (size_t)cap);
		if (bx->b)
			memcpy(n, bx->b, sizeof(sbox_t) * (size_t)bx->num);
		api->free(bx->b);
		bx->b = n;
		bx->cap = cap;
	}
	bx->b[bx->num].bone = bone;
	bx->b[bx->num].color = color;
	memcpy(bx->b[bx->num].mins, mins, sizeof(float) * 3);
	memcpy(bx->b[bx->num].maxs, maxs, sizeof(float) * 3);
	bx->num++;
}

/* hitbox set 0's boxes (bone space), then a box for every bone without
 * one: along the bone to its first child, or a small cube at a tip */
void mdl_boxes(const blob_t *m, const sbone_t *b, int nb, boxes_t *bx)
{
	unsigned nsets = rd32(m->d + 172), setofs = rd32(m->d + 176);
	unsigned char has[256] = {0};
	if (nsets && in(m, setofs, 12)) {
		unsigned n = rd32(m->d + setofs + 4);
		size_t hb = setofs + rd32(m->d + setofs + 8);
		for (unsigned i = 0; i < n && i < 1024; i++) {
			const unsigned char *p;
			float mins[3], maxs[3];
			int bone, group;
			if (!in(m, hb + (size_t)i * 68, 68))
				break;
			p = m->d + hb + (size_t)i * 68;
			bone = rds32(p);
			group = rds32(p + 4);
			if (bone < 0 || bone >= nb)
				continue;
			for (int k = 0; k < 3; k++) {
				mins[k] = rdf(p + 8 + k * 4);
				maxs[k] = rdf(p + 20 + k * 4);
			}
			box_add(bx, bone, group >= 0 && group < 8 ? group : 0, mins, maxs);
			has[bone] = 1;
			bx->numhitboxes++;
		}
	}
	for (int i = 0; i < nb; i++) {
		float mins[3], maxs[3], len = 0;
		int child = -1;
		if (has[i])
			continue;
		for (int c = i + 1; c < nb; c++)
			if (b[c].parent == i) {
				float l = sqrtf(b[c].pos[0] * b[c].pos[0] + b[c].pos[1] * b[c].pos[1] + b[c].pos[2] * b[c].pos[2]);
				if (l > 0.25f) {
					child = c;
					len = l;
					break;
				}
			}
		if (child >= 0) {
			/* a box from the bone to its child, as wide as a fifth of its length */
			float w = len * 0.1f;
			if (w < 0.3f)
				w = 0.3f;
			if (w > 1.5f)
				w = 1.5f;
			for (int k = 0; k < 3; k++) {
				float e = b[child].pos[k];
				mins[k] = (e < 0 ? e : 0) - w;
				maxs[k] = (e > 0 ? e : 0) + w;
			}
		} else {
			for (int k = 0; k < 3; k++)
				mins[k] = -0.5f, maxs[k] = 0.5f;
		}
		box_add(bx, i, BOXCOLOR_BONE, mins, maxs);
	}
}

/* --- animations --------------------------------------------------------------- */

#define ANIMDESC_SIZE 100
#define SEQ_SIZE 212

enum {
	ANIM_RAWPOS = 0x01, /* a Vector48 */
	ANIM_RAWROT = 0x02, /* a Quaternion48 */
	ANIM_ANIMPOS = 0x04, /* run-length values per axis */
	ANIM_ANIMROT = 0x08, /* run-length Euler angles per axis */
	ANIM_DELTA = 0x10,
	ANIM_RAWROT2 = 0x20 /* a Quaternion64 */
};
enum { DESC_DELTA = 0x04, DESC_ALLZEROS = 0x20, DESC_FRAMEANIM = 0x40 };
#define SEQ_LOOPING 0x01

/* one .mdl that sequences come from: the model itself or an included one */
typedef struct {
	blob_t m, ani;
	unsigned char *own, *ownani; /* to free */
	sbone_t *bones;
	int numbones;
	int map[256]; /* its bones to the main model's, -1 for none */
} source_t;

static void *load_ani(const blob_t *m, size_t *len)
{
	const char *name = str_at(m, rd32(m->d + 348));
	char path[300];
	*len = 0;
	if (!rd32(m->d + 352) || !*name)
		return NULL;
	snprintf(path, sizeof(path), "%s", name);
	src_cleanpath(path);
	return api->load_file(path, len);
}

/* the value of one channel at frame from a run-length list: pairs of
 * (valid, total) counts each followed by `valid` values; the last valid
 * value holds for the rest of the run */
static int anim_value(const blob_t *b, size_t p, int frame, short *out)
{
	int k = frame;
	for (int guard = 0; guard < 4096; guard++) {
		unsigned valid, total;
		if (!in(b, p, 2))
			return 0;
		valid = b->d[p];
		total = b->d[p + 1];
		if (!total)
			return 0;
		if ((int)total <= k) {
			k -= (int)total;
			p += (size_t)(valid + 1) * 2;
			continue;
		}
		{
			size_t at = p + (size_t)((int)valid > k ? k + 1 : (int)valid) * 2;
			if (!in(b, at, 2))
				return 0;
			*out = (short)rd16(b->d + at);
			return 1;
		}
	}
	return 0;
}

/* where an animation's data for frame is: the blob, the offset and the
 * frame within that block */
static int anim_data(const source_t *s, size_t desc, int frame, const blob_t **blob, size_t *ofs, int *local)
{
	const blob_t *m = &s->m;
	int block = rds32(m->d + desc + 52), index = rds32(m->d + desc + 56);
	int secframes = rds32(m->d + desc + 84), numframes = rds32(m->d + desc + 16);
	*local = frame;
	if (secframes > 0 && rds32(m->d + desc + 80)) {
		int nsec = numframes > 1 ? (numframes - 1) / secframes + 1 : 1;
		int sec = frame / secframes;
		size_t so;
		if (sec >= nsec)
			sec = nsec - 1;
		so = desc + (size_t)rds32(m->d + desc + 80) + (size_t)sec * 8;
		if (!in(m, so, 8))
			return 0;
		block = rds32(m->d + so);
		index = rds32(m->d + so + 4);
		*local = frame - sec * secframes;
	}
	if (!block) {
		*blob = m;
		*ofs = desc + (size_t)index;
		return in(m, *ofs, 4);
	}
	{
		unsigned nblocks = rd32(m->d + 352), bofs = rd32(m->d + 356);
		if (!s->ani.d || block < 0 || (unsigned)block >= nblocks || !in(m, bofs + (size_t)block * 8, 8))
			return 0;
		*blob = &s->ani;
		*ofs = rd32(m->d + bofs + (size_t)block * 8) + (size_t)index;
		return in(&s->ani, *ofs, 4);
	}
}

static void quat48(const unsigned char *d, float q[4])
{
	unsigned x = rd16(d), y = rd16(d + 2), zw = rd16(d + 4);
	float l;
	q[0] = ((float)x - 32768.0f) / 32768.0f;
	q[1] = ((float)y - 32768.0f) / 32768.0f;
	q[2] = ((float)(zw & 0x7fff) - 16384.0f) / 16384.0f;
	l = 1 - q[0] * q[0] - q[1] * q[1] - q[2] * q[2];
	q[3] = l > 0 ? sqrtf(l) : 0;
	if (zw & 0x8000)
		q[3] = -q[3];
}

/* the "smallest three" 48-bit quaternion of frame animations: three
 * components in 15 bits each, the top bits of the first two saying which
 * component comes first, and the fourth rebuilt with the sign bit */
static void quat48s(const unsigned char *d, float q[4])
{
	unsigned a = rd16(d), b = rd16(d + 2), c = rd16(d + 4);
	int first = (int)((b >> 15) + (a >> 15) * 2);
	float l;
	q[first] = ((float)(a & 0x7fff) - 16384.0f) / 23168.0f;
	q[(first + 1) & 3] = ((float)(b & 0x7fff) - 16384.0f) / 23168.0f;
	q[(first + 2) & 3] = ((float)(c & 0x7fff) - 16384.0f) / 23168.0f;
	l = 1 - q[first] * q[first] - q[(first + 1) & 3] * q[(first + 1) & 3] - q[(first + 2) & 3] * q[(first + 2) & 3];
	q[(first + 3) & 3] = l > 0 ? sqrtf(l) : 0;
	if (c & 0x8000)
		q[(first + 3) & 3] = -q[(first + 3) & 3];
}

enum {
	FRAME_RAWPOS = 0x01,  /* constant Vector48 */
	FRAME_RAWROT = 0x02,  /* constant Quaternion48 */
	FRAME_ANIMPOS = 0x04, /* Vector48 every frame */
	FRAME_ANIMROT = 0x08, /* Quaternion48 every frame */
	FRAME_FULLPOS = 0x10, /* three floats every frame */
	FRAME_CONSTPOS2 = 0x20, /* constant three floats */
	FRAME_CONSTROT2 = 0x40, /* constant Quaternion48S */
	FRAME_ANIMROT2 = 0x80 /* Quaternion48S every frame */
};

static void put_bone(float *o, const sbone_t *mb, const float q[4], const float pos[3], int delta)
{
	if (delta) {
		float r[4];
		quat_mul(mb->quat, q, r);
		memcpy(o + 3, r, sizeof(r));
		for (int k = 0; k < 3; k++)
			o[k] = mb->pos[k] + pos[k];
	} else {
		memcpy(o, pos, sizeof(float) * 3);
		memcpy(o + 3, q, sizeof(float) * 4);
	}
}

/* frame animations (the animation's flag 0x40, L4D's models): a header of
 * constants offset, frame offset and frame size (then 12 unused bytes),
 * one flag byte per bone, the constant values bone by bone and then each
 * frame's values bone by bone, each bone's rotation before its position */
static void decode_frameanim(const source_t *s, const blob_t *b, size_t p, int local, const sbone_t *mainb, int delta,
                             float *out)
{
	int co = rds32(b->d + p), fo = rds32(b->d + p + 4), fl = rds32(b->d + p + 8);
	size_t flags = p + 24, cp, fp;
	if (!in(b, flags, (size_t)s->numbones))
		return;
	cp = p + (size_t)(co > 0 ? co : 0);
	fp = p + (size_t)(fo > 0 ? fo : 0) + (size_t)local * (size_t)(fl > 0 ? fl : 0);
	for (int bone = 0; bone < s->numbones; bone++) {
		unsigned f = b->d[flags + bone];
		const sbone_t *sb = &s->bones[bone];
		float q[4], pos[3];
		int hq = 0, hp = 0;
		if (delta)
			q[0] = q[1] = q[2] = 0, q[3] = 1, pos[0] = pos[1] = pos[2] = 0;
		else
			memcpy(q, sb->quat, sizeof(q)), memcpy(pos, sb->pos, sizeof(pos));
		if (co > 0) {
			if ((f & FRAME_CONSTROT2) && in(b, cp, 6))
				quat48s(b->d + cp, q), cp += 6, hq = 1;
			if ((f & FRAME_RAWROT) && in(b, cp, 6))
				quat48(b->d + cp, q), cp += 6, hq = 1;
			if ((f & FRAME_RAWPOS) && in(b, cp, 6)) {
				for (int k = 0; k < 3; k++)
					pos[k] = half_float(rd16(b->d + cp + k * 2));
				cp += 6, hp = 1;
			}
			if ((f & FRAME_CONSTPOS2) && in(b, cp, 12)) {
				for (int k = 0; k < 3; k++)
					pos[k] = rdf(b->d + cp + k * 4);
				cp += 12, hp = 1;
			}
		}
		if (fo > 0 && fl > 0) {
			if ((f & FRAME_ANIMROT2) && in(b, fp, 6))
				quat48s(b->d + fp, q), fp += 6, hq = 1;
			if ((f & FRAME_ANIMROT) && in(b, fp, 6))
				quat48(b->d + fp, q), fp += 6, hq = 1;
			if ((f & FRAME_ANIMPOS) && in(b, fp, 6)) {
				for (int k = 0; k < 3; k++)
					pos[k] = half_float(rd16(b->d + fp + k * 2));
				fp += 6, hp = 1;
			}
			if ((f & FRAME_FULLPOS) && in(b, fp, 12)) {
				for (int k = 0; k < 3; k++)
					pos[k] = rdf(b->d + fp + k * 4);
				fp += 12, hp = 1;
			}
		}
		if ((hq || hp) && s->map[bone] >= 0)
			put_bone(out + s->map[bone] * 10, &mainb[s->map[bone]], q, pos, delta);
	}
}

/* one frame of an animation into out (main model joints x 10: translate,
 * rotate xyzw, scale); bones it doesn't move keep their rest pose */
static void decode_frame(const source_t *s, size_t desc, int frame, const sbone_t *mainb, int nmain, float *out)
{
	int flags = rds32(s->m.d + desc + 12), delta = (flags & DESC_DELTA) != 0;
	const blob_t *b;
	size_t p;
	int local;
	for (int j = 0; j < nmain; j++) {
		float *o = out + j * 10;
		memcpy(o, mainb[j].pos, sizeof(float) * 3);
		memcpy(o + 3, mainb[j].quat, sizeof(float) * 4);
		o[7] = o[8] = o[9] = 1;
	}
	if (flags & DESC_ALLZEROS)
		return;
	if (!anim_data(s, desc, frame, &b, &p, &local))
		return;
	if (flags & DESC_FRAMEANIM) {
		decode_frameanim(s, b, p, local, mainb, delta, out);
		return;
	}
	for (int guard = 0; guard < 512; guard++) {
		unsigned bone = b->d[p], af = b->d[p + 1];
		int next = (short)rd16(b->d + p + 2);
		size_t data = p + 4;
		if (bone < (unsigned)s->numbones && s->map[bone] >= 0) {
			const sbone_t *sb = &s->bones[bone];
			int bdelta = (af & ANIM_DELTA) != 0;
			float q[4], pos[3];
			size_t posdata = data;
			/* rotation */
			if ((af & ANIM_RAWROT) && in(b, data, 6)) {
				const unsigned char *d = b->d + data;
				unsigned x = rd16(d), y = rd16(d + 2), zw = rd16(d + 4);
				float l;
				q[0] = ((float)x - 32768.0f) / 32768.0f;
				q[1] = ((float)y - 32768.0f) / 32768.0f;
				q[2] = ((float)(zw & 0x7fff) - 16384.0f) / 16384.0f;
				l = 1 - q[0] * q[0] - q[1] * q[1] - q[2] * q[2];
				q[3] = l > 0 ? sqrtf(l) : 0;
				if (zw & 0x8000)
					q[3] = -q[3];
				posdata += 6;
			} else if ((af & ANIM_RAWROT2) && in(b, data, 8)) {
				const unsigned char *d = b->d + data;
				unsigned long long v = (unsigned long long)rd32(d) | ((unsigned long long)rd32(d + 4) << 32);
				float l;
				q[0] = ((float)(v & 0x1fffff) - 1048576.0f) / 1048576.5f;
				q[1] = ((float)((v >> 21) & 0x1fffff) - 1048576.0f) / 1048576.5f;
				q[2] = ((float)((v >> 42) & 0x1fffff) - 1048576.0f) / 1048576.5f;
				l = 1 - q[0] * q[0] - q[1] * q[1] - q[2] * q[2];
				q[3] = l > 0 ? sqrtf(l) : 0;
				if (v >> 63)
					q[3] = -q[3];
				posdata += 8;
			} else if ((af & ANIM_ANIMROT) && in(b, data, 6)) {
				float a[3];
				for (int k = 0; k < 3; k++) {
					unsigned o = rd16(b->d + data + k * 2);
					short v = 0;
					a[k] = o && anim_value(b, data + o, local, &v) ? v * sb->rotscale[k] : 0;
					if (!bdelta)
						a[k] += sb->rot[k];
				}
				euler_quat(a, q);
				posdata += 6;
			} else if (bdelta || delta) {
				q[0] = q[1] = q[2] = 0, q[3] = 1;
			} else
				memcpy(q, sb->quat, sizeof(q));
			/* position */
			if ((af & ANIM_RAWPOS) && in(b, posdata, 6)) {
				for (int k = 0; k < 3; k++)
					pos[k] = half_float(rd16(b->d + posdata + k * 2));
			} else if ((af & ANIM_ANIMPOS) && in(b, posdata, 6)) {
				for (int k = 0; k < 3; k++) {
					unsigned o = rd16(b->d + posdata + k * 2);
					short v = 0;
					pos[k] = o && anim_value(b, posdata + o, local, &v) ? v * sb->posscale[k] : 0;
					if (!bdelta)
						pos[k] += sb->pos[k];
				}
			} else if (bdelta || delta) {
				pos[0] = pos[1] = pos[2] = 0;
			} else
				memcpy(pos, sb->pos, sizeof(pos));
			{
				int j = s->map[bone];
				float *o = out + j * 10;
				if (delta) {
					/* a layer: on top of the rest pose */
					float r[4];
					quat_mul(mainb[j].quat, q, r);
					memcpy(o + 3, r, sizeof(r));
					for (int k = 0; k < 3; k++)
						o[k] = mainb[j].pos[k] + pos[k];
				} else {
					memcpy(o, pos, sizeof(pos));
					memcpy(o + 3, q, sizeof(q));
				}
			}
		}
		if (!next || !in(b, p + (size_t)next, 4))
			break;
		p += (size_t)next;
	}
}

static void anims_push(anims_t *a, const char *name, int count, float fps, int loop, int nj)
{
	if (a->num == a->cap) {
		int cap = a->cap ? a->cap * 2 : 64;
		sanim_t *n = api->alloc(sizeof(sanim_t) * (size_t)cap);
		if (a->a)
			memcpy(n, a->a, sizeof(sanim_t) * (size_t)a->num);
		api->free(a->a);
		a->a = n;
		a->cap = cap;
	}
	snprintf(a->a[a->num].name, sizeof(a->a[a->num].name), "%s", name);
	a->a[a->num].first = a->numframes;
	a->a[a->num].count = count;
	a->a[a->num].fps = fps > 0 ? fps : 30;
	a->a[a->num].loop = loop;
	a->num++;
	if (a->numframes + count > a->capframes) {
		int cap = a->capframes ? a->capframes * 2 : 256;
		float *n;
		while (cap < a->numframes + count)
			cap *= 2;
		n = api->alloc(sizeof(float) * 10 * (size_t)nj * (size_t)cap);
		if (a->frames)
			memcpy(n, a->frames, sizeof(float) * 10 * (size_t)nj * (size_t)a->numframes);
		api->free(a->frames);
		a->frames = n;
		a->capframes = cap;
	}
	a->numframes += count;
}

/* an animation's frames as a new anim */
static void add_anim(anims_t *a, const source_t *s, int anim, const char *name, int loop, const sbone_t *mainb, int nmain)
{
	unsigned nanims = rd32(s->m.d + 180), aofs = rd32(s->m.d + 184);
	size_t desc = aofs + (size_t)anim * ANIMDESC_SIZE;
	int frames;
	float fps;
	sanim_t *g;
	if (anim < 0 || (unsigned)anim >= nanims || !in(&s->m, desc, ANIMDESC_SIZE)) {
		float *o;
		anims_push(a, name, 1, 30, loop, nmain);
		o = a->frames + (size_t)a->a[a->num - 1].first * nmain * 10;
		for (int j = 0; j < nmain; j++, o += 10) {
			memcpy(o, mainb[j].pos, sizeof(float) * 3);
			memcpy(o + 3, mainb[j].quat, sizeof(float) * 4);
			o[7] = o[8] = o[9] = 1;
		}
		return;
	}
	frames = rds32(s->m.d + desc + 16);
	fps = rdf(s->m.d + desc + 8);
	if (frames < 1)
		frames = 1;
	if (frames > 4096)
		frames = 4096;
	anims_push(a, name, frames, fps, loop, nmain);
	g = &a->a[a->num - 1];
	for (int f = 0; f < frames; f++)
		decode_frame(s, desc, f, mainb, nmain, a->frames + (size_t)(g->first + f) * nmain * 10);
}

/* an animation's name without Source's "@" prefix, or the grid cell */
static const char *anim_name(const source_t *s, int anim, int x, int y)
{
	static char buf[64];
	unsigned nanims = rd32(s->m.d + 180), aofs = rd32(s->m.d + 184);
	size_t desc = aofs + (size_t)anim * ANIMDESC_SIZE;
	const char *n = "";
	if (anim >= 0 && (unsigned)anim < nanims && in(&s->m, desc, ANIMDESC_SIZE))
		n = str_at(&s->m, desc + (size_t)rds32(s->m.d + desc + 4));
	if (*n == '@')
		n++;
	if (*n)
		snprintf(buf, sizeof(buf), "%s", n);
	else
		snprintf(buf, sizeof(buf), "%d,%d", x, y);
	return buf;
}

static void add_sequences(anims_t *a, const source_t *s, const sbone_t *mainb, int nmain, int blends)
{
	unsigned nseq = rd32(s->m.d + 188), sofs = rd32(s->m.d + 192);
	for (unsigned i = 0; i < nseq && i < 4096; i++) {
		size_t seq = sofs + (size_t)i * SEQ_SIZE;
		const char *label;
		int flags, g0, g1, x, y;
		size_t idx;
		if (!in(&s->m, seq, SEQ_SIZE))
			break;
		label = str_at(&s->m, seq + (size_t)rds32(s->m.d + seq + 4));
		flags = rds32(s->m.d + seq + 12);
		g0 = rds32(s->m.d + seq + 68);
		g1 = rds32(s->m.d + seq + 72);
		if (g0 < 1)
			g0 = 1;
		if (g1 < 1)
			g1 = 1;
		idx = seq + (size_t)rds32(s->m.d + seq + 60);
		if (!blends) {
			/* the sequence, from the middle of its blend grid */
			x = g0 / 2, y = g1 / 2;
			if (in(&s->m, idx + (size_t)(y * g0 + x) * 2, 2))
				add_anim(a, s, (short)rd16(s->m.d + idx + (size_t)(y * g0 + x) * 2), label, flags & SEQ_LOOPING, mainb, nmain);
			else
				add_anim(a, s, -1, label, flags & SEQ_LOOPING, mainb, nmain);
		} else if (g0 * g1 > 1) {
			/* every other cell of the grid, as "<sequence>:<animation>" */
			for (y = 0; y < g1 && y < 16; y++)
				for (x = 0; x < g0 && x < 16; x++) {
					char name[96];
					if (x == g0 / 2 && y == g1 / 2)
						continue;
					if (!in(&s->m, idx + (size_t)(y * g0 + x) * 2, 2))
						continue;
					int anim = (short)rd16(s->m.d + idx + (size_t)(y * g0 + x) * 2);
					snprintf(name, sizeof(name), "%s:%s", label, anim_name(s, anim, x, y));
					add_anim(a, s, anim, name, flags & SEQ_LOOPING, mainb, nmain);
				}
		}
	}
}

static void source_free(source_t *s)
{
	api->free(s->own);
	api->free(s->ownani);
	api->free(s->bones);
	memset(s, 0, sizeof(*s));
}

static int source_open(source_t *s, unsigned char *own, size_t len, const sbone_t *mainb, int nmain)
{
	size_t alen;
	memset(s, 0, sizeof(*s));
	s->own = own;
	s->m.d = own;
	s->m.len = len;
	if (len < 408 || memcmp(own, "IDST", 4) || rds32(own + 4) < 44 || rds32(own + 4) > 49)
		return 0;
	s->numbones = mdl_bones(&s->m, &s->bones);
	s->ownani = load_ani(&s->m, &alen);
	s->ani.d = s->ownani;
	s->ani.len = alen;
	for (int i = 0; i < 256; i++)
		s->map[i] = -1;
	for (int i = 0; i < s->numbones; i++)
		for (int j = 0; j < nmain; j++)
			if (!strcasecmp(s->bones[i].name, mainb[j].name)) {
				s->map[i] = j;
				break;
			}
	return 1;
}

/* every sequence of the model and the models it includes, in Source's
 * order (the model's own first), then the other cells of blended
 * sequences */
void mdl_anims(const char *path, const blob_t *m, const sbone_t *b, int nb, anims_t *a)
{
	source_t srcs[33];
	int nsrc = 0;
	unsigned ninc = rd32(m->d + 336), iofs = rd32(m->d + 340);
	size_t alen;
	(void)path;
	memset(&srcs[0], 0, sizeof(srcs[0]));
	srcs[0].m = *m;
	srcs[0].bones = NULL;
	srcs[0].numbones = nb;
	srcs[0].ownani = load_ani(m, &alen);
	srcs[0].ani.d = srcs[0].ownani;
	srcs[0].ani.len = alen;
	srcs[0].bones = (sbone_t *)b; /* borrowed: the model's bones map to themselves */
	for (int i = 0; i < 256; i++)
		srcs[0].map[i] = i < nb ? i : -1;
	if (rd32(m->d + 352) && !srcs[0].ownani)
		srcprint("mdl: %s keeps animations in %s, which is missing\n", path, str_at(m, rd32(m->d + 348)));
	nsrc = 1;
	for (unsigned i = 0; i < ninc && i < 32; i++) {
		size_t io = iofs + (size_t)i * 8;
		char ipath[300];
		size_t ilen;
		unsigned char *d;
		if (!in(m, io, 8))
			break;
		snprintf(ipath, sizeof(ipath), "%s", str_at(m, io + (size_t)rds32(m->d + io + 4)));
		src_cleanpath(ipath);
		d = api->load_file(ipath, &ilen);
		if (!d) {
			srcprint("mdl: %s includes %s, which is missing\n", path, ipath);
			continue;
		}
		if (!source_open(&srcs[nsrc], d, ilen, b, nb)) {
			source_free(&srcs[nsrc]);
			continue;
		}
		nsrc++;
	}
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < nsrc; i++)
			add_sequences(a, &srcs[i], b, nb, pass);
	api->free(srcs[0].ownani);
	for (int i = 1; i < nsrc; i++)
		source_free(&srcs[i]);
}
