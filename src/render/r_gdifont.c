/* Bulwark engine - TrueType text drawn the way Windows GDI draws it.
 *
 * Glue between the 2D text path (r_draw.c) and the GDI-compatible
 * rasterizer in src/client/gdifont.c: resolving face names such as
 * "Verdana" to font files, and caching ExtTextOut-style glyphs in a
 * nearest-sampled atlas per cell height. See docs/spec/gdi-text.md.
 * ISC licence, see LICENSE. */
#include "r_local.h"
#include "../common/fs.h"

#ifdef BW_HAVE_GDIFONT

#include "../client/gdifont.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#define RG_ATLAS 512
#define RG_MAX_SIZES 8
#define RG_HASH 1024 /* glyph slots per size; a power of two */

typedef struct {
	int cp; /* -1: empty slot */
	rgglyph_t g;
} rgslot_t;

struct rgsize_s {
	int tall, quality;
	gdifont_t *font;
	image_t *atlas;
	uint8_t *rgba;      /* CPU copy of the atlas */
	int shelfx, shelfy, shelfh;
	bool dirty;
	int numglyphs;
	rgslot_t slots[RG_HASH];
	uint8_t *scratch;   /* one glyph drawn with gdifont_text_out */
	int scratchw, scratchh, pad;
};

struct rgfont_s {
	char name[128];
	uint8_t *data;
	size_t size;
	int weight;
	rgsize_t *sizes[RG_MAX_SIZES];
	int numsizes;
};

static cvar_t *r_gdifont, *r_gdifont_aa;

void RG_Init(void)
{
	r_gdifont = Cvar_Get("r_gdifont", "1", CVAR_ARCHIVE,
	                     "Draw TrueType fonts the way Windows GDI does (gdifont); 0 uses stb_truetype as before. "
	                     "Applies to fonts loaded afterwards");
	r_gdifont_aa = Cvar_Get("r_gdifont_aa", "1", CVAR_ARCHIVE,
	                        "1: ANTIALIASED_QUALITY (the font's gasp table picks greyscale or 1-bit per size), "
	                        "0: NONANTIALIASED_QUALITY (always 1-bit)");
}

/* --- face names ------------------------------------------------------------ */

typedef struct {
	char family[64];
	int weight, italic;
	char path[512];  /* OS path, or a game path when ingame */
	bool ingame;
} rgentry_t;

static rgentry_t *rg_index;
static int rg_count, rg_cap;
static bool rg_scanned;

static bool font_ext(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot && (!Str_ICmp(dot, ".ttf") || !Str_ICmp(dot, ".ttc") || !Str_ICmp(dot, ".otf"));
}

static void add_entry(const char *path, const void *data, size_t size, bool ingame)
{
	rgentry_t e;
	int tt;
	memset(&e, 0, sizeof(e));
	/* only face 0 of a collection is used */
	if (!gdifont_describe(ingame ? NULL : path, data, size, 0, e.family, sizeof(e.family), &e.weight, &e.italic, &tt) ||
	    !tt || !e.family[0])
		return;
	Str_Copy(e.path, path, sizeof(e.path));
	e.ingame = ingame;
	if (rg_count == rg_cap) {
		rg_cap = rg_cap ? rg_cap * 2 : 64;
		rg_index = Mem_Realloc(rg_index, sizeof(*rg_index) * rg_cap);
	}
	rg_index[rg_count++] = e;
}

static void scan_dir(const char *dir, int depth)
{
	DIR *d;
	struct dirent *de;
	if (depth > 5 || !(d = opendir(dir)))
		return;
	while ((de = readdir(d))) {
		char path[512];
		struct stat st;
		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (stat(path, &st))
			continue;
		if (S_ISDIR(st.st_mode))
			scan_dir(path, depth + 1);
		else if (font_ext(de->d_name))
			add_entry(path, NULL, 0, false);
	}
	closedir(d);
}

static void list_game_font(const char *name, const char *searchpath, void *ctx)
{
	size_t len;
	char *data;
	(void)searchpath;
	(void)ctx;
	if (!font_ext(name) || !(data = FS_LoadFile(name, &len)))
		return;
	add_entry(name, data, len, true);
	Mem_Free(data);
}

