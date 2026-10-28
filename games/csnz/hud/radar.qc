/* The radar, client.dll's CS-style radar draw (FUN_02a443b0, sprites set
   up in FUN_02a46fc0, projection FUN_02a45810, dots FUN_02a45530).
   SOURCE-CONFIRMED unless marked:

   - Drawn only while cl_radar_on (default "1") is set and the local player
     is alive. Not in modes 1, 4, 34, 18, 37, 15, 48, 27 or 16; three more
     gates (FUN_029f8c10, FUN_029f8c30, FUN_02821560) and an extra check
     for 32/35/36/43 aren't traced and aren't ported (UNRESOLVED).
   - The hud.txt "radar" sprite (radar640, rect forced to 0,0 128x128),
     additive with colour 25,75,25, at (0, 24); at (0, 0) in the old-HUD
     modes 11, 18, 21, 25, 38 and 39. With cl_radartype (default "0") set,
     "radaropaque" with colour 200,200,200 and holes instead.
   - Coordinates are GoldSrc screen pixels, drawn here in the HUD's
     1024x768 layout like the rest of it; sokky's 1920x1080 capture of the
     real client has the circle about 180 px across at y 35, which is 128
     and 24 scaled by 1080/768 (OBSERVED).
   - Teammates (same team name, alive, not the local player) are dots from
     the "Radar" message positions, published here as userinfo csnz_rpos
     (src/rules/common/csnz_common.qc). Projection: the offset from the
     view, rotated by the view yaw, 32 units per pixel around (64, 64),
     clamped to 2048 units; +24 down in the new HUD, before the 0-128
     bounds test.
   - Dot (FUN_02a45530, FillRGBA): size 2, colour 150,75,250, alpha 235.
     More than 128 units below: a 1-wide line 4 + |dz|/400 long (dz capped
     at 3096) with a 5x1 foot at its end; more than 128 above: a 5x1 bar on
     top of the line.
   - Not ported: the bomb carrier / VIP colour (250,0,0), tracked players
     (trackplayer, 185,20,20 at double size), flashing blips, hostages and
     bomb, the zombie-class icons of modes 11 and 13, and the Soccer ball
     and pass markers. */

var float autocvar_cl_radar_on = 1;
var float autocvar_cl_radartype = 0;

bool
Radar_Hidden(float modeId)
{
	switch (modeId) {
	case 1:
	case 4:
	case 15:
	case 16:
	case 18:
	case 27:
	case 34:
	case 37:
	case 48:
		return true;
	}
	return false;
}

bool
Radar_OldHud(float modeId)
{
	switch (modeId) {
	case 11:
	case 18:
	case 21:
	case 25:
	case 38:
	case 39:
		return true;
	}
	return false;
}

void
Radar_Dot(float x, float y, float dz, float size, vector rgb)
{
	float h;

	if (dz < -128) {
		h = size * 2 - floor(min(-dz, 3096) / -400);
		Art_FillAdd([x, y], [1, h], rgb, 235);
		Art_FillAdd([x - 2, y + h], [5, 1], rgb, 235);
		return;
	}
	if (dz > 128) {
		h = size * 2 - floor(min(dz, 3096) / -400);
		Art_FillAdd([x - 2, y], [5, 1], rgb, 235);
		Art_FillAdd([x, y + 1], [1, h - 1], rgb, 235);
		return;
	}
	Art_FillAdd([x, y], [size, size], rgb, 235);
}

void
Radar_Draw(void)
{
	float modeId = HUD_ModeId();

	if (!autocvar_cl_radar_on || Radar_Hidden(modeId))
		return;

	float top = Radar_OldHud(modeId) ? 0 : 24;
	if (autocvar_cl_radartype)
		Art_SpriteF([0, top], "radaropaque", [200, 200, 200] / 255, 1, 0);
	else
		Art_SpriteAdd([0, top], "radar", [25, 75, 25], 255);

	vector org = player.GetCameraPosition();
	float yaw = player.GetCameraAngles()[1];
	string myTeam = getplayerkeyvalue(player_localnum, "*team");

	for (float i = 0; i < 64; i++) {
		if (i == player_localnum)
			continue;
		if (getplayerkeyvalue(i, "name") == "")
			continue;
		if (getplayerkeyvalue(i, "*team") != myTeam)
			continue;
		string pos = getplayerkeyvalue(i, "csnz_rpos");
		if (pos == "")
			continue;

		vector d = stov(pos) - org;
		float ang = atan2(d[1], d[0]) * (180 / M_PI);
		float dist = sqrt(d[0] * d[0] + d[1] * d[1]);
		float th = (yaw - ang) * (M_PI / 180);
		float rx = sin(th) * dist;
		float ry = -cos(th) * dist;
		if (dist > 2048) {
			rx = rx * 2048 / dist;
			ry = ry * 2048 / dist;
		}
		float x = 64 - (int)(rx * -0.03125);
		float y = 64 - (int)(ry * -0.03125) + top;
		/* the 0..128 test comes after the +24, so in the new HUD dots
		   in the bottom 24 pixels of the circle are dropped (as in the
		   original) */
		if (x < 0 || y < 0 || x > 128 || y > 128)
			continue;
		Radar_Dot(x, y, d[2], 2, [150, 75, 250]);
	}
}
