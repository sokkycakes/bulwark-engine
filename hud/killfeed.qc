/* Kill feed, top right.

   Art (OBSERVED files): sprites/hud.txt "d_<weapon>" icons (16 px tall),
   d_headshot, and resource/hud/deathnotice/{kill,death}bg_{left,center,
   right}_new (3+1+3 x 20). Which entries get a background is INFERRED: the
   blue kill background when you are the killer, the red death background
   when you are the victim, none otherwise (Counter-Strike 1.6 highlights
   the same two cases, STOCK-CS). Position, line spacing and the 6 s life
   are STOCK-CS approximations; capture needed. */

#define KF_MAX 5i
#define KF_TIME 6.0f
#define KF_LINE 22

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

	drawpic(Art_Pos(p), strcat(base, "bg_left_new"), [3, 20] * g_hs, [1,1,1], 1.0f, 0);
	drawpic(Art_Pos(p + [3, 0]), strcat(base, "bg_center_new"), [w - 6, 20] * g_hs, [1,1,1], 1.0f, 0);
	drawpic(Art_Pos(p + [w - 3, 0]), strcat(base, "bg_right_new"), [3, 20] * g_hs, [1,1,1], 1.0f, 0);
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

	float y = 90;
	float th = font.GetHeight(fnt) / g_hs;

	for (int i = 0i; i < g_kfCount; i++) {
		float wa = font.StringWidth(g_kfAttacker[i], false, fnt) / g_hs;
		float wv = font.StringWidth(g_kfVictim[i], false, fnt) / g_hs;
		string icon = g_kfIcon[i];
		float wIcon = (icon != "") ? Art_SpriteSize(icon)[0] : font.StringWidth("[x]", false, fnt) / g_hs;
		float wi = wIcon + (g_kfHead[i] ? 40 : 0);

		float w = 8 + (wa > 0 ? wa + 6 : 0) + wi + 6 + wv + 8;
		float x = -16 - w;

		if (g_kfVictim[i] == me && me != "")
			KF_Background([x, y], w, "death");
		else if (g_kfAttacker[i] == me && me != "")
			KF_Background([x, y], w, "kill");

		x += 8;
		if (wa > 0) {
			draw.Text(Art_Pos([x, y + (20 - th) / 2]), g_kfAttacker[i], fnt);
			x += wa + 6;
		}

		if (icon != "")
			Art_Sprite([x, y + 2], icon, [1,1,1], 1.0f);
		else
			draw.Text(Art_Pos([x, y + (20 - th) / 2]), "[x]", fnt);
		x += wIcon;

		if (g_kfHead[i]) {
			Art_Sprite([x + 2, y + 2], "d_headshot", [1,1,1], 1.0f);
			x += 40;
		}

		draw.Text(Art_Pos([x + 6, y + (20 - th) / 2]), g_kfVictim[i], fnt);
		y += KF_LINE;
	}
}
