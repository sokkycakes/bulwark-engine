/* Bulwark engine - native UI: the in-game pause page (pause-menu,
 * design 6.3). Stub: a labelled box; START and B close it, A on the first
 * row too. ISC licence, see LICENSE. */
#include "uin_local.h"
#include "../client/keys.h"

static void pause_key(uin_view_t *v, int action, int key, bool repeat)
{
	(void)v, (void)repeat;
	/* Esc in a game is closed by the router after it reaches us
	 * (WC_KeyEvent): closing here too would close twice */
	if (key == K_ESCAPE)
		return;
	if (action == UIN_A_START || action == UIN_A_BACK || action == UIN_A_ACCEPT) {
		uin_sound(UIN_SND_BACK);
		uin_cmd("webcore_closemenu\n");
	}
}

static void pause_draw(uin_view_t *v, float x, float y, float w, float h)
{
	uin_xf_t xf;
	uin_xf_stretch(&xf, UIN_FRAME_PAUSE_W, UIN_FRAME_PAUSE_H, x, y, w, h);
	d_xf(&xf);
	/* the panel and banner of pause-menu.css, roughly */
	d_grad_v(149, 53, 1131, 666, 0x272727c9u, 0x494949c9u);
	d_fill(149, 53, 146, 666, 0x1a1a1affu);
	d_xf(NULL);
	uin_debug_box(xf.ox + 149 * xf.sx, xf.oy + 53 * xf.sy, 1131 * xf.sx, 666 * xf.sy, "PAUSE (native stub)",
	              uin_col_alpha(0xffffffffu, v->fade));
	d_text_ex(UIN_F_PAUSE, xf.ox + 222 * xf.sx, xf.oy + 386 * xf.sy, "PAUSE", UIN_CENTER | UIN_MIDDLE, UIN_COL_ACCENT, 0,
	          -90);
	d_text(UIN_F_TG15, xf.ox + 320 * xf.sx, xf.oy + 74 * xf.sy, "RETURN TO GAME", UIN_LEFT | UIN_TOP, 0xffffffffu);
	uin_footer(w, h, "A=RETURN B=BACK");
}

const uin_screen_t uin_pause_screen = {"pause", NULL, NULL, pause_key, NULL, pause_draw};
