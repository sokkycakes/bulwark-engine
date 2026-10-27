/*
 * Unit tests for src/client/gdifont.c. The size rule and the gasp decision
 * are checked on synthetic tables; glyph layout is checked on a real font
 * when one is available (GDIFONT_TEST_FONT, or a common system font).
 */
#include "../../src/client/gdifont.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void test_size_rule(void)
{
	/* no VDMX: largest ppem with round(p * (winA + winD) / upem) <= tall */
	gdifont_sizeinfo_t si = { 2048, 1854, 434, 0, NULL, NULL, NULL };
	int asc, desc, p;
	p = gdifont_select_size(&si, 16, &asc, &desc);
	CHECK(p == 14);              /* 14 -> 15.64 -> 16; 15 -> 16.76 -> 17 */
	CHECK(asc == 13);            /* round(1854 * 16 / 2288) = round(12.97) */
	CHECK(asc + desc == 16);

	/* VDMX: largest fitting height wins, smallest ppem among ties */
	{
		static const unsigned short pp[] = { 8, 9, 10, 11, 12 };
		static const short ymax[] = { 8, 9, 9, 11, 12 }, ymin[] = { -2, -2, -2, -2, -3 };
		gdifont_sizeinfo_t v = { 2048, 2059, 430, 5, pp, ymax, ymin };
		p = gdifont_select_size(&v, 12, &asc, &desc);
		CHECK(p == 9 && asc == 9 && desc == 2); /* height 11 at ppem 9 and 10 */
		p = gdifont_select_size(&v, 15, &asc, &desc);
		CHECK(p == 12 && asc + desc == 15);
		p = gdifont_select_size(&v, 9, &asc, &desc);
		CHECK(p <= 7 && asc + desc == 9);       /* nothing fits: below the table */
	}
}

static void test_gasp(void)
{
	/* ranges <=8: 0x0A, <=16: 0x05, <=17: 0x07, <=65535: 0x0F */
	static const unsigned char gasp[] = { 0, 1, 0, 4, 0, 8, 0, 0x0A, 0, 16, 0, 0x05, 0, 17, 0, 0x07, 0xFF, 0xFF, 0, 0x0F };
	CHECK(gdifont_gasp_gray(gasp, sizeof(gasp), 8) == 1);
	CHECK(gdifont_gasp_gray(gasp, sizeof(gasp), 9) == 0);
	CHECK(gdifont_gasp_gray(gasp, sizeof(gasp), 16) == 0);
	CHECK(gdifont_gasp_gray(gasp, sizeof(gasp), 17) == 1);
	CHECK(gdifont_gasp_gray(gasp, sizeof(gasp), 40) == 1);
	CHECK(gdifont_gasp_gray(NULL, 0, 12) == 1);
}

static void test_font(const char *path)
{
	const char *err = NULL;
	gdifont_t *f = gdifont_create_file(path, 16, 400, 0, GDIFONT_NONANTIALIASED, &err);
	const gdifont_metrics_t *tm;
	gdifont_glyph_t g;
	gdifont_abc_t abc;
	unsigned text[] = { 'H', 'i', '!' };
	unsigned char *buf;
	int x, y, w, ink = 0;

	if (!f) {
		fprintf(stderr, "gdifonttest: %s: %s\n", path, err);
		failures++;
		return;
	}
	tm = gdifont_metrics(f);
	CHECK(tm->height > 0 && tm->height <= 16);
	CHECK(tm->ascent + tm->descent == tm->height);
	CHECK(tm->internal_leading == tm->height - tm->ppem);
	CHECK(!gdifont_text_is_gray(f));

	CHECK(gdifont_glyph_mono(f, 'H', &g));
	CHECK(g.bbx > 0 && g.bby > 0 && g.pitch % 4 == 0 && g.pitch * 8 >= g.bbx);
	CHECK(g.size == g.pitch * g.bby);
	CHECK(gdifont_abc(f, 'H', &abc));
	CHECK(abc.a == g.ox && abc.b == g.bbx && abc.a + abc.b + abc.c == g.incx);
	gdifont_glyph_release(&g);

	CHECK(gdifont_glyph_gray8(f, 'H', &g));
	CHECK(g.pitch % 4 == 0 && g.pitch >= g.bbx);
	for (x = 0; x < g.size; x++)
		CHECK(g.data[x] <= 64);
	gdifont_glyph_release(&g);

	w = gdifont_text_width(f, text, 3);
	buf = calloc(64, 32);
	CHECK(gdifont_text_out(f, 2, 2, text, 3, buf, 64, 32, 64) == w);
	for (y = 0; y < 32; y++)
		for (x = 0; x < 64; x++) {
			CHECK(buf[y * 64 + x] == 0 || buf[y * 64 + x] == 255);
			ink += buf[y * 64 + x] != 0;
		}
	CHECK(ink > 0);
	free(buf);
	gdifont_free(f);

	/* greyscale glyphs at quality 1 when the font's gasp allows it */
	f = gdifont_create_file(path, 40, 0, 0, GDIFONT_ANTIALIASED, &err);
	CHECK(f && gdifont_metrics(f)->weight == 400);
	gdifont_free(f);
	CHECK(gdifont_create_file(path, 0, 400, 0, 0, &err) == NULL);
}

int main(void)
{
	static const char *candidates[] = {
		"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"C:/Windows/Fonts/tahoma.ttf",
		"/Library/Fonts/Arial.ttf",
	};
	const char *font = getenv("GDIFONT_TEST_FONT");
	size_t i;
	test_size_rule();
	test_gasp();
	for (i = 0; !font && i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		FILE *f = fopen(candidates[i], "rb");
		if (f) { fclose(f); font = candidates[i]; }
	}
	if (font)
		test_font(font);
	else
		printf("gdifonttest: no TrueType font found, skipping glyph tests\n");
	if (failures)
		return 1;
	printf("gdifonttest: ok\n");
	return 0;
}
