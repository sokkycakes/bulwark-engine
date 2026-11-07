/* Bulwark engine - the DS link's EVENT records, shown as Nuclide's client shows
 * them. ISC licence, see LICENSE. events_nds.h has the overview; each part below
 * names the client code it follows.
 *
 * Times run on the link's clock in seconds, and each part steps by the frame's
 * time as the QC steps by clframetime. Text is drawn into its layers (BG2 in front of
 * the HUD, BG3 behind it for game_text) only when what shows changes; each text group
 * has its own palette bank, so its fading is a palette write a frame. The PC's text
 * fades to nothing: when every group up is as strong as the others, the layers blend
 * over what is under them at that alpha (REG_BLDALPHA, one for the whole screen);
 * when they differ, the letters darken to their black outline instead. */
#include <math.h>
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "events_nds.h"

enum { LE_PRINT = 1, LE_CENTER, LE_SHAKE, LE_FADE, LE_DAMAGE, LE_TEXT, LE_FEED, LE_PICKUP, LE_TEXTAGAIN, LE_CENTERAGAIN, LE_FLASH };
#define PRINT_CHAT 3

evstats_t Events_Stats;

static uint16_t have;     /* the last record taken, in order */
static bool wasalive;     /* View.qc _m_bWasAlive */
static float now, ftime;  /* the link's clock, and the last frame's time (clframetime) */
static bool started;

