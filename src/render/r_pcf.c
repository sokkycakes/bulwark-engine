/* Bulwark engine - Source particle systems from .pcf files: Valve's binary
 * DMX files of particle system definitions, each a list of emitters,
 * initializers, operators and renderers, with child systems. An effect
 * named "file.system" is system "system" in particles/file.pcf; a bare
 * name is looked up in the files read so far and the ones
 * particles/particles_manifest.txt names. Only the functions the shipped
 * content needs are run; the rest are skipped and listed by r_pcfinfo.
 * How each one was read is in docs/spec/particles.md ("Source particle
 * systems"). ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/cmd.h"
#include "../common/fs.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* --- DMX binary ----------------------------------------------------------------- */

enum {
	AT_ELEMENT = 1, AT_INT, AT_FLOAT, AT_BOOL, AT_STRING, AT_VOID, AT_OBJECTID, AT_COLOR,
	AT_VECTOR2, AT_VECTOR3, AT_VECTOR4, AT_QANGLE, AT_QUATERNION, AT_VMATRIX, AT_ARRAY_BASE = 14
};

typedef struct {
	const char *name;
	int type;  /* AT_, without the array offset */
	int count; /* values; 1 unless an array */
	bool array;
	float *num;    /* numeric values, comps() floats each */
	int *ref;      /* element indices, -1 for none */
	char **str;    /* strings */
} dmattr_t;

typedef struct {
	const char *type, *name;
	int numattrs;
	dmattr_t *attrs;
} dmelem_t;

typedef struct {
	char **strings;
	int numstrings;
	dmelem_t *elems;
	int numelems;
} dmx_t;

typedef struct {
	const uint8_t *d;
	size_t len, p;
	bool bad;
} rd_t;

static int rd_i32(rd_t *r)
{
	int v;
	if (r->p + 4 > r->len) {
		r->bad = true;
		return 0;
	}
	v = (int)((uint32_t)r->d[r->p] | (uint32_t)r->d[r->p + 1] << 8 | (uint32_t)r->d[r->p + 2] << 16 | (uint32_t)r->d[r->p + 3] << 24);
	r->p += 4;
	return v;
}

static int rd_i16(rd_t *r)
{
	int v;
	if (r->p + 2 > r->len) {
		r->bad = true;
		return 0;
	}
	v = (int16_t)(r->d[r->p] | r->d[r->p + 1] << 8);
	r->p += 2;
	return v;
}

static float rd_f32(rd_t *r)
{
	int i = rd_i32(r);
	float f;
	memcpy(&f, &i, 4);
	return f;
}

static char *rd_str(rd_t *r)
{
	size_t s = r->p;
	char *out;
	while (r->p < r->len && r->d[r->p])
		r->p++;
	if (r->p >= r->len) {
		r->bad = true;
		return Mem_Strdup("");
	}
	out = Mem_Alloc(r->p - s + 1);
	memcpy(out, r->d + s, r->p - s);
	r->p++;
	return out;
}

static int comps(int type)
{
	switch (type) {
	case AT_COLOR:
	case AT_VECTOR4:
	case AT_QUATERNION:
		return 4;
	case AT_VECTOR2:
		return 2;
	case AT_VECTOR3:
	case AT_QANGLE:
		return 3;
	case AT_VMATRIX:
		return 16;
	default:
		return 1;
	}
}

static void dmx_free(dmx_t *x)
{
	for (int i = 0; i < x->numelems; i++) {
		for (int a = 0; a < x->elems[i].numattrs; a++) {
			dmattr_t *at = &x->elems[i].attrs[a];
			if (at->str)
				for (int k = 0; k < at->count; k++)
					Mem_Free(at->str[k]);
			Mem_Free(at->str);
			Mem_Free(at->num);
			Mem_Free(at->ref);
		}
		Mem_Free(x->elems[i].attrs);
		Mem_Free((char *)x->elems[i].name);
	}
	for (int i = 0; i < x->numstrings; i++)
		Mem_Free(x->strings[i]);
	Mem_Free(x->strings);
	Mem_Free(x->elems);
	memset(x, 0, sizeof(*x));
}

static void read_value(rd_t *r, dmattr_t *a, int k)
{
	int c = comps(a->type);
	switch (a->type) {
	case AT_ELEMENT: {
		int i = rd_i32(r);
		if (i == -2) { /* an element in another file, by id: not followed */
			Mem_Free(rd_str(r));
			i = -1;
		}
		a->ref[k] = i;
		break;
	}
	case AT_STRING:
		a->str[k] = rd_str(r);
		break;
	case AT_VOID: {
		int n = rd_i32(r);
		if (n < 0 || r->p + (size_t)n > r->len)
			r->bad = true;
		else
			r->p += (size_t)n;
		break;
	}
	case AT_OBJECTID:
		r->p += 16;
		break;
	case AT_INT:
		a->num[k] = (float)rd_i32(r);
		break;
	case AT_BOOL:
		if (r->p >= r->len)
			r->bad = true;
		else
			a->num[k] = r->d[r->p++] ? 1.0f : 0.0f;
		break;
	case AT_COLOR:
		for (int j = 0; j < 4; j++) {
			if (r->p >= r->len) {
				r->bad = true;
				break;
			}
			a->num[k * 4 + j] = r->d[r->p++];
		}
		break;
	default:
		for (int j = 0; j < c; j++)
			a->num[k * c + j] = rd_f32(r);
		break;
	}
}

/* "<!-- dmx encoding binary 2 format pcf 1 -->": only binary version 2,
 * the one TF2-era particle files use, is read. */
