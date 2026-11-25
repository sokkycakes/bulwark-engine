/* Bulwark engine - native UI: the lobby (lobby-menu), its Leave
 * confirmation and read-only chat (design 6.2).
 * Stub: a labelled box showing the lobby's phase; START starts (host) or
 * readies, X readies, B leaves. ISC licence, see LICENSE. */
#include "uin_local.h"
#include "../common/json.h"

#include <stdio.h>

typedef struct {
	double next_poll;
	char phase[24];
	bool host, ready;
	int players;
} lobby_t;

static void lobby_frame(uin_view_t *v, double dt)
{
	lobby_t *l = UIN_STATE(v, lobby_t);
	const char *r;
	json_t *j;
	(void)dt;
	if (uin_now() < l->next_poll)
		return;
	l->next_poll = uin_now() + 0.25; /* lobby-menu polls getlobby every 250 ms */
	if (!(r = uin_query(v, "getlobby")))
		return;
	if ((j = JSON_Parse(r, NULL, 0))) {
		const json_t *players = JSON_Get(j, "players");
		Str_Copy(l->phase, JSON_GetString(j, "phase", "?"), sizeof(l->phase));
		l->host = JSON_GetBool(j, "isHost", false);
		l->players = players ? players->count : 0;
		JSON_Free(j);
	}
}

static void lobby_key(uin_view_t *v, int action, int key, bool repeat)
{
	lobby_t *l = UIN_STATE(v, lobby_t);
	(void)key, (void)repeat;
	if (action == UIN_A_BACK) {
		uin_sound(UIN_SND_BACK);
		uin_query(v, "lobby_action:close"); /* WC_Frame then reopens the title */
	} else if (action == UIN_A_START || action == UIN_A_ACCEPT) {
		uin_sound(UIN_SND_ACCEPT);
		uin_query(v, l->host ? "lobby_action:start" : "lobby_action:ready");
	} else if (action == UIN_A_X) {
		l->ready = !l->ready;
		uin_query(v, l->ready ? "lobby_action:ready" : "lobby_action:unready");
	}
}

static void lobby_draw(uin_view_t *v, float x, float y, float w, float h)
{
	lobby_t *l = UIN_STATE(v, lobby_t);
	char line[96];
	uin_xf_t xf;
	uin_xf_fit(&xf, UIN_FRAME_HERO_W, UIN_FRAME_HERO_H, x, y, w, h);
	d_xf(&xf);
	d_fill(26, 9, 363, 462, UIN_HEXA(0x3d3d3d, 0.7f));
	uin_debug_box(26, 9, 363, 462, "LOBBY (native stub)", 0xffffffffu);
	snprintf(line, sizeof(line), "PHASE %s, %d PLAYERS%s", l->phase, l->players, l->host ? ", HOST" : "");
	d_text(UIN_F_INTER10, 36, 40, line, UIN_LEFT | UIN_TOP, 0xffffffffu);
	d_xf(NULL);
	uin_footer(w, h, l->host ? "START=START B=LEAVE" : "X=READY B=LEAVE");
}

const uin_screen_t uin_lobby_screen = {"lobby", NULL, NULL, lobby_key, lobby_frame, lobby_draw};
