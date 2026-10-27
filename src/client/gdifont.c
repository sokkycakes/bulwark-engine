/*
 * gdifont: GDI-compatible TrueType rasterization on FreeType.
 *
 * Every rule here comes from black-box captures of Windows GDI, written up
 * in docs/spec/gdi-text.md. Section numbers in comments (for example "§3")
 * refer to that file.
 */
#include "gdifont.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_DRIVER_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H
#include FT_OUTLINE_H

struct gdifont_s {
	FT_Library lib;
	FT_Face face;
	unsigned char *file;
	size_t filesize;
	int tall, quality, flags;
	gdifont_metrics_t tm;
	int units_per_em;
	int gray_text;            /* ExtTextOut output is greyscale (§4.4) */
	int gasp_flags;           /* gasp behaviour at this ppem */
	gdifont_tuning_t tune;
	int ul_pos, ul_size;      /* underline: rows below the baseline, thickness */
	int so_pos, so_size;      /* strikeout: rows above the baseline, thickness */
};

/* floor(num / den + 0.5) for den > 0 or < 0 and any sign of num */
static int rnd_div(long num, long den)
{
	long a, b;
	if (den < 0) { num = -num; den = -den; }
	a = 2 * num + den;
	b = 2 * den;
	return (int)(a >= 0 ? a / b : -((-a + b - 1) / b));
}

static unsigned rd16(const unsigned char *p) { return (unsigned)(p[0] << 8 | p[1]); }
static int rds16(const unsigned char *p) { return (short)rd16(p); }

/* Load a whole sfnt table, or NULL */
static unsigned char *load_table(FT_Face face, FT_ULong tag, size_t *len)
{
	FT_ULong n = 0;
	unsigned char *buf;
	if (FT_Load_Sfnt_Table(face, tag, 0, NULL, &n) || n == 0)
		return NULL;
	buf = malloc(n);
	if (!buf)
		return NULL;
	if (FT_Load_Sfnt_Table(face, tag, 0, buf, &n)) {
		free(buf);
		return NULL;
	}
	*len = n;
	return buf;
}

long gdifont_table_size(const gdifont_t *f, const char tag[4])
{
	FT_ULong n = 0;
	FT_ULong t = FT_MAKE_TAG(tag[0], tag[1], tag[2], tag[3]);
	if (FT_Load_Sfnt_Table(f->face, t, 0, NULL, &n) || n == 0)
		return -1;
	return (long)n;
}

/* gasp behaviour flags for a ppem: the first range whose rangeMaxPPEM >=
   ppem. No table or no matching range: grid-fit and grey (0x000F). */
static int gasp_flags(const unsigned char *gasp, size_t len, int ppem)
{
	unsigned i, n;
	if (!gasp || len < 4)
		return 0x000F;
	n = rd16(gasp + 2);
	for (i = 0; i < n && 4 + 4 * i + 4 <= len; i++) {
		const unsigned char *r = gasp + 4 + 4 * i;
		if ((unsigned)ppem <= rd16(r))
			return (int)rd16(r + 2);
	}
	return 0x000F;
}

int gdifont_gasp_gray(const unsigned char *gasp, size_t len, int ppem)
{
	/* §4.4: GASP_DOGRAY (0x0002) means greyscale */
	return (gasp_flags(gasp, len, ppem) & 2) != 0;
}