static bool dmx_parse(const uint8_t *d, size_t len, dmx_t *x, const char *path)
{
	rd_t r = {d, len, 0, false};
	int ver = 0;
	const char *h;
	memset(x, 0, sizeof(*x));
	while (r.p < len && d[r.p] != '\n')
		r.p++;
	h = strstr((const char *)d, "encoding binary ");
	if (!h || (const uint8_t *)h > d + r.p || (ver = atoi(h + 16)) != 2) {
		Con_Printf("%s: not a binary 2 DMX file (version %d)\n", path, ver);
		return false;
	}
	r.p += 2; /* the newline and the header's terminator */
	x->numstrings = rd_i16(&r);
	if (x->numstrings < 0)
		return false;
	x->strings = Mem_Alloc(sizeof(char *) * (size_t)BW_MAX(1, x->numstrings));
	for (int i = 0; i < x->numstrings; i++)
		x->strings[i] = rd_str(&r);
	x->numelems = rd_i32(&r);
	if (r.bad || x->numelems < 0 || x->numelems > 1 << 20)
		goto bad;
	x->elems = Mem_Alloc(sizeof(dmelem_t) * (size_t)BW_MAX(1, x->numelems));
	for (int i = 0; i < x->numelems; i++) {
		int t = rd_i16(&r);
		x->elems[i].type = t >= 0 && t < x->numstrings ? x->strings[t] : "";
		x->elems[i].name = rd_str(&r);
		r.p += 16; /* the element's id */
	}
	for (int i = 0; i < x->numelems && !r.bad; i++) {
		dmelem_t *e = &x->elems[i];
		e->numattrs = rd_i32(&r);
		if (e->numattrs < 0 || e->numattrs > 4096)
			goto bad;
		e->attrs = Mem_Alloc(sizeof(dmattr_t) * (size_t)BW_MAX(1, e->numattrs));
		for (int a = 0; a < e->numattrs && !r.bad; a++) {
			dmattr_t *at = &e->attrs[a];
			int n = rd_i16(&r), t;
			at->name = n >= 0 && n < x->numstrings ? x->strings[n] : "";
			if (r.p >= len)
				goto bad;
			t = d[r.p++];
			at->array = t > AT_ARRAY_BASE;
			at->type = at->array ? t - AT_ARRAY_BASE : t;
			if (at->type < AT_ELEMENT || at->type > AT_VMATRIX)
				goto bad;
			at->count = at->array ? rd_i32(&r) : 1;
			if (at->count < 0 || at->count > 1 << 20)
				goto bad;
			if (at->type == AT_ELEMENT)
				at->ref = Mem_Alloc(sizeof(int) * (size_t)BW_MAX(1, at->count));
			else if (at->type == AT_STRING)
				at->str = Mem_Alloc(sizeof(char *) * (size_t)BW_MAX(1, at->count));
			else
				at->num = Mem_Alloc(sizeof(float) * (size_t)BW_MAX(1, at->count * comps(at->type)));
			for (int k = 0; k < at->count && !r.bad; k++)
				read_value(&r, at, k);
		}
	}
	if (r.bad)
		goto bad;
	return true;
bad:
	Con_Printf("%s: damaged DMX file\n", path);
	dmx_free(x);
	return false;
}

static const dmattr_t *attr(const dmelem_t *e, const char *name)
{
	for (int i = 0; i < e->numattrs; i++)
		if (!Str_ICmp(e->attrs[i].name, name))
			return &e->attrs[i];
	return NULL;
}

static float attr_f(const dmelem_t *e, const char *name, float def)
{
	const dmattr_t *a = attr(e, name);
	return a && a->num && a->count > 0 ? a->num[0] : def;
}

static void attr_v(const dmelem_t *e, const char *name, float *out, int n)
{
	const dmattr_t *a = attr(e, name);
	if (a && a->num && a->count > 0)
		for (int k = 0; k < n && k < comps(a->type); k++)
			out[k] = a->num[k];
}

static const char *attr_s(const dmelem_t *e, const char *name)
{
	const dmattr_t *a = attr(e, name);
	return a && a->str && a->count > 0 ? a->str[0] : "";
}

/* --- definitions ------------------------------------------------------------------ */

enum {
	/* emitters */
	F_EMIT_INSTANT,
	/* initializers */
	F_LIFETIME, F_POS_SPHERE, F_ROTATION, F_YAWFLIP, F_ROTSPEED, F_SEQUENCE, F_COLOR, F_ALPHA, F_RADIUS, F_TRAILLEN,
	/* operators */
	F_ALPHA_FADE_DECAY, F_ALPHA_FADE_OUT_RANDOM, F_COLOR_FADE, F_LIFESPAN_DECAY, F_MOVE_BASIC, F_OSCILLATE,
	F_RADIUS_SCALE, F_ROT_BASIC, F_SPIN_ROLL, F_RANDOM_FORCE, F_COLLIDE,
	/* renderers */
	F_R_SPRITES, F_R_TRAIL, F_R_VELROT,
	F_COUNT
};

/* the stored function names, and the attributes read into each one's a[] */
static const struct {
	const char *name;
	const char *args[18];
} funcs[F_COUNT] = {
	[F_EMIT_INSTANT] = {"emit_instantaneously", {"num_to_emit", "emission_start_time"}},
	[F_LIFETIME] = {"Lifetime Random", {"lifetime_min", "lifetime_max", "lifetime_random_exponent"}},
	[F_POS_SPHERE] = {"Position Within Sphere Random",
	                  {"distance_min", "distance_max", "speed_min", "speed_max", "speed_random_exponent", "control_point_number"}},
	[F_ROTATION] = {"Rotation Random", {"rotation_initial", "rotation_offset_min", "rotation_offset_max", "rotation_random_exponent"}},
	[F_YAWFLIP] = {"Rotation Yaw Flip Random", {"Flip Percentage"}},
	[F_ROTSPEED] = {"Rotation Speed Random",
	                {"rotation_speed_constant", "rotation_speed_random_min", "rotation_speed_random_max",
	                 "rotation_speed_random_exponent", "randomly_flip_direction"}},
	[F_SEQUENCE] = {"Sequence Random", {"sequence_min", "sequence_max"}},
	[F_COLOR] = {"Color Random", {NULL}},
	[F_ALPHA] = {"Alpha Random", {"alpha_min", "alpha_max", "alpha_random_exponent"}},
	[F_RADIUS] = {"Radius Random", {"radius_min", "radius_max", "radius_random_exponent"}},
	[F_TRAILLEN] = {"Trail Length Random", {"length_min", "length_max", "length_random_exponent"}},
	[F_ALPHA_FADE_DECAY] = {"Alpha Fade and Decay",
	                        {"start_alpha", "end_alpha", "start_fade_in_time", "end_fade_in_time", "start_fade_out_time",
	                         "end_fade_out_time"}},
	[F_ALPHA_FADE_OUT_RANDOM] = {"Alpha Fade Out Random",
	                             {"fade out time min", "fade out time max", "fade out time exponent", "proportional 0/1",
	                              "ease in and out", "fade bias"}},
	[F_COLOR_FADE] = {"Color Fade", {"fade_start_time", "fade_end_time", "ease_in_and_out"}},
	[F_LIFESPAN_DECAY] = {"Lifespan Decay", {NULL}},
	[F_MOVE_BASIC] = {"Movement Basic", {"drag"}},
	[F_OSCILLATE] = {"Oscillate Scalar",
	                 {"oscillation field", "oscillation rate min", "oscillation rate max", "oscillation frequency min",
	                  "oscillation frequency max", "proportional 0/1", "start time min", "start time max", "end time min",
	                  "end time max", "oscillation multiplier", "oscillation start phase"}},
	[F_RADIUS_SCALE] = {"Radius Scale", {"start_time", "end_time", "radius_start_scale", "radius_end_scale", "scale_bias", "ease_in_and_out"}},
	[F_ROT_BASIC] = {"Rotation Basic", {NULL}},
	[F_SPIN_ROLL] = {"Rotation Spin Roll", {"spin_rate_degrees", "spin_stop_time", "spin_rate_min"}},
	[F_RANDOM_FORCE] = {"random force", {NULL}},
	[F_COLLIDE] = {"Collision via traces", {"amount of bounce", "amount of slide", "kill particle on collision", "brush only"}},
	[F_R_SPRITES] = {"render_animated_sprites",
	                 {"animation rate", "animation_fit_lifetime", "use animation rate as FPS", "orientation_type"}},
	[F_R_TRAIL] = {"render_sprite_trail", {"min length", "max length", "length fade in time", "animation rate"}},
	[F_R_VELROT] = {"render_screen_velocity_rotate", {"rotate_rate(dps)", "forward_angle"}},
};

