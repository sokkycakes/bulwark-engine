/* Bulwark engine - the renderer's OpenGL 3.3 subset on the 3DS GPU
 * (PICA200), through citro3d. ISC licence, see LICENSE.
 *
 * The renderer reaches GL only through the q* pointers of r_gl.h, uses one
 * vertex layout (rvert_t) and five GLSL programs (r_gl.c). This file
 * implements exactly that subset, so the whole renderer runs unchanged:
 *
 * - Programs are recognised from their GLSL text at link time and mapped
 *   to a PICA vertex shader (shaders/ctr_*.v.pica) plus a texture combiner
 *   (TEV) setup that does the fragment work. Dynamic lights and fog are
 *   not drawn.
 * - Textures are resampled to powers of two (world textures, the ones
 *   that ask for mipmaps, no larger than gl_max_size), stored as 16-bit
 *   texels unless gl_ctr_texquality asks for 32, and swizzled to the GPU's
 *   tiled, bottom-up layout. Mipmaps are made on the CPU.
 * - Buffers live in linear memory. The GPU reads them after the frame is
 *   submitted, so a buffer changed after a draw this frame used it gets
 *   new storage (the old one is freed once the GPU is done), and streamed
 *   buffers come from a ring that resets every frame.
 * - The GPU takes 16-bit indices only: each draw's 32-bit indices are
 *   rebased to their lowest vertex and copied into the ring.
 * - The top screen is portrait in memory: the projection is turned a
 *   quarter and its depth mapped to the GPU's [-1, 0] range, and viewports
 *   and scissors are turned to match.
 *
 * Not supported: framebuffer objects (CheckFramebufferStatus says
 * incomplete), glReadPixels (black), lines and points (dropped).
 * docs/platforms/3ds.md has the details. */
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/common.h"
#include "../../common/cvar.h"
#include "../../render/r_gl.h"
#include "bwctr_shaders_shbin.h"
#include "ctr.h"

#define FB_W CTR_HEIGHT /* the framebuffer is portrait: 240 wide, 400 tall */
#define FB_H CTR_WIDTH
#define RING_SIZE (3 << 20)
#define CMDBUF_SIZE 0x100000
#define RVERT_STRIDE 44 /* sizeof(rvert_t): pos 3f, st 2f, lm 2f, normal 3f, colour 4ub */

#define DISPLAY_TRANSFER_FLAGS                                                                                 \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                           \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |             \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static cvar_t *gl_max_size, *gl_ctr_texquality, *gl_ctr_mipmaps;

/* --- frames ------------------------------------------------------------------ */

static C3D_RenderTarget *target;
static bool inframe;
static uint32_t framenum = 1;
static bool ctr_ready;

/* storage the GPU may still read: freed once the next frame starts */
static void **deferred;
static int numdeferred, maxdeferred;

static void defer_free(void *p)
{
	if (!p)
		return;
	if (numdeferred == maxdeferred) {
		maxdeferred = maxdeferred ? maxdeferred * 2 : 64;
		deferred = realloc(deferred, sizeof(void *) * maxdeferred);
	}
	deferred[numdeferred++] = p;
}

static uint8_t *ring;
static size_t ringused;

static void begin_frame(void)
{
	if (inframe || !ctr_ready)
		return;
	/* waits for the previous frame's GPU work, so everything it read is free */
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C3D_FrameDrawOn(target);
	for (int i = 0; i < numdeferred; i++)
		linearFree(deferred[i]);
	numdeferred = 0;
	ringused = 0;
	inframe = true;
}

/* linear memory the GPU reads during this frame only */
static void *ring_alloc(size_t size)
{
	size = (size + 15) & ~(size_t)15;
	if (ringused + size <= RING_SIZE) {
		void *p = ring + ringused;
		ringused += size;
		return p;
	}
	/* the ring is full: one-off storage, freed after the frame */
	void *p = linearAlloc(size);
	defer_free(p);
	return p;
}

/* --- object tables ------------------------------------------------------------ */

typedef struct {
	bool used, allocated, cube, mipped, dynamic;
	C3D_Tex tex;
	C3D_TexCube faces;
	int w, h;   /* as GL was given it */
	int tw, th; /* on the GPU */
	GPU_TEXCOLOR fmt;
	GLenum minf, magf, wraps, wrapt;
	uint8_t *pending; /* level 0 (tw x th RGBA) kept until glGenerateMipmap or the next upload */
} ctex_t;

typedef struct {
	bool used;
	bool element;  /* data is in the normal heap, read by the CPU when drawing */
	bool ring;     /* data is in this frame's ring */
	uint8_t *data;
	size_t size;
	uint32_t usedframe, ringframe;
} cbuf_t;

typedef struct {
	bool enabled;
	GLuint buf;
	size_t ofs;
	GLint size;
	GLenum type;
	GLsizei stride;
} cattr_t;

typedef struct {
	bool used;
	GLuint ebo;
	cattr_t a[8];
} cvao_t;

enum { PK_NONE, PK_2D, PK_WORLD, PK_MODEL, PK_SKY };

/* uniform locations handed out by glGetUniformLocation: one per name */
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
	float mvp[16], model[16], color[4], tcmod[4], vieworg[3];
	float lamb[3], ldiff[3], ldir[3];
	float lmscale, alphatest, usefullbright;
	bool dirty;
} cprog_t;

typedef struct {
	bool used;
	GLenum type;
	char *src;
} cshader_t;

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

TABLE(ctex_t, texs)
TABLE(cbuf_t, bufs)
TABLE(cvao_t, vaos)
TABLE(cprog_t, progs)
TABLE(cshader_t, shaders)

/* --- GL state ------------------------------------------------------------------ */

static struct {
	GLenum error;
	int unit;
	GLuint tex2d[4], texcube[4];
	GLuint arraybuf, vao;
	cvao_t defvao;
	GLuint prog;
	bool blend, depthtest, cull, scissor, polyoffset;
	GLenum bsrc, bdst, depthfunc, cullface;
	bool depthmask;
	uint8_t colormask;
	float clearcolor[4];
	float depthnear, depthfar, offsetunits;
	int vp[4], sc[4];
	bool vpdirty;
} st;

static void set_error(GLenum e)
{
	if (!st.error)
		st.error = e;
}

static cvao_t *cur_vao(void) { return st.vao ? get_vaos(st.vao) : &st.defvao; }

/* --- PICA programs --------------------------------------------------------------- */

typedef struct {
	shaderProgram_s prog;
	int8_t mvp, tcmod, colmul, coladd, model, lamb, ldiff, ldir, vieworg;
} pica_t;

static DVLB_s *dvlb;
static pica_t pica[5];
static C3D_AttrInfo attrinfo;
static ctex_t whitetex;
static int boundkind = -1;

static void init_pica(pica_t *p, DVLE_s *dvle)
{
	shaderProgramInit(&p->prog);
	shaderProgramSetVsh(&p->prog, dvle);
	p->mvp = shaderInstanceGetUniformLocation(p->prog.vertexShader, "mvp");
	p->tcmod = shaderInstanceGetUniformLocation(p->prog.vertexShader, "tcmod");
	p->colmul = shaderInstanceGetUniformLocation(p->prog.vertexShader, "colmul");
	p->coladd = shaderInstanceGetUniformLocation(p->prog.vertexShader, "coladd");
	p->model = shaderInstanceGetUniformLocation(p->prog.vertexShader, "model");
	p->lamb = shaderInstanceGetUniformLocation(p->prog.vertexShader, "lamb");
	p->ldiff = shaderInstanceGetUniformLocation(p->prog.vertexShader, "ldiff");
	p->ldir = shaderInstanceGetUniformLocation(p->prog.vertexShader, "ldir");
	p->vieworg = shaderInstanceGetUniformLocation(p->prog.vertexShader, "vieworg");
}

