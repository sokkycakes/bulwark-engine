/* Bulwark engine - the view and scene the client game builds each frame:
 * clearscene, view properties, addentity/addentities, polygons, dynamic
 * lights and renderscene. ISC licence, see LICENSE. */
#include "r_local.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

rscene_t r_scene;
rstats_t r_stats;
static cvar_t *fov, *r_nearclip;

/* entity fields addentity reads, looked up once per client game */
static struct {
	qcvm_t *vm;
	int modelindex, origin, angles, frame, frame2, lerpfrac, frame1time, frame2time, skin, scale, alpha;
	int colormod, glowmod, renderflags, effects, drawmask, predraw, forceshader, entnum, drawflags;
	int skeletonindex;
} EF;

static void lookup_fields(qcvm_t *vm)
{
	if (EF.vm == vm)
		return;
	EF.vm = vm;
#define FLD(x) EF.x = QC_FieldOfs(vm, #x)
	FLD(modelindex); FLD(origin); FLD(angles); FLD(frame); FLD(frame2); FLD(lerpfrac); FLD(frame1time);
	FLD(frame2time); FLD(skin); FLD(scale); FLD(alpha); FLD(colormod); FLD(glowmod); FLD(renderflags);
	FLD(effects); FLD(drawmask); FLD(predraw); FLD(forceshader); FLD(entnum); FLD(drawflags);
	FLD(skeletonindex);
#undef FLD
}

/* --- polygons ----------------------------------------------------------------- */

typedef struct {
	material_t *mat;
	int flags;
	int firstvert, numverts;
} rpoly_t;

#define MAX_POLY_VERTS 32768
#define MAX_POLYS 8192
static rvert_t polyverts[MAX_POLY_VERTS];
static int numpolyverts;
static rpoly_t polys[MAX_POLYS];
static int numpolys;
static material_t *poly_mat;
static int poly_flags;
static bool poly_2d;
static int poly_start;

void R_BeginPolygon(const char *shader, int flags, bool twod)
{
	poly_mat = R_FindMaterial(shader, twod ? MATF_2D : 0);
	poly_flags = flags;
	poly_2d = twod || (flags & 4); /* DRAWFLAG_2D */
	poly_start = numpolyverts;
}

void R_PolygonVertex(const float *org, const float *tc, const float *rgb, float alpha)
{
	rvert_t *v;
	if (numpolyverts >= MAX_POLY_VERTS)
		return;
	v = &polyverts[numpolyverts++];
	memcpy(v->xyz, org, sizeof(float) * 3);
	v->st[0] = tc[0];
	v->st[1] = tc[1];
	v->color[0] = (uint8_t)(BW_MAX(0.0f, BW_MIN(1.0f, rgb[0])) * 255);
	v->color[1] = (uint8_t)(BW_MAX(0.0f, BW_MIN(1.0f, rgb[1])) * 255);
	v->color[2] = (uint8_t)(BW_MAX(0.0f, BW_MIN(1.0f, rgb[2])) * 255);
	v->color[3] = (uint8_t)(BW_MAX(0.0f, BW_MIN(1.0f, alpha)) * 255);
}

static void draw_polys_2d(int first, int count, material_t *mat, int flags)
{
	/* fan into triangles through the 2D batcher's GL state */
	static uint16_t idx[MAX_POLY_VERTS * 3];
	int n = 0;
	float m[16];
	if (!R_Active() || count < 3 || !mat || !r_draw2d->ivalue)
		return;
	R_Flush2D();
	for (int i = 1; i + 1 < count && n + 3 <= (int)BW_ARRAY_COUNT(idx); i++) {
		idx[n++] = 0;
		idx[n++] = (uint16_t)i;
		idx[n++] = (uint16_t)(i + 1);
	}
	R_UseProgram(PROG_2D);
	Mat4_Ortho(m, 0, (float)r_vwidth, (float)r_vheight, 0, -1, 1);
	qglUniformMatrix4fv(r_progs[PROG_2D].u_mvp, 1, GL_FALSE, m);
	qglUniform4f(r_progs[PROG_2D].u_tcmod, 0, 0, 1, 1);
	qglUniform1f(r_progs[PROG_2D].u_alphatest, 0);
	GL_BindTexture(0, GL_TEXTURE_2D, mat->diffuse->tex);
	GL_SetBlend((flags & 3) == 1 ? BLEND_ADD : BLEND_ALPHA, 0, 0);
	if (flags & 2048) /* DRAWFLAG_LINES */
		R_StreamDraw(GL_LINE_LOOP, polyverts + first, count, NULL, 0);
	else
		R_StreamDraw(GL_TRIANGLES, polyverts + first, count, idx, n);
}

