/* Bulwark engine - Source BSP (VBSP) container: the lump directory, LZMA
 * compressed lumps and game lumps. The record layouts that the server and
 * renderer read out of the lumps are in docs/spec/vbsp.md.
 * ISC licence, see LICENSE. */
#ifndef BW_VBSP_H
#define BW_VBSP_H

#include "common.h"

enum {
	VL_ENTITIES = 0, VL_PLANES = 1, VL_TEXDATA = 2, VL_VERTEXES = 3, VL_VISIBILITY = 4, VL_NODES = 5,
	VL_TEXINFO = 6, VL_FACES = 7, VL_LIGHTING = 8, VL_LEAFS = 10, VL_EDGES = 12, VL_SURFEDGES = 13,
	VL_MODELS = 14, VL_LEAFFACES = 16, VL_LEAFBRUSHES = 17, VL_BRUSHES = 18, VL_BRUSHSIDES = 19,
	VL_DISPINFO = 26, VL_PHYSCOLLIDE = 29, VL_DISP_VERTS = 33, VL_GAME_LUMP = 35, VL_PAKFILE = 40,
	VL_TEXDATA_STRING_DATA = 43, VL_TEXDATA_STRING_TABLE = 44, VL_DISP_TRIS = 48, VL_LIGHTING_HDR = 53,
	VL_FACES_HDR = 58, VL_NUMLUMPS = 64
};

/* Source content and surface bits that the loaders look at */
#define SRC_CONTENTS_SOLID 0x1
#define SRC_CONTENTS_WINDOW 0x2
#define SRC_CONTENTS_GRATE 0x8
#define SRC_CONTENTS_SLIME 0x10
#define SRC_CONTENTS_WATER 0x20
#define SRC_CONTENTS_PLAYERCLIP 0x10000
#define SRC_CONTENTS_MONSTERCLIP 0x20000
#define SRC_CONTENTS_LADDER 0x20000000

#define SRC_SURF_LIGHT 0x1
#define SRC_SURF_SKY2D 0x2
#define SRC_SURF_SKY 0x4
#define SRC_SURF_WARP 0x8
#define SRC_SURF_TRANS 0x10
#define SRC_SURF_TRIGGER 0x40
#define SRC_SURF_NODRAW 0x80
#define SRC_SURF_HINT 0x100
#define SRC_SURF_SKIP 0x200
#define SRC_SURF_NOLIGHT 0x400
#define SRC_SURF_BUMPLIGHT 0x800

typedef struct {
	const uint8_t *file;
	size_t size;
	int version;
	struct {
		int32_t ofs, len, version, fourcc;
	} lumps[VL_NUMLUMPS];
} vbsp_t;

/* true when the data is a VBSP file; fills the directory */
bool VBSP_Open(vbsp_t *v, const uint8_t *data, size_t size);
/* a lump's bytes, decompressed if the file stores it LZMA compressed.
 * Mem_Free the result; NULL (len 0) when the lump is missing or broken */
uint8_t *VBSP_Lump(const vbsp_t *v, int lump, size_t *len);
/* a game lump by its id (for example "sprp" for static props) */
uint8_t *VBSP_GameLump(const vbsp_t *v, const char id[4], int *version, size_t *len);
/* decodes Valve's "LZMA" block: id, actual size, compressed size, 5 props */
uint8_t *VBSP_Unlzma(const uint8_t *src, size_t srclen, size_t *outlen);

/* the static prop dictionary and records, version 4 to 11 */
typedef struct {
	float origin[3], angles[3];
	int model; /* index into names */
	int solid; /* 0 none, 2 bounding box, 6 vphysics */
	int firstleaf, numleafs;
} vbsp_staticprop_t;

typedef struct {
	char (*names)[128];
	int numnames;
	vbsp_staticprop_t *props;
	int numprops;
	int version;
} vbsp_staticprops_t;

bool VBSP_StaticProps(const vbsp_t *v, vbsp_staticprops_t *out);
void VBSP_FreeStaticProps(vbsp_staticprops_t *sp);

#endif
