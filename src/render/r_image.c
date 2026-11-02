/* Bulwark engine - textures: loading image files and uploading them.
 * Decoding uses stb_image (public domain). ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/fs.h"
#include "../common/cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_NO_STDIO
#include "../thirdparty/stb_image.h" /* built in src/common/stb_image_impl.c */

#define IMAGE_HASH 1024
static image_t *images[IMAGE_HASH];
image_t *r_whitecube, *r_whiteimage, *r_blackimage, *r_checkerimage, *r_flatnormal;
static uint8_t q1palette[768];
static bool have_palette;

static unsigned name_hash(const char *s)
{
	unsigned h = 5381;
	for (; *s; s++)
		h = h * 33 + (unsigned char)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
	return h & (IMAGE_HASH - 1);
}

static void strip_ext(const char *in, char *out, size_t size)
{
	char *dot, *slash;
	Str_Copy(out, in, size);
	dot = strrchr(out, '.');
	slash = strrchr(out, '/');
	if (dot && (!slash || dot > slash))
		*dot = 0;
}

static bool has_ext(const char *name)
{
	const char *dot = strrchr(name, '.'), *slash = strrchr(name, '/');
	return dot && (!slash || dot > slash) && strlen(dot) <= 5;
}

/* Quake .lmp pictures: width, height, then 8-bit palette indices; 255 is clear */
static uint8_t *decode_lmp(const uint8_t *d, size_t len, int *w, int *h)
{
	int iw, ih;
	uint8_t *out;
	if (len < 8)
		return NULL;
	iw = d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24);
	ih = d[4] | (d[5] << 8) | (d[6] << 16) | (d[7] << 24);
	if (iw <= 0 || ih <= 0 || iw > 4096 || ih > 4096 || (size_t)iw * ih + 8 > len)
		return NULL;
	out = Mem_Alloc((size_t)iw * ih * 4);
	for (int i = 0; i < iw * ih; i++) {
		uint8_t c = d[8 + i];
		out[i * 4 + 0] = have_palette ? q1palette[c * 3] : c;
		out[i * 4 + 1] = have_palette ? q1palette[c * 3 + 1] : c;
		out[i * 4 + 2] = have_palette ? q1palette[c * 3 + 2] : c;
		out[i * 4 + 3] = c == 255 ? 0 : 255;
	}
	*w = iw;
	*h = ih;
	return out;
}

/* GoldSrc sprites (IDSP version 2), one frame as an image.
 * Header: ident, version, type, texture format, radius (float), max width,
 * max height, frame count, beam length (float), sync type; then a 16-bit
 * palette size and that many RGB triples; then the frames, each a 32-bit
 * frame type (0 single, otherwise a group of N frames with N intervals),
 * origin x/y, width, height and width*height palette indices. Texture
 * formats: 0 normal, 1 additive, 2 index-alpha, 3 alpha-test (index 255
 * clear). An additive frame keeps full alpha; the caller draws it with the
 * additive flag. Layout checked on CSO's 640hud7.spr (66,366 bytes). */
static const uint8_t *spr_frame(const uint8_t *d, size_t len, size_t *ofs, int *fw, int *fh)
{
	int32_t fr[4]; /* origin x, origin y, width, height */
	if (*ofs + 16 > len)
		return NULL;
	memcpy(fr, d + *ofs, 16);
	if (fr[2] <= 0 || fr[3] <= 0 || fr[2] > 4096 || fr[3] > 4096 ||
	    *ofs + 16 + (size_t)fr[2] * fr[3] > len)
		return NULL;
	*fw = fr[2];
	*fh = fr[3];
	*ofs += 16 + (size_t)fr[2] * fr[3];
	return d + *ofs - (size_t)fr[2] * fr[3];
}

