/* Bulwark engine - the DS link's HUD on the top screen. ISC licence, see LICENSE.
 *
 * What the PC shows over the view in co-op, condensed for 256x192: Nuclide's
 * coop/src/hud/jtf.qc gathers the HUD's state from infokeys and posts it to
 * its Panorama view (coop/panorama/layout/coop/hud.xml, scripts/coop/hud.js,
 * styles/coop/hud.css), and keeps the Half-Life crosshair on top
 * (Jtf_DrawFeedback, valve/src/hud/hud.qc); src/client/nameplates.qc draws
 * the teammates' nameplates and finds the target ID. The server sends those
 * infokeys, the players, the crosshair and the obituaries in HUDINFO
 * (docs/spec/link.md); this file runs hud.js's rules on them.
 *
 * Sizes: the Panorama view is laid out 900 units tall and the PC's 2D canvas
 * 480 rows (vid_conheight), so things placed against the screen keep their
 * place at 192/900 (PY) and the crosshair its size at 192/480. Text can't
 * shrink with them (the PC's 12 px Tahoma would be under 3 rows), so it is
 * the HUD font (5 rows) and panels are as wide as their text needs.
 *
 * Drawing: BG1 is a 4-bit bitmap in front of the 3D (768 tiles, one for each
 * 8x8 cell). Each part of the HUD (a widget) writes what it shows as a short
 * list of fills and texts each frame; only a list that differs from the last
 * frame's is drawn again (its old box cleared first), with whatever overlaps
 * either box drawn again on top in order. Nothing is allocated per frame. */
#include <math.h>
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hud_nds.h"

extern const u8 default_font_bin[]; /* libnds' console font: 256 characters, 8x8, 1 bit */

/* colours: BG palette 0 entries (SokkyDark, data/panorama/styles/qt/qtcurve-sokky-dark.css,
 * and hud.css's game colours) */
enum {
	HC_CLEAR, HC_EDGE, HC_TEXT, HC_RED, HC_DIAG, HC_MUTED, HC_AMBER, HC_ACCENT,
	HC_BARRIER, HC_GOLD, HC_GREEN, HC_WINDOW, HC_FRAME, HC_TROUGH, HC_FILL, HC_XHAIR
};
#define TOP_TILES 768
#define PY(v) ((v) * 192 / 900) /* Panorama layout units (900 tall) to rows */

static uint16_t *topgfx;
static uint8_t *hudfont; /* the .dsf, or NULL for the built-in font */
static int hudh = 8;     /* its rows */

/* --- pixels --------------------------------------------------------------------------- */

static void hud_load_font(void)
{
	size_t size;
	uint8_t *f = Fs_Load("fonts/hud.dsf", &size);
	hudfont = NULL, hudh = 8;
	if (f && size >= 8 && !memcmp(f, "DSF1", 4) && f[4] && f[4] <= 16 && size >= 8 + (size_t)f[6] * (2 + f[4])) {
		hudfont = f, hudh = f[4];
		return;
	}
	free(f);
}

/* a glyph's rows (bit 0 the left pixel), width and advance */
static const uint8_t *hud_glyph(int c, int *w, int *adv)
{
	if (hudfont) {
		int first = hudfont[5], count = hudfont[6];
		if (c < first || c >= first + count)
			c = '?';
		if (c >= first && c < first + count) {
			const uint8_t *g = hudfont + 8 + (c - first) * (2 + hudh);
			*adv = g[0], *w = g[1];
			return g + 2;
		}
		*adv = *w = 0;
		return NULL;
	}
	*adv = *w = 8;
	return default_font_bin + (c & 255) * 8;
}

/* Drawing goes to one row of tiles in RAM (8 rows of the screen, 1 KB), clipped
 * to the columns being redrawn, and is copied to VRAM a tile at a time */
static uint8_t strip[32 * 32] __attribute__((aligned(4)));
static int clipx0, clipx1, clipy0, clipy1; /* the part of the screen being drawn */

static inline void top_px(int x, int y, int col)
{
	uint8_t *p;
	if (x < clipx0 || x >= clipx1 || y < clipy0 || y >= clipy1)
		return;
	p = strip + (x >> 3) * 32 + (y & 7) * 4 + ((x & 7) >> 1);
	*p = (uint8_t)(x & 1 ? (*p & 0x0F) | col << 4 : (*p & 0xF0) | col);
}

/* col | FILL_DITHER: every other pixel, a checkerboard */
#define FILL_DITHER 0x10
static void top_fill(int x, int y, int w, int h, int col)
{
	int x1 = BW_NDS_MIN(x + w, clipx1), y1 = BW_NDS_MIN(y + h, clipy1), step = col & FILL_DITHER ? 2 : 1;
	x = BW_NDS_MAX(x, clipx0), y = BW_NDS_MAX(y, clipy0), col &= 15;
	for (int j = y; j < y1; j++)
		for (int i = step == 2 ? x + ((x + j + 1) & 1) : x; i < x1; i += step)
			top_px(i, j, col);
}

static int text_width(const char *s, int len, int scale)
{
	int n = 0, w, adv;
	for (int i = 0; i < len && s[i]; i++)
		hud_glyph((uint8_t)s[i], &w, &adv), n += adv * scale;
	return n;
}

/* text at (x, y), each font pixel scale x scale, with a 1-pixel outline unless bare */
static void draw_text(int x, int y, int scale, int col, const char *s, int len, bool bare)
{
	for (int pass = bare ? 1 : 0; pass < 2; pass++) {
		int cx = x;
		for (int k = 0; k < len; k++) {
			int w, adv;
			const uint8_t *g = hud_glyph((uint8_t)s[k], &w, &adv);
			for (int j = 0; g && j < hudh; j++) {
				if (y + (j + 1) * scale + 1 <= clipy0 || y + j * scale - 1 >= clipy1)
					continue; /* outside the rows being drawn */
				for (int i = 0; i < w; i++)
					if (g[j] & (1 << i)) {
						if (pass == 0)
							top_fill(cx + i * scale - 1, y + j * scale - 1, scale + 2, scale + 2, HC_EDGE);
						else if (scale == 1)
							top_px(cx + i, y + j, col);
						else
							top_fill(cx + i * scale, y + j * scale, scale, scale, col);
					}
			}
			cx += adv * scale;
		}
	}
}

static void draw_line(int x0, int y0, int x1, int y1, int col)
{
	if (BW_NDS_MAX(y0, y1) < clipy0 || BW_NDS_MIN(y0, y1) >= clipy1)
		return;
	int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
	for (int n = 0; n < 512; n++) {
		top_px(x0, y0, col);
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * err;
		if (e2 >= dy)
			err += dy, x0 += sx;
		if (e2 <= dx)
			err += dx, y0 += sy;
	}
}

/* --- the state HUDINFO brings ----------------------------------------------------------- */

enum {
	HK_HEROHUD, HK_RESPAWN, HK_REVIVE, HK_PROMPT, HK_PROMPTAT, HK_USABLE, HK_STATS, HK_XPM, HK_CADENCE, HK_CADENCEN,
	HK_WAVE = 16, HK_LEFT, HK_NOTICE, HK_FEED, HK_ENGAGECOST, HK_NAMEPLATES, HK_TARGETID,
	HK_WTITLE = 32, HK_STATICNAME, HK_DYNAMICNAME, HK_NOAMMO, HK_COUNT
};
#define MAX_PLAYERS 32
#define MAX_NPC 8
#define NAMELEN 24
#define FEED_MAX 5
#define FEED_MS 6000   /* hud.js: five lines, six seconds each */
#define NOTICE_MS 3000 /* jtf.qc's "hold":3 */
#define SLOTS_MS 2000  /* hud.js: weapon slots show for 2 s after a change */
#define LINGER_MS 500  /* nameplates.qc cg_targetIDLinger */
#define XH_MAX 16      /* the crosshair at 192/480 of a sprite up to 32 */

typedef struct {
	bool used;
	int16_t health;
	char name[NAMELEN], mate[16], team[8];
	uint8_t cad;
} hplayer_t;

typedef struct {
	char a[NAMELEN], w[NAMELEN], v[NAMELEN];
	bool you;
	uint32_t born; /* our ms clock */
} feed_t;

