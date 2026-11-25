/* Bulwark engine - native UI: the pieces every screen shares, the
 * button-hint footer, the error dialog and the loading screen
 * (design 6.0 footer, 6.5 error dialog and loading screen).
 * ISC licence, see LICENSE. */
#include "uin_local.h"

#include <ctype.h>
#include <stdio.h>

/* --- footer ------------------------------------------------------------------------------- */

static uint32_t pad_colour(char button)
{
	switch (button) {
	case 'A': return UIN_COL_PAD_A;
	case 'B': return UIN_COL_PAD_B;
	case 'X': return UIN_COL_PAD_X;
	case 'Y': return UIN_COL_PAD_Y;
	default: return 0x808080ffu;
	}
}

/* a 12 px button disc with its letter, its bottom on the baseline */
static void draw_disc(float x, float base, const char *button, float alpha)
{
	d_img(UIN_R_DISC, x, base - 10, 12, 12, uin_col_alpha(pad_colour(button[0]), alpha));
	d_text(UIN_F_STD8, x + 6, base - 4, button, UIN_CENTER | UIN_MIDDLE, uin_col_alpha(0x101010ffu, alpha));
}

float uin_prompt(float right, float base, const char *button, const char *label, float alpha)
{
	float x = right - d_text_width(UIN_F_STD8, label, 0);
	d_text(UIN_F_STD8, x, base, label, UIN_LEFT | UIN_BASELINE, uin_col_alpha(UIN_COL_FOOTER_TEXT, alpha));
	x -= 4 + 12;
	draw_disc(x, base, button, alpha);
	return x;
}

void uin_footer(float w, float h, const char *spec)
{
	/* laid out right to left from the action-safe corner (design 6.0) */
	float x = UIN_FOOTER_RIGHT * w / 640, base = UIN_FOOTER_BASELINE * h / 480;
	const char *end = spec + strlen(spec);
	while (end > spec) {
		const char *start = end, *eq;
		char label[32], button[8];
		size_t n;
		while (start > spec && start[-1] != ' ')
			start--;
		eq = memchr(start, '=', (size_t)(end - start));
		if (eq) {
			n = BW_MIN((size_t)(eq - start), sizeof(button) - 1);
			memcpy(button, start, n);
			button[n] = 0;
			n = BW_MIN((size_t)(end - eq - 1), sizeof(label) - 1);
			memcpy(label, eq + 1, n);
			label[n] = 0;
			for (char *p = label; *p; p++)
				if (*p == '_')
					*p = ' ';
			x = uin_prompt(x, base, button, label, 1) - 12;
		}
		end = start;
		while (end > spec && end[-1] == ' ')
			end--;
	}
}

/* --- error dialog ----------------------------------------------------------------------- */

/* the reason wraps to this width, in at most this many lines (design 6.5) */
#define ERR_WRAP_W 296
#define ERR_LINES 4
#define ERR_LINE_BYTES 96

static struct {
	bool open;
	double opened;
	char title[32];
	char text[256];
	char lines[ERR_LINES][ERR_LINE_BYTES];
	int numlines; /* -1 until wrapped, which needs the atlas */
} err;

/* breaks err.text into lines of at most ERR_WRAP_W px at spaces (a word
 * longer than a line is cut); text past ERR_LINES ends the last in "..." */
static void err_wrap(void)
{
	const char *p = err.text;
	err.numlines = 0;
	while (*p == ' ')
		p++;
	while (*p && err.numlines < ERR_LINES) {
		char *line = err.lines[err.numlines];
		size_t n = 0, cut = 0;
		/* the longest run that fits, remembering the last space in it */
		while (p[n] && n < ERR_LINE_BYTES - 1) {
			memcpy(line, p, n + 1);
			line[n + 1] = 0;
			if (n > 0 && d_text_width(UIN_F_INTER10, line, 0) > ERR_WRAP_W)
				break;
			n++;
			if (p[n] == ' ')
				cut = n;
		}
		if (p[n] && cut > 0)
			n = cut;
		memcpy(line, p, n);
		line[n] = 0;
		p += n;
		while (*p == ' ')
			p++;
		err.numlines++;
	}
	if (*p && err.numlines > 0) {
		char *last = err.lines[err.numlines - 1], more[ERR_LINE_BYTES + 4];
		snprintf(more, sizeof(more), "%s ...", last);
		d_text_fit(UIN_F_INTER10, more, ERR_WRAP_W, last, ERR_LINE_BYTES);
	}
}

void uin_error_open(const char *title, const char *text)
{
	err.open = true;
	err.opened = uin_now();
	Str_Copy(err.title, title ? title : "", sizeof(err.title));
	Str_Copy(err.text, text ? text : "", sizeof(err.text));
	/* a server's reason may end in a newline: it's one paragraph here */
	for (char *c = err.text; *c; c++)
		if ((unsigned char)*c < 0x20)
			*c = ' ';
	err.numlines = -1;
}

