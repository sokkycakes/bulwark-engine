/* Bulwark engine - the renderer's OpenGL 3.3 subset on the Xbox GPU (NV2A),
 * written straight to its command stream through nxdk's pbkit.
 * ISC licence, see LICENSE.
 *
 * The renderer reaches GL only through the q* pointers of r_gl.h, uses one
 * vertex layout (rvert_t) and five GLSL programs (r_gl.c). This file
 * implements exactly that subset, the way gl_ctr.c does for the 3DS:
 *
 * - Programs are recognised from their GLSL text and run on the NV2A's
 *   fixed-function transform and lighting, with the register combiners
 *   doing the fragment work:
 *     2D       texture x vertex colour x u_color
 *     world    texture x (lightmap x u_lmscale, or the vertex colour),
 *              plus the fullbright texture, times u_color
 *     model    texture x (ambient + diffuse x N.L) from one hardware
 *              light, plus fullbright, times u_color
 *     sky      a cube map looked up by texgen from the view direction
 *   Fog is the hardware's per-vertex exp2 fog from the radial eye distance,
 *   which is the formula the GLSL uses. The modelview is the model matrix
 *   moved by -u_vieworg, so eye space is world space around the viewer:
 *   lighting directions and fog distances match the shaders'. Alpha
 *   testing is the hardware's. Dynamic lights are not drawn yet.
 * - Textures are resampled to powers of two, swizzled, and kept in
 *   contiguous write-combined memory; mipmaps are made on the CPU. World
 *   textures may be stored as 16 bits (gl_xbox_texquality 0).
 * - Vertex buffers live in contiguous memory too. Streamed ones come from a
 *   ring that alternates halves every frame; storage the GPU may still read
 *   is freed two frames later.
 * - Indices are sent inline in the command stream, 16 bits each when the
 *   draw's vertex range allows it (the vertex arrays are rebased to the
 *   draw's lowest index).
 *
 * Not supported: framebuffer objects (CheckFramebufferStatus says
 * incomplete), so the renderer draws straight to the screen. */
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/common.h"
#include "../../common/cmd.h"
#include "../../common/cvar.h"
#include "../../render/r_gl.h"
#include "xbox.h"

#define RVERT_STRIDE 44 /* sizeof(rvert_t): pos 3f, st 2f, lm 2f, normal 3f, colour 4ub */
#define PB_SIZE (4u << 20)
#define PB_DWORDS (PB_SIZE / 4)
#define RING_HALF (2u << 20)
#define ZMAX 16777215.0f
#define GPU_MAXRAM 0x03FFAFFF

#define GL_OUT_OF_MEMORY 0x0505
#define GL_INVALID_ENUM 0x0500

static cvar_t *gl_max_size, *gl_xbox_texquality, *gl_xbox_mipmaps, *vid_xbox_swapinterval, *gl_xbox_aniso, *gl_xbox_stats;

static bool ready;
static int fbw = 640, fbh = 480;
static uint32_t framenum = 1;

/* --- contiguous memory for the GPU ----------------------------------------------- */

/* Small allocations come out of 1 MB chunks (the kernel hands out whole
 * pages, which would waste most of a page on every small texture); big
 * ones are allocated on their own. Block lists live in the normal heap,
 * since reading write-combined memory back is slow. */
#define CHUNK_SIZE (1u << 20)
#define POOL_LIMIT (128u << 10)
#define GPU_ALIGN 128

typedef struct {
	uint32_t ofs, size;
	bool free;
} gblock_t;

typedef struct {
	uint8_t *base;
	gblock_t *blocks;
	int num, max;
} gchunk_t;

static gchunk_t *chunks;
static int numchunks;
static size_t gpu_used;

