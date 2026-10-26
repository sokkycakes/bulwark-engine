/* Bulwark engine - heightmap terrain maps.
 *
 * A terrain map is maps/<name>.hmp: an entity list whose worldspawn
 * describes the terrain (_segmentsize, the _min/_max segment bounds, the
 * default ground and water heights and textures, a _seed). Heights come
 * from section files maps/<name>/sect_<x>_<y>.bwh where they exist, from
 * seeded noise where there is a _seed, and are flat otherwise.
 *
 * Bulwark draws and collides with Q3 BSPs, so loading a terrain map turns
 * it into maps/<name>.bsp: two triangular prism brushes for each height
 * cell, one vertex-lit triangle soup for each block of 8x8 cells, a water
 * brush and surface where the water lies above the lowest ground, and a
 * node tree over the blocks. The generated file is marked in its
 * worldspawn ("_terrain_generated") and rebuilt whenever the map loads;
 * an ordinary .bsp of the same name is left alone.
 *
 * Section files (version 1): "BWH1", int32 version, int32 section x and y,
 * then 17x17 little-endian float heights, row by row along x.
 * ISC licence, see LICENSE. */
#include "common.h"
#include "cmd.h"
#include "cvar.h"
#include "fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECT_CELLS 16
#define SECT_VERTS (SECT_CELLS + 1)
#define BLOCK 8
#define MAX_KEYS 64

typedef struct {
	char name[64];
	char hmp[64];
	strbuf_t ents;        /* the entity lump, quoted and without comments */
	float segsize, cell;
	int sx0, sy0, nsx, nsy; /* sections */
	int nx, ny;           /* cells */
	float x0, y0;
	float ground, water;
	char groundtex[64], watertex[64], seed[64];
	float *h;             /* (nx+1)*(ny+1) */
	bool *fromfile;       /* per section: loaded from a section file */
	uint32_t hash;
} terrain_t;

static terrain_t ter;
static bool ter_loaded;
static cvar_t *ter_shadows, *ter_shadowdist, *ter_ambient, *ter_sundir;

/* --- the .hmp ------------------------------------------------------------ */

typedef struct {
	char key[64], value[256];
} kv_t;

static const char *kv_get(const kv_t *kv, int n, const char *key, const char *def)
{
	for (int i = 0; i < n; i++)
		if (!Str_ICmp(kv[i].key, key))
			return kv[i].value;
	return def;
}

static uint32_t hash_str(const char *s, size_t n)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++)
		h = (h ^ (uint8_t)s[i]) * 16777619u;
	return h;
}

/* parses the entity list into ter.ents and the worldspawn's settings */
static bool parse_hmp(const char *text)
{
	const char *p = text;
	char tok[256];
	kv_t world[MAX_KEYS];
	int nworld = 0, ent = 0;
	SB_Free(&ter.ents);
	while ((p = COM_Parse(p, tok, sizeof(tok)))) {
		kv_t cur[MAX_KEYS];
		int n = 0;
		if (strcmp(tok, "{"))
			return false;
		for (;;) {
			char key[64];
			if (!(p = COM_Parse(p, tok, sizeof(tok))))
				return false;
			if (!strcmp(tok, "}"))
				break;
			Str_Copy(key, tok, sizeof(key));
			if (!(p = COM_Parse(p, tok, sizeof(tok))) || !strcmp(tok, "}"))
				return false;
			if (n < MAX_KEYS) {
				Str_Copy(cur[n].key, key, sizeof(cur[n].key));
				Str_Copy(cur[n].value, tok, sizeof(cur[n].value));
				n++;
			}
		}
		SB_AppendStr(&ter.ents, "{\n");
		for (int i = 0; i < n; i++)
			SB_Printf(&ter.ents, "\"%s\" \"%s\"\n", cur[i].key, cur[i].value);
		if (!ent++) {
			memcpy(world, cur, sizeof(kv_t) * n);
			nworld = n;
			SB_AppendStr(&ter.ents, "\"_terrain_generated\" \"1\"\n");
		}
		SB_AppendStr(&ter.ents, "}\n");
	}
	if (!ent)
		return false;
	ter.segsize = (float)atof(kv_get(world, nworld, "_segmentsize", "1024"));
	if (ter.segsize < 64)
		ter.segsize = 64;
	ter.cell = ter.segsize / SECT_CELLS;
	ter.sx0 = (int)floorf((float)atof(kv_get(world, nworld, "_minxsegment", "-2048")) / ter.segsize);
	ter.sy0 = (int)floorf((float)atof(kv_get(world, nworld, "_minysegment", "-2048")) / ter.segsize);
	ter.nsx = (int)ceilf((float)atof(kv_get(world, nworld, "_maxxsegment", "2048")) / ter.segsize) - ter.sx0;
	ter.nsy = (int)ceilf((float)atof(kv_get(world, nworld, "_maxysegment", "2048")) / ter.segsize) - ter.sy0;
	ter.nsx = BW_MAX(1, BW_MIN(ter.nsx, 64));
	ter.nsy = BW_MAX(1, BW_MIN(ter.nsy, 64));
	ter.nx = ter.nsx * SECT_CELLS;
	ter.ny = ter.nsy * SECT_CELLS;
	ter.x0 = ter.sx0 * ter.segsize;
	ter.y0 = ter.sy0 * ter.segsize;
	ter.ground = (float)atof(kv_get(world, nworld, "_defaultgroundheight", "0"));
	ter.water = (float)atof(kv_get(world, nworld, "_defaultwaterheight", "-99999"));
	Str_Copy(ter.groundtex, kv_get(world, nworld, "_defaultgroundtexture", "ground1_1"), sizeof(ter.groundtex));
	Str_Copy(ter.watertex, kv_get(world, nworld, "_defaultwatertexture", ""), sizeof(ter.watertex));
	Str_Copy(ter.seed, kv_get(world, nworld, "_seed", ""), sizeof(ter.seed));
	return true;
}