int gdifont_select_size(const gdifont_sizeinfo_t *si, int tall, int *ascent, int *descent)
{
	int cap = 1 << 30, p, i, wsum = si->win_ascent + si->win_descent;

	/* §3 with a usable VDMX group (fitted to runs 40463306407 and
	   40463949748): walk the entries in ascending ppem and keep the last
	   whose yMax - yMin fits in tall, but stop at the first whose height
	   is exactly tall. So among equal heights the smallest ppem wins when
	   the height is tall itself, and the largest when it is shorter. The
	   entry gives the ascent and descent; the cell can be shorter than
	   tall. */
	if (si->vdmx_count > 0) {
		int best = -1, first = 1 << 30, q, next;
		for (i = 0; i < si->vdmx_count; i++)
			if (si->vdmx_ppem[i] < first)
				first = si->vdmx_ppem[i];
		for (q = first; q >= 0; q = next) {
			int h = 0, at = -1;
			next = -1;
			for (i = 0; i < si->vdmx_count; i++) {
				if (si->vdmx_ppem[i] == q)
					at = i;
				else if (si->vdmx_ppem[i] > q && (next < 0 || si->vdmx_ppem[i] < next))
					next = si->vdmx_ppem[i];
			}
			h = si->vdmx_ymax[at] - si->vdmx_ymin[at];
			if (h <= tall)
				best = at;
			if (h == tall)
				break;
		}
		if (best >= 0) {
			*ascent = si->vdmx_ymax[best];
			*descent = -si->vdmx_ymin[best];
			return si->vdmx_ppem[best];
		}
		/* nothing fits: stay below the table's first size */
		cap = first - 1;
	}

	/* §3 rule 3: largest ppem whose scaled win height fits */
	p = 1;
	if (wsum > 0 && si->units_per_em > 0)
		while (p + 1 <= cap && rnd_div((long)(p + 1) * wsum, si->units_per_em) <= tall)
			p++;
	*ascent = wsum > 0 ? rnd_div((long)si->win_ascent * tall, wsum) : tall;
	*descent = tall - *ascent;
	return p;
}

/*
 * §3 rule 1: VDMX counts only through a ratio record with bCharSet 1 that
 * covers 1:1. Fills the arrays (malloc'd) and returns the entry count.
 */
static int parse_vdmx(const unsigned char *v, size_t len, unsigned short **ppem, short **ymax, short **ymin)
{
	unsigned nrat, i, off = 0, recs;
	int found = 0;
	if (!v || len < 6)
		return 0;
	nrat = rd16(v + 4);
	if (6 + 6 * (size_t)nrat > len)
		return 0;
	for (i = 0; i < nrat; i++) {
		const unsigned char *r = v + 6 + 4 * i;
		if (r[0] == 1 && r[1] == 1 && r[2] <= 1 && 1 <= r[3]) {
			off = rd16(v + 6 + 4 * nrat + 2 * i);
			found = 1;
			break;
		}
	}
	if (!found || off + 4 > len)
		return 0;
	recs = rd16(v + off);
	if (off + 4 + 6 * (size_t)recs > len)
		recs = (unsigned)((len - off - 4) / 6);
	if (!recs)
		return 0;
	*ppem = malloc(recs * sizeof(**ppem));
	*ymax = malloc(recs * sizeof(**ymax));
	*ymin = malloc(recs * sizeof(**ymin));
	if (!*ppem || !*ymax || !*ymin)
		return 0;
	for (i = 0; i < recs; i++) {
		const unsigned char *e = v + off + 4 + 6 * i;
		(*ppem)[i] = (unsigned short)rd16(e);
		(*ymax)[i] = (short)rds16(e + 2);
		(*ymin)[i] = (short)rds16(e + 4);
	}
	return (int)recs;
}

