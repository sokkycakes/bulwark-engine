/* Bulwark engine - replays a dedicated server's logged link commands
 * through the DS's movement code (src/platform/nds/pmove_nds.c) and
 * compares each result with what the server got. ISC licence, see LICENSE.
 *
 *   cc -O2 -std=c99 -Wall -ffp-contract=off -Isrc/platform/nds -o pmtest \
 *      tools/nds/pmtest.c src/platform/nds/pmove_nds.c -lm
 *   (add -DPMTEST_HULL_NDS src/platform/nds/hull_nds.c to trace through the
 *   DS's own hull code instead of the copy of the server's below)
 *
 *   pmtest -selftest
 *   pmtest <map.bsp> <ded.log> [-slot n] [-tol 0.05] [-chain] [-resample ms]
 *          [-bench] [-nofocus] [-dshull0] [-v] [-dump file [-fuzz n]]
 *
 * The log is the ded's console output with sv_link_log 1: LINKPM lines
 * (docs/spec/link.md, "sv_link_log"). Each logged command runs once from the
 * server's own state before it ("step" mode, which isolates the port), or
 * with -chain from the first state on, keeping its own (the drift between
 * corrections). A step the server's speedhack budget cut short is listed
 * and left out of the figures. Exit status 1 when the largest origin error
 * is over -tol or a flag differs.
 *
 *   -resample ms  runs the commands from the first state as logged, and
 *                 again cut into ms-long commands, and prints how far apart
 *                 they end up and each jump's height in both
 *   -bench        times every step 1000 times: µs per PM_Run and per trace
 *   -nofocus      no broadphase: the results must be bit-identical
 *   -dshull0      point traces through the DS's fixed-point drawing tree
 *                 (needs -DPMTEST_HULL_NDS)
 *   -ownhull      with -DPMTEST_HULL_NDS: trace through the server's copy below
 *   -cmphull      with -DPMTEST_HULL_NDS: run both on every trace and count
 *                 the ones that differ in any bit
 *   pmtest -selftest   checks PM_SinCos against this C library's sinf/cosf on
 *                 every angle a command can carry (yaw and pitch, both
 *                 constants pmove_nds.c turns them into radians with), and
 *                 fexact_nds.h's fx_sqrt against sqrtf on all 2^32 floats;
 *                 meaningful on x86-64 glibc, the server's
 *   -dump file    writes every result of the step and -chain runs, and of n
 *                 made-up variants of each step (-fuzz n, default 8: moved,
 *                 pushed, turned, other keys), as raw bits: two builds of the
 *                 movement code must write the same file (a speed-up changes
 *                 no result, even off the logged path)
 *
 * The same file can hold a hand-written script instead (no server, the
 * states are printed): lines starting "PM", see read_script below.
 *
 * The map and the logs (which carry map data) are the game's: keep them out
 * of git. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pmove_nds.h"
#include "fexact_nds.h"
#ifdef PMTEST_HULL_NDS
#include "hull_nds.h"
#endif

#define MAX_BRUSHES 512
#define MAX_BOXES 64
#ifndef MAX_STEPS /* a DS build of pmtest sets fewer */
#define MAX_STEPS 200000
#endif

typedef struct {
	uint16_t edict, model;
	uint8_t solid;
	int8_t skin;
	float origin[3];
	float angles[3]; /* "brushang": turned, traced in its own frame (sv_world.c brush_frame) */
	bool hasabs;     /* "brushabs": a turned one's absmin/absmax as the server last linked it */
	float amin[3], amax[3];
} brush_t;

typedef struct {
	uint16_t edict;
	float mins[3], maxs[3];
	uint8_t solid;
} box_t;

/* one logged command and everything around it */
typedef struct {
	int vars;                 /* index into the vars sets */
	int brushes, nbrushes;    /* into the brush pool: the list for this command */
	int boxes, nboxes;        /* into the box pool */
	int touches, ntouches;    /* into the touch pool ("touch") */
	pmstate_t pre, post;
	pmcmd_t cmd;
	float ran_msec;
	bool clipped, haspost;
} step_t;

static pmvars_t *varsets;
static int numvarsets, capvarsets;
static brush_t *brushpool;
static int numbrushpool, capbrushpool;
static box_t *boxpool;
static int numboxpool, capboxpool;
static pmtouch_t *touchpool;
static int numtouchpool, captouchpool;
static step_t *steps;
static int numsteps;
static bool verbose, nofocus, dshull0;
static bool ownhull, cmphull;     /* hull_nds builds: the server's copy instead, or both compared */
static long cmp_traces, cmp_bad;

static void *grow(void *p, int *cap, int need, size_t size)
{
	if (need <= *cap)
		return p;
	*cap = need * 2 + 16;
	p = realloc(p, (size_t)*cap * size);
	if (!p) {
		fprintf(stderr, "pmtest: out of memory\n");
		exit(2);
	}
	return p;
}

/* --- the world: a copy of the server's Q1 hull code --------------------
 *
 * bspfile.c load_q1 (:310-447), hull_contents/hull_trace/q1_trace
 * (:953-1077) and sv_world.c SV_Move (:348-471) with trace_aabb and
 * merge_bsp (:242-326), cut down to what the predicted player meets: the
 * world, the brush entities and boxes of the log. Kept apart from
 * hull_nds.c on purpose: it is the reference the DS code is checked against. */

#define CB_SOLID 0x1
#define CB_BODY 0x02000000
#define CB_PLAYERCLIP 0x10000
#define CB_MONSTERCLIP 0x20000
#define Q1C_SOLID (-2)
#define DIST_EPSILON 0.03125f

typedef struct { float normal[3], dist; } hplane_t;
typedef struct { int plane, children[2]; } hnode_t;
typedef struct { float mins[3], maxs[3]; int headnode[4]; } hmodel_t;

static struct {
	bool loaded, hl;
	hplane_t *planes;
	hnode_t *clip, *hull0;
	int numplanes, numclip, numnodes, nummodels;
	hmodel_t *models;
} map;

typedef struct {
	float fraction, endpos[3], normal[3];
	bool startsolid, allsolid;
	uint32_t contents;
} htrace_t;

