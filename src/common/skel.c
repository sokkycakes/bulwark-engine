/* Bulwark engine - skeletal models: IQM joints, poses and animations, and
 * the bone maths the skeleton builtins and the renderer share. ISC licence,
 * see LICENSE. */
#include "skel.h"
#include "studio.h"
#include "fs.h"
#include "cmd.h"

#include <math.h>
#include <stdio.h>
#if defined(__SSE__) && !defined(BW_NO_SSE)
#include <xmmintrin.h>
#define SKEL_SSE 1
#endif

static uint32_t rdu(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static float rdf(const uint8_t *p)
{
	float f;
	uint32_t i = rdu(p);
	memcpy(&f, &i, 4);
	return f;
}

/* --- bone matrices ----------------------------------------------------------- */

void Bone_Identity(bonemat_t m)
{
	memset(m, 0, sizeof(bonemat_t));
	m[0] = m[5] = m[10] = 1;
}

#ifdef SKEL_SSE
/* The SSE versions work four lanes at once (each SSE instruction is one
 * call into the emulator's helpers in xemu, packed or not) and do in each
 * lane the operations the plain C does, in its order, so they give the
 * same bits: subtracting is adding the negated value, and doubling is
 * exact either way. */
#define LANES(a, b, c, d) _MM_SHUFFLE(d, c, b, a)

static inline __m128 sse_mask(int a, int b, int c, int d)
{
	union {
		uint32_t u[4];
		__m128 v;
	} m = {{a ? ~0u : 0, b ? ~0u : 0, c ? ~0u : 0, d ? ~0u : 0}};
	return m.v;
}

/* the rotation and scale of Bone_FromTRS, a row at a time, for the unit
 * quaternion q (x y z w) */
static inline void trs_rows(bonemat_t m, __m128 q, const float *trs)
{
	const __m128 sign = sse_mask(0, 1, 0, 0), one = _mm_set1_ps(1);
	const __m128 scale = _mm_setr_ps(trs[7], trs[8], trs[9], 0);
	/* row 0: 1 - 2(yy + zz), 2(xy - zw), 2(xz + yw) */
	__m128 p = _mm_mul_ps(_mm_shuffle_ps(q, q, LANES(1, 0, 0, 0)), _mm_shuffle_ps(q, q, LANES(1, 1, 2, 0)));
	__m128 n = _mm_mul_ps(_mm_shuffle_ps(q, q, LANES(2, 2, 1, 0)), _mm_shuffle_ps(q, q, LANES(2, 3, 3, 0)));
	__m128 d = _mm_add_ps(p, _mm_xor_ps(n, _mm_and_ps(sign, _mm_set1_ps(-0.0f))));
	__m128 mask = sse_mask(1, 0, 0, 0);
	d = _mm_add_ps(d, d);
	d = _mm_or_ps(_mm_and_ps(mask, _mm_sub_ps(one, d)), _mm_andnot_ps(mask, d));
	_mm_storeu_ps(m, _mm_mul_ps(d, scale));
	/* row 1: 2(xy + zw), 1 - 2(xx + zz), 2(yz - xw) */
	p = _mm_mul_ps(_mm_shuffle_ps(q, q, LANES(0, 0, 1, 0)), _mm_shuffle_ps(q, q, LANES(1, 0, 2, 0)));
	n = _mm_mul_ps(_mm_shuffle_ps(q, q, LANES(2, 2, 0, 0)), _mm_shuffle_ps(q, q, LANES(3, 2, 3, 0)));
	d = _mm_add_ps(p, _mm_xor_ps(n, _mm_and_ps(sse_mask(0, 0, 1, 0), _mm_set1_ps(-0.0f))));
	mask = sse_mask(0, 1, 0, 0);
	d = _mm_add_ps(d, d);
	d = _mm_or_ps(_mm_and_ps(mask, _mm_sub_ps(one, d)), _mm_andnot_ps(mask, d));
	_mm_storeu_ps(m + 4, _mm_mul_ps(d, scale));
	/* row 2: 2(xz - yw), 2(yz + xw), 1 - 2(xx + yy) */
	p = _mm_mul_ps(_mm_shuffle_ps(q, q, LANES(0, 1, 0, 0)), _mm_shuffle_ps(q, q, LANES(2, 2, 0, 0)));
	n = _mm_mul_ps(_mm_shuffle_ps(q, q, LANES(1, 0, 1, 0)), _mm_shuffle_ps(q, q, LANES(3, 3, 1, 0)));
	d = _mm_add_ps(p, _mm_xor_ps(n, _mm_and_ps(sse_mask(1, 0, 0, 0), _mm_set1_ps(-0.0f))));
	mask = sse_mask(0, 0, 1, 0);
	d = _mm_add_ps(d, d);
	d = _mm_or_ps(_mm_and_ps(mask, _mm_sub_ps(one, d)), _mm_andnot_ps(mask, d));
	/* the last row's fourth lane would write past the matrix */
	float r2[4];
	_mm_storeu_ps(r2, _mm_mul_ps(d, scale));
	m[8] = r2[0], m[9] = r2[1], m[10] = r2[2];
	m[3] = trs[0], m[7] = trs[1], m[11] = trs[2];
}
#endif

/* the IQM convention: output = rotate(input * scale) + translate */
void Bone_FromTRS(bonemat_t m, const float *trs)
{
	float x = trs[3], y = trs[4], z = trs[5], w = trs[6];
	float l = x * x + y * y + z * z + w * w;
	if (l > 0 && fabsf(l - 1) > 1e-6f) {
		l = 1 / sqrtf(l);
		x *= l, y *= l, z *= l, w *= l;
	}
#ifdef SKEL_SSE
	trs_rows(m, _mm_setr_ps(x, y, z, w), trs);
#else
	float r[3][3];
	r[0][0] = 1 - 2 * (y * y + z * z), r[0][1] = 2 * (x * y - z * w), r[0][2] = 2 * (x * z + y * w);
	r[1][0] = 2 * (x * y + z * w), r[1][1] = 1 - 2 * (x * x + z * z), r[1][2] = 2 * (y * z - x * w);
	r[2][0] = 2 * (x * z - y * w), r[2][1] = 2 * (y * z + x * w), r[2][2] = 1 - 2 * (x * x + y * y);
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++)
			m[i * 4 + j] = r[i][j] * trs[7 + j];
		m[i * 4 + 3] = trs[i];
	}
