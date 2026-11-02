/* Bulwark engine - bmfdump: what the bitmap font loader makes of a font
 * file (BDF, PCF, .pcf.gz, or a pixel-grid TrueType font).
 *   bmfdump <file> [text]   prints the metrics, then the text as pixels
 * ISC licence, see LICENSE. */
#include "../src/common/bmfont.h"
#include "../src/common/common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(int argc, char **argv)
{
	size_t len;
	uint8_t *data;
	bmfont_t *f;
	const char *text = argc > 2 ? argv[2] : "Hello, World! 0123";
	int upem = 0, width = 0, cell;
	float grid;
	char *canvas;
	if (argc < 2) {
		fprintf(stderr, "usage: bmfdump <font> [text]\n");
		return 2;
	}
	if (!(data = read_file(argv[1], &len))) {
		fprintf(stderr, "can't read %s\n", argv[1]);
		return 1;
	}
	grid = BMF_PixelGrid(data, len, &upem);
	f = BMF_Load(data, len, true);
	if (!f) {
		printf("%s: not a bitmap or pixel font (grid %g, upem %d)\n", argv[1], grid, upem);
		return 1;
	}
	printf("%s: kind %s family \"%s\" weight %d%s pixelsize %d ascent %d descent %d glyphs %d", argv[1],
	       f->kind == BMF_BDF ? "bdf" : f->kind == BMF_PCF ? "pcf" : "ttf", f->family, f->weight,
	       f->italic ? " italic" : "", f->pixelsize, f->ascent, f->descent, f->numglyphs);
	if (f->kind == BMF_TTF)
		printf(" ppem %g of %d units", grid, upem);
	printf("\n");
	for (const char *p = text; *p; p++) {
		const bmglyph_t *g = BMF_Glyph(f, (unsigned char)*p);
		width += g ? g->adv : 0;
	}
	cell = f->ascent + f->descent;
	canvas = malloc((size_t)(width + 1) * cell);
	memset(canvas, '.', (size_t)(width + 1) * cell);
	for (int pen = 0, i = 0; text[i]; i++) {
		const bmglyph_t *g = BMF_Glyph(f, (unsigned char)text[i]);
		if (!g)
			continue;
		for (int y = 0; y < g->h; y++)
			for (int x = 0; x < g->w; x++) {
				int cx = pen + g->xoff + x, cy = f->ascent - (g->yoff + g->h) + y;
				if (cx >= 0 && cx < width && cy >= 0 && cy < cell && f->pixels[g->bits + (size_t)y * g->w + x])
					canvas[cy * (width + 1) + cx] = '#';
			}
		pen += g->adv;
	}
	for (int y = 0; y < cell; y++) {
		canvas[y * (width + 1) + width] = 0;
		printf("%s\n", canvas + y * (width + 1));
	}
	BMF_Free(f);
	free(canvas);
	free(data);
	return 0;
}
