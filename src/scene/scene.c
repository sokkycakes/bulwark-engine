/* Bulwark engine - scene and prefab files. ISC licence, see LICENSE.
 * Format notes and where they came from: docs/spec/scene-format.md. */
#include "scene.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* --- GUIDs ----------------------------------------------------------- */

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

bool Guid_Parse(const char *s, guid_t *out)
{
	int n = 0;
	if (!s)
		return false;
	if (*s == '{')
		s++;
	while (n < 16) {
		int hi, lo;
		if ((n == 4 || n == 6 || n == 8 || n == 10) && *s == '-')
			s++;
		hi = hexval(s[0]);
		lo = hi < 0 ? -1 : hexval(s[1]);
		if (lo < 0)
			return false;
		out->b[n++] = (uint8_t)(hi << 4 | lo);
		s += 2;
	}
	if (*s == '}')
		s++;
	return *s == 0;
}

void Guid_Format(const guid_t *g, char *out)
{
	static const char hex[] = "0123456789abcdef";
	for (int i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			*out++ = '-';
		*out++ = hex[g->b[i] >> 4];
		*out++ = hex[g->b[i] & 15];
	}
	*out = 0;
}

static uint64_t splitmix(uint64_t *s)
{
	uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

static void guid_from_words(uint64_t a, uint64_t b, guid_t *g)
{
	for (int i = 0; i < 8; i++) {
		g->b[i] = (uint8_t)(a >> (56 - 8 * i));
		g->b[8 + i] = (uint8_t)(b >> (56 - 8 * i));
	}
	g->b[6] = (uint8_t)((g->b[6] & 0x0f) | 0x40); /* version 4 */
	g->b[8] = (uint8_t)((g->b[8] & 0x3f) | 0x80); /* RFC 4122 variant */
}

void Guid_Random(guid_t *g)
{
	static uint64_t state;
	uint64_t a, b;
	if (!state) {
		FILE *f = fopen("/dev/urandom", "rb");
		if (!f || fread(&state, sizeof(state), 1, f) != 1)
			state = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)&state ^ (uint64_t)(Sys_Milliseconds() * 1000.0);
		if (f)
			fclose(f);
	}
	a = splitmix(&state);
	b = splitmix(&state);
	guid_from_words(a, b, g);
}

void Guid_Combine(const guid_t *x, const guid_t *y, guid_t *out)
{
	uint64_t s = 0x6a09e667f3bcc908ull, a, b;
	for (int i = 0; i < 16; i++)
		s = (s ^ x->b[i]) * 0x100000001b3ull;
	for (int i = 0; i < 16; i++)
		s = (s ^ y->b[i]) * 0x100000001b3ull;
	a = splitmix(&s);
	b = splitmix(&s);
	guid_from_words(a, b, out);
}

static void new_guid_string(char *out)
{
	guid_t g;
	Guid_Random(&g);
	Guid_Format(&g, out);
}

/* --- upgrades -------------------------------------------------------- */

/* Each step mirrors one of s&box's upgraders for the same version number.
 * They run in order on files older than the current version. */

typedef void (*goupgrade_fn)(json_t *obj);

static void each_object(json_t *list, goupgrade_fn fn);

static void each_object_tree(json_t *go, goupgrade_fn fn)
{
	if (!JSON_IsObject(go))
		return;
	fn(go);
	each_object(JSON_Get(go, "Children"), fn);
}

static void each_object(json_t *list, goupgrade_fn fn)
{
	JSON_FOREACH(go, list)
		each_object_tree(go, fn);
}

/* v1: "Id" (or "id") became "__guid", on objects and components */
static void upgrade_ids(json_t *obj)
{
	if (!JSON_Get(obj, "__guid")) {
		if (!JSON_Rename(obj, "Id", "__guid"))
			JSON_Rename(obj, "id", "__guid");
	}
	JSON_FOREACH(c, JSON_Get(obj, "Components"))
		if (JSON_IsObject(c) && !JSON_Get(c, "__guid") && !JSON_Rename(c, "Id", "__guid"))
			JSON_Rename(c, "id", "__guid");
}

