/* Bulwark engine - GoldSrc-era studio models (IDST v10) and CSO appended
 * sequence files. Layouts: docs/spec/studio-mdl.md. ISC licence, see LICENSE. */
#include "studio.h"
#include "fs.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

typedef struct {
	const uint8_t *buf;
	size_t len;
	char *err;
	size_t errlen;
} rd_t;

static int32_t rdi(const uint8_t *p) { return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }
static int16_t rds(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static uint16_t rdus(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static float rdf(const uint8_t *p)
{
	float f;
	int32_t i = rdi(p);
	memcpy(&f, &i, 4);
	return f;
}

static bool fail(rd_t *r, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(r->err, r->errlen, fmt, ap);
	va_end(ap);
	return false;
}

/* true if count records of size bytes at off lie inside the buffer */
static bool span(const rd_t *r, int64_t off, int64_t count, int64_t size)
{
	if (off < 0 || count < 0 || size < 0)
		return false;
	return off + count * size <= (int64_t)r->len;
}

static void rdname(char *dst, size_t dstlen, const uint8_t *src, size_t srclen)
{
	size_t n = 0;
	while (n < srclen && n + 1 < dstlen && src[n])
		n++;
	memcpy(dst, src, n);
	dst[n] = 0;
}

static void rdvec(float *v, const uint8_t *p)
{
	v[0] = rdf(p), v[1] = rdf(p + 4), v[2] = rdf(p + 8);
}

/* header field offsets, see docs/spec/studio-mdl.md */
enum {
	H_NAME = 8, H_LENGTH = 72, H_EYE = 76, H_MINS = 88, H_MAXS = 100, H_BBMINS = 112, H_BBMAXS = 124,
	H_FLAGS = 136, H_BONES = 140, H_CONTROLLERS = 148, H_HITBOXES = 156, H_SEQS = 164,
	H_SEQGROUPS = 172, H_TEXTURES = 180, H_TEXDATA = 188, H_SKINREF = 192, H_SKINFAM = 196,
	H_SKININDEX = 200, H_BODYPARTS = 204, H_ATTACH = 212, H_SIZE = 244
};
enum { BONE_SIZE = 112, CTRL_SIZE = 24, HITBOX_SIZE = 32, SEQ_SIZE = 176, EVENT_SIZE = 76,
       SEQGROUP_SIZE = 104, TEX_SIZE = 80, BODYPART_SIZE = 76, SUBMODEL_SIZE = 112,
       MESH_SIZE = 20, ATTACH_SIZE = 88 };
/* sanity limits; real content is far below them */
enum { MAX_COUNT = 65536, MAX_TEX_DIM = 4096 };

static bool count_ok(rd_t *r, const char *what, int n, int idx, int size)
{
	if (n < 0 || n > MAX_COUNT)
		return fail(r, "%s count %d out of range", what, n);
	if (n && !span(r, idx, n, size))
		return fail(r, "%s table (%d at %d) runs past the end", what, n, idx);
	return true;
}

/* one sequence descriptor; events and animation are resolved by the caller */
static void parse_seqdesc(studio_seq_t *s, const uint8_t *p)
{
	rdname(s->label, sizeof(s->label), p, 32);
	s->fps = rdf(p + 32);
	s->flags = rdi(p + 36);
	s->activity = rdi(p + 40);
	s->actweight = rdi(p + 44);
	s->numevents = rdi(p + 48);
	s->numframes = rdi(p + 56);
	s->motiontype = rdi(p + 68);
	s->motionbone = rdi(p + 72);
	rdvec(s->linearmovement, p + 76);
	rdvec(s->mins, p + 96);
	rdvec(s->maxs, p + 108);
	s->numblends = rdi(p + 120);
	s->blendtype[0] = rdi(p + 128), s->blendtype[1] = rdi(p + 132);
	s->blendstart[0] = rdf(p + 136), s->blendstart[1] = rdf(p + 140);
	s->blendend[0] = rdf(p + 144), s->blendend[1] = rdf(p + 148);
	s->seqgroup = rdi(p + 156);
	s->entrynode = rdi(p + 160);
	s->exitnode = rdi(p + 164);
	s->nodeflags = rdi(p + 168);
	s->nextseq = rdi(p + 172);
}

static bool seq_sane(rd_t *r, const studio_seq_t *s, int i)
{
	if (s->numframes < 1 || s->numframes > MAX_COUNT)
		return fail(r, "sequence %d: %d frames", i, s->numframes);
	if (s->numblends < 1 || s->numblends > STUDIO_MAX_BLENDS)
		return fail(r, "sequence %d: %d blends", i, s->numblends);
	if (s->numevents < 0 || s->numevents > MAX_COUNT)
		return fail(r, "sequence %d: %d events", i, s->numevents);
	if (!isfinite(s->fps) || s->fps < 0)
		return fail(r, "sequence %d: bad fps", i);
	return true;
}

static bool parse_events(rd_t *r, studio_seq_t *s, int64_t idx, int i)
{
	if (!s->numevents)
		return true;
	if (!span(r, idx, s->numevents, EVENT_SIZE))
		return fail(r, "sequence %d: events run past the end", i);
	s->events = Mem_Alloc(sizeof(studio_event_t) * s->numevents);
	for (int e = 0; e < s->numevents; e++) {
		const uint8_t *p = r->buf + idx + (int64_t)e * EVENT_SIZE;
		s->events[e].frame = rdi(p);
		s->events[e].event = rdi(p + 4);
		s->events[e].type = rdi(p + 8);
		rdname(s->events[e].options, sizeof(s->events[e].options), p + 12, 64);
	}
	return true;
}

static bool parse_meshes(rd_t *r, studio_submodel_t *sm, int nummesh, int meshindex, const char *what)
{
	if (!count_ok(r, what, nummesh, meshindex, MESH_SIZE))
		return false;
	sm->nummeshes = nummesh;
	sm->meshes = Mem_Alloc(sizeof(studio_mesh_t) * (nummesh ? nummesh : 1));
	for (int k = 0; k < nummesh; k++) {
		const uint8_t *p = r->buf + meshindex + k * MESH_SIZE;
		int triindex = rdi(p + 4);
		sm->meshes[k].numtris_cmds = rdi(p);
		sm->meshes[k].skinref = rdi(p + 8);
		if (triindex < 0 || (size_t)triindex >= r->len)
			return fail(r, "%s mesh %d: triangle commands at %d outside the file", what, k, triindex);
		sm->meshes[k].cmds = r->buf + triindex;
		sm->meshes[k].cmdslimit = r->buf + r->len;
	}
	return true;
}

/* the texture and skin tables, from a model or its companion texture file */
static bool parse_textures(rd_t *r, studio_model_t *m)
{
	const uint8_t *buf = r->buf;
	int n = rdi(buf + H_TEXTURES), idx = rdi(buf + H_TEXTURES + 4);
	if (!count_ok(r, "texture", n, idx, TEX_SIZE))
		return false;
	m->numtextures = n;
	m->textures = Mem_Alloc(sizeof(studio_texture_t) * (n ? n : 1));
	for (int i = 0; i < n; i++) {
		const uint8_t *p = buf + idx + i * TEX_SIZE;
		studio_texture_t *t = &m->textures[i];
		rdname(t->name, sizeof(t->name), p, 64);
		t->flags = rdi(p + 64);
		t->width = rdi(p + 68);
		t->height = rdi(p + 72);
		int64_t data = rdi(p + 76);
		if (t->width < 1 || t->height < 1 || t->width > MAX_TEX_DIM || t->height > MAX_TEX_DIM)
			return fail(r, "texture %d: size %dx%d", i, t->width, t->height);
		if (!span(r, data, (int64_t)t->width * t->height + 768, 1))
			return fail(r, "texture %d: pixels run past the end", i);
		t->pixels = buf + data;
		t->palette = t->pixels + (int64_t)t->width * t->height;
	}
	m->numskinref = rdi(buf + H_SKINREF);
	m->numskinfamilies = rdi(buf + H_SKINFAM);
	idx = rdi(buf + H_SKININDEX);
	if (m->numskinref < 0 || m->numskinfamilies < 0 || m->numskinref > 1024 || m->numskinfamilies > 1024)
		return fail(r, "skin table %dx%d", m->numskinfamilies, m->numskinref);
	if (!span(r, idx, (int64_t)m->numskinref * m->numskinfamilies, 2))
		return fail(r, "skin table runs past the end");
	m->skins = Mem_Alloc(sizeof(int16_t) * (m->numskinref * m->numskinfamilies + 1));
	for (int i = 0; i < m->numskinref * m->numskinfamilies; i++) {
		m->skins[i] = rds(buf + idx + 2 * i);
		if (m->numtextures && (m->skins[i] < 0 || m->skins[i] >= m->numtextures))
			return fail(r, "skin entry %d: texture %d out of range", i, m->skins[i]);
	}

	return true;
}

static bool parse(studio_model_t *m, const uint8_t *buf, size_t len, char *err, size_t errlen)
{
	rd_t r = {buf, len, err, errlen};
	if (len < H_SIZE)
		return fail(&r, "file too small for a header (%zu bytes)", len);
	if (memcmp(buf, "IDST", 4))
		return fail(&r, "not a studio model");
	m->version = rdi(buf + 4);
	if (m->version != 10)
		return fail(&r, "studio version %d is not supported", m->version);
	rdname(m->name, sizeof(m->name), buf + H_NAME, 64);
	if (rdi(buf + H_LENGTH) != (int32_t)len)
		return fail(&r, "header length %d does not match the file size %zu", rdi(buf + H_LENGTH), len);
	rdvec(m->eye, buf + H_EYE);
	rdvec(m->mins, buf + H_MINS);
	rdvec(m->maxs, buf + H_MAXS);
	rdvec(m->bbmins, buf + H_BBMINS);
	rdvec(m->bbmaxs, buf + H_BBMAXS);
	m->flags = rdi(buf + H_FLAGS);

	/* bones */
	int n = rdi(buf + H_BONES), idx = rdi(buf + H_BONES + 4);
	if (n < 1 || n > STUDIO_MAX_BONES)
		return fail(&r, "%d bones", n);
	if (!count_ok(&r, "bone", n, idx, BONE_SIZE))
		return false;
	m->numbones = n;
	m->bones = Mem_Alloc(sizeof(studio_bone_t) * n);
	for (int i = 0; i < n; i++) {
		const uint8_t *p = buf + idx + i * BONE_SIZE;
		studio_bone_t *b = &m->bones[i];
		rdname(b->name, sizeof(b->name), p, 32);
		b->parent = rdi(p + 32);
		if (b->parent < -1 || b->parent >= i)
			return fail(&r, "bone %d: parent %d is not an earlier bone", i, b->parent);
		for (int k = 0; k < 6; k++) {
			b->controller[k] = rdi(p + 40 + 4 * k);
			b->value[k] = rdf(p + 64 + 4 * k);
			b->scale[k] = rdf(p + 88 + 4 * k);
			if (!isfinite(b->value[k]) || !isfinite(b->scale[k]))
				return fail(&r, "bone %d: non-finite rest value", i);
		}
	}
	n = rdi(buf + H_CONTROLLERS), idx = rdi(buf + H_CONTROLLERS + 4);
	if (!count_ok(&r, "bone controller", n, idx, CTRL_SIZE))
		return false;

	/* hitboxes */
	n = rdi(buf + H_HITBOXES), idx = rdi(buf + H_HITBOXES + 4);
	if (!count_ok(&r, "hitbox", n, idx, HITBOX_SIZE))
		return false;
	m->numhitboxes = n;
	m->hitboxes = Mem_Alloc(sizeof(studio_hitbox_t) * (n ? n : 1));
	for (int i = 0; i < n; i++) {
		const uint8_t *p = buf + idx + i * HITBOX_SIZE;
		m->hitboxes[i].bone = rdi(p);
		m->hitboxes[i].group = rdi(p + 4);
		rdvec(m->hitboxes[i].mins, p + 8);
		rdvec(m->hitboxes[i].maxs, p + 20);
		if (m->hitboxes[i].bone < 0 || m->hitboxes[i].bone >= m->numbones)
			return fail(&r, "hitbox %d: bone %d out of range", i, m->hitboxes[i].bone);
	}

	/* sequence groups: group 0 is the model itself; others are separate
	 * files this loader does not open (none of the CSO models probed have any) */
	n = rdi(buf + H_SEQGROUPS), idx = rdi(buf + H_SEQGROUPS + 4);
	if (n < 1)
		return fail(&r, "%d sequence groups", n);
	if (!count_ok(&r, "sequence group", n, idx, SEQGROUP_SIZE))
		return false;
	m->numseqgroups = n;

	/* sequences */
	n = rdi(buf + H_SEQS), idx = rdi(buf + H_SEQS + 4);
	if (!count_ok(&r, "sequence", n, idx, SEQ_SIZE))
		return false;
	m->numseqs = n;
	m->seqs = Mem_Alloc(sizeof(studio_seq_t) * (n ? n : 1));
	for (int i = 0; i < n; i++) {
		const uint8_t *p = buf + idx + i * SEQ_SIZE;
		studio_seq_t *s = &m->seqs[i];
		parse_seqdesc(s, p);
		if (!seq_sane(&r, s, i))
			return false;
		if (s->seqgroup < 0 || s->seqgroup >= m->numseqgroups)
			return fail(&r, "sequence %d: group %d out of range", i, s->seqgroup);
		if (!parse_events(&r, s, rdi(p + 52), i))
			return false;
		if (s->seqgroup == 0) {
			int64_t a = rdi(p + 124);
			if (!span(&r, a, (int64_t)s->numblends * m->numbones, 12))
				return fail(&r, "sequence %d: animation at %lld runs past the end", i, (long long)a);
			s->anim = buf + a;
			s->animlimit = buf + len;
		}
	}

	/* textures (a model with none keeps them in a companion file) */
	if (!parse_textures(&r, m))
		return false;

	/* body parts, sub-models and meshes */
	n = rdi(buf + H_BODYPARTS), idx = rdi(buf + H_BODYPARTS + 4);
	if (!count_ok(&r, "body part", n, idx, BODYPART_SIZE))
		return false;
	m->numbodyparts = n;
	m->bodyparts = Mem_Alloc(sizeof(studio_bodypart_t) * (n ? n : 1));
	for (int i = 0; i < n; i++) {
		const uint8_t *p = buf + idx + i * BODYPART_SIZE;
		studio_bodypart_t *bp = &m->bodyparts[i];
		rdname(bp->name, sizeof(bp->name), p, 64);
		int nummodels = rdi(p + 64), mi = rdi(p + 72);
		bp->base = rdi(p + 68);
		if (!count_ok(&r, "sub-model", nummodels, mi, SUBMODEL_SIZE))
			return false;
		bp->models = Mem_Alloc(sizeof(studio_submodel_t) * (nummodels ? nummodels : 1));
		bp->nummodels = nummodels;
		for (int j = 0; j < bp->nummodels; j++) {
			const uint8_t *q = buf + mi + j * SUBMODEL_SIZE;
			studio_submodel_t *sm = &bp->models[j];
			rdname(sm->name, sizeof(sm->name), q, 64);
			int nummesh = rdi(q + 72), meshindex = rdi(q + 76);
			sm->numverts = rdi(q + 80);
			int vinfo = rdi(q + 84), vidx = rdi(q + 88);
			sm->numnorms = rdi(q + 92);
			int ninfo = rdi(q + 96), nidx = rdi(q + 100);
			if (sm->numverts < 0 || sm->numverts > MAX_COUNT || sm->numnorms < 0 || sm->numnorms > MAX_COUNT)
				return fail(&r, "sub-model %s: %d verts, %d normals", sm->name, sm->numverts, sm->numnorms);
			if (!span(&r, vinfo, sm->numverts, 1) || !span(&r, vidx, sm->numverts, 12) ||
			    !span(&r, ninfo, sm->numnorms, 1) || !span(&r, nidx, sm->numnorms, 12))
				return fail(&r, "sub-model %s: vertex data runs past the end", sm->name);
			sm->vertbone = buf + vinfo;
			sm->verts = buf + vidx;
			sm->normbone = buf + ninfo;
			sm->norms = buf + nidx;
			for (int v = 0; v < sm->numverts; v++)
				if (sm->vertbone[v] >= m->numbones)
					return fail(&r, "sub-model %s: vertex %d on bone %d", sm->name, v, sm->vertbone[v]);
			for (int v = 0; v < sm->numnorms; v++)
				if (sm->normbone[v] >= m->numbones)
					return fail(&r, "sub-model %s: normal %d on bone %d", sm->name, v, sm->normbone[v]);
			if (!parse_meshes(&r, sm, nummesh, meshindex, sm->name))
				return false;
			for (int k = 0; k < sm->nummeshes; k++) {
				if (Studio_CountTris(m, &sm->meshes[k], sm->numverts, sm->numnorms) < 0)
					return fail(&r, "sub-model %s mesh %d: malformed triangle commands", sm->name, k);
				if (sm->meshes[k].skinref < 0 || (m->numskinref && sm->meshes[k].skinref >= m->numskinref))
					return fail(&r, "sub-model %s mesh %d: skin ref %d", sm->name, k, sm->meshes[k].skinref);
			}
		}
	}

	/* attachments */
	n = rdi(buf + H_ATTACH), idx = rdi(buf + H_ATTACH + 4);
	if (!count_ok(&r, "attachment", n, idx, ATTACH_SIZE))
		return false;
	m->numattachments = n;
	m->attachments = Mem_Alloc(sizeof(studio_attachment_t) * (n ? n : 1));
	for (int i = 0; i < n; i++) {
		const uint8_t *p = buf + idx + i * ATTACH_SIZE;
		rdname(m->attachments[i].name, sizeof(m->attachments[i].name), p, 32);
		m->attachments[i].bone = rdi(p + 36);
		rdvec(m->attachments[i].org, p + 40);
		if (m->attachments[i].bone < 0 || m->attachments[i].bone >= m->numbones)
			return fail(&r, "attachment %d: bone %d out of range", i, m->attachments[i].bone);
	}
	return true;
}

bool Studio_ParseTextures(studio_model_t *m, const uint8_t *buf, size_t len, char *err, size_t errlen)
{
	rd_t r = {buf, len, err, errlen};
	if (len < H_SIZE || memcmp(buf, "IDST", 4) || rdi(buf + 4) != 10)
		return fail(&r, "not a version 10 studio file");
	Mem_Free(m->textures);
	Mem_Free(m->skins);
	m->textures = NULL, m->skins = NULL;
	m->numtextures = 0;
	if (parse_textures(&r, m))
		return true;
	m->numtextures = m->numskinref = m->numskinfamilies = 0;
	return false;
}

bool Studio_Parse(studio_model_t *m, const uint8_t *buf, size_t len, char *err, size_t errlen)
{
	memset(m, 0, sizeof(*m));
	if (parse(m, buf, len, err, errlen))
		return true;
	Studio_Free(m);
	return false;
}

/* CSO .seq: i32 count, i32 offset of the trailing descriptor table (which
 * ends exactly at EOF), animation data from byte 8. Descriptor animation
 * offsets are relative to an origin that the first one maps to byte 8. */
bool Studio_AppendSeq(studio_model_t *m, const uint8_t *buf, size_t len, char *err, size_t errlen)
{
	rd_t r = {buf, len, err, errlen};
	if (len < 8)
		return fail(&r, ".seq too small (%zu bytes)", len);
	int n = rdi(buf), seqindex = rdi(buf + 4);
	if (n < 1 || n > 4096)
		return fail(&r, ".seq: %d sequences", n);
	if (seqindex < 8 || (int64_t)seqindex + (int64_t)n * SEQ_SIZE != (int64_t)len)
		return fail(&r, ".seq: descriptor table at %d x %d does not end at EOF (%zu)", seqindex, n, len);
	int64_t origin = (int64_t)rdi(buf + seqindex + 124) - 8;
	studio_seq_t *seqs = Mem_Alloc(sizeof(studio_seq_t) * (m->numseqs + n));
	memcpy(seqs, m->seqs, sizeof(studio_seq_t) * m->numseqs);
	for (int i = 0; i < n; i++) {
		const uint8_t *p = buf + seqindex + i * SEQ_SIZE;
		studio_seq_t *s = &seqs[m->numseqs + i];
		parse_seqdesc(s, p);
		s->source = m->numappended + 1;
		bool ok = seq_sane(&r, s, m->numseqs + i);
		/* no probed .seq has events, so where their offsets point is unknown */
		if (ok && s->numevents)
			ok = fail(&r, ".seq sequence %d has %d events; event layout unknown", i, s->numevents);
		int64_t a = (int64_t)rdi(p + 124) - origin;
		if (ok && (a < 8 || a + (int64_t)s->numblends * m->numbones * 12 > seqindex))
			ok = fail(&r, ".seq sequence %d: animation at %lld outside the data region", i, (long long)a);
		if (!ok) {
			for (int j = m->numseqs; j < m->numseqs + i; j++)
				Mem_Free(seqs[j].events);
			Mem_Free(seqs);
			return false;
		}
		s->seqgroup = 0;
		s->anim = buf + a;
		s->animlimit = buf + seqindex;
	}
	Mem_Free(m->seqs);
	m->seqs = seqs;
	m->numseqs += n;
	m->numappended++;
	return true;
}

/* finds the value of a run-length coded channel at frame; p points at the
 * channel's first run. false if a run is malformed or leaves [p, limit) */
static bool anim_value(const uint8_t *p, const uint8_t *limit, int frame, int16_t *out)
{
	for (;;) {
		if (p + 2 > limit)
			return false;
		int valid = p[0], total = p[1];
		if (total == 0 || valid > total)
			return false;
		if (frame < total) {
			const uint8_t *v = p + 2 * (frame < valid ? frame + 1 : valid);
			if (v + 2 > limit)
				return false;
			*out = rds(v);
			return true;
		}
		frame -= total;
		p += 2 * (valid + 1);
	}
}

static const uint8_t *channel(const studio_model_t *m, const studio_seq_t *s, int blend, int bone, int k)
{
	const uint8_t *blk = s->anim + 12 * ((size_t)blend * m->numbones + bone);
	uint16_t off = rdus(blk + 2 * k);
	return off ? blk + off : NULL;
}

bool Studio_ValidateSeq(const studio_model_t *m, int seq, char *err, size_t errlen)
{
	rd_t r = {NULL, 0, err, errlen};
	if (seq < 0 || seq >= m->numseqs)
		return fail(&r, "no sequence %d", seq);
	const studio_seq_t *s = &m->seqs[seq];
	if (!s->anim)
		return fail(&r, "sequence %d (%s) is in external group %d", seq, s->label, s->seqgroup);
	if (s->anim + 12 * (size_t)s->numblends * m->numbones > s->animlimit)
		return fail(&r, "sequence %d: channel table runs past its region", seq);
	for (int b = 0; b < s->numblends; b++)
		for (int j = 0; j < m->numbones; j++)
			for (int k = 0; k < 6; k++) {
				const uint8_t *p = channel(m, s, b, j, k);
				if (!p)
					continue;
				/* walk the runs once instead of per frame */
				int covered = 0;
				while (covered < s->numframes) {
					if (p + 2 > s->animlimit || p[1] == 0 || p[0] > p[1])
						return fail(&r, "sequence %d (%s) blend %d bone %d channel %d: bad run at frame %d",
						            seq, s->label, b, j, k, covered);
					covered += p[1];
					p += 2 * (p[0] + 1);
					if (p > s->animlimit)
						return fail(&r, "sequence %d (%s) bone %d channel %d: values run past the region",
						            seq, s->label, j, k);
				}
			}
	return true;
}

/* rotation from euler angles about x, then y, then z (radians) */
static void euler_to_quat(const float *a, float *q)
{
	float sr = sinf(a[0] * 0.5f), cr = cosf(a[0] * 0.5f);
	float sp = sinf(a[1] * 0.5f), cp = cosf(a[1] * 0.5f);
	float sy = sinf(a[2] * 0.5f), cy = cosf(a[2] * 0.5f);
	q[0] = sr * cp * cy - cr * sp * sy;
	q[1] = cr * sp * cy + sr * cp * sy;
	q[2] = cr * cp * sy - sr * sp * cy;
	q[3] = cr * cp * cy + sr * sp * sy;
}

bool Studio_DecodeFrame(const studio_model_t *m, int seq, int blend, int frame, float *out)
{
	if (seq < 0 || seq >= m->numseqs)
		return false;
	const studio_seq_t *s = &m->seqs[seq];
	if (!s->anim || blend < 0 || blend >= s->numblends)
		return false;
	if (frame < 0)
		frame = 0;
	if (frame >= s->numframes)
		frame = s->numframes - 1;
	for (int j = 0; j < m->numbones; j++) {
		const studio_bone_t *b = &m->bones[j];
		float v[6];
		for (int k = 0; k < 6; k++) {
			const uint8_t *p = channel(m, s, blend, j, k);
			int16_t raw = 0;
			v[k] = b->value[k];
			if (p) {
				if (!anim_value(p, s->animlimit, frame, &raw))
					return false;
				v[k] += raw * b->scale[k];
			}
		}
		float *o = out + 10 * j;
		o[0] = v[0], o[1] = v[1], o[2] = v[2];
		euler_to_quat(v + 3, o + 3);
		o[7] = o[8] = o[9] = 1;
	}
	return true;
}

void Studio_RestPose(const studio_model_t *m, float *out)
{
	for (int j = 0; j < m->numbones; j++) {
		float *o = out + 10 * j;
		const float *v = m->bones[j].value;
		o[0] = v[0], o[1] = v[1], o[2] = v[2];
		euler_to_quat(v + 3, o + 3);
		o[7] = o[8] = o[9] = 1;
	}
}

/* triangle commands: i16 n; n > 0 a strip, n < 0 a fan of -n vertices, 0
 * ends the list. Each vertex is four i16: vertex, normal, s, t */
int Studio_CountTris(const studio_model_t *m, const studio_mesh_t *mesh, int numverts, int numnorms)
{
	const uint8_t *p = mesh->cmds;
	int tris = 0;
	for (;;) {
		if (p + 2 > mesh->cmdslimit)
			return -1;
		int n = rds(p);
		p += 2;
		if (n == 0)
			return tris;
		if (n < 0)
			n = -n;
		if (n < 3 || (size_t)(mesh->cmdslimit - p) < (size_t)n * 8)
			return -1;
		for (int i = 0; i < n; i++, p += 8) {
			int vi = rds(p), ni = rds(p + 2);
			if (vi < 0 || vi >= numverts || ni < 0 || ni >= numnorms)
				return -1;
		}
		tris += n - 2;
		if (tris > 1 << 22)
			return -1;
	}
}

void Studio_Free(studio_model_t *m)
{
	for (int i = 0; i < m->numseqs; i++)
		Mem_Free(m->seqs[i].events);
	for (int i = 0; i < m->numbodyparts; i++) {
		for (int j = 0; j < m->bodyparts[i].nummodels; j++)
			Mem_Free(m->bodyparts[i].models[j].meshes);
		Mem_Free(m->bodyparts[i].models);
	}
	Mem_Free(m->bones);
	Mem_Free(m->hitboxes);
	Mem_Free(m->seqs);
	Mem_Free(m->textures);
	Mem_Free(m->skins);
	Mem_Free(m->bodyparts);
	Mem_Free(m->attachments);
	memset(m, 0, sizeof(*m));
}

/* --- cache and queries for the game VMs ----------------------------------- */

typedef struct studiocache_s {
	char name[128];
	uint8_t *buf, *texbuf;
	studio_model_t model;
	bool ok;
	struct studiocache_s *next;
} studiocache_t;

static studiocache_t *studiocache;

const studio_model_t *Studio_Cached(const char *name)
{
	studiocache_t *c;
	size_t size = 0;
	char err[256];
	if (!name || !*name || name[0] == '*')
		return NULL;
	for (c = studiocache; c; c = c->next)
		if (!Str_ICmp(c->name, name))
			return c->ok ? &c->model : NULL;
	c = Mem_Alloc(sizeof(*c));
	Str_Copy(c->name, name, sizeof(c->name));
	c->next = studiocache;
	studiocache = c;
	c->buf = (uint8_t *)FS_LoadFile(name, &size);
	if (!c->buf || size < 8 || memcmp(c->buf, "IDST", 4)) {
		Mem_Free(c->buf), c->buf = NULL;
		return NULL;
	}
	c->ok = Studio_Parse(&c->model, c->buf, size, err, sizeof(err));
	if (!c->ok) {
		Con_DPrintf("%s: %s\n", name, err);
		Studio_Free(&c->model);
		Mem_Free(c->buf), c->buf = NULL;
		return NULL;
	}
	if (!c->model.numtextures && strlen(name) > 4 && strlen(name) < sizeof(c->name) - 1) {
		/* the textures are in nameT.mdl */
		char tname[130];
		size_t tsize = 0, n = strlen(name) - 4;
		snprintf(tname, sizeof(tname), "%.*sT%s", (int)n, name, name + n);
		c->texbuf = (uint8_t *)FS_LoadFile(tname, &tsize);
		if (!c->texbuf) {
			tname[n] = 't';
			c->texbuf = (uint8_t *)FS_LoadFile(tname, &tsize);
		}
		if (c->texbuf && !Studio_ParseTextures(&c->model, c->texbuf, tsize, err, sizeof(err))) {
			Con_DPrintf("%s: %s\n", tname, err);
			Mem_Free(c->texbuf), c->texbuf = NULL;
		}
	}
	return &c->model;
}

float Studio_SeqDuration(const studio_model_t *m, int seq)
{
	const studio_seq_t *s;
	if (!m || seq < 0 || seq >= m->numseqs)
		return 0;
	s = &m->seqs[seq];
	if (s->fps <= 0)
		return 0;
	/* frame n is shown at n / fps, and the last one ends the sequence;
	   a single-frame sequence still lasts one frame */
	return (float)(s->numframes > 1 ? s->numframes - 1 : 1) / s->fps;
}

int Studio_SeqForActivity(const studio_model_t *m, int activity, unsigned rnd)
{
	int total = 0, pick;
	if (!m)
		return -1;
	for (int i = 0; i < m->numseqs; i++)
		if (m->seqs[i].activity == activity && m->seqs[i].actweight > 0)
			total += m->seqs[i].actweight;
	if (!total)
		return -1;
	pick = (int)(rnd % (unsigned)total);
	for (int i = 0; i < m->numseqs; i++) {
		if (m->seqs[i].activity != activity || m->seqs[i].actweight <= 0)
			continue;
		if (pick < m->seqs[i].actweight)
			return i;
		pick -= m->seqs[i].actweight;
	}
	return -1;
}
