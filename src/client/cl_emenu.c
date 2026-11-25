/* Bulwark engine - the engine's own menus, drawn over the game and any
 * menu QC: the menu_* pages (options, server browser, saves, demos, mods,
 * packages and the rest), and script menus that configs build with
 * conmenu, menutext, menubind and the other menu* commands.
 *
 * Pages stack: Escape or menupop goes back one, closemenu shuts them all.
 * Engine pages lay their rows out themselves; script menus place items on
 * a 320x200 screen scaled to fit, as Quake's menus were. A script menu's
 * text items run "<callback> <option>" when the menu has a callback, and
 * their own command when it doesn't. ISC licence, see LICENSE. */
#include "client.h"
#include "console.h"
#include "keys.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../common/manifest.h"
#include "../net/pkg.h"
#include "../render/render.h"
#include "../server/sv_local.h"
#include "../sound/sound.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern float in_mouse_x, in_mouse_y;

typedef enum { IT_TEXT, IT_COMMAND, IT_CHECK, IT_SLIDER, IT_COMBO, IT_EDIT, IT_BIND, IT_PIC, IT_BOX } itemtype_t;

typedef struct item_s {
	itemtype_t type;
	char *label;
	char *cvar;     /* check, slider, combo, edit */
	char *command;  /* command, bind, a script text's option */
	char *value;    /* menueditpriv's own text */
	float min, max, step;
	int bitmask;    /* check: the bits it sets, 0 for 0/1 */
	int numopts;
	char **captions, **values;
	float x, y, w, h; /* script menus: where; boxes: size in characters */
	bool placed, big, header;
	bool swatch;    /* draws a box of the colour its cvar holds */
	int rows;       /* rows tall on an engine page; 0 is 1 */
	void (*changed)(const char *value); /* after the menu sets the value */
	void (*draw)(const struct item_s *it, float x, float y, float w, float h, float size); /* draws the value part */
	float rx, ry, rw, rh; /* where it was last drawn, for the mouse */
} item_t;

typedef struct page_s {
	char name[32], title[64], footer[160];
	item_t *items;
	int numitems, cap;
	int cursor, scroll;
	bool script;
	char callback[128];
	void (*populate)(struct page_s *p); /* fills the items; again every refresh seconds if set */
	double refresh, nextrefresh;
	bool (*key)(struct page_s *p, int key); /* page keys before the usual ones */
} page_t;

#define MAX_DEPTH 16
static page_t *stack[MAX_DEPTH];
static int depth;
static bool editing;     /* an edit field has the keys */
static char editbuf[256];
static bool binding;     /* waiting for the key to bind */
static double binding_until; /* when the wait gives up */
#define BIND_WAIT 5 /* seconds; a pad has no spare key to cancel with */
static float lastmx = -1, lastmy = -1;

static const float c_text[3] = {0.85f, 0.85f, 0.85f}, c_dim[3] = {0.5f, 0.5f, 0.5f}, c_title[3] = {1, 0.75f, 0.35f};
static const float c_sel[3] = {1, 1, 1}, c_bar[3] = {0.25f, 0.45f, 0.8f}, c_panel[3] = {0.05f, 0.06f, 0.08f};
static const float c_head[3] = {0.6f, 0.75f, 1};

/* --- pages and items --------------------------------------------------------------------- */

static void free_items(page_t *p)
{
	for (int i = 0; i < p->numitems; i++) {
		item_t *it = &p->items[i];
		Mem_Free(it->label);
		Mem_Free(it->cvar);
		Mem_Free(it->command);
		Mem_Free(it->value);
		for (int k = 0; k < it->numopts; k++) {
			Mem_Free(it->captions[k]);
			Mem_Free(it->values[k]);
		}
		Mem_Free(it->captions);
		Mem_Free(it->values);
	}
	p->numitems = 0;
}

static item_t *add_item(page_t *p, itemtype_t type, const char *label)
{
	if (p->numitems == p->cap) {
		p->cap = p->cap ? p->cap * 2 : 16;
		p->items = Mem_Realloc(p->items, sizeof(item_t) * (size_t)p->cap);
	}
	item_t *it = &p->items[p->numitems++];
	memset(it, 0, sizeof(*it));
	it->type = type;
	it->label = Mem_Strdup(label ? label : "");
	return it;
}

static char *dup_or_null(const char *s) { return s ? Mem_Strdup(s) : NULL; }

static item_t *add_text(page_t *p, const char *label) { return add_item(p, IT_TEXT, label); }

static item_t *add_header(page_t *p, const char *label)
{
	item_t *it = add_item(p, IT_TEXT, label);
	it->header = true;
	return it;
}

static item_t *add_cmd(page_t *p, const char *label, const char *command)
{
	item_t *it = add_item(p, IT_COMMAND, label);
	it->command = dup_or_null(command);
	return it;
}

static item_t *add_check(page_t *p, const char *label, const char *cvar)
{
	item_t *it = add_item(p, IT_CHECK, label);
	it->cvar = Mem_Strdup(cvar);
	return it;
}

static item_t *add_slider(page_t *p, const char *label, const char *cvar, float min, float max, float step)
{
	item_t *it = add_item(p, IT_SLIDER, label);
	it->cvar = Mem_Strdup(cvar);
	it->min = min;
	it->max = max;
	it->step = step > 0 ? step : (max - min) / 20;
	return it;
}

static void add_option(item_t *it, const char *caption, const char *value)
{
	it->captions = Mem_Realloc(it->captions, sizeof(char *) * (size_t)(it->numopts + 1));
	it->values = Mem_Realloc(it->values, sizeof(char *) * (size_t)(it->numopts + 1));
	it->captions[it->numopts] = Mem_Strdup(caption);
	it->values[it->numopts] = Mem_Strdup(value);
	it->numopts++;
}

/* caption, value, caption, value, ..., NULL */
static item_t *add_combo(page_t *p, const char *label, const char *cvar, ...)
{
	va_list ap;
	item_t *it = add_item(p, IT_COMBO, label);
	it->cvar = Mem_Strdup(cvar);
	va_start(ap, cvar);
	for (const char *c; (c = va_arg(ap, const char *));) {
		const char *v = va_arg(ap, const char *);
		add_option(it, c, v ? v : c);
	}
	va_end(ap);
	return it;
}

static item_t *add_edit(page_t *p, const char *label, const char *cvar)
{
	item_t *it = add_item(p, IT_EDIT, label);
	it->cvar = Mem_Strdup(cvar);
	return it;
}

static item_t *add_bind(page_t *p, const char *label, const char *command)
{
	item_t *it = add_item(p, IT_BIND, label);
	it->command = Mem_Strdup(command);
	return it;
}

static bool selectable(const item_t *it)
{
	switch (it->type) {
	case IT_TEXT: case IT_PIC: case IT_BOX: return false;
	case IT_COMMAND: return it->command != NULL;
	default: return true;
	}
}

static page_t *top(void) { return depth ? stack[depth - 1] : NULL; }

static void fix_cursor(page_t *p, int dir)
{
	if (!p->numitems)
		return;
	if (p->cursor < 0)
		p->cursor = 0;
	if (p->cursor >= p->numitems)
		p->cursor = p->numitems - 1;
	for (int n = 0; n < p->numitems && !selectable(&p->items[p->cursor]); n++)
		p->cursor = (p->cursor + (dir < 0 ? p->numitems - 1 : 1)) % p->numitems;
}

static void free_page(page_t *p)
{
	free_items(p);
	Mem_Free(p->items);
	Mem_Free(p);
}

static page_t *push(const char *name, const char *title, void (*populate)(page_t *))
{
	if (depth == MAX_DEPTH) {
		free_page(stack[0]);
		memmove(stack, stack + 1, sizeof(stack[0]) * (MAX_DEPTH - 1));
		depth--;
	}
	page_t *p = Mem_Alloc(sizeof(*p));
	memset(p, 0, sizeof(*p));
	Str_Copy(p->name, name, sizeof(p->name));
	Str_Copy(p->title, title, sizeof(p->title));
	p->populate = populate;
	if (populate)
		populate(p);
	fix_cursor(p, 1);
	stack[depth++] = p;
	editing = binding = false;
	Con_SetActive(false);
	return p;
}

static void pop(void)
{
	if (!depth)
		return;
	free_page(stack[--depth]);
	editing = binding = false;
}

static void close_all(void)
{
	while (depth)
		pop();
}

/* rebuilds a page's items, keeping the cursor on the same row */
static void repopulate(page_t *p)
{
	int cursor = p->cursor, scroll = p->scroll;
	free_items(p);
	p->populate(p);
	p->cursor = cursor;
	p->scroll = scroll;
	fix_cursor(p, 1);
}

bool EM_Active(void) { return depth > 0; }

/* --- values ------------------------------------------------------------------------------ */

/* --- colours ------------------------------------------------------------------------------
 * Player colours (topcolor, bottomcolor) are FTE's 24-bit "0xRRGGBB"; old
 * configs may hold a Quake palette row, 0-13. Colours such as crosshaircolor
 * are "R G B" in 0-255. A colour row edits one through virtual cvars:
 * "@hex:<cvar>" is the colour as 0xRRGGBB (presets and typed values), and
 * "@hue:", "@sat:" and "@val:" are its hue (0-360), saturation and
 * brightness (0-100). Each is written back in the cvar's own format. */

/* the middle of each of Quake's 14 player colour rows */
static const unsigned int quake_rows[14] = {0x8b8b8b, 0x8f6f37, 0x8f8fd7, 0x6b6b0f, 0xc72323, 0xaf8f23, 0xd78f6b,
                                            0xcf87bf, 0xaf5fa3, 0xbfa787, 0x77af8f, 0xefe33b, 0x3f5fe3, 0xef8f0f};

static bool rgb_format(const char *cvar) { return !strcmp(cvar, "crosshaircolor"); }

/* parses a colour as typed or stored: 0xRRGGBB, #RRGGBB, RRGGBB, "R G B" or a palette row */
static bool parse_color(const char *s, unsigned int *out)
{
	unsigned int r, g, b, x;
	char tail;
	while (*s == ' ')
		s++;
	if ((sscanf(s, "0x%x%c", &x, &tail) == 1 || sscanf(s, "0X%x%c", &x, &tail) == 1 || sscanf(s, "#%x%c", &x, &tail) == 1)) {
		*out = x & 0xffffff;
		return true;
	}
	if (sscanf(s, "%u %u %u", &r, &g, &b) == 3) {
		*out = (BW_MIN(r, 255u) << 16) | (BW_MIN(g, 255u) << 8) | BW_MIN(b, 255u);
		return true;
	}
	if (strlen(s) == 6 && strspn(s, "0123456789abcdefABCDEF") == 6 && sscanf(s, "%x", &x) == 1) {
		*out = x;
		return true;
	}
	if (sscanf(s, "%u%c", &x, &tail) == 1) {
		*out = x < 14 ? quake_rows[x] : x & 0xffffff;
		return true;
	}
	return false;
}