/* scene v3 / prefab v2: every object has Flags, Children and Components */
static void upgrade_defaults(json_t *obj)
{
	if (JSON_Get(obj, "__Prefab"))
		return; /* instances keep only their patch */
	if (!JSON_Get(obj, "Flags"))
		JSON_SetNumber(obj, "Flags", 0);
	if (!JSON_IsArray(JSON_Get(obj, "Children")))
		JSON_Set(obj, "Children", JSON_New(JSON_ARRAY));
	if (!JSON_IsArray(JSON_Get(obj, "Components")))
		JSON_Set(obj, "Components", JSON_New(JSON_ARRAY));
}

/* GameObject v2: NetworkInterpolation false became NetworkFlags bit 1 */
static void upgrade_gameobject(json_t *obj)
{
	json_t *ni = JSON_Get(obj, "NetworkInterpolation");
	int version = (int)JSON_GetNumber(obj, "__version", 0);
	if (version < 2 && ni) {
		int flags = (int)JSON_GetNumber(obj, "NetworkFlags", 0);
		if (ni->type == JSON_FALSE)
			flags |= 1;
		JSON_SetNumber(obj, "NetworkFlags", flags);
		JSON_Remove(obj, "NetworkInterpolation");
	}
	/* components have no shared upgrades; a missing guid gets one so
	 * references and prefab patches can name it */
	JSON_FOREACH(c, JSON_Get(obj, "Components")) {
		if (JSON_IsObject(c) && !JSON_Get(c, "__guid")) {
			char g[40];
			new_guid_string(g);
			JSON_SetString(c, "__guid", g);
		}
	}
	if (!JSON_Get(obj, "__guid")) {
		char g[40];
		new_guid_string(g);
		JSON_SetString(obj, "__guid", g);
	}
	if (!JSON_Get(obj, "__Prefab"))
		JSON_SetNumber(obj, "__version", GAMEOBJECT_VERSION);
}

static void upgrade_scene(json_t *root, int version)
{
	json_t *objects = JSON_Get(root, "GameObjects");
	if (version < 1) {
		if (!JSON_Get(root, "__guid") && !JSON_Rename(root, "Id", "__guid"))
			JSON_Rename(root, "id", "__guid");
		each_object(objects, upgrade_ids);
	}
	if (version < 2) {
		/* Title and Description moved into SceneProperties.Metadata */
		const char *title = JSON_GetString(root, "Title", NULL);
		const char *desc = JSON_GetString(root, "Description", NULL);
		json_t *props = JSON_Get(root, "SceneProperties");
		if (((title && *title) || (desc && *desc)) && JSON_IsObject(props)) {
			json_t *meta = JSON_New(JSON_OBJECT);
			JSON_Set(meta, "Title", title ? JSON_NewString(title) : JSON_New(JSON_NULL));
			JSON_Set(meta, "Description", desc ? JSON_NewString(desc) : JSON_New(JSON_NULL));
			JSON_Set(props, "Metadata", meta);
		}
	}
	if (version < 3)
		each_object(objects, upgrade_defaults);
	each_object(objects, upgrade_gameobject);
}

static void upgrade_prefab(json_t *root, int version)
{
	json_t *ro = JSON_Get(root, "RootObject");
	if (!JSON_IsObject(ro))
		return;
	if (version < 1)
		each_object_tree(ro, upgrade_ids);
	if (version < 2)
		each_object_tree(ro, upgrade_defaults);
	each_object_tree(ro, upgrade_gameobject);
}

/* --- files ----------------------------------------------------------- */

static bool has_suffix(const char *s, const char *suffix)
{
	size_t a = strlen(s), b = strlen(suffix);
	return a >= b && !Str_ICmp(s + a - b, suffix);
}