typedef struct {
	int fn;
	float a[18];
	float v[4][4]; /* vector attributes, per function */
} pfunc_t;

typedef struct {
	int nseq;
	struct pseq_s {
		int num, nframes;
		bool clamp;
		float total;
		float *dur, (*uv)[4];
	} *seq;
} psheet_t;

typedef struct pdef_s {
	char name[96];
	struct pfile_s *file;
	char matname[128];
	material_t *mat;
	psheet_t *sheet;
	bool matdone, additive;
	float fadesize[2], overbright; /* SpriteCard $startfadesize/$endfadesize, $overbrightfactor */
	int maxparts, initial;
	float radius, color[4], rotation, rotspeed;
	int sequence;
	pfunc_t *funcs;
	int numfuncs;
	struct {
		char name[96];
		float delay;
		struct pdef_s *def;
	} *children;
	int numchildren;
	char skipped[256]; /* functions not run */
} pdef_t;

typedef struct pfile_s {
	char name[64];
	pdef_t *defs;
	int numdefs;
	struct pfile_s *next;
} pfile_t;

static pfile_t *pfiles;
static bool manifest_read;

static int func_index(const char *name)
{
	for (int i = 0; i < F_COUNT; i++)
		if (funcs[i].name && !Str_ICmp(funcs[i].name, name))
			return i;
	return -1;
}

static void read_func(pdef_t *d, const dmelem_t *e)
{
	const char *fname = attr_s(e, "functionName");
	int fn = func_index(fname);
	pfunc_t *f;
	if (fn < 0) {
		if (!strstr(d->skipped, fname) && strlen(d->skipped) + strlen(fname) + 3 < sizeof(d->skipped)) {
			if (d->skipped[0])
				Str_Cat(d->skipped, ", ", sizeof(d->skipped));
			Str_Cat(d->skipped, fname, sizeof(d->skipped));
		}
		return;
	}
	d->funcs = Mem_Realloc(d->funcs, sizeof(pfunc_t) * (size_t)(d->numfuncs + 1));
	f = &d->funcs[d->numfuncs++];
	memset(f, 0, sizeof(*f));
	f->fn = fn;
	/* defaults where a missing attribute isn't zero */
	f->a[2] = fn == F_LIFETIME || fn == F_ALPHA || fn == F_RADIUS || fn == F_TRAILLEN ? 1 : 0;
	for (int i = 0; funcs[fn].args[i]; i++)
		f->a[i] = attr_f(e, funcs[fn].args[i], f->a[i]);
	switch (fn) {
	case F_ALPHA:
		if (!attr(e, "alpha_min"))
			f->a[0] = 255;
		if (!attr(e, "alpha_max"))
			f->a[1] = 255;
		break;
	case F_ROTSPEED:
		break;
	case F_POS_SPHERE:
		f->v[0][0] = f->v[0][1] = f->v[0][2] = 1;
		attr_v(e, "distance_bias", f->v[0], 3);
		attr_v(e, "distance_bias_absolute_value", f->v[1], 3);
		attr_v(e, "speed_in_local_coordinate_system_min", f->v[2], 3);
		attr_v(e, "speed_in_local_coordinate_system_max", f->v[3], 3);
		break;
	case F_COLOR:
		f->v[0][0] = f->v[0][1] = f->v[0][2] = f->v[0][3] = 255;
		memcpy(f->v[1], f->v[0], sizeof(f->v[1]));
		attr_v(e, "color1", f->v[0], 4);
		attr_v(e, "color2", f->v[1], 4);
		break;
	case F_COLOR_FADE:
		f->v[0][0] = f->v[0][1] = f->v[0][2] = f->v[0][3] = 255;
		attr_v(e, "color_fade", f->v[0], 4);
		if (!attr(e, "fade_end_time"))
			f->a[1] = 1;
		break;
	case F_MOVE_BASIC:
		attr_v(e, "gravity", f->v[0], 3);
		break;
	case F_RANDOM_FORCE:
		attr_v(e, "min force", f->v[0], 3);
		attr_v(e, "max force", f->v[1], 3);
		break;
	case F_RADIUS_SCALE:
		if (!attr(e, "end_time"))
			f->a[1] = 1;
		if (!attr(e, "radius_start_scale"))
			f->a[2] = 1;
		if (!attr(e, "radius_end_scale"))
			f->a[3] = 1;
		if (!attr(e, "scale_bias"))
			f->a[4] = 0.5f;
		break;
	case F_ALPHA_FADE_DECAY:
		if (!attr(e, "start_alpha"))
			f->a[0] = 1;
		if (!attr(e, "start_fade_out_time"))
			f->a[4] = 0.5f;
		if (!attr(e, "end_fade_out_time"))
			f->a[5] = 1;
		break;
	case F_R_SPRITES:
	case F_R_TRAIL:
		if (!attr(e, "animation rate"))
			f->a[fn == F_R_SPRITES ? 0 : 3] = 0.1f;
		if (fn == F_R_TRAIL && !attr(e, "max length"))
			f->a[1] = 2000;
		break;
	}
}

static void read_def(pfile_t *pf, pdef_t *d, const dmx_t *x, const dmelem_t *e)
{
	static const char *const lists[] = {"emitters", "initializers", "operators", "renderers", "forces", "constraints"};
	const dmattr_t *a;
	Str_Copy(d->name, e->name, sizeof(d->name));
	d->file = pf;
	Str_Copy(d->matname, attr_s(e, "material"), sizeof(d->matname));
	d->maxparts = (int)attr_f(e, "max_particles", 1000);
	d->initial = (int)attr_f(e, "initial_particles", 0);
	d->radius = attr_f(e, "radius", 5);
	d->color[0] = d->color[1] = d->color[2] = d->color[3] = 255;
	attr_v(e, "color", d->color, 4);
	d->rotation = attr_f(e, "rotation", 0);
	d->rotspeed = attr_f(e, "rotation_speed", 0);
	d->sequence = (int)attr_f(e, "sequence_number", 0);
	for (size_t l = 0; l < BW_ARRAY_COUNT(lists); l++)
		if ((a = attr(e, lists[l])) != NULL && a->ref)
			for (int k = 0; k < a->count; k++)
				if (a->ref[k] >= 0 && a->ref[k] < x->numelems)
					read_func(d, &x->elems[a->ref[k]]);
	if ((a = attr(e, "children")) != NULL && a->ref) {
		d->children = Mem_Alloc(sizeof(*d->children) * (size_t)BW_MAX(1, a->count));
		for (int k = 0; k < a->count; k++) {
			const dmelem_t *c;
			const dmattr_t *ca;
			if (a->ref[k] < 0 || a->ref[k] >= x->numelems)
				continue;
			c = &x->elems[a->ref[k]];
			if (!(ca = attr(c, "child")) || !ca->ref || ca->ref[0] < 0 || ca->ref[0] >= x->numelems)
				continue;
			Str_Copy(d->children[d->numchildren].name, x->elems[ca->ref[0]].name, sizeof(d->children[0].name));
			d->children[d->numchildren].delay = attr_f(c, "delay", 0);
			d->numchildren++;
		}
	}
}