/* SV_Move's trace while it is being built (svtrace_t) */
typedef struct {
	float fraction, normal[3];
	bool startsolid, allsolid;
	int ent;
	uint32_t contents;
} svtr_t;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int rds16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static float rdf(const uint8_t *p)
{
	uint32_t u = rd32(p);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

static uint8_t *read_file(const char *path, long *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *d;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = malloc((size_t)*size + 1);
	if (!d || fread(d, 1, (size_t)*size, f) != (size_t)*size) {
		fclose(f);
		free(d);
		return NULL;
	}
	fclose(f);
	return d;
}

static bool map_load(const char *path)
{
	long size;
	uint8_t *d = read_file(path, &size);
	uint32_t ofs[15], len[15];
	int ver, numleafs;
	if (!d || size < 4 + 15 * 8) {
		fprintf(stderr, "pmtest: can't read %s\n", path);
		return false;
	}
	ver = (int)rd32(d);
	if (ver != 29 && ver != 30) {
		fprintf(stderr, "pmtest: %s is not BSP 29 or 30 (%d)\n", path, ver);
		return false;
	}
	for (int i = 0; i < 15; i++) {
		ofs[i] = rd32(d + 4 + i * 8);
		len[i] = rd32(d + 8 + i * 8);
		if ((long)ofs[i] + (long)len[i] > size) {
			fprintf(stderr, "pmtest: %s: lump %d out of the file\n", path, i);
			return false;
		}
	}
	map.hl = ver == 30;
	map.numplanes = (int)(len[1] / 20);
	map.planes = calloc((size_t)map.numplanes + 1, sizeof(hplane_t));
	for (int i = 0; i < map.numplanes; i++) {
		const uint8_t *p = d + ofs[1] + i * 20;
		for (int k = 0; k < 3; k++)
			map.planes[i].normal[k] = rdf(p + k * 4);
		map.planes[i].dist = rdf(p + 12);
	}
	map.nummodels = (int)(len[14] / 64);
	map.models = calloc((size_t)map.nummodels + 1, sizeof(hmodel_t));
	for (int i = 0; i < map.nummodels; i++) {
		const uint8_t *p = d + ofs[14] + i * 64;
		for (int k = 0; k < 3; k++) {
			map.models[i].mins[k] = rdf(p + k * 4);
			map.models[i].maxs[k] = rdf(p + 12 + k * 4);
		}
		for (int k = 0; k < 4; k++)
			map.models[i].headnode[k] = (int)rd32(p + 36 + k * 4);
	}
	numleafs = (int)(len[10] / 28);
	map.numnodes = (int)(len[5] / 24);
	map.hull0 = calloc((size_t)map.numnodes + 1, sizeof(hnode_t));
	for (int i = 0; i < map.numnodes; i++) {
		const uint8_t *p = d + ofs[5] + i * 24;
		hnode_t *n = &map.hull0[i];
		n->plane = (int)rd32(p);
		if (n->plane < 0 || n->plane >= map.numplanes)
			n->plane = 0;
		for (int c = 0; c < 2; c++) {
			int ch = rds16(p + 4 + c * 2);
			if (ch >= 0 && ch >= map.numnodes)
				ch = (int)(uint16_t)ch - 65536; /* maps with more than 32767 leafs */
			if (ch >= map.numnodes || -(ch + 1) >= numleafs)
				ch = -1;
			if (ch < 0) {
				/* hull 0 leaf children carry the leaf's Q1 contents */
				int q1c = (int)rd32(d + ofs[10] + (-(ch + 1)) * 28);
				ch = q1c < 0 ? q1c : Q1C_SOLID;
			}
			n->children[c] = ch;
		}
	}
	map.numclip = (int)(len[9] / 8);
	map.clip = calloc((size_t)map.numclip + 1, sizeof(hnode_t));
	for (int i = 0; i < map.numclip; i++) {
		const uint8_t *p = d + ofs[9] + i * 8;
		hnode_t *c = &map.clip[i];
		c->plane = (int)rd32(p);
		if (c->plane < 0 || c->plane >= map.numplanes)
			c->plane = 0;
		for (int k = 0; k < 2; k++) {
			int ch = rds16(p + 4 + k * 2);
			if (ch >= map.numclip)
				ch = (int)(uint16_t)ch - 65536;
			if (ch >= map.numclip)
				ch = Q1C_SOLID;
			c->children[k] = ch;
		}
	}
	free(d);
	map.loaded = map.nummodels > 0;
	printf("PMTEST map %s bsp%d models=%d planes=%d nodes=%d clipnodes=%d\n", path, ver, map.nummodels,
	       map.numplanes, map.numnodes, map.numclip);
	return map.loaded;
}

static int hull_contents(const hnode_t *nodes, int num, const float *p)
{
	int guard = 0;
	while (num >= 0 && guard++ < 65536) {
		const hplane_t *pl = &map.planes[nodes[num].plane];
		num = nodes[num].children[(p[0] * pl->normal[0] + p[1] * pl->normal[1] + p[2] * pl->normal[2]) - pl->dist < 0];
	}
	return num;
}

typedef struct {
	const hnode_t *nodes;
	int head, depth;
	htrace_t *tr;
	bool allsolid, startsolid;
} hullwork_t;

static float dotp(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static bool hull_trace(hullwork_t *w, int num, float f1, float f2, const float *p1, const float *p2)
{
	const hplane_t *pl;
	float t1, t2, frac, midf, mid[3];
	int side;
	if (num < 0) {
		if (num != Q1C_SOLID)
			w->allsolid = false;
		else
			w->startsolid = true;
		return true;
	}
	if (++w->depth > 4096)
		return true;
	pl = &map.planes[w->nodes[num].plane];
	t1 = dotp(p1, pl->normal) - pl->dist;
	t2 = dotp(p2, pl->normal) - pl->dist;
	if (t1 >= 0 && t2 >= 0)
		return hull_trace(w, w->nodes[num].children[0], f1, f2, p1, p2);
	if (t1 < 0 && t2 < 0)
		return hull_trace(w, w->nodes[num].children[1], f1, f2, p1, p2);
	frac = t1 < 0 ? (t1 + DIST_EPSILON) / (t1 - t2) : (t1 - DIST_EPSILON) / (t1 - t2);
	frac = frac < 1 ? frac : 1;
	frac = 0 > frac ? 0 : frac;
	side = t1 < 0;
	midf = f1 + (f2 - f1) * frac;
	for (int k = 0; k < 3; k++)
		mid[k] = p1[k] + (p2[k] - p1[k]) * frac;
	if (!hull_trace(w, w->nodes[num].children[side], f1, midf, p1, mid))
		return false;
	if (hull_contents(w->nodes, w->nodes[num].children[side ^ 1], mid) != Q1C_SOLID)
		return hull_trace(w, w->nodes[num].children[side ^ 1], midf, f2, mid, p2);
	if (w->allsolid)
		return false;
	if (!side) {
		for (int k = 0; k < 3; k++)
			w->tr->normal[k] = pl->normal[k];
	} else {
		for (int k = 0; k < 3; k++)
			w->tr->normal[k] = -pl->normal[k];
	}
	while (hull_contents(w->nodes, w->head, mid) == Q1C_SOLID) {
		frac -= 0.1f;
		if (frac < 0) {
			w->tr->fraction = f1;
			memcpy(w->tr->endpos, p1, sizeof(float) * 3);
			return false;
		}
		midf = f1 + (f2 - f1) * frac;
		for (int k = 0; k < 3; k++)
			mid[k] = p1[k] + (p2[k] - p1[k]) * frac;
	}
	w->tr->fraction = midf;
	memcpy(w->tr->endpos, mid, sizeof(float) * 3);
	return false;
}

/* BSP_TraceModel for a Q1 map (q1_trace) */
static void trace_model(int model, const float *origin, const float *start, const float *mins, const float *maxs,
                        const float *end, htrace_t *tr)
{
	static const float hull_mins[3][3] = {{0, 0, 0}, {-16, -16, -24}, {-32, -32, -24}};
	static const float hl_hull_mins[4][3] = {{0, 0, 0}, {-16, -16, -36}, {-32, -32, -32}, {-16, -16, -18}};
	const float (*hm)[3] = hull_mins;
	hullwork_t w;
	float off[3], p1[3], p2[3], size = maxs[0] - mins[0];
	int hull = size < 3 ? 0 : size <= 32 ? 1 : 2;
	memset(tr, 0, sizeof(*tr));
	tr->fraction = 1;
	memcpy(tr->endpos, end, sizeof(float) * 3);
	if (model < 0 || model >= map.nummodels)
		return;
	if (map.hl) {
		hull = size <= 8 ? 0 : size <= 36 ? (maxs[2] - mins[2] <= 36 ? 3 : 1) : 2;
		hm = hl_hull_mins;
	}
	memset(&w, 0, sizeof(w));
	w.nodes = hull ? map.clip : map.hull0;
	w.head = map.models[model].headnode[hull];
	w.tr = tr;
	w.allsolid = true;
	if ((hull ? map.numclip : map.numnodes) == 0)
		return;
	for (int k = 0; k < 3; k++) {
		off[k] = (hull ? hm[hull][k] : 0) - (hull ? mins[k] : 0) + origin[k];
		p1[k] = start[k] - off[k];
		p2[k] = end[k] - off[k];
	}
	tr->fraction = 1;
	memcpy(tr->endpos, p2, sizeof(float) * 3);
	hull_trace(&w, w.head, 0, 1, p1, p2);
	tr->startsolid = w.startsolid;
	tr->allsolid = w.allsolid;
	if (w.allsolid) {
		tr->fraction = 0;
		memcpy(tr->endpos, p1, sizeof(float) * 3);
	}
	if (tr->fraction < 1 || w.startsolid)
		tr->contents = CB_SOLID;
	for (int k = 0; k < 3; k++)
		tr->endpos[k] += off[k];
}

/* bspfile.c q1_contents for a brush entity's negative .skin */
static uint32_t skin_bits(int skin)
{
	switch (skin) {
	case -2: return CB_SOLID;
	case -3: return PM_CB_WATER;
	case -4: return PM_CB_SLIME;
	case -5: return PM_CB_LAVA;
	case -6: return CB_SOLID | PM_CB_SKY;
	case -8: return CB_PLAYERCLIP | CB_MONSTERCLIP;
	case -16: return PM_CB_LADDER;
	default: return 0;
	}
}

static void merge_bsp(svtr_t *tr, const htrace_t *bt, int ent)
{
	if (bt->allsolid || bt->startsolid) {
		tr->startsolid = true;
		if (bt->allsolid)
			tr->allsolid = true;
		if (tr->ent < 0 || bt->fraction < tr->fraction) {
			tr->ent = ent;
			tr->contents = bt->contents;
		}
	}
	if (bt->fraction < tr->fraction) {
		tr->fraction = bt->fraction;
		memcpy(tr->normal, bt->normal, sizeof(float) * 3);
		tr->ent = ent;
		tr->contents = bt->contents;
	}
}

/* sv_world.c trace_aabb (boxes here are never SOLID_TRIGGER) */
static void trace_aabb(const float *start, const float *mins, const float *maxs, const float *end,
                       const float *bmin, const float *bmax, svtr_t *tr, int ent, uint32_t contents)
{
	float enter = -1, leave = 1, n[3] = {0, 0, 0};
	bool startout = false;
	for (int i = 0; i < 3; i++) {
		for (int side = 0; side < 2; side++) {
			float dist = side == 0 ? bmax[i] - mins[i] : -(bmin[i] - maxs[i]);
			float sign = side == 0 ? 1.0f : -1.0f;
			float d1 = sign * start[i] - dist, d2 = sign * end[i] - dist;
			if (d1 > 0)
				startout = true;
			if (d1 > 0 && (d2 >= 0.03125f || d2 >= d1))
				return;
			if (d1 <= 0 && d2 <= 0)
				continue;
			if (d1 > d2) {
				float f = (d1 - 0.03125f) / (d1 - d2);
				if (f > enter) {
					enter = f;
					n[0] = n[1] = n[2] = 0;
					n[i] = sign;
				}
			} else {
				float f = (d1 + 0.03125f) / (d1 - d2);
				if (f < leave)
					leave = f;
			}
		}
	}
	if (!startout) {
		if (!tr->startsolid || tr->ent < 0) {
			tr->startsolid = tr->allsolid = true;
			tr->fraction = 0;
			tr->ent = ent;
			tr->contents = contents;
		}
		return;
	}
	if (enter < leave && enter > -1 && enter < tr->fraction) {
		tr->fraction = enter < 0 ? 0 : enter;
		memcpy(tr->normal, n, sizeof(n));
		tr->ent = ent;
		tr->contents = contents;
	}
}

/* the scene of the current step */
static const brush_t *sc_brushes;
static int sc_nbrushes;
static const box_t *sc_boxes;
static int sc_nboxes;
static const pmtouch_t *sc_touches;
static int sc_ntouches;

#ifndef PMTEST_HULL_NDS
/* the touches of the step, as hull_nds.c's Hull_TouchOf and Hull_Triggers have them */
static const pmtouch_t *own_touchof(void *ctx, int ent)
{
	(void)ctx;
	for (int i = 0; i < sc_ntouches; i++)
		if (sc_touches[i].edict == ent)
			return sc_touches[i].kind == PM_TOUCH_CONVEYOR ? &sc_touches[i] : NULL;
	return NULL;
}

static int own_triggers(void *ctx, const float *amin, const float *amax, const pmtouch_t **out, int max)
{
	int n = 0;
	(void)ctx;
	for (int i = 0; i < sc_ntouches && n < max; i++) {
		const pmtouch_t *t = &sc_touches[i];
		if (t->kind != PM_TOUCH_PUSH || t->absmin[0] > amax[0] || t->absmin[1] > amax[1] || t->absmin[2] > amax[2] ||
		    t->absmax[0] < amin[0] || t->absmax[1] < amin[1] || t->absmax[2] < amin[2])
			continue;
		out[n++] = t;
	}
	return n;
}
#endif
/* focus: which of them the next traces may test */
static bool fc_on;
static uint8_t fc_brush[MAX_BRUSHES], fc_box[MAX_BOXES];

enum { TK_BOX, TK_POINT, TK_LINE, TK_CONTENTS, TK_NUM };
static const char *tk_names[TK_NUM] = {"box", "point", "nomonsters", "contents"};
static long tk_count[TK_NUM];
static double tk_time[TK_NUM];
static bool timing;

static double now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static bool turned(const brush_t *b) { return b->angles[0] != 0 || b->angles[1] != 0 || b->angles[2] != 0; }

/* SV_LinkEdict (sv_world.c:50-75): a turned SOLID_BSP model reaches its farthest corner every way */
static void brush_box(const brush_t *b, float *amin, float *amax)
{
	const hmodel_t *m = &map.models[b->model < map.nummodels ? b->model : 0];
	float radius = 0;
	if (turned(b) && b->hasabs) {
		memcpy(amin, b->amin, sizeof(b->amin));
		memcpy(amax, b->amax, sizeof(b->amax));
		return;
	}
	if (turned(b) && b->solid == PM_SOLID_BSP) {
		for (int k = 0; k < 3; k++) {
			float c = fabsf(m->mins[k]) > fabsf(m->maxs[k]) ? fabsf(m->mins[k]) : fabsf(m->maxs[k]);
			radius += c * c;
		}
		radius = sqrtf(radius);
	}
	for (int k = 0; k < 3; k++) {
		amin[k] = b->origin[k] + (radius > 0 ? -radius : m->mins[k]) - 1;
		amax[k] = b->origin[k] + (radius > 0 ? radius : m->maxs[k]) + 1;
	}
}

/* sv_world.c trace_brush_entity (:157-176) with common.c AngleVectors, as the server has them */
static void trace_brush(const brush_t *b, const float *start, const float *mins, const float *maxs, const float *end,
                        htrace_t *bt)
{
	static const float zero[3] = {0, 0, 0};
	const float d2r = (float)(3.14159265358979323846 / 180.0);
	float axis[3][3], ls[3], le[3], n[3], d[3], e[3], sy, cy, sp, cp, sr, cr;
	if (!turned(b)) {
		trace_model(b->model, b->origin, start, mins, maxs, end, bt);
		return;
	}
	sy = sinf(b->angles[1] * d2r), cy = cosf(b->angles[1] * d2r);
	sp = sinf(b->angles[0] * d2r), cp = cosf(b->angles[0] * d2r);
	sr = sinf(b->angles[2] * d2r), cr = cosf(b->angles[2] * d2r);
	axis[0][0] = cp * cy, axis[0][1] = cp * sy, axis[0][2] = -sp;
	axis[1][0] = -(-sr * sp * cy + cr * sy), axis[1][1] = -(-sr * sp * sy - cr * cy), axis[1][2] = -(-sr * cp);
	axis[2][0] = cr * sp * cy + sr * sy, axis[2][1] = cr * sp * sy - sr * cy, axis[2][2] = cr * cp;
	for (int k = 0; k < 3; k++)
		d[k] = start[k] - b->origin[k], e[k] = end[k] - b->origin[k];
	for (int k = 0; k < 3; k++) {
		ls[k] = d[0] * axis[k][0] + d[1] * axis[k][1] + d[2] * axis[k][2];
		le[k] = e[0] * axis[k][0] + e[1] * axis[k][1] + e[2] * axis[k][2];
	}
	trace_model(b->model, zero, ls, mins, maxs, le, bt);
	for (int k = 0; k < 3; k++)
		n[k] = bt->normal[0] * axis[0][k] + bt->normal[1] * axis[1][k] + bt->normal[2] * axis[2][k];
	memcpy(bt->normal, n, sizeof(n));
	for (int k = 0; k < 3; k++)
		bt->endpos[k] = start[k] + (end[k] - start[k]) * bt->fraction;
}

static void own_move_core(const float *start, const float *mins, const float *maxs, const float *end, int type,
                          int32_t mask, pmhit_t *out)
{
	static const float zero[3] = {0, 0, 0};
	svtr_t tr;
	htrace_t bt;
	float tmin[3], tmax[3];
	int bi = 0, xi = 0, hitkind = 0, hitidx = -1; /* hitkind 1 brush, 2 box */

	memset(&tr, 0, sizeof(tr));
	tr.fraction = 1;
	tr.ent = -1;
	if (!mask)
		mask = CB_SOLID | CB_BODY | CB_PLAYERCLIP; /* default_mask: the player is FL_CLIENT */
	if (type & PM_MOVE_EVERYTHING)
		mask = -1;

	trace_model(0, zero, start, mins, maxs, end, &bt);
	merge_bsp(&tr, &bt, 0);

	for (int i = 0; i < 3; i++) {
		tmin[i] = (start[i] < end[i] ? start[i] : end[i]) + mins[i] - 1;
		tmax[i] = (start[i] > end[i] ? start[i] : end[i]) + maxs[i] + 1;
	}
	/* brushes and boxes as one list in edict order, while fraction > 0 */
	while (tr.fraction > 0 && (bi < sc_nbrushes || xi < sc_nboxes)) {
		float amin[3], amax[3];
		bool isbrush = xi >= sc_nboxes || (bi < sc_nbrushes && sc_brushes[bi].edict < sc_boxes[xi].edict);
		int idx = isbrush ? bi++ : xi++;
		int ent, prevent = tr.ent;
		float prevfrac = tr.fraction;
		if (isbrush) {
			const brush_t *b = &sc_brushes[idx];
			uint32_t skinbits;
			if (fc_on && !fc_brush[idx])
				continue;
			ent = b->edict;
			if (b->solid == 22 && !(type & PM_MOVE_EVERYTHING))
				continue;
			if ((type & PM_MOVE_NOMONSTERS) && b->solid != PM_SOLID_BSP)
				continue;
			brush_box(b, amin, amax);
			if (amin[0] > tmax[0] || amin[1] > tmax[1] || amin[2] > tmax[2] || amax[0] < tmin[0] ||
			    amax[1] < tmin[1] || amax[2] < tmin[2])
				continue;
			skinbits = skin_bits(b->skin);
			if (skinbits && !(skinbits & (uint32_t)mask) && b->solid != 22)
				continue;
			trace_brush(b, start, mins, maxs, end, &bt);
			if (skinbits)
				bt.contents = skinbits;
			if (b->solid == 22 && bt.startsolid) {
				tr.startsolid = true;
				if (tr.ent < 0)
					tr.ent = ent;
			} else {
				merge_bsp(&tr, &bt, ent);
			}
		} else {
			const box_t *x = &sc_boxes[idx];
			uint32_t c = x->solid == PM_SOLID_BSP ? CB_SOLID : CB_BODY;
			if (fc_on && !fc_box[idx])
				continue;
			ent = x->edict;
			if ((type & PM_MOVE_NOMONSTERS) && x->solid != PM_SOLID_BSP)
				continue;
			for (int k = 0; k < 3; k++) {
				amin[k] = x->mins[k] - 1;
				amax[k] = x->maxs[k] + 1;
			}
			if (amin[0] > tmax[0] || amin[1] > tmax[1] || amin[2] > tmax[2] || amax[0] < tmin[0] ||
			    amax[1] < tmin[1] || amax[2] < tmin[2])
				continue;
			if (!(c & (uint32_t)mask))
				continue;
			trace_aabb(start, mins, maxs, end, x->mins, x->maxs, &tr, ent, c);
		}
		if (tr.ent != prevent || tr.fraction != prevfrac) {
			hitkind = isbrush ? 1 : 2;
			hitidx = idx;
		}
	}

	memset(out, 0, sizeof(*out));
	out->fraction = tr.fraction;
	for (int k = 0; k < 3; k++) {
		out->endpos[k] = start[k] + (end[k] - start[k]) * tr.fraction;
		out->normal[k] = tr.normal[k];
	}
	out->startsolid = tr.startsolid;
	out->allsolid = tr.allsolid;
	out->contents = tr.contents;
	out->ent = tr.ent < 0 ? 0 : tr.ent;
	out->entsolid = PM_SOLID_BSP; /* the world, never linked: absmin/absmax 0 0 0 */
	if (out->ent > 0) {
		/* the entity in tr.ent: find it again by edict (the last one that set it) */
		const brush_t *b = NULL;
		const box_t *x = NULL;
		for (int i = 0; i < sc_nbrushes; i++)
			if (sc_brushes[i].edict == out->ent)
				b = &sc_brushes[i];
		for (int i = 0; i < sc_nboxes; i++)
			if (sc_boxes[i].edict == out->ent)
				x = &sc_boxes[i];
		if (hitkind == 1 && hitidx >= 0 && sc_brushes[hitidx].edict == out->ent)
			b = &sc_brushes[hitidx], x = NULL;
		else if (hitkind == 2 && hitidx >= 0 && sc_boxes[hitidx].edict == out->ent)
			x = &sc_boxes[hitidx], b = NULL;
		if (b) {
			out->entsolid = b->solid;
			brush_box(b, out->entmins, out->entmaxs);
		} else if (x) {
			out->entsolid = x->solid;
			for (int k = 0; k < 3; k++) {
				out->entmins[k] = x->mins[k] - 1;
				out->entmaxs[k] = x->maxs[k] + 1;
			}
		}
	}
}

static void own_move(void *ctx, const float start[3], const float mins[3], const float maxs[3], const float end[3],
                     int type, int32_t mask, pmhit_t *tr)
{
	bool point = mins[0] == 0 && mins[1] == 0 && mins[2] == 0 && maxs[0] == 0 && maxs[1] == 0 && maxs[2] == 0;
	int kind = (type & PM_MOVE_EVERYTHING) ? TK_CONTENTS : (type & PM_MOVE_NOMONSTERS) ? TK_LINE : point ? TK_POINT : TK_BOX;
	double t0 = timing ? now_us() : 0;
	(void)ctx;
#ifdef PMTEST_HULL_NDS
	if (ownhull) {
		own_move_core(start, mins, maxs, end, type, mask, tr);
	} else {
		Hull_Move(ctx, start, mins, maxs, end, type, mask, tr);
		if (cmphull) {
			/* the server's code on the same trace: any bit apart is counted */
			pmhit_t ref;
			bool saved = fc_on;
			fc_on = false;
			own_move_core(start, mins, maxs, end, type, mask, &ref);
			fc_on = saved;
			cmp_traces++;
			if (memcmp(&ref.fraction, &tr->fraction, sizeof(float) * 7) || ref.startsolid != tr->startsolid ||
			    ref.allsolid != tr->allsolid || ref.contents != tr->contents || ref.ent != tr->ent ||
			    ref.entsolid != tr->entsolid || memcmp(ref.entmins, tr->entmins, sizeof(float) * 6)) {
				if (cmp_bad++ < 20)
					printf("HULLDIFF type %d mask %d start %a %a %a end %a %a %a size %g: frac %a/%a ent %d/%d "
					       "solid %d/%d contents %#x/%#x\n",
					       type, (int)mask, start[0], start[1], start[2], end[0], end[1], end[2], maxs[0] - mins[0],
					       tr->fraction, ref.fraction, tr->ent, ref.ent, tr->startsolid, ref.startsolid,
					       tr->contents, ref.contents);
			}
		}
	}
#else
	own_move_core(start, mins, maxs, end, type, mask, tr);
#endif
	if (timing) {
		tk_time[kind] += now_us() - t0;
		tk_count[kind]++;
	}
}

static uint32_t own_focus(void *ctx, const float org[3], const float mins[3], const float maxs[3], float radius,
                          int flags)
{
	uint32_t near = 0;
	(void)ctx;
#ifdef PMTEST_HULL_NDS
	near = Hull_Focus(ctx, org, mins, maxs, radius, flags);
	if (!ownhull)
		return near;
	near = 0;
#endif
	{
	fc_on = true;
	for (int i = 0; i < sc_nbrushes; i++) {
		float amin[3], amax[3];
		bool in = true;
		brush_box(&sc_brushes[i], amin, amax);
		for (int k = 0; k < 3; k++)
			if (amin[k] > org[k] + radius + 1 || amax[k] < org[k] - radius - 1)
				in = false;
		fc_brush[i] = in;
		if (in)
			near |= skin_bits(sc_brushes[i].skin);
	}
	for (int i = 0; i < sc_nboxes; i++) {
		const box_t *x = &sc_boxes[i];
		bool in = true, stuck = true;
		for (int k = 0; k < 3; k++) {
			if (x->mins[k] - 1 > org[k] + radius + 1 || x->maxs[k] + 1 < org[k] - radius - 1)
				in = false;
			if (x->mins[k] >= org[k] + maxs[k] || x->maxs[k] <= org[k] + mins[k])
				stuck = false;
		}
		fc_box[i] = in && !((flags & PM_FOCUS_DROPSTUCK) && stuck);
	}
	}
	return near;
}

#ifdef PMTEST_HULL_NDS
static pmtrace_if trace_if = {NULL, own_move, own_focus, Hull_TouchOf, Hull_Triggers};
#else
static pmtrace_if trace_if = {NULL, own_move, own_focus, own_touchof, own_triggers};
#endif

static void set_scene(const brush_t *b, int nb, const box_t *x, int nx, const pmtouch_t *t, int nt)
{
	sc_touches = t;
	sc_ntouches = nt;
	sc_brushes = b;
	sc_nbrushes = nb;
	sc_boxes = x;
	sc_nboxes = nx;
	fc_on = false;
#ifdef PMTEST_HULL_NDS
	{
		static hullbrush_t hb[MAX_BRUSHES];
		static hullbox_t hx[MAX_BOXES];
		for (int i = 0; i < nb && i < MAX_BRUSHES; i++) {
			hb[i].edict = b[i].edict;
			hb[i].model = b[i].model;
			hb[i].solid = b[i].solid;
			hb[i].skin = b[i].skin;
			memcpy(hb[i].origin, b[i].origin, sizeof(hb[i].origin));
			memcpy(hb[i].angles, b[i].angles, sizeof(hb[i].angles));
		}
		for (int i = 0; i < nx && i < MAX_BOXES; i++) {
			hx[i].edict = x[i].edict;
			memcpy(hx[i].mins, x[i].mins, sizeof(hx[i].mins));
			memcpy(hx[i].maxs, x[i].maxs, sizeof(hx[i].maxs));
			hx[i].solid = x[i].solid;
		}
		Hull_SetBrushes(hb, nb < MAX_BRUSHES ? nb : MAX_BRUSHES);
		for (int i = 0; i < nb && i < MAX_BRUSHES; i++)
			if (b[i].hasabs && turned(&b[i]))
				Hull_BrushBox(i, b[i].amin, b[i].amax);
		Hull_SetBoxes(hx, nx < MAX_BOXES ? nx : MAX_BOXES);
		Hull_SetTouches(t, nt);
	}
#endif
}

static void run_cmd(pmstate_t *s, const pmcmd_t *c, const pmvars_t *v)
{
	trace_if.focus = nofocus ? NULL : own_focus;
	fc_on = false;
	PM_Run(s, c, v, &trace_if);
}

/* --- reading the log ---------------------------------------------------- */

static char *tok(char **p)
{
	char *s = *p, *t;
	while (*s == ' ' || *s == '\t')
		s++;
	if (!*s || *s == '\n' || *s == '\r')
		return NULL;
	t = s;
	while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
		s++;
	if (*s)
		*s++ = 0;
	*p = s;
	return t;
}

static bool tf(char **p, float *f)
{
	char *t = tok(p);
	if (!t)
		return false;
	*f = strtof(t, NULL);
	return true;
}

static bool ti(char **p, long *v)
{
	char *t = tok(p);
	if (!t)
		return false;
	*v = strtol(t, NULL, 0);
	return true;
}

static bool tv(char **p, float *v)
{
	return tf(p, &v[0]) && tf(p, &v[1]) && tf(p, &v[2]);
}

/* <STATE> of sv_link_log: the PSTATE body from origin to health */
static bool read_state(char **p, pmstate_t *s)
{
	float speedmod;
	long u[8], rules;
	memset(s, 0, sizeof(*s));
	if (!tv(p, s->origin) || !tv(p, s->velocity) || !tv(p, s->basevel) || !tv(p, s->hookvel) ||
	    !tv(p, s->view_ofs) || !tv(p, s->mins) || !tv(p, s->maxs))
		return false;
	for (int i = 0; i < 8; i++)
		if (!ti(p, &u[i]))
			return false;
	s->flags = (uint32_t)u[0];
	s->vflags = (uint32_t)u[1];
	s->gflags = (uint32_t)u[2];
	s->movetype = (int)u[3];
	s->solid = (int)u[4];
	s->waterlevel = (int)u[5];
	s->watertype = (int)u[6];
	s->groundent = (int)u[7];
	if (!tf(p, &s->gravity) || !tf(p, &s->friction) || !tf(p, &s->maxspeed) || !tf(p, &s->time_since_jump) ||
	    !tf(p, &s->time_since_teleport) || !tf(p, &s->stamina) || !tf(p, &s->speed_target) || !tf(p, &speedmod) ||
	    !tf(p, &s->wsm) || !tf(p, &s->wsmfire) || !tf(p, &s->wsmzoom) || !tf(p, &s->viewzoom) || !tf(p, &s->health) ||
	    !ti(p, &rules))
		return false;
	/* status bits 2-4, either as they sit in status or shifted down */
	s->rules = (uint8_t)(rules > 7 ? (rules >> 2) & 7 : rules);
	return true;
}

static int find_brush(int first, int n, int edict)
{
	for (int i = 0; i < n; i++)
		if (brushpool[first + i].edict == edict)
			return first + i;
	return -1;
}

static int read_log(const char *path, int slot)
{
	FILE *f = fopen(path, "r");
	static char line[65536];
	int brushlist = -1, nbrushlist = 0; /* the current "brush" version */
	int curbrushes = -1, ncurbrushes = 0, curboxes = 0, ncurboxes = 0, curvars = -1, curtouches = 0, ncurtouches = 0;
	step_t *st = NULL;
	if (!f) {
		fprintf(stderr, "pmtest: can't read %s\n", path);
		return -1;
	}
	steps = calloc(MAX_STEPS, sizeof(step_t));
	while (fgets(line, sizeof(line), f)) {
		char *p = strstr(line, "LINKPM "), *kind;
		long sl;
		if (!p)
			continue;
		p += 7;
		if (!ti(&p, &sl) || sl != slot || !(kind = tok(&p)))
			continue;
		if (!strcmp(kind, "map")) {
			brushlist = curbrushes = -1;
			nbrushlist = ncurbrushes = ncurboxes = ncurtouches = 0;
			printf("PMTEST log map %s", p);
		} else if (!strcmp(kind, "vars")) {
			pmvars_t v;
			memset(&v, 0, sizeof(v));
			for (int i = 0; i < PMV_COUNT; i++)
				tf(&p, &v.g[i]);
			for (int i = 0; i < PMV_COUNT; i++)
				tf(&p, &v.p[i]);
			PM_SetVars(&v);
			varsets = grow(varsets, &capvarsets, numvarsets + 1, sizeof(pmvars_t));
			varsets[numvarsets] = v;
			curvars = numvarsets++;
		} else if (!strcmp(kind, "brush")) {
			long n, e, m, so, sk;
			if (!ti(&p, &n) || n > MAX_BRUSHES)
				continue;
			brushpool = grow(brushpool, &capbrushpool, numbrushpool + (int)n, sizeof(brush_t));
			brushlist = numbrushpool;
			for (nbrushlist = 0; nbrushlist < n; nbrushlist++) {
				brush_t *b = &brushpool[numbrushpool + nbrushlist];
				if (!ti(&p, &e) || !ti(&p, &m) || !ti(&p, &so) || !ti(&p, &sk) || !tv(&p, b->origin))
					break;
				b->edict = (uint16_t)e;
				b->model = (uint16_t)m;
				b->solid = (uint8_t)so;
				b->skin = (int8_t)sk;
				memset(b->angles, 0, sizeof(b->angles));
				b->hasabs = false;
			}
			numbrushpool += nbrushlist;
			curbrushes = brushlist;
			ncurbrushes = nbrushlist;
		} else if (!strcmp(kind, "brushpos")) {
			/* this command's origins: a copy of the list with them in */
			long n, e;
			float o[3];
			if (brushlist < 0 || !ti(&p, &n))
				continue;
			if (!n) {
				curbrushes = brushlist;
				ncurbrushes = nbrushlist;
				continue;
			}
			brushpool = grow(brushpool, &capbrushpool, numbrushpool + nbrushlist, sizeof(brush_t));
			memcpy(&brushpool[numbrushpool], &brushpool[brushlist], sizeof(brush_t) * (size_t)nbrushlist);
			for (int i = 0; i < n; i++) {
				int at;
				if (!ti(&p, &e) || !tv(&p, o))
					break;
				at = find_brush(numbrushpool, nbrushlist, (int)e);
				if (at >= 0)
					memcpy(brushpool[at].origin, o, sizeof(o));
			}
			curbrushes = numbrushpool;
			ncurbrushes = nbrushlist;
			numbrushpool += nbrushlist;
		} else if (!strcmp(kind, "brushang") || !strcmp(kind, "brushabs")) {
			/* this command's turned brushes (after brushpos): a copy of the current list with
			 * their angles (brushang: the rest 0), then their absmin/absmax (brushabs) */
			bool ang = kind[5] == 'a' && kind[6] == 'n';
			long n, e;
			float a[3], c[3];
			if (curbrushes < 0 || !ti(&p, &n))
				continue;
			brushpool = grow(brushpool, &capbrushpool, numbrushpool + ncurbrushes, sizeof(brush_t));
			memcpy(&brushpool[numbrushpool], &brushpool[curbrushes], sizeof(brush_t) * (size_t)ncurbrushes);
			if (ang)
				for (int i = 0; i < ncurbrushes; i++) {
					memset(brushpool[numbrushpool + i].angles, 0, sizeof(a));
					brushpool[numbrushpool + i].hasabs = false;
				}
			for (int i = 0; i < n; i++) {
				int at;
				if (!ti(&p, &e) || !tv(&p, a) || (!ang && !tv(&p, c)))
					break;
				at = find_brush(numbrushpool, ncurbrushes, (int)e);
				if (at < 0)
					continue;
				if (ang)
					memcpy(brushpool[at].angles, a, sizeof(a));
				else {
					memcpy(brushpool[at].amin, a, sizeof(a));
					memcpy(brushpool[at].amax, c, sizeof(c));
					brushpool[at].hasabs = true;
				}
			}
			curbrushes = numbrushpool;
			numbrushpool += ncurbrushes;
		} else if (!strcmp(kind, "boxes")) {
			long n, e, so;
			if (!ti(&p, &n))
				continue;
			if (n > MAX_BOXES)
				n = MAX_BOXES;
			boxpool = grow(boxpool, &capboxpool, numboxpool + (int)n, sizeof(box_t));
			curboxes = numboxpool;
			for (ncurboxes = 0; ncurboxes < n; ncurboxes++) {
				box_t *x = &boxpool[numboxpool + ncurboxes];
				if (!ti(&p, &e) || !tv(&p, x->mins) || !tv(&p, x->maxs) || !ti(&p, &so))
					break;
				x->edict = (uint16_t)e;
				x->solid = (uint8_t)so;
			}
			numboxpool += ncurboxes;
		} else if (!strcmp(kind, "touch")) {
			long n, e, kd, fl;
			if (!ti(&p, &n))
				continue;
			touchpool = grow(touchpool, &captouchpool, numtouchpool + (int)n, sizeof(pmtouch_t));
			curtouches = numtouchpool;
			for (ncurtouches = 0; ncurtouches < n; ncurtouches++) {
				pmtouch_t *t = &touchpool[numtouchpool + ncurtouches];
				if (!ti(&p, &e) || !ti(&p, &kd) || !ti(&p, &fl) || !tf(&p, &t->speed) || !tv(&p, t->dir) ||
				    !tv(&p, t->absmin) || !tv(&p, t->absmax))
					break;
				t->edict = (uint16_t)e, t->kind = (uint8_t)kd, t->flags = (uint8_t)fl;
			}
			numtouchpool += ncurtouches;
		} else if (!strcmp(kind, "pre")) {
			if (numsteps >= MAX_STEPS || curvars < 0)
				continue;
			st = &steps[numsteps];
			memset(st, 0, sizeof(*st));
			if (!read_state(&p, &st->pre)) {
				st = NULL;
				continue;
			}
			st->vars = curvars;
			st->brushes = curbrushes < 0 ? 0 : curbrushes;
			st->nbrushes = curbrushes < 0 ? 0 : ncurbrushes;
			st->boxes = curboxes;
			st->nboxes = ncurboxes;
			st->touches = curtouches;
			st->ntouches = ncurtouches;
		} else if (!strcmp(kind, "cmd") && st) {
			long v[9], ranimp;
			for (int i = 0; i < 9; i++)
				if (!ti(&p, &v[i]))
					goto bad;
			if (!tf(&p, &st->ran_msec) || !ti(&p, &ranimp))
				goto bad;
			st->cmd.seq = (uint32_t)v[0];
			st->cmd.msec = (uint8_t)v[1];
			st->cmd.pitch = (int16_t)v[2];
			st->cmd.yaw = (int16_t)v[3];
			st->cmd.move[0] = (int16_t)v[4];
			st->cmd.move[1] = (int16_t)v[5];
			st->cmd.move[2] = (int16_t)v[6];
			st->cmd.buttons = (uint16_t)v[7];
			st->cmd.impulse = (uint8_t)ranimp;
			st->clipped = st->ran_msec != (float)st->cmd.msec / 1000.0f;
			continue;
		bad:
			st = NULL;
		} else if (!strcmp(kind, "post") && st) {
			if (read_state(&p, &st->post)) {
				st->haspost = true;
				numsteps++;
			}
			st = NULL;
		}
	}
	fclose(f);
	return numsteps;
}

/* --- the script (no server) -------------------------------------------- *
 *
 *   PM var <name> <value>     sets the cvar in both sets (valve's defaults first)
 *   PM spawn x y z [rules]    a standing player there, on nothing yet
 *   PM brush edict model solid skin ox oy oz
 *   PM box edict x0 y0 z0 x1 y1 z1 solid
 *   PM cmd msec pitch yaw fwd side up buttons [count]
 *                             (pitch and yaw in degrees, up positive)
 * Every command's state is printed; -emit writes them as an sv_link_log log. */

static const char *pmv_names[PMV_COUNT] = {
	"g_gravity", "pm_accelerate", "pm_airaccelerate", "pm_airstepsize", "pm_boxcenter", "pm_boxwidth",
	"pm_crouchheight", "pm_crouchspeed", "pm_crouchviewheight", "pm_edgefriction", "pm_friction", "pm_gravity",
	"pm_jumpheight", "pm_airjumps", "pm_backspeed", "pm_groundspeedcap", "pm_jumpspeedcap", "pm_maxviewpitch",
	"pm_minviewpitch", "pm_noclipaccelerate", "pm_noclipspeed", "pm_normalheight", "pm_normalviewheight",
	"pm_nospeedcap", "pm_proneheight", "pm_pronespeed", "pm_proneviewheight", "pm_runspeed", "pm_runfiring",
	"pm_stamina", "pm_staminarate", "pm_staminathreshold", "pm_stepsize", "pm_stopspeed", "pm_walkspeed",
	"pm_wateraccelerate", "pm_waterjumpheight"};

/* valve/default_cvar.cfg:64, 117-150; the rest are pmove.qc's autocvar defaults (:118-124) */
static const float pmv_valve[PMV_COUNT] = {800, 10, 10, 0, 1, 32, 36, 90, 30, 1, 4, 800, 265, 0, 1, 0, 0, 89, -89,
                                           5, 500, 72, 64, 0, 0, 40.5f, 16, 0, 1, 24, 0.75f, 4, 18, 100, 270, 10, 350};

static void print_state(const char *what, uint32_t seq, const pmstate_t *s)
{
	printf("%s %u org %.4f %.4f %.4f vel %.4f %.4f %.4f fl %#x vfl %#x gnd %d wl %d mins %g %g %g maxs %g %g %g view %g%s%s\n",
	       what, seq, s->origin[0], s->origin[1], s->origin[2], s->velocity[0], s->velocity[1], s->velocity[2],
	       s->flags, s->vflags, s->groundent, s->waterlevel, s->mins[0], s->mins[1], s->mins[2], s->maxs[0],
	       s->maxs[1], s->maxs[2], s->view_ofs[2], (s->events & PM_EV_JUMPED) ? " JUMP" : "",
	       (s->events & PM_EV_LANDED) ? " LAND" : "");
}

static void emit_state(FILE *f, const pmstate_t *s)
{
	const float *v[7] = {s->origin, s->velocity, s->basevel, s->hookvel, s->view_ofs, s->mins, s->maxs};
	for (int i = 0; i < 7; i++)
		for (int k = 0; k < 3; k++)
			fprintf(f, " %a", v[i][k]);
	fprintf(f, " %u %u %u %d %d %d %d %d", s->flags, s->vflags, s->gflags, s->movetype, s->solid, s->waterlevel,
	        s->watertype, s->groundent);
	fprintf(f, " %a %a %a %a %a %a %a %a %a %a %a %a %a %u\n", s->gravity, s->friction, s->maxspeed,
	        s->time_since_jump, s->time_since_teleport, s->stamina, s->speed_target, 1.0f, s->wsm, s->wsmfire,
	        s->wsmzoom, s->viewzoom, s->health, s->rules);
}

static int run_script(const char *path, const char *emit)
{
	FILE *f = fopen(path, "r"), *out = NULL;
	char line[4096];
	pmvars_t v;
	pmstate_t s;
	static brush_t brushes[MAX_BRUSHES];
	static box_t boxes[MAX_BOXES];
	int nb = 0, nx = 0;
	uint32_t seq = 0;
	bool spawned = false;
	if (!f)
		return -1;
	if (emit && !(out = fopen(emit, "w"))) {
		fprintf(stderr, "pmtest: can't write %s\n", emit);
		return -1;
	}
	memset(&v, 0, sizeof(v));
	memcpy(v.g, pmv_valve, sizeof(v.g));
	memcpy(v.p, pmv_valve, sizeof(v.p));
	PM_SetVars(&v);
	memset(&s, 0, sizeof(s));
	while (fgets(line, sizeof(line), f)) {
		char *p = line, *t = tok(&p), *kind;
		if (!t || strcmp(t, "PM") || !(kind = tok(&p)))
			continue;
		if (!strcmp(kind, "var")) {
			char *name = tok(&p);
			float val;
			if (!name || !tf(&p, &val))
				continue;
			for (int i = 0; i < PMV_COUNT; i++)
				if (!strcmp(name, pmv_names[i]))
					v.g[i] = v.p[i] = val;
			PM_SetVars(&v);
		} else if (!strcmp(kind, "spawn")) {
			long rules = PM_R_HL;
			memset(&s, 0, sizeof(s));
			tv(&p, s.origin);
			ti(&p, &rules);
			s.rules = (uint8_t)rules;
			memcpy(s.mins, v.standmins, sizeof(s.mins));
			memcpy(s.maxs, v.standmaxs, sizeof(s.maxs));
			memcpy(s.view_ofs, v.standview, sizeof(s.view_ofs));
			s.flags = PM_FL_CLIENT | PM_FL_JUMPRELEASED;
			s.movetype = PM_MOVETYPE_WALK;
			s.solid = PM_SOLID_SLIDEBOX;
			s.watertype = PM_CONTENT_EMPTY;
			s.health = 100;
			s.wsm = 1;
			s.viewzoom = 1;
			spawned = true;
		} else if (!strcmp(kind, "brush") && nb < MAX_BRUSHES) {
			long e, m, so, sk;
			if (ti(&p, &e) && ti(&p, &m) && ti(&p, &so) && ti(&p, &sk) && tv(&p, brushes[nb].origin)) {
				brushes[nb].edict = (uint16_t)e, brushes[nb].model = (uint16_t)m;
				brushes[nb].solid = (uint8_t)so, brushes[nb].skin = (int8_t)sk;
				nb++;
			}
		} else if (!strcmp(kind, "box") && nx < MAX_BOXES) {
			long e, so;
			if (ti(&p, &e) && tv(&p, boxes[nx].mins) && tv(&p, boxes[nx].maxs) && ti(&p, &so)) {
				boxes[nx].edict = (uint16_t)e, boxes[nx].solid = (uint8_t)so;
				nx++;
			}
		} else if (!strcmp(kind, "cmd") && spawned) {
			long msec, but, count = 1;
			float pitch, yaw, mv[3];
			pmcmd_t c;
			if (!ti(&p, &msec) || !tf(&p, &pitch) || !tf(&p, &yaw) || !tv(&p, mv) || !ti(&p, &but))
				continue;
			ti(&p, &count);
			memset(&c, 0, sizeof(c));
			c.msec = (uint8_t)msec;
			c.pitch = (int16_t)lrintf(pitch * 65536 / 360);
			c.yaw = (int16_t)lrintf(yaw * 65536 / 360);
			for (int k = 0; k < 3; k++)
				c.move[k] = (int16_t)mv[k];
			c.buttons = (uint16_t)but;
			set_scene(brushes, nb, boxes, nx, NULL, 0);
			for (long i = 0; i < count; i++) {
				pmstate_t pre = s;
				c.seq = ++seq;
				run_cmd(&s, &c, &v);
				print_state("STEP", c.seq, &s);
				if (out) {
					if (seq == 1) {
						fprintf(out, "LINKPM 0 map script 1\nLINKPM 0 vars");
						for (int k = 0; k < PMV_COUNT; k++)
							fprintf(out, " %a", v.g[k]);
						for (int k = 0; k < PMV_COUNT; k++)
							fprintf(out, " %a", v.p[k]);
						fprintf(out, "\nLINKPM 0 brush %d", nb);
						for (int k = 0; k < nb; k++)
							fprintf(out, " %u %u %u %d %a %a %a", brushes[k].edict, brushes[k].model,
							        brushes[k].solid, brushes[k].skin, brushes[k].origin[0],
							        brushes[k].origin[1], brushes[k].origin[2]);
						fprintf(out, "\n");
					}
					fprintf(out, "LINKPM 0 brushpos 0\nLINKPM 0 boxes %d", nx);
					for (int k = 0; k < nx; k++)
						fprintf(out, " %u %a %a %a %a %a %a %u", boxes[k].edict, boxes[k].mins[0],
						        boxes[k].mins[1], boxes[k].mins[2], boxes[k].maxs[0], boxes[k].maxs[1],
						        boxes[k].maxs[2], boxes[k].solid);
					fprintf(out, "\nLINKPM 0 pre");
					emit_state(out, &pre);
					fprintf(out, "LINKPM 0 cmd %u %u %d %d %d %d %d %u %u %a %u\n", c.seq, c.msec, c.pitch,
					        c.yaw, c.move[0], c.move[1], c.move[2], c.buttons, c.impulse,
					        (float)c.msec / 1000.0f, c.impulse);
					fprintf(out, "LINKPM 0 post");
					emit_state(out, &s);
				}
			}
		}
	}
	fclose(f);
	if (out)
		fclose(out);
	return (int)seq;
}

/* --- comparing ------------------------------------------------------------ */

#define CMP_FLAGS (PM_FL_ONGROUND | PM_FL_WATERJUMP | PM_FL_JUMPRELEASED | PM_FL_INWATER)
#define CMP_VFLAGS (PM_VFL_CROUCHING | PM_VFL_ONLADDER | PM_VFL_SPRINTING | PM_VFL_AIRJUMPS)

static float dist3(const float *a, const float *b)
{
	float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
	return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

static bool same_bits(const float *a, const float *b, int n)
{
	return !memcmp(a, b, sizeof(float) * (size_t)n);
}

/* what differs besides origin and velocity, as words; "" when nothing */
static const char *flag_diff(const pmstate_t *a, const pmstate_t *b)
{
	static char buf[256];
	buf[0] = 0;
	if ((a->flags ^ b->flags) & CMP_FLAGS)
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " flags %#x/%#x", a->flags & CMP_FLAGS,
		         b->flags & CMP_FLAGS);
	if ((a->vflags ^ b->vflags) & CMP_VFLAGS)
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " vflags %#x/%#x", a->vflags & CMP_VFLAGS,
		         b->vflags & CMP_VFLAGS);
	if (a->waterlevel != b->waterlevel)
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " waterlevel %d/%d", a->waterlevel, b->waterlevel);
	if (memcmp(a->mins, b->mins, 12) || memcmp(a->maxs, b->maxs, 12))
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " box");
	if (memcmp(a->view_ofs, b->view_ofs, 12))
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " view_ofs %g/%g", a->view_ofs[2], b->view_ofs[2]);
	if (a->groundent != b->groundent)
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " ground %d/%d", a->groundent, b->groundent);
	return buf;
}

