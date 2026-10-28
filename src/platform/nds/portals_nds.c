/* Bulwark engine - portals between a map's leaves, for the DS build. ISC
 * licence, see LICENSE.
 *
 * A Quake map's PVS says which leaves can see each other from anywhere in a
 * leaf, so from any one spot it draws far more than is in view. Portals are
 * the openings between neighbouring empty leaves. Following them out from
 * the eye's leaf, and narrowing the view to each opening as it goes, finds
 * the leaves that can be seen from where the eye actually is, through what
 * it is actually looking at.
 *
 * Quake maps don't keep their portals (the compiler writes them to a
 * separate .prt file that the vis tool reads), so they are worked out again
 * at load from the BSP tree: every node's plane, cut down to the part of
 * space the node covers, is split through the leaves on its front and back;
 * each piece with an empty leaf on both sides is a portal.
 *
 * Written from the BSP29 layout and the general method; no Quake tool code. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "portals_nds.h"

#define MAX_WPOINTS 24
#define MAX_PENDING 128 /* pieces waiting to be split further: kept off the DS's small stack */
#define ON_EPSILON 0.05f

typedef struct {
	int num;
	float p[MAX_WPOINTS][3];
} winding_t;

static const pt_plane_t *planes;
static const pt_node_t *nodes;
static const int *contents;
static int numnodes, numleafs;
static pt_portals_t *out;
static int curplane; /* the node plane being split, for emit */
static int cap_portals, cap_points;

/* a square on the plane, larger than any map */
static void base_winding(const pt_plane_t *pl, winding_t *w)
{
	const float *n = pl->n;
	float up[3] = {0, 0, 0}, right[3], org[3], d;
	int axis = 0;
	for (int c = 1; c < 3; c++)
		if (fabsf(n[c]) > fabsf(n[axis]))
			axis = c;
	/* up: the world axis least like the normal, made square to it */
	if (axis == 2)
		up[0] = 1;
	else
		up[2] = 1;
	d = up[0] * n[0] + up[1] * n[1] + up[2] * n[2];
	for (int c = 0; c < 3; c++)
		up[c] -= d * n[c];
	d = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
	for (int c = 0; c < 3; c++)
		up[c] *= 65536 / d, org[c] = n[c] * pl->dist;
	right[0] = up[1] * n[2] - up[2] * n[1];
	right[1] = up[2] * n[0] - up[0] * n[2];
	right[2] = up[0] * n[1] - up[1] * n[0];
	w->num = 4;
	for (int c = 0; c < 3; c++) {
		w->p[0][c] = org[c] - right[c] + up[c];
		w->p[1][c] = org[c] + right[c] + up[c];
		w->p[2][c] = org[c] + right[c] - up[c];
		w->p[3][c] = org[c] - right[c] - up[c];
	}
}

/* keeps the part on the front of (n, dist), or the back if back; false if nothing is left */
static bool clip(winding_t *w, const float *n, float dist, bool back)
{
	float dists[MAX_WPOINTS + 1];
	int sides[MAX_WPOINTS + 1], front = 0, behind = 0;
	winding_t o;
	for (int i = 0; i < w->num; i++) {
		float d = w->p[i][0] * n[0] + w->p[i][1] * n[1] + w->p[i][2] * n[2] - dist;
		if (back)
			d = -d;
		dists[i] = d;
		sides[i] = d > ON_EPSILON ? 1 : d < -ON_EPSILON ? -1 : 0;
		front += sides[i] > 0, behind += sides[i] < 0;
	}
	if (!behind)
		return true;
	if (!front)
		return false;
	dists[w->num] = dists[0], sides[w->num] = sides[0];
	o.num = 0;
	for (int i = 0; i < w->num && o.num < MAX_WPOINTS - 1; i++) {
		const float *p = w->p[i], *q = w->p[(i + 1) % w->num];
		if (sides[i] >= 0)
			memcpy(o.p[o.num++], p, sizeof(o.p[0]));
		if (sides[i] && sides[i + 1] && sides[i] != sides[i + 1]) {
			float t = dists[i] / (dists[i] - dists[i + 1]);
			for (int c = 0; c < 3; c++)
				o.p[o.num][c] = p[c] + t * (q[c] - p[c]);
			o.num++;
		}
	}
	*w = o;
	return o.num >= 3;
}

static void emit(const winding_t *w, int front, int back)
{
	pt_portal_t *p;
	if (out->numportals >= cap_portals || out->numpoints + w->num > cap_points)
		return;
	p = &out->portals[out->numportals++];
	p->leaf[0] = (int16_t)front, p->leaf[1] = (int16_t)back;
	p->plane = curplane;
	p->first = out->numpoints, p->num = (uint8_t)w->num;
	for (int i = 0; i < w->num; i++)
		for (int c = 0; c < 3; c++) {
			float v = w->p[i][c] * 8;
			out->points[out->numpoints + i][c] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
		}
	out->numpoints += w->num;
}

typedef struct {
	winding_t w;
	int node, front, other;
} pending_t;

static pending_t *pending;
static int numpending;
static bool dropped;

