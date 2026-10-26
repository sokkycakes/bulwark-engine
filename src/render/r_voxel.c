/* Bulwark engine - drawing the voxel world. Each chunk is meshed into its
 * own vertex buffer when it changes: block faces hidden by an opaque full
 * neighbour are skipped, faces are grouped by texture, and light is a fixed
 * per-face shade times the block's colour (no lightmaps; see
 * docs/spec/voxel.md). ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/voxel.h"
#include "../server/sv_local.h"

#include <math.h>
#include <stdlib.h>

typedef struct {
	image_t *image;
	int firstindex, numindexes;
	bool translucent;
} vxbatch_t;

typedef struct {
	GLuint vao, vbo, ebo;
	vxbatch_t *batches;
	int numbatches;
	bool built;
	float mins[3], maxs[3];
} vxmesh_t;

static struct {
	const vxworld_t *world; /* the world the meshes belong to */
	uint32_t revision;
	vxmesh_t *meshes;
	int nummeshes;
	image_t *images[VX_MAX_TYPES];
	bool imagesdone[VX_MAX_TYPES];
} rv;

cvar_t *r_drawvoxels;

static void free_mesh(vxmesh_t *m)
{
	if (m->vao)
		qglDeleteVertexArrays(1, &m->vao);
	if (m->vbo)
		qglDeleteBuffers(1, &m->vbo);
	if (m->ebo)
		qglDeleteBuffers(1, &m->ebo);
	Mem_Free(m->batches);
	memset(m, 0, sizeof(*m));
}

void R_FreeVoxels(void)
{
	for (int i = 0; i < rv.nummeshes; i++)
		free_mesh(&rv.meshes[i]);
	Mem_Free(rv.meshes);
	memset(&rv, 0, sizeof(rv));
}

/* --- geometry per shape --------------------------------------------------- */

/* polygons of a shape in a unit cell for rotation 0, with the side of the
 * cell each lies on (0..5 = +x -x +y -y +z -z, 6 = inside) for culling */
typedef struct {
	int n;
	float v[4][3];
	int side;
} vxpoly_t;

static int box_polys(float x0, float x1, float z0, float z1, vxpoly_t *out)
{
	vxpoly_t p[6] = {
		{4, {{x1, 0, z0}, {x1, 1, z0}, {x1, 1, z1}, {x1, 0, z1}}, 0},
		{4, {{x0, 1, z0}, {x0, 0, z0}, {x0, 0, z1}, {x0, 1, z1}}, 1},
		{4, {{x1, 1, z0}, {x0, 1, z0}, {x0, 1, z1}, {x1, 1, z1}}, 2},
		{4, {{x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}}, 3},
		{4, {{x0, 0, z1}, {x1, 0, z1}, {x1, 1, z1}, {x0, 1, z1}}, 4},
		{4, {{x0, 1, z0}, {x1, 1, z0}, {x1, 0, z0}, {x0, 0, z0}}, 5},
	};
	/* faces that do not lie on the cell boundary cannot be culled */
	if (x0 > 0)
		p[1].side = 6;
	if (x1 < 1)
		p[0].side = 6;
	if (z1 < 1)
		p[4].side = 6;
	if (z0 > 0)
		p[5].side = 6;
	memcpy(out, p, sizeof(p));
	return 6;
}

