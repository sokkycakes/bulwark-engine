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

struct gdifont_s {
	FT_Library lib;
	FT_Face face;
	unsigned char *file;
	size_t filesize;
	int tall, quality, flags;
	gdifont_metrics_t tm;
	int units_per_em;
	int gray_text;            /* ExtTextOut output is greyscale (§4.4) */
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

int gdifont_gasp_gray(const unsigned char *gasp, size_t len, int ppem)
{
	/* §4.4: the first range whose rangeMaxPPEM >= ppem decides; GASP_DOGRAY
	   (0x0002) means greyscale. No table or no matching range: greyscale. */
	unsigned i, n;
	if (!gasp || len < 4)
		return 1;
	n = rd16(gasp + 2);
	for (i = 0; i < n && 4 + 4 * i + 4 <= len; i++) {
		const unsigned char *r = gasp + 4 + 4 * i;
		if ((unsigned)ppem <= rd16(r))
			return (rd16(r + 2) & 2) != 0;
	}
	return 1;
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

static int setup(gdifont_t *f, int height, int weight, int ppem_delta, const char **err)
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
	ppem = gdifont_select_size(&si, height, &asc, &desc) + ppem_delta;
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
	f->gray_text = f->quality == GDIFONT_ANTIALIASED && gdifont_gasp_gray(gasp, glen, ppem);
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
	return gdifont_create_memory_ex(data, size, height, weight, flags, quality, 0, err);
}

gdifont_t *gdifont_create_memory_ex(const void *data, size_t size, int height, int weight,
	int flags, int quality, int ppem_delta, const char **err)
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
	f->quality = quality == GDIFONT_ANTIALIASED ? GDIFONT_ANTIALIASED : GDIFONT_NONANTIALIASED;
	if (!setup(f, height, weight > 0 ? weight : 400, ppem_delta, err)) {
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

/* Render one glyph into the slot: 1-bit (§4.2) or 8-bit grey (§4.3) */
static FT_GlyphSlot render(gdifont_t *f, unsigned cp, int mono)
{
	FT_Int32 flags = FT_LOAD_RENDER | (mono ? FT_LOAD_TARGET_MONO | FT_LOAD_MONOCHROME : FT_LOAD_TARGET_NORMAL);
	if (FT_Load_Char(f->face, cp, flags))
		return NULL;
	if (f->face->glyph->format != FT_GLYPH_FORMAT_BITMAP)
		return NULL;
	return f->face->glyph;
}

static int slot_pixel(const FT_Bitmap *bm, int x, int y)
{
	const unsigned char *row = bm->buffer + (bm->pitch >= 0 ? y * bm->pitch : (y - (int)bm->rows + 1) * bm->pitch);
	if (bm->pixel_mode == FT_PIXEL_MODE_MONO)
		return (row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
	return row[x];
}

static int advance_px(FT_GlyphSlot s)
{
	return (int)((s->advance.x + 32) >> 6);
}

int gdifont_abc(gdifont_t *f, unsigned cp, gdifont_abc_t *out)
{
	/* §5: A = left bearing of the 1-bit render, B = its width,
	   C = rounded advance - A - B */
	FT_GlyphSlot s = render(f, cp, 1);
	if (!s)
		return 0;
	out->a = s->bitmap_left;
	out->b = (int)s->bitmap.width;
	out->c = advance_px(s) - out->a - out->b;
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
	FT_GlyphSlot s = render(f, cp, 1);
	int x, y, w, h, ink = 0;
	memset(out, 0, sizeof(*out));
	if (!s)
		return 0;
	w = (int)s->bitmap.width;
	h = (int)s->bitmap.rows;
	for (y = 0; y < h && !ink; y++)
		for (x = 0; x < w && !ink; x++)
			ink = slot_pixel(&s->bitmap, x, y);
	if (!ink) {
		empty_glyph(out, advance_px(s));
		return 1;
	}
	out->bbx = w;
	out->bby = h;
	out->ox = s->bitmap_left;
	out->oy = s->bitmap_top;
	out->incx = advance_px(s);
	out->pitch = ((w + 31) / 32) * 4;   /* §4.2: DWORD-aligned rows */
	out->size = out->pitch * h;
	out->data = calloc(1, out->size);
	if (!out->data)
		return 0;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			if (slot_pixel(&s->bitmap, x, y))
				out->data[y * out->pitch + (x >> 3)] |= 0x80 >> (x & 7);
	return 1;
}

int gdifont_glyph_gray8(gdifont_t *f, unsigned cp, gdifont_glyph_t *out)
{
	FT_GlyphSlot s = render(f, cp, 0);
	int x, y, w, h, ink = 0;
	memset(out, 0, sizeof(*out));
	if (!s)
		return 0;
	w = (int)s->bitmap.width;
	h = (int)s->bitmap.rows;
	for (y = 0; y < h && !ink; y++)
		for (x = 0; x < w && !ink; x++)
			ink = slot_pixel(&s->bitmap, x, y);
	if (!ink) {
		empty_glyph(out, advance_px(s));
		return 1;
	}
	out->bbx = w;
	out->bby = h;
	out->ox = s->bitmap_left;
	out->oy = s->bitmap_top;
	out->incx = advance_px(s);
	out->pitch = (w + 3) & ~3;          /* §4.3: DWORD-aligned rows */
	out->size = out->pitch * h;
	out->data = calloc(1, out->size);
	if (!out->data)
		return 0;
	/* §4.3: 0..64 levels. The exact GDI mapping is open [U]; this is
	   FreeType's area coverage scaled with rounding, as in the spec. */
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			out->data[y * out->pitch + x] = (unsigned char)((slot_pixel(&s->bitmap, x, y) * 64 + 127) / 255);
	return 1;
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

int gdifont_text_out(gdifont_t *f, int x, int y, const unsigned *cps, int n,
	unsigned char *dst, int dstw, int dsth, int dstpitch)
{
	int i, pen = x, baseline = y + f->tm.ascent;
	for (i = 0; i < n; i++) {
		gdifont_abc_t abc;
		int gx, gy;
		if (!gdifont_abc(f, cps[i], &abc))
			continue;
		if (f->gray_text) {
			/* §4.4: greyscale where gasp allows it at quality 1. The level
			   ramp is open [U]; 17 evenly spaced levels as in the spec. */
			FT_GlyphSlot s = render(f, cps[i], 0);
			if (s)
				for (gy = 0; gy < (int)s->bitmap.rows; gy++)
					for (gx = 0; gx < (int)s->bitmap.width; gx++) {
						int v = slot_pixel(&s->bitmap, gx, gy);
						put_px(dst, dstw, dsth, dstpitch, pen + s->bitmap_left + gx, baseline - s->bitmap_top + gy,
							((v * 16 + 127) / 255) * 255 / 16);
					}
		} else {
			/* 1-bit: identical to GGO_BITMAP on Windows in 99.2-100% of
			   glyphs (run 40463306407, diagnose.py section 1) */
			FT_GlyphSlot s = render(f, cps[i], 1);
			if (s)
				for (gy = 0; gy < (int)s->bitmap.rows; gy++)
					for (gx = 0; gx < (int)s->bitmap.width; gx++)
						put_px(dst, dstw, dsth, dstpitch, pen + s->bitmap_left + gx, baseline - s->bitmap_top + gy,
							slot_pixel(&s->bitmap, gx, gy));
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
