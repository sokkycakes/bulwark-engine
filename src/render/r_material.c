/* Bulwark engine - materials. A material says how a surface or picture is
 * drawn: which GLSL program, which images, how it blends. They come from
 * Nuclide-style .mat files next to the texture, from shader scripts in
 * scripts/NAME.shader, or are made up from a plain image of the same name.
 * The script syntax is the Quake 3 shader language as documented in the
 * Q3Radiant shader manual, plus FTE's program/diffusemap keys and if/else
 * blocks as used by Nuclide's .mat files (see docs/spec/materials.md).
 * ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/fs.h"

#include <stdio.h>
#include <stdlib.h>

#define MAT_HASH 1024
static material_t *materials[MAT_HASH];
#define MAX_HANDLES 4096
static material_t *handles[MAX_HANDLES];
static int numhandles = 1;

int R_MaterialHandle(material_t *m)
{
	if (!m)
		return 0;
	for (int i = 1; i < numhandles; i++)
		if (handles[i] == m)
			return i;
	if (numhandles >= MAX_HANDLES)
		return 0;
	handles[numhandles] = m;
	return numhandles++;
}

material_t *R_MaterialForHandle(int h) { return h > 0 && h < numhandles ? handles[h] : NULL; }

/* shader script bodies from scripts/NAME.shader, by name */
typedef struct scriptdef_s {
	char *name;
	char *text; /* from the opening brace */
	struct scriptdef_s *next;
} scriptdef_t;
static scriptdef_t *scripts[MAT_HASH];

static unsigned name_hash(const char *s)
{
	unsigned h = 5381;
	for (; *s; s++)
		h = h * 33 + (unsigned char)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
	return h & (MAT_HASH - 1);
}

/* --- tokenizer ------------------------------------------------------------- */

typedef struct {
	const char *p;
	char tok[256];
	bool newline; /* a line break came before this token */
} lexer_t;

static bool next_token(lexer_t *l)
{
	const char *p = l->p;
	size_t n = 0;
	l->newline = false;
	for (;;) {
		while (*p && (unsigned char)*p <= ' ') {
			if (*p == '\n')
				l->newline = true;
			p++;
		}
		if (p[0] == '/' && p[1] == '/') {
			while (*p && *p != '\n')
				p++;
			continue;
		}
		if (p[0] == '/' && p[1] == '*') {
			p += 2;
			while (*p && !(p[0] == '*' && p[1] == '/'))
				p++;
			if (*p)
				p += 2;
			continue;
		}
		break;
	}
	if (!*p) {
		l->p = p;
		l->tok[0] = 0;
		return false;
	}
	if (*p == '{' || *p == '}' || *p == '(' || *p == ')') {
		l->tok[0] = *p++;
		l->tok[1] = 0;
		l->p = p;
		return true;
	}
	if (*p == '"') {
		p++;
		while (*p && *p != '"' && n < sizeof(l->tok) - 1)
			l->tok[n++] = *p++;
		if (*p == '"')
			p++;
	} else {
		while (*p && (unsigned char)*p > ' ' && *p != '{' && *p != '}' && *p != '(' && *p != ')' &&
		       n < sizeof(l->tok) - 1)
			l->tok[n++] = *p++;
	}
	l->tok[n] = 0;
	l->p = p;
	return true;
}

/* the rest of the current line, as tokens */
static int line_args(lexer_t *l, char args[][128], int max)
{
	int n = 0;
	for (;;) {
		lexer_t save = *l;
		if (!next_token(l) || l->newline || !strcmp(l->tok, "{") || !strcmp(l->tok, "}")) {
			*l = save;
			return n;
		}
		if (!strcmp(l->tok, "(") || !strcmp(l->tok, ")"))
			continue;
		if (n < max)
			Str_Copy(args[n++], l->tok, 128);
	}
}

/* --- conditions (if/else/endif) ------------------------------------------- */

static bool eval_condition(const char *expr)
{
	bool neg = false;
	while (*expr == '!') {
		neg = !neg;
		expr++;
	}
	if (*expr == '$') {
		/* features this renderer has: GLSL programs */
		bool v = !Str_ICmp(expr, "$programs") || !Str_ICmp(expr, "$glsl");
		return neg ? !v : v;
	}
	{
		cvar_t *v = Cvar_Find(expr);
		bool on = v && v->value != 0;
		return neg ? !on : on;
	}
}

