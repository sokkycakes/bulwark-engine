/* Bulwark engine - host tests for the Xbox texture compressor (dxt_xbox.c).
 * ISC licence, see LICENSE.
 *
 *   src/platform/xbox/tests/run_dxttest.sh [dump-dir]
 *
 * Checks the block layout against hand-worked blocks, the decoder against
 * an independent reading of the format, and the encoder's error on solid
 * colours, gradients and alpha. Given a folder of texture dumps (.tex: four
 * ints w, h, bytes per texel, GL internal format, then the pixels; a .mip
 * beside one marks a mipmapped texture) it also reports the error and the
 * time per kind of texture, against the 16-bit storage it replaces. */
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../dxt_xbox.h"

static int failures;
#define CHECK(cond, ...)                                                                                     \
	do {                                                                                                     \
		if (!(cond)) {                                                                                       \
			failures++;                                                                                      \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                      \
			printf(__VA_ARGS__);                                                                             \
			printf("\n");                                                                                    \
		}                                                                                                    \
	} while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
	rng = rng * 1664525u + 1013904223u;
	return rng >> 8;
}

/* --- a second reading of the format, written from its description ----------------- */

static void ref_color(uint16_t c, double out[3])
{
	/* 5 and 6 bits widened by repeating the top bits */
	int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
	out[0] = r * 8 + r / 4, out[1] = g * 4 + g / 16, out[2] = b * 8 + b / 4;
}

static void ref_decode(const uint8_t *blk, int dxt5, uint8_t px[16][4])
{
	const uint8_t *cb = dxt5 ? blk + 8 : blk;
	uint16_t c0 = (uint16_t)(cb[0] + 256 * cb[1]), c1 = (uint16_t)(cb[2] + 256 * cb[3]);
	double p[4][3];
	int alpha[4] = {255, 255, 255, 255};
	ref_color(c0, p[0]);
	ref_color(c1, p[1]);
	for (int k = 0; k < 3; k++) {
		if (dxt5 || c0 > c1) {
			p[2][k] = floor((2 * p[0][k] + p[1][k]) / 3);
			p[3][k] = floor((p[0][k] + 2 * p[1][k]) / 3);
		} else {
			p[2][k] = floor((p[0][k] + p[1][k]) / 2);
			p[3][k] = 0;
			alpha[3] = 0;
		}
	}
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++) {
			int code = (cb[4 + y] >> (2 * x)) & 3; /* one byte per row, first texel lowest */
			for (int k = 0; k < 3; k++)
				px[y * 4 + x][k] = (uint8_t)p[code][k];
			px[y * 4 + x][3] = (uint8_t)alpha[code];
		}
	if (!dxt5)
		return;
	int a0 = blk[0], a1 = blk[1];
	double ap[8];
	ap[0] = a0, ap[1] = a1;
	for (int k = 2; k < 8; k++) {
		if (a0 > a1)
			ap[k] = floor(((8 - k) * a0 + (k - 1) * a1) / 7.0);
		else if (k < 6)
			ap[k] = floor(((6 - k) * a0 + (k - 1) * a1) / 5.0);
		else
			ap[k] = k == 6 ? 0 : 255;
	}
	/* 48 bits of 3-bit codes after the end points */
	for (int i = 0; i < 16; i++) {
		int bit = 16 + 3 * i, code = 0;
		for (int j = 0; j < 3; j++)
			code |= ((blk[(bit + j) / 8] >> ((bit + j) % 8)) & 1) << j;
		px[i][3] = (uint8_t)ap[code];
	}
}

/* --- tests ------------------------------------------------------------------------------- */