static pfile_t *load_pcf(const char *file)
{
	char path[160];
	uint8_t *data;
	size_t len;
	dmx_t x;
	pfile_t *pf;
	int n = 0;
	for (pf = pfiles; pf; pf = pf->next)
		if (!Str_ICmp(pf->name, file))
			return pf->numdefs ? pf : NULL;
	pf = Mem_Alloc(sizeof(*pf));
	Str_Copy(pf->name, file, sizeof(pf->name));
	pf->next = pfiles;
	pfiles = pf;
	snprintf(path, sizeof(path), "particles/%s.pcf", file);
	if (!(data = (uint8_t *)FS_LoadFile(path, &len)))
		return NULL;
	if (!dmx_parse(data, len, &x, path)) {
		Mem_Free(data);
		return NULL;
	}
	Mem_Free(data);
	for (int i = 0; i < x.numelems; i++)
		n += !Str_ICmp(x.elems[i].type, "DmeParticleSystemDefinition");
	pf->defs = Mem_Alloc(sizeof(pdef_t) * (size_t)BW_MAX(1, n));
	for (int i = 0; i < x.numelems; i++)
		if (!Str_ICmp(x.elems[i].type, "DmeParticleSystemDefinition"))
			read_def(pf, &pf->defs[pf->numdefs++], &x, &x.elems[i]);
	dmx_free(&x);
	Con_DPrintf("%s: %d particle systems\n", path, pf->numdefs);
	return pf;
}

/* particles/particles_manifest.txt: "file" "particles/name.pcf" lines,
 * a leading ! marks a file to load first */
static void read_manifest(void)
{
	char *text, *s;
	size_t len;
	if (manifest_read)
		return;
	manifest_read = true;
	if (!(text = FS_LoadFile("particles/particles_manifest.txt", &len)))
		return;
	for (s = text; (s = strstr(s, "\"file\"")) != NULL;) {
		char name[128], *q, *e;
		s += 6;
		if (!(q = strchr(s, '"')) || !(e = strchr(q + 1, '"')))
			break;
		snprintf(name, sizeof(name), "%.*s", (int)(e - q - 1), q + 1);
		s = e + 1;
		{
			char *b = name[0] == '!' ? name + 1 : name, *slash = strrchr(b, '/'), *dot;
			if (slash)
				b = slash + 1;
			if ((dot = strrchr(b, '.')) != NULL)
				*dot = 0;
			load_pcf(b);
		}
	}
	Mem_Free(text);
}

static pdef_t *find_in(pfile_t *pf, const char *name)
{
	for (int i = 0; pf && i < pf->numdefs; i++)
		if (!Str_ICmp(pf->defs[i].name, name))
			return &pf->defs[i];
	return NULL;
}

static pdef_t *find_def(const char *name)
{
	const char *dot = strchr(name, '.');
	pdef_t *d;
	if (dot && dot > name) {
		char file[64];
		pfile_t *pf;
		snprintf(file, sizeof(file), "%.*s", (int)BW_MIN((size_t)(dot - name), sizeof(file) - 1), name);
		if ((pf = load_pcf(file)) != NULL && (d = find_in(pf, dot + 1)) != NULL)
			return d;
	}
	read_manifest();
	for (pfile_t *pf = pfiles; pf; pf = pf->next)
		if ((d = find_in(pf, name)) != NULL || (dot && (d = find_in(pf, dot + 1)) != NULL))
			return d;
	return NULL;
}

/* --- materials and sprite sheets ------------------------------------------------- */

/* the value of a key in a VMT's text, or NULL */
static bool vmt_number(const char *text, const char *key, float *out)
{
	size_t kl = strlen(key);
	for (const char *s = text; (s = strchr(s, '$')) != NULL; s++) {
		const char *e = s + kl;
		if (Str_NICmp(s, key, kl) || (*e != '"' && *e != ' ' && *e != '\t'))
			continue;
		while (*e == '"' || *e == ' ' || *e == '\t')
			e++;
		if ((*e >= '0' && *e <= '9') || *e == '.' || *e == '-') {
			*out = (float)atof(e);
			return true;
		}
	}
	return false;
}

/* a VTF's sprite sheet resource (tag 0x10): sequences of frames, each with
 * a duration and four sets of texture coordinates (the first is used) */
static psheet_t *load_sheet(const char *vtfpath)
{
	size_t len;
	uint8_t *d = (uint8_t *)FS_LoadFile(vtfpath, &len);
	psheet_t *sh = NULL;
	rd_t r;
	uint32_t ver, nres;
	if (!d)
		return NULL;
	if (len < 80 || memcmp(d, "VTF", 4))
		goto done;
	ver = d[8] | (uint32_t)d[9] << 8;
	if (ver < 3)
		goto done;
	nres = d[68] | (uint32_t)d[69] << 8 | (uint32_t)d[70] << 16 | (uint32_t)d[71] << 24;
	for (uint32_t i = 0; i < nres && 80 + i * 8 + 8 <= len; i++) {
		const uint8_t *e = d + 80 + i * 8;
		uint32_t off = e[4] | (uint32_t)e[5] << 8 | (uint32_t)e[6] << 16 | (uint32_t)e[7] << 24;
		int sver, nseq;
		if (e[0] != 0x10 || e[1] || e[2] || off + 4 > len)
			continue;
		r = (rd_t){d, len, off + 4, false}; /* past the resource's size */
		sver = rd_i32(&r);
		nseq = rd_i32(&r);
		if (sver < 0 || sver > 1 || nseq <= 0 || nseq > 1024)
			break;
		sh = Mem_Alloc(sizeof(*sh));
		sh->seq = Mem_Alloc(sizeof(*sh->seq) * (size_t)nseq);
		sh->nseq = nseq;
		for (int s = 0; s < nseq && !r.bad; s++) {
			struct pseq_s *q = &sh->seq[s];
			int sets = sver == 0 ? 1 : 4;
			q->num = rd_i32(&r);
			q->clamp = rd_i32(&r) != 0;
			q->nframes = rd_i32(&r);
			q->total = rd_f32(&r);
			if (q->nframes <= 0 || q->nframes > 4096) {
				r.bad = true;
				break;
			}
			q->dur = Mem_Alloc(sizeof(float) * (size_t)q->nframes);
			q->uv = Mem_Alloc(sizeof(float[4]) * (size_t)q->nframes);
			for (int f = 0; f < q->nframes; f++) {
				q->dur[f] = rd_f32(&r);
				for (int k = 0; k < sets * 4; k++) {
					float v = rd_f32(&r);
					if (k < 4)
						q->uv[f][k] = v;
				}
			}
			if (q->total <= 0)
				for (int f = 0; f < q->nframes; f++)
					q->total += q->dur[f];
		}
		if (r.bad) {
			Con_Printf("%s: damaged sprite sheet\n", vtfpath);
			for (int s = 0; s < sh->nseq; s++) {
				Mem_Free(sh->seq[s].dur);
				Mem_Free(sh->seq[s].uv);
			}
			Mem_Free(sh->seq);
			Mem_Free(sh);
			sh = NULL;
		}
		break;
	}
done:
	Mem_Free(d);
	return sh;
}