/* --- heights ------------------------------------------------------------- */

static float lattice(uint32_t seed, int x, int y)
{
	uint32_t h = seed ^ ((uint32_t)x * 374761393u) ^ ((uint32_t)y * 668265263u);
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return (float)(h & 0xffff) / 65535.0f * 2 - 1;
}

static float smooth_noise(uint32_t seed, float x, float y)
{
	int ix = (int)floorf(x), iy = (int)floorf(y);
	float fx = x - ix, fy = y - iy;
	float a = lattice(seed, ix, iy), b = lattice(seed, ix + 1, iy);
	float c = lattice(seed, ix, iy + 1), d = lattice(seed, ix + 1, iy + 1);
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

static float noise_height(float x, float y)
{
	uint32_t seed = hash_str(ter.seed, strlen(ter.seed));
	float h = 0, amp = 384, freq = 1.0f / 2048;
	for (int o = 0; o < 5; o++) {
		h += smooth_noise(seed + (uint32_t)o * 977u, x * freq, y * freq) * amp;
		amp *= 0.45f;
		freq *= 2.1f;
	}
	return ter.ground + h;
}

#define H(x, y) ter.h[(y) * (ter.nx + 1) + (x)]

static void sect_path(int sx, int sy, char *out, size_t size)
{
	snprintf(out, size, "maps/%s/sect_%d_%d.bwh", ter.name, sx, sy);
}

static void load_heights(void)
{
	Mem_Free(ter.h);
	Mem_Free(ter.fromfile);
	ter.h = Mem_Alloc(sizeof(float) * (ter.nx + 1) * (ter.ny + 1));
	ter.fromfile = Mem_Alloc(sizeof(bool) * ter.nsx * ter.nsy);
	for (int y = 0; y <= ter.ny; y++)
		for (int x = 0; x <= ter.nx; x++)
			H(x, y) = ter.seed[0] ? noise_height(ter.x0 + x * ter.cell, ter.y0 + y * ter.cell) : ter.ground;
	for (int sy = 0; sy < ter.nsy; sy++)
		for (int sx = 0; sx < ter.nsx; sx++) {
			char path[160];
			size_t len;
			uint8_t *d;
			sect_path(ter.sx0 + sx, ter.sy0 + sy, path, sizeof(path));
			if (!(d = (uint8_t *)FS_LoadFile(path, &len)))
				continue;
			if (len >= 16 + SECT_VERTS * SECT_VERTS * 4 && !memcmp(d, "BWH1", 4)) {
				const uint8_t *p = d + 16;
				for (int y = 0; y < SECT_VERTS; y++)
					for (int x = 0; x < SECT_VERTS; x++, p += 4) {
						float f;
						uint32_t u = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
						memcpy(&f, &u, 4);
						H(sx * SECT_CELLS + x, sy * SECT_CELLS + y) = f;
					}
				ter.fromfile[sy * ter.nsx + sx] = true;
				ter.hash = ter.hash * 31 + hash_str((const char *)d, len);
			} else {
				Con_Printf("%s: not a terrain section\n", path);
			}
			Mem_Free(d);
		}
}

/* --- building the BSP ---------------------------------------------------- */

typedef struct {
	strbuf_t planes, nodes, leafs, leaffaces, leafbrushes, brushes, sides, verts, indexes, surfs;
	int numplanes, numnodes, numleafs, numleaffaces, numleafbrushes, numbrushes, numsides, numverts, numindexes,
	    numsurfs;
	int *blocksurf;  /* per block: its surface */
	int *cellbrush;  /* per cell: its first brush (two each) */
	int waterbrush, watersurf;
} build_t;

static void put32(strbuf_t *sb, int32_t v)
{
	uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)((uint32_t)v >> 24)};
	SB_Append(sb, (const char *)b, 4);
}

static void putf(strbuf_t *sb, float f)
{
	int32_t v;
	memcpy(&v, &f, 4);
	put32(sb, v);
}

static int add_plane(build_t *b, float nx, float ny, float nz, float dist)
{
	putf(&b->planes, nx);
	putf(&b->planes, ny);
	putf(&b->planes, nz);
	putf(&b->planes, dist);
	return b->numplanes++;
}

static void add_side(build_t *b, int plane, int shader)
{
	put32(&b->sides, plane);
	put32(&b->sides, shader);
	b->numsides++;
}

