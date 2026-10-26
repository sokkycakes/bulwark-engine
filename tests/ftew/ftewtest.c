/* Bulwark engine - FTEW world self-tests. ISC licence, see LICENSE.
 * usage: ftewtest <tests/ftew directory> */
#include <stdio.h>
#include <stdlib.h>

#include "../../src/common/common.h"
#include "../../src/common/fs.h"
#include "../../src/common/ftew.h"
#include "../../src/server/bspfile.h"

static int checks, failures;

#define CHECK(cond)                                                              \
	do {                                                                         \
		checks++;                                                                \
		if (!(cond)) {                                                           \
			failures++;                                                          \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
		}                                                                        \
	} while (0)

static void test_parse(const uint8_t *d, size_t size)
{
	ftew_t w;
	char err[128];
	uint8_t *bad;
	int rejected = 0;

	CHECK(FTEW_IsFTEW(d, size));
	CHECK(FTEW_Parse(d, size, &w, err, sizeof(err)));
	CHECK(w.nummodels == 2 && w.nummaterials == 3 && w.nummeshes == 6 && w.numinstances == 8 && w.numcolliders == 1);
	CHECK(!strcmp(w.materials[0], "textures/fteworld/a3a3a065d67c58c3_vl"));
	CHECK(!strncmp(w.entities, "{\n\"classname\" \"worldspawn\"", 26));
	CHECK(w.colliders[0].model == 1 && w.colliders[0].numplanes >= 6);
	{
		float xyz[3], st[2], rgba[4];
		FTEW_Vertex(&w.meshes[0], 0, xyz, st, rgba);
		CHECK(xyz[0] == -384 && xyz[1] == -320 && xyz[2] == 8 && rgba[3] == 1);
		CHECK(FTEW_Index(&w.meshes[0], 0) < w.meshes[0].numverts);
	}
	FTEW_Free(&w);

	/* every truncation is rejected (the header's length no longer matches) */
	for (size_t n = 0; n < size; n++)
		if (!FTEW_Parse(d, n, &w, err, sizeof(err)))
			rejected++;
	CHECK(rejected == (int)size);

	bad = Mem_Alloc(size);
	/* wrong version */
	memcpy(bad, d, size);
	bad[4] = 2;
	CHECK(!FTEW_Parse(bad, size, &w, err, sizeof(err)) && strstr(err, "version"));
	/* an index past the end of its mesh: first mesh's first index */
	memcpy(bad, d, size);
	{
		uint32_t meshoff = bad[36] | bad[37] << 8 | bad[38] << 16 | (uint32_t)bad[39] << 24;
		uint8_t *idx = bad + meshoff + 12 + 24 * FTEW_VERTEX_SIZE;
		idx[0] = 0xff;
		idx[1] = 0xff;
		CHECK(!FTEW_Parse(bad, size, &w, err, sizeof(err)) && strstr(err, "index"));
	}
	/* a NaN vertex */
	memcpy(bad, d, size);
	{
		uint32_t meshoff = bad[36] | bad[37] << 8 | bad[38] << 16 | (uint32_t)bad[39] << 24;
		memset(bad + meshoff + 12, 0xff, 4);
		CHECK(!FTEW_Parse(bad, size, &w, err, sizeof(err)));
	}
	/* an entity lump missing its terminator */
	memcpy(bad, d, size);
	bad[size - 1] = '\n';
	CHECK(!FTEW_Parse(bad, size, &w, err, sizeof(err)) && strstr(err, "entity"));
	Mem_Free(bad);
}

static void test_collision(void)
{
	bspmap_t *m = BSP_Load("maps/editor_lab.ftew");
	bsptrace_t tr;
	const float zero[3] = {0, 0, 0}, mins[3] = {-16, -16, -36}, maxs[3] = {16, 16, 36};
	CHECK(m != NULL);
	if (!m)
		return;
	CHECK(m->type == BSP_Q3 && m->nummodels == 2);
	CHECK(m->entities && strstr(m->entities, "info_player_start"));
	CHECK(m->models[0].numbrushes > 100 && m->models[1].numbrushes == 1);
	CHECK(m->models[0].mins[2] < 0 && m->models[0].maxs[2] > 48);

	/* the player start at z 48 stands on the floor at z 0 */
	{
		float start[3] = {0, -192, 48}, end[3] = {0, -192, -1000};
		BSP_TraceModel(m, 0, zero, start, mins, maxs, end, CONTENTS_SOLID, &tr);
		CHECK(!tr.startsolid && tr.fraction < 1);
		CHECK(tr.endpos[2] > 35.9f && tr.endpos[2] < 36.1f);
		CHECK(tr.normal[2] > 0.99f);
		BSP_TraceModel(m, 0, zero, start, zero, zero, end, CONTENTS_SOLID, &tr);
		CHECK(tr.endpos[2] > -0.1f && tr.endpos[2] < 0.1f);
	}
	/* from below, the floor's underside also blocks (a thin shell) */
	{
		float start[3] = {0, -192, -40}, end[3] = {0, -192, 40};
		BSP_TraceModel(m, 0, zero, start, zero, zero, end, CONTENTS_SOLID, &tr);
		CHECK(tr.fraction < 1);
	}
	/* the trigger's collider is entity-local: model *1 placed at its origin */
	{
		float origin[3] = {0, 0, 48}, inside[3] = {0, 0, 50}, outside[3] = {0, 0, 200};
		CHECK(BSP_PointContents(m, 1, origin, inside) & CONTENTS_SOLID);
		CHECK(!(BSP_PointContents(m, 1, origin, outside) & CONTENTS_SOLID));
	}
	BSP_Free(m);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/ftew";
	char path[512];
	FILE *f;
	long n;
	uint8_t *d;

	snprintf(path, sizeof(path), "%s/maps/editor_lab.ftew", dir);
	if (!(f = fopen(path, "rb"))) {
		printf("can't open %s\n", path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = Mem_Alloc((size_t)n);
	if (fread(d, 1, (size_t)n, f) != (size_t)n)
		n = 0;
	fclose(f);

	test_parse(d, (size_t)n);
	Mem_Free(d);
	/* the test directory is a game directory under its parent */
	snprintf(path, sizeof(path), "%s/..", dir);
	FS_Init(path);
	FS_AddGameDir("ftew");
	test_collision();

	printf("%d/%d ftew checks passed\n", checks - failures, checks);
	return failures ? 1 : 0;
}