/* Every TrueType-outline font the game or the system has, in search order:
   the game's fonts/ folder, the Windows font folders, then the Linux and
   macOS ones. Built once, on the first face-name lookup. */
static void build_index(void)
{
	const char *home = getenv("HOME"), *env;
	char dir[512];
	rg_scanned = true;
	FS_List("fonts/*", true, list_game_font, NULL);
#ifdef _WIN32
	env = getenv("WINDIR");
	snprintf(dir, sizeof(dir), "%s/Fonts", env ? env : "C:/Windows");
	scan_dir(dir, 0);
	if ((env = getenv("LOCALAPPDATA"))) {
		snprintf(dir, sizeof(dir), "%s/Microsoft/Windows/Fonts", env);
		scan_dir(dir, 0);
	}
#else
	if ((env = getenv("XDG_DATA_HOME"))) {
		snprintf(dir, sizeof(dir), "%s/fonts", env);
		scan_dir(dir, 0);
	}
	if (home) {
		snprintf(dir, sizeof(dir), "%s/.local/share/fonts", home);
		scan_dir(dir, 0);
		snprintf(dir, sizeof(dir), "%s/.fonts", home);
		scan_dir(dir, 0);
		snprintf(dir, sizeof(dir), "%s/Library/Fonts", home);
		scan_dir(dir, 0);
	}
	scan_dir("/usr/local/share/fonts", 0);
	scan_dir("/usr/share/fonts", 0);
	scan_dir("/Library/Fonts", 0);
	scan_dir("/System/Library/Fonts", 0);
#endif
	(void)home;
	Con_DPrintf("gdifont: %d TrueType faces indexed\n", rg_count);
}

/* The file for a family: an exact family-name match (case-insensitive),
   upright before italic, then the weight nearest the request, so weight
   >= 600 picks the bold file when there is one. */
static const rgentry_t *find_face(const char *family, int weight)
{
	const rgentry_t *best = NULL;
	int bestscore = 1 << 30;
	if (!rg_scanned)
		build_index();
	for (int i = 0; i < rg_count; i++) {
		const rgentry_t *e = &rg_index[i];
		int score;
		if (Str_ICmp(e->family, family))
			continue;
		score = (e->italic ? 10000 : 0) + abs(e->weight - weight);
		if (score < bestscore) {
			bestscore = score;
			best = e;
		}
	}
	return best;
}

static uint8_t *read_os_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long n;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = n > 0 ? Mem_Alloc((size_t)n) : NULL;
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		Mem_Free(buf);
		buf = NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return buf;
}

/* --- fonts ------------------------------------------------------------------- */

rgfont_t *RG_Load(const char *spec)
{
	static const char *exts[] = {"", ".ttf", ".otf", ".ttc"};
	char path[160], family[64];
	const char *q;
	int weight = 400, tt = 0, w, it;
	bool bare;
	uint8_t *data = NULL;
	size_t len = 0;
	rgfont_t *f;
	gdifont_t *probe;

	if (!r_gdifont || !r_gdifont->value || !spec || !spec[0])
		return NULL;
	/* "Face?weight=700" or "Face?bold" picks a weight */
	Str_Copy(path, spec, sizeof(path));
	if ((q = strchr(spec, '?'))) {
		path[q - spec] = 0;
		if (strstr(q, "bold"))
			weight = 700;
		if (strstr(q, "weight="))
			weight = atoi(strstr(q, "weight=") + 7);
	}

	bare = !strpbrk(path, "/\\.");
	/* A font file named by its path (a mod's own resource/font/x.ttf) stays on
	   stb_truetype unless the path ends in ?gdi. gdifont draws what GDI draws
	   without ClearType, which for ClearType-era faces such as Calibri is
	   bold, cramped text (matched against Windows in CI run 40566970541);
	   a bare face name is the CreateFont-style request GDI rules are for. */
	if (!bare && !(q && strstr(q, "gdi")))
		return NULL;
	/* a file in the game, found the same way the stb_truetype path finds it:
	   gdifont takes it only when it has TrueType outlines */
	for (size_t i = 0; i < BW_ARRAY_COUNT(exts) && !data; i++) {
		char name[200];
		snprintf(name, sizeof(name), "%s%s", path, exts[i]);
		data = (uint8_t *)FS_LoadFile(name, &len);
	}
	if (data) {
		if (!gdifont_describe(NULL, data, len, 0, family, sizeof(family), &w, &it, &tt) || !tt) {
			Mem_Free(data);
			return NULL; /* CFF-flavoured OpenType and the rest stay on stb_truetype */
		}
	} else {
		/* a bare face name such as "Verdana" */
		const rgentry_t *e;
		if (!bare || !(e = find_face(path, weight)))
			return NULL;
		data = e->ingame ? (uint8_t *)FS_LoadFile(e->path, &len) : read_os_file(e->path, &len);
		if (!data)
			return NULL;
		Con_DPrintf("font %s: %s (weight %d)\n", spec, e->path, e->weight);
	}

	/* make sure gdifont accepts it before taking over from stb_truetype */
	if (!(probe = gdifont_create_memory(data, len, 16, weight, 0, GDIFONT_ANTIALIASED, NULL))) {
		Mem_Free(data);
		return NULL;
	}
	gdifont_free(probe);
	f = Mem_Alloc(sizeof(*f));
	memset(f, 0, sizeof(*f));
	Str_Copy(f->name, spec, sizeof(f->name));
	f->data = data;
	f->size = len;
	f->weight = weight;
	return f;
}

