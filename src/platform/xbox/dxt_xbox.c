/* Bulwark engine - S3TC (DXT1 and DXT5) block compression for the Xbox
 * GPU's textures. ISC licence, see LICENSE.
 *
 * Written for this engine from the format's definition (the NV2A reads the
 * standard S3TC blocks: two RGB565 end points, little-endian, then two
 * bits per texel, row by row from the low bits; DXT5 puts an alpha block
 * of two 8-bit end points and three bits per texel in front).
 *
 * The colour fit: the block's principal axis (power iteration on the
 * covariance), its extent along that axis as the end points, then two
 * rounds of least squares on the end points for the chosen indices. A
 * block of one colour is matched through tables that find the 565 pair
 * whose two-thirds point lands closest to it, so flat areas (dark
 * lightmaps above all) keep their 8-bit value instead of snapping to 565.
 * The decoder follows the same arithmetic as the encoder's estimates. */
#include "dxt_xbox.h"

#include <string.h>

/* --- 565 -------------------------------------------------------------------------- */

static inline int expand5(int v) { return (v << 3) | (v >> 2); }
static inline int expand6(int v) { return (v << 2) | (v >> 4); }

static inline uint16_t pack565(int r5, int g6, int b5) { return (uint16_t)((r5 << 11) | (g6 << 5) | b5); }

static inline int q5(float v)
{
	int q = (int)(v * (31.0f / 255.0f) + 0.5f);
	return q < 0 ? 0 : q > 31 ? 31 : q;
}
static inline int q6(float v)
{
	int q = (int)(v * (63.0f / 255.0f) + 0.5f);
	return q < 0 ? 0 : q > 63 ? 63 : q;
}

static void unpack565(uint16_t c, int rgb[3])
{
	rgb[0] = expand5(c >> 11), rgb[1] = expand6((c >> 5) & 63), rgb[2] = expand5(c & 31);
}

/* the palette the GPU makes from two end points */
static void palette(uint16_t c0, uint16_t c1, int four, int pal[4][3])
{
	unpack565(c0, pal[0]);
	unpack565(c1, pal[1]);
	for (int k = 0; k < 3; k++) {
		if (four) {
			pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
			pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
		} else {
			pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
			pal[3][k] = 0;
		}
	}
}

/* --- one colour --------------------------------------------------------------------- */

/* for each 8-bit value, the end points (high, low) whose 2/3 point is closest */
static uint8_t match5[256][2], match6[256][2];
static int tables_ready;

static void make_tables(void)
{
	for (int v = 0; v < 256; v++) {
		int best5 = 1 << 30, best6 = 1 << 30;
		for (int a = 0; a < 64; a++)
			for (int b = 0; b < 64; b++) {
				if (a < 32 && b < 32) {
					int e = (2 * expand5(a) + expand5(b)) / 3 - v;
					e = e * e * 64 + (a - b) * (a - b); /* ties: the closer pair */
					if (e < best5)
						best5 = e, match5[v][0] = (uint8_t)a, match5[v][1] = (uint8_t)b;
				}
				int e = (2 * expand6(a) + expand6(b)) / 3 - v;
				e = e * e * 64 + (a - b) * (a - b);
				if (e < best6)
					best6 = e, match6[v][0] = (uint8_t)a, match6[v][1] = (uint8_t)b;
			}
	}
	tables_ready = 1;
}

/* --- the colour block ----------------------------------------------------------------- */

typedef struct {
	uint16_t c0, c1;
	uint32_t idx;
	int err;
} cfit_t;

/* indices for given end points; `use` marks the texels that count */
static void choose(cfit_t *f, const uint8_t *rgba, const uint8_t *use, int four)
{
	int pal[4][3];
	palette(f->c0, f->c1, four, pal);
	int n = four ? 4 : 3;
	f->idx = 0, f->err = 0;
	for (int i = 0; i < 16; i++) {
		if (!use[i]) {
			f->idx |= 3u << (2 * i); /* transparent (three-colour mode only) */
			continue;
		}
		const uint8_t *p = rgba + i * 4;
		int best = 1 << 30, bi = 0;
		for (int k = 0; k < n; k++) {
			int dr = pal[k][0] - p[0], dg = pal[k][1] - p[1], db = pal[k][2] - p[2];
			int e = dr * dr + dg * dg + db * db;
			if (e < best)
				best = e, bi = k;
		}
		f->idx |= (uint32_t)bi << (2 * i);
		f->err += best;
	}
}

