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
   - ScenarioTX 53 and Zombie Scenario v2 have their own sources
     (FUN_02867ba0, ZBSVer2 FUN_0284bd40); not ported yet. */

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
