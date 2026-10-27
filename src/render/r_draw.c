/* Bulwark engine - the 2D layer: pictures, fills, text with markup, fonts
 * and the console. Coordinates are virtual pixels from the top left.
 * TrueType/OpenType fonts use stb_truetype (public domain); the built-in
 * fallback font is the public domain font8x8 set. ISC licence, see LICENSE. */
#include "r_local.h"
#include "../client/console.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

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
#include "../thirdparty/font8x8_basic.h"
#ifdef BW_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
static FT_Library ftlib;
#endif

int r_vwidth = 640, r_vheight = 480;
int r_pwidth = 640, r_pheight = 480;
float r_2dscale = 1;

static cvar_t *con_font, *scr_conheight, *scr_conalpha, *con_textsize;

/* --- batching ----------------------------------------------------------- */

#define MAX_2D_VERTS 8192
static rvert_t verts[MAX_2D_VERTS];
static uint16_t indices[MAX_2D_VERTS / 4 * 6];
static int numverts;
static GLuint cur_tex;
static int cur_blend = -1;
static GLenum cur_src, cur_dst;
static float cur_tcmod[4] = {0, 0, 1, 1};
static float cur_alphatest;
static bool in2d;
static float clip[4];
static bool clipping;

/* images handed to other modules by number (R_ImageHandle) */
#define MAX_IMAGE_HANDLES 1024
static image_t *imagehandles[MAX_IMAGE_HANDLES];
static int numimagehandles = 1; /* 0 is none */

void R_Flush2D(void)
{
	if (!numverts || !R_Active() || !r_draw2d->ivalue) {
		numverts = 0;
		return;
	}
	R_UseProgram(PROG_2D);
	GL_BindTexture(0, GL_TEXTURE_2D, cur_tex);
	GL_SetBlend(cur_blend, cur_src, cur_dst);
	qglUniform4f(r_progs[PROG_2D].u_tcmod, cur_tcmod[0], cur_tcmod[1], cur_tcmod[2], cur_tcmod[3]);
	qglUniform1f(r_progs[PROG_2D].u_alphatest, cur_alphatest);
	R_StreamDraw(GL_TRIANGLES, verts, numverts, indices, numverts / 4 * 6);
	numverts = 0;
}

static void set_state(GLuint tex, int blend, GLenum src, GLenum dst, const float *tcmod, float alphatest)
{
	static const float notc[4] = {0, 0, 1, 1};
	if (!tcmod)
		tcmod = notc;
	if (tex != cur_tex || blend != cur_blend || src != cur_src || dst != cur_dst ||
	    memcmp(tcmod, cur_tcmod, sizeof(cur_tcmod)) || alphatest != cur_alphatest || numverts + 4 > MAX_2D_VERTS) {
		R_Flush2D();
		cur_tex = tex;
		cur_blend = blend;
		cur_src = src;
		cur_dst = dst;
		memcpy(cur_tcmod, tcmod, sizeof(cur_tcmod));
		cur_alphatest = alphatest;
	}
}

static void put_vert(float x, float y, float s, float t, const float *c)
{
	rvert_t *v = &verts[numverts++];
	v->xyz[0] = x;
	v->xyz[1] = y;
	v->xyz[2] = 0;
	v->st[0] = s;
	v->st[1] = t;
	for (int i = 0; i < 4; i++) {
		float f = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
		v->color[i] = (uint8_t)(f * 255 + 0.5f);
	}
}

static void quad(float x, float y, float w, float h, float s1, float t1, float s2, float t2, const float *c)
{
	put_vert(x, y, s1, t1, c);
	put_vert(x + w, y, s2, t1, c);
	put_vert(x + w, y + h, s2, t2, c);
	put_vert(x, y + h, s1, t2, c);
}

static int flag_blend(int flags, int deflt)
{
	if ((flags & 3) == 1)
		return BLEND_ADD;
	if ((flags & 3) == 2)
		return BLEND_ALPHA;
	return deflt;
}

void R_DrawPic(float x, float y, float w, float h, material_t *mat, float s1, float t1, float s2, float t2,
               const float *rgb, float alpha, int flags)
{
	if (!in2d || !mat)
		return;
	for (int i = 0; i < mat->numstages; i++) {
		mstage_t *st = &mat->stages[i];
		float c[4], tc[4];
		int blend;
		GLuint tex;
		if (st->lightmap)
			continue;
		tex = st->image ? st->image->tex : r_whiteimage->tex;
		c[0] = rgb[0] * st->color[0];
		c[1] = rgb[1] * st->color[1];
		c[2] = rgb[2] * st->color[2];
		c[3] = alpha * st->color[3];
		/* tcMod scroll and scale */
		tc[0] = fmodf((float)r_time * st->scroll[0], 1.0f);
		tc[1] = fmodf((float)r_time * st->scroll[1], 1.0f);
		tc[2] = st->scale[0];
		tc[3] = st->scale[1];
		blend = i == 0 ? flag_blend(flags, st->blend == BLEND_OPAQUE ? BLEND_ALPHA : st->blend) : st->blend;
		set_state(tex, blend, st->src, st->dst, tc, st->alphatest ? 0.5f : 0.0f);
		quad(x, y, w, h, s1, t1, s2, t2, c);
	}
}

void R_DrawFill(float x, float y, float w, float h, const float *rgb, float alpha, int flags)
{
	float c[4] = {rgb[0], rgb[1], rgb[2], alpha};
	if (!in2d)
		return;
	set_state(r_whiteimage->tex, flag_blend(flags, BLEND_ALPHA), 0, 0, NULL, 0);
	quad(x, y, w, h, 0, 0, 1, 1, c);
}

static void apply_clip(void)
{
	if (!R_Active())
		return;
	if (clipping) {
		float sx = (float)r_pwidth / r_vwidth, sy = (float)r_pheight / r_vheight;
		int x = (int)floorf(clip[0] * sx), y = (int)floorf(clip[1] * sy);
		int w = (int)ceilf(clip[2] * sx), h = (int)ceilf(clip[3] * sy);
		qglEnable(GL_SCISSOR_TEST);
		qglScissor(x, r_pheight - (y + h), w < 0 ? 0 : w, h < 0 ? 0 : h);
	} else {
		qglDisable(GL_SCISSOR_TEST);
	}
}

