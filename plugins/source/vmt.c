/* Bulwark Source formats plugin - VMT materials: the shader name and the
 * keys that decide how a surface draws. See docs/spec/source-formats.md.
 * ISC licence, see LICENSE. */
#include "source.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KV 96

typedef struct {
	char shader[64];
	char key[MAX_KV][48];
	char value[MAX_KV][192];
	int num;
} vmt_t;

typedef struct {
	const char *p, *end;
} lex_t;

/* the next token: a quoted string, a brace, or a run of other characters.
 * [conditions] after a value are skipped. 0 at the end. */
static int token(lex_t *l, char *out, size_t size)
{
	size_t n = 0;
	for (;;) {
		while (l->p < l->end && isspace((unsigned char)*l->p))
			l->p++;
		if (l->p + 1 < l->end && l->p[0] == '/' && l->p[1] == '/') {
			while (l->p < l->end && *l->p != '\n')
				l->p++;
			continue;
		}
		if (l->p < l->end && *l->p == '[') {
			while (l->p < l->end && *l->p != ']')
				l->p++;
			if (l->p < l->end)
				l->p++;
			continue;
		}
		break;
	}
	if (l->p >= l->end)
		return 0;
	if (*l->p == '{' || *l->p == '}') {
		out[0] = *l->p++;
		out[1] = 0;
		return 1;
	}
	if (*l->p == '"') {
		l->p++;
		while (l->p < l->end && *l->p != '"') {
			if (n + 1 < size)
				out[n++] = *l->p;
			l->p++;
		}
		if (l->p < l->end)
			l->p++;
	} else {
		while (l->p < l->end && !isspace((unsigned char)*l->p) && *l->p != '{' && *l->p != '}' && *l->p != '"') {
			if (n + 1 < size)
				out[n++] = *l->p;
			l->p++;
		}
	}
	out[n] = 0;
	return 1;
}

static void set_key(vmt_t *v, const char *k, const char *val)
{
	for (int i = 0; i < v->num; i++)
		if (!strcasecmp(v->key[i], k)) {
			snprintf(v->value[i], sizeof(v->value[i]), "%s", val);
			return;
		}
	if (v->num < MAX_KV) {
		snprintf(v->key[v->num], sizeof(v->key[0]), "%s", k);
		snprintf(v->value[v->num], sizeof(v->value[0]), "%s", val);
		v->num++;
	}
}

static const char *get(const vmt_t *v, const char *k)
{
	for (int i = 0; i < v->num; i++)
		if (!strcasecmp(v->key[i], k))
			return v->value[i];
	return "";
}

static void skip_block(lex_t *l)
{
	char t[256];
	int depth = 1;
	while (depth > 0 && token(l, t, sizeof(t))) {
		if (!strcmp(t, "{"))
			depth++;
		else if (!strcmp(t, "}"))
			depth--;
	}
}

/* keys up to the closing brace. Nested blocks named for the hardware level
 * (">=dx90") hold the settings a current card uses, so they count; fallback
 * blocks ("<dx90") and the rest (Proxies) don't. */
static void read_block(lex_t *l, vmt_t *v, vmt_t *replace, vmt_t *insert)
{
	char k[256], val[256];
	while (token(l, k, sizeof(k)) && strcmp(k, "}")) {
		if (!token(l, val, sizeof(val)))
			return;
		if (!strcmp(val, "{")) {
			if (replace && !strcasecmp(k, "replace"))
				read_block(l, replace, NULL, NULL);
			else if (insert && !strcasecmp(k, "insert"))
				read_block(l, insert, NULL, NULL);
			else if (!strncmp(k, ">=", 2))
				read_block(l, v, NULL, NULL);
			else
				skip_block(l);
			continue;
		}
		set_key(v, k, val);
	}
}

static int parse(const char *text, size_t len, vmt_t *v, int depth)
{
	lex_t l = {text, text + len};
	char brace[8];
	vmt_t *rep, *ins;
	memset(v, 0, sizeof(*v));
	if (!token(&l, v->shader, sizeof(v->shader)) || !token(&l, brace, sizeof(brace)) || strcmp(brace, "{"))
		return 0;
	if (strcasecmp(v->shader, "patch")) {
		read_block(&l, v, NULL, NULL);
		return 1;
	}
	/* "patch": another material with keys replaced or added */
	rep = api->alloc(sizeof(*rep));
	ins = api->alloc(sizeof(*ins));
	read_block(&l, v, rep, ins);
	{
		char inc[256];
		size_t ilen;
		char *itext;
		snprintf(inc, sizeof(inc), "%s", get(v, "include"));
		src_cleanpath(inc);
		itext = depth < 4 && inc[0] ? api->load_file(inc, &ilen) : NULL;
		if (!itext || !parse(itext, ilen, v, depth + 1)) {
			api->free(itext);
			api->free(rep);
			api->free(ins);
			return 0;
		}
		api->free(itext);
	}
	for (int i = 0; i < rep->num; i++)
		set_key(v, rep->key[i], rep->value[i]);
	for (int i = 0; i < ins->num; i++)
		if (!get(v, ins->key[i])[0])
			set_key(v, ins->key[i], ins->value[i]);
	api->free(rep);
	api->free(ins);
	return 1;
}

