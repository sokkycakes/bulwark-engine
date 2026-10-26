/* Bulwark engine - scene format self-tests. ISC licence, see LICENSE.
 * usage: scenetest <tests/scene directory> */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/common/common.h"
#include "../../src/common/fs.h"
#include "../../src/common/json.h"
#include "../../src/scene/scene.h"

static int checks, failures;

#define CHECK(cond)                                                              \
	do {                                                                         \
		checks++;                                                                \
		if (!(cond)) {                                                           \
			failures++;                                                          \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
		}                                                                        \
	} while (0)

static bool contains(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }

/* the { } block in entity text whose classname is cn */
static char *entity_block(const char *text, const char *cn, int nth)
{
	char want[128];
	const char *p = text;
	snprintf(want, sizeof(want), "\"classname\" \"%s\"", cn);
	while ((p = strstr(p, want))) {
		const char *s = p, *e = strchr(p, '}');
		while (s > text && *s != '{')
			s--;
		if (!nth--) {
			char *out = Mem_Alloc((size_t)(e - s) + 2);
			memcpy(out, s, (size_t)(e - s) + 1);
			return out;
		}
		p = e;
	}
	return NULL;
}

static int count_of(const char *text, const char *needle)
{
	int n = 0;
	for (const char *p = text; (p = strstr(p, needle)); p += strlen(needle))
		n++;
	return n;
}

/* --- JSON ------------------------------------------------------------ */

static void test_json(void)
{
	char err[128];
	strbuf_t sb = {0};
	const char *text = "\xef\xbb\xbf{\"a\": [1, 2.5e3, -0.25, 18446744073709551615], \"s\": \"x\\u00e9\\ud83d\\ude00\\n\\\"\","
	                   " \"o\": {}, \"e\": [], \"t\": true, \"f\": false, \"n\": null}";
	json_t *j = JSON_Parse(text, err, sizeof(err)), *k;
	CHECK(j != NULL);
	CHECK(JSON_GetNumber(JSON_At(JSON_Get(j, "a"), 0), "x", 0) == 0); /* not an object */
	CHECK(JSON_At(JSON_Get(j, "a"), 1)->num == 2500);
	CHECK(!strcmp(JSON_At(JSON_Get(j, "a"), 3)->str, "18446744073709551615"));
	CHECK(!strcmp(JSON_GetString(j, "s", ""), "x\xc3\xa9\xf0\x9f\x98\x80\n\""));
	JSON_Write(&sb, j, 2);
	k = JSON_Parse(sb.data, err, sizeof(err));
	CHECK(k && JSON_Equal(j, k));
	CHECK(contains(sb.data, "18446744073709551615")); /* big integers survive */
	CHECK(contains(sb.data, "\"o\": {}") && contains(sb.data, "\"e\": []"));
	JSON_Free(k);
	SB_Free(&sb);

	/* setting keeps order; renaming keeps position */
	JSON_SetNumber(j, "a", 5);
	JSON_Rename(j, "s", "z");
	CHECK(!strcmp(j->child->key, "a") && !strcmp(j->child->next->key, "z"));
	JSON_Remove(j, "z");
	CHECK(j->count == 6 && !JSON_Get(j, "z"));
	JSON_Free(j);

	CHECK(JSON_Parse("{\"a\": 1,\n\"b\": }", err, sizeof(err)) == NULL);
	CHECK(contains(err, "line 2"));
	CHECK(JSON_Parse("[1, 2] x", err, sizeof(err)) == NULL);
	CHECK(JSON_Parse("\"abc", err, sizeof(err)) == NULL);
}

/* --- math ------------------------------------------------------------ */

static void forward_of(const float a[3], float f[3])
{
	float r[3], u[3];
	AngleVectors(a, f, r, u);
}

