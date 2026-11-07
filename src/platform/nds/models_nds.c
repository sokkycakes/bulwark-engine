/* Bulwark engine - Quake models (.mdl) for the DS build: the monsters and
 * items a map places, test crowds, and a view model. ISC licence, see LICENSE.
 *
 * Written from the .mdl file layout, for measuring how many objects the DS
 * can draw next to a map:
 *  - Frames stay as the file's bytes (x, y, z) with a shade byte worked out
 *    at load from the geometry, so a frame costs 4 bytes a vertex. A matrix
 *    scales the bytes into place on the GPU.
 *  - Shading is fixed light from above in the model's space, times the
 *    brightness of the floor under the entity.
 *  - Models share the GPU's frame budget with the world: they are fitted in
 *    first, nearest first, up to MAX_ENT_POLYS, and the world gets the rest.
 */
#include <math.h>
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "nds.h"

#define MAX_MDLS 32
#define MAX_ENTS 256
#define MAX_FRAMES 40       /* frames kept a model: enough to see it animate */
#define MAX_HL_FRAMES 96    /* a baked Half-Life model's clips (tools/nds/hlmdl.py) */
#define MAX_CLIPS 16
#define MAX_ENT_POLYS 2400  /* triangles sent; about half survive back-face culling */
#define MAX_SKIN 128
#define HEADER 84

typedef struct {
	uint16_t v[3];
	int16_t s[3], t[3]; /* t16 */
} mtri_t;

typedef struct {
	char name[40];
	bool bad;
	int numverts, numtris, numframes;
	uint8_t *frames; /* numframes * numverts * 4: x, y, z, shade 0..31 */
	mtri_t *tris;
	int32_t scale[3], trans[3]; /* for the matrix: f32, in 1/8 map units */
	int16_t mins[3], maxs[3];   /* map units, model space (yaw turns x and y) */
	int16_t radius;
	int tex;
	/* a baked Half-Life model: its clips and which clip each sequence plays */
	int numclips, numseqs;
	struct {
		uint16_t first;
		uint16_t lap;          /* ms the clip's frames take (frame f at f * lap / frames) */
		uint8_t frames : 7, loop : 1;
		uint8_t act;
	} clips[MAX_CLIPS];
	uint8_t *seqclip;
} mdl_t;

typedef struct {
	int mdl;
	int16_t org[3];
	int8_t next;   /* the next baked frame, from frame, */
	uint8_t blend; /* and blend/256 of the way to it (0: frame alone) */
	int yaw;
	int phase;
	int frame; /* -1: animate from phase */
	int light;
	int32_t dist;
} ent_t;

static mdl_t mdls[MAX_MDLS];
static int nummdls;
static ent_t ents[MAX_ENTS];
static int numents;
static int viewmodel = -1;
static int viewframe, viewlight = 24; /* its frame now, and its light */
static int viewframe2, viewblend;
static uint32_t clockms; /* Ents_SetTime: what plays a walk or run the server doesn't */
static uint32_t *mix;    /* draw_mdl's blended frame: on the heap once a model blends, to Ents_Free */
static bool mixtried;    /* and asked for: without room, models step from frame to frame until then */
bool Ents_LerpFrames = true;
int32_t Ents_ViewOfs[3];
int Ents_ViewAng[3];
static int pal_owner; /* the texture that holds the palette */
static const uint16_t *palette;
static int ticks;