static void scene_of(const step_t *st)
{
	set_scene(brushpool ? &brushpool[st->brushes] : NULL, st->nbrushes, boxpool ? &boxpool[st->boxes] : NULL,
	          st->nboxes, touchpool ? &touchpool[st->touches] : NULL, st->ntouches);
}

/* -dump: every value PM_Run writes, as bits */
static FILE *dumpf;
static void dump_state(const pmstate_t *s)
{
	if (!dumpf)
		return;
	fwrite(s->origin, 4, 3, dumpf), fwrite(s->velocity, 4, 3, dumpf), fwrite(s->basevel, 4, 3, dumpf);
	fwrite(s->view_ofs, 4, 3, dumpf), fwrite(s->mins, 4, 3, dumpf), fwrite(s->maxs, 4, 3, dumpf);
	fwrite(s->v_angle, 4, 3, dumpf), fwrite(s->angles, 4, 3, dumpf);
	fwrite(&s->flags, 4, 1, dumpf), fwrite(&s->vflags, 4, 1, dumpf), fwrite(&s->gflags, 4, 1, dumpf);
	fwrite(&s->waterlevel, 4, 1, dumpf), fwrite(&s->watertype, 4, 1, dumpf), fwrite(&s->groundent, 4, 1, dumpf);
	fwrite(&s->friction, 4, 1, dumpf), fwrite(&s->maxspeed, 4, 1, dumpf), fwrite(&s->time_since_jump, 4, 1, dumpf);
	fwrite(&s->time_since_teleport, 4, 1, dumpf), fwrite(&s->stamina, 4, 1, dumpf);
	fwrite(&s->landspeed, 4, 1, dumpf), fwrite(&s->events, 1, 1, dumpf);
}

