/* Bulwark Source formats plugin - VTF textures to RGBA. Layout and the
 * format numbers: docs/spec/source-formats.md. ISC licence, see LICENSE. */
#include "source.h"

#include <math.h>
#include <string.h>

enum {
	F_RGBA8888, F_ABGR8888, F_RGB888, F_BGR888, F_RGB565, F_I8, F_IA88, F_P8, F_A8, F_RGB888_BLUESCREEN,
	F_BGR888_BLUESCREEN, F_ARGB8888, F_BGRA8888, F_DXT1, F_DXT3, F_DXT5, F_BGRX8888, F_BGR565, F_BGRX5551,
	F_BGRA4444, F_DXT1_ONEBITALPHA, F_BGRA5551, F_UV88, F_UVWQ8888, F_RGBA16161616F, F_RGBA16161616, F_UVLX8888,
	F_COUNT
};

#define VTF_ENVMAP 0x4000

/* bytes a pixel, or 0 for the 4x4 block formats */
static const int bpp[F_COUNT] = {4, 4, 3, 3, 2, 1, 2, 1, 1, 3, 3, 4, 4, 0, 0, 0, 4, 2, 2, 2, 0, 2, 2, 4, 8, 8, 4};

static int block_bytes(int fmt) { return fmt == F_DXT1 || fmt == F_DXT1_ONEBITALPHA ? 8 : 16; }

static size_t image_size(int fmt, int w, int h, int d)
{
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	if (d < 1)
		d = 1;
	if (!bpp[fmt])
		return (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4) * (size_t)block_bytes(fmt) * (size_t)d;
	return (size_t)w * (size_t)h * (size_t)d * (size_t)bpp[fmt];
}

/* --- DXT blocks -------------------------------------------------------------- */