static uint8_t *decode_spr(const uint8_t *d, size_t len, int frame, int *w, int *h)
{
	int32_t hdr[10];
	int16_t npal;
	const uint8_t *pal, *px = NULL;
	size_t ofs;
	uint8_t *out;
	int format, count = 0;
	if (len < 42 || memcmp(d, "IDSP", 4))
		return NULL;
	memcpy(hdr, d, 40);
	if (hdr[1] != 2)
		return NULL;
	format = hdr[3];
	memcpy(&npal, d + 40, 2);
	if (npal <= 0 || npal > 256 || 42 + (size_t)npal * 3 > len)
		return NULL;
	pal = d + 42;
	ofs = 42 + (size_t)npal * 3;
	for (int f = 0; f < hdr[7] && !px; f++) {
		int32_t type;
		if (ofs + 4 > len)
			return NULL;
		memcpy(&type, d + ofs, 4);
		ofs += 4;
		if (type == 0) {
			const uint8_t *p = spr_frame(d, len, &ofs, w, h);
			if (!p)
				return NULL;
			if (count++ == frame)
				px = p;
		} else {
			int32_t n;
			if (ofs + 4 > len)
				return NULL;
			memcpy(&n, d + ofs, 4);
			if (n <= 0 || n > 1024)
				return NULL;
			ofs += 4 + (size_t)n * 4;
			for (int g = 0; g < n && !px; g++) {
				const uint8_t *p = spr_frame(d, len, &ofs, w, h);
				if (!p)
					return NULL;
				if (count++ == frame)
					px = p;
			}
		}
	}
	if (!px)
		return NULL;
	out = Mem_Alloc((size_t)*w * *h * 4);
	for (int i = 0; i < *w * *h; i++) {
		uint8_t c = px[i];
		const uint8_t *rgb = c < npal ? pal + c * 3 : pal;
		out[i * 4 + 0] = rgb[0];
		out[i * 4 + 1] = rgb[1];
		out[i * 4 + 2] = rgb[2];
		out[i * 4 + 3] = 255;
		if (format == 2) { /* index-alpha: the last palette colour, alpha from the index */
			const uint8_t *last = pal + (npal - 1) * 3;
			out[i * 4 + 0] = last[0];
			out[i * 4 + 1] = last[1];
			out[i * 4 + 2] = last[2];
			out[i * 4 + 3] = c;
		} else if (format == 3 && c == 255) {
			out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = out[i * 4 + 3] = 0;
		}
	}
	return out;
}

/* "sprites/x.spr" or "sprites/x.spr:N" (frame N), as spriteframe returns */
static uint8_t *load_spr(const char *name, int *w, int *h)
{
	char path[180];
	const char *colon = strstr(name, ".spr:");
	int frame = colon ? atoi(colon + 5) : 0;
	size_t len;
	uint8_t *data, *out;
	Str_Copy(path, name, sizeof(path));
	if (colon && (size_t)(colon - name) + 4 < sizeof(path))
		path[colon - name + 4] = 0;
	data = (uint8_t *)FS_LoadFile(path, &len);
	if (!data)
		return NULL;
	out = decode_spr(data, len, frame, w, h);
	if (!out)
		Con_DPrintf("couldn't decode sprite %s frame %d\n", path, frame);
	Mem_Free(data);
	return out;
}

/* the texture format of a sprite file (0 normal, 1 additive, 2 index-alpha,
 * 3 alpha-test), or -1 if it isn't one */
int R_SpriteFormat(const char *name)
{
	char path[180];
	const char *colon = strstr(name, ".spr");
	size_t len;
	uint8_t *data;
	int fmt = -1;
	if (!colon)
		return -1;
	Str_Copy(path, name, sizeof(path));
	if ((size_t)(colon - name) + 4 < sizeof(path))
		path[colon - name + 4] = 0;
	data = (uint8_t *)FS_LoadFile(path, &len);
	if (data && len >= 16 && !memcmp(data, "IDSP", 4))
		memcpy(&fmt, data + 12, 4);
	Mem_Free(data);
	return fmt;
}

static uint8_t *decode_file(const char *path, int *w, int *h)
{
	size_t len;
	uint8_t *data = (uint8_t *)FS_LoadFile(path, &len), *out = NULL;
	int comp;
	if (!data)
		return NULL;
	if (strstr(path, ".lmp"))
		out = decode_lmp(data, len, w, h);
	else if (R_LooksLikeSVG(data, len))
		out = R_DecodeSVG(data, len, w, h);
	else if ((out = Plug_DecodeImage(path, data, len, w, h)) != NULL) {
		Mem_Free(data);
		return out;
	} else
		out = stbi_load_from_memory(data, (int)len, w, h, &comp, 4);
	if (!out && !strstr(path, ".lmp"))
		Con_DPrintf("couldn't decode %s: %s\n", path, stbi_failure_reason());
	Mem_Free(data);
	return out;
}

/* an image file already in memory, as RGBA (Mem_Free the result) */
uint8_t *R_DecodeImage(const uint8_t *data, size_t len, int *w, int *h)
{
	int comp;
	if (len > 0 && R_LooksLikeSVG(data, len))
		return R_DecodeSVG(data, len, w, h);
	return len > 0 && len < (size_t)1 << 30 ? stbi_load_from_memory(data, (int)len, w, h, &comp, 4) : NULL;
}

static uint8_t *load_pixels(const char *name, int *w, int *h);