static float rdf(const uint8_t *p)
{
	uint32_t u = (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
	float f;
	memcpy(&f, &u, 4);
	return f;
}
static int rds16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

static float frand(float lo, float hi) { return lo + (hi - lo) * (float)(rand() & 0x7fff) / 32767.0f; }

/* --- the view shake: src/client/shake.qc --------------------------------------------- */

static struct {
	float amp, dur, until, freq, next, off[3], ang;
	float apply[3], roll; /* this frame's */
} sh;

static void shake_start(float amplitude, float frequency, float duration)
{
	if (amplitude > sh.amp)
		sh.amp = amplitude;
	sh.dur = duration;
	sh.until = now + duration;
	sh.freq = frequency;
	sh.next = 0;
}

static void shake_stop(void) { memset(&sh, 0, sizeof(sh)); }

/* Shake_Update: a new random offset every freq / duration seconds, swung on a sine
 * that dies out as the square of the time left */
static void shake_update(void)
{
	float fraction, freq;
	sh.apply[0] = sh.apply[1] = sh.apply[2] = sh.roll = 0;
	if (now > sh.until || sh.amp <= 0 || sh.freq <= 0 || sh.dur <= 0)
		return;
	if (now > sh.next) {
		float amp = sh.amp;
		sh.next = now + sh.freq / sh.dur;
		for (int k = 0; k < 3; k++)
			sh.off[k] = frand(-amp, amp);
		sh.ang = frand(-amp * 0.25f, amp * 0.25f);
	}
	fraction = (now - sh.until) / sh.dur;
	freq = fraction != 0 ? (sh.freq / fraction) * sh.freq : 0;
	fraction *= fraction * sinf(now * freq);
	for (int k = 0; k < 3; k++)
		sh.apply[k] = sh.off[k] * fraction;
	sh.roll = sh.ang * fraction;
	sh.amp -= sh.amp * (ftime / (sh.freq * sh.dur));
}

/* --- the fade: src/client/fade.qc ------------------------------------------------------ */

enum { EVF_FADEDROM = 1, EVF_MODULATE = 2 };
static struct {
	bool active;
	float col[3], maxa, dur, hold, time, alpha;
	int style;
} fd;

/* Fade_Update: in over the duration (or out, FADEFROM), held, then gone */
static void fade_update(void)
{
	if (!fd.active)
		return;
	fd.alpha = 1;
	if (fd.style & EVF_FADEDROM) {
		if (fd.time > fd.dur)
			fd.alpha -= (fd.time - fd.dur) * (1.0f / fd.hold);
	} else if (fd.time < fd.dur)
		fd.alpha = fd.time * (1.0f / fd.dur);
	fd.alpha = fd.alpha > 1 ? 1 : fd.alpha < 0 ? 0 : fd.alpha;
	if (fd.time >= fd.dur + fd.hold) {
		fd.active = false;
		return;
	}
	fd.time += ftime;
}

/* --- damage: CSQC_Parse_Damage_New (src/client/damage.qc), HUD_DamageReceived and
 * the arrows and tint of coop/src/hud/jtf.qc Jtf_DrawFeedback ----------------------- */

static struct {
	float alpha, pos[3];
} dm;

/* the engine's "bf" (src/client/cl_cmds.c BonusFlash_f): gold (215, 186, 69) at 50/255,
 * losing 100/255 a second (src/render/r_overlay.c draw_screen_tints) */
static float flash;
#define CG_DAMAGEFILL 0.25f /* valve/src/hud/hud.h autocvar_cg_damageFill */

/* --- text ------------------------------------------------------------------------------ */

/* the band of the text's layer (BG2) this draws in: under the frame rate lines, over health and ammo */
#define BAND_ROW0 3
#define BAND_ROWS 16
#define BAND_Y0 (BAND_ROW0 * 8)
#define BAND_Y1 ((BAND_ROW0 + BAND_ROWS) * 8)
/* the PC's HUD numbers are in its 2D pixels; its HUD is laid out for 480 rows (hypothesis:
 * Nuclide's vid_conautoscale gives 480 to 540 at 1080p), so a PC pixel is 0.4 of ours */
#define PCY(v) ((int)((v) * 192 / 480))

enum { G_CENTER, G_PRINT, G_CHAT, G_TEXT0, G_QUEUE = G_TEXT0 + 5, G_FEED, G_PICK0, G_COUNT = G_PICK0 + 3 };
#define TEXT_LEN 200
#define TEXTQUEUE_MAX 6 /* src/client/text.h: GameMessage_Setup keeps at most 5 */

typedef struct {
	char msg[TEXT_LEN];
	float x, y, fadein, fadeout, hold, fx, time;
	int effect;
	uint8_t c1[3], c2[3];
} gtext_t; /* hudText_t */

static struct {
	/* prints.qc: five lines, the newest dropped every 5 s */
	char print[5][TEXT_LEN];
	int printlines;
	float printtime;
	/* chat.qc: five lines; what is drawn is made when a line comes */
	char chat[5][TEXT_LEN];
	int chatlines;
	float chattime;
	char chatfinal[5 * TEXT_LEN];
	/* the centerprint, 3 s and a second's fade */
	char center[TEXT_LEN];
	float centeralpha, centertime;
	/* game_text channels 0-4, and channel 0's queue for env_message */
	gtext_t text[5];
	gtext_t queue[TEXTQUEUE_MAX - 1];
	int queued;
	/* the kill feed: five lines, six seconds each, newest on top (coop panorama hud.js) */
	struct {
		char a[40], w[40], v[40];
		bool you;
		float until;
	} feed[5];
	int feeds;
	/* pickups (HLHUD_ItemNotify): battery, health kit, long jump */
	float pickalpha[3];
	int pickcount[3];
	/* what was last drawn */
	uint32_t sig[G_COUNT];
	float alpha[G_COUNT];
	uint16_t rgb[G_COUNT][3];
} tx;

static void copy_text(char *dst, int size, const uint8_t *src, int len)
{
	int n = 0;
	while (n < len && n < size - 1 && src[n])
		dst[n] = (char)src[n], n++;
	dst[n] = 0;
}

/* CSQC_Parse_Print */
static void take_print(int level, const char *msg)
{
	if (level == PRINT_CHAT) { /* Chat_Parse */
		if (tx.chatlines < 4)
			strcpy(tx.chat[++tx.chatlines], msg);
		else {
			for (int i = 0; i < 4; i++)
				strcpy(tx.chat[i], tx.chat[i + 1]);
			strcpy(tx.chat[4], msg);
		}
		tx.chattime = now + 5;
		tx.chatfinal[0] = 0;
		for (int i = 0; i < 5; i++) {
			if (i)
				strcat(tx.chatfinal, "\n");
			strcat(tx.chatfinal, tx.chat[i]);
		}
		return;
	}
	if (tx.printlines < 4)
		strcpy(tx.print[++tx.printlines], msg);
	else {
		for (int i = 0; i < 4; i++)
			strcpy(tx.print[i], tx.print[i + 1]);
		strcpy(tx.print[4], msg);
	}
	tx.printtime = now + 5;
}

static void take_text(const uint8_t *d, int len)
{
	gtext_t *t, q;
	int chan = d[0];
	if (len < 32)
		return;
	if (chan == 0xff) { /* GameMessage_Setup channel 0: the queue */
		if (tx.queued + 1 >= TEXTQUEUE_MAX) { /* text.qc GameMessage_Setup: the oldest goes */
			memmove(&tx.queue[0], &tx.queue[1], sizeof(gtext_t) * (TEXTQUEUE_MAX - 2));
			tx.queued--;
		}
		t = &tx.queue[tx.queued++];
	} else if (chan <= 4)
		t = &tx.text[chan];
	else
		t = &q;
	copy_text(t->msg, TEXT_LEN, d + 32, len - 32);
	t->x = rdf(d + 1), t->y = rdf(d + 5);
	t->effect = d[9];
	memcpy(t->c1, d + 10, 3), memcpy(t->c2, d + 13, 3);
	t->fadein = rdf(d + 16), t->fadeout = rdf(d + 20), t->hold = rdf(d + 24), t->fx = rdf(d + 28);
	t->time = 0;
}

static void take_feed(const uint8_t *d, int len)
{
	int n = 1, la, lw;
	if (len < 3 || (la = d[n]) + 2 > len - 1)
		return;
	if (tx.feeds == 5)
		memmove(&tx.feed[0], &tx.feed[1], sizeof(tx.feed[0]) * 4), tx.feeds--;
	copy_text(tx.feed[tx.feeds].a, 40, d + n + 1, la);
	n += 1 + la;
	lw = d[n];
	copy_text(tx.feed[tx.feeds].w, 40, d + n + 1, BW_NDS_MIN(lw, len - n - 1));
	n += 1 + lw;
	copy_text(tx.feed[tx.feeds].v, 40, d + n, BW_NDS_MAX(len - n, 0));
	tx.feed[tx.feeds].you = d[0] != 0;
	tx.feed[tx.feeds].until = now + 6;
	tx.feeds++;
}

/* HUD_PickedUpItem: only these three show (valve/src/hud/hud.qc:162) */
static void take_pickup(const char *cls)
{
	static const char *items[3] = {"item_battery", "item_healthkit", "item_longjump"};
	for (int i = 0; i < 3; i++)
		if (!strcmp(cls, items[i])) {
			tx.pickcount[i] += 1;
			tx.pickalpha[i] = 2.5f;
		}
}

static void take(int type, const uint8_t *d, int len)
{
	char s[TEXT_LEN];
	Events_Stats.taken++;
	switch (type) {
	case LE_PRINT:
		if (len >= 1) {
			copy_text(s, sizeof(s), d + 1, len - 1);
			take_print(d[0], s);
		}
		break;
	case LE_CENTER: /* CSQC_Parse_CenterPrint */
		copy_text(tx.center, TEXT_LEN, d, len);
		tx.centeralpha = 1;
		tx.centertime = now + 3;
		break;
	case LE_CENTERAGAIN: /* the same centerprint again: its 3 s start over */
		if (tx.center[0]) {
			tx.centeralpha = 1;
			tx.centertime = now + 3;
		}
		break;
	case LE_FLASH:
		flash = 50 / 255.0f;
		break;
	case LE_SHAKE: /* EV_Shake: duration, amplitude, frequency */
		if (len >= 12)
			shake_start(rdf(d + 4), rdf(d + 8), rdf(d));
		break;
	case LE_FADE: /* Fade_Parse; the colour is used as it comes (0-255): FTE's drawfill
	               * clamps it, so any channel above 0 is full (hypothesis) */
		if (len >= 13) {
			for (int k = 0; k < 3; k++)
				fd.col[k] = d[k] ? 1.0f : 0.0f;
			fd.maxa = d[3] / 255.0f;
			fd.dur = rdf(d + 4), fd.hold = rdf(d + 8);
			fd.style = d[12];
			fd.time = 0;
			fd.alpha = fd.style & EVF_FADEDROM ? 1 : 0;
			fd.active = true;
		}
		break;
	case LE_DAMAGE: /* CSQC_Parse_Damage_New, HUD_DamageReceived */
		if (len >= 12 && rds16(d + 6) > 0 && (rds16(d) || rds16(d + 2) || rds16(d + 4))) {
			for (int k = 0; k < 3; k++)
				dm.pos[k] = (float)rds16(d + k * 2);
			dm.alpha = 1;
		}
		break;
	case LE_TEXT:
		take_text(d, len);
		break;
	case LE_TEXTAGAIN: /* the same game_text again: it starts over */
		if (len >= 1 && d[0] <= 4)
			tx.text[d[0]].time = 0;
		break;
	case LE_FEED:
		take_feed(d, len);
		break;
	case LE_PICKUP:
		copy_text(s, sizeof(s), d, len);
		take_pickup(s);
		break;
	}
}

void Events_Reset(void)
{
	have = 0;
	started = false;
	wasalive = false;
	memset(&sh, 0, sizeof(sh));
	memset(&fd, 0, sizeof(fd));
	memset(&dm, 0, sizeof(dm));
	memset(&tx, 0, sizeof(tx));
	tx.printlines = tx.chatlines = -1;
	flash = 0;
	memset(&Events_Stats, 0, sizeof(Events_Stats));
	/* translucent polygons blend only with this on (the fade, the damage feedback, and
	 * link_nds.c's impact effects, whose alphas were drawn solid before) */
	glEnable(GL_BLEND);
}

/* a new map: FTE starts the PC's CSQC again, so what it showed goes (the shake, the fade,
 * the damage cues, prints, chat, centerprint, game_text and its queue, the feed, the
 * pickups); records keep their numbers. What was drawn stays known, so it is cleared */
void Events_MapChange(void)
{
	uint32_t sig[G_COUNT];
	memcpy(sig, tx.sig, sizeof(sig));
	memset(&sh, 0, sizeof(sh));
	memset(&fd, 0, sizeof(fd));
	memset(&dm, 0, sizeof(dm));
	memset(&tx, 0, sizeof(tx));
	tx.printlines = tx.chatlines = -1;
	memcpy(tx.sig, sig, sizeof(sig));
	flash = 0;
}

uint16_t Events_Ack(void) { return have; }

/* u32 session, u16 first, u8 count (its top bit: only new records, the ones before
 * first are on their way), then count records of u8 type, u8 length, payload */
void Events_Packet(const uint8_t *d, int len, uint32_t ms)
{
	uint16_t seq;
	int count, i = 7;
	if (len < 7)
		return;
	if (!started)
		now = ms / 1000.0f, started = true;
	seq = (uint16_t)(d[4] | d[5] << 8);
	count = d[6] & 0x7f;
	/* new records past one we lack: the resend of all of them follows */
	if (d[6] & 0x80 && (int16_t)(seq - (uint16_t)(have + 1)) > 0)
		return;
	/* the server sends from the record after the last we acknowledged; a first one past
	 * ours means it dropped some (its ring was full): take on from there */
	if ((int16_t)(seq - (uint16_t)(have + 1)) > 0) {
		Events_Stats.gaps += (uint16_t)(seq - have - 1);
		have = (uint16_t)(seq - 1);
	}
	for (int k = 0; k < count && i + 2 <= len; k++, seq++) {
		int type = d[i], n = d[i + 1];
		if (i + 2 + n > len)
			break;
		if (seq == (uint16_t)(have + 1)) {
			take(type, d + i + 2, n);
			have = seq;
		}
		i += 2 + n;
	}
}

void Events_Frame(uint32_t ms, int health)
{
	float t = ms / 1000.0f;
	ftime = started ? t - now : 0;
	ftime = ftime < 0 ? 0 : ftime > 0.25f ? 0.25f : ftime;
	now = t, started = true;
	/* ncView::UpdateView (src/client/View.qc:335-345): when the player dies, the damage
	 * cues and the shake go, once; a shake that starts while dead still plays */
	if (health <= 0) {
		if (wasalive) {
			dm.alpha = 0;
			shake_stop();
		}
		wasalive = false;
	} else
		wasalive = true;
	shake_update();
	fade_update();
	Events_Stats.shaking = sh.apply[0] != 0 || sh.roll != 0;
	Events_Stats.fading = fd.active;
	Events_Stats.damage = dm.alpha > 0;
}

void Events_View(ndscam_t *view)
{
	/* SetCameraOrigin + the offset; the roll on the camera's angles (degrees) */
	for (int k = 0; k < 3; k++)
		view->org[k] += (int32_t)(sh.apply[k] * 8);
	view->roll += (int)(sh.roll * 32768.0f / 360.0f);
}

/* --- drawing over the 3D view ------------------------------------------------------------- */

/* a quad in screen pixels, in front of everything */
static void quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3)
{
	float xs[4] = {x0, x1, x2, x3}, ys[4] = {y0, y1, y2, y3};
	glBegin(GL_QUADS);
	for (int i = 0; i < 4; i++)
		glVertex3v16((v16)((xs[i] / 128.0f - 1.0f) * 4096), (v16)((1.0f - ys[i] / 96.0f) * 4096), -4000);
	glEnd();
}

