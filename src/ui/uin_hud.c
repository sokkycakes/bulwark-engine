/* Bulwark engine - native UI: Stiletto's in-game HUD (data/web/hud/app.js,
 * as Bulwark's Panorama port data/panorama/{layout,scripts,styles}/web/hud.*
 * draws it; design section 3). The client game creates the view as its
 * gecko "webcore_hud" and draws it with drawpic into its 4:3 HUD rectangle
 * (hud.qc:532-533), which reaches UI_DrawAt; hud_drawnative draws it over
 * the whole screen. One part_* per app.js drawer, in its order.
 *
 * Data comes from the cvars the client game publishes every frame
 * (HeroActions_PublishHud, hud.qc WebCoreHud_Draw), read only when some
 * cvar changed (Cvar_Generation) and formatted only when a value changed,
 * without snprintf. Everything is page 0 of the atlas: one draw call.
 * ISC licence, see LICENSE. */
#include "uin_local.h"

#include <math.h>
#include <stdlib.h>

#define HUD_PI 3.14159265f

/* --- constants, in the 1440 x 1080 HUD canvas -------------------------------------
 * (kept here rather than in uin_stiletto.h: only this file uses them) */

/* the health icon's shake: Godot hp.gd shake_intensity and shake_duration
 * (app.js:140-141, hud.js:31-33) */
#define HUD_SHAKE_PX 10.0f
#define HUD_SHAKE_S 0.5f
/* the ring: hudring is 2048 px drawn 1386 across (app.js:342), so 0.6768
 * canvas px per art px. Its alpha is 25/255 from art radius 947 to 997,
 * and 48/255 from 967 to 977 (measured in the art). */
#define HUD_RING_R0 640.9f
#define HUD_RING_R1 674.8f
#define HUD_RING_S0 654.5f
#define HUD_RING_S1 661.2f
/* 64 segments: a chord is at most 0.4 px off the circle at 640 x 480 */
#define HUD_RING_SEGS 64
#define RING_SEGS_MAX HUD_RING_SEGS
/* a name is cut with "..." to the port's label width (hud.css .name) */
#define HUD_NAME_W 140
/* Nudges, in screen px at 480 lines (scaled with the HUD), that put each
 * text where the Panorama port draws it (its labels sit in boxes where
 * app.js places a baseline, and our faces are baked at whole pixel sizes),
 * measured with tools/ui/uidiff.py. The EX count keeps app.js's place:
 * the port's .ex-count box draws it about 10 px higher than the canvas. */
#define NUDGE(px) ((px) * F.s * (UIN_FRAME_HUD_H / 480.0f))
#define HUD_DY_SPEED 0.0f
#define HUD_DY_TIMER -1.0f
#define HUD_DY_SCORE -2.0f
#define HUD_DX_SCORE_L -1.0f
#define HUD_DY_NAME 0.0f
#define HUD_DY_CLIP -1.0f
#define HUD_DY_CLIPSIZE 0.0f
#define HUD_DY_RELOAD -1.0f
#define HUD_DY_EXCOUNT 0.0f
#define HUD_DY_MAX -1.0f

/* --- the cvars (design 2.2) ---------------------------------------------------------- */

enum {
	CV_SPEED, CV_TIMER, CV_SCORE_L, CV_SCORE_R, CV_P1, CV_P2, CV_HEALTH, CV_HEALTH_MAX, CV_AMMO, CV_CLIP,
	CV_AMMO_MAX, CV_RELOAD, CV_CHARGES, CV_EX, CV_DRIVE, CV_DRIVE_MAX, CV_CD0, CV_CD1, CV_CD2, NUM_CV
};

static const char *const cv_names[NUM_CV] = {
	"webcore_hud_speed", "webcore_hud_timer", "webcore_hud_score_left", "webcore_hud_score_right",
	"webcore_hud_player1", "webcore_hud_player2", "webcore_hud_health", "webcore_hud_health_max",
	"webcore_hud_ammo", "webcore_hud_clip", "webcore_hud_ammo_max", "webcore_hud_reload",
	"webcore_hud_charges", "webcore_hud_ex", "webcore_hud_drive", "webcore_hud_drive_max",
	"webcore_hud_cooldown_0", "webcore_hud_cooldown_1", "webcore_hud_cooldown_2",
};

/* csprogs registers these late (and only in a match): looked up again
 * while missing */
static cvar_t *cv[NUM_CV];