static material_t *dotmat;

static void def_material(pdef_t *d)
{
	char path[192];
	char *text;
	size_t len;
	if (d->matdone)
		return;
	d->matdone = true;
	d->overbright = 1;
	if (!dotmat) {
		uint8_t px[32 * 32 * 4];
		for (int y = 0; y < 32; y++)
			for (int x = 0; x < 32; x++) {
				float dx = (x + 0.5f) / 16 - 1, dy = (y + 0.5f) / 16 - 1, dd = sqrtf(dx * dx + dy * dy);
				uint8_t *p = px + (y * 32 + x) * 4;
				p[0] = p[1] = p[2] = 255;
				p[3] = (uint8_t)(BW_MAX(0.0f, BW_MIN(1.0f, 1 - dd)) * BW_MAX(0.0f, BW_MIN(1.0f, 1 - dd)) * 255);
			}
		dotmat = R_MaterialForImage("$pcfdot", R_ImageFromPixels("$pcfdot", px, 32, 32, 0), 0);
	}
	snprintf(path, sizeof(path), "materials/%s", d->matname);
	for (char *c = path; *c; c++)
		*c = *c == '\\' ? '/' : (char)tolower((unsigned char)*c);
	if ((text = FS_LoadFile(path, &len)) != NULL) {
		vmt_number(text, "$startfadesize", &d->fadesize[0]);
		vmt_number(text, "$endfadesize", &d->fadesize[1]);
		vmt_number(text, "$overbrightfactor", &d->overbright);
		Mem_Free(text);
	}
	d->mat = R_FindMaterial(path, 0);
	if (!d->mat->diffuse || d->mat->diffuse->missing || d->mat->diffuse == r_checkerimage) {
		/* glows and the like that aren't installed: a soft dot */
		d->mat = dotmat;
		d->additive = true;
		return;
	}
	d->additive = d->mat->blend == BLEND_ADD;
	snprintf(path, sizeof(path), "%s.vtf", d->mat->diffuse->name);
	if (FS_FileExists(path))
		d->sheet = load_sheet(path);
}

/* --- running ------------------------------------------------------------------- */

typedef struct {
	float org[3], vel[3];
	float age, life;
	float radius, radius0, rgb[3], rgb0[3], alpha, alpha0;
	float rot, rotspeed, traillen, osc;
	float rnd[4]; /* per-particle random numbers for operators */
	int seq;
	bool flip;
} pp_t;

typedef struct {
	pdef_t *def;
	float org[3], axis[3][3]; /* control point 0: forward, left, up */
	float time;               /* below zero while a child waits for its delay */
	pp_t *p;
	int n;
	uint32_t fired; /* instant emitters that have fired */
	bool started, emitted;
} psys_t;

#define MAX_SYSTEMS 512
static psys_t systems[MAX_SYSTEMS];
static int numsystems;
static cvar_t *r_drawpcf;

static uint32_t rng = 0x2545f491u;
static float frand(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (rng >> 8) * (1.0f / 16777216.0f);
}
static float rexp(float a, float b, float e) { return a + (b - a) * (e == 1 || e <= 0 ? frand() : powf(frand(), e)); }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float spline(float t) { return t * t * (3 - 2 * t); }
/* t raised to log(bias)/log(0.5): bias 0.5 leaves it straight */
static float bias(float t, float b) { return b <= 0 || b >= 1 || b == 0.5f ? t : powf(t, logf(b) / logf(0.5f)); }

static void start_system(pdef_t *d, const float *org, const float axis[3][3], float delay, int depth)
{
	psys_t *s;
	if (!d || depth > 8 || numsystems >= MAX_SYSTEMS)
		return;
	def_material(d);
	s = &systems[numsystems++];
	memset(s, 0, sizeof(*s));
	s->def = d;
	VectorCopy(org, s->org);
	memcpy(s->axis, axis, sizeof(s->axis));
	s->time = -delay;
	s->p = Mem_Alloc(sizeof(pp_t) * (size_t)BW_MAX(1, BW_MIN(d->maxparts, 4096)));
	for (int i = 0; i < d->numchildren; i++) {
		if (!d->children[i].def)
			d->children[i].def = find_in(d->file, d->children[i].name);
		if (!d->children[i].def)
			d->children[i].def = find_def(d->children[i].name);
		start_system(d->children[i].def, org, axis, delay + d->children[i].delay, depth + 1);
	}
}

static void init_particle(psys_t *s, pp_t *p)
{
	const pdef_t *d = s->def;
	memset(p, 0, sizeof(*p));
	VectorCopy(s->org, p->org);
	p->life = 1;
	p->radius0 = d->radius;
	for (int k = 0; k < 3; k++)
		p->rgb0[k] = d->color[k];
	p->alpha0 = d->color[3] / 255;
	p->rot = d->rotation;
	p->rotspeed = d->rotspeed;
	p->seq = d->sequence;
	for (int k = 0; k < 4; k++)
		p->rnd[k] = frand();
	for (int i = 0; i < d->numfuncs; i++) {
		const pfunc_t *f = &d->funcs[i];
		switch (f->fn) {
		case F_LIFETIME:
			p->life = rexp(f->a[0], f->a[1], f->a[2]);
			break;
		case F_POS_SPHERE: {
			float dir[3], dist, speed, lv[3];
			do {
				for (int k = 0; k < 3; k++)
					dir[k] = frand() * 2 - 1;
			} while (DotProduct(dir, dir) > 1 || DotProduct(dir, dir) < 1e-4f);
			VectorNormalize(dir);
			dist = f->a[0] + (f->a[1] - f->a[0]) * frand();
			for (int k = 0; k < 3; k++) {
				float o = dir[k] * dist * f->v[0][k];
				if (f->v[1][k] != 0)
					o = fabsf(o) * (f->v[1][k] < 0 ? -1 : 1);
				p->org[k] = s->org[k] + o;
			}
			speed = rexp(f->a[2], f->a[3], f->a[4]);
			for (int k = 0; k < 3; k++)
				lv[k] = f->v[2][k] + (f->v[3][k] - f->v[2][k]) * frand();
			for (int k = 0; k < 3; k++)
				p->vel[k] = dir[k] * speed + s->axis[0][k] * lv[0] + s->axis[1][k] * lv[1] + s->axis[2][k] * lv[2];
			break;
		}
		case F_ROTATION:
			p->rot = f->a[0] + rexp(f->a[1], f->a[2], f->a[3]);
			break;
		case F_YAWFLIP:
			p->flip = frand() < f->a[0];
			break;
		case F_ROTSPEED:
			p->rotspeed = f->a[0] + rexp(f->a[1], f->a[2], f->a[3]);
			if (f->a[4] != 0 && frand() < 0.5f)
				p->rotspeed = -p->rotspeed;
			break;
		case F_SEQUENCE:
			p->seq = (int)f->a[0] + (int)(frand() * (f->a[1] - f->a[0] + 1));
			break;
		case F_COLOR: {
			float t = frand();
			for (int k = 0; k < 3; k++)
				p->rgb0[k] = f->v[0][k] + (f->v[1][k] - f->v[0][k]) * t;
			break;
		}
		case F_ALPHA:
			p->alpha0 = rexp(f->a[0], f->a[1], f->a[2]) / 255;
			break;
		case F_RADIUS:
			p->radius0 = rexp(f->a[0], f->a[1], f->a[2]);
			break;
		case F_TRAILLEN:
			p->traillen = rexp(f->a[0], f->a[1], f->a[2]);
			break;
		}
	}
	if (p->life < 0.001f)
		p->life = 0.001f;
	VectorCopy(p->rgb0, p->rgb);
	p->alpha = p->alpha0;
	p->radius = p->radius0;
}

