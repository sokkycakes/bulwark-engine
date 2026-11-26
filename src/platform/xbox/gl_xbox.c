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
 *   contiguous write-combined memory; mipmaps are made on the CPU. Opaque
 *   world textures and the sky are stored as DXT1, compressed on upload by
 *   dxt_xbox.c (gl_xbox_texcompress 0 keeps 16 bits, as world textures
 *   with alpha always do; gl_xbox_texquality 1 keeps 32 bits). Pictures
 *   of 512 x 512 texels or more that nothing mipmaps go to DXT too when
 *   the result is near-lossless (40 dB over the texels that show), at
 *   their first draw or the next upload, so a texture that glGenerateMipmap
 *   is about to replace isn't compressed twice. Smaller pictures, grey
 *   ones with alpha (font and glyph atlases) and any texture given new
 *   pixels after its first upload (glTexImage2D again, glTexSubImage2D)
 *   stay 32-bit; one already compressed goes back to 32 bits for good.
 *   Lightmaps stay 32-bit: their atlas packs unrelated faces into the
 *   same 4x4 blocks, which DXT smears (30 dB on lqdm1).
 * - Vertex buffers live in contiguous memory too. Streamed ones come from a
 *   ring that alternates halves every frame; storage the GPU may still read
 *   is freed two frames later.
 * - Indices are sent in the command stream, 16 bits each when the draw's
 *   vertex range allows it (the vertex arrays are rebased to the draw's
 *   lowest index). A static index buffer's runs are made into commands in
 *   GPU memory once and called from the stream; streamed ones go inline.
 * - The command stream is a ring the CPU keeps a frame ahead of the GPU in,
 *   handed to the GPU in batches (see pb_kick).
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
#include "dxt_xbox.h"
#include "xbox.h"

#define RVERT_STRIDE 44 /* sizeof(rvert_t): pos 3f, st 2f, lm 2f, normal 3f, colour 4ub */
#ifdef BW_LOWMEM
/* a frame of lqdm1 is about 350 draws and 38000 indices (under 1 MB of
 * stream even at the 600 words a draw reserves) and 700 KB of streamed
 * vertices; a full push buffer resets mid-frame and a full ring falls back
 * to pool memory, so these cost time, not correctness */
#define PB_SIZE (2u << 20)
#define RING_HALF (1536u << 10)
#else
#define PB_SIZE (4u << 20)
#define RING_HALF (2u << 20)
#endif
#define PB_DWORDS (PB_SIZE / 4)
#define ZMAX 16777215.0f
#define GPU_MAXRAM 0x03FFAFFF

#define GL_OUT_OF_MEMORY 0x0505
#define GL_INVALID_ENUM 0x0500

static cvar_t *gl_max_size, *gl_xbox_texquality, *gl_xbox_texcompress, *gl_xbox_mipmaps, *vid_xbox_swapinterval,
	*gl_xbox_aniso, *gl_xbox_stats, *gl_xbox_gpuskin;

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
static size_t ringused, ringpeak, ringspill; /* the busiest frame's use and what didn't fit (gl_xbox_stats) */
static size_t st_stream, st_replace, st_index, st_copies; /* what buffer uploads carry (gl_xbox_stats) */

static void *ring_alloc(size_t size)
{
	uint8_t *half = ring + (framenum & 1) * RING_HALF;
	size = (size + 15) & ~(size_t)15;
	if (ringused + size <= RING_HALF) {
		void *p = half + ringused;
		ringused += size;
		ringpeak = BW_MAX(ringpeak, ringused);
		return p;
	}
	ringspill += size;
	void *p = gpu_alloc(size);
	gpu_free(p, size);
	return p;
}

/* --- the command stream ----------------------------------------------------------- */

/* The stream is a ring: when the write position nears the end, a jump
 * sends the GPU back to the start, and the CPU writes over a stretch only
 * once the GPU has read it. Positions are counted in dwords from the
 * start, across laps, so the CPU can tell how far behind the GPU is. Each
 * frame ends with pbkit's flip, which waits for the GPU to go idle; once
 * the GPU reads past that, everything the frame drew is done. The CPU
 * starts a frame when the one before the last is done, so it works on a
 * frame while the GPU draws the previous one, and storage two frames old
 * (the vertex ring's other half, deferred frees) is free. */
static uint32_t *pbbase, *pbend; /* the stream's storage */
static uint64_t pblap;           /* the position of pbbase in this lap */
static uint64_t pbframe;         /* where this frame's commands began (gl_xbox_stats) */
static uint32_t *P;              /* the write position: pbkit's put pointer once kicked */
static uint32_t *kicked;         /* where the GPU was last told the stream ends */
static uint64_t frame_end[2];    /* where the last two frames' commands end */
static int kicks, gpu_waits;     /* this frame's, for gl_xbox_stats */
static ULONGLONG sync_ticks;     /* waiting for the GPU (gl_xbox_stats) */

/* Telling the GPU about new commands (pb_end) costs an sfence, a flush of
 * the write-combine buffers with a busy wait on it, and an MMIO write, so
 * commands are handed over in batches of this many dwords, and whenever
 * the CPU is about to wait for the GPU. Between kicks, the commands sit
 * past pbkit's put pointer, where the GPU doesn't read yet. */
#define KICK_DWORDS (32768 / 4)
/* the GPU fetches up to 128 method words ahead of what it executes */
#define FETCH_AHEAD 512
/* the DMA get pointer of pbkit's channel (0) */
#define DMA_GET (*(volatile uint32_t *)(VIDEO_BASE + NV_USER + 0x44))

static void pb_kick(void)
{
	if (P != kicked) {
		pb_end(P);
		kicked = P;
		kicks++;
	}
}

/* after pbkit wrote to the stream itself */
static void pb_resync(void) { P = kicked = pb_begin(); }

static uint64_t stream_pos(const uint32_t *p) { return pblap + (uint64_t)(p - pbbase); }

/* how far the GPU has read: it is never a whole lap behind the kicked position */
static uint64_t gpu_seen; /* the last gpu_pos: reading the register is slow */

static uint64_t gpu_pos(void)
{
	uint32_t base = (uint32_t)pbbase & 0x03FFFFFF;
	uint32_t get = (DMA_GET & 0x03FFFFFF) - base;
	if (get >= PB_SIZE) {
		/* inside a called index run: GET points into pool memory, so take
		 * the return address, the ring word after the call; the GPU has
		 * read up to there. If that is not in the ring either, report no
		 * new progress and let the caller poll again. */
		get = (VIDEOREG(NV_PFIFO_CACHE1_DMA_SUBROUTINE) & NV_PFIFO_CACHE1_DMA_SUBROUTINE_RETURN_OFFSET & 0x03FFFFFF) - base;
		if (get >= PB_SIZE)
			return gpu_seen;
	}
	get /= 4;
	uint32_t k = (uint32_t)(kicked - pbbase);
	return gpu_seen = stream_pos(kicked) - (k + PB_DWORDS - get) % PB_DWORDS;
}

/* waits until everything before the position has been done */
static void wait_done(uint64_t pos)
{
	ULONGLONG t0 = 0;
	pb_kick();
	while (gpu_pos() < pos + FETCH_AHEAD && pb_busy()) {
		if (!t0)
			t0 = KeQueryPerformanceCounter();
	}
	if (t0)
		sync_ticks += KeQueryPerformanceCounter() - t0;
}

/* waits until the GPU no longer reads what a frame drew with */
static void gpu_done_with(uint32_t frame)
{
	if (frame == framenum)
		wait_done(stream_pos(P));
	else if (frame == framenum - 1)
		wait_done(frame_end[frame & 1]);
	else
		return;
	gpu_waits++;
}

/* room for n more dwords: past the end, the stream jumps back to the
 * start; either way, the GPU must have read what the commands go over */