static cvar_t *cvar(int i)
{
	if (!cv[i])
		cv[i] = Cvar_Find(cv_names[i]);
	return cv[i];
}

/* a number the way the page read it: fallback when unset or not a number */
static float cv_num(int i, float fallback)
{
	cvar_t *c = cvar(i);
	char *end;
	float v;
	if (!c || !c->string[0])
		return fallback;
	v = strtof(c->string, &end);
	return end == c->string || v != v ? fallback : v;
}

static const char *cv_str(int i)
{
	cvar_t *c = cvar(i);
	return c ? c->string : NULL;
}

/* --- state ------------------------------------------------------------------------------ */

/* a field's text, rebuilt only when its value changes */
typedef struct {
	int key; /* the value it was built from; INT32_MIN before the first */
	char text[24];
} field_t;

enum { HP_NORMAL, HP_LOW, HP_DEAD };

typedef struct {
	int gen;          /* Cvar_Generation of the last read; 0 reads at once */
	int hudtest;      /* ui_native_hudtest at the last read */
	/* the values (scripts/web/hud.js HudState) */
	int speed, seconds, score_l, score_r, clip, clip_size, charges, ex;
	float health, health_max, reload;
	bool at_max;
	float cooldown[3]; /* < 0: not shown */
	char name_l[32], name_r[32];
	/* their text */
	field_t f_speed, f_timer, f_score_l, f_score_r, f_clip, f_clip_size, f_reload, f_ex, f_cd[3];
	/* the health icon's damage shake (Godot hp.gd, as app.js:140-193) */
	bool have_health;
	float prev_health;
	float shake_t, shake_x, shake_y; /* seconds left; offset in design px */
	uint32_t seed;
	/* the ring's unit circle, built once */
	bool ring_ready;
	float ring_c[RING_SEGS_MAX + 1], ring_s[RING_SEGS_MAX + 1];
} hud_t;

/* --- formatting ------------------------------------------------------------------------- */

/* decimal digits of v (>= 0), zero-padded to at least pad; returns the end */
static char *put_uint(char *p, unsigned v, int pad)
{
	char tmp[12];
	int n = 0;
	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n < pad)
		tmp[n++] = '0';
	while (n)
		*p++ = tmp[--n];
	*p = 0;
	return p;
}

static char *put_int(char *p, int v)
{
	if (v < 0) {
		*p++ = '-';
		return put_uint(p, (unsigned)-(v + 1) + 1u, 1);
	}
	return put_uint(p, (unsigned)v, 1);
}

/* true if the field must be rebuilt for key */
static bool changed(field_t *f, int key)
{
	if (f->key == key && f->text[0])
		return false;
	f->key = key;
	return true;
}

/* Number.toFixed(1) of v >= 0 */
static void put_fixed1(char *p, float v)
{
	int tenths = (int)floorf(v * 10 + 0.5f);
	p = put_uint(p, (unsigned)(tenths / 10), 1);
	*p++ = '.';
	put_uint(p, (unsigned)(tenths % 10), 1);
}

/* --- reading -------------------------------------------------------------------------- */

static int js_round(float v) { return (int)floorf(v + 0.5f); }

static void set_health(hud_t *h, float health)
{
	/* any drop starts the shake (hud.js applyHudData) */
	if (h->have_health && health < h->prev_health)
		h->shake_t = HUD_SHAKE_S;
	h->health = health;
	h->prev_health = health;
	h->have_health = true;
}

/* the fixture of design 8 (and tools/ui/hud_fixture.cfg) */
static void read_test(hud_t *h)
{
	h->speed = 0;
	h->seconds = 0;
	h->score_l = h->score_r = 0;
	Str_Copy(h->name_l, "Player1", sizeof(h->name_l));
	Str_Copy(h->name_r, "Player2", sizeof(h->name_r));
	set_health(h, 4);
	h->health_max = 4;
	h->clip = h->clip_size = 6;
	h->charges = 3;
	h->ex = 3;
	h->reload = 0;
	h->at_max = true;
	h->cooldown[0] = h->cooldown[1] = h->cooldown[2] = -1;
}

/* scripts/web/hud.js refreshState: gethud's fields, then the cvars it
 * reads itself, which win */