static void *contig_alloc(size_t size)
{
	return MmAllocateContiguousMemoryEx(size, 0, GPU_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
}

static void chunk_insert(gchunk_t *c, int at, gblock_t b)
{
	if (c->num == c->max) {
		c->max = c->max ? c->max * 2 : 32;
		c->blocks = realloc(c->blocks, sizeof(gblock_t) * c->max);
	}
	memmove(c->blocks + at + 1, c->blocks + at, sizeof(gblock_t) * (c->num - at));
	c->blocks[at] = b;
	c->num++;
}

static void *pool_alloc(uint32_t size)
{
	size = (size + GPU_ALIGN - 1) & ~(uint32_t)(GPU_ALIGN - 1);
	for (int pass = 0; pass < 2; pass++) {
		for (int ci = 0; ci < numchunks; ci++) {
			gchunk_t *c = &chunks[ci];
			for (int i = 0; i < c->num; i++) {
				gblock_t *b = &c->blocks[i];
				if (!b->free || b->size < size)
					continue;
				if (b->size > size) {
					gblock_t rest = {b->ofs + size, b->size - size, true};
					b->size = size;
					b->free = false;
					chunk_insert(c, i + 1, rest);
				} else {
					b->free = false;
				}
				return c->base + c->blocks[i].ofs;
			}
		}
		if (pass)
			break;
		/* no room: a new chunk */
		uint8_t *base = contig_alloc(CHUNK_SIZE);
		if (!base)
			return NULL;
		chunks = realloc(chunks, sizeof(gchunk_t) * (numchunks + 1));
		gchunk_t *c = &chunks[numchunks++];
		memset(c, 0, sizeof(*c));
		c->base = base;
		chunk_insert(c, 0, (gblock_t){0, CHUNK_SIZE, true});
	}
	return NULL;
}

static bool pool_free(void *p)
{
	for (int ci = 0; ci < numchunks; ci++) {
		gchunk_t *c = &chunks[ci];
		if ((uint8_t *)p < c->base || (uint8_t *)p >= c->base + CHUNK_SIZE)
			continue;
		uint32_t ofs = (uint32_t)((uint8_t *)p - c->base);
		int lo = 0, hi = c->num - 1;
		while (lo <= hi) {
			int mid = (lo + hi) / 2;
			if (c->blocks[mid].ofs == ofs) {
				gblock_t *b = &c->blocks[mid];
				b->free = true;
				/* merge with the free neighbours */
				if (mid + 1 < c->num && c->blocks[mid + 1].free) {
					b->size += c->blocks[mid + 1].size;
					memmove(c->blocks + mid + 1, c->blocks + mid + 2, sizeof(gblock_t) * (c->num - mid - 2));
					c->num--;
				}
				if (mid > 0 && c->blocks[mid - 1].free) {
					c->blocks[mid - 1].size += b->size;
					memmove(c->blocks + mid, c->blocks + mid + 1, sizeof(gblock_t) * (c->num - mid - 1));
					c->num--;
				}
				return true;
			}
			if (c->blocks[mid].ofs < ofs)
				lo = mid + 1;
			else
				hi = mid - 1;
		}
		return true; /* inside a chunk but not a block start: ignore */
	}
	return false;
}

/* every allocation is preceded by nothing: its size is tracked by the owner */
static void *gpu_alloc(size_t size)
{
	void *p;
	if (!size)
		size = 1;
	p = size <= POOL_LIMIT ? pool_alloc((uint32_t)size) : contig_alloc(size);
	if (p)
		gpu_used += size;
	return p;
}

static void gpu_free_now(void *p, size_t size)
{
	if (!p)
		return;
	gpu_used -= BW_MIN(gpu_used, size);
	if (!pool_free(p))
		MmFreeContiguousMemory(p);
}

size_t XBGL_GpuMemoryUsed(void) { return gpu_used; }

/* storage the GPU may still read: freed two frames on */
typedef struct {
	void *p;
	size_t size;
	uint32_t frame;
} deferred_t;
static deferred_t *deferred;
static int numdeferred, maxdeferred;

static void gpu_free(void *p, size_t size)
{
	if (!p)
		return;
	if (numdeferred == maxdeferred) {
		maxdeferred = maxdeferred ? maxdeferred * 2 : 64;
		deferred = realloc(deferred, sizeof(deferred_t) * maxdeferred);
	}
	deferred[numdeferred++] = (deferred_t){p, size, framenum + 2};
}

static void run_deferred(bool all)
{
	int j = 0;
	for (int i = 0; i < numdeferred; i++) {
		if (all || (int32_t)(framenum - deferred[i].frame) >= 0)
			gpu_free_now(deferred[i].p, deferred[i].size);
		else
			deferred[j++] = deferred[i];
	}
	numdeferred = j;
}

/* this frame's streamed vertices: a ring whose halves alternate by frame */
static uint8_t *ring;
static size_t ringused;

static void *ring_alloc(size_t size)
{
	uint8_t *half = ring + (framenum & 1) * RING_HALF;
	size = (size + 15) & ~(size_t)15;
	if (ringused + size <= RING_HALF) {
		void *p = half + ringused;
		ringused += size;
		return p;
	}
	void *p = gpu_alloc(size);
	gpu_free(p, size);
	return p;
}

/* --- the command stream ----------------------------------------------------------- */

static uint32_t *pbhead; /* where the stream restarted at the last reset */
static uint32_t *P;      /* the write position between pb_begin and pb_end */

/* room for n more dwords, resetting the stream (it waits for the GPU to
 * catch up) when it would run past the end */
static void pb_open(size_t n)
{
	P = pb_begin();
	if ((size_t)(P - pbhead) + n + 4096 > PB_DWORDS) {
		pb_end(P);
		pb_reset();
		P = pbhead = pb_begin();
	}
}

static void pb_close(void) { pb_end(P); }

static inline uint32_t f2u(float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}

static inline void cmd(uint32_t method, uint32_t n) { *P++ = (n << 18) | method; }
static inline void cmd_noinc(uint32_t method, uint32_t n) { *P++ = 0x40000000u | (n << 18) | method; }
static inline void put1(uint32_t method, uint32_t v)
{
	P[0] = (1u << 18) | method;
	P[1] = v;
	P += 2;
}

/* the hardware's state as last sent, to send only what changes */
#define NREGS (0x2000 / 4)
static uint32_t hwval[NREGS];
static uint8_t hwvalid[NREGS];

static inline void reg(uint32_t method, uint32_t v)
{
	uint32_t i = method >> 2;
	if (hwvalid[i] && hwval[i] == v)
		return;
	hwvalid[i] = 1;
	hwval[i] = v;
	put1(method, v);
}

static inline void regf(uint32_t method, float f) { reg(method, f2u(f)); }

/* several consecutive registers at once, sent only if any changed */
static void regs(uint32_t method, const uint32_t *v, int n)
{
	uint32_t i = method >> 2;
	bool same = true;
	for (int k = 0; k < n; k++)
		if (!hwvalid[i + k] || hwval[i + k] != v[k]) {
			same = false;
			break;
		}
	if (same)
		return;
	cmd(method, (uint32_t)n);
	for (int k = 0; k < n; k++) {
		hwvalid[i + k] = 1;
		hwval[i + k] = v[k];
		*P++ = v[k];
	}
}

static void regsf(uint32_t method, const float *v, int n)
{
	uint32_t u[16];
	memcpy(u, v, sizeof(float) * n);
	regs(method, u, n);
}

/* matrices sit above the register cache's range: cached separately */
typedef struct {
	float m[16];
	bool valid;
} mcache_t;
static mcache_t mc_composite, mc_modelview, mc_invmv, mc_texmat[4];

static void push_matrix(uint32_t method, mcache_t *c, const float *m)
{
	if (c->valid && !memcmp(c->m, m, sizeof(c->m)))
		return;
	memcpy(c->m, m, sizeof(c->m));
	c->valid = true;
	cmd(method, 16);
	memcpy(P, m, 64);
	P += 16;
}

static void invalidate_hw(void)
{
	memset(hwvalid, 0, sizeof(hwvalid));
	mc_composite.valid = mc_modelview.valid = mc_invmv.valid = false;
	for (int i = 0; i < 4; i++)
		mc_texmat[i].valid = false;
}

/* --- object tables ------------------------------------------------------------------ */

typedef struct {
	bool used, cube, mipped, dynamic;
	uint8_t *data; /* GPU memory */
	size_t bytes;
	int w, h;   /* as GL was given it */
	int tw, th; /* on the GPU */
	int levels;
	uint32_t fmt; /* NV2A colour format */
	int bpp;
	GLenum minf, magf, wraps, wrapt;
	uint8_t *pending; /* level 0 (tw x th RGBA) kept until glGenerateMipmap or the next upload */
} xtex_t;

typedef struct {
	bool used;
	bool element; /* data is in the normal heap, read by the CPU when drawing */
	bool ring;    /* data is in this frame's ring */
	uint8_t *data;
	size_t size, alloc;
	uint32_t usedframe, ringframe;
} xbuf_t;

typedef struct {
	bool enabled;
	GLuint buf;
	size_t ofs;
	GLint size;
	GLenum type;
	GLsizei stride;
} xattr_t;

typedef struct {
	bool used;
	GLuint ebo;
	xattr_t a[8];
} xvao_t;

/* PK_SKIP links but draws nothing: effects the fixed-function NV2A has no
 * stand-in for (the screen-space model outlines) */
enum { PK_NONE, PK_2D, PK_WORLD, PK_MODEL, PK_SKY, PK_SKIP };

enum {
	U_NONE,
	U_MVP, U_MODEL, U_TEX, U_LIGHTMAP, U_CUBE, U_COLOR, U_LMSCALE, U_VIEWORG, U_LIGHT_AMBIENT, U_LIGHT_DIR,
	U_LIGHT_DIFFUSE, U_TCMOD, U_ALPHATEST, U_FOG, U_TIME, U_DLIGHTS, U_NUMDLIGHTS, U_DLIGHTCOLORS, U_FULLBRIGHT,
	U_USEFULLBRIGHT, U_COUNT
};
static const char *uniform_names[U_COUNT] = {
	"", "u_mvp", "u_model", "u_tex", "u_lightmap", "u_cube", "u_color", "u_lmscale", "u_vieworg",
	"u_light_ambient", "u_light_dir", "u_light_diffuse", "u_tcmod", "u_alphatest", "u_fog", "u_time",
	"u_dlights", "u_numdlights", "u_dlightcolors", "u_fullbright", "u_usefullbright"};

typedef struct {
	bool used;
	int kind;
	GLuint shaders[2];
	float mvp[16], model[16], color[4], tcmod[4], vieworg[3], fog[4];
	float lamb[3], ldiff[3], ldir[3];
	float lmscale, alphatest, usefullbright;
} xprog_t;

typedef struct {
	bool used;
	GLenum type;
	char *src;
} xshader_t;

#define TABLE(type, name)                                                                                      \
	static type *name;                                                                                         \
	static int num_##name;                                                                                     \
	static GLuint new_##name(void)                                                                             \
	{                                                                                                          \
		for (int i = 1; i < num_##name; i++)                                                                   \
			if (!name[i].used) {                                                                               \
				memset(&name[i], 0, sizeof(name[i]));                                                          \
				name[i].used = true;                                                                           \
				return (GLuint)i;                                                                              \
			}                                                                                                  \
		int n = num_##name ? num_##name * 2 : 64;                                                              \
		name = realloc(name, sizeof(type) * n);                                                                \
		memset(name + num_##name, 0, sizeof(type) * (n - num_##name));                                        \
		int i = num_##name ? num_##name : 1;                                                                   \
		num_##name = n;                                                                                        \
		name[i].used = true;                                                                                   \
		return (GLuint)i;                                                                                      \
	}                                                                                                          \
	static type *get_##name(GLuint id) { return id && (int)id < num_##name && name[id].used ? &name[id] : NULL; }

TABLE(xtex_t, texs)
TABLE(xbuf_t, bufs)
TABLE(xvao_t, vaos)
TABLE(xprog_t, progs)
TABLE(xshader_t, shaders)

/* --- GL state ------------------------------------------------------------------------ */

static struct {
	GLenum error;
	int unit;
	GLuint tex2d[4], texcube[4];
	GLuint arraybuf, vao;
	xvao_t defvao;
	GLuint prog;
	bool blend, depthtest, cull, scissor, polyoffset;
	GLenum bsrc, bdst, bsrca, bdsta, depthfunc, cullface;
	bool depthmask;
	uint32_t colormask;
	float clearcolor[4];
	float depthnear, depthfar, offsetfactor, offsetunits;
	int vp[4], sc[4];
} st;

static void set_error(GLenum e)
{
	if (!st.error)
		st.error = e;
}

static xvao_t *cur_vao(void) { return st.vao ? get_vaos(st.vao) : &st.defvao; }

static xtex_t whitetex, blackcube;

/* --- textures: conversion -------------------------------------------------------------- */

static int pot_ceil(int v)
{
	int p = 1;
	while (p < v)
		p <<= 1;
	return p;
}

static int ilog2(int v)
{
	int l = 0;
	while ((1 << l) < v)
		l++;
	return l;
}

/* Area-weighted resample of RGBA, one axis at a time. Each output texel
 * averages the source span it covers, which shrinks cleanly and stretches
 * as near-nearest. */
static void resample_axis(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, bool horizontal)
{
	int n = horizontal ? sw : sh, dn = dw, lines = horizontal ? sh : sw;
	float scale = (float)n / dn;
	for (int l = 0; l < lines; l++)
		for (int i = 0; i < dn; i++) {
			float x0 = i * scale, x1 = (i + 1) * scale, acc[4] = {0}, wsum = 0;
			for (int s = (int)x0; s < n && s < x1; s++) {
				float w = BW_MIN(x1, s + 1.0f) - BW_MAX(x0, (float)s);
				const uint8_t *p = horizontal ? src + ((size_t)l * sw + s) * 4 : src + ((size_t)s * sw + l) * 4;
				if (w <= 0)
					continue;
				for (int c = 0; c < 4; c++)
					acc[c] += p[c] * w;
				wsum += w;
			}
			uint8_t *o = horizontal ? dst + ((size_t)l * dw + i) * 4 : dst + ((size_t)i * sw + l) * 4;
			for (int c = 0; c < 4; c++)
				o[c] = (uint8_t)(wsum > 0 ? acc[c] / wsum + 0.5f : 0);
		}
}

static uint8_t *resample(const uint8_t *src, int sw, int sh, int dw, int dh)
{
	uint8_t *tmp = malloc((size_t)dw * sh * 4), *out = malloc((size_t)dw * dh * 4);
	if (!tmp || !out) {
		free(tmp);
		free(out);
		return NULL;
	}
	if (dw == sw)
		memcpy(tmp, src, (size_t)sw * sh * 4);
	else
		resample_axis(src, sw, sh, tmp, dw, true);
	if (dh == sh)
		memcpy(out, tmp, (size_t)dw * sh * 4);
	else
		resample_axis(tmp, dw, sh, out, dh, false);
	free(tmp);
	return out;
}

/* NV2A colour formats this file stores */
#define FMT_A8B8G8R8 0x3A /* bytes R G B A, as GL's RGBA */
#define FMT_R5G6B5 0x05
#define FMT_A4R4G4B4 0x04
#define FMT_A1R5G5B5 0x02

static int fmt_bpp(uint32_t fmt) { return fmt == FMT_A8B8G8R8 ? 4 : 2; }

/* The GPU's swizzled layout: the bits of x and y interleaved, x first,
 * until the shorter side runs out; the rest come from the longer side. */
static void swizzle_tables(int w, int h, uint32_t *xo, uint32_t *yo)
{
	int lw = ilog2(w), lh = ilog2(h);
	for (int x = 0; x < w; x++) {
		uint32_t o = 0;
		int bit = 0;
		for (int i = 0; i < BW_MAX(lw, lh); i++) {
			if (i < lw)
				o |= (uint32_t)((x >> i) & 1) << bit, bit++;
			if (i < lh)
				bit++;
		}
		xo[x] = o;
	}
	for (int y = 0; y < h; y++) {
		uint32_t o = 0;
		int bit = 0;
		for (int i = 0; i < BW_MAX(lw, lh); i++) {
			if (i < lw)
				bit++;
			if (i < lh)
				o |= (uint32_t)((y >> i) & 1) << bit, bit++;
		}
		yo[y] = o;
	}
}

static inline void put_texel(uint8_t *dst, uint32_t i, uint32_t fmt, const uint8_t *p)
{
	switch (fmt) {
	case FMT_A8B8G8R8: memcpy(dst + i * 4, p, 4); break;
	case FMT_R5G6B5: ((uint16_t *)dst)[i] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3)); break;
	case FMT_A1R5G5B5:
		((uint16_t *)dst)[i] = (uint16_t)(((p[3] >> 7) << 15) | ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3));
		break;
	default: /* A4R4G4B4 */
		((uint16_t *)dst)[i] = (uint16_t)(((p[3] >> 4) << 12) | ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4));
		break;
	}
}

/* rows of rgba (pitch in texels) into the rectangle at x0,y0 of a w x h level */
static void swizzle_rect(uint8_t *dst, int w, int h, const uint8_t *rgba, int pitch, int x0, int y0, int rw, int rh,
                         uint32_t fmt)
{
	static uint32_t *xo, *yo;
	static int tabw, tabh;
	if (tabw != w || tabh != h) {
		xo = realloc(xo, sizeof(uint32_t) * (size_t)w);
		yo = realloc(yo, sizeof(uint32_t) * (size_t)h);
		swizzle_tables(w, h, xo, yo);
		tabw = w, tabh = h;
	}
	/* build each row in cached memory isn't needed: texels are scattered anyway */
	for (int y = 0; y < rh; y++) {
		uint32_t ry = yo[y0 + y];
		const uint8_t *src = rgba + (size_t)y * pitch * 4;
		if (fmt == FMT_A8B8G8R8) {
			uint32_t *d = (uint32_t *)dst;
			for (int x = 0; x < rw; x++) {
				uint32_t v;
				memcpy(&v, src + x * 4, 4);
				d[ry | xo[x0 + x]] = v;
			}
		} else {
			for (int x = 0; x < rw; x++)
				put_texel(dst, ry | xo[x0 + x], fmt, src + x * 4);
		}
	}
}

/* the cheapest format that keeps what the picture has */
static uint32_t pick_format(const uint8_t *rgba, int n, GLint internal, bool world)
{
	bool alpha = false, partial = false, blank = true;
	if (internal == GL_RGB8 || internal == GL_RGB)
		return FMT_A8B8G8R8; /* lightmaps: 565 bands visibly */
	if (!world || (gl_xbox_texquality && gl_xbox_texquality->ivalue))
		return FMT_A8B8G8R8;
	for (int i = 0; i < n; i++) {
		uint8_t a = rgba[i * 4 + 3];
		if (a != 255)
			alpha = true;
		if (a != 0)
			blank = false;
		if (a != 0 && a != 255) {
			partial = true;
			break;
		}
	}
	if (blank)
		partial = true;
	return !alpha ? FMT_R5G6B5 : partial ? FMT_A4R4G4B4 : FMT_A1R5G5B5;
}

static size_t level_bytes(int w, int h, int bpp) { return (size_t)BW_MAX(w, 1) * BW_MAX(h, 1) * bpp; }

static size_t chain_bytes(int w, int h, int levels, int bpp)
{
	size_t n = 0;
	for (int l = 0; l < levels; l++)
		n += level_bytes(w >> l, h >> l, bpp);
	return n;
}

static GLuint pending_id; /* the one texture that may hold its RGBA (upload_2d) */

static GLuint tex_id(const xtex_t *t)
{
	return texs && t >= texs && t < texs + num_texs ? (GLuint)(t - texs) : 0;
}

static void free_tex_storage(xtex_t *t)
{
	if (t->data)
		gpu_free(t->data, t->bytes);
	t->data = NULL;
	t->bytes = 0;
	free(t->pending);
	t->pending = NULL;
	if (pending_id && pending_id == tex_id(t))
		pending_id = 0;
}

/* the size cap for textures of the 3D world (the ones that ask for mipmaps) */
static int world_max_size(void) { return BW_CLAMP(gl_max_size ? gl_max_size->ivalue : 512, 16, 4096); }

