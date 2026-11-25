/* Bulwark engine - native UI: the hero picker, ql-menu's arena selector
 * (design 6.4) and a generic one for the other modes.
 * Stub: a labelled box on the arena panel; B closes it through
 * arena_action:close, which may close this view inside the query.
 * ISC licence, see LICENSE. */
#include "uin_local.h"

static void hero_key(uin_view_t *v, int action, int key, bool repeat)
{
	(void)key, (void)repeat;
	if (action == UIN_A_BACK || action == UIN_A_START) {
		uin_sound(UIN_SND_BACK);
		if (!uin_query(v, "arena_action:close"))
			return; /* closed us: v is gone */
	}
}

static void hero_draw(uin_view_t *v, float x, float y, float w, float h)
{
	uin_xf_t xf;
	char mode[16] = "";
	uin_xf_fit(&xf, UIN_FRAME_HERO_W, UIN_FRAME_HERO_H, x, y, w, h);
	d_xf(&xf);
	/* scoreInset(68,58,504,332), ql-menu/app.js:1526 */
	d_img(UIN_R_SCORE_TL, 68, 58, 20, 20, 0xffffffffu);
	d_img(UIN_R_SCORE_T, 88, 58, 464, 20, 0xffffffffu);
	d_img(UIN_R_SCORE_TR, 552, 58, 20, 20, 0xffffffffu);
	d_img(UIN_R_SCORE_L, 68, 78, 10, 302, 0xffffffffu);
	d_img(UIN_R_SCORE_M, 78, 78, 484, 302, 0xffffffffu);
	d_img(UIN_R_SCORE_R, 562, 78, 10, 302, 0xffffffffu);
	d_img(UIN_R_SCORE_BL, 68, 380, 10, 10, 0xffffffffu);
	d_img(UIN_R_SCORE_B, 78, 380, 484, 10, 0xffffffffu);
	d_img(UIN_R_SCORE_BR, 562, 380, 10, 10, 0xffffffffu);
	uin_search_get(v, "mode", mode, sizeof(mode));
	uin_debug_box(68, 58, 504, 332, "", 0xffffffffu);
	d_text(UIN_F_STD16, 84, 88, "HERO PICKER (native stub)", UIN_LEFT | UIN_TOP, 0xffffffffu);
	d_text(UIN_F_STD8, 84, 112, mode[0] ? mode : "no mode", UIN_LEFT | UIN_TOP, UIN_COL_FOCUS);
	d_img(UIN_R_NAVRIGHT, 300, 160, 80, 20, 0xffffffffu);
	d_img(UIN_R_NAVRIGHTY, 388, 160, 80, 20, 0xffffffffu);
	d_img(UIN_R_BIGRED, 445, 206, 103, 24, 0xffffffffu);
	d_xf(NULL);
	uin_footer(w, h, "B=BACK");
}

const uin_screen_t uin_hero_screen = {"hero", NULL, NULL, hero_key, NULL, hero_draw};