static void test_layout(void)
{
	/* left half red, right half blue: the end points are exact, red first */
	uint8_t rgba[64], out[8];
	for (int i = 0; i < 16; i++) {
		int blue = (i & 3) >= 2;
		rgba[i * 4] = blue ? 0 : 255, rgba[i * 4 + 1] = 0, rgba[i * 4 + 2] = blue ? 255 : 0, rgba[i * 4 + 3] = 255;
	}
	DXT_EncodeBlock1(out, rgba, 0);
	static const uint8_t want[8] = {0x00, 0xF8, 0x1F, 0x00, 0x50, 0x50, 0x50, 0x50};
	CHECK(!memcmp(out, want, 8), "red/blue block: %02x %02x %02x %02x %02x %02x %02x %02x", out[0], out[1], out[2],
	      out[3], out[4], out[5], out[6], out[7]);
	/* a transparent top row: three-colour mode (c0 <= c1), code 3 there */
	for (int i = 0; i < 4; i++)
		rgba[i * 4 + 3] = 0;
	DXT_EncodeBlock1(out, rgba, 1);
	uint16_t c0 = (uint16_t)(out[0] | out[1] << 8), c1 = (uint16_t)(out[2] | out[3] << 8);
	CHECK(c0 <= c1, "punch-through block should use three colours");
	CHECK(out[4] == 0xFF, "transparent row should be code 3, got %02x", out[4]);
	uint8_t dec[64];
	DXT_DecodeBlock1(out, dec);
	for (int i = 0; i < 16; i++)
		CHECK(dec[i * 4 + 3] == (i < 4 ? 0 : 255), "texel %d alpha %d", i, dec[i * 4 + 3]);
	/* DXT5 alpha: 0..255 in a ramp uses eight levels, a0 > a1 */
	uint8_t out5[16];
	for (int i = 0; i < 16; i++)
		rgba[i * 4 + 3] = (uint8_t)(i * 17);
	DXT_EncodeBlock5(out5, rgba);
	CHECK(out5[0] == 255 && out5[1] == 0, "alpha end points %d %d", out5[0], out5[1]);
}

static void test_decoder(void)
{
	/* random blocks, both modes, against the reference reading */
	for (int n = 0; n < 20000; n++) {
		uint8_t blk[16], a[64], b[16][4];
		for (int i = 0; i < 16; i++)
			blk[i] = (uint8_t)rnd();
		if (n & 1) {
			DXT_DecodeBlock5(blk, a);
			ref_decode(blk, 1, b);
		} else {
			DXT_DecodeBlock1(blk, a);
			ref_decode(blk, 0, b);
		}
		if (memcmp(a, b, 64)) {
			CHECK(0, "decoder disagrees with the reference on block %d (dxt%d)", n, n & 1 ? 5 : 1);
			return;
		}
	}
}

static double block_mse(const uint8_t *a, const uint8_t *b, int alpha)
{
	double e = 0;
	for (int i = 0; i < 16; i++)
		for (int c = 0; c < (alpha ? 4 : 3); c++)
			e += (a[i * 4 + c] - b[i * 4 + c]) * (double)(a[i * 4 + c] - b[i * 4 + c]);
	return e / (16 * (alpha ? 4 : 3));
}

static void test_solid(void)
{
	/* every grey and some colours: within a step of the 2/3 points */
	int worst = 0;
	for (int v = 0; v < 256; v++)
		for (int k = 0; k < 4; k++) {
			uint8_t rgba[64], out[8], dec[64];
			int r = v, g = k == 1 ? 255 - v : v, b = k == 2 ? v / 2 : k == 3 ? (v * 7) & 255 : v;
			for (int i = 0; i < 16; i++)
				rgba[i * 4] = (uint8_t)r, rgba[i * 4 + 1] = (uint8_t)g, rgba[i * 4 + 2] = (uint8_t)b, rgba[i * 4 + 3] = 255;
			DXT_EncodeBlock1(out, rgba, 0);
			DXT_DecodeBlock1(out, dec);
			for (int i = 0; i < 16; i++)
				for (int c = 0; c < 3; c++) {
					int d = abs(dec[i * 4 + c] - rgba[i * 4 + c]);
					worst = d > worst ? d : worst;
				}
			CHECK(dec[3] == 255, "solid block decoded transparent (%d %d %d)", r, g, b);
		}
	CHECK(worst <= 2, "solid colours off by up to %d", worst);
	printf("solid colours: worst error %d\n", worst);
}

