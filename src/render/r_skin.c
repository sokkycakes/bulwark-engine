/* Bulwark engine - custom skins (FTE_QC_CUSTOMSKINS), shader remapping and
 * reading images back for the game.
 *
 * A custom skin is a small text in .skin form, from the doc comments in
 * Nuclide's fteextensions.qc:
 *   surfacename,shadername         the legacy (Quake 3) form of replace
 *   replace "surface" "shader"     draw that surface with another material
 *   geomset <group> <value>        which sub-model of a body part to draw (from 1)
 * Nuclide sets body groups this way ("geomset 0 2"); for Half-Life models a
 * group is a body part and the value one of its sub-models. qwskin, q1upper,
 * q1lower, h2class and compose are accepted and ignored.
 * ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/fs.h"

#include <stdio.h>
#include <stdlib.h>

#define MAX_SKINS 1024
#define SKIN_GEOMSETS 8
#define SKIN_REPLACES 16

struct rskin_s {
	int refs;
	int geomset[SKIN_GEOMSETS]; /* -1 unset */
	int numreplace;
	struct {
		char surface[64];
		material_t *mat;
	} replace[SKIN_REPLACES];
};

static rskin_t *skins[MAX_SKINS]; /* skin objects by number; 0 is none */

/* which skin each client game entity wears, by vm */
typedef struct {
	qcvm_t *vm;
	int *skin; /* per entity, a skin number */
	int size;
	void (*oldhook)(qcvm_t *vm, int e);
} entskins_t;
static entskins_t entskins[4];

/* Geomset values count sub-models from 1, with 0 meaning the first one:
 * Nuclide's Half-Life content is written that way (scientist heads 1-4,
 * Barney's drawn gun "1 2", the grunt commander's head "1 2"). */
int R_SkinGeomset(const rskin_t *s, int group)
{
	if (!s || group < 0 || group >= SKIN_GEOMSETS || s->geomset[group] <= 0)
		return 0;
	return s->geomset[group] - 1;
}

material_t *R_SkinReplace(const rskin_t *s, const char *surface)
{
	if (!s)
		return NULL;
	for (int i = 0; i < s->numreplace; i++)
		if (!Str_ICmp(s->replace[i].surface, surface))
			return s->replace[i].mat;
	return NULL;
}

static void parse_skin(rskin_t *s, const char *text)
{
	char line[512];
	for (int i = 0; i < SKIN_GEOMSETS; i++)
		s->geomset[i] = -1;
	while (text && *text) {
		const char *nl = strchr(text, '\n');
		size_t n = nl ? (size_t)(nl - text) : strlen(text);
		char a[128] = "", b[128] = "", c[128] = "";
		if (n >= sizeof(line))
			n = sizeof(line) - 1;
		memcpy(line, text, n);
		line[n] = 0;
		text = nl ? nl + 1 : NULL;
		if (sscanf(line, " geomset %127s %127s", a, b) == 2) {
			int g = atoi(a);
			if (g >= 0 && g < SKIN_GEOMSETS)
				s->geomset[g] = atoi(b);
		} else if (sscanf(line, " replace \"%127[^\"]\" \"%127[^\"]\"", a, b) == 2 ||
		           sscanf(line, " replace %127s %127s", a, b) == 2) {
			if (s->numreplace < SKIN_REPLACES) {
				Str_Copy(s->replace[s->numreplace].surface, a, sizeof(s->replace[0].surface));
				s->replace[s->numreplace++].mat = R_FindMaterial(b, MATF_MODEL);
			}
		} else if (sscanf(line, " %127[^, \t] , %127s", a, c) == 2 && !strchr(a, ' ')) {
			if (s->numreplace < SKIN_REPLACES && Str_NICmp(a, "tag_", 4)) {
				Str_Copy(s->replace[s->numreplace].surface, a, sizeof(s->replace[0].surface));
				s->replace[s->numreplace++].mat = R_FindMaterial(c, MATF_MODEL);
			}
		}
	}
}

