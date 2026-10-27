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
	if (!numverts || !R_Active()) {
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

#define MAX_FONTS 32
#define MAX_FONT_SIZES 8
#define FONT_ATLAS 1024
#define FONT_FIRST 32
#define FONT_COUNT 224 /* 32..255 */

typedef struct {
	int pixelsize;
	image_t *atlas;
	stbtt_bakedchar chars[FONT_COUNT];
	float ascent;
} fontsize_t;

typedef struct {
	bool used;
	char name[128];
	uint8_t *ttf; /* NULL: the built-in bitmap font */
	stbtt_fontinfo info;
	int requested[MAX_FONT_SIZES];
	int numrequested;
	fontsize_t sizes[MAX_FONT_SIZES];
	int numsizes;
} font_t;

static font_t fonts[MAX_FONTS];
static image_t *bitmapfont;
static int r_drawfont; /* the font text uses, from the client game's drawfont global */

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

static fontsize_t *font_size(font_t *f, float height)
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
		if (d < bestdiff)
			bestdiff = d, best = i;
	}
	/* bake a new size when none is close enough */
	if ((best < 0 || bestdiff > 1) && f->numsizes < MAX_FONT_SIZES) {
		fontsize_t *fs = &f->sizes[f->numsizes];
		uint8_t *alpha = Mem_Alloc(FONT_ATLAS * FONT_ATLAS);
		uint8_t *rgba = Mem_Alloc(FONT_ATLAS * FONT_ATLAS * 4);
		char name[160];
		int asc, desc, gap;
		float scale;
		fs->pixelsize = want;
		if (stbtt_BakeFontBitmap(f->ttf, stbtt_GetFontOffsetForIndex(f->ttf, 0), (float)want, alpha, FONT_ATLAS,
		                         FONT_ATLAS, FONT_FIRST, FONT_COUNT, fs->chars) == 0) {
			Con_DPrintf("font %s: size %d did not fit\n", f->name, want);
		}
		for (int i = 0; i < FONT_ATLAS * FONT_ATLAS; i++) {
			rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
			rgba[i * 4 + 3] = alpha[i];
		}
		snprintf(name, sizeof(name), "$font:%s:%d", f->name, want);
		fs->atlas = R_ImageFromPixels(name, rgba, FONT_ATLAS, FONT_ATLAS, IMG_NOMIP | IMG_CLAMP);
		stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
		scale = stbtt_ScaleForPixelHeight(&f->info, (float)want);
		fs->ascent = asc * scale;
		Mem_Free(alpha);
		Mem_Free(rgba);
		return &f->sizes[f->numsizes++];
	}
	return best >= 0 ? &f->sizes[best] : NULL;
}