void R_EndPolygon(void)
{
	int count = numpolyverts - poly_start;
	if (count < 3) {
		numpolyverts = poly_start;
		return;
	}
	if (poly_2d) {
		draw_polys_2d(poly_start, count, poly_mat, poly_flags);
		numpolyverts = poly_start;
		return;
	}
	if (numpolys < MAX_POLYS) {
		rpoly_t *p = &polys[numpolys++];
		p->mat = poly_mat;
		p->flags = poly_flags;
		p->firstvert = poly_start;
		p->numverts = count;
	}
	poly_start = numpolyverts;
}

void R_DrawPolygons(bool twod)
{
	static uint16_t idx[MAX_POLY_VERTS * 3];
	(void)twod;
	if (!numpolys)
		return;
	R_UseProgram(PROG_UNLIT);
	qglUniformMatrix4fv(r_progs[PROG_UNLIT].u_mvp, 1, GL_FALSE, r_scene.viewproj);
	qglUniform4f(r_progs[PROG_UNLIT].u_color, 1, 1, 1, 1);
	qglUniform4f(r_progs[PROG_UNLIT].u_tcmod, 0, 0, 1, 1);
	qglUniform1f(r_progs[PROG_UNLIT].u_alphatest, 0);
	GL_SetDepth(true, false);
	for (int i = 0; i < numpolys; i++) {
		rpoly_t *p = &polys[i];
		int n = 0;
		if (!p->mat || p->mat->nodraw)
			continue;
		for (int k = 1; k + 1 < p->numverts; k++) {
			idx[n++] = 0;
			idx[n++] = (uint16_t)k;
			idx[n++] = (uint16_t)(k + 1);
		}
		GL_BindTexture(0, GL_TEXTURE_2D, p->mat->diffuse->tex);
		GL_SetBlend((p->flags & 3) == 1 ? BLEND_ADD : p->mat->blend == BLEND_OPAQUE ? BLEND_ALPHA : p->mat->blend,
		            p->mat->src, p->mat->dst);
		GL_SetCull(!(p->flags & 1024) && !p->mat->twosided);
		if (p->flags & 2048)
			R_StreamDraw(GL_LINE_LOOP, polyverts + p->firstvert, p->numverts, NULL, 0);
		else
			R_StreamDraw(GL_TRIANGLES, polyverts + p->firstvert, p->numverts, idx, n);
	}
	GL_SetCull(true);
}

/* --- scene ---------------------------------------------------------------- */

void R_ClearScene(void)
{
	r_scene.numents = 0;
	r_scene.numdlights = 0;
	R_ClearTempDecals();
	numpolys = 0;
	numpolyverts = 0;
	poly_start = 0;
	r_scene.vx = 0;
	r_scene.vy = 0;
	r_scene.vw = (float)r_vwidth;
	r_scene.vh = (float)r_vheight;
	r_scene.afov = fov ? fov->value : 90;
	r_scene.fov_x = r_scene.fov_y = 0;
	r_scene.nearclip = r_nearclip ? r_nearclip->value : 4;
	r_scene.farclip = 0;
	r_scene.drawworld = true;
	r_scene.perspective = true;
	r_scene.viewentity = cl.playerent;
	memset(r_scene.origin, 0, sizeof(r_scene.origin));
	memset(r_scene.angles, 0, sizeof(r_scene.angles));
}

rentity_t *R_AllocEntity(void)
{
	rentity_t *e;
	if (r_scene.numents >= R_MAX_ENTITIES)
		return NULL;
	e = &r_scene.ents[r_scene.numents++];
	memset(e, 0, sizeof(*e));
	e->scale = 1;
	e->alpha = 1;
	e->colormod[0] = e->colormod[1] = e->colormod[2] = 1;
	return e;
}

static float efloat(qcvm_t *vm, int e, int ofs, float def) { return ofs < 0 ? def : QC_EFloat(vm, e, ofs); }