/* --- textures: conversion ---------------------------------------------------------- */

static int pot_ceil(int v)
{
	int p = 8;
	while (p < v)
		p <<= 1;
	return p;
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

static int texel_bytes(GPU_TEXCOLOR fmt)
{
	switch (fmt) {
	case GPU_RGBA8: return 4;
	case GPU_RGB8: return 3;
	default: return 2;
	}
}

/* The GPU keeps textures in 8x8 tiles, texels inside a tile in Z order.
 * Its rows run top down from t = 1, so GL's first row (t = 0) is the last
 * one in memory. */
static inline size_t tiled_offset(int x, int y, int w, int h)
{
	int tile;
	y = h - 1 - y;
	tile = ((y >> 3) * (w >> 3) + (x >> 3)) << 6;
	int z = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
	return (size_t)(tile + z);
}

static inline void put_texel(uint8_t *dst, size_t i, GPU_TEXCOLOR fmt, const uint8_t *p)
{
	switch (fmt) {
	case GPU_RGBA8:
		dst[i * 4 + 0] = p[3], dst[i * 4 + 1] = p[2], dst[i * 4 + 2] = p[1], dst[i * 4 + 3] = p[0];
		break;
	case GPU_RGB8:
		dst[i * 3 + 0] = p[2], dst[i * 3 + 1] = p[1], dst[i * 3 + 2] = p[0];
		break;
	case GPU_RGB565:
		((uint16_t *)dst)[i] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
		break;
	case GPU_RGBA5551:
		((uint16_t *)dst)[i] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | (p[3] >> 7));
		break;
	default: /* GPU_RGBA4 */
		((uint16_t *)dst)[i] = (uint16_t)(((p[0] >> 4) << 12) | ((p[1] >> 4) << 8) | ((p[2] >> 4) << 4) | (p[3] >> 4));
		break;
	}
}

static void swizzle_rect(uint8_t *dst, int tw, int th, const uint8_t *rgba, int pitch, int x0, int y0, int w, int h,
                         GPU_TEXCOLOR fmt)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			put_texel(dst, tiled_offset(x0 + x, y0 + y, tw, th), fmt, rgba + ((size_t)y * pitch + x) * 4);
}

/* the cheapest format that keeps what the picture has */
static GPU_TEXCOLOR pick_format(const uint8_t *rgba, int n, GLint internal)
{
	bool alpha = false, partial = false, blank = true;
	if (internal == GL_RGB8 || internal == GL_RGB)
		return GPU_RGB8; /* lightmaps: 565 bands visibly */
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
	/* an empty canvas (a font atlas) gets filled with soft edges later */
	if (blank)
		partial = true;
	if (gl_ctr_texquality && gl_ctr_texquality->ivalue)
		return alpha ? GPU_RGBA8 : GPU_RGB8;
	return !alpha ? GPU_RGB565 : partial ? GPU_RGBA4 : GPU_RGBA5551;
}

static GPU_TEXTURE_FILTER_PARAM filter(GLenum f) { return f == GL_NEAREST ? GPU_NEAREST : GPU_LINEAR; }
static GPU_TEXTURE_WRAP_PARAM wrap(GLenum w) { return w == GL_CLAMP_TO_EDGE ? GPU_CLAMP_TO_EDGE : GPU_REPEAT; }

static void apply_tex_params(ctex_t *t)
{
	if (!t->allocated)
		return;
	C3D_TexSetFilter(&t->tex, filter(t->magf), filter(t->minf));
	if (t->mipped)
		C3D_TexSetFilterMipmap(&t->tex, GPU_LINEAR);
	C3D_TexSetWrap(&t->tex, wrap(t->wraps), wrap(t->wrapt));
}

static void free_tex_storage(ctex_t *t)
{
	if (t->allocated) {
		/* a draw queued this frame may use it: let the frame finish first */
		if (inframe) {
			defer_free(t->tex.data);
		} else
			C3D_TexDelete(&t->tex);
	}
	t->allocated = false;
	free(t->pending);
	t->pending = NULL;
}

/* the size cap for textures of the 3D world (the ones that ask for
 * mipmaps); pictures for the 2D layer and fonts keep their detail */
static int world_max_size(void) { return BW_CLAMP(gl_max_size ? gl_max_size->ivalue : 256, 8, 1024); }

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

static void upload_2d(ctex_t *t, const uint8_t *rgba, int w, int h, GLint internal)
{
	int tw, th;
	uint8_t *img = prepare_rgba(rgba, w, h, 1024, &tw, &th);
	if (!img) {
		set_error(0x0505); /* GL_OUT_OF_MEMORY */
		return;
	}
	free_tex_storage(t);
	t->cube = false;
	t->w = w, t->h = h, t->tw = tw, t->th = th;
	t->fmt = pick_format(img, tw * th, internal);
	t->mipped = false;
	if (!C3D_TexInit(&t->tex, (u16)tw, (u16)th, t->fmt)) {
		Con_Printf("^3gl_ctr: out of linear memory for a %dx%d texture\n", tw, th);
		free(img);
		set_error(0x0505);
		return;
	}
	t->allocated = true;
	swizzle_rect(t->tex.data, tw, th, img, tw, 0, 0, tw, th, t->fmt);
	t->pending = img; /* glGenerateMipmap usually follows */
	apply_tex_params(t);
}

/* a world texture larger than gl_max_size: halve it until it fits */
static void shrink_to_cap(ctex_t *t)
{
	int maxsz = world_max_size(), nw = t->tw, nh = t->th;
	uint8_t *img;
	while (nw > maxsz)
		nw >>= 1;
	while (nh > maxsz)
		nh >>= 1;
	if (nw == t->tw && nh == t->th)
		return;
	img = resample(t->pending, t->tw, t->th, nw, nh);
	if (!img)
		return;
	if (inframe)
		defer_free(t->tex.data);
	else
		C3D_TexDelete(&t->tex);
	t->allocated = false;
	if (!C3D_TexInit(&t->tex, (u16)nw, (u16)nh, t->fmt)) {
		free(img);
		return;
	}
	t->allocated = true;
	t->tw = nw, t->th = nh;
	swizzle_rect(t->tex.data, nw, nh, img, nw, 0, 0, nw, nh, t->fmt);
	free(t->pending);
	t->pending = img;
	apply_tex_params(t);
}

