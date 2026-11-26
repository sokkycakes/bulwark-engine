/* Bulwark engine - skeletal models and skeleton objects, shared by the
 * server and the client. The IQM layout is from the format's public
 * specification (docs/spec/models.md); the skeleton object semantics follow
 * the doc comments of the FTE_CSQC_SKELETONOBJECTS builtins. ISC licence,
 * see LICENSE.
 *
 * Bone matrices are 3x4, row-major: row i is (x_i, y_i, z_i, t_i) where x,
 * y and z are the bone's forward, left and up axes and t its origin. QC sees
 * forward, right (= -left) and up. */
#ifndef BW_SKEL_H
#define BW_SKEL_H

#include "common.h"

typedef float bonemat_t[12];

typedef struct {
	int first, count; /* range of raw frames */
	float fps;
	bool loop;
	char name[32];
} framegroup_t;

typedef struct {
	char name[64];
	int parent; /* -1 for a root */
} skeljoint_t;

typedef struct skelmodel_s {
	char name[128];
	int numjoints;
	skeljoint_t *joints;
	float *base;          /* numjoints x 10: translate, rotate (xyzw), scale */
	bonemat_t *baserel;   /* base as relative matrices, for the bind pose */
	bonemat_t *invbind;   /* inverse of the bind pose, absolute */
	int numframes;
	float *frames;        /* numframes x numjoints x 10, as base; NULL when
	                         they are decoded from `studio` on demand */
	/* low-memory builds keep a studio model's animation in its run-length
	   coded form and decode the frames a pose needs: frame f of the whole
	   model is frame f - seqfirst[s] of sequence s */
	const struct studio_model_s *studio;
	int *seqfirst;        /* numseqs + 1 entries */
	int numseqs;
	framegroup_t *anims;
	int numanims;
	struct skelmodel_s *next;
} skelmodel_t;

/* "first count fps loop // name" per line, from <model>.framegroups */
framegroup_t *Skel_LoadFramegroups(const char *model, int *count);

/* parses the skeleton and animations of an IQM file; NULL for other
 * formats or models without joints. Cached by name. */
skelmodel_t *Skel_LoadModel(const char *name);
void Skel_FreeModels(void);

int Skel_FindJoint(const skelmodel_t *m, const char *name); /* 0-based, -1 if none */

/* relative bone matrices of animation `group` at `time` seconds into it;
 * group < 0 or a model without frames gives the bind pose */
void Skel_SampleRelative(const skelmodel_t *m, int group, float time, bonemat_t *out);
/* frame/frame2 blended by lerpfrac, as an entity's fields describe */
void Skel_EntityPose(const skelmodel_t *m, int frame, float frame1time, int frame2, float frame2time,
                     float lerpfrac, bonemat_t *out);
/* the same for bones first..last alone (0-based, inclusive); the others
 * in out are left as they were */
void Skel_EntityPoseRange(const skelmodel_t *m, int frame, float frame1time, int frame2, float frame2time,
                          float lerpfrac, int first, int last, bonemat_t *out);
/* relative to absolute (model space) */
void Skel_ToAbsolute(const skelmodel_t *m, const bonemat_t *rel, bonemat_t *abs);

/* matrix helpers */
void Bone_Identity(bonemat_t m);
void Bone_FromTRS(bonemat_t m, const float *trs10);
void Bone_Multiply(bonemat_t out, const bonemat_t a, const bonemat_t b); /* out = a * b */
void Bone_Invert(bonemat_t out, const bonemat_t m);
void Bone_FromVectors(bonemat_t m, const float *org, const float *fwd, const float *right, const float *up);
void Bone_ToVectors(const bonemat_t m, float *org, float *fwd, float *right, float *up);
void Bone_Transform(const bonemat_t m, const float *in, float *out);

#endif
