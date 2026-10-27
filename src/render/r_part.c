/* Bulwark engine - particle effects from scripts in the r_part format
 * (fteqw-bulwark specs/particles.txt): an effect named "file.effect" lives
 * in particles/file.cfg. Each effect is a chain of particle types, spawned
 * at a point (pointparticles) or along a line (trailparticles). Particles
 * move with gravity and friction, change colour, size and alpha over time,
 * may bounce or spawn other effects when they hit the world, and may emit
 * effects or dynamic lights. Decal types are placed on the world through
 * r_decal.c. What the spec leaves open is decided in docs/spec/particles.md.
 * ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/cmd.h"
#include "../common/fs.h"
#include "../sound/sound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum { PT_NORMAL, PT_SPARK, PT_TEXTUREDSPARK, PT_BEAM, PT_DECAL, PT_UDECAL };
enum { SM_BOX, SM_BALL, SM_CIRCLE, SM_UNIFORMCIRCLE, SM_TELEBOX, SM_LAVASPLASH, SM_SPIRAL, SM_TRACER };
enum { RAMP_NONE, RAMP_NEAREST, RAMP_LERP, RAMP_DELTA };

#define MAX_RAMP 16
#define NAMELEN 96

typedef struct ptype_s {
	char name[NAMELEN]; /* "file.effect", or "effect" for files without a prefix */
	struct ptype_s *chain; /* the next "+name" part */
	struct ptype_s *next;  /* every type, for lookups */
	bool head;

	char texname[128];
	material_t *mat;
	float tc[4];
	int rsmax;
	float rsstep;
	int atlas, atlasfirst, atlaslast;
	bool fulltc; /* the texture was missing: the built-in dot uses all of it */

	int kind;
	float scale[2], scalefactor, stretch, scaledelta;
	float step[2], count[2];
	float alpha, alpharand, alphadelta;
	float die[2];
	float randomvel[2], veladd, orgadd;
	float orgbias[3], velbias[3], orgwrand[3], velwrand[3];
	float spawnorg[2], spawnvel[2];
	float friction[3], gravity, clipbounce, clipcount;
	char cliptype[NAMELEN];
	float rgb[3], rgbrand[3], rgbrandsync[3], rgbdelta[3], rgbdeltatime;
	int colorindex, colorrand;
	int rampmode, numramp;
	float ramp[MAX_RAMP][5];
	int blend;
	GLenum src, dst;
	int spawnmode;
	float spawnparam[2];
	float rotation[4];
	char assoc[NAMELEN], emit[NAMELEN];
	float emitinterval, emitintervalrand, emitstart;
	float lightradius, lightradiusfade, lightrgb[3], lightrgbfade[3], lighttime;
	float spawnchance;
	bool perframe, nospreadfirst, nospreadlast;
	char sound[128];
	float soundvol, soundattn, soundpitch;
	float beamtexstep, beamtexspeed;
} ptype_t;

typedef struct {
	ptype_t *t;
	float org[3], vel[3], end[3];
	float rgba[4], scale, rot, rotspeed;
	float age, life, nextemit;
	float tc[4];
} particle_t;

typedef struct {
	float org[3], rgb[3], rgbfade[3];
	float radius, fade, die;
} plight_t;

#define MAX_PARTICLES 32768
#define MAX_PLIGHTS 64
#define MAX_EFFECT_INDEX 1024

static particle_t parts[MAX_PARTICLES];
static int numparts;
static plight_t plights[MAX_PLIGHTS];
static int numplights;
static ptype_t *types;
static double lastupdate;
static image_t *dotimage;
static material_t *dotmat;
static cvar_t *r_drawparticles, *r_particles_max, *r_part_density;

/* files read so far, or found missing */
typedef struct pfile_s {
	char name[64];
	struct pfile_s *next;
} pfile_t;
static pfile_t *files;

/* effect index (cl.particles) to its first type, looked up once */
static ptype_t *byindex[MAX_EFFECT_INDEX];
static bool resolved[MAX_EFFECT_INDEX];

static uint32_t rng = 0x9e3779b9u;
static float frand(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (rng >> 8) * (1.0f / 16777216.0f);
}
static float crand(void) { return frand() * 2 - 1; }
static float rrange(float a, float b) { return a + (b - a) * frand(); }

/* --- scripts --------------------------------------------------------------------- */

typedef struct {
	char text[256];
	bool newline; /* first on its line */
} ptok_t;

static int tokenize(const char *s, ptok_t **out)
{
	int n = 0, cap = 0;
	bool nl = true;
	ptok_t *t = NULL;
	while (*s) {
		const char *start;
		size_t len;
		if (*s == '\n') {
			nl = true;
			s++;
			continue;
		}
		if (*s == ' ' || *s == '\t' || *s == '\r' || *s == ';') {
			s++;
			continue;
		}
		if (s[0] == '/' && s[1] == '/') {
			while (*s && *s != '\n')
				s++;
			continue;
		}
		if (s[0] == '/' && s[1] == '*') {
			s += 2;
			while (*s && !(s[0] == '*' && s[1] == '/'))
				s++;
			if (*s)
				s += 2;
			continue;
		}
		if (n == cap) {
			cap = cap ? cap * 2 : 256;
			t = Mem_Realloc(t, sizeof(ptok_t) * cap);
		}
		if (*s == '"') {
			start = ++s;
			while (*s && *s != '"' && *s != '\n')
				s++;
			len = (size_t)(s - start);
			if (*s == '"')
				s++;
		} else if (*s == '{' || *s == '}') {
			start = s++;
			len = 1;
		} else {
			start = s;
			while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n' && *s != '{' && *s != '}' && *s != ';')
				s++;
			len = (size_t)(s - start);
		}
		len = BW_MIN(len, sizeof(t[n].text) - 1);
		memcpy(t[n].text, start, len);
		t[n].text[len] = 0;
		t[n].newline = nl;
		nl = false;
		n++;
	}
	*out = t;
	return n;
}

static ptype_t *find_type(const char *name)
{
	for (ptype_t *t = types; t; t = t->next)
		if (t->head && !Str_ICmp(t->name, name))
			return t;
	return NULL;
}

static void type_defaults(ptype_t *t)
{
	t->tc[0] = 0, t->tc[1] = 0, t->tc[2] = 1, t->tc[3] = 1;
	t->scale[0] = t->scale[1] = 1;
	t->scalefactor = 1;
	t->stretch = 1;
	t->count[0] = t->count[1] = 1;
	t->alpha = 1;
	t->die[0] = t->die[1] = 1;
	t->rgb[0] = t->rgb[1] = t->rgb[2] = 1;
	t->colorindex = -1;
	t->blend = BLEND_ALPHA;
	t->src = GL_SRC_ALPHA;
	t->dst = GL_ONE_MINUS_SRC_ALPHA;
	t->clipbounce = 0.8f;
	t->clipcount = 1;
	t->spawnchance = 1;
	t->soundvol = 1;
	t->soundattn = 1;
	t->soundpitch = 100;
	t->beamtexstep = 64;
}

