/*
 * gdifont: TrueType text rasterized the way Windows GDI does it, using
 * FreeType on every platform.
 *
 * Built from black-box measurements of Windows GDI (see docs/spec/gdi-text.md
 * for every rule and the evidence behind it). Self-contained: it does not
 * touch the renderer, it only turns a font file into metrics and bitmaps.
 *
 * Coordinates follow GDI: y grows down on bitmaps, glyph origins are given
 * as (left bearing, distance from baseline up to the top row).
 */
#ifndef GDIFONT_H
#define GDIFONT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gdifont_s gdifont_t;

/* CreateFont fdwQuality values that are emulated */
enum {
	GDIFONT_NONANTIALIASED = 0, /* NONANTIALIASED_QUALITY: always 1-bit */
	GDIFONT_ANTIALIASED = 1     /* ANTIALIASED_QUALITY: grey or 1-bit per gasp */
};

/* CreateFont flags. Italic is recorded only: synthetic italic and bold were
   not measured, so pass the italic or bold font file itself. */
enum {
	GDIFONT_ITALIC = 1,
	GDIFONT_UNDERLINE = 2,
	GDIFONT_STRIKEOUT = 4
};

/* TEXTMETRIC subset, in pixels */
typedef struct {
	int height;           /* tmHeight */
	int ascent;           /* tmAscent */
	int descent;          /* tmDescent */
	int internal_leading; /* tmInternalLeading = height - ppem */
	int external_leading; /* tmExternalLeading */
	int ave_char_width;   /* tmAveCharWidth */
	int max_char_width;   /* tmMaxCharWidth */
	int weight;           /* tmWeight */
	int overhang;         /* tmOverhang (0, measured for bold faces) */
	int ppem;             /* the em size glyphs are hinted and rendered at */
} gdifont_metrics_t;

/* GetCharABCWidths */
typedef struct {
	int a;
	int b;
	int c;
} gdifont_abc_t;

/* GetGlyphOutline result */
typedef struct {
	int bbx, bby;          /* gmBlackBoxX / gmBlackBoxY */
	int ox, oy;            /* gmptGlyphOrigin */
	int incx, incy;        /* gmCellIncX / gmCellIncY */
	int pitch;             /* bytes per row, a multiple of 4 */
	int size;              /* pitch * bby: the value GetGlyphOutline returns */
	unsigned char *data;   /* malloc'd; NULL when size is 0 */
} gdifont_glyph_t;

/* The sizing inputs from the font file, for gdifont_select_size */
typedef struct {
	int units_per_em;
	int win_ascent, win_descent;
	int vdmx_count;          /* entries in the usable VDMX group, 0 if none */
	const unsigned short *vdmx_ppem;
	const short *vdmx_ymax, *vdmx_ymin;
} gdifont_sizeinfo_t;

/*
 * Create a font. height is the CreateFont cell height (> 0). weight 0 means
 * 400. The data is copied. Returns NULL on failure; err (if not NULL)
 * receives a short reason.
 */
gdifont_t *gdifont_create_memory(const void *data, size_t size, int height, int weight,
	int flags, int quality, const char **err);
gdifont_t *gdifont_create_file(const char *path, int height, int weight,
	int flags, int quality, const char **err);
void gdifont_free(gdifont_t *f);

/*
 * Rendering choices that were measured against Windows. The defaults
 * (gdifont_default_tuning) are the best fit found; the other values exist
 * so tools/gdidump can score alternatives in CI. See docs/spec/gdi-text.md.
 */