static int is_true(const char *s) { return s[0] && atof(s) != 0; }

/* a texture key's value as a game path under materials/, without .vtf */
static void texture_path(const char *val, char *out, size_t size)
{
	char t[192];
	size_t n;
	snprintf(t, sizeof(t), "%s", val);
	src_cleanpath(t);
	n = strlen(t);
	if (n > 4 && !strcmp(t + n - 4, ".vtf"))
		t[n - 4] = 0;
	if (!t[0])
		out[0] = 0;
	else if (!strncmp(t, "materials/", 10))
		snprintf(out, size, "%s", t);
	else
		snprintf(out, size, "materials/%s", t);
}

int VMT_Load(const char *path, const void *data, size_t len, bwplug_material_t *out)
{
	vmt_t *v = api->alloc(sizeof(*v));
	char shader[64], name[256];
	const char *base;
	if (!parse(data, len, v, 0)) {
		srcprint("vmt: couldn't read %s\n", path);
		api->free(v);
		return 0;
	}
	snprintf(shader, sizeof(shader), "%s", v->shader);
	for (char *c = shader; *c; c++)
		*c = (char)tolower((unsigned char)*c);
	snprintf(name, sizeof(name), "%s", path);
	src_cleanpath(name);

	base = get(v, "$basetexture");
	texture_path(base, out->diffuse, sizeof(out->diffuse));
	texture_path(get(v, "$bumpmap")[0] ? get(v, "$bumpmap") : get(v, "$normalmap"), out->normal, sizeof(out->normal));
	if (is_true(get(v, "$translucent")))
		out->flags |= BWPLUG_MAT_TRANSLUCENT;
	if (is_true(get(v, "$additive")))
		out->flags |= BWPLUG_MAT_ADDITIVE;
	if (is_true(get(v, "$alphatest")))
		out->flags |= BWPLUG_MAT_ALPHATEST;
	if (is_true(get(v, "$nocull")))
		out->flags |= BWPLUG_MAT_TWOSIDED;
	if (get(v, "$alpha")[0])
		out->alpha = (float)atof(get(v, "$alpha"));
	if (strstr(shader, "unlit") || !strcmp(shader, "sprite"))
		out->flags |= BWPLUG_MAT_UNLIT;
	if (!strcmp(shader, "water")) {
		out->flags |= BWPLUG_MAT_WATER;
		/* water has no base texture: its normal map stands in */
		if (!out->diffuse[0])
			snprintf(out->diffuse, sizeof(out->diffuse), "%s", out->normal);
	}
	if (!strcmp(shader, "refract"))
		out->flags |= BWPLUG_MAT_TRANSLUCENT;
	/* tool textures: the compile flags, or the shipped tools/ names */
	if (is_true(get(v, "%compilenodraw")) || is_true(get(v, "%compileskip")) || is_true(get(v, "%compilehint")) ||
	    is_true(get(v, "%compileclip")) || is_true(get(v, "%compileplayerclip")) ||
	    is_true(get(v, "%compilenpcclip")) || is_true(get(v, "%compiletrigger")) ||
	    is_true(get(v, "%compileinvisible")) || strstr(name, "tools/toolsnodraw") || strstr(name, "tools/toolsskip") ||
	    strstr(name, "tools/toolshint") || strstr(name, "tools/toolsclip") || strstr(name, "tools/toolsplayerclip") ||
	    strstr(name, "tools/toolsnpcclip") || strstr(name, "tools/toolstrigger") || strstr(name, "tools/toolsinvisible") ||
	    strstr(name, "tools/toolsareaportal") || strstr(name, "tools/toolsoccluder"))
		out->flags |= BWPLUG_MAT_NODRAW;
	if (is_true(get(v, "%compilesky")) || is_true(get(v, "%compile2dsky")) || strstr(name, "tools/toolsskybox"))
		out->flags |= BWPLUG_MAT_SKY;
	api->free(v);
	return 1;
}
