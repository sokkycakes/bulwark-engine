/* Bulwark engine - prefab instance expansion. ISC licence, see LICENSE.
 *
 * A prefab instance in a scene is stored as a stub: the prefab's path, a
 * patch (objects added, removed, moved and properties overridden, all named
 * by the prefab's GUIDs) and a table mapping prefab GUIDs to the instance's
 * GUIDs. Expanding one takes a copy of the prefab (with its own nested
 * instances already expanded), applies the patch, then renames every GUID
 * through the table. See docs/spec/scene-format.md. */
#include "scene.h"

#include <stdio.h>

#define MAX_PREFAB_DEPTH 16

/* --- prefab cache ---------------------------------------------------- */

typedef struct {
	char *path;
	scenefile_t *sf; /* NULL: tried and failed */
} cached_prefab_t;

struct scene_prefabcache_s {
	cached_prefab_t *items;
	int count, cap;
};

scene_prefabcache_t *Scene_NewPrefabCache(void) { return Mem_Alloc(sizeof(scene_prefabcache_t)); }

void Scene_FreePrefabCache(scene_prefabcache_t *pc)
{
	if (!pc)
		return;
	for (int i = 0; i < pc->count; i++) {
		Mem_Free(pc->items[i].path);
		Scene_Free(pc->items[i].sf);
	}
	Mem_Free(pc->items);
	Mem_Free(pc);
}

void Scene_PrefabCacheAdd(scene_prefabcache_t *pc, const char *path, scenefile_t *sf)
{
	if (pc->count == pc->cap) {
		pc->cap = pc->cap ? pc->cap * 2 : 16;
		pc->items = Mem_Realloc(pc->items, sizeof(*pc->items) * (size_t)pc->cap);
	}
	pc->items[pc->count].path = Mem_Strdup(path);
	pc->items[pc->count].sf = sf;
	pc->count++;
}

const scenefile_t *Scene_PrefabCacheLoad(const char *path, void *ctx)
{
	scene_prefabcache_t *pc = ctx;
	scenefile_t *sf;
	for (int i = 0; i < pc->count; i++)
		if (!Str_ICmp(pc->items[i].path, path))
			return pc->items[i].sf;
	sf = Scene_Load(path);
	if (sf && sf->kind != SCENEFILE_PREFAB) {
		Con_Printf("^3%s is not a prefab\n", path);
		Scene_Free(sf);
		sf = NULL;
	}
	Scene_PrefabCacheAdd(pc, path, sf);
	return sf;
}

/* --- tracked objects ------------------------------------------------- */

/* every GameObject and component under a root, with the array it sits in */
typedef struct {
	const char *id;
	json_t *node;
	json_t *container; /* NULL for the root */
} tracked_t;

typedef struct {
	tracked_t *items;
	int count, cap;
} tracklist_t;

static void track_add(tracklist_t *tl, json_t *node, json_t *container)
{
	const char *id = JSON_GetString(node, "__guid", NULL);
	if (!id)
		return;
	if (tl->count == tl->cap) {
		tl->cap = tl->cap ? tl->cap * 2 : 64;
		tl->items = Mem_Realloc(tl->items, sizeof(*tl->items) * (size_t)tl->cap);
	}
	tl->items[tl->count].id = id;
	tl->items[tl->count].node = node;
	tl->items[tl->count].container = container;
	tl->count++;
}

static void track_object(tracklist_t *tl, json_t *go, json_t *container)
{
	json_t *list;
	track_add(tl, go, container);
	list = JSON_Get(go, "Components");
	JSON_FOREACH(c, list)
		if (JSON_IsObject(c))
			track_add(tl, c, list);
	list = JSON_Get(go, "Children");
	JSON_FOREACH(c, list)
		if (JSON_IsObject(c))
			track_object(tl, c, list);
}

static void track_rebuild(tracklist_t *tl, json_t *root)
{
	tl->count = 0;
	track_object(tl, root, NULL);
}