static struct {
	bool have;    /* a HUDINFO came: the server sends the co-op HUD's state */
	uint16_t version;
	int self;     /* our player slot */
	char keys[HK_COUNT][96];
	char herohud[256];            /* the one longer than 95 */
	hplayer_t pl[MAX_PLAYERS];
	struct {
		uint16_t ent;
		int16_t health, max;
		char name[NAMELEN];
	} npc[MAX_NPC];
	int numnpc;
	uint8_t xw, xh;               /* the crosshair as drawn here, 0 none */
	uint16_t xrgb;
	uint16_t xmask[XH_MAX];       /* row bits, bit 0 the left */
	bool obitinit, feedinit, noticeinit;
	uint8_t obitseq;
	int feedseq, noticeseq;
	feed_t feed[FEED_MAX];        /* newest first */
	char ntitle[48], nsub[64];
	uint32_t nborn;               /* our ms clock; 0 none up */
	char wkey[160];               /* hud.js weaponKey */
	bool wkeyinit;
	uint32_t slotsuntil;
	/* the target ID (nameplates.qc g_targetID*) */
	int tident;
	char tidname[NAMELEN];
	int tidhealth, tidmax, tidcad;
	uint32_t tidseen;
	bool tidany;
} hud;

static uint32_t now_ms(void) { return (uint32_t)Nds_Vblanks() * 1000u / 60u; }

static void copy_n(char *dst, int size, const uint8_t *src, int len)
{
	int n = 0;
	while (n < len && n < size - 1 && src[n])
		dst[n] = (char)src[n], n++;
	dst[n] = 0;
}

/* a length-prefixed string at *pos; false when past the end */
static bool rd_str(const uint8_t *d, int len, int *pos, char *out, int size)
{
	int n;
	if (*pos >= len || *pos + 1 + (n = d[*pos]) > len)
		return false;
	copy_n(out, size, d + *pos + 1, n);
	*pos += 1 + n;
	return true;
}

static void feed_push(const char *a, const char *w, const char *v, bool you)
{
	memmove(&hud.feed[1], &hud.feed[0], sizeof(hud.feed[0]) * (FEED_MAX - 1));
	snprintf(hud.feed[0].a, NAMELEN, "%s", a);
	snprintf(hud.feed[0].w, NAMELEN, "%s", w);
	snprintf(hud.feed[0].v, NAMELEN, "%s", v);
	hud.feed[0].you = you;
	hud.feed[0].born = now_ms() | 1;
}

static const char *my_name(void) { return hud.self >= 0 && hud.self < MAX_PLAYERS ? hud.pl[hud.self].name : ""; }

/* jtf.qc HUD_Obituary: players' deaths only */
static void obituary(const char *target, const char *attacker, const char *weapon, const char *how)
{
	bool player = false, self;
	const char *me = my_name();
	for (int i = 0; i < MAX_PLAYERS && target[0]; i++)
		if (hud.pl[i].used && !strcmp(hud.pl[i].name, target))
			player = true;
	if (!player)
		return;
	/* wordInString(meansOfDeath, "self") */
	self = false;
	for (const char *p = how; *p;) {
		while (*p == ' ')
			p++;
		if (!strncmp(p, "self", 4) && (p[4] == ' ' || !p[4]))
			self = true;
		while (*p && *p != ' ')
			p++;
	}
	if (self)
		attacker = "";
	if (!attacker[0])
		feed_push(target, "died", "", !strcmp(target, me));
	else
		feed_push(attacker, weapon, target, !strcmp(attacker, me) || !strcmp(target, me));
}

/* the server's crosshair (its size on the PC's 480-row canvas) at 192/480, a pixel
 * set where any of the sprite's pixels it covers is */
static void set_crosshair(int w, int h, uint16_t rgb, const uint8_t *mask)
{
	int dw = BW_NDS_CLAMP((w * 2 + 4) / 5, 1, XH_MAX), dh = BW_NDS_CLAMP((h * 2 + 4) / 5, 1, XH_MAX);
	memset(hud.xmask, 0, sizeof(hud.xmask));
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			if (mask[(y * w + x) >> 3] & (1 << ((y * w + x) & 7)))
				hud.xmask[BW_NDS_MIN(y * dh / h, XH_MAX - 1)] |= (uint16_t)(1 << BW_NDS_MIN(x * dw / w, XH_MAX - 1));
	hud.xw = (uint8_t)dw, hud.xh = (uint8_t)dh, hud.xrgb = rgb;
	BG_PALETTE[HC_XHAIR] = rgb;
}

void Hud_Packet(const uint8_t *d, int len)
{
	int pos = 7, n;
	char key[96], s[4][NAMELEN];
	if (len < 8)
		return;
	if ((uint16_t)(d[4] | d[5] << 8) == hud.version && hud.have)
		return;
	hud.version = (uint16_t)(d[4] | d[5] << 8);
	hud.self = d[6];
	for (int k = 0; k < HK_COUNT; k++)
		hud.keys[k][0] = 0;
	hud.herohud[0] = 0;
	n = d[pos++];
	for (int i = 0; i < n; i++) {
		int id, at;
		if (pos >= len)
			return;
		id = d[pos++];
		at = pos;
		if (!rd_str(d, len, &pos, key, sizeof(key)))
			return;
		if (id == HK_HEROHUD)
			copy_n(hud.herohud, sizeof(hud.herohud), d + at + 1, d[at]);
		if (id < HK_COUNT)
			memcpy(hud.keys[id], key, sizeof(key));
	}
	memset(hud.pl, 0, sizeof(hud.pl));
	if (pos >= len)
		return;
	n = d[pos++];
	for (int i = 0; i < n; i++) {
		hplayer_t p = {0};
		int slot;
		if (pos + 3 > len)
			return;
		slot = d[pos], p.health = (int16_t)(d[pos + 1] | d[pos + 2] << 8), pos += 3;
		if (!rd_str(d, len, &pos, p.name, sizeof(p.name)) || !rd_str(d, len, &pos, p.mate, sizeof(p.mate)) || pos >= len)
			return;
		p.cad = d[pos++];
		if (!rd_str(d, len, &pos, p.team, sizeof(p.team)))
			return;
		p.used = true;
		if (slot < MAX_PLAYERS)
			hud.pl[slot] = p;
	}
	if (pos >= len)
		return;
	n = d[pos++];
	hud.numnpc = 0;
	for (int i = 0; i < n; i++) {
		if (pos + 6 > len)
			return;
		if (hud.numnpc < MAX_NPC) {
			hud.npc[hud.numnpc].ent = (uint16_t)(d[pos] | d[pos + 1] << 8);
			hud.npc[hud.numnpc].health = (int16_t)(d[pos + 2] | d[pos + 3] << 8);
			hud.npc[hud.numnpc].max = (int16_t)(d[pos + 4] | d[pos + 5] << 8);
		}
		pos += 6;
		if (!rd_str(d, len, &pos, hud.numnpc < MAX_NPC ? hud.npc[hud.numnpc].name : key, NAMELEN))
			return;
		if (hud.numnpc < MAX_NPC)
			hud.numnpc++;
	}
	if (pos + 2 > len)
		return;
	{
		int w = d[pos], h = d[pos + 1];
		pos += 2;
		if (w && h) {
			int bytes = (w * h + 7) / 8;
			if (pos + 2 + bytes > len || w > 32 || h > 32)
				return;
			set_crosshair(w, h, (uint16_t)(d[pos] | d[pos + 1] << 8), d + pos + 2);
			pos += 2 + bytes;
		} else
			hud.xw = hud.xh = 0;
	}
	/* the newest obituaries: the ones already there at the first HUDINFO aren't news */
	if (pos + 2 > len)
		return;
	{
		uint8_t seq = d[pos];
		int count = d[pos + 1], fresh = (uint8_t)(seq - hud.obitseq);
		pos += 2;
		for (int i = 0; i < count; i++) {
			for (int k = 0; k < 4; k++)
				if (!rd_str(d, len, &pos, s[k], NAMELEN))
					return;
			if (hud.obitinit && i >= count - fresh)
				obituary(s[0], s[1], s[2], s[3]);
		}
		hud.obitseq = seq, hud.obitinit = true;
	}
	hud.have = true;
}

