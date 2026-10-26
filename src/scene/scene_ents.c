/* Bulwark engine - scenes to QuakeC entities and back. ISC licence, see LICENSE.
 *
 * QuakeC has no components, so each entity-making component in a scene
 * becomes one edict, spawned through the same entity lump text a BSP
 * provides. That keeps the progs' own spawn path (CheckSpawn, SpawnKey and
 * __fullspawndata in Nuclide) the only one. See docs/spec/scene-format.md. */
#include "scene.h"

#include <math.h>
#include <stdio.h>

#define ENTITY_COMPONENT "Bulwark.Entity"

static bool is_type(const json_t *c, const char *want)
{
	const char *t = JSON_GetString(c, "__type", NULL);
	if (!t)
		return false;
	if (!Str_NICmp(t, "Sandbox.", 8))
		t += 8;
	return !strcmp(t, want);
}

static bool component_enabled(const json_t *c) { return JSON_GetBool(c, "__enabled", true); }

static bool object_spawns(const json_t *go)
{
	int flags = (int)JSON_GetNumber(go, "Flags", 0);
	return JSON_GetBool(go, "Enabled", true) && !(flags & GOFLAG_EDITORONLY);
}

static const char *entity_classname(const json_t *c)
{
	const char *cn = JSON_GetString(c, "classname", NULL);
	if (!cn)
		cn = JSON_GetString(c, "ClassName", NULL);
	return cn && *cn ? cn : NULL;
}

/* the QC class a component spawns, or NULL. s&box's own components get a
 * small set of stand-ins so its sample scenes show something. */
static const char *component_class(const json_t *go, const json_t *c)
{
	bool rigid;
	if (!JSON_IsObject(c) || !component_enabled(c))
		return NULL;
	if (is_type(c, ENTITY_COMPONENT) || is_type(c, "Entity"))
		return entity_classname(c);
	if (Scene_FindComponent(go, ENTITY_COMPONENT))
		return NULL; /* the object's entity handles its model and physics */
	rigid = Scene_FindComponent(go, "Rigidbody") != NULL;
	if (is_type(c, "ModelRenderer"))
		return rigid ? "prop_physics" : "prop_static";
	if (is_type(c, "SkinnedModelRenderer"))
		return rigid ? "prop_physics" : "prop_dynamic";
	if (is_type(c, "SpawnPoint"))
		return "info_player_start";
	return NULL;
}

/* --- map ------------------------------------------------------------- */

static const json_t *find_mapinstance(const json_t *list)
{
	JSON_FOREACH(go, list) {
		const json_t *hit;
		if (!object_spawns(go))
			continue;
		JSON_FOREACH(c, JSON_Get(go, "Components"))
			if ((is_type(c, "MapInstance") || is_type(c, "MapComponent")) && component_enabled(c))
				return c;
		if ((hit = find_mapinstance(JSON_Get(go, "Children"))))
			return hit;
	}
	return NULL;
}

void Scene_MapInfo(const scenefile_t *resolved, scene_entinfo_t *info)
{
	const json_t *mi;
	const char *name;
	memset(info, 0, sizeof(*info));
	info->spawnmapentities = true;
	if (resolved->kind == SCENEFILE_PREFAB) {
		const json_t *ro = Scene_RootObject(resolved);
		mi = NULL;
		JSON_FOREACH(c, JSON_Get(ro, "Components"))
			if (!mi && is_type(c, "MapInstance") && component_enabled(c))
				mi = c;
		if (!mi)
			mi = find_mapinstance(JSON_Get(ro, "Children"));
	} else {
		mi = find_mapinstance(Scene_Objects(resolved));
	}
	if (!mi)
		return;
	info->spawnmapentities = JSON_GetBool(mi, "SpawnMapEntities", true);
	name = JSON_GetString(mi, "MapName", NULL);
	if (!name || !*name)
		return;
	{
		const char *slash = strrchr(name, '/');
		const char *dot = strrchr(slash ? slash : name, '.');
		snprintf(info->mapname, sizeof(info->mapname), "%s%s%s", slash ? "" : "maps/", name, dot ? "" : ".bsp");
		if (dot && Str_ICmp(dot, ".bsp"))
			Con_Printf("^3MapInstance names %s; only BSP maps are supported\n", name);
	}
}

