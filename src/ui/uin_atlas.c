/* Bulwark engine - native UI: the atlas. Every glyph and picture the
 * screens draw is baked once into two RGBA pages (uin_stiletto.h lists
 * them) so a frame of UI is one or two draw calls.
 *
 * Fonts are rendered with a private copy of stb_truetype (public domain;
 * the renderer has its own, non-static one). Pictures decode through the
 * renderer (R_DecodeSVGSized draws SVG masks, which plain nanosvg skips)
 * and are box-filtered down with premultiplied averaging. A skyline packer
 * places everything.
 *
 * The result is cached as ui/native_<H>.txt, _p0.tga and _p1.tga in the
 * writable game dir, H being the layout height. On the PC a missing or
 * stale cache is baked and written on first use; "ui_native_bake 432" makes
 * the Xbox's, which the stager copies into its game dir. An Xbox without
 * the cache bakes in memory and writes nothing (its game dir is a disc).
 * ISC licence, see LICENSE. */
#include "uin_local.h"
#include "../common/fs.h"

#include <stdio.h>
#include <stdlib.h>

/* a private copy: the renderer has its own */
#define STBTT_STATIC
#define STBTT_malloc(x, u) ((void)(u), Mem_Alloc(x))
#define STBTT_free(x, u) ((void)(u), Mem_Free(x))
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../thirdparty/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define ATLAS_VERSION 3 /* 3: CFF FontMatrix slant */
#define MAX_GLYPHS 1400
#define MAX_KERNS 8192
#define MAX_FAMILIES 24
#define MAX_FILES 16
#define WHITE_SIZE 4 /* the fill block at every page's origin */

/* --- the manifest as tables --------------------------------------------------------- */

typedef struct {
	const char *name, *family;
	int weight;
	const char *path;
	float px, floor;
	int flags, page;
	const char *chars;
} facedef_t;

typedef struct {
	const char *name, *path;
	float w, h;
	int flags, page;
} imgdef_t;

#define X_FACE(id, fam, wt, path, px, fl, flags, page, chars) {#id, fam, wt, path, px, fl, flags, page, chars},
#define X_IMG(id, path, w, h, flags, page) {#id, path, w, h, flags, page},
static const facedef_t facedefs[UIN_NUM_FACES] = {UIN_FACES(X_FACE)};
static const imgdef_t imgdefs[UIN_NUM_REGIONS] = {UIN_IMAGES(X_IMG)};
#undef X_FACE
#undef X_IMG

/* ui_font lines */
static struct {
	char family[48];
	char path[96];
	int weight;
} families[MAX_FAMILIES];
static int numfamilies;

/* --- the loaded atlas ------------------------------------------------------------------ */

/* a kerning pair in 4 bytes: face << 24 | a << 16 | b << 8 | adjustment
 * in 1/16 px (signed), so a sorted array is searched on the top 24 bits */
typedef uint32_t kern_t;
#define KERN(face, a, b, adj16) ((uint32_t)(face) << 24 | (uint32_t)(a) << 16 | (uint32_t)(b) << 8 | (uint8_t)(int8_t)(adj16))

static struct {
	bool loaded;   /* pages uploaded for height h */
	bool tried;    /* a load for h failed: don't retry every frame */
	bool dirty;    /* reload on next use */
	int h;
	int pw[UIN_NUM_PAGES], ph[UIN_NUM_PAGES];
	int image[UIN_NUM_PAGES];
	uin_face_t faces[UIN_NUM_FACES];
	uin_region_t regions[UIN_NUM_REGIONS];
	uin_glyph_t glyphs[MAX_GLYPHS];
	int nglyphs;
	uint16_t gidx[UIN_NUM_FACES][256]; /* glyph index + 1 by code point */
	kern_t kerns[MAX_KERNS];
	int nkerns;
	char paths[UIN_NUM_FACES][96]; /* the file each face was baked from */
} A;

bool uin_font_add(const char *family, const char *path, int weight)
{
	int i;
	if (!family || !*family || !path || !*path)
		return false;
	/* a page set names fonts a game may not have: skip those quietly, as
	 * the Panorama build does */
	if (!FS_FileExists(path)) {
		Con_DPrintf("ui_font: no %s, skipped\n", path);
		return true;
	}
	for (i = 0; i < numfamilies; i++)
		if (!Str_ICmp(families[i].family, family) && families[i].weight == weight)
			break;
	if (i == numfamilies) {
		if (numfamilies == MAX_FAMILIES)
			return false;
		numfamilies++;
	}
	Str_Copy(families[i].family, family, sizeof(families[i].family));
	Str_Copy(families[i].path, path, sizeof(families[i].path));
	families[i].weight = weight;
	return true;
}

const char *uin_font_path(const char *family, int weight)
{
	int best = -1, bestd = 1 << 30;
	for (int i = 0; i < numfamilies; i++)
		if (!Str_ICmp(families[i].family, family)) {
			int d = abs(families[i].weight - weight);
			if (d < bestd) {
				bestd = d;
				best = i;
			}
		}
	return best >= 0 ? families[best].path : NULL;
}

/* --- lookups ---------------------------------------------------------------------------- */

int uin_atlas_height(void) { return A.loaded ? A.h : 0; }
int uin_atlas_image(int page) { return page >= 0 && page < UIN_NUM_PAGES && A.loaded ? A.image[page] : 0; }

void uin_atlas_page_size(int page, int *w, int *h)
{
	bool ok = page >= 0 && page < UIN_NUM_PAGES && A.loaded;
	*w = ok ? A.pw[page] : 0;
	*h = ok ? A.ph[page] : 0;
}

void uin_white_uv(int page, float *s, float *t)
{
	/* the block's middle: bilinear filtering stays inside it */
	*s = A.pw[page] ? (WHITE_SIZE / 2.0f) / A.pw[page] : 0;
	*t = A.ph[page] ? (WHITE_SIZE / 2.0f) / A.ph[page] : 0;
}

const uin_face_t *uin_face(int face) { return face >= 0 && face < UIN_NUM_FACES ? &A.faces[face] : NULL; }

const uin_glyph_t *uin_glyph(int face, int cp)
{
	int i;
	if (face < 0 || face >= UIN_NUM_FACES || cp < 0 || cp > 255)
		return NULL;
	i = A.gidx[face][cp];
	return i ? &A.glyphs[i - 1] : NULL;
}

float uin_kern(int face, int a, int b)
{
	uint32_t key;
	int lo = 0, hi = A.nkerns - 1;
	if (!A.nkerns || a > 255 || b > 255 || a < 0 || b < 0)
		return 0;
	key = KERN(face, a, b, 0) >> 8;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (A.kerns[mid] >> 8 == key)
			return (int8_t)(A.kerns[mid] & 255) / 16.0f;
		if (A.kerns[mid] >> 8 < key)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return 0;
}

const uin_region_t *uin_region(int id) { return id >= 0 && id < UIN_NUM_REGIONS ? &A.regions[id] : NULL; }