static void generate_mips(ctex_t *t)
{
	int levels, w, h;
	uint8_t *cur;
	C3D_Tex mipped;
	if (!t->allocated || t->cube || !t->pending)
		return;
	shrink_to_cap(t);
	if (!t->allocated || (gl_ctr_mipmaps && !gl_ctr_mipmaps->ivalue))
		return;
	levels = C3D_TexCalcMaxLevel(t->tw, t->th);
	/* the GPU's smallest tile is 8x8 */
	while (levels > 0 && (t->tw >> levels < 8 || t->th >> levels < 8))
		levels--;
	if (levels <= 0)
		return;
	C3D_TexInitParams p = {(u16)t->tw, (u16)t->th, (u8)levels, t->fmt, GPU_TEX_2D, false};
	if (!C3D_TexInitWithParams(&mipped, NULL, p))
		return; /* no memory: keep the single level */
	memcpy(mipped.data, t->tex.data, (size_t)t->tw * t->th * texel_bytes(t->fmt));
	cur = t->pending;
	w = t->tw, h = t->th;
	for (int l = 1; l <= levels; l++) {
		int nw = w / 2, nh = h / 2;
		uint8_t *next = malloc((size_t)nw * nh * 4);
		u32 size;
		uint8_t *dst = C3D_Tex2DGetImagePtr(&mipped, l, &size);
		if (!next)
			break;
		for (int y = 0; y < nh; y++)
			for (int x = 0; x < nw; x++)
				for (int c = 0; c < 4; c++)
					next[(y * nw + x) * 4 + c] =
						(uint8_t)((cur[((2 * y) * w + 2 * x) * 4 + c] + cur[((2 * y) * w + 2 * x + 1) * 4 + c] +
					               cur[((2 * y + 1) * w + 2 * x) * 4 + c] + cur[((2 * y + 1) * w + 2 * x + 1) * 4 + c] + 2) >> 2);
		swizzle_rect(dst, nw, nh, next, nw, 0, 0, nw, nh, t->fmt);
		if (cur != t->pending)
			free(cur);
		cur = next;
		w = nw, h = nh;
	}
	if (cur != t->pending)
		free(cur);
	if (inframe)
		defer_free(t->tex.data);
	else
		C3D_TexDelete(&t->tex);
	t->tex = mipped;
	t->mipped = true;
	free(t->pending);
	t->pending = NULL;
	apply_tex_params(t);
}

static void upload_cube_face(ctex_t *t, int face, const uint8_t *rgba, int w, int h)
{
	int tw, th;
	uint8_t *img = prepare_rgba(rgba, w, h, world_max_size(), &tw, &th);
	if (!img)
		return;
	/* the six faces share one size: the first face decides it */
	if (!t->allocated || !t->cube || t->tw != tw || t->th != th) {
		free_tex_storage(t);
		t->cube = true;
		t->w = w, t->h = h, t->tw = tw, t->th = th;
		t->fmt = gl_ctr_texquality && gl_ctr_texquality->ivalue ? GPU_RGBA8 : GPU_RGB565;
		if (!C3D_TexInitCube(&t->tex, &t->faces, (u16)tw, (u16)th, t->fmt)) {
			free(img);
			return;
		}
		t->allocated = true;
	}
	u32 size;
	uint8_t *dst = C3D_TexCubeGetImagePtr(&t->tex, (GPU_TEXFACE)face, 0, &size);
	swizzle_rect(dst, tw, th, img, tw, 0, 0, tw, th, t->fmt);
	free(img);
	apply_tex_params(t);
}

/* --- GL entry points: textures -------------------------------------------------------- */

static void ctr_glGenTextures(GLsizei n, GLuint *ids)
{
	for (int i = 0; i < n; i++) {
		ids[i] = new_texs();
		ctex_t *t = &texs[ids[i]];
		t->minf = GL_LINEAR, t->magf = GL_LINEAR, t->wraps = t->wrapt = GL_REPEAT;
	}
}