static void pb_open(size_t n)
{
	n += 256; /* pbkit's flip and target commands, written without asking */
	if (P + n + 1 > pbend) {
		*P = ((uint32_t)pbbase & 0x0FFFFFFF) | 1; /* jump */
		P = pbbase;
		pblap += PB_DWORDS;
		pb_end(P);
		kicked = P;
	}
	uint64_t end = stream_pos(P) + n;
	if (end > PB_DWORDS && gpu_seen + PB_DWORDS < end && gpu_pos() + PB_DWORDS < end) {
		ULONGLONG t0 = KeQueryPerformanceCounter();
		pb_kick();
		while (gpu_pos() + PB_DWORDS < end && pb_busy())
			;
		sync_ticks += KeQueryPerformanceCounter() - t0;
	}
}

static void pb_close(void)
{
	if (P - kicked >= KICK_DWORDS)
		pb_kick();
}

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

/* bumped by anything that changes what set_fixed_state and set_program
 * send, so that draws in a row with nothing changed between them skip both */
static uint32_t state_gen = 1;

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
	state_gen++;
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
	GLenum minf, magf, wraps, wrapt;
	uint8_t *pending; /* level 0 (tw x th RGBA) kept until glGenerateMipmap or the next upload */
	bool try_dxt;     /* a big picture: compress it from pending unless it gets mipmaps */
	bool updated;     /* re-specified or sub-updated since it was made: stays 32-bit */
	uint32_t usedframe; /* the last frame that drew with it */
} xtex_t;

/* a run of a static index buffer as GPU commands, called from the stream */
typedef struct {
	size_t ofs;
	int count; /* 0: an empty slot */
	bool u32;  /* the buffer holds 32-bit indices (part of the key) */
	bool wide; /* sent as 32-bit indices */
	uint32_t lo, hi;
	uint32_t *cmds; /* GPU memory, ending in a return */
	uint32_t bytes;
} iseg_t;

typedef struct {
	bool used;
	bool element; /* data is in the normal heap, read by the CPU when drawing */
	bool stream;  /* given as GL_STREAM_DRAW */
	iseg_t *segs; /* element buffers: a hash of the runs drawn so far */
	int numsegs, maxsegs;
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
#define FMT_DXT1 0x0C /* COLOR_L_DXT1_A1R5G5B5: 4x4 blocks of 8 bytes, not swizzled */
#define FMT_DXT5 0x0F /* COLOR_L_DXT45_A8R8G8B8: 4x4 blocks of 16 bytes */

static bool fmt_dxt(uint32_t fmt) { return fmt == FMT_DXT1 || fmt == FMT_DXT5; }
static int dxt_block_bytes(uint32_t fmt) { return fmt == FMT_DXT1 ? DXT1_BLOCK_BYTES : DXT5_BLOCK_BYTES; }
/* bits per texel, for the memory report */
static int fmt_bits(uint32_t fmt) { return fmt == FMT_A8B8G8R8 ? 32 : fmt == FMT_DXT1 ? 4 : fmt == FMT_DXT5 ? 8 : 16; }
static bool texcompress(void) { return !gl_xbox_texcompress || gl_xbox_texcompress->ivalue; }

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
	/* opaque ones as DXT1 (36.4 dB against 37.7 for 565 on lqdm1); the cut-out
	 * ones keep 16 bits, as DXT smears their edges (34 dB against 40) */
	if (!alpha && texcompress())
		return FMT_DXT1;
	return !alpha ? FMT_R5G6B5 : partial ? FMT_A4R4G4B4 : FMT_A1R5G5B5;
}

static size_t level_bytes(int w, int h, uint32_t fmt)
{
	w = BW_MAX(w, 1), h = BW_MAX(h, 1);
	if (fmt_dxt(fmt))
		return DXT_LevelBytes(w, h, dxt_block_bytes(fmt));
	return (size_t)w * h * (fmt == FMT_A8B8G8R8 ? 4 : 2);
}

static size_t chain_bytes(int w, int h, int levels, uint32_t fmt)
{
	size_t n = 0;
	for (int l = 0; l < levels; l++)
		n += level_bytes(w >> l, h >> l, fmt);
	return n;
}