uint16_t Hud_Version(void) { return hud.version; }

/* jtf.qc Jtf_Post's notice and feed: serverinfo "seq|..." that changed since the last */
static void news(void)
{
	const char *notice = hud.keys[HK_NOTICE], *feed = hud.keys[HK_FEED];
	if (notice[0]) {
		int seq = atoi(notice);
		if (!hud.noticeinit)
			hud.noticeseq = seq;
		else if (seq != hud.noticeseq) {
			const char *text = strchr(notice, '|'), *split;
			text = text ? text + 1 : "";
			split = strstr(text, ": ");
			if (!split)
				split = strstr(text, "! ");
			if (split) {
				snprintf(hud.ntitle, sizeof(hud.ntitle), "%.*s", (int)(split - text), text);
				snprintf(hud.nsub, sizeof(hud.nsub), "%s", split + 2);
			} else
				snprintf(hud.ntitle, sizeof(hud.ntitle), "%s", text), hud.nsub[0] = 0;
			hud.nborn = now_ms() | 1;
			hud.noticeseq = seq;
		}
	}
	hud.noticeinit = true;
	if (feed[0]) {
		char f[4][NAMELEN];
		int n = 0, seq = atoi(feed);
		const char *p = feed;
		memset(f, 0, sizeof(f));
		while (n < 4) {
			const char *e = strchr(p, '|');
			int l = e ? (int)(e - p) : (int)strlen(p);
			snprintf(f[n++], NAMELEN, "%.*s", l, p);
			if (!e)
				break;
			p = e + 1;
		}
		if (!hud.feedinit)
			hud.feedseq = seq;
		else if (n >= 4 && seq != hud.feedseq) {
			const char *me = my_name();
			hud.feedseq = seq;
			feed_push(f[1], f[2], f[3], !strcmp(f[1], me) || !strcmp(f[3], me));
		}
	}
	hud.feedinit = true;
}

/* --- widgets ------------------------------------------------------------------------------ */

enum { W_DIAG, W_OBJ, W_FEED, W_NOTICE, W_VITALS, W_WEAPONS, W_DOWN, W_PLATES, W_PROMPT, W_TID, W_CROSS, W_COUNT };
enum { OP_FILL = 1, OP_TEXT, OP_LINE, OP_XHAIR };
static const uint16_t wcap[W_COUNT] = {200, 160, 400, 160, 900, 300, 400, 300, 200, 120, 48};

typedef struct {
	uint8_t *buf;
	int len, cap;
	int16_t x0, y0, x1, y1; /* what it covers, as last drawn */
	int16_t nx0, ny0, nx1, ny1;
	uint32_t sig;
	bool overflow;
} widget_t;

static widget_t wid[W_COUNT];
static uint8_t warena[3200];
static widget_t *cw; /* the one being written */

static void w_bytes(const void *p, int n)
{
	if (cw->len + n > cw->cap) {
		cw->overflow = true;
		return;
	}
	memcpy(cw->buf + cw->len, p, (size_t)n);
	cw->len += n;
}

static void w_box(int x0, int y0, int x1, int y1)
{
	cw->nx0 = (int16_t)BW_NDS_MIN(cw->nx0, x0), cw->ny0 = (int16_t)BW_NDS_MIN(cw->ny0, y0);
	cw->nx1 = (int16_t)BW_NDS_MAX(cw->nx1, x1), cw->ny1 = (int16_t)BW_NDS_MAX(cw->ny1, y1);
}

static void w_fill(int x, int y, int w, int h, int col)
{
	int16_t v[4] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
	uint8_t op[2] = {OP_FILL, (uint8_t)col};
	if (w <= 0 || h <= 0)
		return;
	w_bytes(op, 2), w_bytes(v, 8);
	w_box(x, y, x + w, y + h);
}

/* a 1-pixel frame */
static void w_frame(int x, int y, int w, int h, int col)
{
	w_fill(x, y, w, 1, col), w_fill(x, y + h - 1, w, 1, col);
	w_fill(x, y + 1, 1, h - 2, col), w_fill(x + w - 1, y + 1, 1, h - 2, col);
}

/* returns the text's width; flags 1: no outline */
static int w_text(int x, int y, int scale, int col, const char *s, int flags)
{
	int len = (int)BW_NDS_MIN(strlen(s), 64), wd = text_width(s, len, scale);
	int16_t v[2] = {(int16_t)x, (int16_t)y};
	uint8_t op[5] = {OP_TEXT, (uint8_t)col, (uint8_t)scale, (uint8_t)flags, (uint8_t)len};
	if (!len)
		return 0;
	w_bytes(op, 5), w_bytes(v, 4), w_bytes(s, len);
	w_box(x - 1, y - 1, x + wd + 1, y + hudh * scale + 1);
	return wd;
}

static int t_w(const char *s, int scale) { return text_width(s, (int)BW_NDS_MIN(strlen(s), 64), scale); }