/* a prism under the triangle a, b, c, whose third axis is the diagonal */
static void add_prism(build_t *b, const float *a, const float *pb, const float *c, float diag[4], float bottom)
{
	float e1[3], e2[3], n[3], minx, maxx, miny, maxy, top;
	int first = b->numsides;
	for (int k = 0; k < 3; k++) {
		e1[k] = pb[k] - a[k];
		e2[k] = c[k] - a[k];
	}
	CrossProduct(e1, e2, n);
	if (n[2] < 0)
		n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
	VectorNormalize(n);
	minx = BW_MIN(a[0], BW_MIN(pb[0], c[0]));
	maxx = BW_MAX(a[0], BW_MAX(pb[0], c[0]));
	miny = BW_MIN(a[1], BW_MIN(pb[1], c[1]));
	maxy = BW_MAX(a[1], BW_MAX(pb[1], c[1]));
	top = BW_MAX(a[2], BW_MAX(pb[2], c[2]));
	add_side(b, add_plane(b, 1, 0, 0, maxx), 0);
	add_side(b, add_plane(b, -1, 0, 0, -minx), 0);
	add_side(b, add_plane(b, 0, 1, 0, maxy), 0);
	add_side(b, add_plane(b, 0, -1, 0, -miny), 0);
	add_side(b, add_plane(b, 0, 0, 1, top), 0);
	add_side(b, add_plane(b, 0, 0, -1, -bottom), 0);
	add_side(b, add_plane(b, diag[0], diag[1], diag[2], diag[3]), 0);
	add_side(b, add_plane(b, n[0], n[1], n[2], DotProduct(n, a)), 0);
	put32(&b->brushes, first);
	put32(&b->brushes, b->numsides - first);
	put32(&b->brushes, 0);
	b->numbrushes++;
}

static void vert_normal(int x, int y, float *n)
{
	int x0 = BW_MAX(x - 1, 0), x1 = BW_MIN(x + 1, ter.nx), y0 = BW_MAX(y - 1, 0), y1 = BW_MIN(y + 1, ter.ny);
	n[0] = -(H(x1, y) - H(x0, y)) / ((x1 - x0) * ter.cell);
	n[1] = -(H(x, y1) - H(x, y0)) / ((y1 - y0) * ter.cell);
	n[2] = 1;
	VectorNormalize(n);
}

/* walks the heightfield towards the sun from a vertex */
static bool in_shadow(int x, int y, const float *sun)
{
	float p[3] = {ter.x0 + x * ter.cell, ter.y0 + y * ter.cell, H(x, y) + 1};
	float step = ter.cell * 0.5f, dist = ter_shadowdist ? ter_shadowdist->value : 2048;
	for (float t = step; t < dist; t += step) {
		float q[3] = {p[0] + sun[0] * t, p[1] + sun[1] * t, p[2] + sun[2] * t};
		float fx = (q[0] - ter.x0) / ter.cell, fy = (q[1] - ter.y0) / ter.cell;
		int ix = (int)fx, iy = (int)fy;
		if (fx < 0 || fy < 0 || ix >= ter.nx || iy >= ter.ny)
			return false;
		if (q[2] < H(ix, iy) && q[2] < H(ix + 1, iy + 1))
			return true;
	}
	return false;
}

static void add_vert(build_t *b, float x, float y, float z, const float *n, const uint8_t *rgba)
{
	putf(&b->verts, x);
	putf(&b->verts, y);
	putf(&b->verts, z);
	putf(&b->verts, x / 256.0f);
	putf(&b->verts, -y / 256.0f);
	putf(&b->verts, 0);
	putf(&b->verts, 0);
	for (int k = 0; k < 3; k++)
		putf(&b->verts, n[k]);
	SB_Append(&b->verts, (const char *)rgba, 4);
	b->numverts++;
}

static void add_surface(build_t *b, int shader, int type, int firstvert, int numverts, int firstindex, int numindexes,
                        const float *normal)
{
	put32(&b->surfs, shader);
	put32(&b->surfs, -1);
	put32(&b->surfs, type);
	put32(&b->surfs, firstvert);
	put32(&b->surfs, numverts);
	put32(&b->surfs, firstindex);
	put32(&b->surfs, numindexes);
	put32(&b->surfs, -1);
	for (int k = 0; k < 4; k++)
		put32(&b->surfs, 0);
	for (int k = 0; k < 9; k++)
		putf(&b->surfs, 0);
	for (int k = 0; k < 3; k++)
		putf(&b->surfs, normal[k]);
	put32(&b->surfs, 0);
	put32(&b->surfs, 0);
	b->numsurfs++;
}