/* RGBA in, sized to powers of two for the GPU; returns tw x th RGBA (malloc) */
static uint8_t *prepare_rgba(const uint8_t *rgba, int w, int h, int maxsz, int *tw, int *th)
{
	*tw = pot_ceil(w), *th = pot_ceil(h);
	while (*tw > maxsz)
		*tw >>= 1;
	while (*th > maxsz)
		*th >>= 1;
	if (*tw == w && *th == h) {
		uint8_t *copy = malloc((size_t)w * h * 4);
		if (copy)
			memcpy(copy, rgba, (size_t)w * h * 4);
		return copy;
	}
	return resample(rgba, w, h, *tw, *th);
}

static uint8_t *expand_rgb(const uint8_t *rgb, int w, int h)
{
	uint8_t *out = malloc((size_t)w * h * 4);
	if (!out)
		return NULL;
	for (int i = 0; i < w * h; i++)
		out[i * 4] = rgb[i * 3], out[i * 4 + 1] = rgb[i * 3 + 1], out[i * 4 + 2] = rgb[i * 3 + 2], out[i * 4 + 3] = 255;
	return out;
}

static void drop_pending(void)
{
	xtex_t *p = get_texs(pending_id);
	if (p) {
		free(p->pending);
		p->pending = NULL;
	}
	pending_id = 0;
}

/* level 0 of a 32-bit texture back to linear RGBA */
static uint8_t *unswizzle(const xtex_t *t)
{
	if (t->fmt != FMT_A8B8G8R8 || !t->data)
		return NULL;
	uint8_t *out = malloc((size_t)t->tw * t->th * 4);
	uint32_t *xo = malloc(sizeof(uint32_t) * (size_t)t->tw), *yo = malloc(sizeof(uint32_t) * (size_t)t->th);
	if (!out || !xo || !yo) {
		free(out), free(xo), free(yo);
		return NULL;
	}
	swizzle_tables(t->tw, t->th, xo, yo);
	const uint32_t *src = (const uint32_t *)t->data;
	for (int y = 0; y < t->th; y++)
		for (int x = 0; x < t->tw; x++)
			memcpy(out + ((size_t)y * t->tw + x) * 4, &src[yo[y] | xo[x]], 4);
	free(xo), free(yo);
	return out;
}

static void upload_2d(xtex_t *t, const uint8_t *rgba, int w, int h, GLint internal, bool dynamic)
{
	int tw, th;
	uint8_t *img = prepare_rgba(rgba, w, h, 4096, &tw, &th);
	if (!img) {
		set_error(GL_OUT_OF_MEMORY);
		return;
	}
	free_tex_storage(t);
	t->cube = false;
	t->w = w, t->h = h, t->tw = tw, t->th = th;
	t->fmt = dynamic ? FMT_A8B8G8R8 : pick_format(img, tw * th, internal, false);
	t->bpp = fmt_bpp(t->fmt);
	t->levels = 1;
	t->mipped = false;
	t->bytes = level_bytes(tw, th, t->bpp);
	t->data = gpu_alloc(t->bytes);
	if (!t->data) {
		Con_Printf("^3gl_xbox: out of memory for a %dx%d texture\n", tw, th);
		free(img);
		set_error(GL_OUT_OF_MEMORY);
		return;
	}
	swizzle_rect(t->data, tw, th, img, tw, 0, 0, tw, th, t->fmt);
	/* glGenerateMipmap usually follows right away: keep the RGBA for it, but
	 * only for the latest upload, or every HUD picture and lightmap would hold
	 * a second copy in main memory. Mips for an older one come back out of
	 * the GPU copy (unswizzle). */
	drop_pending();
	t->pending = img;
	pending_id = tex_id(t);
}

/* a world texture: capped to gl_max_size, maybe 16-bit, with its mip chain */
static void generate_mips(xtex_t *t)
{
	int maxsz = world_max_size(), nw = t->tw, nh = t->th, levels = 1;
	uint8_t *cur, *img;
	uint32_t fmt;
	if (!t->data || t->cube)
		return;
	if (!t->pending && !(t->pending = unswizzle(t)))
		return;
	while (nw > maxsz)
		nw >>= 1;
	while (nh > maxsz)
		nh >>= 1;
	img = t->pending;
	if (nw != t->tw || nh != t->th) {
		img = resample(t->pending, t->tw, t->th, nw, nh);
		if (!img)
			return;
	}
	fmt = pick_format(img, nw * nh, GL_RGBA8, true);
	if (!gl_xbox_mipmaps || gl_xbox_mipmaps->ivalue)
		levels = ilog2(BW_MAX(nw, nh)) + 1;
	int bpp = fmt_bpp(fmt);
	size_t bytes = chain_bytes(nw, nh, levels, bpp);
	uint8_t *data = gpu_alloc(bytes);
	if (!data) {
		if (img != t->pending)
			free(img);
		return; /* keep what there is */
	}
	uint8_t *dst = data;
	int w = nw, h = nh, pw = nw, ph = nh;
	cur = img;
	for (int l = 0; l < levels; l++) {
		if (l > 0) {
			/* a box filter over the level above; a side already at 1 doesn't halve */
			int sx = pw > 1 ? 2 : 1, sy = ph > 1 ? 2 : 1;
			uint8_t *next = malloc((size_t)w * h * 4);
			if (!next) {
				levels = l;
				break;
			}
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++)
					for (int c = 0; c < 4; c++) {
						int x0 = x * sx, y0 = y * sy, x1 = x0 + sx - 1, y1 = y0 + sy - 1;
						next[(y * w + x) * 4 + c] =
							(uint8_t)((cur[(y0 * pw + x0) * 4 + c] + cur[(y0 * pw + x1) * 4 + c] +
						               cur[(y1 * pw + x0) * 4 + c] + cur[(y1 * pw + x1) * 4 + c] + 2) >> 2);
					}
			if (cur != img)
				free(cur);
			cur = next;
			pw = w, ph = h;
		}
		swizzle_rect(dst, w, h, cur, w, 0, 0, w, h, fmt);
		dst += level_bytes(w, h, bpp);
		w = BW_MAX(w >> 1, 1), h = BW_MAX(h >> 1, 1);
	}
	if (cur != img)
		free(cur);
	if (img != t->pending)
		free(img);
	gpu_free(t->data, t->bytes);
	free(t->pending);
	t->pending = NULL;
	t->data = data;
	t->bytes = bytes;
	t->tw = nw, t->th = nh;
	t->fmt = fmt;
	t->bpp = bpp;
	t->levels = levels;
	t->mipped = levels > 1;
}

static void upload_cube_face(xtex_t *t, int face, const uint8_t *rgba, int w, int h)
{
	int tw, th;
	uint8_t *img = prepare_rgba(rgba, w, h, world_max_size(), &tw, &th);
	if (!img)
		return;
	/* the six faces share one size and are square: the first face decides it */
	tw = th = BW_MAX(tw, th);
	if (tw != pot_ceil(w) || th != pot_ceil(h)) {
		uint8_t *sq = resample(rgba, w, h, tw, th);
		free(img);
		if (!(img = sq))
			return;
	}
	if (!t->data || !t->cube || t->tw != tw) {
		free_tex_storage(t);
		t->cube = true;
		t->w = w, t->h = h, t->tw = tw, t->th = th;
		t->fmt = gl_xbox_texquality && gl_xbox_texquality->ivalue ? FMT_A8B8G8R8 : FMT_R5G6B5;
		t->bpp = fmt_bpp(t->fmt);
		t->levels = 1;
		size_t face_bytes = (level_bytes(tw, th, t->bpp) + 127) & ~(size_t)127;
		t->bytes = face_bytes * 6;
		t->data = gpu_alloc(t->bytes);
		if (!t->data) {
			free(img);
			return;
		}
	}
	size_t face_bytes = (level_bytes(tw, th, t->bpp) + 127) & ~(size_t)127;
	swizzle_rect(t->data + face_bytes * face, tw, th, img, tw, 0, 0, tw, th, t->fmt);
	free(img);
}

/* --- GL entry points: textures ----------------------------------------------------------- */

static void BWGLAPI xb_glGenTextures(GLsizei n, GLuint *ids)
{
	for (int i = 0; i < n; i++) {
		ids[i] = new_texs();
		xtex_t *t = &texs[ids[i]];
		t->minf = GL_LINEAR, t->magf = GL_LINEAR, t->wraps = t->wrapt = GL_REPEAT;
	}
}

static void BWGLAPI xb_glDeleteTextures(GLsizei n, const GLuint *ids)
{
	for (int i = 0; i < n; i++) {
		xtex_t *t = get_texs(ids[i]);
		if (!t)
			continue;
		free_tex_storage(t);
		t->used = false;
		for (int u = 0; u < 4; u++) {
			if (st.tex2d[u] == ids[i])
				st.tex2d[u] = 0;
			if (st.texcube[u] == ids[i])
				st.texcube[u] = 0;
		}
	}
}

static void BWGLAPI xb_glActiveTexture(GLenum unit) { st.unit = BW_CLAMP((int)unit - GL_TEXTURE0, 0, 3); }

static void BWGLAPI xb_glBindTexture(GLenum target, GLuint id)
{
	if (target == GL_TEXTURE_CUBE_MAP)
		st.texcube[st.unit] = id;
	else
		st.tex2d[st.unit] = id;
}

static xtex_t *bound_tex(GLenum target)
{
	bool cube = target == GL_TEXTURE_CUBE_MAP ||
	            (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target < GL_TEXTURE_CUBE_MAP_POSITIVE_X + 6);
	return get_texs(cube ? st.texcube[st.unit] : st.tex2d[st.unit]);
}

static void BWGLAPI xb_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                                    GLenum fmt, GLenum type, const void *pixels)
{
	xtex_t *t = bound_tex(target);
	uint8_t *expanded = NULL;
	const uint8_t *rgba = pixels;
	(void)border;
	if (!t || level != 0 || type != GL_UNSIGNED_BYTE || w <= 0 || h <= 0)
		return;
	if (!pixels) {
		expanded = calloc((size_t)w * h, 4);
		rgba = expanded;
	} else if (fmt == GL_RGB) {
		rgba = expanded = expand_rgb(pixels, w, h);
	} else if (fmt != GL_RGBA) {
		set_error(GL_INVALID_ENUM);
		return;
	}
	if (!rgba)
		return;
	if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target < GL_TEXTURE_CUBE_MAP_POSITIVE_X + 6)
		upload_cube_face(t, (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X), rgba, w, h);
	else
		upload_2d(t, rgba, w, h, ifmt, !pixels);
	t->dynamic = !pixels;
	free(expanded);
}

static void BWGLAPI xb_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                       GLenum fmt, GLenum type, const void *pixels)
{
	xtex_t *t = bound_tex(target);
	const uint8_t *src = pixels;
	uint8_t *expanded = NULL;
	if (!t || !t->data || t->cube || level != 0 || type != GL_UNSIGNED_BYTE || !pixels || w <= 0 || h <= 0)
		return;
	if (fmt == GL_RGB)
		src = expanded = expand_rgb(pixels, w, h);
	if (!src)
		return;
	if (t->pending && t->tw == t->w && t->th == t->h) {
		/* keep the copy that glGenerateMipmap works from up to date */
		for (int r = 0; r < h; r++)
			memcpy(t->pending + ((size_t)(y + r) * t->tw + x) * 4, src + (size_t)r * w * 4, (size_t)w * 4);
	}
	if (t->tw == t->w && t->th == t->h && x >= 0 && y >= 0 && x + w <= t->tw && y + h <= t->th) {
		swizzle_rect(t->data, t->tw, t->th, src, w, x, y, w, h, t->fmt);
	} else {
		/* the texture was resized: scale the rectangle and point-sample it */
		float sx = (float)t->tw / t->w, sy = (float)t->th / t->h;
		int x0 = (int)(x * sx), y0 = (int)(y * sy), x1 = (int)((x + w) * sx + 0.999f), y1 = (int)((y + h) * sy + 0.999f);
		uint8_t texel[4];
		for (int ty = BW_MAX(y0, 0); ty < BW_MIN(y1, t->th); ty++)
			for (int tx = BW_MAX(x0, 0); tx < BW_MIN(x1, t->tw); tx++) {
				int ix = BW_CLAMP((int)((tx + 0.5f) / sx) - x, 0, w - 1);
				int iy = BW_CLAMP((int)((ty + 0.5f) / sy) - y, 0, h - 1);
				memcpy(texel, src + ((size_t)iy * w + ix) * 4, 4);
				swizzle_rect(t->data, t->tw, t->th, texel, 1, tx, ty, 1, 1, t->fmt);
			}
	}
	free(expanded);
}