static bool load_ttf(font_t *f, const char *path)
{
	static const char *exts[] = {"", ".ttf", ".otf"};
	for (size_t i = 0; i < BW_ARRAY_COUNT(exts); i++) {
		char name[160];
		size_t len;
		uint8_t *data;
		snprintf(name, sizeof(name), "%s%s", path, exts[i]);
		data = (uint8_t *)FS_LoadFile(name, &len);
		if (!data)
			continue;
		if (!stbtt_InitFont(&f->info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
			Mem_Free(data);
			continue;
		}
		f->ttf = data;
		return true;
	}
	return false;
}

int R_LoadFont(const char *name, const char *path, const char *sizes, int slot)
{
	font_t *f;
	if (slot < 0) {
		for (slot = 1; slot < MAX_FONTS; slot++)
			if (fonts[slot].used && !Str_ICmp(fonts[slot].name, path) && path[0])
				return slot;
		for (slot = 1; slot < MAX_FONTS && fonts[slot].used; slot++)
			;
	}
	if (slot <= 0 || slot >= MAX_FONTS)
		return 0;
	f = &fonts[slot];
	Mem_Free(f->ttf);
	memset(f, 0, sizeof(*f));
	f->used = true;
	Str_Copy(f->name, path, sizeof(f->name));
	if (!load_ttf(f, path))
		Con_DPrintf("font %s: no TrueType file, using the built-in font\n", path);
	(void)name;
	(void)sizes;
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
	if (fs && c >= FONT_FIRST && c < FONT_FIRST + FONT_COUNT)
		return fs->chars[c - FONT_FIRST].xadvance * (sy / fs->pixelsize) * (sx / sy);
	(void)f;
	return sx;
}

static float draw_text(float x, float y, const char *text, float sx, float sy, const float *rgb, float alpha,
                       int flags, bool usemarkup, bool measure)
{
	font_t *f = &fonts[r_drawfont];
	fontsize_t *fs = sy > 0 ? font_size(f, sy) : NULL;
	float color[4] = {rgb[0], rgb[1], rgb[2], alpha};
	float startx = x, maxx = x;
	const char *p = text;
	GLuint tex = fs ? fs->atlas->tex : bitmapfont ? bitmapfont->tex : 0;
	if (sx <= 0)
		sx = sy;
	if (!measure && in2d)
		set_state(tex, flag_blend(flags, BLEND_ALPHA), 0, 0, NULL, 0);
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
			if (fs && c >= FONT_FIRST && c < FONT_FIRST + FONT_COUNT) {
				stbtt_bakedchar *b = &fs->chars[c - FONT_FIRST];
				float k = sy / fs->pixelsize; /* atlas pixels to virtual ones */
				float kx = k * (sx / sy);
				float base = y + fs->ascent * k;
				set_state(tex, cur_blend, cur_src, cur_dst, NULL, 0);
				quad(x + b->xoff * kx, base + b->yoff * k, (b->x1 - b->x0) * kx, (b->y1 - b->y0) * k,
				     b->x0 / (float)FONT_ATLAS, b->y0 / (float)FONT_ATLAS, b->x1 / (float)FONT_ATLAS,
				     b->y1 / (float)FONT_ATLAS, color);
			} else if (!fs && c > 32 && c < 128) {
				float s = (c % 16) / 16.0f, t = (c / 16) / 16.0f;
				/* flushes a full batch (a console page of text is more
				 * than one) and restores the font after inline images */
				set_state(tex, cur_blend, cur_src, cur_dst, NULL, 0);
				quad(x, y, sx, sy, s, t, s + 1 / 16.0f, t + 1 / 16.0f, color);
			}
		}
		x += char_advance(f, fs, c, sx, sy);
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

void R_DrawConsole(int w, int h)
{
	static const float white[3] = {1, 1, 1}, dim[3] = {0, 0, 0};
	float size = con_textsize->value > 0 ? con_textsize->value : 8;
	float height, y;
	int cursor, saved = r_drawfont;
	const char *input;
	if (!Con_IsActive())
		return;
	r_drawfont = con_font->string[0] ? R_LoadFont("", con_font->string, "", -1) : 0;
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
		R_DrawString(4, y, line, size, size, white, 1, 0, false);
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
		R_DrawString(4, y, l, size, size, white, 1, 0, true);
	}
	r_drawfont = saved;
}

void R_InitDraw(void)
{
	con_font = Cvar_Get("con_font", "", CVAR_ARCHIVE, "TrueType or OpenType font for the console, empty for the built-in one");
	scr_conheight = Cvar_Get("scr_conheight", "0.5", CVAR_ARCHIVE, "Console height, as a fraction of the screen");
	scr_conalpha = Cvar_Get("scr_conalpha", "0.85", CVAR_ARCHIVE, "Console background opacity");
	con_textsize = Cvar_Get("con_textsize", "8", CVAR_ARCHIVE, "Console text height in virtual pixels");
	fonts[0].used = true;
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
	for (int i = 0; i < MAX_FONTS; i++) {
		Mem_Free(fonts[i].ttf);
		memset(&fonts[i], 0, sizeof(fonts[i]));
	}
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
	image_t *img = R_FindImage(name, IMG_NOMIP);
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
	if (!in2d || !R_Active() || numverts <= 0 || numidx <= 0)
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
