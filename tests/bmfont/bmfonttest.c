/* Bulwark engine - bitmap font loader tests (src/common/bmfont.c).
 *   bmfonttest <test.bdf> [pcf files...]
 * Checks the BDF fixture's metrics and pixels, then that every PCF file
 * (the same font written in other byte orders, bit orders, paddings and
 * metric formats by mkpcf.py, some gzipped) loads to the same glyphs.
 * ISC licence, see LICENSE. */
#include "../../src/common/bmfont.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                                                              \
	do {                                                                                                          \
		if (!(cond)) {                                                                                        \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                   \
			printf(__VA_ARGS__);                                                                          \
			printf("\n");                                                                                 \
			failures++;                                                                                   \
		}                                                                                                     \
	} while (0)

static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long n;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n + 1);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(buf);
		return NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return buf;
}

static bmfont_t *load(const char *path)
{
	size_t len;
	uint8_t *data = read_file(path, &len);
	bmfont_t *f;
	if (!data) {
		printf("FAIL can't read %s\n", path);
		failures++;
		return NULL;
	}
	f = BMF_Load(data, len, false);
	free(data);
	CHECK(f, "%s didn't load", path);
	return f;
}

/* a glyph's pixels as rows of # and . */
static void rows(const bmfont_t *f, const bmglyph_t *g, char *out, size_t size)
{
	size_t n = 0;
	for (int y = 0; y < g->h; y++) {
		for (int x = 0; x < g->w && n + 2 < size; x++)
			out[n++] = f->pixels[g->bits + (size_t)y * g->w + x] ? '#' : '.';
		if (n + 2 < size)
			out[n++] = '/';
	}
	out[n] = 0;
}

static void check_fixture(const bmfont_t *f, const char *what)
{
	const bmglyph_t *g;
	char r[256];
	CHECK(!strcmp(f->family, "Bulwark Test"), "%s: family \"%s\"", what, f->family);
	CHECK(f->weight == 700, "%s: weight %d", what, f->weight);
	CHECK(f->italic, "%s: not italic", what);
	CHECK(f->ascent == 7 && f->descent == 2, "%s: ascent %d descent %d", what, f->ascent, f->descent);
	CHECK(f->pixelsize == 9, "%s: pixelsize %d", what, f->pixelsize);
	CHECK(f->numglyphs == 5, "%s: %d glyphs", what, f->numglyphs);
	g = BMF_Glyph(f, 'A');
	CHECK(g && g->w == 5 && g->h == 7 && g->xoff == 0 && g->yoff == 0 && g->adv == 6, "%s: A metrics", what);
	if (g) {
		rows(f, g, r, sizeof(r));
		CHECK(!strcmp(r, "..#../.#.#./#...#/#...#/#####/#...#/#...#/"), "%s: A is %s", what, r);
	}
	g = BMF_Glyph(f, 'g');
	CHECK(g && g->w == 4 && g->h == 6 && g->yoff == -2 && g->adv == 5, "%s: g metrics", what);
	if (g) {
		rows(f, g, r, sizeof(r));
		CHECK(!strcmp(r, ".###/#..#/#..#/.###/...#/###./"), "%s: g is %s", what, r);
	}
	g = BMF_Glyph(f, 0x263a);
	CHECK(g && g->w == 9 && g->h == 3 && g->yoff == 2 && g->adv == 10, "%s: U+263A metrics", what);
	if (g) {
		rows(f, g, r, sizeof(r));
		CHECK(!strcmp(r, "#.......#/.#.....#./..#####../"), "%s: U+263A is %s", what, r);
	}
	g = BMF_Glyph(f, ' ');
	CHECK(g && g->w == 0 && g->adv == 4, "%s: space", what);
	/* a missing character draws the default glyph, '?' */
	g = BMF_Glyph(f, 'Z');
	CHECK(g && g->cp == '?', "%s: default glyph", what);
}

int main(int argc, char **argv)
{
	bmfont_t *bdf;
	char name[64];
	int weight;
	bool italic;
	size_t len;
	uint8_t *data;
	if (argc < 2) {
		fprintf(stderr, "usage: bmfonttest <test.bdf> [pcf files...]\n");
		return 2;
	}
	if (!(bdf = load(argv[1])))
		return 1;
	check_fixture(bdf, "bdf");
	data = read_file(argv[1], &len);
	CHECK(data && BMF_Describe(data, len, name, sizeof(name), &weight, &italic) && !strcmp(name, "Bulwark Test") &&
	          weight == 700 && italic,
	      "describe");
	CHECK(BMF_PixelGrid(data, len, NULL) == 0, "a BDF file isn't a pixel TrueType font");
	free(data);
	CHECK(BMF_IsBitmapName("fonts/a.BDF") && BMF_IsBitmapName("a.pcf") && BMF_IsBitmapName("x/a.pcf.gz") &&
	          !BMF_IsBitmapName("a.ttf") && !BMF_IsBitmapName("a.gz"),
	      "names");
	for (int i = 2; i < argc; i++) {
		bmfont_t *pcf = load(argv[i]);
		if (pcf) {
			check_fixture(pcf, argv[i]);
			BMF_Free(pcf);
		}
	}
	/* rubbish and truncated files are refused, not read past their end */
	{
		static const uint8_t junk[] = {1, 'f', 'c', 'p', 200, 0, 0, 0, 1, 2, 3};
		CHECK(!BMF_Load(junk, sizeof(junk), true), "junk PCF");
		CHECK(!BMF_Load("STARTFONT 2.1\n", 14, true), "empty BDF");
		CHECK(!BMF_Load("hello", 5, true), "not a font");
	}
	BMF_Free(bdf);
	printf("%s: %d failures\n", argv[0], failures);
	return failures ? 1 : 0;
}
