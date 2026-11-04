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

#define MAX_MDLS 48
#define MAX_ENTS 256
#define MAX_FRAMES 40       /* frames kept a model: enough to see it animate */
#define MAX_HL_FRAMES 96    /* a baked Half-Life model's clips (tools/nds/hlmdl.py) */
#define MAX_CLIPS 24
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
		uint8_t frames, fps, loop, act;
	} clips[MAX_CLIPS];
	uint8_t *seqclip;
} mdl_t;

typedef struct {
	int mdl;
	int16_t org[3];
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
static void shade_frame(mdl_t *m, uint8_t *fr, const int32_t *idx, const float *scale)
{
	float *n = calloc((size_t)m->numverts * 3, sizeof(float));
	if (!n)
		return;
	for (int t = 0; t < m->numtris; t++) {
		const uint8_t *a = fr + idx[t * 3] * 4, *b = fr + idx[t * 3 + 1] * 4, *c = fr + idx[t * 3 + 2] * 4;
		float e1[3], e2[3], fn[3];
		for (int k = 0; k < 3; k++)
			e1[k] = (b[k] - a[k]) * scale[k], e2[k] = (c[k] - a[k]) * scale[k];
		/* Quake's triangles wind clockwise seen from the front */
		fn[0] = e2[1] * e1[2] - e2[2] * e1[1];
		fn[1] = e2[2] * e1[0] - e2[0] * e1[2];
		fn[2] = e2[0] * e1[1] - e2[1] * e1[0];
		for (int v = 0; v < 3; v++)
			for (int k = 0; k < 3; k++)
				n[idx[t * 3 + v] * 3 + k] += fn[k];
	}
	for (int v = 0; v < m->numverts; v++) {
		float *p = n + v * 3, len = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
		float d = len > 0 ? (p[0] * 0.40f + p[1] * 0.30f + p[2] * 0.87f) / len : 0;
		fr[v * 4 + 3] = (uint8_t)(13 + (d > 0 ? d * 18 : 0));
	}
	free(n);
}

static int load_mdl(const char *name)
{
	char path[64];
	size_t size;
	uint8_t *d;
	const uint8_t *p, *end;
	mdl_t *m;
	int numskins, sw, sh, nv, nt, nf, tw = 0, th = 0;
	int32_t *idx = NULL;
	float scale[3];
	const uint8_t *skin = NULL;
	static uint16_t ownpal[256];
	bool haveown = false;
	int maxframes;
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
	if (!(d = Fs_Load(path, &size)))
		return -1;
	end = d + size;
	if (size < HEADER || memcmp(d, "IDPO", 4) || rd32(d + 4) != 6)
		goto fail;
	numskins = rd32(d + 48), sw = rd32(d + 52), sh = rd32(d + 56);
	nv = rd32(d + 60), nt = rd32(d + 64), nf = rd32(d + 68);
	if (sw <= 0 || sh <= 0 || nv <= 0 || nt <= 0 || nf <= 0 || nv > 4096)
		goto fail;
	for (int k = 0; k < 3; k++) {
		scale[k] = rdf(d + 8 + k * 4);
		m->scale[k] = (int32_t)lrintf(scale[k] * 8 * 4096);
		m->trans[k] = (int32_t)lrintf(rdf(d + 20 + k * 4) * 8);
		m->mins[k] = (int16_t)lrintf(rdf(d + 20 + k * 4));
		m->maxs[k] = (int16_t)lrintf(rdf(d + 20 + k * 4) + 255 * scale[k]);
	}
	m->radius = (int16_t)lrintf(rdf(d + 32));
	/* the trailer hlmdl.py adds: clips, the sequence map and the skin's own palette */
	if (size > 8 + 4 + 768 && !memcmp(end - 4, "DSHL", 4)) {
		uint32_t at = (uint32_t)rd32(end - 8);
		const uint8_t *t = d + at;
		if (at + 6 < size && !memcmp(t, "DSHL", 4)) {
			int nc = t[4] | t[5] << 8, ns;
			t += 6;
			for (int c = 0; c < nc && c < MAX_CLIPS && t + 6 <= end; c++, t += 6) {
				m->clips[c].first = (uint16_t)(t[0] | t[1] << 8);
				m->clips[c].frames = t[2] ? t[2] : 1, m->clips[c].fps = t[3] ? t[3] : 10;
				m->clips[c].loop = t[4], m->clips[c].act = t[5];
				m->numclips = c + 1;
			}
			t = d + at + 6 + nc * 6;
			ns = t + 2 <= end ? t[0] | t[1] << 8 : 0;
			t += 2;
			if (ns > 0 && t + ns + 768 <= end && (m->seqclip = malloc((size_t)ns))) {
				memcpy(m->seqclip, t, (size_t)ns);
				m->numseqs = ns;
				t += ns;
				for (int i = 0; i < 256; i++)
					ownpal[i] = RGB15(t[i * 3] >> 3, t[i * 3 + 1] >> 3, t[i * 3 + 2] >> 3);
				haveown = true;
			}
		}
	}
	p = d + HEADER;
	for (int s = 0; s < numskins; s++) {
		int type = rd32(p);
		p += 4;
		if (type == 0) {
			if (!skin)
				skin = p;
			p += sw * sh;
		} else {
			int n = rd32(p);
			if (!skin)
				skin = p + 4 + n * 4;
			p += 4 + n * 4 + n * sw * sh;
		}
		if (p > end)
			goto fail;
	}
	if (skin)
		m->tex = upload_skin(skin, sw, sh, &tw, &th, haveown ? ownpal : NULL);

	/* triangles, with their texture coordinates at each corner */
	{
		const uint8_t *st = p, *tri = p + nv * 12;
		if (tri + nt * 16 > end)
			goto fail;
		m->tris = malloc(sizeof(mtri_t) * (size_t)nt);
		idx = malloc(sizeof(int32_t) * (size_t)nt * 3);
		if (!m->tris || !idx)
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
				idx[t * 3 + k] = v;
			}
		}
		p = tri + nt * 16;
	}
	m->numverts = nv, m->numtris = nt;

	/* frames, flattening groups, up to MAX_FRAMES */
	maxframes = m->numclips ? MAX_HL_FRAMES : MAX_FRAMES;
	m->frames = malloc((size_t)BW_NDS_MIN(nf * 4, maxframes) * nv * 4);
	if (!m->frames)
		goto fail;
	for (int f = 0; f < nf && m->numframes < maxframes; f++) {
		int type = rd32(p), n = 1;
		p += 4;
		if (type != 0) {
			n = rd32(p);
			p += 4 + 8 + n * 4; /* count, bounds, intervals */
		}
		for (int g = 0; g < n && m->numframes < maxframes; g++) {
			uint8_t *fr = m->frames + (size_t)m->numframes * nv * 4;
			p += 8 + 16; /* bounds and name */
			if (p + nv * 4 > end)
				goto fail;
			for (int v = 0; v < nv; v++)
				fr[v * 4] = p[v * 4], fr[v * 4 + 1] = p[v * 4 + 1], fr[v * 4 + 2] = p[v * 4 + 2];
			shade_frame(m, fr, idx, scale);
			m->numframes++;
			p += nv * 4;
		}
	}
	if (!m->numframes)
		goto fail;
	free(idx);
	free(d);
	m->bad = false;
	return (int)(m - mdls);