scenefile_t *Scene_Parse(const char *text, const char *name)
{
	char err[256];
	json_t *root = JSON_Parse(text, err, sizeof(err));
	scenefile_t *sf;
	int version;
	if (!name)
		name = "scene";
	if (!root) {
		Con_Printf("^1%s: %s\n", name, err);
		return NULL;
	}
	if (!JSON_IsObject(root)) {
		Con_Printf("^1%s: not a scene or prefab (top level is not an object)\n", name);
		JSON_Free(root);
		return NULL;
	}
	sf = Mem_Alloc(sizeof(*sf));
	sf->root = root;
	Str_Copy(sf->name, name, sizeof(sf->name));
	if (has_suffix(name, ".prefab") || (!JSON_Get(root, "GameObjects") && JSON_Get(root, "RootObject")))
		sf->kind = SCENEFILE_PREFAB;
	else
		sf->kind = SCENEFILE_SCENE;

	version = (int)JSON_GetNumber(root, "__version", 0);
	if (sf->kind == SCENEFILE_SCENE) {
		if (!JSON_IsArray(JSON_Get(root, "GameObjects")))
			JSON_Set(root, "GameObjects", JSON_New(JSON_ARRAY));
		if (version > SCENE_FILE_VERSION)
			Con_Printf("^3%s: scene version %d is newer than this engine knows (%d)\n", name, version, SCENE_FILE_VERSION);
		upgrade_scene(root, version);
	} else {
		if (!JSON_IsObject(JSON_Get(root, "RootObject"))) {
			Con_Printf("^1%s: prefab has no RootObject\n", name);
			Scene_Free(sf);
			return NULL;
		}
		if (version > PREFAB_FILE_VERSION)
			Con_Printf("^3%s: prefab version %d is newer than this engine knows (%d)\n", name, version, PREFAB_FILE_VERSION);
		upgrade_prefab(root, version);
	}
	return sf;
}

scenefile_t *Scene_Load(const char *path)
{
	size_t len;
	char *text = FS_LoadFile(path, &len);
	scenefile_t *sf;
	if (!text) {
		Con_Printf("^1couldn't load %s\n", path);
		return NULL;
	}
	sf = Scene_Parse(text, path);
	Mem_Free(text);
	return sf;
}

scenefile_t *Scene_New(scenefile_kind_t kind, const char *name)
{
	scenefile_t *sf = Mem_Alloc(sizeof(*sf));
	char g[40];
	sf->kind = kind;
	sf->root = JSON_New(JSON_OBJECT);
	Str_Copy(sf->name, name ? name : "untitled", sizeof(sf->name));
	if (kind == SCENEFILE_SCENE) {
		new_guid_string(g);
		JSON_SetString(sf->root, "__guid", g);
		JSON_Set(sf->root, "GameObjects", JSON_New(JSON_ARRAY));
		JSON_Set(sf->root, "SceneProperties", JSON_New(JSON_OBJECT));
		JSON_Set(sf->root, "__references", JSON_New(JSON_ARRAY));
	} else {
		JSON_Set(sf->root, "RootObject", Scene_NewGameObject(name ? name : "root"));
		JSON_Set(sf->root, "__references", JSON_New(JSON_ARRAY));
	}
	return sf;
}

void Scene_Free(scenefile_t *sf)
{
	if (!sf)
		return;
	JSON_Free(sf->root);
	Mem_Free(sf);
}

void Scene_Write(const scenefile_t *sf, strbuf_t *out)
{
	json_t *root = sf->root;
	int v = sf->kind == SCENEFILE_SCENE ? SCENE_FILE_VERSION : PREFAB_FILE_VERSION;
	/* both version fields describe the format we write */
	if (sf->kind == SCENEFILE_SCENE)
		JSON_SetNumber(root, "ResourceVersion", v);
	JSON_SetNumber(root, "__version", v);
	JSON_Write(out, root, 2);
}

json_t *Scene_Objects(const scenefile_t *sf)
{
	return sf->kind == SCENEFILE_SCENE ? JSON_Get(sf->root, "GameObjects") : NULL;
}

json_t *Scene_RootObject(const scenefile_t *sf)
{
	return sf->kind == SCENEFILE_PREFAB ? JSON_Get(sf->root, "RootObject") : NULL;
}

/* --- building -------------------------------------------------------- */