static void test_angles(void)
{
	static const float cases[][3] = {{0, 0, 0}, {0, 90, 0}, {30, 45, 0}, {-60, 170, 20}, {10, -120, -45}, {89, 0, 0}};
	for (size_t i = 0; i < BW_ARRAY_COUNT(cases); i++) {
		float q[4], back[3], f1[3], f2[3];
		Scene_AnglesToQuat(cases[i], q);
		Scene_QuatToAngles(q, back);
		forward_of(cases[i], f1);
		forward_of(back, f2);
		CHECK(fabsf(f1[0] - f2[0]) < 1e-3f && fabsf(f1[1] - f2[1]) < 1e-3f && fabsf(f1[2] - f2[2]) < 1e-3f);
		if (fabsf(cases[i][0]) < 80)
			CHECK(fabsf(back[2] - cases[i][2]) < 0.01f);
	}
	/* s&box writes a 90 degree turn about Z as 0,0,0.7071068,0.7071068 */
	{
		float q[4] = {0, 0, 0.7071068f, 0.7071068f}, a[3];
		Scene_QuatToAngles(q, a);
		CHECK(fabsf(a[0]) < 0.01f && fabsf(a[1] - 90) < 0.01f && fabsf(a[2]) < 0.01f);
	}
	/* parent rotation and scale apply to the child's position */
	{
		scene_xform_t p = {{0, 0, 64}, {0, 0, 0.7071068f, 0.7071068f}, {2, 2, 2}};
		scene_xform_t l = {{32, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}}, w;
		Scene_CombineTransform(&p, &l, &w);
		CHECK(fabsf(w.pos[0]) < 0.01f && fabsf(w.pos[1] - 64) < 0.01f && fabsf(w.pos[2] - 64) < 0.01f);
		CHECK(w.scale[0] == 2);
	}
	{
		guid_t a, b, c, d;
		char s[40];
		CHECK(Guid_Parse("bfc59c12-1ed2-4f91-8956-a95a315eac3c", &a));
		Guid_Format(&a, s);
		CHECK(!strcmp(s, "bfc59c12-1ed2-4f91-8956-a95a315eac3c"));
		CHECK(Guid_Parse("BFC59C121ED24F918956A95A315EAC3C", &b) && !memcmp(&a, &b, 16));
		CHECK(!Guid_Parse("bfc59c12-1ed2-4f91-8956", &b));
		CHECK(!Guid_Parse("not a guid", &b));
		Guid_Random(&b);
		Guid_Combine(&a, &b, &c);
		Guid_Combine(&a, &b, &d);
		CHECK(!memcmp(&c, &d, 16));
		Guid_Combine(&b, &a, &d);
		CHECK(memcmp(&c, &d, 16) != 0);
	}
}

/* --- files ----------------------------------------------------------- */

static char *load_os(const char *dir, const char *name)
{
	char path[512];
	FILE *f;
	long n;
	char *buf;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (!(f = fopen(path, "rb")))
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = Mem_Alloc((size_t)n + 1);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n)
		n = 0;
	buf[n] = 0;
	fclose(f);
	return buf;
}

/* load, save, load again: the second save must match the first */
static scenefile_t *roundtrip(const char *dir, const char *name, char **saved)
{
	char *text = load_os(dir, name);
	scenefile_t *a, *b;
	strbuf_t s1 = {0}, s2 = {0};
	CHECK(text != NULL);
	a = Scene_Parse(text, name);
	Mem_Free(text);
	CHECK(a != NULL);
	if (!a)
		return NULL;
	Scene_Write(a, &s1);
	b = Scene_Parse(s1.data, name);
	CHECK(b != NULL);
	if (b) {
		Scene_Write(b, &s2);
		CHECK(s1.len == s2.len && !strcmp(s1.data, s2.data));
		CHECK(JSON_Equal(a->root, b->root));
		Scene_Free(b);
	}
	SB_Free(&s2);
	*saved = s1.data;
	return a;
}

