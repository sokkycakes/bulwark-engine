/* Bulwark engine - skeleton objects and tags for QC (FTE_CSQC_SKELETONOBJECTS
 * and DP_QC_GETTAGINFO, as their doc comments describe them). Every VM gets
 * these; a VM finds its models through vm->modelname. ISC licence, see
 * LICENSE. */
#include "qc_internal.h"
#include "../common/skel.h"
#include "../common/cmd.h"
#include "../common/fs.h"
#include "../common/studio.h"
#include "../common/cvar.h"

#include <math.h>

typedef struct {
	qcvm_t *owner;
	skelmodel_t *model;
	bonemat_t *bones;
	bool absolute;      /* bones are relative to the entity, not the parent */
	bool deleted;
	double deletetime;
} skelobj_t;

#define MAX_SKELOBJS 4096
static skelobj_t skelobjs[MAX_SKELOBJS];
static int numskelobjs;

/* entity fields, looked up once per VM */
static struct fieldcache_s {
	qcvm_t *vm;
	int skeletonindex, frame, frame2, lerpfrac, frame1time, frame2time;
} fieldcache[4];

static struct fieldcache_s *fields(qcvm_t *vm)
{
	struct fieldcache_s *c = &fieldcache[0];
	for (int i = 0; i < (int)BW_ARRAY_COUNT(fieldcache); i++)
		if (fieldcache[i].vm == vm)
			return &fieldcache[i];
	memmove(fieldcache + 1, fieldcache, sizeof(fieldcache) - sizeof(fieldcache[0]));
	c->vm = vm;
	c->skeletonindex = QC_FieldOfs(vm, "skeletonindex");
	c->frame = QC_FieldOfs(vm, "frame");
	c->frame2 = QC_FieldOfs(vm, "frame2");
	c->lerpfrac = QC_FieldOfs(vm, "lerpfrac");
	c->frame1time = QC_FieldOfs(vm, "frame1time");
	c->frame2time = QC_FieldOfs(vm, "frame2time");
	return c;
}

static skelmodel_t *model_for_index(qcvm_t *vm, int index)
{
	const char *name = vm->modelname ? vm->modelname(vm, index) : NULL;
	return name ? Skel_LoadModel(name) : NULL;
}

static skelobj_t *get_skel(qcvm_t *vm, int id)
{
	skelobj_t *s;
	if (id <= 0 || id > numskelobjs)
		return NULL;
	s = &skelobjs[id - 1];
	/* a deleted object stays usable until the frame ends */
	return s->owner == vm && s->model ? s : NULL;
}

bool QC_SkelBones(qcvm_t *vm, int id, skelmodel_t **model, const float (**bones)[12], bool *absolute)
{
	skelobj_t *s = get_skel(vm, id);
	if (!s)
		return false;
	*model = s->model;
	*bones = (const float(*)[12])s->bones;
	*absolute = s->absolute;
	return true;
}

static void free_skel(skelobj_t *s)
{
	Mem_Free(s->bones);
	memset(s, 0, sizeof(*s));
}

void QC_SkelFreeVM(qcvm_t *vm)
{
	for (int i = 0; i < numskelobjs; i++)
		if (skelobjs[i].owner == vm)
			free_skel(&skelobjs[i]);
	while (numskelobjs && !skelobjs[numskelobjs - 1].model)
		numskelobjs--;
	for (int i = 0; i < (int)BW_ARRAY_COUNT(fieldcache); i++)
		if (fieldcache[i].vm == vm)
			fieldcache[i].vm = NULL;
}

/* bone n (1-based) of s, or NULL */
static float *bone_arg(skelobj_t *s, float n)
{
	int b = (int)n - 1;
	return s && b >= 0 && b < s->model->numjoints ? s->bones[b] : NULL;
}

/* the absolute matrix of bone b (0-based) */
static void bone_abs(const skelobj_t *s, int b, bonemat_t out)
{
	memcpy(out, s->bones[b], sizeof(bonemat_t));
	if (s->absolute)
		return;
	for (int p = s->model->joints[b].parent; p >= 0; p = s->model->joints[p].parent)
		Bone_Multiply(out, s->bones[p], out);
}

static void return_bone(qcvm_t *vm, const bonemat_t m)
{
	float org[3], f[3], r[3], u[3];
	Bone_ToVectors(m, org, f, r, u);
	QC_SetGVec(vm, vm->gofs.v_forward, f);
	QC_SetGVec(vm, vm->gofs.v_right, r);
	QC_SetGVec(vm, vm->gofs.v_up, u);
	QC_ReturnVector(vm, org);
}