void R_SetClip(float x, float y, float w, float h)
{
	R_Flush2D();
	clip[0] = x;
	clip[1] = y;
	clip[2] = w;
	clip[3] = h;
	clipping = true;
	apply_clip();
}

void R_ResetClip(void)
{
	R_Flush2D();
	clipping = false;
	apply_clip();
}

void R_Begin2D(int w, int h)
{
	float m[16];
	r_vwidth = w;
	r_vheight = h;
	in2d = true;
	if (!R_Active())
		return;
	qglViewport(0, 0, r_pwidth, r_pheight);
	Mat4_Ortho(m, 0, (float)w, (float)h, 0, -1, 1);
	R_UseProgram(PROG_2D);
	qglUniformMatrix4fv(r_progs[PROG_2D].u_mvp, 1, GL_FALSE, m);
	qglUniform4f(r_progs[PROG_2D].u_color, 1, 1, 1, 1);
	GL_SetDepth(false, false);
	GL_SetCull(false);
	clipping = false;
	apply_clip();
}

void R_End2D(void)
{
	R_Flush2D();
	if (clipping)
		R_ResetClip();
}

/* --- fonts -------------------------------------------------------------- */

/* Text whose font has no TrueType file (font slot 0, a game font that
 * failed to load, the console without con_font) uses the default face,
 * r_font, like Source's VGUI does with Tahoma. It is looked for in the
 * game folders, then the system's font folders, and the bundled DejaVu
 * Sans stands in when it isn't installed, as it does for Source on Linux:
 * Tahoma is Microsoft's and can't be shipped. Rendering follows
 * docs/spec/fonts.md. r_quakefont 1 brings back the 8x8 bitmap font. */

#define MAX_FONTS 32
#define MAX_FONT_SIZES 16

/* the characters baked for each size: Latin-1, Greek, Cyrillic (Source
 * games' chat carries Russian names), punctuation and currency */
static const int font_ranges[][2] = {{32, 224}, {0x370, 0x90}, {0x400, 0x100}, {0x2000, 0x70}, {0x20a0, 0x30}};
#define FONT_GLYPHS (224 + 0x90 + 0x100 + 0x70 + 0x30)

static int glyph_index(int c)
{
	int base = 0;
	for (size_t i = 0; i < BW_ARRAY_COUNT(font_ranges); i++) {
		if (c >= font_ranges[i][0] && c < font_ranges[i][0] + font_ranges[i][1])
			return base + c - font_ranges[i][0];
		base += font_ranges[i][1];
	}
	return -1;
}

typedef struct {
	int pixelsize;
	bool aa; /* antialiased; otherwise one bit per pixel, like VGUI fonts without "antialias" */
	int atlasdim;
	image_t *atlas;
	stbtt_packedchar chars[FONT_GLYPHS];
	float ascent;
} fontsize_t;

typedef struct {
	bool used;
	char name[128];
	char file[256]; /* where the TrueType data came from */
	uint8_t *ttf; /* NULL: the built-in bitmap font, or the default face */
	size_t ttfsize;
	bool embedded; /* ttf is the bundled font, not to be freed */
	bool shadow, outline; /* asked for in loadfont's sizes ("outline=1") */
	bool bold; /* without a file of its own, uses the default face's bold */
	int antialias; /* -1 unset (game files smooth, installed fonts crisp), 0 or 1 from loadfont */
	bool system; /* an installed font, found by its family name */
	stbtt_fontinfo info;
#ifdef BW_FREETYPE
	FT_Face ft;
#endif
	fontsize_t sizes[MAX_FONT_SIZES];
	int numsizes;
} font_t;

extern const unsigned char bw_deffont[], bw_deffont_bold[];
extern const size_t bw_deffont_size, bw_deffont_bold_size;

static cvar_t *r_font, *r_quakefont, *r_font_shadow, *r_font_outline, *r_font_antialias;
static font_t fonts[MAX_FONTS];
static font_t deffonts[2]; /* the default face and its bold */
static image_t *bitmapfont;
static int r_drawfont; /* the font text uses, from the client game's drawfont global */
static bool plaintext; /* no default shadow or outline (the console) */

void R_SetDrawFont(int slot) { r_drawfont = slot >= 0 && slot < MAX_FONTS && fonts[slot].used ? slot : 0; }

static void make_bitmap_font(void)
{
	/* 16x16 grid of 8x8 cells, upscaled 2x with a border for filtering */
	enum { CELL = 16 };
	uint8_t *px = Mem_Alloc(CELL * 16 * CELL * 16 * 4);
	for (int c = 0; c < 128; c++) {
		int cx = (c % 16) * CELL, cy = (c / 16) * CELL;
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++) {
				bool on = (font8x8_basic[c][y] >> x) & 1;
				for (int sy = 0; sy < 2; sy++)
					for (int sx = 0; sx < 2; sx++) {
						uint8_t *p = px + (((cy + y * 2 + sy) * CELL * 16) + cx + x * 2 + sx) * 4;
						p[0] = p[1] = p[2] = 255;
						p[3] = on ? 255 : 0;
					}
			}
	}
	bitmapfont = R_ImageFromPixels("$bitmapfont", px, CELL * 16, CELL * 16, IMG_NOMIP | IMG_CLAMP);
	Mem_Free(px);
}

/* stb_truetype rasterising: no hinting, used when FreeType isn't built in */
static void bake_stb(font_t *f, fontsize_t *fs, uint8_t **alpha, int *dimp)
{
	int dim, asc, desc, gap;
	for (dim = 256; dim <= 4096; dim *= 2) {
		stbtt_pack_context pc;
		stbtt_pack_range ranges[BW_ARRAY_COUNT(font_ranges)];
		int base = 0, ok;
		Mem_Free(*alpha);
		*alpha = Mem_Alloc((size_t)dim * dim);
		memset(fs->chars, 0, sizeof(fs->chars));
		for (size_t i = 0; i < BW_ARRAY_COUNT(font_ranges); i++) {
			memset(&ranges[i], 0, sizeof(ranges[i]));
			ranges[i].font_size = (float)fs->pixelsize;
			ranges[i].first_unicode_codepoint_in_range = font_ranges[i][0];
			ranges[i].num_chars = font_ranges[i][1];
			ranges[i].chardata_for_range = fs->chars + base;
			base += font_ranges[i][1];
		}
		if (!stbtt_PackBegin(&pc, *alpha, dim, dim, 0, 1, NULL))
			continue;
		ok = stbtt_PackFontRanges(&pc, f->ttf, 0, ranges, (int)BW_ARRAY_COUNT(ranges));
		stbtt_PackEnd(&pc);
		if (ok)
			break;
	}
	if (dim > 4096) {
		dim = 4096;
		Con_DPrintf("font %s: size %d did not fit\n", f->name, fs->pixelsize);
	}
	if (!fs->aa)
		for (int i = 0; i < dim * dim; i++)
			(*alpha)[i] = (*alpha)[i] >= 128 ? 255 : 0;
	stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
	fs->ascent = asc * stbtt_ScaleForPixelHeight(&f->info, (float)fs->pixelsize);
	*dimp = dim;
}