bool uin_error_active(void) { return err.open; }

void uin_error_key(int action, int key)
{
	(void)key;
	/* input waits for the fade-in, as it does in the views: an A still held
	 * from the screen behind doesn't dismiss it unread */
	if (uin_now() - err.opened < UIN_FADE_S)
		return;
	if (action == UIN_A_ACCEPT || action == UIN_A_BACK || action == UIN_A_START) {
		err.open = false;
		uin_sound(UIN_SND_ACCEPT);
	}
}

void uin_error_draw(float w, float h)
{
	/* design 6.5: (160,180) 320x120 at 640x480, #1a1a1a at 0.92 with a 1 px
	 * accent edge on top, the title in TG 15 and the reason in Inter 10 */
	uin_xf_t xf;
	float fade = BW_CLAMP((float)((uin_now() - err.opened) / UIN_FADE_S), 0.0f, 1.0f);
	if (err.numlines < 0)
		err_wrap();
	uin_xf_stretch(&xf, 640, 480, 0, 0, w, h);
	d_xf(&xf);
	d_fill(0, 0, 640, 480, UIN_HEXA(0x000000, 0.55f * fade));
	d_fill(160, 180, 320, 120, UIN_HEXA(0x1a1a1a, 0.92f * fade));
	d_fill(160, 180, 320, 1, uin_col_alpha(UIN_COL_ACCENT, fade));
	d_text(UIN_F_TG15, 172, 191, err.title, UIN_LEFT | UIN_TOP, uin_col_alpha(0xffffffffu, fade));
	for (int i = 0; i < err.numlines; i++)
		d_text(UIN_F_INTER10, 172, 216 + 14.0f * i, err.lines[i], UIN_LEFT | UIN_TOP, uin_col_alpha(0xd0d0d0ffu, fade));
	/* its own prompt inside the box: the screen's footer is behind the dim */
	uin_prompt(468, 290, "A", "OK", fade);
	d_xf(NULL);
}

/* --- loading screen -----------------------------------------------------------------------
 * Drawn by UI_Draw while cl_loadstage is set: from the main loop while a
 * client connects, and from cl_menu.c's loading hook during a blocking map
 * load (E1). The title's own backdrop map loads under the title instead. */

static cvar_t *cl_loadstage, *cl_loadmap, *cl_loadprogress, *webcore_menumap;

/* s upper-cased into out: the pixel face's capitals are its look */
static void upper(const char *s, char *out, size_t outsize)
{
	size_t n = 0;
	for (; s && *s && n + 1 < outsize; s++)
		out[n++] = (char)toupper((unsigned char)*s);
	out[n] = 0;
}

bool uin_loading_draw(float w, float h)
{
	char map[64], stage[96];
	float frac;
	uin_xf_t xf;
	/* the client's cvars (cl_menu.c), registered after UI_Init */
	if (!cl_loadstage) {
		cl_loadstage = Cvar_Find("cl_loadstage");
		cl_loadmap = Cvar_Find("cl_loadmap");
		cl_loadprogress = Cvar_Find("cl_loadprogress");
	}
	if (!webcore_menumap)
		webcore_menumap = Cvar_Find("webcore_menumap");
	if (!cl_loadstage || !cl_loadstage->string[0])
		return false;
	/* the title's backdrop map (webcore_menumap): the title stays up over
	 * its load. While the client connects cl_loadmap is still empty, so
	 * the open title tells it apart; a map typed at the console under the
	 * title names another map and gets the loading screen. */
	if (uin_kind_open(UIN_KIND_TITLE) && (!cl_loadmap || !cl_loadmap->string[0] ||
	                                      (webcore_menumap && !Str_ICmp(cl_loadmap->string, webcore_menumap->string))))
		return false;
	upper(cl_loadmap ? cl_loadmap->string : "", map, sizeof(map));
	upper(cl_loadstage->string, stage, sizeof(stage));
	frac = cl_loadprogress ? BW_CLAMP(cl_loadprogress->value, 0.0f, 1.0f) : 0;
	/* design 6.5, at 640x480: the map in FOCUS, the stage under it, the bar */
	uin_xf_stretch(&xf, 640, 480, 0, 0, w, h);
	d_xf(&xf);
	d_fill(0, 0, 640, 480, 0x000000ffu);
	d_text(UIN_F_STD16, 48, 384, map[0] ? map : "LOADING", UIN_LEFT | UIN_TOP, UIN_COL_FOCUS);
	d_text(UIN_F_STD8, 48, 408, stage, UIN_LEFT | UIN_TOP, 0xaaaaaaffu);
	d_fill(48, 424, 544, 6, 0x333333ffu);
	d_fill(48, 424, 544 * frac, 6, UIN_COL_ACCENT);
	d_xf(NULL);
	return true;
}
