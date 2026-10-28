/* Mode overlays: values the rules already publish that nothing drew
   (handoff H2). Text and plain bars on the right, below the progression
   lines; DESIGN layout. The CSO art and layout of these HUDs (CHudSnowman,
   CHudZombieZ, CZombieChangeTimePanel, ...) are not ported (H7). */

float g_modeY;

void
Mode_Line(string txt, vector col, font_s fnt)
{
	draw.RText_RGB(Art_Pos([HUD_W - 10, g_modeY]), txt, col, fnt);
	g_modeY += 16;
}

/* a bar right-aligned with the text lines */
void
Mode_Bar(float frac, vector col)
{
	float w = 160, h = 6;
	vector p = [HUD_W - 10 - w, g_modeY];

	frac = bound(0, frac, 1);
	drawfill(Art_Pos(p), [w, h] * g_hs, [0.2, 0.2, 0.2], 0.5);
	drawfill(Art_Pos(p), [w * frac, h] * g_hs, col, 0.8);
	g_modeY += h + 6;
}

vector
Mode_HealthColour(float frac)
{
	if (frac > 0.5)
		return [0.3, 1.0, 0.3];
	if (frac > 0.25)
		return [1.0, 1.0, 0.3];
	return [1.0, 0.3, 0.3];
}

void
Mode_Draw(font_s fnt)
{
	float mode = HUD_ModeId();
	float me = player_localnum;

	g_modeY = 88;

	/* Snowman core sub-type: both cores (fun.qc SM_CoreRoundStart) */
	if (mode == 27 && serverkey("csnz_core1_hp") != "") {
		float cmax = max(1, stof(serverkey("csnz_core_max")));
		float c1 = stof(serverkey("csnz_core1_hp")), c2 = stof(serverkey("csnz_core2_hp"));
		Mode_Line(sprintf("TR core: %d", c1), [1, 0.5, 0.5], fnt);
		Mode_Bar(c1 / cmax, [1, 0.3, 0.3]);
		Mode_Line(sprintf("CT core: %d", c2), [0.6, 0.8, 1], fnt);
		Mode_Bar(c2 / cmax, [0.3, 0.5, 1]);
	}

	/* Shelter Team: the other team's shelter (shelter.qc, csnz_shelter_hp2);
	   the own shelter is Prog_DrawShelter's centre bar */
	if ((mode == 28 || mode == 52) && serverkey("csnz_shelter_hp2") != "") {
		float shMax = max(1, stof(serverkey("csnz_shelter_max")));
		float sh2 = stof(serverkey("csnz_shelter_hp2"));
		Mode_Line(sprintf("Team 1 shelter: %d / %d", sh2, shMax), [1, 1, 1], fnt);
		Mode_Bar(sh2 / shMax, Mode_HealthColour(sh2 / shMax));
	}

	/* Zombie Z: mutations (zombie.qc ZV_Command; OBSERVED tip "mutate using
	   the 'V' key", the client sends cmd csnz_mutate) */
	if (mode == 45 || mode == 55) {
		float muts = stof(getplayerkeyvalue(me, "csnz_mutations"));
		if (muts > 0)
			Mode_Line(sprintf("Mutations: %d", muts), [0.8, 1, 0.6], fnt);
		if (getplayerkeyvalue(me, "csnz_canmutate") == "1")
			Mode_Line("Mutation ready: press V", [1, 0.9, 0.3], fnt);
	}

	/* Zombie Darkness day and night (zombie.qc, csnz_night) */
	if (mode == 29)
		Mode_Line(serverkey("csnz_night") == "1" ? "Night" : "Day",
			serverkey("csnz_night") == "1" ? [0.6, 0.6, 1] : [1, 0.9, 0.6], fnt);

	/* Bazooka: hits and the 3 s combo (basic.qc CSM_BazookaDamaged) */
	if (mode == 21) {
		float hits = stof(getplayerkeyvalue(me, "csnz_hits"));
		float combo = stof(getplayerkeyvalue(me, "csnz_combo"));
		if (hits > 0)
			Mode_Line(sprintf("Hits: %d", hits), [1, 1, 1], fnt);
		if (combo > 1)
			Mode_Line(sprintf("Combo x%d", combo), [1, 0.8, 0.3], fnt);
	}
}
