/* Bulwark engine - FTEW v1 compiled worlds. ISC licence, see LICENSE.
 * Written from the format notes in docs/spec/ftew.md. A parsed file points
 * into the caller's buffer, which must outlive it. */
#ifndef BW_FTEW_H
#define BW_FTEW_H

#include "common.h"

#define FTEW_HEADER_SIZE 96
#define FTEW_VERTEX_SIZE 36   /* xyz, st, rgba as floats */
#define FTEW_INSTANCE_SIZE 60

#define FTEW_INST_VISIBLE 1
#define FTEW_INST_COLLIDE 2

typedef struct {
	uint32_t material;
	uint32_t numverts, numindexes;
	const uint8_t *verts, *indexes;
} ftew_mesh_t;

typedef struct {
	uint32_t mesh, model, flags;
	float axis[3][3]; /* basis columns */
	float origin[3];
} ftew_instance_t;

typedef struct {
	uint32_t model, contents;
	float mins[3], maxs[3];
	uint32_t numplanes;
	const uint8_t *planes; /* normal xyz, dist; inside where dot(n, p) <= dist */
} ftew_collider_t;

typedef struct {
	uint32_t nummodels;
	int nummaterials;
	char (*materials)[64];
	int nummeshes;
	ftew_mesh_t *meshes;
	int numinstances;
	ftew_instance_t *instances;
	int numcolliders;
	ftew_collider_t *colliders;
	const char *entities; /* NUL-terminated entity lump */
} ftew_t;

bool FTEW_IsFTEW(const void *data, size_t size);
/* validates the whole file; false with a reason in err on any problem */
bool FTEW_Parse(const void *data, size_t size, ftew_t *out, char *err, size_t errsize);
void FTEW_Free(ftew_t *w);

void FTEW_Vertex(const ftew_mesh_t *m, uint32_t i, float xyz[3], float st[2], float rgba[4]);
uint32_t FTEW_Index(const ftew_mesh_t *m, uint32_t i);
void FTEW_Plane(const ftew_collider_t *c, uint32_t i, float normal[3], float *dist);
void FTEW_Transform(const ftew_instance_t *in, const float p[3], float out[3]);
/* true when the instance mirrors its mesh, so triangle order flips */
bool FTEW_Mirrored(const ftew_instance_t *in);

#endif