static void ctr_glDeleteTextures(GLsizei n, const GLuint *ids)
{
	begin_frame();
	for (int i = 0; i < n; i++) {
		ctex_t *t = get_texs(ids[i]);
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

static void ctr_glActiveTexture(GLenum unit) { st.unit = BW_CLAMP((int)unit - GL_TEXTURE0, 0, 3); }

static void ctr_glBindTexture(GLenum target_, GLuint id)
{
	if (target_ == GL_TEXTURE_CUBE_MAP)
		st.texcube[st.unit] = id;
	else
		st.tex2d[st.unit] = id;
}

static ctex_t *bound_tex(GLenum target_)
{
	bool cube = target_ == GL_TEXTURE_CUBE_MAP || (target_ >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
	                                               target_ < GL_TEXTURE_CUBE_MAP_POSITIVE_X + 6);
	return get_texs(cube ? st.texcube[st.unit] : st.tex2d[st.unit]);
}

static void ctr_glTexImage2D(GLenum target_, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt,
                             GLenum type, const void *pixels)
{
	ctex_t *t = bound_tex(target_);
	uint8_t *expanded = NULL;
	const uint8_t *rgba = pixels;
	(void)border;
	if (!t || level != 0 || type != GL_UNSIGNED_BYTE || w <= 0 || h <= 0)
		return;
	begin_frame();
	if (!pixels) {
		/* storage only: start it out black, filled later with glTexSubImage2D */
		expanded = calloc((size_t)w * h, 4);
		rgba = expanded;
	} else if (fmt == GL_RGB) {
		rgba = expanded = expand_rgb(pixels, w, h);
	} else if (fmt != GL_RGBA) {
		set_error(0x0500);
		return;
	}
	if (!rgba)
		return;
	if (target_ >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target_ < GL_TEXTURE_CUBE_MAP_POSITIVE_X + 6)
		upload_cube_face(t, (int)(target_ - GL_TEXTURE_CUBE_MAP_POSITIVE_X), rgba, w, h);
	else
		upload_2d(t, rgba, w, h, ifmt);
	t->dynamic = !pixels;
	free(expanded);
}

static void ctr_glTexSubImage2D(GLenum target_, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt,
                                GLenum type, const void *pixels)
{
	ctex_t *t = bound_tex(target_);
	const uint8_t *src = pixels;
	uint8_t *expanded = NULL;
	if (!t || !t->allocated || t->cube || level != 0 || type != GL_UNSIGNED_BYTE || !pixels || w <= 0 || h <= 0)
		return;
	begin_frame();
	if (fmt == GL_RGB)
		src = expanded = expand_rgb(pixels, w, h);
	if (!src)
		return;
	if (t->tw == t->w && t->th == t->h) {
		swizzle_rect(t->tex.data, t->tw, t->th, src, w, x, y, w, h, t->fmt);
	} else {
		/* the texture was resized: scale the rectangle and point-sample it */
		float sx = (float)t->tw / t->w, sy = (float)t->th / t->h;
		int x0 = (int)(x * sx), y0 = (int)(y * sy), x1 = (int)((x + w) * sx + 0.999f), y1 = (int)((y + h) * sy + 0.999f);
		for (int ty = y0; ty < BW_MIN(y1, t->th); ty++)
			for (int tx = x0; tx < BW_MIN(x1, t->tw); tx++) {
				int ix = BW_CLAMP((int)((tx + 0.5f) / sx) - x, 0, w - 1);
				int iy = BW_CLAMP((int)((ty + 0.5f) / sy) - y, 0, h - 1);
				put_texel(t->tex.data, tiled_offset(tx, ty, t->tw, t->th), t->fmt, src + ((size_t)iy * w + ix) * 4);
			}
	}
	free(expanded);
	/* the mip chain no longer matches; dynamic textures don't ask for one */
}

static void ctr_glTexParameteri(GLenum target_, GLenum pname, GLint v)
{
	ctex_t *t = bound_tex(target_);
	if (!t)
		return;
	switch (pname) {
	case GL_TEXTURE_MIN_FILTER: t->minf = v == GL_NEAREST ? GL_NEAREST : GL_LINEAR; break;
	case GL_TEXTURE_MAG_FILTER: t->magf = (GLenum)v; break;
	case GL_TEXTURE_WRAP_S: t->wraps = (GLenum)v; break;
	case GL_TEXTURE_WRAP_T: t->wrapt = (GLenum)v; break;
	default: return;
	}
	apply_tex_params(t);
}

static void ctr_glTexParameterf(GLenum target_, GLenum pname, GLfloat v)
{
	if (pname == GL_TEXTURE_MAX_ANISOTROPY) {
		set_error(0x0500); /* not on this GPU; the renderer treats it as optional */
		return;
	}
	ctr_glTexParameteri(target_, pname, (GLint)v);
}

static void ctr_glGenerateMipmap(GLenum target_)
{
	ctex_t *t = bound_tex(target_);
	if (t)
		generate_mips(t);
}

/* --- GL entry points: buffers and vertex arrays ----------------------------------------- */

static void ctr_glGenBuffers(GLsizei n, GLuint *ids)
{
	for (int i = 0; i < n; i++)
		ids[i] = new_bufs();
}

static void release_storage(cbuf_t *b)
{
	if (!b->data || b->ring)
		return;
	if (b->element)
		free(b->data);
	else if (b->usedframe == framenum && inframe)
		defer_free(b->data);
	else
		linearFree(b->data);
}

static void ctr_glDeleteBuffers(GLsizei n, const GLuint *ids)
{
	begin_frame();
	for (int i = 0; i < n; i++) {
		cbuf_t *b = get_bufs(ids[i]);
		if (!b)
			continue;
		release_storage(b);
		b->used = false;
		if (st.arraybuf == ids[i])
			st.arraybuf = 0;
	}
}

static void ctr_glBindBuffer(GLenum target_, GLuint id)
{
	if (target_ == GL_ELEMENT_ARRAY_BUFFER)
		cur_vao()->ebo = id;
	else
		st.arraybuf = id;
}

static cbuf_t *bound_buf(GLenum target_)
{
	return get_bufs(target_ == GL_ELEMENT_ARRAY_BUFFER ? cur_vao()->ebo : st.arraybuf);
}

static void ctr_glBufferData(GLenum target_, GLsizeiptr size, const void *data, GLenum usage)
{
	cbuf_t *b = bound_buf(target_);
	if (!b || size < 0)
		return;
	begin_frame();
	release_storage(b);
	b->data = NULL;
	b->size = (size_t)size;
	b->element = target_ == GL_ELEMENT_ARRAY_BUFFER;
	b->ring = false;
	if (!size)
		return;
	if (b->element) {
		b->data = malloc((size_t)size);
	} else if (usage == GL_STREAM_DRAW && inframe) {
		/* respecified before every draw (R_StreamDraw): this frame only */
		b->data = ring_alloc((size_t)size);
		b->ring = true;
		b->ringframe = framenum;
	} else {
		b->data = linearAlloc((size_t)size);
	}
	if (!b->data) {
		Con_Printf("^3gl_ctr: out of memory for a %u byte buffer\n", (unsigned)size);
		set_error(0x0505);
		b->size = 0;
		return;
	}
	if (data)
		memcpy(b->data, data, (size_t)size);
}

static void ctr_glBufferSubData(GLenum target_, GLintptr ofs, GLsizeiptr size, const void *data)
{
	cbuf_t *b = bound_buf(target_);
	if (!b || !b->data || ofs < 0 || (size_t)(ofs + size) > b->size)
		return;
	begin_frame();
	if (!b->element && b->usedframe == framenum) {
		/* a draw queued this frame reads the old contents: copy on write */
		uint8_t *copy = linearAlloc(b->size);
		if (copy) {
			memcpy(copy, b->data, b->size);
			if (!b->ring)
				defer_free(b->data);
			b->data = copy;
			b->ring = false;
		}
	}
	memcpy(b->data + ofs, data, (size_t)size);
}

static void ctr_glGenVertexArrays(GLsizei n, GLuint *ids)
{
	for (int i = 0; i < n; i++)
		ids[i] = new_vaos();
}

static void ctr_glDeleteVertexArrays(GLsizei n, const GLuint *ids)
{
	for (int i = 0; i < n; i++) {
		cvao_t *v = get_vaos(ids[i]);
		if (v)
			v->used = false;
		if (st.vao == ids[i])
			st.vao = 0;
	}
}

static void ctr_glBindVertexArray(GLuint id) { st.vao = id; }

static void ctr_glEnableVertexAttribArray(GLuint i)
{
	if (i < 8)
		cur_vao()->a[i].enabled = true;
}

static void ctr_glDisableVertexAttribArray(GLuint i)
{
	if (i < 8)
		cur_vao()->a[i].enabled = false;
}

static void ctr_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p)
{
	(void)norm;
	if (i >= 8)
		return;
	cattr_t *a = &cur_vao()->a[i];
	a->buf = st.arraybuf;
	a->ofs = (size_t)p;
	a->size = size;
	a->type = type;
	a->stride = stride;
}

static void ctr_glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
	(void)i, (void)x, (void)y, (void)z, (void)w; /* the renderer always feeds every attribute from rvert_t */
}

/* --- GL entry points: shaders and programs ---------------------------------------------- */

static GLuint ctr_glCreateShader(GLenum type)
{
	GLuint id = new_shaders();
	shaders[id].type = type;
	return id;
}

static void ctr_glDeleteShader(GLuint id)
{
	cshader_t *s = get_shaders(id);
	if (s) {
		free(s->src);
		s->src = NULL;
		s->used = false;
	}
}