void R_AddEntityFromQC(qcvm_t *vm, int ent)
{
	rentity_t *e;
	qcval_t *f;
	int mi;
	lookup_fields(vm);
	f = QC_EdictFields(vm, ent);
	mi = (int)efloat(vm, ent, EF.modelindex, 0);
	if (mi <= 0 || mi >= cl.nummodels)
		return;
	if (((int)efloat(vm, ent, EF.renderflags, 0)) & 2) /* RF_EXTERNALMODEL: not in the main view */
		return;
	e = R_AllocEntity();
	if (!e)
		return;
	e->vm = vm;
	e->qcent = ent;
	e->modelindex = mi;
	e->model = R_ModelForIndex(mi);
	if (EF.origin >= 0)
		memcpy(e->origin, &f[EF.origin], sizeof(float) * 3);
	if (EF.angles >= 0)
		memcpy(e->angles, &f[EF.angles], sizeof(float) * 3);
	e->frame = (int)efloat(vm, ent, EF.frame, 0);
	e->frame2 = (int)efloat(vm, ent, EF.frame2, 0);
	e->lerpfrac = efloat(vm, ent, EF.lerpfrac, 0);
	e->frame1time = efloat(vm, ent, EF.frame1time, 0);
	e->frame2time = efloat(vm, ent, EF.frame2time, 0);
	e->skin = (int)efloat(vm, ent, EF.skin, 0);
	e->skeletonindex = (int)efloat(vm, ent, EF.skeletonindex, 0);
	e->scale = efloat(vm, ent, EF.scale, 1);
	if (e->scale == 0)
		e->scale = 1;
	e->alpha = efloat(vm, ent, EF.alpha, 1);
	if (e->alpha == 0)
		e->alpha = 1;
	if (EF.colormod >= 0) {
		memcpy(e->colormod, &f[EF.colormod], sizeof(float) * 3);
		if (!e->colormod[0] && !e->colormod[1] && !e->colormod[2])
			e->colormod[0] = e->colormod[1] = e->colormod[2] = 1;
	}
	e->renderflags = (int)efloat(vm, ent, EF.renderflags, 0);
	e->effects = (int)efloat(vm, ent, EF.effects, 0);
	e->entnum = (int)efloat(vm, ent, EF.entnum, 0);
	if (EF.forceshader >= 0 && f[EF.forceshader].f > 0)
		e->forceshader = R_MaterialForHandle((int)f[EF.forceshader].f);
	/* models face their angles with pitch inverted, as in Quake; brush
	 * models use the angles as given */
	{
		float a[3] = {e->angles[0], e->angles[1], e->angles[2]};
		if (e->model && e->model->type != MOD_BRUSH)
			a[0] = -a[0];
		AnglesToAxis(a, e->axis);
	}
	if (e->renderflags & 1) {
		/* RF_VIEWMODEL: placed relative to the view */
		float o[3], axis[3][3];
		AnglesToAxis(r_scene.angles, axis);
		for (int k = 0; k < 3; k++)
			o[k] = r_scene.origin[k] + axis[0][k] * e->origin[0] + axis[1][k] * e->origin[1] + axis[2][k] * e->origin[2];
		memcpy(e->origin, o, sizeof(o));
		memcpy(e->axis, axis, sizeof(axis));
	}
}

void R_AddEntities(qcvm_t *vm, int mask)
{
	/* predraws run with self set to their entity; the QC that called
	 * addentities is usually a method that still needs its own self */
	int oldself = QC_GInt(vm, vm->gofs.self);
	lookup_fields(vm);
	if (EF.drawmask < 0)
		return;
	for (int e = 1; e < vm->num_edicts; e++) {
		qcval_t *f;
		if (vm->edicts[e].free)
			continue;
		f = QC_EdictFields(vm, e);
		if (!((int)f[EF.drawmask].f & mask))
			continue;
		if (EF.predraw >= 0 && f[EF.predraw].i) {
			/* predraw returns PREDRAW_NEXT (1) to skip the automatic add */
			QC_SetGInt(vm, vm->gofs.self, e);
			if (!QC_Execute(vm, f[EF.predraw].i))
				continue;
			if (vm->edicts[e].free || vm->globals[OFS_RETURN].f != 0)
				continue;
		}
		R_AddEntityFromQC(vm, e);
	}
	QC_SetGInt(vm, vm->gofs.self, oldself);
}