#ifdef BW_FREETYPE
/* FreeType rasterising with the font's hinting, which is what keeps small
 * Tahoma and Verdana crisp. The size is the cell height, ascent plus
 * descent, as Windows reads a font height (usWinAscent + usWinDescent). */
static bool bake_freetype(font_t *f, fontsize_t *fs, uint8_t **alpha, int *dimp)
{
	TT_OS2 *os2;
	FT_Size_RequestRec req;
	int dim, cell, ascent;
	if (!ftlib && FT_Init_FreeType(&ftlib))
		return false;
	if (!f->ft && FT_New_Memory_Face(ftlib, f->ttf, (FT_Long)f->ttfsize, 0, &f->ft)) {
		f->ft = NULL;
		return false;
	}
	os2 = (TT_OS2 *)FT_Get_Sfnt_Table(f->ft, FT_SFNT_OS2);
	if (os2 && os2->usWinAscent + os2->usWinDescent > 0)
		cell = os2->usWinAscent + os2->usWinDescent, ascent = os2->usWinAscent;
	else
		cell = f->ft->ascender - f->ft->descender, ascent = f->ft->ascender;
	if (cell <= 0)
		return false;
	memset(&req, 0, sizeof(req));
	req.type = FT_SIZE_REQUEST_TYPE_NOMINAL;
	req.height = (FT_Long)((double)fs->pixelsize * f->ft->units_per_EM / cell * 64 + 0.5);
	if (FT_Request_Size(f->ft, &req))
		return false;
	fs->ascent = (float)(int)((double)ascent * fs->pixelsize / cell + 0.5);
	for (dim = 256; dim <= 4096; dim *= 2) {
		int x = 1, y = 1, rowh = 0, base = 0;
		bool full = false;
		Mem_Free(*alpha);
		*alpha = Mem_Alloc((size_t)dim * dim);
		memset(fs->chars, 0, sizeof(fs->chars));
		for (size_t r = 0; r < BW_ARRAY_COUNT(font_ranges) && !full; r++) {
			for (int i = 0; i < font_ranges[r][1] && !full; i++) {
				stbtt_packedchar *pc = &fs->chars[base + i];
				FT_GlyphSlot g;
				FT_Bitmap *bm;
				FT_UInt idx = FT_Get_Char_Index(f->ft, (FT_ULong)(font_ranges[r][0] + i));
				if (!idx && font_ranges[r][0] + i > 255)
					continue; /* not in the font */
				if (FT_Load_Glyph(f->ft, idx, FT_LOAD_RENDER | (fs->aa ? FT_LOAD_TARGET_NORMAL : FT_LOAD_TARGET_MONO)))
					continue;
				g = f->ft->glyph;
				bm = &g->bitmap;
				if (x + (int)bm->width + 1 > dim)
					x = 1, y += rowh + 1, rowh = 0;
				if (y + (int)bm->rows + 1 > dim) {
					full = true;
					break;
				}
				for (unsigned row = 0; row < bm->rows; row++) {
					const uint8_t *src = bm->buffer + (int)row * bm->pitch;
					uint8_t *dst = *alpha + (size_t)(y + row) * dim + x;
					for (unsigned col = 0; col < bm->width; col++)
						dst[col] = bm->pixel_mode == FT_PIXEL_MODE_MONO ? ((src[col >> 3] >> (7 - (col & 7))) & 1) * 255
						                                                : src[col];
				}
				pc->x0 = (unsigned short)x, pc->y0 = (unsigned short)y;
				pc->x1 = (unsigned short)(x + bm->width), pc->y1 = (unsigned short)(y + bm->rows);
				pc->xoff = (float)g->bitmap_left, pc->yoff = (float)-g->bitmap_top;
				pc->xadvance = (float)((g->advance.x + 32) >> 6);
				x += bm->width + 1;
				rowh = BW_MAX(rowh, (int)bm->rows);
			}
			base += font_ranges[r][1];
		}
		if (!full)
			break;
	}
	*dimp = BW_MIN(dim, 4096);
	return true;
}
#endif

static fontsize_t *font_size(font_t *f, float height, bool aa)
{
	int want = (int)(height * r_2dscale + 0.5f), best = -1, bestdiff = 1 << 30;
	if (!f->ttf)
		return NULL;
	if (want < 4)
		want = 4;
	if (want > 128)
		want = 128;
	for (int i = 0; i < f->numsizes; i++) {
		int d = abs(f->sizes[i].pixelsize - want);
		if (f->sizes[i].aa == aa && d < bestdiff)
			bestdiff = d, best = i;
	}
	/* bake each size exactly, so glyphs land one texel per screen pixel */
	if (bestdiff && f->numsizes < MAX_FONT_SIZES) {
		fontsize_t *fs = &f->sizes[f->numsizes];
		uint8_t *alpha = NULL, *rgba;
		char name[300];
		int dim = 0;
		bool done = false;
		fs->pixelsize = want;
		fs->aa = aa;
#ifdef BW_FREETYPE
		done = bake_freetype(f, fs, &alpha, &dim);
#endif
		if (!done)
			bake_stb(f, fs, &alpha, &dim);
		fs->atlasdim = dim;
		rgba = Mem_Alloc((size_t)dim * dim * 4);
		for (int i = 0; i < dim * dim; i++) {
			rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
			rgba[i * 4 + 3] = alpha[i];
		}
		snprintf(name, sizeof(name), "$font:%s:%d%s", f->file[0] ? f->file : f->name, want, aa ? "" : ":mono");
		/* point sampled: each texel lands on one screen pixel */
		fs->atlas = R_ImageFromPixels(name, rgba, dim, dim, IMG_NOMIP | IMG_CLAMP | IMG_NEAREST);
		Mem_Free(alpha);
		Mem_Free(rgba);
		return &f->sizes[f->numsizes++];
	}
	return best >= 0 ? &f->sizes[best] : NULL;
}

