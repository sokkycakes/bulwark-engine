/* Bulwark engine - bitmap fonts (BDF, PCF, pixel-grid TrueType).
 * See bmfont.h and docs/spec/bitmap-fonts.md. Inflate by stb_image and
 * outlines by stb_truetype (both public domain). ISC licence, see LICENSE. */
#include "bmfont.h"
#include "common.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../thirdparty/stb_image.h"

/* a private copy: the renderer has its own */
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../thirdparty/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define BMF_MAX_GLYPHS 70000
#define BMF_MAX_DIM 512

/* --- building ---------------------------------------------------------------- */

typedef struct {
	bmfont_t *f;
	int cap;
	size_t pixcap, pixlen;
} builder_t;

static bmfont_t *new_font(builder_t *b, bmfkind_t kind)
{
	memset(b, 0, sizeof(*b));
	b->f = Mem_Alloc(sizeof(bmfont_t));
	b->f->kind = kind;
	b->f->weight = 400;
	b->f->defaultglyph = -1;
	return b->f;
}

/* room for one more glyph of w*h pixels; returns its pixels (zeroed) */
static uint8_t *add_glyph(builder_t *b, int cp, int w, int h, int xoff, int yoff, int adv)
{
	bmglyph_t *g;
	size_t n = (size_t)w * (size_t)h;
	if (b->f->numglyphs == b->cap) {
		b->cap = b->cap ? b->cap * 2 : 128;
		b->f->glyphs = Mem_Realloc(b->f->glyphs, sizeof(bmglyph_t) * (size_t)b->cap);
	}
	if (b->pixlen + n > b->pixcap) {
		size_t old = b->pixcap;
		while (b->pixlen + n > b->pixcap)
			b->pixcap = b->pixcap ? b->pixcap * 2 : 4096;
		b->f->pixels = Mem_Realloc(b->f->pixels, b->pixcap);
		memset(b->f->pixels + old, 0, b->pixcap - old);
	}
	g = &b->f->glyphs[b->f->numglyphs++];
	g->cp = cp;
	g->w = w;
	g->h = h;
	g->xoff = xoff;
	g->yoff = yoff;
	g->adv = adv;
	g->bits = b->pixlen;
	b->pixlen += n;
	return b->f->pixels + g->bits;
}

static int cmp_glyph(const void *a, const void *b)
{
	return ((const bmglyph_t *)a)->cp - ((const bmglyph_t *)b)->cp;
}

/* sorts, drops repeated code points and resolves the default glyph */
static bmfont_t *finish(builder_t *b, int defaultcp)
{
	bmfont_t *f = b->f;
	int n = 0;
	if (!f->numglyphs) {
		BMF_Free(f);
		return NULL;
	}
	qsort(f->glyphs, (size_t)f->numglyphs, sizeof(bmglyph_t), cmp_glyph);
	for (int i = 0; i < f->numglyphs; i++)
		if (!n || f->glyphs[i].cp != f->glyphs[n - 1].cp)
			f->glyphs[n++] = f->glyphs[i];
	f->numglyphs = n;
	f->defaultglyph = -1;
	if (defaultcp >= 0) {
		const bmglyph_t *g = BMF_Glyph(f, defaultcp);
		if (g)
			f->defaultglyph = (int)(g - f->glyphs);
	}
	if (f->ascent <= 0 && f->descent <= 0) {
		for (int i = 0; i < f->numglyphs; i++) {
			f->ascent = BW_MAX(f->ascent, f->glyphs[i].yoff + f->glyphs[i].h);
			f->descent = BW_MAX(f->descent, -f->glyphs[i].yoff);
		}
	}
	if (f->pixelsize <= 0)
		f->pixelsize = f->ascent + f->descent;
	return f;
}

void BMF_Free(bmfont_t *f)
{
	if (!f)
		return;
	Mem_Free(f->glyphs);
	Mem_Free(f->pixels);
	Mem_Free(f);
}