static void rect(float x, float y, float w, float h) { quad(x, y, x + w, y, x + w, y + h, x, y + h); }

static int alpha5(float a)
{
	int v = (int)(a * 31 + 0.5f);
	return v < 1 ? 0 : v > 31 ? 31 : v;
}

/* the pain sprite (sprites/640_pain.spr) as a trapezoid: wide at the screen's edge side */
static void arrow(int dir, float a)
{
	const float cx = 128, cy = 96;
	int al = alpha5(a);
	float l = PCY(128), t = PCY(48), in = l * 0.2f;
	if (!al)
		return;
	glPolyFmt(POLY_ALPHA(al) | POLY_CULL_NONE | POLY_ID(1 + dir));
	glColor(RGB15(31, 12, 6)); /* the sprite's colours are Valve's data: an orange-red (hypothesis) */
	switch (dir) {
	case 0: { /* ahead: center + [-64,-102], 128x48 */
		float y = cy + PCY(-102);
		quad(cx - l / 2, y, cx + l / 2, y, cx + l / 2 - in, y + t, cx - l / 2 + in, y + t);
		break;
	}
	case 1: { /* behind: center + [-64,70] */
		float y = cy + PCY(70);
		quad(cx - l / 2 + in, y, cx + l / 2 - in, y, cx + l / 2, y + t, cx - l / 2, y + t);
		break;
	}
	case 2: { /* right: center + [70,-64], 48x128 */
		float x = cx + PCY(70);
		quad(x, cy - l / 2 + in, x + t, cy - l / 2, x + t, cy + l / 2, x, cy + l / 2 - in);
		break;
	}
	default: { /* left: center + [-102,-64] */
		float x = cx + PCY(-102);
		quad(x, cy - l / 2, x + t, cy - l / 2 + in, x + t, cy + l / 2 - in, x, cy + l / 2);
		break;
	}
	}
}

