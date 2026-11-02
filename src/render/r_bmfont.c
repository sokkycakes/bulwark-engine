/* Bulwark engine - bitmap fonts in 2D text: BDF, PCF and pixel TrueType
 * fonts (src/common/bmfont.c) drawn crisp.
 *
 * A bitmap font only has its own pixel size, so text is drawn at a whole
 * multiple of it, the one nearest the size asked for (never below 1x): every
 * font pixel becomes a k-by-k block of screen pixels placed on whole screen
 * pixels from a nearest-sampled atlas. Font names resolve like TrueType face
 * names: "anorexia", "Visitor TT1 BRK" or the file's own name ("visitor1")
 * from the fonts/ folders of the game and of Bulwark's data directory. See
 * docs/spec/bitmap-fonts.md. ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/bmfont.h"
#include "../common/cmd.h"
#include "../common/fs.h"

#include <stdio.h>
#include <stdlib.h>

#define BF_ATLAS 512
#define BF_MAX_SIZES 6
#define BF_HASH 1024 /* glyph slots per size; a power of two */
#define BF_SCAN_LIMIT (2u << 20) /* TrueType files bigger than this aren't checked for a pixel grid */

typedef struct {
	int cp; /* -1: empty slot */
	rgglyph_t g;
} bfslot_t;

struct bfsize_s {
	int k; /* screen pixels per font pixel */
	image_t *atlas;
	uint8_t *rgba;
	int shelfx, shelfy, shelfh;
	bool dirty;
	int numglyphs;
	bfslot_t slots[BF_HASH];
};

struct bffont_s {
	char name[160];
	bmfont_t *bmf;
	bfsize_t *sizes[BF_MAX_SIZES];
	int numsizes;
};

static cvar_t *r_pixelfonts, *r_bitmapfont_scale;

static void Bitmapfonts_f(int argc, const char **argv);

void BF_Init(void)
{
	r_pixelfonts = Cvar_Get("r_pixelfonts", "1", CVAR_ARCHIVE,
	                        "Draw TrueType fonts made of square pixels (outlines on a pixel grid) as bitmap fonts: "
	                        "crisp, at whole multiples of their pixel size. Applies to fonts loaded afterwards");
	r_bitmapfont_scale = Cvar_Get("r_bitmapfont_scale", "0", CVAR_ARCHIVE,
	                              "Bitmap and pixel fonts: 0 draws at the whole multiple of the font's own size "
	                              "nearest the size asked for; N > 0 always draws at N times");
	Cmd_AddD("bitmapfonts", Bitmapfonts_f,
	         "bitmapfonts [name]: the bitmap and pixel fonts in fonts/ that font names can resolve to");
}

bool BF_PixelFontsEnabled(void) { return !r_pixelfonts || r_pixelfonts->value; }

/* --- names ------------------------------------------------------------------------ */

typedef struct {
	char family[64];
	char stem[64]; /* the file name without its extension(s) */
	int weight;
	bool italic;
	char path[160];
} bfentry_t;

static bfentry_t *bf_index;
static int bf_count, bf_cap;
static int bf_gen = -1, bf_pixel = -1;

static void file_stem(const char *path, char *out, size_t size)
{
	const char *base = strrchr(path, '/'), *dot;
	base = base ? base + 1 : path;
	Str_Copy(out, base, size);
	if (strlen(out) > 7 && !Str_ICmp(out + strlen(out) - 7, ".pcf.gz"))
		out[strlen(out) - 7] = 0;
	else if ((dot = strrchr(out, '.')))
		out[dot - out] = 0;
}

static bool ttf_name(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot && (!Str_ICmp(dot, ".ttf") || !Str_ICmp(dot, ".otf") || !Str_ICmp(dot, ".ttc"));
}

static void list_font(const char *name, const char *searchpath, void *ctx)
{
	bfentry_t e;
	size_t len;
	uint8_t *data;
	bool bitmap = BMF_IsBitmapName(name);
	(void)searchpath;
	(void)ctx;
	if (!bitmap && !(ttf_name(name) && BF_PixelFontsEnabled()))
		return;
	if (!(data = (uint8_t *)FS_LoadFile(name, &len)))
		return;
	memset(&e, 0, sizeof(e));
	/* a TrueType font only counts when it is drawn on a pixel grid */
	if ((bitmap || (len <= BF_SCAN_LIMIT && BMF_PixelGrid(data, len, NULL) > 0)) &&
	    BMF_Describe(data, len, e.family, sizeof(e.family), &e.weight, &e.italic)) {
		file_stem(name, e.stem, sizeof(e.stem));
		Str_Copy(e.path, name, sizeof(e.path));
		if (bf_count == bf_cap) {
			bf_cap = bf_cap ? bf_cap * 2 : 32;
			bf_index = Mem_Realloc(bf_index, sizeof(*bf_index) * (size_t)bf_cap);
		}
		bf_index[bf_count++] = e;
	}
	Mem_Free(data);
}