static int shape_polys(int shape, vxpoly_t *out)
{
	switch (shape) {
	case VXS_BOX:
	case VXS_FIELD:
		return box_polys(0, 1, 0, 1, out);
	case VXS_SLAB_LOW:
		return box_polys(0, 1, 0, 0.5f, out);
	case VXS_SLAB_HIGH:
		return box_polys(0, 1, 0.5f, 1, out);
	case VXS_THIN:
		return box_polys(0.875f, 1, 0, 1, out);
	case VXS_SLOPE: {
		vxpoly_t p[5] = {
			{4, {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}, 0},
			{4, {{0, 1, 0}, {1, 1, 0}, {0, 0, 0}, {0, 0, 0}}, 6}, /* replaced below */
			{3, {{1, 1, 0}, {0, 1, 0}, {1, 1, 1}}, 2},
			{3, {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}}, 3},
			{4, {{0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {0, 0, 0}}, 5},
		};
		vxpoly_t slope = {4, {{0, 0, 0}, {1, 0, 1}, {1, 1, 1}, {0, 1, 0}}, 6};
		p[1] = slope;
		memcpy(out, p, sizeof(p));
		return 5;
	}
	case VXS_RSLOPE: {
		vxpoly_t p[5] = {
			{4, {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}, 0},
			{4, {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}, 4},
			{3, {{1, 1, 0}, {1, 1, 1}, {0, 1, 1}}, 2},
			{3, {{1, 0, 1}, {1, 0, 0}, {0, 0, 1}}, 3},
			{4, {{0, 1, 1}, {1, 1, 0}, {1, 0, 0}, {0, 0, 1}}, 6},
		};
		memcpy(out, p, sizeof(p));
		return 5;
	}
	default:
		return 0;
	}
}

/* turns the side index with the cell: +x -> +y -> -x -> -y */
static int rotate_side(int side, int rot)
{
	static const int next[6] = {2, 3, 1, 0, 4, 5};
	for (int k = 0; k < (rot & 3); k++)
		if (side < 4)
			side = next[side];
	return side;
}

static void rotate_point(float p[3], int rot)
{
	for (int k = 0; k < (rot & 3); k++) {
		float x = p[0], y = p[1];
		p[0] = 1 - y;
		p[1] = x;
	}
}

/* --- meshing ------------------------------------------------------------------ */

typedef struct {
	rvert_t *v;
	int nv, cv;
	uint32_t *idx;
	int ni, ci;
} vxbuild_t;

static bool hides(const vxworld_t *w, int x, int y, int z)
{
	int t = Vox_Get(w, x, y, z, NULL);
	const vxdef_t *d;
	if (!t)
		return false;
	d = &w->defs[t];
	return d->shape == VXS_BOX && d->color[3] == 255 && !(d->flags & VXF_NODRAW);
}

static image_t *type_image(const vxworld_t *w, int t)
{
	if (!rv.imagesdone[t]) {
		const char *tex = w->defs[t].texture;
		rv.images[t] = r_whiteimage;
		if (tex[0] && R_ImageExists(tex))
			rv.images[t] = R_FindImage(tex, 0);
		rv.imagesdone[t] = true;
	}
	return rv.images[t];
}

/* a point inside each shape (rotation 0), to turn every face outwards */
static void shape_interior(int shape, float out[3])
{
	out[0] = out[1] = out[2] = 0.5f;
	switch (shape) {
	case VXS_SLAB_LOW: out[2] = 0.25f; break;
	case VXS_SLAB_HIGH: out[2] = 0.75f; break;
	case VXS_THIN: out[0] = 0.9375f; break;
	case VXS_SLOPE: out[0] = 0.75f, out[2] = 0.25f; break;
	case VXS_RSLOPE: out[0] = 0.75f, out[2] = 0.75f; break;
	default: break;
	}
}