static bool take_ttf(font_t *f, uint8_t *data, size_t size, const char *from)
{
	if (!stbtt_InitFont(&f->info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
		Mem_Free(data);
		return false;
	}
	f->ttf = data;
	f->ttfsize = size;
	Str_Copy(f->file, from, sizeof(f->file));
	return true;
}

static uint8_t *read_os_file(const char *path, size_t *size)
{
	FILE *fp = fopen(path, "rb");
	uint8_t *data;
	long len;
	if (!fp)
		return NULL;
	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (len <= 0 || len > 64 << 20) {
		fclose(fp);
		return NULL;
	}
	data = Mem_Alloc((size_t)len);
	*size = (size_t)len;
	if (fread(data, 1, (size_t)len, fp) != (size_t)len) {
		Mem_Free(data);
		data = NULL;
	}
	fclose(fp);
	return data;
}

/* the file names a font goes by: VGUI schemes and QC name fonts by family
 * ("Tahoma", "Lucida Console"), Windows by short file name */
static int font_filenames(const char *name, char out[][64], int max)
{
	static const char *const aliases[][2] = {
		{"tahoma", "tahoma.ttf"},         {"tahoma bold", "tahomabd.ttf"}, {"verdana", "verdana.ttf"},
		{"verdana bold", "verdanab.ttf"}, {"lucida console", "lucon.ttf"}, {"courier new", "cour.ttf"},
		{"cour", "cour.ttf"},             {"arial", "arial.ttf"},          {"trebuchet ms", "trebuc.ttf"},
		{"marlett", "marlett.ttf"},
	};
	char lower[48], packed[48];
	int n = 0, k = 0;
	size_t i;
	for (i = 0; name[i] && i < sizeof(lower) - 1; i++)
		lower[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
	lower[i] = 0;
	for (i = 0; lower[i]; i++)
		if (lower[i] != ' ')
			packed[k++] = lower[i];
	packed[k] = 0;
	for (i = 0; i < BW_ARRAY_COUNT(aliases) && n < max; i++)
		if (!strcmp(lower, aliases[i][0]))
			Str_Copy(out[n++], aliases[i][1], 64);
	if (strchr(lower, '.')) {
		if (n < max)
			Str_Copy(out[n++], lower, 64);
		return n;
	}
	if (n + 6 <= max) {
		snprintf(out[n++], 64, "%s.ttf", lower);
		snprintf(out[n++], 64, "%s.otf", lower);
		snprintf(out[n++], 64, "%s.ttf", packed);
		snprintf(out[n++], 64, "%s.otf", packed);
		snprintf(out[n++], 64, "%s-regular.ttf", packed);
		snprintf(out[n++], 64, "%s-regular.otf", packed);
	}
	return n;
}

#ifndef _WIN32
/* looks for one of names (any case) in dir and its subfolders */
static bool find_in_dir(const char *dir, char names[][64], int numnames, int depth, char *out, size_t outsize)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	bool found = false;
	if (!d)
		return false;
	while (!found && (de = readdir(d))) {
		char path[1024];
		struct stat st;
		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (stat(path, &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth > 0)
				found = find_in_dir(path, names, numnames, depth - 1, out, outsize);
			continue;
		}
		for (int i = 0; i < numnames; i++)
			if (!Str_ICmp(de->d_name, names[i])) {
				Str_Copy(out, path, outsize);
				found = true;
				break;
			}
	}
	closedir(d);
	return found;
}
#endif

/* an installed copy of a font, by family or file name */
static bool load_system_font(font_t *f, const char *name)
{
	char names[16][64], path[1024];
	int n = font_filenames(name, names, 16);
	size_t size;
	uint8_t *data;
#ifdef _WIN32
	const char *windir = getenv("WINDIR");
	for (int i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "%s/Fonts/%s", windir ? windir : "C:/Windows", names[i]);
		if ((data = read_os_file(path, &size)) && take_ttf(f, data, size, path))
			return f->system = true;
	}
#else
	const char *home = getenv("HOME");
	const char *dirs[] = {
#ifdef __APPLE__
		"~/Library/Fonts", "/Library/Fonts", "/System/Library/Fonts/Supplemental", "/System/Library/Fonts",
#else
		"~/.local/share/fonts", "~/.fonts", "/usr/local/share/fonts", "/usr/share/fonts",
		"~/.wine/drive_c/windows/Fonts",
#endif
	};
	for (size_t i = 0; i < BW_ARRAY_COUNT(dirs); i++) {
		char dir[512];
		if (dirs[i][0] == '~') {
			if (!home)
				continue;
			snprintf(dir, sizeof(dir), "%s%s", home, dirs[i] + 1);
		} else {
			Str_Copy(dir, dirs[i], sizeof(dir));
		}
		if (find_in_dir(dir, names, n, 4, path, sizeof(path)) && (data = read_os_file(path, &size)) &&
		    take_ttf(f, data, size, path))
			return f->system = true;
	}
#endif
	return false;
}

static bool load_ttf(font_t *f, const char *path)
{
	static const char *const forms[] = {"%s", "%s.ttf", "%s.otf", "fonts/%s.ttf", "fonts/%s.otf"};
	if (!path[0])
		return false;
	for (size_t i = 0; i < BW_ARRAY_COUNT(forms); i++) {
		char name[160];
		uint8_t *data;
		size_t len;
		snprintf(name, sizeof(name), forms[i], path);
		if ((data = (uint8_t *)FS_LoadFile(name, &len)) && take_ttf(f, data, len, name))
			return true;
	}
	/* a bare name may be an installed font; a path may be a file outside
	 * the game (r_font /usr/share/fonts/...) */
	if (path[0] == '/' || (path[0] && path[1] == ':')) {
		size_t size;
		uint8_t *data = read_os_file(path, &size);
		return data && take_ttf(f, data, size, path);
	}
	return !strchr(path, '/') && load_system_font(f, path);
}

static void free_font(font_t *f)
{
#ifdef BW_FREETYPE
	if (f->ft)
		FT_Done_Face(f->ft);
#endif
	if (!f->embedded)
		Mem_Free(f->ttf);
	memset(f, 0, sizeof(*f));
}

