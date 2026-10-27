/* Bulwark engine - checks Q1 hull collision and visibility on the generated
 * map tests/data/maps/q1test.bsp, and GoldSrc hulls on its BSP30 twin
 * hltest.bsp (tools/mkq1map_test.py, with --hl for the second). Run from the
 * source tree's root. ISC licence, see LICENSE. */
#include "../../src/server/bspfile.h"
#include "../../src/common/fs.h"

#include <math.h>
#include <stdio.h>

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
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