static float argf(const ptok_t *a, int n, int i, float def) { return i < n ? (float)atof(a[i].text) : def; }

static void set_blend(ptype_t *t, const char *m)
{
	static const struct {
		const char *name;
		GLenum src, dst;
	} modes[] = {
		{"add", GL_SRC_ALPHA, GL_ONE},
		{"adda", GL_SRC_ALPHA, GL_ONE},
		{"addc", GL_SRC_COLOR, GL_ONE},
		{"subtract", GL_ZERO, GL_ONE_MINUS_SRC_COLOR},
		{"invmoda", GL_ZERO, GL_ONE_MINUS_SRC_ALPHA},
		{"invmodc", GL_ZERO, GL_ONE_MINUS_SRC_COLOR},
		{"blendcolour", GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR},
		{"blendcolor", GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR},
		{"blendalpha", GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA},
		{"blend", GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA},
		{"premul_subtract", GL_ZERO, GL_ONE_MINUS_SRC_COLOR},
		{"premul_add", GL_ONE, GL_ONE},
		{"premul_blend", GL_ONE, GL_ONE_MINUS_SRC_ALPHA},
		{"rtsmoke", GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA},
	};
	for (size_t i = 0; i < BW_ARRAY_COUNT(modes); i++)
		if (!Str_ICmp(m, modes[i].name)) {
			t->blend = BLEND_CUSTOM;
			t->src = modes[i].src;
			t->dst = modes[i].dst;
			return;
		}
}

static int vec_args(const ptok_t *a, int n, float *v, float scale)
{
	if (n >= 4) {
		for (int k = 0; k < 3; k++)
			v[k] = argf(a, n, k + 1, 0) * scale;
	} else if (n == 3) {
		v[0] = v[1] = argf(a, n, 1, 0) * scale;
		v[2] = argf(a, n, 2, 0) * scale;
	} else {
		v[0] = v[1] = v[2] = argf(a, n, 1, 0) * scale;
	}
	return n;
}

