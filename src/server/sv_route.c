/* Bulwark engine - route_calculate: paths over the map's waypoint graph,
 * data/<map>.way, in the text layout Nuclide's node editor reads and writes
 * (src/nav/NodeEditor.qc): a node count, then per node "x y z radius
 * links" and one " node cost flags" line per link. The builtin follows the
 * fteextensions.qc description: the callback gets the route a frame later
 * as a memalloc'd nodeslist_t array to be followed from the last entry
 * back to the first (the destination). ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/fs.h"
#include "../common/cmd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
	int node, flags;
	float cost;
} waylink_t;

typedef struct {
	float origin[3], radius;
	waylink_t *links;
	int numlinks;
} waynode_t;

/* one finished route waiting for its callback */
typedef struct {
	int ent;
	float dest[3];
	func_t callback;
	uint32_t list;
	int count;
} pending_t;

static struct {
	char map[64];
	bool loaded;
	waynode_t *nodes;
	int numnodes;
	pending_t *pending;
	int numpending;
} way;

void SV_FreeRoutes(void)
{
	for (int i = 0; i < way.numnodes; i++)
		Mem_Free(way.nodes[i].links);
	Mem_Free(way.nodes);
	Mem_Free(way.pending);
	memset(&way, 0, sizeof(way));
}

static float dist(const float *a, const float *b)
{
	float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
	return VectorLength(d);
}

static void load_graph(void)
{
	char path[128], *text, *p, *line;
	size_t len;
	int n = 0;
	if (way.loaded && !strcmp(way.map, sv.mapname))
		return;
	SV_FreeRoutes();
	Str_Copy(way.map, sv.mapname, sizeof(way.map));
	way.loaded = true;
	snprintf(path, sizeof(path), "data/%s.way", sv.mapname);
	text = FS_LoadFile(path, &len);
	if (!text)
		return;
	p = text;
#define NEXT_LINE() (line = p, p = strchr(p, '\n'), p ? (*p++ = 0, line) : (p = line + strlen(line), line))
	NEXT_LINE();
	n = atoi(line);
	if (n <= 0 || n > 65536) {
		Mem_Free(text);
		return;
	}
	way.nodes = Mem_Alloc(sizeof(waynode_t) * n);
	for (int i = 0; i < n && *p; i++) {
		waynode_t *w = &way.nodes[i];
		NEXT_LINE();
		if (sscanf(line, "%f %f %f %f %d", &w->origin[0], &w->origin[1], &w->origin[2], &w->radius, &w->numlinks) < 5 ||
		    w->numlinks < 0 || w->numlinks > 1024)
			w->numlinks = 0;
		w->links = Mem_Alloc(sizeof(waylink_t) * (w->numlinks + 1));
		for (int j = 0; j < w->numlinks; j++) {
			NEXT_LINE();
			sscanf(line, "%d %f %d", &w->links[j].node, &w->links[j].cost, &w->links[j].flags);
			if (w->links[j].node < 0 || w->links[j].node >= n)
				w->links[j].node = i;
		}
		way.numnodes = i + 1;
	}
#undef NEXT_LINE
	Mem_Free(text);
	Con_DPrintf("%s: %d waypoints\n", path, way.numnodes);
}

/* the nearest node the point can see, else the nearest */
static int nearest_node(const float *p)
{
	static const float zero[3];
	int best = -1, bestseen = -1;
	float bestd = 1e30f, bestseend = 1e30f;
	const bspmap_t *world = SV_CurrentWorld();
	for (int i = 0; i < way.numnodes; i++) {
		float d = dist(way.nodes[i].origin, p);
		if (d < bestd)
			bestd = d, best = i;
		if (d < bestseend && world) {
			bsptrace_t tr;
			BSP_TraceModel(world, 0, zero, p, zero, zero, way.nodes[i].origin, CONTENTS_SOLID, &tr);
			if (tr.fraction == 1 && !tr.startsolid)
				bestseend = d, bestseen = i;
		}
	}
	return bestseen >= 0 ? bestseen : best;
}

/* A* over the links, skipping any whose flags meet deny; fills the path
 * from goal back to start and returns its length */
static int find_path(int start, int goal, int deny, int *out)
{
	int n = way.numnodes, count = 0, cap = n, *from = Mem_Alloc(sizeof(int) * n), *open;
	for (int i = 0; i < n; i++)
		cap += way.nodes[i].numlinks;
	open = Mem_Alloc(sizeof(int) * cap); /* a node is queued again whenever its cost drops */
	float *g = Mem_Alloc(sizeof(float) * n), *f = Mem_Alloc(sizeof(float) * n);
	bool *closed = Mem_Alloc(sizeof(bool) * n);
	int numopen = 0;
	for (int i = 0; i < n; i++)
		g[i] = 1e30f, from[i] = -1;
	g[start] = 0;
	f[start] = dist(way.nodes[start].origin, way.nodes[goal].origin);
	open[numopen++] = start;
	while (numopen) {
		int bi = 0, cur;
		for (int i = 1; i < numopen; i++)
			if (f[open[i]] < f[open[bi]])
				bi = i;
		cur = open[bi];
		open[bi] = open[--numopen];
		if (cur == goal)
			break;
		if (closed[cur])
			continue;
		closed[cur] = true;
		for (int l = 0; l < way.nodes[cur].numlinks; l++) {
			const waylink_t *lk = &way.nodes[cur].links[l];
			int nb = lk->node;
			float cost;
			if ((lk->flags & deny) || closed[nb])
				continue;
			cost = lk->cost > 0 ? lk->cost : dist(way.nodes[cur].origin, way.nodes[nb].origin);
			if (g[cur] + cost < g[nb] && numopen < cap) {
				g[nb] = g[cur] + cost;
				f[nb] = g[nb] + dist(way.nodes[nb].origin, way.nodes[goal].origin);
				from[nb] = cur;
				open[numopen++] = nb;
			}
		}
	}
	if (g[goal] < 1e30f)
		for (int i = goal; i >= 0 && count < n; i = i == start ? -1 : from[i])
			out[count++] = i;
	Mem_Free(from);
	Mem_Free(open);
	Mem_Free(g);
	Mem_Free(f);
	Mem_Free(closed);
	return count;
}