static int setup(gdifont_t *f, int height, int weight, const char **err)
{
	TT_OS2 *os2;
	TT_HoriHeader *hhea;
	TT_Postscript *post;
	gdifont_sizeinfo_t si;
	unsigned short *vp = NULL;
	short *vmax = NULL, *vmin = NULL;
	unsigned char *vdmx, *gasp;
	size_t vlen = 0, glen = 0;
	FT_UInt v35 = TT_INTERPRETER_VERSION_35;
	int asc, desc, ppem, upem;

	if (FT_Init_FreeType(&f->lib)) { *err = "FreeType init failed"; return 0; }
	/* §4.1: hinting is the TrueType bytecode, interpreter v35 */
	FT_Property_Set(f->lib, "truetype", "interpreter-version", &v35);
	if (FT_New_Memory_Face(f->lib, f->file, (FT_Long)f->filesize, 0, &f->face)) {
		*err = "not a font FreeType can open";
		return 0;
	}
	if (!FT_IS_SFNT(f->face) || !FT_IS_SCALABLE(f->face)) {
		*err = "not a scalable sfnt font";
		return 0;
	}
	os2 = FT_Get_Sfnt_Table(f->face, FT_SFNT_OS2);
	hhea = FT_Get_Sfnt_Table(f->face, FT_SFNT_HHEA);
	post = FT_Get_Sfnt_Table(f->face, FT_SFNT_POST);
	if (!os2 || os2->version == 0xFFFF || !hhea) {
		*err = "font has no OS/2 or hhea table";
		return 0;
	}
	upem = f->face->units_per_EM;
	f->units_per_em = upem;

	memset(&si, 0, sizeof(si));
	si.units_per_em = upem;
	si.win_ascent = os2->usWinAscent;
	si.win_descent = os2->usWinDescent;
	vdmx = load_table(f->face, TTAG_VDMX, &vlen);
	si.vdmx_count = parse_vdmx(vdmx, vlen, &vp, &vmax, &vmin);
	si.vdmx_ppem = vp;
	si.vdmx_ymax = vmax;
	si.vdmx_ymin = vmin;
	ppem = gdifont_select_size(&si, height, &asc, &desc) + f->tune.ppem_delta;
	if (ppem < 1)
		ppem = 1;
	free(vdmx); free(vp); free(vmax); free(vmin);

	if (FT_Set_Pixel_Sizes(f->face, 0, ppem)) {
		*err = "FreeType could not set the size";
		return 0;
	}

	f->tm.ppem = ppem;
	f->tm.ascent = asc;
	f->tm.descent = desc;
	f->tm.height = asc + desc;
	f->tm.internal_leading = f->tm.height - ppem;     /* §3 rule 4 */
	{
		/* [D] OpenType spec, hhea/OS/2 notes on Windows line spacing:
		   external leading = max(0, lineGap - ((winA + winD) - (hheaA - hheaD))) */
		long el = hhea->Line_Gap - ((long)os2->usWinAscent + os2->usWinDescent - (hhea->Ascender - hhea->Descender));
		f->tm.external_leading = el > 0 ? rnd_div(el * ppem, upem) : 0;
	}
	f->tm.ave_char_width = rnd_div((long)os2->xAvgCharWidth * ppem, upem);
	/* tmMaxCharWidth: the head bounding box width scaled to the ppem. The
	   scaled hhea advanceWidthMax and the largest hinted advance were both
	   too small in every config of runs 40463306407 and 40463949748; Arial
	   (bbox 5457 units) gives 19 at ppem 7 and 21 at ppem 8 as Windows
	   reports. [F] 204/204 in run 40464365435 */
	{
		long bw = (long)f->face->bbox.xMax - f->face->bbox.xMin;
		f->tm.max_char_width = rnd_div(bw * ppem, upem);
	}
	f->tm.weight = weight;
	f->tm.overhang = 0;

	gasp = load_table(f->face, TTAG_gasp, &glen);
	f->gasp_flags = gasp_flags(gasp, glen, ppem);
	f->gray_text = f->quality == GDIFONT_ANTIALIASED && (f->gasp_flags & 2);
	free(gasp);

	/* Underline and strikeout: not measured yet, taken from post and OS/2 */
	if (post) {
		f->ul_pos = -rnd_div((long)post->underlinePosition * ppem, upem);
		f->ul_size = rnd_div((long)post->underlineThickness * ppem, upem);
	}
	f->so_pos = rnd_div((long)os2->yStrikeoutPosition * ppem, upem);
	f->so_size = rnd_div((long)os2->yStrikeoutSize * ppem, upem);
	if (f->ul_size < 1) f->ul_size = 1;
	if (f->so_size < 1) f->so_size = 1;
	if (f->ul_pos < 1) f->ul_pos = 1;
	return 1;
}

gdifont_t *gdifont_create_memory(const void *data, size_t size, int height, int weight,
	int flags, int quality, const char **err)
{
	return gdifont_create_memory_ex(data, size, height, weight, flags, quality, NULL, err);
}

gdifont_t *gdifont_create_memory_ex(const void *data, size_t size, int height, int weight,
	int flags, int quality, const gdifont_tuning_t *tuning, const char **err)
{
	const char *dummy;
	gdifont_t *f;
	if (!err)
		err = &dummy;
	*err = NULL;
	if (!data || !size || height <= 0) {
		*err = "bad arguments (height must be > 0)";
		return NULL;
	}
	f = calloc(1, sizeof(*f));
	if (!f || !(f->file = malloc(size))) {
		free(f);
		*err = "out of memory";
		return NULL;
	}
	memcpy(f->file, data, size);
	f->filesize = size;
	f->tall = height;
	f->flags = flags;
	f->tune = tuning ? *tuning : gdifont_default_tuning;
	f->quality = quality == GDIFONT_ANTIALIASED ? GDIFONT_ANTIALIASED : GDIFONT_NONANTIALIASED;
	if (!setup(f, height, weight > 0 ? weight : 400, err)) {
		gdifont_free(f);
		return NULL;
	}
	return f;
}