static void read_cvars(hud_t *h)
{
	const char *s;
	float v, drive;
	h->speed = BW_CLAMP((int)floorf(cv_num(CV_SPEED, 0)), 0, 999);
	if ((s = cv_str(CV_TIMER)) && *s) {
		/* "M:SS" of the client game's clock (hud.qc:458-534) */
		char *colon;
		long m = strtol(s, &colon, 10);
		long sec = *colon == ':' ? strtol(colon + 1, NULL, 10) : 0;
		h->seconds = (int)BW_CLAMP(m * 60 + sec, 0, 99 * 3600);
	}
	h->score_l = (int)cv_num(CV_SCORE_L, (float)h->score_l);
	h->score_r = (int)cv_num(CV_SCORE_R, (float)h->score_r);
	if ((s = cv_str(CV_P1)))
		Str_Copy(h->name_l, s, sizeof(h->name_l));
	if ((s = cv_str(CV_P2)))
		Str_Copy(h->name_r, s, sizeof(h->name_r));
	if (cvar(CV_HEALTH))
		set_health(h, cv_num(CV_HEALTH, h->health));
	h->health_max = BW_MAX(1.0f, cv_num(CV_HEALTH_MAX, h->health_max));
	/* ammoCurrent and ammoMax from gethud, then webcore_hud_clip and
	 * _ammo_max read directly (both the same cvars but for the fallback to
	 * webcore_hud_ammo) */
	if ((v = cv_num(CV_CLIP, -1)) >= 0)
		h->clip = js_round(v);
	if ((v = cv_num(CV_AMMO_MAX, -1)) >= 0)
		h->clip_size = js_round(v);
	else if ((v = cv_num(CV_AMMO, -1)) >= 0)
		h->clip_size = js_round(v);
	if (cvar(CV_CHARGES))
		h->charges = BW_CLAMP(js_round(cv_num(CV_CHARGES, 0)), 0, 3);
	h->ex = (int)cv_num(CV_EX, (float)h->ex);
	h->reload = BW_MAX(0.0f, cv_num(CV_RELOAD, h->reload));
	drive = cv_num(CV_DRIVE, -1);
	if (drive >= 0)
		h->at_max = drive >= BW_MAX(1.0f, cv_num(CV_DRIVE_MAX, 1000));
	/* nothing in Stiletto writes these: the drawer is kept, never shown */
	for (int i = 0; i < 3; i++)
		h->cooldown[i] = cv_num(CV_CD0 + i, -1);
}

static void format(hud_t *h)
{
	char *p;
	if (changed(&h->f_speed, h->speed))
		put_uint(h->f_speed.text, (unsigned)h->speed, 3);
	if (changed(&h->f_timer, h->seconds)) {
		p = put_uint(h->f_timer.text, (unsigned)(h->seconds / 60), 2);
		*p++ = ':';
		put_uint(p, (unsigned)(h->seconds % 60), 2);
	}
	if (changed(&h->f_score_l, h->score_l))
		put_int(h->f_score_l.text, h->score_l);
	if (changed(&h->f_score_r, h->score_r))
		put_int(h->f_score_r.text, h->score_r);
	if (changed(&h->f_clip, h->clip)) {
		p = put_int(h->f_clip.text, h->clip);
		*p++ = '|';
		*p = 0;
	}
	if (changed(&h->f_clip_size, h->clip_size))
		put_int(h->f_clip_size.text, h->clip_size);
	if (h->reload > 0 && changed(&h->f_reload, js_round(h->reload * 10)))
		put_fixed1(h->f_reload.text, h->reload);
	if (changed(&h->f_ex, h->ex))
		put_int(h->f_ex.text, h->ex);
	for (int i = 0; i < 3; i++) {
		float cd = h->cooldown[i];
		/* toFixed(1) under 10 s, whole seconds above; blank when ready */
		int key = cd <= 0 ? -1 : cd < 10 ? js_round(cd * 10) : 100000 + js_round(cd);
		if (!changed(&h->f_cd[i], key))
			continue;
		if (cd <= 0)
			h->f_cd[i].text[0] = 0;
		else if (cd < 10)
			put_fixed1(h->f_cd[i].text, cd);
		else
			put_int(h->f_cd[i].text, js_round(cd));
	}
}

/* --- the frame ---------------------------------------------------------------------------- */