/* the matrix given as org [, fwd, right, up], or the v_ globals when the
 * orientation is left out */
static void matrix_args(qcvm_t *vm, int first, bonemat_t m)
{
	static const float zero[3];
	const float *f, *r, *u;
	if (vm->argc > first + 3) {
		f = QC_PVEC(vm, first + 1), r = QC_PVEC(vm, first + 2), u = QC_PVEC(vm, first + 3);
	} else if (vm->argc == first + 2) {
		/* just a forward vector: right and up made level with it */
		float rr[3], uu[3], ff[3];
		VectorCopy(QC_PVEC(vm, first + 1), ff);
		VectorNormalize(ff);
		rr[0] = ff[1], rr[1] = -ff[0], rr[2] = 0;
		if (VectorNormalize(rr) == 0)
			rr[0] = 0, rr[1] = -1, rr[2] = 0;
		uu[0] = rr[1] * ff[2] - rr[2] * ff[1];
		uu[1] = rr[2] * ff[0] - rr[0] * ff[2];
		uu[2] = rr[0] * ff[1] - rr[1] * ff[0];
		Bone_FromVectors(m, QC_PVEC(vm, first), ff, rr, uu);
		return;
	} else {
		f = vm->gofs.v_forward >= 0 ? &vm->globals[vm->gofs.v_forward].f : zero;
		r = vm->gofs.v_right >= 0 ? &vm->globals[vm->gofs.v_right].f : zero;
		u = vm->gofs.v_up >= 0 ? &vm->globals[vm->gofs.v_up].f : zero;
	}
	Bone_FromVectors(m, QC_PVEC(vm, first), f, r, u);
}

/* --- builtins ---------------------------------------------------------------- */

static void bi_skel_create(qcvm_t *vm)
{
	skelmodel_t *m = model_for_index(vm, (int)QC_PFLOAT(vm, 0));
	skelobj_t *s = NULL;
	int id = 0;
	if (!m) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	for (int i = 0; i < numskelobjs; i++) {
		if (skelobjs[i].deleted && skelobjs[i].deletetime != host_time)
			free_skel(&skelobjs[i]);
		if (!s && !skelobjs[i].model)
			s = &skelobjs[i], id = i + 1;
	}
	if (!s) {
		if (numskelobjs >= MAX_SKELOBJS) {
			Con_Printf("^3%s: out of skeletal objects\n", vm->name);
			QC_ReturnFloat(vm, 0);
			return;
		}
		s = &skelobjs[numskelobjs++];
		id = numskelobjs;
	}
	s->owner = vm;
	s->model = m;
	s->absolute = vm->argc > 1 && QC_PFLOAT(vm, 1) != 0;
	s->bones = Mem_Alloc(sizeof(bonemat_t) * m->numjoints);
	Skel_SampleRelative(m, -1, 0, s->bones);
	if (s->absolute)
		Skel_ToAbsolute(m, s->bones, s->bones);
	QC_ReturnFloat(vm, (float)id);
}

/* room for a pose of n bones, reused by every call: skel_build runs for
 * each player every frame */
static bonemat_t *pose_scratch(int n)
{
	static bonemat_t *pose;
	static int max;
	if (n > max) {
		max = n;
		pose = Mem_Realloc(pose, sizeof(bonemat_t) * max);
	}
	return pose;
}

/* bones first..last of the entity's animation pose */
static void entity_pose(qcvm_t *vm, int ent, skelmodel_t *m, int first, int last, bonemat_t *out)
{
	struct fieldcache_s *c = fields(vm);
	Skel_EntityPoseRange(m, (int)QC_EFloat(vm, ent, c->frame), QC_EFloat(vm, ent, c->frame1time),
	                     (int)QC_EFloat(vm, ent, c->frame2), QC_EFloat(vm, ent, c->frame2time),
	                     QC_EFloat(vm, ent, c->lerpfrac), first, last, out);
}

/* skel_build(skel, ent, modelindex, retainfrac, firstbone, lastbone, addfrac):
 * bones = bones * retainfrac + the entity's animation * addfrac */
