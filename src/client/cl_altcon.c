/* Bulwark engine - named consoles (FTE_CSQC_ALTCONSOLES): text buffers the
 * client game and the menu create by printing to them, and draw into a box
 * of their own (Nuclide's VGUI text views use them). Behaviour from the
 * doc comments in Nuclide's fteextensions.qc:
 *
 *   string con_getset(conname, field, optional newvalue)  old value returned;
 *       fields: title, name, next, unseen, markup, forceutf8, close, clear,
 *       hidden, linecount
 *   void con_printf(conname, fmt, ...)     prints, creating the console
 *   void con_draw(conname, pos, size, fontsize)
 *   float con_input(conname, inevtype, a, b, c)  mouse updates are absolute
 *
 * The console named "" (or "main") is the engine's own and is not touched.
 * ISC licence, see LICENSE. */
#include "client.h"
#include "keys.h"
#include "../render/render.h"

#include <stdio.h>
#include <stdlib.h>

#define ALTCON_MAX 64
#define ALTCON_LINES 512

typedef struct {
	bool used;
	char name[64];
	char title[64];
	bool hidden, forceutf8, markup, unseen;
	char *lines[ALTCON_LINES]; /* a ring: lines[(first + i) % ALTCON_LINES] */
	int first, count;
	strbuf_t partial; /* text printed since the last newline */
	int scroll;       /* lines up from the bottom */
	bool focus;
	float mouse[2];
} altcon_t;

static altcon_t cons[ALTCON_MAX];

static altcon_t *find_con(const char *name, bool create)
{
	altcon_t *freecon = NULL;
	if (!*name || !Str_ICmp(name, "main"))
		return NULL;
	for (int i = 0; i < ALTCON_MAX; i++) {
		if (cons[i].used && !Str_ICmp(cons[i].name, name))
			return &cons[i];
		if (!cons[i].used && !freecon)
			freecon = &cons[i];
	}
	if (!create || !freecon)
		return NULL;
	memset(freecon, 0, sizeof(*freecon));
	freecon->used = true;
	Str_Copy(freecon->name, name, sizeof(freecon->name));
	Str_Copy(freecon->title, name, sizeof(freecon->title));
	freecon->markup = true;
	return freecon;
}

static void clear_con(altcon_t *c)
{
	for (int i = 0; i < c->count; i++)
		Mem_Free(c->lines[(c->first + i) % ALTCON_LINES]);
	c->first = c->count = c->scroll = 0;
	SB_Free(&c->partial);
	memset(&c->partial, 0, sizeof(c->partial));
}

static void add_line(altcon_t *c, const char *s, size_t n)
{
	char *line = Mem_Alloc(n + 1);
	memcpy(line, s, n);
	line[n] = 0;
	if (c->count == ALTCON_LINES) {
		Mem_Free(c->lines[c->first]);
		c->first = (c->first + 1) % ALTCON_LINES;
		c->count--;
	}
	c->lines[(c->first + c->count++) % ALTCON_LINES] = line;
	c->unseen = true;
}

static void print_con(altcon_t *c, const char *text)
{
	for (const char *s = text; *s;) {
		const char *nl = strchr(s, '\n');
		if (!nl) {
			SB_AppendStr(&c->partial, s);
			break;
		}
		SB_Append(&c->partial, s, (size_t)(nl - s));
		add_line(c, c->partial.data ? c->partial.data : "", c->partial.data ? c->partial.len : 0);
		SB_Free(&c->partial);
		memset(&c->partial, 0, sizeof(c->partial));
		s = nl + 1;
	}
}

