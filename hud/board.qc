/* Score board at the top centre and the round timer.

   SOURCE-CONFIRMED from client.dll:
   - the board is a set of child panels of resource/Hud/hud_scoreboard_bg
     (280x83), centred, at y 0 (FUN_02823110). Their defaults are made in
     FUN_02823380; FUN_02823c70 picks the layout per mode and places the
     children relative to the background:
       team modes (FUN_02823a60) .......... FUN_028242f0
       zombie modes (FUN_02823b60) ........ FUN_02824ca0
       1, 4, 10, 30 (FUN_028239a0) ........ FUN_02823ee0, background
                                             hud_scoreboard_bg_zombie-gaint
     Other modes draw no generic board (some have their own HUD class).
   - number panels (CHudImageNumber::Paint FUN_02a8fb70): the value is
     printed "%0<n>d", each digit is a cell of the digit strip, cells are
     <gap> apart, and the run is centred in the panel's box both ways
     (alignment 2, 2).
   - the timer (CHudRoundTimer, VidInit FUN_02821690, Draw FUN_028205a0):
     hud_sb_num_bottom 8x12 digits 2 apart, mm, the 3x10 colon one pixel
     lower, ss, the whole 43 px centred on the screen at y 59 (65 in 26,
     28, 52; 62 in 12; 76 in 51; 18 in 56). Orange 255,152,30 while its
     warning flag is set (low time, INFERRED), white otherwise.

   What feeds each number is partly INFERRED: the board's think
   (FUN_02a74aa0) reads player and team data through fields not traced
   yet. Panel 7 is the round (DAT_03a83f10 + 1, SOURCE-CONFIRMED). The
   rest follow the art: team scores in the big red/blue numbers, living
   players in the small ones beside the figures, the win limit after the
   slash.

   Data: the shared serverinfo keys (INTERFACES.md section 3): team 1 is
   the left (red) side, team 2 the right (blue). */

#define BOARD_PIC "resource/hud/hud_scoreboard_bg"
#define BOARD_W 280

string g_lastMsg;
float g_lastMsgSeq;
float g_msgUntil;

/* layout x of the screen's centre */
float
Board_CX(void)
{
	return (screen.HUDSize()[0] / g_hs) / 2;
}

/* the label art for a team name (team modes) */
string
Board_TeamLabel(string team, bool left)
{
	string side = left ? "right" : "left";	/* CSO's art names are mirrored: tr_right sits on the left */

	switch (strtolower(team)) {
	case "zombie":
	case "zombies":
		return strcat("resource/hud/hud_text_icon_zb_", side);
	case "human":
	case "humans":
		return strcat("resource/hud/hud_text_icon_hm_", side);
	case "tr": return strcat("resource/hud/hud_text_icon_tr_", side);
	case "ct": return strcat("resource/hud/hud_text_icon_ct_", side);
	}

	return "";
}

/* seconds left in the current state, or -1 when there is no timer */
float
Board_TimeLeft(void)
{
	float end = serverkeyfloat("csnz_stateend");

	if (end <= 0)
		return -1;

	return max(0, ceil(end - time));
}

/* a CHudImageNumber: value "%0<pad>d" from a digit strip of cells
   dw x dh, gap apart, centred in the box at p (layout units) */
void
Board_Number(vector p, vector box, string strip, float dw, float dh, float gap, float pad, float value)
{
	string s = sprintf("%i", (int)max(0, value));
	vector isz = drawgetimagesize(strip);

	if (isz[0] <= 0)
		return;
	while (strlen(s) < pad)
		s = strcat("0", s);

	float n = strlen(s);
	float w = (n - 1) * gap + dw * n;
	float x = p[0] + floor((box[0] - w) / 2);
	float y = p[1] + floor((box[1] - dh) / 2);

	for (float i = 0; i < n; i++) {
		float d = str2chr(s, i) - '0';
		drawsubpic(Art_Pos([x + (dw + gap) * i, y]), [dw, dh] * g_hs, strip,
			[d * dw / isz[0], 0], [dw / isz[0], dh / isz[1]], [1,1,1], 1.0f, 0);
	}
}

void
BN_BIG(vector p, string strip, float v)
{
	Board_Number(p, [60, 22], strip, 18, 22, 3, 2, v);
}

void
BN_CENTER(vector p, float w, float v)
{
	Board_Number(p, [w, 13], "resource/hud/hud_sb_num_center", 11, 13, 2, 2, v);
}