/* loads the default face, or its bold, when r_font changes */
static font_t *default_face(bool bold)
{
	static const struct {
		const unsigned char *data;
		const size_t *size;
		const char *name;
	} bundled[2] = {{bw_deffont, &bw_deffont_size, "DejaVu Sans"},
	                {bw_deffont_bold, &bw_deffont_bold_size, "DejaVu Sans Bold"}};
	font_t *f = &deffonts[bold];
	char want[140];
	if (f->used && !strcmp(f->name, r_font->string))
		return f;
	free_font(f);
	f->used = true;
	Str_Copy(f->name, r_font->string, sizeof(f->name));
	/* "Tahoma" becomes "Tahoma Bold", which finds tahomabd.ttf */
	snprintf(want, sizeof(want), bold ? "%s Bold" : "%s", r_font->string);
	if (r_font->string[0] && load_ttf(f, want)) {
		Con_DPrintf("default font %s from %s\n", want, f->file);
	} else {
		if (r_font->string[0])
			Con_DPrintf("default font %s isn't installed, using %s\n", want, bundled[bold].name);
		stbtt_InitFont(&f->info, bundled[bold].data, stbtt_GetFontOffsetForIndex(bundled[bold].data, 0));
		f->ttf = (uint8_t *)bundled[bold].data;
		f->ttfsize = *bundled[bold].size;
		f->embedded = true;
		snprintf(f->file, sizeof(f->file), "(bundled) %s", bundled[bold].name);
	}
	return f;
}

/* the font that draws slot's text */
static font_t *face_for(int slot)
{
	font_t *f = &fonts[slot];
	if (f->ttf || !r_quakefont || r_quakefont->value)
		return f;
	return default_face(f->bold);
}

const char *R_DefaultFontFile(void)
{
	return r_quakefont && r_quakefont->value ? NULL : default_face(false)->file;
}

/* the effect flags in loadfont's sizes string ("12 16 outline=1") */
static void font_flags(const char *sizes, bool *shadow, bool *outline, int *antialias)
{
	*shadow = *outline = false;
	*antialias = -1;
	for (const char *s = sizes ? sizes : ""; *s;) {
		size_t len = strcspn(s, " \t,");
		if (len > 8 && !strncmp(s, "outline=", 8))
			*outline = atoi(s + 8) != 0;
		else if (len > 7 && !strncmp(s, "shadow=", 7))
			*shadow = atoi(s + 7) != 0;
		else if (len > 11 && !strncmp(s, "dropshadow=", 11))
			*shadow = atoi(s + 11) != 0;
		else if (len > 10 && !strncmp(s, "antialias=", 10))
			*antialias = atoi(s + 10) != 0;
		s += len;
		s += strspn(s, " \t,");
	}
}

int R_LoadFont(const char *name, const char *path, const char *sizes, int slot)
{
	font_t *f;
	bool shadow, outline;
	int antialias;
	font_flags(sizes, &shadow, &outline, &antialias);
	if (slot < 0) {
		/* the same file with the same effects shares a slot; sizes are
		 * baked on demand */
		for (slot = 1; slot < MAX_FONTS; slot++)
			if (fonts[slot].used && !Str_ICmp(fonts[slot].name, path) && path[0] && fonts[slot].shadow == shadow &&
			    fonts[slot].outline == outline && fonts[slot].antialias == antialias)
				return slot;
		for (slot = 1; slot < MAX_FONTS && fonts[slot].used; slot++)
			;
	}
	if (slot <= 0 || slot >= MAX_FONTS)
		return 0;
	f = &fonts[slot];
	free_font(f);
	f->used = true;
	f->shadow = shadow;
	f->outline = outline;
	f->antialias = antialias;
	Str_Copy(f->name, path, sizeof(f->name));
	if (!load_ttf(f, path)) {
		/* a missing "Verdana Bold" or "fonts/x-bold.ttf" falls back to the
		 * default face's bold */
		for (const char *p = path; *p && !f->bold; p++)
			f->bold = !Str_NICmp(p, "bold", 4);
		Con_DPrintf("font %s: no TrueType file, using %s\n", path, f->ttf ? f->file : "the default font");
	}
	(void)name;
	return slot;
}

int R_FindFont(const char *name)
{
	for (int i = 1; i < MAX_FONTS; i++)
		if (fonts[i].used && !Str_ICmp(fonts[i].name, name))
			return i;
	return 0;
}

/* --- text ------------------------------------------------------------------ */

static const float qcolors[10][3] = {{0, 0, 0}, {1, 0.2f, 0.2f}, {0.2f, 1, 0.2f}, {1, 1, 0.2f}, {0.3f, 0.3f, 1},
                                     {0.2f, 1, 1}, {1, 0.2f, 1}, {1, 1, 1}, {0.5f, 0.5f, 0.5f}, {0.75f, 0.75f, 0.75f}};

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* decodes one UTF-8 character, advancing *s */
static int utf8_next(const char **s)
{
	const unsigned char *u = (const unsigned char *)*s;
	int c = u[0], n = 0;
	if (c < 0x80)
		n = 0;
	else if ((c & 0xe0) == 0xc0)
		c &= 0x1f, n = 1;
	else if ((c & 0xf0) == 0xe0)
		c &= 0x0f, n = 2;
	else if ((c & 0xf8) == 0xf0)
		c &= 0x07, n = 3;
	else {
		(*s)++;
		return c;
	}
	for (int i = 1; i <= n; i++) {
		if ((u[i] & 0xc0) != 0x80) {
			*s += i;
			return '?';
		}
		c = (c << 6) | (u[i] & 0x3f);
	}
	*s += n + 1;
	return c;
}

/* Handles a ^ markup sequence at *p: colours (^0-^9, ^xRGB), ^d (default
 * colour), other ^letters (styles, ignored) and ^[ ... ^] links, whose text
 * is skipped. Returns false if *p is not markup. */
static bool markup(const char **p, float *color, const float *deflt)
{
	const char *s = *p;
	if (s[0] != '^' || !s[1])
		return false;
	if (s[1] >= '0' && s[1] <= '9') {
		memcpy(color, qcolors[s[1] - '0'], sizeof(float) * 3);
		*p = s + 2;
		return true;
	}
	if (s[1] == 'x' && hexval(s[2]) >= 0 && hexval(s[3]) >= 0 && hexval(s[4]) >= 0) {
		color[0] = hexval(s[2]) / 15.0f;
		color[1] = hexval(s[3]) / 15.0f;
		color[2] = hexval(s[4]) / 15.0f;
		*p = s + 5;
		return true;
	}
	if (s[1] == '[') {
		const char *end = strstr(s + 2, "^]");
		*p = end ? end + 2 : s + strlen(s);
		return true;
	}
	if (s[1] == 'd') {
		memcpy(color, deflt, sizeof(float) * 3);
		*p = s + 2;
		return true;
	}
	if ((s[1] >= 'a' && s[1] <= 'z') || (s[1] >= 'A' && s[1] <= 'Z')) {
		*p = s + 2;
		return true;
	}
	return false;
}