static void free_size(rgsize_t *s)
{
	gdifont_free(s->font);
	Mem_Free(s->rgba);
	Mem_Free(s->scratch);
	Mem_Free(s);
}

void RG_Free(rgfont_t *f)
{
	if (!f)
		return;
	for (int i = 0; i < f->numsizes; i++)
		free_size(f->sizes[i]);
	Mem_Free(f->data);
	Mem_Free(f);
}

rgsize_t *RG_Size(rgfont_t *f, int tall)
{
	int quality = r_gdifont_aa && !r_gdifont_aa->value ? GDIFONT_NONANTIALIASED : GDIFONT_ANTIALIASED;
	const gdifont_metrics_t *tm;
	rgsize_t *s;
	char name[192];
	if (tall < 1)
		tall = 1;
	if (tall > 256)
		tall = 256;
	for (int i = 0; i < f->numsizes; i++)
		if (f->sizes[i]->tall == tall && f->sizes[i]->quality == quality)
			return f->sizes[i];
	if (f->numsizes == RG_MAX_SIZES) {
		/* drop the oldest */
		free_size(f->sizes[0]);
		memmove(f->sizes, f->sizes + 1, sizeof(f->sizes[0]) * (RG_MAX_SIZES - 1));
		f->numsizes--;
	}
	s = Mem_Alloc(sizeof(*s));
	memset(s, 0, sizeof(*s));
	s->tall = tall;
	s->quality = quality;
	/* §3: tall is the CreateFont cell height */
	s->font = gdifont_create_memory(f->data, f->size, tall, f->weight, 0, quality, NULL);
	if (!s->font) {
		Mem_Free(s);
		return NULL;
	}
	for (int i = 0; i < RG_HASH; i++)
		s->slots[i].cp = -1;
	tm = gdifont_metrics(s->font);
	s->pad = tall * 2;
	s->scratchw = tm->max_char_width + s->pad * 2;
	s->scratchh = tm->height + s->pad * 2;
	s->scratch = Mem_Alloc((size_t)s->scratchw * s->scratchh);
	s->rgba = Mem_Alloc(RG_ATLAS * RG_ATLAS * 4);
	for (int i = 0; i < RG_ATLAS * RG_ATLAS; i++) {
		s->rgba[i * 4] = s->rgba[i * 4 + 1] = s->rgba[i * 4 + 2] = 255;
		s->rgba[i * 4 + 3] = 0;
	}
	s->shelfx = s->shelfy = 1;
	snprintf(name, sizeof(name), "$gdifont:%s:%d:%d", f->name, tall, quality);
	s->atlas = R_ImageFromPixels(name, s->rgba, RG_ATLAS, RG_ATLAS, IMG_NOMIP | IMG_CLAMP | IMG_NEAREST);
	f->sizes[f->numsizes++] = s;
	return s;
}