static void ctr_glShaderSource(GLuint id, GLsizei n, const GLchar *const *src, const GLint *len)
{
	cshader_t *s = get_shaders(id);
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

static void ctr_glCompileShader(GLuint id) { (void)id; }

static void ctr_glGetShaderiv(GLuint id, GLenum p, GLint *v)
{
	(void)id;
	*v = p == GL_COMPILE_STATUS ? GL_TRUE : 0;
}

static void ctr_glGetShaderInfoLog(GLuint id, GLsizei max, GLsizei *len, GLchar *log)
{
	(void)id;
	if (max > 0)
		log[0] = 0;
	if (len)
		*len = 0;
}

static GLuint ctr_glCreateProgram(void)
{
	GLuint id = new_progs();
	cprog_t *p = &progs[id];
	p->color[0] = p->color[1] = p->color[2] = p->color[3] = 1;
	p->tcmod[2] = p->tcmod[3] = 1;
	return id;
}

static void ctr_glDeleteProgram(GLuint id)
{
	cprog_t *p = get_progs(id);
	if (p)
		p->used = false;
}

static void ctr_glAttachShader(GLuint prog, GLuint sh)
{
	cprog_t *p = get_progs(prog);
	cshader_t *s = get_shaders(sh);
	if (p && s)
		p->shaders[s->type == GL_FRAGMENT_SHADER] = sh;
}

static void ctr_glBindAttribLocation(GLuint p, GLuint i, const GLchar *name) { (void)p, (void)i, (void)name; }

/* which of the renderer's programs this is, from its GLSL (r_gl.c) */
static void ctr_glLinkProgram(GLuint id)
{
	cprog_t *p = get_progs(id);
	cshader_t *vs, *fs;
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
	else if (strstr(fs->src, "u_tex") && strstr(vs->src, "a_color"))
		p->kind = PK_2D;
	else
		Con_Printf("^3gl_ctr: a program the 3DS GPU can't run; its draws are skipped\n");
}

static void ctr_glGetProgramiv(GLuint id, GLenum pn, GLint *v)
{
	cprog_t *p = get_progs(id);
	*v = pn == GL_LINK_STATUS ? (p && p->kind != PK_NONE) : 0;
}

static void ctr_glGetProgramInfoLog(GLuint id, GLsizei max, GLsizei *len, GLchar *log)
{
	(void)id;
	snprintf(log, (size_t)max, "not one of the programs the 3DS backend knows");
	if (len)
		*len = (GLsizei)strlen(log);
}

static void ctr_glUseProgram(GLuint id) { st.prog = id; }

static GLint ctr_glGetUniformLocation(GLuint prog, const GLchar *name)
{
	(void)prog;
	for (int i = 1; i < U_COUNT; i++)
		if (!strcmp(uniform_names[i], name))
			return i;
	return -1;
}

static cprog_t *cur_prog(void) { return get_progs(st.prog); }

static void set_uniform(GLint loc, int n, const float *v)
{
	cprog_t *p = cur_prog();
	if (!p || loc <= 0)
		return;
	p->dirty = true;
	switch (loc) {
	case U_COLOR: memcpy(p->color, v, sizeof(float) * BW_MIN(n, 4)); break;
	case U_TCMOD: memcpy(p->tcmod, v, sizeof(float) * BW_MIN(n, 4)); break;
	case U_VIEWORG: memcpy(p->vieworg, v, sizeof(float) * BW_MIN(n, 3)); break;
	case U_LIGHT_AMBIENT: memcpy(p->lamb, v, sizeof(float) * BW_MIN(n, 3)); break;
	case U_LIGHT_DIFFUSE: memcpy(p->ldiff, v, sizeof(float) * BW_MIN(n, 3)); break;
	case U_LIGHT_DIR: memcpy(p->ldir, v, sizeof(float) * BW_MIN(n, 3)); break;
	case U_LMSCALE: p->lmscale = v[0]; break;
	case U_ALPHATEST: p->alphatest = v[0]; break;
	case U_USEFULLBRIGHT: p->usefullbright = v[0]; break;
	default: break; /* samplers are fixed per unit; fog, time and dlights aren't drawn */
	}
}

static void ctr_glUniform1i(GLint l, GLint v) { (void)l, (void)v; }
static void ctr_glUniform1f(GLint l, GLfloat v) { set_uniform(l, 1, &v); }

static void ctr_glUniform2f(GLint l, GLfloat a, GLfloat b)
{
	float v[2] = {a, b};
	set_uniform(l, 2, v);
}

static void ctr_glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c)
{
	float v[3] = {a, b, c};
	set_uniform(l, 3, v);
}

static void ctr_glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d)
{
	float v[4] = {a, b, c, d};
	set_uniform(l, 4, v);
}

static void ctr_glUniform3fv(GLint l, GLsizei n, const GLfloat *v)
{
	if (n == 1)
		set_uniform(l, 3, v);
}

static void ctr_glUniform4fv(GLint l, GLsizei n, const GLfloat *v)
{
	if (n == 1)
		set_uniform(l, 4, v);
}

static void ctr_glUniformMatrix4fv(GLint l, GLsizei n, GLboolean tr, const GLfloat *v)
{
	cprog_t *p = cur_prog();
	(void)n, (void)tr;
	if (!p)
		return;
	if (l == U_MVP)
		memcpy(p->mvp, v, sizeof(p->mvp));
	else if (l == U_MODEL)
		memcpy(p->model, v, sizeof(p->model));
	p->dirty = true;
}

/* --- GL entry points: fixed state ------------------------------------------------------- */

static void ctr_glEnable(GLenum cap)
{
	switch (cap) {
	case GL_BLEND: st.blend = true; break;
	case GL_DEPTH_TEST: st.depthtest = true; break;
	case GL_CULL_FACE: st.cull = true; break;
	case GL_SCISSOR_TEST: st.scissor = true, st.vpdirty = true; break;
	case GL_POLYGON_OFFSET_FILL: st.polyoffset = true; break;
	default: break;
	}
}

static void ctr_glDisable(GLenum cap)
{
	switch (cap) {
	case GL_BLEND: st.blend = false; break;
	case GL_DEPTH_TEST: st.depthtest = false; break;
	case GL_CULL_FACE: st.cull = false; break;
	case GL_SCISSOR_TEST: st.scissor = false, st.vpdirty = true; break;
	case GL_POLYGON_OFFSET_FILL: st.polyoffset = false; break;
	default: break;
	}
}

static void ctr_glBlendFunc(GLenum s, GLenum d) { st.bsrc = s, st.bdst = d; }
static void ctr_glDepthFunc(GLenum f) { st.depthfunc = f; }
static void ctr_glDepthMask(GLboolean f) { st.depthmask = f; }
static void ctr_glDepthRange(double n, double f) { st.depthnear = (float)n, st.depthfar = (float)f; }
static void ctr_glCullFace(GLenum m) { st.cullface = m; }
static void ctr_glPolygonOffset(GLfloat factor, GLfloat units) { (void)factor, st.offsetunits = units; }
static void ctr_glLineWidth(GLfloat w) { (void)w; }
static void ctr_glPixelStorei(GLenum p, GLint v) { (void)p, (void)v; }
static void ctr_glFinish(void) {}

static void ctr_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	st.colormask = (uint8_t)((r ? GPU_WRITE_RED : 0) | (g ? GPU_WRITE_GREEN : 0) | (b ? GPU_WRITE_BLUE : 0) |
	                         (a ? GPU_WRITE_ALPHA : 0));
}

static void ctr_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
	st.vp[0] = x, st.vp[1] = y, st.vp[2] = w, st.vp[3] = h;
	st.vpdirty = true;
}

static void ctr_glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
	st.sc[0] = x, st.sc[1] = y, st.sc[2] = w, st.sc[3] = h;
	st.vpdirty = true;
}

static void ctr_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	st.clearcolor[0] = r, st.clearcolor[1] = g, st.clearcolor[2] = b, st.clearcolor[3] = a;
}

static GPU_TESTFUNC test_func(GLenum f)
{
	switch (f) {
	case GL_NEVER: return GPU_NEVER;
	case GL_LESS: return GPU_LESS;
	case GL_EQUAL: return GPU_EQUAL;
	case GL_LEQUAL: return GPU_LEQUAL;
	case GL_GREATER: return GPU_GREATER;
	default: return GPU_ALWAYS;
	}
}

static GPU_BLENDFACTOR blend_factor(GLenum f)
{
	switch (f) {
	case GL_ZERO: return GPU_ZERO;
	case GL_ONE: return GPU_ONE;
	case GL_SRC_COLOR: return GPU_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR: return GPU_ONE_MINUS_SRC_COLOR;
	case GL_DST_COLOR: return GPU_DST_COLOR;
	case GL_ONE_MINUS_DST_COLOR: return GPU_ONE_MINUS_DST_COLOR;
	case GL_SRC_ALPHA: return GPU_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA: return GPU_ONE_MINUS_SRC_ALPHA;
	case GL_DST_ALPHA: return GPU_DST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA: return GPU_ONE_MINUS_DST_ALPHA;
	default: return GPU_ONE;
	}
}

/* GL's landscape rectangles (origin bottom left) on the portrait
 * framebuffer: landscape x runs down the framebuffer from its far end,
 * landscape y runs across it */