/* --- objects --------------------------------------------------------- */

typedef struct {
	const json_t *go;
	const char *guid;
	int parent;    /* nearest ancestor that spawns an entity, -1 if none */
	int numents;   /* entity components on this object */
	bool needname; /* something refers to it, or it parents entities */
	char targetname[128];
	scene_xform_t world;
} goinfo_t;

typedef struct {
	goinfo_t *items;
	int count, cap;
	char **refs; /* GameObject GUIDs that references point at */
	int numrefs, maxrefs;
} golist_t;

static int find_go(const golist_t *gl, const char *guid)
{
	if (!guid)
		return -1;
	for (int i = 0; i < gl->count; i++)
		if (gl->items[i].guid && !Str_ICmp(gl->items[i].guid, guid))
			return i;
	return -1;
}

/* s&box writes references as { "_type": "gameobject", "go": guid } or
 * { "_type": "component", "component_id": guid, "go": guid, ... } */
static const char *reference_target(const json_t *v)
{
	const char *t;
	if (!JSON_IsObject(v) || !(t = JSON_GetString(v, "_type", NULL)))
		return NULL;
	if (strcmp(t, "gameobject") && strcmp(t, "component"))
		return NULL;
	return JSON_GetString(v, "go", NULL);
}

static void collect_refs(golist_t *gl, const json_t *v)
{
	const char *target = reference_target(v);
	if (target) {
		if (gl->numrefs == gl->maxrefs) {
			gl->maxrefs = gl->maxrefs ? gl->maxrefs * 2 : 32;
			gl->refs = Mem_Realloc(gl->refs, sizeof(char *) * (size_t)gl->maxrefs);
		}
		gl->refs[gl->numrefs++] = (char *)target;
		return;
	}
	for (const json_t *it = v->child; it; it = it->next)
		collect_refs(gl, it);
}

static void gather(golist_t *gl, const json_t *go, int parent_ent, const scene_xform_t *parent_xf)
{
	goinfo_t *gi;
	scene_xform_t local;
	int me, ent_for_children;
	if (!JSON_IsObject(go) || !object_spawns(go))
		return;
	if (gl->count == gl->cap) {
		gl->cap = gl->cap ? gl->cap * 2 : 64;
		gl->items = Mem_Realloc(gl->items, sizeof(*gl->items) * (size_t)gl->cap);
	}
	me = gl->count++;
	gi = &gl->items[me];
	memset(gi, 0, sizeof(*gi));
	gi->go = go;
	gi->guid = JSON_GetString(go, "__guid", NULL);
	gi->parent = parent_ent;
	Scene_LocalTransform(go, &local);
	if (parent_xf && !((int)JSON_GetNumber(go, "Flags", 0) & GOFLAG_ABSOLUTE))
		Scene_CombineTransform(parent_xf, &local, &gi->world);
	else
		gi->world = local;
	JSON_FOREACH(c, JSON_Get(go, "Components")) {
		if (component_class(go, c)) {
			if (!gi->numents++) {
				const char *tn = JSON_GetString(c, "targetname", NULL);
				if (tn)
					Str_Copy(gi->targetname, tn, sizeof(gi->targetname));
			}
		}
		if (component_enabled(c))
			collect_refs(gl, c);
	}
	if (gi->numents > 1)
		gi->needname = true; /* its extra entities attach to the first */
	if (gi->numents && parent_ent >= 0)
		gl->items[parent_ent].needname = true;
	ent_for_children = gi->numents ? me : parent_ent;
	{
		scene_xform_t world = gi->world; /* gl->items may move while recursing */
		JSON_FOREACH(child, JSON_Get(go, "Children"))
			gather(gl, child, ent_for_children, &world);
	}
}