/* --- sizes at a layout height -------------------------------------------------------- */

static float face_px(const facedef_t *f, int h)
{
	float px = f->flags & UF_HUD ? f->px * h / UIN_FRAME_HUD_H : f->px;
	px = floorf(px + 0.5f);
	if (px < f->floor)
		px = f->floor;
	if (f->flags & UF_PIXEL) {
		/* pixel fonts: whole multiples of their 8 px grid only */
		px = floorf(px / 8 + 0.5f) * 8;
		if (px < 8)
			px = 8;
	}
	return px < 1 ? 1 : px;
}

static void image_size(const imgdef_t *d, int h, int *w, int *hh)
{
	float s = d->flags & UF_HUD ? (float)h / UIN_FRAME_HUD_H : 1;
	*w = BW_MAX(1, (int)floorf(d->w * s + 0.5f));
	*hh = BW_MAX(1, (int)floorf(d->h * s + 0.5f));
}

static void charset(const facedef_t *f, uint8_t *set)
{
	memset(set, 0, 256);
	if (!f->chars)
		for (int c = 32; c < 127; c++)
			set[c] = 1;
	else
		for (const unsigned char *p = (const unsigned char *)f->chars; *p; p++)
			set[*p] = 1;
	set[' '] = 1; /* every face can space */
}

/* --- font files -------------------------------------------------------------------------- */

typedef struct {
	char path[96];
	uint8_t *data;
	size_t len;
	stbtt_fontinfo info;
	bool ok;
} fontfile_t;

typedef struct {
	fontfile_t files[MAX_FILES];
	int nfiles;
} fontset_t;

static fontfile_t *open_font(fontset_t *fs, const char *path)
{
	fontfile_t *f;
	for (int i = 0; i < fs->nfiles; i++)
		if (!strcmp(fs->files[i].path, path))
			return fs->files[i].ok ? &fs->files[i] : NULL;
	if (fs->nfiles == MAX_FILES)
		return NULL;
	f = &fs->files[fs->nfiles++];
	memset(f, 0, sizeof(*f));
	Str_Copy(f->path, path, sizeof(f->path));
	f->data = (uint8_t *)FS_LoadFile(path, &f->len);
	if (f->data) {
		int off = stbtt_GetFontOffsetForIndex(f->data, 0);
		f->ok = off >= 0 && stbtt_InitFont(&f->info, f->data, off);
		if (!f->ok)
			Con_Printf("^3ui: %s isn't a font stb_truetype reads\n", path);
	}
	return f->ok ? f : NULL;
}

static void close_fonts(fontset_t *fs)
{
	for (int i = 0; i < fs->nfiles; i++)
		Mem_Free(fs->files[i].data);
	fs->nfiles = 0;
}

/* the file a face uses: its ui_font family, its own path, then Bulwark's
 * pixel font. A runtime bake on the Xbox leaves out the heavy files. */
static fontfile_t *face_font(fontset_t *fs, int face, bool runtime, char *path, size_t pathsize)
{
	const facedef_t *d = &facedefs[face];
	const char *family = d->family, *own = d->path, *p;
	int weight = d->weight;
	fontfile_t *f;
#ifdef BW_XBOX
	if (runtime && (d->flags & UF_HEAVY)) {
		static const struct {
			const char *family;
			int weight;
			const char *path;
		} sub = {UIN_HEAVY_SUBSTITUTE};
		family = sub.family;
		weight = sub.weight;
		own = sub.path;
	}
#else
	(void)runtime;
#endif
	if ((p = uin_font_path(family, weight)) && (f = open_font(fs, p))) {
		Str_Copy(path, p, pathsize);
		return f;
	}
	if ((f = open_font(fs, own))) {
		Str_Copy(path, own, pathsize);
		return f;
	}
	if ((f = open_font(fs, UIN_FALLBACK_FONT))) {
		Con_Printf("^3ui: no %s, %s drawn in %s\n", own, facedefs[face].name, UIN_FALLBACK_FONT);
		Str_Copy(path, UIN_FALLBACK_FONT, pathsize);
		return f;
	}
	path[0] = 0;
	return NULL;
}

/* --- slanted CFF faces --------------------------------------------------------------------
 * A CFF font can keep upright outlines and slant them with its FontMatrix
 * (HelveticaNeue97BlackCondensedOblique.otf: [a 0 c d 0 0], c/a the
 * slant). stb_truetype ignores the matrix and draws such a face upright, so
 * the baker slants the outline itself. TrueType outlines are drawn as
 * they are. */

/* a CFF real operand (b0 30): packed decimal nibbles */
static double cff_real(stbtt__buf *b)
{
	char txt[32];
	int n = 0;
	stbtt__buf_skip(b, 1);
	while (b->cursor < b->size) {
		int v = stbtt__buf_get8(b);
		for (int k = 0; k < 2; k++) {
			int nib = k ? v & 15 : v >> 4;
			if (nib == 15 || n >= (int)sizeof(txt) - 3) {
				txt[n] = 0;
				return atof(txt);
			}
			if (nib <= 9)
				txt[n++] = (char)('0' + nib);
			else if (nib == 10)
				txt[n++] = '.';
			else if (nib == 11 || nib == 12) {
				txt[n++] = 'e';
				if (nib == 12)
					txt[n++] = '-';
			} else if (nib == 14)
				txt[n++] = '-';
		}
	}
	txt[n] = 0;
	return atof(txt);
}

/* the x slant (x' = x + slant y, font units) of a CFF face's FontMatrix */
static float cff_slant(const stbtt_fontinfo *info)
{
	stbtt__buf b = info->cff, top, m;
	double v[6];
	int n = 0;
	if (!b.data || b.size < 4)
		return 0;
	stbtt__buf_skip(&b, 2);
	stbtt__buf_seek(&b, stbtt__buf_get8(&b)); /* past the header */
	stbtt__cff_get_index(&b);                 /* names */
	top = stbtt__cff_get_index(&b);
	if (stbtt__cff_index_count(&top) < 1)
		return 0;
	top = stbtt__cff_index_get(top, 0);
	m = stbtt__dict_get(&top, 0x100 | 7); /* FontMatrix, operator 12 7 */
	while (n < 6 && m.cursor < m.size)
		v[n++] = stbtt__buf_peek8(&m) == 30 ? cff_real(&m) : (double)(int)stbtt__cff_int(&m);
	if (n < 6 || v[0] == 0 || v[3] == 0)
		return 0;
	return fabs(v[2] / v[0]) > 0.001 ? (float)(v[2] / v[0]) : 0;
}