static void test_sbox_files(const char *dir)
{
	char *saved, *orig;
	scenefile_t *sf;
	json_t *go;
	char err[128];

	/* oldest format: "Id" keys, no versions */
	sf = roundtrip(dir, "sbox/minimal_v0.scene", &saved);
	if (sf) {
		CHECK(sf->kind == SCENEFILE_SCENE);
		go = JSON_At(Scene_Objects(sf), 0);
		CHECK(!strcmp(JSON_GetString(go, "__guid", ""), "bfc59c12-1ed2-4f91-8956-a95a315eac3c"));
		CHECK(!JSON_Get(go, "Id"));
		CHECK(JSON_IsArray(JSON_Get(go, "Children")) && JSON_Get(go, "Flags"));
		CHECK(JSON_GetString(JSON_At(JSON_Get(go, "Components"), 0), "__guid", NULL) != NULL);
		CHECK(!strcmp(JSON_GetString(sf->root, "__guid", ""), "9f832399-4887-46b0-8f21-2ee284f538e2"));
		CHECK(JSON_GetNumber(sf->root, "__version", 0) == SCENE_FILE_VERSION);
		/* unknown component data is kept */
		CHECK(contains(saved, "\"BodyGroups\": 18446744073709551615"));
		CHECK(contains(saved, "light_test_sky_sunny03.vmat"));
		Scene_Free(sf);
		Mem_Free(saved);
	}

	/* current format: saving must not change the file's content */
	sf = roundtrip(dir, "sbox/example_v3.scene", &saved);
	if (sf) {
		json_t *a, *b;
		orig = load_os(dir, "sbox/example_v3.scene");
		a = JSON_Parse(orig, err, sizeof(err));
		b = JSON_Parse(saved, err, sizeof(err));
		CHECK(a && b && JSON_Equal(a, b));
		JSON_Free(a);
		JSON_Free(b);
		Mem_Free(orig);
		Scene_Free(sf);
		Mem_Free(saved);
	}

	/* prefabs at v1 and v2 */
	sf = roundtrip(dir, "sbox/light_point_v1.prefab", &saved);
	if (sf) {
		go = Scene_RootObject(sf);
		CHECK(sf->kind == SCENEFILE_PREFAB);
		CHECK(!strcmp(JSON_GetString(go, "Name", ""), "light - point"));
		CHECK(Scene_FindComponent(go, "PointLight") != NULL);
		CHECK(JSON_GetNumber(sf->root, "__version", 0) == PREFAB_FILE_VERSION);
		CHECK(contains(saved, "\"MenuPath\": \"Light/Point Light\""));
		Scene_Free(sf);
		Mem_Free(saved);
	}
	sf = roundtrip(dir, "sbox/my_entity_v2.prefab", &saved);
	if (sf) {
		CHECK(Scene_FindByGuid(sf, "bb98fe1d-a032-48d6-9587-efc53f9eb82e") != NULL);
		Scene_Free(sf);
		Mem_Free(saved);
	}

	/* s&box stand-ins: its SpawnPoint becomes a player start */
	{
		char *text = load_os(dir, "sbox/example_v3.scene");
		scenefile_t *ex = Scene_Parse(text, "example.scene");
		strbuf_t ents = {0};
		scene_entinfo_t info;
		Mem_Free(text);
		Scene_BuildEntities(ex, NULL, &ents, &info);
		CHECK(contains(ents.data, "\"classname\" \"info_player_start\""));
		CHECK(info.numentities == 1 && info.numskipped > 0);
		SB_Free(&ents);
		Scene_Free(ex);
	}

	CHECK(Scene_Parse("[1, 2]", "bad.scene") == NULL);
	CHECK(Scene_Parse("{\"RootObject\": 5}", "bad.prefab") == NULL);
}

static void test_building(void)
{
	scenefile_t *sf = Scene_New(SCENEFILE_SCENE, "built.scene"), *back;
	json_t *a = Scene_AddGameObject(sf, NULL, "a");
	json_t *b = Scene_AddGameObject(sf, a, "b");
	json_t *c = Scene_AddComponent(b, "Bulwark.Entity");
	strbuf_t sb = {0};
	JSON_SetString(c, "classname", "info_notnull");
	CHECK(Scene_FindByGuid(sf, JSON_GetString(c, "__guid", "")) == c);
	CHECK(Scene_FindComponent(b, "Sandbox.Bulwark.Entity") == c);
	Scene_Write(sf, &sb);
	back = Scene_Parse(sb.data, "built.scene");
	CHECK(back && JSON_Equal(back->root, sf->root));
	CHECK(contains(sb.data, "\"__version\": 3"));
	Scene_Free(back);
	Scene_Free(sf);
	SB_Free(&sb);
}