static tracked_t *track_find(tracklist_t *tl, const char *id)
{
	if (!id)
		return NULL;
	for (int i = 0; i < tl->count; i++)
		if (!Str_ICmp(tl->items[i].id, id))
			return &tl->items[i];
	return NULL;
}

/* ObjectIdentifier: { "Type": "GameObject" | "Component", "IdValue": guid } */
static const char *ident(const json_t *j) { return JSON_GetString(j, "IdValue", NULL); }

/* the array property of parent named by prop, created if missing */
static json_t *container_of(json_t *parent, const char *prop)
{
	json_t *c = JSON_Get(parent, prop);
	if (!JSON_IsArray(c)) {
		JSON_Set(parent, prop, JSON_New(JSON_ARRAY));
		c = JSON_Get(parent, prop);
	}
	return c;
}

/* inserts after the element whose guid is previd, or first if there is none */
static void insert_after_id(json_t *container, const char *previd, json_t *node)
{
	json_t *after = NULL;
	if (previd) {
		JSON_FOREACH(it, container) {
			const char *g = JSON_GetString(it, "__guid", NULL);
			if (g && !Str_ICmp(g, previd)) {
				after = it;
				break;
			}
		}
	}
	JSON_InsertAfter(container, after, node);
}

static void place_added(tracklist_t *tl, json_t *root, const json_t *a)
{
	tracked_t *parent = track_find(tl, ident(JSON_Get(a, "Parent")));
	const char *prop = JSON_GetString(a, "ContainerProperty", "Children");
	json_t *data = JSON_Get(a, "Data");
	if (!parent || !JSON_IsObject(data))
		return; /* parent was removed: the addition goes too, as in s&box */
	if (JSON_GetBool(a, "IsContainerArray", true)) {
		json_t *c = container_of(parent->node, prop);
		json_t *node = JSON_Clone(data);
		insert_after_id(c, ident(JSON_Get(a, "PreviousElement")), node);
		track_object(tl, node, c);
	} else {
		JSON_Set(parent->node, prop, JSON_Clone(data));
		track_rebuild(tl, root);
	}
}

static void apply_patch(json_t *root, const json_t *patch, const char *name)
{
	tracklist_t tl = {0};
	json_t *dead = JSON_New(JSON_ARRAY); /* removed subtrees, freed at the end */
	json_t *added;

	/* removals */
	track_rebuild(&tl, root);
	JSON_FOREACH(r, JSON_Get(patch, "RemovedObjects")) {
		tracked_t *t = track_find(&tl, ident(JSON_Get(r, "Id")));
		if (t && t->container && JSON_Detach(t->container, t->node)) {
			JSON_Append(dead, t->node);
			t->container = NULL;
			t->id = "";
		}
	}

	/* additions: an object may follow another added object, so each one is
	 * placed once its predecessor is in the tree */
	added = JSON_Get(patch, "AddedObjects");
	if (added && added->count) {
		int n = added->count, left = n;
		json_t **items = Mem_Alloc(sizeof(json_t *) * (size_t)n);
		int k = 0;
		JSON_FOREACH(a, added)
			items[k++] = a;
		track_rebuild(&tl, root);
		while (left > 0) {
			int placed = 0;
			for (int i = 0; i < n; i++) {
				const char *previd;
				bool waiting = false;
				if (!items[i])
					continue;
				previd = ident(JSON_Get(items[i], "PreviousElement"));
				for (int j = 0; previd && j < n && !waiting; j++)
					if (j != i && items[j] && ident(JSON_Get(items[j], "Id")) && !Str_ICmp(ident(JSON_Get(items[j], "Id")), previd))
						waiting = true;
				if (waiting)
					continue;
				place_added(&tl, root, items[i]);
				items[i] = NULL;
				left--;
				placed++;
			}
			if (!placed) {
				/* predecessors form a cycle: place the rest in file order */
				for (int i = 0; i < n; i++) {
					if (items[i]) {
						place_added(&tl, root, items[i]);
						items[i] = NULL;
						left--;
					}
				}
			}
		}
		Mem_Free(items);
	}

	/* moves */
	track_rebuild(&tl, root);
	JSON_FOREACH(m, JSON_Get(patch, "MovedObjects")) {
		tracked_t *t = track_find(&tl, ident(JSON_Get(m, "Id")));
		tracked_t *np;
		json_t *node;
		if (!t || !t->container)
			continue;
		node = t->node;
		np = track_find(&tl, ident(JSON_Get(m, "NewParent")));
		JSON_Detach(t->container, node);
		if (!np) {
			JSON_Append(dead, node);
		} else {
			json_t *c = container_of(np->node, JSON_GetString(m, "NewContainerProperty", "Children"));
			insert_after_id(c, ident(JSON_Get(m, "NewPreviousElement")), node);
		}
		track_rebuild(&tl, root);
	}

	/* property overrides */
	JSON_FOREACH(o, JSON_Get(patch, "PropertyOverrides")) {
		tracked_t *t = track_find(&tl, ident(JSON_Get(o, "Target")));
		const char *prop = JSON_GetString(o, "Property", NULL);
		json_t *v = JSON_Get(o, "Value");
		if (!t || !prop)
			continue;
		JSON_Set(t->node, prop, v ? JSON_Clone(v) : JSON_New(JSON_NULL));
	}

	if (developer && dead->count)
		Con_DPrintf("%s: patch removed %d objects\n", name, dead->count);
	JSON_Free(dead);
	Mem_Free(tl.items);
}

