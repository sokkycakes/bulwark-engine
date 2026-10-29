/* csnz_buymenu: expanded buy menu system with categories and weapon shop.
 *
 * Provides a text-based categorized buy interface and a persistent weapon
 * shop using the inventory and progression systems. The buy menu is triggered
 * via console commands that show weapon lists organized by category.
 *
 * Original code (ISC). DESIGN menu layout. */

/* weapon categories for buy menu */
#define BUY_PISTOL	1
#define BUY_SHOTGUN	2
#define BUY_SMG		3
#define BUY_RIFLE	4
#define BUY_MG		5
#define BUY_EQUIPMENT	6
#define BUY_MELEE	7

/* buy menu state per player */
.float bm_category;

/* show category menu */
void
BM_ShowCategories(entity p)
{
	sprint(p, PRINT_HIGH,
		"=== CSNZ WEAPON SHOP ===\n"
		"  1. Pistols\n"
		"  2. Shotguns\n"
		"  3. Sub-Machine Guns\n"
		"  4. Rifles & Snipers\n"
		"  5. Machine Guns\n"
		"  6. Equipment\n"
		"  7. Characters\n"
		"  8. Item Boxes\n"
		"Type: buy <category number>\n"
		"Direct buy: buy <weapon_name>\n"
		"========================\n");
}

/* CS stock weapons by category with prices (STOCK-CS prices) */
string bm_pistols = "weapon_glock18:400 weapon_usp:500 weapon_p228:600 weapon_deagle:650 weapon_fiveseven:750 weapon_elite:800";
string bm_shotguns = "weapon_m3:1700 weapon_xm1014:3000";
string bm_smgs = "weapon_mac10:1400 weapon_tmp:1250 weapon_mp5:1500 weapon_ump45:1700 weapon_p90:2350";
string bm_rifles = "weapon_galil:2000 weapon_famas:2250 weapon_ak47:2500 weapon_m4a1:3100 weapon_sg552:3500 weapon_aug:3500 weapon_scout:2750 weapon_awp:4750 weapon_g3sg1:5000 weapon_sg550:4200";
string bm_mgs = "weapon_m249:5750";
string bm_equip = "kevlar:650 kevlar_helmet:1000 defuser:200 hegrenade:300 flashbang:200 smokegrenade:300";

void
BM_ShowList(entity p, string list, string catName)
{
	sprint(p, PRINT_HIGH, sprintf("=== %s ===\n", catName));

	float n = tokenize(list);
	for (float i = 0; i < n; i++) {
		tokenize(list);
		string entry = argv(i);
		/* split on : */
		float pos = strstrofs(entry, ":", 0);
		string wname = substring(entry, 0, pos);
		string price = substring(entry, pos + 1, -1);
		string owned = INV_HasItem(p, wname) ? " [OWNED]" : "";
		sprint(p, PRINT_HIGH, sprintf("  %s - $%s%s\n", wname, price, owned));
	}
	sprint(p, PRINT_HIGH, "Type: buy <weapon_name>\n");
}

/* expanded buy command handler */
bool
BM_Command(entity p, string cmd)
{
	tokenize(cmd);
	string c = argv(0);

	if (c != "buy" && c != "csnz_shop")
		return false;

	string arg = argv(1);

	if (arg == "" || arg == "menu") {
		BM_ShowCategories(p);
		return true;
	}

	/* category numbers */
	float cat = stof(arg);
	if (cat == 1) { BM_ShowList(p, bm_pistols, "PISTOLS"); return true; }
	if (cat == 2) { BM_ShowList(p, bm_shotguns, "SHOTGUNS"); return true; }
	if (cat == 3) { BM_ShowList(p, bm_smgs, "SUB-MACHINE GUNS"); return true; }
	if (cat == 4) { BM_ShowList(p, bm_rifles, "RIFLES & SNIPERS"); return true; }
	if (cat == 5) { BM_ShowList(p, bm_mgs, "MACHINE GUNS"); return true; }
	if (cat == 6) { BM_ShowList(p, bm_equip, "EQUIPMENT"); return true; }
	if (cat == 7) {
		sprint(p, PRINT_HIGH,
			"=== CHARACTERS ===\n"
			"  csm_char_fernando (free)\n"
			"  csm_char_phoenix (free)\n"
			"  csm_char_blair - $30000\n"
			"  csm_char_yuri - $30000\n"
			"  csm_char_lycan - $30000\n"
			"  csm_char_hunter - $30000\n"
			"  csm_char_medic - $30000\n"
			"  csm_char_engineer - $30000\n"
			"  csm_char_commando - $30000\n"
			"  csm_char_assassin - $50000\n"
			"  csm_char_shadow - $50000\n"
			"  csm_char_guardian - $50000\n"
			"  csm_char_berserker - $50000\n"
			"Type: csnz_buy <character_name>\n");
		return true;
	}
	if (cat == 8) {
		sprint(p, PRINT_HIGH,
			"=== ITEM BOXES ===\n"
			"  csnz_daily - Free daily box\n"
			"  csnz_openbox basic - $1000\n"
			"  csnz_openbox standard - $5000\n"
			"  csnz_openbox premium - $10000\n"
			"  csnz_openbox elite - $25000\n");
		return true;
	}

	/* direct weapon buy by name - use the existing CS_Buy for stock weapons */
	/* for CSNZ weapons, handle via inventory */
	if (substring(arg, 0, 7) == "weapon_") {
		/* CSNZ weapon from inventory */
		if (INV_HasItem(p, arg)) {
			ents.Input(p, "GiveItem", arg, world);
			CS_FillAmmo(p);
			ents.Input(p, "SwitchToWeapon", arg, world);
			sprint(p, PRINT_HIGH, sprintf("Equipped %s\n", arg));
		} else {
			sprint(p, PRINT_HIGH, sprintf("You don't own %s. Buy it from the shop first.\n", arg));
		}
		return true;
	}

	/* pass through to CS_Buy for stock weapon names (ak47, m4a1, etc.) */
	return false;
}
