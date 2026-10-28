/* Bulwark engine - scene and prefab files. ISC licence, see LICENSE.
 *
 * The file format is s&box's JSON scene format (Facepunch/sbox-public, MIT),
 * so scenes and prefabs can be shared with its editor. Files are kept as
 * JSON trees: anything this engine does not understand is written back
 * untouched. See docs/spec/scene-format.md. */
#ifndef BW_SCENE_H
#define BW_SCENE_H

#include "../common/json.h"

/* versions written on save; older files are upgraded on load */
#define SCENE_FILE_VERSION 3
#define PREFAB_FILE_VERSION 2
#define GAMEOBJECT_VERSION 2

/* GameObject "Flags" bits that affect loading */
#define GOFLAG_EDITORONLY 2048
#define GOFLAG_ABSOLUTE 4096

typedef enum {
	SCENEFILE_SCENE,  /* top level "GameObjects" array */
	SCENEFILE_PREFAB  /* top level "RootObject" */
} scenefile_kind_t;

typedef struct {
	scenefile_kind_t kind;
	json_t *root;
	char name[128];
} scenefile_t;

/* kind is taken from the name's extension (.prefab) or, failing that, the
 * file's contents. Returns NULL and prints why on error. */
scenefile_t *Scene_Parse(const char *text, const char *name);
scenefile_t *Scene_Load(const char *path); /* through the file system */
scenefile_t *Scene_New(scenefile_kind_t kind, const char *name);
void Scene_Free(scenefile_t *sf);
/* writes the file at the current format version */
void Scene_Write(const scenefile_t *sf, strbuf_t *out);

/* the scene's top level GameObjects, or a prefab's root object */
json_t *Scene_Objects(const scenefile_t *sf);
json_t *Scene_RootObject(const scenefile_t *sf);

/* --- building scenes ------------------------------------------------- */

/* GUIDs are written as s&box does: lowercase 8-4-4-4-12 */
typedef struct {
	uint8_t b[16];
} guid_t;
bool Guid_Parse(const char *s, guid_t *out);
void Guid_Format(const guid_t *g, char *out); /* out holds at least 37 chars */
void Guid_Random(guid_t *g);
/* a stable GUID made from two others, used to name objects of a prefab
 * instance that the file has no mapping for */
void Guid_Combine(const guid_t *a, const guid_t *b, guid_t *out);

json_t *Scene_NewGameObject(const char *name);
json_t *Scene_AddGameObject(scenefile_t *sf, json_t *parent, const char *name); /* parent NULL: top level */
json_t *Scene_AddComponent(json_t *go, const char *type);
json_t *Scene_FindComponent(const json_t *go, const char *type); /* type match ignores a "Sandbox." prefix */
/* lookup by __guid anywhere under the scene (GameObjects and components) */
json_t *Scene_FindByGuid(const scenefile_t *sf, const char *guid);

/* --- prefabs --------------------------------------------------------- */

/* returns a prefab for a path such as "prefabs/door.prefab"; the scene code
 * does not free it. NULL if it cannot be loaded. */
typedef const scenefile_t *(*scene_prefab_fn)(const char *path, void *ctx);

/* a prefab loader that reads through the file system and caches what it loads */
typedef struct scene_prefabcache_s scene_prefabcache_t;
scene_prefabcache_t *Scene_NewPrefabCache(void);
void Scene_FreePrefabCache(scene_prefabcache_t *pc);
const scenefile_t *Scene_PrefabCacheLoad(const char *path, void *pc);
/* adds an already loaded prefab under a path (tests, tools); takes ownership */
void Scene_PrefabCacheAdd(scene_prefabcache_t *pc, const char *path, scenefile_t *sf);

/* returns a copy of sf with every prefab instance expanded into plain
 * GameObjects: the prefab's objects, the instance's patch applied, and the
 * prefab's GUIDs swapped for the instance's. Missing prefabs become disabled
 * objects and a warning. */
scenefile_t *Scene_Resolve(const scenefile_t *sf, scene_prefab_fn load, void *ctx);

/* --- transforms ------------------------------------------------------ */

typedef struct {
	float pos[3];
	float rot[4]; /* quaternion x, y, z, w */
	float scale[3];
} scene_xform_t;

void Scene_LocalTransform(const json_t *go, scene_xform_t *out);
void Scene_CombineTransform(const scene_xform_t *parent, const scene_xform_t *local, scene_xform_t *out);
/* quaternion to Quake/Source angles: pitch (positive looks down), yaw, roll, in degrees */
void Scene_QuatToAngles(const float q[4], float angles[3]);
void Scene_AnglesToQuat(const float angles[3], float q[4]);
/* reads "x,y,z" (commas or spaces) into n floats; false if fewer were found */
bool Scene_ParseFloats(const char *s, float *out, int n);

/* --- QuakeC entities ------------------------------------------------- */

typedef struct {
	char mapname[128];     /* BSP named by the scene's MapInstance, "" if none */
	bool spawnmapentities; /* false: only the BSP's worldspawn spawns */
	int numentities;       /* entity blocks written from the scene */
	int numskipped;        /* enabled components with no entity mapping */
} scene_entinfo_t;

/* Finds the scene's MapInstance. Call before Scene_BuildEntities to learn
 * which BSP to load. */
void Scene_MapInfo(const scenefile_t *resolved, scene_entinfo_t *info);

/* Writes entity lump text ({ "key" "value" } blocks) for the server's normal
 * map spawning path: the BSP's entities first (mapentities, may be NULL),
 * all of them or only worldspawn as the MapInstance asks, then one block
 * per entity component in the scene. */
void Scene_BuildEntities(const scenefile_t *resolved, const char *mapentities, strbuf_t *out, scene_entinfo_t *info);

/* Turns an entity lump into a scene: a MapInstance for mapname (which then
 * spawns only its worldspawn), and one GameObject per other entity holding a
 * Bulwark.Entity component with the entity's keys. origin and angles move
 * into the GameObject's transform. */
scenefile_t *Scene_FromEntities(const char *entities, const char *mapname, const char *name);

#endif