static float char_advance(font_t *f, fontsize_t *fs, int c, float sx, float sy)
{
	int g = fs ? glyph_index(c) : -1;
	if (g >= 0)
		return fs->chars[g].xadvance * (sy / fs->pixelsize) * (sx / sy);
	(void)f;
	return sx;
}

/* Effects in black under the text, like Source's dropshadow and outline
 * font flags: a shadow one screen pixel down and right, an outline one
 * pixel all round (docs/spec/fonts.md). */
static const float effect_offsets[9][2] = {{1, 1}, {-1, -1}, {0, -1}, {1, -1}, {-1, 0},
                                           {1, 0}, {-1, 1},  {0, 1},  {1, 1}};

static float draw_text(float x, float y, const char *text, float sx, float sy, const float *rgb, float alpha,
                       int flags, bool usemarkup, bool measure)
{
	font_t *slot = &fonts[r_drawfont], *f = face_for(r_drawfont);
	/* Fonts from game files are smoothed; the default face and installed
	 * fonts are drawn crisp, as VGUI draws scheme fonts without "antialias" */
	bool aa = slot->antialias >= 0 ? slot->antialias != 0 : f != slot || f->system ? r_font_antialias->value != 0 : true;
	fontsize_t *fs = sy > 0 ? font_size(f, sy, aa) : NULL;
	float color[4], black[4] = {0, 0, 0, alpha};
	float startx = x, starty = y, maxx = x, px = 1 / r_2dscale, k = 1, kx = 1, ascent = 0;
	bool shadow = slot->shadow, outline = slot->outline, snap = false;
	int first = 0, numoffsets = 0;
	GLuint tex = fs ? fs->atlas->tex : bitmapfont ? bitmapfont->tex : 0;
	if (sx <= 0)
		sx = sy;
	if (fs) {
		k = sy / fs->pixelsize; /* atlas pixels to virtual ones */
		kx = k * (sx / sy);
		ascent = fs->ascent * k;
		/* glyphs baked at the size they're drawn are put on whole screen
		 * pixels, which keeps small text sharp */
		snap = sx == sy && fabsf(k * r_2dscale - 1) < 0.01f;
	}
	if (f != slot && !plaintext) {
		shadow |= r_font_shadow->value != 0;
		outline |= r_font_outline->value != 0;
	}
	if (measure || !in2d)
		shadow = outline = false;
	if (outline)
		first = 1, numoffsets = 8;
	else if (shadow)
		first = 0, numoffsets = 1;
	if (!measure && in2d)
		set_state(tex, flag_blend(flags, BLEND_ALPHA), 0, 0, NULL, 0);
	/* pass 0 draws the effect under all of the text, pass 1 the text */
	for (int pass = numoffsets ? 0 : 1; pass < 2; pass++) {
		const char *p = text;
		x = startx;
		y = starty;
		color[0] = rgb[0], color[1] = rgb[1], color[2] = rgb[2], color[3] = alpha;
		while (*p) {
			int c;
			if (usemarkup && p[0] == '^' && p[1] == '^') {
				p++;
			} else if (usemarkup && markup(&p, color, rgb)) {
				continue;
			}
			if (*p == '\n') {
				p++;
				maxx = BW_MAX(maxx, x);
				x = startx;
				y += sy;
				continue;
			}
			c = utf8_next(&p);
			if (!measure && in2d) {
				float gx, gy, gw, gh, s1, t1, s2, t2;
				bool draw = false;
				int g = fs ? glyph_index(c) : -1;
				if (g >= 0) {
					stbtt_packedchar *b = &fs->chars[g];
					float inv = 1.0f / fs->atlasdim, ox = x, base = y + ascent;
					if (snap) {
						ox = roundf(ox * r_2dscale) / r_2dscale;
						base = roundf(base * r_2dscale) / r_2dscale;
					}
					gx = ox + b->xoff * kx, gy = base + b->yoff * k;
					gw = (b->x1 - b->x0) * kx, gh = (b->y1 - b->y0) * k;
					s1 = b->x0 * inv, t1 = b->y0 * inv, s2 = b->x1 * inv, t2 = b->y1 * inv;
					draw = b->x1 > b->x0;
				} else if (!fs && c > 32 && c < 128) {
					gx = x, gy = y, gw = sx, gh = sy;
					s1 = (c % 16) / 16.0f, t1 = (c / 16) / 16.0f, s2 = s1 + 1 / 16.0f, t2 = t1 + 1 / 16.0f;
					draw = true;
				}
				if (draw)
					set_state(tex, cur_blend, cur_src, cur_dst, NULL, 0);
				if (draw && pass == 0) {
					black[3] = color[3];
					for (int o = first; o < first + numoffsets; o++) {
						set_state(tex, cur_blend, cur_src, cur_dst, NULL, 0);
						quad(gx + effect_offsets[o][0] * px, gy + effect_offsets[o][1] * px, gw, gh, s1, t1, s2, t2,
						     black);
					}
					set_state(tex, cur_blend, cur_src, cur_dst, NULL, 0);
					if (outline && shadow)
						quad(gx + 2 * px, gy + 2 * px, gw, gh, s1, t1, s2, t2, black);
				} else if (draw) {
					quad(gx, gy, gw, gh, s1, t1, s2, t2, color);
				}
			}
			x += char_advance(f, fs, c, sx, sy);
		}
	}
	return BW_MAX(maxx, x) - startx;
}

float R_DrawString(float x, float y, const char *text, float sx, float sy, const float *rgb, float alpha, int flags,
                   bool usemarkup)
{
	return draw_text(x, y, text, sx, sy, rgb, alpha, flags, usemarkup, false);
}

float R_StringWidth(const char *text, float sx, float sy, bool usemarkup)
{
	static const float white[3] = {1, 1, 1};
	return draw_text(0, 0, text, sx, sy, white, 1, 0, usemarkup, true);
}