static void BWGLAPI xb_glTexParameteri(GLenum target, GLenum pname, GLint v)
{
	xtex_t *t = bound_tex(target);
	if (!t)
		return;
	switch (pname) {
	case GL_TEXTURE_MIN_FILTER: t->minf = (GLenum)v; break;
	case GL_TEXTURE_MAG_FILTER: t->magf = (GLenum)v; break;
	case GL_TEXTURE_WRAP_S: t->wraps = (GLenum)v; break;
	case GL_TEXTURE_WRAP_T: t->wrapt = (GLenum)v; break;
	default: break;
	}
}

static void BWGLAPI xb_glTexParameterf(GLenum target, GLenum pname, GLfloat v)
{
	if (pname == GL_TEXTURE_MAX_ANISOTROPY)
		return; /* gl_xbox_aniso decides */
	xb_glTexParameteri(target, pname, (GLint)v);
}

static void BWGLAPI xb_glGenerateMipmap(GLenum target)
{
	xtex_t *t = bound_tex(target);
	if (t)
		generate_mips(t);
}

/* --- GL entry points: buffers and vertex arrays ------------------------------------------ */

static void BWGLAPI xb_glGenBuffers(GLsizei n, GLuint *ids)
{
	for (int i = 0; i < n; i++)
		ids[i] = new_bufs();
}

static void release_storage(xbuf_t *b)
{
	if (!b->data || b->ring)
		return;
	if (b->element)
		free(b->data);
	else
		gpu_free(b->data, b->alloc);
	b->data = NULL;
}

static void BWGLAPI xb_glDeleteBuffers(GLsizei n, const GLuint *ids)
{
	for (int i = 0; i < n; i++) {
		xbuf_t *b = get_bufs(ids[i]);
		if (!b)
			continue;
		release_storage(b);
		b->used = false;
		if (st.arraybuf == ids[i])
			st.arraybuf = 0;
	}
}

static void BWGLAPI xb_glBindBuffer(GLenum target, GLuint id)
{
	if (target == GL_ELEMENT_ARRAY_BUFFER)
		cur_vao()->ebo = id;
	else
		st.arraybuf = id;
}

static xbuf_t *bound_buf(GLenum target) { return get_bufs(target == GL_ELEMENT_ARRAY_BUFFER ? cur_vao()->ebo : st.arraybuf); }

/* new storage for a buffer's whole contents */
static bool respecify(xbuf_t *b, GLenum target, size_t size, bool stream)
{
	release_storage(b);
	b->data = NULL;
	b->size = size;
	b->alloc = 0;
	b->element = target == GL_ELEMENT_ARRAY_BUFFER;
	b->ring = false;
	if (!size)
		return true;
	if (b->element) {
		b->data = malloc(size);
	} else if (stream) {
		b->data = ring_alloc(size);
		b->ring = true;
		b->ringframe = framenum;
	} else {
		b->data = gpu_alloc(size);
		b->alloc = size;
	}
	if (!b->data) {
		Con_Printf("^3gl_xbox: out of memory for a %u byte buffer\n", (unsigned)size);
		set_error(GL_OUT_OF_MEMORY);
		b->size = 0;
		return false;
	}
	return true;
}

static void BWGLAPI xb_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	xbuf_t *b = bound_buf(target);
	if (!b || size < 0)
		return;
	if (!respecify(b, target, (size_t)size, usage == GL_STREAM_DRAW))
		return;
	if (data && size)
		memcpy(b->data, data, (size_t)size);
}

static void BWGLAPI xb_glBufferSubData(GLenum target, GLintptr ofs, GLsizeiptr size, const void *data)
{
	xbuf_t *b = bound_buf(target);
	if (!b || !b->data || ofs < 0 || (size_t)(ofs + size) > b->size)
		return;
	if (!b->element && (uint32_t)(framenum - b->usedframe) < 2) {
		/* the GPU may still read the old contents */
		if (ofs == 0 && (size_t)size == b->size) {
			/* all of it replaced (CPU-animated models, every frame): the ring */
			respecify(b, target, (size_t)size, true);
		} else {
			uint8_t *copy = gpu_alloc(b->size);
			if (copy) {
				memcpy(copy, b->data, b->size); /* slow (write-combined), but rare */
				if (!b->ring)
					gpu_free(b->data, b->alloc);
				b->data = copy;
				b->alloc = b->size;
				b->ring = false;
			}
		}
		if (!b->data)
			return;
	}
	memcpy(b->data + ofs, data, (size_t)size);
}

static void BWGLAPI xb_glGenVertexArrays(GLsizei n, GLuint *ids)
{
	for (int i = 0; i < n; i++)
		ids[i] = new_vaos();
}

static void BWGLAPI xb_glDeleteVertexArrays(GLsizei n, const GLuint *ids)
{
	for (int i = 0; i < n; i++) {
		xvao_t *v = get_vaos(ids[i]);
		if (v)
			v->used = false;
		if (st.vao == ids[i])
			st.vao = 0;
	}
}

static void BWGLAPI xb_glBindVertexArray(GLuint id) { st.vao = id; }

static void BWGLAPI xb_glEnableVertexAttribArray(GLuint i)
{
	if (i < 8)
		cur_vao()->a[i].enabled = true;
}

static void BWGLAPI xb_glDisableVertexAttribArray(GLuint i)
{
	if (i < 8)
		cur_vao()->a[i].enabled = false;
}

static void BWGLAPI xb_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride,
                                             const void *p)
{
	(void)norm;
	if (i >= 8)
		return;
	xattr_t *a = &cur_vao()->a[i];
	a->buf = st.arraybuf;
	a->ofs = (size_t)p;
	a->size = size;
	a->type = type;
	a->stride = stride;
}

static void BWGLAPI xb_glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
	(void)i, (void)x, (void)y, (void)z, (void)w; /* the renderer always feeds every attribute from rvert_t */
}

/* --- GL entry points: shaders and programs ----------------------------------------------- */

static GLuint BWGLAPI xb_glCreateShader(GLenum type)
{
	GLuint id = new_shaders();
	shaders[id].type = type;
	return id;
}

static void BWGLAPI xb_glDeleteShader(GLuint id)
{
	xshader_t *s = get_shaders(id);
	if (s) {
		free(s->src);
		s->src = NULL;
		s->used = false;
	}
}

static void BWGLAPI xb_glShaderSource(GLuint id, GLsizei n, const GLchar *const *src, const GLint *len)
{
	xshader_t *s = get_shaders(id);
	strbuf_t sb = {0};
	if (!s)
		return;
	for (int i = 0; i < n; i++) {
		if (len && len[i] >= 0)
			SB_Append(&sb, src[i], (size_t)len[i]);
		else
			SB_AppendStr(&sb, src[i]);
	}
	free(s->src);
	s->src = strdup(sb.data ? sb.data : "");
	SB_Free(&sb);
}

static void BWGLAPI xb_glCompileShader(GLuint id) { (void)id; }

static void BWGLAPI xb_glGetShaderiv(GLuint id, GLenum p, GLint *v)
{
	(void)id;
	*v = p == GL_COMPILE_STATUS ? GL_TRUE : 0;
}

static void BWGLAPI xb_glGetShaderInfoLog(GLuint id, GLsizei max, GLsizei *len, GLchar *log)
{
	(void)id;
	if (max > 0)
		log[0] = 0;
	if (len)
		*len = 0;
}

static GLuint BWGLAPI xb_glCreateProgram(void)
{
	GLuint id = new_progs();
	xprog_t *p = &progs[id];
	p->color[0] = p->color[1] = p->color[2] = p->color[3] = 1;
	p->tcmod[2] = p->tcmod[3] = 1;
	for (int i = 0; i < 4; i++)
		p->mvp[i * 5] = p->model[i * 5] = 1;
	return id;
}

static void BWGLAPI xb_glDeleteProgram(GLuint id)
{
	xprog_t *p = get_progs(id);
	if (p)
		p->used = false;
}

static void BWGLAPI xb_glAttachShader(GLuint prog, GLuint sh)
{
	xprog_t *p = get_progs(prog);
	xshader_t *s = get_shaders(sh);
	if (p && s)
		p->shaders[s->type == GL_FRAGMENT_SHADER] = sh;
}

static void BWGLAPI xb_glBindAttribLocation(GLuint p, GLuint i, const GLchar *name) { (void)p, (void)i, (void)name; }

/* which of the renderer's programs this is, from its GLSL (r_gl.c) */
static void BWGLAPI xb_glLinkProgram(GLuint id)
{
	xprog_t *p = get_progs(id);
	xshader_t *vs, *fs;
	if (!p)
		return;
	vs = get_shaders(p->shaders[0]);
	fs = get_shaders(p->shaders[1]);
	p->kind = PK_NONE;
	if (!vs || !fs || !vs->src || !fs->src)
		return;
	if (strstr(fs->src, "samplerCube"))
		p->kind = PK_SKY;
	else if (strstr(fs->src, "u_lightmap"))
		p->kind = PK_WORLD;
	else if (strstr(fs->src, "u_light_ambient"))
		p->kind = PK_MODEL;
	else if (strstr(fs->src, "u_tex") && strstr(vs->src, "a_color") && !strstr(fs->src, "texelFetch"))
		p->kind = PK_2D;
	else if (strstr(fs->src, "u_outline"))
		p->kind = PK_SKIP;
}

static void BWGLAPI xb_glGetProgramiv(GLuint id, GLenum pn, GLint *v)
{
	xprog_t *p = get_progs(id);
	*v = pn == GL_LINK_STATUS ? (p && p->kind != PK_NONE) : 0;
}

static void BWGLAPI xb_glGetProgramInfoLog(GLuint id, GLsizei max, GLsizei *len, GLchar *log)
{
	(void)id;
	snprintf(log, (size_t)max, "not one of the programs the Xbox backend knows");
	if (len)
		*len = (GLsizei)strlen(log);
}

static void BWGLAPI xb_glUseProgram(GLuint id) { st.prog = id; }

static GLint BWGLAPI xb_glGetUniformLocation(GLuint prog, const GLchar *name)
{
	(void)prog;
	for (int i = 1; i < U_COUNT; i++)
		if (!strcmp(uniform_names[i], name))
			return i;
	return -1;
}

static xprog_t *cur_prog(void) { return get_progs(st.prog); }

static void set_uniform(GLint loc, int n, const float *v)
{
	xprog_t *p = cur_prog();
	if (!p || loc <= 0)
		return;
	switch (loc) {
	case U_COLOR: memcpy(p->color, v, sizeof(float) * BW_MIN(n, 4)); break;
	case U_TCMOD: memcpy(p->tcmod, v, sizeof(float) * BW_MIN(n, 4)); break;
	case U_VIEWORG: memcpy(p->vieworg, v, sizeof(float) * BW_MIN(n, 3)); break;
	case U_FOG: memcpy(p->fog, v, sizeof(float) * BW_MIN(n, 4)); break;
	case U_LIGHT_AMBIENT: memcpy(p->lamb, v, sizeof(float) * BW_MIN(n, 3)); break;
	case U_LIGHT_DIFFUSE: memcpy(p->ldiff, v, sizeof(float) * BW_MIN(n, 3)); break;
	case U_LIGHT_DIR: memcpy(p->ldir, v, sizeof(float) * BW_MIN(n, 3)); break;
	case U_LMSCALE: p->lmscale = v[0]; break;
	case U_ALPHATEST: p->alphatest = v[0]; break;
	case U_USEFULLBRIGHT: p->usefullbright = v[0]; break;
	default: break; /* samplers are fixed per unit; time and dlights aren't drawn */
	}
}

static void BWGLAPI xb_glUniform1i(GLint l, GLint v) { (void)l, (void)v; }
static void BWGLAPI xb_glUniform1f(GLint l, GLfloat v) { set_uniform(l, 1, &v); }

static void BWGLAPI xb_glUniform2f(GLint l, GLfloat a, GLfloat b)
{
	float v[2] = {a, b};
	set_uniform(l, 2, v);
}

