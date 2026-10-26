/* Bulwark engine - scene file tool. ISC licence, see LICENSE. */
#include <stdio.h>
#include <stdlib.h>

#include "../src/common/common.h"
#include "../src/common/fs.h"
#include "../src/scene/scene.h"
#include "../src/server/bspfile.h"

static void usage(void)
{
	printf("usage: bwscene [-basedir dir] [-game dir]... command args\n"
	       "  check <file>              load, expand prefabs and list what would spawn\n"
	       "  ents <file>               print the entity text a scene spawns (BSP entities left out)\n"
	       "  resave <in> <out>         load and save, upgrading to the current format\n"
	       "  frombsp <maps/x.bsp> <out> make a scene holding the map's entities\n"
	       "Files are OS paths; prefabs and BSPs are found through -basedir and -game.\n");
}

static char *read_os(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	char *buf;
	if (!f) {
		fprintf(stderr, "can't open %s\n", path);
		return NULL;
	}
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

static bool write_os(const char *path, const strbuf_t *sb)
{
	FILE *f = fopen(path, "wb");
	bool ok;
	if (!f) {
		fprintf(stderr, "can't write %s\n", path);
		return false;
	}
	ok = fwrite(sb->data, 1, sb->len, f) == sb->len;
	ok = fputc('\n', f) != EOF && ok;
	fclose(f);
	return ok;
}

static scenefile_t *load_os(const char *path)
{
	char *text = read_os(path);
	scenefile_t *sf;
	if (!text)
		return NULL;
	sf = Scene_Parse(text, path);
	Mem_Free(text);
	return sf;
}

static int build(const char *path, bool print)
{
	scenefile_t *sf = load_os(path), *res;
	scene_prefabcache_t *pc;
	strbuf_t ents = {0};
	scene_entinfo_t info;
	if (!sf)
		return 1;
	pc = Scene_NewPrefabCache();
	res = Scene_Resolve(sf, Scene_PrefabCacheLoad, pc);
	Scene_BuildEntities(res, NULL, &ents, &info);
	if (print) {
		fputs(ents.data ? ents.data : "", stdout);
	} else {
		printf("%s: %s\n", path, sf->kind == SCENEFILE_SCENE ? "scene" : "prefab");
		printf("map: %s%s\n", info.mapname[0] ? info.mapname : "(none)",
		       info.mapname[0] && !info.spawnmapentities ? " (worldspawn only)" : "");
		printf("%d entities, %d components with no entity\n", info.numentities, info.numskipped);
	}
	SB_Free(&ents);
	Scene_Free(res);
	Scene_Free(sf);
	Scene_FreePrefabCache(pc);
	return 0;
}

int main(int argc, char **argv)
{
	const char *basedir = ".";
	const char *games[16];
	int numgames = 0, i;
	const char *cmd;

	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-basedir") && i + 1 < argc)
			basedir = argv[++i];
		else if (!strcmp(argv[i], "-game") && i + 1 < argc && numgames < 16)
			games[numgames++] = argv[++i];
		else {
			usage();
			return 1;
		}
	}
	if (i >= argc) {
		usage();
		return 1;
	}
	FS_Init(basedir);
	for (int g = 0; g < numgames; g++)
		FS_AddGameDir(games[g]);
	cmd = argv[i++];

	if (!strcmp(cmd, "check") && i + 1 == argc)
		return build(argv[i], false);
	if (!strcmp(cmd, "ents") && i + 1 == argc)
		return build(argv[i], true);
	if (!strcmp(cmd, "resave") && i + 2 == argc) {
		scenefile_t *sf = load_os(argv[i]);
		strbuf_t out = {0};
		bool ok;
		if (!sf)
			return 1;
		Scene_Write(sf, &out);
		ok = write_os(argv[i + 1], &out);
		SB_Free(&out);
		Scene_Free(sf);
		return ok ? 0 : 1;
	}
	if (!strcmp(cmd, "frombsp") && i + 2 == argc) {
		bspmap_t *bsp = BSP_Load(argv[i]);
		scenefile_t *sf;
		strbuf_t out = {0};
		bool ok;
		if (!bsp) {
			fprintf(stderr, "can't load %s\n", argv[i]);
			return 1;
		}
		sf = Scene_FromEntities(bsp->entities, argv[i], argv[i + 1]);
		Scene_Write(sf, &out);
		ok = write_os(argv[i + 1], &out);
		printf("%s: %d entities\n", argv[i + 1], Scene_Objects(sf)->count - 1);
		SB_Free(&out);
		Scene_Free(sf);
		BSP_Free(bsp);
		return ok ? 0 : 1;
	}
	usage();
	return 1;
}