const bmglyph_t *BMF_Glyph(const bmfont_t *f, int cp)
{
	int lo = 0, hi = f->numglyphs - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (f->glyphs[mid].cp == cp)
			return &f->glyphs[mid];
		if (f->glyphs[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return f->defaultglyph >= 0 ? &f->glyphs[f->defaultglyph] : NULL;
}

/* XLFD weight and slant names to CSS weights */
static int weight_from_name(const char *w)
{
	char l[32];
	size_t i;
	for (i = 0; w[i] && i < sizeof(l) - 1; i++)
		l[i] = (char)tolower((unsigned char)w[i]);
	l[i] = 0;
	if (strstr(l, "black") || strstr(l, "heavy"))
		return 900;
	if (strstr(l, "extrabold") || strstr(l, "ultrabold"))
		return 800;
	if (strstr(l, "semibold") || strstr(l, "demi"))
		return 600;
	if (strstr(l, "bold"))
		return 700;
	/* XLFD "medium" is the regular weight (Medium-R is the ordinary X font) */
	if (strstr(l, "medium") || strstr(l, "regular") || strstr(l, "normal") || strstr(l, "book"))
		return 400;
	if (strstr(l, "light"))
		return 300;
	if (strstr(l, "thin"))
		return 100;
	return 400;
}

static bool slant_italic(const char *s) { return s[0] == 'i' || s[0] == 'I' || s[0] == 'o' || s[0] == 'O'; }

/* Whether codes in this registry are Unicode: ISO10646, ISO8859-1 (whose
   codes are the first 256 code points) and fonts that name none. Other
   registries keep their codes, which is right for ASCII at least. */
static bool unicode_registry(const char *reg, const char *enc)
{
	(void)enc;
	return !reg[0] || !Str_ICmp(reg, "ISO10646") || !Str_ICmp(reg, "ISO8859");
}

/* --- BDF ------------------------------------------------------------------------- */

typedef struct {
	const char *p, *end;
} lines_t;

/* the next line, NUL-terminated in buf (truncated to its size) */
static bool next_line(lines_t *l, char *buf, size_t size)
{
	size_t n = 0;
	if (l->p >= l->end)
		return false;
	while (l->p < l->end && *l->p != '\n') {
		if (n + 1 < size && *l->p != '\r')
			buf[n++] = *l->p;
		l->p++;
	}
	if (l->p < l->end)
		l->p++;
	buf[n] = 0;
	return true;
}

static bool keyword(const char *line, const char *kw, const char **rest)
{
	size_t n = strlen(kw);
	if (strncmp(line, kw, n) || (line[n] && line[n] != ' ' && line[n] != '\t'))
		return false;
	line += n;
	while (*line == ' ' || *line == '\t')
		line++;
	*rest = line;
	return true;
}

/* a property's value with its quotes removed ("" doubled quotes kept as one) */
static void prop_string(const char *v, char *out, size_t size)
{
	size_t n = 0;
	if (*v == '"') {
		v++;
		while (*v && n + 1 < size) {
			if (v[0] == '"' && v[1] == '"') {
				out[n++] = '"';
				v += 2;
			} else if (*v == '"') {
				break;
			} else {
				out[n++] = *v++;
			}
		}
	} else {
		while (*v && n + 1 < size)
			out[n++] = *v++;
		while (n && (out[n - 1] == ' ' || out[n - 1] == '\t'))
			n--;
	}
	out[n] = 0;
}

static int hexdigit(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bmfont_t *load_bdf(const char *data, size_t len, bool describeonly)
{
	builder_t b;
	bmfont_t *f = new_font(&b, BMF_BDF);
	lines_t l = {data, data + len};
	char line[1024], reg[32] = "", enc[32] = "";
	const char *v;
	int fbb[4] = {0, 0, 0, 0}, defaultcp = -1;
	bool haveasc = false, havedesc = false, unicode = true;
	/* header and properties, up to CHARS */
	while (next_line(&l, line, sizeof(line))) {
		if (keyword(line, "FONTBOUNDINGBOX", &v)) {
			sscanf(v, "%d %d %d %d", &fbb[0], &fbb[1], &fbb[2], &fbb[3]);
		} else if (keyword(line, "SIZE", &v)) {
			int pt = 0, xres = 0, yres = 0;
			if (sscanf(v, "%d %d %d", &pt, &xres, &yres) >= 3 && !f->pixelsize && yres > 0)
				f->pixelsize = (pt * yres + 36) / 72;
		} else if (keyword(line, "FAMILY_NAME", &v)) {
			prop_string(v, f->family, sizeof(f->family));
		} else if (keyword(line, "WEIGHT_NAME", &v)) {
			char w[32];
			prop_string(v, w, sizeof(w));
			f->weight = weight_from_name(w);
		} else if (keyword(line, "SLANT", &v)) {
			char s[8];
			prop_string(v, s, sizeof(s));
			f->italic = slant_italic(s);
		} else if (keyword(line, "PIXEL_SIZE", &v)) {
			f->pixelsize = atoi(v);
		} else if (keyword(line, "FONT_ASCENT", &v)) {
			f->ascent = atoi(v);
			haveasc = true;
		} else if (keyword(line, "FONT_DESCENT", &v)) {
			f->descent = atoi(v);
			havedesc = true;
		} else if (keyword(line, "CHARSET_REGISTRY", &v)) {
			prop_string(v, reg, sizeof(reg));
		} else if (keyword(line, "CHARSET_ENCODING", &v)) {
			prop_string(v, enc, sizeof(enc));
		} else if (keyword(line, "DEFAULT_CHAR", &v)) {
			defaultcp = atoi(v);
		} else if (keyword(line, "CHARS", &v)) {
			break;
		}
	}
	if (!haveasc)
		f->ascent = fbb[1] + fbb[3];
	if (!havedesc)
		f->descent = -fbb[3];
	unicode = unicode_registry(reg, enc);
	(void)unicode;
	if (describeonly) {
		f->numglyphs = 0;
		return f;
	}
	/* glyphs */
	while (next_line(&l, line, sizeof(line))) {
		int code = -1, adv = fbb[0], bbx[4] = {fbb[0], fbb[1], fbb[2], fbb[3]};
		uint8_t *px;
		if (!keyword(line, "STARTCHAR", &v))
			continue;
		while (next_line(&l, line, sizeof(line))) {
			if (keyword(line, "ENCODING", &v)) {
				int alt = -1;
				if (sscanf(v, "%d %d", &code, &alt) == 2 && code < 0)
					code = alt; /* "ENCODING -1 n": a code outside the standard encoding */
			} else if (keyword(line, "DWIDTH", &v)) {
				adv = atoi(v);
			} else if (keyword(line, "BBX", &v)) {
				sscanf(v, "%d %d %d %d", &bbx[0], &bbx[1], &bbx[2], &bbx[3]);
			} else if (keyword(line, "BITMAP", &v) || keyword(line, "ENDCHAR", &v)) {
				break;
			}
		}
		if (!strncmp(line, "ENDCHAR", 7))
			continue;
		if (bbx[0] < 0 || bbx[1] < 0 || bbx[0] > BMF_MAX_DIM || bbx[1] > BMF_MAX_DIM || f->numglyphs >= BMF_MAX_GLYPHS)
			code = -1;
		px = code >= 0 ? add_glyph(&b, code, bbx[0], bbx[1], bbx[2], bbx[3], adv) : NULL;
		/* rows of hex, the leftmost pixel in the high bit of the first byte */
		for (int y = 0; next_line(&l, line, sizeof(line)); y++) {
			if (!strncmp(line, "ENDCHAR", 7))
				break;
			if (!px || y >= bbx[1])
				continue;
			for (int x = 0; x < bbx[0]; x++) {
				int d = hexdigit((unsigned char)line[x / 4]);
				if (d < 0)
					break;
				if (d & (8 >> (x & 3)))
					px[y * bbx[0] + x] = 255;
			}
		}
	}
	return finish(&b, defaultcp);
}

/* --- PCF ------------------------------------------------------------------------- */

#define PCF_PROPERTIES (1 << 0)
#define PCF_ACCELERATORS (1 << 1)
#define PCF_METRICS (1 << 2)
#define PCF_BITMAPS (1 << 3)
#define PCF_BDF_ENCODINGS (1 << 5)
#define PCF_BDF_ACCELERATORS (1 << 8)

#define PCF_COMPRESSED_METRICS 0x100
#define PCF_BYTE_MSB (1 << 2)
#define PCF_BIT_MSB (1 << 3)

typedef struct {
	const uint8_t *data;
	size_t len, pos;
	bool msb;
	bool bad;
} reader_t;

static uint32_t rd32(reader_t *r)
{
	const uint8_t *p;
	if (r->pos + 4 > r->len) {
		r->bad = true;
		return 0;
	}
	p = r->data + r->pos;
	r->pos += 4;
	return r->msb ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
	              : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static int rd16(reader_t *r)
{
	const uint8_t *p;
	if (r->pos + 2 > r->len) {
		r->bad = true;
		return 0;
	}
	p = r->data + r->pos;
	r->pos += 2;
	return (int16_t)(r->msb ? p[0] << 8 | p[1] : p[1] << 8 | p[0]);
}

static int rd8(reader_t *r)
{
	if (r->pos + 1 > r->len) {
		r->bad = true;
		return 0;
	}
	return r->data[r->pos++];
}

typedef struct {
	uint32_t type, format, size, offset;
} pcftable_t;

/* positions a reader at a table; its first word (always little-endian) is
   the table's format, which decides the byte order of the rest */
static bool pcf_table(const uint8_t *data, size_t len, const pcftable_t *tabs, int n, uint32_t type,
                      reader_t *r, uint32_t *format)
{
	for (int i = 0; i < n; i++) {
		if (tabs[i].type != type)
			continue;
		if (tabs[i].offset > len || tabs[i].size > len - tabs[i].offset)
			return false;
		r->data = data;
		r->len = tabs[i].offset + tabs[i].size;
		r->pos = tabs[i].offset;
		r->msb = false;
		r->bad = false;
		*format = rd32(r);
		r->msb = (*format & PCF_BYTE_MSB) != 0;
		return !r->bad;
	}
	return false;
}

static const char *pcf_prop_string(const char *strings, size_t strsize, uint32_t off)
{
	if (off >= strsize || !memchr(strings + off, 0, strsize - off))
		return "";
	return strings + off;
}

static bmfont_t *load_pcf(const uint8_t *data, size_t len, bool describeonly)
{
	builder_t b;
	bmfont_t *f;
	reader_t r = {data, len, 4, false, false};
	pcftable_t tabs[16];
	int ntabs, defaultcp = -1, nmetrics = 0;
	uint32_t format;
	char reg[32] = "", enc[32] = "";
	bool haveasc = false, havedesc = false;
	struct {
		int lsb, rsb, adv, asc, desc;
	} *metrics = NULL;
	int *glyphcode = NULL;

	ntabs = (int)rd32(&r);
	if (r.bad || ntabs <= 0 || ntabs > 64)
		return NULL;
	if (ntabs > (int)BW_ARRAY_COUNT(tabs))
		ntabs = (int)BW_ARRAY_COUNT(tabs);
	for (int i = 0; i < ntabs; i++) {
		tabs[i].type = rd32(&r);
		tabs[i].format = rd32(&r);
		tabs[i].size = rd32(&r);
		tabs[i].offset = rd32(&r);
	}
	if (r.bad)
		return NULL;
	f = new_font(&b, BMF_PCF);

	/* properties: names and string values point into a string pool after
	   the property records */
	if (pcf_table(data, len, tabs, ntabs, PCF_PROPERTIES, &r, &format)) {
		int n = (int)rd32(&r);
		size_t recs = r.pos, strsize;
		const char *strings;
		if (n < 0 || n > 4096)
			n = 0;
		r.pos += (size_t)n * 9;
		if (n & 3)
			r.pos += 4 - (n & 3);
		strsize = rd32(&r);
		strings = (const char *)data + r.pos;
		if (!r.bad && r.pos + strsize <= r.len) {
			r.pos = recs;
			for (int i = 0; i < n; i++) {
				uint32_t name = rd32(&r);
				int isstr = rd8(&r);
				uint32_t value = rd32(&r);
				const char *key = pcf_prop_string(strings, strsize, name);
				const char *sval = isstr ? pcf_prop_string(strings, strsize, value) : "";
				if (!strcmp(key, "FAMILY_NAME") && isstr)
					Str_Copy(f->family, sval, sizeof(f->family));
				else if (!strcmp(key, "WEIGHT_NAME") && isstr)
					f->weight = weight_from_name(sval);
				else if (!strcmp(key, "SLANT") && isstr)
					f->italic = slant_italic(sval);
				else if (!strcmp(key, "PIXEL_SIZE") && !isstr)
					f->pixelsize = (int)value;
				else if (!strcmp(key, "FONT_ASCENT") && !isstr)
					f->ascent = (int)value, haveasc = true;
				else if (!strcmp(key, "FONT_DESCENT") && !isstr)
					f->descent = (int)value, havedesc = true;
				else if (!strcmp(key, "CHARSET_REGISTRY") && isstr)
					Str_Copy(reg, sval, sizeof(reg));
				else if (!strcmp(key, "CHARSET_ENCODING") && isstr)
					Str_Copy(enc, sval, sizeof(enc));
			}
		}
	}
	/* the accelerator tables hold the font's ascent and descent */
	if ((!haveasc || !havedesc) && (pcf_table(data, len, tabs, ntabs, PCF_BDF_ACCELERATORS, &r, &format) ||
	                                pcf_table(data, len, tabs, ntabs, PCF_ACCELERATORS, &r, &format))) {
		int asc, desc;
		r.pos += 8; /* the eight flag bytes */
		asc = (int)rd32(&r);
		desc = (int)rd32(&r);
		if (!r.bad) {
			if (!haveasc)
				f->ascent = asc;
			if (!havedesc)
				f->descent = desc;
		}
	}
	(void)unicode_registry(reg, enc);
	if (describeonly)
		return f;

	/* metrics, one per glyph in file order */
	if (pcf_table(data, len, tabs, ntabs, PCF_METRICS, &r, &format)) {
		bool comp = (format & 0xffffff00) == PCF_COMPRESSED_METRICS;
		nmetrics = comp ? (uint16_t)rd16(&r) : (int)rd32(&r);
		if (nmetrics < 0 || nmetrics > BMF_MAX_GLYPHS)
			nmetrics = 0;
		metrics = Mem_Alloc(sizeof(*metrics) * (size_t)(nmetrics ? nmetrics : 1));
		for (int i = 0; i < nmetrics && !r.bad; i++) {
			if (comp) {
				metrics[i].lsb = rd8(&r) - 0x80;
				metrics[i].rsb = rd8(&r) - 0x80;
				metrics[i].adv = rd8(&r) - 0x80;
				metrics[i].asc = rd8(&r) - 0x80;
				metrics[i].desc = rd8(&r) - 0x80;
			} else {
				metrics[i].lsb = rd16(&r);
				metrics[i].rsb = rd16(&r);
				metrics[i].adv = rd16(&r);
				metrics[i].asc = rd16(&r);
				metrics[i].desc = rd16(&r);
				rd16(&r); /* attributes */
			}
		}
		if (r.bad)
			nmetrics = 0;
	}
	/* encodings: which glyph each character code is */
	glyphcode = Mem_Alloc(sizeof(int) * (size_t)(nmetrics ? nmetrics : 1));
	for (int i = 0; i < nmetrics; i++)
		glyphcode[i] = -1;
	if (nmetrics && pcf_table(data, len, tabs, ntabs, PCF_BDF_ENCODINGS, &r, &format)) {
		int min2 = rd16(&r), max2 = rd16(&r), min1 = rd16(&r), max1 = rd16(&r), def = rd16(&r);
		int cols = max2 - min2 + 1;
		if (!r.bad && cols > 0 && max1 >= min1 && min1 >= 0 && max1 < 256 && min2 >= 0 && max2 < 256) {
			defaultcp = def;
			for (int b1 = min1; b1 <= max1; b1++)
				for (int b2 = min2; b2 <= max2; b2++) {
					int gi = (uint16_t)rd16(&r);
					if (r.bad)
						break;
					if (gi != 0xffff && gi < nmetrics && glyphcode[gi] < 0)
						glyphcode[gi] = b1 << 8 | b2;
				}
		}
	}
	/* bitmaps: rows padded to 1 << (format & 3) bytes, in scan units of
	   1 << (format >> 4 & 3) bytes, with the format's bit and byte order */
	if (nmetrics && pcf_table(data, len, tabs, ntabs, PCF_BITMAPS, &r, &format)) {
		int count = (int)rd32(&r);
		int pad = 1 << (format & 3), unit = 1 << (format >> 4 & 3);
		bool bitmsb = (format & PCF_BIT_MSB) != 0, bytemsb = (format & PCF_BYTE_MSB) != 0;
		size_t offsets, base;
		uint8_t row[BMF_MAX_DIM / 8 + 16];
		if (count > nmetrics || count < 0)
			count = nmetrics;
		offsets = r.pos;
		r.pos += (size_t)count * 4 + 16; /* the offsets, then four bitmap sizes */
		base = r.pos;
		for (int i = 0; i < count && !r.bad; i++) {
			int w = metrics[i].rsb - metrics[i].lsb, h = metrics[i].asc + metrics[i].desc;
			int stride;
			uint32_t off;
			uint8_t *px;
			if (glyphcode[i] < 0 || w < 0 || h < 0 || w > BMF_MAX_DIM || h > BMF_MAX_DIM)
				continue;
			stride = ((w + 7) / 8 + pad - 1) / pad * pad;
			r.pos = offsets + (size_t)i * 4;
			off = rd32(&r);
			if (r.bad || base + off + (size_t)stride * h > r.len)
				continue;
			px = add_glyph(&b, glyphcode[i], w, h, metrics[i].lsb, -metrics[i].desc, metrics[i].adv);
			for (int y = 0; y < h; y++) {
				memcpy(row, data + base + off + (size_t)stride * y, (size_t)stride);
				/* to the most significant byte and bit first */
				if (bytemsb != bitmsb && unit > 1)
					for (int k = 0; k + unit <= stride; k += unit)
						for (int a = 0, z = unit - 1; a < z; a++, z--) {
							uint8_t t = row[k + a];
							row[k + a] = row[k + z];
							row[k + z] = t;
						}
				for (int x = 0; x < w; x++) {
					int byte = row[x >> 3];
					int bit = bitmsb ? 0x80 >> (x & 7) : 1 << (x & 7);
					if (byte & bit)
						px[y * w + x] = 255;
				}
			}
		}
	}
	Mem_Free(metrics);
	Mem_Free(glyphcode);
	return finish(&b, defaultcp);
}

/* --- gzip -------------------------------------------------------------------- */

uint8_t *BMF_Gunzip(const void *data, size_t len, size_t *outlen)
{
	const uint8_t *p = data;
	size_t pos = 10, isize;
	uint8_t *out;
	int flags, n;
	if (len < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8)
		return NULL;
	flags = p[3];
	if (flags & 4) { /* FEXTRA */
		if (pos + 2 > len)
			return NULL;
		pos += 2 + (size_t)(p[pos] | p[pos + 1] << 8);
	}
	if (flags & 8) /* FNAME */
		while (pos < len && p[pos++])
			;
	if (flags & 16) /* FCOMMENT */
		while (pos < len && p[pos++])
			;
	if (flags & 2) /* FHCRC */
		pos += 2;
	if (pos >= len - 8)
		return NULL;
	isize = (size_t)p[len - 4] | (size_t)p[len - 3] << 8 | (size_t)p[len - 2] << 16 | (size_t)p[len - 1] << 24;
	if (!isize || isize > 64u << 20)
		return NULL;
	out = Mem_Alloc(isize + 1);
	n = stbi_zlib_decode_noheader_buffer((char *)out, (int)isize, (const char *)p + pos, (int)(len - 8 - pos));
	if (n != (int)isize) {
		Mem_Free(out);
		return NULL;
	}
	out[isize] = 0;
	*outlen = isize;
	return out;
}

/* --- pixel-grid TrueType ----------------------------------------------------------- */

static int gcd(int a, int b)
{
	a = abs(a);
	b = abs(b);
	while (b) {
		int t = a % b;
		a = b;
		b = t;
	}
	return a;
}

static bool init_ttf(stbtt_fontinfo *info, const void *data, size_t len)
{
	const uint8_t *p = data;
	int off;
	if (len < 12)
		return false;
	/* TrueType outlines only ("\0\1\0\0" or "true"); collections use face 0 */
	if (!(p[0] == 0 && p[1] == 1 && p[2] == 0 && p[3] == 0) && memcmp(p, "true", 4) && memcmp(p, "ttcf", 4))
		return false;
	off = stbtt_GetFontOffsetForIndex(p, 0);
	return off >= 0 && stbtt_InitFont(info, p, off) && info->glyf;
}

/* A pixel font is one whose printable ASCII glyphs are drawn only with
   horizontal and vertical straight lines, every point on a common grid
   coarse enough for whole pixels: a font made of squares, as FontStruct,
   Fontographer pixel fonts and most pixel-font editors make them.

   The grid is the gcd of every coordinate and advance when that is exact.
   Many editors round a grid that doesn't divide the em (2048 units at 9
   pixels: 227.6 units a pixel, stored as 227, 455, 682...), so failing
   that, the smallest whole pixels-per-em from 4 to 64 at which every
   coordinate lands within a font unit (or 2.5% of a pixel) of a pixel
   boundary. Returns
   pixels per em, or 0. */
#define GRID_MAX_COORDS 8192
static float pixel_grid(const stbtt_fontinfo *info, int *upem)
{
	int grid = 0, checked = 0, ncoords = 0, unitsperem = ttUSHORT(info->data + info->head + 18);
	static int coords[GRID_MAX_COORDS];
	float ppem = 0;
	if (upem)
		*upem = unitsperem;
	if (unitsperem <= 0)
		return 0;
	for (int cp = 33; cp < 127; cp++) {
		stbtt_vertex *v;
		int gi = stbtt_FindGlyphIndex(info, cp), n, adv, lsb;
		if (!gi)
			continue;
		n = stbtt_GetGlyphShape(info, gi, &v);
		stbtt_GetGlyphHMetrics(info, gi, &adv, &lsb);
		grid = gcd(grid, adv);
		if (ncoords < GRID_MAX_COORDS)
			coords[ncoords++] = adv;
		for (int i = 0; i < n; i++) {
			if (v[i].type == STBTT_vcurve || v[i].type == STBTT_vcubic ||
			    (v[i].type == STBTT_vline && i > 0 && v[i].x != v[i - 1].x && v[i].y != v[i - 1].y)) {
				STBTT_free(v, NULL);
				return 0; /* a curve or a diagonal */
			}
			grid = gcd(grid, v[i].x);
			grid = gcd(grid, v[i].y);
			if (ncoords + 2 <= GRID_MAX_COORDS) {
				coords[ncoords++] = v[i].x;
				coords[ncoords++] = v[i].y;
			}
		}
		if (n)
			checked++;
		STBTT_free(v, NULL);
	}
	if (checked < 20)
		return 0;
	if (grid > 0 && grid * 64 >= unitsperem)
		return (float)unitsperem / (float)grid;
	for (int p = 4; p <= 64 && !ppem; p++) {
		float unit = (float)unitsperem / (float)p, slack = BW_MAX(1.01f, unit * 0.025f);
		bool fits = true;
		for (int i = 0; i < ncoords && fits; i++) {
			float k = roundf((float)coords[i] / unit);
			fits = fabsf((float)coords[i] - k * unit) <= slack;
		}
		if (fits)
			ppem = (float)p;
	}
	return ppem;
}

float BMF_PixelGrid(const void *data, size_t len, int *upem)
{
	stbtt_fontinfo info;
	if (upem)
		*upem = 0;
	if (!init_ttf(&info, data, len))
		return 0;
	return pixel_grid(&info, upem);
}

/* name id 16 (typographic family), else 1, from a Windows Unicode or Mac Roman record */
static void ttf_family(const stbtt_fontinfo *info, char *out, size_t size)
{
	static const int ids[] = {16, 1};
	out[0] = 0;
	for (size_t k = 0; k < BW_ARRAY_COUNT(ids) && !out[0]; k++) {
		int len = 0;
		const char *s = stbtt_GetFontNameString(info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
		                                        STBTT_MS_LANG_ENGLISH, ids[k]);
		if (s) {
			size_t n = 0;
			for (int i = 0; i + 1 < len && n + 1 < size; i += 2) {
				int c = (unsigned char)s[i] << 8 | (unsigned char)s[i + 1];
				out[n++] = c < 128 ? (char)c : '?';
			}
			out[n] = 0;
			continue;
		}
		s = stbtt_GetFontNameString(info, &len, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN, STBTT_MAC_LANG_ENGLISH,
		                            ids[k]);
		if (s) {
			size_t n = BW_MIN((size_t)len, size - 1);
			memcpy(out, s, n);
			out[n] = 0;
		}
	}
}

static void ttf_style(const stbtt_fontinfo *info, int *weight, bool *italic)
{
	uint32_t os2 = stbtt__find_table(info->data, (uint32_t)info->fontstart, "OS/2");
	*weight = 400;
	*italic = false;
	if (os2) {
		*weight = ttUSHORT(info->data + os2 + 4);
		*italic = (ttUSHORT(info->data + os2 + 62) & 1) != 0;
	}
	if (*weight < 1 || *weight > 1000)
		*weight = 400;
}

static bmfont_t *load_pixel_ttf(const void *data, size_t len)
{
	stbtt_fontinfo info;
	builder_t b;
	bmfont_t *f;
	int upem, asc, desc, gap;
	float ppem, scale;
	uint8_t *scratch;
	if (!init_ttf(&info, data, len) || !(ppem = pixel_grid(&info, &upem)))
		return NULL;
	f = new_font(&b, BMF_TTF);
	ttf_family(&info, f->family, sizeof(f->family));
	ttf_style(&info, &f->weight, &f->italic);
	/* one grid step is one pixel, so every edge falls on a pixel boundary */
	scale = ppem / (float)upem;
	f->pixelsize = (int)lroundf(ppem);
	stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
	f->ascent = (int)lroundf(asc * scale);
	f->descent = (int)lroundf(-desc * scale);
	scratch = Mem_Alloc(BMF_MAX_DIM * BMF_MAX_DIM);
	for (int cp = 0; cp < 0x10000 && f->numglyphs < BMF_MAX_GLYPHS; cp++) {
		int gi = stbtt_FindGlyphIndex(&info, cp), adv, lsb, x0, y0, x1, y1, w, h;
		uint8_t *px;
		if (!gi || cp < 32)
			continue;
		stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
		stbtt_GetGlyphBitmapBox(&info, gi, scale, scale, &x0, &y0, &x1, &y1);
		w = BW_MAX(0, x1 - x0);
		h = BW_MAX(0, y1 - y0);
		if (w > BMF_MAX_DIM || h > BMF_MAX_DIM)
			continue;
		px = add_glyph(&b, cp, w, h, x0, -y1, (int)lroundf(adv * scale));
		if (!w || !h)
			continue;
		stbtt_MakeGlyphBitmap(&info, scratch, w, h, w, scale, scale, gi);
		/* edges on pixel boundaries give full or no coverage; keep the
		   centre of anything in between */
		for (int i = 0; i < w * h; i++)
			px[i] = scratch[i] >= 128 ? 255 : 0;
	}
	Mem_Free(scratch);
	return finish(&b, '?');
}

/* --- entry points ---------------------------------------------------------------- */

bool BMF_IsBitmapName(const char *path)
{
	size_t n = strlen(path);
	return (n > 4 && (!Str_ICmp(path + n - 4, ".bdf") || !Str_ICmp(path + n - 4, ".pcf"))) ||
	       (n > 7 && !Str_ICmp(path + n - 7, ".pcf.gz"));
}

static bmfont_t *load_any(const void *data, size_t len, bool pixelttf, bool describeonly)
{
	const uint8_t *p = data;
	bmfont_t *f = NULL;
	if (len >= 4 && p[0] == 1 && p[1] == 'f' && p[2] == 'c' && p[3] == 'p')
		return load_pcf(p, len, describeonly);
	if (len >= 9 && !memcmp(p, "STARTFONT", 9))
		return load_bdf((const char *)p, len, describeonly);
	if (len >= 2 && p[0] == 0x1f && p[1] == 0x8b) {
		size_t n;
		uint8_t *raw = BMF_Gunzip(data, len, &n);
		if (raw && n >= 4 && raw[0] == 1 && raw[1] == 'f' && raw[2] == 'c' && raw[3] == 'p')
			f = load_pcf(raw, n, describeonly);
		else if (raw && n >= 9 && !memcmp(raw, "STARTFONT", 9))
			f = load_bdf((const char *)raw, n, describeonly);
		Mem_Free(raw);
		return f;
	}
	if (pixelttf)
		return load_pixel_ttf(data, len);
	return NULL;
}

bmfont_t *BMF_Load(const void *data, size_t len, bool pixelttf) { return load_any(data, len, pixelttf, false); }

bool BMF_Describe(const void *data, size_t len, char *family, size_t familysize, int *weight, bool *italic)
{
	stbtt_fontinfo info;
	bmfont_t *f = load_any(data, len, false, true);
	if (f) {
		Str_Copy(family, f->family, familysize);
		*weight = f->weight;
		*italic = f->italic;
		BMF_Free(f);
		return true;
	}
	if (!init_ttf(&info, data, len))
		return false;
	ttf_family(&info, family, familysize);
	ttf_style(&info, weight, italic);
	return true;
}