#endif
}

void Bone_Multiply(bonemat_t out, const bonemat_t a, const bonemat_t b)
{
#ifdef SKEL_SSE
	/* all of a and b are read before out is written: they may be out */
	const __m128 b0 = _mm_loadu_ps(b), b1 = _mm_loadu_ps(b + 4), b2 = _mm_setr_ps(b[8], b[9], b[10], b[11]);
	const __m128 last = sse_mask(0, 0, 0, 1);
	__m128 r[3];
	for (int i = 0; i < 3; i++) {
		const float *ar = a + i * 4;
		__m128 v = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(ar[0]), b0), _mm_mul_ps(_mm_set1_ps(ar[1]), b1)),
		                      _mm_mul_ps(_mm_set1_ps(ar[2]), b2));
		/* the translation adds a's own, in the fourth lane only */
		__m128 t = _mm_add_ps(v, _mm_set1_ps(ar[3]));
		r[i] = _mm_or_ps(_mm_and_ps(last, t), _mm_andnot_ps(last, v));
	}
	_mm_storeu_ps(out, r[0]);
	_mm_storeu_ps(out + 4, r[1]);
	float r2[4];
	_mm_storeu_ps(r2, r[2]);
	out[8] = r2[0], out[9] = r2[1], out[10] = r2[2], out[11] = r2[3];
#else
	bonemat_t o;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++)
			o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
		o[i * 4 + 3] += a[i * 4 + 3];
	}
	memcpy(out, o, sizeof(o));
#endif
}