static unsigned int cvar_color(const char *cvar)
{
	unsigned int c = 0xffffff;
	cvar_t *v = Cvar_Find(cvar);
	if (v)
		parse_color(v->string, &c);
	return c;
}

static void color_rgbf(unsigned int c, float *rgb)
{
	rgb[0] = ((c >> 16) & 255) / 255.0f;
	rgb[1] = ((c >> 8) & 255) / 255.0f;
	rgb[2] = (c & 255) / 255.0f;
}

static void rgb_to_hsv(unsigned int c, float *h, float *s, float *v)
{
	float rgb[3], mx, mn, d;
	color_rgbf(c, rgb);
	mx = BW_MAX(rgb[0], BW_MAX(rgb[1], rgb[2]));
	mn = BW_MIN(rgb[0], BW_MIN(rgb[1], rgb[2]));
	d = mx - mn;
	*v = mx * 100;
	*s = mx > 0 ? d / mx * 100 : 0;
	if (d <= 0)
		*h = 0;
	else if (mx == rgb[0])
		*h = 60 * fmodf((rgb[1] - rgb[2]) / d + 6, 6);
	else if (mx == rgb[1])
		*h = 60 * ((rgb[2] - rgb[0]) / d + 2);
	else
		*h = 60 * ((rgb[0] - rgb[1]) / d + 4);
}

static unsigned int hsv_to_rgb(float h, float s, float v)
{
	float c, x, m, r = 0, g = 0, b = 0;
	h = fmodf(BW_MAX(h, 0.0f), 360);
	s = BW_MAX(0.0f, BW_MIN(s, 100.0f)) / 100;
	v = BW_MAX(0.0f, BW_MIN(v, 100.0f)) / 100;
	c = v * s;
	x = c * (1 - fabsf(fmodf(h / 60, 2) - 1));
	m = v - c;
	switch ((int)(h / 60)) {
	case 0: r = c, g = x; break;
	case 1: r = x, g = c; break;
	case 2: g = c, b = x; break;
	case 3: g = x, b = c; break;
	case 4: r = x, b = c; break;
	default: r = c, b = x; break;
	}
	return ((unsigned int)lroundf((r + m) * 255) << 16) | ((unsigned int)lroundf((g + m) * 255) << 8) |
	       (unsigned int)lroundf((b + m) * 255);
}

/* the hue, saturation and value last set through the menu, per cvar: grey and
 * black have no hue of their own, so without this the hue slider would jump
 * back to red whenever saturation or brightness reached 0 */
static struct {
	char cvar[32];
	unsigned int color;
	float h, s, v;
} hsv_memory[4];

static void color_hsv(const char *cvar, float *h, float *s, float *v)
{
	unsigned int c = cvar_color(cvar);
	for (int i = 0; i < 4; i++)
		if (!strcmp(hsv_memory[i].cvar, cvar) && hsv_memory[i].color == c) {
			*h = hsv_memory[i].h, *s = hsv_memory[i].s, *v = hsv_memory[i].v;
			return;
		}
	rgb_to_hsv(c, h, s, v);
}

static void set_color(const char *cvar, unsigned int c);

static void set_color_hsv(const char *cvar, float h, float s, float v)
{
	int slot = 0;
	unsigned int c = hsv_to_rgb(h, s, v);
	for (int i = 0; i < 4; i++)
		if (!strcmp(hsv_memory[i].cvar, cvar) || !hsv_memory[i].cvar[0]) {
			slot = i;
			break;
		}
	Str_Copy(hsv_memory[slot].cvar, cvar, sizeof(hsv_memory[slot].cvar));
	hsv_memory[slot].color = c, hsv_memory[slot].h = h, hsv_memory[slot].s = s, hsv_memory[slot].v = v;
	set_color(cvar, c);
}

static void set_value(const char *cvar, const char *value);

static void set_color(const char *cvar, unsigned int c)
{
	char buf[32];
	if (rgb_format(cvar))
		snprintf(buf, sizeof(buf), "%u %u %u", (c >> 16) & 255, (c >> 8) & 255, c & 255);
	else
		snprintf(buf, sizeof(buf), "0x%06x", c & 0xffffff);
	set_value(cvar, buf);
}

/* "@resolution" stands for vid_width and vid_height together; "@hex:",
 * "@hue:", "@sat:" and "@val:" for a colour cvar, as above */
static const char *get_value(const char *cvar, char *buf, size_t size)
{
	if (!strcmp(cvar, "@resolution")) {
		snprintf(buf, size, "%dx%d", (int)Cvar_Value("vid_width"), (int)Cvar_Value("vid_height"));
		return buf;
	}
	if (!strncmp(cvar, "@hex:", 5)) {
		snprintf(buf, size, "0x%06x", cvar_color(cvar + 5));
		return buf;
	}
	if (!strncmp(cvar, "@hue:", 5) || !strncmp(cvar, "@sat:", 5) || !strncmp(cvar, "@val:", 5)) {
		float h, s, v;
		color_hsv(cvar + 5, &h, &s, &v);
		snprintf(buf, size, "%d", (int)lroundf(cvar[1] == 'h' ? h : cvar[1] == 's' ? s : v));
		return buf;
	}
	cvar_t *v = Cvar_Find(cvar);
	return v ? v->string : "";
}

static void set_value(const char *cvar, const char *value)
{
	if (!strcmp(cvar, "@resolution")) {
		int w = 0, h = 0;
		if (sscanf(value, "%dx%d", &w, &h) == 2) {
			Cvar_SetValue("vid_width", (float)w);
			Cvar_SetValue("vid_height", (float)h);
		}
		return;
	}
	if (!strncmp(cvar, "@hex:", 5)) {
		unsigned int c;
		if (parse_color(value, &c))
			set_color(cvar + 5, c);
		return;
	}
	if (!strncmp(cvar, "@hue:", 5) || !strncmp(cvar, "@sat:", 5) || !strncmp(cvar, "@val:", 5)) {
		float hsv[3], f = (float)atof(value);
		color_hsv(cvar + 5, &hsv[0], &hsv[1], &hsv[2]);
		hsv[cvar[1] == 'h' ? 0 : cvar[1] == 's' ? 1 : 2] = f;
		set_color_hsv(cvar + 5, hsv[0], hsv[1], hsv[2]);
		return;
	}
	cvar_t *v = Cvar_Find(cvar);
	if (v) {
		Cvar_Set(cvar, value);
		/* a game's own setting (made by "set" in its default configs) is the
		 * player's once the menu changes it, and is saved like FTE's seta */
		if ((v->flags & CVAR_USERCREATED) && !(v->flags & (CVAR_ARCHIVE | CVAR_NOSAVE))) {
			v->flags |= CVAR_ARCHIVE;
			cfg_modified++;
		}
	} else
		Cbuf_AddText(va("seta \"%s\" \"%s\"\n", cvar, value));
}

static void set_item_value(item_t *it, const char *value)
{
	set_value(it->cvar, value);
	if (it->changed)
		it->changed(value);
}

static int combo_index(const item_t *it)
{
	char buf[64];
	const char *v = get_value(it->cvar, buf, sizeof(buf));
	for (int i = 0; i < it->numopts; i++)
		if (!Str_ICmp(v, it->values[i]) || (atof(v) == atof(it->values[i]) && (isdigit((unsigned char)*v) || *v == '-')))
			return i;
	return -1;
}

static void run(page_t *p, const char *command)
{
	if (!strcmp(command, "@startserver")) { /* menu_newmulti's settings */
		int dm = (int)Cvar_Value("deathmatch");
		Cbuf_AddText(va("closemenu; set coop %d; map \"%s\"\n", dm ? 0 : 1, Cvar_String("_menu_map")));
		return;
	}
	if (p->script && p->callback[0])
		Cbuf_AddText(va("%s %s\n", p->callback, command));
	else
		Cbuf_AddText(va("%s\n", command));
}

/* the keys bound to a command, as "K, MOUSE1", or "" */
static void keys_for(const char *command, char *out, size_t size)
{
	out[0] = 0;
	for (int k = 0; k < K_MAX; k++) {
		const char *b = Key_GetBinding(k);
		if (b && *b && !Str_ICmp(b, command)) {
			size_t n = strlen(out);
			snprintf(out + n, size - n, "%s%s", n ? ", " : "", Key_NameForNum(k));
		}
	}
}

static void activate(page_t *p, item_t *it, int dir)
{
	char buf[256];
	switch (it->type) {
	case IT_COMMAND:
		if (dir == 0 && it->command)
			run(p, it->command);
		break;
	case IT_CHECK: {
		int v = (int)atof(get_value(it->cvar, buf, sizeof(buf)));
		if (it->bitmask)
			v ^= it->bitmask;
		else
			v = !v;
		set_item_value(it, va("%d", v));
		break;
	}
	case IT_SLIDER: {
		float v = (float)atof(get_value(it->cvar, buf, sizeof(buf)));
		v += (dir ? dir : 1) * it->step * (it->min > it->max ? -1 : 1);
		float lo = BW_MIN(it->min, it->max), hi = BW_MAX(it->min, it->max);
		if (dir == 0 && v > hi + it->step * 0.5f)
			v = lo; /* enter wraps round */
		v = BW_MAX(lo, BW_MIN(v, hi));
		snprintf(buf, sizeof(buf), "%g", roundf(v / it->step) * it->step);
		set_item_value(it, buf);
		break;
	}
	case IT_COMBO: {
		if (!it->numopts)
			break;
		int i = combo_index(it);
		i = i < 0 ? 0 : (i + (dir < 0 ? it->numopts - 1 : 1)) % it->numopts;
		set_item_value(it, it->values[i]);
		break;
	}
	case IT_EDIT:
		if (dir == 0) {
			editing = true;
			Str_Copy(editbuf, it->cvar ? get_value(it->cvar, buf, sizeof(buf)) : (it->value ? it->value : ""), sizeof(editbuf));
		}
		break;
	case IT_BIND:
		if (dir == 0) {
			binding = true;
			binding_until = Sys_Seconds() + BIND_WAIT;
		}
		break;
	default:
		break;
	}
}

