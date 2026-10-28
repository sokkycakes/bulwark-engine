/* Score board at the top centre, and the round messages.

   Art: resource/hud/hud_scoreboard_bg (280x83, OBSERVED). Its regions,
   measured from the image (OBSERVED): a red left panel (x 21-103), a grey
   centre box (103-177) and a blue right panel (177-259) over y 6-55, and a
   strip below (y 56-75) with a red figure at the left and a blue one at
   the right. What goes where is INFERRED from those regions and the art
   names: team labels hud_text_icon_*, team scores in hud_sb_num_big_red /
   _blue, the round in the centre (hud_text_icon_round, big white digits),
   living players beside the figures and the round timer in the strip
   (hud_sb_num_bottom with its colon). Needs a capture to confirm.

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

/* the label art for a team name */
string
Board_TeamLabel(string team, bool left)
{
	string side = left ? "left" : "right";

	switch (strtolower(team)) {
	case "zombie": return strcat("resource/hud/hud_text_icon_zb_", side);
	case "human": return strcat("resource/hud/hud_text_icon_hm_", side);
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

void
Board_Draw(void)
{
	float x0 = Board_CX() - BOARD_W / 2;
	string t1 = serverkey("csnz_t1");
	string t2 = serverkey("csnz_t2");
	string lbl;

	if (serverkey("csnz_state") == "")
		return;

	Art_Pic([x0, 0], BOARD_PIC, 1.0f);

	/* teams: label, then score */
	if (t1 != "" || t2 != "") {
		lbl = Board_TeamLabel(t1, true);
		if (lbl != "")
			Art_Pic([x0 + 62 - 31, 10], lbl, 1.0f);
		Art_NumC([x0 + 62, 24], NUM_BIGRED, serverkey("csnz_s1"), [1,1,1], 1.0f);

		lbl = Board_TeamLabel(t2, false);
		if (lbl != "")
			Art_Pic([x0 + 218 - 31, 10], lbl, 1.0f);
		Art_NumC([x0 + 218, 24], NUM_BIGBLUE, serverkey("csnz_s2"), [1,1,1], 1.0f);

		/* living players beside the figures */
		Art_NumR([x0 + 84, 60], NUM_BOTTOM, serverkey("csnz_a1"), [1,1,1], 1.0f);
		Art_Num([x0 + 197, 60], NUM_BOTTOM, serverkey("csnz_a2"), [1,1,1], 1.0f);
	}

	/* round */
	Art_Pic([x0 + 140 - 35, 10], "resource/hud/hud_text_icon_round", 1.0f);
	Art_NumC([x0 + 140, 24], NUM_BIGWHITE, serverkey("csnz_round"), [1,1,1], 1.0f);

	/* timer */
	float left = Board_TimeLeft();
	if (left >= 0) {
		string mm = sprintf("%i", (int)floor(left / 60));
		string ss = sprintf("%02i", (int)floor(left) % 60i);
		float w = Art_NumWidth(NUM_BOTTOM, mm) + 6 + Art_NumWidth(NUM_BOTTOM, ss);
		float x = x0 + 140 - w / 2;

		x = Art_Num([x, 60], NUM_BOTTOM, mm, [1,1,1], 1.0f);
		Art_Pic([x + 1, 61], "resource/hud/hud_sb_num_bottom_colon", 1.0f);
		Art_Num([x + 6, 60], NUM_BOTTOM, ss, [1,1,1], 1.0f);
	}
}

/* Round messages: the rules' banner (csnz_msg) for 5 s after it changes,
   and Zombi's selection countdown (zb_status, OBSERVED text
   CSO_ZombiSelectCount) while it runs. Drawn as centred text below the
   board; CSO's own announcement art (resource/announceribbon) is not used
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

	if (serverkey("csnz_state") == "freeze" && serverkey("csnz_mode") == "8") {
		msg = serverkey("zb_status");
		draw.Text(p - [font.StringWidth(msg, false, fnt) / 2, 0], msg, fnt);
		p[1] += font.GetHeight(fnt) + 4;
	}

	if (time < g_msgUntil) {
		msg = serverkey("csnz_msg");
		draw.Text(p - [font.StringWidth(msg, false, fnt) / 2, 0], msg, fnt);
	}
}