/* a whole w x h level of RGBA into GPU memory: swizzled, or as DXT blocks in rows */
static void store_level(uint8_t *dst, int w, int h, const uint8_t *rgba, uint32_t fmt)
{
	if (fmt_dxt(fmt))
		DXT_EncodeImage(dst, rgba, w, h, dxt_block_bytes(fmt), 0);
	else
		swizzle_rect(dst, w, h, rgba, w, 0, 0, w, h, fmt);
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
	t->try_dxt = false;
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

/* How far a DXT copy is from the picture, as the screen shows it (colour
 * weighted by alpha, and alpha): peak signal to noise in dB. */
static float dxt_psnr(const uint8_t *blocks, const uint8_t *rgba, int w, int h, uint32_t fmt)
{
	int bb = dxt_block_bytes(fmt), bw = (w + 3) / 4, bh = (h + 3) / 4;
	uint8_t dec[64];
	double se = 0;
	size_t shown = 0; /* texels with alpha in the picture or the copy */
	for (int by = 0; by < bh; by++)
		for (int bx = 0; bx < bw; bx++, blocks += bb) {
			if (fmt == FMT_DXT1)
				DXT_DecodeBlock1(blocks, dec);
			else
				DXT_DecodeBlock5(blocks, dec);
			for (int y = 0; y < 4 && by * 4 + y < h; y++)
				for (int x = 0; x < 4 && bx * 4 + x < w; x++) {
					const uint8_t *s = rgba + ((size_t)(by * 4 + y) * w + bx * 4 + x) * 4, *d = dec + (y * 4 + x) * 4;
					/* fully transparent texels would let a sparse picture
					   pass on its empty space alone */
					if (!s[3] && !d[3])
						continue;
					shown++;
					for (int c = 0; c < 3; c++) {
						int e = (d[c] * d[3] - s[c] * s[3]) / 255;
						se += e * e;
					}
					se += (d[3] - s[3]) * (d[3] - s[3]);
				}
		}
	if (!shown)
		return 0; /* nothing shows: nothing to gain from compressing either */
	if (se <= 0)
		return 99;
	return (float)(10.0 * log10(255.0 * 255.0 * 4.0 * (double)shown / se));
}

/* a big picture that nothing mipmapped: to DXT if that is near-lossless */
static void compress_picture(xtex_t *t)
{
	t->try_dxt = false;
	if (!t->pending || !t->data || t->cube || t->mipped || t->updated || t->fmt != FMT_A8B8G8R8)
		return;
	bool alpha = false, grey = true;
	for (int i = 0; i < t->tw * t->th; i++) {
		const uint8_t *px = t->pending + (size_t)i * 4;
		alpha |= px[3] != 255;
		grey &= px[0] == px[1] && px[1] == px[2];
	}
	/* grey with alpha is a coverage atlas (fonts: white texels, or
	 * premultiplied coverage in every channel), whose glyph edges DXT smears */
	if (alpha && grey)
		return;
	uint32_t fmt = alpha ? FMT_DXT5 : FMT_DXT1;
	size_t bytes = level_bytes(t->tw, t->th, fmt);
	uint8_t *blocks = malloc(bytes);
	if (!blocks)
		return;
	DXT_EncodeImage(blocks, t->pending, t->tw, t->th, dxt_block_bytes(fmt), 0);
	uint8_t *data = dxt_psnr(blocks, t->pending, t->tw, t->th, fmt) >= 40 ? gpu_alloc(bytes) : NULL;
	if (data) {
		memcpy(data, blocks, bytes);
		gpu_free(t->data, t->bytes);
		t->data = data;
		t->bytes = bytes;
		t->fmt = fmt;
	}
	free(blocks);
}

static void drop_pending(void)
{
	xtex_t *p = get_texs(pending_id);
	if (p) {
		if (p->try_dxt)
			compress_picture(p);
		free(p->pending);
		p->pending = NULL;
	}
	pending_id = 0;
}

/* level 0 of a 32-bit or DXT texture back to linear RGBA */
static uint8_t *unswizzle(const xtex_t *t)
{
	if ((t->fmt != FMT_A8B8G8R8 && !fmt_dxt(t->fmt)) || !t->data)
		return NULL;
	if (fmt_dxt(t->fmt)) {
		uint8_t *rgba = malloc((size_t)t->tw * t->th * 4);
		if (rgba)
			DXT_DecodeImage(t->data, rgba, t->tw, t->th, dxt_block_bytes(t->fmt));
		return rgba;
	}
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

/* back to 32 bits */
static void decompress_picture(xtex_t *t)
{
	uint8_t *rgba = unswizzle(t);
	size_t bytes = level_bytes(t->tw, t->th, FMT_A8B8G8R8);
	uint8_t *data = rgba ? gpu_alloc(bytes) : NULL;
	if (data) {
		swizzle_rect(data, t->tw, t->th, rgba, t->tw, 0, 0, t->tw, t->th, FMT_A8B8G8R8);
		gpu_free(t->data, t->bytes);
		t->data = data;
		t->bytes = bytes;
		t->fmt = FMT_A8B8G8R8;
	}
	free(rgba);
}

static void upload_2d(xtex_t *t, const uint8_t *rgba, int w, int h, GLint internal, bool dynamic)
{
	int tw, th;
	uint8_t *img = prepare_rgba(rgba, w, h, 4096, &tw, &th);
	if (!img) {
		set_error(GL_OUT_OF_MEMORY);
		return;
	}
	/* new pixels for a texture that had some: an atlas or a moving picture */
	if (t->data && !t->cube)
		t->updated = true;
	free_tex_storage(t);
	t->cube = false;
	t->w = w, t->h = h, t->tw = tw, t->th = th;
	t->fmt = dynamic ? FMT_A8B8G8R8 : pick_format(img, tw * th, internal, false);
	t->levels = 1;
	t->mipped = false;
	t->bytes = level_bytes(tw, th, t->fmt);
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
	/* lightmaps (RGB), pictures under 512 x 512 (HUD sheets, small fonts) and
	 * textures updated before stay as they are; compress_picture also leaves
	 * coverage atlases */
	t->try_dxt = !dynamic && !t->updated && texcompress() && !(gl_xbox_texquality && gl_xbox_texquality->ivalue) &&
	             internal != GL_RGB8 && internal != GL_RGB && tw * th >= 512 * 512;
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
	size_t bytes = chain_bytes(nw, nh, levels, fmt);
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
		store_level(dst, w, h, cur, fmt);
		dst += level_bytes(w, h, fmt);
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
	t->levels = levels;
	t->mipped = levels > 1;
	t->try_dxt = false;
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
		t->fmt = gl_xbox_texquality && gl_xbox_texquality->ivalue ? FMT_A8B8G8R8
		         : texcompress()                                   ? FMT_DXT1
		                                                           : FMT_R5G6B5;
		t->levels = 1;
		size_t face_bytes = (level_bytes(tw, th, t->fmt) + 127) & ~(size_t)127;
		t->bytes = face_bytes * 6;
		t->data = gpu_alloc(t->bytes);
		if (!t->data) {
			free(img);
			return;
		}
	} else {
		/* writing over storage a frame in flight may still sample */
		gpu_done_with(t->usedframe);
	}
	size_t face_bytes = (level_bytes(tw, th, t->fmt) + 127) & ~(size_t)127;
	store_level(t->data + face_bytes * face, tw, th, img, t->fmt);
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
	state_gen++;
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
	state_gen++;
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
	state_gen++;
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

/* rows of RGBA (pitch in texels) into level 0 at x0,y0. DXT blocks the
 * rectangle touches are read back, patched and compressed again. */
static void write_rect(xtex_t *t, const uint8_t *rgba, int pitch, int x0, int y0, int rw, int rh)
{
	if (!fmt_dxt(t->fmt)) {
		swizzle_rect(t->data, t->tw, t->th, rgba, pitch, x0, y0, rw, rh, t->fmt);
		return;
	}
	int bb = dxt_block_bytes(t->fmt), bw = (t->tw + 3) / 4;
	uint8_t block[64], enc[DXT5_BLOCK_BYTES];
	for (int by = y0 / 4; by <= (y0 + rh - 1) / 4; by++)
		for (int bx = x0 / 4; bx <= (x0 + rw - 1) / 4; bx++) {
			uint8_t *b = t->data + ((size_t)by * bw + bx) * bb;
			memcpy(enc, b, (size_t)bb); /* one read of write-combined memory */
			if (bb == DXT1_BLOCK_BYTES)
				DXT_DecodeBlock1(enc, block);
			else
				DXT_DecodeBlock5(enc, block);
			for (int y = 0; y < 4; y++)
				for (int x = 0; x < 4; x++) {
					int tx = bx * 4 + x, ty = by * 4 + y;
					if (tx >= x0 && tx < x0 + rw && ty >= y0 && ty < y0 + rh)
						memcpy(block + (y * 4 + x) * 4, rgba + ((size_t)(ty - y0) * pitch + tx - x0) * 4, 4);
				}
			if (bb == DXT1_BLOCK_BYTES)
				DXT_EncodeBlock1(enc, block, 0);
			else
				DXT_EncodeBlock5(enc, block);
			memcpy(b, enc, (size_t)bb);
		}
}

static void BWGLAPI xb_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                       GLenum fmt, GLenum type, const void *pixels)
{
	state_gen++;
	xtex_t *t = bound_tex(target);
	const uint8_t *src = pixels;
	uint8_t *expanded = NULL;
	if (!t || !t->data || t->cube || level != 0 || type != GL_UNSIGNED_BYTE || !pixels || w <= 0 || h <= 0)
		return;
	if (fmt == GL_RGB)
		src = expanded = expand_rgb(pixels, w, h);
	if (!src)
		return;
	/* a picture that gets updated (a cinematic's frames) stays 32-bit: no
	 * compressing each update, and no compressing it from a stale copy */
	t->try_dxt = false;
	t->updated = true;
	gpu_done_with(t->usedframe);
	if (fmt_dxt(t->fmt) && !t->mipped)
		decompress_picture(t);
	if (t->pending && t->tw == t->w && t->th == t->h) {
		/* keep the copy that glGenerateMipmap works from up to date */
		for (int r = 0; r < h; r++)
			memcpy(t->pending + ((size_t)(y + r) * t->tw + x) * 4, src + (size_t)r * w * 4, (size_t)w * 4);
	}
	if (t->tw == t->w && t->th == t->h && x >= 0 && y >= 0 && x + w <= t->tw && y + h <= t->th) {
		write_rect(t, src, w, x, y, w, h);
	} else {
		/* the texture was resized: scale the rectangle and point-sample it */
		float sx = (float)t->tw / t->w, sy = (float)t->th / t->h;
		int x0 = BW_MAX((int)(x * sx), 0), y0 = BW_MAX((int)(y * sy), 0);
		int x1 = BW_MIN((int)((x + w) * sx + 0.999f), t->tw), y1 = BW_MIN((int)((y + h) * sy + 0.999f), t->th);
		uint8_t *patch = x1 > x0 && y1 > y0 ? malloc((size_t)(x1 - x0) * (y1 - y0) * 4) : NULL;
		if (patch) {
			for (int ty = y0; ty < y1; ty++)
				for (int tx = x0; tx < x1; tx++) {
					int ix = BW_CLAMP((int)((tx + 0.5f) / sx) - x, 0, w - 1);
					int iy = BW_CLAMP((int)((ty + 0.5f) / sy) - y, 0, h - 1);
					memcpy(patch + ((size_t)(ty - y0) * (x1 - x0) + tx - x0) * 4, src + ((size_t)iy * w + ix) * 4, 4);
				}
			write_rect(t, patch, x1 - x0, x0, y0, x1 - x0, y1 - y0);
			free(patch);
		}
	}
	free(expanded);
}

static void BWGLAPI xb_glTexParameteri(GLenum target, GLenum pname, GLint v)
{
	state_gen++;
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
	state_gen++;
	if (pname == GL_TEXTURE_MAX_ANISOTROPY)
		return; /* gl_xbox_aniso decides */
	xb_glTexParameteri(target, pname, (GLint)v);
}

static void BWGLAPI xb_glGenerateMipmap(GLenum target)
{
	state_gen++;
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

static void free_segs(xbuf_t *b)
{
	for (int i = 0; i < b->maxsegs; i++)
		if (b->segs[i].count)
			gpu_free(b->segs[i].cmds, b->segs[i].bytes); /* a frame in flight may call it */
	free(b->segs);
	b->segs = NULL;
	b->numsegs = b->maxsegs = 0;
}

static void release_storage(xbuf_t *b)
{
	free_segs(b);
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
	b->stream = stream;
	if (b->element && target == GL_ELEMENT_ARRAY_BUFFER && b->data && size && size <= b->alloc) {
		/* only the CPU reads indices, at draw time: the old storage can be
		 * overwritten (streamed indices come every draw) */
		free_segs(b);
		b->size = size;
		return true;
	}
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
		b->alloc = size;
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
	if (b->element)
		st_index += (size_t)size;
	else if (usage == GL_STREAM_DRAW)
		st_stream += (size_t)size;
}

static void BWGLAPI xb_glBufferSubData(GLenum target, GLintptr ofs, GLsizeiptr size, const void *data)
{
	xbuf_t *b = bound_buf(target);
	if (!b || !b->data || ofs < 0 || (size_t)(ofs + size) > b->size)
		return;
	if (b->element)
		free_segs(b);
	if (!b->element && (uint32_t)(framenum - b->usedframe) < 2) {
		/* the GPU may still read the old contents */
		if (ofs == 0 && (size_t)size == b->size) {
			/* all of it replaced (CPU-animated models, every frame): the ring */
			respecify(b, target, (size_t)size, true);
			st_replace += (size_t)size;
		} else {
			st_copies++;
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
	state_gen++;
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
	state_gen++;
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

static void BWGLAPI xb_glUseProgram(GLuint id) { state_gen++; st.prog = id; }

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
	if (!p || loc <= 0) {
		state_gen++;
		return;
	}
	/* a value the program has already: nothing to send again */
	const float *cur = NULL;
	int w = 1;
	switch (loc) {
	case U_COLOR: cur = p->color, w = 4; break;
	case U_TCMOD: cur = p->tcmod, w = 4; break;
	case U_FOG: cur = p->fog, w = 4; break;
	case U_VIEWORG: cur = p->vieworg, w = 3; break;
	case U_LIGHT_AMBIENT: cur = p->lamb, w = 3; break;
	case U_LIGHT_DIFFUSE: cur = p->ldiff, w = 3; break;
	case U_LIGHT_DIR: cur = p->ldir, w = 3; break;
	case U_LMSCALE: cur = &p->lmscale; break;
	case U_ALPHATEST: cur = &p->alphatest; break;
	case U_USEFULLBRIGHT: cur = &p->usefullbright; break;
	default: break;
	}
	if (cur && !memcmp(cur, v, sizeof(float) * (size_t)BW_MIN(n, w)))
		return;
	state_gen++;
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
	state_gen++;
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
	state_gen++;
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
	state_gen++;
	switch (cap) {
	case GL_BLEND: st.blend = false; break;
	case GL_DEPTH_TEST: st.depthtest = false; break;
	case GL_CULL_FACE: st.cull = false; break;
	case GL_SCISSOR_TEST: st.scissor = false; break;
	case GL_POLYGON_OFFSET_FILL: st.polyoffset = false; break;
	default: break;
	}
}

static void BWGLAPI xb_glBlendFunc(GLenum s, GLenum d) { state_gen++; st.bsrc = st.bsrca = s, st.bdst = st.bdsta = d; }
static void BWGLAPI xb_glBlendFuncSeparate(GLenum s, GLenum d, GLenum sa, GLenum da)
{
	state_gen++;
	st.bsrc = s, st.bdst = d, st.bsrca = sa, st.bdsta = da;
}
static void BWGLAPI xb_glDepthFunc(GLenum f) { state_gen++; st.depthfunc = f; }
static void BWGLAPI xb_glDepthMask(GLboolean f) { state_gen++; st.depthmask = f; }
static void BWGLAPI xb_glDepthRange(double n, double f)
{
	state_gen++;
	st.depthnear = (float)n, st.depthfar = (float)f;
}
static void BWGLAPI xb_glCullFace(GLenum m) { state_gen++; st.cullface = m; }
static void BWGLAPI xb_glPolygonOffset(GLfloat factor, GLfloat units)
{
	state_gen++;
	st.offsetfactor = factor, st.offsetunits = units;
}
static void BWGLAPI xb_glLineWidth(GLfloat w) { (void)w; }
static void BWGLAPI xb_glPixelStorei(GLenum p, GLint v) { (void)p, (void)v; }

static void BWGLAPI xb_glFinish(void)
{
	pb_kick();
	while (pb_busy())
		;
}

static void BWGLAPI xb_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	state_gen++;
	st.colormask = (r ? NV097_SET_COLOR_MASK_RED_WRITE_ENABLE : 0) | (g ? NV097_SET_COLOR_MASK_GREEN_WRITE_ENABLE : 0) |
	               (b ? NV097_SET_COLOR_MASK_BLUE_WRITE_ENABLE : 0) | (a ? NV097_SET_COLOR_MASK_ALPHA_WRITE_ENABLE : 0);
}

static void BWGLAPI xb_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
	state_gen++;
	st.vp[0] = x, st.vp[1] = y, st.vp[2] = w, st.vp[3] = h;
}

static void BWGLAPI xb_glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
	state_gen++;
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

static void bind_unit(int unit, xtex_t *t)
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
	t->usedframe = framenum;
	reg(NV097_SET_TEXTURE_OFFSET + base, (uint32_t)t->data & 0x03FFFFFF);
	reg(NV097_SET_TEXTURE_FORMAT + base, format);
	reg(NV097_SET_TEXTURE_ADDRESS + base, address);
	reg(NV097_SET_TEXTURE_CONTROL0 + base, control0);
	reg(NV097_SET_TEXTURE_FILTER + base, tex_filter(t));
}

static xtex_t *tex2d(GLuint id)
{
	xtex_t *t = get_texs(id);
	if (t && t->try_dxt) {
		/* drawn without mipmaps: the picture is final */
		compress_picture(t);
		if (pending_id == id)
			drop_pending();
	}
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

/* --- skinning on the GPU ----------------------------------------------------------------- */

/* Models whose vertices each follow one bone are posed by a vertex program
 * instead of on the CPU: the renderer hands over the bones' matrices
 * (XBGL_SkinBones), and draws its rest-pose vertices with the bone's index
 * times 3 in the first lightmap coordinate. The program does what the
 * fixed-function path does for models (set_lighting, set_fog): the eye
 * space is the world around the viewer, one infinite light plus ambient,
 * and the radial eye distance for the hardware's fog.
 *
 * The palette is three rows a bone from constant 0 up, which in the
 * privileged range is where the fixed-function pipeline keeps its
 * matrices, viewport and texgen planes: after skinned draws they are sent
 * again (skin_restore). The program's own constants are c[84..95], the
 * top of the range. Source, for nxdk's vp20compiler:
 *
 *   !!VP1.1
 *   ARL A0.x, v[TEX1].x;                    bone x 3, from the lightmap st
 *   DP4 R0.x, c[A0.x+0], v[OPOS];           the posed position, in the model
 *   DP4 R0.y, c[A0.x+1], v[OPOS];
 *   DP4 R0.z, c[A0.x+2], v[OPOS];
 *   DP3 R1.x, c[A0.x+0], v[NRML];           and normal
 *   DP3 R1.y, c[A0.x+1], v[NRML];
 *   DP3 R1.z, c[A0.x+2], v[NRML];
 *   DPH R2.x, R0, c[84];                    eye space: model moved by -vieworg
 *   DPH R2.y, R0, c[85];
 *   DPH R2.z, R0, c[86];
 *   DP3 R4.x, R1, c[84];
 *   DP3 R4.y, R1, c[85];
 *   DP3 R4.z, R1, c[86];
 *   DPH R6.x, R0, c[87];                    viewport x projection x model
 *   DPH R6.y, R0, c[88];
 *   DPH R6.z, R0, c[89];
 *   DPH R6.w, R0, c[90];
 *   DP3 R2.w, R2, R2;
 *   DP3 R4.w, R4, R4;
 *   RSQ R3.x, R2.w;
 *   RSQ R4.w, R4.w;
 *   RCC R7.x, R6.w;
 *   RCP R3.y, R3.x;                         the eye distance, for fog
 *   MUL R4.xyz, R4, R4.w;                   the unit normal
 *   MUL o[HPOS].xyz, R6, R7.x;              screen space, as the hardware wants
 *   MOV o[HPOS].w, R6.w;
 *   MOV o[FOGC].x, R3.y;
 *   DP3 R5.x, R4, c[91];                    N.L
 *   MAX R5.x, R5.x, c[95].z;
 *   MUL R5, c[92], R5.x;
 *   ADD o[COL0], R5, c[93];                 diffuse x N.L + ambient, alpha 1
 *   MUL R8, v[TEX0], c[94];                 the texture coordinates' tcmod
 *   ADD o[TEX0], R8, c[95];
 *   ADD o[TEX2], R8, c[95];                 the fullbright texture's
 *   END
 *
 * The compiler can only address the user range relative to A0 (c[A0+0] is
 * constant 96), so skin_init moves those reads down to constant 0. */
static const uint32_t skin_program[] = {
	0x00000000, 0x01a01400, 0x0836106c, 0x207007f8, 0x00000000, 0x00ec001b, 0x0c36106c, 0x280007fa,
	0x00000000, 0x00ec201b, 0x0c36106c, 0x240007fa, 0x00000000, 0x00ec401b, 0x0c36106c, 0x220007fa,
	0x00000000, 0x00ac041b, 0x0c36106c, 0x281007fa, 0x00000000, 0x00ac241b, 0x0c36106c, 0x241007fa,
	0x00000000, 0x00ac441b, 0x0c36106c, 0x221007fa, 0x00000000, 0x00d6801b, 0x0436186c, 0x282007f8,
	0x00000000, 0x00d6a01b, 0x0436186c, 0x242007f8, 0x00000000, 0x00d6c01b, 0x0436186c, 0x222007f8,
	0x00000000, 0x00b6801b, 0x1436186c, 0x284007f8, 0x00000000, 0x00b6a01b, 0x1436186c, 0x244007f8,
	0x00000000, 0x00b6c01b, 0x1436186c, 0x224007f8, 0x00000000, 0x00d6e01b, 0x0436186c, 0x286007f8,
	0x00000000, 0x00d7001b, 0x0436186c, 0x246007f8, 0x00000000, 0x00d7201b, 0x0436186c, 0x226007f8,
	0x00000000, 0x00d7401b, 0x0436186c, 0x216007f8, 0x00000000, 0x00a0001b, 0x2436486c, 0x212007f8,
	0x00000000, 0x00a0001b, 0x4436886c, 0x214007f8, 0x00000000, 0x0800001b, 0x08361300, 0x903807f8,
	0x00000000, 0x0800001b, 0x08361301, 0x104107f8, 0x00000000, 0x0600001b, 0x08361301, 0x907807f8,
	0x00000000, 0x0400001b, 0x08361000, 0xd03407f8, 0x00000000, 0x0040001b, 0x45fe886c, 0x2e4007f8,
	0x00000000, 0x0040001b, 0x6400e86c, 0x2070e800, 0x00000000, 0x002000ff, 0x6436106c, 0x20701800,
	0x00000000, 0x00200055, 0x3436106c, 0x20708828, 0x00000000, 0x00b7601b, 0x4436186c, 0x285007f8,
	0x00000000, 0x0157e000, 0x5554186c, 0x285007f8, 0x00000000, 0x0057801b, 0x0c00a86c, 0x2f5007f8,
	0x00000000, 0x0077a01b, 0x5436106c, 0x3070f818, 0x00000000, 0x0057d21b, 0x0836186c, 0x2f8007f8,
	0x00000000, 0x0077e01b, 0x8436106c, 0x3070f848, 0x00000000, 0x0077e01b, 0x8436106c, 0x3070f859,
};
#define SKIN_MAXBONES 60 /* palette rows 0..179, below the program's constants */
#define SKIN_CONSTS 180  /* c[84] in the privileged range */
#define EXEC_FIXED 4     /* fixed function, privileged range */
#define EXEC_PROGRAM 6   /* the program, privileged range */

static bool skin_on;      /* XBGL_SkinBones gave a palette for the draws that follow */
static bool skin_prog;    /* the transform engine runs the program */
static bool skin_clobber; /* the fixed-function matrices and viewport were overwritten */

/* loads the program, with its palette reads moved to constant 0: an
 * instruction's constant is bits 13-20 of its second word, and bit 1 of
 * its fourth says the read is relative to A0 */
static void skin_init(void)
{
	int n = (int)BW_ARRAY_COUNT(skin_program);
	pb_open((size_t)n + 64);
	put1(NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
	put1(NV097_SET_TRANSFORM_PROGRAM_LOAD, 0);
	for (int i = 0; i < n; i += 32) {
		int k = BW_MIN(32, n - i);
		cmd(NV097_SET_TRANSFORM_PROGRAM, (uint32_t)k);
		for (int j = 0; j < k; j++) {
			uint32_t w = skin_program[i + j];
			if (((i + j) & 3) == 1 && (skin_program[i + j + 2] & 2))
				w -= 96u << 13;
			*P++ = w;
		}
	}
	pb_close();
}

/* the fixed-function state the palette overwrote, sent again */
static void skin_restore(void)
{
	float zero4[4] = {0, 0, 0, 0}, one4[4] = {1, 1, 1, 1};
	invalidate_hw();
	cmd(NV097_SET_VIEWPORT_OFFSET, 4);
	memcpy(P, zero4, 16), P += 4;
	cmd(NV097_SET_VIEWPORT_SCALE, 4);
	memcpy(P, one4, 16), P += 4;
	skin_clobber = false;
}

/* the transform engine's mode for a draw: the program for skinned models */
static void skin_mode(bool prog)
{
	if (prog != skin_prog) {
		put1(NV097_SET_TRANSFORM_EXECUTION_MODE, prog ? EXEC_PROGRAM : EXEC_FIXED);
		skin_prog = prog;
	}
	if (!prog && skin_clobber)
		skin_restore();
}

bool XBGL_SkinBones(const float (*bones)[12], int n)
{
	if (!bones || n <= 0) {
		if (skin_on)
			state_gen++;
		skin_on = false;
		return false;
	}
	if (!ready || n > SKIN_MAXBONES || (gl_xbox_gpuskin && !gl_xbox_gpuskin->ivalue))
		return false;
	pb_open((size_t)n * 12 + (size_t)n / 2 + 16);
	put1(NV097_SET_TRANSFORM_CONSTANT_LOAD, 0);
	for (int i = 0; i < n * 12; i += 32) {
		int k = BW_MIN(32, n * 12 - i);
		cmd(NV097_SET_TRANSFORM_CONSTANT, (uint32_t)k);
		memcpy(P, bones[0] + i, sizeof(float) * k);
		P += k;
	}
	pb_close();
	skin_clobber = skin_on = true;
	state_gen++;
	return true;
}

/* the program's constants for model program p, from the same uniforms the
 * fixed-function path reads */
static void skin_constants(const xprog_t *p, const float *comp)
{
	float c[12][4], mv[16];
	memcpy(mv, p->model, sizeof(mv));
	mv[12] -= p->vieworg[0], mv[13] -= p->vieworg[1], mv[14] -= p->vieworg[2];
	for (int r = 0; r < 3; r++)
		for (int k = 0; k < 4; k++)
			c[r][k] = mv[k * 4 + r];
	memcpy(c[3], comp, sizeof(float) * 16);
	float len = sqrtf(p->ldir[0] * p->ldir[0] + p->ldir[1] * p->ldir[1] + p->ldir[2] * p->ldir[2]);
	c[7][0] = 0, c[7][1] = 0, c[7][2] = 1, c[7][3] = 0;
	if (len > 1e-6f)
		c[7][0] = p->ldir[0] / len, c[7][1] = p->ldir[1] / len, c[7][2] = p->ldir[2] / len;
	for (int k = 0; k < 3; k++) {
		c[8][k] = p->ldiff[k] * 0.5f;
		c[9][k] = p->lamb[k] * 0.5f;
	}
	c[8][3] = 0, c[9][3] = 1;
	c[10][0] = p->tcmod[2], c[10][1] = p->tcmod[3], c[10][2] = 1, c[10][3] = 1;
	c[11][0] = p->tcmod[0], c[11][1] = p->tcmod[1], c[11][2] = 0, c[11][3] = 0;
	put1(NV097_SET_TRANSFORM_CONSTANT_LOAD, SKIN_CONSTS);
	cmd(NV097_SET_TRANSFORM_CONSTANT, 32);
	memcpy(P, c, 32 * 4), P += 32;
	cmd(NV097_SET_TRANSFORM_CONSTANT, 16);
	memcpy(P, c[8], 16 * 4), P += 16;
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
	skin_mode(lit && skin_on);
	if (skin_prog) {
		skin_constants(p, comp);
		lit = false; /* the program lights the vertices */
	} else {
		push_matrix(NV097_SET_COMPOSITE_MATRIX, &mc_composite, comp);
	}
	if ((fog || lit) && !skin_prog) {
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

/* The renderer's attributes (pos, tc, lightmap tc, normal, colour) and
 * the NV2A's vertex slots they feed; the fullbright texture (slot 11)
 * reads a_tc again. Each attribute can come from its own buffer with its
 * own stride (skinned models stream positions and normals alone); a
 * disabled one reads rvert_t's layout from the position's buffer. */
#define NATTRS 5
static const int attr_slot[NATTRS] = {0, 9, 10, 2, 3};

typedef struct {
	xbuf_t *b;
	size_t ofs;
	uint32_t stride, format;
} vsrc_t;

/* where each attribute comes from; false if a draw can't read them up to vertex hi */
static bool vertex_sources(vsrc_t *src, uint32_t hi, bool *ring)
{
	static const size_t offsets[NATTRS] = {0, 12, 20, 28, 40};
	static const uint32_t defformats[NATTRS] = {2 | (3 << 4), 2 | (2 << 4), 2 | (2 << 4), 2 | (3 << 4), 4 | (4 << 4)};
	xvao_t *v = cur_vao();
	if (!v->a[0].enabled)
		return false;
	*ring = false;
	for (int i = 0; i < NATTRS; i++) {
		xattr_t *a = &v->a[i];
		vsrc_t *o = &src[i];
		size_t bytes;
		if (a->enabled) {
			uint32_t type = a->type == GL_FLOAT ? 2 : a->type == GL_UNSIGNED_BYTE ? 4 : 0;
			if (!type || a->size < 1 || a->size > 4) {
				static bool warned;
				if (!warned)
					Con_Printf("^3gl_xbox: a vertex attribute type other than float or bytes; draws skipped\n");
				warned = true;
				return false;
			}
			bytes = (size_t)a->size * (type == 2 ? 4 : 1);
			o->b = get_bufs(a->buf);
			o->ofs = a->ofs;
			o->stride = a->stride ? (uint32_t)a->stride : (uint32_t)bytes;
			o->format = type | ((uint32_t)a->size << 4);
		} else {
			/* a program that doesn't read it */
			o->b = src[0].b;
			o->ofs = v->a[0].ofs + offsets[i];
			o->stride = RVERT_STRIDE;
			o->format = defformats[i];
			bytes = 4;
		}
		o->format |= o->stride << 8;
		if (!o->b || !o->b->data || o->b->element)
			return false;
		if (o->b->ring && o->b->ringframe != framenum)
			return false; /* streamed in an earlier frame: gone */
		if (o->ofs + (size_t)hi * o->stride + bytes > o->b->size)
			return false; /* past the buffer */
		*ring |= o->b->ring;
	}
	return true;
}

/* the arrays' formats and where vertex lo of the draw is */
static void set_arrays(const vsrc_t *src, uint32_t lo)
{
	uint32_t f[16], addr[NATTRS];
	for (int i = 0; i < 16; i++)
		f[i] = 2; /* float, size 0: off */
	for (int i = 0; i < NATTRS; i++) {
		f[attr_slot[i]] = src[i].format;
		addr[i] = (uint32_t)(src[i].b->data + src[i].ofs + (size_t)lo * src[i].stride) & 0x03FFFFFF;
	}
	f[11] = src[1].format;
	regs(NV097_SET_VERTEX_DATA_ARRAY_FORMAT, f, 16);
	for (int i = 0; i < NATTRS; i++)
		reg(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + attr_slot[i] * 4, addr[i]);
	reg(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 11 * 4, addr[1]);
}

static int draws_this_frame, tris_this_frame;

/* the bounds of the indices a draw uses */
static void index_range(int count, const uint32_t *idx32, const uint16_t *idx16, uint32_t *lo, uint32_t *hi)
{
	uint32_t l = UINT32_MAX, h = 0;
	if (idx32) {
		for (int i = 0; i < count; i++)
			l = BW_MIN(l, idx32[i]), h = BW_MAX(h, idx32[i]);
	} else {
		for (int i = 0; i < count; i++)
			l = BW_MIN(l, idx16[i]), h = BW_MAX(h, idx16[i]);
	}
	*lo = l, *hi = h;
}

/* the dwords put_indices may write */
static size_t index_words(int count, bool wide)
{
	return (wide ? (size_t)count : (size_t)count / 2 + 1) + (size_t)count / 2047 + 2;
}

/* the commands that send indices, rebased to lo: 16 bits each when the
 * range allows, two a word, an odd last one alone */
static uint32_t *put_indices(uint32_t *p, int count, const uint32_t *idx32, const uint16_t *idx16, uint32_t lo,
                             bool wide)
{
	if (!wide) {
		int pairs = count / 2, i = 0;
		while (pairs > 0) {
			int words = BW_MIN(pairs, 2047);
			*p++ = 0x40000000u | ((uint32_t)words << 18) | NV097_ARRAY_ELEMENT16;
			if (idx16) {
				for (int w = 0; w < words; w++, i += 2)
					*p++ = (uint32_t)(idx16[i] - lo) | ((uint32_t)(idx16[i + 1] - lo) << 16);
			} else {
				for (int w = 0; w < words; w++, i += 2)
					*p++ = (idx32[i] - lo) | ((idx32[i + 1] - lo) << 16);
			}
			pairs -= words;
		}
		if (count & 1) {
			*p++ = (1u << 18) | NV097_ARRAY_ELEMENT32;
			*p++ = (idx32 ? idx32[count - 1] : idx16[count - 1]) - lo;
		}
	} else {
		int left = count, i = 0;
		while (left > 0) {
			int words = BW_MIN(left, 2047);
			*p++ = 0x40000000u | ((uint32_t)words << 18) | NV097_ARRAY_ELEMENT32;
			for (int w = 0; w < words; w++, i++)
				*p++ = (idx32 ? idx32[i] : idx16[i]) - lo;
			left -= words;
		}
	}
	return p;
}

/* A static index buffer's runs are made into commands in GPU memory the
 * first time they are drawn; the stream then calls them, so drawing the
 * world or a model doesn't read and copy its indices again. They live in
 * a hash by offset and count. */
static iseg_t *seg_slot(iseg_t *segs, int max, size_t ofs, int count, bool u32)
{
	uint32_t i = ((uint32_t)ofs * 2654435761u ^ (uint32_t)count * 40503u) & (uint32_t)(max - 1);
	while (segs[i].count && !(segs[i].ofs == ofs && segs[i].count == count && segs[i].u32 == u32))
		i = (i + 1) & (uint32_t)(max - 1);
	return &segs[i];
}

static iseg_t *find_seg(xbuf_t *b, size_t ofs, int count, const uint32_t *idx32, const uint16_t *idx16)
{
	iseg_t *g;
	if (b->maxsegs && (g = seg_slot(b->segs, b->maxsegs, ofs, count, idx32 != NULL))->count)
		return g;
	if (b->numsegs * 2 >= b->maxsegs) {
		/* grow the hash */
		int max = b->maxsegs ? b->maxsegs * 2 : 64;
		iseg_t *segs = calloc((size_t)max, sizeof(iseg_t));
		if (!segs)
			return NULL;
		for (int i = 0; i < b->maxsegs; i++)
			if (b->segs[i].count)
				*seg_slot(segs, max, b->segs[i].ofs, b->segs[i].count, b->segs[i].u32) = b->segs[i];
		free(b->segs);
		b->segs = segs, b->maxsegs = max;
	}
	uint32_t lo, hi;
	index_range(count, idx32, idx16, &lo, &hi);
	/* runs of one buffer that all fit 16 bits share the vertex arrays'
	 * offsets, so draws in a row (the world's surfaces) send none */
	if (hi <= 0xFFFF)
		lo = 0;
	bool wide = hi - lo > 0xFFFF;
	uint32_t bytes = (uint32_t)(index_words(count, wide) + 1) * 4;
	uint32_t *cmds = gpu_alloc(bytes);
	if (!cmds)
		return NULL;
	uint32_t *e = put_indices(cmds, count, idx32, idx16, lo, wide);
	*e = 0x00020000; /* return */
	g = seg_slot(b->segs, b->maxsegs, ofs, count, idx32 != NULL);
	*g = (iseg_t){ofs, count, idx32 != NULL, wide, lo, hi, cmds, bytes};
	b->numsegs++;
	return g;
}

/* the fixed state and program for a draw, sent when anything changed
 * since the last; false if the program draws nothing */
static bool draw_state(void)
{
	static uint32_t drawn_gen;
	static int drawn_aniso;
	static bool drawn_ok;
	int aniso = gl_xbox_aniso ? gl_xbox_aniso->ivalue : 0;
	if (state_gen != drawn_gen || aniso != drawn_aniso) {
		set_fixed_state();
		drawn_ok = set_program();
		drawn_gen = state_gen, drawn_aniso = aniso;
	}
	return drawn_ok;
}

/* eb: the element buffer the indices are in, at byte ofs, or NULL */
static void draw(GLenum mode, int count, const uint32_t *idx32, const uint16_t *idx16, int first, xbuf_t *eb,
                 size_t ofs)
{
	vsrc_t src[NATTRS];
	bool ring, wide = false;
	uint32_t lo, hi;
	iseg_t *seg = NULL;
	if (mode > GL_TRIANGLE_FAN || count <= 0 || !ready)
		return;
	if (eb && !eb->stream) {
		seg = find_seg(eb, ofs, count, idx32, idx16);
	}
	if (seg) {
		lo = seg->lo, hi = seg->hi;
	} else if (idx32 || idx16) {
		index_range(count, idx32, idx16, &lo, &hi);
		wide = hi - lo > 0xFFFF;
	} else {
		lo = (uint32_t)first, hi = (uint32_t)(first + count - 1);
	}
	if (!vertex_sources(src, hi, &ring))
		return;

	pb_open(700 + (seg || !(idx32 || idx16) ? (size_t)count / 256 : index_words(count, wide)));
	if (!draw_state()) {
		pb_close();
		return;
	}
	set_arrays(src, lo);
	if (ring)
		put1(NV097_BREAK_VERTEX_BUFFER_CACHE, 0);

	put1(NV097_SET_BEGIN_END, mode + 1); /* GL's primitives, one up */
	if (seg) {
		*P++ = ((uint32_t)seg->cmds & 0x03FFFFFF) | 2; /* call */
	} else if (!idx32 && !idx16) {
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
	} else {
		P = put_indices(P, count, idx32, idx16, lo, wide);
	}
	put1(NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
	pb_close();
	for (int i = 0; i < NATTRS; i++)
		src[i].b->usedframe = framenum;
	draws_this_frame++;
	if (mode >= GL_TRIANGLES)
		tris_this_frame += mode == GL_TRIANGLES ? count / 3 : count - 2;
}

static void BWGLAPI xb_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	draw(mode, count, NULL, NULL, first, NULL, 0);
}

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
		draw(mode, count, (const uint32_t *)(eb->data + o), NULL, 0, eb, o);
	else if (type == GL_UNSIGNED_SHORT)
		draw(mode, count, NULL, (const uint16_t *)(eb->data + o), 0, eb, o);
}

static void BWGLAPI xb_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *ofs)
{
	xb_glDrawElementsBaseVertex(mode, count, type, ofs, 0);
}

/* triangle runs of a static index buffer whose commands start at vertex 0
 * (find_seg), called one after another inside one begin and end: the
 * world's surfaces of one texture and lightmap are one draw */
typedef struct {
	const uint32_t *cmds;
	int count;
} run_t; /* a copy: finding the next run may move the hash its iseg_t is in */

static void draw_runs(const run_t *runs, int n, uint32_t hi)
{
	vsrc_t src[NATTRS];
	bool ring;
	if (!vertex_sources(src, hi, &ring))
		return;
	pb_open(700 + (size_t)n);
	if (!draw_state()) {
		pb_close();
		return;
	}
	set_arrays(src, 0);
	if (ring)
		put1(NV097_BREAK_VERTEX_BUFFER_CACHE, 0);
	put1(NV097_SET_BEGIN_END, GL_TRIANGLES + 1);
	for (int i = 0; i < n; i++) {
		*P++ = ((uint32_t)runs[i].cmds & 0x03FFFFFF) | 2; /* call */
		tris_this_frame += runs[i].count / 3;
	}
	put1(NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
	pb_close();
	for (int i = 0; i < NATTRS; i++)
		src[i].b->usedframe = framenum;
	draws_this_frame++;
}

static void BWGLAPI xb_glMultiDrawElements(GLenum mode, const GLsizei *count, GLenum type, const void *const *ofs,
                                           GLsizei n)
{
	enum { MAXRUNS = 256 };
	run_t runs[MAXRUNS];
	int nr = 0;
	uint32_t hi = 0;
	xbuf_t *eb = get_bufs(cur_vao()->ebo);
	size_t sz = type == GL_UNSIGNED_INT ? 4 : 2;
	if (mode != GL_TRIANGLES || !ready || !eb || !eb->data || eb->stream ||
	    (type != GL_UNSIGNED_INT && type != GL_UNSIGNED_SHORT)) {
		for (int i = 0; i < n; i++)
			xb_glDrawElements(mode, count[i], type, ofs[i]);
		return;
	}
	for (int i = 0; i < n; i++) {
		size_t o = (size_t)ofs[i];
		iseg_t *g = NULL;
		if (count[i] > 0 && count[i] % 3 == 0 && o + sz * (size_t)count[i] <= eb->size)
			g = find_seg(eb, o, count[i], sz == 4 ? (const uint32_t *)(eb->data + o) : NULL,
			             sz == 2 ? (const uint16_t *)(eb->data + o) : NULL);
		if (g && g->lo == 0 && nr < MAXRUNS) {
			runs[nr++] = (run_t){g->cmds, g->count};
			hi = BW_MAX(hi, g->hi);
			continue;
		}
		/* in order: what is gathered first, then this one alone */
		if (nr)
			draw_runs(runs, nr, hi);
		nr = 0, hi = 0;
		if (g && g->lo == 0)
			runs[nr++] = (run_t){g->cmds, g->count}, hi = g->hi;
		else
			xb_glDrawElements(mode, count[i], type, ofs[i]);
	}
	if (nr)
		draw_runs(runs, nr, hi);
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
	pb_kick();
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
	{"glMultiDrawElements", (void *)xb_glMultiDrawElements},
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
	put1(NV097_SET_TRANSFORM_EXECUTION_MODE, EXEC_FIXED);
	skin_prog = false;
	put1(NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);
	put1(NV097_SET_SKIN_MODE, NV097_SET_SKIN_MODE_OFF);
	put1(NV097_SET_FRONT_FACE, NV097_SET_FRONT_FACE_V_CW); /* GL's counter-clockwise, after the y flip */
	put1(NV097_SET_LIGHT_LOCAL_RANGE, 0x7149f2ca);         /* 1e30 */
	pb_close();
}

static ULONGLONG stat_start; /* when the frame's work began (the end of the last flip) */
static struct {
	int frames, missed, draws, tris, kicks, waits;
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
		fst.draws += draws_this_frame, fst.tris += tris_this_frame, fst.kicks += kicks, fst.waits += gpu_waits;
		if (pbused > fst.pb)
			fst.pb = pbused;
	}
	stat_start = t1;
	DWORD now = GetTickCount();
	if (!fst.since)
		fst.since = now;
	if (now - fst.since >= 5000 && fst.frames) {
		double secs = (now - fst.since) / 1000.0;
		Con_Printf("xbox: %.1f fps, frame work %.1f ms (worst %.1f, GPU sync %.1f), wait %.1f ms, %d late, %d draws, %d tris, "
		           "%d kicks, pb %u of %u KB, ring %u of %u KB (%u KB spilled), GPU %u KB, %u KB free\n",
		           fst.frames / secs, fst.work / fst.frames, fst.worst, (double)sync_ticks * ms / fst.frames,
		           fst.wait / fst.frames, fst.missed, fst.draws / fst.frames, fst.tris / fst.frames, fst.kicks / fst.frames,
		           (unsigned)(fst.pb * 4 / 1024), PB_SIZE >> 10, (unsigned)(ringpeak >> 10), RING_HALF >> 10,
		           (unsigned)(ringspill >> 10), (unsigned)(gpu_used >> 10), (unsigned)(XBX_FreeMemory() >> 10));
		Con_Printf("xbox: a frame streams %u KB draws, %u KB replaced buffers, %u KB indices; %u buffer copies, "
		           "%d texture update waits\n",
		           (unsigned)(st_stream / fst.frames >> 10), (unsigned)(st_replace / fst.frames >> 10),
		           (unsigned)(st_index / fst.frames >> 10), (unsigned)(st_copies / fst.frames), fst.waits);
		st_stream = st_replace = st_index = st_copies = 0;
		sync_ticks = 0;
		memset(&fst, 0, sizeof(fst));
		ringpeak = ringspill = 0;
		fst.since = now;
	}
}

void XBGL_EndFrame(void)
{
	if (!ready)
		return;
	/* timing for gl_xbox_stats: the frame's work, then the wait for the GPU and the vblank */
	ULONGLONG t0 = KeQueryPerformanceCounter();
	pb_kick();
	size_t pbused = (size_t)(stream_pos(P) - pbframe);
	/* frame pacing: flips land every vid_xbox_swapinterval vblanks */
	int interval = vid_xbox_swapinterval ? BW_CLAMP(vid_xbox_swapinterval->ivalue, 0, 4) : 2;
	DWORD now = pb_get_vbl_counter();
	while ((int)(now - last_swap) < interval)
		now = pb_wait_for_vbl();
	int missed = (int)(now - last_swap) - interval;
	last_swap = now;
	pb_open(0);
	pb_kick();
	while (pb_finished())
		; /* every back buffer is waiting to be shown */
	pb_resync();
	frame_end[framenum & 1] = stream_pos(P);
	if (gl_xbox_stats && gl_xbox_stats->ivalue)
		frame_stats(t0, pbused, missed);
	/* the frame before this one must be done before the next reuses its storage */
	wait_done(frame_end[(framenum - 1) & 1]);
	pbframe = stream_pos(P);
	framenum++;
	ringused = 0;
	run_deferred(false);
	after_target();
	draws_this_frame = tris_this_frame = kicks = gpu_waits = 0;
}

int XBGL_Width(void) { return fbw; }
int XBGL_Height(void) { return fbh; }

static void make_fallback_textures(void)
{
	memset(&whitetex, 0, sizeof(whitetex));
	whitetex.tw = whitetex.th = whitetex.w = whitetex.h = 4;
	whitetex.fmt = FMT_A8B8G8R8, whitetex.levels = 1;
	whitetex.minf = whitetex.magf = GL_NEAREST, whitetex.wraps = whitetex.wrapt = GL_REPEAT;
	whitetex.bytes = 64;
	whitetex.data = gpu_alloc(64);
	if (whitetex.data)
		memset(whitetex.data, 0xFF, 64);
	memset(&blackcube, 0, sizeof(blackcube));
	blackcube.cube = true;
	blackcube.tw = blackcube.th = blackcube.w = blackcube.h = 4;
	blackcube.fmt = FMT_A8B8G8R8, blackcube.levels = 1;
	blackcube.minf = blackcube.magf = GL_NEAREST;
	blackcube.bytes = 128 * 6;
	blackcube.data = gpu_alloc(blackcube.bytes);
	if (blackcube.data)
		for (int i = 0; i < 6; i++)
			for (int j = 0; j < 16; j++)
				((uint32_t *)(blackcube.data + i * 128))[j] = 0xFF000000u;
}

/* where the GPU's memory is: textures by size class, the biggest by name, buffers */
void XBGL_PrintGpuMemory(void)
{
	size_t tex = 0, mipped = 0, cube = 0, buf = 0;
	int ntex = 0, top[8] = {0};
	for (int i = 1; i < num_texs; i++) {
		xtex_t *t = &texs[i];
		if (!t->used || !t->data)
			continue;
		ntex++;
		tex += t->bytes;
		if (t->mipped)
			mipped += t->bytes;
		if (t->cube)
			cube += t->bytes;
		for (int k = 0; k < 8; k++)
			if (!top[k] || t->bytes > texs[top[k]].bytes) {
				memmove(top + k + 1, top + k, sizeof(int) * (7 - k));
				top[k] = i;
				break;
			}
	}
	for (int i = 1; i < num_bufs; i++)
		if (bufs[i].used && !bufs[i].element && !bufs[i].ring)
			buf += bufs[i].alloc;
	Con_Printf("GPU objects: %u KB in %d pool chunks: %d textures %u KB (mipmapped %u KB, cube %u KB), buffers %u KB\n",
	           (unsigned)(gpu_used >> 10), numchunks, ntex, (unsigned)(tex >> 10), (unsigned)(mipped >> 10),
	           (unsigned)(cube >> 10), (unsigned)(buf >> 10));
	for (int k = 0; k < 8 && top[k]; k++) {
		xtex_t *t = &texs[top[k]];
		Con_Printf("  texture %d: %dx%d as %dx%d, %d levels, %d bits/texel%s, %u KB\n", top[k], t->w, t->h, t->tw, t->th,
		           t->levels, fmt_bits(t->fmt), t->cube ? ", cube" : "", (unsigned)(t->bytes >> 10));
	}
}

static void meminfo_gpu_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	XBGL_PrintGpuMemory();
	Con_Printf("last frame %d draws, %d triangles\n", draws_this_frame, tris_this_frame);
}

void XBGL_RegisterCvars(void)
{
	gl_max_size = Cvar_Get("gl_max_size", "512", CVAR_ARCHIVE,
	                       "Largest side of a world texture on the GPU; bigger ones are scaled down when loaded (Xbox)");
	gl_xbox_texquality = Cvar_Get("gl_xbox_texquality", "0", CVAR_ARCHIVE,
	                              "1 keeps world textures at 32 bits; 0 stores them as 16-bit, half the memory (Xbox)");
	gl_xbox_texcompress = Cvar_Get("gl_xbox_texcompress", "1", CVAR_ARCHIVE,
	                               "1 stores opaque world textures and the sky as DXT1 (a quarter of 16 bits) and "
	                               "big pictures as DXT when that is near-lossless; 0 keeps 16 and 32 bits (Xbox)");
	gl_xbox_mipmaps = Cvar_Get("gl_xbox_mipmaps", "1", CVAR_ARCHIVE, "Make mipmaps for textures that ask for them (Xbox)");
	gl_xbox_aniso = Cvar_Get("gl_xbox_aniso", "1", CVAR_ARCHIVE,
	                         "Anisotropic filtering of mipmapped textures: 0 off, 1 2x, 2 4x, 3 8x (Xbox)");
	vid_xbox_swapinterval = Cvar_Get("vid_xbox_swapinterval", "2", CVAR_ARCHIVE,
	                                 "Vertical blanks per frame: 1 for 60 fps, 2 for a steady 30 (Xbox)");
	gl_xbox_stats = Cvar_Get("gl_xbox_stats", "0", 0, "Print frame rate and timing every 5 seconds (Xbox)");
	gl_xbox_gpuskin = Cvar_Get("gl_xbox_gpuskin", "1", 0,
	                           "Pose models whose vertices follow one bone each on the GPU, not the CPU (Xbox)");
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
	pbbase = pb_begin();
	pbend = pbbase + PB_DWORDS;
	pblap = 0;
	pb_target_back_buffer();
	pb_resync();
	pbframe = stream_pos(P);
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
	skin_init();
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