/* end points from two float colours, in the order the mode needs */
static void set_ends(cfit_t *f, const float a[3], const float b[3], int four)
{
	uint16_t ca = pack565(q5(a[0]), q6(a[1]), q5(a[2]));
	uint16_t cb = pack565(q5(b[0]), q6(b[1]), q5(b[2]));
	/* four colours need c0 > c1, three need c0 <= c1 */
	if (four ? ca < cb : ca > cb) {
		uint16_t t = ca;
		ca = cb, cb = t;
	}
	f->c0 = ca, f->c1 = cb;
}

/* least squares: the end points that best fit the texels for the indices */
static int refit(const cfit_t *f, const uint8_t *rgba, const uint8_t *use, int four, float a[3], float b[3])
{
	static const float w4[4] = {1.0f, 0.0f, 2.0f / 3, 1.0f / 3}, w3[3] = {1.0f, 0.0f, 0.5f};
	float aa = 0, ab = 0, bb = 0, ax[3] = {0}, bx[3] = {0};
	for (int i = 0; i < 16; i++) {
		if (!use[i])
			continue;
		int k = (f->idx >> (2 * i)) & 3;
		float wa = four ? w4[k] : w3[k], wb = 1.0f - wa;
		aa += wa * wa, ab += wa * wb, bb += wb * wb;
		for (int c = 0; c < 3; c++)
			ax[c] += wa * rgba[i * 4 + c], bx[c] += wb * rgba[i * 4 + c];
	}
	float det = aa * bb - ab * ab;
	if (det < 1e-6f && det > -1e-6f)
		return 0;
	for (int c = 0; c < 3; c++) {
		a[c] = (ax[c] * bb - bx[c] * ab) / det;
		b[c] = (bx[c] * aa - ax[c] * ab) / det;
	}
	return 1;
}