/* the glyph's outline, slanted: free it with stbtt_FreeShape */
static int slanted_shape(const stbtt_fontinfo *info, int g, float slant, stbtt_vertex **pv)
{
	int n = stbtt_GetGlyphShape(info, g, pv);
	for (int i = 0; i < n; i++) {
		stbtt_vertex *v = &(*pv)[i];
		v->x = (stbtt_vertex_type)floorf(v->x + slant * v->y + 0.5f);
		v->cx = (stbtt_vertex_type)floorf(v->cx + slant * v->cy + 0.5f);
		v->cx1 = (stbtt_vertex_type)floorf(v->cx1 + slant * v->cy1 + 0.5f);
	}
	return n;
}

/* as stbtt_GetGlyphBitmapBox, of the slanted outline (control points
 * included: a box at most a little large) */
static void slanted_box(const stbtt_fontinfo *info, int g, float slant, float s, int *x0, int *y0, int *x1, int *y1)
{
	stbtt_vertex *v;
	int n = slanted_shape(info, g, slant, &v);
	float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
	for (int i = 0; i < n; i++) {
		lx = BW_MIN(lx, (float)BW_MIN(v[i].x, v[i].type == STBTT_vline || v[i].type == STBTT_vmove ? v[i].x : v[i].cx));
		hx = BW_MAX(hx, (float)BW_MAX(v[i].x, v[i].type == STBTT_vline || v[i].type == STBTT_vmove ? v[i].x : v[i].cx));
		ly = BW_MIN(ly, (float)BW_MIN(v[i].y, v[i].type == STBTT_vline || v[i].type == STBTT_vmove ? v[i].y : v[i].cy));
		hy = BW_MAX(hy, (float)BW_MAX(v[i].y, v[i].type == STBTT_vline || v[i].type == STBTT_vmove ? v[i].y : v[i].cy));
		if (v[i].type == STBTT_vcubic) {
			lx = BW_MIN(lx, (float)v[i].cx1), hx = BW_MAX(hx, (float)v[i].cx1);
			ly = BW_MIN(ly, (float)v[i].cy1), hy = BW_MAX(hy, (float)v[i].cy1);
		}
	}
	stbtt_FreeShape(info, v);
	if (n <= 0) {
		*x0 = *y0 = *x1 = *y1 = 0;
		return;
	}
	*x0 = (int)floorf(lx * s);
	*y0 = (int)floorf(-hy * s);
	*x1 = (int)ceilf(hx * s);
	*y1 = (int)ceilf(-ly * s);
}

/* as stbtt_MakeGlyphBitmap, of the slanted outline; x0 y0 its box's corner */
static void slanted_bitmap(const stbtt_fontinfo *info, int g, float slant, float s, uint8_t *out, int w, int h, int x0,
                           int y0)
{
	stbtt_vertex *v;
	stbtt__bitmap bm;
	int n = slanted_shape(info, g, slant, &v);
	bm.pixels = out;
	bm.w = w;
	bm.h = h;
	bm.stride = w;
	memset(out, 0, (size_t)w * h);
	if (n > 0)
		stbtt_Rasterize(&bm, 0.35f, v, n, s, s, 0, 0, x0, y0, 1, NULL);
	stbtt_FreeShape(info, v);
}

/* --- packing ---------------------------------------------------------------------------- */

enum { IT_WHITE, IT_GLYPH, IT_REGION };

typedef struct {
	uint16_t w, h;      /* with padding */
	uint8_t pad, kind, page;
	uint16_t ref;       /* glyph or region index */
	uint16_t x, y;      /* top left of the padded box */
} item_t;

#define MAX_ITEMS (MAX_GLYPHS + UIN_NUM_REGIONS + UIN_NUM_PAGES)
#define MAX_SKY 512

typedef struct {
	int x, y, w;
} sky_t;

/* skyline bottom-left: each rect goes where its top is lowest */
static bool skyline_pack(item_t **items, int n, int pw, int ph, sky_t *sky)
{
	int nsky = 1;
	sky[0].x = 0, sky[0].y = 0, sky[0].w = pw;
	for (int k = 0; k < n; k++) {
		item_t *it = items[k];
		int best = -1, besty = 1 << 30, bestx = 0;
		for (int i = 0; i < nsky; i++) {
			int x = sky[i].x, y = 0, wl = it->w, j = i;
			if (x + it->w > pw)
				break;
			while (wl > 0 && j < nsky) {
				y = BW_MAX(y, sky[j].y);
				wl -= sky[j].w;
				j++;
			}
			if (wl > 0 || y + it->h > ph)
				continue;
			if (y < besty) {
				besty = y;
				best = i;
				bestx = x;
			}
		}
		if (best < 0)
			return false;
		it->x = (uint16_t)bestx;
		it->y = (uint16_t)besty;
		/* the new segment, then what it covers trimmed away */
		{
			sky_t seg = {bestx, besty + it->h, it->w};
			int end = bestx + it->w, i = best;
			while (i < nsky && sky[i].x + sky[i].w <= end)
				i++;
			if (i < nsky && sky[i].x < end) {
				sky[i].w -= end - sky[i].x;
				sky[i].x = end;
			}
			if (nsky - (i - best) + 1 > MAX_SKY)
				return false;
			memmove(&sky[best + 1], &sky[i], (size_t)(nsky - i) * sizeof(sky_t));
			nsky -= i - best;
			sky[best] = seg;
			nsky++;
			/* neighbours at one height merge */
			for (int m = 0; m + 1 < nsky;)
				if (sky[m].y == sky[m + 1].y) {
					sky[m].w += sky[m + 1].w;
					memmove(&sky[m + 1], &sky[m + 2], (size_t)(nsky - m - 2) * sizeof(sky_t));
					nsky--;
				} else {
					m++;
				}
		}
	}
	return true;
}

static int cmp_height(const void *a, const void *b)
{
	const item_t *x = *(item_t *const *)a, *y = *(item_t *const *)b;
	if (x->kind == IT_WHITE || y->kind == IT_WHITE)
		return (y->kind == IT_WHITE) - (x->kind == IT_WHITE);
	if (x->h != y->h)
		return y->h - x->h;
	return y->w - x->w;
}

/* --- pixels ---------------------------------------------------------------------------- */

/* box filter src (sw x sh RGBA) to exactly dw x dh, averaging premultiplied
 * colour so transparent texels don't darken the edges */
