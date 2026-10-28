/* The bottom of the HUD: character panel (health, armour, portrait),
   weapon panel (ammo) and the money panel.

   Everything here follows client.dll's "new" HUD, the one CSO draws in
   every mode except 11, 18, 21, 25, 38 and 39 (those keep the classic
   CS 1.6 HUD; the switch is in each VidInit). SOURCE-CONFIRMED unless
   marked otherwise:

   - panel art and the portrait: the HUD panel's paint, FUN_02aa30e0
   - health: CHudHealth::Draw FUN_02a41bf0
   - armour: CHudBattery::Draw FUN_02a25960
   - ammo: CHudAmmo::Draw FUN_02a1bb00
   - money: CHudAccountBalance::Draw FUN_02a1b3b0
   - numbers: DrawHudNumber FUN_02a6ff50 with hud.txt number_N_new
     (17x22, from FUN_02a5a470), the money with dollarNum_N_new (13x18)

   CSO places all of this in screen pixels from the bottom-left and
   bottom-right corners. Our layout is 1024x768 scaled by the screen
   height (art.qc), so at 1024x768 the two are the same pixel for pixel.

   The HUD colour of the new HUD is white at alpha 255 (FUN_02a5a470 sets
   0xffffff / 255 there; the classic HUD has 0xffa000 / 100). Sprites are
   drawn additively with the colour scaled by alpha. */

#define HUD_WHITE [255, 255, 255]
#define HUD_GREY [75, 75, 75]
/* m_iFontHeight: the height of the number sprites (INFERRED: GoldSrc
   takes it from the number_0 sprite; number_0_new is 22 high) */
#define HUD_FONTH 22

/* --- character panel -------------------------------------------------------- */

/* resource/Hud/Portrait/hud_<name>. client.dll builds the name from the
   character: humans by the character (FUN_02aa5380), zombies as the
   zombie class's name + "zombie" + "_host" for an origin zombie
   (FUN_02aa5d10: "normal" ... "_host", "_trans"). The human files are
   named after the player models (hud_gign, hud_leet, OBSERVED), so the
   model's name is used; our two zombie models map to the normal zombie
   (INFERRED). The server publishes the model as userinfo csnz_model
   (src/rules/common/csnz_hudinfo.qc). */
string
Panels_Portrait(void)
{
	string m = getplayerkeyvalue(player_localnum, "csnz_model");

	if (m == "")
		return "";
	if (m == "zombi_origin")
		m = "normalzombie";
	else if (m == "zombi_host")
		m = "normalzombie_host";
	m = strcat("resource/hud/portrait/hud_", m);

	if (drawgetimagesize(m)[0] <= 0)
		return "";

	return m;
}

void
Panels_DrawCharacter(font_s fnt)
{
	float H = HUD_H;
	float hp = max(0, player.GetHealth());
	float ap = max(0, player.GetArmor());
	vector col;

	/* 288x85 at (0, H-93) with the surface colour 255,255,255,225; mode
	   51 has none. The top layer's condition depends on the player's
	   inventory slots 2 and 3; drawn always here (INFERRED). */
	if (HUD_ModeId() != 51) {
		Art_PicRect([0, H - 93], [288, 85], "resource/hud/hud_character_bg_new_bottom", 225);
		Art_PicRect([0, H - 93], [288, 85], "resource/hud/hud_character_bg_new_top", 225);

		/* portrait 99x116 at (0, H-156); a square image is cut on the left
		   to 99:116 (u0 = 1 - 99/116) */
		string por = Panels_Portrait();
		if (por != "") {
			vector ps = drawgetimagesize(por);
			float u0 = (ps[0] == ps[1]) ? 1 - (ps[1] * 99) / (ps[0] * 116) : 0;
			drawsubpic(Art_Pos([0, H - 156]), [99, 116] * g_hs, por,
				[u0, 0], [1 - u0, 1], [1,1,1], 225 / 255, 0);
		}
	}

	/* health: cross_new at (9, H-34), the number from (28, H-39). Red
	   250,0,0 under 26 hp, the HUD colour otherwise. */
	col = (hp < 26) ? [250, 0, 0] : HUD_WHITE;
	Art_SpriteAdd([9, H - 34], "cross_new", col, 255);
	Art_HudNum([28, H - 39], "number_", hp, col, 255, true);

	/* armour: suit_empty_new at (160, H-36) at alpha 100, then
	   suit_full_new over it with the top (100 - armour)% cut off; the
	   number from (179, H-39). The icon sits h/6 above H-34. */
	float ih = Art_SpriteSize("suit_full_new")[1];
	float iy = H - 34 - floor(ih / 6);
	Art_SpriteAdd([160, iy], "suit_empty_new", HUD_WHITE, 100);
	Art_SpriteCutTop([160, iy], "suit_full_new", floor((100 - min(ap, 100)) * 0.01 * ih), HUD_WHITE, 255);
	Art_HudNum([179, H - 39], "number_", ap, HUD_WHITE, 255, true);
}