static void w_line(int x0, int y0, int x1, int y1, int col)
{
	int16_t v[4] = {(int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1};
	uint8_t op[2] = {OP_LINE, (uint8_t)col};
	w_bytes(op, 2), w_bytes(v, 8);
	w_box(BW_NDS_MIN(x0, x1), BW_NDS_MIN(y0, y1), BW_NDS_MAX(x0, x1) + 1, BW_NDS_MAX(y0, y1) + 1);
}

static void w_xhair(int x, int y)
{
	int16_t v[2] = {(int16_t)x, (int16_t)y};
	uint8_t op[3] = {OP_XHAIR, hud.xw, hud.xh};
	w_bytes(op, 3), w_bytes(v, 4), w_bytes(hud.xmask, sizeof(hud.xmask));
	w_box(x, y, x + hud.xw, y + hud.xh);
}

static void w_begin(int i)
{
	cw = &wid[i];
	cw->len = 0, cw->overflow = false;
	cw->nx0 = cw->ny0 = 32767, cw->nx1 = cw->ny1 = -32768;
}

static uint32_t fnv(const uint8_t *p, int n)
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

static void w_exec(const widget_t *w)
{
	const uint8_t *p = w->buf, *end = w->buf + w->len;
	while (p < end) {
		int16_t v[4];
		switch (p[0]) {
		case OP_FILL:
			memcpy(v, p + 2, 8);
			top_fill(v[0], v[1], v[2], v[3], p[1]);
			p += 10;
			break;
		case OP_TEXT:
			memcpy(v, p + 5, 4);
			if (v[1] - 1 < clipy1 && v[1] + hudh * p[2] + 1 > clipy0)
				draw_text(v[0], v[1], p[2], p[1], (const char *)p + 9, p[4], p[3] & 1);
			p += 9 + p[4];
			break;
		case OP_LINE:
			memcpy(v, p + 2, 8);
			draw_line(v[0], v[1], v[2], v[3], p[1]);
			p += 10;
			break;
		case OP_XHAIR: {
			uint16_t m[XH_MAX];
			memcpy(v, p + 3, 4);
			memcpy(m, p + 7, sizeof(m));
			for (int y = 0; y < p[2]; y++)
				for (int x = 0; x < p[1]; x++)
					if (m[y] & (1 << x))
						top_px(v[0] + x, v[1] + y, HC_XHAIR);
			p += 7 + sizeof(m);
			break;
		}
		default:
			return;
		}
	}
}

static void mark_tiles(uint32_t *dirty, int x0, int y0, int x1, int y1)
{
	x0 = BW_NDS_MAX(x0, 0), y0 = BW_NDS_MAX(y0, 0), x1 = BW_NDS_MIN(x1, 256), y1 = BW_NDS_MIN(y1, 192);
	if (x1 <= x0 || y1 <= y0)
		return;
	for (int ty = y0 >> 3; ty <= (y1 - 1) >> 3; ty++)
		for (int tx = x0 >> 3; tx <= (x1 - 1) >> 3; tx++)
			dirty[ty] |= 1u << tx;
}

/* --- the parts --------------------------------------------------------------------------- */

static const char *key(int k) { return k == HK_HEROHUD ? hud.herohud : hud.keys[k]; }
static bool down_now(void) { return hud.have && key(HK_RESPAWN)[0]; }
static int split_bar(const char *s, char out[][96], int max) /* fields cut at 95 */
{
	int n = 0;
	while (n < max) {
		const char *e = strchr(s, '|');
		int l = e ? (int)(e - s) : (int)strlen(s);
		snprintf(out[n++], 96, "%.*s", l, s);
		if (!e)
			break;
		s = e + 1;
	}
	return n;
}

/* a QtCurve progress bar: trough with its border, and the fill (frac 0..1000) */
static void meter(int x, int y, int w, int frac, int fillcol)
{
	w_fill(x, y, w, 4, HC_FRAME);
	w_fill(x + 1, y + 1, w - 2, 2, HC_TROUGH);
	if (frac > 0)
		w_fill(x + 1, y + 1, BW_NDS_MAX((w - 2) * BW_NDS_MIN(frac, 1000) / 1000, 1), 2, fillcol);
}

/* a QtCurve window: colour and frame */
static void window(int x, int y, int w, int h)
{
	w_fill(x + 1, y + 1, w - 2, h - 2, HC_WINDOW);
	w_frame(x, y, w, h, HC_FRAME);
}

static int top_y; /* below the diagnostics */

static void part_diag(const hudin_t *in)
{
	top_y = 0;
	if (in->diag1[0])
		w_text(2, 1, 1, HC_DIAG, in->diag1, 0), top_y = hudh + 1;
	if (in->diag2[0])
		w_text(2, top_y + 1, 1, HC_AMBER, in->diag2, 0), top_y += hudh + 1;
	top_y += top_y ? 2 : 0;
}

/* the objective panel: top left (hud.css .objective x 32 y 32), hud.js wave() */
static void part_obj(void)
{
	char st[2][96], v[16];
	int y, w = 74, x0 = 3, y0 = top_y + 3, wave = atoi(key(HK_WAVE)), left = key(HK_LEFT)[0] ? atoi(key(HK_LEFT)) : -1;
	bool fighting = wave > 0 && left >= 0;
	if (!hud.have)
		return;
	memset(st, 0, sizeof(st));
	split_bar(key(HK_STATS)[0] ? key(HK_STATS) : "0|0", st, 2);
	window(x0, y0, w, 4 * 7 + 7);
	y = y0 + 2;
	snprintf(v, sizeof(v), "WAVE %d", BW_NDS_MAX(1, wave));
	w_text(x0 + w - 3 - t_w(v, 1), y, 1, HC_TEXT, v, 1), y += 7;
	w_text(x0 + 3, y, 1, HC_TEXT, "LEFT", 1);
	snprintf(v, sizeof(v), "%d", fighting ? left : 0);
	w_text(x0 + w - 3 - t_w(v, 1), y, 1, fighting ? HC_RED : HC_MUTED, v, 1), y += 7;
	w_text(x0 + 3, y, 1, HC_TEXT, "SCORE", 1);
	snprintf(v, sizeof(v), "%s", st[0][0] ? st[0] : "0");
	w_text(x0 + w - 3 - t_w(v, 1), y, 1, HC_TEXT, v, 1), y += 7;
	w_fill(x0 + 3, y + 1, w - 6, 1, HC_FRAME), y += 3;
	snprintf(v, sizeof(v), "p%.12s", st[1][0] ? st[1] : "0");
	w_text(x0 + w - 3 - t_w(v, 1), y, 1, HC_TEXT, v, 1);
}

/* the kill feed: top right, newest on top (hud.css .kill-feed, hud.js "feed") */
static int feed_y; /* below the feed's lines: the notice keeps clear of them */
static void part_feed(uint32_t t)
{
	int y = top_y + 3;
	feed_y = 0;
	if (!hud.have || down_now())
		return;
	for (int i = 0; i < FEED_MAX; i++) {
		const feed_t *f = &hud.feed[i];
		int w, x;
		const char *wtext;
		if (!f->born || t - f->born >= FEED_MS)
			continue;
		wtext = f->w[0] ? f->w : "killed";
		w = (f->a[0] ? t_w(f->a, 1) + 5 : 0) + t_w(wtext, 1) + (f->v[0] ? 5 + t_w(f->v, 1) : 0) + 6;
		x = 253 - w;
		window(x, y, w, 10);
		if (f->you)
			w_frame(x, y, w, 10, HC_ACCENT);
		x += 3;
		if (f->a[0])
			x += w_text(x, y + 1, 1, HC_ACCENT, f->a, 1) + 5;
		x += w_text(x, y + 1, 1, HC_MUTED, wtext, 1) + 5;
		if (f->v[0])
			w_text(x, y + 1, 1, HC_TEXT, f->v, 1);
		y += 11;
	}
	feed_y = y;
}

static void upper(char *s)
{
	for (; *s; s++)
		if (*s >= 'a' && *s <= 'z')
			*s = (char)(*s - 32);
}

/* wave news: centred near the top (hud.css .hud-notice y 140), three seconds */
static void part_notice(uint32_t t)
{
	char title[48];
	int tw, sw, w, x, y = BW_NDS_MAX(PY(140), feed_y + 3), h = hud.nsub[0] ? 23 : 14;
	if (!hud.nborn || t - hud.nborn >= NOTICE_MS || down_now())
		return;
	snprintf(title, sizeof(title), "%s", hud.ntitle);
	upper(title);
	tw = t_w(title, 2), sw = hud.nsub[0] ? t_w(hud.nsub, 1) : 0;
	w = BW_NDS_MAX(tw, sw) + 24;
	x = 128 - w / 2;
	/* a band that fades out at both ends: solid in the middle, then every other pixel */
	w_fill(x + 8, y, w - 16, h, HC_WINDOW);
	w_fill(x, y, 8, h, HC_WINDOW | FILL_DITHER), w_fill(x + w - 8, y, 8, h, HC_WINDOW | FILL_DITHER);
	w_text(128 - tw / 2, y + 2, 2, HC_TEXT, title, 0);
	if (hud.nsub[0])
		w_text(128 - sw / 2, y + 15, 1, HC_TEXT, hud.nsub, 0);
}

/* a big number over a faint row of 8s, right-aligned in cells (hud.js bigNum) */
static int bignum(int x, int y, int scale, int col, const char *v, int cells)
{
	char ghost[8];
	int n = (int)strlen(v), c = BW_NDS_MAX(cells, n), w;
	memset(ghost, '8', sizeof(ghost));
	ghost[BW_NDS_MIN(c, 7)] = 0;
	w = t_w(ghost, scale);
	w_text(x, y, scale, HC_FRAME, ghost, 1);
	if (n)
		w_text(x + w - t_w(v, scale), y, scale, col, v, 0);
	return w;
}

typedef struct {
	bool on;
	char name[32];
	int energy, emax, barrier, cap, form;
	char status[96], formline[96];
} hero_t;

static void read_hero(hero_t *h)
{
	char f[10][96];
	memset(h, 0, sizeof(*h));
	if (!key(HK_HEROHUD)[0] || split_bar(key(HK_HEROHUD), f, 10) < 10)
		return;
	h->on = true;
	snprintf(h->name, sizeof(h->name), "%.31s", f[0]);
	h->energy = atoi(f[1]), h->emax = atoi(f[2]), h->barrier = atoi(f[3]), h->cap = atoi(f[4]), h->form = atoi(f[5]);
	memcpy(h->status, f[6], 96), memcpy(h->formline, f[7], 96);
}

/* hud.js abilities(): "[KEY] text state" parts two spaces apart */
static bool ability_state(const char *line, char keyc, char *state, int size)
{
	const char *p = line;
	while ((p = strchr(p, '['))) {
		const char *e = strchr(p, ']'), *end;
		if (!e)
			return false;
		if (e == p + 2 && p[1] == keyc) {
			const char *last;
			end = strstr(e, "  ");
			if (!end)
				end = e + strlen(e);
			while (end > e + 1 && end[-1] == ' ')
				end--;
			last = end;
			while (last > e + 1 && last[-1] != ' ')
				last--;
			/* the state: ready, used, Ns, N/N, N%, N */
			{
				int l = (int)(end - last);
				bool ok = l > 0 && (!strncasecmp(last, "ready", 5) || !strncasecmp(last, "used", 4));
				if (!ok && l > 0 && last[0] >= '0' && last[0] <= '9') {
					const char *q = last;
					while (q < end && *q >= '0' && *q <= '9')
						q++;
					if (q == end || (q + 1 == end && (*q == 's' || *q == '%')))
						ok = true;
					else if (*q == '/' && q + 1 < end) {
						const char *r = q + 1;
						while (r < end && *r >= '0' && *r <= '9')
							r++;
						ok = r == end;
					}
				}
				snprintf(state, (size_t)size, "%.*s", ok ? l : 0, last);
			}
			return true;
		}
		p = e;
	}
	return false;
}

/* bottom left (hud.css .vitals): squad, abilities, Cadence, armour or energy, then
 * health, Barrier and level in one row at the bottom */
static void part_vitals(const hudin_t *in, const hudent_t *ents, int nents, const ndscam_t *view)
{
	hero_t h;
	char v[24];
	int x0 = 4, y = 188, hp = BW_NDS_MAX(0, in->health), ar;
	if (down_now())
		return;
	read_hero(&h);
	ar = h.on ? h.barrier : in->armor;
	/* the bottom row: a plus, health, Barrier, level */
	{
		int x = x0, ry = y - 15, col = hp < 25 ? HC_RED : HC_TEXT;
		char xp[8][96];
		int nx = key(HK_XPM)[0] ? split_bar(key(HK_XPM), xp, 8) : 0;
		w_fill(x, ry + 6, 9, 3, col), w_fill(x + 3, ry + 3, 3, 9, col);
		x += 12;
		snprintf(v, sizeof(v), "%d", hp);
		x += bignum(x, ry, 3, col, v, 3) + 6;
		if (h.on) {
			snprintf(v, sizeof(v), ar > 0 ? "%d" : "", ar);
			x += bignum(x, ry, 3, HC_BARRIER, v, 2) + 5;
		}
		y = ry - 3;
		/* the XPM plugin's level, rank and medals over the experience gauge ("xp|need|level|max
		 * level|rank|medals|max medals|level start"): beside the health on the PC, above it here */
		if (nx >= 7) {
			int from = nx > 7 ? atoi(xp[7]) : 0, need = atoi(xp[1]), frac, lx = x0;
			y -= 11;
			snprintf(v, sizeof(v), "Lv%.8s", xp[2]);
			lx += w_text(lx, y, 1, HC_TEXT, v, 0) + 4;
			lx += w_text(lx, y, 1, HC_TEXT, xp[4], 0) + 4;
			if (atoi(xp[5]) > 0) {
				snprintf(v, sizeof(v), "*x%.8s", xp[5]);
				w_text(lx, y, 1, HC_GOLD, v, 0);
			}
			frac = !strcmp(xp[1], "0") ? 1000 : need - from > 0 ? (atoi(xp[0]) - from) * 1000 / (need - from) : 0;
			meter(x0, y + 7, 100, frac, HC_FILL);
			y -= 3;
		}
	}
	/* energy (a hero) with Engage's tick, or armour (no hero, some armour) */
	if (h.on || ar > 0) {
		int bw = 100, frac;
		if (h.on) {
			int ecost = atoi(key(HK_ENGAGECOST));
			frac = h.emax > 0 ? h.energy * 1000 / h.emax : 0;
			snprintf(v, sizeof(v), "%d / %d", h.energy, h.emax);
			y -= 11;
			w_text(x0, y, 1, HC_MUTED, "ENERGY", 0);
			w_text(x0 + bw - t_w(v, 1), y, 1, HC_TEXT, v, 0);
			meter(x0, y + 7, bw, frac, HC_FILL);
			if (ecost > 0 && h.energy >= ecost && !(h.form > 0) && h.emax > 0)
				w_fill(x0 + 1 + (bw - 2) * BW_NDS_MIN(ecost, h.emax) / h.emax, y + 7, 1, 4, HC_TEXT);
		} else {
			snprintf(v, sizeof(v), "%d", ar);
			y -= 11;
			w_text(x0, y, 1, HC_MUTED, "ARMOR", 0);
			w_text(x0 + bw - t_w(v, 1), y, 1, HC_TEXT, v, 0);
			meter(x0, y + 7, bw, ar * 10, HC_FILL);
		}
		y -= 3;
	}
	/* Choir's Cadence on us: gold, three pips */
	if (atoi(key(HK_CADENCE)) > 0) {
		int c = atoi(key(HK_CADENCE));
		y -= 9;
		window(x0, y, 100, 9);
		w_frame(x0, y, 100, 9, HC_GOLD);
		w_text(x0 + 3, y + 1, 1, HC_GOLD, "CADENCE", 1);
		for (int i = 0; i < 3; i++)
			w_fill(x0 + 100 - 3 - (3 - i) * 6, y + 3, 4, 2, i < c ? HC_GOLD : HC_FRAME);
		y -= 3;
	}
	/* the hero's abilities: T 1 2 ENG EX, from the status line (hud.js SQUARES) */
	if (h.on) {
		static const char sq[5][2][4] = {{"C", "T"}, {"G", "1"}, {"X", "2"}, {"Z", "ENG"}, {"V", "EX"}};
		int x = x0, any = 0;
		y -= 15;
		for (int i = 0; i < 5; i++) {
			char state[16];
			bool fromform = false, ready, engaged, unavail;
			int w, glyphw;
			if (!ability_state(h.status, sq[i][0][0], state, sizeof(state))) {
				if (!h.formline[0] || !ability_state(h.formline, sq[i][0][0], state, sizeof(state)))
					continue;
				fromform = true;
			}
			ready = !strcasecmp(state, "ready");
			engaged = fromform && h.form > 0;
			unavail = !ready && !engaged;
			if (unavail && state[0] >= '1' && state[0] <= '9' && strchr(state, '/'))
				unavail = false;
			glyphw = t_w(sq[i][1], 1);
			w = BW_NDS_MAX(15, BW_NDS_MAX(glyphw, ready ? 0 : t_w(state, 1)) + 5);
			w_fill(x + 1, y + 1, w - 2, 13, engaged ? HC_FILL : unavail ? HC_FRAME : HC_WINDOW);
			w_frame(x, y, w, 15, engaged ? HC_ACCENT : HC_FRAME);
			if (ready) /* the corner triangle */
				for (int k = 0; k < 4; k++)
					w_fill(x + w - 1 - 4 + k, y + 1, 4 - k, 1, HC_ACCENT);
			w_text(x + (w - glyphw) / 2, y + 2, 1, unavail ? HC_MUTED : HC_TEXT, sq[i][1], 1);
			if (!ready && state[0])
				w_text(x + (w - t_w(state, 1)) / 2, y + 8, 1, unavail ? HC_MUTED : engaged ? HC_TEXT : HC_MUTED, state,
				       1);
			x += w + 2, any++;
		}
		y -= 3;
		(void)any;
	}
	/* the squad: the three closest others with a card (jtf.qc Jtf_Team) */
	if (hud.have) {
		int dist[MAX_PLAYERS], pick[3], n = 0;
		bool used[MAX_PLAYERS] = {false};
		for (int i = 0; i < MAX_PLAYERS; i++)
			dist[i] = 1000000 + i;
		for (int i = 0; i < nents; i++)
			if (ents[i].slot >= 0 && ents[i].slot < MAX_PLAYERS && ents[i].slot != hud.self) {
				int32_t d[3];
				float l;
				for (int k = 0; k < 3; k++)
					d[k] = ents[i].org[k] * 8 - view->org[k];
				l = sqrtf((float)d[0] * d[0] + (float)d[1] * d[1] + (float)d[2] * d[2]) / 8;
				dist[ents[i].slot] = (int)(l + 0.5f);
			}
		for (; n < 3; n++) {
			int best = -1;
			for (int i = 0; i < MAX_PLAYERS; i++) {
				if (i == hud.self || used[i] || !hud.pl[i].used || !hud.pl[i].name[0] || !hud.pl[i].mate[0])
					continue;
				if (best < 0 || dist[i] < dist[best])
					best = i;
			}
			if (best < 0)
				break;
			used[best] = true, pick[n] = best;
		}
		/* drawn bottom up, so the first is on top */
		for (int k = n - 1; k >= 0; k--) {
			const hplayer_t *p = &hud.pl[pick[k]];
			char st = 'a';
			int mhp = atoi(p->mate), x;
			const char *bar = strchr(p->mate, '|');
			bool out, dead;
			if (bar)
				st = bar[1];
			out = st == 'd' || st == 'x', dead = st == 'x';
			y -= 13;
			x = x0 + w_text(x0, y, 1, dead ? HC_MUTED : HC_TEXT, p->name, 0);
			if (out)
				w_text(x + 4, y, 1, dead ? HC_MUTED : HC_AMBER, "DOWN", 0);
			else if (p->cad > 0)
				w_text(x + 4, y, 1, HC_GOLD, "^", 0);
			meter(x0, y + 7, 48, out ? 0 : mhp * 10,
			      dead ? HC_MUTED : st == 'd' ? HC_AMBER : !out && mhp < 25 ? HC_RED : HC_GREEN);
			y -= 1;
		}
	}
}

/* bottom right (hud.css .weapons): the hero's two slots for 2 s after a change, the
 * weapon's name, then clip | reserve (hud.js weapons()) */
static void part_weapons(const hudin_t *in, uint32_t t)
{
	hero_t h;
	char v[16], wk[160];
	const char *title = hud.have ? key(HK_WTITLE) : in->weapon, *sname = key(HK_STATICNAME), *dname = key(HK_DYNAMICNAME);
	bool noammo = hud.have ? key(HK_NOAMMO)[0] != 0 : false;
	int held = 0, y = 188, xr = 252;
	if (down_now())
		return;
	read_hero(&h);
	if (!in->haveweapon && !title[0])
		title = "";
	if (title[0] && !strcmp(title, sname))
		held = 1;
	else if (title[0] && !strcmp(title, dname))
		held = 2;
	snprintf(wk, sizeof(wk), "%.48s|%d|%.48s|%.48s", title, held, h.on ? sname : "", h.on ? dname : "");
	if (hud.wkeyinit && strcmp(wk, hud.wkey))
		hud.slotsuntil = (t + SLOTS_MS) | 1;
	memcpy(hud.wkey, wk, sizeof(wk)), hud.wkeyinit = true;
	if (title[0] && in->haveweapon) {
		bool noclip = in->clipsize <= 0, noreserve = h.on && held == 1;
		bool low = in->clipsize > 0 && in->clip <= BW_NDS_MAX(1, in->clipsize / 4);
		int x = xr, ry = y - 15;
		if (!noreserve && !noammo) {
			snprintf(v, sizeof(v), "%d", in->reserve);
			x -= t_w(v, 2);
			w_text(x, ry + 5, 2, HC_MUTED, v, 0);
		}
		if (!noclip && !noreserve) {
			x -= 4;
			w_fill(x - 1, ry + 4, 1, 11, HC_MUTED);
			x -= 4;
		}
		if (!noclip) {
			snprintf(v, sizeof(v), "%d", in->clip);
			x -= t_w(v, 3);
			w_text(x, ry, 3, low ? HC_RED : HC_TEXT, v, 0);
		}
		y = ry - 2;
		if (!held) { /* the hero slots above name a hero weapon already */
			char up[48];
			snprintf(up, sizeof(up), "%.47s", title);
			upper(up);
			y -= 7;
			w_text(xr - t_w(up, 1), y, 1, HC_MUTED, up, 0);
		}
		y -= 3;
	}
	if (h.on && hud.slotsuntil && t < hud.slotsuntil) {
		for (int s = 1; s >= 0; s--) { /* SlotDynamic is drawn below SlotStatic */
			const char *name = s ? dname : sname;
			bool active = held == s + 1;
			int col = active ? HC_TEXT : HC_MUTED, w = 70, x = xr - w;
			y -= 11;
			w_frame(x, y, w, 11, col);
			if (active) /* the arrow tip on its left */
				w_line(x, y, x - 5, y + 5, col), w_line(x - 5, y + 5, x, y + 10, col);
			w_text(x + 3, y + 2, 1, col, s ? "2" : "1", 1);
			w_text(x + 12, y + 2, 1, col, name[0] ? name : "-", 1);
			y -= 2;
		}
	}
}

/* word-wrapped lines of at most maxw pixels, centred at cx */
static int wrapped(int cx, int y, int maxw, int col, const char *s)
{
	char line[64];
	int n = 0;
	while (*s) {
		int len = 0, fit = 0;
		while (s[len] && len < 63) {
			len++;
			if ((!s[len] || s[len] == ' ') && text_width(s, len, 1) <= maxw)
				fit = len;
			if (text_width(s, len, 1) > maxw)
				break;
		}
		if (!fit)
			fit = BW_NDS_MAX(len - 1, 1);
		snprintf(line, sizeof(line), "%.*s", fit, s);
		w_text(cx - t_w(line, 1) / 2, y, 1, col, line, 1);
		y += 7, n++;
		s += fit;
		while (*s == ' ')
			s++;
	}
	return n;
}

/* while down: a teammate's revive as a white bar, then the respawn box (hud.css
 * .down-box y 540, hud.js respawn()) */
static void part_down(const hudin_t *in)
{
	const char *r = key(HK_RESPAWN), *rv = key(HK_REVIVE);
	float t = in->srvms / 1000.0f;
	int w = 150, x = 128 - w / 2, y = PY(540);
	bool survival;
	if (!down_now())
		return;
	survival = !strcmp(r, "survival");
	if (rv[0]) { /* "<start> <time>|<name>" */
		char label[48];
		const char *bar = strchr(rv, '|');
		float start = (float)atof(rv), len = 0.01f;
		const char *sp = strchr(rv, ' ');
		int frac;
		if (sp)
			len = BW_NDS_MAX((float)atof(sp + 1), 0.01f);
		frac = (int)((t - start) / len * 100);
		frac = BW_NDS_CLAMP(frac, 0, 100);
		snprintf(label, sizeof(label), "%s is reviving you", bar ? bar + 1 : "");
		w_text(128 - t_w(label, 1) / 2, y, 1, HC_TEXT, label, 0);
		w_fill(x, y + 7, w, 2, HC_MUTED);
		w_fill(x, y + 7, w * frac / 100, 2, HC_TEXT);
		y += 12;
	}
	{
		float down = (float)atof(r), delay = 0.01f, el;
		const char *sp = strchr(r, ' ');
		int frac = 0, left = 0, lines;
		bool ready;
		char value[12], hint[64];
		const char *label;
		if (!survival) {
			if (sp)
				delay = BW_NDS_MAX((float)atof(sp + 1), 0.01f);
			el = t - down;
			frac = (int)(BW_NDS_CLAMP(el / delay, 0.0f, 1.0f) * 100); /* floor(frac * 100) / 100 */
			left = (int)BW_NDS_MAX(delay - el, 0.0f);
			left += (float)left < delay - el ? 1 : 0; /* ceil */
		}
		ready = !survival && frac >= 100;
		label = survival ? "DOWN" : ready ? "READY TO RESPAWN" : "RESPAWNING";
		snprintf(value, sizeof(value), survival || ready ? "" : "%ds", left);
		if (survival)
			snprintf(hint, sizeof(hint), "Survival: wait for a teammate to revive you");
		else if (ready)
			snprintf(hint, sizeof(hint), "Press %s to respawn", in->firekey);
		else
			snprintf(hint, sizeof(hint), "A teammate can revive you");
		/* the hint's lines first, to size the box */
		lines = 1 + (t_w(hint, 1) > w - 8);
		window(x, y, w, 23 + 7 * lines);
		if (ready)
			w_frame(x + 1, y + 1, w - 2, 21 + 7 * lines, HC_ACCENT);
		w_text(x + 4, y + 3, 1, HC_TEXT, label, 1);
		if (value[0])
			w_text(x + w - 4 - t_w(value, 1), y + 3, 1, HC_TEXT, value, 1);
		meter(x + 4, y + 11, w - 8, survival ? 0 : frac * 10, HC_FILL);
		wrapped(128, y + 18, w - 8, HC_MUTED, hint);
	}
}

/* map units to the top screen, as the GPU projects them (fov 70 up and down, 4:3):
 * false when behind the eye */
static bool project(const ndscam_t *v, const float p[3], int *sx, int *sy)
{
	float sy_ = sinLerp(v->yaw) / 4096.0f, cy = cosLerp(v->yaw) / 4096.0f;
	float sp = sinLerp(v->pitch) / 4096.0f, cp = cosLerp(v->pitch) / 4096.0f;
	float d[3], f, r, u;
	for (int k = 0; k < 3; k++)
		d[k] = p[k] - v->org[k] / 8.0f;
	f = (cy * cp) * d[0] + (sy_ * cp) * d[1] + sp * d[2];
	r = sy_ * d[0] - cy * d[1];
	u = (-cy * sp) * d[0] + (-sy_ * sp) * d[1] + cp * d[2];
	if (f <= 0.5f)
		return false;
	/* 96 / tan(35 degrees) */
	*sx = 128 + (int)(r / f * 137.1f), *sy = 96 - (int)(u / f * 137.1f);
	return true;
}

static void eye_fwd(const ndscam_t *v, float eye[3], float fwd[3])
{
	float sy = sinLerp(v->yaw) / 4096.0f, cy = cosLerp(v->yaw) / 4096.0f;
	float sp = sinLerp(v->pitch) / 4096.0f, cp = cosLerp(v->pitch) / 4096.0f;
	for (int k = 0; k < 3; k++)
		eye[k] = v->org[k] / 8.0f;
	fwd[0] = cy * cp, fwd[1] = sy * cp, fwd[2] = sp;
}

static float trace_frac(const float a[3], const float b[3])
{
	return World_Loaded() ? World_TraceFraction(a, b) : 1.0f;
}

/* a teammate (the same team, not 0 or none) or a friendly NPC the server listed:
 * its name and health, false otherwise */
static bool friendly(const hudent_t *e, const char **name, int *hp, int *max)
{
	if (e->slot >= 0) {
		const char *team = hud.self >= 0 && hud.self < MAX_PLAYERS ? hud.pl[hud.self].team : "";
		const hplayer_t *p = e->slot < MAX_PLAYERS ? &hud.pl[e->slot] : NULL;
		if (e->slot == hud.self || !p || !p->used || !team[0] || !strcmp(team, "0") || strcmp(p->team, team) ||
		    p->health <= 0)
			return false;
		/* players don't network max_health */
		*name = p->name, *hp = p->health, *max = BW_NDS_MAX(100, p->health);
		return true;
	}
	for (int i = 0; i < hud.numnpc; i++)
		if (hud.npc[i].ent == e->ent && hud.npc[i].health > 0) {
			*name = hud.npc[i].name, *hp = hud.npc[i].health, *max = hud.npc[i].max > 0 ? hud.npc[i].max : hud.npc[i].health;
			return true;
		}
	return false;
}

/* nameplates.qc Nameplates_DrawOne (cg_nameplates: serverinfo "nameplates"), and the
 * target ID's search (Nameplates_Consider, Nameplates_RayHit) */
static void part_plates(const ndscam_t *view, const hudent_t *ents, int nents, uint32_t t)
{
	float eye[3], fwd[3];
	bool plates = atoi(key(HK_NAMEPLATES)) > 0, tid = atoi(key(HK_TARGETID)) > 0;
	int drawn = 0, bestent = -1;
	float bestd = 0;
	if (!hud.have || (!plates && !tid))
		return;
	eye_fwd(view, eye, fwd);
	for (int i = 0; i < nents; i++) {
		const hudent_t *e = &ents[i];
		const char *name;
		int hp, max;
		if (!friendly(e, &name, &hp, &max))
			continue;
		if (plates && drawn < 4) {
			float head[3] = {e->org[0], e->org[1], (float)e->org[2] + e->height + 10}, d[3], dist;
			int sx, sy;
			for (int k = 0; k < 3; k++)
				d[k] = head[k] - eye[k];
			dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			/* in range and in front, nothing between (alpha past the last quarter of the range: shown while over a half) */
			if (dist <= 875 && d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2] > 0 && project(view, head, &sx, &sy) &&
			    trace_frac(eye, head) >= 1.0f) {
				int frac = BW_NDS_CLAMP(hp * 1000 / BW_NDS_MAX(max, 1), 0, 1000);
				int bw = 19, nw = t_w(name, 1);
				/* [1 - frac, frac, 0.1]: red to green */
				int col = frac > 600 ? HC_GREEN : frac > 300 ? HC_AMBER : HC_RED;
				w_text(sx - nw / 2, sy - hudh - 1, 1, HC_BARRIER, name, 0);
				w_fill(sx - bw / 2 - 1, sy - 1, bw + 2, 4, HC_EDGE);
				w_fill(sx - bw / 2, sy, BW_NDS_MAX(bw * frac / 1000, 1), 2, col);
				drawn++;
			}
		}
		if (tid) { /* the ray against its box grown by 4, then a wall between */
			float mn[3] = {e->org[0] - e->half - 4.0f, e->org[1] - e->half - 4.0f, e->org[2] - 4.0f};
			float mx[3] = {e->org[0] + e->half + 4.0f, e->org[1] + e->half + 4.0f, (float)e->org[2] + e->height + 4.0f};
			float nearD = 0, farD = 2000; /* cg_targetIDRange */
			bool miss = false;
			for (int k = 0; k < 3 && !miss; k++) {
				float t1, t2;
				if (fabsf(fwd[k]) < 0.0001f) {
					miss = eye[k] < mn[k] || eye[k] > mx[k];
					continue;
				}
				t1 = (mn[k] - eye[k]) / fwd[k], t2 = (mx[k] - eye[k]) / fwd[k];
				nearD = BW_NDS_MAX(nearD, BW_NDS_MIN(t1, t2));
				farD = BW_NDS_MIN(farD, BW_NDS_MAX(t1, t2));
				miss = nearD > farD;
			}
			if (!miss) {
				float end[3] = {eye[0] + fwd[0] * nearD, eye[1] + fwd[1] * nearD, eye[2] + fwd[2] * nearD};
				if ((bestent < 0 || nearD < bestd) && trace_frac(eye, end) >= 0.99f) {
					bestent = i, bestd = nearD;
					hud.tident = e->ent;
					snprintf(hud.tidname, sizeof(hud.tidname), "%s", name);
					hud.tidhealth = hp, hud.tidmax = max;
					hud.tidcad = e->slot >= 0 && e->slot < MAX_PLAYERS ? hud.pl[e->slot].cad : 0;
				}
			}
		}
	}
	if (bestent >= 0)
		hud.tidseen = t | 1, hud.tidany = true;
	/* the target died or left: drop it */
	if (hud.tidany && bestent < 0) {
		bool there = false;
		const char *n;
		int hp, max;
		for (int i = 0; i < nents; i++)
			if (ents[i].ent == hud.tident && friendly(&ents[i], &n, &hp, &max))
				there = true, hud.tidhealth = hp, hud.tidmax = max;
		if (!there)
			hud.tidany = false;
	}
}