/* one triangle soup for the cells of a block */
static int block_surface(build_t *b, int bx, int by, const float *sun, float ambient)
{
	int cx0 = bx * BLOCK, cy0 = by * BLOCK, cx1 = BW_MIN(cx0 + BLOCK, ter.nx), cy1 = BW_MIN(cy0 + BLOCK, ter.ny);
	int w = cx1 - cx0 + 1, fv = b->numverts, fi = b->numindexes;
	bool shadows = ter_shadows && ter_shadows->ivalue;
	float up[3] = {0, 0, 1};
	for (int y = cy0; y <= cy1; y++)
		for (int x = cx0; x <= cx1; x++) {
			float n[3];
			uint8_t rgba[4];
			float l;
			vert_normal(x, y, n);
			l = DotProduct(n, sun);
			if (l < 0 || (shadows && in_shadow(x, y, sun)))
				l = 0;
			l = ambient + (1 - ambient) * l;
			rgba[0] = rgba[1] = rgba[2] = (uint8_t)BW_MIN(255.0f, l * 255.0f);
			rgba[3] = 255;
			add_vert(b, ter.x0 + x * ter.cell, ter.y0 + y * ter.cell, H(x, y), n, rgba);
		}
	/* Q3 winding: clockwise seen from above */
	for (int y = 0; y < cy1 - cy0; y++)
		for (int x = 0; x < cx1 - cx0; x++) {
			int i00 = y * w + x, i10 = i00 + 1, i01 = i00 + w, i11 = i01 + 1;
			int tri[6] = {i00, i11, i10, i00, i01, i11};
			for (int k = 0; k < 6; k++)
				put32(&b->indexes, tri[k]);
			b->numindexes += 6;
		}
	add_surface(b, 0, 3, fv, b->numverts - fv, fi, b->numindexes - fi, up);
	return b->numsurfs - 1;
}

static void block_bounds(int bx0, int by0, int bx1, int by1, float *mins, float *maxs)
{
	int cx0 = bx0 * BLOCK, cy0 = by0 * BLOCK, cx1 = BW_MIN(bx1 * BLOCK, ter.nx), cy1 = BW_MIN(by1 * BLOCK, ter.ny);
	mins[0] = ter.x0 + cx0 * ter.cell;
	mins[1] = ter.y0 + cy0 * ter.cell;
	maxs[0] = ter.x0 + cx1 * ter.cell;
	maxs[1] = ter.y0 + cy1 * ter.cell;
	mins[2] = 1e30f;
	maxs[2] = -1e30f;
	for (int y = cy0; y <= cy1; y++)
		for (int x = cx0; x <= cx1; x++) {
			mins[2] = BW_MIN(mins[2], H(x, y) - 64);
			maxs[2] = BW_MAX(maxs[2], H(x, y));
		}
	if (ter.watertex[0] && ter.water > mins[2])
		maxs[2] = BW_MAX(maxs[2], ter.water);
	maxs[2] += 1024; /* room above the ground still belongs to this leaf */
}

static void put_bounds(strbuf_t *sb, const float *mins, const float *maxs)
{
	for (int k = 0; k < 3; k++)
		put32(sb, (int32_t)floorf(mins[k]));
	for (int k = 0; k < 3; k++)
		put32(sb, (int32_t)ceilf(maxs[k]));
}

/* returns the node or leaf (as -(leaf + 1)) for the blocks in [bx0,bx1) x [by0,by1) */
static int build_tree(build_t *b, int bx0, int by0, int bx1, int by1)
{
	float mins[3], maxs[3];
	block_bounds(bx0, by0, bx1, by1, mins, maxs);
	if (bx1 - bx0 == 1 && by1 - by0 == 1) {
		int cx0 = bx0 * BLOCK, cy0 = by0 * BLOCK, cx1 = BW_MIN(cx0 + BLOCK, ter.nx), cy1 = BW_MIN(cy0 + BLOCK, ter.ny);
		int ff = b->numleaffaces, fb = b->numleafbrushes;
		put32(&b->leaffaces, b->blocksurf[by0 * ((ter.nx + BLOCK - 1) / BLOCK) + bx0]);
		b->numleaffaces++;
		if (b->watersurf >= 0) {
			put32(&b->leaffaces, b->watersurf);
			b->numleaffaces++;
		}
		for (int y = cy0; y < cy1; y++)
			for (int x = cx0; x < cx1; x++)
				for (int k = 0; k < 2; k++) {
					put32(&b->leafbrushes, b->cellbrush[y * ter.nx + x] + k);
					b->numleafbrushes++;
				}
		if (b->waterbrush >= 0) {
			put32(&b->leafbrushes, b->waterbrush);
			b->numleafbrushes++;
		}
		put32(&b->leafs, 0); /* every leaf is the one cluster */
		put32(&b->leafs, 0);
		put_bounds(&b->leafs, mins, maxs);
		put32(&b->leafs, ff);
		put32(&b->leafs, b->numleaffaces - ff);
		put32(&b->leafs, fb);
		put32(&b->leafs, b->numleafbrushes - fb);
		return -(b->numleafs++ + 1);
	}
	{
		bool splitx = bx1 - bx0 >= by1 - by0;
		int mid = splitx ? (bx0 + bx1) / 2 : (by0 + by1) / 2;
		float d = splitx ? ter.x0 + mid * BLOCK * ter.cell : ter.y0 + mid * BLOCK * ter.cell;
		int plane = splitx ? add_plane(b, 1, 0, 0, d) : add_plane(b, 0, 1, 0, d);
		int node = b->numnodes++, front, back;
		size_t at = b->nodes.len;
		SB_Append(&b->nodes, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 36);
		front = splitx ? build_tree(b, mid, by0, bx1, by1) : build_tree(b, bx0, mid, bx1, by1);
		back = splitx ? build_tree(b, bx0, by0, mid, by1) : build_tree(b, bx0, by0, bx1, mid);
		{
			strbuf_t rec = {0};
			put32(&rec, plane);
			put32(&rec, front);
			put32(&rec, back);
			put_bounds(&rec, mins, maxs);
			memcpy(b->nodes.data + at, rec.data, 36);
			SB_Free(&rec);
		}
		return node;
	}
}