/* --- writing --------------------------------------------------------- */

/* fixed decimals with trailing zeros dropped: "16", "0.5", "-12.25" */
static void fmt_float(char *out, size_t size, float v, int decimals)
{
	size_t n;
	snprintf(out, size, "%.*f", decimals, v);
	n = strlen(out);
	while (n > 1 && out[n - 1] == '0')
		out[--n] = 0;
	if (n > 1 && out[n - 1] == '.')
		out[--n] = 0;
	if (!strcmp(out, "-0"))
		Str_Copy(out, "0", size);
}

static void fmt_vec(char *out, size_t size, const float *v, int n, const char *sep, int decimals)
{
	char f[32];
	out[0] = 0;
	for (int i = 0; i < n; i++) {
		fmt_float(f, sizeof(f), v[i], decimals);
		if (i)
			Str_Cat(out, sep, size);
		Str_Cat(out, f, size);
	}
}

/* entity lump values cannot hold double quotes or line breaks */
static void write_quoted(strbuf_t *out, const char *s)
{
	SB_Append(out, "\"", 1);
	for (; *s; s++) {
		char c = *s;
		if (c == '"')
			c = '\'';
		else if (c == '\n' || c == '\r')
			c = ' ';
		SB_Append(out, &c, 1);
	}
	SB_Append(out, "\"", 1);
}

static void write_pair(strbuf_t *out, const char *key, const char *value)
{
	write_quoted(out, key);
	SB_Append(out, " ", 1);
	write_quoted(out, value);
	SB_Append(out, "\n", 1);
}

/* a JSON property as an entity key value; false if it has no text form */
static bool value_text(const golist_t *gl, const json_t *v, strbuf_t *sb)
{
	const char *target;
	switch (v->type) {
	case JSON_STRING:
		SB_AppendStr(sb, v->str);
		return true;
	case JSON_NUMBER:
		SB_AppendStr(sb, v->str);
		return true;
	case JSON_TRUE:
		SB_AppendStr(sb, "1");
		return true;
	case JSON_FALSE:
		SB_AppendStr(sb, "0");
		return true;
	case JSON_ARRAY:
		/* [x, y, z] -> "x y z" */
		JSON_FOREACH(it, v) {
			if (it->type != JSON_NUMBER && it->type != JSON_STRING)
				return false;
			if (it != v->child)
				SB_Append(sb, " ", 1);
			SB_AppendStr(sb, it->str);
		}
		return true;
	case JSON_OBJECT: {
		int i;
		if (!(target = reference_target(v)))
			return false;
		i = find_go(gl, target);
		if (i < 0 || !gl->items[i].targetname[0])
			return false;
		SB_AppendStr(sb, gl->items[i].targetname);
		return true;
	}
	default:
		return false;
	}
}

static bool skip_key(const char *key)
{
	return !strncmp(key, "__", 2) || !strcmp(key, "Flags") || !strcmp(key, "classname") ||
	       !strcmp(key, "ClassName") || !strncmp(key, "OnComponent", 11);
}