static uint8_t *box_filter(const uint8_t *src, int sw, int sh, int dw, int dh)
{
	float *tmp = Mem_Alloc(sizeof(float) * 4 * (size_t)dw * sh);
	uint8_t *out = Mem_Alloc((size_t)dw * dh * 4);
	float fx = (float)sw / dw, fy = (float)sh / dh;
	for (int y = 0; y < sh; y++)
		for (int x = 0; x < dw; x++) {
			float a0 = x * fx, a1 = (x + 1) * fx, acc[4] = {0, 0, 0, 0};
			for (int sx = (int)a0; sx < sw && sx < a1; sx++) {
				float wgt = BW_MIN(a1, sx + 1.0f) - BW_MAX(a0, (float)sx);
				const uint8_t *p = src + ((size_t)y * sw + sx) * 4;
				float al = p[3] / 255.0f;
				acc[0] += p[0] * al * wgt, acc[1] += p[1] * al * wgt, acc[2] += p[2] * al * wgt;
				acc[3] += p[3] * wgt;
			}
			memcpy(tmp + ((size_t)y * dw + x) * 4, acc, sizeof(acc));
		}
	for (int y = 0; y < dh; y++)
		for (int x = 0; x < dw; x++) {
			float a0 = y * fy, a1 = (y + 1) * fy, acc[4] = {0, 0, 0, 0}, tot = 0;
			uint8_t *o = out + ((size_t)y * dw + x) * 4;
			for (int sy = (int)a0; sy < sh && sy < a1; sy++) {
				float wgt = BW_MIN(a1, sy + 1.0f) - BW_MAX(a0, (float)sy);
				const float *p = tmp + ((size_t)sy * dw + x) * 4;
				for (int c = 0; c < 4; c++)
					acc[c] += p[c] * wgt;
			}
			tot = fx * fy;
			if (acc[3] > 0) {
				for (int c = 0; c < 3; c++)
					o[c] = (uint8_t)BW_CLAMP(acc[c] / (acc[3] / 255.0f) + 0.5f, 0, 255);
			} else {
				o[0] = o[1] = o[2] = 255;
			}
			o[3] = (uint8_t)BW_CLAMP(acc[3] / tot + 0.5f, 0, 255);
		}
	Mem_Free(tmp);
	return out;
}

/* a picture at w x h: SVGs drawn at ss times the size then filtered
 * (antialiased as a browser would), others filtered */
static uint8_t *load_image(const char *path, int w, int h, int ss)
{
	size_t len;
	uint8_t *data, *px, *out = NULL;
	int sw, sh;
	if (!strcmp(path, "*disc")) {
		out = Mem_Alloc((size_t)w * h * 4);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				/* 4x4 samples per pixel against the inscribed circle */
				int in = 0;
				float r = BW_MIN(w, h) * 0.5f;
				for (int sy = 0; sy < 4; sy++)
					for (int sx = 0; sx < 4; sx++) {
						float dx = x + (sx + 0.5f) / 4 - w * 0.5f, dy = y + (sy + 0.5f) / 4 - h * 0.5f;
						in += dx * dx + dy * dy <= r * r;
					}
				uint8_t *o = out + ((size_t)y * w + x) * 4;
				o[0] = o[1] = o[2] = 255;
				o[3] = (uint8_t)(in * 255 / 16);
			}
		return out;
	}
	if (!(data = (uint8_t *)FS_LoadFile(path, &len)))
		return NULL;
	if ((px = R_DecodeSVGSized(data, len, w * ss, h * ss))) {
		out = box_filter(px, w * ss, h * ss, w, h);
	} else if ((px = R_DecodeImageFile(data, len, &sw, &sh))) {
		out = box_filter(px, sw, sh, w, h);
	}
	Mem_Free(px);
	Mem_Free(data);
	return out;
}

/* copies w x h RGBA into the page at x y, then repeats its edge pixels into
 * pad pixels around it, so a stretched or filtered region keeps its edges */
static void blit_extruded(uint8_t *page, int pw, const uint8_t *src, int x, int y, int w, int h, int pad)
{
	for (int j = -pad; j < h + pad; j++)
		for (int i = -pad; i < w + pad; i++) {
			int si = BW_CLAMP(i, 0, w - 1), sj = BW_CLAMP(j, 0, h - 1);
			memcpy(page + ((size_t)(y + j) * pw + x + i) * 4, src + ((size_t)sj * w + si) * 4, 4);
		}
}

/* a glyph's ink into the page; with an outline, the ink spreads a pixel
 * into black at alpha 0.5 (design 4): alpha = ink + (1 - ink) * 0.5 * spread,
 * colour = ink / alpha, so a vertex colour tints the ink and not the edge */
static void blit_glyph(uint8_t *page, int pw, const uint8_t *ink, int iw, int ih, int x, int y, int outline)
{
	int w = iw + 2 * outline, h = ih + 2 * outline;
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			uint8_t *o = page + ((size_t)(y + j) * pw + x + i) * 4;
			int si = i - outline, sj = j - outline;
			float a = si >= 0 && sj >= 0 && si < iw && sj < ih ? ink[sj * iw + si] / 255.0f : 0;
			if (outline) {
				float spread = 0;
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						int xx = si + dx, yy = sj + dy;
						float v, wgt = dx && dy ? 0.7f : 1;
						if (xx < 0 || yy < 0 || xx >= iw || yy >= ih)
							continue;
						v = ink[yy * iw + xx] / 255.0f * wgt;
						spread = BW_MAX(spread, v);
					}
				float al = a + (1 - a) * 0.5f * spread;
				uint8_t c = al > 0 ? (uint8_t)BW_CLAMP(a / al * 255 + 0.5f, 0, 255) : 255;
				o[0] = o[1] = o[2] = c;
				o[3] = (uint8_t)(al * 255 + 0.5f);
			} else {
				o[0] = o[1] = o[2] = 255;
				o[3] = (uint8_t)(a * 255 + 0.5f);
			}
		}
}

/* --- baking ------------------------------------------------------------------------------- */

typedef struct {
	uint8_t *px[UIN_NUM_PAGES];
	int pw[UIN_NUM_PAGES], ph[UIN_NUM_PAGES];
} pages_t;

static void free_pages(pages_t *p)
{
	for (int i = 0; i < UIN_NUM_PAGES; i++) {
		Mem_Free(p->px[i]);
		p->px[i] = NULL;
	}
}

static int cmp_kern(const void *a, const void *b)
{
	uint32_t x = *(const kern_t *)a, y = *(const kern_t *)b;
	return x < y ? -1 : x > y;
}

/* page sizes tried in turn when the manifest doesn't fit (design 5.2) */
static const int page_sizes[UIN_NUM_PAGES][3][2] = {
	{{512, 256}, {512, 512}, {1024, 512}},
	{{512, 512}, {1024, 512}, {1024, 1024}},
};