static void emit_poly(vxbuild_t *b, const vxworld_t *w, const vxpoly_t *p, int rot, const float org[3],
                      const vxdef_t *d)
{
	float pts[4][3], n[3], e1[3], e2[3], len, shade, inside[3], cen[3] = {0, 0, 0};
	int base = b->nv;
	shape_interior(d->shape, inside);
	rotate_point(inside, rot);
	for (int i = 0; i < p->n; i++) {
		memcpy(pts[i], p->v[i], sizeof(pts[i]));
		rotate_point(pts[i], rot);
		for (int k = 0; k < 3; k++)
			cen[k] += pts[i][k] / p->n;
	}
	for (int i = 0; i < 3; i++) {
		e1[i] = pts[1][i] - pts[0][i];
		e2[i] = pts[2][i] - pts[0][i];
	}
	n[0] = e1[1] * e2[2] - e1[2] * e2[1];
	n[1] = e1[2] * e2[0] - e1[0] * e2[2];
	n[2] = e1[0] * e2[1] - e1[1] * e2[0];
	len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (len <= 0)
		return;
	n[0] /= len, n[1] /= len, n[2] /= len;
	/* counter-clockwise seen from outside, the GL front face */
	if (n[0] * (cen[0] - inside[0]) + n[1] * (cen[1] - inside[1]) + n[2] * (cen[2] - inside[2]) < 0) {
		float tmp[3];
		for (int i = 0; i < p->n / 2; i++) {
			memcpy(tmp, pts[i], sizeof(tmp));
			memcpy(pts[i], pts[p->n - 1 - i], sizeof(tmp));
			memcpy(pts[p->n - 1 - i], tmp, sizeof(tmp));
		}
		n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
	}
	/* a fixed sun-like shade: tops bright, bottoms dark, sides in between */
	shade = (d->flags & VXF_FULLBRIGHT) ? 1.0f : 0.72f + 0.28f * n[2] + 0.08f * n[0] - 0.04f * n[1];
	if (shade > 1)
		shade = 1;
	if (b->nv + 4 > b->cv) {
		b->cv = b->cv ? b->cv * 2 : 1024;
		b->v = Mem_Realloc(b->v, sizeof(rvert_t) * b->cv);
	}
	if (b->ni + 6 > b->ci) {
		b->ci = b->ci ? b->ci * 2 : 1536;
		b->idx = Mem_Realloc(b->idx, sizeof(uint32_t) * b->ci);
	}
	for (int i = 0; i < p->n; i++) {
		rvert_t *v = &b->v[b->nv++];
		float ax, ay;
		memset(v, 0, sizeof(*v));
		for (int k = 0; k < 3; k++)
			v->xyz[k] = org[k] + pts[i][k] * w->blocksize;
		/* one texture repeat per block, projected on the face's main plane */
		if (fabsf(n[2]) > fabsf(n[0]) && fabsf(n[2]) > fabsf(n[1]))
			ax = pts[i][0], ay = pts[i][1];
		else if (fabsf(n[0]) > fabsf(n[1]))
			ax = pts[i][1], ay = pts[i][2];
		else
			ax = pts[i][0], ay = pts[i][2];
		v->st[0] = ax;
		v->st[1] = 1 - ay;
		memcpy(v->normal, n, sizeof(n));
		for (int k = 0; k < 3; k++)
			v->color[k] = (uint8_t)(d->color[k] * shade);
		v->color[3] = d->color[3];
	}
	b->idx[b->ni++] = base;
	b->idx[b->ni++] = base + 1;
	b->idx[b->ni++] = base + 2;
	if (p->n == 4) {
		b->idx[b->ni++] = base;
		b->idx[b->ni++] = base + 2;
		b->idx[b->ni++] = base + 3;
	}
}

static int batch_cmp(const void *a, const void *b)
{
	const int *x = a, *y = b;
	return x[0] - y[0];
}