/* one property line: a[0] is the key */
static void parse_property(ptype_t *t, const ptok_t *a, int n)
{
	const char *k = a[0].text;
#define IS(x) (!Str_ICmp(k, x))
	if (IS("texture") && n > 1)
		Str_Copy(t->texname, a[1].text, sizeof(t->texname));
	else if (IS("shader") && n > 1)
		Str_Copy(t->texname, a[1].text, sizeof(t->texname));
	else if (IS("tcoords") && n > 4) {
		float sc = argf(a, n, 5, 1);
		if (sc == 0)
			sc = 1;
		for (int i = 0; i < 4; i++)
			t->tc[i] = argf(a, n, i + 1, 0) / sc;
		t->rsmax = (int)argf(a, n, 6, 1);
		t->rsstep = argf(a, n, 7, 0) / sc;
		t->atlas = 0;
	} else if (IS("atlas") && n > 2) {
		t->atlas = BW_MAX(1, (int)argf(a, n, 1, 1));
		t->atlasfirst = (int)argf(a, n, 2, 0);
		t->atlaslast = (int)argf(a, n, 3, (float)t->atlasfirst);
	} else if (IS("rotation")) {
		for (int i = 0; i < 4; i++)
			t->rotation[i] = argf(a, n, i + 1, 0);
	} else if (IS("rotationstart")) {
		t->rotation[0] = argf(a, n, 1, 0);
		t->rotation[1] = argf(a, n, 2, t->rotation[0]);
	} else if (IS("rotationspeed")) {
		t->rotation[2] = argf(a, n, 1, 0);
		t->rotation[3] = argf(a, n, 2, t->rotation[2]);
	} else if (IS("beamtexstep"))
		t->beamtexstep = argf(a, n, 1, 64);
	else if (IS("beamtexspeed"))
		t->beamtexspeed = argf(a, n, 1, 0);
	else if (IS("scale")) {
		t->scale[0] = argf(a, n, 1, 1);
		t->scale[1] = argf(a, n, 2, t->scale[0]);
	} else if (IS("scalerand"))
		t->scale[1] = t->scale[0] + argf(a, n, 1, 0);
	else if (IS("scalefactor"))
		t->scalefactor = argf(a, n, 1, 1);
	else if (IS("stretchfactor"))
		t->stretch = argf(a, n, 1, 1);
	else if (IS("scaledelta"))
		t->scaledelta = argf(a, n, 1, 0);
	else if (IS("step")) {
		t->step[0] = argf(a, n, 1, 0);
		t->step[1] = argf(a, n, 2, t->step[0]);
	} else if (IS("count")) {
		t->count[0] = argf(a, n, 1, 1);
		t->count[1] = argf(a, n, 2, t->count[0]);
	} else if (IS("alpha"))
		t->alpha = argf(a, n, 1, 1);
	else if (IS("alpharand"))
		t->alpharand = argf(a, n, 1, 0);
	else if (IS("alphadelta"))
		t->alphadelta = argf(a, n, 1, 0);
	else if (IS("die")) {
		t->die[0] = argf(a, n, 1, 1);
		t->die[1] = argf(a, n, 2, t->die[0]);
	} else if (IS("diesubrand"))
		t->die[1] = t->die[0] - argf(a, n, 1, 0);
	else if (IS("randomvel")) {
		t->randomvel[0] = argf(a, n, 1, 0);
		t->randomvel[1] = argf(a, n, 2, t->randomvel[0]);
	} else if (IS("veladd"))
		t->veladd = argf(a, n, 1, 0);
	else if (IS("orgadd"))
		t->orgadd = argf(a, n, 1, 0);
	else if (IS("orgbias"))
		vec_args(a, n, t->orgbias, 1);
	else if (IS("velbias"))
		vec_args(a, n, t->velbias, 1);
	else if (IS("orgwrand"))
		vec_args(a, n, t->orgwrand, 1);
	else if (IS("velwrand"))
		vec_args(a, n, t->velwrand, 1);
	else if (IS("spawnorg")) {
		t->spawnorg[0] = argf(a, n, 1, 0);
		t->spawnorg[1] = argf(a, n, 2, t->spawnorg[0]);
	} else if (IS("spawnvel")) {
		t->spawnvel[0] = argf(a, n, 1, 0);
		t->spawnvel[1] = argf(a, n, 2, t->spawnvel[0]);
	} else if (IS("friction"))
		vec_args(a, n, t->friction, 1);
	else if (IS("gravity"))
		t->gravity = argf(a, n, 1, 0);
	else if (IS("clipbounce"))
		t->clipbounce = argf(a, n, 1, 0.8f);
	else if (IS("cliptype") && n > 1)
		Str_Copy(t->cliptype, a[1].text, sizeof(t->cliptype));
	else if (IS("clipcount"))
		t->clipcount = argf(a, n, 1, 1);
	else if (IS("assoc") && n > 1)
		Str_Copy(t->assoc, a[1].text, sizeof(t->assoc));
	else if (IS("colorindex")) {
		t->colorindex = (int)argf(a, n, 1, 0);
		t->colorrand = (int)argf(a, n, 2, 0);
	} else if (IS("colorrand"))
		t->colorrand = (int)argf(a, n, 1, 0);
	else if (IS("red"))
		t->rgb[0] = argf(a, n, 1, 255) / 255;
	else if (IS("green"))
		t->rgb[1] = argf(a, n, 1, 255) / 255;
	else if (IS("blue"))
		t->rgb[2] = argf(a, n, 1, 255) / 255;
	else if (IS("rgb"))
		vec_args(a, n == 3 ? 2 : n, t->rgb, 1.0f / 255);
	else if (IS("rgbf"))
		vec_args(a, n == 3 ? 2 : n, t->rgb, 1);
	else if (IS("redrand"))
		t->rgbrand[0] = argf(a, n, 1, 0) / 255;
	else if (IS("greenrand"))
		t->rgbrand[1] = argf(a, n, 1, 0) / 255;
	else if (IS("bluerand"))
		t->rgbrand[2] = argf(a, n, 1, 0) / 255;
	else if (IS("rgbrand"))
		vec_args(a, n == 3 ? 2 : n, t->rgbrand, 1.0f / 255);
	else if (IS("rgbrandf"))
		vec_args(a, n == 3 ? 2 : n, t->rgbrand, 1);
	else if (IS("redrandsync"))
		t->rgbrandsync[0] = argf(a, n, 1, 0) / 255;
	else if (IS("greenrandsync"))
		t->rgbrandsync[1] = argf(a, n, 1, 0) / 255;
	else if (IS("bluerandsync"))
		t->rgbrandsync[2] = argf(a, n, 1, 0) / 255;
	else if (IS("rgbrandsync"))
		vec_args(a, n == 3 ? 2 : n, t->rgbrandsync, 1.0f / 255);
	else if (IS("reddelta"))
		t->rgbdelta[0] = argf(a, n, 1, 0) / 255;
	else if (IS("greendelta"))
		t->rgbdelta[1] = argf(a, n, 1, 0) / 255;
	else if (IS("bluedelta"))
		t->rgbdelta[2] = argf(a, n, 1, 0) / 255;
	else if (IS("rgbdelta"))
		vec_args(a, n == 3 ? 2 : n, t->rgbdelta, 1.0f / 255);
	else if (IS("rgbdeltaf"))
		vec_args(a, n == 3 ? 2 : n, t->rgbdelta, 1);
	else if (IS("rgbdeltatime"))
		t->rgbdeltatime = argf(a, n, 1, 0);
	else if (IS("rampmode") && n > 1) {
		const char *m = a[1].text;
		t->rampmode = !Str_ICmp(m, "nearest") || !Str_ICmp(m, "absolute") ? RAMP_NEAREST
		              : !Str_ICmp(m, "lerp")                               ? RAMP_LERP
		              : !Str_ICmp(m, "delta")                              ? RAMP_DELTA
		                                                                   : RAMP_NONE;
	} else if (IS("ramp") && t->numramp < MAX_RAMP) {
		float *r = t->ramp[t->numramp++];
		r[0] = argf(a, n, 1, 0) / 255;
		r[1] = argf(a, n, 2, 0) / 255;
		r[2] = argf(a, n, 3, 0) / 255;
		r[3] = argf(a, n, 4, 1);
		r[4] = argf(a, n, 5, t->scale[0]);
	} else if ((IS("rampindex") || IS("rampindexlist"))) {
		const uint8_t *pal = R_Palette();
		bool list = IS("rampindexlist");
		for (int i = 1; i < n && t->numramp < MAX_RAMP; i += list ? 1 : 2) {
			int c = BW_MAX(0, BW_MIN(255, atoi(a[i].text)));
			float *r = t->ramp[t->numramp++];
			r[0] = pal[c * 3] / 255.0f;
			r[1] = pal[c * 3 + 1] / 255.0f;
			r[2] = pal[c * 3 + 2] / 255.0f;
			r[3] = 1;
			r[4] = list ? t->scale[0] : argf(a, n, i + 1, t->scale[0]);
		}
	} else if (IS("blend") && n > 1)
		set_blend(t, a[1].text);
	else if (IS("spawnmode") && n > 1) {
		const char *m = a[1].text;
		t->spawnmode = !Str_ICmp(m, "ball") || !Str_ICmp(m, "distball") ? SM_BALL
		               : !Str_ICmp(m, "circle")                         ? SM_CIRCLE
		               : !Str_ICmp(m, "uniformcircle")                  ? SM_UNIFORMCIRCLE
		               : !Str_ICmp(m, "telebox")                        ? SM_TELEBOX
		               : !Str_ICmp(m, "lavasplash")                     ? SM_LAVASPLASH
		               : !Str_ICmp(m, "spiral")                         ? SM_SPIRAL
		               : !Str_ICmp(m, "tracer")                         ? SM_TRACER
		                                                                : SM_BOX;
		t->spawnparam[0] = argf(a, n, 2, t->spawnparam[0]);
		t->spawnparam[1] = argf(a, n, 3, t->spawnparam[1]);
	} else if (IS("spawnparam1"))
		t->spawnparam[0] = argf(a, n, 1, 0);
	else if (IS("spawnparam2"))
		t->spawnparam[1] = argf(a, n, 1, 0);
	else if (IS("up"))
		t->orgbias[2] = argf(a, n, 1, 0);
	else if (IS("type") && n > 1) {
		const char *m = a[1].text;
		t->kind = !Str_ICmp(m, "beam")                                ? PT_BEAM
		          : !Str_ICmp(m, "spark") || !Str_ICmp(m, "sparkfan") ? PT_SPARK
		          : !Str_ICmp(m, "texturedspark")                     ? PT_TEXTUREDSPARK
		          : !Str_ICmp(m, "decal") || !Str_ICmp(m, "cdecal")   ? PT_DECAL
		          : !Str_ICmp(m, "udecal")                            ? PT_UDECAL
		                                                              : PT_NORMAL;
	} else if (IS("isbeam"))
		t->kind = PT_BEAM;
	else if (IS("clippeddecal"))
		t->kind = PT_DECAL;
	else if (IS("emit") && n > 1)
		Str_Copy(t->emit, a[1].text, sizeof(t->emit));
	else if (IS("emitinterval"))
		t->emitinterval = argf(a, n, 1, 0);
	else if (IS("emitintervalrand"))
		t->emitintervalrand = argf(a, n, 1, 0);
	else if (IS("emitstart"))
		t->emitstart = argf(a, n, 1, 0);
	else if (IS("lightradius"))
		t->lightradius = argf(a, n, 1, 0);
	else if (IS("lightradiusfade"))
		t->lightradiusfade = argf(a, n, 1, 0);
	else if (IS("lightrgb"))
		vec_args(a, n, t->lightrgb, 1);
	else if (IS("lightrgbfade"))
		vec_args(a, n, t->lightrgbfade, 1);
	else if (IS("lighttime"))
		t->lighttime = argf(a, n, 1, 0);
	else if (IS("spawnchance"))
		t->spawnchance = argf(a, n, 1, 1);
	else if (IS("perframe"))
		t->perframe = true;
	else if (IS("nospreadfirst"))
		t->nospreadfirst = true;
	else if (IS("nospreadlast"))
		t->nospreadlast = true;
	else if (IS("sound") && n > 1) {
		Str_Copy(t->sound, a[1].text, sizeof(t->sound));
		for (int i = 2; i < n; i++) {
			const char *o = a[i].text;
			if (!Str_NICmp(o, "vol=", 4))
				t->soundvol = (float)atof(o + 4);
			else if (!Str_NICmp(o, "attn=", 5))
				t->soundattn = (float)atof(o + 5);
			else if (!Str_NICmp(o, "pitch=", 6))
				t->soundpitch = (float)atof(o + 6);
			else if (i == 2)
				t->soundvol = (float)atof(o);
			else if (i == 3)
				t->soundattn = (float)atof(o);
			else if (i == 4)
				t->soundpitch = (float)atof(o);
		}
	}
	/* not done: model, spawnstain, stains, rainfrequency, flurry, viewspace,
	 * underwater, notunderwater, inwater, light cubemaps and coronas */
#undef IS
}

