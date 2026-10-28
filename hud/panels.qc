/* Player panels at the bottom corners.

   Character panel (bottom left): resource/hud/hud_character_bg_new_bottom
   under hud_character_bg_new_top, both 288x85 (OBSERVED). Regions measured
   from the art: a box at the left (x 0-85, y 0-46), a lighter strip to its
   right (x 85-270, y 0-46) and a band along the bottom (y 46-85).
   Weapon panel (bottom right): hud_weapon_bg_bottom under
   hud_weapon_bg_top, 288x76 (OBSERVED): a strip at the top right (x 92-288,
   y 0-36) and a band below (y 36-76).

   Contents and their places are INFERRED: health and armour in the
   character band with the hud.txt icons cross_new and suit_full_new (16x16,
   the size of hud_character_num's digits); the player's name in the strip;
   clip and reserve ammo in the weapon band, the weapon's name in its
   strip. The left box probably holds a portrait (client.dll names
   resource/Hud/Portrait/hud_*), which is not drawn: UNRESOLVED. */

void
Panels_DrawCharacter(font_s fnt)
{
	float y0 = HUD_H - 85;
	float x;
	string hp = sprintf("%i", (int)max(0, player.GetHealth()));
	string ap = sprintf("%i", (int)max(0, player.GetArmor()));

	Art_Pic([0, y0], "resource/hud/hud_character_bg_new_bottom", 1.0f);
	Art_Pic([0, y0], "resource/hud/hud_character_bg_new_top", 1.0f);

	draw.Text(Art_Pos([100, y0 + 16]), getplayerkeyvalue(player_localnum, "name"), fnt);

	Art_Sprite([16, y0 + 58], "cross_new", [1,1,1], 1.0f);
	x = Art_Num([38, y0 + 58], NUM_CHAR, hp, [1,1,1], 1.0f);
	Art_Sprite([max(x + 16, 110), y0 + 58], "suit_full_new", [1,1,1], 1.0f);
	Art_Num([max(x + 16, 110) + 22, y0 + 58], NUM_CHAR, ap, [1,1,1], 1.0f);
}

string Loc(string token, string fallback); /* scores.qc */

void
Panels_DrawWeapon(font_s fnt)
{
	float y0 = HUD_H - 76;
	string title = weapon.GetTitle();

	if (substring(title, 0, 1) == "#")
		title = Loc(substring(title, 1, -1), title);
	vector p;

	Art_Pic([-288, y0], "resource/hud/hud_weapon_bg_bottom", 1.0f);
	Art_Pic([-288, y0], "resource/hud/hud_weapon_bg_top", 1.0f);

	if (title != "") {
		p = Art_Pos([-18, y0 + 12]);
		draw.RText(p, title, fnt);
	}

	if (!weapon.AmmoRequired())
		return;

	string clip = sprintf("%i", weapon.GetClip());
	string reserve = sprintf("%i", weapon.GetAmmo1());

	Art_NumR([-18, y0 + 48], NUM_CHAR, reserve, [1,1,1], 1.0f);
	float rx = -18 - Art_NumWidth(NUM_CHAR, reserve) - 14;
	Art_Pic([rx, y0 + 49], "resource/hud/hud_sb_num_center_slash", 1.0f);
	Art_NumR([rx - 6, y0 + 48], NUM_CHAR, clip, [1,1,1], 1.0f);
}