/* --- weapon panel ----------------------------------------------------------- */

string Loc(string token, string fallback); /* scores.qc */

/* the sprites/weapon_<name>.txt name for the active weapon. The HUD
   progs cannot read the weapon's decl name (OBSERVED: its classname is
   "ncWeapon"), so it comes from the title our decls give
   (decls/def/cs_weapons.def inv_name): lower case, letters and digits
   only, and three that the CS files name differently. */
string
Panels_WeaponName(void)
{
	string t = strtolower(weapon.GetTitle());
	string n = "";

	for (int i = 0i; i < strlen(t); i++) {
		float c = str2chr(t, i);
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))
			n = strcat(n, chr2str(c));
	}

	switch (n) {
	case "deserteagle": return "deagle";
	case "dualelites": return "elite";
	case "mp5": return "mp5navy";
	}
	return n;
}

void
Panels_DrawWeapon(font_s fnt)
{
	float H = HUD_H;

	/* 288x76 at (W-288, H-84), colour 255,255,255,225 */
	Art_PicRect([-288, H - 84], [288, 76], "resource/hud/hud_weapon_bg_bottom", 225);
	Art_PicRect([-288, H - 84], [288, 76], "resource/hud/hud_weapon_bg_top", 225);

	/* the knife and anything else without ammo: nothing */
	if (!weapon.AmmoRequired())
		return;

	string icon = Art_WeaponAmmoIcon(Panels_WeaponName());
	float iconW = (icon != "") ? Art_SpriteSize(icon)[0] : 24;
	float clip = weapon.GetClip();
	float reserve = weapon.GetAmmo1();

	if (weapon.GetClipSize() > 0) {
		/* clip right-aligned to W-110, a 2 px grey bar at W-106, the
		   reserve right-aligned to W-10-iconW */
		Art_HudNum([-110, H - 39], "number_", clip, HUD_WHITE, 255, false);
		Art_FillAdd([-106, H - 39], [2, HUD_FONTH], HUD_GREY, 255);
		Art_HudNum([-10 - iconW, H - 39], "number_", reserve, HUD_WHITE, 255, false);
	} else {
		/* no clip (grenades): the count in three places from
		   W - 4*digit - iconW */
		float dw = Art_SpriteSize("number_0_new")[0];	/* INFERRED: the original uses a global sprite index here */
		Art_HudNum3([-4 * dw - iconW, H - 39], "number_", reserve, HUD_WHITE, 255);
	}

	/* the ammo icon at (W-8-iconW, H-41) */
	if (icon != "")
		Art_SpriteAdd([-8 - iconW, H - 41], icon, HUD_WHITE, 255);

	/* secondary ammo right-aligned to W-208, colour 114,197,255 (the
	   ammo2 icon is not drawn: Nuclide has no ammo type per weapon) */
	if (weapon.UsesSecondaryAmmo() && weapon.GetAmmo2() > 0)
		Art_HudNum([-208, H - 39], "number_", weapon.GetAmmo2(), [114, 197, 255], 255, false);
}

/* --- weapon box ------------------------------------------------------------- */