static void hud_open(uin_view_t *v)
{
	hud_t *h = UIN_STATE(v, hud_t);
	field_t *fields[] = {&h->f_speed, &h->f_timer, &h->f_score_l, &h->f_score_r, &h->f_clip, &h->f_clip_size,
	                     &h->f_reload, &h->f_ex, &h->f_cd[0], &h->f_cd[1], &h->f_cd[2]};
	for (size_t i = 0; i < BW_ARRAY_COUNT(fields); i++)
		fields[i]->key = INT32_MIN;
	/* HudState's starting values (hud.js) */
	h->health = h->health_max = 100;
	h->charges = 3;
	Str_Copy(h->name_l, "P1", sizeof(h->name_l));
	Str_Copy(h->name_r, "P2", sizeof(h->name_r));
	h->cooldown[0] = h->cooldown[1] = h->cooldown[2] = -1;
	h->seed = 0x2545f491u;
	h->hudtest = -1;
}

static void hud_frame(uin_view_t *v, double dt)
{
	hud_t *h = UIN_STATE(v, hud_t);
	int gen = Cvar_Generation();
	if (gen != h->gen || ui_native_hudtest->ivalue != h->hudtest) {
		h->gen = gen;
		h->hudtest = ui_native_hudtest->ivalue;
		if (h->hudtest)
			read_test(h);
		else
			read_cvars(h);
		format(h);
	}
	/* the shake: a random offset each frame, its size falling linearly to
	 * 0 over HUD_SHAKE_S (hud.js updateHpShake; dt capped at 0.1 s) */
	if (h->shake_t > 0) {
		h->shake_t -= (float)BW_MIN(dt, 0.1);
		if (h->shake_t <= 0) {
			h->shake_t = 0;
			h->shake_x = h->shake_y = 0;
		} else {
			float amp = HUD_SHAKE_PX * h->shake_t / HUD_SHAKE_S;
			h->seed = h->seed * 1664525u + 1013904223u;
			h->shake_x = ((h->seed >> 8) / 8388608.0f - 1) * amp;
			h->seed = h->seed * 1664525u + 1013904223u;
			h->shake_y = ((h->seed >> 8) / 8388608.0f - 1) * amp;
		}
	}
}

/* --- drawing ------------------------------------------------------------------------------ */

/* the frame of one draw: design (1440x1080) to screen */
typedef struct {
	float s;      /* min(w / 1440, h / 1080) */
	float ox, oy; /* screen of design (0,0) for the centred stage */
	float lx;     /* screen x of design 0 for the left panel */
	float rgb[3], alpha; /* UI_DrawAt's tint */
} frame_t;

static frame_t F;

#define SX(x) (F.ox + (x) * F.s)
#define SY(y) (F.oy + (y) * F.s)
#define LX(x) (F.lx + (x) * F.s)

/* a colour times the drawpic's tint */
static uint32_t tint(uint32_t rgba)
{
	float r = (rgba >> 24) * F.rgb[0], g = ((rgba >> 16) & 255) * F.rgb[1], b = ((rgba >> 8) & 255) * F.rgb[2];
	float a = (rgba & 255) * F.alpha;
	return UIN_RGBA((uint32_t)BW_CLAMP(r + 0.5f, 0, 255), (uint32_t)BW_CLAMP(g + 0.5f, 0, 255),
	                (uint32_t)BW_CLAMP(b + 0.5f, 0, 255), (uint32_t)BW_CLAMP(a + 0.5f, 0, 255));
}

/* an image at screen x y w h. When the size is the baked one (it is, at
 * the layout height the atlas was baked for) it is drawn 1:1 on whole
 * pixels, centred where it was asked for: the art stays sharp. */
static void image(int region, float x, float y, float w, float h, uint32_t rgba)
{
	const uin_region_t *r = uin_region(region);
	if (!r || !r->ok)
		return;
	if (fabsf(r->w - w) < 1 && fabsf(r->h - h) < 1) {
		x = floorf(x + (w - r->w) * 0.5f + 0.5f);
		y = floorf(y + (h - r->h) * 0.5f + 0.5f);
		w = r->w;
		h = r->h;
	}
	d_img(region, x, y, w, h, rgba);
}

static void outline(float x, float y, float w, float h, uint32_t rgba)
{
	d_fill(x, y, w, 1, rgba);
	d_fill(x, y + h - 1, w, 1, rgba);
	d_fill(x, y + 1, 1, h - 2, rgba);
	d_fill(x + w - 1, y + 1, 1, h - 2, rgba);
}

