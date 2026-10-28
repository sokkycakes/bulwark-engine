/* Crosshair: Counter-Strike 1.6's four-line crosshair (STOCK-CS). CSO's
   own default crosshair is UNRESOLVED (sprites/crosshair.csv, OBSERVED,
   lists only two sprite crosshairs, "cross" and "rect"; which one a weapon
   uses is not known).

   Read from the cvars the options dialog writes: cl_crosshair_size
   (auto, small, medium, large), cl_crosshair_color ("r g b", 0-255) and
   cl_crosshair_translucent. The gap widens while moving or in the air
   (an approximation of CS's spread-driven gap: the numbers are DESIGN). */

float g_crossGap;

void
Crosshair_Draw(void)
{
	string size = cvar_string("cl_crosshair_size");
	vector rgb = stov(cvar_string("cl_crosshair_color")) / 255;
	bool add = cvar("cl_crosshair_translucent") != 0;
	float len, gapBase, want;
	vector c = screen.HUDMins() + screen.HUDSize() / 2;

	if (rgb == [0,0,0])
		rgb = [50, 250, 50] / 255;

	switch (size) {
	case "small": len = 4; break;
	case "large": len = 8; break;
	case "medium": len = 6; break;
	default: len = (screen.HUDSize()[0] >= 1024) ? 6 : 4; break;
	}
	gapBase = len;

	want = gapBase;
	if (player.IsMoving())
		want += 6;
	if (player.IsFalling())
		want += 10;
	if (player.IsCrouched())
		want -= 2;

	/* eases toward the target like CS's crosshair does (DESIGN rate) */
	g_crossGap += (want - g_crossGap) * min(1, clframetime * 10);

	float g = rint(g_crossGap);
	float flags = add ? DRAWFLAG_ADDITIVE : DRAWFLAG_NORMAL;
	float a = add ? 0.8f : 1.0f;

	c[0] = rint(c[0]);
	c[1] = rint(c[1]);
	drawfill(c + [-g - len, 0], [len, 1], rgb, a, flags);
	drawfill(c + [g + 1, 0], [len, 1], rgb, a, flags);
	drawfill(c + [0, -g - len], [1, len], rgb, a, flags);
	drawfill(c + [0, g + 1], [1, len], rgb, a, flags);
}