/* variant k of a step: a deterministic shake of where it starts and what it asks */
static void fuzz_step(int k, pmstate_t *s, pmcmd_t *c)
{
	static const float d[8] = {0.03125f, -0.5f, 1.75f, -3.0f, 7.5f, 0.0078125f, -12.25f, 0.33f};
	uint32_t h = (uint32_t)k * 2654435761u ^ c->seq * 40503u;
	switch (k & 7) {
	case 0: s->origin[0] += d[h % 8], s->origin[1] += d[(h >> 3) % 8]; break;
	case 1: s->origin[2] += d[h % 8] * 0.25f; break;
	case 2: s->velocity[0] += d[h % 8] * 40, s->velocity[1] -= d[(h >> 3) % 8] * 40; break;
	case 3: s->velocity[2] += d[h % 8] * 30, s->flags ^= PM_FL_ONGROUND; break;
	case 4: c->yaw = (int16_t)(c->yaw + (int)(h % 8192) - 4096), c->pitch = (int16_t)((int)(h >> 13) % 16384 - 8192); break;
	case 5: c->move[0] = (int16_t)((int)(h % 3) * 400 - 400), c->move[1] = (int16_t)((int)(h >> 4) % 3 * 400 - 400); break;
	case 6: c->buttons ^= (uint16_t)((h & 1 ? PM_IN_JUMP : 0) | (h & 2 ? PM_IN_CROUCH : 0) | (h & 4 ? PM_IN_SPRINT : 0) | (h & 8 ? PM_IN_USE : 0)); break;
	case 7: c->msec = (uint8_t)(1 + h % 60); s->origin[0] -= d[h % 8], s->velocity[2] -= 100; break;
	}
}