static void apply_viewport(void)
{
	if (!st.vpdirty)
		return;
	st.vpdirty = false;
	int x = st.vp[0], y = st.vp[1], w = BW_MAX(st.vp[2], 1), h = BW_MAX(st.vp[3], 1);
	C3D_SetViewport((u32)BW_MAX(y, 0), (u32)BW_MAX(FB_H - x - w, 0), (u32)h, (u32)w);
	if (st.scissor) {
		int l = BW_CLAMP(st.sc[1], 0, FB_W), r = BW_CLAMP(st.sc[1] + st.sc[3], 0, FB_W);
		int t = BW_CLAMP(FB_H - st.sc[0] - st.sc[2], 0, FB_H), b = BW_CLAMP(FB_H - st.sc[0], 0, FB_H);
		if (r <= l || b <= t)
			l = r = t = b = 0; /* empty: nothing passes */
		C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)l, (u32)t, (u32)BW_MAX(r, l + 1), (u32)BW_MAX(b, t + 1));
	}
}

static void apply_fixed_state(void)
{
	uint8_t mask = st.colormask;
	apply_viewport();
	if (st.blend) {
		GPU_BLENDFACTOR s = blend_factor(st.bsrc), d = blend_factor(st.bdst);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, s, d, s, d);
	} else {
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	}
	/* as in GL, no depth test means no depth writes either */
	if (st.depthtest)
		C3D_DepthTest(true, test_func(st.depthfunc), mask | (st.depthmask ? GPU_WRITE_DEPTH : 0));
	else
		C3D_DepthTest(true, GPU_ALWAYS, mask);
	/* depth = n + (f - n) * d, d in [0, 1] arriving as -z (see fix_mvp) */
	float offset = st.polyoffset ? st.offsetunits * 2e-5f : 0;
	C3D_DepthMap(true, -(st.depthfar - st.depthnear), st.depthnear + offset);
	if (!st.cull)
		C3D_CullFace(GPU_CULL_NONE);
	else
		C3D_CullFace(st.cullface == GL_FRONT ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW);
}

/* --- drawing -------------------------------------------------------------------------------- */

/* mvp on the PICA: turned a quarter for the portrait screen, and z from
 * GL's [-w, w] to [0, -w] so that depth comes out as d = -z/w in [0, 1]
 * (near 0) and GL's depth tests apply as they are */
static void upload_mvp(int8_t loc, const float *m)
{
	C3D_Mtx out;
	for (int c = 0; c < 4; c++) {
		float r0 = m[c * 4 + 0], r1 = m[c * 4 + 1], r2 = m[c * 4 + 2], r3 = m[c * 4 + 3];
		out.r[0].c[3 - c] = r1;
		out.r[1].c[3 - c] = -r0;
		out.r[2].c[3 - c] = -0.5f * (r2 + r3);
		out.r[3].c[3 - c] = r3;
	}
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, loc, &out);
}

static void unif4(int8_t loc, float x, float y, float z, float w)
{
	if (loc >= 0)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, loc, x, y, z, w);
}

static C3D_Tex *gpu_tex(GLuint id, bool cube)
{
	ctex_t *t = get_texs(id);
	if (!t || !t->allocated || t->cube != cube)
		return cube ? NULL : &whitetex.tex;
	return &t->tex;
}

static void tev_stage(int i, GPU_TEVSRC a, GPU_TEVSRC b, GPU_COMBINEFUNC rgbfn, GPU_TEVSRC alpha_a,
                      GPU_TEVSRC alpha_b, GPU_COMBINEFUNC alphafn, GPU_TEVSCALE rgbscale)
{
	C3D_TexEnv *env = C3D_GetTexEnv(i);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, a, b, GPU_PRIMARY_COLOR);
	C3D_TexEnvSrc(env, C3D_Alpha, alpha_a, alpha_b, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_RGB, rgbfn);
	C3D_TexEnvFunc(env, C3D_Alpha, alphafn);
	C3D_TexEnvScale(env, C3D_RGB, rgbscale);
}