void Bone_Invert(bonemat_t out, const bonemat_t m)
{
	float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
	float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
	float det = a * A + b * B + c * C;
	bonemat_t o;
	if (fabsf(det) < 1e-12f) {
		Bone_Identity(out);
		return;
	}
	det = 1 / det;
	o[0] = A * det, o[1] = -(b * i - c * h) * det, o[2] = (b * f - c * e) * det;
	o[4] = B * det, o[5] = (a * i - c * g) * det, o[6] = -(a * f - c * d) * det;
	o[8] = C * det, o[9] = -(a * h - b * g) * det, o[10] = (a * e - b * d) * det;
	for (int r = 0; r < 3; r++)
		o[r * 4 + 3] = -(o[r * 4] * m[3] + o[r * 4 + 1] * m[7] + o[r * 4 + 2] * m[11]);
	memcpy(out, o, sizeof(o));
}

void Bone_FromVectors(bonemat_t m, const float *org, const float *fwd, const float *right, const float *up)
{
	for (int i = 0; i < 3; i++) {
		m[i * 4] = fwd[i];
		m[i * 4 + 1] = -right[i];
		m[i * 4 + 2] = up[i];
		m[i * 4 + 3] = org[i];
	}
}

void Bone_ToVectors(const bonemat_t m, float *org, float *fwd, float *right, float *up)
{
	for (int i = 0; i < 3; i++) {
		if (fwd)
			fwd[i] = m[i * 4];
		if (right)
			right[i] = -m[i * 4 + 1];
		if (up)
			up[i] = m[i * 4 + 2];
		if (org)
			org[i] = m[i * 4 + 3];
	}
}

void Bone_Transform(const bonemat_t m, const float *in, float *out)
{
	float o[3];
	for (int i = 0; i < 3; i++)
		o[i] = m[i * 4] * in[0] + m[i * 4 + 1] * in[1] + m[i * 4 + 2] * in[2] + m[i * 4 + 3];
	VectorCopy(o, out);
}

/* --- framegroups ------------------------------------------------------------- */

framegroup_t *Skel_LoadFramegroups(const char *model, int *count)
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

/* --- IQM skeletons ----------------------------------------------------------- */

static skelmodel_t *skelmodels;

/* header words after the 16-byte magic, in the specification's order */
enum {
	H_VERSION, H_FILESIZE, H_FLAGS, H_NUM_TEXT, H_OFS_TEXT, H_NUM_MESHES, H_OFS_MESHES,
	H_NUM_VERTEXARRAYS, H_NUM_VERTEXES, H_OFS_VERTEXARRAYS, H_NUM_TRIANGLES, H_OFS_TRIANGLES,
	H_OFS_ADJACENCY, H_NUM_JOINTS, H_OFS_JOINTS, H_NUM_POSES, H_OFS_POSES, H_NUM_ANIMS, H_OFS_ANIMS,
	H_NUM_FRAMES, H_NUM_FRAMECHANNELS, H_OFS_FRAMES, H_OFS_BOUNDS, H_NUM_COMMENT, H_OFS_COMMENT,
	H_NUM_EXTENSIONS, H_OFS_EXTENSIONS, H_COUNT
};

/* the bind pose's relative matrices, kept for every pose that asks for
 * it, and its inverse in model space */
static void bind_pose(skelmodel_t *m)
{
	m->baserel = Mem_Alloc(sizeof(bonemat_t) * m->numjoints);
	for (int j = 0; j < m->numjoints; j++)
		Bone_FromTRS(m->baserel[j], m->base + j * 10);
	Skel_ToAbsolute(m, m->baserel, m->invbind);
	for (int j = 0; j < m->numjoints; j++)
		Bone_Invert(m->invbind[j], m->invbind[j]);
}