json_t *Scene_NewGameObject(const char *name)
{
	json_t *go = JSON_New(JSON_OBJECT);
	char g[40];
	new_guid_string(g);
	JSON_SetString(go, "__guid", g);
	JSON_SetNumber(go, "__version", GAMEOBJECT_VERSION);
	JSON_SetNumber(go, "Flags", 0);
	JSON_SetString(go, "Name", name ? name : "Object");
	JSON_SetString(go, "Position", "0,0,0");
	JSON_SetString(go, "Rotation", "0,0,0,1");
	JSON_SetString(go, "Scale", "1,1,1");
	JSON_SetString(go, "Tags", "");
	JSON_SetBool(go, "Enabled", true);
	JSON_Set(go, "Components", JSON_New(JSON_ARRAY));
	JSON_Set(go, "Children", JSON_New(JSON_ARRAY));
	return go;
}

json_t *Scene_AddGameObject(scenefile_t *sf, json_t *parent, const char *name)
{
	json_t *go = Scene_NewGameObject(name);
	json_t *list = parent ? JSON_Get(parent, "Children") : Scene_Objects(sf);
	if (!list) {
		if (!parent && sf->kind == SCENEFILE_PREFAB) {
			/* a prefab has one root: new top-level objects go under it */
			list = JSON_Get(Scene_RootObject(sf), "Children");
		} else if (parent) {
			JSON_Set(parent, "Children", JSON_New(JSON_ARRAY));
			list = JSON_Get(parent, "Children");
		}
	}
	if (!list) {
		JSON_Free(go);
		return NULL;
	}
	JSON_Append(list, go);
	return go;
}

static bool type_matches(const char *have, const char *want)
{
	if (!have)
		return false;
	if (!Str_NICmp(have, "Sandbox.", 8))
		have += 8;
	if (!Str_NICmp(want, "Sandbox.", 8))
		want += 8;
	return !strcmp(have, want);
}

json_t *Scene_AddComponent(json_t *go, const char *type)
{
	json_t *list = JSON_Get(go, "Components");
	json_t *c = JSON_New(JSON_OBJECT);
	char g[40];
	if (!JSON_IsArray(list)) {
		JSON_Set(go, "Components", JSON_New(JSON_ARRAY));
		list = JSON_Get(go, "Components");
	}
	new_guid_string(g);
	JSON_SetString(c, "__type", type);
	JSON_SetString(c, "__guid", g);
	JSON_SetBool(c, "__enabled", true);
	JSON_SetNumber(c, "Flags", 0);
	JSON_Append(list, c);
	return c;
}

json_t *Scene_FindComponent(const json_t *go, const char *type)
{
	JSON_FOREACH(c, JSON_Get(go, "Components"))
		if (type_matches(JSON_GetString(c, "__type", NULL), type))
			return c;
	return NULL;
}

static json_t *find_guid(json_t *go, const char *guid)
{
	const char *g = JSON_GetString(go, "__guid", NULL);
	if (g && !Str_ICmp(g, guid))
		return go;
	JSON_FOREACH(c, JSON_Get(go, "Components")) {
		g = JSON_GetString(c, "__guid", NULL);
		if (g && !Str_ICmp(g, guid))
			return c;
	}
	JSON_FOREACH(child, JSON_Get(go, "Children")) {
		json_t *hit = find_guid(child, guid);
		if (hit)
			return hit;
	}
	return NULL;
}

json_t *Scene_FindByGuid(const scenefile_t *sf, const char *guid)
{
	if (sf->kind == SCENEFILE_PREFAB)
		return find_guid(Scene_RootObject(sf), guid);
	JSON_FOREACH(go, Scene_Objects(sf)) {
		json_t *hit = find_guid(go, guid);
		if (hit)
			return hit;
	}
	return NULL;
}

/* --- transforms ------------------------------------------------------ */

bool Scene_ParseFloats(const char *s, float *out, int n)
{
	int got = 0;
	if (!s)
		return false;
	while (got < n) {
		char *end;
		double v;
		while (*s == ' ' || *s == ',' || *s == '\t')
			s++;
		if (!*s)
			break;
		v = strtod(s, &end);
		if (end == s)
			break;
		out[got++] = (float)v;
		s = end;
	}
	return got == n;
}