static void build_mesh(const vxworld_t *w, int cx, int cy, int cz, vxmesh_t *m)
{
	static const int dir[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	vxbuild_t b = {0};
	int (*cells)[2] = NULL; /* type, cell index; sorted so batches are contiguous */
	int ncells = 0;
	const vxchunk_t *c = w->chunks[(cz * w->nchunks[1] + cy) * w->nchunks[0] + cx];
	free_mesh(m);
	m->built = true;
	if (!c || !R_Active())
		return;
	cells = Mem_Alloc(sizeof(*cells) * c->count);
	for (int i = 0; i < VX_CHUNKCELLS; i++)
		if (c->type[i]) {
			const vxdef_t *d = &w->defs[c->type[i]];
			/* translucent blocks after opaque ones, then by image */
			int key = (d->color[3] < 255 ? 1 << 24 : 0) | (int)(type_image(w, c->type[i])->tex & 0xFFFFFF);
			cells[ncells][0] = key;
			cells[ncells][1] = i;
			ncells++;
		}
	qsort(cells, ncells, sizeof(*cells), batch_cmp);
	for (int k = 0; k < ncells; k++) {
		int i = cells[k][1];
		int x = cx * VX_CHUNK + (i % VX_CHUNK), y = cy * VX_CHUNK + (i / VX_CHUNK) % VX_CHUNK,
		    z = cz * VX_CHUNK + i / (VX_CHUNK * VX_CHUNK);
		int t = c->type[i], rot = c->rot[i], np;
		const vxdef_t *d = &w->defs[t];
		vxpoly_t polys[6];
		float org[3];
		image_t *img = type_image(w, t);
		bool trans = d->color[3] < 255;
		if (d->flags & VXF_NODRAW)
			continue;
		if (!m->numbatches || m->batches[m->numbatches - 1].image != img ||
		    m->batches[m->numbatches - 1].translucent != trans) {
			m->batches = Mem_Realloc(m->batches, sizeof(vxbatch_t) * (m->numbatches + 1));
			m->batches[m->numbatches].image = img;
			m->batches[m->numbatches].translucent = trans;
			m->batches[m->numbatches].firstindex = b.ni;
			m->batches[m->numbatches].numindexes = 0;
			m->numbatches++;
		}
		Vox_CellMins(w, x, y, z, org);
		np = shape_polys(d->shape, polys);
		for (int p = 0; p < np; p++) {
			int side = polys[p].side < 6 ? rotate_side(polys[p].side, rot) : 6;
			if (side < 6 && hides(w, x + dir[side][0], y + dir[side][1], z + dir[side][2]))
				continue;
			/* water and glass do not draw faces against the same block */
			if (side < 6 && trans && Vox_Get(w, x + dir[side][0], y + dir[side][1], z + dir[side][2], NULL) == t)
				continue;
			emit_poly(&b, w, &polys[p], rot, org, d);
		}
		m->batches[m->numbatches - 1].numindexes = b.ni - m->batches[m->numbatches - 1].firstindex;
	}
	Mem_Free(cells);
	Vox_CellMins(w, cx * VX_CHUNK, cy * VX_CHUNK, cz * VX_CHUNK, m->mins);
	for (int i = 0; i < 3; i++)
		m->maxs[i] = m->mins[i] + VX_CHUNK * w->blocksize;
	if (b.ni) {
		qglGenVertexArrays(1, &m->vao);
		qglGenBuffers(1, &m->vbo);
		qglGenBuffers(1, &m->ebo);
		qglBindVertexArray(m->vao);
		qglBindBuffer(GL_ARRAY_BUFFER, m->vbo);
		qglBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(rvert_t) * b.nv), b.v, GL_STATIC_DRAW);
		qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ebo);
		qglBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(uint32_t) * b.ni), b.idx, GL_STATIC_DRAW);
		R_SetupVertexAttribs();
		qglBindVertexArray(0);
	}
	r_stats.worldsurfs += b.ni / 3;
	Mem_Free(b.v);
	Mem_Free(b.idx);
}

static void update_meshes(const vxworld_t *w)
{
	int n = w->nchunks[0] * w->nchunks[1] * w->nchunks[2];
	if (rv.world != w || rv.nummeshes != n) {
		R_FreeVoxels();
		rv.world = w;
		rv.nummeshes = n;
		rv.meshes = Mem_Alloc(sizeof(vxmesh_t) * n);
	}
	if (!((vxworld_t *)w)->anydirty)
		return;
	/* a type definition may have changed its texture */
	for (int i = 0; i < VX_MAX_TYPES; i++)
		rv.imagesdone[i] = false;
	for (int i = 0; i < n; i++) {
		vxchunk_t *c = w->chunks[i];
		vxmesh_t *m = &rv.meshes[i];
		int cx = i % w->nchunks[0], cy = (i / w->nchunks[0]) % w->nchunks[1], cz = i / (w->nchunks[0] * w->nchunks[1]);
		if (c && (c->dirty || !m->built)) {
			build_mesh(w, cx, cy, cz, m);
			c->dirty = false;
		} else if (!c && m->built) {
			free_mesh(m);
		}
	}
	((vxworld_t *)w)->anydirty = false;
}