static bool parse_iqm(skelmodel_t *m, const uint8_t *d, size_t size)
{
	uint32_t h[H_COUNT];
	const char *text;
	if (size < 16 + H_COUNT * 4 || memcmp(d, "INTERQUAKEMODEL", 16))
		return false;
	for (int i = 0; i < H_COUNT; i++)
		h[i] = rdu(d + 16 + i * 4);
	if (h[H_VERSION] != 2 || !h[H_NUM_JOINTS] || h[H_NUM_JOINTS] > 4096)
		return false;
	if ((uint64_t)h[H_OFS_TEXT] + h[H_NUM_TEXT] > size ||
	    (uint64_t)h[H_OFS_JOINTS] + h[H_NUM_JOINTS] * 48ull > size ||
	    (uint64_t)h[H_OFS_ANIMS] + h[H_NUM_ANIMS] * 20ull > size)
		return false;
	text = (const char *)d + h[H_OFS_TEXT];
#define TEXT(o) ((o) < h[H_NUM_TEXT] ? text + (o) : "")
	m->numjoints = (int)h[H_NUM_JOINTS];
	m->joints = Mem_Alloc(sizeof(skeljoint_t) * m->numjoints);
	m->base = Mem_Alloc(sizeof(float) * 10 * m->numjoints);
	m->invbind = Mem_Alloc(sizeof(bonemat_t) * m->numjoints);
	for (int j = 0; j < m->numjoints; j++) {
		const uint8_t *jp = d + h[H_OFS_JOINTS] + j * 48;
		int parent = (int32_t)rdu(jp + 4);
		Str_Copy(m->joints[j].name, TEXT(rdu(jp)), sizeof(m->joints[j].name));
		/* parents come first; anything else is treated as a root */
		m->joints[j].parent = parent >= 0 && parent < j ? parent : -1;
		for (int k = 0; k < 10; k++)
			m->base[j * 10 + k] = rdf(jp + 8 + k * 4);
	}
	bind_pose(m);
	/* poses: each of the ten channels is offset + frame value * scale when
	 * its mask bit is set, otherwise just the offset */
	if (h[H_NUM_POSES] == h[H_NUM_JOINTS] && h[H_NUM_FRAMES] &&
	    (uint64_t)h[H_OFS_POSES] + h[H_NUM_POSES] * 88ull <= size &&
	    (uint64_t)h[H_OFS_FRAMES] + (uint64_t)h[H_NUM_FRAMES] * h[H_NUM_FRAMECHANNELS] * 2 <= size) {
		const uint8_t *fr = d + h[H_OFS_FRAMES];
		const uint8_t *end = fr + (size_t)h[H_NUM_FRAMES] * h[H_NUM_FRAMECHANNELS] * 2;
		m->numframes = (int)h[H_NUM_FRAMES];
		m->frames = Mem_Alloc(sizeof(float) * 10 * m->numjoints * m->numframes);
		for (int f = 0; f < m->numframes; f++) {
			for (int j = 0; j < m->numjoints; j++) {
				const uint8_t *pp = d + h[H_OFS_POSES] + j * 88;
				uint32_t mask = rdu(pp + 4);
				float *out = m->frames + ((size_t)f * m->numjoints + j) * 10;
				for (int c = 0; c < 10; c++) {
					out[c] = rdf(pp + 8 + c * 4);
					if ((mask & (1u << c)) && fr + 2 <= end) {
						out[c] += (float)(fr[0] | (fr[1] << 8)) * rdf(pp + 48 + c * 4);
						fr += 2;
					}
				}
			}
		}
	}
	m->numanims = (int)h[H_NUM_ANIMS];
	m->anims = Mem_Alloc(sizeof(framegroup_t) * (m->numanims + 1));
	for (int i = 0; i < m->numanims; i++) {
		const uint8_t *an = d + h[H_OFS_ANIMS] + i * 20;
		framegroup_t *g = &m->anims[i];
		Str_Copy(g->name, TEXT(rdu(an)), sizeof(g->name));
		g->first = (int)rdu(an + 4);
		g->count = BW_MAX(1, (int)rdu(an + 8));
		g->fps = rdf(an + 12) > 0 ? rdf(an + 12) : 10;
		g->loop = (rdu(an + 16) & 1) != 0;
	}
#undef TEXT
	return true;
}

static void free_skelmodel(skelmodel_t *m)
{
	Mem_Free(m->joints);
	Mem_Free(m->base);
	Mem_Free(m->baserel);
	Mem_Free(m->invbind);
	Mem_Free(m->frames);
	Mem_Free(m->seqfirst);
	Mem_Free(m->anims);
	Mem_Free(m);
}