static void put_shader(strbuf_t *sb, const char *name, int surfflags, int contents)
{
	char n[64] = {0};
	Str_Copy(n, name, sizeof(n));
	SB_Append(sb, n, 64);
	put32(sb, surfflags);
	put32(sb, contents);
}

static bool write_bsp(const char *path)
{
	build_t b;
	strbuf_t shaders = {0}, models = {0}, vis = {0}, out = {0};
	float sun[3], ambient, mins[3], maxs[3], lowest = 1e30f, highest = -1e30f;
	int nbx = (ter.nx + BLOCK - 1) / BLOCK, nby = (ter.ny + BLOCK - 1) / BLOCK;
	char ospath[512];
	FILE *f;
	bool ok;

	memset(&b, 0, sizeof(b));
	b.waterbrush = b.watersurf = -1;
	sun[0] = 0.4f, sun[1] = 0.7f, sun[2] = 2;
	if (ter_sundir)
		sscanf(ter_sundir->string, "%f %f %f", &sun[0], &sun[1], &sun[2]);
	if (VectorNormalize(sun) == 0)
		sun[2] = 1;
	ambient = ter_ambient ? BW_MAX(0.0f, BW_MIN(1.0f, ter_ambient->value)) : 0.5f;
	for (int i = 0; i < (ter.nx + 1) * (ter.ny + 1); i++) {
		lowest = BW_MIN(lowest, ter.h[i]);
		highest = BW_MAX(highest, ter.h[i]);
	}

	put_shader(&shaders, va("textures/%s", ter.groundtex), 0, 1);
	put_shader(&shaders, va("textures/%s", ter.watertex), 0, 0x20);

	/* brushes: two prisms a cell, split along the diagonal from (x0,y0) to (x1,y1) */
	b.cellbrush = Mem_Alloc(sizeof(int) * ter.nx * ter.ny);
	for (int y = 0; y < ter.ny; y++)
		for (int x = 0; x < ter.nx; x++) {
			float x0 = ter.x0 + x * ter.cell, y0 = ter.y0 + y * ter.cell, x1 = x0 + ter.cell, y1 = y0 + ter.cell;
			float p00[3] = {x0, y0, H(x, y)}, p10[3] = {x1, y0, H(x + 1, y)};
			float p01[3] = {x0, y1, H(x, y + 1)}, p11[3] = {x1, y1, H(x + 1, y + 1)};
			float r = 0.70710678f;
			float da[4] = {-r, r, 0, (y0 - x0) * r}, db[4] = {r, -r, 0, (x0 - y0) * r};
			float bottom = BW_MIN(BW_MIN(p00[2], p10[2]), BW_MIN(p01[2], p11[2])) - 64;
			b.cellbrush[y * ter.nx + x] = b.numbrushes;
			add_prism(&b, p00, p10, p11, da, bottom);
			add_prism(&b, p00, p11, p01, db, bottom);
		}

	mins[0] = ter.x0, mins[1] = ter.y0, mins[2] = lowest - 64;
	maxs[0] = ter.x0 + ter.nx * ter.cell, maxs[1] = ter.y0 + ter.ny * ter.cell, maxs[2] = highest;

	if (ter.watertex[0] && ter.water > lowest) {
		/* one box of water over the whole map, and its surface */
		int first = b.numsides, fv = b.numverts, fi = b.numindexes;
		float up[3] = {0, 0, 1};
		uint8_t white[4] = {255, 255, 255, 255};
		int quad[6] = {0, 2, 1, 0, 3, 2};
		add_side(&b, add_plane(&b, 1, 0, 0, maxs[0]), 1);
		add_side(&b, add_plane(&b, -1, 0, 0, -mins[0]), 1);
		add_side(&b, add_plane(&b, 0, 1, 0, maxs[1]), 1);
		add_side(&b, add_plane(&b, 0, -1, 0, -mins[1]), 1);
		add_side(&b, add_plane(&b, 0, 0, 1, ter.water), 1);
		add_side(&b, add_plane(&b, 0, 0, -1, -mins[2]), 1);
		put32(&b.brushes, first);
		put32(&b.brushes, 6);
		put32(&b.brushes, 1);
		b.waterbrush = b.numbrushes++;
		add_vert(&b, mins[0], mins[1], ter.water, up, white);
		add_vert(&b, maxs[0], mins[1], ter.water, up, white);
		add_vert(&b, maxs[0], maxs[1], ter.water, up, white);
		add_vert(&b, mins[0], maxs[1], ter.water, up, white);
		for (int k = 0; k < 6; k++)
			put32(&b.indexes, quad[k]);
		b.numindexes += 6;
		add_surface(&b, 1, 3, fv, 4, fi, 6, up);
		b.watersurf = b.numsurfs - 1;
		maxs[2] = BW_MAX(maxs[2], ter.water);
	}

	b.blocksurf = Mem_Alloc(sizeof(int) * nbx * nby);
	for (int by = 0; by < nby; by++)
		for (int bx = 0; bx < nbx; bx++)
			b.blocksurf[by * nbx + bx] = block_surface(&b, bx, by, sun, ambient);

	build_tree(&b, 0, 0, nbx, nby);

	for (int k = 0; k < 3; k++)
		putf(&models, mins[k]);
	for (int k = 0; k < 3; k++)
		putf(&models, maxs[k] + 1024);
	put32(&models, 0);
	put32(&models, b.numsurfs);
	put32(&models, 0);
	put32(&models, b.numbrushes);

	put32(&vis, 1);
	put32(&vis, 1);
	SB_Append(&vis, "\1", 1);
	SB_Append(&vis, "\0\0\0", 3);

	{
		strbuf_t *lumps[17] = {&ter.ents, &shaders, &b.planes, &b.nodes, &b.leafs, &b.leaffaces, &b.leafbrushes,
		                       &models, &b.brushes, &b.sides, &b.verts, &b.indexes, NULL, &b.surfs, NULL, NULL, &vis};
		size_t ofs = 8 + 17 * 8;
		SB_Append(&out, "IBSP", 4);
		put32(&out, 46);
		for (int i = 0; i < 17; i++) {
			size_t len = lumps[i] ? lumps[i]->len + (i == 0) : 0;
			put32(&out, (int32_t)ofs);
			put32(&out, (int32_t)len);
			ofs += (len + 3) & ~(size_t)3;
		}
		for (int i = 0; i < 17; i++) {
			size_t len = lumps[i] ? lumps[i]->len : 0;
			if (len)
				SB_Append(&out, lumps[i]->data, len);
			if (i == 0) {
				SB_Append(&out, "", 1);
				len++;
			}
			while (len & 3) {
				SB_Append(&out, "", 1);
				len++;
			}
		}
	}

	ok = FS_WritePath(path, ospath, sizeof(ospath)) && FS_CreatePath(ospath) && (f = fopen(ospath, "wb"));
	if (ok) {
		ok = fwrite(out.data, 1, out.len, f) == out.len;
		fclose(f);
	}
	if (!ok)
		Con_Printf("couldn't write %s\n", path);
	else
		Con_DPrintf("%s: %d brushes, %d surfaces, %d leafs\n", path, b.numbrushes, b.numsurfs, b.numleafs);

	SB_Free(&shaders);
	SB_Free(&models);
	SB_Free(&vis);
	SB_Free(&out);
	SB_Free(&b.planes);
	SB_Free(&b.nodes);
	SB_Free(&b.leafs);
	SB_Free(&b.leaffaces);
	SB_Free(&b.leafbrushes);
	SB_Free(&b.brushes);
	SB_Free(&b.sides);
	SB_Free(&b.verts);
	SB_Free(&b.indexes);
	SB_Free(&b.surfs);
	Mem_Free(b.cellbrush);
	Mem_Free(b.blocksurf);
	return ok;
}

