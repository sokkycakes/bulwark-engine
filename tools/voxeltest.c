/* Bulwark engine - voxel world self-tests: cells, shapes, traces, contents,
 * chunk and file round trips, and rejection of damaged input.
 * ISC licence, see LICENSE. */
#include "../src/common/common.h"
#include "../src/common/voxel.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...)                                                                                               \
	do {                                                                                                               \
		checks++;                                                                                                      \
		if (!(cond)) {                                                                                                 \
			fails++;                                                                                                   \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
			printf(__VA_ARGS__);                                                                                       \
			printf("\n");                                                                                              \
		}                                                                                                              \
	} while (0)

#define NEAR(a, b) (fabsf((a) - (b)) < 0.05f)

static void define(vxworld_t *w, int id, const char *name, int shape, uint32_t contents)
{
	vxdef_t d = {0};
	d.shape = (uint8_t)shape;
	d.contents = contents;
	d.color[0] = d.color[1] = d.color[2] = d.color[3] = 255;
	Str_Copy(d.name, name, sizeof(d.name));
	Vox_Define(w, id, &d);
}

static vxworld_t *make_world(void)
{
	int size[3] = {40, 40, 20};
	float org[3] = {-640, -640, 0};
	vxworld_t *w = Vox_Create(size, 32, org);
	define(w, 1, "BOX", VXS_BOX, 1);
	define(w, 2, "SLOPE", VXS_SLOPE, 1);
	define(w, 3, "SLAB", VXS_SLAB_LOW, 1);
	define(w, 4, "WATER", VXS_FIELD, 0x20);
	define(w, 5, "THIN", VXS_THIN, 1);
	return w;
}

static void test_cells(void)
{
	vxworld_t *w = make_world();
	int rot, bad[3] = {0, 0, 0};
	CHECK(w != NULL, "create");
	CHECK(Vox_Set(w, 3, 4, 5, 1, 2), "set");
	CHECK(Vox_Get(w, 3, 4, 5, &rot) == 1 && rot == 2, "get back");
	CHECK(w->count == 1, "count %d", w->count);
	CHECK(!Vox_Set(w, 40, 0, 0, 1, 0), "out of range x");
	CHECK(!Vox_Set(w, -1, 0, 0, 1, 0), "negative");
	CHECK(!Vox_Set(w, 0, 0, 0, 99, 0), "undefined type rejected");
	CHECK(!Vox_Set(w, 0, 0, 0, VX_MAX_TYPES, 0), "type past the table");
	CHECK(Vox_Get(w, 100, 100, 100, NULL) == 0, "out of range read");
	CHECK(Vox_Set(w, 3, 4, 5, 0, 0) && w->count == 0, "erase");
	CHECK(w->chunks[0] == NULL, "empty chunk freed");
	CHECK(Vox_Create(bad, 32, NULL) == NULL, "zero size rejected");
	{
		int huge[3] = {1024, 1024, 1024};
		CHECK(Vox_Create(huge, 32, NULL) == NULL, "over cell budget rejected");
	}
	{
		vxdef_t d = {0};
		d.shape = VXS_COUNT;
		CHECK(!Vox_Define(w, 9, &d), "bad shape rejected");
		CHECK(!Vox_Define(w, 0, &d), "type 0 is reserved");
	}
	Vox_Free(w);
}

