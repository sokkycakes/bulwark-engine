/* Kill feed, top right (CHudDeathNotice: VidInit FUN_02a306f0, Init
   FUN_02a2f590, Draw FUN_02a2dcd0). SOURCE-CONFIRMED:
   - 4 lines (8 in modes 11 and 13), hud_deathnotice_time 6 s;
   - new HUD: right margin 5 and top 5 (scaled from 1024x768), line
     height 16 and gap 5 in VGUI proportional units;
   - right to left: victim, gap, weapon icon (and headshot etc., each with
     a gap), gap, attacker;
   - the background, when the local player is the killer (KillBg_*_new)
     or the victim (DeathBg_*_new): a 3 px left cap from the attacker's x
     - 3, the centre to W - margin, a 3 px right cap; y - 2 to y + 18,
     surface colour 255,255,255,220;
   - names with the console font at y + 2.
   INFERRED: proportional units are VGUI's 480-line base, so a line is
   16 * 768 / 480 = 25.6 layout px. Name colours (team colours in CS) are
   not traced; white here. */

#define KF_MAX 4i
#define KF_TIME 6.0f
#define KF_LINE 25.6
#define KF_MARGIN 5
#define KF_GAP 5

string g_kfAttacker[KF_MAX];
string g_kfVictim[KF_MAX];
string g_kfIcon[KF_MAX];
float g_kfHead[KF_MAX];
float g_kfUntil[KF_MAX];
int g_kfCount;

/* the hud.txt icon for a weapon decl: "weapon_csnz_ak47" -> "d_ak47" */
string
KF_Icon(string weaponDef)
{
	string n = strtolower(weaponDef);

	if (substring(n, 0, 7) == "weapon_")
		n = substring(n, 7, -1);
	if (substring(n, 0, 5) == "csnz_")
		n = substring(n, 5, -1);

	if (hash_get(g_hudSprites, strcat("d_", n), "", EV_STRING) != "")
		return strcat("d_", n);

	return "";
}

void
HUD_Obituary(string targetName, string attackerName, string weaponDef, string meansOfDeath)
{
	int i;

	if (g_kfCount >= KF_MAX) {
		for (i = 1i; i < KF_MAX; i++) {
			g_kfAttacker[i - 1i] = g_kfAttacker[i];
			g_kfVictim[i - 1i] = g_kfVictim[i];
			g_kfIcon[i - 1i] = g_kfIcon[i];
			g_kfHead[i - 1i] = g_kfHead[i];
			g_kfUntil[i - 1i] = g_kfUntil[i];
		}
		g_kfCount = KF_MAX - 1i;
	}

	/* a suicide or a death by the world: the victim alone, with the skull
	   (Counter-Strike 1.6 does this with d_skull, STOCK-CS) */
	string icon = KF_Icon(weaponDef);
	if (attackerName == targetName || wordInString(meansOfDeath, "self")) {
		attackerName = "";
		icon = "d_skull";
	}

	i = g_kfCount++;
	/* strings from the caller are temporary */
	g_kfAttacker[i] = strzone(attackerName);
	g_kfVictim[i] = strzone(targetName);
	g_kfIcon[i] = strzone(icon);
	g_kfHead[i] = wordInString(meansOfDeath, "headshot") ? 1 : 0;
	g_kfUntil[i] = time + KF_TIME;
}

void
KF_Background(vector p, float w, string kind)
{
	string base = strcat("resource/hud/deathnotice/", kind);

	drawpic(Art_Pos(p), strcat(base, "bg_left_new"), [3, 20] * g_hs, [1,1,1], 220 / 255, 0);
	drawpic(Art_Pos(p + [3, 0]), strcat(base, "bg_center_new"), [w - 6, 20] * g_hs, [1,1,1], 220 / 255, 0);
	drawpic(Art_Pos(p + [w - 3, 0]), strcat(base, "bg_right_new"), [3, 20] * g_hs, [1,1,1], 220 / 255, 0);
}

void
KF_Draw(font_s fnt)
{
	string me = getplayerkeyvalue(player_localnum, "name");

	/* expire from the top */
	while (g_kfCount > 0i && g_kfUntil[0] < time) {
		strunzone(g_kfAttacker[0]);
		strunzone(g_kfVictim[0]);
		strunzone(g_kfIcon[0]);
		for (int i = 1i; i < g_kfCount; i++) {
			g_kfAttacker[i - 1i] = g_kfAttacker[i];
			g_kfVictim[i - 1i] = g_kfVictim[i];
			g_kfIcon[i - 1i] = g_kfIcon[i];
			g_kfHead[i - 1i] = g_kfHead[i];
			g_kfUntil[i - 1i] = g_kfUntil[i];
		}
		g_kfCount--;
	}

	float y = KF_MARGIN;
	float th = font.GetHeight(fnt) / g_hs;
	float right = screen.HUDSize()[0] / g_hs - KF_MARGIN;

	for (int i = 0i; i < g_kfCount; i++) {
		float wa = font.StringWidth(g_kfAttacker[i], false, fnt) / g_hs;
		float wv = font.StringWidth(g_kfVictim[i], false, fnt) / g_hs;
		string icon = g_kfIcon[i];
		float wIcon = (icon != "") ? Art_SpriteSize(icon)[0] : font.StringWidth("[x]", false, fnt) / g_hs;
		float wHead = g_kfHead[i] ? Art_SpriteSize("d_headshot")[0] : 0;

		/* right to left */
		float xv = right - wv;
		float xi = xv - KF_GAP - wIcon;
		/* the extra icons (headshot) go left of the weapon icon; their
		   order is INFERRED */
		float xh = (wHead > 0) ? xi - KF_GAP - wHead : xi;
		float xa = (wa > 0) ? xh - KF_GAP - wa : xh;

		if (g_kfVictim[i] == me && me != "")
			KF_Background([xa - 3, y - 2], right + 3 - (xa - 3), "death");
		else if (g_kfAttacker[i] == me && me != "")
			KF_Background([xa - 3, y - 2], right + 3 - (xa - 3), "kill");

		if (wa > 0)
			draw.Text(Art_Pos([xa, y + 2]), g_kfAttacker[i], fnt);

		if (icon != "")
			Art_Sprite([xi, y], icon, [1,1,1], 1.0f);
		else
			draw.Text(Art_Pos([xi, y + 2]), "[x]", fnt);

		if (wHead > 0)
			Art_Sprite([xh, y], "d_headshot", [1,1,1], 1.0f);

		draw.Text(Art_Pos([xv, y + 2]), g_kfVictim[i], fnt);
		y += KF_LINE;
	}
}