static void dump_all(int nfuzz)
{
	pmstate_t cur;
	for (int i = 0; i < numsteps; i++) {
		pmstate_t s = steps[i].pre;
		scene_of(&steps[i]);
		run_cmd(&s, &steps[i].cmd, &varsets[steps[i].vars]);
		dump_state(&s);
		/* the chain: from the first state on, keeping our own */
		if (i == 0 || steps[i].cmd.seq != steps[i - 1].cmd.seq + 1)
			cur = steps[i].pre;
		run_cmd(&cur, &steps[i].cmd, &varsets[steps[i].vars]);
		dump_state(&cur);
		for (int k = 0; k < nfuzz; k++) {
			pmcmd_t c = steps[i].cmd;
			s = steps[i].pre;
			fuzz_step(k, &s, &c);
			run_cmd(&s, &c, &varsets[steps[i].vars]);
			dump_state(&s);
		}
	}
}

static int compare(float tol, bool chain)
{
	int within = 0, exact = 0, clipped = 0, flagbad = 0, n = 0, restarts = 0;
	uint32_t firstbad = 0;
	double orgsum = 0;
	float orgmax = 0, velmax = 0;
	pmstate_t cur;
	for (int i = 0; i < numsteps; i++) {
		step_t *st = &steps[i];
		pmstate_t s;
		float oe, ve;
		const char *fd;
		/* a new join, a lost command, or the server changed the player between
		 * two commands (a teleport, a pusher): start again from its state */
		if (!chain || i == 0 || st->cmd.seq != steps[i - 1].cmd.seq + 1 ||
		    memcmp(&st->pre, &steps[i - 1].post, offsetof(pmstate_t, v_angle))) {
			if (chain && i) {
				printf("CHAIN restart at %u\n", st->cmd.seq);
				restarts++;
			}
			cur = st->pre;
		}
		s = cur;
		scene_of(st);
		run_cmd(&s, &st->cmd, &varsets[st->vars]);
		cur = s;
		if (st->clipped) {
			printf("CLIPPED %u msec %u ran %a\n", st->cmd.seq, st->cmd.msec, st->ran_msec);
			clipped++;
			if (chain)
				cur = st->post; /* the server ran another command: take its result */
			continue;
		}
		n++;
		oe = dist3(s.origin, st->post.origin);
		ve = dist3(s.velocity, st->post.velocity);
		fd = flag_diff(&s, &st->post);
		orgsum += oe;
		if (oe > orgmax)
			orgmax = oe;
		if (ve > velmax)
			velmax = ve;
		if (oe <= tol)
			within++;
		if (same_bits(s.origin, st->post.origin, 3) && same_bits(s.velocity, st->post.velocity, 3) &&
		    same_bits(s.mins, st->post.mins, 3) && same_bits(s.maxs, st->post.maxs, 3) &&
		    same_bits(s.view_ofs, st->post.view_ofs, 3))
			exact++;
		if (*fd) {
			flagbad++;
			if (!firstbad)
				firstbad = st->cmd.seq;
		}
		if (verbose || oe > tol || *fd) {
			printf("STEP %u msec %u move %d %d %d but %#x org_err %.5f vel_err %.4f%s\n", st->cmd.seq,
			       st->cmd.msec, st->cmd.move[0], st->cmd.move[1], st->cmd.move[2], st->cmd.buttons, oe, ve, fd);
			if (verbose) {
				print_state("  pre ", st->cmd.seq, &st->pre);
				print_state("  ours", st->cmd.seq, &s);
				print_state("  srv ", st->cmd.seq, &st->post);
			}
		}
		if (oe > tol && !firstbad)
			firstbad = st->cmd.seq;
	}
	printf("PMTEST steps=%d within=%d exact=%d clipped=%d org_max=%.5f org_avg=%.5f vel_max=%.4f flag_bad=%d "
	       "first_bad=%u",
	       n, within, exact, clipped, orgmax, n ? orgsum / n : 0.0, velmax, flagbad, firstbad);
	if (chain)
		printf(" chain restarts=%d", restarts);
	printf("\n");
	return orgmax > tol || flagbad > 0;
}