static void write_entity(const golist_t *gl, const goinfo_t *gi, const json_t *c, const char *classname,
                         bool primary, strbuf_t *out)
{
	char buf[256];
	bool entity = is_type(c, ENTITY_COMPONENT) || is_type(c, "Entity");
	bool has_origin = entity && JSON_Get(c, "origin");
	bool has_angles = entity && (JSON_Get(c, "angles") || JSON_Get(c, "angle"));
	bool has_scale = entity && JSON_Get(c, "scale");
	bool has_target = entity && JSON_Get(c, "targetname");
	bool has_parent = entity && JSON_Get(c, "parentname");
	float angles[3];
	float s;

	SB_AppendStr(out, "{\n");
	write_pair(out, "classname", classname);
	if (!has_origin) {
		fmt_vec(buf, sizeof(buf), gi->world.pos, 3, " ", 4);
		write_pair(out, "origin", buf);
	}
	Scene_QuatToAngles(gi->world.rot, angles);
	if (!has_angles && (angles[0] || angles[1] || angles[2])) {
		fmt_vec(buf, sizeof(buf), angles, 3, " ", 4);
		write_pair(out, "angles", buf);
	}
	s = (gi->world.scale[0] + gi->world.scale[1] + gi->world.scale[2]) / 3.0f;
	if (!has_scale && fabsf(s - 1) > 0.0001f) {
		/* QuakeC entities only scale uniformly */
		if (fabsf(gi->world.scale[0] - s) > 0.001f || fabsf(gi->world.scale[1] - s) > 0.001f)
			Con_DPrintf("%s: non-uniform scale averaged to %g\n", classname, s);
		fmt_float(buf, sizeof(buf), s, 4);
		write_pair(out, "scale", buf);
	}
	if (primary && !has_target && gi->targetname[0])
		write_pair(out, "targetname", gi->targetname);
	if (!has_parent) {
		/* extra entities on an object hang off its first; the first hangs
		 * off the nearest ancestor that has an entity */
		const char *pn = !primary ? gi->targetname : gi->parent >= 0 ? gl->items[gi->parent].targetname : "";
		if (pn[0])
			write_pair(out, "parentname", pn);
	}

	if (entity) {
		JSON_FOREACH(kv, c) {
			strbuf_t v = {0};
			if (skip_key(kv->key))
				continue;
			if (value_text(gl, kv, &v))
				write_pair(out, kv->key, v.data ? v.data : "");
			SB_Free(&v);
		}
	} else {
		/* s&box stand-ins */
		const char *model = JSON_GetString(c, "Model", NULL);
		float tint[4];
		if (model && *model)
			write_pair(out, "model", model);
		if (Scene_ParseFloats(JSON_GetString(c, "Tint", NULL), tint, 3) && (tint[0] < 1 || tint[1] < 1 || tint[2] < 1)) {
			snprintf(buf, sizeof(buf), "%d %d %d", (int)(tint[0] * 255 + 0.5f), (int)(tint[1] * 255 + 0.5f),
			         (int)(tint[2] * 255 + 0.5f));
			write_pair(out, "rendercolor", buf);
		}
	}
	SB_AppendStr(out, "}\n");
}

/* length of the first { ... } block in entity text, quotes respected */
static size_t first_block(const char *s)
{
	const char *p = strchr(s, '{');
	bool quoted = false;
	if (!p)
		return 0;
	for (; *p; p++) {
		if (*p == '"')
			quoted = !quoted;
		else if (*p == '}' && !quoted)
			return (size_t)(p + 1 - s);
	}
	return 0;
}

void Scene_BuildEntities(const scenefile_t *resolved, const char *mapentities, strbuf_t *out, scene_entinfo_t *info)
{
	golist_t gl = {0};
	Scene_MapInfo(resolved, info);

	if (mapentities) {
		if (info->spawnmapentities) {
			SB_AppendStr(out, mapentities);
		} else {
			SB_Append(out, mapentities, first_block(mapentities));
		}
		SB_AppendStr(out, "\n");
	}

	if (resolved->kind == SCENEFILE_PREFAB) {
		gather(&gl, Scene_RootObject(resolved), -1, NULL);
	} else {
		JSON_FOREACH(go, Scene_Objects(resolved))
			gather(&gl, go, -1, NULL);
	}

	/* name everything that needs one: referenced objects and entity parents */
	for (int r = 0; r < gl.numrefs; r++) {
		int i = find_go(&gl, gl.refs[r]);
		if (i >= 0)
			gl.items[i].needname = true;
	}
	for (int i = 0; i < gl.count; i++) {
		goinfo_t *gi = &gl.items[i];
		if (gi->numents && gi->needname && !gi->targetname[0] && gi->guid)
			Str_Copy(gi->targetname, gi->guid, sizeof(gi->targetname));
	}

	for (int i = 0; i < gl.count; i++) {
		goinfo_t *gi = &gl.items[i];
		bool primary = true;
		JSON_FOREACH(c, JSON_Get(gi->go, "Components")) {
			const char *cn = component_class(gi->go, c);
			if (!cn) {
				if (JSON_IsObject(c) && component_enabled(c) && !is_type(c, "MapInstance") &&
				    !is_type(c, "MapComponent")) {
					info->numskipped++;
					if ((is_type(c, ENTITY_COMPONENT) || is_type(c, "Entity")))
						Con_Printf("^3%s: entity component without a classname\n", JSON_GetString(gi->go, "Name", "?"));
				}
				continue;
			}
			write_entity(&gl, gi, c, cn, primary, out);
			info->numentities++;
			primary = false;
		}
	}
	Mem_Free(gl.items);
	Mem_Free(gl.refs);
}