gdifont_t *gdifont_create_file(const char *path, int height, int weight,
	int flags, int quality, const char **err)
{
	FILE *fp = fopen(path, "rb");
	long n;
	unsigned char *buf;
	gdifont_t *f;
	if (!fp) {
		if (err) *err = "cannot open file";
		return NULL;
	}
	fseek(fp, 0, SEEK_END);
	n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (n <= 0 || !(buf = malloc(n))) {
		fclose(fp);
		if (err) *err = "cannot read file";
		return NULL;
	}
	if (fread(buf, 1, n, fp) != (size_t)n) {
		fclose(fp);
		free(buf);
		if (err) *err = "cannot read file";
		return NULL;
	}
	fclose(fp);
	f = gdifont_create_memory(buf, (size_t)n, height, weight, flags, quality, err);
	free(buf);
	return f;
}

void gdifont_free(gdifont_t *f)
{
	if (!f)
		return;
	if (f->face) FT_Done_Face(f->face);
	if (f->lib) FT_Done_FreeType(f->lib);
	free(f->file);
	free(f);
}

const gdifont_metrics_t *gdifont_metrics(const gdifont_t *f) { return &f->tm; }
const char *gdifont_family(const gdifont_t *f) { return f->face->family_name ? f->face->family_name : ""; }
int gdifont_quality(const gdifont_t *f) { return f->quality; }
int gdifont_flags(const gdifont_t *f) { return f->flags; }
int gdifont_text_is_gray(const gdifont_t *f) { return f->gray_text; }

const gdifont_tuning_t gdifont_default_tuning = {
	0,  /* ppem_delta */
	0,  /* gray_method */
	0,  /* gray_hint */
	0,  /* sample_phase */
	0,  /* nogridfit */
	0   /* aa_measure */
};

/* A rendered glyph: one byte per pixel, rows top down. left/top as FreeType
   bitmap_left/bitmap_top. */
typedef struct {
	int w, h, left, top, adv;
	unsigned char *px;
} bmp_t;

static int advance_px(FT_GlyphSlot s)
{
	return (int)((s->advance.x + 32) >> 6);
}

/* Unhinted where gasp lacks GASP_GRIDFIT, when the tuning asks for it */
static int unhinted(const gdifont_t *f, int grey)
{
	return (f->tune.nogridfit & (grey ? 2 : 1)) && !(f->gasp_flags & 1);
}

/*
 * Load the outline. mono_hint picks the hinting mode: in v35 the font's
 * GETINFO reports grayscale only in grayscale mode, so hints can differ.
 * The advance always comes from a hinted load.
 */
static FT_GlyphSlot load(gdifont_t *f, unsigned cp, int mono_hint, int nohint, int outline_only, int *adv)
{
	FT_Int32 fl = (outline_only ? FT_LOAD_NO_BITMAP : 0) | (mono_hint ? FT_LOAD_TARGET_MONO | FT_LOAD_MONOCHROME : FT_LOAD_TARGET_NORMAL);
	if (nohint) {
		if (FT_Load_Char(f->face, cp, fl))
			return NULL;
		*adv = advance_px(f->face->glyph);
		fl |= FT_LOAD_NO_HINTING;
	}
	if (FT_Load_Char(f->face, cp, fl))
		return NULL;
	if (!nohint)
		*adv = advance_px(f->face->glyph);
	return f->face->glyph;
}