int R_AddDynamicLight(const float *org, float radius, const float *color)
{
	rdlight_t *l;
	if (r_scene.numdlights >= R_MAX_DLIGHTS)
		return -1;
	l = &r_scene.dlights[r_scene.numdlights];
	memcpy(l->origin, org, sizeof(float) * 3);
	l->radius = radius;
	memcpy(l->color, color, sizeof(float) * 3);
	return r_scene.numdlights++;
}

/* the field of view for the viewport: either set explicitly or derived from
 * the approximate fov, which, as in Source, is the horizontal angle a 4:3
 * screen would get, widened to the actual aspect */
static void compute_fov(float *fx, float *fy)
{
	float aspect = r_scene.vh > 0 ? r_scene.vw / r_scene.vh : 4.0f / 3;
	if (r_scene.fov_x > 0 && r_scene.fov_y > 0) {
		*fx = r_scene.fov_x;
		*fy = r_scene.fov_y;
		return;
	}
	{
		float a = BW_MAX(1.0f, BW_MIN(170.0f, r_scene.afov > 0 ? r_scene.afov : 90));
		float ty = tanf(a * (float)M_PI / 360.0f) * 0.75f;
		*fy = atanf(ty) * 360.0f / (float)M_PI;
		*fx = atanf(ty * aspect) * 360.0f / (float)M_PI;
	}
}

void R_SetupView(void)
{
	float fx, fy;
	compute_fov(&fx, &fy);
	Mat4_View(r_scene.view, r_scene.origin, r_scene.angles);
	if (r_scene.perspective) {
		Mat4_Perspective(r_scene.proj, fx, fy, r_scene.nearclip > 0 ? r_scene.nearclip : 4, r_scene.farclip);
	} else {
		/* isometric: fov is the units shown each side */
		Mat4_Ortho(r_scene.proj, -fx, fx, -fy, fy, -65536, 65536);
	}
	Mat4_Multiply(r_scene.viewproj, r_scene.proj, r_scene.view);
	AngleVectors(r_scene.angles, r_scene.forward, r_scene.right, r_scene.up);
}

/* world point to virtual screen point; z is the distance in front of the view */
void R_Project(const float *in, float *out)
{
	float clip[4];
	R_SetupView();
	Mat4_Transform(r_scene.viewproj, in, clip);
	if (fabsf(clip[3]) < 1e-6f)
		clip[3] = 1e-6f;
	out[0] = r_scene.vx + (clip[0] / clip[3] * 0.5f + 0.5f) * r_scene.vw;
	out[1] = r_scene.vy + (0.5f - clip[1] / clip[3] * 0.5f) * r_scene.vh;
	out[2] = clip[3];
}

/* virtual screen point plus a distance along the view to a world point */
void R_Unproject(const float *in, float *out)
{
	float fx, fy, nx, ny;
	R_SetupView();
	compute_fov(&fx, &fy);
	nx = ((in[0] - r_scene.vx) / r_scene.vw) * 2 - 1;
	ny = 1 - ((in[1] - r_scene.vy) / r_scene.vh) * 2;
	nx *= tanf(fx * (float)M_PI / 360.0f);
	ny *= tanf(fy * (float)M_PI / 360.0f);
	for (int k = 0; k < 3; k++)
		out[k] = r_scene.origin[k] + (r_scene.forward[k] + r_scene.right[k] * nx + r_scene.up[k] * ny) * in[2];
}

static int dist_cmp(const void *a, const void *b)
{
	const rentity_t *x = *(rentity_t *const *)a, *y = *(rentity_t *const *)b;
	float dx = 0, dy = 0;
	for (int k = 0; k < 3; k++) {
		dx += (x->origin[k] - r_scene.origin[k]) * (x->origin[k] - r_scene.origin[k]);
		dy += (y->origin[k] - r_scene.origin[k]) * (y->origin[k] - r_scene.origin[k]);
	}
	return dx < dy ? 1 : dx > dy ? -1 : 0;
}