static void bi_skel_build(qcvm_t *vm)
{
	skelobj_t *s = get_skel(vm, (int)QC_PFLOAT(vm, 0));
	int ent = QC_PENT(vm, 1);
	skelmodel_t *src = model_for_index(vm, (int)QC_PFLOAT(vm, 2));
	float retain = QC_PFLOAT(vm, 3), add = vm->argc > 6 ? QC_PFLOAT(vm, 6) : 1;
	int first = (int)QC_PFLOAT(vm, 4), last = (int)QC_PFLOAT(vm, 5), n;
	bonemat_t *pose;
	if (!s || !src) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	n = BW_MIN(s->model->numjoints, src->numjoints);
	first = first <= 0 ? 0 : first - 1;
	last = last <= 0 ? n - 1 : BW_MIN(last - 1, n - 1);
	pose = pose_scratch(src->numjoints);
	if (s->absolute) {
		/* every parent is needed */
		entity_pose(vm, ent, src, 0, src->numjoints - 1, pose);
		Skel_ToAbsolute(src, pose, pose);
	} else {
		/* just the bones it blends in: a player's torso, on its second build */
		entity_pose(vm, ent, src, first, last, pose);
	}
	if (last < first) {
		/* nothing to blend */
	} else if (retain == 0 && add == 1) {
		/* 0 + x * 1 is x, except that -0 comes out +0: done on the bits,
		   as float maths is slow on the Xbox (an arithmetic result is
		   never a signalling NaN, which the multiply would have quietened) */
		size_t words = (size_t)(last - first + 1) * 12;
		float *d = (float *)(s->bones + first);
		memcpy(d, pose + first, sizeof(float) * words);
		for (size_t i = 0; i < words; i++) {
			uint32_t u;
			memcpy(&u, d + i, 4);
			if (u == 0x80000000u)
				d[i] = 0;
		}
	} else {
		for (int b = first; b <= last; b++)
			for (int k = 0; k < 12; k++)
				s->bones[b][k] = (retain != 0 ? s->bones[b][k] * retain : 0) + pose[b][k] * add;
	}
	QC_ReturnFloat(vm, QC_PFLOAT(vm, 0));
}

static void bi_skel_delete(qcvm_t *vm)
{
	skelobj_t *s = get_skel(vm, (int)QC_PFLOAT(vm, 0));
	if (s && !s->deleted) {
		s->deleted = true;
		s->deletetime = host_time;
	}
}

static void bi_skel_get_numbones(qcvm_t *vm)
{
	skelobj_t *s = get_skel(vm, (int)QC_PFLOAT(vm, 0));
	QC_ReturnFloat(vm, s ? (float)s->model->numjoints : 0);
}

static void bi_skel_get_bonename(qcvm_t *vm)
{
	skelobj_t *s = get_skel(vm, (int)QC_PFLOAT(vm, 0));
	int b = (int)QC_PFLOAT(vm, 1) - 1;
	QC_ReturnString(vm, s && b >= 0 && b < s->model->numjoints ? s->model->joints[b].name : "");
}

static void bi_skel_get_boneparent(qcvm_t *vm)
{
	skelobj_t *s = get_skel(vm, (int)QC_PFLOAT(vm, 0));
	int b = (int)QC_PFLOAT(vm, 1) - 1;
	QC_ReturnFloat(vm, s && b >= 0 && b < s->model->numjoints ? (float)(s->model->joints[b].parent + 1) : 0);
}

static void bi_skel_find_bone(qcvm_t *vm)
{
	skelobj_t *s = get_skel(vm, (int)QC_PFLOAT(vm, 0));
	QC_ReturnFloat(vm, s ? (float)(Skel_FindJoint(s->model, QC_PSTR(vm, 1)) + 1) : 0);
}

static void bi_skel_get_bonerel(qcvm_t *vm)
{
	skelobj_t *s = get_skel(vm, (int)QC_PFLOAT(vm, 0));
	float *b = bone_arg(s, QC_PFLOAT(vm, 1));
	bonemat_t m;
	Bone_Identity(m);
	if (b) {
		memcpy(m, b, sizeof(m));
		if (s->absolute) {
			int p = s->model->joints[(int)QC_PFLOAT(vm, 1) - 1].parent;
			if (p >= 0) {
				bonemat_t inv;
				Bone_Invert(inv, s->bones[p]);
				Bone_Multiply(m, inv, m);
			}
		}
	}
	return_bone(vm, m);
}

static void bi_skel_get_boneabs(qcvm_t *vm)
{
	skelobj_t *s = get_skel(vm, (int)QC_PFLOAT(vm, 0));
	bonemat_t m;
	Bone_Identity(m);
	if (bone_arg(s, QC_PFLOAT(vm, 1)))
		bone_abs(s, (int)QC_PFLOAT(vm, 1) - 1, m);
	return_bone(vm, m);
}