/* tries the name as given, then with each known extension in turn, then
 * under textures/ (Nuclide's .mat files name images relative to it) */
uint8_t *R_LoadPixels(const char *name, int *w, int *h)
{
	char tex[192];
	/* .svg isn't one of the extensions tried: SVG files are named in full */
	uint8_t *p = load_pixels(name, w, h);
	if (p || !name || !*name || !Str_NICmp(name, "textures/", 9) || name[0] == '*' || strstr(name, ".spr"))
		return p;
	snprintf(tex, sizeof(tex), "textures/%s", name);
	return load_pixels(tex, w, h);
}

static uint8_t *load_pixels(const char *name, int *w, int *h)
{
	static const char *exts[] = {".tga", ".png", ".jpg", ".jpeg", ".bmp", ".lmp"};
	char base[160], path[180];
	uint8_t *p;
	if (!name || !*name)
		return NULL;
	if (strstr(name, ".spr"))
		return load_spr(name, w, h);
	if (has_ext(name) && (p = decode_file(name, w, h)))
		return p;
	strip_ext(name, base, sizeof(base));
	for (size_t i = 0; i < BW_ARRAY_COUNT(exts); i++) {
		snprintf(path, sizeof(path), "%s%s", base, exts[i]);
		if (FS_FileExists(path) && (p = decode_file(path, w, h)))
			return p;
	}
	/* then the formats plugins add (.vtf) */
	for (int i = 0; Plug_ImageExt(i); i++) {
		snprintf(path, sizeof(path), "%s%s", base, Plug_ImageExt(i));
		if (FS_FileExists(path) && (p = decode_file(path, w, h)))
			return p;
	}
	return NULL;
}

static bool image_exists(const char *name);

bool R_ImageExists(const char *name)
{
	char tex[192];
	if (image_exists(name))
		return true;
	if (!name || !*name || !Str_NICmp(name, "textures/", 9) || strstr(name, ".spr"))
		return false;
	snprintf(tex, sizeof(tex), "textures/%s", name);
	return image_exists(tex);
}

static bool image_exists(const char *name)
{
	static const char *exts[] = {".tga", ".png", ".jpg", ".jpeg", ".bmp", ".lmp"};
	char base[160], path[180];
	const char *spr = strstr(name, ".spr");
	if (spr) {
		Str_Copy(path, name, sizeof(path));
		if ((size_t)(spr - name) + 4 < sizeof(path))
			path[spr - name + 4] = 0;
		return FS_FileExists(path);
	}
	if (has_ext(name) && FS_FileExists(name))
		return true;
	strip_ext(name, base, sizeof(base));
	for (size_t i = 0; i < BW_ARRAY_COUNT(exts); i++) {
		snprintf(path, sizeof(path), "%s%s", base, exts[i]);
		if (FS_FileExists(path))
			return true;
	}
	for (int i = 0; Plug_ImageExt(i); i++) {
		snprintf(path, sizeof(path), "%s%s", base, Plug_ImageExt(i));
		if (FS_FileExists(path))
			return true;
	}
	return false;
}