/* programs, uniforms, textures and combiners for a draw; false to skip it */
static bool apply_program(void)
{
	cprog_t *p = cur_prog();
	pica_t *pc;
	int kind = p ? p->kind : PK_NONE, stages = 0;
	if (kind == PK_NONE)
		return false;
	pc = &pica[kind];
	if (boundkind != kind) {
		C3D_BindProgram(&pc->prog);
		C3D_SetAttrInfo(&attrinfo);
		boundkind = kind;
		p->dirty = true;
	}
	/* uniforms belong to the GPU, not the program: send them every draw
	 * that changes program or values */
	static GLuint lastprog;
	if (p->dirty || lastprog != st.prog) {
		const float *c = p->color;
		upload_mvp(pc->mvp, p->mvp);
		unif4(pc->tcmod, p->tcmod[0], p->tcmod[1], p->tcmod[2], p->tcmod[3]);
		switch (kind) {
		case PK_2D:
			unif4(pc->colmul, c[0], c[1], c[2], c[3]);
			unif4(pc->coladd, 0, 0, 0, 0);
			break;
		case PK_WORLD:
			if (p->lmscale >= 0) {
				float s = p->lmscale * 0.5f;
				unif4(pc->colmul, 0, 0, 0, 0);
				unif4(pc->coladd, c[0] * s, c[1] * s, c[2] * s, c[3]);
			} else {
				unif4(pc->colmul, c[0] * 0.5f, c[1] * 0.5f, c[2] * 0.5f, c[3]);
				unif4(pc->coladd, 0, 0, 0, 0);
			}
			break;
		case PK_MODEL:
			for (int r = 0; r < 3; r++)
				unif4(pc->model + r, p->model[r], p->model[4 + r], p->model[8 + r], 0);
			unif4(pc->lamb, p->lamb[0] * c[0] * 0.5f, p->lamb[1] * c[1] * 0.5f, p->lamb[2] * c[2] * 0.5f, c[3]);
			unif4(pc->ldiff, p->ldiff[0] * c[0] * 0.5f, p->ldiff[1] * c[1] * 0.5f, p->ldiff[2] * c[2] * 0.5f, 0);
			unif4(pc->ldir, p->ldir[0], p->ldir[1], p->ldir[2], 0);
			break;
		case PK_SKY:
			unif4(pc->vieworg, p->vieworg[0], p->vieworg[1], p->vieworg[2], 0);
			break;
		}
		p->dirty = false;
		lastprog = st.prog;
	}

	if (kind == PK_SKY) {
		C3D_Tex *cube = gpu_tex(st.texcube[3], true);
		if (!cube)
			return false;
		C3D_TexBind(0, cube);
		C3D_TexEnv *env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
		C3D_TexEnvColor(env, (u32)(BW_CLAMP(p->color[0], 0, 1) * 255) | (u32)(BW_CLAMP(p->color[1], 0, 1) * 255) << 8 |
		                         (u32)(BW_CLAMP(p->color[2], 0, 1) * 255) << 16 | 0xFF000000u);
		stages = 1;
	} else {
		C3D_TexBind(0, gpu_tex(st.tex2d[0], false));
		if (kind == PK_2D) {
			tev_stage(0, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_MODULATE, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_MODULATE,
			          GPU_TEVSCALE_1);
			stages = 1;
		} else if (kind == PK_WORLD && p->lmscale >= 0) {
			C3D_TexBind(1, gpu_tex(st.tex2d[1], false));
			tev_stage(0, GPU_TEXTURE0, GPU_TEXTURE1, GPU_MODULATE, GPU_TEXTURE0, GPU_TEXTURE0, GPU_REPLACE,
			          GPU_TEVSCALE_1);
			tev_stage(1, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_MODULATE, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_MODULATE,
			          GPU_TEVSCALE_2);
			stages = 2;
		} else {
			/* vertex-lit world and models: the colour carries the light, halved */
			tev_stage(0, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_MODULATE, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_MODULATE,
			          GPU_TEVSCALE_2);
			stages = 1;
		}
		if (kind != PK_2D && p->usefullbright > 0) {
			C3D_TexBind(2, gpu_tex(st.tex2d[2], false));
			tev_stage(stages, GPU_PREVIOUS, GPU_TEXTURE2, GPU_ADD, GPU_PREVIOUS, GPU_PREVIOUS, GPU_REPLACE,
			          GPU_TEVSCALE_1);
			stages++;
		}
	}
	for (int i = stages; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	if (p->alphatest > 0)
		C3D_AlphaTest(true, GPU_GEQUAL, (int)BW_CLAMP(p->alphatest * 255, 0, 255));
	else
		C3D_AlphaTest(false, GPU_ALWAYS, 0);
	return true;
}

static GPU_Primitive_t prim(GLenum mode, bool *ok)
{
	*ok = true;
	switch (mode) {
	case GL_TRIANGLES: return GPU_TRIANGLES;
	case GL_TRIANGLE_STRIP: return GPU_TRIANGLE_STRIP;
	case GL_TRIANGLE_FAN: return GPU_TRIANGLE_FAN;
	default: *ok = false; return GPU_TRIANGLES; /* lines and points: no such primitives */
	}
}

/* the vertex buffer of the bound arrays, in the one layout the renderer uses */
static cbuf_t *vertex_source(size_t *base)
{
	cvao_t *v = cur_vao();
	static const size_t offsets[5] = {0, 12, 20, 28, 40};
	cbuf_t *b;
	if (!v->a[0].enabled)
		return NULL;
	b = get_bufs(v->a[0].buf);
	for (int i = 0; i < 5; i++) {
		if (!v->a[i].enabled || v->a[i].buf != v->a[0].buf || v->a[i].stride != RVERT_STRIDE ||
		    v->a[i].ofs != v->a[0].ofs + offsets[i]) {
			static bool warned;
			if (!warned)
				Con_Printf("^3gl_ctr: a vertex layout other than rvert_t; draws skipped\n");
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

static void draw(GLenum mode, int count, const uint32_t *idx32, const uint16_t *idx16, int first)
{
	bool ok;
	GPU_Primitive_t p = prim(mode, &ok);
	size_t base;
	cbuf_t *vb;
	uint32_t lo = UINT32_MAX, hi = 0;
	uint16_t *out;
	C3D_BufInfo bi;
	if (!ok || count <= 0)
		return;
	begin_frame();
	if (!(vb = vertex_source(&base)) || !apply_program())
		return;
	apply_fixed_state();
	for (int i = 0; i < count; i++) {
		uint32_t x = idx32 ? idx32[i] : idx16 ? idx16[i] : (uint32_t)(first + i);
		lo = BW_MIN(lo, x), hi = BW_MAX(hi, x);
	}
	if (hi - lo > 0xFFFF || base + (size_t)(hi + 1) * RVERT_STRIDE > vb->size)
		return; /* more than 16-bit indices reach, or past the buffer */
	out = ring_alloc(sizeof(uint16_t) * (size_t)count);
	if (!out)
		return;
	for (int i = 0; i < count; i++)
		out[i] = (uint16_t)((idx32 ? idx32[i] : idx16 ? idx16[i] : (uint32_t)(first + i)) - lo);
	BufInfo_Init(&bi);
	BufInfo_Add(&bi, vb->data + base + (size_t)lo * RVERT_STRIDE, RVERT_STRIDE, 5, 0x43210);
	C3D_SetBufInfo(&bi);
	C3D_DrawElements(p, count, C3D_UNSIGNED_SHORT, out);
	vb->usedframe = framenum;
}

static void ctr_glDrawArrays(GLenum mode, GLint first, GLsizei count) { draw(mode, count, NULL, NULL, first); }

static void ctr_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *ofs, GLint basevertex)
{
	cbuf_t *eb = get_bufs(cur_vao()->ebo);
	size_t o = (size_t)ofs, sz = type == GL_UNSIGNED_INT ? 4 : 2;
	if (!eb || !eb->data || o + sz * (size_t)count > eb->size || basevertex) {
		if (basevertex) {
			static bool warned;
			if (!warned)
				Con_Printf("^3gl_ctr: glDrawElementsBaseVertex with a base vertex isn't supported\n");
			warned = true;
		}
		return;
	}
	if (type == GL_UNSIGNED_INT)
		draw(mode, count, (const uint32_t *)(eb->data + o), NULL, 0);
	else if (type == GL_UNSIGNED_SHORT)
		draw(mode, count, NULL, (const uint16_t *)(eb->data + o), 0);
}

static void ctr_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *ofs)
{
	ctr_glDrawElementsBaseVertex(mode, count, type, ofs, 0);
}

/* a scissored clear: a quad over the scissor box, at the far plane */
static void clear_box(bool color, bool depth)
{
	float verts[4][11];
	uint8_t col[4];
	C3D_BufInfo bi;
	uint8_t *vb = ring_alloc(RVERT_STRIDE * 4);
	uint16_t *ib = ring_alloc(sizeof(uint16_t) * 6);
	if (!vb || !ib)
		return;
	for (int c = 0; c < 4; c++)
		col[c] = (uint8_t)(BW_CLAMP(st.clearcolor[c], 0, 1) * 255);
	memset(verts, 0, sizeof(verts));
	for (int i = 0; i < 4; i++) {
		verts[i][0] = (i & 1) ? 1.0f : -1.0f;
		verts[i][1] = (i & 2) ? 1.0f : -1.0f;
		verts[i][2] = 1.0f; /* GL's far plane */
		memcpy(vb + i * RVERT_STRIDE, verts[i], 40);
		memcpy(vb + i * RVERT_STRIDE + 40, col, 4);
	}
	ib[0] = 0, ib[1] = 1, ib[2] = 2, ib[3] = 2, ib[4] = 1, ib[5] = 3;
	pica_t *pc = &pica[PK_2D];
	C3D_BindProgram(&pc->prog);
	C3D_SetAttrInfo(&attrinfo);
	boundkind = -1; /* the next draw rebinds and resends its uniforms */
	static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	upload_mvp(pc->mvp, ident);
	unif4(pc->tcmod, 0, 0, 1, 1);
	unif4(pc->colmul, 1, 1, 1, 1);
	unif4(pc->coladd, 0, 0, 0, 0);
	C3D_TexBind(0, &whitetex.tex);
	tev_stage(0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_REPLACE, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_REPLACE,
	          GPU_TEVSCALE_1);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	apply_viewport();
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_ALWAYS, (color ? GPU_WRITE_COLOR : 0) | (depth ? GPU_WRITE_DEPTH : 0));
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_CullFace(GPU_CULL_NONE);
	BufInfo_Init(&bi);
	BufInfo_Add(&bi, vb, RVERT_STRIDE, 5, 0x43210);
	C3D_SetBufInfo(&bi);
	C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ib);
}