/* listed again whenever the search path changes, like the TrueType faces */
static void update_index(void)
{
	int pixel = BF_PixelFontsEnabled() ? 1 : 0;
	if (bf_gen == FS_Generation() && bf_pixel == pixel)
		return;
	bf_count = 0;
	bf_gen = FS_Generation();
	bf_pixel = pixel;
	FS_List("fonts/*", true, list_font, NULL);
	Con_DPrintf("bitmapfonts: %d bitmap and pixel fonts indexed\n", bf_count);
}

/* a family name or file name, upright first, then the nearest weight */
static const bfentry_t *find_font(const char *name, int weight)
{
	const bfentry_t *best = NULL;
	int bestscore = 1 << 30;
	update_index();
	for (int i = 0; i < bf_count; i++) {
		const bfentry_t *e = &bf_index[i];
		int score;
		if (!Str_ICmp(e->stem, name))
			return e; /* a file named exactly */
		if (Str_ICmp(e->family, name))
			continue;
		score = (e->italic ? 10000 : 0) + abs(e->weight - weight);
		if (score < bestscore) {
			bestscore = score;
			best = e;
		}
	}
	return best;
}

static void Bitmapfonts_f(int argc, const char **argv)
{
	int n = 0;
	update_index();
	for (int i = 0; i < bf_count; i++) {
		const bfentry_t *e = &bf_index[i];
		if (argc > 1 && Str_ICmp(e->family, argv[1]) && Str_ICmp(e->stem, argv[1]))
			continue;
		Con_Printf("  \"%s\" (%s) weight %d%s: %s\n", e->family, e->stem, e->weight, e->italic ? " italic" : "",
		           e->path);
		n++;
	}
	if (argc > 1) {
		const bfentry_t *e = find_font(argv[1], 400);
		Con_Printf("%s resolves to %s\n", argv[1], e ? e->path : "nothing");
	}
	Con_Printf("%d fonts%s\n", n, BF_PixelFontsEnabled() ? "" : " (r_pixelfonts 0: TrueType pixel fonts left out)");
}

/* --- fonts ---------------------------------------------------------------------------- */

/* A font list entry: "anorexia" (a name), "fonts/x.bdf", "fonts/x.pcf.gz", or
   a TrueType file that turns out to be a pixel font. "?bold" or
   "?weight=N" picks a weight as for TrueType names. */
bffont_t *BF_Load(const char *spec)
{
	static const char *exts[] = {"", ".bdf", ".pcf", ".pcf.gz", ".ttf"};
	char path[160];
	const char *q;
	int weight = 400;
	uint8_t *data = NULL;
	size_t len = 0;
	bmfont_t *bmf = NULL;
	bffont_t *f;
	bool bare;
	if (!spec || !spec[0])
		return NULL;
	Str_Copy(path, spec, sizeof(path));
	if ((q = strchr(spec, '?'))) {
		path[q - spec] = 0;
		if (strstr(q, "bold"))
			weight = 700;
		if (strstr(q, "weight="))
			weight = atoi(strstr(q, "weight=") + 7);
	}
	bare = !strpbrk(path, "/\\.");
	if (bare) {
		const bfentry_t *e = find_font(path, weight);
		if (!e)
			return NULL;
		data = (uint8_t *)FS_LoadFile(e->path, &len);
	} else {
		bool bitmapname = BMF_IsBitmapName(path);
		/* only check TrueType files when pixel fonts are wanted */
		if (!bitmapname && !(BF_PixelFontsEnabled() && (ttf_name(path) || !strrchr(path, '.'))))
			return NULL;
		for (size_t i = 0; i < BW_ARRAY_COUNT(exts) && !data; i++) {
			char name[200];
			if (bitmapname && i)
				break;
			snprintf(name, sizeof(name), "%s%s", path, exts[i]);
			data = (uint8_t *)FS_LoadFile(name, &len);
			if (data && !BMF_IsBitmapName(name) && (len > BF_SCAN_LIMIT || !BMF_PixelGrid(data, len, NULL))) {
				Mem_Free(data); /* an ordinary TrueType font */
				data = NULL;
			}
		}
	}
	if (data) {
		bmf = BMF_Load(data, len, BF_PixelFontsEnabled());
		Mem_Free(data);
	}
	if (!bmf)
		return NULL;
	f = Mem_Alloc(sizeof(*f));
	memset(f, 0, sizeof(*f));
	Str_Copy(f->name, spec, sizeof(f->name));
	f->bmf = bmf;
	Con_DPrintf("font %s: bitmap font \"%s\", %d pixels, %d glyphs\n", spec, bmf->family, bmf->ascent + bmf->descent,
	            bmf->numglyphs);
	return f;
}

static void free_size(bfsize_t *s)
{
	Mem_Free(s->rgba);
	Mem_Free(s);
}

