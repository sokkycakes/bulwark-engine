/* Bulwark engine - GoldSrc-era studio models (IDST version 10) and the
 * appended sequence files (.seq) that Counter-Strike Online ships next to
 * them. The layouts are written up in docs/spec/studio-mdl.md, where each
 * one says how it was established. ISC licence, see LICENSE.
 *
 * Everything is parsed from a byte buffer with bounds checks; a model that
 * fails a check is rejected with a reason instead of being read past its
 * end. The buffer must outlive the model, because animation data is read
 * from it in place. */
#ifndef BW_STUDIO_H
#define BW_STUDIO_H

#include "common.h"

#define STUDIO_MAX_BONES 256
#define STUDIO_MAX_BLENDS 16

typedef struct {
	char name[33];
	int parent;          /* -1 for a root */
	int controller[6];   /* -1 if none */
	float value[6];      /* rest position (xyz) and rotation (euler, radians) */
	float scale[6];      /* animation value scale per channel */
} studio_bone_t;

typedef struct {
	int bone, group;
	float mins[3], maxs[3];
} studio_hitbox_t;

typedef struct {
	char name[33];
	int bone;
	float org[3];
} studio_attachment_t;

typedef struct {
	int frame, event, type;
	char options[65];
} studio_event_t;

typedef struct {
	char label[33];
	float fps;
	int flags, activity, actweight;
	int numframes;
	int motiontype, motionbone;
	float linearmovement[3];
	float mins[3], maxs[3];
	int numblends;
	int blendtype[2];
	float blendstart[2], blendend[2];
	int seqgroup;
	int entrynode, exitnode, nodeflags, nextseq;
	int numevents;
	studio_event_t *events;
	/* animation data: numblends x numbones blocks of six 16-bit channel
	 * offsets, followed by run-length coded values; read in place */
	const uint8_t *anim;
	const uint8_t *animlimit; /* first byte past the region it may use */
	int source;               /* 0 = the model, n = the n-th appended .seq */
} studio_seq_t;

typedef struct {
	char name[65];
	int flags, width, height;
	const uint8_t *pixels;  /* width x height palette indices */
	const uint8_t *palette; /* 256 x RGB */
} studio_texture_t;

typedef struct {
	int numtris_cmds; /* number of triangle-command lists */
	int skinref;
	const uint8_t *cmds, *cmdslimit;
} studio_mesh_t;

typedef struct {
	char name[65];
	int numverts, numnorms, nummeshes;
	const uint8_t *vertbone; /* numverts bone indices */
	const uint8_t *verts;    /* numverts x 3 floats */
	const uint8_t *normbone;
	const uint8_t *norms;
	studio_mesh_t *meshes;
} studio_submodel_t;

typedef struct {
	char name[65];
	int base, nummodels;
	studio_submodel_t *models;
} studio_bodypart_t;

typedef struct studio_model_s {
	char name[65];
	int version;
	int flags;
	float eye[3], mins[3], maxs[3], bbmins[3], bbmaxs[3];
	int numbones;
	studio_bone_t *bones;
	int numhitboxes;
	studio_hitbox_t *hitboxes;
	int numseqs;
	studio_seq_t *seqs;
	int numseqgroups;
	int numtextures;
	studio_texture_t *textures;
	int numskinref, numskinfamilies;
	int16_t *skins; /* numskinfamilies x numskinref */
	int numbodyparts;
	studio_bodypart_t *bodyparts;
	int numattachments;
	studio_attachment_t *attachments;
	int numappended; /* .seq files merged so far */
} studio_model_t;

/* parses an IDST v10 model; returns false with a reason in err */
bool Studio_Parse(studio_model_t *m, const uint8_t *buf, size_t len, char *err, size_t errlen);
/* appends the sequences of a CSO .seq file; the model's earlier sequences
 * are kept and the new ones follow them in file order */
bool Studio_AppendSeq(studio_model_t *m, const uint8_t *buf, size_t len, char *err, size_t errlen);
/* the texture and skin tables of a model's companion file (nameT.mdl),
 * in place of the model's own; the buffer must outlive the model */
bool Studio_ParseTextures(studio_model_t *m, const uint8_t *buf, size_t len, char *err, size_t errlen);
/* the bones' rest positions, as Studio_DecodeFrame writes a frame */
void Studio_RestPose(const studio_model_t *m, float *out);
/* walks every run of every channel of a sequence for all its frames */
bool Studio_ValidateSeq(const studio_model_t *m, int seq, char *err, size_t errlen);
/* bone transforms of one frame of one blend: out is numbones x 10 floats
 * (translate, rotate xyzw, scale), relative to each bone's parent */
bool Studio_DecodeFrame(const studio_model_t *m, int seq, int blend, int frame, float *out);
/* number of triangles a mesh's command lists describe, or -1 if malformed */
int Studio_CountTris(const studio_model_t *m, const studio_mesh_t *mesh, int numverts, int numnorms);
void Studio_Free(studio_model_t *m);

/* sequence flag: the sequence loops (public descriptions of the format;
 * also set on looping CSO shop sequences, see studio-mdl.md) */
#define STUDIO_LOOPING 0x0001

/* a model loaded through the file system and kept for the rest of the run,
 * by name; NULL if the file is missing or isn't an IDST v10 model. Failures
 * are remembered too, so asking again is cheap. */
const studio_model_t *Studio_Cached(const char *name);
#ifdef BW_LOWMEM
/* the pixels of a cached model's textures are only read to upload them,
 * so low-memory builds cut them off the cached file (the textures' sizes,
 * names and flags stay): Studio_LoadPixels reads them back for an upload
 * and Studio_DropPixels lets them go again. Studio_LoadPixels is false,
 * and the pixels NULL, if they cannot be had. */
void Studio_DropPixels(const char *name);
bool Studio_LoadPixels(const char *name);
#endif
/* length of a sequence in seconds (0 if it has no frames or no rate) */
float Studio_SeqDuration(const studio_model_t *m, int seq);
/* a sequence tagged with the activity, picked at random by actweight;
 * rnd is any random number. -1 if no sequence has that activity. */
int Studio_SeqForActivity(const studio_model_t *m, int activity, unsigned rnd);

#endif