fail:
	free(idx);
	free(d);
	free(m->tris);
	free(m->frames);
	free(m->seqclip);
	m->tris = NULL, m->frames = NULL, m->seqclip = NULL;
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
	e->frame = -1;
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

/* speed: map units a second the thing is moving at, to walk or run a model
 * the server only has standing (players animate on their own machines) */
void Ents_AddLinked(int mdl, const int16_t *org, int yaw, int seq, int elapsed, int speed)
{
	const mdl_t *m;
	ent_t *e;
	float o[3] = {org[0], org[1], org[2]};
	int clip = 0, f;
	if (mdl < 0 || mdl >= nummdls || mdls[mdl].bad || numents >= MAX_ENTS)
		return;
	m = &mdls[mdl];
	add_ent(mdl, o, yaw * 360.0f / 32768, 0);
	e = &ents[numents - 1];
	if (!m->numclips)
		return;
	if (seq >= 0 && seq < m->numseqs)
		clip = m->seqclip[seq];
	if (m->clips[clip].act == 1 || clip == 0) { /* standing while moving: walk or run */
		int c = speed > 200 ? clip_for(m, 4) : speed > 40 ? clip_for(m, 3) : -1;
		if (c < 0 && speed > 40)
			c = clip_for(m, speed > 200 ? 3 : 4);
		if (c >= 0)
			clip = c, elapsed = ticks;
	}
	f = elapsed * m->clips[clip].fps / 60;
	f = m->clips[clip].loop ? f % m->clips[clip].frames : BW_NDS_MIN(f, m->clips[clip].frames - 1);
	e->frame = m->clips[clip].first + f;
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
}

ITCM_CODE static void draw_mdl(const mdl_t *m, int frame, int light)
{
	const uint8_t *fr = m->frames + (size_t)(frame % m->numframes) * m->numverts * 4;
	uint16_t lut[32];
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
	draw_mdl(m, e->frame >= 0 ? e->frame : ticks / 6 + e->phase, e->light);
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
		ndscam_t eyecam = {{0, 0, 0}, 0, 0};
		const mdl_t *m = &mdls[viewmodel];
		World_SetView(&eyecam);
		glTranslatef32(m->trans[0], m->trans[1], m->trans[2]);
		draw_mdl(m, 0, 24);
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
