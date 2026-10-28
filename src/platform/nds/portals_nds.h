/* Bulwark engine - portals between a map's leaves (portals_nds.c). ISC licence, see LICENSE. */
#ifndef BW_PORTALS_NDS_H
#define BW_PORTALS_NDS_H
#include <stdbool.h>
#include <stdint.h>

#define PT_SOLID (-2) /* Quake's CONTENTS_SOLID */

typedef struct {
	float n[3], dist; /* map units */
} pt_plane_t;

typedef struct {
	int plane;
	int children[2]; /* >= 0 node, < 0 leaf -(c + 1) */
} pt_node_t;

typedef struct {
	int16_t leaf[2]; /* leaf[0] is on the plane's front */
	int plane;
	int first;   /* into points */
	uint8_t num;
} pt_portal_t;

typedef struct {
	pt_portal_t *portals;
	int numportals;
	int16_t (*points)[3]; /* 1/8 map units */
	int numpoints;
	int *leaffirst;       /* leaf l's portals: leafportals[leaffirst[l] .. leaffirst[l + 1]) */
	int *leafportals;
	int numleafs;
	bool overflow;        /* ran out of room: some portals are missing, don't trust them */
} pt_portals_t;

bool Pt_Build(pt_portals_t *p, const pt_plane_t *planes, const pt_node_t *nodes, int numnodes, const int *contents,
              int numleafs, const float *mins, const float *maxs);
void Pt_Free(pt_portals_t *p);

#endif