static void BWGLAPI xb_glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c)
{
	float v[3] = {a, b, c};
	set_uniform(l, 3, v);
}

static void BWGLAPI xb_glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d)
{
	float v[4] = {a, b, c, d};
	set_uniform(l, 4, v);
}

static void BWGLAPI xb_glUniform3fv(GLint l, GLsizei n, const GLfloat *v)
{
	if (n == 1)
		set_uniform(l, 3, v);
}

static void BWGLAPI xb_glUniform4fv(GLint l, GLsizei n, const GLfloat *v)
{
	if (n == 1)
		set_uniform(l, 4, v);
}

static void BWGLAPI xb_glUniformMatrix4fv(GLint l, GLsizei n, GLboolean tr, const GLfloat *v)
{
	xprog_t *p = cur_prog();
	(void)n, (void)tr;
	if (!p)
		return;
	if (l == U_MVP)
		memcpy(p->mvp, v, sizeof(p->mvp));
	else if (l == U_MODEL)
		memcpy(p->model, v, sizeof(p->model));
}

/* --- GL entry points: fixed state ---------------------------------------------------------- */

static void BWGLAPI xb_glEnable(GLenum cap)
{
	switch (cap) {
	case GL_BLEND: st.blend = true; break;
	case GL_DEPTH_TEST: st.depthtest = true; break;
	case GL_CULL_FACE: st.cull = true; break;
	case GL_SCISSOR_TEST: st.scissor = true; break;
	case GL_POLYGON_OFFSET_FILL: st.polyoffset = true; break;
	default: break;
	}
}

static void BWGLAPI xb_glDisable(GLenum cap)
{
	switch (cap) {
	case GL_BLEND: st.blend = false; break;
	case GL_DEPTH_TEST: st.depthtest = false; break;
	case GL_CULL_FACE: st.cull = false; break;
	case GL_SCISSOR_TEST: st.scissor = false; break;
	case GL_POLYGON_OFFSET_FILL: st.polyoffset = false; break;
	default: break;
	}
}

static void BWGLAPI xb_glBlendFunc(GLenum s, GLenum d) { st.bsrc = st.bsrca = s, st.bdst = st.bdsta = d; }
static void BWGLAPI xb_glBlendFuncSeparate(GLenum s, GLenum d, GLenum sa, GLenum da)
{
	st.bsrc = s, st.bdst = d, st.bsrca = sa, st.bdsta = da;
}
static void BWGLAPI xb_glDepthFunc(GLenum f) { st.depthfunc = f; }
static void BWGLAPI xb_glDepthMask(GLboolean f) { st.depthmask = f; }
static void BWGLAPI xb_glDepthRange(double n, double f) { st.depthnear = (float)n, st.depthfar = (float)f; }
static void BWGLAPI xb_glCullFace(GLenum m) { st.cullface = m; }
static void BWGLAPI xb_glPolygonOffset(GLfloat factor, GLfloat units) { st.offsetfactor = factor, st.offsetunits = units; }
static void BWGLAPI xb_glLineWidth(GLfloat w) { (void)w; }
static void BWGLAPI xb_glPixelStorei(GLenum p, GLint v) { (void)p, (void)v; }

static void BWGLAPI xb_glFinish(void)
{
	while (pb_busy())
		;
}

static void BWGLAPI xb_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	st.colormask = (r ? NV097_SET_COLOR_MASK_RED_WRITE_ENABLE : 0) | (g ? NV097_SET_COLOR_MASK_GREEN_WRITE_ENABLE : 0) |
	               (b ? NV097_SET_COLOR_MASK_BLUE_WRITE_ENABLE : 0) | (a ? NV097_SET_COLOR_MASK_ALPHA_WRITE_ENABLE : 0);
}

static void BWGLAPI xb_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
	st.vp[0] = x, st.vp[1] = y, st.vp[2] = w, st.vp[3] = h;
}

static void BWGLAPI xb_glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
	st.sc[0] = x, st.sc[1] = y, st.sc[2] = w, st.sc[3] = h;
}

static void BWGLAPI xb_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	st.clearcolor[0] = r, st.clearcolor[1] = g, st.clearcolor[2] = b, st.clearcolor[3] = a;
}

/* --- drawing: matrices ------------------------------------------------------------------------ */

/* column-major 4x4, out = a * b */
static void mat_mul(float *out, const float *a, const float *b)
{
	float r[16];
	for (int c = 0; c < 4; c++)
		for (int rr = 0; rr < 4; rr++)
			r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] + a[2 * 4 + rr] * b[c * 4 + 2] +
			                a[3 * 4 + rr] * b[c * 4 + 3];
	memcpy(out, r, sizeof(r));
}

/* row-major copy: the order the transform engine takes its matrices in */
static void transpose(float *out, const float *m)
{
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			out[r * 4 + c] = m[c * 4 + r];
}

/* inverse of an affine column-major matrix */
static void mat_inverse_affine(float *out, const float *m)
{
	float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
	float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
	float det = a * A + b * B + c * C;
	float inv = fabsf(det) > 1e-20f ? 1.0f / det : 0.0f;
	float r[16] = {0};
	r[0] = A * inv;
	r[1] = B * inv;
	r[2] = C * inv;
	r[4] = -(b * i - c * h) * inv;
	r[5] = (a * i - c * g) * inv;
	r[6] = -(a * h - b * g) * inv;
	r[8] = (b * f - c * e) * inv;
	r[9] = -(a * f - c * d) * inv;
	r[10] = (a * e - b * d) * inv;
	for (int k = 0; k < 3; k++)
		r[12 + k] = -(r[k] * m[12] + r[4 + k] * m[13] + r[8 + k] * m[14]);
	r[15] = 1;
	memcpy(out, r, sizeof(r));
}

/* GL's normalised device coordinates to the framebuffer: y down, origin
 * top left, depth over the 24-bit buffer's range */
static void viewport_matrix(float *m)
{
	float x = (float)st.vp[0], w = (float)st.vp[2], h = (float)st.vp[3];
	float y = (float)(fbh - st.vp[1] - st.vp[3]);
	float n = st.depthnear * ZMAX, f = st.depthfar * ZMAX;
	memset(m, 0, sizeof(float) * 16);
	m[0] = w * 0.5f;
	m[5] = -h * 0.5f;
	m[10] = (f - n) * 0.5f;
	m[12] = x + w * 0.5f;
	m[13] = y + h * 0.5f;
	m[14] = (f + n) * 0.5f;
	m[15] = 1;
}

/* --- drawing: state ------------------------------------------------------------------------- */

static uint32_t wrap_mode(GLenum w) { return w == GL_CLAMP_TO_EDGE ? 3 : 1; }

static uint32_t tex_filter(const xtex_t *t)
{
	/* min: 1 box, 2 tent (linear), 6 tent between tent-filtered levels */
	uint32_t mag = t->magf == GL_NEAREST ? 1 : 2, min;
	if (!t->mipped)
		min = t->minf == GL_NEAREST ? 1 : 2;
	else
		min = t->minf == GL_NEAREST ? 3 : 6;
	return (min << 16) | (mag << 24) | (1u << 13); /* quincunx convolution kernel */
}

static void bind_unit(int unit, const xtex_t *t)
{
	uint32_t base = (uint32_t)unit * 64;
	if (!t || !t->data) {
		reg(NV097_SET_TEXTURE_CONTROL0 + base, 0x0003FFC0);
		return;
	}
	uint32_t format = 2 /* DMA B */ | (t->cube ? NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE : 0) | (2u << 4) /* 2D */ |
	                  (t->fmt << 8) | ((uint32_t)t->levels << 16) | ((uint32_t)ilog2(t->tw) << 20) |
	                  ((uint32_t)ilog2(t->th) << 24) | (0u << 28) |
	                  (NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE_COLOR << 3);
	uint32_t address = t->cube ? 0x00030303 : (wrap_mode(t->wraps) | (wrap_mode(t->wrapt) << 8) | (1u << 16));
	int aniso = gl_xbox_aniso ? BW_CLAMP(gl_xbox_aniso->ivalue, 0, 3) : 0;
	uint32_t control0 = NV097_SET_TEXTURE_CONTROL0_ENABLE | (0xFFFu << 6) | (t->mipped ? (uint32_t)aniso << 4 : 0);
	reg(NV097_SET_TEXTURE_OFFSET + base, (uint32_t)t->data & 0x03FFFFFF);
	reg(NV097_SET_TEXTURE_FORMAT + base, format);
	reg(NV097_SET_TEXTURE_ADDRESS + base, address);
	reg(NV097_SET_TEXTURE_CONTROL0 + base, control0);
	reg(NV097_SET_TEXTURE_FILTER + base, tex_filter(t));
}

static const xtex_t *tex2d(GLuint id)
{
	xtex_t *t = get_texs(id);
	return t && t->data && !t->cube ? t : &whitetex;
}

/* register combiner inputs: source register, its alpha, the mapping */
enum { R_ZERO = 0, R_C0 = 1, R_C1 = 2, R_FOG = 3, R_V0 = 4, R_V1 = 5, R_T0 = 8, R_T1 = 9, R_T2 = 10, R_T3 = 11,
       R_R0 = 12, R_R1 = 13, R_EF = 15 };
#define CIN(src, alpha, invert) ((uint32_t)(src) | ((alpha) ? 0x10u : 0) | ((invert) ? 0x20u : 0))
#define ONE CIN(R_ZERO, 0, 1)
#define ONE_A CIN(R_ZERO, 1, 1)
#define ZERO CIN(R_ZERO, 0, 0)
#define ICW(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
/* outputs: AB and CD discarded, their sum (or AB alone) into r0, scaled */
#define OCW_AB(shift) (((uint32_t)(shift) << 15) | ((uint32_t)R_R0 << 4))
#define OCW_SUM(shift) (((uint32_t)(shift) << 15) | ((uint32_t)R_R0 << 8))

static uint32_t pack_argb(float r, float g, float b, float a)
{
	return (uint32_t)(BW_CLAMP(a, 0, 1) * 255 + 0.5f) << 24 | (uint32_t)(BW_CLAMP(r, 0, 1) * 255 + 0.5f) << 16 |
	       (uint32_t)(BW_CLAMP(g, 0, 1) * 255 + 0.5f) << 8 | (uint32_t)(BW_CLAMP(b, 0, 1) * 255 + 0.5f);
}

static uint32_t shift_for(int k) { return k >= 4 ? 4 : k >= 2 ? 2 : 0; }