static void push(const winding_t *w, int node, int front, int other)
{
	if (numpending < MAX_PENDING) {
		pending_t *q = &pending[numpending++];
		q->w = *w, q->node = node, q->front = front, q->other = other;
	} else {
		dropped = true;
	}
}

/* splits a node's winding through the subtree on its front, then each piece
 * through the subtree on its back; a piece with an empty leaf on both sides is a portal */
static void filter(const winding_t *start, int node, int other)
{
	numpending = 0;
	push(start, node, -1, other);
	while (numpending) {
		pending_t q = pending[--numpending];
		int n = q.node;
		while (n >= 0 && n < numnodes) {
			const pt_node_t *nd = &nodes[n];
			const pt_plane_t *pl = &planes[nd->plane];
			winding_t b = q.w;
			bool f = clip(&q.w, pl->n, pl->dist, false), k = clip(&b, pl->n, pl->dist, true);
			if (f && k)
				push(&b, nd->children[1], q.front, q.other);
			else if (k)
				q.w = b;
			/* (neither: lying in the plane, taken as its front, as the compiler does) */
			n = nd->children[f || !k ? 0 : 1];
		}
		if (n >= 0)
			continue;
		{
			int leaf = -(n + 1);
			if (leaf <= 0 || leaf >= numleafs || contents[leaf] == PT_SOLID)
				continue;
			if (q.front < 0)
				push(&q.w, q.other, leaf, -1);
			else if (leaf != q.front)
				emit(&q.w, q.front, leaf);
		}
	}
}

/* every node: its plane cut to its region, then split through both sides */
static void build(int n, int depth, const float *mins, const float *maxs)
{
	static int stack[256];
	static bool sback[256];
	static winding_t w; /* used up before going deeper */
	const pt_node_t *nd;
	if (n < 0 || n >= numnodes || depth >= 255)
		return;
	nd = &nodes[n];
	base_winding(&planes[nd->plane], &w);
	for (int c = 0; c < 3 && w.num; c++) {
		float axis[3] = {0, 0, 0};
		axis[c] = 1;
		if (!clip(&w, axis, mins[c], false) || !clip(&w, axis, maxs[c], true))
			w.num = 0;
	}
	for (int i = 0; i < depth && w.num; i++) {
		const pt_plane_t *pl = &planes[nodes[stack[i]].plane];
		if (!clip(&w, pl->n, pl->dist, sback[i]))
			w.num = 0;
	}
	curplane = nd->plane;
	if (w.num >= 3)
		filter(&w, nd->children[0], nd->children[1]);
	stack[depth] = n;
	for (int s = 0; s < 2; s++) {
		sback[depth] = s == 1;
		build(nd->children[s], depth + 1, mins, maxs);
	}
}

bool Pt_Build(pt_portals_t *p, const pt_plane_t *pl, const pt_node_t *nd, int nnodes, const int *leafcontents,
              int nleafs, const float *mins, const float *maxs)
{
	float lo[3], hi[3];
	memset(p, 0, sizeof(*p));
	planes = pl, nodes = nd, numnodes = nnodes, contents = leafcontents, numleafs = nleafs, out = p;
	cap_portals = nleafs * 4 + 64;
	cap_points = cap_portals * 6;
	p->portals = malloc(sizeof(pt_portal_t) * (size_t)cap_portals);
	p->points = malloc(sizeof(p->points[0]) * (size_t)cap_points);
	p->leaffirst = calloc((size_t)nleafs + 1, sizeof(int));
	pending = malloc(sizeof(pending_t) * MAX_PENDING);
	if (!p->portals || !p->points || !p->leaffirst || !pending) {
		free(pending);
		Pt_Free(p);
		return false;
	}
	for (int c = 0; c < 3; c++)
		lo[c] = mins[c] - 16, hi[c] = maxs[c] + 16;
	dropped = false;
	build(0, 0, lo, hi);
	free(pending);
	pending = NULL;
	/* each leaf's portals, as a list of portal numbers */
	p->leafportals = malloc(sizeof(int) * (size_t)(p->numportals * 2 + 1));
	if (!p->leafportals) {
		Pt_Free(p);
		return false;
	}
	for (int i = 0; i < p->numportals; i++)
		for (int s = 0; s < 2; s++)
			p->leaffirst[p->portals[i].leaf[s] + 1]++;
	for (int l = 0; l < nleafs; l++)
		p->leaffirst[l + 1] += p->leaffirst[l];
	{
		int *fill = calloc((size_t)nleafs, sizeof(int));
		if (!fill) {
			Pt_Free(p);
			return false;
		}
		for (int i = 0; i < p->numportals; i++)
			for (int s = 0; s < 2; s++) {
				int l = p->portals[i].leaf[s];
				p->leafportals[p->leaffirst[l] + fill[l]++] = i;
			}
		free(fill);
	}
	p->numleafs = nleafs;
	p->overflow = dropped || p->numportals >= cap_portals || p->numpoints >= cap_points - MAX_WPOINTS;
	return true;
}

void Pt_Free(pt_portals_t *p)
{
	free(p->portals), free(p->points), free(p->leaffirst), free(p->leafportals);
	memset(p, 0, sizeof(*p));
}