void Events_Draw3D(const ndscam_t *view)
{
	/* down or dead: HUD_Draw skips Jtf_DrawFeedback (coop/src/hud/jtf.qc:536-540), so the
	 * arrows and tint neither show nor fade until the player is up again */
	bool feedback = dm.alpha > 0 && !Hud_Down(), any = fd.active || feedback || flash > 0;
	if (!any)
		return;
	glBindTexture(GL_TEXTURE_2D, 0);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
	/* the fade, over the 3D and under the HUD, as View.qc draws it before the HUD.
	 * drawfill blends the colour over the scene; the modulate style (fade_modulate:
	 * blendFunc GL_DST_COLOR GL_ONE_MINUS_SRC_ALPHA) makes each channel dst * (col + 1 - a):
	 * a channel at 0 dims by a, one at 1 brightens (to the clamp). The DS blends toward one
	 * colour at one alpha: toward (col ? 31 : 0) at a, so the dimmed channels dim as on the
	 * PC and the others keep their light (the brightening can't be done; a white modulate
	 * shows nothing) */
	if (flash > 0) {
		if (alpha5(flash)) {
			glPolyFmt(POLY_ALPHA(alpha5(flash)) | POLY_CULL_NONE | POLY_ID(60));
			glColor(RGB15(215 >> 3, 186 >> 3, 69 >> 3));
			rect(0, 0, 256, 192);
		}
		flash -= 100 / 255.0f * ftime;
	}
	if (fd.active) {
		float a = fd.alpha * fd.maxa;
		uint16_t rgb = RGB15((int)(fd.col[0] * 31), (int)(fd.col[1] * 31), (int)(fd.col[2] * 31));
		if (fd.style & EVF_MODULATE && fd.col[0] > 0 && fd.col[1] > 0 && fd.col[2] > 0)
			a = 0;
		if (alpha5(a)) {
			glPolyFmt(POLY_ALPHA(alpha5(a)) | POLY_CULL_NONE | POLY_ID(62));
			glColor(rgb);
			rect(0, 0, 256, 192);
		}
	}
	/* Jtf_DrawFeedback: where the hit came from, against the camera's forward and right
	 * on the level of the damage, each side as strong as it lines up; then the tint,
	 * a modulate by (1, 1 - a * fill, 1 - a * fill): here a blend toward a dark red */
	if (feedback) {
		float rel[3], len, fw, rt;
		float yaw = view->yaw * (2 * (float)M_PI / 32768.0f), pitch = -view->pitch * (2 * (float)M_PI / 32768.0f);
		float sy = sinf(yaw), cy = cosf(yaw), sp = sinf(pitch), cp = cosf(pitch);
		rel[0] = dm.pos[0] - view->org[0] / 8.0f, rel[1] = dm.pos[1] - view->org[1] / 8.0f, rel[2] = 0;
		len = sqrtf(rel[0] * rel[0] + rel[1] * rel[1]);
		if (len > 0) {
			rel[0] /= len, rel[1] /= len;
			/* anglesToForward / anglesToRight of (pitch down positive, yaw) */
			fw = rel[0] * cp * cy + rel[1] * cp * sy;
			rt = rel[0] * sy - rel[1] * cy;
			(void)sp;
			if (fw > 0.25f)
				arrow(0, fabsf(fw) * dm.alpha);
			else if (fw < -0.25f)
				arrow(1, fabsf(fw) * dm.alpha);
			if (rt > 0.25f)
				arrow(2, fabsf(rt) * dm.alpha);
			else if (rt < -0.25f)
				arrow(3, fabsf(rt) * dm.alpha);
		}
		if (alpha5(dm.alpha * CG_DAMAGEFILL)) {
			glPolyFmt(POLY_ALPHA(alpha5(dm.alpha * CG_DAMAGEFILL)) | POLY_CULL_NONE | POLY_ID(61));
			glColor(RGB15(16, 0, 0));
			rect(0, 0, 256, 192);
		}
		/* Jtf_DrawFeedback takes the frame's time off g_damageAlpha (coop/src/hud/jtf.qc:507),
		 * then HLHUD_ClampTimers again (jtf.qc:516, valve/src/hud/hud.qc:213): twice a frame,
		 * so the feedback is gone in 0.5 s */
		dm.alpha -= 2 * ftime;
		if (dm.alpha < 0)
			dm.alpha = 0;
	}
	glPopMatrix(1);
	glMatrixMode(GL_PROJECTION);
	glPopMatrix(1);
	glMatrixMode(GL_MODELVIEW);
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);
	glColor(RGB15(31, 31, 31));
}

/* --- text on its layer (BG2, in front of the HUD) ------------------------------------------ */

/* The band is drawn in main RAM, in the layer's own tile format (a cell's row of 8
 * pixels is a word), and only its rows of cells that changed are copied to VRAM:
 * the PC's text changes a few times a second (the co-op hero line, the XPM line),
 * and drawing every line again into VRAM pixel by pixel took more than a frame */
#define LINE_H (Hud_FontHeight() + 2)
#define MAX_LINES 12

/* two layers: 0 BG2 (prints, chat, centerprint, feed, pickups), 1 BG3 (game_text) */
static uint32_t bands[2][BAND_ROWS * 32 * 8];
static uint8_t bandbanks[2][BAND_ROWS][32];
static uint32_t (*band) = bands[0];
static uint8_t (*bandbank)[32] = bandbanks[0];
static uint32_t cliprows; /* the rows of cells being drawn now */
static uint32_t grouprows; /* the rows a group's lines take (measuring) */
static bool measuring;

/* each bit of a byte as a nibble of 0xF */
static uint32_t nib[256];

/* the engine's text colours (src/render/r_draw.c qcolors: ^0-^9), at palette entries 6-15
 * of each group's bank */
#define PAL_Q0 6
static const uint8_t qcol[10][3] = {{0, 0, 0},       {255, 51, 51},   {51, 255, 51}, {255, 255, 51},
                                    {77, 77, 255},   {51, 255, 255},  {255, 51, 255}, {255, 255, 255},
                                    {128, 128, 128}, {191, 191, 191}};

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* r_draw.c markup: a ^ sequence at s, its length (0: not one) and *pal what it does to the
 * colour: a palette entry, -1 back to the text's own (^d), 0 nothing (styles, a ^[ link's
 * text, skipped). ^xRGB takes the nearest of the ten (the bank has no room for more) */
static int markup(const char *s, int *pal)
{
	*pal = 0;
	if (s[0] != '^' || !s[1] || s[1] == '^')
		return 0;
	if (s[1] >= '0' && s[1] <= '9') {
		*pal = PAL_Q0 + s[1] - '0';
		return 2;
	}
	if (s[1] == 'x' && hexv(s[2]) >= 0 && hexv(s[3]) >= 0 && hexv(s[4]) >= 0) {
		int best = 7, bd = 1 << 30;
		for (int i = 0; i < 10; i++) {
			int d = 0;
			for (int k = 0; k < 3; k++) {
				int v = hexv(s[2 + k]) * 17 - qcol[i][k];
				d += v * v;
			}
			if (d < bd)
				bd = d, best = i;
		}
		*pal = PAL_Q0 + best;
		return 5;
	}
	if (s[1] == '[') {
		const char *end = strstr(s + 2, "^]");
		return end ? (int)(end + 2 - s) : (int)strlen(s);
	}
	if (s[1] == 'd') {
		*pal = -1;
		return 2;
	}
	if ((s[1] >= 'a' && s[1] <= 'z') || (s[1] >= 'A' && s[1] <= 'Z'))
		return 2;
	return 0;
}