/* bakes every face and image for layout height h into A and pages */
static bool bake(int h, bool runtime, pages_t *out)
{
	/* scratch for the bake alone, freed with it: nothing stays resident */
	struct work {
		fontset_t fs;
		item_t items[MAX_ITEMS];
		item_t *sorted[MAX_ITEMS];
		sky_t sky[MAX_SKY];
		float scales[UIN_NUM_FACES];
		float slants[UIN_NUM_FACES];
		fontfile_t *fonts[UIN_NUM_FACES];
	} *wk = Mem_Alloc(sizeof(*wk));
	fontset_t *fs = &wk->fs;
	item_t *items = wk->items, **sorted = wk->sorted;
	float *scales = wk->scales, *slants = wk->slants;
	fontfile_t **fonts = wk->fonts;
	int nitems = 0;
	double t0 = Sys_Seconds(), t_metrics, t_pack, t_glyphs = 0, t_images = 0;
	memset(out, 0, sizeof(*out));
	memset(A.faces, 0, sizeof(A.faces));
	memset(A.regions, 0, sizeof(A.regions));
	memset(A.gidx, 0, sizeof(A.gidx));
	A.nglyphs = A.nkerns = 0;

	for (int p = 0; p < UIN_NUM_PAGES; p++) {
		item_t *it = &items[nitems++];
		memset(it, 0, sizeof(*it));
		it->w = it->h = WHITE_SIZE + 1;
		it->kind = IT_WHITE;
		it->page = (uint8_t)p;
	}
	/* glyph boxes */
	for (int f = 0; f < UIN_NUM_FACES; f++) {
		const facedef_t *d = &facedefs[f];
		uin_face_t *face = &A.faces[f];
		uint8_t set[256];
		int asc, desc, gap;
		float s;
		fontfile_t *ff = face_font(fs, f, runtime, A.paths[f], sizeof(A.paths[f]));
		face->px = face_px(d, h);
		face->page = (uint8_t)d->page;
		face->outline = d->flags & UF_OUTLINE ? 1 : 0;
		fonts[f] = ff;
		if (!ff)
			continue;
		s = scales[f] = stbtt_ScaleForMappingEmToPixels(&ff->info, face->px);
		slants[f] = cff_slant(&ff->info);
		stbtt_GetFontVMetrics(&ff->info, &asc, &desc, &gap);
		/* the em box, split as the font splits ascent and descent */
		face->ascent = asc - desc > 0 ? face->px * asc / (float)(asc - desc) : face->px * 0.8f;
		face->descent = face->px - face->ascent;
		face->line = (asc - desc + gap) * s;
		face->ok = true;
		charset(d, set);
		for (int cp = 0; cp < 256; cp++) {
			int g, x0, y0, x1, y1, adv, lsb;
			uin_glyph_t *gl;
			if (!set[cp] || (!(g = stbtt_FindGlyphIndex(&ff->info, cp)) && cp != ' '))
				continue;
			if (A.nglyphs == MAX_GLYPHS || nitems == MAX_ITEMS) {
				Con_Printf("^3ui: atlas glyph table full at %s\n", d->name);
				break;
			}
			if (slants[f])
				slanted_box(&ff->info, g, slants[f], s, &x0, &y0, &x1, &y1);
			else
				stbtt_GetGlyphBitmapBox(&ff->info, g, s, s, &x0, &y0, &x1, &y1);
			stbtt_GetGlyphHMetrics(&ff->info, g, &adv, &lsb);
			if (stbtt_IsGlyphEmpty(&ff->info, g))
				x0 = y0 = x1 = y1 = 0;
			gl = &A.glyphs[A.nglyphs];
			memset(gl, 0, sizeof(*gl));
			gl->face = (uint8_t)f;
			gl->cp = (uint16_t)cp;
			gl->page = (uint8_t)d->page;
			gl->adv64 = (uint16_t)BW_CLAMP(adv * s * 64 + 0.5f, 0, 65535);
			if (x1 > x0 && y1 > y0) {
				item_t *it = &items[nitems++];
				int o = face->outline;
				gl->w = (uint8_t)BW_MIN(255, x1 - x0 + 2 * o);
				gl->h = (uint8_t)BW_MIN(255, y1 - y0 + 2 * o);
				gl->xoff = (int8_t)BW_CLAMP(x0 - o, -128, 127);
				gl->yoff = (int8_t)BW_CLAMP(y0 - o, -128, 127);
				memset(it, 0, sizeof(*it));
				it->pad = d->flags & UF_ROT ? 2 : 1;
				it->w = (uint16_t)(gl->w + 2 * it->pad);
				it->h = (uint16_t)(gl->h + 2 * it->pad);
				it->kind = IT_GLYPH;
				it->page = (uint8_t)d->page;
				it->ref = (uint16_t)A.nglyphs;
			}
			A.gidx[f][cp] = (uint16_t)++A.nglyphs;
		}
		/* pair kerning among the baked glyphs (kern table or GPOS) */
		{
			int cps[256], gis[256], n = 0;
			for (int cp = 33; cp < 256; cp++)
				if (A.gidx[f][cp] && (gis[n] = stbtt_FindGlyphIndex(&ff->info, cp)))
					cps[n++] = cp;
			for (int i = 0; i < n; i++)
				for (int j = 0; j < n; j++) {
					int k = stbtt_GetGlyphKernAdvance(&ff->info, gis[i], gis[j]);
					int adj = k ? (int)floorf(k * s * 16 + 0.5f) : 0;
					if (adj && A.nkerns < MAX_KERNS)
						A.kerns[A.nkerns++] = KERN(f, cps[i], cps[j], BW_CLAMP(adj, -128, 127));
				}
		}
	}
	qsort(A.kerns, (size_t)A.nkerns, sizeof(kern_t), cmp_kern);
	t_metrics = Sys_Seconds();
	/* image boxes */
	for (int r = 0; r < UIN_NUM_REGIONS; r++) {
		const imgdef_t *d = &imgdefs[r];
		item_t *it = &items[nitems++];
		int w, hh;
		image_size(d, h, &w, &hh);
		memset(it, 0, sizeof(*it));
		it->pad = d->flags & UF_ROT ? 2 : 1;
		it->w = (uint16_t)(w + 2 * it->pad);
		it->h = (uint16_t)(hh + 2 * it->pad);
		it->kind = IT_REGION;
		it->page = (uint8_t)d->page;
		it->ref = (uint16_t)r;
		A.regions[r].w = (uint16_t)w;
		A.regions[r].h = (uint16_t)hh;
		A.regions[r].page = (uint8_t)d->page;
	}
	/* pack each page, growing it when it doesn't fit */
	for (int p = 0; p < UIN_NUM_PAGES; p++) {
		int n = 0, try;
		for (int i = 0; i < nitems; i++)
			if (items[i].page == p)
				sorted[n++] = &items[i];
		qsort(sorted, (size_t)n, sizeof(*sorted), cmp_height);
		for (try = 0; try < 3; try++)
			if (skyline_pack(sorted, n, page_sizes[p][try][0], page_sizes[p][try][1], wk->sky))
				break;
		if (try == 3) {
			Con_Printf("^1ui: atlas page %d doesn't fit in %dx%d\n", p, page_sizes[p][2][0], page_sizes[p][2][1]);
			close_fonts(fs);
			free_pages(out);
			Mem_Free(wk);
			return false;
		}
		if (try > 0)
			Con_Printf("^3ui: atlas page %d grew to %dx%d\n", p, page_sizes[p][try][0], page_sizes[p][try][1]);
		out->pw[p] = page_sizes[p][try][0];
		out->ph[p] = page_sizes[p][try][1];
		out->px[p] = Mem_Alloc((size_t)out->pw[p] * out->ph[p] * 4);
		/* transparent white: filtering at a glyph's edge never pulls in black */
		for (size_t q = 0; q < (size_t)out->pw[p] * out->ph[p]; q++)
			out->px[p][q * 4] = out->px[p][q * 4 + 1] = out->px[p][q * 4 + 2] = 255;
	}
	t_pack = Sys_Seconds();
	/* draw everything where it was placed */
	for (int i = 0; i < nitems; i++) {
		item_t *it = &items[i];
		double ti = Sys_Seconds();
		uint8_t *page = out->px[it->page];
		int pw = out->pw[it->page];
		if (it->kind == IT_WHITE) {
			for (int y = 0; y < WHITE_SIZE; y++)
				memset(page + ((size_t)(it->y + y) * pw + it->x) * 4, 255, WHITE_SIZE * 4);
		} else if (it->kind == IT_GLYPH) {
			uin_glyph_t *gl = &A.glyphs[it->ref];
			fontfile_t *ff = fonts[gl->face];
			const facedef_t *d = &facedefs[gl->face];
			int o = A.faces[gl->face].outline, iw = gl->w - 2 * o, ih = gl->h - 2 * o;
			uint8_t *ink = Mem_Alloc((size_t)iw * ih);
			int g = stbtt_FindGlyphIndex(&ff->info, gl->cp);
			float s = scales[gl->face];
			if (slants[gl->face])
				slanted_bitmap(&ff->info, g, slants[gl->face], s, ink, iw, ih, gl->xoff + o, gl->yoff + o);
			else
				stbtt_MakeGlyphBitmap(&ff->info, ink, iw, ih, iw, s, s, g);
			if (d->flags & UF_PIXEL)
				for (int q = 0; q < iw * ih; q++)
					ink[q] = ink[q] >= 128 ? 255 : 0;
			gl->x = (uint16_t)(it->x + it->pad);
			gl->y = (uint16_t)(it->y + it->pad);
			blit_glyph(page, pw, ink, iw, ih, gl->x, gl->y, o);
			Mem_Free(ink);
		} else {
			uin_region_t *r = &A.regions[it->ref];
			/* a runtime bake (an Xbox without the cache) draws SVGs at 2x */
			uint8_t *px = load_image(imgdefs[it->ref].path, r->w, r->h, runtime ? 2 : 4);
			r->x = (uint16_t)(it->x + it->pad);
			r->y = (uint16_t)(it->y + it->pad);
			if (px) {
				blit_extruded(page, pw, px, r->x, r->y, r->w, r->h, it->pad);
				r->ok = true;
				Mem_Free(px);
			} else {
				Con_Printf("^3ui: couldn't load %s\n", imgdefs[it->ref].path);
			}
		}
		*(it->kind == IT_REGION ? &t_images : &t_glyphs) += Sys_Seconds() - ti;
	}
	close_fonts(fs);
	{
		/* how full the pages are, for the manifest's owners */
		long used[UIN_NUM_PAGES] = {0};
		for (int i = 0; i < nitems; i++)
			used[items[i].page] += (long)items[i].w * items[i].h;
		Con_DPrintf("ui: baked atlas H %d in %.0f ms (fonts and kerning %.0f, packing %.0f, glyphs %.0f, images %.0f): "
		            "%d glyphs, %d kerning pairs, page 0 %ld%%, page 1 %ld%%\n",
		            h, (Sys_Seconds() - t0) * 1000, (t_metrics - t0) * 1000, (t_pack - t_metrics) * 1000,
		            t_glyphs * 1000, t_images * 1000, A.nglyphs, A.nkerns,
		            used[0] * 100 / ((long)out->pw[0] * out->ph[0]), used[1] * 100 / ((long)out->pw[1] * out->ph[1]));
	}
	Mem_Free(wk);
	for (int p = 0; p < UIN_NUM_PAGES; p++) {
		A.pw[p] = out->pw[p];
		A.ph[p] = out->ph[p];
	}
	return true;
}