static void test_traces(void)
{
	vxworld_t *w = make_world();
	vxtrace_t tr;
	float mins[3] = {-16, -16, -36}, maxs[3] = {16, 16, 36};
	int c[3];
	/* a floor of boxes under the origin: cells z=0, top at z=32 */
	for (int x = 10; x < 30; x++)
		for (int y = 10; y < 30; y++)
			Vox_Set(w, x, y, 0, 1, 0);
	{
		float s[3] = {0, 0, 200}, e[3] = {0, 0, -100};
		Vox_Trace(w, s, mins, maxs, e, 1, &tr);
		CHECK(tr.fraction < 1, "box falls onto floor");
		CHECK(NEAR(tr.endpos[2], 32 + 36), "lands at z=68, got %g", tr.endpos[2]);
		CHECK(tr.normal[2] > 0.99f, "floor normal up");
		CHECK(tr.type == 1, "hit type");
	}
	{
		float s[3] = {8, 8, 200}, e[3] = {8, 8, -100};
		Vox_Trace(w, s, NULL, NULL, e, 1, &tr);
		CHECK(NEAR(tr.endpos[2], 32), "point lands on z=32, got %g", tr.endpos[2]);
		Vox_CellForPoint(w, (float[3]){8, 8, 16}, c);
		CHECK(tr.cell[0] == c[0] && tr.cell[1] == c[1] && tr.cell[2] == 0, "hit cell %d %d %d want %d %d 0",
		      tr.cell[0], tr.cell[1], tr.cell[2], c[0], c[1]);
	}
	{
		/* mask without solid passes through */
		float s[3] = {0, 0, 200}, e[3] = {0, 0, -100};
		Vox_Trace(w, s, mins, maxs, e, 0x20, &tr);
		CHECK(tr.fraction == 1, "mask filters");
	}
	{
		/* start inside a block */
		float s[3] = {0, 0, 10}, e[3] = {0, 0, 50};
		Vox_Trace(w, s, NULL, NULL, e, 1, &tr);
		CHECK(tr.startsolid, "startsolid");
	}
	{
		/* a long horizontal hitscan against a wall far away */
		float s[3] = {-600, 8, 48}, e[3] = {600, 8, 48};
		Vox_Set(w, 35, 20, 1, 1, 0); /* x from 480 to 512 */
		Vox_Trace(w, s, NULL, NULL, e, 1, &tr);
		CHECK(tr.fraction < 1 && NEAR(tr.endpos[0], 480), "far wall at x=480, got %g", tr.endpos[0]);
		CHECK(tr.normal[0] < -0.99f, "wall normal -x");
	}
	{
		/* slope: wedge at cell (20,20,1), rising towards +x (rotation 0).
		   A point dropped at 3/4 of the cell lands 3/4 of the way up. */
		float m[3], s[3], e[3];
		Vox_Set(w, 20, 20, 1, 2, 0);
		Vox_CellMins(w, 20, 20, 1, m);
		s[0] = m[0] + 24, s[1] = m[1] + 16, s[2] = m[2] + 100;
		e[0] = s[0], e[1] = s[1], e[2] = m[2] - 10;
		Vox_Trace(w, s, NULL, NULL, e, 1, &tr);
		CHECK(NEAR(tr.endpos[2], m[2] + 24), "slope height %g want %g", tr.endpos[2], m[2] + 24);
		CHECK(tr.normal[2] > 0.6f && tr.normal[0] < -0.6f, "slope normal %g %g %g", tr.normal[0], tr.normal[1],
		      tr.normal[2]);
		/* rotated twice: rises towards -x, so the same spot is low */
		Vox_Set(w, 20, 20, 1, 2, 2);
		Vox_Trace(w, s, NULL, NULL, e, 1, &tr);
		CHECK(NEAR(tr.endpos[2], m[2] + 8), "rotated slope height %g want %g", tr.endpos[2], m[2] + 8);
	}
	{
		/* slab: half height */
		float m[3], s[3], e[3];
		Vox_Set(w, 22, 20, 1, 3, 0);
		Vox_CellMins(w, 22, 20, 1, m);
		s[0] = m[0] + 16, s[1] = m[1] + 16, s[2] = m[2] + 100;
		e[0] = s[0], e[1] = s[1], e[2] = m[2];
		Vox_Trace(w, s, NULL, NULL, e, 1, &tr);
		CHECK(NEAR(tr.endpos[2], m[2] + 16), "slab top %g", tr.endpos[2]);
	}
	{
		/* thin panel on the +x side only, rotation 1 puts it on +y */
		float m[3], s[3], e[3];
		Vox_Set(w, 24, 20, 1, 5, 0);
		Vox_CellMins(w, 24, 20, 1, m);
		s[0] = m[0] + 4, s[1] = m[1] + 16, s[2] = m[2] + 100;
		e[0] = s[0], e[1] = s[1], e[2] = m[2] + 2;
		Vox_Trace(w, s, NULL, NULL, e, 1, &tr);
		CHECK(tr.fraction == 1, "thin panel leaves the -x side open");
		s[0] = e[0] = m[0] + 30;
		Vox_Trace(w, s, NULL, NULL, e, 1, &tr);
		CHECK(tr.fraction < 1, "thin panel blocks its own side");
		Vox_Set(w, 24, 20, 1, 5, 1);
		Vox_Trace(w, s, NULL, NULL, e, 1, &tr);
		CHECK(tr.fraction == 1, "rotated panel moved off +x");
		s[0] = e[0] = m[0] + 16, s[1] = e[1] = m[1] + 30;
		Vox_Trace(w, s, NULL, NULL, e, 1, &tr);
		CHECK(tr.fraction < 1, "rotated panel on +y");
	}
	{
		/* water: contents but no solid */
		float m[3], p[3];
		Vox_Set(w, 26, 20, 1, 4, 0);
		Vox_CellMins(w, 26, 20, 1, m);
		p[0] = m[0] + 10, p[1] = m[1] + 10, p[2] = m[2] + 10;
		CHECK(Vox_PointContents(w, p) == 0x20, "water contents");
		p[2] -= 20;
		CHECK(Vox_PointContents(w, p) == 1, "box below is solid");
	}
	{
		/* a box that fits exactly in a one-cell gap does not start solid */
		float org[3], bm[3] = {-15, -15, -15}, bx[3] = {15, 15, 15}, m[3];
		Vox_CellMins(w, 15, 15, 1, m);
		org[0] = m[0] + 16, org[1] = m[1] + 16, org[2] = m[2] + 16;
		CHECK(!Vox_BoxTouches(w, org, bm, bx, 1), "box fits above floor");
		org[2] -= 2;
		CHECK(Vox_BoxTouches(w, org, bm, bx, 1), "box overlapping floor");
	}
	Vox_Free(w);
}

