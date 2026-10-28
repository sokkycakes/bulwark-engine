/* Spectator bars (handoff M3): CCSSpectatorGUI.

   SOURCE-CONFIRMED (client.dll): the spectator GUI loads
   Resource/UI/Spectator.res (FUN_02d318b0) and its bottom bar
   Resource/UI/CSO_BottomSpectator.res (FUN_02d31450). client.dll names
   #Spec_Mode%d, #Spec_PlayerItem / _Team, #Cstrike_Spec_CT_Score and
   #Cstrike_Spec_Ter_Score.

   Layout (OBSERVED, cstrike res files): a 66-tall TopBar with the CT and
   terrorist scores right-aligned (CTScoreLabel at r268,12, its value at
   r112; TERScoreLabel at r268,24), a divider at r64, the timer icon
   gfx/vgui/timer at r56,26 with timerlabel at r42,24 and extrainfo at
   r56,12; a 66-tall BottomBar with playerlabel centred and the
   bottom-bar row (settingscombo, playerprev, playercombo, playernext,
   viewcombo) at y 30. playerlabel is hidden in the res file and stays
   hidden; playercombo shows the spectated player.

   Spectator.res is in 640x480 units with the bottom bar at y 413. Here the
   top bar keeps its offsets from the top and the bottom bar is pinned to
   the screen's bottom, with the right-aligned labels kept from the right
   edge (INFERRED: CS's spectator GUI is resized to the screen in code).
   The bars are flat black at 75% (DESIGN: their scheme colour is not
   read). The combos show their current value and don't open (DESIGN):
   Nuclide's spectator controls (fire for next/previous, jump for the
   mode) still drive the camera. extrainfo shows the map name. */

hashtable g_sgRes;
hashtable g_sgBottomRes;

void
SG_Init(void)
{
	g_sgRes = Res_Load("resource/ui/spectator.res");
	g_sgBottomRes = Res_Load("resource/ui/cso_bottomspectator.res");
	Loc_Load("resource/cstrike_na_en.utf8.txt", "Cstrike_Spec_");
	Loc_Load("resource/valve_na_en.utf8.txt", "Spec_Mode Spec_PlayerItem");
	precache_pic("gfx/vgui/timer");
}

/* a res label: its text right- ("east"), left- ("west") or centre-aligned
   in its box */
void
SG_Label(hashtable tab, vector org, vector area, string ctl, string txt, vector rgb)
{
	vector p = org + Res_Pos(tab, ctl, area);
	vector s = Res_Size(tab, ctl);
	string align = Res_Get(tab, ctl, "textAlignment");
	float w = font.StringWidth(txt, false, g_hudFont) / g_hs;

	if (align == "east")
		p[0] += s[0] - w;
	else if (align == "center")
		p[0] += (s[0] - w) / 2;

	draw.Text_RGB(Art_Pos(p + [0, (s[1] - font.GetHeight(g_hudFont) / g_hs) / 2]), txt, rgb, g_hudFont);
}

/* a combo box drawn closed: a frame and its current text */
void
SG_Combo(vector org, vector area, string ctl, string txt)
{
	vector p = org + Res_Pos(g_sgBottomRes, ctl, area);
	vector s = Res_Size(g_sgBottomRes, ctl);

	drawfill(Art_Pos(p), s * g_hs, [0.15, 0.15, 0.15], 0.9, 0);
	drawfill(Art_Pos(p), [s[0], 1] * g_hs, [0.4, 0.4, 0.4], 1, 0);
	drawfill(Art_Pos(p + [0, s[1] - 1]), [s[0], 1] * g_hs, [0.4, 0.4, 0.4], 1, 0);
	draw.Text_RGB(Art_Pos(p + [6, (s[1] - font.GetHeight(g_hudFont) / g_hs) / 2]), txt, [1,1,1], g_hudFont);
}

void
SG_Draw(void)
{
	vector area = screen.HUDSize() / g_hs;
	float barH = 66;
	vector top = [0, 0];
	vector bottom = [0, area[1] - barH];
	ncSpectatorMode_t mode = spectating.Mode();
	string target = spectating.Name();

	/* top bar */
	drawfill(Art_Pos(top), [area[0], barH] * g_hs, [0, 0, 0], 0.75, 0);
	SG_Label(g_sgRes, top, area, "CTScoreLabel", Loc("Cstrike_Spec_CT_Score", "Counter-Terrorists:"), [1,1,1]);
	SG_Label(g_sgRes, top, area, "CTScoreValue", serverkey("csnz_s2"), [1,1,1]);
	SG_Label(g_sgRes, top, area, "TERScoreLabel", Loc("Cstrike_Spec_Ter_Score", "Terrorists:"), [1,1,1]);
	SG_Label(g_sgRes, top, area, "TERScoreValue", serverkey("csnz_s1"), [1,1,1]);

	vector dp = top + Res_Pos(g_sgRes, "DividerBar", area);
	drawfill(Art_Pos(dp), Res_Size(g_sgRes, "DividerBar") * g_hs, [0.6, 0.6, 0.6], 1, 0);

	vector tp = top + Res_Pos(g_sgRes, "timerimage", area);
	drawpic(Art_Pos(tp), "gfx/vgui/timer", Res_Size(g_sgRes, "timerimage") * g_hs, [1,1,1], 1, 0);

	float left = Board_TimeLeft();
	if (left >= 0)
		SG_Label(g_sgRes, top, area, "timerlabel", sprintf("%i:%02i", (int)floor(left / 60), (int)floor(left) % 60i), [1,1,1]);
	SG_Label(g_sgRes, top, area, "extrainfo", mapname, [0.8, 0.8, 0.8]);

	/* bottom bar; its res y values are from the bar's own top (413) */
	drawfill(Art_Pos(bottom), [area[0], barH] * g_hs, [0, 0, 0], 0.75, 0);


	SG_Combo(bottom, area, "playercombo", target);
	SG_Combo(bottom, area, "viewcombo", Loc(sprintf("Spec_Mode%d", mode), ""));
	SG_Combo(bottom, area, "settingscombo", "");
	Res_DrawButton(g_sgBottomRes, bottom, area, "playerprev", "<", true, false);
	Res_DrawButton(g_sgBottomRes, bottom, area, "playernext", ">", true, false);
}