static void load_file(const char *file)
{
	char path[128];
	char *text;
	size_t len;
	ptok_t *tok;
	int n, i = 0;
	pfile_t *pf;
	ptype_t *last = NULL;
	for (pf = files; pf; pf = pf->next)
		if (!Str_ICmp(pf->name, file))
			return;
	pf = Mem_Alloc(sizeof(*pf));
	Str_Copy(pf->name, file, sizeof(pf->name));
	pf->next = files;
	files = pf;
	snprintf(path, sizeof(path), "particles/%s.cfg", file);
	text = FS_LoadFile(path, &len);
	if (!text) {
		Con_DPrintf("no particle file %s\n", path);
		return;
	}
	n = tokenize(text, &tok);
	Mem_Free(text);
	while (i < n) {
		ptype_t *t;
		char full[320];
		const char *name;
		bool plus;
		if (Str_ICmp(tok[i].text, "r_part") || i + 1 >= n) {
			i++;
			continue;
		}
		name = tok[i + 1].text;
		plus = name[0] == '+';
		if (plus)
			name++;
		snprintf(full, sizeof(full), "%s.%s", file, name);
		i += 2;
		if (i >= n || strcmp(tok[i].text, "{")) {
			continue;
		}
		i++;
		t = Mem_Alloc(sizeof(*t));
		type_defaults(t);
		Str_Copy(t->name, full, sizeof(t->name));
		if (plus && last && !Str_ICmp(last->name, full)) {
			ptype_t *c = last;
			while (c->chain)
				c = c->chain;
			c->chain = t;
		} else {
			/* a new head replaces an earlier effect of the same name */
			ptype_t *old = find_type(full);
			if (old)
				old->head = false;
			t->head = true;
			last = t;
		}
		t->next = types;
		types = t;
		while (i < n && strcmp(tok[i].text, "}")) {
			int start = i, cnt;
			i++;
			while (i < n && !tok[i].newline && strcmp(tok[i].text, "}") && strcmp(tok[i].text, "{"))
				i++;
			cnt = i - start;
			parse_property(t, tok + start, cnt);
			/* a shader's body: skip it */
			if (i < n && !strcmp(tok[i].text, "{")) {
				int depth = 0;
				do {
					if (!strcmp(tok[i].text, "{"))
						depth++;
					else if (!strcmp(tok[i].text, "}"))
						depth--;
					i++;
				} while (i < n && depth > 0);
			}
		}
		i++; /* the closing brace */
	}
	Mem_Free(tok);
}

/* "file.effect": the file is read the first time it is named */
static ptype_t *effect_by_name(const char *name)
{
	const char *dot = strchr(name, '.');
	ptype_t *t;
	if (!name || !*name)
		return NULL;
	if (dot && dot > name) {
		char file[64];
		snprintf(file, sizeof(file), "%.*s", (int)BW_MIN((size_t)(dot - name), sizeof(file) - 1), name);
		load_file(file);
	}
	if ((t = find_type(name)) != NULL)
		return t;
	if (!dot) {
		/* an unprefixed name matches the effect in any file read so far */
		for (t = types; t; t = t->next) {
			const char *d = strchr(t->name, '.');
			if (t->head && d && !Str_ICmp(d + 1, name))
				return t;
		}
	}
	return NULL;
}

static ptype_t *effect_by_index(int index)
{
	if (index <= 0 || index >= MAX_EFFECT_INDEX || index >= CL_MAX_PARTICLES || !cl.particles[index])
		return NULL;
	if (!resolved[index]) {
		resolved[index] = true;
		byindex[index] = effect_by_name(cl.particles[index]);
		if (!byindex[index])
			Con_DPrintf("unknown particle effect %s\n", cl.particles[index]);
	}
	return byindex[index];
}

/* a material for the type: the built-in soft dot when the picture is missing */
static void type_material(ptype_t *t)
{
	if (t->mat)
		return;
	if (!dotmat) {
		uint8_t px[32 * 32 * 4];
		for (int y = 0; y < 32; y++)
			for (int x = 0; x < 32; x++) {
				float dx = (x + 0.5f) / 16 - 1, dy = (y + 0.5f) / 16 - 1, d = sqrtf(dx * dx + dy * dy);
				uint8_t *p = px + (y * 32 + x) * 4;
				p[0] = p[1] = p[2] = 255;
				p[3] = (uint8_t)(BW_MAX(0.0f, BW_MIN(1.0f, 1 - d)) * 255);
			}
		dotimage = R_ImageFromPixels("$particledot", px, 32, 32, 0);
		dotmat = R_MaterialForImage("$particledot", dotimage, 0);
	}
	if (t->kind == PT_SPARK || !t->texname[0]) {
		t->mat = dotmat;
		t->fulltc = true;
		return;
	}
	t->mat = R_FindMaterial(t->texname, 0);
	if (!t->mat->diffuse || t->mat->diffuse->missing || t->mat->diffuse == r_checkerimage) {
		t->mat = dotmat;
		t->fulltc = true;
	}
}