/* a line's width in pixels, its markup taking none ("^^" is one '^') */
static int ev_width(const char *s)
{
	int n = 0, w, adv, pal, ml;
	while (*s) {
		if (*s == '^' && (ml = markup(s, &pal))) {
			s += ml;
			continue;
		}
		Hud_Glyph((uint8_t)*s, &w, &adv);
		n += adv;
		s += s[0] == '^' && s[1] == '^' ? 2 : 1;
	}
	return n;
}

/* hud_text's look at scale 1: the glyph in col, with a 1-pixel black (index 1) outline.
 * A glyph row and its outline are masks, shifted to the cells they fall in and merged
 * a cell's row (a word) at a time */
ITCM_CODE __attribute__((target("arm"))) static int band_text(int x, int y, int col, int bank, const char *s)
{
	int h = Hud_FontHeight(), x0 = x;
	const uint32_t edgew = 0x11111111u;
	if (!nib[1])
		for (int b = 0; b < 256; b++)
			for (int i = 0; i < 8; i++)
				if (b >> i & 1)
					nib[b] |= 0xFu << (i * 4);
	const int col0 = col;
	uint32_t colw = (uint32_t)col * 0x11111111u;
	for (; *s; s++) {
		int w, adv, pal, ml;
		const uint8_t *g;
		if (*s == '^' && (ml = markup(s, &pal))) { /* a colour from here on (or the text's own again) */
			if (pal)
				col = pal > 0 ? pal : col0, colw = (uint32_t)col * 0x11111111u;
			s += ml - 1;
			continue;
		}
		if (s[0] == '^' && s[1] == '^') /* "^^": one '^' */
			s++;
		g = Hud_Glyph((uint8_t)*s, &w, &adv);
		int px0 = x - 1, sh = px0 & 7, cell0 = px0 >> 3;
		if (g && px0 >= 0)
			for (int j = -1; j <= h; j++) {
				int r = y + j - BAND_Y0;
				unsigned up, me, dn, tm, em;
				if ((unsigned)r >= BAND_ROWS * 8 || !(cliprows & 1u << (r >> 3)))
					continue;
				up = j > 0 ? g[j - 1] : 0, me = j >= 0 && j < h ? g[j] : 0, dn = j + 1 < h ? g[j + 1] : 0;
				tm = me << 1;
				em = (((up | me | dn) << 1) | ((up | me | dn) << 2) | (up | me | dn)) & ~tm;
				tm <<= sh, em <<= sh;
				for (int k = 0; k < 3 && cell0 + k < 32; k++) {
					uint32_t t = nib[(tm >> (8 * k)) & 0xFF], e = nib[(em >> (8 * k)) & 0xFF], *wp;
					if (!(t | e))
						continue;
					wp = &band[((r >> 3) * 32 + cell0 + k) * 8 + (r & 7)];
					*wp = (*wp & ~(t | e)) | (t & colw) | (e & edgew);
				}
			}
		x += adv;
	}
	/* the cells it covers take the group's palette bank */
	for (int cy = BW_NDS_MAX(y - 1 - BAND_Y0, 0) >> 3; cy <= (y + h - BAND_Y0) >> 3 && cy < BAND_ROWS; cy++)
		if (cliprows & 1u << cy)
			for (int cx = BW_NDS_MAX(x0 - 1, 0) >> 3; cx <= BW_NDS_MIN(x, 255) >> 3; cx++)
				bandbank[cy][cx] = (uint8_t)bank;
	return x - x0;
}

/* a line of a group: measured (its rows of cells), or drawn */
static void emit(int x, int y, int col, int bank, const char *s)
{
	if (!s[0] || y < BAND_Y0 || y + LINE_H > BAND_Y1)
		return;
	{
		uint32_t m = 0; /* the rows of cells the line touches */
		for (int r = (y - 1 - BAND_Y0) >> 3; r <= (y + Hud_FontHeight() - BAND_Y0) >> 3; r++)
			if (r >= 0 && r < BAND_ROWS)
				m |= 1u << r;
		if (measuring) {
			grouprows |= m;
			return;
		}
		if (!(m & cliprows)) /* none being drawn now */
			return;
	}
	band_text(x, y, col, bank, s);
}

/* text split at new lines, and wrapped at maxw pixels (at a space when there is one). A
 * wrapped line goes on in the colour its text had (one line on the PC, which needs no
 * wrapping); after a new line, only when carry is set (a text field, chat) */
static int split_lines(const char *s, int maxw, char out[MAX_LINES][TEXT_LEN], bool carry)
{
	int n = 0;
	char cur[6] = ""; /* the colour code in force */
	while (*s && n < MAX_LINES) {
		int len = 0, olen, lastsp = -1, olastsp = 0, wpx = 0, ml, pal;
		char *o = out[n], linecur[6], atsp[6] = "";
		strcpy(linecur, cur);
		olen = (int)strlen(cur);
		memcpy(o, cur, (size_t)olen);
		while (s[len] && s[len] != '\n' && olen < TEXT_LEN - 6) {
			int w, adv, step = 1;
			if (s[len] == '^' && (ml = markup(s + len, &pal))) {
				if (pal) /* the code itself, as the colour in force (^d: none) */
					snprintf(linecur, sizeof(linecur), "%.*s", pal > 0 ? BW_NDS_MIN(ml, 5) : 0, s + len);
				memcpy(o + olen, s + len, (size_t)ml);
				olen += ml, len += ml;
				continue;
			}
			if (s[len] == '^' && s[len + 1] == '^')
				step = 2;
			Hud_Glyph((uint8_t)s[len], &w, &adv);
			if (wpx + adv > maxw && len > 0) {
				if (lastsp > 0)
					len = lastsp, olen = olastsp, strcpy(linecur, atsp);
				break;
			}
			if (s[len] == ' ')
				lastsp = len, olastsp = olen, strcpy(atsp, linecur);
			wpx += adv;
			for (int k = 0; k < step; k++)
				o[olen++] = s[len++];
		}
		o[olen] = 0;
		n++;
		s += len;
		if (*s == '\n') { /* the line's end: what follows is the next line, empty or not */
			s++;
			strcpy(cur, carry ? linecur : "");
			if (!*s && n < MAX_LINES) /* text that ends in a new line has an empty last line */
				out[n++][0] = 0;
		} else { /* a wrapped line: the spaces it broke at go */
			strcpy(cur, linecur);
			while (*s == ' ')
				s++;
		}
	}
	return n;
}

static int clampy(int y, int lines) { return BW_NDS_CLAMP(y, BAND_Y0 + 1, BAND_Y1 - lines * LINE_H); }