void
BN_SMALL(vector p, string strip, float v)
{
	Board_Number(p, [18, 11], strip, 8, 11, 2, 2, v);
}

/* 0 none, 1 team, 2 zombie, 3 deathmatch */
float
Board_Layout(float modeId)
{
	switch (modeId) {
	case 8: case 9: case 14: case 20: case 24: case 29: case 37:
	case 44: case 45: case 54: case 56: case 58:
		return 2;
	case 0: case 2: case 3: case 5: case 6: case 7: case 16: case 19:
	case 23: case 32: case 42: case 57: case 27: case 50: case 40:
		return 1;
	case 1: case 4: case 10: case 30:
		return 3;
	}
	return 0;
}

/* the most frags of any player, and the local player's */
vector
Board_Frags(void)
{
	float best = 0;

	for (float i = 0; i < maxclients; i++) {
		if (getplayerkeyvalue(i, "name") == "")
			continue;
		best = max(best, getplayerkeyfloat(i, "frags"));
	}
	return [best, getplayerkeyfloat(player_localnum, "frags")];
}

void
Board_DrawTimer(float modeId)
{
	float left = Board_TimeLeft();
	float y = 59;
	string strip = "resource/hud/hud_sb_num_bottom";
	vector isz = drawgetimagesize(strip);

	if (left < 0 || isz[0] <= 0)
		return;

	switch (modeId) {
	case 26: case 28: case 52: y = 65; break;
	case 12: y = 62; break;
	case 51: y = 76; break;
	case 56: y = 18; break;
	}

	string mm = sprintf("%02i", (int)floor(left / 60));
	string ss = sprintf("%02i", (int)floor(left) % 60i);
	float x = floor(Board_CX() - 43 / 2);
	vector rgb = (left <= 10) ? [255, 152, 30] / 255 : [1,1,1];

	for (float i = 0; i < 2; i++) {
		float d = str2chr(mm, i) - '0';
		drawsubpic(Art_Pos([x + 10 * i, y]), [8, 12] * g_hs, strip,
			[d * 8 / isz[0], 0], [8 / isz[0], 1], rgb, 1.0f, 0);
		d = str2chr(ss, i) - '0';
		drawsubpic(Art_Pos([x + 25 + 10 * i, y]), [8, 12] * g_hs, strip,
			[d * 8 / isz[0], 0], [8 / isz[0], 1], rgb, 1.0f, 0);
	}
	drawpic(Art_Pos([x + 20, y + 1]), "resource/hud/hud_sb_num_bottom_colon", [3, 10] * g_hs, rgb, 1.0f, 0);
}