/* the decorative ring (app.js:322-356): the 2048^2 hudring art at half
 * opacity in a 1386 px square about the centre. Drawn as its radial profile
 * instead (design 3.2 #1): a band at alpha 25/255 with a stripe at 48/255
 * in it, both halved. The stripe goes over the band at the alpha that makes
 * the two composite to the art's. */
static void part_ring(hud_t *h, float cx, float cy)
{
	static const float band[2] = {HUD_RING_R0, HUD_RING_R1}, stripe[2] = {HUD_RING_S0, HUD_RING_S1};
	const float a_band = 0.5f * 25 / 255, a_stripe = 0.5f * 48 / 255;
	const float a_over = (a_stripe - a_band) / (1 - a_band);
	int segs = HUD_RING_SEGS;
	if (!h->ring_ready) {
		for (int i = 0; i <= segs; i++) {
			h->ring_c[i] = cosf(2 * HUD_PI * i / segs);
			h->ring_s[i] = sinf(2 * HUD_PI * i / segs);
		}
		h->ring_ready = true;
	}
	for (int pass = 0; pass < 2; pass++) {
		const float *r = pass ? stripe : band;
		float r0 = r[0] * F.s, r1 = r[1] * F.s;
		uint32_t c = tint(UIN_HEXA(0xffffff, pass ? a_over : a_band));
		uint32_t c4[4] = {c, c, c, c};
		for (int i = 0; i < segs; i++) {
			float xy[8] = {cx + h->ring_c[i] * r0,     cy + h->ring_s[i] * r0,     cx + h->ring_c[i] * r1,
			               cy + h->ring_s[i] * r1,     cx + h->ring_c[i + 1] * r1, cy + h->ring_s[i + 1] * r1,
			               cx + h->ring_c[i + 1] * r0, cy + h->ring_s[i + 1] * r0};
			d_quad4(xy, c4);
		}
	}
}

/* speedometer (app.js:365-376): zero-padded, faint, under the crosshair */
static void part_speed(hud_t *h)
{
	d_text(UIN_F_SPEED, SX(720), SY(928) + NUDGE(HUD_DY_SPEED), h->f_speed.text, UIN_CENTER | UIN_MIDDLE,
	       tint(UIN_HEXA(0xd9e0e5, 0.54f)));
}

/* match header (app.js:412-487): time plate, clock, scores, nameplates */
static void part_header(hud_t *h)
{
	uint32_t white = tint(0xffffffffu), score = tint(0xc7c7c7ffu);
	char name[32];
	float maxw = HUD_NAME_W * F.s;
	image(UIN_R_TIMEPLATE, SX(596), SY(17), 248 * F.s, 164 * F.s, white);
	d_text(UIN_F_TIMER, SX(720), SY(120.46f) + NUDGE(HUD_DY_TIMER), h->f_timer.text, UIN_CENTER | UIN_MIDDLE, white);
	d_text(UIN_F_SCORE, SX(642) + NUDGE(HUD_DX_SCORE_L), SY(115.5f) + NUDGE(HUD_DY_SCORE), h->f_score_l.text,
	       UIN_RIGHT | UIN_MIDDLE, score);
	d_text(UIN_F_SCORE, SX(803), SY(115.38f) + NUDGE(HUD_DY_SCORE), h->f_score_r.text, UIN_LEFT | UIN_MIDDLE,
	       score);
	/* the left plate: app.js paints its blue gradient, then flat pink over
	 * it (app.js:449-455); the right one keeps the gradient, top to bottom */
	d_fill(SX(482), SY(152), 159.843f * F.s, 34.817f * F.s, tint(0xf88282ffu));
	d_grad_v(SX(803), SY(152), 160.635f * F.s, 34.817f * F.s, tint(UIN_HEXA(0x3698e2, 0.95f)),
	         tint(UIN_HEXA(0x14568f, 0.95f)));
	/* names: white on both plates, as the port draws them (app.js leaves
	 * the right one in its plate's gradient, unreadable) */
	d_text(UIN_F_NAME, SX(494.66f), SY(161.5f) + NUDGE(HUD_DY_NAME),
	       d_text_fit(UIN_F_NAME, h->name_l, maxw, name, sizeof(name)), UIN_LEFT | UIN_TOP, white);
	d_text(UIN_F_NAME, SX(951.6f), SY(161.5f) + NUDGE(HUD_DY_NAME),
	       d_text_fit(UIN_F_NAME, h->name_r, maxw, name, sizeof(name)), UIN_RIGHT | UIN_TOP, white);
	/* the opponent's three EX pips: always full, not bound to data */
	{
		static const float px[3] = {813.29f, 835.45f, 856.81f};
		float d = 20.574f * F.s;
		for (int i = 0; i < 3; i++)
			image(UIN_R_MINIPIP, SX(px[i]) - d / 2, SY(202.25f) - d / 2, d, d, white);
	}
}