/* --- GoldSrc studio skeletons ----------------------------------------------- */

/* a studio model's bones, with each sequence's first blend as an animation */
static bool from_studio(skelmodel_t *m, const studio_model_t *st)
{
	int total = 0;
	if (!st || !st->numbones)
		return false;
	m->numjoints = st->numbones;
	m->joints = Mem_Alloc(sizeof(skeljoint_t) * m->numjoints);
	m->base = Mem_Alloc(sizeof(float) * 10 * m->numjoints);
	m->invbind = Mem_Alloc(sizeof(bonemat_t) * m->numjoints);
	for (int j = 0; j < m->numjoints; j++) {
		Str_Copy(m->joints[j].name, st->bones[j].name, sizeof(m->joints[j].name));
		m->joints[j].parent = st->bones[j].parent;
	}
	Studio_RestPose(st, m->base);
	bind_pose(m);
	for (int i = 0; i < st->numseqs; i++)
		total += BW_MAX(1, st->seqs[i].numframes);
	m->numframes = total;
#ifdef BW_LOWMEM
	/* the cached model keeps its file, so the coded animation stays where
	   it is and the frames are decoded when a pose asks for them */
	m->studio = st;
	m->numseqs = st->numseqs;
	m->seqfirst = Mem_Alloc(sizeof(int) * (st->numseqs + 1));
#else
	m->frames = Mem_Alloc(sizeof(float) * 10 * m->numjoints * (size_t)BW_MAX(1, total));
#endif
	m->numanims = st->numseqs;
	m->anims = Mem_Alloc(sizeof(framegroup_t) * (m->numanims + 1));
	total = 0;
	for (int i = 0; i < st->numseqs; i++) {
		const studio_seq_t *sq = &st->seqs[i];
		framegroup_t *g = &m->anims[i];
		int n = BW_MAX(1, sq->numframes);
		Str_Copy(g->name, sq->label, sizeof(g->name));
		g->first = total;
		g->count = n;
		g->fps = sq->fps > 0 ? sq->fps : 10;
		g->loop = (sq->flags & STUDIO_LOOPING) != 0;
		if (m->seqfirst) {
			m->seqfirst[i] = total;
			total += n;
			continue;
		}
		for (int f = 0; f < n; f++) {
			float *out = m->frames + ((size_t)(total + f) * m->numjoints) * 10;
			/* a sequence kept in another file stands in its rest pose */
			if (!Studio_DecodeFrame(st, i, 0, f, out))
				Studio_RestPose(st, out);
		}
		total += n;
	}
	if (m->seqfirst)
		m->seqfirst[st->numseqs] = total;
	return true;
}

#ifdef BW_LOWMEM
/* decoded studio frames, found by a hash of model and frame and replaced
 * least recently used first. A frame decodes with six sines and cosines a
 * bone, and the players' animations loop, so the cache is made to hold
 * the frames the match plays: idle, run and the rest loop over a second
 * or two, and every player plays them. A 128 MB Xbox keeps 2560 (4 MB at
 * 40 bones), a 64 MB one 512; other small systems the 96 a pose and its
 * neighbours need. */
#define FRAMEHASH 1024
typedef struct {
	const skelmodel_t *model;
	int frame, size, next; /* next in its hash chain, + 1 */
	int older, newer;      /* the use list, + 1 */
	float *trs;
} cachedframe_t;
static cachedframe_t *framecache;
static int numframecache, maxframecache;
static int framehash[FRAMEHASH]; /* first entry + 1 of each chain, 0 for none */
static int newest, oldest;       /* ends of the use list, + 1 */

static int frame_capacity(void)
{
#if defined(BW_XBOX)
	return BW_SMALL_MEMORY() ? 512 : 2560;
#elif defined(BW_FORCE_LOWMEM)
	return 512; /* a desktop playing an Xbox: as on a 64 MB one */
#else
	return 96;
#endif
}

static unsigned frame_bucket(const skelmodel_t *m, int f)
{
	return ((uint32_t)(uintptr_t)m * 2654435761u ^ (uint32_t)f * 40503u) >> 8 & (FRAMEHASH - 1);
}