/* --- parsing --------------------------------------------------------------- */

static int blend_factor(const char *s)
{
	static const struct {
		const char *name;
		GLenum v;
	} f[] = {{"GL_ONE", GL_ONE}, {"GL_ZERO", GL_ZERO}, {"GL_SRC_ALPHA", GL_SRC_ALPHA},
	         {"GL_ONE_MINUS_SRC_ALPHA", GL_ONE_MINUS_SRC_ALPHA}, {"GL_DST_COLOR", GL_DST_COLOR},
	         {"GL_ONE_MINUS_DST_COLOR", GL_ONE_MINUS_DST_COLOR}, {"GL_SRC_COLOR", GL_SRC_COLOR},
	         {"GL_ONE_MINUS_SRC_COLOR", GL_ONE_MINUS_SRC_COLOR}, {"GL_DST_ALPHA", GL_DST_ALPHA},
	         {"GL_ONE_MINUS_DST_ALPHA", GL_ONE_MINUS_DST_ALPHA}};
	for (size_t i = 0; i < BW_ARRAY_COUNT(f); i++)
		if (!Str_ICmp(s, f[i].name))
			return (int)f[i].v;
	return -1;
}

static int image_flags(const material_t *m, int flags)
{
	return (flags & MATF_2D) ? IMG_NOMIP : 0; /* clampmap asks for clamping */
}

static int program_for(const char *name)
{
	char base[64];
	char *hash;
	Str_Copy(base, name, sizeof(base));
	hash = strchr(base, '#'); /* permutation defines such as #MASK=0.5 */
	if (hash)
		*hash = 0;
	if (!Str_ICmp(base, "defaultwall") || !Str_ICmp(base, "lightmapped") || !Str_ICmp(base, "terrain") ||
	    !Str_ICmp(base, "lightmapped_reflect") || !Str_ICmp(base, "decal_reflectcube"))
		return MP_WALL;
	if (!Str_ICmp(base, "defaultskin"))
		return MP_SKIN;
	if (!Str_ICmp(base, "unlit") || !Str_ICmp(base, "heat") || !Str_ICmp(base, "refract"))
		return MP_UNLIT;
	if (!Str_ICmp(base, "skybox") || !Str_ICmp(base, "skybox_hdr") || !Str_ICmp(base, "skybox_parallax"))
		return MP_SKY;
	if (!Str_ICmp(base, "water") || !Str_ICmp(base, "water_high"))
		return MP_WATER;
	return MP_DEFAULT;
}