/* the same stream cut into ms-long commands (§6.1 of the prediction spec) */
typedef struct {
	bool in;
	float z0, top;
	int jumps;
} jump_t;

static void jump_track(jump_t *j, const pmstate_t *s, const char *what)
{
	if (s->events & PM_EV_JUMPED) {
		if (j->in)
			printf("RESAMPLE %s jump apex %.3f (jumped again before landing)\n", what, j->top - j->z0);
		j->in = true;
		j->z0 = j->top = s->origin[2];
		j->jumps++;
		return;
	}
	if (j->in && s->origin[2] > j->top)
		j->top = s->origin[2];
	if (j->in && (s->events & PM_EV_LANDED)) {
		printf("RESAMPLE %s jump apex %.3f\n", what, j->top - j->z0);
		j->in = false;
	}
}

static void resample(int ms)
{
	pmstate_t a = steps[0].pre, b = steps[0].pre;
	float maxd = 0;
	uint32_t maxseq = 0;
	jump_t ja = {0}, jb = {0};
	char name[32];
	snprintf(name, sizeof(name), "%dms", ms);
	for (int i = 0; i < numsteps; i++) {
		step_t *st = &steps[i];
		pmcmd_t c = st->cmd;
		int left = st->cmd.msec;
		if (i && st->cmd.seq != steps[i - 1].cmd.seq + 1)
			break; /* only the first unbroken run */
		scene_of(st);
		run_cmd(&a, &st->cmd, &varsets[st->vars]);
		jump_track(&ja, &a, "logged");
		while (left > 0) {
			c.msec = (uint8_t)(left > ms ? ms : left);
			left -= c.msec;
			run_cmd(&b, &c, &varsets[st->vars]);
			jump_track(&jb, &b, name);
		}
		if (dist3(a.origin, b.origin) > maxd)
			maxd = dist3(a.origin, b.origin), maxseq = st->cmd.seq;
	}
	printf("PMTEST resample=%d max_pos_diff=%.4f at=%u end_diff=%.4f jumps=%d/%d\n", ms, maxd, maxseq,
	       dist3(a.origin, b.origin), ja.jumps, jb.jumps);
}

