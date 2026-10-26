/* Bulwark engine - checks Q1 hull collision and visibility on the generated
 * map tests/data/maps/q1test.bsp, and GoldSrc hulls on its BSP30 twin
 * hltest.bsp (tools/mkq1map_test.py, with --hl for the second). Run from the
 * source tree's root.
 *
 * If BULWARK_GOLDSRC_DIR names a game directory (game content is never
 * committed), every .bsp in its maps folder is also loaded and each player spawn is
 * checked: the standing hull must come to rest exactly 36 units above the
 * floor that a point trace finds below the spawn, so the hull and point
 * collision paths agree on real geometry. ISC licence, see LICENSE. */
#include "../../src/server/bspfile.h"
#include "../../src/common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures, checks;

static void check(bool ok, const char *name)
{
	checks++;
	if (!ok) {
		printf("FAIL: %s\n", name);
		failures++;
	}
}

static bool near(const float *a, float x, float y, float z)
{
	return fabsf(a[0] - x) < 0.1f && fabsf(a[1] - y) < 0.1f && fabsf(a[2] - z) < 0.1f;
}

typedef struct {
	char names[64][64];
	int count;
} maplist_t;

static void addmap(const char *name, const char *searchpath, void *ctx)
{
	maplist_t *l = ctx;
	(void)searchpath;
	if (l->count < 64)
		Str_Copy(l->names[l->count++], name, sizeof(l->names[0]));
}

/* real GoldSrc maps: spawns, hull against point collision, visibility */
static void goldsrc_fixtures(const char *dir)
{
	static const float zero[3], mins[3] = {-16, -16, -36}, maxs[3] = {16, 16, 36};
	char base[1024], game[256];
	const char *slash = strrchr(dir, '/');
	if (!slash || !slash[1]) {
		check(false, "BULWARK_GOLDSRC_DIR must be <basedir>/<game>");
		return;
	}
	snprintf(base, sizeof(base), "%.*s", (int)(slash - dir), dir);
	Str_Copy(game, slash + 1, sizeof(game));
	FS_Init(base);
	FS_AddGameDir(game);
	maplist_t list = {0};
	FS_List("maps/*.bsp", true, addmap, &list);
	check(list.count > 0, "BULWARK_GOLDSRC_DIR has maps/*.bsp");
	for (int i = 0; i < list.count; i++) {
		bspmap_t *m = BSP_Load(list.names[i]);
		check(m && m->hl, va("%s loads as BSP30", list.names[i]));
		if (!m)
			continue;
		int spawns = 0, clear = 0, agree = 0, stuck = 0;
		char tok[1024], key[256], cls[64] = "", org[64] = "";
		for (const char *d = m->entities; (d = COM_Parse(d, tok, sizeof(tok)));) {
			if (!strcmp(tok, "{")) {
				cls[0] = org[0] = 0;
				continue;
			}
			if (!strcmp(tok, "}")) {
				float o[3], down[3];
				if (strncmp(cls, "info_player_", 12) || sscanf(org, "%f %f %f", &o[0], &o[1], &o[2]) != 3)
					continue;
				spawns++;
				VectorCopy(o, down);
				down[2] -= 1024;
				bsptrace_t hull, point;
				BSP_TraceModel(m, 0, zero, o, mins, maxs, o, CONTENTS_SOLID, &hull);
				if (hull.startsolid) {
					/* the spawn's box already overlaps the world; reported, not judged */
					printf("%s: %s at %s starts in solid\n", list.names[i], cls, org);
					stuck++;
					continue;
				}
				clear++;
				BSP_TraceModel(m, 0, zero, o, mins, maxs, down, CONTENTS_SOLID, &hull);
				BSP_TraceModel(m, 0, zero, o, zero, zero, down, CONTENTS_SOLID, &point);
				if (hull.fraction < 1 && point.fraction < 1 && fabsf(hull.endpos[2] - (point.endpos[2] + 36)) < 0.1f &&
				    BSP_CheckPVS(m, o, o))
					agree++;
				else
					printf("%s: %s at %s: hull rests at %.1f, floor at %.1f\n", list.names[i], cls, org,
					       hull.endpos[2], point.endpos[2]);
				continue;
			}
			Str_Copy(key, tok, sizeof(key));
			if (!(d = COM_Parse(d, tok, sizeof(tok))))
				break;
			if (!strcmp(key, "classname"))
				Str_Copy(cls, tok, sizeof(cls));
			else if (!strcmp(key, "origin"))
				Str_Copy(org, tok, sizeof(org));
		}
		printf("%s: %d models, %d leafs, %d spawns: %d clear (%d agree), %d start in solid\n", list.names[i],
		       m->nummodels, m->numleafs, spawns, clear, agree, stuck);
		check(spawns > 0 && clear > 0, va("%s has clear spawns", list.names[i]));
		check(agree == clear, va("%s: hull and point traces agree at every clear spawn", list.names[i]));
		BSP_Free(m);
	}
}

