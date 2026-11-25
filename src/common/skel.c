/* Bulwark engine - skeletal models: IQM joints, poses and animations, and
 * the bone maths the skeleton builtins and the renderer share. ISC licence,
 * see LICENSE. */
#include "skel.h"
#include "studio.h"
#include "fs.h"
#include "cmd.h"

#include <math.h>
#include <stdio.h>

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

/* the IQM convention: output = rotate(input * scale) + translate */
void Bone_FromTRS(bonemat_t m, const float *trs)
{
	float x = trs[3], y = trs[4], z = trs[5], w = trs[6];
	float l = x * x + y * y + z * z + w * w;
	float r[3][3];
	if (l > 0 && fabsf(l - 1) > 1e-6f) {
		l = 1 / sqrtf(l);
		x *= l, y *= l, z *= l, w *= l;
	}
	r[0][0] = 1 - 2 * (y * y + z * z), r[0][1] = 2 * (x * y - z * w), r[0][2] = 2 * (x * z + y * w);
	r[1][0] = 2 * (x * y + z * w), r[1][1] = 1 - 2 * (x * x + z * z), r[1][2] = 2 * (y * z - x * w);
	r[2][0] = 2 * (x * z - y * w), r[2][1] = 2 * (y * z + x * w), r[2][2] = 1 - 2 * (x * x + y * y);
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++)
			m[i * 4 + j] = r[i][j] * trs[7 + j];
		m[i * 4 + 3] = trs[i];
	}
}

void Bone_Multiply(bonemat_t out, const bonemat_t a, const bonemat_t b)
{
	bonemat_t o;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++)
			o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
		o[i * 4 + 3] += a[i * 4 + 3];
	}
	memcpy(out, o, sizeof(o));
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
	{
		bonemat_t *rel = Mem_Alloc(sizeof(bonemat_t) * m->numjoints);
		for (int j = 0; j < m->numjoints; j++)
			Bone_FromTRS(rel[j], m->base + j * 10);
		Skel_ToAbsolute(m, rel, m->invbind);
		for (int j = 0; j < m->numjoints; j++)
			Bone_Invert(m->invbind[j], m->invbind[j]);
		Mem_Free(rel);
	}
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
	{
		bonemat_t *rel = Mem_Alloc(sizeof(bonemat_t) * m->numjoints);
		for (int j = 0; j < m->numjoints; j++)
			Bone_FromTRS(rel[j], m->base + j * 10);
		Skel_ToAbsolute(m, rel, m->invbind);
		for (int j = 0; j < m->numjoints; j++)
			Bone_Invert(m->invbind[j], m->invbind[j]);
		Mem_Free(rel);
	}
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
/* decoded studio frames, most recently used first; a pose takes two at a
 * time, and several entities on one model tend to share them */
#define FRAMECACHE 8
typedef struct {
	const skelmodel_t *model;
	int frame, size;
	float *trs;
} cachedframe_t;
static cachedframe_t framecache[FRAMECACHE];

static const float *decode_frame(const skelmodel_t *m, int f)
{
	int i, s, lo = 0, hi = m->numseqs - 1, n = m->numjoints * 10;
	for (i = 0; i < FRAMECACHE; i++)
		if (framecache[i].model == m && framecache[i].frame == f)
			break;
	if (i == FRAMECACHE) {
		i = FRAMECACHE - 1;
		if (framecache[i].size < n) {
			framecache[i].trs = Mem_Realloc(framecache[i].trs, sizeof(float) * n);
			framecache[i].size = n;
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
		if (!Studio_DecodeFrame(m->studio, s, 0, f - m->seqfirst[s], framecache[i].trs))
			Studio_RestPose(m->studio, framecache[i].trs);
		framecache[i].model = m;
		framecache[i].frame = f;
	}
	if (i) {
		/* to the front */
		cachedframe_t e = framecache[i];
		memmove(&framecache[1], &framecache[0], sizeof(framecache[0]) * i);
		framecache[0] = e;
	}
	return framecache[0].trs;
}

static void forget_frames(const skelmodel_t *m)
{
	for (int i = 0; i < FRAMECACHE; i++)
		if (framecache[i].model == m)
			framecache[i].model = NULL;
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

static void lerp_trs(const float *a, const float *b, float t, float *out)
{
	float dot = a[3] * b[3] + a[4] * b[4] + a[5] * b[5] + a[6] * b[6];
	float s = dot < 0 ? -1.0f : 1.0f; /* the shorter way round */
	for (int k = 0; k < 10; k++) {
		float bk = k >= 3 && k < 7 ? b[k] * s : b[k];
		out[k] = a[k] + (bk - a[k]) * t;
	}
}

void Skel_SampleRelative(const skelmodel_t *m, int group, float time, bonemat_t *out)
{
	const framegroup_t *g;
	int f0, f1;
	float t;
	if (!m->numframes || group < 0 || group >= m->numanims) {
		for (int j = 0; j < m->numjoints; j++)
			Bone_FromTRS(out[j], m->base + j * 10);
		return;
	}
	g = &m->anims[group];
	t = time * g->fps;
	if (!(t >= 0)) /* also catches NaN */
		t = 0;
	f0 = (int)floorf(t);
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
	{
		const float *a = frame_trs(m, f0), *b = frame_trs(m, f1);
		for (int j = 0; j < m->numjoints; j++) {
			float trs[10];
			lerp_trs(a + j * 10, b + j * 10, t, trs);
			Bone_FromTRS(out[j], trs);
		}
	}
}

void Skel_EntityPose(const skelmodel_t *m, int frame, float frame1time, int frame2, float frame2time,
                     float lerpfrac, bonemat_t *out)
{
	Skel_SampleRelative(m, frame, frame1time, out);
	if (lerpfrac > 0 && frame2 != frame) {
		bonemat_t *b = Mem_Alloc(sizeof(bonemat_t) * m->numjoints);
		float l = BW_MIN(lerpfrac, 1);
		Skel_SampleRelative(m, frame2, frame2time, b);
		for (int j = 0; j < m->numjoints; j++)
			for (int k = 0; k < 12; k++)
				out[j][k] += (b[j][k] - out[j][k]) * l;
		Mem_Free(b);
	}
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