static void bench(void)
{
	double t0, total;
	long runs = 0;
	timing = false;
	t0 = now_us();
	for (int i = 0; i < numsteps; i++) {
		scene_of(&steps[i]);
		for (int r = 0; r < 1000; r++) {
			pmstate_t s = steps[i].pre;
			run_cmd(&s, &steps[i].cmd, &varsets[steps[i].vars]);
			runs++;
		}
	}
	total = now_us() - t0;
	timing = true;
	memset(tk_count, 0, sizeof(tk_count));
	memset(tk_time, 0, sizeof(tk_time));
	for (int i = 0; i < numsteps; i++) {
		scene_of(&steps[i]);
		for (int r = 0; r < 100; r++) {
			pmstate_t s = steps[i].pre;
			run_cmd(&s, &steps[i].cmd, &varsets[steps[i].vars]);
		}
	}
	timing = false;
	printf("PMTEST bench runs=%ld us_per_run=%.3f", runs, runs ? total / runs : 0.0);
	for (int k = 0; k < TK_NUM; k++)
		printf(" %s=%.2f/%.3fus", tk_names[k], numsteps ? tk_count[k] / (numsteps * 100.0) : 0.0,
		       tk_count[k] ? tk_time[k] / tk_count[k] : 0.0);
	printf(" (per run: count/us each, host, timer included)\n");
}