int main(void)
{
	static const float zero[3], pmins[3] = {-16, -16, -24}, pmaxs[3] = {16, 16, 32};
	static const float bmins[3] = {-16, -16, -36}, bmaxs[3] = {16, 16, 36};
	bspmap_t *m;
	bsptrace_t tr;
	FS_Init("tests");
	FS_AddGameDir("data");
	m = BSP_Load("maps/q1test.bsp");
	check(m && m->type == BSP_Q1, "load");
	if (!m)
		return 1;
	check(BSP_PointContents(m, 0, zero, (float[]){96, 96, 40}) == 0, "empty in room A");
	check(BSP_PointContents(m, 0, zero, (float[]){-100, 0, 0}) & CONTENTS_SOLID, "solid outside");
	check(BSP_PointContents(m, 0, zero, (float[]){256, 256, 32}) & CONTENTS_SOLID, "solid in the block");
	check(BSP_PointContents(m, 0, zero, (float[]){800, 256, 128}) & CONTENTS_SOLID, "solid between rooms");

	BSP_TraceModel(m, 0, zero, (float[]){96, 96, 100}, pmins, pmaxs, (float[]){96, 96, -100}, CONTENTS_SOLID, &tr);
	check(tr.fraction < 1 && near(tr.endpos, 96, 96, 24) && tr.normal[2] > 0.99f, "player lands on the floor");
	BSP_TraceModel(m, 0, zero, (float[]){100, 256, 40}, pmins, pmaxs, (float[]){400, 256, 40}, CONTENTS_SOLID, &tr);
	check(tr.fraction < 1 && fabsf(tr.endpos[0] - 176) < 0.1f && tr.normal[0] < -0.99f, "player stops at the block");
	BSP_TraceModel(m, 0, zero, (float[]){256, 256, 200}, pmins, pmaxs, (float[]){256, 256, 0}, CONTENTS_SOLID, &tr);
	check(tr.fraction < 1 && near(tr.endpos, 256, 256, 88), "player stands on the block");
	BSP_TraceModel(m, 0, zero, (float[]){96, 96, 100}, bmins, bmaxs, (float[]){96, 96, -100}, CONTENTS_SOLID, &tr);
	check(tr.fraction < 1 && near(tr.endpos, 96, 96, 36), "taller box: hull lined up at the bottom");
	BSP_TraceModel(m, 0, zero, (float[]){96, 96, 40}, zero, zero, (float[]){96, 96, 40}, CONTENTS_SOLID, &tr);
	check(!tr.startsolid && tr.fraction == 1, "point in the open");
	BSP_TraceModel(m, 0, zero, (float[]){256, 256, 32}, zero, zero, (float[]){256, 256, 300}, CONTENTS_SOLID, &tr);
	check(tr.startsolid && !tr.allsolid, "point leaving the block starts solid");
	BSP_TraceModel(m, 0, zero, (float[]){96, 96, 40}, zero, zero, (float[]){96, 600, 40}, CONTENTS_SOLID, &tr);
	check(tr.fraction < 1 && fabsf(tr.endpos[1] - 512) < 0.1f, "point hits the wall");

	check(BSP_PointLeaf(m, (float[]){96, 96, 40}) > 0, "leaf in room A");
	check(BSP_CheckPVS(m, (float[]){96, 96, 40}, (float[]){480, 480, 200}), "room A sees itself");
	check(!BSP_CheckPVS(m, (float[]){96, 96, 40}, (float[]){1200, 100, 40}), "room A can't see room B");
	BSP_Free(m);

	/* GoldSrc: the hull follows the box's width and height */
	m = BSP_Load("maps/hltest.bsp");
	check(m && m->type == BSP_Q1 && m->hl, "load BSP30");
	if (!m)
		return 1;
	BSP_TraceModel(m, 0, zero, (float[]){96, 96, 100}, bmins, bmaxs, (float[]){96, 96, -100}, CONTENTS_SOLID, &tr);
	check(tr.fraction < 1 && near(tr.endpos, 96, 96, 36), "HL player stands on the floor");
	BSP_TraceModel(m, 0, zero, (float[]){96, 96, 100}, (float[]){-16, -16, -18}, (float[]){16, 16, 18},
	               (float[]){96, 96, -100}, CONTENTS_SOLID, &tr);
	check(tr.fraction < 1 && near(tr.endpos, 96, 96, 18), "HL crouching player uses hull 3");
	BSP_TraceModel(m, 0, zero, (float[]){100, 256, 40}, (float[]){-32, -32, -32}, (float[]){32, 32, 32},
	               (float[]){400, 256, 40}, CONTENTS_SOLID, &tr);
	check(tr.fraction < 1 && fabsf(tr.endpos[0] - 160) < 0.1f, "HL large hull stops at the block");
	BSP_TraceModel(m, 0, zero, (float[]){256, 256, 200}, bmins, bmaxs, (float[]){256, 256, 0}, CONTENTS_SOLID, &tr);
	check(tr.fraction < 1 && near(tr.endpos, 256, 256, 100), "HL player stands on the block");
	check(!BSP_CheckPVS(m, (float[]){96, 96, 40}, (float[]){1200, 100, 40}), "BSP30 visibility");
	BSP_Free(m);

	const char *dir = getenv("BULWARK_GOLDSRC_DIR");
	if (dir && *dir)
		goldsrc_fixtures(dir);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