static void unhash_frame(int i)
{
	int *link = &framehash[frame_bucket(framecache[i].model, framecache[i].frame)];
	while (*link && *link != i + 1)
		link = &framecache[*link - 1].next;
	if (*link)
		*link = framecache[i].next;
	framecache[i].model = NULL;
}

static void unlink_use(int i)
{
	cachedframe_t *c = &framecache[i];
	if (c->older)
		framecache[c->older - 1].newer = c->newer;
	else
		oldest = c->newer;
	if (c->newer)
		framecache[c->newer - 1].older = c->older;
	else
		newest = c->older;
	c->older = c->newer = 0;
}

static void link_newest(int i)
{
	cachedframe_t *c = &framecache[i];
	c->older = newest;
	c->newer = 0;
	if (newest)
		framecache[newest - 1].newer = i + 1;
	else
		oldest = i + 1;
	newest = i + 1;
}

static const float *decode_frame(const skelmodel_t *m, int f)
{
	int i, s, lo = 0, hi = m->numseqs - 1, n = m->numjoints * 10;
	unsigned h = frame_bucket(m, f);
	for (i = framehash[h]; i; i = framecache[i - 1].next)
		if (framecache[i - 1].model == m && framecache[i - 1].frame == f) {
			if (newest != i) {
				unlink_use(i - 1);
				link_newest(i - 1);
			}
			return framecache[i - 1].trs;
		}
	if (!maxframecache) {
		maxframecache = frame_capacity();
		framecache = Mem_Alloc(sizeof(cachedframe_t) * (size_t)maxframecache);
	}
	/* a new entry while there is room, else the one used longest ago */
	if (numframecache < maxframecache) {
		i = numframecache++;
	} else {
		i = oldest - 1;
		unlink_use(i);
		if (framecache[i].model)
			unhash_frame(i);
	}
	cachedframe_t *c = &framecache[i];
	if (c->size < n) {
		c->trs = Mem_Realloc(c->trs, sizeof(float) * n);
		c->size = n;
	}
	/* the sequence holding frame f: the last one starting at or before it */
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (m->seqfirst[mid] <= f)
			lo = mid;
		else
			hi = mid - 1;
	}
	s = lo;
	/* a sequence kept in another file stands in its rest pose */
	if (!Studio_DecodeFrame(m->studio, s, 0, f - m->seqfirst[s], c->trs))
		Studio_RestPose(m->studio, c->trs);
	c->model = m;
	c->frame = f;
	c->next = framehash[h];
	framehash[h] = i + 1;
	link_newest(i);
	return c->trs;
}

/* a freed model's frames go to the old end, to be reused first */
static void forget_frames(const skelmodel_t *m)
{
	for (int i = 0; i < numframecache; i++)
		if (framecache[i].model == m) {
			unhash_frame(i);
			unlink_use(i);
			cachedframe_t *c = &framecache[i];
			c->newer = oldest;
			c->older = 0;
			if (oldest)
				framecache[oldest - 1].older = i + 1;
			else
				newest = i + 1;
			oldest = i + 1;
		}
}
#endif

/* the ten channels of every joint at raw frame f, which is in range */
static const float *frame_trs(const skelmodel_t *m, int f)
{
#ifdef BW_LOWMEM
	if (!m->frames)
		return decode_frame(m, f);
#endif
	return m->frames + (size_t)f * m->numjoints * 10;
}