/* --- spawning ------------------------------------------------------------------- */

static void spawn_effect(ptype_t *head, const float *org, const float *dir, float count, int depth);

static void particle_colour(const ptype_t *t, float *rgba)
{
	if (t->colorindex >= 0) {
		const uint8_t *pal = R_Palette();
		int c = BW_MAX(0, BW_MIN(255, t->colorindex + (t->colorrand > 0 ? (int)(frand() * (t->colorrand + 1)) : 0)));
		for (int k = 0; k < 3; k++)
			rgba[k] = pal[c * 3 + k] / 255.0f;
	} else {
		float sync = frand();
		for (int k = 0; k < 3; k++)
			rgba[k] = t->rgb[k] + t->rgbrand[k] * frand() + t->rgbrandsync[k] * sync;
	}
	rgba[3] = t->alpha + t->alpharand * frand();
}

static void pick_tc(const ptype_t *t, float *tc)
{
	if (t->fulltc) {
		tc[0] = 0, tc[1] = 0, tc[2] = 1, tc[3] = 1;
	} else if (t->atlas > 0) {
		int idx = t->atlasfirst + (t->atlaslast > t->atlasfirst ? (int)(frand() * (t->atlaslast - t->atlasfirst + 1)) : 0);
		float c = 1.0f / t->atlas;
		tc[0] = (idx % t->atlas) * c;
		tc[1] = (idx / t->atlas) * c;
		tc[2] = tc[0] + c;
		tc[3] = tc[1] + c;
	} else {
		float off = t->rsmax > 1 ? (int)(frand() * t->rsmax) * t->rsstep : 0;
		tc[0] = t->tc[0] + off;
		tc[1] = t->tc[1];
		tc[2] = t->tc[2] + off;
		tc[3] = t->tc[3];
	}
}

static particle_t *new_particle(ptype_t *t)
{
	int max = r_particles_max ? BW_MAX(0, BW_MIN(MAX_PARTICLES, r_particles_max->ivalue)) : MAX_PARTICLES;
	particle_t *p;
	if (numparts >= max)
		return NULL;
	p = &parts[numparts++];
	memset(p, 0, sizeof(*p));
	p->t = t;
	particle_colour(t, p->rgba);
	p->scale = rrange(t->scale[0], t->scale[1]);
	p->rot = rrange(t->rotation[0], t->rotation[1]);
	p->rotspeed = rrange(t->rotation[2], t->rotation[3]);
	p->life = rrange(t->die[1], t->die[0]);
	if (p->life <= 0)
		p->life = 0.001f;
	p->nextemit = t->emitstart;
	pick_tc(t, p->tc);
	return p;
}

/* random point in a unit ball */
static void rand_ball(float *v)
{
	do {
		v[0] = crand();
		v[1] = crand();
		v[2] = crand();
	} while (DotProduct(v, v) > 1);
}

/* placement and velocity shared by point and trail spawns */
static void finish_spawn(ptype_t *t, particle_t *p, const float *dir, bool spread)
{
	for (int k = 0; k < 3; k++) {
		float rv = t->randomvel[k == 2 ? 1 : 0];
		if (spread)
			p->vel[k] += crand() * rv;
		p->vel[k] += dir[k] * t->veladd + t->velbias[k] + (spread ? crand() * t->velwrand[k] : 0);
		p->org[k] += dir[k] * t->orgadd + t->orgbias[k] + (spread ? crand() * t->orgwrand[k] : 0);
	}
}

static void spawn_light(const ptype_t *t, const float *org)
{
	plight_t *l;
	if (t->lightradius <= 0 || numplights >= MAX_PLIGHTS)
		return;
	l = &plights[numplights++];
	VectorCopy(org, l->org);
	VectorCopy(t->lightrgb, l->rgb);
	VectorCopy(t->lightrgbfade, l->rgbfade);
	l->radius = t->lightradius;
	l->fade = t->lightradiusfade;
	l->die = (float)r_time + (t->lighttime > 0 ? t->lighttime : 0.1f);
}

static void spawn_decal(ptype_t *t, const float *org, const float *dir)
{
	float n[3], rgba[4], tc[4], o[3];
	VectorCopy(dir, n);
	VectorCopy(org, o);
	for (int k = 0; k < 3; k++)
		o[k] += t->orgbias[k] + crand() * t->orgwrand[k];
	if (VectorNormalize(n) < 1e-4f) {
		/* no direction given: face away from the nearest wall */
		int s = R_SurfaceNearPoint(&r_world.submodels[0], o);
		const rsurface_t *rs = R_ModelSurface(&r_world.submodels[0], s);
		float sa[3], ta[3];
		if (!rs)
			return;
		R_SurfaceAxes(rs, sa, ta, n);
	}
	type_material(t);
	particle_colour(t, rgba);
	pick_tc(t, tc);
	{
		int b = t->blend;
		GLenum src = t->src, dst = t->dst;
		R_SpawnDecal(t->mat, b, src, dst, o, n, rrange(t->scale[0], t->scale[1]) * 0.5f,
		             rrange(t->rotation[0], t->rotation[1]), tc, rgba, t->alphadelta, rrange(t->die[1], t->die[0]));
	}
}