static void set_params(GLenum target, int flags)
{
	bool mip = !(flags & IMG_NOMIP);
	bool nearest = (flags & IMG_NEAREST) || (gl_texturemode && !Str_ICmp(gl_texturemode->string, "nearest"));
	qglTexParameteri(target, GL_TEXTURE_MIN_FILTER,
	                 nearest ? GL_NEAREST : (mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR));
	qglTexParameteri(target, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
	qglTexParameteri(target, GL_TEXTURE_WRAP_S, (flags & IMG_CLAMP) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	qglTexParameteri(target, GL_TEXTURE_WRAP_T, (flags & IMG_CLAMP) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	if (mip)
		qglTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
	qglGetError(); /* anisotropy is optional */
}

static image_t *new_image(const char *name)
{
	image_t *img = Mem_Alloc(sizeof(*img));
	unsigned h = name_hash(name);
	Str_Copy(img->name, name, sizeof(img->name));
	img->next = images[h];
	images[h] = img;
	return img;
}

static void upload(image_t *img, const uint8_t *rgba, int w, int h, int flags)
{
	img->width = w;
	img->height = h;
	img->target = GL_TEXTURE_2D;
	img->alpha = false;
	for (int i = 0; i < w * h; i++)
		if (rgba[i * 4 + 3] < 255) {
			img->alpha = true;
			break;
		}
	if (!R_Active())
		return;
	if (!img->tex)
		qglGenTextures(1, &img->tex);
	GL_BindTexture(0, GL_TEXTURE_2D, img->tex);
	qglPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	if (!(flags & IMG_NOMIP))
		qglGenerateMipmap(GL_TEXTURE_2D);
	set_params(GL_TEXTURE_2D, flags);
}

image_t *R_ImageFromPixels(const char *name, const uint8_t *rgba, int w, int h, int flags)
{
	image_t *img = NULL;
	for (image_t *i = images[name_hash(name)]; i; i = i->next)
		if (!Str_ICmp(i->name, name)) {
			img = i;
			break;
		}
	if (!img)
		img = new_image(name);
	if (img->missing)
		img->tex = 0; /* it borrowed the checker texture */
	img->missing = false;
	upload(img, rgba, w, h, flags);
	return img;
}

image_t *R_FindImage(const char *name, int flags)
{
	image_t *img;
	uint8_t *pix;
	int w, h;
	if (!name || !*name)
		return r_checkerimage;
	/* $rt:name is an image the game uploads itself (r_uploadimage) */
	if (!Str_NICmp(name, "$rt:", 4))
		name += 4;
	for (img = images[name_hash(name)]; img; img = img->next)
		if (!Str_ICmp(img->name, name))
			return img;
	img = new_image(name);
	pix = R_LoadPixels(name, &w, &h);
	if (!pix) {
		/* remember the miss so the file system is not searched every frame */
		img->missing = true;
		img->tex = r_checkerimage ? r_checkerimage->tex : 0;
		img->target = GL_TEXTURE_2D;
		img->width = img->height = 64;
		Con_DPrintf("image %s not found\n", name);
		return img;
	}
	upload(img, pix, w, h, flags);
	Mem_Free(pix);
	return img;
}

/* Skyboxes are six images named like env/dune_rt. Each one is uploaded to
 * the cube face that the sky shader's lookup (-y, z, -x) reaches when the
 * view looks the matching way: rt is -y (right), lf +y, ft +x (forward),
 * bk -x, up +z and dn -z. */
/* turns a square RGBA image a quarter, counter-clockwise or clockwise */
static void rotate_quarter(uint8_t *pix, int n, bool ccw)
{
	uint32_t *src = Mem_Alloc((size_t)n * n * 4), *dst = (uint32_t *)pix;
	memcpy(src, pix, (size_t)n * n * 4);
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++)
			dst[y * n + x] = ccw ? src[x * n + (n - 1 - y)] : src[(n - 1 - x) * n + y];
	Mem_Free(src);
}

image_t *R_LoadCubemap(const char *basename)
{
	/* cube faces +X, -X, +Y, -Y, +Z, -Z in lookup space (-y, z, -x) */
	static const char *suffix[6] = {"rt", "lf", "up", "dn", "bk", "ft"};
	image_t *img;
	char name[160];
	int size = 0;
	snprintf(name, sizeof(name), "%s_cube", basename);
	for (img = images[name_hash(name)]; img; img = img->next)
		if (!Str_ICmp(img->name, name))
			return img->missing ? NULL : img;
	img = new_image(name);
	img->target = GL_TEXTURE_CUBE_MAP;
	if (R_Active()) {
		qglGenTextures(1, &img->tex);
		GL_BindTexture(3, GL_TEXTURE_CUBE_MAP, img->tex);
	}
	for (int f = 0; f < 6; f++) {
		char path[180];
		int w, h;
		uint8_t *pix;
		snprintf(path, sizeof(path), "%s_%s", basename, suffix[f]);
		pix = R_LoadPixels(path, &w, &h);
		if (!pix) {
			/* GoldSrc names its faces without the underscore: desertrt */
			snprintf(path, sizeof(path), "%s%s", basename, suffix[f]);
			pix = R_LoadPixels(path, &w, &h);
		}
		if (!pix) {
			Con_DPrintf("sky %s: missing %s\n", basename, path);
			img->missing = true;
			return NULL;
		}
		size = w;
		/* Quake-style sets store up and down turned a quarter: measured by
		 * matching their edges against the side faces (docs/spec/materials.md) */
		if ((f == 2 || f == 3) && w == h)
			rotate_quarter(pix, w, f == 2);
		if (R_Active())
			qglTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
		Mem_Free(pix);
	}
	img->width = img->height = size;
	if (R_Active()) {
		qglTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		qglTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		qglTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		qglTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	return img;
}

static image_t *solid(const char *name, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	uint8_t px[4 * 4 * 4];
	for (int i = 0; i < 16; i++) {
		px[i * 4] = r;
		px[i * 4 + 1] = g;
		px[i * 4 + 2] = b;
		px[i * 4 + 3] = a;
	}
	return R_ImageFromPixels(name, px, 4, 4, IMG_NOMIP);
}

/* a 1x1 white cubemap, so a sky without a box can still be tinted */
static image_t *white_cube(void)
{
	static const uint8_t white[4] = {255, 255, 255, 255};
	image_t *img = new_image("$whitecube");
	img->target = GL_TEXTURE_CUBE_MAP;
	img->width = img->height = 1;
	if (!R_Active())
		return img;
	qglGenTextures(1, &img->tex);
	GL_BindTexture(3, GL_TEXTURE_CUBE_MAP, img->tex);
	for (int f = 0; f < 6; f++)
		qglTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
	qglTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	qglTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	return img;
}

/* the Quake palette, or a grey ramp when the game has none */
const uint8_t *R_Palette(void)
{
	if (!have_palette)
		for (int i = 0; i < 768; i++)
			q1palette[i] = (uint8_t)(i / 3);
	return q1palette;
}

void R_InitImages(void)
{
	uint8_t check[64 * 64 * 4];
	size_t len;
	uint8_t *pal = (uint8_t *)FS_LoadFile("gfx/palette.lmp", &len);
	if (pal && len >= 768) {
		memcpy(q1palette, pal, 768);
		have_palette = true;
	}
	Mem_Free(pal);
	if (!have_palette) {
		/* Nuclide ships the palette as a 16x16 picture */
		int w, h;
		uint8_t *px = R_LoadPixels("gfx/palette.tga", &w, &h);
		if (px && w * h == 256) {
			for (int i = 0; i < 256; i++)
				memcpy(q1palette + i * 3, px + i * 4, 3);
			have_palette = true;
		}
		Mem_Free(px);
	}
	r_whiteimage = solid("$white", 255, 255, 255, 255);
	r_blackimage = solid("$black", 0, 0, 0, 255);
	r_flatnormal = solid("$flatnormal", 128, 128, 255, 255);
	{
		/* colours for debug geometry, picked by texture column (the Source
		 * plugin's bone boxes: hit groups 0 to 7, then plain bones) */
		static const uint8_t pal[16][3] = {
		    {200, 200, 200}, {230, 40, 40}, {240, 140, 30}, {240, 220, 40}, {60, 200, 60}, {40, 200, 220},
		    {60, 90, 240}, {210, 60, 220}, {110, 110, 120}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255},
		    {255, 255, 255}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
		uint8_t px[16 * 4];
		for (int i = 0; i < 16; i++) {
			memcpy(px + i * 4, pal[i], 3);
			px[i * 4 + 3] = 255;
		}
		R_ImageFromPixels("$bonepalette", px, 16, 1, IMG_NOMIP | IMG_NEAREST);
	}
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			bool on = ((x >> 3) ^ (y >> 3)) & 1;
			uint8_t *p = check + (y * 64 + x) * 4;
			p[0] = on ? 255 : 32;
			p[1] = on ? 0 : 32;
			p[2] = on ? 255 : 32;
			p[3] = 255;
		}
	r_checkerimage = R_ImageFromPixels("$checker", check, 64, 64, IMG_NEAREST);
	r_whitecube = white_cube();
}

void R_ShutdownImages(void)
{
	for (int i = 0; i < IMAGE_HASH; i++) {
		image_t *img = images[i];
		while (img) {
			image_t *next = img->next;
			if (img->tex && !img->missing && R_Active())
				GL_DeleteTextures(1, &img->tex);
			Mem_Free(img);
			img = next;
		}
		images[i] = NULL;
	}
	r_whiteimage = r_blackimage = r_checkerimage = r_flatnormal = NULL;
}

void R_ForEachImage(void (*fn)(image_t *img, void *ctx), void *ctx)
{
	for (int i = 0; i < IMAGE_HASH; i++)
		for (image_t *img = images[i]; img; img = img->next)
			fn(img, ctx);
}

/* loads an image's file again (skins after new ones were added); one that
 * was missing gets a texture of its own */
void R_ReloadImage(const char *name)
{
	image_t *img;
	uint8_t *pix;
	int w, h;
	for (img = images[name_hash(name)]; img; img = img->next)
		if (!Str_ICmp(img->name, name))
			break;
	if (!img) {
		R_FindImage(name, 0);
		return;
	}
	if (!(pix = R_LoadPixels(name, &w, &h)))
		return;
	if (img->missing) {
		img->missing = false;
		img->tex = 0;
	}
	upload(img, pix, w, h, 0);
	Mem_Free(pix);
}