skelmodel_t *Skel_LoadModel(const char *name)
{
	skelmodel_t *m;
	uint8_t *d;
	size_t size;
	bool ok;
	if (!name || !*name || name[0] == '*')
		return NULL;
	for (m = skelmodels; m; m = m->next)
		if (!Str_ICmp(m->name, name))
			return m->numjoints ? m : NULL;
	m = Mem_Alloc(sizeof(*m));
	Str_Copy(m->name, name, sizeof(m->name));
	m->next = skelmodels;
	skelmodels = m;
	{
		/* a GoldSrc model is parsed and kept by the studio cache already,
		   so its file need not be read a second time */
		const studio_model_t *st = Studio_Peek(name);
		if (st && st->version == 10 && from_studio(m, st))
			goto loaded;
	}
	d = (uint8_t *)FS_LoadFile(name, &size);
	if (d && size >= 8 && !memcmp(d, "IDST", 4) && d[4] == 10) {
		/* only a GoldSrc model goes to the studio cache, which keeps d */
		ok = from_studio(m, Studio_CacheBuffer(name, d, size));
		d = NULL;
	} else
		ok = d && parse_iqm(m, d, size);
	if (!ok && d && size >= 8 && !memcmp(d, "IDST", 4) && d[4] != 10) {
		/* a format a plugin turns into an IQM (Source .mdl) */
		size_t qlen;
		uint8_t *q = Plug_ConvertModel(name, d, size, &qlen);
		if (q)
			ok = parse_iqm(m, q, qlen);
		Mem_Free(q);
	}
	Mem_Free(d);
	if (!ok) {
		/* remembered as having no skeleton */
		Mem_Free(m->joints), m->joints = NULL;
		Mem_Free(m->base), m->base = NULL;
		Mem_Free(m->baserel), m->baserel = NULL;
		Mem_Free(m->invbind), m->invbind = NULL;
		m->numjoints = 0;
		return NULL;
	}
loaded:
	{
		int n;
		framegroup_t *g = Skel_LoadFramegroups(name, &n);
		if (g) {
			Mem_Free(m->anims);
			m->anims = g;
			m->numanims = n;
		}
	}
	return m;
}

void Skel_FreeModels(void)
{
	while (skelmodels) {
		skelmodel_t *n = skelmodels->next;
#ifdef BW_LOWMEM
		forget_frames(skelmodels);
#endif
		free_skelmodel(skelmodels);
		skelmodels = n;
	}
}

int Skel_FindJoint(const skelmodel_t *m, const char *name)
{
	if (m)
		for (int j = 0; j < m->numjoints; j++)
			if (!Str_ICmp(m->joints[j].name, name))
				return j;
	return -1;
}

/* --- poses ------------------------------------------------------------------- */

#ifndef SKEL_SSE
static void lerp_trs(const float *a, const float *b, float t, float *out)
{
	float dot = a[3] * b[3] + a[4] * b[4] + a[5] * b[5] + a[6] * b[6];
	float s = dot < 0 ? -1.0f : 1.0f; /* the shorter way round */
	for (int k = 0; k < 10; k++) {
		float bk = k >= 3 && k < 7 ? b[k] * s : b[k];
		out[k] = a[k] + (bk - a[k]) * t;
	}
}
#else
/* lerp_trs then Bone_FromTRS, four lanes at a time; b times -1 is b with
 * its sign flipped, which the plain C's multiply gives too */
static void lerp_bone(const float *a, const float *b, float t, bonemat_t m)
{
	float dot = a[3] * b[3] + a[4] * b[4] + a[5] * b[5] + a[6] * b[6];
	const __m128 neg = _mm_set1_ps(-0.0f), tv = _mm_set1_ps(t);
	__m128 flip0 = _mm_setzero_ps(), flip1 = _mm_setzero_ps();
	float trs[12];
	if (dot < 0) /* the shorter way round */
		flip0 = _mm_and_ps(sse_mask(0, 0, 0, 1), neg), flip1 = _mm_and_ps(sse_mask(1, 1, 1, 0), neg);
	__m128 a0 = _mm_loadu_ps(a), a1 = _mm_loadu_ps(a + 4);
	__m128 b0 = _mm_xor_ps(_mm_loadu_ps(b), flip0), b1 = _mm_xor_ps(_mm_loadu_ps(b + 4), flip1);
	_mm_storeu_ps(trs, _mm_add_ps(a0, _mm_mul_ps(_mm_sub_ps(b0, a0), tv)));
	_mm_storeu_ps(trs + 4, _mm_add_ps(a1, _mm_mul_ps(_mm_sub_ps(b1, a1), tv)));
	trs[8] = a[8] + (b[8] - a[8]) * t;
	trs[9] = a[9] + (b[9] - a[9]) * t;
	Bone_FromTRS(m, trs);
}
#endif