typedef struct {
	int ppem_delta;     /* render this many ems away from the size rule */
	int gray_method;    /* 0: FreeType area coverage; 1: point sampling */
	int gray_hint;      /* hinting for greyscale: 0 grayscale mode, 1 mono mode */
	int sample_phase;   /* point sampling at 0: (i + 0.5) / n, 1: i / n,
	                       2: i / n across only, 3: i / n down only */
	int nogridfit;      /* where gasp lacks GRIDFIT, load unhinted:
	                       bit 0 for 1-bit output and ABC, bit 1 for grey */
	int aa_measure;     /* measuring at ANTIALIASED quality:
	                       0 as at quality 0;
	                       1 grayscale-mode hinting for ABC and GGO_BITMAP,
	                       2 the same only where gasp turns grey on;
	                       ABC from a greyscale box, only where gasp turns grey
	                       on (4, 5, 6, 7) or everywhere (3): 3 and 4 FreeType
	                       area coverage, 5 the 4x4 sampled glyph, 6 the 4x4
	                       sampled glyph's visible pixels (count >= 2),
	                       7 the 8x8 sampled glyph */
	int sample_nudge;   /* move the outline 1/64 sub-pixel before sampling:
	                       0 none, 1 (+x,+y), 2 (+x,-y), 3 (-x,+y), 4 (-x,-y) */
	int mono_method;    /* 1-bit: 0 FreeType's rasterizer with the font's
	                       dropout control, 1 one centre sample, no dropout;
	                       forced dropout mode: 2 smart with stubs, 3 smart
	                       without stubs, 4 simple with stubs, 5 simple
	                       without stubs */
	int v40_below;      /* use the v40 interpreter at ppems below this
	                       (0: always v35) */
} gdifont_tuning_t;

extern const gdifont_tuning_t gdifont_default_tuning;

/* As gdifont_create_memory with explicit tuning (NULL for the defaults) */
gdifont_t *gdifont_create_memory_ex(const void *data, size_t size, int height, int weight,
	int flags, int quality, const gdifont_tuning_t *tuning, const char **err);

const gdifont_metrics_t *gdifont_metrics(const gdifont_t *f);
const char *gdifont_family(const gdifont_t *f);
int gdifont_quality(const gdifont_t *f);
int gdifont_flags(const gdifont_t *f);

/*
 * Describe face `index` of a font file without creating a font: the family
 * name GDI matches on (name ID 1, Windows platform, US English preferred),
 * OS/2 usWeightClass, whether it is italic, and whether it has TrueType
 * outlines (a glyf table; CFF-flavoured OpenType has none). Pass path, or
 * data and size. Returns the number of faces in the file, 0 if it is not a
 * font FreeType can open.
 */
int gdifont_describe(const char *path, const void *data, size_t size, int index,
	char *family, size_t familysize, int *weight, int *italic, int *truetype);

/* Size of an sfnt table in bytes, or -1 when the font has none */
long gdifont_table_size(const gdifont_t *f, const char tag[4]);

/* GetCharABCWidthsW for one codepoint. Returns 1 on success. */
int gdifont_abc(gdifont_t *f, unsigned cp, gdifont_abc_t *out);

/* GetGlyphOutline(GGO_BITMAP): 1 bit per pixel, MSB first, rows padded to 4
   bytes. Returns 1 on success; release with gdifont_glyph_release. */
int gdifont_glyph_mono(gdifont_t *f, unsigned cp, gdifont_glyph_t *out);

/* GetGlyphOutline(GGO_GRAY8_BITMAP): one byte per pixel, 0..64, rows padded
   to 4 bytes. Ignores gasp: always greyscale. */
int gdifont_glyph_gray8(gdifont_t *f, unsigned cp, gdifont_glyph_t *out);

void gdifont_glyph_release(gdifont_glyph_t *g);

/* Whether ExtTextOut output for this font is greyscale (1) or 1-bit (0) */
int gdifont_text_is_gray(const gdifont_t *f);

/*
 * ExtTextOutW with TA_LEFT | TA_TOP, transparent background, no lpDx. The
 * pen starts at (x, y), y being the top of the cell, and advances by
 * A + B + C per character. Writes coverage 0..255 into an 8-bit buffer
 * (1-bit output gives 0 or 255), keeping the larger value where glyphs
 * overlap. Returns the total advance.
 */
int gdifont_text_out(gdifont_t *f, int x, int y, const unsigned *cps, int n,
	unsigned char *dst, int dstw, int dsth, int dstpitch);

/* Total advance of a string, as gdifont_text_out would move the pen */
int gdifont_text_width(gdifont_t *f, const unsigned *cps, int n);

/*
 * The size rule on its own (spec section 3), for testing: pick the ppem,
 * ascent and descent for a cell height. Returns the ppem.
 */
int gdifont_select_size(const gdifont_sizeinfo_t *si, int tall, int *ascent, int *descent);

/* Whether a gasp table (raw sfnt bytes) asks for greyscale at this ppem */
int gdifont_gasp_gray(const unsigned char *gasp, size_t len, int ppem);

#ifdef __cplusplus
}
#endif

#endif