static void spawn_point(ptype_t *t, const float *org, const float *dir, float count, int depth)
{
	int n;
	float density = r_part_density ? r_part_density->value : 1;
	if (t->spawnchance < 1 && frand() > t->spawnchance)
		return;
	if (t->sound[0] && depth == 0)
		S_StartSound(0, 0, t->sound, org, t->soundvol, t->soundattn, t->soundpitch / 100.0f, 0);
	spawn_light(t, org);
	if (t->kind == PT_DECAL || t->kind == PT_UDECAL) {
		if (t->count[0] > 0 || t->count[1] > 0)
			spawn_decal(t, org, dir);
		return;
	}
	n = (int)(rrange(t->count[0], t->count[1]) * count * density + frand());
	type_material(t);
	for (int i = 0, prev = -1; i < n; i++) {
		particle_t *p = new_particle(t);
		float r[3], h = t->spawnorg[0], v = t->spawnorg[1], sv = t->spawnvel[0], svv = t->spawnvel[1];
		bool spread = !((i == 0 && t->nospreadfirst) || (i == n - 1 && t->nospreadlast));
		if (!p)
			break;
		VectorCopy(org, p->org);
		switch (t->spawnmode) {
		case SM_BALL:
			rand_ball(r);
			for (int k = 0; k < 3; k++) {
				p->org[k] += r[k] * (k == 2 ? v : h);
				p->vel[k] += r[k] * (k == 2 ? svv : sv);
			}
			break;
		case SM_CIRCLE:
		case SM_UNIFORMCIRCLE: {
			float turns = t->spawnparam[0] != 0 ? t->spawnparam[0] : 1;
			float a = t->spawnmode == SM_UNIFORMCIRCLE ? (float)i / BW_MAX(1, n) * 2 * (float)M_PI * turns
			                                          : frand() * 2 * (float)M_PI * turns;
			r[0] = cosf(a), r[1] = sinf(a), r[2] = t->spawnmode == SM_CIRCLE ? crand() : 0;
			for (int k = 0; k < 3; k++) {
				p->org[k] += r[k] * (k == 2 ? v : h);
				p->vel[k] += r[k] * (k == 2 ? svv : sv);
			}
			break;
		}
		case SM_TELEBOX:
		case SM_LAVASPLASH:
		case SM_BOX:
		default:
			for (int k = 0; k < 3; k++) {
				float c = crand();
				p->org[k] += c * (k == 2 ? v : h);
				p->vel[k] += crand() * (k == 2 ? svv : sv);
			}
			break;
		}
		finish_spawn(t, p, dir, spread);
		if (t->kind == PT_BEAM) {
			/* a point effect's beam joins its particles in spawn order;
			 * the first has nothing to join, so it draws nothing (and
			 * may still emit) instead of stretching to the origin */
			VectorCopy(prev >= 0 ? parts[prev].org : p->org, p->end);
			prev = (int)(p - parts);
		}
	}
}

static void spawn_effect(ptype_t *head, const float *org, const float *dir, float count, int depth)
{
	if (depth > 4)
		return;
	for (ptype_t *t = head; t; t = t->chain) {
		spawn_point(t, org, dir, count, depth);
		if (t->assoc[0]) {
			ptype_t *a = effect_by_name(t->assoc);
			if (a && a != head)
				spawn_effect(a, org, dir, count, depth + 1);
		}
	}
}

static void spawn_trail(ptype_t *head, const float *start, const float *end)
{
	for (ptype_t *t = head; t; t = t->chain) {
		float d[3], len, step, travelled = 0, side[3], up[3];
		int i = 0;
		for (int k = 0; k < 3; k++)
			d[k] = end[k] - start[k];
		len = VectorNormalize(d);
		type_material(t);
		if (t->kind == PT_BEAM) {
			particle_t *p = new_particle(t);
			if (p) {
				VectorCopy(start, p->org);
				VectorCopy(end, p->end);
			}
			continue;
		}
		if (t->kind == PT_DECAL || t->kind == PT_UDECAL)
			continue;
		step = rrange(t->step[0], t->step[1]);
		if (step <= 0)
			step = len / BW_MAX(1.0f, rrange(t->count[0], t->count[1]));
		if (step <= 0.01f)
			step = 0.01f;
		step /= BW_MAX(0.01f, r_part_density ? r_part_density->value : 1);
		spawn_light(t, end);
		{
			float upv[3] = {0, 0, 1};
			if (fabsf(d[2]) > 0.9f)
				upv[0] = 1, upv[2] = 0;
			CrossProduct(d, upv, side);
			VectorNormalize(side);
			CrossProduct(side, d, up);
		}
		while (travelled <= len && i < 4096) {
			particle_t *p = new_particle(t);
			if (!p)
				break;
			for (int k = 0; k < 3; k++)
				p->org[k] = start[k] + d[k] * travelled;
			if (t->spawnmode == SM_SPIRAL) {
				float a = travelled / BW_MAX(1.0f, t->spawnparam[0] != 0 ? t->spawnparam[0] : 32) * 2 * (float)M_PI;
				for (int k = 0; k < 3; k++)
					p->vel[k] += (side[k] * cosf(a) + up[k] * sinf(a)) * t->spawnvel[0];
			} else if (t->spawnmode == SM_TRACER) {
				float s = (i & 1) ? 1 : -1;
				for (int k = 0; k < 3; k++)
					p->vel[k] += side[k] * s * t->spawnvel[0];
			} else {
				for (int k = 0; k < 3; k++) {
					p->org[k] += crand() * (k == 2 ? t->spawnorg[1] : t->spawnorg[0]);
					p->vel[k] += crand() * (k == 2 ? t->spawnvel[1] : t->spawnvel[0]);
				}
			}
			finish_spawn(t, p, d, true);
			travelled += step;
			i++;
		}
	}
}

void R_PointParticles(int index, const float *org, const float *dir, float count)
{
	ptype_t *t = effect_by_index(index);
	float zero[3] = {0, 0, 0};
	if (!t)
		return;
	spawn_effect(t, org, dir ? dir : zero, count > 0 ? count : 1, 0);
}

void R_TrailParticles(int index, const float *start, const float *end)
{
	ptype_t *t = effect_by_index(index);
	if (t)
		spawn_trail(t, start, end);
}

const char *R_ParticleEffectName(int index)
{
	ptype_t *t = effect_by_index(index);
	return t ? t->name : "";
}

/* --- running ------------------------------------------------------------------ */

static void ramp_at(const ptype_t *t, float frac, float *out)
{
	float f = BW_MAX(0.0f, BW_MIN(0.9999f, frac)) * t->numramp;
	int i = (int)f;
	if (t->rampmode == RAMP_LERP && i + 1 < t->numramp) {
		float l = f - i;
		for (int k = 0; k < 5; k++)
			out[k] = t->ramp[i][k] + (t->ramp[i + 1][k] - t->ramp[i][k]) * l;
	} else {
		memcpy(out, t->ramp[BW_MIN(i, t->numramp - 1)], sizeof(float) * 5);
	}
}

/* effects particles emit, spawned once the update is done */
#define MAX_EMITS 256
static struct {
	ptype_t *t;
	float org[3], dir[3];
} emits[MAX_EMITS];
static int numemits;