/* --- loading ------------------------------------------------------------- */

/* the .bsp is ours to replace when it is missing or one we generated */
static bool bsp_is_ours(const char *path)
{
	size_t len;
	char *d = FS_LoadFile(path, &len);
	bool ours;
	if (!d)
		return true;
	ours = false;
	if (len > 16 + 17 * 8 && !memcmp(d, "IBSP", 4)) {
		const uint8_t *u = (const uint8_t *)d;
		size_t ofs = u[8] | (u[9] << 8) | (u[10] << 16) | ((size_t)u[11] << 24);
		size_t n = u[12] | (u[13] << 8) | (u[14] << 16) | ((size_t)u[15] << 24);
		if (ofs + n <= len)
			for (size_t i = ofs; i + 18 <= ofs + n && !ours; i++)
				ours = !memcmp(d + i, "_terrain_generated", 18);
	}
	Mem_Free(d);
	return ours;
}

static bool load_terrain(const char *name)
{
	char path[160];
	size_t len;
	char *text;
	snprintf(path, sizeof(path), "maps/%s.hmp", name);
	if (!(text = FS_LoadFile(path, &len)))
		return false;
	ter_loaded = false;
	Str_Copy(ter.name, name, sizeof(ter.name));
	Str_Copy(ter.hmp, path, sizeof(ter.hmp));
	if (!parse_hmp(text)) {
		Con_Printf("%s: couldn't parse the entities\n", path);
		Mem_Free(text);
		return false;
	}
	ter.hash = hash_str(text, len);
	Mem_Free(text);
	load_heights();
	ter_loaded = true;
	return true;
}

/* called before maps/<name>.bsp is loaded; builds it from maps/<name>.hmp */
bool Terrain_PrepareMap(const char *name)
{
	char bsp[160];
	uint32_t oldhash = ter_loaded && !strcmp(ter.name, name) ? ter.hash : 0;
	snprintf(bsp, sizeof(bsp), "maps/%s.bsp", name);
	{
		char hmp[160];
		snprintf(hmp, sizeof(hmp), "maps/%s.hmp", name);
		if (!FS_FileExists(hmp) || !bsp_is_ours(bsp))
			return false;
	}
	if (!load_terrain(name))
		return false;
	/* the listen server's client asks again for the same map */
	if (oldhash && oldhash == ter.hash && FS_FileExists(bsp))
		return true;
	return write_bsp(bsp);
}