/* centre status (app.js:489-565): health icon, clip, ability pips, reload */
static void part_center(hud_t *h)
{
	uint32_t white = tint(0xffffffffu);
	int hp = js_round(h->health);
	int state = hp <= 0 ? HP_DEAD : hp == 1 ? HP_LOW : HP_NORMAL;
	float sx = h->shake_x * F.s, sy = h->shake_y * F.s;
	uint32_t heart = white;
	/* the fill tints, by vertex colour: the art's white fill takes it and
	 * its black outline stays black, as app.js's fill-only multiply. Red in
	 * the first fifth of a shake, blue at full health with none running. */
	if (h->shake_t > HUD_SHAKE_S * 0.8f)
		heart = tint(0xff0000ffu);
	else if (state == HP_NORMAL && h->shake_t <= 0 && hp > 0 && hp >= h->health_max)
		heart = tint(0x4d99ffffu);
	if (state == HP_DEAD) {
		/* app.js's size and place: the port draws the X in the heart's
		 * 35 px box instead, about a third smaller */
		float d = 50.865f * F.s;
		image(UIN_R_HEARTDEAD, SX(694.62f) + sx, SY(591.13f) + sy, d, d, heart);
	} else {
		float d = 35 * F.s;
		image(state == HP_LOW ? UIN_R_HEARTLOW : UIN_R_HEART, SX(702) + sx, SY(600) + sy, d, d, heart);
	}
	d_text(UIN_F_CLIP, SX(774.5f), SY(588.5f) + NUDGE(HUD_DY_CLIP), h->f_clip.text, UIN_CENTER | UIN_MIDDLE, white);
	d_text(UIN_F_CLIPSIZE, SX(803), SY(584) + NUDGE(HUD_DY_CLIPSIZE), h->f_clip_size.text, UIN_CENTER | UIN_MIDDLE,
	       white);
	{
		static const float px[3] = {669.66f, 664.71f, 683.27f}, py[3] = {583.51f, 603.05f, 596.24f};
		float d = 17.37f * F.s;
		for (int i = 0; i < 3; i++)
			d_img_rot(UIN_R_PIP, SX(px[i]), SY(py[i]), d, d, -45,
			          tint(UIN_HEXA(0xffffff, i < h->charges ? 0.7f : 0.2f)));
	}
	if (h->reload > 0)
		d_text(UIN_F_RELOAD, SX(787), SY(601) + NUDGE(HUD_DY_RELOAD), h->f_reload.text, UIN_CENTER | UIN_MIDDLE,
		       tint(0xe5e5ebffu));
}

/* resource panel (app.js:567-595), from the left edge: EX plate, the
 * Drive gauge (always full, as shipped), EX count, MAX */
static void part_resource(hud_t *h)
{
	uint32_t white = tint(0xffffffffu);
	image(UIN_R_EXPLATE, LX(71), SY(954), 86 * F.s, 33 * F.s, white);
	image(UIN_R_GAUGE, LX(63), SY(921.7f), 482 * F.s, 31.6f * F.s, white);
	d_text(UIN_F_EXCOUNT, LX(304), SY(945.5f) + NUDGE(HUD_DY_EXCOUNT), h->f_ex.text, UIN_CENTER | UIN_MIDDLE, white);
	if (h->at_max)
		d_text(UIN_F_MAX, LX(467), SY(917) + NUDGE(HUD_DY_MAX), "MAX", UIN_CENTER | UIN_MIDDLE, white);
}

/* cooldowns (app.js:597-641): one per webcore_hud_cooldown_i >= 0. No
 * QuakeC writes those, so this never shows in a match today. The icons
 * aren't clipped to their circle (their corners are blank anyway). */