static void bi_con_getset(qcvm_t *vm)
{
	const char *name = QC_PSTR(vm, 0), *field = QC_PSTR(vm, 1);
	bool set = vm->argc > 2;
	const char *nv = set ? QC_PSTR(vm, 2) : "";
	char old[128] = "";
	altcon_t *c = find_con(name, false);

	if (!Str_ICmp(field, "next")) {
		/* the console after this one, "" after the last; "" starts the walk */
		int start = 0;
		if (c)
			start = (int)(c - cons) + 1;
		for (int i = start; i < ALTCON_MAX; i++)
			if (cons[i].used) {
				QC_ReturnString(vm, cons[i].name);
				return;
			}
		QC_ReturnString(vm, "");
		return;
	}
	if (!c) {
		QC_ReturnString(vm, "");
		return;
	}
	if (!Str_ICmp(field, "title")) {
		Str_Copy(old, c->title, sizeof(old));
		if (set)
			Str_Copy(c->title, nv, sizeof(c->title));
	} else if (!Str_ICmp(field, "name")) {
		Str_Copy(old, c->name, sizeof(old));
		if (set && *nv && !find_con(nv, false))
			Str_Copy(c->name, nv, sizeof(c->name));
	} else if (!Str_ICmp(field, "unseen")) {
		snprintf(old, sizeof(old), "%d", c->unseen);
		if (set)
			c->unseen = atoi(nv) != 0;
	} else if (!Str_ICmp(field, "markup")) {
		snprintf(old, sizeof(old), "%d", c->markup);
		if (set)
			c->markup = atoi(nv) != 0;
	} else if (!Str_ICmp(field, "forceutf8")) {
		snprintf(old, sizeof(old), "%d", c->forceutf8);
		if (set)
			c->forceutf8 = atoi(nv) != 0;
	} else if (!Str_ICmp(field, "hidden")) {
		snprintf(old, sizeof(old), "%d", c->hidden);
		if (set)
			c->hidden = atoi(nv) != 0;
	} else if (!Str_ICmp(field, "linecount")) {
		snprintf(old, sizeof(old), "%d", c->count + (c->partial.len ? 1 : 0));
	} else if (!Str_ICmp(field, "clear")) {
		clear_con(c);
	} else if (!Str_ICmp(field, "close")) {
		clear_con(c);
		c->used = false;
	}
	QC_ReturnString(vm, old);
}

static void bi_con_printf(qcvm_t *vm)
{
	altcon_t *c = find_con(QC_PSTR(vm, 0), true);
	strbuf_t sb;
	QC_Format(vm, 1, &sb);
	if (c)
		print_con(c, sb.data ? sb.data : "");
	else
		Con_Printf("%s", sb.data ? sb.data : "");
	SB_Free(&sb);
}

static void bi_con_draw(qcvm_t *vm)
{
	altcon_t *c = find_con(QC_PSTR(vm, 0), false);
	const float *pos = QC_PVEC(vm, 1), *size = QC_PVEC(vm, 2);
	float fs = vm->argc > 3 && QC_PFLOAT(vm, 3) > 0 ? QC_PFLOAT(vm, 3) : 8;
	static const float white[3] = {1, 1, 1};
	int rows, total, last;
	float y;
	if (!c || size[0] <= 0 || size[1] <= 0)
		return;
	c->unseen = false;
	rows = (int)(size[1] / fs);
	total = c->count + (c->partial.len ? 1 : 0);
	if (c->scroll > total - rows)
		c->scroll = BW_MAX(0, total - rows);
	last = total - c->scroll; /* one past the bottom line drawn */
	y = pos[1] + size[1] - fs;
	R_Set2DClip(pos[0], pos[1], size[0], size[1]);
	for (int i = last - 1; i >= 0 && y > pos[1] - fs; i--, y -= fs) {
		const char *line = i < c->count ? c->lines[(c->first + i) % ALTCON_LINES] : c->partial.data;
		if (line)
			R_Text(pos[0], y, line, fs, white, 1);
	}
	R_Reset2DClip();
}

static void bi_con_input(qcvm_t *vm)
{
	altcon_t *c = find_con(QC_PSTR(vm, 0), false);
	int type = (int)QC_PFLOAT(vm, 1);
	float a = QC_PFLOAT(vm, 2), b = QC_PFLOAT(vm, 3);
	float used = 0;
	if (!c) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	switch (type) {
	case 0: /* IE_KEYDOWN: a is the key */
		if ((int)a == K_MWHEELUP || (int)a == K_PGUP) {
			c->scroll += (int)a == K_PGUP ? 10 : 3;
			used = 1;
		} else if ((int)a == K_MWHEELDOWN || (int)a == K_PGDN) {
			c->scroll = BW_MAX(0, c->scroll - ((int)a == K_PGDN ? 10 : 3));
			used = 1;
		} else if ((int)a == K_END) {
			c->scroll = 0;
			used = 1;
		}
		break;
	case 3: /* IE_MOUSEABS */
		c->mouse[0] = a, c->mouse[1] = b;
		break;
	case 5: /* IE_FOCUS: a mouse focus, b keyboard focus, -1 unchanged */
		if (a >= 0)
			c->focus = a != 0;
		break;
	default:
		break;
	}
	QC_ReturnFloat(vm, used);
}

const qc_builtin_t cl_altcon_builtins[] = {
	{"con_getset", bi_con_getset}, {"con_printf", bi_con_printf}, {"con_draw", bi_con_draw},
	{"con_input", bi_con_input}, {NULL, NULL}};