static void finish_edit(page_t *p, item_t *it)
{
	editing = false;
	if (it->cvar)
		set_item_value(it, editbuf);
	else {
		Mem_Free(it->value);
		it->value = Mem_Strdup(editbuf);
		/* a private field tells the script what was typed */
		if (p->script && p->callback[0])
			Cbuf_AddText(va("%s \"%s\" \"%s\"\n", p->callback, it->label, editbuf));
	}
}

/* --- drawing -------------------------------------------------------------------------- */

static void value_text(const item_t *it, char *out, size_t size)
{
	char buf[256];
	out[0] = 0;
	switch (it->type) {
	case IT_CHECK: {
		int v = (int)atof(get_value(it->cvar, buf, sizeof(buf)));
		bool on = it->bitmask ? (v & it->bitmask) == it->bitmask : v != 0;
		Str_Copy(out, on ? "On" : "Off", size);
		break;
	}
	case IT_COMBO: {
		int i = combo_index(it);
		Str_Copy(out, i >= 0 ? it->captions[i] : get_value(it->cvar, buf, sizeof(buf)), size);
		break;
	}
	case IT_EDIT:
		Str_Copy(out, it->cvar ? get_value(it->cvar, buf, sizeof(buf)) : (it->value ? it->value : ""), size);
		break;
	case IT_BIND:
		keys_for(it->command, out, size);
		if (!out[0])
			Str_Copy(out, "not bound", size);
		break;
	case IT_SLIDER:
		snprintf(out, size, "%g", atof(get_value(it->cvar, buf, sizeof(buf))));
		break;
	default:
		break;
	}
}

static void draw_slider(const item_t *it, float x, float y, float w, float size, bool sel)
{
	char buf[64];
	float v = (float)atof(get_value(it->cvar, buf, sizeof(buf)));
	float f = it->max != it->min ? (v - it->min) / (it->max - it->min) : 0;
	f = BW_MAX(0.0f, BW_MIN(f, 1.0f));
	R_Rect(x, y + size * 0.3f, w, size * 0.4f, c_dim, 0.6f);
	R_Rect(x, y + size * 0.3f, w * f, size * 0.4f, sel ? c_sel : c_bar, 0.9f);
	R_Rect(x + w * f - 1, y, 3, size, sel ? c_sel : c_text, 1);
	snprintf(buf, sizeof(buf), "%g", v);
	R_Text(x + w + size, y, buf, size, sel ? c_sel : c_text, 1);
}

/* a framed box of a colour */
static void draw_swatch(float x, float y, float w, float h, unsigned int color)
{
	float rgb[3];
	color_rgbf(color, rgb);
	R_Rect(x, y, w, h, c_dim, 1);
	R_Rect(x + 1, y + 1, w - 2, h - 2, rgb, 1);
}

/* the value part of a row, from x */
static void draw_value(const item_t *it, float x, float y, float w, float size, bool sel)
{
	char buf[256];
	const float *col = sel ? c_sel : c_text;
	if (it->draw) {
		it->draw(it, x, y, w, size * 1.5f * BW_MAX(it->rows, 1) - size * 0.5f, size);
		return;
	}
	if (it->swatch && it->cvar) {
		const char *c = !strncmp(it->cvar, "@", 1) && strchr(it->cvar, ':') ? strchr(it->cvar, ':') + 1 : it->cvar;
		draw_swatch(x, y - 1, size * 2.5f, size + 2, cvar_color(c));
		x += size * 3;
		w -= size * 3;
	}
	if (it->type == IT_SLIDER) {
		draw_slider(it, x, y, BW_MIN(w * 0.6f, size * 14), size, sel);
		return;
	}
	if (it->type == IT_EDIT) {
		R_Rect(x - 2, y - 1, BW_MIN(w, size * 24), size + 2, c_dim, 0.35f);
		if (sel && editing) {
			float tw = R_Text(x, y, editbuf, size, c_sel, 1);
			if ((int)(Sys_Seconds() * 3) & 1)
				R_Rect(x + tw, y, size * 0.5f, size, c_sel, 1);
			return;
		}
	}
	if (it->type == IT_BIND && sel && binding) {
		int left = (int)(binding_until - Sys_Seconds()) + 1;
		snprintf(buf, sizeof(buf), "press a key or button... %d", left < 1 ? 1 : left);
		R_Text(x, y, buf, size, c_title, 1);
		return;
	}
	value_text(it, buf, sizeof(buf));
	R_Text(x, y, buf, size, col, 1);
}

static void draw_box(float x, float y, float w, float h, float size)
{
	R_Rect(x, y, w * size, h * size, c_panel, 0.85f);
	R_Rect(x, y, w * size, 1, c_dim, 1);
	R_Rect(x, y + h * size - 1, w * size, 1, c_dim, 1);
	R_Rect(x, y, 1, h * size, c_dim, 1);
	R_Rect(x + w * size - 1, y, 1, h * size, c_dim, 1);
}

static void draw_script(page_t *p, int sw, int sh)
{
	float scale = BW_MIN(sw / 320.0f, sh / 200.0f);
	float ox = (sw - 320 * scale) / 2, oy = (sh - 200 * scale) / 2, size = 8 * scale;
	for (int i = 0; i < p->numitems; i++) {
		item_t *it = &p->items[i];
		float x = ox + it->x * scale, y = oy + it->y * scale, s = it->big ? size * 2 : size;
		bool sel = i == p->cursor && selectable(it);
		if (it->type == IT_PIC) {
			int w, h;
			if (R_PictureSize(it->label, &w, &h))
				R_Picture(x, y, w * scale, h * scale, it->label, 1);
			continue;
		}
		if (it->type == IT_BOX) {
			draw_box(x, y, it->w, it->h, size);
			continue;
		}
		if (sel)
			R_Text(x - size * 1.5f, y, ">", s, c_title, (int)(Sys_Seconds() * 4) & 1 ? 1 : 0.4f);
		float lw = R_Text(x, y, it->label, s, sel ? c_sel : c_text, 1);
		it->rx = x, it->ry = y, it->rw = BW_MAX(lw, size * 12), it->rh = s;
		if (it->type != IT_TEXT && it->type != IT_COMMAND) {
			draw_value(it, x + lw + size, y, sw - (x + lw + size), s, sel);
			it->rw = sw - x;
		}
	}
}

static void draw_page(page_t *p, int sw, int sh)
{
	/* about 40 rows on a 720 line screen, larger on bigger ones */
	const float size = BW_MAX(10.0f, floorf(sh / 56.0f)), row = size * 1.5f;
	float pw = BW_MIN(sw - 32.0f, size * 60), ph = BW_MIN(sh - 32.0f, size * 46);
	float px = (sw - pw) / 2, py = (sh - ph) / 2;
	int visible = (int)((ph - size * 6) / row);
	R_Rect(0, 0, (float)sw, (float)sh, c_panel, 0.45f);
	R_Rect(px, py, pw, ph, c_panel, 0.92f);
	R_Rect(px, py, pw, 2, c_bar, 1);
	R_Text(px + size, py + size, p->title, size * 2, c_title, 1);
	/* keep the cursor in view */
	if (p->cursor < p->scroll)
		p->scroll = p->cursor;
	if (p->cursor >= p->scroll + visible)
		p->scroll = p->cursor - visible + 1;
	p->scroll = BW_MAX(0, BW_MIN(p->scroll, p->numitems - visible));
	float y = py + size * 4, valx = px + pw * 0.5f, bottom = py + ph - size * 2.5f;
	for (int i = 0; i < p->numitems; i++)
		p->items[i].rw = 0;
	for (int i = p->scroll; i < p->numitems && i < p->scroll + visible; y += row * BW_MAX(p->items[i].rows, 1), i++) {
		item_t *it = &p->items[i];
		bool sel = i == p->cursor && selectable(it);
		float rh = row * BW_MAX(it->rows, 1);
		if (y + rh > bottom && i > p->scroll)
			break; /* tall rows leave less room */
		it->rx = px + 4, it->ry = y - 2, it->rw = pw - 8, it->rh = rh;
		if (sel)
			R_Rect(px + 4, y - 2, pw - 8, rh, c_bar, 0.5f);
		if (it->header && !strchr(it->label, '\t')) {
			R_Text(px + size, y, it->label, size, c_head, 1);
			continue;
		}
		const float *col = it->header ? c_head : !selectable(it) && it->type != IT_TEXT ? c_dim : sel ? c_sel : c_text;
		if (strchr(it->label, '\t')) {
			/* columns: split at tabs */
			static const float at[] = {0, 0.5f, 0.75f, 0.88f, 0.95f};
			char cell[256];
			const char *c = it->label;
			for (int k = 0; *c && k < 5; k++) {
				size_t n = strcspn(c, "\t");
				Str_Copy(cell, c, BW_MIN(sizeof(cell), n + 1));
				R_Text(px + size * 1.5f + (pw - size * 3) * at[k], y, cell, size, col, 1);
				c += n + (c[n] == '\t');
			}
		} else
			R_Text(px + size * 1.5f, y, it->label, size, col, 1);
		if ((it->type != IT_TEXT && it->type != IT_COMMAND) || it->draw)
			draw_value(it, valx, y, px + pw - valx - 12, size, sel);
	}
	if (p->scroll > 0)
		R_Text(px + pw - size * 2, py + size * 4, "^", size, c_dim, 1);
	if (p->scroll + visible < p->numitems)
		R_Text(px + pw - size * 2, py + ph - size * 3.5f, "v", size, c_dim, 1);
	if (p->footer[0])
		R_Text(px + size, py + ph - size * 2, p->footer, size, c_dim, 1);
}

void EM_Draw(int w, int h)
{
	page_t *p = top();
	if (!p)
		return;
	if (binding && Sys_Seconds() >= binding_until)
		binding = false; /* nothing pressed: leave the binding as it was */
	if (p->populate && p->refresh > 0 && Sys_Seconds() >= p->nextrefresh) {
		p->nextrefresh = Sys_Seconds() + p->refresh;
		if (!editing && !binding)
			repopulate(p);
	}
	/* the pointer picks the row under it when it moves */
	float scale = R_Get2DScale(), mx = in_mouse_x / scale, my = in_mouse_y / scale;
	if (mx != lastmx || my != lastmy) {
		lastmx = mx, lastmy = my;
		for (int i = 0; i < p->numitems && !editing && !binding; i++) {
			const item_t *it = &p->items[i];
			if (it->rw > 0 && selectable(it) && mx >= it->rx && mx < it->rx + it->rw && my >= it->ry && my < it->ry + it->rh)
				p->cursor = i;
		}
	}
	if (p->script)
		draw_script(p, w, h);
	else
		draw_page(p, w, h);
}