/* the key prompt under the crosshair (hud.css .key-prompt y 510), or for a pickup a
 * callout on the item: a leader line from it up to the prompt, underlined */
static void part_prompt(const hudin_t *in, const ndscam_t *view)
{
	const char *p = key(HK_PROMPT), *at = key(HK_PROMPTAT);
	int kw, aw, w, x, y = PY(510), sx, sy;
	bool callout = false;
	if (!p[0] || down_now())
		return;
	if (at[0]) {
		float pt[3] = {0, 0, 0};
		sscanf(at, "%f %f %f", &pt[0], &pt[1], &pt[2]);
		callout = project(view, pt, &sx, &sy);
	}
	kw = BW_NDS_MAX(t_w(in->usekey, 1) + 6, 9), aw = t_w(p, 1);
	w = kw + 3 + aw;
	if (callout) {
		/* the leader's dot on the item, the row 9 right and 17 up (44 and 81 layout units) */
		x = sx + 9, y = sy - 17 - 9;
		w_fill(sx - 1, sy - 1, 3, 3, HC_TEXT);
		w_line(sx, sy, x, y + 10, HC_TEXT);
		w_fill(x, y + 10, w + 2, 1, HC_TEXT);
	} else
		x = 128 - w / 2;
	/* a QtCurve push button with the key, then what it does */
	w_fill(x + 1, y + 1, kw - 2, 7, HC_WINDOW);
	w_frame(x, y, kw, 9, HC_FRAME);
	w_text(x + (kw - t_w(in->usekey, 1)) / 2, y + 1, 1, HC_TEXT, in->usekey, 1);
	w_text(x + kw + 3, y + 1, 1, HC_TEXT, p, 0);
}

