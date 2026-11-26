/* Bulwark engine - prints the result of many seeded random traces and
 * point-contents queries on real maps, every field as raw bits, so two
 * builds of the collision code can be compared with cmp: a speedup must
 * not change a single bit. It also checks that the remembered world
 * queries (BSP_TraceWorld, BSP_WorldContents) answer exactly as the plain
 * ones, on a first ask and on a repeat, and exits 1 if not.
 * usage: tracefuzz <basedir> <game> <count per map> <maps/x.bsp>...
 * ISC licence, see LICENSE. */
#include "../../src/server/bspfile.h"
#include "../../src/common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng = 12345;
static int cache_mismatches;

static bool same_trace(const bsptrace_t *a, const bsptrace_t *b)
{
	return !memcmp(&a->fraction, &b->fraction, 4) && !memcmp(a->endpos, b->endpos, 12) &&
	       !memcmp(a->normal, b->normal, 12) && !memcmp(&a->dist, &b->dist, 4) && a->startsolid == b->startsolid &&
	       a->allsolid == b->allsolid && a->contents == b->contents && a->surfaceflags == b->surfaceflags &&
	       a->shader == b->shader && a->brush == b->brush && a->brushside == b->brushside;
}

static uint32_t next(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static float frand(float lo, float hi) { return lo + (hi - lo) * (float)(next() & 0xffffff) / (float)0xffffff; }

static uint32_t bits(float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}

static void print_trace(const bsptrace_t *t)
{
	printf("%08x %08x %08x %08x %08x %08x %08x %08x %d %d %d %d %d %d %d\n", bits(t->fraction), bits(t->endpos[0]),
	       bits(t->endpos[1]), bits(t->endpos[2]), bits(t->normal[0]), bits(t->normal[1]), bits(t->normal[2]),
	       bits(t->dist), t->startsolid, t->allsolid, t->contents, t->surfaceflags, t->shader, t->brush,
	       t->brushside);
}

static const float sizes[][6] = {
	{0, 0, 0, 0, 0, 0},         {-16, -16, -24, 16, 16, 32}, {-16, -16, -36, 16, 16, 36},
	{-32, -32, -24, 32, 32, 64}, {-8, -8, -8, 8, 8, 8},       {-1, -1, -1, 1, 1, 1},
	{-16, -16, -18, 16, 16, 18}, {-16, -16, 0, 16, 16, 72},
};
static const int masks[] = {-1, CONTENTS_SOLID, CONTENTS_SOLID | CONTENTS_BODY | CONTENTS_PLAYERCLIP,
                            CONTENTS_WATER | CONTENTS_SLIME | CONTENTS_LAVA};

static void fuzz_map(bspmap_t *m, int count)
{
	float last[3] = {0, 0, 0};
	const cmodel_t *w = &m->models[0];
	for (int i = 0; i < count; i++) {
		int model = (next() & 7) == 0 && m->nummodels > 1 ? (int)(next() % (uint32_t)m->nummodels) : 0;
		const cmodel_t *cm = &m->models[model];
		float origin[3] = {0, 0, 0}, start[3], end[3], mins[3], maxs[3], p[3];
		int kind = next() % 8, mask = masks[next() % 4];
		bsptrace_t t;
		if (model)
			for (int k = 0; k < 3; k++)
				origin[k] = (next() & 1) ? frand(-256, 256) : 0;
		for (int k = 0; k < 3; k++)
			start[k] = frand(cm->mins[k] - 64, cm->maxs[k] + 64) + origin[k];
		if (kind < 3 && !model)
			memcpy(start, last, sizeof(start)); /* from where a trace stopped */
		if (next() % 8 == 0) {
			for (int k = 0; k < 6; k++)
				(k < 3 ? mins : maxs)[k % 3] = (k < 3 ? -1 : 1) * frand(0, 40);
		} else {
			const float *s = sizes[next() % (sizeof(sizes) / sizeof(sizes[0]))];
			memcpy(mins, s, sizeof(mins));
			memcpy(maxs, s + 3, sizeof(maxs));
		}
		switch (next() % 6) {
		case 0: /* stand still: a position test */
			memcpy(end, start, sizeof(end));
			break;
		case 1: /* straight down, as for the floor */
			memcpy(end, start, sizeof(end));
			end[2] -= frand(0, 512);
			break;
		case 2: /* a short step */
			for (int k = 0; k < 3; k++)
				end[k] = start[k] + frand(-24, 24);
			break;
		case 3: /* along one axis */
			memcpy(end, start, sizeof(end));
			end[next() % 3] += frand(-1024, 1024);
			break;
		default: /* anywhere in the world */
			for (int k = 0; k < 3; k++)
				end[k] = frand(w->mins[k] - 64, w->maxs[k] + 64);
			break;
		}
		if (i % 997 == 0) {
			/* a few degenerate inputs */
			static const float odd[] = {INFINITY, -INFINITY, NAN, 1e30f, -1e30f, 0.0f, -0.0f};
			end[next() % 3] = odd[next() % 7];
		}
		BSP_TraceModel(m, model, origin, start, mins, maxs, end, mask, &t);
		print_trace(&t);
		if (!model && !origin[0] && !origin[1] && !origin[2]) {
			for (int r = 0; r < 2; r++) {
				bsptrace_t c;
				BSP_TraceWorld(m, start, mins, maxs, end, mask, &c);
				cache_mismatches += !same_trace(&c, &t);
			}
		}
		if (!model && isfinite(t.endpos[0]) && isfinite(t.endpos[1]) && isfinite(t.endpos[2]))
			memcpy(last, t.endpos, sizeof(last));
		for (int k = 0; k < 3; k++)
			p[k] = (next() & 1) ? t.endpos[k] : frand(cm->mins[k] - 16, cm->maxs[k] + 16) + origin[k];
		{
			int c = BSP_PointContents(m, model, origin, p);
			printf("c %d\n", c);
			if (!model && !origin[0] && !origin[1] && !origin[2])
				for (int r = 0; r < 2; r++)
					cache_mismatches += BSP_WorldContents(m, p) != c;
		}
	}
}

int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: tracefuzz <basedir> <game> <count per map> <maps/x.bsp>...\n");
		return 2;
	}
	FS_Init(argv[1]);
	FS_AddGameDir(argv[2]);
	for (int i = 4; i < argc; i++) {
		bspmap_t *m = BSP_Load(argv[i]);
		printf("map %s %s\n", argv[i], m ? "loaded" : "failed");
		if (!m || !m->nummodels)
			continue;
		rng = 12345;
		fuzz_map(m, atoi(argv[3]));
		BSP_Free(m);
	}
	if (cache_mismatches)
		fprintf(stderr, "tracefuzz: %d remembered answers differ\n", cache_mismatches);
	return cache_mismatches ? 1 : 0;
}