/* a new skin object from a .skin file and/or text; its number, 0 on failure */
static int new_skin(const char *file, const char *data)
{
	rskin_t *s;
	char *filetext = NULL;
	int n;
	for (n = 1; n < MAX_SKINS && skins[n]; n++)
		;
	if (n == MAX_SKINS)
		return 0;
	if (file && *file)
		filetext = FS_LoadFile(file, NULL);
	s = Mem_Alloc(sizeof(*s));
	parse_skin(s, filetext);
	if (data && *data) {
		/* the data adds to (or overrides) what the file says */
		rskin_t extra;
		memset(&extra, 0, sizeof(extra));
		parse_skin(&extra, data);
		for (int i = 0; i < SKIN_GEOMSETS; i++)
			if (extra.geomset[i] >= 0)
				s->geomset[i] = extra.geomset[i];
		for (int i = 0; i < extra.numreplace && s->numreplace < SKIN_REPLACES; i++)
			s->replace[s->numreplace++] = extra.replace[i];
	}
	Mem_Free(filetext);
	skins[n] = s;
	return n;
}

static void release(int n)
{
	if (n <= 0 || n >= MAX_SKINS || !skins[n])
		return;
	if (--skins[n]->refs <= 0) {
		Mem_Free(skins[n]);
		skins[n] = NULL;
	}
}

static void entity_freed(qcvm_t *vm, int e);

static entskins_t *table_for(qcvm_t *vm, bool create)
{
	entskins_t *freeslot = NULL;
	for (int i = 0; i < (int)BW_ARRAY_COUNT(entskins); i++) {
		if (entskins[i].vm == vm)
			return &entskins[i];
		if (!entskins[i].vm && !freeslot)
			freeslot = &entskins[i];
	}
	if (!create || !freeslot)
		return NULL;
	freeslot->vm = vm;
	freeslot->size = vm->max_edicts;
	freeslot->skin = Mem_Alloc(sizeof(int) * (size_t)vm->max_edicts);
	/* a freed entity drops its skin; the hook another module set still runs */
	freeslot->oldhook = vm->free_hook;
	vm->free_hook = entity_freed;
	return freeslot;
}

static void entity_freed(qcvm_t *vm, int e)
{
	entskins_t *t = table_for(vm, false);
	if (!t)
		return;
	if (e > 0 && e < t->size && t->skin[e]) {
		release(t->skin[e]);
		t->skin[e] = 0;
	}
	if (t->oldhook)
		t->oldhook(vm, e);
}

static void set_entity_skin(qcvm_t *vm, int e, int n)
{
	entskins_t *t = table_for(vm, n != 0);
	if (!t || e <= 0 || e >= t->size)
		return;
	if (n)
		skins[n]->refs++;
	release(t->skin[e]);
	t->skin[e] = n;
}

const rskin_t *R_EntitySkin(qcvm_t *vm, int ent)
{
	entskins_t *t;
	if (!vm || ent <= 0 || !(t = table_for(vm, false)) || ent >= t->size || !t->skin[ent])
		return NULL;
	return skins[t->skin[ent]];
}

/* a VM is going away (or a map changed): its entities' skins go with it */
void R_ClearSkins(void)
{
	for (int i = 0; i < (int)BW_ARRAY_COUNT(entskins); i++) {
		entskins_t *t = &entskins[i];
		if (!t->vm)
			continue;
		if (QC_VMCount() > 0) {
			bool alive = false;
			for (int k = 0; k < QC_VMCount(); k++)
				alive |= QC_VMAt(k) == t->vm;
			if (alive && t->vm->free_hook == entity_freed)
				t->vm->free_hook = t->oldhook;
		}
		Mem_Free(t->skin);
		memset(t, 0, sizeof(*t));
	}
	for (int n = 1; n < MAX_SKINS; n++) {
		Mem_Free(skins[n]);
		skins[n] = NULL;
	}
}

/* setcustomskin(e, file, optional data); empty both to clear */
static void bi_setcustomskin(qcvm_t *vm)
{
	const char *file = QC_PSTR(vm, 1), *data = vm->argc > 2 ? QC_PSTR(vm, 2) : "";
	int n = (*file || *data) ? new_skin(file, data) : 0;
	set_entity_skin(vm, QC_PENT(vm, 0), n);
}

