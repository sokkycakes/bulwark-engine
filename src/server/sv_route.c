/* Bulwark engine - waypoint routes for the route_calculate builtin.
 * ISC licence, see LICENSE.
 *
 * The graph is the progs' own waypoint file, data/<map>.way:
 *   <node count>
 *   <x> <y> <z> <radius> <link count>      once per node
 *    <node> <cost> <flags>                 once per link
 * route_calculate(ent, dest, denyflags, callback) finds a path with A* and
 * calls callback(ent, dest, numnodes, nodelist) at the start of the next
 * server frame. nodelist is heap memory the progs frees with memfree; it is
 * ordered last to first (the first node to walk to is nodelist[numnodes-1])
 * and its element 0 is dest itself (the only element when there is no path). Each element is { vector dest; int
 * linkflags; float radius; }. */
#include "sv_local.h"
#include "../common/fs.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define F vm->fofs
#define G vm->gofs

typedef struct {
	int to;
	float cost;
	int flags;
} waylink_t;

typedef struct {
	float org[3];
	float radius;
	int firstlink, numlinks;
} waynode_t;

static struct {
	char map[64];
	int spawncount;
	waynode_t *nodes;
	int numnodes;
	waylink_t *links;
	int numlinks;
} way;

typedef struct {
	int ent;
	float dest[3];
	int deny;
	func_t callback;
	float start[3];
} routereq_t;

#define MAX_ROUTE_REQS 64
static routereq_t reqs[MAX_ROUTE_REQS];
static int numreqs;

static void way_free(void)
{
	Mem_Free(way.nodes);
	Mem_Free(way.links);
	way.nodes = NULL;
	way.links = NULL;
	way.numnodes = way.numlinks = 0;
}

/* reads the next whitespace-separated number; false at the end of the text */
static bool next_number(const char **p, float *out)
{
	char *end;
	while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')
		(*p)++;
	if (!**p)
		return false;
	*out = strtof(*p, &end);
	if (end == *p)
		return false;
	*p = end;
	return true;
}

/* The progs may rewrite the file while the map runs (Nuclide saves the
 * graph it builds), so it is read again for each new batch of requests. */
static void way_load(void)
{
	char path[128];
	size_t len;
	char *text;
	const char *p;
	float v;
	int count;

	way_free();
	Str_Copy(way.map, sv.mapname, sizeof(way.map));
	snprintf(path, sizeof(path), "data/%s.way", sv.mapname);
	text = FS_LoadFile(path, &len);
	if (!text)
		return;
	p = text;
	if (!next_number(&p, &v) || v < 1 || v > 65536) {
		Mem_Free(text);
		return;
	}
	count = (int)v;
	way.nodes = Mem_Alloc(sizeof(waynode_t) * count);
	for (int i = 0; i < count; i++) {
		waynode_t *n = &way.nodes[i];
		float nl;
		if (!next_number(&p, &n->org[0]) || !next_number(&p, &n->org[1]) || !next_number(&p, &n->org[2]) ||
		    !next_number(&p, &n->radius) || !next_number(&p, &nl) || nl < 0 || nl > 1024)
			break;
		n->firstlink = way.numlinks;
		n->numlinks = 0;
		for (int k = 0; k < (int)nl; k++) {
			float to, cost, flags;
			if (!next_number(&p, &to) || !next_number(&p, &cost) || !next_number(&p, &flags))
				break;
			if (to < 0 || to >= count)
				continue;
			way.links = Mem_Realloc(way.links, sizeof(waylink_t) * (way.numlinks + 1));
			way.links[way.numlinks].to = (int)to;
			way.links[way.numlinks].cost = cost > 0 ? cost : 1;
			way.links[way.numlinks].flags = (int)flags;
			way.numlinks++;
			n->numlinks++;
		}
		way.numnodes = i + 1;
	}
	Mem_Free(text);
	Con_DPrintf("%s: %d waypoints, %d links\n", path, way.numnodes, way.numlinks);
}