/* the target ID as a squad card (hud.css .target-id y 558, hud.js targetID()) */
static void part_tid(uint32_t t)
{
	int since, nw, w, x, y = PY(558), frac;
	bool low;
	if (!hud.tidany || down_now())
		return;
	since = (int)(t - hud.tidseen);
	if (since > LINGER_MS)
		return;
	/* the alpha rounded to a tenth: shown at full while over a half, then gone */
	if (since > LINGER_MS / 2)
		return;
	nw = t_w(hud.tidname, 1);
	/* a QtCurve window, gold framed in a Cadence; the name with the gold arrow, the gauge */
	w = BW_NDS_MAX(nw + (hud.tidcad ? 8 : 0), 48) + 6;
	x = 128 - w / 2;
	low = hud.tidhealth * 4 < hud.tidmax;
	frac = BW_NDS_CLAMP(hud.tidhealth * 1000 / BW_NDS_MAX(hud.tidmax, 1), 0, 1000);
	window(x, y, w, 16);
	if (hud.tidcad)
		w_frame(x, y, w, 16, HC_GOLD), w_text(x + 3 + nw + 3, y + 2, 1, HC_GOLD, "^", 1);
	w_text(x + 3, y + 2, 1, HC_TEXT, hud.tidname, 1);
	meter(x + 3, y + 9, w - 6, frac, low ? HC_RED : HC_GREEN);
}