#ifdef PMTEST_HULL_NDS
static long file_read(void *ctx, const char *path, long ofs, long len, void *dst)
{
	FILE *f = fopen(path, "rb");
	long n;
	(void)ctx;
	if (!f)
		return 0;
	if (fseek(f, ofs, SEEK_SET)) {
		fclose(f);
		return 0;
	}
	n = (long)fread(dst, 1, (size_t)len, f);
	fclose(f);
	return n;
}
#endif

/* -selftest: the exact replacements against the host's own functions */
static int selftest(void)
{
	const float d2r = (float)(3.14159265358979323846 / 180.0), k = 0x1.1df45cp-6f;
	long bad = 0, n = 0;
	uint32_t u = 0;
	for (int c = 0; c < 2; c++)
		for (int i = -32768; i <= 32768; i++) {
			float a = i * (360.0f / 65536.0f), x = a * (c ? k : d2r), s, co;
			PM_SinCos(x, &s, &co);
			n++;
			if (fx_bits(s) != fx_bits(sinf(x)) || fx_bits(co) != fx_bits(cosf(x))) {
				if (bad++ < 5)
					printf("SELFTEST sincos %a: %a %a, sinf/cosf %a %a\n", x, s, co, sinf(x), cosf(x));
			}
		}
	printf("PMTEST selftest sincos angles=%ld differ=%ld\n", n, bad);
	n = bad;
	bad = 0;
	do {
		float x = fx_float(u);
		if (fx_bits(fx_sqrt(x)) != fx_bits(sqrtf(x)) && bad++ < 5)
			printf("SELFTEST sqrt %08x: %08x, sqrtf %08x\n", u, fx_bits(fx_sqrt(x)), fx_bits(sqrtf(x)));
	} while (++u);
	printf("PMTEST selftest sqrt floats=4294967296 differ=%ld\n", bad);
	return n || bad;
}

int main(int argc, char **argv)
{
	const char *mappath = NULL, *logpath = NULL, *emit = NULL, *dumppath = NULL;
	int slot = 0, resample_ms = 0, status = 0, nfuzz = 8;
	float tol = 0.05f;
	bool chain = false, dobench = false;
	if (argc == 2 && !strcmp(argv[1], "-selftest"))
		return selftest();
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-slot") && i + 1 < argc)
			slot = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-tol") && i + 1 < argc)
			tol = strtof(argv[++i], NULL);
		else if (!strcmp(argv[i], "-chain"))
			chain = true;
		else if (!strcmp(argv[i], "-resample") && i + 1 < argc)
			resample_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-bench"))
			dobench = true;
		else if (!strcmp(argv[i], "-nofocus"))
			nofocus = true;
		else if (!strcmp(argv[i], "-dshull0"))
			dshull0 = true;
		else if (!strcmp(argv[i], "-ownhull"))
			ownhull = true;
		else if (!strcmp(argv[i], "-cmphull"))
			cmphull = true;
		else if (!strcmp(argv[i], "-v"))
			verbose = true;
		else if (!strcmp(argv[i], "-emit") && i + 1 < argc)
			emit = argv[++i];
		else if (!strcmp(argv[i], "-dump") && i + 1 < argc)
			dumppath = argv[++i];
		else if (!strcmp(argv[i], "-fuzz") && i + 1 < argc)
			nfuzz = atoi(argv[++i]);
		else if (!mappath)
			mappath = argv[i];
		else if (!logpath)
			logpath = argv[i];
	}
	if (!mappath || !logpath) {
		fprintf(stderr, "usage: pmtest <map.bsp> <ded.log | script> [-slot n] [-tol 0.05] [-chain] "
		                "[-resample ms] [-bench] [-nofocus] [-dshull0] [-ownhull] [-cmphull] [-v] [-emit log] "
		                "[-dump file [-fuzz n]]\n");
		return 2;
	}
#ifdef PMTEST_HULL_NDS
	{
		char err[256] = "";
		/* -dshull0: hull 0 is the DS's fixed-point drawing tree, walked as on the DS */
		if (!Hull_Load(mappath, dshull0 ? HULL_HULL0_FIXED : HULL_WITH_HULL0, file_read, NULL, 0, err, sizeof(err))) {
			fprintf(stderr, "pmtest: Hull_Load %s: %s\n", mappath, err);
			return 2;
		}
		printf("PMTEST hull_nds rule=%d bytes=%zu models=%d\n", Hull_Rule(), Hull_Bytes(), Hull_NumModels());
	}
	if (!map_load(mappath)) /* the brush bounds (brush_box) */
		return 2;
#else
	if (dshull0) {
		fprintf(stderr, "pmtest: -dshull0 needs -DPMTEST_HULL_NDS and hull_nds.c\n");
		return 2;
	}
	if (!map_load(mappath))
		return 2;
#endif
	if (read_log(logpath, slot) <= 0) {
		int n = run_script(logpath, emit);
		if (n > 0) {
			printf("PMTEST script commands=%d\n", n);
			return 0;
		}
		fprintf(stderr, "pmtest: no LINKPM steps for slot %d and no PM script in %s\n", slot, logpath);
		return 2;
	}
	printf("PMTEST log steps=%d varsets=%d\n", numsteps, numvarsets);
	status = compare(tol, false);
	if (chain)
		compare(tol, true);
	if (nofocus == false) {
		/* the broadphase must change nothing: run every step without it too */
		int diff = 0;
		for (int i = 0; i < numsteps; i++) {
			pmstate_t a = steps[i].pre, b = steps[i].pre;
			scene_of(&steps[i]);
			run_cmd(&a, &steps[i].cmd, &varsets[steps[i].vars]);
			nofocus = true;
			run_cmd(&b, &steps[i].cmd, &varsets[steps[i].vars]);
			nofocus = false;
			if (memcmp(a.origin, b.origin, 12) || memcmp(a.velocity, b.velocity, 12) || a.flags != b.flags ||
			    a.vflags != b.vflags || a.groundent != b.groundent)
				diff++;
		}
		printf("PMTEST focus_vs_nofocus differ=%d\n", diff);
		if (diff)
			status = 1;
	}
	if (dumppath) {
		dumpf = fopen(dumppath, "wb");
		if (!dumpf) {
			fprintf(stderr, "pmtest: can't write %s\n", dumppath);
			return 2;
		}
		dump_all(nfuzz);
		printf("PMTEST dump %s results=%ld\n", dumppath, (long)numsteps * (2 + nfuzz));
		fclose(dumpf);
		dumpf = NULL;
	}
	if (cmphull)
		printf("PMTEST cmphull traces=%ld differ=%ld\n", cmp_traces, cmp_bad);
	if (cmp_bad)
		status = 1;
	if (resample_ms > 0)
		resample(resample_ms);
	if (dobench)
		bench();
	return status;
}