static int32_t rd32(const uint8_t *p) { return (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }
static float rdf(const uint8_t *p)
{
	uint32_t u = (uint32_t)rd32(p);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

static int pot_floor(int v)
{
	int p = 8;
	while (p * 2 <= v && p < 1024)
		p *= 2;
	return p;
}

static int size_enum(int v)
{
	int e = 0;
	while ((8 << e) < v)
		e++;
	return e;
}

/* the skin, shrunk to a power of two no larger than MAX_SKIN; smaller again if VRAM is short */
static int upload_skin(const uint8_t *src, int sw, int sh, int *tw, int *th, const uint16_t *own)
{
	for (int cap = MAX_SKIN; cap >= 16; cap /= 2) {
		int w = BW_NDS_MIN(pot_floor(sw), cap), h = BW_NDS_MIN(pot_floor(sh), cap), name = 0;
		uint8_t *pix = malloc((size_t)w * h);
		if (!pix)
			return 0;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				pix[y * w + x] = src[(y * sh / h) * sw + x * sw / w];
		glGenTextures(1, &name);
		glBindTexture(GL_TEXTURE_2D, name);
		if (glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB256, size_enum(w), size_enum(h), 0, 0, pix)) {
			free(pix);
			if (own)
				glColorTableEXT(GL_TEXTURE_2D, 0, 256, 0, 0, own);
			else if (pal_owner)
				glAssignColorTable(GL_TEXTURE_2D, pal_owner);
			else {
				static uint16_t pal[256];
				for (int i = 0; i < 256; i++)
					pal[i] = palette ? palette[i] : RGB15(i >> 3, i >> 3, i >> 3);
				glColorTableEXT(GL_TEXTURE_2D, 0, 256, 0, 0, pal);
				pal_owner = name;
			}
			*tw = w, *th = h;
			return name;
		}
		free(pix);
		glDeleteTextures(1, &name);
	}
	return 0;
}

/* shade bytes for one frame: light from above and a little in front, by vertex normal */
static void shade_frame(mdl_t *m, uint8_t *fr, const float *scale)
{
	float *n = calloc((size_t)m->numverts * 3, sizeof(float));
	if (!n)
		return;
	for (int t = 0; t < m->numtris; t++) {
		const uint16_t *idx = m->tris[t].v;
		const uint8_t *a = fr + idx[0] * 4, *b = fr + idx[1] * 4, *c = fr + idx[2] * 4;
		float e1[3], e2[3], fn[3];
		for (int k = 0; k < 3; k++)
			e1[k] = (b[k] - a[k]) * scale[k], e2[k] = (c[k] - a[k]) * scale[k];
		/* Quake's triangles wind clockwise seen from the front */
		fn[0] = e2[1] * e1[2] - e2[2] * e1[1];
		fn[1] = e2[2] * e1[0] - e2[0] * e1[2];
		fn[2] = e2[0] * e1[1] - e2[1] * e1[0];
		for (int v = 0; v < 3; v++)
			for (int k = 0; k < 3; k++)
				n[idx[v] * 3 + k] += fn[k];
	}
	for (int v = 0; v < m->numverts; v++) {
		float *p = n + v * 3, len = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
		float d = len > 0 ? (p[0] * 0.40f + p[1] * 0.30f + p[2] * 0.87f) / len : 0;
		fr[v * 4 + 3] = (uint8_t)(13 + (d > 0 ? d * 18 : 0));
	}
	free(n);
}

/* A model file, read whole when it is small and a part at a time when not,
 * so a big one never sits in RAM next to the frames made from it */
#define MDL_WHOLE (64 * 1024) /* files up to this size are read in one go */
#define MDL_PART (16 * 1024)  /* bigger ones this much at a time */

typedef struct {
	const char *path;
	uint8_t *buf; /* the file's bytes [at, at + len) */
	long at, len;
	long least; /* the file is at least this long, if it is well formed */
	bool whole;
} mfile_t;

/* n bytes from pos, or NULL if the file is shorter. When the bytes have to
 * be read, up to ahead of them are, so the next calls find theirs; what an
 * earlier call returned is gone then. */
static const uint8_t *mfile_get(mfile_t *f, long pos, long n, long ahead)
{
	size_t got = 0;
	long len;
	if (pos < 0 || n <= 0)
		return NULL;
	if (f->buf && pos >= f->at && pos + n <= f->at + f->len)
		return f->buf + (pos - f->at);
	if (f->whole)
		return NULL;
	free(f->buf);
	f->len = 0;
	/* no further than the file surely goes: past the end a loose file reads nothing */
	len = BW_NDS_MAX(n, BW_NDS_MIN(ahead, f->least - pos));
	f->buf = Fs_LoadRange(f->path, pos, len, &got);
	if (len > n && (!f->buf || (long)got < n)) {
		free(f->buf);
		f->buf = Fs_LoadRange(f->path, pos, n, &got);
	}
	if (!f->buf || (long)got < n) {
		free(f->buf);
		f->buf = NULL;
		return NULL;
	}
	f->at = pos, f->len = (long)got;
	return f->buf;
}

/* the trailer hlmdl.py adds after the frames: clips, the sequence map and the
 * skin's own palette; true if the model has its own palette */
static bool load_trailer(mdl_t *m, mfile_t *f, long at, uint16_t *ownpal)
{
	const uint8_t *t = mfile_get(f, at, 6, 6);
	long len;
	int nc, ns;
	if (!t || memcmp(t, "DSHL", 4))
		return false;
	nc = t[4] | t[5] << 8;
	len = 6 + nc * 6 + 2;
	if (!(t = mfile_get(f, at, len, len)))
		return false;
	ns = t[len - 2] | t[len - 1] << 8;
	len += ns + 768 + 8;
	/* the file ends with the trailer's offset and "DSHL" again */
	if (!(t = mfile_get(f, at, len, len)) || rd32(t + len - 8) != at || memcmp(t + len - 4, "DSHL", 4))
		return false;
	f->least = BW_NDS_MAX(f->least, at + len);
	for (int c = 0; c < nc && c < MAX_CLIPS; c++) {
		const uint8_t *e = t + 6 + c * 6;
		int frames = BW_NDS_CLAMP(e[2], 1, 127), fps = e[3] ? e[3] : 10;
		m->clips[c].first = (uint16_t)(e[0] | e[1] << 8);
		m->clips[c].frames = (uint8_t)frames, m->clips[c].lap = (uint16_t)((frames * 1000 + fps / 2) / fps);
		m->clips[c].loop = e[4] != 0, m->clips[c].act = e[5];
		m->numclips = c + 1;
	}
	/* hlmdl.py since the u8 fps: each clip's lap in ms, just before the trailer, which
	 * an older loader doesn't read (the fps rounds a 3 s, 10-frame clip to 3.33 s) */
	if (nc > 0 && nc <= MAX_CLIPS && at >= 4 + 2 * nc) {
		/* read with the trailer, which stays where t points */
		long pre = 4 + 2 * nc;
		const uint8_t *d = mfile_get(f, at - pre, pre + len, pre + len);
		if (d) {
			if (!memcmp(d, "DSLP", 4))
				for (int c = 0; c < nc; c++)
					if (d[4 + c * 2] | d[5 + c * 2] << 8)
						m->clips[c].lap = (uint16_t)(d[4 + c * 2] | d[5 + c * 2] << 8);
			t = d + pre;
		} else if (!(t = mfile_get(f, at, len, len)))
			return false;
	}
	t += 6 + nc * 6 + 2;
	if (ns > 0 && (m->seqclip = malloc((size_t)ns))) {
		memcpy(m->seqclip, t, (size_t)ns);
		m->numseqs = ns;
		t += ns;
		for (int i = 0; i < 256; i++)
			ownpal[i] = RGB15(t[i * 3] >> 3, t[i * 3 + 1] >> 3, t[i * 3 + 2] >> 3);
		return true;
	}
	return false;
}

static int load_mdl(const char *name)
{
	char path[64];
	size_t size;
	const uint8_t *p;
	long at, skinat = -1;
	mdl_t *m;
	mfile_t f = {0};
	int numskins, sw, sh, nv, nt, nf, tw = 0, th = 0;
	float scale[3];
	static uint16_t ownpal[256];
	bool haveown = false;
	int maxframes, room;
	for (int i = 0; i < nummdls; i++)
		if (!strcmp(mdls[i].name, name))
			return mdls[i].bad ? -1 : i;
	if (nummdls >= MAX_MDLS)
		return -1;
	m = &mdls[nummdls++];
	memset(m, 0, sizeof(*m));
	snprintf(m->name, sizeof(m->name), "%s", name);
	m->bad = true;
	if (strchr(name, '/'))
		snprintf(path, sizeof(path), "%s", name);
	else
		snprintf(path, sizeof(path), "progs/%s.mdl", name);
	f.path = path;
	if (!(p = mfile_get(&f, 0, HEADER, HEADER)))
		return -1;
	if (memcmp(p, "IDPO", 4) || rd32(p + 4) != 6)
		goto fail;
	numskins = rd32(p + 48), sw = rd32(p + 52), sh = rd32(p + 56);
	nv = rd32(p + 60), nt = rd32(p + 64), nf = rd32(p + 68);
	/* past these the sizes below wrap in the DS's 32-bit long */
	if (sw <= 0 || sh <= 0 || nv <= 0 || nt <= 0 || nf <= 0 || nv > 4096 || nt > 65536 || nf > 65536 || sw > 1024 ||
	    sh > 1024 || numskins > 256)
		goto fail;
	for (int k = 0; k < 3; k++) {
		scale[k] = rdf(p + 8 + k * 4);
		m->scale[k] = (int32_t)lrintf(scale[k] * 8 * 4096);
		m->trans[k] = (int32_t)lrintf(rdf(p + 20 + k * 4) * 8);
		m->mins[k] = (int16_t)lrintf(rdf(p + 20 + k * 4));
		m->maxs[k] = (int16_t)lrintf(rdf(p + 20 + k * 4) + 255 * scale[k]);
	}
	m->radius = (int16_t)lrintf(rdf(p + 32));
	/* the shortest the rest can be (a group of skins or frames is longer than
	 * one of them); a small file is read whole */
	f.least = HEADER + BW_NDS_MAX(numskins, 0) * (4 + (long)sw * sh) + (long)nv * 12 + (long)nt * 16 +
	          (long)nf * (28 + (long)nv * 4);
	if (f.least <= MDL_WHOLE) {
		free(f.buf);
		f.buf = NULL;
		if (!(f.buf = Fs_Load(path, &size)))
			return -1;
		f.at = 0, f.len = (long)size, f.whole = true;
	}

	/* the first skin; the rest are passed over */
	at = HEADER;
	for (int s = 0; s < numskins; s++) {
		if (!(p = mfile_get(&f, at, 4, 8)))
			goto fail;
		if (rd32(p) == 0) {
			if (skinat < 0)
				skinat = at + 4;
			at += 4 + (long)sw * sh;
		} else {
			int n;
			if (!(p = mfile_get(&f, at + 4, 4, 4)))
				goto fail;
			n = rd32(p);
			if (n <= 0 || n > 256) /* a group of skins; past this the sizes wrap */
				goto fail;
			if (skinat < 0)
				skinat = at + 8 + n * 4;
			at += 8 + n * 4 + (long)n * sw * sh;
		}
	}
	/* the trailer starts where the frames end, each one frame as hlmdl.py writes them */
	haveown = load_trailer(m, &f, at + (long)nv * 12 + (long)nt * 16 + (long)nf * (28 + (long)nv * 4), ownpal);
	if (skinat >= 0 && (p = mfile_get(&f, skinat, (long)sw * sh, (long)sw * sh)))
		m->tex = upload_skin(p, sw, sh, &tw, &th, haveown ? ownpal : NULL);

	/* triangles, with their texture coordinates at each corner */
	{
		const uint8_t *st, *tri;
		if (!(st = mfile_get(&f, at, (long)nv * 12 + (long)nt * 16, (long)nv * 12 + (long)nt * 16)))
			goto fail;
		tri = st + nv * 12;
		m->tris = malloc(sizeof(mtri_t) * (size_t)nt);
		if (!m->tris)
			goto fail;
		for (int t = 0; t < nt; t++) {
			bool front = rd32(tri + t * 16) != 0;
			for (int k = 0; k < 3; k++) {
				int v = rd32(tri + t * 16 + 4 + k * 4), s, tt;
				if (v < 0 || v >= nv)
					goto fail;
				s = rd32(st + v * 12 + 4), tt = rd32(st + v * 12 + 8);
				if (!front && rd32(st + v * 12))
					s += sw / 2; /* the back half of a seam */
				m->tris[t].v[k] = (uint16_t)v;
				m->tris[t].s[k] = (int16_t)((s * tw * 16 + tw * 8) / sw);
				m->tris[t].t[k] = (int16_t)((tt * th * 16 + th * 8) / sh);
			}
		}
		at += (long)nv * 12 + (long)nt * 16;
	}
	if (!f.whole) {
		/* the triangles' bytes are done with before the frames take their room */
		free(f.buf);
		f.buf = NULL, f.len = 0;
	}
	m->numverts = nv, m->numtris = nt;

	/* frames, flattening groups, up to MAX_FRAMES; room for one a group
	 * (exact unless there are groups), more when a group needs it */
	maxframes = m->numclips ? MAX_HL_FRAMES : MAX_FRAMES;
	room = BW_NDS_MIN(nf, maxframes);
	m->frames = malloc((size_t)room * nv * 4);
	if (!m->frames)
		goto fail;
	for (int fi = 0; fi < nf && m->numframes < maxframes; fi++) {
		int type, n = 1;
		if (!(p = mfile_get(&f, at, 4, MDL_PART)))
			goto fail;
		type = rd32(p);
		at += 4;
		if (type != 0) {
			if (!(p = mfile_get(&f, at, 4, MDL_PART)))
				goto fail;
			n = rd32(p);
			if (n <= 0 || n > 65536) /* a group of frames; past this the sizes wrap */
				goto fail;
			at += 4 + 8 + n * 4; /* count, bounds, intervals */
		}
		for (int g = 0; g < n && m->numframes < maxframes; g++) {
			uint8_t *fr;
			if (m->numframes == room) {
				/* this group's frames, and one for each entry after it */
				int want = (int)BW_NDS_MIN((int64_t)m->numframes + (n - g) + (nf - fi - 1), maxframes);
				uint8_t *more = realloc(m->frames, (size_t)want * nv * 4);
				if (!more)
					goto fail;
				m->frames = more, room = want;
			}
			fr = m->frames + (size_t)m->numframes * nv * 4;
			at += 8 + 16; /* bounds and name */
			if (!(p = mfile_get(&f, at, (long)nv * 4, MDL_PART)))
				goto fail;
			for (int v = 0; v < nv; v++)
				fr[v * 4] = p[v * 4], fr[v * 4 + 1] = p[v * 4 + 1], fr[v * 4 + 2] = p[v * 4 + 2];
			shade_frame(m, fr, scale);
			m->numframes++;
			at += (long)nv * 4;
		}
	}
	if (!m->numframes)
		goto fail;
	if (m->numframes < room) {
		/* a group shorter than it said: give the rest back */
		uint8_t *less = realloc(m->frames, (size_t)m->numframes * nv * 4);
		if (less)
			m->frames = less;
	}
	free(f.buf);
	m->bad = false;
	return (int)(m - mdls);
fail:
	free(f.buf);
	free(m->tris);
	free(m->frames);
	free(m->seqclip);
	m->tris = NULL, m->frames = NULL, m->seqclip = NULL;
	m->numframes = 0;
	if (m->tex)
		glDeleteTextures(1, &m->tex), m->tex = 0;
	return -1;
}

static void add_ent(int mdl, const float *org, float angle, int phase)
{
	ent_t *e;
	if (mdl < 0 || numents >= MAX_ENTS)
		return;
	e = &ents[numents++];
	e->mdl = mdl;
	for (int c = 0; c < 3; c++)
		e->org[c] = (int16_t)BW_NDS_CLAMP(lrintf(org[c]), -4000, 4000);
	e->yaw = (int)(angle * 32768 / 360);
	e->phase = phase;
	e->frame = -1, e->blend = 0;
	e->light = World_LightAt(e->org);
}

/* classnames to models, for the shapes Quake's own progs give them */
static const struct {
	const char *classname, *model;
} classes[] = {
	{"monster_army", "soldier"},       {"monster_dog", "dog"},           {"monster_ogre", "ogre"},
	{"monster_ogre_marksman", "ogre"}, {"monster_knight", "knight"},     {"monster_hell_knight", "hknight"},
	{"monster_zombie", "zombie"},      {"monster_wizard", "wizard"},     {"monster_demon1", "demon"},
	{"monster_shambler", "shambler"},  {"monster_enforcer", "enforcer"}, {"monster_fish", "fish"},
	{"monster_shalrath", "shalrath"},  {"monster_tarbaby", "tarbaby"},   {"weapon_supershotgun", "g_shot"},
	{"weapon_nailgun", "g_nail"},      {"weapon_supernailgun", "g_nail2"},
	{"weapon_grenadelauncher", "g_rock"}, {"weapon_rocketlauncher", "g_rock2"},
	{"weapon_lightning", "g_light"},   {"item_armor1", "armor"},         {"item_armor2", "armor"},
	{"item_armorInv", "armor"},        {"item_artifact_super_damage", "quaddama"},
	{"item_artifact_invulnerability", "invulner"}, {"item_artifact_invisibility", "invisibl"},
	{"item_artifact_envirosuit", "suit"}, {"item_key1", "w_s_key"},      {"item_key2", "w_g_key"},
};

void Ents_Load(const uint16_t *pal)
{
	const char *e = World_Entities();
	palette = pal;
	while (e && (e = strchr(e, '{'))) {
		const char *end = strchr(e, '}');
		char key[64], value[128], classname[64] = "";
		float org[3] = {0, 0, 0}, angle = 0;
		const char *p = e + 1;
		if (!end)
			break;
		while (p < end) {
			int n = 0;
			if (sscanf(p, " \"%63[^\"]\" \"%127[^\"]\"%n", key, value, &n) != 2 || !n)
				break;
			p += n;
			if (!strcmp(key, "classname"))
				snprintf(classname, sizeof(classname), "%.63s", value);
			else if (!strcmp(key, "origin"))
				sscanf(value, "%f %f %f", &org[0], &org[1], &org[2]);
			else if (!strcmp(key, "angle"))
				angle = strtof(value, NULL);
		}
		for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
			if (!strcmp(classname, classes[i].classname)) {
				add_ent(load_mdl(classes[i].model), org, angle, numents * 7);
				break;
			}
		e = end + 1;
	}
	viewmodel = load_mdl("v_shot");
	viewframe = 0, viewlight = 24;
}

int Ents_AddCrowd(const ndscam_t *cam, const char *model, int count)
{
	int mdl = load_mdl(model), cols = 1, added = 0;
	float fx = cosf(cam->yaw * 6.2831853f / 32768), fy = sinf(cam->yaw * 6.2831853f / 32768);
	if (mdl < 0)
		return 0;
	while (cols * cols < count)
		cols++;
	for (int i = 0; i < count; i++) {
		float ahead = 128 + (i / cols) * 64, side = ((i % cols) - (cols - 1) * 0.5f) * 64, org[3];
		org[0] = cam->org[0] / 8.0f + fx * ahead + fy * side;
		org[1] = cam->org[1] / 8.0f + fy * ahead - fx * side;
		org[2] = cam->org[2] / 8.0f - 22 + 24;
		add_ent(mdl, org, cam->yaw * 360.0f / 32768 + 180, i * 5);
		added++;
	}
	return added;
}

int Ents_Model(const char *path) { return load_mdl(path); }

void Ents_Counts(int *loaded, int *failed)
{
	*loaded = *failed = 0;
	for (int i = 0; i < nummdls; i++)
		(*(mdls[i].bad ? failed : loaded))++;
}

void Ents_Clear(void) { numents = 0; }

/* the clip for an activity, -1 if the model has none */
static int clip_for(const mdl_t *m, int act)
{
	for (int c = 0; c < m->numclips; c++)
		if (m->clips[c].act == act)
			return c;
	return -1;
}

void Ents_SetTime(uint32_t ms) { clockms = ms; }

/* the baked frames ms into a clip, and how far between them: Half-Life
 * models are posed at any time into a sequence (the PC's frame1time), the
 * DS has at most 10 frames a second of each, so it blends the two around
 * that time when Ents_LerpFrames is set */
static void clip_frames(const mdl_t *m, int clip, uint32_t ms, int *frame, int *frame2, int *blend)
{
	uint32_t lap = m->clips[clip].lap ? m->clips[clip].lap : 1000, pos;
	int n = m->clips[clip].frames, f, next;
	/* frames x 256: under 65536 ms x 127 frames x 256, within 32 bits */
	if (m->clips[clip].loop)
		ms %= lap;
	else if (ms >= lap)
		ms = lap;
	pos = ms * (uint32_t)n * 256 / lap;
	f = (int)(pos >> 8);
	*blend = Ents_LerpFrames ? (int)(pos & 255) : 0;
	if (m->clips[clip].loop)
		f %= n, next = f + 1 < n ? f + 1 : 0;
	else if (f >= n - 1)
		f = next = n - 1, *blend = 0;
	else
		next = f + 1;
	*frame = m->clips[clip].first + f;
	*frame2 = m->clips[clip].first + next;
}

/* speed: map units a second the thing is moving at, to walk or run a model
 * the server only has standing (players animate on their own machines);
 * ms: how long its sequence has played */
int Ents_AddLinked(int mdl, const int16_t *org, int yaw, int seq, uint32_t ms, int speed, int light)
{
	const mdl_t *m;
	ent_t *e;
	float o[3] = {org[0], org[1], org[2]};
	int clip = 0;
	if (mdl < 0 || mdl >= nummdls || mdls[mdl].bad || numents >= MAX_ENTS)
		return light;
	m = &mdls[mdl];
	e = &ents[numents++];
	e->mdl = mdl;
	for (int c = 0; c < 3; c++)
		e->org[c] = (int16_t)BW_NDS_CLAMP(o[c], -4000, 4000);
	e->yaw = yaw, e->phase = 0, e->frame = -1, e->blend = 0;
	/* the floor's light takes a walk through the leaf's faces: the caller keeps it */
	e->light = light >= 0 ? light : World_LightAt(e->org);
	if (!m->numclips)
		return e->light;
	if (seq >= 0 && seq < m->numseqs)
		clip = m->seqclip[seq];
	if (m->clips[clip].act == 1 || clip == 0) { /* standing while moving: walk or run */
		int c = speed > 200 ? clip_for(m, 4) : speed > 40 ? clip_for(m, 3) : -1;
		if (c < 0 && speed > 40)
			c = clip_for(m, speed > 200 ? 3 : 4);
		if (c >= 0)
			clip = c, ms = clockms;
	}
	{
		int f2, bl;
		clip_frames(m, clip, ms, &e->frame, &f2, &bl);
		/* the next frame as an offset: a clip of over 128 frames goes without blending at its end */
		e->next = (int8_t)(f2 - e->frame >= -128 ? f2 - e->frame : 0), e->blend = (uint8_t)(f2 - e->frame >= -128 ? bl : 0);
	}
	return e->light;
}

/* the weapon in hand, for linked play: the model (-1 for none) and its sequence, playing for ms */
void Ents_SetViewModel(int mdl, int seq, uint32_t ms, int light)
{
	const mdl_t *m;
	int clip = 0;
	viewmodel = mdl >= 0 && mdl < nummdls && !mdls[mdl].bad ? mdl : -1;
	viewframe = 0, viewblend = 0, viewlight = light >= 0 ? light : 24;
	if (viewmodel < 0 || !(m = &mdls[viewmodel])->numclips)
		return;
	if (seq >= 0 && seq < m->numseqs)
		clip = m->seqclip[seq];
	clip_frames(m, clip, ms, &viewframe, &viewframe2, &viewblend);
}

void Ents_Free(void)
{
	for (int i = 0; i < nummdls; i++) {
		free(mdls[i].frames);
		free(mdls[i].tris);
		free(mdls[i].seqclip);
		if (mdls[i].tex)
			glDeleteTextures(1, &mdls[i].tex);
	}
	nummdls = numents = 0;
	viewmodel = -1;
	pal_owner = 0;
	free(mix);
	mix = NULL, mixtried = false;
}

#define MAX_BLEND_VERTS 512 /* a bigger model is drawn without blending */
/* blending costs about 50 us a model (melonDS): only the nearest
 * BLEND_MODELS within BLEND_DIST map units blend; further off, a blend moves
 * a vertex by less than a pixel */
#define BLEND_DIST 768
#define BLEND_MODELS 12
static int32_t blend_lim;
ITCM_CODE static void draw_mdl(const mdl_t *m, int frame, int frame2, int blend, int light)
{
	const uint8_t *fr = m->frames + (size_t)(frame % m->numframes) * m->numverts * 4;
	uint16_t lut[32];
	if (blend > 0 && frame2 != frame && m->numverts <= MAX_BLEND_VERTS && (mix || (!mixtried && (mixtried = true, mix = malloc(MAX_BLEND_VERTS * 4))))) {
		/* between two baked frames: each vertex blend/256 of the way to the
		 * next one, once (a vertex serves about five triangle corners) */
		const uint32_t *a = (const uint32_t *)fr;
		const uint32_t *b = (const uint32_t *)(m->frames + (size_t)(frame2 % m->numframes) * m->numverts * 4);
		uint32_t wb = (uint32_t)blend, wa = 256 - wb;
		/* x, y, z and shade at once, two bytes to a 16-bit lane: a byte
		 * times 256 at most still fits its lane */
		for (int i = 0; i < m->numverts; i++) {
			uint32_t va = a[i], vb = b[i];
			uint32_t lo = ((va & 0xff00ffu) * wa + (vb & 0xff00ffu) * wb) >> 8 & 0xff00ffu;
			uint32_t hi = (((va >> 8) & 0xff00ffu) * wa + ((vb >> 8) & 0xff00ffu) * wb) & 0xff00ff00u;
			mix[i] = lo | hi;
		}
		fr = (const uint8_t *)mix;
	}
	for (int i = 0; i < 32; i++) {
		int c = i * light / 24;
		c = c > 31 ? 31 : c;
		lut[i] = RGB15(c, c, c);
	}
	glBindTexture(GL_TEXTURE_2D, m->tex);
	glScalef32(m->scale[0], m->scale[1], m->scale[2]);
	glBegin(GL_TRIANGLES);
	for (int t = 0; t < m->numtris; t++) {
		const mtri_t *tr = &m->tris[t];
		for (int k = 0; k < 3; k++) {
			const uint8_t *v = fr + tr->v[k] * 4;
			glColor(lut[v[3]]);
			glTexCoord2t16(tr->s[k], tr->t[k]);
			glVertex3v16(v[0], v[1], v[2]);
		}
	}
	glEnd();
}

/* GPU polygons a model stores, about: half its triangles survive back-face culling */
#define STORED(m) ((m)->numtris * 55 / 100)

static int order[MAX_ENTS], numorder;
bool Ents_Flicker;

void Ents_Select(const ndscam_t *cam)
{
	numorder = 0;
	ticks++;
	for (int i = 0; i < numents; i++) {
		ent_t *e = &ents[i];
		const mdl_t *m = &mdls[e->mdl];
		int16_t mins[3], maxs[3];
		int32_t dx = e->org[0] - cam->org[0] / 8, dy = e->org[1] - cam->org[1] / 8, dz = e->org[2] - cam->org[2] / 8;
		int k;
		for (int c = 0; c < 3; c++) {
			int r = c < 2 ? m->radius : 0;
			mins[c] = (int16_t)(e->org[c] + (c < 2 ? -r : m->mins[c]));
			maxs[c] = (int16_t)(e->org[c] + (c < 2 ? r : m->maxs[c]));
		}
		if (!World_BoxVisible(mins, maxs))
			continue;
		e->dist = dx * dx + dy * dy + dz * dz;
		/* nearest first */
		k = numorder++;
		while (k > 0 && ents[order[k - 1]].dist > e->dist)
			order[k] = order[k - 1], k--;
		order[k] = i;
	}
	blend_lim = numorder > BLEND_MODELS ? ents[order[BLEND_MODELS - 1]].dist : BLEND_DIST * BLEND_DIST;
	blend_lim = BW_NDS_MIN(blend_lim, BLEND_DIST * BLEND_DIST);
}

/* what the visible models will cost the GPU, nearest first: about half
 * their triangles survive back-face culling, three vertices each */
int Ents_Costs(int32_t *dist2, int *polys, int max, int *viewpolys)
{
	int n = 0;
	*viewpolys = viewmodel >= 0 ? STORED(&mdls[viewmodel]) : 0;
	for (int k = 0; k < numorder && n < max; k++, n++) {
		dist2[n] = ents[order[k]].dist;
		polys[n] = STORED(&mdls[ents[order[k]].mdl]);
	}
	return n;
}

static void draw_ent(const ent_t *e)
{
	const mdl_t *m = &mdls[e->mdl];
	glPushMatrix();
	glTranslatef32(e->org[0] * 8, e->org[1] * 8, e->org[2] * 8);
	glRotateZi(e->yaw);
	glTranslatef32(m->trans[0], m->trans[1], m->trans[2]);
	if (e->frame >= 0) /* further off, a blend moves a vertex less than a pixel */
		draw_mdl(m, e->frame, e->frame + e->next, e->dist <= blend_lim ? e->blend : 0, e->light);
	else
		draw_mdl(m, ticks / 6 + e->phase, 0, 0, e->light);
	glPopMatrix(1);
}

/* flicker < 0: the nearest models that fit MAX_ENT_POLYS, the rest left out.
 * flicker >= 0 is the GPU polygons the world leaves models in this pass. The
 * nearest models up to STEADY_POLYS are always drawn; past that, the models
 * that don't all fit take turns, a different run of them each frame, as NES
 * games did with sprites. */
#define STEADY_POLYS 600
void Ents_DrawPass(const ndscam_t *cam, int32_t split2, bool nearside, bool withviewmodel, int flicker, ndsstats_t *st)
{
	static int rest[MAX_ENTS];
	int sent = 0, drawn = 0, stored = 0, numrest = 0, restcost = 0;
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
	if (withviewmodel && viewmodel >= 0) {
		ndscam_t eyecam = {{0, 0, 0}, 0, 0, 0};
		const mdl_t *m = &mdls[viewmodel];
		World_SetView(&eyecam);
		glTranslatef32(Ents_ViewOfs[0], Ents_ViewOfs[1], Ents_ViewOfs[2]);
		/* turned about the eye the way Quake turns an entity: yaw, then
		 * pitch, then roll (map axes: x forward, y left, z up) */
		glRotateZi(Ents_ViewAng[1]);
		glRotateYi(Ents_ViewAng[0]);
		glRotateXi(Ents_ViewAng[2]);
		glTranslatef32(m->trans[0], m->trans[1], m->trans[2]);
		draw_mdl(m, viewframe, viewframe2, viewblend, viewlight);
		World_SetView(cam);
		sent += m->numtris, stored += STORED(m), drawn++;
	}
	for (int k = 0; k < numorder; k++) {
		const ent_t *e = &ents[order[k]];
		const mdl_t *m = &mdls[e->mdl];
		if ((e->dist < split2) != nearside)
			continue;
		if (sent + m->numtris > MAX_ENT_POLYS ||
		    (flicker >= 0 && (numrest || stored + STORED(m) > STEADY_POLYS))) {
			if (flicker >= 0)
				rest[numrest++] = order[k], restcost += STORED(&mdls[e->mdl]);
			else
				st->entsdropped++;
			continue;
		}
		draw_ent(e);
		sent += m->numtris, stored += STORED(m), drawn++;
	}
	if (numrest) {
		/* the rest, starting a little further along the list each frame */
		int room = flicker - stored, start = 0;
		if (restcost > room) {
			int fit = room > 0 ? room * numrest / restcost : 0;
			start = fit ? ticks * fit % numrest : 0;
			st->entsflicker += numrest;
		}
		for (int i = 0; i < numrest; i++) {
			const ent_t *e = &ents[rest[(start + i) % numrest]];
			const mdl_t *m = &mdls[e->mdl];
			if (stored + STORED(m) > flicker || sent + m->numtris > MAX_ENT_POLYS) {
				if (restcost <= room)
					st->entsdropped++;
				continue;
			}
			draw_ent(e);
			sent += m->numtris, stored += STORED(m), drawn++;
		}
	}
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);
	st->ents += drawn;
	st->entpolys += sent;
}

