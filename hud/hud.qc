/* CSNZ in-game HUD (Nuclide HudC progs, loaded as hud.dat).

   Pieces: board.qc (score board and round messages at the top),
   panels.qc (character and weapon panels at the bottom), crosshair.qc,
   killfeed.qc.
   Evidence levels are in each file; specifications/ui/hud-manifest.csv
   tracks every HUD element. */


void
HUD_Init(void)
{
	font.Load("fonts/ui.font", g_hudFont);
	Art_Init();
	Scores_Init();
	BM_Init();
	TM_Init();
	CM_Init();
	SG_Init();
	PTN_Init();
	VK_Init();
	SHH_Init();
	precache_pic("resource/hud/hud_scoreboard_bg");
	precache_pic("resource/hud/hud_character_bg_new_top");
	precache_pic("resource/hud/hud_character_bg_new_bottom");
	precache_pic("resource/hud/hud_weapon_bg_top");
	precache_pic("resource/hud/hud_weapon_bg_bottom");
	precache_pic("resource/hud/hud_dollar_bg");
	precache_pic("resource/hud/hud_sb_num_bottom");
	precache_pic("resource/hud/hud_sb_num_bottom_colon");
	precache_pic("resource/hud/hud_sb_num_small_red");
	precache_pic("resource/hud/hud_sb_num_small_blue");
	precache_pic("resource/hud/hud_sb_num_center");
	precache_pic("resource/hud/hud_sb_num_center_slash");
	precache_pic("resource/hud/hud_sb_num_big_red");
	precache_pic("resource/hud/hud_sb_num_big_blue");
	precache_pic("resource/hud/hud_sb_num_big_white");
}

void
HUD_Draw(void)
{
	Art_Frame();

	if (player.GetHealth() > 0) {
		Crosshair_Draw();
		Panels_DrawCharacter(g_hudFont);
		Panels_DrawWeapon(g_hudFont);
		Panels_DrawMoney();
	} else {
		/* dead players watch through the spectator bars (INFERRED, as
		   in CS) */
		SG_Draw();
	}

	/* progression and mode-specific overlays; the level and money line
	   would sit under the spectator bar's scores */
	if (player.GetHealth() > 0)
		Prog_Draw(g_hudFont);
	Prog_DrawShelter(g_hudFont);
	Prog_DrawScenario(g_hudFont);
	Mode_Draw(g_hudFont);

	Board_Draw();
	Board_DrawMessages(g_hudFont);
	KF_Draw(g_hudFont);
	BM_Draw();
	TM_Draw();
	CM_Draw();
	PTN_Draw();
	VK_Draw();
	SHH_Draw();
}

void
HUD_DrawSpectator(void)
{
	Art_Frame();
	SG_Draw();
	Board_Draw();
	Board_DrawMessages(g_hudFont);
	KF_Draw(g_hudFont);
	TM_Draw();
	CM_Draw();
	VK_Draw();
	SHH_Draw();
}

void
HUD_WeaponSwitched(string weaponName)
{
}

/* input for the HUD's menus (Nuclide HUDProgs_InputEvent); the mouse
   arrives in screen units and is kept in layout space */
bool
HUD_InputEvent(float evType, float key, float chr, float dev, vector mouse)
{
	if (g_hs > 0)
		g_resMouse = (mouse - g_hmins) / g_hs;

	if (SHH_Input(evType, key, chr, dev))
		return true;
	if (VK_Input(evType, key, chr, dev))
		return true;
	if (PTN_Input(evType, key, chr, dev))
		return true;
	if (TM_Input(evType, key, chr, dev))
		return true;
	if (CM_Input(evType, key, chr, dev))
		return true;
	return BM_Input(evType, key, chr, dev);
}

bool
HUD_WantsCursor(void)
{
	return g_bmOpen || g_tmOpen || g_cmOpen || g_ptnOpen || g_vkCallOpen || SHH_WantsCursor();
}