/* GameText_CharCount: the typing effect's letters by now */
static int char_count(float fadein, float timer, const char *msg)
{
	float n = fadein ? timer / fadein : 0;
	int len = (int)strlen(msg);
	return !fadein || n > len ? len : (int)n;
}

/* GameText_DrawMessage (src/client/text.qc): how strong a pass is now (0: not shown), and
 * its letters. The first pass runs fx behind the channel's clock in colour 1; the
 * second (any effect but 0) on the clock itself in colour 2, drawn over the first */
static float gtext_alpha(const gtext_t *t, int highlight, int *chars)
{
	float timer = highlight ? t->time : t->time - t->fx, a = 0, mtime, btime;
	int len = (int)strlen(t->msg);
	*chars = t->effect == 2 ? char_count(t->fadein, timer, t->msg) : len;
	if (timer < 0)
		timer = 0;
	if (highlight) {
		btime = t->fadein * len;
		mtime = btime + t->fadeout;
	} else {
		mtime = t->fadein + t->hold + t->fadeout;
		btime = t->fadein + t->hold;
	}
	if (t->effect == 2) {
		mtime = len * t->fadein + t->hold;
		btime = mtime + t->fadeout;
	}
	if (timer > mtime)
		return 0;
	if (timer < t->fadein)
		a = timer / t->fadein;
	else if (timer < btime)
		a = 1;
	else if (timer < mtime && t->fadeout)
		a = 1 - (timer - btime) / t->fadeout;
	return a;
}

/* GameText_DrawString: x -1 centres, 0.5 and over right-aligns; y -1 is the middle */
static char lines_a[MAX_LINES][TEXT_LEN], lines_b[MAX_LINES][TEXT_LEN]; /* off the DTCM stack */

static void draw_gtext(const gtext_t *t, int chars, int col, int bank)
{
	char s[TEXT_LEN], (*lines)[TEXT_LEN] = lines_b;
	int n, y0;
	memcpy(s, t->msg, (size_t)chars);
	s[chars] = 0;
	n = split_lines(s, 248, lines, false);
	if (t->y == -1)
		y0 = 96 - PCY(6);
	else
		y0 = (int)((192 - PCY(12)) * t->y);
	y0 -= LINE_H * n / 2;
	y0 = clampy(y0, n);
	for (int i = 0; i < n; i++) {
		int w = ev_width(lines[i]), x;
		if (t->x == -1)
			x = 128 - w / 2;
		else {
			x = (int)(256 * t->x);
			if (t->x >= 0.5f)
				x -= w;
		}
		emit(BW_NDS_CLAMP(x, 1, 255 - w), y0 + LINE_H * i, col, bank, lines[i]);
	}
}