/* --- the cache ------------------------------------------------------------------------------ */

/* changes with anything that changes the bake: the manifest, the files the
 * faces resolve to, this code's version */
static uint32_t manifest_hash(int h, bool runtime)
{
	uint32_t x = 2166136261u;
	char buf[256], path[96];
#define MIX(s)                                     \
	for (const char *q_ = (s); *q_; q_++) {         \
		x ^= (uint8_t)*q_;                          \
		x *= 16777619u;                             \
	}
	snprintf(buf, sizeof(buf), "v%d h%d r%d", ATLAS_VERSION, h, runtime ? 1 : 0);
	MIX(buf);
	for (int f = 0; f < UIN_NUM_FACES; f++) {
		const facedef_t *d = &facedefs[f];
		const char *p = uin_font_path(d->family, d->weight);
		Str_Copy(path, p ? p : d->path, sizeof(path));
		snprintf(buf, sizeof(buf), "%s|%s|%d|%s|%g|%g|%d|%d|", d->name, d->family, d->weight, path, d->px, d->floor,
		         d->flags, d->page);
		MIX(buf);
		MIX(d->chars ? d->chars : "*ascii*");
	}
	for (int r = 0; r < UIN_NUM_REGIONS; r++) {
		const imgdef_t *d = &imgdefs[r];
		snprintf(buf, sizeof(buf), "%s|%s|%g|%g|%d|%d|", d->name, d->path, d->w, d->h, d->flags, d->page);
		MIX(buf);
	}
#undef MIX
	return x;
}

static void cache_name(char *out, size_t size, int h, const char *suffix)
{
	snprintf(out, size, "ui/native_%d%s", h, suffix);
}

static bool write_tga(const char *rel, const uint8_t *px, int w, int h)
{
	char path[1024];
	uint8_t head[18] = {0};
	uint8_t *row;
	FILE *f;
	if (!FS_WritePath(rel, path, sizeof(path)) || !FS_CreatePath(path) || !(f = fopen(path, "wb")))
		return false;
	head[2] = 2; /* uncompressed true colour */
	head[12] = (uint8_t)w, head[13] = (uint8_t)(w >> 8);
	head[14] = (uint8_t)h, head[15] = (uint8_t)(h >> 8);
	head[16] = 32;
	head[17] = 0x28; /* 8 alpha bits, rows top first */
	fwrite(head, 1, sizeof(head), f);
	row = Mem_Alloc((size_t)w * 4);
	for (int y = 0; y < h; y++) {
		const uint8_t *s = px + (size_t)y * w * 4;
		for (int x = 0; x < w; x++) {
			row[x * 4] = s[x * 4 + 2];
			row[x * 4 + 1] = s[x * 4 + 1];
			row[x * 4 + 2] = s[x * 4];
			row[x * 4 + 3] = s[x * 4 + 3];
		}
		fwrite(row, 1, (size_t)w * 4, f);
	}
	Mem_Free(row);
	return fclose(f) == 0;
}