static void bi_skel_set_bone(qcvm_t *vm)
{
	float *b = bone_arg(get_skel(vm, (int)QC_PFLOAT(vm, 0)), QC_PFLOAT(vm, 1));
	if (b)
		matrix_args(vm, 2, b);
}

static void bi_skel_premul_bone(qcvm_t *vm)
{
	float *b = bone_arg(get_skel(vm, (int)QC_PFLOAT(vm, 0)), QC_PFLOAT(vm, 1));
	bonemat_t m;
	if (!b)
		return;
	matrix_args(vm, 2, m);
	Bone_Multiply(b, m, b);
}

static void bi_skel_postmul_bone(qcvm_t *vm)
{
	float *b = bone_arg(get_skel(vm, (int)QC_PFLOAT(vm, 0)), QC_PFLOAT(vm, 1));
	bonemat_t m;
	if (!b)
		return;
	matrix_args(vm, 2, m);
	Bone_Multiply(b, b, m);
}

static void bi_skel_premul_bones(qcvm_t *vm)
{
	skelobj_t *s = get_skel(vm, (int)QC_PFLOAT(vm, 0));
	int first, last;
	bonemat_t m;
	if (!s)
		return;
	first = BW_MAX(1, (int)QC_PFLOAT(vm, 1));
	last = (int)QC_PFLOAT(vm, 2);
	last = last <= 0 ? s->model->numjoints : BW_MIN(last, s->model->numjoints);
	matrix_args(vm, 3, m);
	for (int b = first - 1; b < last; b++)
		Bone_Multiply(s->bones[b], m, s->bones[b]);
}

static void bi_skel_copybones(qcvm_t *vm)
{
	skelobj_t *d = get_skel(vm, (int)QC_PFLOAT(vm, 0)), *s = get_skel(vm, (int)QC_PFLOAT(vm, 1));
	int first, last;
	if (!d || !s)
		return;
	first = BW_MAX(1, (int)QC_PFLOAT(vm, 2));
	last = (int)QC_PFLOAT(vm, 3);
	last = BW_MIN(last <= 0 ? d->model->numjoints : last, BW_MIN(d->model->numjoints, s->model->numjoints));
	for (int b = first - 1; b < last; b++) {
		if (d->absolute == s->absolute)
			memcpy(d->bones[b], s->bones[b], sizeof(bonemat_t));
		else if (d->absolute)
			bone_abs(s, b, d->bones[b]);
		else {
			/* absolute source into a relative skeleton */
			int p = s->model->joints[b].parent;
			memcpy(d->bones[b], s->bones[b], sizeof(bonemat_t));
			if (p >= 0) {
				bonemat_t inv;
				Bone_Invert(inv, s->bones[p]);
				Bone_Multiply(d->bones[b], inv, d->bones[b]);
			}
		}
	}
}

static void bi_gettagindex(qcvm_t *vm)
{
	int ent = QC_PENT(vm, 0);
	skelmodel_t *m = model_for_index(vm, (int)QC_EFloat(vm, ent, vm->fofs.modelindex));
	QC_ReturnFloat(vm, m ? (float)(Skel_FindJoint(m, QC_PSTR(vm, 1)) + 1) : 0);
}

/* the entity's placement, with the pitch of a model turned as r_meshpitch says.
 * View models (RF_VIEWMODEL or RF_DEPTHHACK) keep the camera's pitch, as
 * the renderer draws them (r_viewmodel_quakepitch, r_scene.c), and are
 * mirrored when it mirrors them */
static void entity_matrix(qcvm_t *vm, int ent, bonemat_t out)
{
	static const float zero[3];
	static cvar_t *quakepitch, *mirror;
	const float *o = vm->fofs.origin >= 0 ? &QC_EdictFields(vm, ent)[vm->fofs.origin].f : zero;
	const float *a = vm->fofs.angles >= 0 ? &QC_EdictFields(vm, ent)[vm->fofs.angles].f : zero;
	float ang[3] = {a[0] * QC_MeshPitch(), a[1], a[2]}, f[3], r[3], u[3];
	float scale = QC_EFloat(vm, ent, vm->fofs.scale);
	if (!quakepitch)
		quakepitch = Cvar_Find("r_viewmodel_quakepitch");
	int renderflags = (int)QC_EFloat(vm, ent, vm->fofs.renderflags);
	bool vm_model = (renderflags & (1 | 4)) != 0;
	bool mirrored = (renderflags & 65536) != 0;	/* RF_MIRROR, r_local.h */
	if (!mirror)
		mirror = Cvar_Find("r_viewmodel_mirror");
	if (vm_model && !(quakepitch && quakepitch->ivalue))
		ang[0] = a[0];
	if (scale == 0)
		scale = 1;
	AngleVectors(ang, f, r, u);
	for (int k = 0; k < 3; k++)
		f[k] *= scale, r[k] *= scale, u[k] *= scale;
	Bone_FromVectors(out, o, f, r, u);
	/* mirrored view models (r_viewmodel_mirror, r_scene.c) and RF_MIRROR
	 * entities: their tags and attachments are mirrored with them */
	if (vm_model && mirror && mirror->ivalue)
		mirrored = !mirrored;
	if (mirrored)
		out[1] = -out[1], out[5] = -out[5], out[9] = -out[9];
}