static void test_gradients(void)
{
	/* smooth blocks (what lightmaps and most textures are): PSNR well above 565's */
	double e1 = 0, e565 = 0;
	for (int n = 0; n < 5000; n++) {
		uint8_t rgba[64], out[8], dec[64];
		double base[3], dx[3], dy[3];
		for (int c = 0; c < 3; c++)
			base[c] = rnd() % 200, dx[c] = (int)(rnd() % 13) - 6, dy[c] = (int)(rnd() % 13) - 6;
		for (int y = 0; y < 4; y++)
			for (int x = 0; x < 4; x++)
				for (int c = 0; c < 4; c++) {
					double v = c == 3 ? 255 : base[c] + dx[c] * x + dy[c] * y;
					rgba[(y * 4 + x) * 4 + c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
				}
		DXT_EncodeBlock1(out, rgba, 0);
		DXT_DecodeBlock1(out, dec);
		e1 += block_mse(rgba, dec, 0);
		for (int i = 0; i < 16; i++) {
			int r = rgba[i * 4] >> 3, g = rgba[i * 4 + 1] >> 2, b = rgba[i * 4 + 2] >> 3;
			dec[i * 4] = (uint8_t)(r << 3 | r >> 2), dec[i * 4 + 1] = (uint8_t)(g << 2 | g >> 4),
			dec[i * 4 + 2] = (uint8_t)(b << 3 | b >> 2);
		}
		e565 += block_mse(rgba, dec, 0);
	}
	double p1 = 10 * log10(255.0 * 255 / (e1 / 5000)), p565 = 10 * log10(255.0 * 255 / (e565 / 5000));
	printf("gradients: DXT1 %.1f dB, 565 %.1f dB\n", p1, p565);
	CHECK(p1 > 36, "gradient PSNR %.1f dB", p1);
}

static void test_alpha(void)
{
	/* smooth alpha, and blocks mixing 0 and 255 with soft values */
	double e = 0;
	int exact = 1;
	for (int n = 0; n < 5000; n++) {
		uint8_t rgba[64], out[16], dec[64];
		int mixed = n & 1, a0 = rnd() % 256, da = (int)(rnd() % 31) - 15;
		for (int i = 0; i < 16; i++) {
			int a = a0 + da * (i % 4) + da * (i / 4);
			if (mixed && i % 5 == 0)
				a = i % 2 ? 255 : 0;
			rgba[i * 4] = (uint8_t)rnd(), rgba[i * 4 + 1] = 0, rgba[i * 4 + 2] = 0;
			rgba[i * 4 + 3] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
		}
		DXT_EncodeBlock5(out, rgba);
		DXT_DecodeBlock5(out, dec);
		for (int i = 0; i < 16; i++) {
			int d = dec[i * 4 + 3] - rgba[i * 4 + 3];
			e += d * d;
			if ((rgba[i * 4 + 3] == 0 || rgba[i * 4 + 3] == 255) && d)
				exact = 0;
		}
	}
	double p = 10 * log10(255.0 * 255 / (e / (5000 * 16)));
	printf("alpha: DXT5 %.1f dB, 0 and 255 exact: %s\n", p, exact ? "yes" : "no");
	CHECK(p > 40, "alpha PSNR %.1f dB", p);
}

static void test_image_padding(void)
{
	/* a 2x2 and a 1x1 level take one block, padded from the edge */
	uint8_t img[16] = {10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255};
	uint8_t out[8], dec[16];
	CHECK(DXT_LevelBytes(2, 2, 8) == 8 && DXT_LevelBytes(1, 1, 16) == 16 && DXT_LevelBytes(512, 256, 8) == 128 * 64 * 8,
	      "level sizes");
	DXT_EncodeImage(out, img, 2, 2, 8, 0);
	DXT_DecodeImage(out, dec, 2, 2, 8);
	for (int i = 0; i < 16; i++)
		CHECK(abs(dec[i] - img[i]) <= 12, "2x2 texel byte %d: %d vs %d", i, dec[i], img[i]);
}

/* --- real textures ---------------------------------------------------------------------- */

static double psnr(double se, double n) { return se > 0 ? 10 * log10(255.0 * 255 * n / se) : 99; }

static void survey(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d) {
		printf("no folder %s\n", dir);
		return;
	}
	struct dirent *de;
	/* kinds: 0 mipmapped opaque, 1 mipmapped with alpha, 2 lightmap (RGB), 3 other */
	double nlit = 0, se[4] = {0}, se16[4] = {0}, n[4] = {0}, secs[4] = {0}, lmse_scaled = 0, lmse565 = 0, lmse_hi = 0;
	int count[4] = {0};
	size_t bytes16[4] = {0}, bytesdxt[4] = {0};
	while ((de = readdir(d))) {
		size_t len = strlen(de->d_name);
		if (len < 5 || strcmp(de->d_name + len - 4, ".tex"))
			continue;
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		FILE *f = fopen(path, "rb");
		int hdr[4];
		if (!f || fread(hdr, sizeof(hdr), 1, f) != 1) {
			if (f)
				fclose(f);
			continue;
		}
		int w = hdr[0], h = hdr[1], bpp = hdr[2];
		uint8_t *px = malloc((size_t)w * h * bpp), *rgba = malloc((size_t)w * h * 4), *dec = malloc((size_t)w * h * 4);
		uint8_t *blocks = malloc(DXT_LevelBytes(w, h, 16));
		if (fread(px, (size_t)w * h * bpp, 1, f) != 1) {
			fclose(f);
			free(px), free(rgba), free(dec), free(blocks);
			continue;
		}
		fclose(f);
		for (int i = 0; i < w * h; i++) {
			memcpy(rgba + i * 4, px + i * bpp, 3);
			rgba[i * 4 + 3] = bpp == 4 ? px[i * 4 + 3] : 255;
		}
		path[strlen(path) - 4] = 0;
		strcat(path, ".mip");
		struct stat st;
		int mip = !stat(path, &st);
		int alpha = 0, partial = 0;
		for (int i = 0; i < w * h; i++) {
			alpha |= rgba[i * 4 + 3] != 255;
			partial |= rgba[i * 4 + 3] != 255 && rgba[i * 4 + 3] != 0;
		}
		int kind = bpp == 3 ? 2 : mip ? (alpha ? 1 : 0) : 3;
		int bb = alpha ? 16 : 8; /* any alpha: DXT5 (four colours, alpha exact at 0 and 255) */
		clock_t t0 = clock();
		DXT_EncodeImage(blocks, rgba, w, h, bb, 0);
		secs[kind] += (double)(clock() - t0) / CLOCKS_PER_SEC;
		DXT_DecodeImage(blocks, dec, w, h, bb);
		count[kind]++;
		bytes16[kind] += (size_t)w * h * (kind == 2 || kind == 3 ? 4 : 2);
		bytesdxt[kind] += DXT_LevelBytes(w, h, bb);
		for (int i = 0; i < w * h; i++) {
			if (!rgba[i * 4 + 3])
				continue; /* colour under a transparent texel doesn't show */
			for (int c = 0; c < 3; c++) {
				double e = dec[i * 4 + c] - rgba[i * 4 + c];
				se[kind] += e * e;
				/* what it replaces: 565, 4444 for soft alpha, and 8888 for the lightmap */
				int v = rgba[i * 4 + c], q;
				if (partial)
					q = (v >> 4) * 17;
				else if (c == 1)
					q = (v >> 2) << 2 | v >> 6;
				else
					q = (v >> 3) << 3 | v >> 5;
				se16[kind] += (q - v) * (double)(q - v);
				if (kind == 2) {
					/* the world shader doubles the lightmap: errors double too */
					lmse_scaled += 4 * e * e;
					lmse565 += 4 * (q - v) * (double)(q - v);
					if (v >= 8)
						lmse_hi += 4 * e * e, nlit++;
				}
			}
			n[kind] += 3;
		}
		free(px), free(rgba), free(dec), free(blocks);
	}
	closedir(d);
	static const char *names[4] = {"mipmapped opaque", "mipmapped alpha", "lightmap", "other 2D"};
	for (int k = 0; k < 4; k++)
		if (count[k])
			printf("%-17s %3d textures: DXT %.1f dB (16-bit %.1f dB), level 0 %zu KB -> %zu KB, %.0f ms to encode\n",
			       names[k], count[k], psnr(se[k], n[k]), psnr(se16[k], n[k]), bytes16[k] >> 10, bytesdxt[k] >> 10,
			       secs[k] * 1000);
	if (count[2])
		printf("lightmap as lit (x2): DXT1 %.1f dB, 565 %.1f dB\n", psnr(lmse_scaled, n[2]), psnr(lmse565, n[2]));
	if (nlit)
		printf("lightmap texels lit above 8/255, as lit: DXT1 %.1f dB\n", psnr(lmse_hi, nlit));
}

int main(int argc, char **argv)
{
	test_layout();
	test_decoder();
	test_solid();
	test_gradients();
	test_alpha();
	test_image_padding();
	if (argc > 1)
		survey(argv[1]);
	printf(failures ? "dxttest: %d failures\n" : "dxttest: all passed\n", failures);
	return failures != 0;
}