/* our own TGAs only: uncompressed, 32 bits. Converted in place to RGBA, so
 * the file buffer is the only copy before the upload. */
static uint8_t *read_tga(const char *rel, int *w, int *h)
{
	size_t len;
	uint8_t *d = (uint8_t *)FS_LoadFile(rel, &len), *px;
	int top;
	if (!d)
		return NULL;
	*w = d[12] | d[13] << 8;
	*h = d[14] | d[15] << 8;
	top = d[17] & 0x20;
	if (len < 18 || d[2] != 2 || d[16] != 32 || *w <= 0 || *h <= 0 || len < 18 + (size_t)d[0] + (size_t)*w * *h * 4) {
		Mem_Free(d);
		return NULL;
	}
	px = d + 18 + d[0];
	for (size_t q = 0; q < (size_t)*w * *h; q++) {
		uint8_t t = px[q * 4];
		px[q * 4] = px[q * 4 + 2];
		px[q * 4 + 2] = t;
	}
	if (!top)
		for (int y = 0; y < *h / 2; y++) {
			uint8_t *a = px + (size_t)y * *w * 4, *b = px + (size_t)(*h - 1 - y) * *w * 4;
			for (int x = 0; x < *w * 4; x++) {
				uint8_t t = a[x];
				a[x] = b[x];
				b[x] = t;
			}
		}
	/* the caller frees from the pixels: move them to the front */
	memmove(d, px, (size_t)*w * *h * 4);
	return d;
}

static bool write_cache(int h, const pages_t *pg)
{
	char rel[64], path[1024];
	strbuf_t sb = {0};
	FILE *f;
	bool ok;
	SB_Printf(&sb, "bwui-atlas %d\nheight %d\nmanifest %08x\n", ATLAS_VERSION, h, manifest_hash(h, false));
	for (int p = 0; p < UIN_NUM_PAGES; p++)
		SB_Printf(&sb, "page %d %d %d\n", p, pg->pw[p], pg->ph[p]);
	for (int f2 = 0; f2 < UIN_NUM_FACES; f2++) {
		const uin_face_t *fc = &A.faces[f2];
		SB_Printf(&sb, "face %s %d %g %g %g %g %d %d %s\n", facedefs[f2].name, fc->ok, fc->px, fc->ascent, fc->descent,
		          fc->line, fc->outline, fc->page, A.paths[f2][0] ? A.paths[f2] : "-");
	}
	for (int i = 0; i < A.nglyphs; i++) {
		const uin_glyph_t *g = &A.glyphs[i];
		SB_Printf(&sb, "g %s %d %d %d %d %d %d %d %d\n", facedefs[g->face].name, g->cp, g->x, g->y, g->w, g->h, g->xoff,
		          g->yoff, g->adv64);
	}
	for (int r = 0; r < UIN_NUM_REGIONS; r++) {
		const uin_region_t *rg = &A.regions[r];
		SB_Printf(&sb, "r %s %d %d %d %d %d\n", imgdefs[r].name, rg->ok, rg->x, rg->y, rg->w, rg->h);
	}
	for (int k = 0; k < A.nkerns; k++)
		SB_Printf(&sb, "k %s %u %u %d\n", facedefs[A.kerns[k] >> 24].name, (A.kerns[k] >> 16) & 255,
		          (A.kerns[k] >> 8) & 255, (int8_t)(A.kerns[k] & 255));
	cache_name(rel, sizeof(rel), h, ".txt");
	ok = FS_WritePath(rel, path, sizeof(path)) && FS_CreatePath(path) && (f = fopen(path, "wb"));
	if (ok) {
		ok = fwrite(sb.data, 1, sb.len, f) == sb.len;
		ok = fclose(f) == 0 && ok;
	}
	SB_Free(&sb);
	for (int p = 0; ok && p < UIN_NUM_PAGES; p++) {
		cache_name(rel, sizeof(rel), h, p ? "_p1.tga" : "_p0.tga");
		ok = write_tga(rel, pg->px[p], pg->pw[p], pg->ph[p]);
	}
	if (ok)
		Con_Printf("ui: wrote the atlas cache ui/native_%d.* in %s\n", h, FS_GameDir());
	else
		Con_Printf("^3ui: couldn't write the atlas cache ui/native_%d.*\n", h);
	return ok;
}

static int face_by_name(const char *name)
{
	for (int f = 0; f < UIN_NUM_FACES; f++)
		if (!strcmp(facedefs[f].name, name))
			return f;
	return -1;
}

static int region_by_name(const char *name)
{
	for (int r = 0; r < UIN_NUM_REGIONS; r++)
		if (!strcmp(imgdefs[r].name, name))
			return r;
	return -1;
}

/* reads ui/native_<h>.txt into A; false if it's missing or not for this
 * manifest */