/* --- console ------------------------------------------------------------- */

/* what Tab would complete, in a box hanging under the console */
static void draw_suggestions(int w, int h, float top, float size)
{
	static const float white[3] = {1, 1, 1}, grey[3] = {0.6f, 0.6f, 0.6f}, pick[3] = {1, 0.85f, 0.3f},
	                   back[3] = {0.05f, 0.05f, 0.08f};
	int n = Con_Suggestions(), shown, first = 0, rows;
	float namew = 0, y;
	char info[160];
	bool sel;
	cvar_t *on = Cvar_Find("con_suggestions");
	if (!n || (on && !on->value))
		return;
	if (n < 0) {
		/* only a hint: the command being typed and what it does */
		R_DrawFill(0, top, (float)w, size + 4, back, 0.9f, 0);
		R_DrawString(4, top + 2, Con_Suggestion(0, info, sizeof(info), NULL), size, size, grey, 1, 0, false);
		return;
	}
	rows = BW_MAX(1, BW_MIN(12, (int)((h - top) / size) - 2));
	shown = BW_MIN(n, rows);
	/* keep the one Tab picked in view */
	for (int i = 0; i < n; i++) {
		Con_Suggestion(i, info, sizeof(info), &sel);
		if (sel && i >= shown)
			first = i - shown + 1;
	}
	for (int i = first; i < first + shown; i++)
		namew = BW_MAX(namew, R_StringWidth(Con_Suggestion(i, info, sizeof(info), NULL), size, size, false));
	namew = BW_MIN(namew, w * 0.45f);
	R_DrawFill(0, top, (float)w, size * (shown + (n > shown)) + 4, back, 0.9f, 0);
	y = top + 2;
	for (int i = first; i < first + shown; i++, y += size) {
		const char *name = Con_Suggestion(i, info, sizeof(info), &sel);
		R_DrawString(4, y, name, size, size, sel ? pick : white, 1, 0, false);
		if (info[0])
			R_DrawString(4 + namew + size * 2, y, info, size, size, grey, 1, 0, false);
	}
	if (n > shown) {
		char more[64];
		snprintf(more, sizeof(more), "... %d more", n - shown);
		R_DrawString(4, y, more, size, size, grey, 1, 0, false);
	}
}

void R_DrawConsole(int w, int h)
{
	static const float white[3] = {1, 1, 1}, dim[3] = {0, 0, 0};
	/* Source's console text colour, off-white, with a TrueType face */
	static const float source_text[3] = {216 / 255.0f, 222 / 255.0f, 211 / 255.0f};
	const float *text;
	float size, height, y;
	int cursor, saved = r_drawfont;
	const char *input;
	if (!Con_IsActive())
		return;
	r_drawfont = con_font->string[0] ? R_LoadFont("", con_font->string, "", -1) : 0;
	/* 0 picks a size for the font: 8 for the Quake font's 8x8 cells, 12 for
	 * a TrueType face, whose letters sit smaller in their line. Configs
	 * saved before the default face came in hold the old default, 8, which
	 * is treated the same way. */
	size = con_textsize->value;
	if (size <= 0 || size == 8)
		size = face_for(r_drawfont)->ttf ? 12 : 8;
	text = face_for(r_drawfont)->ttf ? source_text : white;
	plaintext = true;
	height = floorf(h * BW_MAX(0.1f, BW_MIN(1.0f, scr_conheight->value)));
	R_DrawFill(0, 0, (float)w, height, dim, scr_conalpha->value, 0);
	{
		material_t *back = R_FindMaterial("gfx/conback", MATF_2D);
		if (back->fromfile || !back->diffuse->missing)
			R_DrawPic(0, 0, (float)w, height, back, 0, 0, 1, 1, white, scr_conalpha->value, 0);
	}
	{
		static const float bar[3] = {0.6f, 0.6f, 0.6f};
		R_DrawFill(0, height - 2, (float)w, 2, bar, 1, 0);
	}
	/* the input line, then scrollback upwards from it */
	y = height - size * 2 - 4;
	input = Con_InputLine(&cursor);
	{
		char line[300];
		snprintf(line, sizeof(line), "]%s", input);
		R_DrawString(4, y, line, size, size, text, 1, 0, false);
		if (((int)(r_time * 3)) & 1) {
			char pre[300];
			float cx;
			snprintf(pre, sizeof(pre), "]%.*s", cursor, input);
			cx = 4 + R_StringWidth(pre, size, size, false);
			R_DrawFill(cx, y + size - 2, size * 0.6f, 2, white, 1, 0);
		}
	}
	y -= size;
	for (int back = Con_Scroll(); y > -size; back++, y -= size) {
		const char *l = Con_Line(back);
		if (!l)
			break;
		if (back == 0 && !*l)
			continue; /* the line being written is empty */
		R_DrawString(4, y, l, size, size, text, 1, 0, true);
	}
	draw_suggestions(w, h, height, size);
	r_drawfont = saved;
	plaintext = false;
}

void R_InitDraw(void)
{
	con_font = Cvar_Get("con_font", "", CVAR_ARCHIVE, "TrueType or OpenType font for the console, empty for the built-in one");
	scr_conheight = Cvar_Get("scr_conheight", "0.5", CVAR_ARCHIVE, "Console height, as a fraction of the screen");
	scr_conalpha = Cvar_Get("scr_conalpha", "0.85", CVAR_ARCHIVE, "Console background opacity");
	con_textsize = Cvar_Get("con_textsize", "0", CVAR_ARCHIVE,
	                        "Console text height in virtual pixels; 0 (or 8) picks 8 for the Quake font and 12 for TrueType");
	r_font = Cvar_Get("r_font", "Tahoma", CVAR_ARCHIVE,
	                  "Default font for text without a font of its own: a font file or an installed font's name. "
	                  "DejaVu Sans stands in when it isn't found");
	r_quakefont = Cvar_Get("r_quakefont", "0", CVAR_ARCHIVE,
	                       "1 draws text without a font of its own in the Quake-style 8x8 bitmap font");
	r_font_shadow = Cvar_Get("r_font_shadow", "1", CVAR_ARCHIVE,
	                         "Drop shadow under text in the default font (not the console)");
	r_font_antialias = Cvar_Get("r_font_antialias", "0", CVAR_ARCHIVE,
	                            "Smooth the default font and installed fonts (Source draws its fonts crisp unless a "
	                            "scheme asks for antialias)");
	r_font_outline = Cvar_Get("r_font_outline", "0", CVAR_ARCHIVE,
	                          "Black outline round text in the default font (not the console)");
	fonts[0].used = true;
	fonts[0].antialias = -1;
	Str_Copy(fonts[0].name, "$builtin", sizeof(fonts[0].name));
	for (int i = 0; i < (int)BW_ARRAY_COUNT(indices) / 6; i++) {
		indices[i * 6 + 0] = (uint16_t)(i * 4 + 0);
		indices[i * 6 + 1] = (uint16_t)(i * 4 + 1);
		indices[i * 6 + 2] = (uint16_t)(i * 4 + 2);
		indices[i * 6 + 3] = (uint16_t)(i * 4 + 0);
		indices[i * 6 + 4] = (uint16_t)(i * 4 + 2);
		indices[i * 6 + 5] = (uint16_t)(i * 4 + 3);
	}
	make_bitmap_font();
}