/* --- GUID remapping -------------------------------------------------- */

typedef struct {
	guid_t from, to;
} guidpair_t;

typedef struct {
	guidpair_t *items;
	int count, cap;
} guidmap_t;

static const guid_t *map_find(const guidmap_t *m, const guid_t *g)
{
	for (int i = 0; i < m->count; i++)
		if (!memcmp(&m->items[i].from, g, sizeof(*g)))
			return &m->items[i].to;
	return NULL;
}

static void map_add(guidmap_t *m, const guid_t *from, const guid_t *to)
{
	if (map_find(m, from))
		return;
	if (m->count == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 64;
		m->items = Mem_Realloc(m->items, sizeof(*m->items) * (size_t)m->cap);
	}
	m->items[m->count].from = *from;
	m->items[m->count].to = *to;
	m->count++;
}

/* objects the instance's table does not cover get a GUID derived from the
 * instance and the prefab GUID, so two instances never share one */
static void fill_gaps(guidmap_t *m, json_t *go, const guid_t *instance)
{
	guid_t g, made;
	if (Guid_Parse(JSON_GetString(go, "__guid", NULL), &g) && !map_find(m, &g)) {
		Guid_Combine(instance, &g, &made);
		map_add(m, &g, &made);
	}
	JSON_FOREACH(c, JSON_Get(go, "Components")) {
		if (Guid_Parse(JSON_GetString(c, "__guid", NULL), &g) && !map_find(m, &g)) {
			Guid_Combine(instance, &g, &made);
			map_add(m, &g, &made);
		}
	}
	JSON_FOREACH(c, JSON_Get(go, "Children"))
		fill_gaps(m, c, instance);
}

/* every string that is a mapped GUID is replaced, which also fixes
 * references between objects inside the prefab */
static void remap_strings(json_t *j, const guidmap_t *m)
{
	guid_t g;
	const guid_t *to;
	if (j->type == JSON_STRING && strlen(j->str) >= 32 && Guid_Parse(j->str, &g) && (to = map_find(m, &g))) {
		char buf[40];
		Guid_Format(to, buf);
		Mem_Free(j->str);
		j->str = Mem_Strdup(buf);
		return;
	}
	for (json_t *it = j->child; it; it = it->next)
		remap_strings(it, m);
}

/* --- expansion ------------------------------------------------------- */

static void expand_list(json_t *list, scene_prefab_fn load, void *ctx, int depth);

