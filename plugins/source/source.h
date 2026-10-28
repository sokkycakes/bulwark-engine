/* Bulwark Source formats plugin - shared helpers. ISC licence, see LICENSE. */
#ifndef BW_SOURCE_H
#define BW_SOURCE_H

#include "../../src/common/bwplug.h"

#include <stddef.h>
#include <stdint.h>

extern const bwplug_api_t *api;

void srcprint(const char *fmt, ...);

static inline unsigned rd16(const unsigned char *p) { return (unsigned)(p[0] | (p[1] << 8)); }
static inline unsigned rd32(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static inline int rds32(const unsigned char *p) { return (int)rd32(p); }
static inline float rdf(const unsigned char *p)
{
	union {
		unsigned u;
		float f;
	} c;
	c.u = rd32(p);
	return c.f;
}

/* a game path: lower case, forward slashes, no leading slash */
void src_cleanpath(char *s);

unsigned char *VTF_Decode(const void *data, size_t len, int *width, int *height);
int VMT_Load(const char *path, const void *data, size_t len, bwplug_material_t *out);
void *MDL_Convert(const char *path, const void *data, size_t len, size_t *outlen);

#endif