static void rgb565(unsigned v, unsigned char *c)
{
	c[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31);
	c[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
	c[2] = (unsigned char)((v & 31) * 255 / 31);
	c[3] = 255;
}

/* the colour part of a DXT block into 16 RGBA pixels; three_colour lets a
 * DXT1 block use colour 3 as transparent black */
static void colour_block(const unsigned char *b, unsigned char out[16][4], int dxt1)
{
	unsigned char pal[4][4];
	unsigned c0 = rd16(b), c1 = rd16(b + 2), bits = rd32(b + 4);
	rgb565(c0, pal[0]);
	rgb565(c1, pal[1]);
	if (c0 > c1 || !dxt1) {
		for (int k = 0; k < 3; k++) {
			pal[2][k] = (unsigned char)((2 * pal[0][k] + pal[1][k] + 1) / 3);
			pal[3][k] = (unsigned char)((pal[0][k] + 2 * pal[1][k] + 1) / 3);
		}
		pal[2][3] = pal[3][3] = 255;
	} else {
		for (int k = 0; k < 3; k++)
			pal[2][k] = (unsigned char)((pal[0][k] + pal[1][k]) / 2);
		pal[2][3] = 255;
		memset(pal[3], 0, 4);
	}
	for (int i = 0; i < 16; i++)
		memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
}

static void decode_block(int fmt, const unsigned char *b, unsigned char out[16][4])
{
	if (fmt == F_DXT1 || fmt == F_DXT1_ONEBITALPHA) {
		colour_block(b, out, 1);
		return;
	}
	colour_block(b + 8, out, 0);
	if (fmt == F_DXT3) {
		for (int i = 0; i < 16; i++) {
			unsigned a = (b[i / 2] >> ((i & 1) * 4)) & 15;
			out[i][3] = (unsigned char)(a * 17);
		}
	} else {
		unsigned char a[8];
		unsigned long long bits = 0;
		a[0] = b[0];
		a[1] = b[1];
		if (a[0] > a[1]) {
			for (int i = 1; i < 7; i++)
				a[i + 1] = (unsigned char)(((7 - i) * a[0] + i * a[1] + 3) / 7);
		} else {
			for (int i = 1; i < 5; i++)
				a[i + 1] = (unsigned char)(((5 - i) * a[0] + i * a[1] + 2) / 5);
			a[6] = 0;
			a[7] = 255;
		}
		for (int i = 0; i < 6; i++)
			bits |= (unsigned long long)b[2 + i] << (8 * i);
		for (int i = 0; i < 16; i++)
			out[i][3] = a[(bits >> (3 * i)) & 7];
	}
}

/* --- plain pixels -------------------------------------------------------------- */

static unsigned char half_to_byte(unsigned h)
{
	unsigned e = (h >> 10) & 31, m = h & 1023;
	float f;
	if (h & 0x8000)
		return 0;
	if (e == 0)
		f = (float)m / 1024.0f / 16384.0f;
	else if (e == 31)
		f = 1;
	else
		f = (1.0f + (float)m / 1024.0f) * ldexpf(1.0f, (int)e - 15);
	/* HDR: brightness above 1 is clipped; good enough for a texture view */
	f = f > 1 ? 1 : f;
	return (unsigned char)(f * 255.0f + 0.5f);
}

static void pixel(int fmt, const unsigned char *p, unsigned char *o)
{
	unsigned v;
	switch (fmt) {
	case F_RGBA8888: case F_UVWQ8888: case F_UVLX8888:
		o[0] = p[0], o[1] = p[1], o[2] = p[2], o[3] = p[3];
		break;
	case F_ABGR8888:
		o[0] = p[3], o[1] = p[2], o[2] = p[1], o[3] = p[0];
		break;
	case F_ARGB8888:
		o[0] = p[1], o[1] = p[2], o[2] = p[3], o[3] = p[0];
		break;
	case F_BGRA8888:
		o[0] = p[2], o[1] = p[1], o[2] = p[0], o[3] = p[3];
		break;
	case F_BGRX8888:
		o[0] = p[2], o[1] = p[1], o[2] = p[0], o[3] = 255;
		break;
	case F_RGB888:
		o[0] = p[0], o[1] = p[1], o[2] = p[2], o[3] = 255;
		break;
	case F_BGR888:
		o[0] = p[2], o[1] = p[1], o[2] = p[0], o[3] = 255;
		break;
	case F_RGB888_BLUESCREEN:
		o[0] = p[0], o[1] = p[1], o[2] = p[2];
		o[3] = (p[0] == 0 && p[1] == 0 && p[2] == 255) ? 0 : 255;
		break;
	case F_BGR888_BLUESCREEN:
		o[0] = p[2], o[1] = p[1], o[2] = p[0];
		o[3] = (p[0] == 255 && p[1] == 0 && p[2] == 0) ? 0 : 255;
		break;
	case F_RGB565: /* red in the low bits */
		v = rd16(p);
		o[0] = (unsigned char)((v & 31) * 255 / 31), o[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
		o[2] = (unsigned char)(((v >> 11) & 31) * 255 / 31), o[3] = 255;
		break;
	case F_BGR565: /* blue in the low bits */
		rgb565(rd16(p), o);
		break;
	case F_BGRX5551: case F_BGRA5551:
		v = rd16(p);
		o[2] = (unsigned char)((v & 31) * 255 / 31), o[1] = (unsigned char)(((v >> 5) & 31) * 255 / 31);
		o[0] = (unsigned char)(((v >> 10) & 31) * 255 / 31);
		o[3] = fmt == F_BGRA5551 ? ((v & 0x8000) ? 255 : 0) : 255;
		break;
	case F_BGRA4444:
		v = rd16(p);
		o[2] = (unsigned char)((v & 15) * 17), o[1] = (unsigned char)(((v >> 4) & 15) * 17);
		o[0] = (unsigned char)(((v >> 8) & 15) * 17), o[3] = (unsigned char)(((v >> 12) & 15) * 17);
		break;
	case F_I8:
		o[0] = o[1] = o[2] = p[0], o[3] = 255;
		break;
	case F_IA88:
		o[0] = o[1] = o[2] = p[0], o[3] = p[1];
		break;
	case F_A8:
		o[0] = o[1] = o[2] = 255, o[3] = p[0];
		break;
	case F_UV88:
		o[0] = p[0], o[1] = p[1], o[2] = 255, o[3] = 255;
		break;
	case F_RGBA16161616F:
		for (int k = 0; k < 4; k++)
			o[k] = half_to_byte(rd16(p + 2 * k));
		break;
	case F_RGBA16161616:
		for (int k = 0; k < 4; k++)
			o[k] = (unsigned char)(rd16(p + 2 * k) >> 8);
		break;
	default:
		o[0] = o[1] = o[2] = o[3] = 255;
	}
}

static unsigned char *decode(int fmt, const unsigned char *d, int w, int h)
{
	unsigned char *out = api->alloc((size_t)w * (size_t)h * 4);
	if (!bpp[fmt]) {
		int bw = (w + 3) / 4, bh = (h + 3) / 4, bb = block_bytes(fmt);
		for (int by = 0; by < bh; by++)
			for (int bx = 0; bx < bw; bx++) {
				unsigned char px[16][4];
				decode_block(fmt, d + ((size_t)by * bw + bx) * bb, px);
				for (int i = 0; i < 16; i++) {
					int x = bx * 4 + (i & 3), y = by * 4 + i / 4;
					if (x < w && y < h)
						memcpy(out + ((size_t)y * w + x) * 4, px[i], 4);
				}
			}
	} else {
		for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
			pixel(fmt, d + i * (size_t)bpp[fmt], out + i * 4);
	}
	return out;
}

/* --- the file -------------------------------------------------------------------- */

unsigned char *VTF_Decode(const void *data, size_t len, int *width, int *height)
{
	const unsigned char *d = data;
	unsigned major, minor, hdrsize, flags, frames, firstframe, mips, faces, depth = 1;
	int w, h, fmt, lowfmt, loww, lowh;
	size_t hi = 0, skip = 0;
	if (len < 64 || memcmp(d, "VTF\0", 4))
		return NULL;
	major = rd32(d + 4);
	minor = rd32(d + 8);
	hdrsize = rd32(d + 12);
	w = (int)rd16(d + 16);
	h = (int)rd16(d + 18);
	flags = rd32(d + 20);
	frames = rd16(d + 24);
	firstframe = rd16(d + 26);
	fmt = (int)rd32(d + 52);
	mips = d[56];
	lowfmt = (int)rd32(d + 57);
	loww = d[61];
	lowh = d[62];
	if (major != 7 || w < 1 || h < 1 || w > 16384 || h > 16384 || fmt < 0 || fmt >= F_COUNT || fmt == F_P8) {
		srcprint("vtf: unsupported (version %u.%u, %dx%d, format %d)\n", major, minor, w, h, fmt);
		return NULL;
	}
	if (minor >= 2 && len >= 65)
		depth = rd16(d + 63);
	if (depth < 1)
		depth = 1;
	if (frames < 1)
		frames = 1;
	if (mips < 1)
		mips = 1;
	faces = 1;
	if (flags & VTF_ENVMAP)
		faces = (minor >= 5 || firstframe == 0xffff) ? 6 : 7;
	if (minor >= 3 && len >= 80) {
		/* resources: a three-byte tag, a flags byte, then an offset */
		unsigned n = rd32(d + 68);
		for (unsigned i = 0; i < n && 80 + (size_t)i * 8 + 8 <= len; i++) {
			const unsigned char *r = d + 80 + (size_t)i * 8;
			if (r[0] == 0x30 && r[1] == 0 && r[2] == 0)
				hi = rd32(r + 4);
			/* Strata's v7.6 compression: level 0 means stored plain */
			if (!memcmp(r, "AXC", 3) && rd32(r + 4) + 8 <= len && rd32(d + rd32(r + 4) + 4) != 0) {
				srcprint("vtf: compressed (AXC) textures aren't supported\n");
				return NULL;
			}
		}
		if (!hi)
			return NULL;
	} else {
		hi = hdrsize;
		if (lowfmt >= 0 && lowfmt < F_COUNT && loww && lowh)
			hi += image_size(lowfmt, loww, lowh, 1);
	}
	/* mipmaps run smallest first; inside each, frames, faces, then slices */
	for (unsigned m = mips - 1; m > 0; m--)
		skip += image_size(fmt, w >> m, h >> m, (int)(depth >> m)) * frames * faces;
	if (hi + skip + image_size(fmt, w, h, 1) > len) {
		srcprint("vtf: file is shorter than its header says\n");
		return NULL;
	}
	*width = w;
	*height = h;
	return decode(fmt, d + hi + skip, w, h);
}