static void emit(psys_t *s, int count)
{
	int max = BW_MIN(s->def->maxparts, 4096);
	for (int i = 0; i < count && s->n < max; i++)
		init_particle(s, &s->p[s->n++]);
}

/* one particle's step; false when it dies */
static bool step_particle(psys_t *s, pp_t *p, float dt)
{
	const pdef_t *d = s->def;
	float t, old[3];
	p->age += dt;
	if (p->age >= p->life)
		return false;
	t = p->age / p->life;
	VectorCopy(p->org, old);
	VectorCopy(p->rgb0, p->rgb);
	p->alpha = p->alpha0;
	p->radius = p->radius0;
	for (int i = 0; i < d->numfuncs; i++) {
		const pfunc_t *f = &d->funcs[i];
		switch (f->fn) {
		case F_ALPHA_FADE_DECAY: {
			/* start_alpha rises to full over the fade-in window, full falls
			 * to end_alpha over the fade-out window, and the particle dies
			 * when the fade-out ends; times are fractions of its life */
			float a = 1;
			if (t < f->a[3])
				a = f->a[3] > f->a[2] ? f->a[0] + (1 - f->a[0]) * clamp01((t - f->a[2]) / (f->a[3] - f->a[2])) : 1;
			else if (t > f->a[4])
				a = f->a[5] > f->a[4] ? 1 + (f->a[1] - 1) * clamp01((t - f->a[4]) / (f->a[5] - f->a[4])) : f->a[1];
			if (t >= f->a[5])
				return false;
			p->alpha *= a;
			break;
		}
		case F_ALPHA_FADE_OUT_RANDOM: {
			float ft = f->a[0] + (f->a[1] - f->a[0]) * (f->a[2] == 1 ? p->rnd[0] : powf(p->rnd[0], f->a[2]));
			float span = f->a[3] != 0 ? ft : ft / p->life, x;
			x = span > 0 ? clamp01((t - (1 - span)) / span) : 0;
			x = bias(x, f->a[5]);
			if (f->a[4] != 0)
				x = spline(x);
			p->alpha *= 1 - x;
			break;
		}
		case F_COLOR_FADE: {
			float x = f->a[1] > f->a[0] ? clamp01((t - f->a[0]) / (f->a[1] - f->a[0])) : (t >= f->a[0]);
			if (f->a[2] != 0)
				x = spline(x);
			for (int k = 0; k < 3; k++)
				p->rgb[k] = p->rgb0[k] + (f->v[0][k] - p->rgb0[k]) * x;
			break;
		}
		case F_MOVE_BASIC: {
			/* drag removes that fraction of the speed per thirtieth of a second */
			float keep = f->a[0] > 0 ? powf(BW_MAX(0.0f, 1 - f->a[0]), dt * 30) : 1;
			for (int k = 0; k < 3; k++) {
				p->vel[k] = (p->vel[k] + f->v[0][k] * dt) * keep;
				p->org[k] += p->vel[k] * dt;
			}
			break;
		}
		case F_RANDOM_FORCE:
			for (int k = 0; k < 3; k++)
				p->vel[k] += (f->v[0][k] + (f->v[1][k] - f->v[0][k]) * frand()) * dt;
			break;
		case F_OSCILLATE: {
			float rate = f->a[1] + (f->a[2] - f->a[1]) * p->rnd[1];
			float freq = f->a[3] + (f->a[4] - f->a[3]) * p->rnd[2];
			float tt = f->a[5] != 0 ? t : p->age;
			float st = f->a[6] + (f->a[7] - f->a[6]) * p->rnd[3], et = f->a[8] + (f->a[9] - f->a[8]) * p->rnd[3];
			float mult = f->a[10] != 0 ? f->a[10] : 1, v;
			if (tt < st || tt > et)
				break;
			v = rate * sinf((tt * freq * mult + f->a[11]) * 2 * (float)M_PI) * dt;
			if ((int)f->a[0] == 4)
				p->rot += v * 180 / (float)M_PI;
			else
				p->osc += v;
			break;
		}
		case F_RADIUS_SCALE: {
			float x = f->a[1] > f->a[0] ? clamp01((t - f->a[0]) / (f->a[1] - f->a[0])) : (t >= f->a[0]);
			x = bias(x, f->a[4]);
			if (f->a[5] != 0)
				x = spline(x);
			p->radius *= f->a[2] + (f->a[3] - f->a[2]) * x;
			break;
		}
		case F_ROT_BASIC:
			p->rot += p->rotspeed * dt;
			break;
		case F_SPIN_ROLL: {
			float rate = f->a[0];
			if (f->a[1] > 0)
				rate = f->a[2] + (f->a[0] - f->a[2]) * clamp01(1 - (s->time / f->a[1]));
			p->rot += rate * dt;
			break;
		}
		case F_COLLIDE:
			if (cl.world) {
				bsptrace_t tr;
				float zero[3] = {0, 0, 0};
				BSP_TraceModel(cl.world, 0, zero, old, zero, zero, p->org, CONTENTS_SOLID, &tr);
				if (tr.fraction < 1) {
					float dd = DotProduct(p->vel, tr.normal);
					if (f->a[2] != 0)
						return false;
					for (int k = 0; k < 3; k++) {
						float n = dd * tr.normal[k], tang = p->vel[k] - n;
						p->vel[k] = -n * f->a[0] + tang * (1 - BW_MIN(1.0f, f->a[1]));
						p->org[k] = tr.endpos[k] + tr.normal[k] * 0.5f;
					}
				}
			}
			break;
		}
	}
	/* oscillation of anything but rotation lands on the radius or alpha */
	for (int i = 0; i < d->numfuncs; i++)
		if (d->funcs[i].fn == F_OSCILLATE) {
			int field = (int)d->funcs[i].a[0];
			if (field == 3)
				p->radius = BW_MAX(0.0f, p->radius + p->osc);
			else if (field == 7)
				p->alpha = clamp01(p->alpha + p->osc);
			break;
		}
	return true;
}