static void set_combiners(xprog_t *p, int kind, bool fog)
{
	const float *uc = p->color;
	uint32_t c0, c1 = 0, s1;
	int k = 1;
	bool fb = kind != PK_2D && kind != PK_SKY && p->usefullbright > 0;
	int stages = 2;

	if (kind == PK_SKY) {
		/* r0 = cube x u_color */
		reg(NV097_SET_COMBINER_COLOR_ICW + 0, ICW(CIN(R_T0, 0, 0), CIN(R_C0, 0, 0), ZERO, ZERO));
		reg(NV097_SET_COMBINER_ALPHA_ICW + 0, ICW(CIN(R_T0, 1, 0), CIN(R_C0, 1, 0), ZERO, ZERO));
		reg(NV097_SET_COMBINER_COLOR_OCW + 0, OCW_AB(0));
		reg(NV097_SET_COMBINER_ALPHA_OCW + 0, OCW_AB(0));
		c0 = pack_argb(uc[0], uc[1], uc[2], uc[3]);
		stages = 1;
	} else {
		/* stage 0: r0 = texture x light; alpha = texture alpha x u_color alpha */
		uint32_t light = kind == PK_WORLD && p->lmscale >= 0 ? CIN(R_T1, 0, 0) : CIN(R_V0, 0, 0);
		reg(NV097_SET_COMBINER_COLOR_ICW + 0, ICW(CIN(R_T0, 0, 0), light, ZERO, ZERO));
		if (kind == PK_2D)
			reg(NV097_SET_COMBINER_ALPHA_ICW + 0, ICW(CIN(R_T0, 1, 0), CIN(R_V0, 1, 0), ZERO, ZERO));
		else
			reg(NV097_SET_COMBINER_ALPHA_ICW + 0, ICW(CIN(R_T0, 1, 0), CIN(R_C0, 1, 0), ZERO, ZERO));
		reg(NV097_SET_COMBINER_COLOR_OCW + 0, OCW_AB(0));
		reg(NV097_SET_COMBINER_ALPHA_OCW + 0, OCW_AB(0));

		/* stage 1: r0 = (r0 x c0 + fullbright x c1) x k, k being 1, 2 or 4:
		 * the light's overbright scale, which the inputs can't carry */
		float f0 = 1;
		if (kind == PK_WORLD && p->lmscale >= 0) {
			float s = BW_CLAMP(p->lmscale, 0, 4);
			k = s > 2 ? 4 : s > 1 ? 2 : 1;
			f0 = s / k;
		} else if (kind == PK_MODEL) {
			k = 2; /* the hardware light carries half the light (see set_lighting) */
		}
		c0 = pack_argb(uc[0] * f0, uc[1] * f0, uc[2] * f0, uc[3]);
		c1 = pack_argb(uc[0] / k, uc[1] / k, uc[2] / k, 1);
		reg(NV097_SET_COMBINER_COLOR_ICW + 4, ICW(CIN(R_R0, 0, 0), CIN(R_C0, 0, 0), fb ? CIN(R_T2, 0, 0) : ZERO,
		                                          fb ? CIN(R_C1, 0, 0) : ZERO));
		if (kind == PK_2D)
			reg(NV097_SET_COMBINER_ALPHA_ICW + 4, ICW(CIN(R_R0, 1, 0), CIN(R_C0, 1, 0), ZERO, ZERO));
		else
			reg(NV097_SET_COMBINER_ALPHA_ICW + 4, ICW(CIN(R_R0, 1, 0), ONE_A, ZERO, ZERO));
		s1 = shift_for(k);
		reg(NV097_SET_COMBINER_COLOR_OCW + 4, OCW_SUM(s1));
		reg(NV097_SET_COMBINER_ALPHA_OCW + 4, OCW_SUM(0));
	}
	reg(NV097_SET_COMBINER_FACTOR0, c0);
	reg(NV097_SET_COMBINER_FACTOR1, c1);
	reg(NV097_SET_COMBINER_CONTROL, (uint32_t)stages); /* factors shared by every stage */

	/* final: out = fog x r0 + (1 - fog) x fog colour, or r0 */
	if (fog)
		reg(NV097_SET_COMBINER_SPECULAR_FOG_CW0, ICW(CIN(R_FOG, 1, 0), CIN(R_R0, 0, 0), CIN(R_FOG, 0, 0), ZERO));
	else
		reg(NV097_SET_COMBINER_SPECULAR_FOG_CW0, ICW(ZERO, ZERO, ZERO, CIN(R_R0, 0, 0)));
	reg(NV097_SET_COMBINER_SPECULAR_FOG_CW1, ICW(ZERO, ZERO, CIN(R_R0, 1, 0), 0));
}

static void set_fog(xprog_t *p, bool fog)
{
	reg(NV097_SET_FOG_ENABLE, fog);
	if (!fog)
		return;
	reg(NV097_SET_FOG_MODE, NV097_SET_FOG_MODE_V_EXP2);
	reg(NV097_SET_FOG_GEN_MODE, NV097_SET_FOG_GEN_MODE_V_RADIAL);
	/* the hardware's exp2 fog is 1.5 + 2^(-(d * y)^2 * 32) - 1.5:
	 * y = -density / (2 sqrt(ln 256)) makes it e^-(d * density)^2 */
	float params[3] = {1.5f, -p->fog[3] / (2.0f * 2.354820045f), 0};
	regsf(NV097_SET_FOG_PARAMS, params, 3);
	uint32_t col = (uint32_t)(BW_CLAMP(p->fog[0], 0, 1) * 255 + 0.5f) |
	               (uint32_t)(BW_CLAMP(p->fog[1], 0, 1) * 255 + 0.5f) << 8 |
	               (uint32_t)(BW_CLAMP(p->fog[2], 0, 1) * 255 + 0.5f) << 16 | 0xFF000000u;
	reg(NV097_SET_FOG_COLOR, col);
}

/* models: ambient plus one directional light, at half strength (the
 * combiners double it) so that lights up to twice white fit */
static void set_lighting(xprog_t *p, bool on)
{
	reg(NV097_SET_LIGHTING_ENABLE, on);
	reg(NV097_SET_NORMALIZATION_ENABLE, on);
	if (!on)
		return;
	reg(NV097_SET_LIGHT_CONTROL, NV097_SET_LIGHT_CONTROL_V_SEPARATE_SPECULAR);
	reg(NV097_SET_COLOR_MATERIAL, NV097_SET_COLOR_MATERIAL_ALL_FROM_MATERIAL);
	reg(NV097_SET_LIGHT_ENABLE_MASK, NV097_SET_LIGHT_ENABLE_MASK_LIGHT0_INFINITE);
	float amb[3] = {p->lamb[0] * 0.5f, p->lamb[1] * 0.5f, p->lamb[2] * 0.5f};
	float dif[3] = {p->ldiff[0] * 0.5f, p->ldiff[1] * 0.5f, p->ldiff[2] * 0.5f};
	float zero[3] = {0, 0, 0};
	regsf(NV097_SET_SCENE_AMBIENT_COLOR, amb, 3);
	regsf(NV097_SET_MATERIAL_EMISSION, zero, 3);
	regf(NV097_SET_MATERIAL_ALPHA, 1.0f);
	regsf(NV097_SET_LIGHT_AMBIENT_COLOR, zero, 3);
	regsf(NV097_SET_LIGHT_DIFFUSE_COLOR, dif, 3);
	regsf(NV097_SET_LIGHT_SPECULAR_COLOR, zero, 3);
	float len = sqrtf(p->ldir[0] * p->ldir[0] + p->ldir[1] * p->ldir[1] + p->ldir[2] * p->ldir[2]);
	float dir[3] = {0, 0, 1};
	if (len > 1e-6f)
		dir[0] = p->ldir[0] / len, dir[1] = p->ldir[1] / len, dir[2] = p->ldir[2] / len;
	regsf(NV097_SET_LIGHT_INFINITE_DIRECTION, dir, 3);
	/* the half vector only matters for specular, which is off */
	float half[3] = {dir[0] * 0.5f, dir[1] * 0.5f, dir[2] * 0.5f + 0.5f};
	regsf(NV097_SET_LIGHT_INFINITE_HALF_VECTOR, half, 3);
}

static uint32_t test_func(GLenum f) { return f; } /* the hardware takes GL's values */

static void set_fixed_state(void)
{
	/* scissor: the window clip rectangle, inclusive, top left origin */
	int x0 = 0, y0 = 0, x1 = fbw - 1, y1 = fbh - 1;
	if (st.scissor) {
		x0 = BW_MAX(st.sc[0], 0);
		x1 = BW_MIN(st.sc[0] + st.sc[2] - 1, fbw - 1);
		y0 = BW_MAX(fbh - st.sc[1] - st.sc[3], 0);
		y1 = BW_MIN(fbh - st.sc[1] - 1, fbh - 1);
		if (x1 < x0 || y1 < y0)
			x0 = y0 = 1, x1 = y1 = 0; /* empty */
	}
	reg(NV097_SET_WINDOW_CLIP_HORIZONTAL, (uint32_t)x0 | ((uint32_t)x1 << 16));
	reg(NV097_SET_WINDOW_CLIP_VERTICAL, (uint32_t)y0 | ((uint32_t)y1 << 16));

	reg(NV097_SET_BLEND_ENABLE, st.blend);
	if (st.blend) {
		reg(NV097_SET_BLEND_FUNC_SFACTOR, st.bsrc);
		reg(NV097_SET_BLEND_FUNC_DFACTOR, st.bdst);
		reg(NV097_SET_BLEND_EQUATION, 0x8006); /* add */
	}
	/* as in GL, no depth test means no depth writes either */
	reg(NV097_SET_DEPTH_TEST_ENABLE, st.depthtest);
	reg(NV097_SET_DEPTH_FUNC, test_func(st.depthfunc));
	reg(NV097_SET_DEPTH_MASK, st.depthtest && st.depthmask);
	reg(NV097_SET_COLOR_MASK, st.colormask);
	reg(NV097_SET_POLY_OFFSET_FILL_ENABLE, st.polyoffset);
	if (st.polyoffset) {
		regf(NV097_SET_POLYGON_OFFSET_SCALE_FACTOR, st.offsetfactor);
		regf(NV097_SET_POLYGON_OFFSET_BIAS, st.offsetunits);
	}
	reg(NV097_SET_CULL_FACE_ENABLE, st.cull);
	if (st.cull)
		reg(NV097_SET_CULL_FACE, st.cullface);
	regf(NV097_SET_CLIP_MIN, st.depthnear * ZMAX);
	regf(NV097_SET_CLIP_MAX, st.depthfar * ZMAX);
}

/* programs, uniforms, textures and combiners for a draw; false to skip it */
static bool set_program(void)
{
	xprog_t *p = cur_prog();
	int kind = p ? p->kind : PK_NONE;
	float vp[16], comp[16], tmp[16];
	if (kind == PK_NONE || kind == PK_SKIP)
		return false;
	bool fog = (kind == PK_WORLD || kind == PK_MODEL) && p->fog[3] > 0;
	bool lit = kind == PK_MODEL;

	/* transforms: composite = viewport x mvp; modelview = model moved to the eye */
	viewport_matrix(vp);
	mat_mul(tmp, vp, p->mvp);
	transpose(comp, tmp);
	push_matrix(NV097_SET_COMPOSITE_MATRIX, &mc_composite, comp);
	if (fog || lit) {
		float mv[16], inv[16];
		memcpy(mv, p->model, sizeof(mv));
		mv[12] -= p->vieworg[0], mv[13] -= p->vieworg[1], mv[14] -= p->vieworg[2];
		transpose(tmp, mv);
		push_matrix(NV097_SET_MODEL_VIEW_MATRIX, &mc_modelview, tmp);
		if (lit) {
			mat_inverse_affine(inv, mv);
			push_matrix(NV097_SET_INVERSE_MODEL_VIEW_MATRIX, &mc_invmv, inv);
		}
	}
	set_lighting(p, lit);
	set_fog(p, fog);

	/* textures */
	uint32_t stagemode = 0;
	bool tcmod = p->tcmod[0] != 0 || p->tcmod[1] != 0 || p->tcmod[2] != 1 || p->tcmod[3] != 1;
	if (kind == PK_SKY) {
		xtex_t *cube = get_texs(st.texcube[3]);
		if (!cube || !cube->data || !cube->cube)
			cube = &blackcube;
		bind_unit(0, cube);
		bind_unit(1, NULL);
		bind_unit(2, NULL);
		stagemode = 3; /* cube map */
		/* the direction a_pos - u_vieworg, as the GLSL looks it up: (-x, z, y) */
		const float *o = p->vieworg;
		float s[4] = {-1, 0, 0, o[0]}, t[4] = {0, 0, 1, -o[2]}, r[4] = {0, 1, 0, -o[1]};
		reg(NV097_SET_TEXGEN_S, NV097_SET_TEXGEN_S_OBJECT_LINEAR);
		reg(NV097_SET_TEXGEN_T, NV097_SET_TEXGEN_S_OBJECT_LINEAR);
		reg(NV097_SET_TEXGEN_R, NV097_SET_TEXGEN_S_OBJECT_LINEAR);
		reg(NV097_SET_TEXGEN_Q, NV097_SET_TEXGEN_S_DISABLE);
		regsf(NV097_SET_TEXGEN_PLANE_S, s, 4);
		regsf(NV097_SET_TEXGEN_PLANE_T, t, 4);
		regsf(NV097_SET_TEXGEN_PLANE_R, r, 4);
		reg(NV097_SET_TEXTURE_MATRIX_ENABLE, 0);
	} else {
		reg(NV097_SET_TEXGEN_S, NV097_SET_TEXGEN_S_DISABLE);
		reg(NV097_SET_TEXGEN_T, NV097_SET_TEXGEN_S_DISABLE);
		reg(NV097_SET_TEXGEN_R, NV097_SET_TEXGEN_S_DISABLE);
		reg(NV097_SET_TEXGEN_Q, NV097_SET_TEXGEN_S_DISABLE);
		bind_unit(0, tex2d(st.tex2d[0]));
		stagemode = 1;
		if (kind == PK_WORLD && p->lmscale >= 0) {
			bind_unit(1, tex2d(st.tex2d[1]));
			stagemode |= 1u << 5;
		} else {
			bind_unit(1, NULL);
		}
		if (kind != PK_2D && p->usefullbright > 0) {
			bind_unit(2, tex2d(st.tex2d[2]));
			stagemode |= 1u << 10;
		} else {
			bind_unit(2, NULL);
		}
		if (tcmod) {
			/* s' = s * z + x, t' = t * w + y, for the units reading a_tc */
			float m[16] = {p->tcmod[2], 0, 0, p->tcmod[0], 0, p->tcmod[3], 0, p->tcmod[1], 0, 0, 1, 0, 0, 0, 0, 1};
			push_matrix(NV097_SET_TEXTURE_MATRIX, &mc_texmat[0], m);
			push_matrix(NV097_SET_TEXTURE_MATRIX + 128, &mc_texmat[2], m);
		}
		reg(NV097_SET_TEXTURE_MATRIX_ENABLE, tcmod);
		reg(NV097_SET_TEXTURE_MATRIX_ENABLE + 8, tcmod);
	}
	reg(NV097_SET_TEXTURE_MATRIX_ENABLE + 4, 0);
	bind_unit(3, NULL);
	reg(NV097_SET_SHADER_STAGE_PROGRAM, stagemode);

	set_combiners(p, kind, fog);

	if (p->alphatest > 0) {
		reg(NV097_SET_ALPHA_TEST_ENABLE, 1);
		reg(NV097_SET_ALPHA_FUNC, 0x206); /* GL_GEQUAL */
		reg(NV097_SET_ALPHA_REF, (uint32_t)BW_CLAMP(p->alphatest * 255 + 0.5f, 0, 255));
	} else {
		reg(NV097_SET_ALPHA_TEST_ENABLE, 0);
	}
	return true;
}

