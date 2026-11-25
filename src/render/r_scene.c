/* Bulwark engine - the view and scene the client game builds each frame:
 * clearscene, view properties, addentity/addentities, polygons, dynamic
 * lights and renderscene. ISC licence, see LICENSE. */
#include "r_local.h"
#include "../server/sv_local.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

rscene_t r_scene;
rstats_t r_stats;
static cvar_t *fov, *r_nearclip, *r_viewmodel_fov, *r_viewmodel_quakepitch, *r_viewmodel_mirror, *r_seqblend;

/* whether an entity with these render flags is drawn mirrored left to right
 * (r_viewmodel_mirror for view models, RF_MIRROR for any entity) */
bool R_EntityMirrored(int renderflags)
{
	bool mirror = (renderflags & RF_MIRROR) != 0;
	if ((renderflags & (1 | 4)) && r_viewmodel_mirror && r_viewmodel_mirror->ivalue)
		mirror = !mirror;
	return mirror;
}

/* entity fields addentity reads, looked up once per client game */
static struct {
	qcvm_t *vm;
	int modelindex, origin, angles, frame, frame2, lerpfrac, frame1time, frame2time, skin, scale, alpha;
	int colormod, glowmod, renderflags, effects, drawmask, predraw, forceshader, entnum, drawflags;
	int skeletonindex, outlinecolor, mins, maxs;
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
	FLD(skeletonindex); FLD(outlinecolor); FLD(mins); FLD(maxs);
#undef FLD
}

/* --- polygons ----------------------------------------------------------------- */

typedef struct {
	material_t *mat;
	int flags;
	int firstvert, numverts;
} rpoly_t;

#ifdef BW_LOWMEM
#define MAX_POLY_VERTS 8192
#define MAX_POLYS 2048
#else
#define MAX_POLY_VERTS 32768
#define MAX_POLYS 8192
#endif
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

/* R_EndPolygonRibbon(radius, texcoordbias): the vertices given are a line;
 * each segment becomes a quad of that half-width facing the view, the
 * second edge's texture coordinates offset by texcoordbias */