/* --- entity lump to scene -------------------------------------------- */

scenefile_t *Scene_FromEntities(const char *entities, const char *mapname, const char *name)
{
	scenefile_t *sf = Scene_New(SCENEFILE_SCENE, name);
	json_t *mapgo = Scene_AddGameObject(sf, NULL, "Map");
	json_t *mi = Scene_AddComponent(mapgo, "MapInstance");
	const char *data = entities;
	static char key[256], value[8192];

	JSON_SetString(mi, "MapName", mapname ? mapname : "");
	JSON_SetBool(mi, "EnableCollision", true);
	/* the BSP keeps its worldspawn; everything else now lives in the scene */
	JSON_SetBool(mi, "SpawnMapEntities", false);

	while (data && (data = COM_Parse(data, key, sizeof(key)))) {
		json_t *ent;
		json_t *go;
		const char *classname = NULL, *targetname = NULL;
		float origin[3] = {0, 0, 0}, angles[3] = {0, 0, 0};
		bool has_origin = false, has_angles = false;

		if (strcmp(key, "{")) {
			Con_Printf("^3%s: entity text: expected {, got %s\n", name ? name : "entities", key);
			break;
		}
		ent = JSON_New(JSON_OBJECT);
		for (;;) {
			if (!(data = COM_Parse(data, key, sizeof(key))) || !strcmp(key, "}"))
				break;
			if (!(data = COM_Parse(data, value, sizeof(value))))
				break;
			if (!strcmp(key, "origin") && Scene_ParseFloats(value, origin, 3)) {
				has_origin = true;
				continue;
			}
			if (!strcmp(key, "angles") && Scene_ParseFloats(value, angles, 3)) {
				has_angles = true;
				continue;
			}
			if (!strcmp(key, "angle") && Scene_ParseFloats(value, angles + 1, 1) && angles[1] != -1 && angles[1] != -2) {
				/* -1 and -2 mean up and down for movers; leave those as keys */
				has_angles = true;
				continue;
			}
			JSON_SetString(ent, key, value);
		}
		classname = JSON_GetString(ent, "classname", NULL);
		if (!classname || !strcmp(classname, "worldspawn")) {
			JSON_Free(ent);
			continue;
		}
		targetname = JSON_GetString(ent, "targetname", NULL);
		go = Scene_AddGameObject(sf, NULL, targetname && *targetname ? targetname : classname);
		if (has_origin) {
			char buf[128];
			fmt_vec(buf, sizeof(buf), origin, 3, ",", 4);
			JSON_SetString(go, "Position", buf);
		}
		if (has_angles) {
			char buf[160];
			float q[4];
			Scene_AnglesToQuat(angles, q);
			fmt_vec(buf, sizeof(buf), q, 4, ",", 7);
			JSON_SetString(go, "Rotation", buf);
		}
		{
			json_t *c = Scene_AddComponent(go, ENTITY_COMPONENT);
			JSON_SetString(c, "classname", classname);
			JSON_FOREACH(kv, ent)
				if (strcmp(kv->key, "classname"))
					JSON_SetString(c, kv->key, kv->str);
		}
		JSON_Free(ent);
	}
	return sf;
}