/* --- drawing ---------------------------------------------------------------------------------- */

/* the vertex buffer of the bound arrays, in the one layout the renderer uses */
static xbuf_t *vertex_source(size_t *base)
{
	xvao_t *v = cur_vao();
	static const size_t offsets[5] = {0, 12, 20, 28, 40};
	xbuf_t *b;
	if (!v->a[0].enabled)
		return NULL;
	b = get_bufs(v->a[0].buf);
	for (int i = 0; i < 5; i++) {
		if (!v->a[i].enabled)
			continue; /* a program that doesn't read it; the layout is still rvert_t */
		if (v->a[i].buf != v->a[0].buf || v->a[i].stride != RVERT_STRIDE || v->a[i].ofs != v->a[0].ofs + offsets[i]) {
			static bool warned;
			if (!warned)
				Con_Printf("^3gl_xbox: a vertex layout other than rvert_t; draws skipped\n");
			warned = true;
			return NULL;
		}
	}
	if (!b || !b->data || b->element)
		return NULL;
	if (b->ring && b->ringframe != framenum)
		return NULL; /* streamed in an earlier frame: gone */
	*base = v->a[0].ofs;
	return b;
}

static void set_arrays(const uint8_t *verts)
{
	uint32_t a = (uint32_t)verts & 0x03FFFFFF;
	reg(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, a);       /* position */
	reg(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 2 * 4, a + 28);  /* normal */
	reg(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, a + 40);  /* diffuse colour */
	reg(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 9 * 4, a + 12);  /* texture 0: a_tc */
	reg(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 10 * 4, a + 20); /* texture 1: lightmap */
	reg(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 11 * 4, a + 12); /* texture 2: fullbright, a_tc again */
}

static void set_array_formats(void)
{
	for (int i = 0; i < 16; i++) {
		uint32_t f = 2; /* float, size 0: off */
		switch (i) {
		case 0:
		case 2: f = 2 | (3 << 4) | (RVERT_STRIDE << 8); break;
		case 3: f = 4 | (4 << 4) | (RVERT_STRIDE << 8); break; /* bytes in GL's RGBA order */
		case 9:
		case 10:
		case 11: f = 2 | (2 << 4) | (RVERT_STRIDE << 8); break;
		default: break;
		}
		reg(NV097_SET_VERTEX_DATA_ARRAY_FORMAT + i * 4, f);
	}
}

static int draws_this_frame, tris_this_frame;

static void draw(GLenum mode, int count, const uint32_t *idx32, const uint16_t *idx16, int first)
{
	size_t base;
	xbuf_t *vb;
	uint32_t lo = UINT32_MAX, hi = 0;
	if (mode > GL_TRIANGLE_FAN || count <= 0 || !ready)
		return;
	if (!(vb = vertex_source(&base)))
		return;
	if (idx32 || idx16) {
		for (int i = 0; i < count; i++) {
			uint32_t x = idx32 ? idx32[i] : idx16[i];
			lo = BW_MIN(lo, x), hi = BW_MAX(hi, x);
		}
	} else {
		lo = (uint32_t)first, hi = (uint32_t)(first + count - 1);
	}
	if (base + (size_t)(hi + 1) * RVERT_STRIDE > vb->size)
		return; /* past the buffer */
	bool small = hi - lo <= 0xFFFF;

	pb_open(600 + (size_t)count + (size_t)count / 2047);
	set_fixed_state();
	if (!set_program()) {
		pb_close();
		return;
	}
	set_array_formats();
	set_arrays(vb->data + base + (size_t)lo * RVERT_STRIDE);
	if (vb->ring)
		put1(NV097_BREAK_VERTEX_BUFFER_CACHE, 0);

	put1(NV097_SET_BEGIN_END, mode + 1); /* GL's primitives, one up */
	if (!idx32 && !idx16) {
		/* runs of up to 256 vertices, from the rebased start */
		int left = count, start = 0;
		while (left > 0) {
			int words = BW_MIN((left + 255) / 256, 2047);
			cmd_noinc(NV097_DRAW_ARRAYS, (uint32_t)words);
			for (int w = 0; w < words && left > 0; w++) {
				int n = BW_MIN(left, 256);
				*P++ = ((uint32_t)(n - 1) << 24) | (uint32_t)start;
				start += n, left -= n;
			}
		}
	} else if (small) {
		/* two 16-bit indices a word; an odd last one goes alone */
		int pairs = count / 2, i = 0;
		while (pairs > 0) {
			int words = BW_MIN(pairs, 2047);
			cmd_noinc(NV097_ARRAY_ELEMENT16, (uint32_t)words);
			for (int w = 0; w < words; w++, i += 2) {
				uint32_t a = (idx32 ? idx32[i] : idx16[i]) - lo, b = (idx32 ? idx32[i + 1] : idx16[i + 1]) - lo;
				*P++ = a | (b << 16);
			}
			pairs -= words;
		}
		if (count & 1)
			put1(NV097_ARRAY_ELEMENT32, (idx32 ? idx32[count - 1] : idx16[count - 1]) - lo);
	} else {
		int left = count, i = 0;
		while (left > 0) {
			int words = BW_MIN(left, 2047);
			cmd_noinc(NV097_ARRAY_ELEMENT32, (uint32_t)words);
			for (int w = 0; w < words; w++, i++)
				*P++ = (idx32 ? idx32[i] : idx16[i]) - lo;
			left -= words;
		}
	}
	put1(NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
	pb_close();
	vb->usedframe = framenum;
	draws_this_frame++;
	if (mode >= GL_TRIANGLES)
		tris_this_frame += mode == GL_TRIANGLES ? count / 3 : count - 2;
}

static void BWGLAPI xb_glDrawArrays(GLenum mode, GLint first, GLsizei count) { draw(mode, count, NULL, NULL, first); }

static void BWGLAPI xb_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *ofs,
                                                GLint basevertex)
{
	xbuf_t *eb = get_bufs(cur_vao()->ebo);
	size_t o = (size_t)ofs, sz = type == GL_UNSIGNED_INT ? 4 : 2;
	if (!eb || !eb->data || o + sz * (size_t)count > eb->size || basevertex) {
		if (basevertex) {
			static bool warned;
			if (!warned)
				Con_Printf("^3gl_xbox: glDrawElementsBaseVertex with a base vertex isn't supported\n");
			warned = true;
		}
		return;
	}
	if (type == GL_UNSIGNED_INT)
		draw(mode, count, (const uint32_t *)(eb->data + o), NULL, 0);
	else if (type == GL_UNSIGNED_SHORT)
		draw(mode, count, NULL, (const uint16_t *)(eb->data + o), 0);
}

static void BWGLAPI xb_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *ofs)
{
	xb_glDrawElementsBaseVertex(mode, count, type, ofs, 0);
}

static void BWGLAPI xb_glClear(GLbitfield mask)
{
	bool color = mask & GL_COLOR_BUFFER_BIT, depth = mask & GL_DEPTH_BUFFER_BIT;
	int x0 = 0, y0 = 0, x1 = fbw, y1 = fbh;
	uint32_t flags = 0;
	if (!ready || (!color && !depth))
		return;
	if (st.scissor) {
		x0 = BW_CLAMP(st.sc[0], 0, fbw), x1 = BW_CLAMP(st.sc[0] + st.sc[2], 0, fbw);
		y0 = BW_CLAMP(fbh - st.sc[1] - st.sc[3], 0, fbh), y1 = BW_CLAMP(fbh - st.sc[1], 0, fbh);
		if (x1 <= x0 || y1 <= y0)
			return;
	}
	if (color) {
		/* the colour mask's bits are the clear's, a nibble up */
		flags |= (st.colormask & NV097_SET_COLOR_MASK_RED_WRITE_ENABLE ? 0x10 : 0) |
		         (st.colormask & NV097_SET_COLOR_MASK_GREEN_WRITE_ENABLE ? 0x20 : 0) |
		         (st.colormask & NV097_SET_COLOR_MASK_BLUE_WRITE_ENABLE ? 0x40 : 0) |
		         (st.colormask & NV097_SET_COLOR_MASK_ALPHA_WRITE_ENABLE ? 0x80 : 0);
	}
	if (depth)
		flags |= 0x03; /* depth and stencil */
	if (!flags)
		return;
	pb_open(16);
	put1(NV097_SET_CLEAR_RECT_HORIZONTAL, (uint32_t)x0 | ((uint32_t)(x1 - 1) << 16));
	put1(NV097_SET_CLEAR_RECT_VERTICAL, (uint32_t)y0 | ((uint32_t)(y1 - 1) << 16));
	put1(NV097_SET_ZSTENCIL_CLEAR_VALUE, 0xFFFFFF00);
	put1(NV097_SET_COLOR_CLEAR_VALUE,
	     pack_argb(st.clearcolor[0], st.clearcolor[1], st.clearcolor[2], st.clearcolor[3]));
	put1(NV097_CLEAR_SURFACE, flags);
	pb_close();
}

/* --- GL entry points: queries and the rest ------------------------------------------------- */

static const GLubyte *BWGLAPI xb_glGetString(GLenum name)
{
	switch (name) {
	case GL_VENDOR: return (const GLubyte *)"Bulwark";
	case GL_RENDERER: return (const GLubyte *)"NV2A (pbkit)";
	case GL_VERSION: return (const GLubyte *)"3.3 subset";
	default: return (const GLubyte *)"";
	}
}

static GLenum BWGLAPI xb_glGetError(void)
{
	GLenum e = st.error;
	st.error = 0;
	return e;
}

static void BWGLAPI xb_glGetIntegerv(GLenum p, GLint *v)
{
	switch (p) {
	case GL_MAX_TEXTURE_SIZE: *v = 4096; break;
	case GL_VIEWPORT: memcpy(v, st.vp, sizeof(st.vp)); break;
	default: *v = 0; break;
	}
}