void R_RenderScene(void)
{
	float sx, sy, model[16];
	int x, y, w, h;
	static rentity_t *sorted[R_MAX_ENTITIES];
	int nsorted = 0;
	if (!R_Active())
		return;
	R_Flush2D();
	R_DebugView();
	R_SetupView();
	/* the viewport is given in virtual pixels; GL wants physical ones, from the bottom */
	sx = (float)r_pwidth / r_vwidth;
	sy = (float)r_pheight / r_vheight;
	x = (int)(r_scene.vx * sx);
	w = (int)(r_scene.vw * sx);
	h = (int)(r_scene.vh * sy);
	y = r_pheight - (int)(r_scene.vy * sy) - h;
	qglViewport(x, y, w, h);
	memset(&r_stats, 0, sizeof(r_stats));
	qglEnable(GL_SCISSOR_TEST);
	qglScissor(x, y, w, h);
	GL_SetDepth(true, true);
	qglDepthFunc(GL_LEQUAL);
	qglClear(GL_DEPTH_BUFFER_BIT);
	if (!r_scene.drawworld || !r_world.loaded) {
		qglClearColor(0, 0, 0, 1);
		qglClear(GL_COLOR_BUFFER_BIT);
	}

	if (r_scene.vw * r_scene.vh >= 0.5f * r_vwidth * r_vheight) {
		VectorCopy(r_scene.origin, r_scene.mainorigin);
		VectorCopy(r_scene.forward, r_scene.mainforward);
	}
	R_UpdateParticles();
	R_AddParticleLights();
	Mat4_Identity(model);
	if (r_scene.drawworld && r_world.loaded && r_drawworld->ivalue && r_world.numsubmodels)
		R_DrawWorldModel(&r_world.submodels[0], r_scene.viewproj, model, 1, false);
	if (r_scene.drawworld && r_drawworld->ivalue)
		R_DrawVoxels(false);
	if (r_drawentities->ivalue)
		for (int i = 0; i < r_scene.numents; i++)
			R_DrawModelEntity(&r_scene.ents[i], false);
	if (r_scene.drawworld && r_world.loaded && r_drawworld->ivalue) {
		R_DrawSky(r_scene.viewproj);
		R_DrawDecals();
	}

	/* translucent: world water and glass, then entities back to front, then polygons */
	if (r_scene.drawworld && r_world.loaded && r_drawworld->ivalue && r_world.numsubmodels)
		R_DrawWorldModel(&r_world.submodels[0], r_scene.viewproj, model, 1, true);
	if (r_scene.drawworld && r_drawworld->ivalue)
		R_DrawVoxels(true);
	if (r_drawentities->ivalue) {
		for (int i = 0; i < r_scene.numents; i++)
			sorted[nsorted++] = &r_scene.ents[i];
		qsort(sorted, nsorted, sizeof(sorted[0]), dist_cmp);
		for (int i = 0; i < nsorted; i++)
			R_DrawModelEntity(sorted[i], true);
	}
	R_DrawParticles();
	R_DrawClipBrushes();
	r_stats.polys = numpolys;
	R_DrawPolygons(false);
	numpolys = 0;
	numpolyverts = 0;
	poly_start = 0;

	if (r_speeds->ivalue)
		Con_Printf("view %dx%d at %.0f %.0f %.0f (%.0f %.0f): %d world surfaces, %d culled, %d entities, %d polys, %d particles%s\n", w,
		           h, r_scene.origin[0], r_scene.origin[1], r_scene.origin[2], r_scene.angles[0], r_scene.angles[1],
		           r_stats.worldsurfs, r_stats.culled, r_scene.numents, r_stats.polys, r_stats.particles,
		           r_scene.drawworld ? "" : ", no world");
	if (r_speeds->ivalue > 1)
		for (int i = 0; i < r_scene.numents; i++) {
			rentity_t *e = &r_scene.ents[i];
			float amb[3], dif[3], dir[3];
			R_LightPoint(e->origin, amb, dif, dir);
			Con_Printf("  ent %d: %s frame %d at %.0f %.0f %.0f flags %d alpha %.2f light %.2f %.2f %.2f + %.2f %.2f %.2f\n",
			           e->entnum, e->model ? e->model->name : "(no model)", e->frame, e->origin[0], e->origin[1],
			           e->origin[2], e->renderflags, e->alpha, amb[0], amb[1], amb[2], dif[0], dif[1], dif[2]);
		}
	qglDisable(GL_SCISSOR_TEST);
	GL_SetBlend(BLEND_OPAQUE, 0, 0);
	/* back to 2D for whatever the client game draws next */
	R_Begin2D(r_vwidth, r_vheight);
}

void R_InitScene(void)
{
	fov = Cvar_Get("fov", "90", CVAR_ARCHIVE, "Horizontal field of view at 4:3, widened for wider screens");
	r_nearclip = Cvar_Get("r_nearclip", "4", 0, "Distance to the near clip plane");
	R_InitDecals();
	R_InitParticles();
	R_ClearScene();
}