static void bi_loadcustomskin(qcvm_t *vm)
{
	int n = new_skin(QC_PSTR(vm, 0), vm->argc > 1 ? QC_PSTR(vm, 1) : "");
	if (n)
		skins[n]->refs = 1; /* the caller's reference, until releasecustomskin */
	QC_ReturnFloat(vm, (float)n);
}

static void bi_applycustomskin(qcvm_t *vm)
{
	int n = (int)QC_PFLOAT(vm, 1);
	set_entity_skin(vm, QC_PENT(vm, 0), n > 0 && n < MAX_SKINS && skins[n] ? n : 0);
}

static void bi_releasecustomskin(qcvm_t *vm) { release((int)QC_PFLOAT(vm, 0)); }

/* --- remapshader -------------------------------------------------------------------- */

/* a remapped material takes the replacement's look; its original is kept
 * to come back when it is remapped to itself (or to nothing) */
typedef struct {
	material_t *mat;
	material_t original;
} remap_t;
static remap_t *remaps;
static int numremaps;

static void bi_remapshader(qcvm_t *vm)
{
	const char *from = QC_PSTR(vm, 0), *to = QC_PSTR(vm, 1);
	material_t *m = R_FindMaterial(from, 0), *r;
	remap_t *saved = NULL;
	for (int i = 0; i < numremaps; i++)
		if (remaps[i].mat == m)
			saved = &remaps[i];
	if (!*to || !Str_ICmp(from, to)) {
		if (saved) {
			material_t *next = m->next;
			*m = saved->original;
			m->next = next;
		}
		return;
	}
	r = R_FindMaterial(to, 0);
	if (!r || r == m)
		return;
	if (!saved) {
		remaps = Mem_Realloc(remaps, sizeof(*remaps) * (size_t)(numremaps + 1));
		saved = &remaps[numremaps++];
		saved->mat = m;
		saved->original = *m;
	}
	{
		char name[sizeof(m->name)];
		material_t *next = m->next;
		Str_Copy(name, m->name, sizeof(name));
		*m = *r;
		Str_Copy(m->name, name, sizeof(m->name));
		m->next = next;
	}
}

/* a new map starts with every material as it was */
void R_ClearRemaps(void)
{
	for (int i = 0; i < numremaps; i++) {
		material_t *next = remaps[i].mat->next;
		*remaps[i].mat = remaps[i].original;
		remaps[i].mat->next = next;
	}
	Mem_Free(remaps);
	remaps = NULL;
	numremaps = 0;
}

/* --- r_readimage --------------------------------------------------------------------- */

/* r_readimage(file, out width, out height, out format): RGBA pixels in the
 * game's heap (it frees them with memfree), or null */
static void bi_r_readimage(qcvm_t *vm)
{
	size_t len = 0;
	uint8_t *data = (uint8_t *)FS_LoadFile(QC_PSTR(vm, 0), &len), *px;
	int w = 0, h = 0;
	uint32_t ptr = 0;
	if (data && (px = R_DecodeImage(data, len, &w, &h))) {
		size_t bytes = (size_t)w * (size_t)h * 4;
		Mem_Free(data); /* before the heap block: a map overview is 3 MB */
		data = NULL;
		ptr = QC_HeapAlloc(vm, bytes);
		if (ptr)
			memcpy(QC_Pointer(vm, ptr, bytes), px, bytes);
		Mem_Free(px);
	}
	Mem_Free(data);
	/* __out parameters: the caller copies them back from its parameter slots */
	QC_PINT(vm, 1) = ptr ? w : 0;
	QC_PINT(vm, 2) = ptr ? h : 0;
	QC_PINT(vm, 3) = 1; /* PTI_RGBA8 */
	QC_ReturnInt(vm, (int32_t)ptr);
}

const qc_builtin_t r_skin_builtins[] = {
	{"setcustomskin", bi_setcustomskin}, {"loadcustomskin", bi_loadcustomskin},
	{"applycustomskin", bi_applycustomskin}, {"releasecustomskin", bi_releasecustomskin},
	{"remapshader", bi_remapshader}, {"r_readimage", bi_r_readimage}, {NULL, NULL}};