static json_t *missing_stub(const json_t *inst, const char *path)
{
	json_t *go = JSON_New(JSON_OBJECT);
	char name[256];
	JSON_SetString(go, "__guid", JSON_GetString(inst, "__guid", ""));
	JSON_SetNumber(go, "__version", GAMEOBJECT_VERSION);
	JSON_SetNumber(go, "Flags", 0);
	snprintf(name, sizeof(name), "[Missing Prefab] %s", path);
	JSON_SetString(go, "Name", name);
	JSON_SetBool(go, "Enabled", false);
	JSON_Set(go, "Components", JSON_New(JSON_ARRAY));
	JSON_Set(go, "Children", JSON_New(JSON_ARRAY));
	return go;
}

static json_t *instantiate(const json_t *inst, scene_prefab_fn load, void *ctx, int depth)
{
	const char *path = JSON_GetString(inst, "__Prefab", NULL);
	const scenefile_t *prefab = path && load ? load(path, ctx) : NULL;
	const json_t *patch = JSON_Get(inst, "__PrefabInstancePatch");
	guid_t instguid, rootguid, g, to;
	guidmap_t map = {0};
	json_t *full;

	if (!path)
		path = "(no path)";
	if (!prefab || !Scene_RootObject(prefab)) {
		Con_Printf("^3missing prefab %s\n", path);
		return missing_stub(inst, path);
	}
	if (depth >= MAX_PREFAB_DEPTH) {
		Con_Printf("^3prefab %s: nested more than %d deep (does it contain itself?)\n", path, MAX_PREFAB_DEPTH);
		return missing_stub(inst, path);
	}
	if (!JSON_IsObject(patch))
		Con_Printf("^3prefab instance of %s has no patch (made by a very old s&box?); using the prefab as is\n", path);

	full = JSON_Clone(Scene_RootObject(prefab));
	Mem_Free(full->key);
	full->key = NULL;
	/* the prefab's own nested instances first, so the patch can reach into them */
	expand_list(JSON_Get(full, "Children"), load, ctx, depth + 1);
	if (JSON_IsObject(patch))
		apply_patch(full, patch, path);

	/* rename prefab GUIDs to instance GUIDs */
	JSON_FOREACH(pair, JSON_Get(inst, "__PrefabIdToInstanceId"))
		if (Guid_Parse(pair->key, &g) && JSON_IsString(pair) && Guid_Parse(pair->str, &to))
			map_add(&map, &g, &to);
	if (Guid_Parse(JSON_GetString(inst, "__guid", NULL), &instguid)) {
		if (Guid_Parse(JSON_GetString(full, "__guid", NULL), &rootguid))
			map_add(&map, &rootguid, &instguid);
		fill_gaps(&map, full, &instguid);
	}
	remap_strings(full, &map);
	Mem_Free(map.items);

	/* editor bookkeeping that means nothing once expanded */
	JSON_Remove(full, "__EditorPrefabNestedInstance");
	JSON_Remove(full, "__EditorSkipPrefabBreakOnRefresh");
	return full;
}

/* replaces each instance in list with its expansion, then descends */
static void expand_list(json_t *list, scene_prefab_fn load, void *ctx, int depth)
{
	json_t *prev = NULL;
	if (!list)
		return;
	for (json_t *go = list->child; go;) {
		json_t *next = go->next;
		if (JSON_IsObject(go) && JSON_Get(go, "__Prefab")) {
			json_t *full = instantiate(go, load, ctx, depth);
			JSON_Detach(list, go);
			JSON_InsertAfter(list, prev, full);
			JSON_Free(go);
			go = full;
			/* objects the patch added may be instances themselves */
			expand_list(JSON_Get(go, "Children"), load, ctx, depth + 1);
		} else if (JSON_IsObject(go)) {
			expand_list(JSON_Get(go, "Children"), load, ctx, depth);
		}
		prev = go;
		go = next;
	}
}

scenefile_t *Scene_Resolve(const scenefile_t *sf, scene_prefab_fn load, void *ctx)
{
	scenefile_t *out = Mem_Alloc(sizeof(*out));
	*out = *sf;
	out->root = JSON_Clone(sf->root);
	if (sf->kind == SCENEFILE_SCENE)
		expand_list(Scene_Objects(out), load, ctx, 0);
	else
		expand_list(JSON_Get(Scene_RootObject(out), "Children"), load, ctx, 0);
	return out;
}