static void parse_stage(material_t *m, lexer_t *l, int flags)
{
	mstage_t *st = m->numstages < MAT_MAX_STAGES ? &m->stages[m->numstages++] : NULL;
	mstage_t dummy;
	char args[8][128];
	int depth = 1;
	if (!st)
		st = &dummy;
	memset(st, 0, sizeof(*st));
	st->color[0] = st->color[1] = st->color[2] = st->color[3] = 1;
	st->scale[0] = st->scale[1] = 1;
	while (depth > 0 && next_token(l)) {
		char k[128]; /* the lexer's buffer is reused by line_args */
		int n;
		Str_Copy(k, l->tok, sizeof(k));
		if (!strcmp(k, "{")) {
			depth++;
			continue;
		}
		if (!strcmp(k, "}")) {
			depth--;
			continue;
		}
		n = line_args(l, args, 8);
		if (!Str_ICmp(k, "map") || !Str_ICmp(k, "clampmap") || !Str_ICmp(k, "animmap")) {
			const char *img = !Str_ICmp(k, "animmap") ? (n > 1 ? args[1] : "") : (n > 0 ? args[0] : "");
			if (!Str_ICmp(img, "$lightmap"))
				st->lightmap = true;
			else if (!Str_ICmp(img, "$whiteimage") || !Str_ICmp(img, "$whitetexture"))
				st->image = r_whiteimage;
			else if (img[0] && (img[0] != '$' || !Str_NICmp(img, "$rt:", 4)))
				st->image = R_FindImage(img, image_flags(m, flags) | (!Str_ICmp(k, "clampmap") ? IMG_CLAMP : 0));
		} else if (!Str_ICmp(k, "blendFunc") && n > 0) {
			if (!Str_ICmp(args[0], "add"))
				st->blend = BLEND_ADD, st->src = GL_ONE, st->dst = GL_ONE;
			else if (!Str_ICmp(args[0], "blend"))
				st->blend = BLEND_ALPHA;
			else if (!Str_ICmp(args[0], "filter"))
				st->blend = BLEND_FILTER;
			else if (n > 1 && blend_factor(args[0]) >= 0 && blend_factor(args[1]) >= 0) {
				st->src = (GLenum)blend_factor(args[0]);
				st->dst = (GLenum)blend_factor(args[1]);
				if (st->src == GL_ONE && st->dst == GL_ZERO)
					st->blend = BLEND_OPAQUE;
				else if (st->src == GL_SRC_ALPHA && st->dst == GL_ONE_MINUS_SRC_ALPHA)
					st->blend = BLEND_ALPHA;
				else
					st->blend = BLEND_CUSTOM;
			}
		} else if (!Str_ICmp(k, "rgbGen") && n > 0) {
			if (!Str_ICmp(args[0], "const") && n >= 4) {
				st->color[0] = (float)atof(args[1]);
				st->color[1] = (float)atof(args[2]);
				st->color[2] = (float)atof(args[3]);
			} else if (!Str_ICmp(args[0], "vertex") || !Str_ICmp(args[0], "exactVertex")) {
				st->vertexcolor = true;
			}
		} else if (!Str_ICmp(k, "alphaGen") && n > 1 && !Str_ICmp(args[0], "const")) {
			st->color[3] = (float)atof(args[1]);
		} else if (!Str_ICmp(k, "tcMod") && n > 0) {
			if (!Str_ICmp(args[0], "scroll") && n >= 3) {
				st->scroll[0] = (float)atof(args[1]);
				st->scroll[1] = (float)atof(args[2]);
			} else if (!Str_ICmp(args[0], "scale") && n >= 3) {
				st->scale[0] = (float)atof(args[1]);
				st->scale[1] = (float)atof(args[2]);
			} else if (!Str_ICmp(args[0], "rotate") && n >= 2) {
				st->rotate = (float)atof(args[1]);
			}
		} else if (!Str_ICmp(k, "alphaFunc")) {
			st->alphatest = true;
		}
	}
}

static void parse_body(material_t *m, lexer_t *l, int flags)
{
	char args[8][128];
	int depth = 0;
	/* nesting of if/else blocks: each level says whether it is taken */
	bool skipping[16] = {false};
	bool taken[16] = {false};
	int level = 0;

	if (!next_token(l) || strcmp(l->tok, "{"))
		return;
	depth = 1;
	while (depth > 0 && next_token(l)) {
		char k[128]; /* the lexer's buffer is reused by line_args */
		int n;
		bool skip = level > 0 && skipping[level];
		Str_Copy(k, l->tok, sizeof(k));
		if (!Str_ICmp(k, "if")) {
			n = line_args(l, args, 8);
			if (level < 15) {
				level++;
				taken[level] = !skip && n > 0 && eval_condition(args[0]);
				skipping[level] = skip || !taken[level];
			}
			continue;
		}
		if (!Str_ICmp(k, "elif") || !Str_ICmp(k, "else")) {
			bool outer = level > 1 && skipping[level - 1];
			n = line_args(l, args, 8);
			if (level > 0) {
				bool cond = !Str_ICmp(k, "else") || (n > 0 && eval_condition(args[0]));
				skipping[level] = outer || taken[level] || !cond;
				if (!skipping[level])
					taken[level] = true;
			}
			continue;
		}
		if (!Str_ICmp(k, "endif")) {
			if (level > 0)
				level--;
			continue;
		}
		if (!strcmp(k, "{")) {
			if (skip) {
				/* skip the whole stage */
				int d = 1;
				while (d > 0 && next_token(l)) {
					if (!strcmp(l->tok, "{"))
						d++;
					else if (!strcmp(l->tok, "}"))
						d--;
				}
			} else {
				parse_stage(m, l, flags);
			}
			continue;
		}
		if (!strcmp(k, "}")) {
			depth--;
			continue;
		}
		n = line_args(l, args, 8);
		if (skip)
			continue;
		if (!Str_ICmp(k, "program") && n > 0)
			m->program = program_for(args[0]);
		else if (!Str_ICmp(k, "diffusemap") && n > 0)
			m->diffuse = R_FindImage(args[0], image_flags(m, flags));
		else if (!Str_ICmp(k, "normalmap") && n > 0)
			m->normal = R_FindImage(args[0], 0);
		else if (!Str_ICmp(k, "specularmap") && n > 0)
			m->specular = R_FindImage(args[0], 0);
		else if ((!Str_ICmp(k, "fullbrightmap") || !Str_ICmp(k, "glowmap")) && n > 0)
			m->fullbright = R_FindImage(args[0], 0);
		else if (!Str_ICmp(k, "surfaceparm") && n > 0) {
			if (!Str_ICmp(args[0], "nodraw"))
				m->nodraw = true;
			else if (!Str_ICmp(args[0], "sky"))
				m->sky = true;
			else if (!Str_ICmp(args[0], "trans") && m->blend == BLEND_OPAQUE)
				m->blend = BLEND_ALPHA;
		} else if (!Str_ICmp(k, "skyparms") && n > 0) {
			m->sky = true;
			if (args[0][0] && strcmp(args[0], "-"))
				m->skybox = R_LoadCubemap(args[0]);
		} else if (!Str_ICmp(k, "cull") && n > 0) {
			if (!Str_ICmp(args[0], "none") || !Str_ICmp(args[0], "disable") || !Str_ICmp(args[0], "twosided"))
				m->twosided = true;
		} else if (!Str_ICmp(k, "polygonOffset")) {
			m->polygonoffset = true;
		}
	}
}