static void update_system(psys_t *s, float dt)
{
	const pdef_t *d = s->def;
	int out = 0, e = 0;
	bool waiting = false;
	float was = s->time;
	s->time += dt;
	if (s->time < 0)
		return;
	if (was < 0)
		dt = s->time; /* the delay ended inside this frame */
	if (!s->started) {
		s->started = true;
		if (d->initial > 0)
			emit(s, d->initial);
	}
	/* each instant emitter fires once, when its start time has passed */
	for (int i = 0; i < d->numfuncs; i++) {
		if (d->funcs[i].fn != F_EMIT_INSTANT)
			continue;
		if (e < 32 && !(s->fired & (1u << e))) {
			if (s->time >= d->funcs[i].a[1]) {
				s->fired |= 1u << e;
				emit(s, (int)d->funcs[i].a[0]);
			} else {
				waiting = true;
			}
		}
		e++;
	}
	s->emitted = !waiting;
	for (int i = 0; i < s->n; i++) {
		pp_t *p = &s->p[i];
		if (!step_particle(s, p, dt))
			continue;
		if (out != i)
			s->p[out] = *p;
		out++;
	}
	s->n = out;
}

void R_PCF_Update(float dt)
{
	int out = 0;
	for (int i = 0; i < numsystems; i++) {
		psys_t *s = &systems[i];
		update_system(s, dt);
		if (s->time >= 0 && s->emitted && !s->n) {
			Mem_Free(s->p);
			continue;
		}
		if (out != i)
			systems[out] = *s;
		out++;
	}
	numsystems = out;
}

/* spawns a system at org facing dir (control point 0); false when there is
 * no system of that name */