void R_EndPolygonRibbon(float radius, const float *tcbias)
{
	enum { MAX_RIBBON = 256 };
	rvert_t line[MAX_RIBBON];
	int count = BW_MIN(numpolyverts - poly_start, MAX_RIBBON);
	float side[MAX_RIBBON][3];
	if (count < 2) {
		numpolyverts = poly_start;
		return;
	}
	memcpy(line, polyverts + poly_start, sizeof(rvert_t) * (size_t)count);
	numpolyverts = poly_start;
	/* per vertex: the sideways direction, across the line and the view */
	for (int i = 0; i < count; i++) {
		const float *a = line[i > 0 ? i - 1 : 0].xyz, *b = line[i + 1 < count ? i + 1 : count - 1].xyz;
		float dir[3], toview[3], len;
		VectorSubtract(b, a, dir);
		VectorSubtract(r_scene.origin, line[i].xyz, toview);
		CrossProduct(dir, toview, side[i]);
		len = sqrtf(DotProduct(side[i], side[i]));
		VectorScale(side[i], len > 0 ? radius / len : 0, side[i]);
	}
	for (int i = 0; i + 1 < count && numpolyverts + 4 <= MAX_POLY_VERTS && numpolys < MAX_POLYS; i++) {
		const int order[4][2] = {{0, 1}, {1, 1}, {1, -1}, {0, -1}};
		rpoly_t *p;
		poly_start = numpolyverts;
		for (int k = 0; k < 4; k++) {
			const rvert_t *src = &line[i + order[k][0]];
			rvert_t *v = &polyverts[numpolyverts++];
			*v = *src;
			for (int c = 0; c < 3; c++)
				v->xyz[c] += side[i + order[k][0]][c] * (float)order[k][1];
			if (order[k][1] < 0) {
				v->st[0] += tcbias[0];
				v->st[1] += tcbias[1];
			}
		}
		p = &polys[numpolys++];
		p->mat = poly_mat;
		p->flags = poly_flags;
		p->firstvert = poly_start;
		p->numverts = 4;
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
	r_scene.skyroom = false;
	r_scene.isolated = false;
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

/* Sequence blending, as GoldSrc does it: when a skeletal model's sequence
 * changes, the pose it had then fades out over r_seqblend seconds instead
 * of snapping. Only for client game entities that blend nothing themselves
 * (lerpfrac 0) and have no skeleton object; the old pose is held still
 * while it fades. Kept per entity slot and forgotten when the slot changes
 * model or isn't drawn for a while. */
typedef struct {
	int modelindex, frame, prevframe;
	float frame1time, prevtime;
	double changed, seen;
} seqblend_t;

static void seq_blend(qcvm_t *vm, int ent, rentity_t *e)
{
	static seqblend_t *slots;
	static int numslots;
	seqblend_t *b;
	double now = cl.time;
	float len = r_seqblend ? r_seqblend->value : 0, s;
	if (vm != cl.csqc || ent <= 0 || len <= 0 || !e->model || e->model->type != MOD_IQM || e->skeletonindex ||
	    e->lerpfrac != 0)
		return;
	if (ent >= numslots) {
		int n = BW_MAX(ent + 1, numslots * 2);
		slots = Mem_Realloc(slots, sizeof(*slots) * n);
		memset(slots + numslots, 0, sizeof(*slots) * (n - numslots));
		numslots = n;
	}
	b = &slots[ent];
	if (b->modelindex != e->modelindex || now < b->seen || now - b->seen > 0.5) {
		b->modelindex = e->modelindex;
		b->frame = e->frame;
		b->changed = -1e9;
	} else if (e->frame != b->frame) {
		/* a change in the middle of a blend starts from the sequence then
		 * showing most, as only two poses are mixed */
		bool mid = now - b->changed < len && (now - b->changed) / len < 0.5f;
		if (!mid) {
			b->prevframe = b->frame;
			b->prevtime = b->frame1time;
		}
		b->changed = now;
		b->frame = e->frame;
	}
	b->frame1time = e->frame1time;
	b->seen = now;
	s = (float)((now - b->changed) / len);
	if (s >= 0 && s < 1 && b->prevframe != e->frame) {
		e->frame2 = b->prevframe;
		e->frame2time = b->prevtime;
		e->lerpfrac = 1 - s;
	}
}

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
	e->customskin = R_EntitySkin(vm, ent);
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
	if (e->renderflags & RF_OUTLINE) {
		/* without a colour, Counter-Strike Online's teammate green */
		static const float green[3] = {76 / 255.0f, 208 / 255.0f, 113 / 255.0f};
		if (EF.outlinecolor >= 0)
			memcpy(e->outline, &f[EF.outlinecolor], sizeof(float) * 3);
		if (!e->outline[0] && !e->outline[1] && !e->outline[2])
			memcpy(e->outline, green, sizeof(green));
		if (EF.mins >= 0 && EF.maxs >= 0) {
			memcpy(e->mins, &f[EF.mins], sizeof(float) * 3);
			memcpy(e->maxs, &f[EF.maxs], sizeof(float) * 3);
		}
	}
	e->effects = (int)efloat(vm, ent, EF.effects, 0);
	e->entnum = (int)efloat(vm, ent, EF.entnum, 0);
	if (vm == cl.csqc) {
		float fr = (float)e->frame;
		if (CSQC_LerpEntity(e->entnum, ent, e->origin, e->angles, &fr, &e->frame1time))
			e->frame = (int)fr;
	}
	seq_blend(vm, ent, e);
	if (EF.forceshader >= 0 && f[EF.forceshader].f > 0)
		e->forceshader = R_MaterialForHandle((int)f[EF.forceshader].f);
	/* models face their angles with pitch inverted, as in Quake, unless
	 * r_meshpitch is 1 (Nuclide); brush models use the angles as given. View models (RF_VIEWMODEL or
	 * RF_DEPTHHACK) are given the camera's angles by games such as Nuclide,
	 * so they keep the camera's convention and point where the camera does;
	 * r_viewmodel_quakepitch 1 inverts them like other models, as FTE does */
	{
		float a[3] = {e->angles[0], e->angles[1], e->angles[2]};
		bool vm = (e->renderflags & (1 | 4)) && !(r_viewmodel_quakepitch && r_viewmodel_quakepitch->ivalue);
		if (e->model && e->model->type != MOD_BRUSH && !vm)
			a[0] *= QC_MeshPitch();
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
	/* view models mirrored left to right: Counter-Strike's view models are
	 * built for the left hand and the game shows them mirrored. RF_MIRROR
	 * (65536, Bulwark's own) mirrors one entity, so a game can mirror only
	 * the view models that need it; with r_viewmodel_mirror it cancels. */
	e->mirrored = false;
	if (R_EntityMirrored(e->renderflags)) {
		for (int k = 0; k < 3; k++)
			e->axis[1][k] = -e->axis[1][k];
		e->mirrored = true;
	}
}

/* client game entities given to makestatic: they stay where they were put
 * and join every scene that adds engine entities, until the map changes */
#define MAX_STATIC_ENTS 1024
static struct {
	qcvm_t *vm;
	int ent;
} static_ents[MAX_STATIC_ENTS];
static int num_static_ents;

void R_MakeStatic(qcvm_t *vm, int e)
{
	lookup_fields(vm);
	if (e <= 0 || num_static_ents >= MAX_STATIC_ENTS)
		return;
	for (int i = 0; i < num_static_ents; i++)
		if (static_ents[i].vm == vm && static_ents[i].ent == e)
			return;
	static_ents[num_static_ents].vm = vm;
	static_ents[num_static_ents++].ent = e;
	/* addentities would add it again */
	if (EF.drawmask >= 0)
		QC_EdictFields(vm, e)[EF.drawmask].f = 0;
}

void R_ClearStatics(void) { num_static_ents = 0; }

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
	if (mask & 1) /* MASK_ENGINE */
		for (int i = 0; i < num_static_ents; i++)
			if (static_ents[i].vm == vm && static_ents[i].ent < vm->num_edicts && !vm->edicts[static_ents[i].ent].free)
				R_AddEntityFromQC(vm, static_ents[i].ent);
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
	/* view models (RF_VIEWMODEL, RF_DEPTHHACK) get their own projection: a
	 * near plane close enough that the gun never clips, and optionally their
	 * own field of view, as Source's viewmodel_fov */
	{
		float vproj[16], vfx = fx, vfy = fy;
		if (r_viewmodel_fov && r_viewmodel_fov->value > 0 && r_scene.perspective) {
			float saved = r_scene.afov, sx = r_scene.fov_x, sy = r_scene.fov_y;
			r_scene.afov = r_viewmodel_fov->value;
			r_scene.fov_x = r_scene.fov_y = 0;
			compute_fov(&vfx, &vfy);
			r_scene.afov = saved, r_scene.fov_x = sx, r_scene.fov_y = sy;
		}
		if (r_scene.perspective)
			Mat4_Perspective(vproj, vfx, vfy, 1, r_scene.farclip);
		else
			memcpy(vproj, r_scene.proj, sizeof(vproj));
		Mat4_Multiply(r_scene.vmviewproj, vproj, r_scene.view);
	}
	AngleVectors(r_scene.angles, r_scene.forward, r_scene.right, r_scene.up);
	/* the map's fog, waterfog when the eye is under water, skyroomfog in the skyroom */
	{
		static const float zero[3];
		const float *f = NULL;
		r_viewfog[3] = 0;
		if (r_scene.noskyroom && r_world.skyroomfog[0] > 0)
			f = r_world.skyroomfog;
		else if (!r_scene.noskyroom && r_world.waterfog[0] > 0 && cl.world &&
		         (BSP_PointContents(cl.world, 0, zero, r_scene.origin) & (CONTENTS_WATER | CONTENTS_SLIME | CONTENTS_LAVA)))
			f = r_world.waterfog;
		if (f) {
			r_viewfog[0] = f[1], r_viewfog[1] = f[2], r_viewfog[2] = f[3], r_viewfog[3] = f[0];
		} else if (r_world.hasfog) {
			memcpy(r_viewfog, r_world.fog, sizeof(r_viewfog));
		}
		/* densities are as Quake maps give them ("fog" "0.05 r g b"): squared
		 * exponential fog at a 64th of the density per unit, the convention
		 * Quake mappers' tools and documentation use */
		r_viewfog[3] /= 64;
	}
}

float r_viewfog[4];

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
		qglClearColor(0, 0, 0, r_scene.isolated ? 0 : 1);
		qglClear(GL_COLOR_BUFFER_BIT);
	}

	if (r_scene.vw * r_scene.vh >= 0.5f * r_vwidth * r_vheight && !R_ToolsRedrawing() && !r_scene.isolated) {
		float fx, fy;
		compute_fov(&fx, &fy);
		VectorCopy(r_scene.origin, r_scene.mainorigin);
		VectorCopy(r_scene.forward, r_scene.mainforward);
		VectorCopy(r_scene.angles, r_scene.mainangles);
		r_scene.mainfov[0] = fx, r_scene.mainfov[1] = fy;
		r_scene.maindrawworld = r_scene.drawworld;
		memcpy(r_scene.mainviewproj, r_scene.viewproj, sizeof(r_scene.mainviewproj));
		r_scene.mainvp[0] = r_scene.vx, r_scene.mainvp[1] = r_scene.vy;
		r_scene.mainvp[2] = r_scene.vw, r_scene.mainvp[3] = r_scene.vh;
		r_scene.havemain = true;
	}
	if (!r_scene.isolated) {
		R_UpdateParticles();
		R_AddRTLights();
		R_AddQCLights();
		R_AddParticleLights();
	}
	Mat4_Identity(model);
	/* the skyroom first, seen through the sky: then the sky itself isn't drawn */
	if (r_scene.skyroom && r_scene.drawworld && r_world.loaded && r_drawworld->ivalue && r_world.numsubmodels) {
		float saved[3];
		VectorCopy(r_scene.origin, saved);
		VectorCopy(r_scene.skyroom_origin, r_scene.origin);
		r_scene.noskyroom = true;
		R_SetupView();
		R_DrawWorldModel(&r_world.submodels[0], r_scene.viewproj, model, 1, false);
		R_DrawWorldModel(&r_world.submodels[0], r_scene.viewproj, model, 1, true);
		r_scene.noskyroom = false;
		VectorCopy(saved, r_scene.origin);
		R_SetupView();
		qglClear(GL_DEPTH_BUFFER_BIT);
	}
	if (r_scene.drawworld && r_world.loaded && r_drawworld->ivalue && r_world.numsubmodels)
		R_DrawWorldModel(&r_world.submodels[0], r_scene.viewproj, model, 1, false);
	if (r_scene.drawworld && r_drawworld->ivalue && vx_clientworld && !(r_world.loaded && r_world.numsubmodels))
		R_DrawBackgroundSky(r_scene.viewproj);
	if (r_scene.drawworld && r_drawworld->ivalue)
		R_DrawVoxels(false);
	if (r_drawentities->ivalue) {
		if (r_scene.drawworld && r_world.loaded)
			R_DrawStaticProps(false);
		for (int i = 0; i < r_scene.numents; i++)
			R_DrawModelEntity(&r_scene.ents[i], false);
	}
	if (r_scene.drawworld && r_world.loaded && r_drawworld->ivalue) {
		if (!r_scene.skyroom)
			R_DrawSky(r_scene.viewproj);
		R_DrawDecals();
		R_DrawToolsWorld();
	}

	/* translucent: world water and glass, then entities back to front, then polygons */
	if (r_scene.drawworld && r_world.loaded && r_drawworld->ivalue && r_world.numsubmodels)
		R_DrawWorldModel(&r_world.submodels[0], r_scene.viewproj, model, 1, true);
	if (r_scene.drawworld && r_drawworld->ivalue)
		R_DrawVoxels(true);
	if (r_drawentities->ivalue) {
		if (r_scene.drawworld && r_world.loaded)
			R_DrawStaticProps(true);
		for (int i = 0; i < r_scene.numents; i++)
			sorted[nsorted++] = &r_scene.ents[i];
		qsort(sorted, nsorted, sizeof(sorted[0]), dist_cmp);
		for (int i = 0; i < nsorted; i++)
			R_DrawModelEntity(sorted[i], true);
		R_DrawOutlines();
	}
	if (!r_scene.isolated) {
		R_DrawParticles();
		R_DrawClipBrushes();
		r_stats.polys = numpolys;
		R_DrawPolygons(false);
		numpolys = 0;
		numpolyverts = 0;
		poly_start = 0;
	}

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
	r_viewmodel_quakepitch = Cvar_Get("r_viewmodel_quakepitch", "0", CVAR_ARCHIVE,
	                                  "1 inverts view model pitch like other models (FTE's behaviour); 0 makes them follow the camera");
	r_viewmodel_mirror = Cvar_Get("r_viewmodel_mirror", "0", CVAR_ARCHIVE,
	                              "1 mirrors view models left to right (for models built for the other hand)");
	r_viewmodel_fov = Cvar_Get("r_viewmodel_fov", "0", CVAR_ARCHIVE,
	                           "Field of view for view models (like Source's viewmodel_fov); 0 uses the view's");
	r_nearclip = Cvar_Get("r_nearclip", "4", 0, "Distance to the near clip plane");
	r_seqblend = Cvar_Get("r_seqblend", "0.1", CVAR_ARCHIVE,
	                      "Seconds a skeletal model takes to blend into a new sequence, 0 to switch at once");
	R_InitDecals();
	R_InitParticles();
	R_ClearScene();
}
