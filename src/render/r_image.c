/* Bulwark engine - textures: loading image files and uploading them.
 * Decoding uses stb_image (public domain). ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/fs.h"

#include <stdio.h>
#include <stdlib.h>

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

static uint8_t *decode_file(const char *path, int *w, int *h)
{
	size_t len;
	uint8_t *data = (uint8_t *)FS_LoadFile(path, &len), *out = NULL;
	int comp;
	if (!data)
		return NULL;
	if (strstr(path, ".lmp"))
		out = decode_lmp(data, len, w, h);
	else
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
	return len > 0 && len < (size_t)1 << 30 ? stbi_load_from_memory(data, (int)len, w, h, &comp, 4) : NULL;
}

/* tries the name as given, then with each known extension in turn */
uint8_t *R_LoadPixels(const char *name, int *w, int *h)
{
	static const char *exts[] = {".tga", ".png", ".jpg", ".jpeg", ".bmp", ".lmp"};
	char base[160], path[180];
	uint8_t *p;
	if (!name || !*name)
		return NULL;
	if (has_ext(name) && (p = decode_file(name, w, h)))
		return p;
	strip_ext(name, base, sizeof(base));
	for (size_t i = 0; i < BW_ARRAY_COUNT(exts); i++) {
		snprintf(path, sizeof(path), "%s%s", base, exts[i]);
		if (FS_FileExists(path) && (p = decode_file(path, w, h)))
			return p;
	}
	return NULL;
}

bool R_ImageExists(const char *name)
{
	static const char *exts[] = {".tga", ".png", ".jpg", ".jpeg", ".bmp", ".lmp"};
	char base[160], path[180];
	if (has_ext(name) && FS_FileExists(name))
		return true;
	strip_ext(name, base, sizeof(base));
	for (size_t i = 0; i < BW_ARRAY_COUNT(exts); i++) {
		snprintf(path, sizeof(path), "%s%s", base, exts[i]);
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
				qglDeleteTextures(1, &img->tex);
			Mem_Free(img);
			img = next;
		}
		images[i] = NULL;
	}
	r_whiteimage = r_blackimage = r_checkerimage = r_flatnormal = NULL;
}
