/* The "Lv." badge above the character panel and the exp bar along the
   bottom edge of the screen. SOURCE-CONFIRMED in client.dll unless marked:

   - LevelPanel (ctor FUN_028022f0, images FUN_02802830, paint
     FUN_028029b0): resource/zombiez/zmode_level_bg sized 100x24 at the
     panel's origin; the "Lv." glyph cut from resource/zombiez/
     zmode_level_num (256x16: "Lv." then 0-9) at (12, 4), 25x14, texture
     rect (0, 1/16)-(25/256, 15/16); then the level in the hud.txt
     dollarNum_N_new digits (FUN_02a70be0 with 1), left to right from
     (43, 3) (FUN_02a70330), colour 255,255,255.
   - ExpPanel "ZBZExpPanel" (ctor FUN_02802230, images FUN_02802690,
     paint FUN_028028f0): resource/zombiez/zmode_level_bar_bg across the
     whole width, 4 high; the progress bar child "ZBZExpProgressBar" fills
     with resource/zombiez/zmode_level_bar. For 0.5 s after the value
     changes (FUN_02802ac0 stamps the time) the fill fades from red to
     white: colour 255, x, x with x = 2 * elapsed * 255.
   - Placement, the owner's PerformLayout (FUN_02830680 / FUN_0286bac0):
     LevelPanel at (0, H-70) 100x24, ExpPanel at (0, H-4) W x 4.
   - Panel coordinates are local to the panel; the digits sitting next to
     "Lv." in sokky's screenshot of the real client confirm that the
     number's (43, 3) is too (OBSERVED).

   Owners and data:
   - Zombie Z 45 and Playroom Zombie Z 55 (ZombieZBasePanel, think
     FUN_028be930): level and exp percent of the ZombieZPlayer, sent as
     ZBZInitLV / ZBZExp; the rules publish them as userinfo csnz_zbzlv /
     csnz_zbzexp (src/rules/modes/zombie.qc).
   - Voxel 38/39 (VoxelScriptBasePanel, FUN_0282fa20, drawn in the voxel
     modes only, FUN_0282f930): the account level and the exp into it,
     unless a map script turned on its own levels (vxl_lv_enable, not
     ported). Our account level is the progression's csnz_level; the
     progression has no exp key yet, so the bar shows 0 (INFERRED stand-in
     for the account data).
   - Zombie Scenario v2 15 (ZombieScenarioVer2 panel FUN_02852ce0, data
     from ZBSVer2 FUN_0284bd40) and ScenarioTX 53 (FUN_02867ba0): the
     scenario player's level and exp percent; the rules publish userinfo
     csnz_zbslv / csnz_zbsexp (src/rules/modes/scenario.qc).

   Level-up message, class ZombieScenarioVer2LevelUpMessage (ctor
   FUN_0282f030, named "ZBZLevelUpMsgPanel" by the Zombie Z, Zombie
   Scenario v2 and ScenarioTX owners, so one panel for all three):
   - Show (vtable 0xac, FUN_028308d0): the text is the localised
     #CSO_ZBSVER2_LevelUpMsg and the level, "%s %d"; on a fresh show it
     also starts resource/ZBSVER2/levelup_line_top.bik and
     levelup_line_bottom.bik. Bulwark can't play .bik, so the lines are
     not drawn (not ported).
   - It hides itself 2 s after showing (FUN_02830640).
   - Layout (FUN_02830710), in screen pixels: the background and the
     label fill (0, H * 0.7, W, 32); the background is the 3-slice
     resource/hud/zbs/textbg_left / textbg_center / textbg_right (left and
     right 128x32 images, centre 1x32 stretched). The label is centred in
     font "ResultTeam", colour 210,187,129. The font's size isn't traced;
     the HUD font stands in (DESIGN).
   - Zombie Z shows it from ZBZNotify subtype 0 (FUN_028c78d0) with the
     level mod 100; Zombie Scenario v2 from ZBSVer2 case 5, which also
     plays events/Lvup.wav (the rules play it here). The rules count level
     ups in userinfo csnz_zbzlvup / csnz_zbslvup and the HUD shows the
     message when the count goes up. */

