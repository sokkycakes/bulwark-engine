/* Bulwark engine - bitmap fonts: X11 BDF and PCF files (PCF also gzipped),
 * and TrueType fonts drawn on a pixel grid ("pixel TTFs").
 *
 * Every glyph comes out as a 1-bit image (bytes of 0 or 255) with X-style
 * metrics, at the font's own pixel size; the renderer and the UI scale it by
 * whole numbers so the pixels stay square and sharp. Written from the public
 * BDF 2.1 specification and the X.Org PCF format notes; see
 * docs/spec/bitmap-fonts.md. ISC licence, see LICENSE. */
#ifndef BW_BMFONT_H
#define BW_BMFONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	int cp;          /* Unicode code point */
	int w, h;        /* ink box */
	int xoff, yoff;  /* box offset from the pen: x right, y = bottom edge above the baseline */
	int adv;         /* pen advance */
	size_t bits;     /* offset of w*h bytes in bmfont_t.pixels */
} bmglyph_t;

typedef enum { BMF_BDF = 1, BMF_PCF, BMF_TTF } bmfkind_t;

typedef struct {
	bmfkind_t kind;
	char family[64];
	int weight;           /* CSS-style: 400 regular, 700 bold */
	bool italic;
	int ascent, descent;  /* pixels above and below the baseline; the cell is ascent + descent tall */
	int pixelsize;        /* the nominal size the file declares (PIXEL_SIZE, or ppem for a TTF) */
	int numglyphs;
	bmglyph_t *glyphs;    /* sorted by code point */
	uint8_t *pixels;
	int defaultglyph;     /* index drawn for missing characters, or -1 */
} bmfont_t;

/* .bdf, .pcf or .pcf.gz by name */
bool BMF_IsBitmapName(const char *path);
/* A BDF, PCF or gzipped PCF file, or a TrueType font whose outlines all lie
 * on a pixel grid (with pixelttf). NULL when it is none of these. */
bmfont_t *BMF_Load(const void *data, size_t len, bool pixelttf);
void BMF_Free(bmfont_t *f);
/* the glyph for a code point, the default glyph, or NULL */
const bmglyph_t *BMF_Glyph(const bmfont_t *f, int cp);

/* The size a pixel TrueType font was drawn at, in pixels per em (its
 * outlines are only horizontal and vertical lines on that pixel grid), or 0
 * when the font is not one. *upem gets the font's units per em. */
float BMF_PixelGrid(const void *data, size_t len, int *upem);
/* family name, weight and italic of a BDF/PCF/TTF without loading glyphs */
bool BMF_Describe(const void *data, size_t len, char *family, size_t familysize, int *weight, bool *italic);

/* gzip data (RFC 1952) inflated, or NULL; Mem_Free the result */
uint8_t *BMF_Gunzip(const void *data, size_t len, size_t *outlen);

#endif
