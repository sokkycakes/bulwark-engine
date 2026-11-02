/* Bulwark Source formats plugin - studio model skeletons, shared between
 * mdl.c and mdlskel.c. ISC licence, see LICENSE. */
#ifndef BW_MDLSKEL_H
#define BW_MDLSKEL_H

#include "source.h"

typedef struct {
	const unsigned char *d;
	size_t len;
} blob_t;

static inline int in(const blob_t *b, size_t ofs, size_t n) { return ofs <= b->len && n <= b->len - ofs; }

/* a NUL-terminated string at ofs, or "" */
const char *str_at(const blob_t *b, size_t ofs);

typedef struct {
	char name[64];
	int parent;
	float pos[3], quat[4], rot[3], posscale[3], rotscale[3];
	float posetobone[12]; /* model space to bone space at bind, 3x4 rows */
} sbone_t;

/* boxes in bone space; colour is a hit group (0 to 7) or BOXCOLOR_BONE */
#define BOXCOLOR_BONE 8
typedef struct {
	int bone, color;
	float mins[3], maxs[3];
} sbox_t;
typedef struct {
	sbox_t *b;
	int num, cap, numhitboxes;
} boxes_t;

typedef struct {
	char name[96];
	int first, count, loop;
	float fps;
} sanim_t;
typedef struct {
	sanim_t *a;
	int num, cap;
	float *frames; /* numframes x joints x 10 */
	int numframes, capframes;
	int unsupported;
} anims_t;

void mat_from_pq(float m[12], const float p[3], const float q[4]);
void mat_mul(float o[12], const float a[12], const float b[12]);
void mat_point(const float m[12], const float p[3], float o[3]);
void mat_dir(const float m[12], const float p[3], float o[3]);

int mdl_bones(const blob_t *m, sbone_t **out);
void mdl_restpose(const sbone_t *b, int n, float (*abs)[12]);
void mdl_boxes(const blob_t *m, const sbone_t *b, int nb, boxes_t *bx);
void mdl_anims(const char *path, const blob_t *m, const sbone_t *b, int nb, anims_t *a);

#endif
