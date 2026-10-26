/* Bulwark engine - VGUI layout and scheme files. Rules:
 * docs/spec/vgui-res.md. ISC licence, see LICENSE. */
#include "vgui_res.h"

#include <stdio.h>
#include <stdlib.h>

bool VGUI_ParseColor(const char *s, uint8_t out[4])
{
	long v[4] = { 0, 0, 0, 255 };
	int n = 0;
	const char *p = s;
	while (n < 5) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		char *end;
		long x = strtol(p, &end, 10);
		if (end == p || n == 4)
			return false;
		/* a number must end at a separator, so "12ab" is not a colour */
		if (*end && *end != ' ' && *end != '\t')
			return false;
		v[n++] = x;
		p = end;
	}
	if (n < 3)
		return false;
	for (int i = 0; i < 4; i++)
		out[i] = (uint8_t)(v[i] < 0 ? 0 : v[i] > 255 ? 255 : v[i]);
	return true;
}

static bool flag(const kv_t *def, const char *key)
{
	return KV_GetInt(def, key, 0) != 0;
}

static void loadfont(vgui_font_t *f, const kv_t *block)
{
	f->name = block->key;
	int n = 0;
	for (const kv_t *d = block->child; d; d = d->next)
		n += KV_IsBlock(d);
	f->defs = n ? Mem_Alloc(n * sizeof(*f->defs)) : NULL;
	for (const kv_t *d = block->child; d; d = d->next) {
		if (!KV_IsBlock(d))
			continue;
		vgui_fontdef_t *fd = &f->defs[f->numdefs++];
		fd->name = KV_GetString(d, "name", "");
		fd->tall = KV_GetInt(d, "tall", 0);
		fd->weight = KV_GetInt(d, "weight", 0);
		const char *yres = KV_GetString(d, "yres", NULL);
		if (yres) {
			char *end;
			fd->yres_min = (int)strtol(yres, &end, 10);
			fd->yres_max = (int)strtol(end, NULL, 10);
		}
		fd->flags = (flag(d, "antialias") ? VGUI_FONT_ANTIALIAS : 0) |
			(flag(d, "dropshadow") ? VGUI_FONT_DROPSHADOW : 0) |
			(flag(d, "underline") ? VGUI_FONT_UNDERLINE : 0) |
			(flag(d, "italic") ? VGUI_FONT_ITALIC : 0) |
			(flag(d, "outline") ? VGUI_FONT_OUTLINE : 0) |
			(flag(d, "symbol") ? VGUI_FONT_SYMBOL : 0) |
			(flag(d, "additive") ? VGUI_FONT_ADDITIVE : 0);
	}
}

bool VGUI_LoadScheme(vgui_scheme_t *s, const uint8_t *buf, size_t len, kv_report_t *rep)
{
	kv_report_t dummy;
	if (!rep)
		rep = &dummy;
	memset(s, 0, sizeof(*s));
	s->kv = KV_Parse(buf, len, 0, rep);
	if (!s->kv)
		return false;
	/* the first block is the scheme; later top-level entries are ignored */
	const kv_t *root = s->kv;
	while (root && !KV_IsBlock(root))
		root = root->next;
	if (!root) {
		snprintf(rep->err, sizeof(rep->err), "no scheme block");
		VGUI_FreeScheme(s);
		return false;
	}
	s->colors = KV_Find(root, "Colors");
	s->base = KV_Find(root, "BaseSettings");
	s->borders = KV_Find(root, "Borders");
	s->fontfiles = KV_Find(root, "CustomFontFiles");
	const kv_t *fonts = KV_Find(root, "Fonts");
	if (fonts) {
		int n = 0;
		for (const kv_t *f = fonts->child; f; f = f->next)
			n += KV_IsBlock(f);
		s->fonts = n ? Mem_Alloc(n * sizeof(*s->fonts)) : NULL;
		for (const kv_t *f = fonts->child; f; f = f->next)
			if (KV_IsBlock(f))
				loadfont(&s->fonts[s->numfonts++], f);
	}
	return true;
}

void VGUI_FreeScheme(vgui_scheme_t *s)
{
	for (int i = 0; i < s->numfonts; i++)
		Mem_Free(s->fonts[i].defs);
	Mem_Free(s->fonts);
	KV_Free(s->kv);
	memset(s, 0, sizeof(*s));
}

