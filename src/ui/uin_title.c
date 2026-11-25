/* Bulwark engine - native UI: the title (title-menu-v2), its Create and
 * Join dialogs and the quit confirmation (design 6.1).
 * Stub: a labelled box with a plain list, enough to reach the lobby, the
 * settings and quit with a pad. ISC licence, see LICENSE. */
#include "uin_local.h"

typedef struct {
	int focus;
} title_t;

static const struct {
	const char *label, *cmd;
} items[] = {
	{"CREATE GAME (OFFLINE)", "lobby_create_offline\nmenu_webcore_lobby\n"},
	{"CREATE GAME (SYSTEM LINK)", "lobby_create_lan\nmenu_webcore_lobby\n"},
	{"SETTINGS", "menu_options\n"},
	{"QUIT", "quit\n"},
};
#define NUM_ITEMS ((int)BW_ARRAY_COUNT(items))

static void title_key(uin_view_t *v, int action, int key, bool repeat)
{
	title_t *t = UIN_STATE(v, title_t);
	(void)key, (void)repeat;
	if (action == UIN_A_UP || action == UIN_A_DOWN) {
		t->focus = (t->focus + (action == UIN_A_UP ? NUM_ITEMS - 1 : 1)) % NUM_ITEMS;
		uin_sound(UIN_SND_MOVE);
	} else if (action == UIN_A_ACCEPT) {
		uin_sound(UIN_SND_ACCEPT);
		uin_cmd(items[t->focus].cmd);
	} else if (action == UIN_A_Y) {
		uin_cmd("menu_options\n");
	}
}

static void title_draw(uin_view_t *v, float x, float y, float w, float h)
{
	title_t *t = UIN_STATE(v, title_t);
	uin_xf_t xf;
	uint32_t a = (uint32_t)(v->fade * 255);
	uin_xf_stretch(&xf, 640, 480, x, y, w, h);
	d_xf(&xf);
	/* the backdrop map shows through: a wash as the page has (title-menu-v2.css:34-42) */
	d_fill(0, 0, 640, 480, UIN_HEXA(0x690002, 0.67f * v->fade));
	uin_debug_box(32, 300, 300, 24 + 16 * NUM_ITEMS, "TITLE (native stub)", 0xffffff00u | a);
	for (int i = 0; i < NUM_ITEMS; i++) {
		float iy = 320 + 16.0f * i;
		if (i == t->focus)
			d_fill(36, iy - 2, 292, 13, 0xbfbfbf00u | a);
		d_text(UIN_F_STD8, 44, iy, items[i].label, UIN_LEFT | UIN_TOP, (i == t->focus ? 0x40404000u : 0xffffff00u) | a);
	}
	d_xf(NULL);
	uin_footer(w, h, "A=SELECT Y=SETTINGS");
}

const uin_screen_t uin_title_screen = {"title", NULL, NULL, title_key, NULL, title_draw};