static void part_cooldowns(hud_t *h)
{
	static const int icon[3] = {UIN_R_CD0, UIN_R_CD1, UIN_R_CD2};
	float d = 20 * F.s;
	for (int i = 0; i < 3; i++) {
		float cx = SX(659.5f + 60 * i), cy = SY(947);
		if (h->cooldown[i] < 0)
			continue;
		image(icon[i], cx - d / 2, cy - d / 2, d, d, tint(UIN_HEXA(0xffffff, h->cooldown[i] <= 0 ? 1.0f : 0.5f)));
		d_annulus(cx, cy, d / 2 - 0.5f, d / 2 + 0.5f, 24, tint(UIN_HEXA(0xffffff, 0.2f)));
		if (h->f_cd[i].text[0])
			d_text(UIN_F_COOLDOWN, cx, SY(949), h->f_cd[i].text, UIN_CENTER | UIN_MIDDLE, tint(0xe5e5ebffu));
	}
}

/* ui_hud_debug: HUD_LAYOUT's cluster boxes (app.js:12-19), 1 px outlines */
static void part_debug(float x, float y, float w, float h)
{
	static const struct {
		float w, h, oy;
		int ax, ay; /* 0 left/top, 1 centre, 2 right/bottom */
		uint32_t rgba;
	} box[] = {
		{1386, 1386, 0, 1, 1, 0xffc864b0u}, {756, 317, 0, 1, 0, 0x64c8ffb0u}, {400, 360, 0, 1, 1, 0xc864ffb0u},
		{50, 50, 0, 1, 1, 0xff6464ffu},     {546, 261, -93, 0, 2, 0x64ff64b0u}, {413, 118, -54, 1, 2, 0xffff64b0u},
	};
	for (size_t i = 0; i < BW_ARRAY_COUNT(box); i++) {
		float bw = box[i].w * F.s, bh = box[i].h * F.s;
		float bx = box[i].ax == 0 ? x : box[i].ax == 1 ? x + (w - bw) / 2 : x + w - bw;
		float by = (box[i].ay == 0 ? y : box[i].ay == 1 ? y + (h - bh) / 2 : y + h - bh) + box[i].oy * F.s;
		outline(floorf(bx), floorf(by), floorf(bw), floorf(bh), box[i].rgba);
	}
	outline(floorf(x), floorf(y), floorf(w), floorf(h), 0xffffff80u);
}

static void hud_draw(uin_view_t *v, float x, float y, float w, float h)
{
	hud_t *st = UIN_STATE(v, hud_t);
	float sx, sy, sw, sh, x1, y1, cx, cy, rx = x, ry = y, rw = w, rh = h;
	if (!st->f_speed.text[0]) /* drawn before any UI_Frame */
		hud_frame(v, 0);
	/* the drawpic's rectangle, inside the TV-safe one (design 3.1) */
	uin_safe_rect(&sx, &sy, &sw, &sh);
	x1 = BW_MIN(x + w, sx + sw);
	y1 = BW_MIN(y + h, sy + sh);
	x = BW_MAX(x, sx);
	y = BW_MAX(y, sy);
	w = x1 - x;
	h = y1 - y;
	if (w < 1 || h < 1)
		return;
	/* the canvas scales by its height (app.js:106); the port's stage is a
	 * 1440 x 1080 panel centred in the view and its resource panel hangs
	 * off the left edge, both scaled about their middles (hud.js fitView) */
	F.s = BW_MIN(w / UIN_FRAME_HUD_W, h / UIN_FRAME_HUD_H);
	cx = x + w / 2;
	cy = y + h / 2;
	F.ox = cx - UIN_FRAME_HUD_W / 2 * F.s;
	F.oy = cy - UIN_FRAME_HUD_H / 2 * F.s;
	F.lx = x;
	for (int i = 0; i < 3; i++)
		F.rgb[i] = v->rgb[i];
	F.alpha = v->alpha;
	d_xf(NULL);
	if (ui_hud_debug->ivalue)
		part_debug(x, y, w, h);
	/* the ring runs off the top and bottom: clipped to the drawpic's
	 * rectangle, not the safe one (design 3.1), when that isn't the screen */
	{
		int scw, sch;
		R_GetSize(&scw, &sch);
		if (rx > 0.5f || ry > 0.5f || rw < scw - 0.5f || rh < sch - 0.5f) {
			d_clip(rx, ry, rw, rh);
			part_ring(st, SX(720), SY(540));
			d_unclip();
		} else {
			part_ring(st, SX(720), SY(540));
		}
	}
	part_speed(st);
	part_header(st);
	part_center(st);
	part_resource(st);
	part_cooldowns(st);
}

const uin_screen_t uin_hud_screen = {"hud", hud_open, NULL, NULL, hud_frame, hud_draw};