/* bone b (0-based) of an entity in model space, from its skeleton object
 * if it has one, otherwise from its animation */
static bool entity_bone(qcvm_t *vm, int ent, skelobj_t *s, skelmodel_t *m, int b, bonemat_t out)
{
	if (s && b >= 0 && b < s->model->numjoints) {
		bone_abs(s, b, out);
		return true;
	}
	if (m && b >= 0 && b < m->numjoints) {
		bonemat_t *pose = pose_scratch(m->numjoints);
		entity_pose(vm, ent, m, 0, m->numjoints - 1, pose);
		Skel_ToAbsolute(m, pose, pose);
		memcpy(out, pose[b], sizeof(bonemat_t));
		return true;
	}
	return false;
}

/* gettaginfo(ent, tag): the world position of a bone, the orientation in
 * v_. Tags past the bones are a studio model's attachments, in order, as
 * games such as Nuclide number them (bone count + 1 + attachment) */
static void bi_gettaginfo(qcvm_t *vm)
{
	int ent = QC_PENT(vm, 0), tag = (int)QC_PFLOAT(vm, 1) - 1;
	int modelindex = (int)QC_EFloat(vm, ent, vm->fofs.modelindex);
	struct fieldcache_s *c = fields(vm);
	skelobj_t *s = get_skel(vm, (int)QC_EFloat(vm, ent, c->skeletonindex));
	skelmodel_t *m = model_for_index(vm, modelindex);
	int numjoints = s ? s->model->numjoints : m ? m->numjoints : 0;
	const studio_model_t *st = tag >= numjoints && numjoints > 0 ? QC_StudioModel(vm, modelindex) : NULL;
	bonemat_t world, bone;
	entity_matrix(vm, ent, world);
	if (tag >= 0 && tag < numjoints && entity_bone(vm, ent, s, m, tag, bone)) {
		Bone_Multiply(world, world, bone);
	} else if (st && tag - numjoints < st->numattachments &&
	           entity_bone(vm, ent, s, m, st->attachments[tag - numjoints].bone, bone)) {
		const studio_attachment_t *at = &st->attachments[tag - numjoints];
		bonemat_t off;
		Bone_Identity(off);
		off[3] = at->org[0], off[7] = at->org[1], off[11] = at->org[2];
		Bone_Multiply(bone, bone, off);
		Bone_Multiply(world, world, bone);
	} else {
		/* no such bone or attachment: the entity's own placement, unscaled */
		for (int i = 0; i < 3; i++) {
			float l = sqrtf(world[i] * world[i] + world[4 + i] * world[4 + i] + world[8 + i] * world[8 + i]);
			if (l > 0)
				for (int r = 0; r < 3; r++)
					world[r * 4 + i] /= l;
		}
	}
	return_bone(vm, world);
}

static void bi_zero(qcvm_t *vm) { QC_ReturnFloat(vm, 0); }
static void bi_nop(qcvm_t *vm) { (void)vm; }

const qc_builtin_t qc_builtins_skel[] = {
	{"skel_create", bi_skel_create}, {"skel_build", bi_skel_build}, {"skel_delete", bi_skel_delete},
	{"skel_get_numbones", bi_skel_get_numbones}, {"skel_get_bonename", bi_skel_get_bonename},
	{"skel_get_boneparent", bi_skel_get_boneparent}, {"skel_find_bone", bi_skel_find_bone},
	{"skel_get_bonerel", bi_skel_get_bonerel}, {"skel_get_boneabs", bi_skel_get_boneabs},
	{"skel_set_bone", bi_skel_set_bone}, {"skel_premul_bone", bi_skel_premul_bone},
	{"skel_premul_bones", bi_skel_premul_bones}, {"skel_postmul_bone", bi_skel_postmul_bone},
	{"skel_copybones", bi_skel_copybones}, {"gettagindex", bi_gettagindex}, {"gettaginfo", bi_gettaginfo},
	/* not done yet: ragdolls and world-space bone setting */
	{"skel_ragupdate", bi_zero}, {"skel_mmap", bi_zero}, {"skel_set_bone_world", bi_nop},
	{NULL, NULL}};