/* the weapon's crosshair sprite, at the centre (Cross_GetPos), not while down */
static void part_cross(void)
{
	if (!hud.xw || down_now())
		return;
	w_xhair(128 - hud.xw / 2, 96 - hud.xh / 2);
}

/* --- each frame -------------------------------------------------------------------------- */

void Hud_Frame(const ndscam_t *view, const hudent_t *ents, int nents, const hudin_t *in)
{
	uint32_t t = now_ms();
	uint32_t dirty[24] = {0}; /* a bit for each tile */
	if (!topgfx)
		return;
	if (hud.have)
		news();
	for (int i = 0; i < W_COUNT; i++) {
		widget_t *w = &wid[i];
		uint32_t sig;
		w_begin(i);
		switch (i) {
		case W_DIAG: part_diag(in); break;
		case W_OBJ: part_obj(); break;
		case W_FEED: part_feed(t); break;
		case W_NOTICE: part_notice(t); break;
		case W_VITALS: part_vitals(in, ents, nents, view); break;
		case W_WEAPONS: part_weapons(in, t); break;
		case W_DOWN: part_down(in); break;
		case W_PLATES: part_plates(view, ents, nents, t); break;
		case W_PROMPT: part_prompt(in, view); break;
		case W_TID: part_tid(t); break;
		case W_CROSS: part_cross(); break;
		}
		sig = fnv(w->buf, w->len);
		if (sig != w->sig || w->nx0 != w->x0 || w->ny0 != w->y0 || w->nx1 != w->x1 || w->ny1 != w->y1) {
			/* the tiles it covered and covers now are drawn again */
			mark_tiles(dirty, w->x0, w->y0, w->x1, w->y1);
			mark_tiles(dirty, w->nx0, w->ny0, w->nx1, w->ny1);
			w->sig = sig;
			w->x0 = w->nx0, w->y0 = w->ny0, w->x1 = w->nx1, w->y1 = w->ny1;
			if (w->x1 <= w->x0 || w->y1 <= w->y0)
				w->x0 = w->y0 = w->x1 = w->y1 = 0;
		}
	}
	/* each row of tiles with any to draw: every widget over them, in order, then the tiles out */
	for (int ty = 0; ty < 24; ty++) {
		uint32_t m = dirty[ty];
		int tx0, tx1;
		if (!m)
			continue;
		tx0 = __builtin_ctz(m), tx1 = 32 - __builtin_clz(m);
		clipx0 = tx0 * 8, clipx1 = tx1 * 8, clipy0 = ty * 8, clipy1 = ty * 8 + 8;
		memset(strip + tx0 * 32, 0, (size_t)(tx1 - tx0) * 32);
		for (int i = 0; i < W_COUNT; i++)
			if (wid[i].x1 > clipx0 && wid[i].x0 < clipx1 && wid[i].y1 > clipy0 && wid[i].y0 < clipy1)
				w_exec(&wid[i]);
		for (int tx = tx0; tx < tx1; tx++)
			if (m & (1u << tx)) {
				const uint32_t *src = (const uint32_t *)(strip + tx * 32);
				volatile uint32_t *dst = (volatile uint32_t *)(void *)(topgfx + (ty * 32 + tx) * 16);
				for (int k = 0; k < 8; k++)
					dst[k] = src[k];
			}
	}
}