void R_DrawVoxels(bool translucentpass)
{
	const vxworld_t *w = vx_clientworld;
	const glprog_t *p = &r_progs[PROG_WORLD];
	float planes[16], model[16];
	if (!w || !r_drawvoxels || !r_drawvoxels->ivalue) {
		if (!w && rv.meshes)
			R_FreeVoxels();
		return;
	}
	if (!translucentpass)
		update_meshes(w);
	for (int i = 0; i < 4; i++) {
		int row = i / 2;
		float sign = (i & 1) ? -1.0f : 1.0f;
		for (int k = 0; k < 4; k++)
			planes[i * 4 + k] = r_scene.viewproj[k * 4 + 3] + sign * r_scene.viewproj[k * 4 + row];
	}
	Mat4_Identity(model);
	R_UseProgram(PROG_WORLD);
	qglUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, r_scene.viewproj);
	qglUniformMatrix4fv(p->u_model, 1, GL_FALSE, model);
	qglUniform3f(p->u_vieworg, r_scene.origin[0], r_scene.origin[1], r_scene.origin[2]);
	qglUniform4f(p->u_fog, r_world.fog[0], r_world.fog[1], r_world.fog[2], r_world.hasfog ? r_world.fog[3] : 0);
	qglUniform1f(p->u_usefullbright, 0);
	qglUniform1f(p->u_lmscale, -1); /* light from the vertex colour */
	qglUniform4f(p->u_tcmod, 0, 0, 1, 1);
	qglUniform1i(p->u_numdlights, 0);
	GL_BindTexture(1, GL_TEXTURE_2D, r_whiteimage->tex);
	GL_SetCull(true);
	for (int i = 0; i < rv.nummeshes; i++) {
		vxmesh_t *m = &rv.meshes[i];
		bool culled = false;
		if (!m->vao)
			continue;
		for (int k = 0; k < 4 && !culled; k++) {
			const float *pl = planes + k * 4;
			float x = pl[0] > 0 ? m->maxs[0] : m->mins[0];
			float y = pl[1] > 0 ? m->maxs[1] : m->mins[1];
			float z = pl[2] > 0 ? m->maxs[2] : m->mins[2];
			if (pl[0] * x + pl[1] * y + pl[2] * z + pl[3] < 0)
				culled = true;
		}
		if (culled) {
			r_stats.culled++;
			continue;
		}
		qglBindVertexArray(m->vao);
		for (int b = 0; b < m->numbatches; b++) {
			const vxbatch_t *bt = &m->batches[b];
			if (bt->translucent != translucentpass || !bt->numindexes)
				continue;
			GL_BindTexture(0, GL_TEXTURE_2D, bt->image->tex);
			qglUniform4f(p->u_color, 1, 1, 1, 1);
			qglUniform1f(p->u_alphatest, bt->image->alpha && !bt->translucent ? 0.5f : 0.0f);
			if (bt->translucent) {
				GL_SetBlend(BLEND_ALPHA, 0, 0);
				GL_SetDepth(true, false);
			} else {
				GL_SetBlend(BLEND_OPAQUE, 0, 0);
				GL_SetDepth(true, true);
			}
			qglDrawElements(GL_TRIANGLES, bt->numindexes, GL_UNSIGNED_INT,
			                (const void *)(uintptr_t)(sizeof(uint32_t) * bt->firstindex));
		}
	}
	qglBindVertexArray(0);
	GL_SetDepth(true, true);
	GL_SetBlend(BLEND_OPAQUE, 0, 0);
}