void BF_Free(bffont_t *f)
{
	if (!f)
		return;
	for (int i = 0; i < f->numsizes; i++)
		free_size(f->sizes[i]);
	BMF_Free(f->bmf);
	Mem_Free(f);
}

int BF_Cell(const bffont_t *f) { return BW_MAX(1, f->bmf->ascent + f->bmf->descent); }

/* the multiple of the font's pixel size that text asked for at tall screen
   pixels is drawn at */
int BF_Scale(const bffont_t *f, int tall)
{
	int fixed = r_bitmapfont_scale ? (int)r_bitmapfont_scale->value : 0;
	if (fixed > 0)
		return BW_MIN(fixed, 16);
	return BW_CLAMP((tall + BF_Cell(f) / 2) / BF_Cell(f), 1, 16);
}

bfsize_t *BF_Size(bffont_t *f, int k)
{
	bfsize_t *s;
	char name[200];
	for (int i = 0; i < f->numsizes; i++)
		if (f->sizes[i]->k == k)
			return f->sizes[i];
	if (f->numsizes == BF_MAX_SIZES) {
		free_size(f->sizes[0]);
		memmove(f->sizes, f->sizes + 1, sizeof(f->sizes[0]) * (BF_MAX_SIZES - 1));
		f->numsizes--;
	}
	s = Mem_Alloc(sizeof(*s));
	memset(s, 0, sizeof(*s));
	s->k = k;
	for (int i = 0; i < BF_HASH; i++)
		s->slots[i].cp = -1;
	s->rgba = Mem_Alloc(BF_ATLAS * BF_ATLAS * 4);
	for (int i = 0; i < BF_ATLAS * BF_ATLAS; i++) {
		s->rgba[i * 4] = s->rgba[i * 4 + 1] = s->rgba[i * 4 + 2] = 255;
		s->rgba[i * 4 + 3] = 0;
	}
	s->shelfx = s->shelfy = 1;
	snprintf(name, sizeof(name), "$bmfont:%s:%d", f->name, k);
	s->atlas = R_ImageFromPixels(name, s->rgba, BF_ATLAS, BF_ATLAS, IMG_NOMIP | IMG_CLAMP | IMG_NEAREST);
	f->sizes[f->numsizes++] = s;
	return s;
}

image_t *BF_Atlas(const bfsize_t *s) { return s->atlas; }
int BF_AtlasSize(void) { return BF_ATLAS; }

/* One glyph scaled k times, packed into the size's atlas. Offsets are from
   the pen and the top of the cell, as for gdifont glyphs. */
const rgglyph_t *BF_Glyph(bffont_t *f, bfsize_t *s, int cp)
{
	unsigned h = ((unsigned)cp * 2654435761u) & (BF_HASH - 1);
	const bmglyph_t *src;
	bfslot_t *slot;
	rgglyph_t *g;
	int k = s->k;
	for (;; h = (h + 1) & (BF_HASH - 1)) {
		slot = &s->slots[h];
		if (slot->cp == cp)
			return &slot->g;
		if (slot->cp < 0)
			break;
	}
	if (s->numglyphs >= BF_HASH * 3 / 4)
		return NULL;
	slot->cp = cp;
	s->numglyphs++;
	g = &slot->g;
	memset(g, 0, sizeof(*g));
	if (cp < 32 || !(src = BMF_Glyph(f->bmf, cp)))
		return g;
	g->adv = src->adv * k;
	g->left = src->xoff * k;
	g->top = (f->bmf->ascent - (src->yoff + src->h)) * k;
	g->w = src->w * k;
	g->h = src->h * k;
	if (!g->w || !g->h)
		return g;
	if (s->shelfx + g->w + 1 > BF_ATLAS) {
		s->shelfx = 1;
		s->shelfy += s->shelfh + 1;
		s->shelfh = 0;
	}
	if (s->shelfy + g->h + 1 > BF_ATLAS) {
		g->w = g->h = 0; /* atlas full: advance only */
		return g;
	}
	g->x = s->shelfx;
	g->y = s->shelfy;
	s->shelfx += g->w + 1;
	s->shelfh = BW_MAX(s->shelfh, g->h);
	for (int y = 0; y < g->h; y++)
		for (int x = 0; x < g->w; x++)
			s->rgba[((g->y + y) * BF_ATLAS + g->x + x) * 4 + 3] =
			    f->bmf->pixels[src->bits + (size_t)(y / k) * src->w + x / k];
	s->dirty = true;
	return g;
}

void BF_Upload(bfsize_t *s)
{
	if (!s->dirty)
		return;
	R_Flush2D();
	s->atlas = R_ImageFromPixels(s->atlas->name, s->rgba, BF_ATLAS, BF_ATLAS, IMG_NOMIP | IMG_CLAMP | IMG_NEAREST);
	s->dirty = false;
}