void Hud_Init(void)
{
	uint16_t *map;
	int bg, off = 0;
	vramSetBankF(VRAM_F_MAIN_BG_0x06000000);
	vramSetBankG(VRAM_G_MAIN_BG_0x06004000);
	/* tiles at 0 (24 KB), the map at 24 KB */
	bg = bgInit(1, BgType_Text4bpp, BgSize_T_256x256, 12, 0);
	topgfx = bgGetGfxPtr(bg);
	map = bgGetMapPtr(bg);
	for (int i = 0; i < 32 * 32; i++)
		map[i] = (uint16_t)(i < TOP_TILES ? i : 0);
	dmaFillHalfWords(0, topgfx, TOP_TILES * 32);
	BG_PALETTE[HC_EDGE] = RGB15(0, 0, 0);
	BG_PALETTE[HC_TEXT] = RGB15(29, 29, 29);    /* #eeeeec */
	BG_PALETTE[HC_RED] = RGB15(28, 10, 7);      /* #e0503a */
	BG_PALETTE[HC_DIAG] = RGB15(18, 31, 18);
	BG_PALETTE[HC_MUTED] = RGB15(18, 18, 18);   /* #919190 */
	BG_PALETTE[HC_AMBER] = RGB15(31, 20, 3);    /* #ffa31a */
	BG_PALETTE[HC_ACCENT] = RGB15(6, 18, 26);   /* #3396d4 */
	BG_PALETTE[HC_BARRIER] = RGB15(11, 21, 31); /* #5aa8ff */
	BG_PALETTE[HC_GOLD] = RGB15(31, 24, 8);     /* #ffc440 */
	BG_PALETTE[HC_GREEN] = RGB15(12, 23, 3);    /* the gauge's #4e9a06 .. #8ae234 */
	BG_PALETTE[HC_WINDOW] = RGB15(6, 6, 6);     /* #353535 */
	BG_PALETTE[HC_FRAME] = RGB15(5, 5, 5);      /* #282828 */
	BG_PALETTE[HC_TROUGH] = RGB15(24, 24, 24);  /* #f3f3f3 .. #bdbdbd */
	BG_PALETTE[HC_FILL] = RGB15(4, 11, 21);     /* #2063b4, #15539e */
	BG_PALETTE[HC_XHAIR] = RGB15(0, 31, 0);
	bgSetPriority(0, 1);
	bgSetPriority(bg, 0);
	REG_DISPCNT |= DISPLAY_BG1_ACTIVE;
	hud_load_font();
	memset(&hud, 0, sizeof(hud));
	hud.self = -1;
	for (int i = 0; i < W_COUNT; i++) {
		memset(&wid[i], 0, sizeof(wid[i]));
		wid[i].buf = warena + off, wid[i].cap = wcap[i];
		off += wcap[i];
	}
}

void Hud_Done(void)
{
	REG_DISPCNT &= ~DISPLAY_BG1_ACTIVE;
	bgSetPriority(0, 0);
	vramSetBankF(VRAM_F_LCD);
	vramSetBankG(VRAM_G_LCD);
	topgfx = NULL;
	free(hudfont);
	hudfont = NULL;
}