/* --- keys ---------------------------------------------------------------------------------- */

static void move_cursor(page_t *p, int dir)
{
	if (!p->numitems)
		return;
	for (int n = 0; n < p->numitems; n++) {
		p->cursor = (p->cursor + dir + p->numitems) % p->numitems;
		if (selectable(&p->items[p->cursor]))
			return;
	}
}

static bool edit_key(page_t *p, item_t *it, int key, int unicode)
{
	size_t n = strlen(editbuf);
	/* a controller can't type, but it must be able to leave the row:
	 * A keeps the text like Enter, B or Start drops it like Escape */
	if (key == K_ENTER || key == K_KP_ENTER || key == K_GP_A)
		finish_edit(p, it);
	else if (key == K_ESCAPE || key == K_GP_B || key == K_GP_MENU)
		editing = false;
	else if (key == K_BACKSPACE) {
		if (n)
			editbuf[n - 1] = 0;
	} else if (unicode >= 32 && unicode < 127 && n + 1 < sizeof(editbuf)) {
		editbuf[n] = (char)unicode;
		editbuf[n + 1] = 0;
	}
	return true;
}

bool EM_KeyEvent(int key, bool down, int unicode)
{
	page_t *p = top();
	if (!p)
		return false;
	if (!down)
		return true;
	item_t *it = p->numitems ? &p->items[p->cursor] : NULL;
	if (binding && it && it->type == IT_BIND) {
		binding = false;
		/* Escape cancels; any other key or button, every pad button
		 * included, becomes the binding. A controller cancels by waiting
		 * BIND_WAIT seconds, since each of its buttons may need binding */
		if (key != K_ESCAPE)
			Key_SetBinding(key, it->command);
		return true;
	}
	if (editing && it)
		return edit_key(p, it, key, unicode);
	if (p->key && p->key(p, key))
		return true;
	switch (key) {
	case K_ESCAPE:
	case K_MOUSE2:
	case K_GP_B:
	case K_GP_MENU:
		pop();
		break;
	case K_UPARROW: case K_KP_UPARROW: case K_MWHEELUP: case K_GP_DPAD_UP:
		move_cursor(p, -1);
		break;
	case K_DOWNARROW: case K_KP_DOWNARROW: case K_MWHEELDOWN: case K_GP_DPAD_DOWN:
		move_cursor(p, 1);
		break;
	case K_PGUP: case K_GP_LSHOULDER:
		for (int i = 0; i < 10; i++)
			move_cursor(p, -1);
		break;
	case K_PGDN: case K_GP_RSHOULDER:
		for (int i = 0; i < 10; i++)
			move_cursor(p, 1);
		break;
	case K_HOME:
		p->cursor = 0;
		fix_cursor(p, 1);
		break;
	case K_END:
		p->cursor = p->numitems - 1;
		fix_cursor(p, -1);
		break;
	case K_LEFTARROW: case K_KP_LEFTARROW: case K_GP_DPAD_LEFT:
		if (it && selectable(it))
			activate(p, it, -1);
		break;
	case K_RIGHTARROW: case K_KP_RIGHTARROW: case K_GP_DPAD_RIGHT:
		if (it && selectable(it))
			activate(p, it, 1);
		break;
	case K_ENTER: case K_KP_ENTER: case K_MOUSE1: case K_GP_A:
		if (key == K_MOUSE1) {
			float scale = R_Get2DScale(), mx = in_mouse_x / scale, my = in_mouse_y / scale;
			if (!it || !(mx >= it->rx && mx < it->rx + it->rw && my >= it->ry && my < it->ry + it->rh))
				break; /* a click off the rows does nothing */
		}
		if (it && selectable(it))
			activate(p, it, 0);
		break;
	case K_BACKSPACE: case K_DEL:
		if (it && it->type == IT_BIND)
			for (int k = 0; k < K_MAX; k++) {
				const char *b = Key_GetBinding(k);
				if (b && *b && !Str_ICmp(b, it->command))
					Key_SetBinding(k, "");
			}
		break;
	default:
		break;
	}
	return true;
}

/* --- the pages ------------------------------------------------------------------------------ */

static void open_page(const char *name);

static void pg_main(page_t *p)
{
	add_cmd(p, "Single player", "menu_single");
	add_cmd(p, "Multiplayer", "menu_multi");
	add_cmd(p, "Options", "menu_options");
	add_cmd(p, "Demos", "menu_demo");
	add_cmd(p, "Mods", "menu_mods");
	add_cmd(p, "Packages and updates", "menu_download");
	add_cmd(p, "Media player", "menu_media");
	add_cmd(p, "Credits", "menu_credits");
	add_cmd(p, "Quit", "menu_quit");
}

static void pg_single(page_t *p)
{
	add_cmd(p, "New game", "closemenu; set deathmatch 0; set coop 0; maxplayers 1; map start");
	add_combo(p, "Skill", "skill", "Easy", "0", "Normal", "1", "Hard", "2", "Nightmare", "3", NULL);
	add_cmd(p, "Load a game", "menu_load");
	add_cmd(p, sv.active ? "Save the game" : "Save the game (not playing)", sv.active ? "menu_save" : NULL);
	add_cmd(p, "Cheats", "menu_spcheats");
}

typedef struct {
	page_t *page;
	const char *command; /* "load" or "save" */
} savelist_t;

static void list_save(const char *name, const char *searchpath, void *ctx)
{
	savelist_t *l = ctx;
	char base[128], label[160];
	const char *slash = strrchr(name, '/');
	Str_Copy(base, slash ? slash + 1 : name, sizeof(base));
	if (strlen(base) > 4)
		base[strlen(base) - 4] = 0;
	snprintf(label, sizeof(label), "%s", base);
	add_cmd(l->page, label, va("closemenu; %s \"%s\"", l->command, base));
}

static void pg_load(page_t *p)
{
	savelist_t l = {p, "load"};
	FS_List("saves/*.sav", true, list_save, &l);
	if (!p->numitems)
		add_text(p, "No saved games");
	Str_Copy(p->footer, "Enter loads", sizeof(p->footer));
}

static void pg_save(page_t *p)
{
	for (int i = 0; i < 12; i++) {
		char name[16], rel[64];
		snprintf(name, sizeof(name), "s%d", i);
		snprintf(rel, sizeof(rel), "saves/%s.sav", name);
		add_cmd(p, va("Slot %d%s", i + 1, FS_FileExists(rel) ? " (in use)" : " (empty)"), va("closemenu; save %s", name));
	}
	Str_Copy(p->footer, "Enter saves over the slot", sizeof(p->footer));
}

static void pg_spcheats(page_t *p)
{
	add_cmd(p, "God mode", "god");
	add_cmd(p, "No clipping", "noclip");
	add_cmd(p, "Fly", "fly");
	add_cmd(p, "Not a target", "notarget");
	add_cmd(p, "Give everything", "give all");
	add_cmd(p, "Kill yourself", "closemenu; kill");
	add_cmd(p, "Restart the map", "closemenu; restart");
}

static void pg_multi(page_t *p)
{
	add_cmd(p, "Join a server", "menu_servers");
	add_cmd(p, "Start a server", "menu_newmulti");
	add_cmd(p, "Player setup", "menu_setup");
	add_cmd(p, "Network settings", "menu_network");
	add_cmd(p, "Teamplay messages", "menu_teamplay");
	add_cmd(p, "Demos", "menu_demo");
}

static void pg_servers(page_t *p)
{
	char name[64], map[64], players[16], max[16], ping[16], addr[128];
	int n = CL_HostcacheCount();
	add_header(p, "Server\tMap\tPlayers\tPing");
	for (int i = 0; i < n; i++) {
		CL_HostcacheField(i, "name", name, sizeof(name));
		CL_HostcacheField(i, "map", map, sizeof(map));
		CL_HostcacheField(i, "numplayers", players, sizeof(players));
		CL_HostcacheField(i, "maxplayers", max, sizeof(max));
		CL_HostcacheField(i, "ping", ping, sizeof(ping));
		CL_HostcacheField(i, "cname", addr, sizeof(addr));
		add_cmd(p, va("%.40s\t%.20s\t%s/%s\t%s", name[0] ? name : addr, map, players, max, ping),
		        va("closemenu; connect \"%s\"", addr));
	}
	if (!n)
		add_text(p, "Looking for servers...");
	snprintf(p->footer, sizeof(p->footer), "%d servers. Enter joins, F5 asks again, S sorts", n);
}

static bool servers_key(page_t *p, int key)
{
	static int sort;
	static const char *const sorts[] = {"ping", "name", "map", "numplayers"};
	if (key == K_F5) {
		CL_HostcacheRefresh(true);
		repopulate(p);
		return true;
	}
	if (key == 's' || key == 'S') {
		sort = (sort + 1) % 4;
		CL_HostcacheSort(sorts[sort], sort == 3);
		repopulate(p);
		return true;
	}
	return false;
}

static void list_map(const char *name, const char *searchpath, void *ctx)
{
	item_t *it = ctx;
	char base[128];
	const char *slash = strrchr(name, '/');
	Str_Copy(base, slash ? slash + 1 : name, sizeof(base));
	if (strlen(base) > 4)
		base[strlen(base) - 4] = 0;
	for (int i = 0; i < it->numopts; i++)
		if (!strcmp(it->values[i], base))
			return;
	if (it->numopts < 512)
		add_option(it, base, base);
}

static void pg_newmulti(page_t *p)
{
	Cvar_Get("_menu_map", "start", 0, "The map menu_newmulti starts");
	add_edit(p, "Server name", "hostname");
	add_combo(p, "Players", "maxplayers", "2", "2", "4", "4", "8", "8", "16", "16", "32", "32", "64", "64", NULL);
	add_combo(p, "Game", "deathmatch", "Cooperative", "0", "Deathmatch", "1", "Deathmatch 2", "2", "Deathmatch 3", "3", NULL);
	add_check(p, "Teamplay", "teamplay");
	add_edit(p, "Frag limit", "fraglimit");
	add_edit(p, "Time limit", "timelimit");
	add_check(p, "Public (tell the masters)", "sv_public");
	item_t *maps = add_combo(p, "Map", "_menu_map", NULL);
	FS_List("maps/*.bsp", true, list_map, maps);
	add_cmd(p, "Start", "@startserver");
}