/* resolves ref inside one scheme; on failure *ref is left at the last name
 * the chain reached, for the next scheme to carry on from */
static bool schemecolor(const vgui_scheme_t *s, const char **ref, uint8_t out[4])
{
	/* BaseSettings entries name Colors entries; the hop limit stops a loop */
	for (int hop = 0; hop < 8; hop++) {
		if (VGUI_ParseColor(*ref, out))
			return true;
		const kv_t *c = KV_FindPath(s->colors, *ref);
		if (c && c->value && VGUI_ParseColor(c->value, out))
			return true;
		const kv_t *b = KV_FindPath(s->base, *ref);
		if (!b || !b->value)
			break;
		*ref = b->value;
	}
	return false;
}

bool VGUI_SchemeColor(const vgui_scheme_t *s, const char *ref, uint8_t out[4])
{
	for (int i = 0; i < 4 && s; i++, s = s->fallback)
		if (schemecolor(s, &ref, out))
			return true;
	return false;
}

const vgui_font_t *VGUI_SchemeFont(const vgui_scheme_t *s, const char *name)
{
	for (int i = 0; i < s->numfonts; i++)
		if (!Str_ICmp(s->fonts[i].name, name))
			return &s->fonts[i];
	return NULL;
}

const vgui_fontdef_t *VGUI_PickFontDef(const vgui_font_t *f, int screenh)
{
	if (!f || !f->numdefs)
		return NULL;
	for (int i = 0; i < f->numdefs; i++) {
		const vgui_fontdef_t *d = &f->defs[i];
		if ((!d->yres_min && !d->yres_max) || (screenh >= d->yres_min && screenh <= d->yres_max))
			return d;
	}
	/* no range holds the height: the nearest range (a design choice) */
	const vgui_fontdef_t *best = &f->defs[0];
	int bestgap = INT32_MAX;
	for (int i = 0; i < f->numdefs; i++) {
		const vgui_fontdef_t *d = &f->defs[i];
		int gap = screenh < d->yres_min ? d->yres_min - screenh : screenh - d->yres_max;
		if (gap < bestgap)
			bestgap = gap, best = d;
	}
	return best;
}

bool VGUI_ResolveCoord(const char *s, int parent, float scale, int *out)
{
	*out = 0;
	while (*s == ' ' || *s == '\t')
		s++;
	char mode = 0;
	if (*s == 'r' || *s == 'R' || *s == 'c' || *s == 'C')
		mode = (char)(*s++ | 32);
	char *end;
	long v = strtol(s, &end, 10);
	if (end == s)
		return false;
	while (*end == ' ' || *end == '\t')
		end++;
	if (*end)
		return false;
	if (v > 1000000 || v < -1000000)
		return false;
	int n = (int)(v * scale);
	if (mode == 'r')
		*out = parent - n;
	else if (mode == 'c')
		*out = parent / 2 + n;
	else
		*out = n;
	return true;
}

static int coord(vgui_panel_t *p, const char *key, int parent, float scale)
{
	const char *s = KV_GetString(p->kv, key, NULL);
	int v;
	if (!s)
		return 0;
	if (!VGUI_ResolveCoord(s, parent, scale, &v))
		p->badcoord = true;
	return v;
}

int VGUI_LayoutPanels(const kv_t *root, int pw, int ph, float scale, vgui_panel_t *out, int max)
{
	int n = 0;
	if (!root)
		return 0;
	for (const kv_t *kv = root->child; kv; kv = kv->next) {
		if (!KV_IsBlock(kv))
			continue;
		if (n < max) {
			vgui_panel_t *p = &out[n];
			memset(p, 0, sizeof(*p));
			p->kv = kv;
			p->name = KV_GetString(kv, "fieldName", kv->key);
			p->control = KV_GetString(kv, "ControlName", "");
			p->x = coord(p, "xpos", pw, scale);
			p->y = coord(p, "ypos", ph, scale);
			p->w = coord(p, "wide", pw, scale);
			p->h = coord(p, "tall", ph, scale);
			p->z = KV_GetInt(kv, "zpos", 0);
			p->visible = KV_GetInt(kv, "visible", 1) != 0;
			p->enabled = KV_GetInt(kv, "enabled", 1) != 0;
		}
		n++;
	}
	return n;
}