/* bones first..last of Skel_SampleRelative's pose; each bone is worked
 * out on its own, so part of the skeleton comes out as in the whole */
static void sample_range(const skelmodel_t *m, int group, float time, int first, int last, bonemat_t *out)
{
	const framegroup_t *g;
	int f0, f1;
	float t;
	if (last < first)
		return;
	if (!m->numframes || group < 0 || group >= m->numanims) {
		if (m->numjoints) /* baserel is NULL without a skeleton */
			memcpy(out + first, m->baserel + first, sizeof(bonemat_t) * (size_t)(last - first + 1));
		return;
	}
	g = &m->anims[group];
	t = time * g->fps;
	if (!(t >= 0)) /* also catches NaN */
		t = 0;
	/* for t >= 0 truncating is the floor, without the libm call */
	f0 = t < 2147483520.0f ? (int)t : (int)floorf(t);
	t -= (float)f0;
	if (g->loop) {
		f0 %= g->count;
		f1 = (f0 + 1) % g->count;
	} else {
		if (f0 >= g->count - 1)
			f0 = g->count - 1, t = 0;
		f1 = BW_MIN(f0 + 1, g->count - 1);
	}
	f0 = BW_MAX(0, BW_MIN(g->first + f0, m->numframes - 1));
	f1 = BW_MAX(0, BW_MIN(g->first + f1, m->numframes - 1));
	/* a is the most recently used cache entry, so decoding b keeps it */
	const float *a = frame_trs(m, f0), *b = frame_trs(m, f1);
	for (int j = first; j <= last; j++) {
#ifdef SKEL_SSE
		lerp_bone(a + j * 10, b + j * 10, t, out[j]);
#else
		float trs[10];
		lerp_trs(a + j * 10, b + j * 10, t, trs);
		Bone_FromTRS(out[j], trs);
#endif
	}
}

void Skel_SampleRelative(const skelmodel_t *m, int group, float time, bonemat_t *out)
{
	sample_range(m, group, time, 0, m->numjoints - 1, out);
}

void Skel_EntityPoseRange(const skelmodel_t *m, int frame, float frame1time, int frame2, float frame2time,
                          float lerpfrac, int first, int last, bonemat_t *out)
{
	static bonemat_t *b;
	static int maxb;
	first = BW_MAX(first, 0);
	last = BW_MIN(last, m->numjoints - 1);
	sample_range(m, frame, frame1time, first, last, out);
	if (lerpfrac > 0 && frame2 != frame) {
		float l = BW_MIN(lerpfrac, 1);
		if (m->numjoints > maxb) {
			maxb = m->numjoints;
			b = Mem_Realloc(b, sizeof(bonemat_t) * maxb);
		}
		sample_range(m, frame2, frame2time, first, last, b);
		for (int j = first; j <= last; j++) {
#ifdef SKEL_SSE
			const __m128 lv = _mm_set1_ps(l);
			for (int k = 0; k < 12; k += 4) {
				__m128 o = _mm_loadu_ps(out[j] + k);
				_mm_storeu_ps(out[j] + k, _mm_add_ps(o, _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(b[j] + k), o), lv)));
			}
#else
			for (int k = 0; k < 12; k++)
				out[j][k] += (b[j][k] - out[j][k]) * l;
#endif
		}
	}
}

void Skel_EntityPose(const skelmodel_t *m, int frame, float frame1time, int frame2, float frame2time,
                     float lerpfrac, bonemat_t *out)
{
	Skel_EntityPoseRange(m, frame, frame1time, frame2, frame2time, lerpfrac, 0, m->numjoints - 1, out);
}

void Skel_ToAbsolute(const skelmodel_t *m, const bonemat_t *rel, bonemat_t *abs)
{
	for (int j = 0; j < m->numjoints; j++) {
		int p = m->joints[j].parent;
		if (p >= 0)
			Bone_Multiply(abs[j], abs[p], rel[j]);
		else
			memcpy(abs[j], rel[j], sizeof(bonemat_t));
	}
}