void R_ShutdownDraw(void)
{
	for (int i = 0; i < MAX_FONTS; i++)
		free_font(&fonts[i]);
	free_font(&deffonts[0]);
	free_font(&deffonts[1]);
	bitmapfont = NULL;
	memset(imagehandles, 0, sizeof(imagehandles));
	numimagehandles = 1;
	cur_tex = 0;
	cur_blend = -1;
}

/* --- 2D for other modules ------------------------------------------------- */

static int handle_for(image_t *img)
{
	for (int i = 1; i < numimagehandles; i++)
		if (imagehandles[i] == img)
			return i;
	if (numimagehandles >= MAX_IMAGE_HANDLES)
		return 0;
	imagehandles[numimagehandles] = img;
	return numimagehandles++;
}

int R_ImageHandle(const char *name)
{
	/* pictures for 2D drawing: clamped, so a stretched edge doesn't wrap */
	image_t *img = R_FindImage(name, IMG_NOMIP | IMG_CLAMP);
	return img->missing ? 0 : handle_for(img);
}

int R_UploadImage(const char *name, const uint8_t *rgba, int w, int h)
{
	return handle_for(R_ImageFromPixels(name, rgba, w, h, IMG_NOMIP | IMG_CLAMP));
}

void R_UpdateImage(int handle, int x, int y, int w, int h, const uint8_t *rgba)
{
	image_t *img = handle > 0 && handle < numimagehandles ? imagehandles[handle] : NULL;
	if (!img || !R_Active() || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > img->width || y + h > img->height)
		return;
	R_Flush2D(); /* the batch may still use the old pixels */
	GL_BindTexture(0, GL_TEXTURE_2D, img->tex);
	qglPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	qglTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

float R_Get2DScale(void) { return r_2dscale; }

void R_ImageSize(int handle, int *w, int *h)
{
	image_t *img = handle > 0 && handle < numimagehandles ? imagehandles[handle] : NULL;
	*w = img ? img->width : 0;
	*h = img ? img->height : 0;
}

void R_Draw2DTriangles(int image, const r2dvert_t *in, int numverts, const uint16_t *idx, int numidx, int blend)
{
	image_t *img = image > 0 && image < numimagehandles ? imagehandles[image] : r_whiteimage;
	rvert_t *v;
	if (!in2d || !R_Active() || numverts <= 0 || numidx <= 0 || !r_draw2d->ivalue)
		return;
	R_Flush2D();
	v = Mem_Alloc(sizeof(*v) * (size_t)numverts);
	for (int i = 0; i < numverts; i++) {
		v[i].xyz[0] = in[i].x;
		v[i].xyz[1] = in[i].y;
		v[i].st[0] = in[i].s;
		v[i].st[1] = in[i].t;
		memcpy(v[i].color, in[i].rgba, 4);
	}
	R_UseProgram(PROG_2D);
	GL_BindTexture(0, GL_TEXTURE_2D, img->tex);
	if (blend == R2D_ADD)
		GL_SetBlend(BLEND_ADD, 0, 0);
	else if (blend == R2D_PREMULTIPLIED)
		GL_SetBlend(BLEND_CUSTOM, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	else if (blend == R2D_MULTIPLY) /* premultiplied source times destination */
		GL_SetBlend(BLEND_CUSTOM, GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
	else
		GL_SetBlend(BLEND_ALPHA, 0, 0);
	qglUniform4f(r_progs[PROG_2D].u_tcmod, 0, 0, 1, 1);
	qglUniform1f(r_progs[PROG_2D].u_alphatest, 0);
	R_StreamDraw(GL_TRIANGLES, v, numverts, idx, numidx);
	Mem_Free(v);
	cur_blend = -1; /* the batch must set its state again */
}

void R_Set2DClip(float x, float y, float w, float h) { R_SetClip(x, y, w, h); }
void R_Reset2DClip(void) { R_ResetClip(); }

/* a loaded font: false for a free slot */
bool R_FontInfo(int slot, const char **name, bool *truetype, int *sizes, int *numsizes)
{
	const font_t *f;
	if (slot < 0 || slot >= MAX_FONTS || !fonts[slot].used)
		return false;
	f = &fonts[slot];
	*name = f->name;
	*truetype = f->ttf != NULL;
	*numsizes = f->numsizes;
	for (int i = 0; i < f->numsizes; i++)
		sizes[i] = f->sizes[i].pixelsize;
	return true;
}

/* --- the plain interface (render.h) ------------------------------------------------- */

float R_Text(float x, float y, const char *text, float size, const float *rgb, float alpha)
{
	return R_DrawString(x, y, text, size, size, rgb, alpha, 0, true);
}

float R_TextWidth(const char *text, float size) { return R_StringWidth(text, size, size, true); }

void R_Rect(float x, float y, float w, float h, const float *rgb, float alpha) { R_DrawFill(x, y, w, h, rgb, alpha, 0); }

bool R_PictureSize(const char *name, int *w, int *h)
{
	int handle = R_ImageHandle(name);
	*w = *h = 0;
	if (!handle)
		return false;
	R_ImageSize(handle, w, h);
	return *w > 0;
}

bool R_Picture(float x, float y, float w, float h, const char *name, float alpha)
{
	static const float white[3] = {1, 1, 1};
	material_t *m = R_FindMaterial(name, MATF_2D);
	if (!m)
		return false;
	R_DrawPic(x, y, w, h, m, 0, 0, 1, 1, white, alpha, 0);
	return true;
}