/* --- commands ------------------------------------------------------------ */

static void TerrainCreate_f(int argc, const char **argv)
{
	char path[160], ospath[512];
	strbuf_t sb = {0};
	FILE *f;
	if (argc < 2) {
		Con_Printf("%s: NAME \"DESCRIPTION\" SKYNAME DEFAULTGROUNDTEX DEFAULTHEIGHT DEFAULTWATER DEFAULTWATERHEIGHT seed\n"
		           "Generates a fresh maps/foo.hmp file. You may wish to edit it with notepad later to customise it. "
		           "You will need csaddon.dat in order to edit the actual terrain.\n",
		           argv[0]);
		return;
	}
	snprintf(path, sizeof(path), "maps/%s.hmp", argv[1]);
	if (FS_FileExists(path)) {
		Con_Printf("%s: already exists, not overwriting.\n", path);
		return;
	}
	SB_Printf(&sb,
	          "{\nclassname \"worldspawn\"\nmessage \"%s\"\n_sky \"%s\"\n_fog 0.02\n"
	          "_maxdrawdist 0 /*overrides fog distance (if greater)*/\n"
	          "_segmentsize 1024 /*how big each section is. this affects texturing and resolutions*/\n"
	          "_minxsegment -2048\n_minysegment -2048\n_maxxsegment 2048\n_maxysegment 2048\n"
	          "_seed \"%s\" /*for auto-gen plugins*/\n_exterior solid\n"
	          "_defaultgroundtexture \"%s\"\n_defaultgroundheight \"%s\"\n"
	          "_defaultwatertexture \"%s\"\n_defaultwaterheight \"%s\"\n}\n"
	          "{\nclassname info_player_start\norigin \"0 0 1024\" /*EDITME*/\n}",
	          argc > 2 ? argv[2] : argv[1], argc > 3 ? argv[3] : "terrsky1", argc > 8 ? argv[8] : "",
	          argc > 4 ? argv[4] : "ground1_1", argc > 5 ? argv[5] : "-1024", argc > 6 ? argv[6] : "*water2",
	          argc > 7 ? argv[7] : "0");
	if (!FS_WritePath(path, ospath, sizeof(ospath)) || !FS_CreatePath(ospath) || !(f = fopen(ospath, "wb"))) {
		Con_Printf("couldn't write %s\n", path);
		SB_Free(&sb);
		return;
	}
	fwrite(sb.data, 1, sb.len, f);
	fclose(f);
	SB_Free(&sb);
	Con_Printf("Wrote %s\n", path);
}

static bool current(int argc, const char **argv, int arg)
{
	const char *name = argc > arg ? argv[arg] : NULL;
	if (!name) {
		cvar_t *mn = Cvar_Find("mapname");
		name = mn && mn->string[0] ? mn->string : (ter_loaded ? ter.name : NULL);
	}
	if (!name)
		return false;
	if (ter_loaded && !strcmp(ter.name, name))
		return true;
	return load_terrain(name);
}

static void TerrainSave_f(int argc, const char **argv)
{
	int saved = 0;
	if (!current(argc, argv, 1))
		return;
	for (int sy = 0; sy < ter.nsy; sy++)
		for (int sx = 0; sx < ter.nsx; sx++) {
			char path[160], ospath[512];
			strbuf_t sb = {0};
			FILE *f;
			sect_path(ter.sx0 + sx, ter.sy0 + sy, path, sizeof(path));
			SB_Append(&sb, "BWH1", 4);
			put32(&sb, 1);
			put32(&sb, ter.sx0 + sx);
			put32(&sb, ter.sy0 + sy);
			for (int y = 0; y < SECT_VERTS; y++)
				for (int x = 0; x < SECT_VERTS; x++)
					putf(&sb, H(sx * SECT_CELLS + x, sy * SECT_CELLS + y));
			if (FS_WritePath(path, ospath, sizeof(ospath)) && FS_CreatePath(ospath) && (f = fopen(ospath, "wb"))) {
				saved += fwrite(sb.data, 1, sb.len, f) == sb.len;
				fclose(f);
			}
			SB_Free(&sb);
		}
	Con_Printf("ter_save: %d sections saved\n", saved);
}

static void TerrainReload_f(int argc, const char **argv)
{
	char bsp[160];
	cvar_t *mn = Cvar_Find("mapname");
	if (!current(argc, argv, 1))
		return;
	if (!load_terrain(ter.name))
		return;
	snprintf(bsp, sizeof(bsp), "maps/%s.bsp", ter.name);
	if (!bsp_is_ours(bsp)) {
		Con_Printf("%s is not a generated terrain map, not replacing it\n", bsp);
		return;
	}
	if (write_bsp(bsp) && mn && !strcmp(mn->string, ter.name))
		Cbuf_AddText("restart\n");
}