static bool read_cache_text(int h)
{
	char rel[64], name[64];
	char *text, *line, *next;
	int version = 0, height = 0;
	unsigned hash = 0;
	size_t tlen;
	bool ok = true;
	cache_name(rel, sizeof(rel), h, ".txt");
	if (!(text = FS_LoadFile(rel, &tlen)))
		return false;
	memset(A.faces, 0, sizeof(A.faces));
	memset(A.regions, 0, sizeof(A.regions));
	memset(A.gidx, 0, sizeof(A.gidx));
	A.nglyphs = A.nkerns = 0;
	for (line = text; ok && line && *line; line = next) {
		if ((next = strchr(line, '\n')))
			*next++ = 0;
		if (!strncmp(line, "bwui-atlas ", 11)) {
			version = atoi(line + 11);
		} else if (!strncmp(line, "height ", 7)) {
			height = atoi(line + 7);
		} else if (!strncmp(line, "manifest ", 9)) {
			hash = (unsigned)strtoul(line + 9, NULL, 16);
		} else if (!strncmp(line, "page ", 5)) {
			int p, w, hh;
			if (sscanf(line + 5, "%d %d %d", &p, &w, &hh) == 3 && p >= 0 && p < UIN_NUM_PAGES) {
				A.pw[p] = w;
				A.ph[p] = hh;
			}
		} else if (!strncmp(line, "face ", 5)) {
			int okf, outline, page, f;
			float px, asc, desc, lh;
			char path[96];
			if (sscanf(line + 5, "%63s %d %g %g %g %g %d %d %95s", name, &okf, &px, &asc, &desc, &lh, &outline, &page,
			           path) == 9 &&
			    (f = face_by_name(name)) >= 0) {
				uin_face_t *fc = &A.faces[f];
				fc->ok = okf != 0;
				fc->px = px;
				fc->ascent = asc;
				fc->descent = desc;
				fc->line = lh;
				fc->outline = (uint8_t)outline;
				fc->page = (uint8_t)page;
				Str_Copy(A.paths[f], strcmp(path, "-") ? path : "", sizeof(A.paths[f]));
			}
		} else if (line[0] == 'g' && line[1] == ' ') {
			int f, cp, x, y, w, hh, xo, yo, adv;
			if (sscanf(line + 2, "%63s %d %d %d %d %d %d %d %d", name, &cp, &x, &y, &w, &hh, &xo, &yo, &adv) == 9 &&
			    (f = face_by_name(name)) >= 0 && cp >= 0 && cp < 256 && A.nglyphs < MAX_GLYPHS) {
				uin_glyph_t *g = &A.glyphs[A.nglyphs];
				g->face = (uint8_t)f;
				g->cp = (uint16_t)cp;
				g->x = (uint16_t)x, g->y = (uint16_t)y;
				g->w = (uint8_t)w, g->h = (uint8_t)hh;
				g->xoff = (int8_t)xo, g->yoff = (int8_t)yo;
				g->adv64 = (uint16_t)adv;
				g->page = A.faces[f].page;
				A.gidx[f][cp] = (uint16_t)++A.nglyphs;
			}
		} else if (line[0] == 'r' && line[1] == ' ') {
			int r, okr, x, y, w, hh;
			if (sscanf(line + 2, "%63s %d %d %d %d %d", name, &okr, &x, &y, &w, &hh) == 6 &&
			    (r = region_by_name(name)) >= 0) {
				uin_region_t *rg = &A.regions[r];
				rg->ok = okr != 0;
				rg->x = (uint16_t)x, rg->y = (uint16_t)y, rg->w = (uint16_t)w, rg->h = (uint16_t)hh;
				rg->page = (uint8_t)imgdefs[r].page;
			}
		} else if (line[0] == 'k' && line[1] == ' ') {
			int f, a, b, adj;
			if (sscanf(line + 2, "%63s %d %d %d", name, &a, &b, &adj) == 4 && (f = face_by_name(name)) >= 0 &&
			    A.nkerns < MAX_KERNS)
				A.kerns[A.nkerns++] = KERN(f, a & 255, b & 255, BW_CLAMP(adj, -128, 127));
		}
	}
	Mem_Free(text);
	if (version != ATLAS_VERSION || height != h || hash != manifest_hash(h, false)) {
		Con_DPrintf("ui: atlas cache ui/native_%d.txt is for another manifest\n", h);
		return false;
	}
	qsort(A.kerns, (size_t)A.nkerns, sizeof(kern_t), cmp_kern);
	return true;
}

/* --- loading ------------------------------------------------------------------------------- */

static void region_uvs(void)
{
	for (int r = 0; r < UIN_NUM_REGIONS; r++) {
		uin_region_t *rg = &A.regions[r];
		int p = rg->page;
		if (!A.pw[p] || !A.ph[p]) {
			rg->ok = false;
			continue;
		}
		rg->s0 = (float)rg->x / A.pw[p];
		rg->t0 = (float)rg->y / A.ph[p];
		rg->s1 = (float)(rg->x + rg->w) / A.pw[p];
		rg->t1 = (float)(rg->y + rg->h) / A.ph[p];
	}
}

static bool upload(uint8_t *const *px)
{
	for (int p = 0; p < UIN_NUM_PAGES; p++) {
		char name[32];
		snprintf(name, sizeof(name), "$uin_page%d", p);
		A.image[p] = R_UploadImage(name, px[p], A.pw[p], A.ph[p]);
		if (!A.image[p])
			return false;
	}
	region_uvs();
	return true;
}

static bool load_cache(int h)
{
	uint8_t *px[UIN_NUM_PAGES] = {0};
	bool ok;
	double t0 = Sys_Seconds();
	if (!read_cache_text(h))
		return false;
	ok = true;
	for (int p = 0; p < UIN_NUM_PAGES && ok; p++) {
		char rel[64];
		int w, hh;
		cache_name(rel, sizeof(rel), h, p ? "_p1.tga" : "_p0.tga");
		px[p] = read_tga(rel, &w, &hh);
		ok = px[p] && w == A.pw[p] && hh == A.ph[p];
		/* one page in memory at a time on the way to the GPU */
		if (ok) {
			char name[32];
			snprintf(name, sizeof(name), "$uin_page%d", p);
			A.image[p] = R_UploadImage(name, px[p], w, hh);
			ok = A.image[p] != 0;
		}
		Mem_Free(px[p]);
		px[p] = NULL;
	}
	if (!ok)
		return false;
	region_uvs();
	Con_DPrintf("ui: atlas cache ui/native_%d loaded in %.1f ms\n", h, (Sys_Seconds() - t0) * 1000);
	return true;
}

#ifndef BW_XBOX
/* the PC checks a cache against the files it was baked from: their sizes */
static bool cache_sources_current(int h)
{
	(void)h;
	for (int f = 0; f < UIN_NUM_FACES; f++) {
		const facedef_t *d = &facedefs[f];
		const char *want = uin_font_path(d->family, d->weight);
		/* a face baked from the fallback while its file now exists, or the
		 * other way round, is stale */
		if (!want)
			want = FS_FileExists(d->path) ? d->path : UIN_FALLBACK_FONT;
		if (strcmp(A.paths[f][0] ? A.paths[f] : UIN_FALLBACK_FONT, want) && FS_FileExists(want))
			return false;
	}
	return true;
}
#endif

/* the atlas for height h: the cache, else a bake (written to the cache on
 * the PC) */
static bool load(int h)
{
	pages_t pg;
	if (load_cache(h)) {
#ifndef BW_XBOX
		if (cache_sources_current(h))
			return true;
		Con_DPrintf("ui: atlas cache ui/native_%d is stale\n", h);
#else
		return true;
#endif
	}
#ifdef BW_XBOX
	Con_Printf("^3ui: no atlas cache, baked at runtime\n");
	if (!bake(h, true, &pg))
		return false;
#else
	if (!bake(h, false, &pg))
		return false;
	write_cache(h, &pg);
#endif
	{
		bool ok = upload(pg.px);
		free_pages(&pg);
		return ok;
	}
}

bool uin_atlas_ready(void)
{
	float x, y, w, hh;
	int h;
	if (!R_Active())
		return false;
	uin_safe_rect(&x, &y, &w, &hh);
	h = BW_MAX(120, (int)(hh + 0.5f));
	if (A.loaded && A.h == h && !A.dirty)
		return true;
	if (A.tried && A.h == h && !A.dirty)
		return false;
	A.dirty = false;
	A.h = h;
	A.loaded = load(h);
	A.tried = !A.loaded;
	return A.loaded;
}

bool uin_atlas_bake_to_cache(int h)
{
	pages_t pg;
	bool ok;
	/* baking replaces the tables: the live atlas reloads afterwards */
	A.loaded = false;
	if (!bake(h, false, &pg))
		return false;
	ok = write_cache(h, &pg);
	free_pages(&pg);
	return ok;
}

void uin_atlas_invalidate(void) { A.dirty = true; }

void uin_atlas_init(void) { memset(&A, 0, sizeof(A)); }

void uin_atlas_shutdown(void) { A.loaded = false; }