/* The current weapon's box above the ammo panel, FUN_02a1ebf0 (called
   from CHudAmmo::Draw for the new HUD; not in mode 16, nor in mode 37
   with the zombie knife). Its list starts with the current weapon and,
   while a slot is being picked, adds that slot's weapons above it; only
   the current weapon's entry is drawn here (the selection list is not
   ported yet).

   - box: x = W-174, bottom H-88, 170x61 (the hud.txt sprite
     weapon_selection_new, 640hud8 0 133 170 61, set by VidInit
     FUN_02a233d0); the vgui image resource/Hud/hud_weapon_off_bg
     (loaded by CHudAmmo::Init FUN_02a1fbf0) at 255,255,255,225
   - the weapon's "weapon" sprite from sprites/weapon_<name>.txt at
     (x, top+15), HUD colour at 0xc0; 255,16,16 at 0x80 when the weapon
     uses ammo and has none in the clip or the reserve
   - the reserve bar, FUN_02a1d240/FUN_02a1d300: at (x+10, top+18),
     20x4 (DAT_039f133c/DAT_039f1338 at 640 wide and up), FillRGBA 0,160,0
     for reserve/max, the rest in the HUD colour at 0x80; not drawn with
     no reserve. The second bar (ammo2) is left out: Nuclide has no max
     for it here.
   - the weapon's localized name at (x+8, top+4), colour 1,1,1. The font
     (the text drawer's +0x1c) is not traced, so the HUD font is used
     (INFERRED). */
void
Panels_DrawWeaponBox(font_s fnt)
{
	float H = HUD_H;
	float modeId = HUD_ModeId();
	string wname = Panels_WeaponName();

	if (modeId == 16 || wname == "")
		return;

	float x = -174;
	float top = H - 88 - 61;

	Art_PicRect([x, top], [170, 61], "resource/hud/hud_weapon_off_bg", 225);

	vector col = HUD_WHITE;
	float a = 0xc0;
	if (weapon.AmmoRequired() && weapon.GetClip() < 1 && weapon.GetAmmo1() <= 0) {
		col = [255, 16, 16];
		a = 0x80;
	}

	string spr = Art_WeaponSprite(wname, "weapon");
	if (spr != "")
		Art_SpriteAdd([x, top + 15], spr, col, a);

	float maxAmmo = weapon.MaxAmmo();
	float reserve = weapon.GetAmmo1();
	if (weapon.AmmoRequired() && reserve > 0 && maxAmmo > 0) {
		float bw = 20;
		float fw = max(1, floor(bw * min(1, reserve / maxAmmo)));
		Art_FillAdd([x + 10, top + 18], [fw, 4], [0, 160, 0], 255);
		Art_FillAdd([x + 10 + fw, top + 18], [bw - fw, 4], HUD_WHITE, 0x80);
	}

	string title = weapon.GetTitle();
	if (substring(title, 0, 1) == "#")
		title = Loc(substring(title, 1, -1), title);
	if (title != "")
		draw.Text(Art_Pos([x + 8, top + 4]), title, fnt);
}

/* --- flashlight ------------------------------------------------------------- */

/* CHudFlashlight::Draw FUN_02a3ae60 (sprites from VidInit FUN_02a3b2a0:
   flash_empty, flash_full, flash_beam). New HUD: flash_empty at
   (W-54, H-82); when on, flash_beam right of it at x+32; flash_full over
   the empty one with its left (1 - battery) * width cut off. HUD white,
   alpha 225 when on and 100 when off; red 255,16,16 under 20% battery.

   Nuclide has no flashlight or battery (mp_flashlight is only a cvar in
   server/defs.h), so this always shows the off state with a full battery
   (UNRESOLVED until a flashlight exists). */
bool
Panels_FlashlightOn(void)
{
	return false;
}

void
Panels_DrawFlashlight(void)
{
	float H = HUD_H;
	float battery = 1;
	bool on = Panels_FlashlightOn();
	vector col = (battery < 0.2) ? [255, 16, 16] : HUD_WHITE;
	float a = on ? 225 : 100;
	vector p = [-54, H - 82];

	Art_SpriteAdd(p, "flash_empty", col, a);
	if (on)
		Art_SpriteAdd(p + [32, 0], "flash_beam", col, a);

	float w = Art_SpriteSize("flash_full")[0];
	Art_SpriteCutLeft(p, "flash_full", floor((1 - battery) * w), col, a);
}

/* --- money ------------------------------------------------------------------ */