/* route_calculate(entity ent, vector dest, int denylinkflags, callback) */
static void bi_route_calculate(qcvm_t *vm)
{
	int ent = QC_PENT(vm, 0), deny = QC_PINT(vm, 2), start, goal, count = 0, *path = NULL;
	const float *dest = QC_PVEC(vm, 1);
	pending_t *pd;
	qcval_t *org;
	load_graph();
	way.pending = Mem_Realloc(way.pending, sizeof(pending_t) * (way.numpending + 1));
	pd = &way.pending[way.numpending++];
	memset(pd, 0, sizeof(*pd));
	pd->ent = ent;
	VectorCopy(dest, pd->dest);
	pd->callback = QC_PINT(vm, 3);
	org = QC_EField(vm, ent, vm->fofs.origin);
	if (way.numnodes && org) {
		start = nearest_node(&org->f);
		goal = nearest_node(dest);
		path = Mem_Alloc(sizeof(int) * way.numnodes);
		if (start >= 0 && goal >= 0)
			count = find_path(start, goal, deny, path);
	}
	/* entry 0 is the destination itself, the last the first node to reach */
	if (count) {
		uint8_t *mem;
		pd->count = count + 1;
		pd->list = QC_HeapAlloc(vm, (size_t)pd->count * 20);
		mem = QC_Pointer(vm, pd->list, (size_t)pd->count * 20);
		for (int i = 0; i < pd->count; i++) {
			float o[3], r = 0;
			int32_t flags = 0;
			if (i == 0) {
				VectorCopy(dest, o);
			} else {
				const waynode_t *w = &way.nodes[path[i - 1]];
				VectorCopy(w->origin, o);
				r = w->radius;
				/* the flags of the link taken from this node toward the goal */
				if (i > 1)
					for (int l = 0; l < w->numlinks; l++)
						if (w->links[l].node == path[i - 2])
							flags = w->links[l].flags;
			}
			memcpy(mem + i * 20, o, 12);
			memcpy(mem + i * 20 + 12, &flags, 4);
			memcpy(mem + i * 20 + 16, &r, 4);
		}
	}
	Mem_Free(path);
}

/* hands finished routes to their callbacks, once per server frame */
void SV_RunRoutes(void)
{
	qcvm_t *vm = sv.vm;
	int n = way.numpending;
	pending_t *list;
	if (!n || !vm)
		return;
	list = way.pending;
	way.pending = NULL;
	way.numpending = 0;
	for (int i = 0; i < n; i++) {
		pending_t *pd = &list[i];
		if (pd->ent < 0 || pd->ent >= vm->num_edicts || vm->edicts[pd->ent].free || !pd->callback) {
			if (pd->list)
				QC_HeapFree(vm, pd->list);
			continue;
		}
		QC_Parm(vm, 0)->i = pd->ent;
		VectorCopy(pd->dest, &QC_Parm(vm, 1)->f);
		QC_Parm(vm, 2)->i = pd->count;
		QC_Parm(vm, 3)->i = (int32_t)pd->list;
		vm->argc = 4;
		QC_Execute(vm, pd->callback);
	}
	Mem_Free(list);
}

const qc_builtin_t sv_route_builtins[] = {{"route_calculate", bi_route_calculate}, {NULL, NULL}};

/* the graph, for route_visualise on a listen server */
int SV_RouteNumNodes(void)
{
	if (sv.active)
		load_graph();
	return way.numnodes;
}

bool SV_RouteNode(int i, float *origin, float *radius, int *numlinks)
{
	if (i < 0 || i >= way.numnodes)
		return false;
	VectorCopy(way.nodes[i].origin, origin);
	*radius = way.nodes[i].radius;
	*numlinks = way.nodes[i].numlinks;
	return true;
}

int SV_RouteLink(int i, int k) { return i >= 0 && i < way.numnodes && k >= 0 && k < way.nodes[i].numlinks ? way.nodes[i].links[k].node : -1; }

/* route_reload: read data/<map>.way again */
static void RouteReload_f(int argc, const char **argv)
{
	SV_FreeRoutes();
	if (sv.active) {
		load_graph();
		Con_Printf("%d waypoints in data/%s.way\n", way.numnodes, sv.mapname);
	}
}

void SV_InitRoutes(void) { Cmd_AddD("route_reload", RouteReload_f, "Read the map's waypoint graph (data/<map>.way) again"); }