/* --- player setup: name, model, colours and spray ------------------------------------------- */

/* a colour cvar's rows: the value (typed, with a swatch), presets, then hue,
 * saturation and brightness */
static void color_rows(page_t *p, const char *cvar)
{
	item_t *it = add_edit(p, "Colour", va("@hex:%s", cvar));
	it->swatch = true;
	it = add_combo(p, "Preset", va("@hex:%s", cvar), "Red", "0xff2020", "Orange", "0xff8000", "Yellow", "0xffe000",
	               "Green", "0x20d020", "Teal", "0x00c0b0", "Blue", "0x2060ff", "Purple", "0x9040ff", "Pink", "0xff40c0",
	               "White", "0xffffff", "Grey", "0x909090", "Black", "0x000000", NULL);
	it->swatch = true;
	add_slider(p, "Hue", va("@hue:%s", cvar), 0, 350, 10);
	add_slider(p, "Saturation", va("@sat:%s", cvar), 0, 100, 5);
	add_slider(p, "Brightness", va("@val:%s", cvar), 0, 100, 5);
}

/* the tint the game draws a colour as: scaled to full brightness, then mixed
 * with white by cg_playerColorTint (see the game's Player_ColorTint) */
static unsigned int hero_tint(unsigned int c)
{
	float rgb[3], peak, k = BW_MAX(0.0f, BW_MIN(Cvar_Value("cg_playerColorTint"), 1.0f));
	unsigned int out = 0;
	color_rgbf(c, rgb);
	peak = BW_MAX(rgb[0], BW_MAX(rgb[1], rgb[2]));
	for (int i = 0; i < 3; i++) {
		float f = peak > 0 ? 1 + (rgb[i] / peak - 1) * k : 1;
		out = (out << 8) | (unsigned int)lroundf(f * 255);
	}
	return out;
}

static void draw_color_preview(const item_t *it, float x, float y, float w, float h, float size)
{
	float bw = size * 5;
	h -= size * 1.2f; /* room for the captions */
	draw_swatch(x, y, bw, h, cvar_color("topcolor"));
	draw_swatch(x + bw + size, y, bw, h, cvar_color("bottomcolor"));
	draw_swatch(x + (bw + size) * 2.5f, y, bw, h, hero_tint(cvar_color("topcolor")));
	draw_swatch(x + (bw + size) * 3.5f, y, bw, h, hero_tint(cvar_color("bottomcolor")));
	R_Text(x, y + h + 2, "Upper", size * 0.8f, c_dim, 1);
	R_Text(x + bw + size, y + h + 2, "Lower", size * 0.8f, c_dim, 1);
	R_Text(x + (bw + size) * 2.5f, y + h + 2, "As the game tints them", size * 0.8f, c_dim, 1);
}

static void draw_spray_preview(const item_t *it, float x, float y, float w, float h, float size)
{
	const char *spray = Cvar_String("_cl_playerspray");
	R_Rect(x, y, h, h, c_dim, 0.35f);
	if (!spray[0] || !R_Picture(x + 2, y + 2, h - 4, h - 4, spray, 1))
		R_Text(x + 4, y + h * 0.5f - size * 0.5f, "no preview", size * 0.8f, c_dim, 1);
}

static void list_player_model(const char *name, const char *searchpath, void *ctx)
{
	item_t *it = ctx;
	char dir[64], file[80];
	/* models/player/<name>/<name>.mdl */
	if (sscanf(name, "models/player/%63[^/]/%79s", dir, file) != 2 || Str_ICmp(file, va("%s.mdl", dir)))
		return;
	for (int i = 0; i < it->numopts; i++)
		if (!Str_ICmp(it->values[i], dir))
			return;
	add_option(it, dir, dir);
}

#define SPRAY_MAX_BYTES 7680 /* leaves room for the other user info keys */