static void ctr_glClear(GLbitfield mask)
{
	bool color = mask & GL_COLOR_BUFFER_BIT, depth = mask & GL_DEPTH_BUFFER_BIT;
	bool full = !st.scissor || (st.sc[0] <= 0 && st.sc[1] <= 0 && st.sc[0] + st.sc[2] >= CTR_WIDTH &&
	                            st.sc[1] + st.sc[3] >= CTR_HEIGHT);
	begin_frame();
	if (!inframe || (!color && !depth))
		return;
	if (!full) {
		clear_box(color, depth);
		return;
	}
	/* the fill runs when it is queued: submit the draws before it first */
	C3D_FrameSplit(0);
	u32 rgba = (u32)(BW_CLAMP(st.clearcolor[0], 0, 1) * 255) << 24 | (u32)(BW_CLAMP(st.clearcolor[1], 0, 1) * 255) << 16 |
	           (u32)(BW_CLAMP(st.clearcolor[2], 0, 1) * 255) << 8 | 0xFF;
	C3D_RenderTargetClear(target, (C3D_ClearBits)((color ? C3D_CLEAR_COLOR : 0) | (depth ? C3D_CLEAR_DEPTH : 0)), rgba,
	                      0xFFFFFF);
}

/* --- GL entry points: queries and the rest ----------------------------------------------- */

static const GLubyte *ctr_glGetString(GLenum name)
{
	switch (name) {
	case GL_VENDOR: return (const GLubyte *)"Bulwark";
	case GL_RENDERER: return (const GLubyte *)"PICA200 (citro3d)";
	case GL_VERSION: return (const GLubyte *)"3.3 subset";
	default: return (const GLubyte *)"";
	}
}

static GLenum ctr_glGetError(void)
{
	GLenum e = st.error;
	st.error = 0;
	return e;
}

static void ctr_glGetIntegerv(GLenum p, GLint *v)
{
	if (p == GL_MAX_TEXTURE_SIZE)
		*v = 1024;
	else
		*v = 0;
}

static void ctr_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *px)
{
	(void)x, (void)y, (void)t;
	memset(px, 0, (size_t)w * h * (f == GL_RGBA ? 4 : 3));
}

static void ctr_glGenFramebuffers(GLsizei n, GLuint *f)
{
	for (int i = 0; i < n; i++)
		f[i] = 0;
}
static void ctr_glDeleteFramebuffers(GLsizei n, const GLuint *f) { (void)n, (void)f; }
static void ctr_glBindFramebuffer(GLenum t, GLuint f) { (void)t, (void)f; }
static GLenum ctr_glCheckFramebufferStatus(GLenum t) { (void)t; return 0; }
static void ctr_glGenRenderbuffers(GLsizei n, GLuint *r)
{
	for (int i = 0; i < n; i++)
		r[i] = 0;
}
static void ctr_glDeleteRenderbuffers(GLsizei n, const GLuint *r) { (void)n, (void)r; }
static void ctr_glBindRenderbuffer(GLenum t, GLuint r) { (void)t, (void)r; }
static void ctr_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h) { (void)t, (void)f, (void)w, (void)h; }
static void ctr_glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint r) { (void)t, (void)a, (void)rt, (void)r; }

static const struct {
	const char *name;
	void *fn;
} procs[] = {
#define X(ret, name, args) {#name, (void *)ctr_##name},
	BW_GL_FUNCS(X)
#undef X
};

void *CTRGL_GetProc(const char *name)
{
	for (size_t i = 0; i < BW_ARRAY_COUNT(procs); i++)
		if (!strcmp(procs[i].name, name))
			return procs[i].fn;
	return NULL;
}

/* --- set-up --------------------------------------------------------------------------------- */

void CTRGL_RegisterCvars(void)
{
	gl_max_size = Cvar_Get("gl_max_size", "256", CVAR_ARCHIVE,
	                       "Largest texture side on the GPU; bigger pictures are scaled down when loaded (3DS)");
	gl_ctr_texquality = Cvar_Get("gl_ctr_texquality", "0", CVAR_ARCHIVE,
	                             "1 keeps textures at 32 bits; 0 stores them as 16-bit, half the memory (3DS)");
	gl_ctr_mipmaps = Cvar_Get("gl_ctr_mipmaps", "1", CVAR_ARCHIVE, "Make mipmaps for textures that ask for them (3DS)");
}

bool CTRGL_Init(void)
{
	if (!C3D_Init(CMDBUF_SIZE)) {
		Con_Printf("citro3d failed to start\n");
		return false;
	}
	target = C3D_RenderTargetCreate(FB_W, FB_H, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (!target) {
		C3D_Fini();
		return false;
	}
	C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	ring = linearAlloc(RING_SIZE);
	dvlb = DVLB_ParseFile((u32 *)bwctr_shaders_shbin, bwctr_shaders_shbin_size);
	if (!ring || !dvlb || dvlb->numDVLE < 4) {
		Con_Printf("gl_ctr: couldn't set up the shaders or the vertex ring\n");
		return false;
	}
	init_pica(&pica[PK_2D], &dvlb->DVLE[0]);
	init_pica(&pica[PK_WORLD], &dvlb->DVLE[1]);
	init_pica(&pica[PK_MODEL], &dvlb->DVLE[2]);
	init_pica(&pica[PK_SKY], &dvlb->DVLE[3]);
	AttrInfo_Init(&attrinfo);
	AttrInfo_AddLoader(&attrinfo, 0, GPU_FLOAT, 3);         /* position */
	AttrInfo_AddLoader(&attrinfo, 1, GPU_FLOAT, 2);         /* texture coordinates */
	AttrInfo_AddLoader(&attrinfo, 2, GPU_FLOAT, 2);         /* lightmap coordinates */
	AttrInfo_AddLoader(&attrinfo, 3, GPU_FLOAT, 3);         /* normal */
	AttrInfo_AddLoader(&attrinfo, 4, GPU_UNSIGNED_BYTE, 4); /* colour, 0-255 */
	/* drawn when a unit has nothing bound */
	C3D_TexInit(&whitetex.tex, 8, 8, GPU_RGBA8);
	memset(whitetex.tex.data, 0xFF, 8 * 8 * 4);
	whitetex.allocated = true;

	memset(&st, 0, sizeof(st));
	st.bsrc = GL_ONE, st.bdst = GL_ZERO;
	st.depthfunc = GL_LESS;
	st.depthmask = true;
	st.colormask = GPU_WRITE_COLOR;
	st.depthfar = 1;
	st.cullface = GL_BACK;
	st.vp[2] = CTR_WIDTH, st.vp[3] = CTR_HEIGHT;
	st.vpdirty = true;
	ctr_ready = true;
	return true;
}

void CTRGL_EndFrame(void)
{
	if (!ctr_ready)
		return;
	begin_frame();
	C3D_FrameEnd(0);
	inframe = false;
	framenum++;
	boundkind = -1;
	st.vpdirty = true;
}

void CTRGL_Shutdown(void)
{
	if (!ctr_ready)
		return;
	if (inframe)
		CTRGL_EndFrame();
	C3D_RenderTargetDelete(target);
	if (dvlb) {
		for (int i = 1; i <= PK_SKY; i++)
			shaderProgramFree(&pica[i].prog);
		DVLB_Free(dvlb);
	}
	C3D_Fini();
	ctr_ready = false;
}