static void quat_mul(const float a[4], const float b[4], float out[4])
{
	float r[4];
	r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	memcpy(out, r, sizeof(r));
}

static void quat_rotate(const float q[4], const float v[3], float out[3])
{
	/* v' = v + 2w(q x v) + 2 q x (q x v) */
	float t[3], r[3];
	t[0] = 2 * (q[1] * v[2] - q[2] * v[1]);
	t[1] = 2 * (q[2] * v[0] - q[0] * v[2]);
	t[2] = 2 * (q[0] * v[1] - q[1] * v[0]);
	r[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
	r[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
	r[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
	VectorCopy(r, out);
}

#define DEG2RAD(a) ((a) * (float)(M_PI / 180.0))
#define RAD2DEG(a) ((a) * (float)(180.0 / M_PI))

/* yaw about Z, then pitch about Y, then roll about X; positive pitch turns
 * the forward axis downwards, as in Quake and Source map angles */
void Scene_AnglesToQuat(const float angles[3], float q[4])
{
	float p = DEG2RAD(angles[0]) * 0.5f, y = DEG2RAD(angles[1]) * 0.5f, r = DEG2RAD(angles[2]) * 0.5f;
	float qy[4] = {0, 0, sinf(y), cosf(y)};
	float qp[4] = {0, sinf(p), 0, cosf(p)};
	float qr[4] = {sinf(r), 0, 0, cosf(r)};
	quat_mul(qy, qp, q);
	quat_mul(q, qr, q);
}

void Scene_QuatToAngles(const float q[4], float angles[3])
{
	float x = q[0], y = q[1], z = q[2], w = q[3];
	float fwd[3] = {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)};
	float left[3] = {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)};
	float upz = 1 - 2 * (x * x + y * y);
	float xy = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]);
	angles[0] = RAD2DEG(atan2f(-fwd[2], xy));
	if (xy > 0.001f) {
		angles[1] = RAD2DEG(atan2f(fwd[1], fwd[0]));
		angles[2] = RAD2DEG(atan2f(left[2], upz));
	} else {
		/* looking straight up or down: fold roll into yaw */
		angles[1] = RAD2DEG(atan2f(-left[0], left[1]));
		angles[2] = 0;
	}
	for (int i = 0; i < 3; i++)
		if (fabsf(angles[i]) < 1e-4f)
			angles[i] = 0;
}

void Scene_LocalTransform(const json_t *go, scene_xform_t *out)
{
	float a[4];
	const char *s;
	VectorClear(out->pos);
	out->rot[0] = out->rot[1] = out->rot[2] = 0;
	out->rot[3] = 1;
	out->scale[0] = out->scale[1] = out->scale[2] = 1;
	Scene_ParseFloats(JSON_GetString(go, "Position", NULL), out->pos, 3);
	if ((s = JSON_GetString(go, "Rotation", NULL))) {
		if (Scene_ParseFloats(s, a, 4)) {
			float len = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
			if (len > 1e-6f)
				for (int i = 0; i < 4; i++)
					out->rot[i] = a[i] / len;
		} else if (Scene_ParseFloats(s, a, 3)) {
			Scene_AnglesToQuat(a, out->rot); /* pitch,yaw,roll */
		}
	}
	if ((s = JSON_GetString(go, "Scale", NULL)) && !Scene_ParseFloats(s, out->scale, 3) && Scene_ParseFloats(s, a, 1))
		out->scale[0] = out->scale[1] = out->scale[2] = a[0];
}

void Scene_CombineTransform(const scene_xform_t *parent, const scene_xform_t *local, scene_xform_t *out)
{
	float p[3];
	scene_xform_t r;
	for (int i = 0; i < 3; i++)
		p[i] = local->pos[i] * parent->scale[i];
	quat_rotate(parent->rot, p, p);
	for (int i = 0; i < 3; i++) {
		r.pos[i] = parent->pos[i] + p[i];
		r.scale[i] = parent->scale[i] * local->scale[i];
	}
	quat_mul(parent->rot, local->rot, r.rot);
	*out = r;
}