static void TerrainConvert_f(int argc, const char **argv)
{
	char path[160], ospath[512];
	const char *tex = ter.groundtex;
	strbuf_t sb = {0};
	FILE *f;
	if (!current(argc, argv, 1)) {
		Con_Printf("no model loaded by that name\n");
		return;
	}
	if (argc > 2) {
		if (Str_ICmp(argv[2], ter.groundtex)) {
			Con_Printf("no tiles use %s (this terrain has only %s)\n", argv[2], ter.groundtex);
			return;
		}
		tex = argv[2];
	}
	/* the worldspawn and entities, then the ground as brushes in the worldspawn */
	{
		const char *close = strstr(ter.ents.data ? ter.ents.data : "", "}\n");
		if (!close) {
			Con_Printf("no model loaded by that name\n");
			return;
		}
		SB_Append(&sb, ter.ents.data, (size_t)(close - ter.ents.data));
	}
	for (int y = 0; y < ter.ny; y++)
		for (int x = 0; x < ter.nx; x++) {
			float x0 = ter.x0 + x * ter.cell, y0 = ter.y0 + y * ter.cell, x1 = x0 + ter.cell, y1 = y0 + ter.cell;
			float h00 = H(x, y), h10 = H(x + 1, y), h01 = H(x, y + 1), h11 = H(x + 1, y + 1);
			float bot = BW_MIN(BW_MIN(h00, h10), BW_MIN(h01, h11)) - 64;
			/* each face: three points clockwise seen from outside */
			SB_Printf(&sb,
			          "{\n( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 1 1\n"
			          "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 1 1\n"
			          "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 1 1\n"
			          "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 1 1\n"
			          "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 1 1\n}\n",
			          x0, y0, h00, x1, y1, h11, x1, y0, h10, tex,
			          x0, y0, bot, x1, y0, bot, x1, y1, bot, tex,
			          x0, y0, h00, x1, y0, h10, x1, y0, bot, tex,
			          x1, y0, h10, x1, y1, h11, x1, y1, bot, tex,
			          x1, y1, h11, x0, y0, h00, x0, y0, bot, tex);
			SB_Printf(&sb,
			          "{\n( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 1 1\n"
			          "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 1 1\n"
			          "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 1 1\n"
			          "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 1 1\n"
			          "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s 0 0 0 1 1\n}\n",
			          x0, y0, h00, x0, y1, h01, x1, y1, h11, tex,
			          x0, y0, bot, x1, y1, bot, x0, y1, bot, tex,
			          x0, y0, h00, x1, y1, h11, x1, y1, bot, tex,
			          x1, y1, h11, x0, y1, h01, x0, y1, bot, tex,
			          x0, y1, h01, x0, y0, h00, x0, y0, bot, tex);
		}
	SB_AppendStr(&sb, "}\n");
	{
		const char *rest = strstr(ter.ents.data, "}\n");
		SB_AppendStr(&sb, rest + 2);
	}
	snprintf(path, sizeof(path), "maps/%s.map", ter.name);
	if (!FS_WritePath(path, ospath, sizeof(ospath)) || !FS_CreatePath(ospath) || !(f = fopen(ospath, "wb"))) {
		Con_Printf("unable to open %s\n", path);
		SB_Free(&sb);
		return;
	}
	fwrite(sb.data, 1, sb.len, f);
	fclose(f);
	Con_Printf("Wrote %s (%d brushes)\n", path, ter.nx * ter.ny * 2);
	SB_Free(&sb);
}

void Terrain_Init(void)
{
	ter_shadowdist = Cvar_Get("mod_terrain_shadow_dist", "2048", 0,
	                          "How far rays should be cast in order to look for occlusing geometry.");
	ter_shadows = Cvar_Get("mod_terrain_shadows", "0", 0,
	                       "Cast rays to determine whether parts of the terrain should be in shadow.");
	ter_ambient = Cvar_Get("mod_terrain_ambient", "0.5", 0, "Proportion of ambient light.");
	ter_sundir = Cvar_Get("mod_terrain_sundir", "0.4 0.7 2", 0, "The direction of the sun (vector will be normalised).");
	Cvar_Get("mod_terrain_savever", "", 0, "Which terrain section version to write if terrain was edited.");
	Cvar_Get("mod_terrain_defaulttexture", "", 0,
	         "Newly created terrain tiles will use this texture. This should generally be updated by the terrain editor.");
	Cvar_Get("mod_terrain_networked", "0", 0,
	         "Terrain edits are networked. Clients will download sections on demand, and servers will notify clients "
	         "of changes.");
	Cmd_AddD("mod_terrain_create", TerrainCreate_f,
	         "Generates a fresh maps/foo.hmp file. You may wish to edit it with notepad later to customise it.");
	Cmd_AddD("mod_terrain_save", TerrainSave_f, "Writes the current terrain's heights out as section files.");
	Cmd_AddD("mod_terrain_reload", TerrainReload_f, "Rebuilds the current terrain map from its .hmp and sections.");
	Cmd_AddD("mod_terrain_convert", TerrainConvert_f,
	         "mod_terrain_convert [mapname] [texkill]\nConverts a terrain to a .map file of brushes.");
}