void Gpu_Counts(int *polys, int *verts)
{
	while (GFX_STATUS & BIT(27))
		; /* the geometry engine is still working through what was sent */
	glGetInt(GL_GET_POLYGON_RAM_COUNT, polys);
	glGetInt(GL_GET_VERTEX_RAM_COUNT, verts);
}

int Ents_FlickerBudget(int worldpolys, int worldverts)
{
	int polys = WORLD_MAX_POLYS - worldpolys, byverts = (WORLD_MAX_VERTS - worldverts) / 3;
	if (!Ents_Flicker)
		return -1;
	polys = byverts < polys ? byverts : polys;
	return polys > 0 ? polys : 0;
}

/* models first, then the world in whatever the GPU has left: after
 * back-face culling a model stores about half its triangles, so the count
 * is read back from the GPU rather than guessed. With Ents_Flicker the
 * world is counted first, and models past the nearest few share what it leaves. */
void Ents_Draw(const ndscam_t *cam, ndsstats_t *st)
{
	int polys, verts, wverts, wpolys;
	World_Begin(cam, st);
	Ents_Select(cam);
	World_Collect();
	wpolys = World_Cost(0, World_NumTreeFaces(), INT32_MAX, true, &wverts);
	Ents_DrawPass(cam, INT32_MAX, true, true, Ents_FlickerBudget(wpolys, wverts), st);
	Gpu_Counts(&polys, &verts);
	World_DrawPass(0, World_NumTreeFaces(), INT32_MAX, true, st, WORLD_MAX_VERTS - verts, WORLD_MAX_POLYS - polys);
}