void
Board_Draw(void)
{
	float modeId = HUD_ModeId();
	float layout = Board_Layout(modeId);
	float x0 = floor(Board_CX() - BOARD_W / 2);
	string lbl;

	if (serverkey("csnz_state") == "")
		return;
	/* Shelter Team draws its own (shelter.qc) */
	if (modeId == 28)
		return;

	if (layout == 3)
		Art_Pic([x0, 0], "resource/hud/hud_scoreboard_bg_zombie-gaint", 1.0f);
	else if (layout != 0)
		Art_Pic([x0, 0], BOARD_PIC, 1.0f);

	if (layout == 2) {
		/* zombies left, humans right */
		Art_Pic([x0 + 31, 9], "resource/hud/hud_text_icon_zb_right", 1.0f);
		Art_Pic([x0 + 178, 9], "resource/hud/hud_text_icon_hm_left", 1.0f);
		Art_Pic([x0 + 105, 17], "resource/hud/hud_text_icon_round", 1.0f);
		BN_BIG([x0 + 39, 26], "resource/hud/hud_sb_num_big_red", serverkeyfloat("csnz_s1"));
		BN_BIG([x0 + 181, 26], "resource/hud/hud_sb_num_big_blue", serverkeyfloat("csnz_s2"));
		BN_CENTER([x0 + 109, 31], 24, serverkeyfloat("csnz_round"));
		Art_Pic([x0 + 135, 31], "resource/hud/hud_sb_num_center_slash", 1.0f);
		BN_CENTER([x0 + 145, 31], 24, serverkeyfloat("csnz_goal"));
		BN_SMALL([x0 + 65, 60], "resource/hud/hud_sb_num_small_red", serverkeyfloat("csnz_a1"));
		BN_SMALL([x0 + 196, 60], "resource/hud/hud_sb_num_small_blue", serverkeyfloat("csnz_a2"));
	} else if (layout == 1) {
		/* tr_right / ct_left, a_right / b_left in 32, 35, 36, 43 */
		float ab = 0;
		if (modeId == 32 || modeId == 35 || modeId == 36 || modeId == 43)
			ab = 1;
		lbl = ab ? "resource/hud/hud_text_icon_a_right" : Board_TeamLabel(serverkey("csnz_t1"), true);
		if (lbl == "")
			lbl = "resource/hud/hud_text_icon_tr_right";
		Art_Pic([x0 + 39, 9], lbl, 1.0f);
		lbl = ab ? "resource/hud/hud_text_icon_b_left" : Board_TeamLabel(serverkey("csnz_t2"), false);
		if (lbl == "")
			lbl = "resource/hud/hud_text_icon_ct_left";
		Art_Pic([x0 + 178, 9], lbl, 1.0f);

		/* centre: goal in 16, team-kill in 2 5 19 22 25 27 40 57, win */
		if (modeId == 16)
			lbl = "resource/hud/hud_text_icon_goal_center";
		else if (modeId == 2 || modeId == 5 || modeId == 19 || modeId == 22 || modeId == 25 || modeId == 27 || modeId == 40 || modeId == 57)
			lbl = "resource/hud/hud_text_icon_team-kill";
		else
			lbl = "resource/hud/hud_text_icon_win_center";
		Art_Pic([x0 + 105, 17], lbl, 1.0f);

		BN_BIG([x0 + 39, 26], "resource/hud/hud_sb_num_big_red", serverkeyfloat("csnz_s1"));
		BN_BIG([x0 + 181, 26], "resource/hud/hud_sb_num_big_blue", serverkeyfloat("csnz_s2"));
		BN_CENTER([x0 + 122, 32], 37, serverkeyfloat("csnz_goal"));
		BN_SMALL([x0 + 65, 60], "resource/hud/hud_sb_num_small_red", serverkeyfloat("csnz_a1"));
		BN_SMALL([x0 + 196, 60], "resource/hud/hud_sb_num_small_blue", serverkeyfloat("csnz_a2"));
	} else if (layout == 3) {
		/* 1st / my kills (score in 10 and 30), goal in the centre */
		vector fr = Board_Frags();
		Art_Pic([x0 + 38, 9], "resource/hud/hud_text_icon_1st", 1.0f);
		Art_Pic([x0 + 178, 9], "resource/hud/hud_text_icon_my", 1.0f);
		Art_Pic([x0 + 105, 17], (modeId == 10 || modeId == 30) ? "resource/hud/hud_text_icon_score" : "resource/hud/hud_text_icon_kill_center", 1.0f);
		BN_BIG([x0 + 39, 26], "resource/hud/hud_sb_num_big_white", fr[0]);
		BN_BIG([x0 + 181, 26], "resource/hud/hud_sb_num_big_white", fr[1]);
		BN_CENTER([x0 + 122, 32], 37, serverkeyfloat("csnz_goal"));
	}

	Board_DrawTimer(modeId);
}

/* Round messages: the rules' banner (csnz_msg) for 5 s after it changes,
   and the zombie selection countdown (zb_status, OBSERVED text
   CSO_ZombiSelectCount) during freeze time; zombi.dat and csnz.dat's
   infection modes publish it, other modes leave it empty. Drawn as centred
   text below the board; CSO's own announcement art (resource/announceribbon) is not used
   yet (DESIGN). */
void
Board_DrawMessages(font_s fnt)
{
	float seq = serverkeyfloat("csnz_msgseq");
	string msg;
	vector p;

	if (seq != g_lastMsgSeq) {
		g_lastMsgSeq = seq;
		g_msgUntil = time + 5.0f;
	}

	p = Art_Pos([Board_CX(), 96]);

	msg = serverkey("zb_status");
	if (serverkey("csnz_state") == "freeze" && msg != "") {
		draw.Text(p - [font.StringWidth(msg, false, fnt) / 2, 0], msg, fnt);
		p[1] += font.GetHeight(fnt) + 4;
	}

	if (time < g_msgUntil) {
		msg = serverkey("csnz_msg");
		draw.Text(p - [font.StringWidth(msg, false, fnt) / 2, 0], msg, fnt);
	}
}