static void list_spray(const char *name, const char *searchpath, void *ctx)
{
	/* only what R_Picture (and so the game's spray decal) can decode: no .webp */
	static const char *const exts[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp"};
	item_t *it = ctx;
	const char *dot = strrchr(name, '.');
	size_t size = 0;
	bool image = false;
	for (size_t i = 0; dot && i < BW_ARRAY_COUNT(exts); i++)
		image |= !Str_ICmp(dot, exts[i]);
	if (!image)
		return;
	for (int i = 0; i < it->numopts; i++)
		if (!Str_ICmp(it->values[i], name))
			return;
	void *d = FS_LoadFile(name, &size);
	Mem_Free(d);
	/* the blob counts towards the server's user info limit: FTE's default
	 * sv_userinfo_bytelimit is 8192 bytes for every key together, so a
	 * larger image would be refused there and nobody would see the spray */
	if (size > SPRAY_MAX_BYTES)
		return;
	add_option(it, name + 6, name); /* past "logos/" */
}

static void model_changed(const char *value) { Cbuf_AddText(va("setinfo model \"%s\"\n", value)); }

/* the spray goes to the server as the "spray" user info blob, as Nuclide's
 * own menus send it: bmp images are monochrome (spraytype 1) */
static void spray_changed(const char *value)
{
	const char *dot = strrchr(value, '.');
	Cbuf_AddText(va("setinfoblob spray \"%s\"\nsetinfo spraytype %d\n", value, dot && !Str_ICmp(dot, ".bmp") ? 1 : 0));
	/* spraycolor is a user info key: setinfo keeps those as CVAR_USERINFO
	 * cvars, which is also what the game's getlocaluserinfo reads */
	const cvar_t *sc = Cvar_Find("spraycolor");
	if (!sc || !(sc->flags & CVAR_USERINFO) || !sc->string[0])
		Cbuf_AddText("setinfo spraycolor \"1 1 1\"\n");
}

static void pg_setup(page_t *p)
{
	item_t *it;
	/* cg_heroModel, not _cl_playermodel: older Stiletto builds archived
	 * _cl_playermodel as alice2 in every config */
	Cvar_Get("cg_heroModel", "", CVAR_ARCHIVE, "The player model choice (models/player/<name>/), sent as the model user info key");
	Cvar_Get("_cl_playerspray", "", CVAR_ARCHIVE, "The spray image (under logos/), sent as the spray user info blob");
	Cvar_Get("cg_playerColorTint", "0.5", CVAR_ARCHIVE, "How strongly the game tints heroes with player colours, 0-1");
	add_edit(p, "Name", "name");
	add_edit(p, "Team", "team");
	it = add_combo(p, "Model", "cg_heroModel", "Each hero's own", "", NULL);
	FS_List("models/player/*/*.mdl", true, list_player_model, it);
	it->changed = model_changed;
	add_header(p, "Upper colour: tints the hero (team modes use the team's)");
	color_rows(p, "topcolor");
	add_header(p, "Lower colour: tints what they carry");
	color_rows(p, "bottomcolor");
	add_slider(p, "Tint strength", "cg_playerColorTint", 0, 1, 0.1f);
	it = add_text(p, "Preview");
	it->draw = draw_color_preview;
	it->rows = 3;
	add_header(p, "Spray (impulse 201): png/jpg/tga/bmp, 7.5 KB at most");
	it = add_combo(p, "Image", "_cl_playerspray", NULL);
	FS_List("logos/*", true, list_spray, it);
	it->changed = spray_changed;
	if (!it->numopts)
		add_text(p, "No images in logos/");
	it = add_text(p, "Spray preview");
	it->draw = draw_spray_preview;
	it->rows = 4;
	add_combo(p, "Connection", "rate", "Modem", "5000", "ISDN", "10000", "DSL", "25000", "Fast", "50000", "LAN", "100000", NULL);
	Str_Copy(p->footer, "Left/Right change. Colours take 0xRRGGBB, #RRGGBB or R G B", sizeof(p->footer));
}

/* --- crosshair, weapon view and HUD: the game's display settings ---------------------------- */

static void pg_view(page_t *p)
{
	add_header(p, "Crosshair");
	add_combo(p, "Shape", "crosshair", "Off", "0", "Plus", "1", "Open plus", "2", "Dot", "3", "Open plus and dot", "4",
	          "Ring", "5", "Corners", "6", NULL);
	add_slider(p, "Size", "crosshairsize", 2, 32, 1);
	add_slider(p, "Opacity", "crosshairalpha", 0, 1, 0.05f);
	color_rows(p, "crosshaircolor");
	/* the game registers cg_viewmodelFov, Flip, Offset and Pass too, but its
	 * client never reads them (Flip is switched off in its source): the
	 * weapon's field of view is the engine's r_viewmodel_fov */
	add_header(p, "Weapon view");
	add_slider(p, "Weapon field of view", "r_viewmodel_fov", 50, 120, 5);
	add_slider(p, "Weapon size", "cg_viewmodelScale", 0.5f, 2, 0.1f);
	add_slider(p, "Weapon bob and sway", "cg_viewmodelMotionScale", 0, 2, 0.1f);
	add_combo(p, "Weapon lag", "cg_viewmodelLag", "Off", "0", "On", "1", "Smooth", "2", NULL);
	add_header(p, "HUD and screen");
	add_combo(p, "HUD shape", "cg_hudAspect", "Fill the screen", "0", "4:3", "1.34", "16:10", "1.6", "16:9", "1.78", NULL);
	add_check(p, "Shake the view when hurt", "cg_damageShake");
	add_slider(p, "Player colour tint", "cg_playerColorTint", 0, 1, 0.1f);
	add_cmd(p, "Player setup (name, model, colours, spray)", "menu_setup");
}

static void pg_network(page_t *p)
{
	add_combo(p, "Rate", "rate", "5000", "5000", "10000", "10000", "25000", "25000", "50000", "50000", "100000", "100000", NULL);
	add_slider(p, "Updates a second", "cl_updaterate", 10, 120, 5);
	add_slider(p, "Moves a second", "cl_cmdrate", 10, 250, 10);
	add_slider(p, "Interpolation delay", "cl_interp", 0, 0.2f, 0.01f);
	add_check(p, "Download missing files", "cl_downloads");
	add_slider(p, "Connection timeout", "cl_timeout", 10, 120, 5);
}

static void pg_options(page_t *p)
{
	add_cmd(p, "Player setup", "menu_setup");
	add_cmd(p, "Crosshair, weapon view and HUD", "menu_view");
	add_cmd(p, "Keys", "menu_keys");
	add_cmd(p, "Video", "menu_video");
	add_cmd(p, "Rendering", "menu_render");
	add_cmd(p, "Textures", "menu_textures");
	add_cmd(p, "Lighting", "menu_lighting");
	add_cmd(p, "Particles and decals", "menu_particles");
	add_cmd(p, "Audio and voice", "menu_audio");
	add_cmd(p, "Speakers", "menu_speakers");
	add_cmd(p, "Performance presets", "menu_fps");
	add_cmd(p, "Network", "menu_network");
	add_cmd(p, "Teamplay", "menu_teamplay");
	add_header(p, "Controls");
	add_slider(p, "Mouse speed", "sensitivity", 1, 20, 0.5f);
	add_combo(p, "Mouse look", "m_pitch", "Normal", "0.022", "Inverted", "-0.022", NULL);
	add_check(p, "Smooth the mouse", "m_filter");
	add_check(p, "Always run", "cl_run");
	add_cmd(p, "Controller...", "menu_controller");
	add_header(p, "Screen");
	add_slider(p, "Field of view", "fov", 70, 130, 5);
	add_combo(p, "Crosshair", "crosshair", "Off", "0", "Plus", "1", "Open plus", "2", "Dot", "3", "Open plus and dot", "4",
	          "Ring", "5", "Corners", "6", NULL);
	add_check(p, "Show the HUD", "cl_drawhud");
	add_check(p, "Show frames a second", "cl_showfps");
	add_header(p, "");
	add_cmd(p, "Reset everything to defaults", "cfg_reset");
}

/* the game's own list of actions to bind, in the format Quake-engine games
 * ship: one action a line, "command" "description" [tooltip], with a
 * "blank" or "-" command (or a lone description) for a section heading.
 * FTE's bindlist.lst comes first, then the kb_act.lst a Half-Life style
 * game keeps in scripts/ or gfx/shell/. Returns the rows it added. */
static int keys_from_list(page_t *p)
{
	static const char *const files[] = {"bindlist.lst", "scripts/kb_act.lst", "gfx/shell/kb_act.lst"};
	for (size_t f = 0; f < BW_ARRAY_COUNT(files); f++) {
		char *text = FS_LoadFile(files[f], NULL);
		int rows = 0;
		if (!text)
			continue;
		for (char *line = text; line && *line;) {
			char *end = strchr(line, '\n'), tok[3][128];
			int n = 0;
			if (end)
				*end = 0;
			for (const char *d = line; n < 3 && (d = COM_Parse(d, tok[n], sizeof(tok[n]))); n++)
				;
			if (n == 1 && line[strspn(line, " \t")] == '"') /* a heading on its own */
				add_header(p, tok[0]), rows++;
			else if (n >= 1 && (!strcmp(tok[0], "blank") || !strcmp(tok[0], "-")))
				add_header(p, n > 1 ? tok[1] : ""), rows++;
			else if (n >= 1)
				add_bind(p, n > 1 ? tok[1] : tok[0], tok[0]), rows++;
			line = end ? end + 1 : NULL;
		}
		Mem_Free(text);
		if (rows)
			return rows;
		free_items(p);
	}
	return 0;
}

static void pg_keys(page_t *p)
{
	/* a game without a list of its own gets the common Quake-engine actions */
	static const char *const binds[][2] = {
		{"Attack", "+attack"}, {"Secondary attack", "+attack2"}, {"Reload", "+reload"}, {"Use", "+use"},
		{"Jump", "+jump"}, {"Crouch", "+duck"}, {"Forward", "+forward"}, {"Back", "+back"},
		{"Step left", "+moveleft"}, {"Step right", "+moveright"}, {"Next weapon", "weapnext"},
		{"Previous weapon", "weapprev"}, {"Spray", "impulse 201"}, {"Scores", "+showscores"}, {"Chat", "messagemode"},
		{"Team chat", "messagemode2"}, {"Voice chat", "+voip"}, {"Screenshot", "screenshot"},
		{"Console", "toggleconsole"}};
	if (!keys_from_list(p))
		for (size_t i = 0; i < BW_ARRAY_COUNT(binds); i++)
			add_bind(p, binds[i][0], binds[i][1]);
	Str_Copy(p->footer, "A/Enter, then a key. Esc or 5 s cancels, Bksp clears", sizeof(p->footer));
}

/* the sticks' tuning (cl_input.c) and vibration (vid_sdl.c); the buttons
 * are ordinary binds, set on the Keys page */
static void pg_controller(page_t *p)
{
	add_check(p, "Use the sticks", "joy_enable");
	add_slider(p, "Turn speed", "joy_yawspeed", 60, 600, 20);
	add_slider(p, "Look up and down speed", "joy_pitchspeed", 40, 400, 20);
	add_check(p, "Invert look", "joy_invert");
	add_slider(p, "Dead zone", "joy_deadzone", 0, 0.5f, 0.02f);
	add_combo(p, "Look acceleration", "joy_curve", "None (linear)", "1", "Low", "1.5", "Normal", "2", "High", "3", NULL);
	add_slider(p, "Movement scale", "joy_movescale", 0.2f, 1, 0.05f);
	add_slider(p, "Vibration", "joy_rumble", 0, 1, 0.1f);
	add_header(p, "Controller layout");
	for (int k = K_GP_DPAD_UP; k <= K_GP_LAST; k++) {
		const char *b = Key_GetBinding(k);
		if (b && *b)
			add_text(p, va("  %-14s %s", Key_NameForNum(k), b));
	}
	add_text(p, "Change a button on the Keys page: A on a row, then the button.");
	add_cmd(p, "Keys and buttons...", "menu_keys");
	add_cmd(p, "Restore the default controller layout", "in_paddefaults");
	Str_Copy(p->footer, "Left/right change a setting. B goes back", sizeof(p->footer));
}

static void pg_video(page_t *p)
{
	add_check(p, "Full screen", "vid_fullscreen");
	add_combo(p, "Resolution", "@resolution", "640x480", NULL, "800x600", NULL, "1024x768", NULL, "1280x720", NULL,
	          "1280x1024", NULL, "1366x768", NULL, "1600x900", NULL, "1920x1080", NULL, "2560x1440", NULL,
	          "3840x2160", NULL, NULL);
	add_check(p, "Vertical sync", "vid_vsync");
	add_combo(p, "2D scale", "scr_scale", "Automatic", "0", "1", "1", "2", "2", "3", "3", "4", "4", NULL);
	add_slider(p, "Gamma", "vid_gamma", 0.5f, 2, 0.05f);
	add_slider(p, "Brightness", "vid_brightness", 0, 1, 0.05f);
	add_combo(p, "Frame limit", "cl_maxfps", "Unlimited", "0", "60", "60", "120", "120", "144", "144", "240", "240", NULL);
	add_cmd(p, "Apply", "vid_restart");
}

static void pg_render(page_t *p)
{
	add_slider(p, "Weapon field of view", "r_viewmodel_fov", 50, 110, 5);
	add_check(p, "Full bright", "r_fullbright");
	add_check(p, "Wireframe", "r_wireframe");
	add_check(p, "Decals", "r_drawdecals");
	add_check(p, "Particles", "r_drawparticles");
	add_check(p, "Realtime world lights", "r_shadow_realtime_world");
	add_check(p, "Light coronas", "r_coronas");
	add_check(p, "Show render speeds", "r_speeds");
	add_combo(p, "Screenshot format", "r_screenshot_format", "PNG", "png", "JPEG", "jpg", "TGA", "tga", NULL);
}

static void pg_textures(page_t *p)
{
	add_combo(p, "Filtering", "gl_texturemode", "None", "GL_NEAREST", "Mipmapped", "GL_NEAREST_MIPMAP_LINEAR",
	          "Bilinear", "GL_LINEAR_MIPMAP_NEAREST", "Trilinear", "GL_LINEAR_MIPMAP_LINEAR", NULL);
	add_slider(p, "Lightmap brightness", "r_lightmapscale", 0.5f, 4, 0.25f);
}

static void pg_lighting(page_t *p)
{
	add_check(p, "Realtime world lights", "r_shadow_realtime_world");
	add_check(p, "Lights from the map's entities", "r_shadow_realtime_world_importlightentitiesfrommap");
	add_check(p, "Coronas", "r_coronas");
	add_slider(p, "Corona brightness", "r_coronas_intensity", 0, 2, 0.1f);
	add_slider(p, "Lightmap brightness", "r_lightmapscale", 0.5f, 4, 0.25f);
	add_check(p, "Edit the lights", "r_editlights");
	add_cmd(p, "Reload the lights", "r_editlights_reload");
	add_cmd(p, "Save the lights", "r_editlights_save");
}

static void pg_particles(page_t *p)
{
	add_check(p, "Particles", "r_drawparticles");
	add_slider(p, "Particle density", "r_part_density", 0.1f, 2, 0.1f);
	add_combo(p, "Most particles", "r_particles_max", "2048", "2048", "8192", "8192", "32768", "32768", "65536", "65536", NULL);
	add_check(p, "Decals", "r_drawdecals");
	add_combo(p, "Most decals", "r_decal_max", "256", "256", "1024", "1024", "4096", "4096", NULL);
}

static void pg_audio(page_t *p)
{
	add_slider(p, "Volume", "volume", 0, 1, 0.05f);
	add_slider(p, "Music volume", "bgmvolume", 0, 1, 0.05f);
	add_slider(p, "Room effects", "dsp_volume", 0, 2, 0.1f);
	add_cmd(p, "Restart sound", "snd_restart");
	add_header(p, "Voice chat");
	add_combo(p, "Send", "cl_voip_send", "While the talk key is held", "0", "When I speak", "1", "Always", "2", NULL);
	add_combo(p, "Codec", "cl_voip_codec", "Best there is", "", "Opus", "2", "Speex", "0", "Raw", "1", "G.711 A-law", "6",
	          "G.711 mu-law", "7", NULL);
	add_slider(p, "Voices volume", "cl_voip_play", 0, 2, 0.1f);
	add_slider(p, "Microphone boost", "cl_voip_micamp", 0, 8, 0.25f);
	add_check(p, "Level the microphone", "cl_voip_autogain");
	add_slider(p, "Speaking threshold", "cl_voip_vad_threshhold", 0, 50, 1);
	add_slider(p, "Game volume while others talk", "cl_voip_ducking", 0, 1, 0.05f);
	add_check(p, "Hear yourself", "cl_voip_test");
}

static void pg_speakers(page_t *p)
{
	add_cmd(p, "Stereo", "soundcontrol card0 stereo");
	add_cmd(p, "Mono", "soundcontrol card0 mono");
	add_cmd(p, "Speakers in front", "soundcontrol card0 front");
	add_cmd(p, "Speakers behind", "soundcontrol card0 back");
	add_cmd(p, "Sound off", "soundcontrol off");
	add_cmd(p, "Sound on", "soundcontrol single");
}

static void pg_fps(page_t *p)
{
	add_cmd(p, "Fastest", "fps_preset fast");
	add_cmd(p, "Normal", "fps_preset normal");
	add_cmd(p, "Nicest", "fps_preset nice");
	add_combo(p, "Frame limit", "cl_maxfps", "Unlimited", "0", "60", "60", "120", "120", "144", "144", "240", "240", NULL);
	add_check(p, "Show frames a second", "cl_showfps");
}

static void pg_media(page_t *p)
{
	const char *t;
	int cur = S_MediaCurrent();
	add_cmd(p, "Next track", "media_next");
	add_cmd(p, "Skip ahead", "music_fforward");
	add_cmd(p, "Go back", "music_rewind");
	add_cmd(p, "Stop", "stopmusic");
	add_cmd(p, "Add music...", "menu_mediafiles");
	add_header(p, "Playlist (Enter takes a track off)");
	for (int i = 0; (t = S_MediaTrack(i)); i++)
		add_cmd(p, va("%s%s", i == cur ? "> " : "  ", t), va("media_remove \"%s\"", t));
	p->refresh = 1;
}

static void list_music(const char *name, const char *searchpath, void *ctx)
{
	page_t *p = ctx;
	for (int i = 0; i < p->numitems; i++)
		if (!strcmp(p->items[i].label, name))
			return;
	add_cmd(p, name, va("media_add \"%s\"", name));
}

static void pg_mediafiles(page_t *p)
{
	static const char *const pats[] = {"music/*.ogg", "music/*.wav", "sound/music/*.ogg", "sound/music/*.wav",
	                                   "sound/cdtracks/*.ogg", "sound/cdtracks/*.wav", "music/*.mp3"};
	for (size_t i = 0; i < BW_ARRAY_COUNT(pats); i++)
		FS_List(pats[i], true, list_music, p);
	if (!p->numitems)
		add_text(p, "No music found in music/ or sound/music/");
	Str_Copy(p->footer, "Enter adds the track to the playlist", sizeof(p->footer));
}

static void list_demo(const char *name, const char *searchpath, void *ctx)
{
	page_t *p = ctx;
	add_cmd(p, name + 6, va("closemenu; playdemo \"%s\"", name + 6)); /* past "demos/" */
}

static void pg_demo(page_t *p)
{
	FS_List("demos/*.dem", true, list_demo, p);
	FS_List("demos/*.mvd", true, list_demo, p);
	if (!p->numitems)
		add_text(p, "No demos in demos/");
	Str_Copy(p->footer, "Enter plays", sizeof(p->footer));
}

static void pg_mods(page_t *p)
{
	const char *dir;
	for (int i = 0; (dir = Mf_GameDirInfo(i, 0)); i++) {
		char d[64];
		Str_Copy(d, dir, sizeof(d));
		const char *desc = Mf_GameDirInfo(i, 1), *load = Mf_GameDirInfo(i, 3);
		add_cmd(p, desc && *desc ? va("%s (%s)", desc, d) : d, load && *load ? load : va("gamedir \"%s\"", d));
	}
	if (!p->numitems)
		add_text(p, "No other game directories");
}

static void pg_download(page_t *p)
{
	const char *name;
	add_cmd(p, "Apply the changes", "pkg apply");
	add_cmd(p, "Look for updates", "pkg refresh");
	add_header(p, "Package\tState\tChange");
	for (int i = 0; (name = PKG_Info(i, 0)); i++) {
		char n[96], title[96], state[32], action[32];
		Str_Copy(n, name, sizeof(n));
		if (strchr(n, '='))
			*strchr(n, '=') = 0;
		Str_Copy(title, PKG_Info(i, 2) ? PKG_Info(i, 2) : n, sizeof(title));
		Str_Copy(state, PKG_Info(i, 8) ? PKG_Info(i, 8) : "", sizeof(state));
		Str_Copy(action, PKG_Info(i, 9) ? PKG_Info(i, 9) : "", sizeof(action));
		bool installed = !strcmp(state, "enabled") || !strcmp(state, "present");
		add_cmd(p, va("%.40s\t%s\t%s", title[0] ? title : n, state[0] ? state : "-", action),
		        va("pkg %s \"%s\"", installed ? "rem" : "add", n));
	}
	char prog[128];
	if (PKG_Progress(prog, sizeof(prog)))
		Str_Copy(p->footer, prog, sizeof(p->footer));
	else
		Str_Copy(p->footer, "Enter marks a package to add or remove", sizeof(p->footer));
	p->refresh = 1;
}

static void pg_credits(page_t *p)
{
	static const char *const lines[] = {
		"Bulwark engine", "", "A clean-room engine for Nuclide games,", "written from public specifications.", "",
		"Running:", "", "Thanks to the authors of the formats and", "specifications it follows: id Software's",
		"Quake file formats, the DarkPlaces and FTE", "QuakeC extension documentation, Vera Visions'",
		"Nuclide, and the libraries it can use:", "SDL, Opus, Speex, SQLite and stb."};
	for (size_t i = 0; i < BW_ARRAY_COUNT(lines); i++)
		add_text(p, i == 6 ? va("  %s", Cvar_String("com_fullgamename")) : lines[i]);
}

static void pg_quit(page_t *p)
{
	add_text(p, "Really quit?");
	add_text(p, "");
	add_cmd(p, "Yes", "quit");
	add_cmd(p, "No", "menupop");
}

static bool quit_key(page_t *p, int key)
{
	if (key == 'y' || key == 'Y') {
		Cbuf_AddText("quit\n");
		return true;
	}
	if (key == 'n' || key == 'N') {
		pop();
		return true;
	}
	return false;
}

/* --- teamplay (the tp_ and loc_ names in messages) ------------------------------------------ */

static void edits(page_t *p, const char *const *names)
{
	for (; *names; names++)
		if (Cvar_Find(*names))
			add_edit(p, *names, *names);
}

static void pg_teamplay(page_t *p)
{
	add_cmd(p, "Ammo and health names", "menu_teamplay_ammo_health");
	add_cmd(p, "Armour names", "menu_teamplay_armor");
	add_cmd(p, "Item names", "menu_teamplay_items");
	add_cmd(p, "Location names", "menu_teamplay_locations");
	add_cmd(p, "What you need", "menu_teamplay_needs");
	add_cmd(p, "Powerup names", "menu_teamplay_powerups");
	add_cmd(p, "Status, location and other words", "menu_teamplay_status_location_misc");
	add_cmd(p, "Team Fortress names", "menu_teamplay_team_fortress");
	add_cmd(p, "Weapon names", "menu_teamplay_weapons");
	add_header(p, "");
	add_check(p, "Expand macros in chat", "cl_parseSay");
	add_check(p, "Message triggers", "cl_triggers");
	add_check(p, "Load location files", "tp_loadlocs");
	add_edit(p, "Sound for team messages", "tp_soundtrigger");
}

static void pg_tp_ammo(page_t *p)
{
	static const char *const n[] = {"tp_name_shells", "tp_name_nails", "tp_name_rockets", "tp_name_cells",
	                                "tp_name_health", "tp_name_mh", "tp_name_backpack", NULL};
	edits(p, n);
}

static void pg_tp_armor(page_t *p)
{
	static const char *const n[] = {"tp_name_armor", "tp_name_ga", "tp_name_ya", "tp_name_ra", "tp_name_armortype_ga",
	                                "tp_name_armortype_ya", "tp_name_armortype_ra", NULL};
	edits(p, n);
}

static void pg_tp_items(page_t *p)
{
	static const char *const n[] = {"tp_name_backpack", "tp_name_flag", "tp_name_rune_1", "tp_name_rune_2",
	                                "tp_name_rune_3", "tp_name_rune_4", NULL};
	edits(p, n);
}

static void pg_tp_locations(page_t *p)
{
	static const char *const n[] = {"loc_name_separator", "loc_name_ssg", "loc_name_ng", "loc_name_sng", "loc_name_gl",
	                                "loc_name_rl", "loc_name_lg", "loc_name_ga", "loc_name_ya", "loc_name_ra",
	                                "loc_name_mh", "loc_name_quad", "loc_name_pent", "loc_name_ring", "loc_name_suit", NULL};
	edits(p, n);
}

static void pg_tp_needs(page_t *p)
{
	static const char *const n[] = {"tp_need_health", "tp_need_ga", "tp_need_ya", "tp_need_ra", "tp_need_shells",
	                                "tp_need_nails", "tp_need_rockets", "tp_need_cells", "tp_need_rl", "tp_need_weapon", NULL};
	edits(p, n);
}

static void pg_tp_powerups(page_t *p)
{
	static const char *const n[] = {"tp_name_quad", "tp_name_pent", "tp_name_ring", "tp_name_suit", "tp_name_quaded",
	                                "tp_name_pented", "tp_name_eyes", NULL};
	edits(p, n);
}

static void pg_tp_misc(page_t *p)
{
	static const char *const n[] = {"tp_name_separator", "tp_name_none", "tp_name_nothing", "tp_name_at",
	                                "tp_name_enemy", "tp_name_teammate", "tp_autostatus", "tp_forceTriggers",
	                                "tp_disputablemacros", NULL};
	edits(p, n);
}

static void pg_tp_tf(page_t *p)
{
	static const char *const n[] = {"tp_name_sentry", "tp_name_disp", "tp_name_flag", NULL};
	edits(p, n);
}

static void pg_tp_weapons(page_t *p)
{
	static const char *const n[] = {"tp_name_axe", "tp_name_sg", "tp_name_ssg", "tp_name_ng", "tp_name_sng",
	                                "tp_name_gl", "tp_name_rl", "tp_name_lg", "tp_name_weapon", "tp_weapon_order", NULL};
	edits(p, n);
}

static const struct {
	const char *name, *title;
	void (*populate)(page_t *);
	double refresh;
	bool (*key)(page_t *, int);
} pages[] = {
	{"menu_main", "Main menu", pg_main, 0, NULL},
	{"menu_single", "Single player", pg_single, 0, NULL},
	{"menu_load", "Load a game", pg_load, 0, NULL},
	{"menu_loadgame", "Load a game", pg_load, 0, NULL},
	{"menu_save", "Save the game", pg_save, 0, NULL},
	{"menu_spcheats", "Cheats", pg_spcheats, 0, NULL},
	{"menu_multi", "Multiplayer", pg_multi, 0, NULL},
	{"menu_servers", "Servers", pg_servers, 0.5, servers_key},
	{"menu_slist", "Servers", pg_servers, 0.5, servers_key},
	{"menu_newmulti", "Start a server", pg_newmulti, 0, NULL},
	{"menu_setup", "Player setup", pg_setup, 0, NULL},
	{"menu_view", "Crosshair, weapon view and HUD", pg_view, 0, NULL},
	{"menu_network", "Network", pg_network, 0, NULL},
	{"menu_options", "Options", pg_options, 0, NULL},
	{"menu_keys", "Keys", pg_keys, 0, NULL},
	{"menu_controller", "Controller", pg_controller, 1, NULL}, /* refreshes: the layout follows binds */
	{"menu_video", "Video", pg_video, 0, NULL},
	{"menu_render", "Rendering", pg_render, 0, NULL},
	{"menu_textures", "Textures", pg_textures, 0, NULL},
	{"menu_lighting", "Lighting", pg_lighting, 0, NULL},
	{"menu_particles", "Particles and decals", pg_particles, 0, NULL},
	{"menu_audio", "Audio and voice", pg_audio, 0, NULL},
	{"menu_speakers", "Speakers", pg_speakers, 0, NULL},
	{"menu_fps", "Performance presets", pg_fps, 0, NULL},
	{"menu_media", "Media player", pg_media, 1, NULL},
	{"menu_mediafiles", "Add music", pg_mediafiles, 0, NULL},
	{"menu_demo", "Demos", pg_demo, 0, NULL},
	{"menu_mods", "Mods", pg_mods, 0, NULL},
	{"menu_download", "Packages and updates", pg_download, 1, NULL},
	{"menu_credits", "Credits", pg_credits, 0, NULL},
	{"menu_quit", "Quit", pg_quit, 0, quit_key},
	{"menu_teamplay", "Teamplay", pg_teamplay, 0, NULL},
	{"menu_teamplay_ammo_health", "Ammo and health", pg_tp_ammo, 0, NULL},
	{"menu_teamplay_armor", "Armour", pg_tp_armor, 0, NULL},
	{"menu_teamplay_items", "Items", pg_tp_items, 0, NULL},
	{"menu_teamplay_locations", "Locations", pg_tp_locations, 0, NULL},
	{"menu_teamplay_needs", "Needs", pg_tp_needs, 0, NULL},
	{"menu_teamplay_powerups", "Powerups", pg_tp_powerups, 0, NULL},
	{"menu_teamplay_status_location_misc", "Status, location and other words", pg_tp_misc, 0, NULL},
	{"menu_teamplay_team_fortress", "Team Fortress", pg_tp_tf, 0, NULL},
	{"menu_teamplay_weapons", "Weapons", pg_tp_weapons, 0, NULL},
};

static void open_page(const char *name)
{
	for (size_t i = 0; i < BW_ARRAY_COUNT(pages); i++)
		if (!Str_ICmp(pages[i].name, name)) {
			/* the same page again goes back to it rather than stacking */
			for (int d = 0; d < depth; d++)
				if (!stack[d]->script && !Str_ICmp(stack[d]->name, name)) {
					while (depth > d + 1)
						pop();
					return;
				}
			if (!strcmp(name, "menu_servers") || !strcmp(name, "menu_slist"))
				CL_HostcacheRefresh(true);
			page_t *p = push(name, pages[i].title, pages[i].populate);
			p->refresh = pages[i].refresh;
			p->key = pages[i].key;
			return;
		}
}

static void Page_f(int argc, const char **argv) { open_page(argv[0]); }

/* --- script menus ---------------------------------------------------------------------------- */

static page_t *script_page(void)
{
	page_t *p = top();
	if (p && p->script)
		return p;
	p = push("script", "", NULL);
	p->script = true;
	return p;
}

static item_t *script_item(int argc, const char **argv, int need, itemtype_t type, const char *usage)
{
	if (argc < need) {
		Con_Printf("%s %s\n", argv[0], usage);
		return NULL;
	}
	page_t *p = script_page();
	item_t *it = add_item(p, type, argc > 3 ? argv[3] : "");
	it->x = (float)atof(argv[1]);
	it->y = (float)atof(argv[2]);
	it->placed = true;
	fix_cursor(p, 1);
	return it;
}

/* conmenu [callback]: a new script menu */
static void Conmenu_f(int argc, const char **argv)
{
	page_t *p = push("script", "", NULL);
	p->script = true;
	Str_Copy(p->callback, argc > 1 ? argv[1] : "", sizeof(p->callback));
}

static void Menutext_f(int argc, const char **argv)
{
	item_t *it = script_item(argc, argv, 4, IT_COMMAND, "x y caption cbcommand");
	if (!it)
		return;
	it->command = argc > 4 ? Mem_Strdup(argv[4]) : NULL;
	if (!it->command)
		it->type = IT_TEXT;
	it->big = !Str_ICmp(argv[0], "menutextbig");
	fix_cursor(top(), 1);
}

static void Menubind_f(int argc, const char **argv)
{
	item_t *it = script_item(argc, argv, 5, IT_BIND, "x y caption bindcommand");
	if (it)
		it->command = Mem_Strdup(argv[4]);
}

static void Menucheck_f(int argc, const char **argv)
{
	item_t *it = script_item(argc, argv, 5, IT_CHECK, "x y caption cvarname bitmask");
	if (!it)
		return;
	it->cvar = Mem_Strdup(argv[4]);
	it->bitmask = argc > 5 ? atoi(argv[5]) : 0;
}

static void Menuslider_f(int argc, const char **argv)
{
	item_t *it = script_item(argc, argv, 7, IT_SLIDER, "x y caption cvarname min max");
	if (!it)
		return;
	it->cvar = Mem_Strdup(argv[4]);
	it->min = (float)atof(argv[5]);
	it->max = (float)atof(argv[6]);
	it->step = fabsf(it->max - it->min) / 20;
	if (it->step <= 0)
		it->step = 1;
}

static void Menucombo_f(int argc, const char **argv)
{
	bool strings = !Str_ICmp(argv[0], "menucombos");
	item_t *it = script_item(argc, argv, 5, IT_COMBO,
	                         strings ? "x y caption cvarname [caption0] [value0] [caption1] [value1] ..."
	                                 : "x y caption cvarname [caption0] [caption1] ...");
	if (!it)
		return;
	it->cvar = Mem_Strdup(argv[4]);
	for (int i = 5, n = 0; i < argc; n++) {
		if (strings) {
			add_option(it, argv[i], i + 1 < argc ? argv[i + 1] : argv[i]);
			i += 2;
		} else {
			add_option(it, argv[i], va("%d", n));
			i++;
		}
	}
}

static void Menuedit_f(int argc, const char **argv)
{
	item_t *it = script_item(argc, argv, 5, IT_EDIT, "x y caption cvarname");
	if (it)
		it->cvar = Mem_Strdup(argv[4]);
}

static void Menueditpriv_f(int argc, const char **argv)
{
	item_t *it = script_item(argc, argv, 4, IT_EDIT, "x y caption def");
	if (it)
		it->value = Mem_Strdup(argc > 4 ? argv[4] : "");
}

static void Menupic_f(int argc, const char **argv)
{
	if (argc < 4) {
		Con_Printf("menupic x y picname\n");
		return;
	}
	page_t *p = script_page();
	item_t *it = add_item(p, IT_PIC, argv[3]);
	it->x = (float)atof(argv[1]);
	it->y = (float)atof(argv[2]);
	fix_cursor(p, 1);
}

static void Menubox_f(int argc, const char **argv)
{
	if (argc < 5) {
		Con_Printf("menubox x y width height\n");
		return;
	}
	page_t *p = script_page();
	item_t *it = add_item(p, IT_BOX, "");
	it->x = (float)atof(argv[1]);
	it->y = (float)atof(argv[2]);
	it->w = (float)atof(argv[3]);
	it->h = (float)atof(argv[4]);
	/* boxes go behind what is already there */
	item_t box = *it;
	memmove(p->items + 1, p->items, sizeof(item_t) * (size_t)(p->numitems - 1));
	p->items[0] = box;
	p->cursor++;
	fix_cursor(p, 1);
}

static void Menucallback_f(int argc, const char **argv)
{
	page_t *p = top();
	if (!p || !p->script || !p->callback[0])
		return;
	Cbuf_AddText(va("%s %s\n", p->callback, Cmd_Args()));
}

static void Menuclear_f(int argc, const char **argv)
{
	if (top() && top()->script)
		pop();
}

static void Menupop_f(int argc, const char **argv) { pop(); }
static void Closemenu_f(int argc, const char **argv) { close_all(); }

void EM_Close(void) { close_all(); }
void EM_OpenMain(void) { open_page("menu_main"); }

void EM_Init(void)
{
	for (size_t i = 0; i < BW_ARRAY_COUNT(pages); i++)
		Cmd_AddD(pages[i].name, Page_f, va("Open the engine's %s menu", pages[i].title));
	Cmd_AddD("conmenu", Conmenu_f, "conmenu <callback>");
	Cmd_AddD("menutext", Menutext_f, "x y caption cbcommand");
	Cmd_AddD("menutextbig", Menutext_f, "x y caption cbcommand");
	Cmd_AddD("menubind", Menubind_f, "x y caption bindcommand");
	Cmd_AddD("menucheck", Menucheck_f, "x y caption cvarname bitmask");
	Cmd_AddD("menuslider", Menuslider_f, "x y caption cvarname min max");
	Cmd_AddD("menucomboi", Menucombo_f, "x y caption cvarname [caption0] [caption1] ...");
	Cmd_AddD("menucombos", Menucombo_f, "x y caption cvarname [caption0] [value0] [caption1] [value1] ...");
	Cmd_AddD("menuedit", Menuedit_f, "x y caption cvarname");
	Cmd_AddD("menueditpriv", Menueditpriv_f, "x y caption def");
	Cmd_AddD("menupic", Menupic_f, "x y picname");
	Cmd_AddD("menubox", Menubox_f, "x y width height: a framed box, in characters");
	Cmd_AddD("menucallback", Menucallback_f, "Explicitly invoke the active script menu's callback function with the given option set.");
	Cmd_AddD("menuclear", Menuclear_f, "Pop the currently scripted menu.");
	Cmd_AddD("menupop", Menupop_f, "Go back one menu");
	Cmd_AddD("closemenu", Closemenu_f, "Close the engine's menus");
}
