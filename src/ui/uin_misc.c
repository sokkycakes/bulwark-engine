/* Bulwark engine - native UI: the pieces every screen shares, the
 * button-hint footer, the error dialog and the loading screen.
 * (P0/P1: a working footer and a plain error box; the loading screen and
 * the dialog's final look are design 6.5, phase P3.) ISC licence, see LICENSE. */
#include "uin_local.h"

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
			x -= d_text_width(UIN_F_STD8, label, 0);
			d_text(UIN_F_STD8, x, base, label, UIN_LEFT | UIN_BASELINE, UIN_COL_FOOTER_TEXT);
			x -= 4 + 12;
			d_img(UIN_R_DISC, x, base - 10, 12, 12, pad_colour(button[0]));
			d_text(UIN_F_STD8, x + 6, base - 4, button, UIN_CENTER | UIN_MIDDLE, 0x101010ffu);
			x -= 12;
		}
		end = start;
		while (end > spec && end[-1] == ' ')
			end--;
	}
}

/* --- error dialog ----------------------------------------------------------------------- */

static struct {
	bool open;
	char title[32];
	char text[256];
} err;

void uin_error_open(const char *title, const char *text)
{
	err.open = true;
	Str_Copy(err.title, title ? title : "", sizeof(err.title));
	Str_Copy(err.text, text ? text : "", sizeof(err.text));
}

bool uin_error_active(void) { return err.open; }

void uin_error_key(int action, int key)
{
	(void)key;
	if (action == UIN_A_ACCEPT || action == UIN_A_BACK) {
		err.open = false;
		uin_sound(UIN_SND_ACCEPT);
	}
}

void uin_error_draw(float w, float h)
{
	/* design 6.5: (160,180) 320x120 at 640x480 */
	uin_xf_t xf;
	uin_xf_stretch(&xf, 640, 480, 0, 0, w, h);
	d_xf(&xf);
	d_fill(160, 180, 320, 120, UIN_HEXA(0x1a1a1a, 0.92f));
	d_fill(160, 180, 320, 1, UIN_COL_ACCENT);
	d_text(UIN_F_TG15, 172, 190, err.title, UIN_LEFT | UIN_TOP, 0xffffffffu);
	d_text(UIN_F_INTER10, 172, 216, err.text, UIN_LEFT | UIN_TOP, 0xd0d0d0ffu);
	d_xf(NULL);
	uin_footer(w, h, "A=OK");
}

/* --- loading screen ----------------------------------------------------------------------- */

bool uin_loading_draw(float w, float h)
{
	/* P3 (design 6.5): nothing yet, the views draw as they are */
	(void)w, (void)h;
	return false;
}