static void update(float dt)
{
	int out = 0;
	numemits = 0;
	for (int i = 0; i < numparts; i++) {
		particle_t *p = &parts[i];
		ptype_t *t = p->t;
		float old[3];
		p->age += dt;
		if (p->age >= p->life)
			continue;
		VectorCopy(p->org, old);
		if (t->kind != PT_BEAM) {
			p->vel[2] -= t->gravity * dt;
			for (int k = 0; k < 3; k++) {
				p->vel[k] *= 1 - t->friction[k] * dt;
				p->org[k] += p->vel[k] * dt;
			}
		}
		if (t->numramp && t->rampmode != RAMP_NONE) {
			float r[5];
			ramp_at(t, p->age / p->life, r);
			if (t->rampmode == RAMP_DELTA) {
				for (int k = 0; k < 4; k++)
					p->rgba[k] -= r[k] * dt;
			} else {
				memcpy(p->rgba, r, sizeof(float) * 4);
				p->scale = r[4];
			}
		} else {
			if (t->rgbdeltatime <= 0 || p->age < t->rgbdeltatime)
				for (int k = 0; k < 3; k++)
					p->rgba[k] += t->rgbdelta[k] * dt;
			p->scale += t->scaledelta * dt;
		}
		p->rgba[3] -= t->alphadelta * dt;
		p->rot += p->rotspeed * dt;
		if (p->scale <= 0)
			continue;
		/* hitting the world: bounce, or become another effect */
		if ((t->cliptype[0] || t->clipbounce != 0.8f) && cl.world && t->kind != PT_BEAM) {
			bsptrace_t tr;
			float zero[3] = {0, 0, 0};
			BSP_TraceModel(cl.world, 0, zero, old, zero, zero, p->org, CONTENTS_SOLID, &tr);
			if (tr.fraction < 1) {
				ptype_t *ct = t->cliptype[0] ? effect_by_name(t->cliptype) : NULL;
				bool bounce = !t->cliptype[0] || !Str_ICmp(t->cliptype, "bounce") || ct == t ||
				              (ct && !Str_ICmp(ct->name, t->name));
				if (bounce) {
					float d = DotProduct(p->vel, tr.normal);
					for (int k = 0; k < 3; k++) {
						p->vel[k] = (p->vel[k] - 2 * d * tr.normal[k]) * t->clipbounce;
						p->org[k] = tr.endpos[k] + tr.normal[k] * 0.1f;
					}
				} else {
					if (ct)
						spawn_effect(ct, tr.endpos, tr.normal, t->clipcount, 1);
					continue;
				}
			}
		}
		if (t->emit[0] && p->age >= p->nextemit && numemits < MAX_EMITS) {
			p->nextemit = p->age + t->emitinterval + frand() * t->emitintervalrand;
			emits[numemits].t = t;
			VectorCopy(p->org, emits[numemits].org);
			VectorCopy(p->vel, emits[numemits].dir);
			VectorNormalize(emits[numemits].dir);
			numemits++;
		}
		if (out != i)
			parts[out] = *p;
		out++;
	}
	numparts = out;
	for (int i = 0; i < numemits; i++) {
		ptype_t *e = effect_by_name(emits[i].t->emit);
		if (e && e != emits[i].t)
			spawn_effect(e, emits[i].org, emits[i].dir, 1, 1);
	}
	for (int i = 0; i < numplights; i++) {
		plight_t *l = &plights[i];
		l->radius -= l->fade * dt;
		for (int k = 0; k < 3; k++)
			l->rgb[k] -= l->rgbfade[k] * dt;
		if (l->radius <= 0 || r_time >= l->die)
			plights[i--] = plights[--numplights];
	}
}

void R_UpdateParticles(void)
{
	float dt;
	if (lastupdate == 0 || r_time < lastupdate) {
		lastupdate = r_time;
		return;
	}
	dt = (float)(r_time - lastupdate);
	if (dt <= 0)
		return;
	lastupdate = r_time;
	if (dt > 0.1f)
		dt = 0.1f;
	update(dt);
	R_UpdateDecals();
}

void R_AddParticleLights(void)
{
	for (int i = 0; i < numplights; i++)
		R_AddDynamicLight(plights[i].org, plights[i].radius, plights[i].rgb);
}

void R_ClearParticles(void)
{
	numparts = 0;
	numplights = 0;
	/* effect numbers belong to the map's precache list */
	memset(byindex, 0, sizeof(byindex));
	memset(resolved, 0, sizeof(resolved));
}

/* --- drawing ----------------------------------------------------------------- */

static int part_cmp(const void *a, const void *b)
{
	const particle_t *x = a, *y = b;
	return x->t == y->t ? 0 : x->t < y->t ? -1 : 1;
}

#define BATCH_QUADS 4096
static rvert_t batchv[BATCH_QUADS * 4];
static uint16_t batchi[BATCH_QUADS * 6];
static int batchn;

static void flush(const ptype_t *t)
{
	int b;
	GLenum s, d;
	if (!batchn || !t)
		return;
	R_PickBlend(t->mat, t->blend, t->src, t->dst, &b, &s, &d);
	if (b == BLEND_OPAQUE)
		s = GL_SRC_ALPHA, d = GL_ONE_MINUS_SRC_ALPHA;
	GL_BindTexture(0, GL_TEXTURE_2D, t->mat->diffuse->tex);
	GL_SetBlend(BLEND_CUSTOM, s, d);
	R_StreamDraw(GL_TRIANGLES, batchv, batchn * 4, batchi, batchn * 6);
	batchn = 0;
}

static void put_quad(const ptype_t *t, const float corners[4][3], const float *tc, const float *rgba)
{
	static const float uv[4][2] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
	float c[4];
	rvert_t *v;
	if (batchn == BATCH_QUADS)
		flush(t);
	v = &batchv[batchn * 4];
	R_BlendColor(t->src, rgba, c);
	if (t->mat->fromfile && t->mat->blend != BLEND_OPAQUE)
		R_BlendColor(t->mat->src, rgba, c);
	for (int i = 0; i < 4; i++) {
		VectorCopy(corners[i], v[i].xyz);
		v[i].st[0] = tc[0] + (tc[2] - tc[0]) * uv[i][0];
		v[i].st[1] = tc[1] + (tc[3] - tc[1]) * uv[i][1];
		for (int k = 0; k < 4; k++)
			v[i].color[k] = (uint8_t)(BW_MAX(0.0f, BW_MIN(1.0f, c[k])) * 255);
	}
	for (int i = 0; i < 6; i++)
		batchi[batchn * 6 + i] = (uint16_t)(batchn * 4 + (int[]){0, 1, 2, 0, 2, 3}[i]);
	batchn++;
}