static void fit_colour(cfit_t *best, const uint8_t *rgba, const uint8_t *use, int four)
{
	int n = 0;
	float mean[3] = {0}, lo[3] = {255, 255, 255}, hi[3] = {0};
	const uint8_t *first = NULL;
	int solid = 1;
	for (int i = 0; i < 16; i++) {
		if (!use[i])
			continue;
		const uint8_t *p = rgba + i * 4;
		if (!first)
			first = p;
		else if (p[0] != first[0] || p[1] != first[1] || p[2] != first[2])
			solid = 0;
		for (int c = 0; c < 3; c++) {
			mean[c] += p[c];
			if (p[c] < lo[c])
				lo[c] = p[c];
			if (p[c] > hi[c])
				hi[c] = p[c];
		}
		n++;
	}
	if (!n) { /* all transparent */
		best->c0 = 0, best->c1 = 0xFFFF, best->idx = 0xFFFFFFFFu, best->err = 0;
		return;
	}
	if (solid) {
		if (!tables_ready)
			make_tables();
		/* the 2/3 point of a matched pair (four colours) or the end point itself */
		cfit_t f;
		if (four) {
			uint16_t ca = pack565(match5[first[0]][0], match6[first[1]][0], match5[first[2]][0]);
			uint16_t cb = pack565(match5[first[0]][1], match6[first[1]][1], match5[first[2]][1]);
			f.c0 = ca, f.c1 = cb;
			if (ca < cb)
				f.c0 = cb, f.c1 = ca;
			if (f.c0 == f.c1) {
				/* index 0 only: equal end points read as three colours in DXT1 */
				f.idx = 0;
				int pal[4][3];
				palette(f.c0, f.c1, 1, pal);
				int dr = pal[0][0] - first[0], dg = pal[0][1] - first[1], db = pal[0][2] - first[2];
				f.err = (dr * dr + dg * dg + db * db) * n;
				for (int i = 0; i < 16; i++)
					if (!use[i])
						f.idx |= 3u << (2 * i);
			} else {
				choose(&f, rgba, use, four);
			}
		} else {
			float c[3] = {first[0], first[1], first[2]};
			set_ends(&f, c, c, four);
			choose(&f, rgba, use, four);
		}
		*best = f;
		return;
	}
	for (int c = 0; c < 3; c++)
		mean[c] /= n;
	/* covariance, then its main axis by power iteration */
	float cov[6] = {0};
	for (int i = 0; i < 16; i++) {
		if (!use[i])
			continue;
		float d[3] = {rgba[i * 4] - mean[0], rgba[i * 4 + 1] - mean[1], rgba[i * 4 + 2] - mean[2]};
		cov[0] += d[0] * d[0], cov[1] += d[0] * d[1], cov[2] += d[0] * d[2];
		cov[3] += d[1] * d[1], cov[4] += d[1] * d[2], cov[5] += d[2] * d[2];
	}
	float axis[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
	/* the bounding box's diagonal goes the wrong way for anti-correlated channels */
	if (cov[1] < 0)
		axis[1] = -axis[1];
	if (cov[2] < 0)
		axis[2] = -axis[2];
	for (int it = 0; it < 4; it++) {
		float x = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
		float y = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
		float z = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
		float m = x * x > y * y ? (x * x > z * z ? x : z) : (y * y > z * z ? y : z);
		if (m == 0)
			break;
		axis[0] = x / m, axis[1] = y / m, axis[2] = z / m;
	}
	float len2 = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
	if (len2 < 1e-12f)
		axis[0] = axis[1] = axis[2] = 1, len2 = 3;
	float tmin = 1e30f, tmax = -1e30f;
	for (int i = 0; i < 16; i++) {
		if (!use[i])
			continue;
		float t = ((rgba[i * 4] - mean[0]) * axis[0] + (rgba[i * 4 + 1] - mean[1]) * axis[1] +
		           (rgba[i * 4 + 2] - mean[2]) * axis[2]) / len2;
		if (t < tmin)
			tmin = t;
		if (t > tmax)
			tmax = t;
	}
	float a[3], b[3];
	for (int c = 0; c < 3; c++)
		a[c] = mean[c] + axis[c] * tmax, b[c] = mean[c] + axis[c] * tmin;
	set_ends(best, a, b, four);
	choose(best, rgba, use, four);
	for (int it = 0; it < 2 && best->err; it++) {
		cfit_t f;
		if (!refit(best, rgba, use, four, a, b))
			break;
		set_ends(&f, a, b, four);
		if (four && f.c0 == f.c1)
			break;
		choose(&f, rgba, use, four);
		if (f.err >= best->err)
			break;
		*best = f;
	}
	if (four && best->c0 == best->c1) {
		/* equal end points would read as three colours: index 0 everywhere */
		best->idx = 0;
	}
}

static void put_colour(uint8_t *out, const cfit_t *f)
{
	out[0] = (uint8_t)f->c0, out[1] = (uint8_t)(f->c0 >> 8);
	out[2] = (uint8_t)f->c1, out[3] = (uint8_t)(f->c1 >> 8);
	out[4] = (uint8_t)f->idx, out[5] = (uint8_t)(f->idx >> 8);
	out[6] = (uint8_t)(f->idx >> 16), out[7] = (uint8_t)(f->idx >> 24);
}

void DXT_EncodeBlock1(uint8_t out[8], const uint8_t rgba[64], int punch)
{
	uint8_t use[16];
	int transparent = 0;
	for (int i = 0; i < 16; i++) {
		use[i] = !punch || rgba[i * 4 + 3] >= 128;
		transparent |= !use[i];
	}
	cfit_t f;
	fit_colour(&f, rgba, use, !transparent);
	put_colour(out, &f);
}

/* --- the alpha block ---------------------------------------------------------------------- */

static void alpha_palette(int a0, int a1, int pal[8])
{
	pal[0] = a0, pal[1] = a1;
	if (a0 > a1) {
		for (int k = 1; k < 7; k++)
			pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
	} else {
		for (int k = 1; k < 5; k++)
			pal[k + 1] = ((5 - k) * a0 + k * a1) / 5;
		pal[6] = 0, pal[7] = 255;
	}
}

static int alpha_fit(const uint8_t *rgba, int a0, int a1, uint64_t *bits)
{
	int pal[8], err = 0;
	alpha_palette(a0, a1, pal);
	*bits = 0;
	for (int i = 0; i < 16; i++) {
		int v = rgba[i * 4 + 3], best = 1 << 30, bi = 0;
		for (int k = 0; k < 8; k++) {
			int e = (pal[k] - v) * (pal[k] - v);
			if (e < best)
				best = e, bi = k;
		}
		*bits |= (uint64_t)bi << (3 * i);
		err += best;
	}
	return err;
}

void DXT_EncodeBlock5(uint8_t out[16], const uint8_t rgba[64])
{
	int lo = 255, hi = 0, lo_in = 255, hi_in = 0;
	for (int i = 0; i < 16; i++) {
		int a = rgba[i * 4 + 3];
		lo = a < lo ? a : lo, hi = a > hi ? a : hi;
		if (a != 0 && a != 255)
			lo_in = a < lo_in ? a : lo_in, hi_in = a > hi_in ? a : hi_in;
	}
	uint64_t bits;
	int a0, a1;
	if (lo == hi) {
		a0 = a1 = lo, bits = 0;
	} else {
		/* eight levels between the extremes, or six inside them plus exact 0 and 255 */
		a0 = hi, a1 = lo;
		int err = alpha_fit(rgba, a0, a1, &bits);
		if (err && (lo == 0 || hi == 255)) {
			uint64_t b6;
			int s0 = lo_in <= hi_in ? lo_in : 0, s1 = lo_in <= hi_in ? hi_in : 0;
			if (alpha_fit(rgba, s0, s1, &b6) < err)
				a0 = s0, a1 = s1, bits = b6;
		}
	}
	out[0] = (uint8_t)a0, out[1] = (uint8_t)a1;
	for (int k = 0; k < 6; k++)
		out[2 + k] = (uint8_t)(bits >> (8 * k));
	/* colour: always four colours in DXT5, fitted to the texels that show */
	uint8_t use[16];
	int shown = 0;
	for (int i = 0; i < 16; i++)
		shown += use[i] = rgba[i * 4 + 3] != 0;
	if (!shown)
		memset(use, 1, sizeof(use));
	cfit_t f;
	fit_colour(&f, rgba, use, 1);
	put_colour(out + 8, &f);
}

/* --- decoding ------------------------------------------------------------------------------- */

static void decode_colour(const uint8_t *in, uint8_t rgba[64], int dxt1)
{
	uint16_t c0 = (uint16_t)(in[0] | in[1] << 8), c1 = (uint16_t)(in[2] | in[3] << 8);
	uint32_t idx = (uint32_t)in[4] | (uint32_t)in[5] << 8 | (uint32_t)in[6] << 16 | (uint32_t)in[7] << 24;
	int pal[4][3], four = !dxt1 || c0 > c1;
	palette(c0, c1, four, pal);
	for (int i = 0; i < 16; i++) {
		int k = (idx >> (2 * i)) & 3;
		rgba[i * 4] = (uint8_t)pal[k][0], rgba[i * 4 + 1] = (uint8_t)pal[k][1], rgba[i * 4 + 2] = (uint8_t)pal[k][2];
		rgba[i * 4 + 3] = !four && k == 3 ? 0 : 255;
	}
}

void DXT_DecodeBlock1(const uint8_t in[8], uint8_t rgba[64]) { decode_colour(in, rgba, 1); }

void DXT_DecodeBlock5(const uint8_t in[16], uint8_t rgba[64])
{
	decode_colour(in + 8, rgba, 0);
	int pal[8];
	alpha_palette(in[0], in[1], pal);
	uint64_t bits = 0;
	for (int k = 0; k < 6; k++)
		bits |= (uint64_t)in[2 + k] << (8 * k);
	for (int i = 0; i < 16; i++)
		rgba[i * 4 + 3] = (uint8_t)pal[(bits >> (3 * i)) & 7];
}

/* --- images ---------------------------------------------------------------------------------- */

size_t DXT_LevelBytes(int w, int h, int blockbytes)
{
	int bw = w > 4 ? (w + 3) / 4 : 1, bh = h > 4 ? (h + 3) / 4 : 1;
	return (size_t)bw * bh * blockbytes;
}

void DXT_GatherBlock(uint8_t block[64], const uint8_t *rgba, int w, int h, int bx, int by)
{
	for (int y = 0; y < 4; y++) {
		int sy = by * 4 + y < h ? by * 4 + y : h - 1;
		for (int x = 0; x < 4; x++) {
			int sx = bx * 4 + x < w ? bx * 4 + x : w - 1;
			memcpy(block + (y * 4 + x) * 4, rgba + ((size_t)sy * w + sx) * 4, 4);
		}
	}
}

void DXT_EncodeImage(uint8_t *out, const uint8_t *rgba, int w, int h, int blockbytes, int punch)
{
	int bw = (w + 3) / 4, bh = (h + 3) / 4;
	uint8_t block[64];
	for (int by = 0; by < bh; by++)
		for (int bx = 0; bx < bw; bx++) {
			DXT_GatherBlock(block, rgba, w, h, bx, by);
			if (blockbytes == DXT1_BLOCK_BYTES)
				DXT_EncodeBlock1(out, block, punch);
			else
				DXT_EncodeBlock5(out, block);
			out += blockbytes;
		}
}

void DXT_DecodeImage(const uint8_t *in, uint8_t *rgba, int w, int h, int blockbytes)
{
	int bw = (w + 3) / 4, bh = (h + 3) / 4;
	uint8_t block[64];
	for (int by = 0; by < bh; by++)
		for (int bx = 0; bx < bw; bx++) {
			if (blockbytes == DXT1_BLOCK_BYTES)
				DXT_DecodeBlock1(in, block);
			else
				DXT_DecodeBlock5(in, block);
			in += blockbytes;
			for (int y = 0; y < 4 && by * 4 + y < h; y++)
				for (int x = 0; x < 4 && bx * 4 + x < w; x++)
					memcpy(rgba + ((size_t)(by * 4 + y) * w + bx * 4 + x) * 4, block + (y * 4 + x) * 4, 4);
		}
}