bool R_PCF_Spawn(const char *name, const float *org, const float *dir)
{
	pdef_t *d;
	float axis[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	if (!name || !*name || !(d = find_def(name)))
		return false;
	if (dir && DotProduct(dir, dir) > 1e-6f) {
		float up[3] = {0, 0, 1};
		VectorCopy(dir, axis[0]);
		VectorNormalize(axis[0]);
		if (fabsf(axis[0][2]) > 0.99f)
			up[0] = 1, up[2] = 0;
		CrossProduct(up, axis[0], axis[1]);
		VectorNormalize(axis[1]);
		CrossProduct(axis[0], axis[1], axis[2]);
	}
	start_system(d, org, (const float(*)[3])axis, 0, 0);
	return true;
}

bool R_PCF_Exists(const char *name) { return name && *name && find_def(name) != NULL; }

/* --- drawing ------------------------------------------------------------------- */

#define PCF_BATCH 2048
static rvert_t bv[PCF_BATCH * 4];
static uint16_t bi[PCF_BATCH * 6];
static int bn;

static void pcf_flush(const pdef_t *d)
{
	if (!bn || !d)
		return;
	GL_BindTexture(0, GL_TEXTURE_2D, d->mat->diffuse->tex);
	if (d->additive)
		GL_SetBlend(BLEND_CUSTOM, GL_SRC_ALPHA, GL_ONE);
	else
		GL_SetBlend(BLEND_CUSTOM, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	R_StreamDraw(GL_TRIANGLES, bv, bn * 4, bi, bn * 6);
	bn = 0;
}

static void pcf_quad(const pdef_t *d, const float c[4][3], const float *tc, bool flip, const float *rgba)
{
	static const float uv[4][2] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
	rvert_t *v;
	if (bn == PCF_BATCH)
		pcf_flush(d);
	v = &bv[bn * 4];
	for (int i = 0; i < 4; i++) {
		float u = flip ? 1 - uv[i][0] : uv[i][0];
		VectorCopy(c[i], v[i].xyz);
		v[i].st[0] = tc[0] + (tc[2] - tc[0]) * u;
		v[i].st[1] = tc[1] + (tc[3] - tc[1]) * uv[i][1];
		for (int k = 0; k < 4; k++)
			v[i].color[k] = (uint8_t)(clamp01(rgba[k]) * 255);
	}
	for (int i = 0; i < 6; i++)
		bi[bn * 6 + i] = (uint16_t)(bn * 4 + (int[]){0, 1, 2, 0, 2, 3}[i]);
	bn++;
}

/* the sheet frame for a particle, as texture coordinates */
static void sheet_tc(const pdef_t *d, const pp_t *p, const pfunc_t *r, float *tc)
{
	const struct pseq_s *q = NULL;
	float time;
	tc[0] = 0, tc[1] = 0, tc[2] = 1, tc[3] = 1;
	if (!d->sheet)
		return;
	for (int i = 0; i < d->sheet->nseq; i++)
		if (d->sheet->seq[i].num == p->seq) {
			q = &d->sheet->seq[i];
			break;
		}
	if (!q)
		q = &d->sheet->seq[BW_MAX(0, BW_MIN(p->seq, d->sheet->nseq - 1))];
	if (r && r->fn == F_R_SPRITES && r->a[1] != 0)
		time = p->age / p->life * q->total;
	else if (r && r->fn == F_R_SPRITES && r->a[2] != 0)
		time = p->age * r->a[0];
	else
		time = p->age * (r ? r->a[r->fn == F_R_TRAIL ? 3 : 0] : 0.1f) * q->total;
	if (q->clamp)
		time = BW_MIN(time, q->total * 0.9999f);
	else if (q->total > 0)
		time = fmodf(time, q->total);
	for (int f = 0; f < q->nframes; f++) {
		if (time < q->dur[f] || f == q->nframes - 1) {
			memcpy(tc, q->uv[f], sizeof(float) * 4);
			return;
		}
		time -= q->dur[f];
	}
}

static int depth_cmp(const void *a, const void *b)
{
	const pp_t *x = a, *y = b;
	float dx = 0, dy = 0;
	for (int k = 0; k < 3; k++) {
		dx += (x->org[k] - r_scene.origin[k]) * r_scene.forward[k];
		dy += (y->org[k] - r_scene.origin[k]) * r_scene.forward[k];
	}
	return dx < dy ? 1 : dx > dy ? -1 : 0;
}

static void draw_system(psys_t *s)
{
	const pdef_t *d = s->def;
	const pfunc_t *velrot = NULL;
	float tanfov = tanf(r_scene.fov_y * (float)M_PI / 360);
	for (int i = 0; i < d->numfuncs; i++)
		if (d->funcs[i].fn == F_R_VELROT)
			velrot = &d->funcs[i];
	if (!d->additive)
		qsort(s->p, s->n, sizeof(pp_t), depth_cmp);
	for (int ri = 0; ri < d->numfuncs; ri++) {
		const pfunc_t *r = &d->funcs[ri];
		if (r->fn != F_R_SPRITES && r->fn != F_R_TRAIL)
			continue;
		for (int i = 0; i < s->n; i++) {
			const pp_t *p = &s->p[i];
			float c[4][3], tc[4], rgba[4], to[3], dist;
			for (int k = 0; k < 3; k++) {
				to[k] = p->org[k] - r_scene.origin[k];
				rgba[k] = p->rgb[k] / 255 * d->overbright;
			}
			rgba[3] = p->alpha;
			dist = DotProduct(to, r_scene.forward);
			if (dist < 1 || p->radius <= 0)
				continue;
			/* SpriteCard: fade out as the particle fills more of the screen */
			if (d->fadesize[1] > d->fadesize[0] && d->fadesize[1] > 0) {
				float frac = p->radius / (dist * tanfov);
				rgba[3] *= 1 - clamp01((frac - d->fadesize[0]) / (d->fadesize[1] - d->fadesize[0]));
			}
			if (rgba[3] <= 0.002f)
				continue;
			sheet_tc(d, p, r, tc);
			if (r->fn == F_R_SPRITES) {
				float rot = p->rot, a, cs, sn, rv[3], uv[3];
				if (velrot) {
					float sx = DotProduct(p->vel, r_scene.right), sy = DotProduct(p->vel, r_scene.up);
					if (sx * sx + sy * sy > 1e-4f)
						rot = atan2f(sy, sx) * 180 / (float)M_PI - velrot->a[1];
				}
				a = rot * (float)M_PI / 180;
				cs = cosf(a) * p->radius, sn = sinf(a) * p->radius;
				for (int k = 0; k < 3; k++) {
					rv[k] = r_scene.right[k] * cs + r_scene.up[k] * sn;
					uv[k] = r_scene.up[k] * cs - r_scene.right[k] * sn;
				}
				for (int k = 0; k < 3; k++) {
					c[0][k] = p->org[k] - rv[k] - uv[k];
					c[1][k] = p->org[k] - rv[k] + uv[k];
					c[2][k] = p->org[k] + rv[k] + uv[k];
					c[3][k] = p->org[k] + rv[k] - uv[k];
				}
				pcf_quad(d, c, tc, p->flip, rgba);
			} else {
				/* a quad from the particle back along its motion */
				float speed = VectorLength(p->vel), len, tail[3], dir[3], side[3];
				if (speed < 1e-3f)
					continue;
				len = speed * p->traillen;
				len = BW_MAX(r->a[0], BW_MIN(r->a[1], len));
				if (r->a[2] > 0)
					len *= clamp01(p->age / r->a[2]);
				for (int k = 0; k < 3; k++) {
					dir[k] = p->vel[k] / speed;
					tail[k] = p->org[k] - dir[k] * len;
				}
				CrossProduct(dir, to, side);
				if (VectorNormalize(side) < 1e-6f)
					continue;
				for (int k = 0; k < 3; k++) {
					c[0][k] = tail[k] - side[k] * p->radius;
					c[1][k] = tail[k] + side[k] * p->radius;
					c[2][k] = p->org[k] + side[k] * p->radius;
					c[3][k] = p->org[k] - side[k] * p->radius;
				}
				pcf_quad(d, c, tc, false, rgba);
			}
		}
		pcf_flush(d);
	}
}

void R_PCF_Draw(void)
{
	const glprog_t *pr = &r_progs[PROG_UNLIT];
	if (!numsystems || !r_drawpcf->ivalue)
		return;
	R_UseProgram(PROG_UNLIT);
	qglUniformMatrix4fv(pr->u_mvp, 1, GL_FALSE, r_scene.viewproj);
	qglUniform4f(pr->u_color, 1, 1, 1, 1);
	qglUniform4f(pr->u_tcmod, 0, 0, 1, 1);
	qglUniform1f(pr->u_alphatest, 0);
	GL_SetDepth(true, false);
	GL_SetCull(false);
	bn = 0;
	for (int i = 0; i < numsystems; i++)
		if (systems[i].time >= 0 && systems[i].n)
			draw_system(&systems[i]);
	GL_SetCull(true);
	GL_SetDepth(true, true);
}

int R_PCF_Count(void)
{
	int n = 0;
	for (int i = 0; i < numsystems; i++)
		n += systems[i].n;
	return n;
}

void R_PCF_Clear(void)
{
	for (int i = 0; i < numsystems; i++)
		Mem_Free(systems[i].p);
	numsystems = 0;
}

/* --- commands ------------------------------------------------------------------- */

static void free_files(void)
{
	R_PCF_Clear();
	while (pfiles) {
		pfile_t *n = pfiles->next;
		for (int i = 0; i < pfiles->numdefs; i++) {
			pdef_t *d = &pfiles->defs[i];
			if (d->sheet) {
				for (int s = 0; s < d->sheet->nseq; s++) {
					Mem_Free(d->sheet->seq[s].dur);
					Mem_Free(d->sheet->seq[s].uv);
				}
				Mem_Free(d->sheet->seq);
				Mem_Free(d->sheet);
			}
			Mem_Free(d->funcs);
			Mem_Free(d->children);
		}
		Mem_Free(pfiles->defs);
		Mem_Free(pfiles);
		pfiles = n;
	}
	manifest_read = false;
}

static void pcfinfo_f(int argc, const char **argv)
{
	pdef_t *d;
	if (argc < 2) {
		int n = 0;
		for (pfile_t *pf = pfiles; pf; pf = pf->next)
			if (pf->numdefs) {
				Con_Printf("particles/%s.pcf: %d systems\n", pf->name, pf->numdefs);
				n++;
			}
		Con_Printf("%d files, %d systems running, %d particles\n", n, numsystems, R_PCF_Count());
		return;
	}
	if (!(d = find_def(argv[1]))) {
		Con_Printf("no particle system %s\n", argv[1]);
		return;
	}
	def_material(d);
	Con_Printf("%s (particles/%s.pcf)\n  material %s%s%s, %d sequences\n  max %d particles\n", d->name, d->file->name,
	           d->matname, d->mat == dotmat ? " (missing: soft dot)" : "", d->additive ? ", additive" : "",
	           d->sheet ? d->sheet->nseq : 0, d->maxparts);
	for (int i = 0; i < d->numfuncs; i++)
		Con_Printf("  %s\n", funcs[d->funcs[i].fn].name);
	if (d->skipped[0])
		Con_Printf("  not run: %s\n", d->skipped);
	for (int i = 0; i < d->numchildren; i++)
		Con_Printf("  child %s, after %gs\n", d->children[i].name, d->children[i].delay);
}

static void pcfreload_f(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	free_files();
	Con_Printf("particle systems will be read again\n");
}

void R_PCF_Init(void)
{
	r_drawpcf = Cvar_Get("r_drawpcf", "1", CVAR_ARCHIVE, "Draw Source particle systems (.pcf)");
	Cmd_AddD("r_pcfinfo", pcfinfo_f, "r_pcfinfo [system]: the .pcf files read, or what one system does");
	Cmd_AddD("r_pcfreload", pcfreload_f, "Forget the .pcf files read so they are read again");
}

void R_PCF_Shutdown(void)
{
	free_files();
	dotmat = NULL;
}