static void draw_particle(const particle_t *p)
{
	const ptype_t *t = p->t;
	float corners[4][3], size, dist, d[3], sf = BW_MAX(0.0f, BW_MIN(1.0f, t->scalefactor));
	for (int k = 0; k < 3; k++)
		d[k] = p->org[k] - r_scene.origin[k];
	dist = DotProduct(d, r_scene.forward);
	if (dist < 0 && t->kind != PT_BEAM)
		return;
	size = p->scale * 0.5f;
	/* scalefactor 0 keeps a constant size on screen, as at 256 units */
	if (sf < 1)
		size *= powf(BW_MAX(dist, 1.0f) / 256, 1 - sf);
	if (t->kind == PT_NORMAL) {
		float a = p->rot * (float)M_PI / 180, cs = cosf(a) * size, sn = sinf(a) * size, r[3], u[3];
		for (int k = 0; k < 3; k++) {
			r[k] = r_scene.right[k] * cs + r_scene.up[k] * sn;
			u[k] = r_scene.up[k] * cs - r_scene.right[k] * sn;
		}
		for (int k = 0; k < 3; k++) {
			corners[0][k] = p->org[k] - r[k] - u[k];
			corners[1][k] = p->org[k] - r[k] + u[k];
			corners[2][k] = p->org[k] + r[k] + u[k];
			corners[3][k] = p->org[k] + r[k] - u[k];
		}
		put_quad(t, corners, p->tc, p->rgba);
		return;
	}
	{
		/* lines: sparks along their motion, beams from start to end */
		float a[3], b[3], dir[3], side[3], tov[3], tc[4];
		memcpy(tc, p->tc, sizeof(tc));
		if (t->kind == PT_BEAM) {
			float len;
			VectorCopy(p->org, a);
			VectorCopy(p->end, b);
			for (int k = 0; k < 3; k++)
				dir[k] = b[k] - a[k];
			len = VectorLength(dir);
			if (t->beamtexstep > 0) {
				float scroll = t->beamtexspeed * (float)r_time;
				tc[0] = scroll;
				tc[2] = scroll + len / t->beamtexstep;
			}
		} else {
			float speed = VectorLength(p->vel), len;
			if (speed < 1e-3f)
				return;
			len = t->stretch < 0 ? -t->stretch : speed * t->stretch * 0.05f;
			for (int k = 0; k < 3; k++) {
				b[k] = p->org[k];
				a[k] = p->org[k] - p->vel[k] / speed * len;
			}
			if (t->kind == PT_SPARK)
				size = BW_MAX(0.5f, size * 0.5f);
		}
		for (int k = 0; k < 3; k++) {
			dir[k] = b[k] - a[k];
			tov[k] = a[k] - r_scene.origin[k];
		}
		CrossProduct(dir, tov, side);
		if (VectorNormalize(side) < 1e-6f)
			return;
		for (int k = 0; k < 3; k++) {
			corners[0][k] = a[k] - side[k] * size;
			corners[1][k] = a[k] + side[k] * size;
			corners[2][k] = b[k] + side[k] * size;
			corners[3][k] = b[k] - side[k] * size;
		}
		put_quad(t, corners, tc, p->rgba);
	}
}

void R_DrawParticles(void)
{
	const glprog_t *pr = &r_progs[PROG_UNLIT];
	const ptype_t *cur = NULL;
	if (!numparts || !r_drawparticles->ivalue)
		return;
	qsort(parts, numparts, sizeof(particle_t), part_cmp);
	R_UseProgram(PROG_UNLIT);
	qglUniformMatrix4fv(pr->u_mvp, 1, GL_FALSE, r_scene.viewproj);
	qglUniform4f(pr->u_color, 1, 1, 1, 1);
	qglUniform4f(pr->u_tcmod, 0, 0, 1, 1);
	qglUniform1f(pr->u_alphatest, 0);
	GL_SetDepth(true, false);
	GL_SetCull(false);
	batchn = 0;
	for (int i = 0; i < numparts; i++) {
		const particle_t *p = &parts[i];
		if (p->t != cur) {
			flush(cur);
			cur = p->t;
		}
		if (p->rgba[3] <= 0)
			continue;
		draw_particle(p);
	}
	flush(cur);
	GL_SetCull(true);
	GL_SetDepth(true, true);
	r_stats.particles = numparts;
}

/* --- commands ------------------------------------------------------------------ */

/* forgets every script so they are read again when next named */
static void free_scripts(void)
{
	while (types) {
		ptype_t *n = types->next;
		Mem_Free(types);
		types = n;
	}
	while (files) {
		pfile_t *n = files->next;
		Mem_Free(files);
		files = n;
	}
	memset(byindex, 0, sizeof(byindex));
	memset(resolved, 0, sizeof(resolved));
	numparts = 0;
}

static void reload_f(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	free_scripts();
	Con_Printf("particle scripts will be read again\n");
}

static void effect_f(int argc, const char **argv)
{
	/* r_effect name [distance]: spawns an effect in front of the view, or on
	 * the wall it looks at, for testing */
	ptype_t *t;
	float org[3], dir[3];
	if (argc < 2) {
		Con_Printf("usage: r_effect <file.effect> [distance]\n");
		return;
	}
	t = effect_by_name(argv[1]);
	if (!t) {
		Con_Printf("no effect %s\n", argv[1]);
		return;
	}
	for (int k = 0; k < 3; k++) {
		org[k] = r_scene.mainorigin[k] + r_scene.mainforward[k] * (argc > 2 ? (float)atof(argv[2]) : 4096);
		dir[k] = -r_scene.mainforward[k];
	}
	if (argc <= 2 && cl.world) {
		bsptrace_t tr;
		float zero[3] = {0, 0, 0};
		BSP_TraceModel(cl.world, 0, zero, r_scene.mainorigin, zero, zero, org, CONTENTS_SOLID, &tr);
		VectorCopy(tr.endpos, org);
		if (tr.fraction < 1)
			VectorCopy(tr.normal, dir);
	}
	spawn_effect(t, org, dir, 1, 0);
}

static void list_f(int argc, const char **argv)
{
	int n = 0;
	(void)argc;
	(void)argv;
	for (ptype_t *t = types; t; t = t->next)
		if (t->head) {
			Con_Printf("%s\n", t->name);
			n++;
		}
	Con_Printf("%d effects, %d particles alive\n", n, numparts);
}

void R_InitParticles(void)
{
	r_drawparticles = Cvar_Get("r_drawparticles", "1", CVAR_ARCHIVE, "Draw particle effects");
	r_particles_max = Cvar_Get("r_particles_max", "16384", CVAR_ARCHIVE, "Most particles alive at once");
	r_part_density = Cvar_Get("r_part_density", "1", CVAR_ARCHIVE, "Scales how many particles effects spawn");
	Cmd_Add("r_partreload", reload_f);
	Cmd_Add("r_effect", effect_f);
	Cmd_Add("r_partlist", list_f);
}

void R_ShutdownParticles(void)
{
	free_scripts();
	dotmat = NULL;
}