static float dist3(const float *a, const float *b)
{
	float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
	return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

/* the closest node that can be reached in a straight line, else the closest */
static int nearest_node(const float *pos, int passent)
{
	int best = -1, bestclear = -1;
	float bestd = FLT_MAX, bestcleard = FLT_MAX;
	for (int i = 0; i < way.numnodes; i++) {
		float d = dist3(pos, way.nodes[i].org);
		if (d < bestd) {
			bestd = d;
			best = i;
		}
		if (d < bestcleard && d < 1024) {
			svtrace_t tr;
			static const float zero[3] = {0, 0, 0};
			SV_Move(pos, zero, zero, way.nodes[i].org, MOVE_NOMONSTERS, passent, &tr);
			if (tr.fraction == 1.0f) {
				bestcleard = d;
				bestclear = i;
			}
		}
	}
	return bestclear >= 0 ? bestclear : best;
}

/* A* from start to goal over links not in deny; returns the node count of
 * the path written to out (goal first), 0 if there is none */
static int find_path(int start, int goal, int deny, int *out)
{
	int n = way.numnodes, count = 0;
	float *g = Mem_Alloc(sizeof(float) * n);
	float *f = Mem_Alloc(sizeof(float) * n);
	int *from = Mem_Alloc(sizeof(int) * n);
	uint8_t *state = Mem_Alloc(n); /* 0 unseen, 1 open, 2 closed */

	for (int i = 0; i < n; i++) {
		g[i] = FLT_MAX;
		from[i] = -1;
	}
	g[start] = 0;
	f[start] = dist3(way.nodes[start].org, way.nodes[goal].org);
	state[start] = 1;

	for (;;) {
		int cur = -1;
		/* the graphs are small enough for a linear scan of the open set */
		for (int i = 0; i < n; i++)
			if (state[i] == 1 && (cur < 0 || f[i] < f[cur]))
				cur = i;
		if (cur < 0)
			break;
		if (cur == goal) {
			for (int at = goal; at >= 0; at = from[at])
				out[count++] = at;
			break;
		}
		state[cur] = 2;
		for (int k = 0; k < way.nodes[cur].numlinks; k++) {
			const waylink_t *l = &way.links[way.nodes[cur].firstlink + k];
			float ng;
			if ((l->flags & deny) || state[l->to] == 2)
				continue;
			ng = g[cur] + l->cost;
			if (ng < g[l->to]) {
				g[l->to] = ng;
				f[l->to] = ng + dist3(way.nodes[l->to].org, way.nodes[goal].org);
				from[l->to] = cur;
				state[l->to] = 1;
			}
		}
	}
	Mem_Free(g);
	Mem_Free(f);
	Mem_Free(from);
	Mem_Free(state);
	return count;
}

static void run_request(qcvm_t *vm, const routereq_t *r)
{
	int numnodes, count = 0;
	int *path = NULL;
	uint32_t addr;

	if (way.numnodes > 0) {
		int start = nearest_node(r->start, r->ent);
		int goal = nearest_node(r->dest, r->ent);
		path = Mem_Alloc(sizeof(int) * way.numnodes);
		count = (start >= 0 && goal >= 0) ? find_path(start, goal, r->deny, path) : 0;
		Con_DPrintf("route for %d: node %d -> node %d, %d nodes\n", r->ent, start, goal, count);
	}

	/* element 0 is the destination, then the path from the goal node back.
	 * Without a path the list still holds the destination: Nuclide's bots
	 * index the list without checking it for NULL, and with only the
	 * destination they try to walk straight there and give up if they can't. */
	numnodes = count + 1;
	addr = QC_HeapAlloc(vm, (size_t)numnodes * 5 * 4);
	if (!addr) {
		numnodes = 0;
	} else {
		qcval_t *list = QC_Pointer(vm, addr, (size_t)numnodes * 5 * 4);
		for (int i = 0; i < numnodes; i++) {
			qcval_t *e = list + i * 5;
			const float *org = i == 0 ? r->dest : way.nodes[path[i - 1]].org;
			e[0].f = org[0];
			e[1].f = org[1];
			e[2].f = org[2];
			e[3].i = 0;
			e[4].f = i == 0 ? 32.0f : way.nodes[path[i - 1]].radius;
		}
	}
	Mem_Free(path);

	QC_SetGFloat(vm, G.time, (float)sv.time);
	QC_PINT(vm, 0) = r->ent;
	QC_Parm(vm, 1)->f = r->dest[0];
	QC_Parm(vm, 1)[1].f = r->dest[1];
	QC_Parm(vm, 1)[2].f = r->dest[2];
	QC_PINT(vm, 2) = numnodes;
	QC_PINT(vm, 3) = (int32_t)addr;
	vm->argc = 4;
	QC_Execute(vm, r->callback);
}

/* runs the requests made during the last frame */
void SV_RunRoutes(void)
{
	qcvm_t *vm = sv.vm;
	routereq_t batch[MAX_ROUTE_REQS];
	int count = numreqs;

	if (!count || !vm)
		return;
	memcpy(batch, reqs, sizeof(routereq_t) * count);
	numreqs = 0;
	way_load();
	for (int i = 0; i < count; i++) {
		if (batch[i].ent > 0 && !QC_ValidEdict(vm, batch[i].ent))
			continue;
		run_request(vm, &batch[i]);
	}
}

void SV_ClearRoutes(void)
{
	numreqs = 0;
	way_free();
}

/* void(entity ent, vector dest, int denylinkflags, void(entity, vector, int, nodeslist_t *) callback) */
void bi_route_calculate(qcvm_t *vm)
{
	routereq_t *r;
	const float *org;
	if (numreqs >= MAX_ROUTE_REQS) {
		Con_DPrintf("route_calculate: too many requests this frame\n");
		return;
	}
	r = &reqs[numreqs++];
	r->ent = QC_PENT(vm, 0);
	VectorCopy(QC_PVEC(vm, 1), r->dest);
	r->deny = QC_PINT(vm, 2);
	r->callback = QC_PINT(vm, 3);
	org = &QC_EdictFields(vm, r->ent)[F.origin].f;
	VectorCopy(org, r->start);
}