static int copy_slot(FT_GlyphSlot s, bmp_t *b, int adv)
{
	int x, y;
	const FT_Bitmap *bm = &s->bitmap;
	memset(b, 0, sizeof(*b));
	b->w = (int)bm->width;
	b->h = (int)bm->rows;
	b->left = s->bitmap_left;
	b->top = s->bitmap_top;
	b->adv = adv;
	if (!b->w || !b->h)
		return 1;
	b->px = malloc((size_t)b->w * b->h);
	if (!b->px)
		return 0;
	for (y = 0; y < b->h; y++) {
		const unsigned char *row = bm->buffer + (bm->pitch >= 0 ? y * bm->pitch : (y - b->h + 1) * bm->pitch);
		for (x = 0; x < b->w; x++)
			b->px[y * b->w + x] = bm->pixel_mode == FT_PIXEL_MODE_MONO
				? ((row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0) : row[x];
	}
	return 1;
}

/* FreeType's own rasterizers: 1-bit (§4.2) or 8-bit area coverage */
static int render_ft(gdifont_t *f, unsigned cp, int mono_hint, int nohint, int mono_out, bmp_t *b)
{
	int adv = 0;
	FT_GlyphSlot s = load(f, cp, mono_hint, nohint, 0, &adv);
	if (!s)
		return 0;
	if (s->format == FT_GLYPH_FORMAT_OUTLINE &&
		FT_Render_Glyph(s, mono_out ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL))
		return 0;
	return copy_slot(s, b, adv);
}

/*
 * Point sampling: each pixel counts which of n x n sample points fall inside
 * the outline, with no dropout control. The points sit at (i + 0.5) / n of
 * the pixel (phase 0) or at i / n from its top left corner (phase 1).
 */
static int render_sampled(gdifont_t *f, unsigned cp, int n, bmp_t *b)
{
	FT_GlyphSlot s;
	FT_Outline o;
	FT_BBox cb;
	FT_Bitmap bm;
	FT_Matrix m;
	int x0, y0, x1, y1, W, H, x, y, minx, miny, maxx, maxy, adv = 0;
	unsigned char *counts;

	memset(b, 0, sizeof(*b));
	s = load(f, cp, f->tune.gray_hint, unhinted(f, 1), 1, &adv);
	if (!s)
		return 0;
	b->adv = adv;
	if (s->format != FT_GLYPH_FORMAT_OUTLINE || s->outline.n_points == 0)
		return 1;
	FT_Outline_Get_CBox(&s->outline, &cb);
	/* one pixel of margin so a shifted sample grid cannot clip */
	x0 = (int)(cb.xMin >> 6) - 1;
	y0 = (int)(cb.yMin >> 6) - 1;
	x1 = (int)((cb.xMax + 63) >> 6) + 1;
	y1 = (int)((cb.yMax + 63) >> 6) + 1;
	W = x1 - x0;
	H = y1 - y0;
	if (W <= 0 || H <= 0 || W > 4096 || H > 4096)
		return 0;
	if (FT_Outline_New(f->lib, s->outline.n_points, s->outline.n_contours, &o))
		return 0;
	FT_Outline_Copy(&s->outline, &o);
	FT_Outline_Translate(&o, -(FT_Pos)x0 * 64, -(FT_Pos)y0 * 64);
	m.xx = m.yy = (FT_Fixed)n << 16;
	m.xy = m.yx = 0;
	FT_Outline_Transform(&o, &m);
	/* the rasterizer samples sub-pixel centres; shifting the outline by
	   half a sub-pixel moves the samples to i / n from the top left */
	if (f->tune.sample_phase)
		FT_Outline_Translate(&o, 32, -32);
	o.flags &= ~(FT_OUTLINE_SMART_DROPOUTS | FT_OUTLINE_INCLUDE_STUBS);
	o.flags |= FT_OUTLINE_IGNORE_DROPOUTS;

	memset(&bm, 0, sizeof(bm));
	bm.width = (unsigned)(W * n);
	bm.rows = (unsigned)(H * n);
	bm.pitch = (int)((bm.width + 15) / 16 * 2);
	bm.pixel_mode = FT_PIXEL_MODE_MONO;
	bm.num_grays = 2;
	bm.buffer = calloc(1, (size_t)bm.pitch * bm.rows);
	counts = calloc(1, (size_t)W * H);
	if (!bm.buffer || !counts || FT_Outline_Get_Bitmap(f->lib, &o, &bm)) {
		free(bm.buffer);
		free(counts);
		FT_Outline_Done(f->lib, &o);
		return 0;
	}
	FT_Outline_Done(f->lib, &o);
	for (y = 0; y < H * n; y++)
		for (x = 0; x < W * n; x++)
			if (bm.buffer[y * bm.pitch + (x >> 3)] & (0x80 >> (x & 7)))
				counts[(y / n) * W + x / n]++;
	free(bm.buffer);

	minx = W; miny = H; maxx = -1; maxy = -1;
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			if (counts[y * W + x]) {
				if (x < minx) minx = x;
				if (x > maxx) maxx = x;
				if (y < miny) miny = y;
				if (y > maxy) maxy = y;
			}
	if (maxx >= 0) {
		b->w = maxx - minx + 1;
		b->h = maxy - miny + 1;
		b->left = x0 + minx;
		b->top = y1 - miny;     /* bitmap row 0 is pixel row y1 - 1 */
		b->px = malloc((size_t)b->w * b->h);
		if (!b->px) {
			free(counts);
			return 0;
		}
		for (y = 0; y < b->h; y++)
			memcpy(b->px + y * b->w, counts + (miny + y) * W + minx, (size_t)b->w);
	}
	free(counts);
	return 1;
}

/* Hinting mode for ABC, GGO_BITMAP and 1-bit text (tuning aa_measure) */
static int measure_mono_hint(const gdifont_t *f)
{
	int m = f->tune.aa_measure;
	if (f->quality != GDIFONT_ANTIALIASED || m == 0)
		return 1;
	if ((m == 2 || m == 4) && !f->gray_text)
		return 1;
	return 0;
}

/* The 1-bit glyph behind GGO_BITMAP, ABC and 1-bit text */
static int mono_glyph(gdifont_t *f, unsigned cp, bmp_t *b)
{
	return render_ft(f, cp, measure_mono_hint(f), unhinted(f, 0), 1, b);
}

/* The greyscale glyph: values 0..64 for GGO_GRAY8 (n = 8) or 0..16 sample
   counts for text (n = 4); with area coverage, 0..255 */
static int grey_glyph(gdifont_t *f, unsigned cp, int n, bmp_t *b)
{
	if (f->tune.gray_method == 1)
		return render_sampled(f, cp, n, b);
	return render_ft(f, cp, f->tune.gray_hint, unhinted(f, 1), 0, b);
}

static int has_ink(const bmp_t *b)
{
	int i;
	for (i = 0; i < b->w * b->h; i++)
		if (b->px[i])
			return 1;
	return 0;
}

int gdifont_abc(gdifont_t *f, unsigned cp, gdifont_abc_t *out)
{
	/* §5: A = left bearing of the 1-bit render, B = its width,
	   C = rounded advance - A - B */
	bmp_t b;
	int m = f->tune.aa_measure;
	int ok = (f->quality == GDIFONT_ANTIALIASED && (m == 3 || (m == 4 && f->gray_text)))
		? render_ft(f, cp, 0, unhinted(f, 0), 0, &b) : mono_glyph(f, cp, &b);
	if (!ok)
		return 0;
	out->a = b.left;
	out->b = b.w;
	out->c = b.adv - b.left - b.w;
	free(b.px);
	return 1;
}

/* An empty glyph (space): GDI returns no data, a 1x1 black box and a zero
   origin (run 40463306407, section 8 of diagnose.py) */
static void empty_glyph(gdifont_glyph_t *out, int incx)
{
	memset(out, 0, sizeof(*out));
	out->bbx = out->bby = 1;
	out->incx = incx;
}

int gdifont_glyph_mono(gdifont_t *f, unsigned cp, gdifont_glyph_t *out)
{
	bmp_t b;
	int x, y;
	memset(out, 0, sizeof(*out));
	if (!mono_glyph(f, cp, &b))
		return 0;
	if (!has_ink(&b)) {
		empty_glyph(out, b.adv);
		free(b.px);
		return 1;
	}
	out->bbx = b.w;
	out->bby = b.h;
	out->ox = b.left;
	out->oy = b.top;
	out->incx = b.adv;
	out->pitch = ((b.w + 31) / 32) * 4;   /* §4.2: DWORD-aligned rows */
	out->size = out->pitch * b.h;
	out->data = calloc(1, out->size);
	if (out->data)
		for (y = 0; y < b.h; y++)
			for (x = 0; x < b.w; x++)
				if (b.px[y * b.w + x])
					out->data[y * out->pitch + (x >> 3)] |= 0x80 >> (x & 7);
	free(b.px);
	return out->data != NULL;
}

int gdifont_glyph_gray8(gdifont_t *f, unsigned cp, gdifont_glyph_t *out)
{
	bmp_t b;
	int x, y;
	memset(out, 0, sizeof(*out));
	if (!grey_glyph(f, cp, 8, &b))
		return 0;
	if (!has_ink(&b)) {
		empty_glyph(out, b.adv);
		free(b.px);
		return 1;
	}
	out->bbx = b.w;
	out->bby = b.h;
	out->ox = b.left;
	out->oy = b.top;
	out->incx = b.adv;
	out->pitch = (b.w + 3) & ~3;          /* §4.3: DWORD-aligned rows */
	out->size = out->pitch * b.h;
	out->data = calloc(1, out->size);
	if (out->data)
		for (y = 0; y < b.h; y++)
			for (x = 0; x < b.w; x++) {
				int v = b.px[y * b.w + x];
				/* sampled counts are 0..64 already; area coverage is
				   scaled with rounding (§4.3) */
				out->data[y * out->pitch + x] = (unsigned char)(f->tune.gray_method == 1 ? v : (v * 64 + 127) / 255);
			}
	free(b.px);
	return out->data != NULL;
}

void gdifont_glyph_release(gdifont_glyph_t *g)
{
	if (g) {
		free(g->data);
		g->data = NULL;
	}
}

static void put_px(unsigned char *dst, int dstw, int dsth, int pitch, int x, int y, int v)
{
	if (x < 0 || y < 0 || x >= dstw || y >= dsth || v <= 0)
		return;
	if (dst[y * pitch + x] < v)
		dst[y * pitch + x] = (unsigned char)v;
}

int gdifont_text_width(gdifont_t *f, const unsigned *cps, int n)
{
	int i, w = 0;
	gdifont_abc_t abc;
	for (i = 0; i < n; i++)
		if (gdifont_abc(f, cps[i], &abc))
			w += abc.a + abc.b + abc.c;
	return w;
}

/* Greyscale text with point sampling: 4 x 4 count -> level. Counts 0 and 1
   draw nothing. Observed in run 40463306407 (Windows build 26100, ClearType
   contrast 1200): these are the only 15 non-zero levels Windows produced. */
static const unsigned char eto_levels[17] = {
	0, 0, 104, 124, 140, 154, 167, 178, 189, 199, 208, 217, 225, 233, 240, 248, 255
};

int gdifont_text_out(gdifont_t *f, int x, int y, const unsigned *cps, int n,
	unsigned char *dst, int dstw, int dsth, int dstpitch)
{
	int i, pen = x, baseline = y + f->tm.ascent;
	for (i = 0; i < n; i++) {
		gdifont_abc_t abc;
		bmp_t b;
		int gx, gy, ok;
		if (!gdifont_abc(f, cps[i], &abc))
			continue;
		/* §4.4: greyscale where gasp allows it at quality 1, else 1-bit.
		   1-bit text equals GGO_BITMAP on Windows in 99.2-100% of glyphs
		   (diagnose.py section 1). */
		ok = f->gray_text ? grey_glyph(f, cps[i], 4, &b) : mono_glyph(f, cps[i], &b);
		if (ok) {
			for (gy = 0; gy < b.h; gy++)
				for (gx = 0; gx < b.w; gx++) {
					int v = b.px[gy * b.w + gx];
					if (f->gray_text)
						v = f->tune.gray_method == 1 ? eto_levels[v > 16 ? 16 : v]
							: ((v * 16 + 127) / 255) * 255 / 16; /* 17 even levels */
					put_px(dst, dstw, dsth, dstpitch, pen + b.left + gx, baseline - b.top + gy, v);
				}
			free(b.px);
		}
		pen += abc.a + abc.b + abc.c;
	}
	if (f->flags & (GDIFONT_UNDERLINE | GDIFONT_STRIKEOUT)) {
		int r, c;
		if (f->flags & GDIFONT_UNDERLINE)
			for (r = 0; r < f->ul_size; r++)
				for (c = x; c < pen; c++)
					put_px(dst, dstw, dsth, dstpitch, c, baseline + f->ul_pos + r, 255);
		if (f->flags & GDIFONT_STRIKEOUT)
			for (r = 0; r < f->so_size; r++)
				for (c = x; c < pen; c++)
					put_px(dst, dstw, dsth, dstpitch, c, baseline - f->so_pos + r, 255);
	}
	return pen - x;
}
