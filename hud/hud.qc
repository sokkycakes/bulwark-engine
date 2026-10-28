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
	precache_pic("resource/hud/hud_scoreboard_bg");
	precache_pic("resource/hud/hud_character_bg_new_top");
	precache_pic("resource/hud/hud_character_bg_new_bottom");
	precache_pic("resource/hud/hud_weapon_bg_top");
	precache_pic("resource/hud/hud_weapon_bg_bottom");
}

void
HUD_Draw(void)
{
	Art_Frame();

	if (player.GetHealth() > 0) {
		Crosshair_Draw();
		Panels_DrawCharacter(g_hudFont);
		Panels_DrawWeapon(g_hudFont);
	}

	/* progression and mode-specific overlays */
	Prog_Draw(g_hudFont);
	Prog_DrawShelter(g_hudFont);
	Prog_DrawScenario(g_hudFont);

	Board_Draw();
	Board_DrawMessages(g_hudFont);
	KF_Draw(g_hudFont);
}

void
HUD_DrawSpectator(void)
{
	Art_Frame();
	Board_Draw();
	Board_DrawMessages(g_hudFont);
	KF_Draw(g_hudFont);
}

void
HUD_WeaponSwitched(string weaponName)
{
}