/* from the frame being drawn: waits for the GPU */
static void BWGLAPI xb_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *px)
{
	int bpp = f == GL_RGBA ? 4 : 3;
	uint8_t *out = px;
	(void)t;
	while (pb_busy())
		;
	const uint8_t *fb = (const uint8_t *)pb_back_buffer();
	int pitch = (int)pb_back_buffer_pitch();
	for (int r = 0; r < h; r++) {
		int sy = fbh - 1 - (y + r);
		for (int c = 0; c < w; c++) {
			int sx = x + c;
			uint8_t *o = out + ((size_t)r * w + c) * bpp;
			if (sx < 0 || sx >= fbw || sy < 0 || sy >= fbh) {
				memset(o, 0, (size_t)bpp);
				continue;
			}
			const uint8_t *s = fb + (size_t)sy * pitch + (size_t)sx * 4; /* B G R X */
			o[0] = s[2], o[1] = s[1], o[2] = s[0];
			if (bpp == 4)
				o[3] = 255;
		}
	}
}

static void BWGLAPI xb_glGenFramebuffers(GLsizei n, GLuint *f)
{
	for (int i = 0; i < n; i++)
		f[i] = 0;
}
static void BWGLAPI xb_glDeleteFramebuffers(GLsizei n, const GLuint *f) { (void)n, (void)f; }
static void BWGLAPI xb_glBindFramebuffer(GLenum t, GLuint f) { (void)t, (void)f; }
static GLenum BWGLAPI xb_glCheckFramebufferStatus(GLenum t)
{
	(void)t;
	return 0;
}
static void BWGLAPI xb_glGenRenderbuffers(GLsizei n, GLuint *r)
{
	for (int i = 0; i < n; i++)
		r[i] = 0;
}
static void BWGLAPI xb_glDeleteRenderbuffers(GLsizei n, const GLuint *r) { (void)n, (void)r; }
static void BWGLAPI xb_glBindRenderbuffer(GLenum t, GLuint r) { (void)t, (void)r; }
static void BWGLAPI xb_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h)
{
	(void)t, (void)f, (void)w, (void)h;
}
static void BWGLAPI xb_glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint r)
{
	(void)t, (void)a, (void)rt, (void)r;
}
static void BWGLAPI xb_glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)
{
	(void)t, (void)a, (void)tt, (void)tex, (void)l;
}

static const struct {
	const char *name;
	void *fn;
} procs[] = {
#define X(ret, name, args) {#name, (void *)xb_##name},
	BW_GL_FUNCS(X)
#undef X
};

void *XBGL_GetProc(const char *name)
{
	for (size_t i = 0; i < BW_ARRAY_COUNT(procs); i++)
		if (!strcmp(procs[i].name, name))
			return procs[i].fn;
	return NULL;
}

/* --- frames and set-up ------------------------------------------------------------------------ */

static uint32_t last_swap;

/* pbkit's own set-up after it targets a new back buffer: it turns depth
 * and stencil tests on and sets a z mode we don't use */
static void after_target(void)
{
	invalidate_hw();
	pb_open(64);
	put1(NV097_SET_CONTROL0, NV097_SET_CONTROL0_TEXTURE_PERSPECTIVE_ENABLE | NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE);
	put1(NV097_SET_STENCIL_TEST_ENABLE, 0);
	put1(NV097_SET_SPECULAR_ENABLE, 0);
	put1(NV097_SET_TRANSFORM_EXECUTION_MODE, 4); /* fixed function, privileged range */
	put1(NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);
	put1(NV097_SET_SKIN_MODE, NV097_SET_SKIN_MODE_OFF);
	put1(NV097_SET_FRONT_FACE, NV097_SET_FRONT_FACE_V_CW); /* GL's counter-clockwise, after the y flip */
	put1(NV097_SET_LIGHT_LOCAL_RANGE, 0x7149f2ca);         /* 1e30 */
	pb_close();
}

static ULONGLONG stat_start; /* when the frame's work began (the end of the last flip) */
static struct {
	int frames, missed, draws, tris;
	double work, wait, worst;
	size_t pb;
	DWORD since;
} fst;

/* every 5 seconds: frames per second, the CPU's share of a frame and the
 * wait for the GPU and the vblank, misses, and the busiest pushbuffer */
static void frame_stats(ULONGLONG t0, size_t pbused, int missed)
{
	ULONGLONG t1 = KeQueryPerformanceCounter();
	double ms = 1000.0 / (double)KeQueryPerformanceFrequency();
	if (stat_start) {
		double work = (double)(t0 - stat_start) * ms;
		double wait = (double)(t1 - t0) * ms;
		fst.frames++;
		fst.work += work, fst.wait += wait;
		if (work > fst.worst)
			fst.worst = work;
		fst.missed += missed > 0;
		fst.draws += draws_this_frame, fst.tris += tris_this_frame;
		if (pbused > fst.pb)
			fst.pb = pbused;
	}
	stat_start = t1;
	DWORD now = GetTickCount();
	if (!fst.since)
		fst.since = now;
	if (now - fst.since >= 5000 && fst.frames) {
		double secs = (now - fst.since) / 1000.0;
		Con_Printf("xbox: %.1f fps, frame work %.1f ms (worst %.1f), wait %.1f ms, %d late, %d draws, %d tris, pb %u KB, GPU %u KB, %u KB free\n",
		           fst.frames / secs, fst.work / fst.frames, fst.worst, fst.wait / fst.frames, fst.missed, fst.draws / fst.frames,
		           fst.tris / fst.frames, (unsigned)(fst.pb * 4 / 1024), (unsigned)(gpu_used >> 10), (unsigned)(XBX_FreeMemory() >> 10));
		memset(&fst, 0, sizeof(fst));
		fst.since = now;
	}
}

void XBGL_EndFrame(void)
{
	if (!ready)
		return;
	/* timing for gl_xbox_stats: the frame's work, then the wait for the GPU and the vblank */
	ULONGLONG t0 = KeQueryPerformanceCounter();
	uint32_t *cur = pb_begin();
	pb_end(cur);
	size_t pbused = (size_t)(cur - pbhead);
	/* frame pacing: flips land every vid_xbox_swapinterval vblanks */
	int interval = vid_xbox_swapinterval ? BW_CLAMP(vid_xbox_swapinterval->ivalue, 0, 4) : 2;
	DWORD now = pb_get_vbl_counter();
	while ((int)(now - last_swap) < interval)
		now = pb_wait_for_vbl();
	int missed = (int)(now - last_swap) - interval;
	last_swap = now;
	while (pb_finished())
		; /* every back buffer is waiting to be shown */
	if (gl_xbox_stats && gl_xbox_stats->ivalue)
		frame_stats(t0, pbused, missed);
	pb_reset();
	pbhead = pb_begin();
	pb_end(pbhead);
	framenum++;
	ringused = 0;
	run_deferred(false);
	after_target();
	draws_this_frame = tris_this_frame = 0;
}

int XBGL_Width(void) { return fbw; }
int XBGL_Height(void) { return fbh; }

static void make_fallback_textures(void)
{
	memset(&whitetex, 0, sizeof(whitetex));
	whitetex.tw = whitetex.th = whitetex.w = whitetex.h = 4;
	whitetex.fmt = FMT_A8B8G8R8, whitetex.bpp = 4, whitetex.levels = 1;
	whitetex.minf = whitetex.magf = GL_NEAREST, whitetex.wraps = whitetex.wrapt = GL_REPEAT;
	whitetex.bytes = 64;
	whitetex.data = gpu_alloc(64);
	if (whitetex.data)
		memset(whitetex.data, 0xFF, 64);
	memset(&blackcube, 0, sizeof(blackcube));
	blackcube.cube = true;
	blackcube.tw = blackcube.th = blackcube.w = blackcube.h = 4;
	blackcube.fmt = FMT_A8B8G8R8, blackcube.bpp = 4, blackcube.levels = 1;
	blackcube.minf = blackcube.magf = GL_NEAREST;
	blackcube.bytes = 128 * 6;
	blackcube.data = gpu_alloc(blackcube.bytes);
	if (blackcube.data)
		for (int i = 0; i < 6; i++)
			for (int j = 0; j < 16; j++)
				((uint32_t *)(blackcube.data + i * 128))[j] = 0xFF000000u;
}

static void meminfo_gpu_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	Con_Printf("GPU objects: %u KB in %d pool chunks; last frame %d draws, %d triangles\n", (unsigned)(gpu_used >> 10),
	           numchunks, draws_this_frame, tris_this_frame);
}

void XBGL_RegisterCvars(void)
{
	gl_max_size = Cvar_Get("gl_max_size", "512", CVAR_ARCHIVE,
	                       "Largest side of a world texture on the GPU; bigger ones are scaled down when loaded (Xbox)");
	gl_xbox_texquality = Cvar_Get("gl_xbox_texquality", "0", CVAR_ARCHIVE,
	                              "1 keeps world textures at 32 bits; 0 stores them as 16-bit, half the memory (Xbox)");
	gl_xbox_mipmaps = Cvar_Get("gl_xbox_mipmaps", "1", CVAR_ARCHIVE, "Make mipmaps for textures that ask for them (Xbox)");
	gl_xbox_aniso = Cvar_Get("gl_xbox_aniso", "1", CVAR_ARCHIVE,
	                         "Anisotropic filtering of mipmapped textures: 0 off, 1 2x, 2 4x, 3 8x (Xbox)");
	vid_xbox_swapinterval = Cvar_Get("vid_xbox_swapinterval", "2", CVAR_ARCHIVE,
	                                 "Vertical blanks per frame: 1 for 60 fps, 2 for a steady 30 (Xbox)");
	gl_xbox_stats = Cvar_Get("gl_xbox_stats", "0", 0, "Print frame rate and timing every 5 seconds (Xbox)");
	Cmd_AddD("xbox_gpuinfo", meminfo_gpu_f, "xbox_gpuinfo: GPU memory in use and the last frame's draw count");
}

bool XBGL_Init(void)
{
	int err;
	pb_size(PB_SIZE);
	if ((err = pb_init()) != 0) {
		Con_Printf("pbkit failed to start (%d)\n", err);
		return false;
	}
	pb_show_front_screen();
	fbw = (int)pb_back_buffer_width();
	fbh = (int)pb_back_buffer_height();
	ring = contig_alloc(RING_HALF * 2);
	if (!ring) {
		Con_Printf("gl_xbox: no memory for the vertex ring\n");
		return false;
	}
	make_fallback_textures();

	memset(&st, 0, sizeof(st));
	st.bsrc = st.bsrca = GL_ONE, st.bdst = st.bdsta = GL_ZERO;
	st.depthfunc = GL_LESS;
	st.depthmask = true;
	st.colormask = NV097_SET_COLOR_MASK_RED_WRITE_ENABLE | NV097_SET_COLOR_MASK_GREEN_WRITE_ENABLE |
	               NV097_SET_COLOR_MASK_BLUE_WRITE_ENABLE | NV097_SET_COLOR_MASK_ALPHA_WRITE_ENABLE;
	st.depthfar = 1;
	st.cullface = GL_BACK;
	st.vp[2] = fbw, st.vp[3] = fbh;

	/* the stream starts here; set what never changes */
	pb_reset();
	pbhead = pb_begin();
	pb_end(pbhead);
	pb_target_back_buffer();
	pb_open(64);
	put1(NV097_SET_TRANSFORM_PROGRAM_START, 0);
	float zero4[4] = {0, 0, 0, 0}, one4[4] = {1, 1, 1, 1};
	cmd(NV097_SET_VIEWPORT_OFFSET, 4);
	memcpy(P, zero4, 16), P += 4;
	cmd(NV097_SET_VIEWPORT_SCALE, 4);
	memcpy(P, one4, 16), P += 4;
	put1(NV097_SET_ZMIN_MAX_CONTROL, 1);
	put1(NV097_SET_COMPRESS_ZBUFFER_EN, 1);
	put1(NV097_SET_WINDOW_CLIP_TYPE, 0);
	put1(NV097_SET_DITHER_ENABLE, 1);
	pb_close();
	after_target();
	last_swap = pb_get_vbl_counter();
	ready = true;
	Con_Printf("NV2A at %dx%d, %u KB push buffer\n", fbw, fbh, PB_SIZE >> 10);
	return true;
}

void XBGL_Shutdown(void)
{
	if (!ready)
		return;
	while (pb_busy())
		;
	ready = false;
	pb_kill();
}