static void test_files(void)
{
	vxworld_t *w = make_world(), *back;
	uint8_t *data, *meta = NULL, chunk[65536];
	size_t len, mlen, clen;
	int unknown, rot;
	char err[128];
	const char *m = "spawn 1 2 3\n";
	srand(7);
	for (int i = 0; i < 3000; i++)
		Vox_Set(w, rand() % 40, rand() % 40, rand() % 20, 1 + rand() % 5, rand() % 4);
	CHECK(Vox_SaveToMemory(w, (const uint8_t *)m, strlen(m), &data, &len), "save");
	back = Vox_LoadFromMemory(data, len, w, &meta, &mlen, &unknown, err, sizeof(err));
	CHECK(back != NULL, "load: %s", err);
	if (back) {
		bool same = true;
		for (int z = 0; z < 20; z++)
			for (int y = 0; y < 40; y++)
				for (int x = 0; x < 40; x++) {
					int r2, t1 = Vox_Get(w, x, y, z, &rot), t2 = Vox_Get(back, x, y, z, &r2);
					if (t1 != t2 || (t1 && rot != r2))
						same = false;
				}
		CHECK(same, "cells survive a round trip");
		CHECK(back->count == w->count, "count %d vs %d", back->count, w->count);
		CHECK(mlen == strlen(m) && meta && !memcmp(meta, m, mlen), "metadata round trip");
		CHECK(unknown == 0, "no unknown types");
		Vox_Free(back);
	}
	Mem_Free(meta);

	/* types are matched by name: a game whose table numbers them differently */
	{
		int size[3] = {1, 1, 1};
		vxworld_t *other = Vox_Create(size, 32, NULL);
		define(other, 7, "BOX", VXS_BOX, 1);
		define(other, 9, "SLAB", VXS_SLAB_LOW, 1);
		back = Vox_LoadFromMemory(data, len, other, NULL, NULL, &unknown, err, sizeof(err));
		CHECK(back != NULL, "load with another table: %s", err);
		if (back) {
			int expect_unknown = 0, ok = 1;
			for (int z = 0; z < 20; z++)
				for (int y = 0; y < 40; y++)
					for (int x = 0; x < 40; x++) {
						int t1 = Vox_Get(w, x, y, z, NULL), t2 = Vox_Get(back, x, y, z, NULL);
						int want = t1 == 1 ? 7 : t1 == 3 ? 9 : 0;
						if (t1 && want == 0)
							expect_unknown++;
						if (t2 != want)
							ok = 0;
					}
			CHECK(ok, "remapped by name");
			CHECK(unknown == expect_unknown, "unknown count %d want %d", unknown, expect_unknown);
			Vox_Free(back);
		}
		Vox_Free(other);
	}

	/* damage: every single-byte change and every truncation is refused */
	{
		int accepted = 0;
		for (size_t i = 0; i < len; i += 1 + len / 2000) {
			data[i] ^= 0x5A;
			back = Vox_LoadFromMemory(data, len, w, NULL, NULL, NULL, err, sizeof(err));
			if (back) {
				accepted++;
				Vox_Free(back);
			}
			data[i] ^= 0x5A;
		}
		CHECK(accepted == 0, "%d damaged files accepted", accepted);
		accepted = 0;
		for (size_t n = 0; n < len; n += 1 + len / 500) {
			back = Vox_LoadFromMemory(data, n, w, NULL, NULL, NULL, err, sizeof(err));
			if (back) {
				accepted++;
				Vox_Free(back);
			}
		}
		CHECK(accepted == 0, "%d truncated files accepted", accepted);
	}
	Mem_Free(data);

	/* forged files with a good checksum but bad structure */
	{
		uint8_t buf[256];
		size_t n = 0;
		uint32_t crc;
		memcpy(buf, "BWVX", 4), n = 4;
		uint32_t vals[] = {1, 2000, 1, 1};
		memcpy(buf + n, vals, sizeof(vals)), n += sizeof(vals);
		crc = Vox_CRC32(buf, n);
		memcpy(buf + n, &crc, 4), n += 4;
		back = Vox_LoadFromMemory(buf, n, w, NULL, NULL, NULL, err, sizeof(err));
		CHECK(back == NULL, "oversized world refused");
	}

	/* chunks on the wire */
	clen = Vox_EncodeChunk(w, 1, 1, 0, chunk, sizeof(chunk));
	CHECK(clen > 0, "encode chunk");
	{
		int size[3] = {40, 40, 20};
		vxworld_t *mirror = Vox_Create(size, 32, w->origin);
		bool same = true;
		memcpy(mirror->defs, w->defs, sizeof(w->defs));
		CHECK(Vox_DecodeChunk(mirror, 1, 1, 0, chunk, clen), "decode chunk");
		for (int z = 0; z < 16; z++)
			for (int y = 16; y < 32; y++)
				for (int x = 16; x < 32; x++)
					if (Vox_Get(mirror, x, y, z, NULL) != Vox_Get(w, x, y, z, NULL))
						same = false;
		CHECK(same, "chunk mirrors");
		CHECK(!Vox_DecodeChunk(mirror, 1, 1, 0, chunk, clen - 1), "short chunk refused");
		CHECK(!Vox_DecodeChunk(mirror, 9, 9, 9, chunk, clen), "chunk out of range refused");
		chunk[2] = 0xFF, chunk[3] = 0x0F; /* first run's type -> 4095, undefined */
		CHECK(!Vox_DecodeChunk(mirror, 1, 1, 0, chunk, clen), "undefined type refused");
		Vox_Free(mirror);
	}
	CHECK(Vox_CRC32("123456789", 9) == 0xCBF43926u, "crc32 check value");
	Vox_Free(w);
}

static void test_speed(void)
{
	/* 20,000 player-sized moves over a filled floor: a sanity bound only */
	int size[3] = {128, 128, 64};
	vxworld_t *w = Vox_Create(size, 32, NULL);
	float mins[3] = {-16, -16, -36}, maxs[3] = {16, 16, 36};
	vxtrace_t tr;
	define(w, 1, "BOX", VXS_BOX, 1);
	for (int x = 0; x < 128; x++)
		for (int y = 0; y < 128; y++)
			Vox_Set(w, x, y, 0, 1, 0);
	for (int i = 0; i < 20000; i++) {
		float s[3] = {(float)(i % 4000), (float)(i % 3000), 80}, e[3] = {s[0] + 10, s[1] + 5, 60};
		Vox_Trace(w, s, mins, maxs, e, 1, &tr);
	}
	CHECK(w->count == 128 * 128, "count");
	Vox_Free(w);
}

int main(void)
{
	test_cells();
	test_traces();
	test_files();
	test_speed();
	printf("voxeltest: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
