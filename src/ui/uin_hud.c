/* Bulwark engine - native UI: Stiletto's HUD (data/web/hud/app.js, design
 * section 3), drawn by UI_DrawAt into the rectangle hud.qc's drawpic gives.
 * Stub: the safe rectangle outlined and a few of the baked pieces in their
 * places, to show the atlas works. ISC licence, see LICENSE. */
#include "uin_local.h"

static void hud_draw(uin_view_t *v, float x, float y, float w, float h)
{
	float sx, sy, sw, sh, s;
	uint32_t white = uin_col_alpha(0xffffffffu, v->alpha);
	(void)x, (void)y, (void)w, (void)h;
	uin_safe_rect(&sx, &sy, &sw, &sh);
	s = BW_MIN(sw / UIN_FRAME_HUD_W, sh / UIN_FRAME_HUD_H);
	uin_debug_box(sx, sy, sw, sh, "HUD (native stub)", uin_col_alpha(0xffffff60u, v->alpha));
	/* centre clusters: X = cx + (x - 720) s, Y = top + y s (design 3.1) */
	{
		float cx = sx + sw / 2;
		d_img(UIN_R_TIMEPLATE, cx + (596 - 720) * s, sy + 17 * s, 248 * s, 164 * s, white);
		d_text(UIN_F_TIMER, cx, sy + 120.46f * s, "00:00", UIN_CENTER | UIN_MIDDLE, white);
		d_text(UIN_F_SCORE, cx + (642 - 720) * s, sy + 115.5f * s, "0", UIN_RIGHT | UIN_MIDDLE, 0xc7c7c7ffu);
		d_text(UIN_F_SCORE, cx + (803 - 720) * s, sy + 115.4f * s, "0", UIN_LEFT | UIN_MIDDLE, 0xc7c7c7ffu);
		d_img(UIN_R_HEART, cx + (702 - 720) * s, sy + 600 * s, 35 * s, 35 * s, white);
		d_text(UIN_F_CLIP, cx + (774.5f - 720) * s, sy + 588.5f * s, "6|", UIN_CENTER | UIN_MIDDLE, white);
		d_text(UIN_F_SPEED, cx, sy + 928 * s, "000", UIN_CENTER | UIN_MIDDLE, uin_col_alpha(0xd9e0e5ffu, 0.54f));
	}
}

const uin_screen_t uin_hud_screen = {"hud", NULL, NULL, NULL, NULL, hud_draw};