/* --- console ----------------------------------------------------------------------- */

/* skel_info: the live skeletal objects */
static void SkelInfo_f(int argc, const char **argv)
{
	int n = 0;
	for (int i = 0; i < numskelobjs; i++) {
		const skelobj_t *s = &skelobjs[i];
		if (!s->owner || s->deleted)
			continue;
		Con_Printf("%4d %-8s %-40s %3d bones%s\n", i + 1, s->owner->name, s->model ? s->model->name : "?",
		           s->model ? s->model->numjoints : 0, s->absolute ? " (absolute)" : "");
		n++;
	}
	Con_Printf("%d skeletal objects\n", n);
}

/* skel_generateragdoll <model>: a starting .doll file beside the model: a
 * capsule body along each bone and a joint to its parent's body */
static void SkelGenerateRagdoll_f(int argc, const char **argv)
{
	skelmodel_t *m = argc > 1 ? Skel_LoadModel(argv[1]) : NULL;
	bonemat_t *rel, *abs;
	strbuf_t sb = {0};
	char name[256], path[1024], *dot;
	FILE *f;
	if (!m || !m->numjoints) {
		Con_Printf("Model %s has no bones\n", argc > 1 ? argv[1] : "");
		return;
	}
	rel = Mem_Alloc(sizeof(bonemat_t) * (size_t)m->numjoints);
	abs = Mem_Alloc(sizeof(bonemat_t) * (size_t)m->numjoints);
	for (int j = 0; j < m->numjoints; j++)
		Bone_FromTRS(rel[j], m->base + j * 10);
	Skel_ToAbsolute(m, rel, abs);
	SB_Printf(&sb, "//generated by skel_generateragdoll from %s; tune the sizes, joint types and stops\n", m->name);
	for (int j = 0; j < m->numjoints; j++) {
		int p = m->joints[j].parent;
		float len = 4;
		if (p >= 0) {
			float d[3] = {abs[j][3] - abs[p][3], abs[j][7] - abs[p][7], abs[j][11] - abs[p][11]};
			len = BW_MAX(VectorLength(d), 1.0f);
		}
		SB_Printf(&sb, "body \"%s\" \"%s\"\n\tshape %s\n\tsize %g %g %g\n%s", m->joints[j].name, m->joints[j].name,
		          p >= 0 ? "capsule" : "box", BW_MAX(len * 0.25f, 2.0f), BW_MAX(len * 0.25f, 2.0f), len, p >= 0 ? "\torient\n" : "");
	}
	for (int j = 0; j < m->numjoints; j++) {
		int p = m->joints[j].parent;
		if (p < 0)
			continue;
		SB_Printf(&sb, "joint \"%s\" \"%s\" \"%s\"\n\ttype universal\n\tHiStop 0.5\n\tLoStop -0.5\n\tHiStop2 0.5\n\tLoStop2 -0.5\n",
		          m->joints[j].name, m->joints[p].name, m->joints[j].name);
	}
	Str_Copy(name, m->name, sizeof(name));
	if ((dot = strrchr(name, '.')) && !strchr(dot, '/'))
		*dot = 0;
	Str_Cat(name, ".doll", sizeof(name));
	if (!FS_WritePath(name, path, sizeof(path)) || !FS_CreatePath(path) || !(f = fopen(path, "wb"))) {
		Con_Printf("can't write %s\n", name);
	} else {
		fwrite(sb.data, 1, sb.len, f);
		fclose(f);
		Con_Printf("wrote %s (%d bodies)\n", name, m->numjoints);
	}
	SB_Free(&sb);
	Mem_Free(rel);
	Mem_Free(abs);
}

void QC_InitSkelCommands(void)
{
	Cmd_AddD("skel_info", SkelInfo_f, "The live skeletal objects");
	Cmd_AddD("skel_generateragdoll", SkelGenerateRagdoll_f, "skel_generateragdoll <model>: write a starting .doll ragdoll file for a model");
}