/* The modes whose HUD shows money (SOURCE-CONFIRMED switch at the top of
   FUN_02a1b3b0 and in FUN_02aa30e0). Other modes show it only when a
   server flag (vtable 0x674 with 8) is set, which is not traced
   (UNRESOLVED). */
bool
Panels_MoneyMode(float modeId)
{
	switch (modeId) {
	case 0: case 3: case 6: case 7: case 15: case 16: case 17:
	case 26: case 28: case 31: case 42: case 45: case 48: case 50:
	case 52: case 55: case 60:
		return true;
	}
	return false;
}

float g_moneyLast;
float g_moneyDelta;
float g_moneyFade;	/* 5 s colour fade after a change */
float g_moneyDeltaFade;	/* 2.5 s for the +/- line */
float g_moneyFrame;

void
Panels_DrawMoney(void)
{
	/* csnz_cash is the round money (CS_SetMoney); csnz_money is
	   overwritten by the progression money */
	float money = floor(stof(getplayerkeyvalue(player_localnum, "csnz_cash")));
	float modeId = HUD_ModeId();
	float dt = time - g_moneyFrame;
	vector col = HUD_WHITE;
	float a = 255;

	g_moneyFrame = time;
	if (dt < 0 || dt > 1)
		dt = 0;

	if (money != g_moneyLast) {
		g_moneyDelta = money - g_moneyLast;
		g_moneyLast = money;
		g_moneyFade = 5;
		g_moneyDeltaFade = 2.5;
	}

	if (!Panels_MoneyMode(modeId))
		return;

	/* y 159, or 216 with DAT_03b607c8 set (not traced, INFERRED off);
	   +56 in mode 16, +41 in mode 15 and the PVE modes */
	float y = 0x9f;
	if (modeId == 16)
		y += 0x38;
	if (modeId == 15)
		y += 0x29;

	/* hud_dollar_bg 191x28 five pixels higher, colour 255,255,255,225 */
	Art_PicRect([0, y - 5], [191, 28], "resource/hud/hud_dollar_bg", 225);

	/* after a change the colour goes from green 180,225,92 (gain) or red
	   231,26,26 (loss) back to white over 5 s, alpha 100 + 128 * t/5 */
	if (g_moneyFade > 0) {
		g_moneyFade = max(0, g_moneyFade - dt);
		vector flash = (g_moneyDelta > 0) ? [180, 225, 92] : [231, 26, 26];
		float k = g_moneyFade / 5;
		col = flash * k + HUD_WHITE * (1 - k);
		a = min(255, (g_moneyFade / 5) * 128 + 100);
		if (g_moneyFade <= 0)
			a = 100;
	}

	/* dollar_new right before x 69, then "%5i" in dollarNum_N_new: five
	   13 px places from x 69, spaces advance but draw nothing */
	float x = 69;
	Art_SpriteAdd([x - Art_SpriteSize("dollar_new")[0], y], "dollar_new", col, a);
	string s = sprintf("%i", (int)money);
	float cw = Art_SpriteSize("dollarNum_0_new")[0];
	x += max(0, 5 - strlen(s)) * cw;
	for (int i = 0i; i < strlen(s); i++) {
		Art_SpriteAdd([x, y], strcat("dollarNum_", substring(s, i, 1), "_new"), col, a);
		x += cw;
	}

	/* the +/- line 1.5 font heights below, fading over 2.5 s */
	if (g_moneyDeltaFade > 0 && g_moneyDelta != 0) {
		g_moneyDeltaFade = max(0, g_moneyDeltaFade - dt);
		float da = (g_moneyDeltaFade / 2.5) * 255;
		vector dc = (g_moneyDelta > 0) ? [180, 225, 92] : [231, 26, 26];
		float dy = y + HUD_FONTH / 2 + HUD_FONTH;
		string sign = (g_moneyDelta > 0) ? "plus_new" : "minus_new";
		Art_SpriteAdd([69 - Art_SpriteSize(sign)[0], dy], sign, dc, da);
		s = sprintf("%i", (int)fabs(g_moneyDelta));
		x = 69 + max(0, 5 - strlen(s)) * cw;
		for (int i = 0i; i < strlen(s); i++) {
			Art_SpriteAdd([x, dy], strcat("dollarNum_", substring(s, i, 1), "_new"), dc, da);
			x += cw;
		}
	}
}