/* fills in what the stages imply once parsing is done */
static void finish(material_t *m, int flags)
{
	if (!m->diffuse)
		for (int i = 0; i < m->numstages; i++)
			if (m->stages[i].image && !m->stages[i].lightmap) {
				m->diffuse = m->stages[i].image;
				if (m->blend == BLEND_OPAQUE && i == 0 && m->stages[i].blend != BLEND_OPAQUE &&
				    m->stages[i].blend != BLEND_FILTER) {
					m->blend = m->stages[i].blend;
					m->src = m->stages[i].src;
					m->dst = m->stages[i].dst;
				}
				if (m->stages[i].alphatest)
					m->alphatest = true;
				break;
			}
	/* the classic two-stage lightmap * texture form: draw as one lightmapped surface */
	if (m->numstages >= 1 && m->stages[0].lightmap && m->program == MP_DEFAULT)
		m->program = MP_WALL;
	if (m->diffuse && m->blend == BLEND_OPAQUE && m->diffuse->alpha && (flags & MATF_2D))
		m->blend = BLEND_ALPHA;
	if (!m->diffuse)
		m->diffuse = m->numstages ? r_whiteimage : r_checkerimage;
	m->width = m->diffuse->width;
	m->height = m->diffuse->height;
	if (!m->numstages) {
		/* a single stage drawing the diffuse image, for the 2D path */
		mstage_t *st = &m->stages[m->numstages++];
		memset(st, 0, sizeof(*st));
		st->image = m->diffuse;
		st->blend = m->blend;
		st->src = m->src;
		st->dst = m->dst;
		st->color[0] = st->color[1] = st->color[2] = st->color[3] = 1;
		st->scale[0] = st->scale[1] = 1;
		st->alphatest = m->alphatest;
	}
}

static material_t *new_material(const char *name)
{
	material_t *m = Mem_Alloc(sizeof(*m));
	unsigned h = name_hash(name);
	Str_Copy(m->name, name, sizeof(m->name));
	m->alpha = 1;
	m->next = materials[h];
	materials[h] = m;
	return m;
}

static void strip_ext(const char *in, char *out, size_t size)
{
	char *dot, *slash;
	Str_Copy(out, in, size);
	dot = strrchr(out, '.');
	slash = strrchr(out, '/');
	if (dot && (!slash || dot > slash))
		*dot = 0;
}

static scriptdef_t *find_script(const char *name)
{
	for (scriptdef_t *s = scripts[name_hash(name)]; s; s = s->next)
		if (!Str_ICmp(s->name, name))
			return s;
	return NULL;
}

material_t *R_MaterialForImage(const char *name, image_t *img, int flags)
{
	material_t *m;
	for (m = materials[name_hash(name)]; m; m = m->next)
		if (!Str_ICmp(m->name, name))
			return m;
	m = new_material(name);
	m->diffuse = img;
	m->program = (flags & MATF_2D) ? MP_UNLIT : (flags & MATF_MODEL) ? MP_SKIN : MP_WALL;
	finish(m, flags);
	return m;
}