/* --- prefabs and entities -------------------------------------------- */

static void test_prefabs(void)
{
	scene_prefabcache_t *pc = Scene_NewPrefabCache();
	scenefile_t *sf = Scene_Load("maps/test.scene"), *res;
	json_t *a, *b, *lid_a, *lid_b, *glow;
	strbuf_t ents = {0};
	scene_entinfo_t info;
	char *blk;

	CHECK(sf != NULL);
	if (!sf)
		return;
	res = Scene_Resolve(sf, Scene_PrefabCacheLoad, pc);
	/* the source is not changed */
	CHECK(JSON_Get(JSON_At(Scene_Objects(sf), 1), "__Prefab") != NULL);

	a = Scene_FindByGuid(res, "aaaaaaaa-0000-4000-8000-000000000001");
	b = Scene_FindByGuid(res, "bbbbbbbb-0000-4000-8000-000000000001");
	CHECK(a && b);
	if (a && b) {
		CHECK(!JSON_Get(a, "__Prefab"));
		CHECK(!strcmp(JSON_GetString(a, "Name", ""), "crate"));
		CHECK(!strcmp(JSON_GetString(a, "Position", ""), "100,0,0"));
		CHECK(!strcmp(JSON_GetString(b, "Position", ""), "-100,0,0"));
		/* mapped component GUID and its overridden key */
		CHECK(!strcmp(JSON_GetString(Scene_FindByGuid(res, "aaaaaaaa-0000-4000-8000-000000000002"), "model", ""),
		              "models/crate_big.iqm"));
		/* the decal was removed from a only; the glow was added after the lid */
		CHECK(JSON_Get(a, "Children")->count == 2);
		CHECK(JSON_Get(b, "Children")->count == 2);
		lid_a = JSON_At(JSON_Get(a, "Children"), 0);
		glow = JSON_At(JSON_Get(a, "Children"), 1);
		lid_b = JSON_At(JSON_Get(b, "Children"), 0);
		CHECK(!strcmp(JSON_GetString(lid_a, "Name", ""), "lid") && !strcmp(JSON_GetString(glow, "Name", ""), "glow"));
		CHECK(!strcmp(JSON_GetString(JSON_At(JSON_Get(b, "Children"), 1), "Name", ""), "decal"));
		/* unmapped prefab GUIDs get distinct, stable instance GUIDs */
		CHECK(strcmp(JSON_GetString(lid_a, "__guid", ""), JSON_GetString(lid_b, "__guid", "")));
		CHECK(strcmp(JSON_GetString(lid_a, "__guid", ""), "11111111-0000-4000-8000-000000000003"));
		/* the reference inside the prefab follows the rename */
		{
			json_t *ent = Scene_FindComponent(a, "Bulwark.Entity");
			CHECK(!strcmp(JSON_GetString(JSON_Get(ent, "target"), "go", ""), JSON_GetString(lid_a, "__guid", "")));
		}
	}
	{
		/* same input, same GUIDs */
		scenefile_t *res2 = Scene_Resolve(sf, Scene_PrefabCacheLoad, pc);
		CHECK(JSON_Equal(res->root, res2->root));
		Scene_Free(res2);
	}

	Scene_BuildEntities(res, "{\n\"classname\" \"worldspawn\"\n}\n{\n\"classname\" \"light\"\n}\n", &ents, &info);
	CHECK(!strcmp(info.mapname, "maps/testmap.bsp"));
	CHECK(info.spawnmapentities);
	CHECK(contains(ents.data, "\"classname\" \"worldspawn\"") && contains(ents.data, "\"classname\" \"light\"\n"));
	CHECK(!contains(ents.data, "should_not_spawn"));
	/* 2 crates x (prop + lid) + glow + door + button + barrel */
	CHECK(info.numentities == 8);
	CHECK(count_of(ents.data, "\"classname\" \"prop_physics\"") == 3);

	blk = entity_block(ents.data, "func_door", 0);
	CHECK(contains(blk, "\"origin\" \"0 0 64\""));
	CHECK(contains(blk, "\"angles\" \"0 90 0\""));
	CHECK(contains(blk, "\"targetname\" \"dddddddd-0000-4000-8000-000000000001\""));
	CHECK(contains(blk, "\"speed\" \"100\"") && contains(blk, "\"spawnflags\" \"1\"") && contains(blk, "\"locked\" \"0\""));
	CHECK(contains(blk, "\"message\" \"say 'hi'\""));
	CHECK(!contains(blk, "__guid") && !contains(blk, "\"Flags\""));
	Mem_Free(blk);

	blk = entity_block(ents.data, "func_button", 0);
	CHECK(contains(blk, "\"origin\" \"0 32 64\""));
	CHECK(contains(blk, "\"targetname\" \"door_button\""));
	CHECK(contains(blk, "\"parentname\" \"dddddddd-0000-4000-8000-000000000001\""));
	CHECK(contains(blk, "\"target\" \"dddddddd-0000-4000-8000-000000000001\""));
	Mem_Free(blk);

	blk = entity_block(ents.data, "light_dynamic", 0);
	CHECK(contains(blk, "\"origin\" \"100 0 8\""));
	CHECK(contains(blk, "\"_light\" \"255 128 0 200\""));
	CHECK(contains(blk, "\"parentname\" \"aaaaaaaa-0000-4000-8000-000000000001\""));
	Mem_Free(blk);

	blk = entity_block(ents.data, "info_null", 1); /* crate b's lid */
	CHECK(contains(blk, "\"origin\" \"-100 0 16\""));
	CHECK(contains(blk, "\"parentname\" \"bbbbbbbb-0000-4000-8000-000000000001\""));
	Mem_Free(blk);

	/* s&box model stand-in */
	blk = entity_block(ents.data, "prop_physics", 2);
	CHECK(contains(blk, "\"model\" \"models/barrel.iqm\""));
	CHECK(contains(blk, "\"scale\" \"2\""));
	CHECK(contains(blk, "\"rendercolor\" \"255 0 0\""));
	Mem_Free(blk);

	SB_Free(&ents);
	Scene_Free(res);

	/* nested prefab: shelf holds a crate */
	{
		const scenefile_t *shelf = Scene_PrefabCacheLoad("prefabs/shelf.prefab", pc);
		scenefile_t *r = Scene_Resolve(shelf, Scene_PrefabCacheLoad, pc);
		json_t *crate = JSON_At(JSON_Get(Scene_RootObject(r), "Children"), 0);
		CHECK(!strcmp(JSON_GetString(crate, "Name", ""), "crate"));
		CHECK(!strcmp(JSON_GetString(crate, "Position", ""), "0,0,32"));
		CHECK(!strcmp(JSON_GetString(crate, "__guid", ""), "22222222-0000-4000-8000-000000000002"));
		CHECK(JSON_Get(crate, "Children")->count == 2);
		Scene_Free(r);
	}

	/* a prefab that contains itself stops instead of recursing forever */
	{
		const scenefile_t *loop = Scene_PrefabCacheLoad("prefabs/loop.prefab", pc);
		scenefile_t *r = Scene_Resolve(loop, Scene_PrefabCacheLoad, pc);
		CHECK(r != NULL);
		Scene_Free(r);
	}

	/* missing prefab: a disabled stub that keeps its GUID */
	{
		scenefile_t *m = Scene_Parse("{\"GameObjects\": [{\"__guid\": \"12345678-0000-4000-8000-000000000001\","
		                             " \"__Prefab\": \"prefabs/nope.prefab\", \"__PrefabInstancePatch\": {}}], \"__version\": 3}",
		                             "missing.scene");
		scenefile_t *r = Scene_Resolve(m, Scene_PrefabCacheLoad, pc);
		json_t *stub = JSON_At(Scene_Objects(r), 0);
		CHECK(!JSON_GetBool(stub, "Enabled", true));
		CHECK(contains(JSON_GetString(stub, "Name", ""), "Missing Prefab"));
		CHECK(!strcmp(JSON_GetString(stub, "__guid", ""), "12345678-0000-4000-8000-000000000001"));
		Scene_Free(r);
		Scene_Free(m);
	}

	Scene_Free(sf);
	Scene_FreePrefabCache(pc);
}