int RG_Ascent(const rgsize_t *s) { return gdifont_metrics(s->font)->ascent; }
image_t *RG_Atlas(const rgsize_t *s) { return s->atlas; }

/* One glyph as ExtTextOut draws it: drawn on its own with gdifont_text_out
   (pen at the cell's top left), cropped to its ink and packed. */
const rgglyph_t *RG_Glyph(rgsize_t *s, int cp)
{
	unsigned h = ((unsigned)cp * 2654435761u) & (RG_HASH - 1);
	rgslot_t *slot;
	rgglyph_t *g;
	unsigned u = (unsigned)cp;
	int minx = s->scratchw, miny = s->scratchh, maxx = -1, maxy = -1;
	for (;; h = (h + 1) & (RG_HASH - 1)) {
		slot = &s->slots[h];
		if (slot->cp == cp)
			return &slot->g;
		if (slot->cp < 0)
			break;
	}
	if (s->numglyphs >= RG_HASH * 3 / 4)
		return NULL;
	slot->cp = cp;
	s->numglyphs++;
	g = &slot->g;
	memset(g, 0, sizeof(*g));
	if (cp < 32)
		return g; /* control characters: nothing, no advance */
	memset(s->scratch, 0, (size_t)s->scratchw * s->scratchh);
	g->adv = gdifont_text_out(s->font, s->pad, s->pad, &u, 1, s->scratch, s->scratchw, s->scratchh, s->scratchw);
	for (int y = 0; y < s->scratchh; y++)
		for (int x = 0; x < s->scratchw; x++)
			if (s->scratch[y * s->scratchw + x]) {
				minx = BW_MIN(minx, x);
				maxx = BW_MAX(maxx, x);
				miny = BW_MIN(miny, y);
				maxy = BW_MAX(maxy, y);
			}
	if (maxx < 0)
		return g;
	g->w = maxx - minx + 1;
	g->h = maxy - miny + 1;
	g->left = minx - s->pad;
	g->top = miny - s->pad;
	/* shelf packing with a pixel of space around each glyph */
	if (s->shelfx + g->w + 1 > RG_ATLAS) {
		s->shelfx = 1;
		s->shelfy += s->shelfh + 1;
		s->shelfh = 0;
	}
	if (s->shelfy + g->h + 1 > RG_ATLAS) {
		g->w = g->h = 0; /* atlas full: advance only */
		return g;
	}
	g->x = s->shelfx;
	g->y = s->shelfy;
	s->shelfx += g->w + 1;
	s->shelfh = BW_MAX(s->shelfh, g->h);
	for (int y = 0; y < g->h; y++)
		for (int x = 0; x < g->w; x++)
			s->rgba[((g->y + y) * RG_ATLAS + g->x + x) * 4 + 3] = s->scratch[(miny + y) * s->scratchw + minx + x];
	s->dirty = true;
	return g;
}

void RG_Upload(rgsize_t *s)
{
	if (!s->dirty)
		return;
	R_Flush2D();
	s->atlas = R_ImageFromPixels(s->atlas->name, s->rgba, RG_ATLAS, RG_ATLAS, IMG_NOMIP | IMG_CLAMP | IMG_NEAREST);
	s->dirty = false;
}

int RG_AtlasSize(void) { return RG_ATLAS; }

#else /* no FreeType: everything stays on stb_truetype */

void RG_Init(void) {}
rgfont_t *RG_Load(const char *spec) { (void)spec; return NULL; }
void RG_Free(rgfont_t *f) { (void)f; }
rgsize_t *RG_Size(rgfont_t *f, int tall) { (void)f; (void)tall; return NULL; }
int RG_Ascent(const rgsize_t *s) { (void)s; return 0; }
image_t *RG_Atlas(const rgsize_t *s) { (void)s; return NULL; }
const rgglyph_t *RG_Glyph(rgsize_t *s, int cp) { (void)s; (void)cp; return NULL; }
void RG_Upload(rgsize_t *s) { (void)s; }
int RG_AtlasSize(void) { return 1; }

#endif