material_t *R_MaterialFromText(const char *name, const char *text, int flags)
{
	material_t *m;
	lexer_t l = {text, "", false};
	for (m = materials[name_hash(name)]; m; m = m->next)
		if (!Str_ICmp(m->name, name))
			return m;
	m = new_material(name);
	m->program = (flags & MATF_2D) ? MP_UNLIT : (flags & MATF_MODEL) ? MP_SKIN : MP_WALL;
	parse_body(m, &l, flags);
	m->fromfile = true;
	finish(m, flags);
	return m;
}

/* a .mat file or a shader script describes this material */
bool R_MaterialFileExists(const char *name)
{
	char base[128];
	strip_ext(name, base, sizeof(base));
	return FS_FileExists(va("%s.mat", base)) || find_script(base) || find_script(name);
}

material_t *R_FindMaterial(const char *name, int flags)
{
	material_t *m;
	char base[128], path[160];
	char *text = NULL;
	size_t len;
	scriptdef_t *sd;
	if (!name || !*name)
		name = "$default";
	for (m = materials[name_hash(name)]; m; m = m->next)
		if (!Str_ICmp(m->name, name))
			return m;
	m = new_material(name);
	m->program = (flags & MATF_2D) ? MP_UNLIT : (flags & MATF_MODEL) ? MP_SKIN : MP_WALL;
	strip_ext(name, base, sizeof(base));
	snprintf(path, sizeof(path), "%s.mat", base);
	if ((text = FS_LoadFile(path, &len)) != NULL) {
		lexer_t l = {text, "", false};
		parse_body(m, &l, flags);
		m->fromfile = true;
		Mem_Free(text);
	} else if ((sd = find_script(base)) != NULL || (sd = find_script(name)) != NULL) {
		lexer_t l = {sd->text, "", false};
		parse_body(m, &l, flags);
		m->fromfile = true;
	} else {
		m->diffuse = R_FindImage(name, image_flags(m, flags));
	}
	finish(m, flags);
	return m;
}

/* --- shader scripts ------------------------------------------------------- */

static void add_script_file(const char *path, const char *searchpath, void *ctx)
{
	size_t len;
	char *text = FS_LoadFile(path, &len);
	lexer_t l;
	(void)ctx;
	(void)searchpath;
	if (!text)
		return;
	l.p = text;
	while (next_token(&l)) {
		char name[128];
		const char *body;
		int depth;
		Str_Copy(name, l.tok, sizeof(name));
		body = l.p;
		if (!next_token(&l) || strcmp(l.tok, "{"))
			break;
		/* body points just before the brace */
		while (*body && *body != '{')
			body++;
		depth = 1;
		while (depth > 0 && next_token(&l)) {
			if (!strcmp(l.tok, "{"))
				depth++;
			else if (!strcmp(l.tok, "}"))
				depth--;
		}
		if (!find_script(name)) {
			scriptdef_t *s = Mem_Alloc(sizeof(*s));
			unsigned h = name_hash(name);
			s->name = Mem_Strdup(name);
			s->text = Mem_Alloc((size_t)(l.p - body) + 1);
			memcpy(s->text, body, (size_t)(l.p - body));
			s->next = scripts[h];
			scripts[h] = s;
		}
	}
	Mem_Free(text);
}

void R_InitMaterials(void)
{
	FS_List("scripts/NAME.shader", true, add_script_file, NULL);
}

void R_ShutdownMaterials(void)
{
	numhandles = 1;
	for (int i = 0; i < MAT_HASH; i++) {
		while (materials[i]) {
			material_t *n = materials[i]->next;
			Mem_Free(materials[i]);
			materials[i] = n;
		}
		while (scripts[i]) {
			scriptdef_t *n = scripts[i]->next;
			Mem_Free(scripts[i]->name);
			Mem_Free(scripts[i]->text);
			Mem_Free(scripts[i]);
			scripts[i] = n;
		}
	}
}

void R_ForEachMaterial(void (*fn)(material_t *m, void *ctx), void *ctx)
{
	for (int i = 0; i < MAT_HASH; i++)
		for (material_t *m = materials[i]; m; m = m->next)
			fn(m, ctx);
}