static void test_from_entities(void)
{
	const char *lump = "{\n\"classname\" \"worldspawn\"\n\"message\" \"test\"\n}\n"
	                   "{\n\"classname\" \"info_player_start\"\n\"origin\" \"16 -32 24\"\n\"angle\" \"90\"\n}\n"
	                   "{\n\"classname\" \"func_door\"\n\"model\" \"*1\"\n\"targetname\" \"door1\"\n\"angle\" \"-1\"\n\"speed\" \"50\"\n}\n"
	                   "{\n\"classname\" \"light\"\n\"origin\" \"0 0 128\"\n\"angles\" \"-30 45 0\"\n\"_light\" \"255 255 255 300\"\n}\n";
	scenefile_t *sf = Scene_FromEntities(lump, "maps/e1m1.bsp", "e1m1.scene"), *back;
	strbuf_t text = {0}, ents = {0};
	scene_entinfo_t info;
	char *blk;

	CHECK(Scene_Objects(sf)->count == 4); /* map + 3 entities, worldspawn stays in the BSP */
	Scene_Write(sf, &text);
	back = Scene_Parse(text.data, "e1m1.scene");
	CHECK(back != NULL);
	Scene_BuildEntities(back, lump, &ents, &info);
	CHECK(!strcmp(info.mapname, "maps/e1m1.bsp") && !info.spawnmapentities);
	CHECK(count_of(ents.data, "\"classname\" \"worldspawn\"") == 1);
	CHECK(count_of(ents.data, "\"classname\" \"info_player_start\"") == 1);
	CHECK(info.numentities == 3);
	blk = entity_block(ents.data, "info_player_start", 0);
	CHECK(contains(blk, "\"origin\" \"16 -32 24\"") && contains(blk, "\"angles\" \"0 90 0\""));
	Mem_Free(blk);
	blk = entity_block(ents.data, "func_door", 0);
	CHECK(contains(blk, "\"model\" \"*1\"") && contains(blk, "\"angle\" \"-1\"") && contains(blk, "\"targetname\" \"door1\""));
	CHECK(!contains(blk, "\"angles\""));
	Mem_Free(blk);
	blk = entity_block(ents.data, "light", 0);
	CHECK(contains(blk, "\"angles\" \"-30 45 0\"") && contains(blk, "\"origin\" \"0 0 128\""));
	Mem_Free(blk);
	Scene_Free(back);
	Scene_Free(sf);
	SB_Free(&text);
	SB_Free(&ents);

	/* MapInstance may name an FTEW world */
	{
		scenefile_t *f = Scene_Parse("{\"GameObjects\": [{\"Name\": \"Map\", \"Components\": [{\"__type\": \"Sandbox.MapInstance\","
		                             " \"MapName\": \"editor_lab.ftew\"}]}], \"__version\": 3}",
		                             "ftew.scene");
		Scene_MapInfo(f, &info);
		CHECK(!strcmp(info.mapname, "maps/editor_lab.ftew"));
		Scene_Free(f);
	}
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/scene";
	FS_Init(dir);
	FS_AddGameDir("data");

	test_json();
	test_angles();
	test_sbox_files(dir);
	test_building();
	test_prefabs();
	test_from_entities();

	printf("%d/%d scene checks passed\n", checks - failures, checks);
	return failures ? 1 : 0;
}