static uint32_t hash(uint32_t h, const void *p, int n)
{
	const uint8_t *b = p;
	for (int i = 0; i < n; i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}

static uint16_t stagepal[G_COUNT][15]; /* the groups' palette banks (entries 1-15), to write */
static uint16_t stagebld;              /* REG_BLDALPHA, 0: no blending */

static uint16_t rgb_of(float r, float g, float b)
{
	int c[3] = {(int)r >> 3, (int)g >> 3, (int)b >> 3};
	for (int k = 0; k < 3; k++)
		c[k] = c[k] < 0 ? 0 : c[k] > 31 ? 31 : c[k];
	return RGB15(c[0], c[1], c[2]);
}

/* a group's colours at alpha a, over a layer blended at alpha e (1: not blended). Entry 5
 * is the second pass of game_text (colour 2 at a2) over the first (colour 1 at a) */
static void set_bank(int g, const uint8_t *rgb8, float a, const uint8_t *rgb2, float a2, float e)
{
	uint16_t *pal = stagepal[g]; /* the bank's entries 1-5 */
	a = a < 0 ? 0 : a > 1 ? 1 : a;
	a2 = a2 < 0 ? 0 : a2 > 1 ? 1 : a2;
	a /= e, a2 /= e; /* blended, the layer's alpha does the rest */
	pal[0] = RGB15(0, 0, 0);
	pal[1] = rgb_of(rgb8[0] * a, rgb8[1] * a, rgb8[2] * a);
	pal[2] = rgb_of(0x33 * a, 0x96 * a, 0xd4 * a); /* the feed's attacker, #3396d4 */
	pal[3] = rgb_of(0x9a * a, 0x9a * a, 0x9a * a); /* the feed's weapon (disabled text) */
	pal[4] = rgb2 ? rgb_of(rgb2[0] * a2 + rgb8[0] * a * (1 - a2 * e), rgb2[1] * a2 + rgb8[1] * a * (1 - a2 * e),
	                       rgb2[2] * a2 + rgb8[2] * a * (1 - a2 * e))
	              : 0;
	for (int i = 0; i < 10; i++) /* ^0-^9 */
		pal[PAL_Q0 - 1 + i] = rgb_of(qcol[i][0] * a, qcol[i][1] * a, qcol[i][2] * a);
}

static int glayer(int g) { return g >= G_TEXT0 && g <= G_QUEUE; }

/* one group's lines, measured or drawn; chars[c] and chars2[c] are game_text channel c's
 * letters in its two passes, -1 for a pass not shown */
static void layout(int g, const int *chars, const int *chars2)
{
	char (*lines)[TEXT_LEN] = lines_a;
	int n, y;
	if (g == G_PRINT) { /* prints: hudmins + [16,16], 14 apart */
		y = BAND_Y0 + 1;
		for (int i = 0; i < 5; i++) {
			n = split_lines(tx.print[i], 250, lines, false);
			for (int j = 0; j < n; j++, y += LINE_H)
				emit(PCY(16), y, 2, G_PRINT + 1, lines[j]);
		}
	} else if (g >= G_TEXT0 && g <= G_QUEUE) { /* game_text: the first pass, the second over it */
		int c = g - G_TEXT0;
		if (chars[c] >= 0)
			draw_gtext(c < 5 ? &tx.text[c] : &tx.queue[0], chars[c], 2, g + 1);
		if (chars2[c] >= 0)
			draw_gtext(c < 5 ? &tx.text[c] : &tx.queue[0], chars2[c], 5, g + 1);
	} else if (g == G_CHAT) { /* a field from [16, h/2 - 80], half the HUD wide and tall, its lines at the bottom */
		n = split_lines(tx.chatfinal, 128, lines, true);
		y = clampy(BW_NDS_MIN(96 - PCY(80) + 96, BAND_Y1) - n * LINE_H, n);
		for (int j = 0; j < n; j++)
			emit(PCY(16), y + j * LINE_H, 2, G_CHAT + 1, lines[j]);
	} else if (g == G_CENTER) { /* centred, from the middle - 69 */
		n = split_lines(tx.center, 250, lines, false);
		y = clampy(96 - (n - 4) - PCY(69), n);
		for (int j = 0; j < n; j++)
			emit(128 - ev_width(lines[j]) / 2, y + j * LINE_H, 2, G_CENTER + 1, lines[j]);
	} else if (g == G_FEED) { /* top right, newest on top: attacker (blue), weapon (grey), victim */
		y = BAND_Y0 + 1;
		for (int i = tx.feeds - 1; i >= 0; i--, y += LINE_H + 1) {
			char a[44], w[44], v[44];
			int wa, ww, wv, x;
			snprintf(a, sizeof(a), "%s ", tx.feed[i].a);
			snprintf(w, sizeof(w), "%s%s", tx.feed[i].w[0] ? tx.feed[i].w : "killed", tx.feed[i].v[0] ? " " : "");
			snprintf(v, sizeof(v), "%s", tx.feed[i].v);
			wa = Hud_TextWidth(a, 1), ww = Hud_TextWidth(w, 1), wv = Hud_TextWidth(v, 1);
			x = BW_NDS_MAX(255 - PCY(32) - wa - ww - wv, 1);
			emit(x, y, 3, G_FEED + 1, a);
			emit(x + wa, y, 4, G_FEED + 1, w);
			emit(x + wa + ww, y, tx.feed[i].you ? 3 : 2, G_FEED + 1, v);
		}
	} else if (g >= G_PICK0) { /* pickups: up from 70% of the screen, right side, in the order of the three */
		static const char *names[3] = {"BATTERY", "HEALTH KIT", "LONG JUMP"};
		char s[32];
		int i = g - G_PICK0;
		y = PCY(336);
		for (int k = 0; k <= i; k++)
			if (tx.pickalpha[k] > 0)
				y -= LINE_H + 1;
		if (tx.pickcount[i] > 1)
			snprintf(s, sizeof(s), "%d  %s", tx.pickcount[i], names[i]);
		else
			snprintf(s, sizeof(s), "%s", names[i]);
		emit(255 - PCY(20) - Hud_TextWidth(s, 1), y, 2, g + 1, s);
	}
}

/* the rows of cells in flushrows, to the layers */
static uint32_t flushrowsl[2];
/* a redraw's time in a frame (before glFlush; Hud_Frame has its own after the blank),
 * checked between batches of rows of cells */
#define TEXT_FRAME_US 2000
#define TEXT_BATCH 2
static uint32_t pend[2]; /* rows of cells still to draw again, each layer */

static void band_flush(void)
{
	for (int l = 0; l < 2; l++) {
		uint32_t *tiles = (uint32_t *)Hud_TextTiles(l);
		uint16_t *map = Hud_TextMap(l);
		if (!tiles || !map)
			continue;
		for (int r = 0; r < BAND_ROWS; r++) {
			if (!(flushrowsl[l] & 1u << r))
				continue;
			for (int k = 0; k < 32 * 8; k++)
				tiles[(BAND_ROW0 + r) * 32 * 8 + k] = bands[l][r * 32 * 8 + k];
			for (int c = 0; c < 32; c++)
				map[(BAND_ROW0 + r) * 32 + c] = (uint16_t)(((BAND_ROW0 + r) * 32 + c) | bandbanks[l][r][c] << 12);
		}
		flushrowsl[l] = 0;
	}
}

/* what Events_Draw2D made, to the layer and its palette: called right after the wait for
 * the vertical blank, so no row of text or colour changes while the screen is drawn */
void Events_Commit2D(void)
{
	band_flush();
	/* by halfwords: palette RAM takes no byte writes, which memcpy may make */
	for (int g = 0; g < G_COUNT; g++)
		for (int k = 0; k < 15; k++)
			BG_PALETTE[(g + 1) * 16 + 1 + k] = stagepal[g][k];
	/* the text layers over the 3D view, the HUD and the backdrop */
	REG_BLDCNT = stagebld ? BLEND_ALPHA | BLEND_SRC_BG2 | BLEND_SRC_BG3 | BLEND_DST_BG0 | BLEND_DST_BG1 | BLEND_DST_BACKDROP
	                      : BLEND_NONE;
	REG_BLDALPHA = stagebld;
}

void Events_Draw2D(void)
{
	uint32_t sig[G_COUNT];
	float alpha[G_COUNT], alpha2[6] = {0}, lo = 2, hi = 0, e = 1;
	int chars[6], chars2[6];
	bool redraw = false;
	static const uint8_t white[3] = {255, 255, 255}, orange[3] = {255, 136, 0}; /* ^xF80 */

	/* the timers, as the draw functions step them */
	if (tx.printlines >= 0 && tx.printtime < now) { /* Print_Draw drops the newest line */
		tx.print[tx.printlines][0] = 0;
		tx.printlines--;
		tx.printtime = now + 5;
	}
	if (tx.chatlines >= 0 && tx.chattime < now) { /* Chat_Draw: the oldest; the block stays as it was made */
		for (int i = 0; i < tx.chatlines; i++)
			strcpy(tx.chat[i], tx.chat[i + 1]);
		tx.chat[tx.chatlines][0] = 0;
		tx.chatlines--;
		tx.chattime = now + 5;
	}
	if (tx.centeralpha > 0) { /* Print_DrawCenterprint */
		if (tx.centertime > now)
			tx.centeralpha = 1;
		else if ((tx.centeralpha -= ftime) < 0)
			tx.centeralpha = 0;
	}
	for (int i = 0; i < tx.feeds; i++)
		if (tx.feed[i].until < now) {
			memmove(&tx.feed[i], &tx.feed[i + 1], sizeof(tx.feed[0]) * (size_t)(tx.feeds - i - 1));
			tx.feeds--, i--;
		}
	if (tx.queued > 0 && !tx.queue[0].msg[0]) { /* GameText_QueueNext */
		memmove(&tx.queue[0], &tx.queue[1], sizeof(gtext_t) * (TEXTQUEUE_MAX - 2));
		tx.queued--;
	}

	/* what each group shows, and how strongly */
	memset(sig, 0, sizeof(sig));
	alpha[G_CENTER] = tx.centeralpha;
	if (tx.centeralpha > 0)
		sig[G_CENTER] = hash(2166136261u, tx.center, (int)strlen(tx.center)) | 1;
	alpha[G_PRINT] = 1;
	if (tx.printlines >= 0)
		for (int i = 0; i < 5; i++)
			sig[G_PRINT] = hash(sig[G_PRINT] ? sig[G_PRINT] : 2166136261u, tx.print[i], (int)strlen(tx.print[i]) + 1) | 1;
	alpha[G_CHAT] = 1;
	if (tx.chatlines >= 0)
		sig[G_CHAT] = hash(2166136261u, tx.chatfinal, (int)strlen(tx.chatfinal)) | 1;
	for (int c = 0; c < 6; c++) {
		gtext_t *t = c < 5 ? &tx.text[c] : &tx.queue[0];
		float a = 0, a2 = 0;
		chars[c] = chars2[c] = -1;
		if (t->msg[0] && (c < 5 || tx.queued > 0)) {
			a = gtext_alpha(t, 0, &chars[c]);
			if (t->effect != 0) /* effect 0 has no highlight */
				a2 = gtext_alpha(t, 1, &chars2[c]);
		}
		if (a <= 0)
			chars[c] = -1;
		if (a2 <= 0)
			chars2[c] = -1;
		alpha[G_TEXT0 + c] = a, alpha2[c] = a2;
		if (a > 0 || a2 > 0) {
			sig[G_TEXT0 + c] = hash(2166136261u, t, (int)offsetof(gtext_t, time));
			sig[G_TEXT0 + c] = hash(sig[G_TEXT0 + c], &chars[c], 4);
			sig[G_TEXT0 + c] = hash(sig[G_TEXT0 + c], &chars2[c], 4) | 1;
		}
	}
	alpha[G_FEED] = 1;
	if (tx.feeds)
		sig[G_FEED] = hash(2166136261u, tx.feed, (int)sizeof(tx.feed[0]) * tx.feeds) | 1;
	for (int i = 0, above = 0; i < 3; i++) { /* a notice's place depends on the ones before it */
		alpha[G_PICK0 + i] = tx.pickalpha[i] > 1 ? 1 : tx.pickalpha[i];
		if (tx.pickalpha[i] > 0)
			sig[G_PICK0 + i] = ((uint32_t)tx.pickcount[i] << 4 | (uint32_t)above << 1) | 1, above++;
	}
	for (int g = 0; g < G_COUNT; g++)
		if (sig[g] != tx.sig[g])
			redraw = true;
	if (pend[0] | pend[1]) /* rows a redraw left for this frame */
		redraw = true;

	if (redraw) {
		static uint32_t rows[G_COUNT]; /* the rows each group's lines took when last drawn */
		uint32_t now_rows[G_COUNT], dirty[2] = {pend[0], pend[1]};
		uint64_t t0 = tickGetCount(), deadline = t0 + (uint64_t)TEXT_FRAME_US * TICK_FREQ / 1000000;
		bool drew = false;
		Events_Stats.redraws++;
		measuring = true;
		for (int g = 0; g < G_COUNT; g++) {
			/* a group whose signature is the same takes the rows it took: only the
			 * ones that changed are laid out to measure them */
			if (sig[g] == tx.sig[g]) {
				now_rows[g] = rows[g];
				continue;
			}
			grouprows = 0;
			if (sig[g])
				layout(g, chars, chars2);
			now_rows[g] = grouprows;
			dirty[glayer(g)] |= rows[g] | now_rows[g];
		}
		measuring = false;
		/* the rows to draw, TEXT_BATCH at a time: every group over them, cleared and drawn
		 * again; past TEXT_FRAME_US the rest wait for the next frame (at least one batch a
		 * frame), showing what they showed until then */
		pend[0] = pend[1] = 0;
		for (int l = 0; l < 2; l++)
			while (dirty[l]) {
				uint32_t m = 0;
				if (drew && tickGetCount() >= deadline) {
					pend[0] |= dirty[0], pend[1] |= dirty[1];
					break;
				}
				drew = true;
				for (int r = __builtin_ctz(dirty[l]), k = 0; r < BAND_ROWS && k < TEXT_BATCH; r++)
					if (dirty[l] & 1u << r)
						m |= 1u << r, k++;
				dirty[l] &= ~m;
				for (int r = 0; r < BAND_ROWS; r++)
					if (m & 1u << r) {
						memset(&bands[l][r * 32 * 8], 0, 32 * 8 * 4);
						memset(bandbanks[l][r], 0, 32);
					}
				band = bands[l], bandbank = bandbanks[l], cliprows = m;
				for (int g = 0; g < G_COUNT; g++)
					if (glayer(g) == l && sig[g] && now_rows[g] & m)
						layout(g, chars, chars2);
				flushrowsl[l] |= m;
			}
		band = bands[0], bandbank = bandbanks[0];
		memcpy(rows, now_rows, sizeof(rows));
		memcpy(tx.sig, sig, sizeof(sig));
		Events_Stats.lastdraw_ticks = (uint32_t)(tickGetCount() - t0);
	}
	/* every group up as strong as the others: the layers blend at that alpha (to 1/16),
	 * and the colours stay whole; otherwise each group's colours darken by its own */
	for (int g = 0; g < G_COUNT; g++) {
		float a[2] = {alpha[g], glayer(g) ? alpha2[g - G_TEXT0] : 0};
		if (!sig[g])
			continue;
		for (int k = 0; k < 2; k++)
			if (a[k] > 0)
				lo = a[k] < lo ? a[k] : lo, hi = a[k] > hi ? a[k] : hi;
	}
	stagebld = 0;
	if (hi > 0 && hi < 1 && hi - lo < 1.0f / 16) {
		int eva = (int)(hi * 16 + 0.5f);
		eva = eva < 1 ? 1 : eva > 16 ? 16 : eva;
		e = eva / 16.0f;
		stagebld = (uint16_t)(eva | (16 - eva) << 8);
	}
	for (int g = 0; g < G_COUNT; g++) {
		const uint8_t *rgb = white, *rgb2 = NULL;
		float a2 = 0;
		if (g == G_CENTER)
			rgb = orange;
		else if (g >= G_TEXT0 && g <= G_QUEUE) {
			const gtext_t *t = g < G_QUEUE ? &tx.text[g - G_TEXT0] : &tx.queue[0];
			rgb = t->c1, rgb2 = t->c2, a2 = alpha2[g - G_TEXT0];
		}
		set_bank(g, rgb, alpha[g], rgb2, a2, e);
	}
	/* HLHUD_ItemNotify_Draw: 0.5 a second off the notice's 2.5 */
	for (int i = 0; i < 3; i++)
		if (tx.pickalpha[i] > 0 && (tx.pickalpha[i] -= ftime * 0.5f) <= 0)
			tx.pickcount[i] = 0;
	/* GameText_Draw steps the channels' clocks */
	for (int c = 0; c < 5; c++)
		tx.text[c].time += ftime;
	if (tx.queued > 0) {
		gtext_t *q = &tx.queue[0];
		float maxtime = q->effect == 2 ? strlen(q->msg) * q->fadein + q->fadeout + q->hold + q->fx
		                               : q->fadein + q->fadeout + q->hold + q->fx;
		q->time += ftime;
		if (q->time >= maxtime)
			q->msg[0] = 0;
	}
	Events_Stats.texts = 0;
	for (int g = 0; g < G_COUNT; g++)
		Events_Stats.texts += sig[g] != 0;
}