float g_lpFrac;
float g_lpChanged;

void
LevelPanel_Draw(void)
{
	float modeId = HUD_ModeId();
	float level = 0;
	float frac = 0;

	switch (modeId) {
	case 45:
	case 55:
		level = stof(getplayerkeyvalue(player_localnum, "csnz_zbzlv"));
		frac = stof(getplayerkeyvalue(player_localnum, "csnz_zbzexp")) / 100;
		if (level <= 0)
			return;
		break;
	case 15:
	case 53:
		level = stof(getplayerkeyvalue(player_localnum, "csnz_zbslv"));
		frac = stof(getplayerkeyvalue(player_localnum, "csnz_zbsexp")) / 100;
		if (level <= 0)
			return;
		break;
	case 38:
	case 39:
		level = stof(getplayerkeyvalue(player_localnum, "csnz_level"));
		frac = 0;
		if (level <= 0)
			level = 1;
		break;
	default:
		return;
	}

	float H = HUD_H;
	float W = screen.HUDSize()[0] / g_hs;

	/* ExpPanel */
	if (frac != g_lpFrac) {
		g_lpFrac = frac;
		g_lpChanged = time;
	}
	vector col = [1, 1, 1];
	float t = time - g_lpChanged;
	if (t >= 0 && t < 0.5)
		col = [1, t * 2, t * 2];
	Art_PicRect([0, H - 4], [W, 4], "resource/zombiez/zmode_level_bar_bg", 255);
	frac = bound(0, frac, 1);
	if (frac > 0)
		drawpic(Art_Pos([0, H - 4]), "resource/zombiez/zmode_level_bar", [W * frac, 4] * g_hs, col, 1, 0);

	/* LevelPanel */
	vector o = [0, H - 70];
	Art_PicRect(o, [100, 24], "resource/zombiez/zmode_level_bg", 255);
	drawsubpic(Art_Pos(o + [12, 4]), [25, 14] * g_hs, "resource/zombiez/zmode_level_num",
		[0, 1 / 16], [25 / 256, 14 / 16], [1, 1, 1], 1, 0);
	Art_HudNum(o + [43, 3], "dollarNum_", level, [255, 255, 255], 255, true);
}

float g_lumCount;
var float g_lumShown = -100;
float g_lumLevel;

void
LevelUpMsg_Draw(void)
{
	float modeId = HUD_ModeId();
	string countKey = "", levelKey = "";

	switch (modeId) {
	case 45:
	case 55:
		countKey = "csnz_zbzlvup";
		levelKey = "csnz_zbzlv";
		break;
	case 15:
	case 53:
		countKey = "csnz_zbslvup";
		levelKey = "csnz_zbslv";
		break;
	default:
		return;
	}

	float count = stof(getplayerkeyvalue(player_localnum, countKey));
	if (count < g_lumCount)
		g_lumCount = count;	/* a new map or a reset */
	if (count > g_lumCount) {
		g_lumCount = count;
		g_lumShown = time;
		g_lumLevel = stof(getplayerkeyvalue(player_localnum, levelKey)) % 100;
	}
	if (time - g_lumShown >= 2 || time < g_lumShown)
		return;

	vector size = screen.HUDSize();
	vector o = screen.HUDMins() + [0, floor(size[1] * 0.7)];
	float W = size[0];
	drawpic(o, "resource/hud/zbs/textbg_left", [128, 32], [1,1,1], 1, 0);
	drawpic(o + [128, 0], "resource/hud/zbs/textbg_center", [max(0, W - 256), 32], [1,1,1], 1, 0);
	drawpic(o + [W - 128, 0], "resource/hud/zbs/textbg_right", [128, 32], [1,1,1], 1, 0);

	string msg = sprintf("%s %d", Loc("CSO_ZBSVER2_LevelUpMsg", "Level Up! Level:"), g_lumLevel);
	vector tp = o + [(W - font.StringWidth(msg, false, g_hudFont)) / 2, (32 - g_hudFont.iScaleY) / 2];
	draw.Text_RGB(tp, msg, [210, 187, 129] / 255, g_hudFont);
}
