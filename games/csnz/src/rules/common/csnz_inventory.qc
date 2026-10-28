/* csnz_inventory: local persistent inventory system.
 *
 * Players own weapons, characters, and items. Ownership is stored per-player
 * in data/csnz/players/<id>_inventory.txt. Items are identified by their
 * entityDef name (e.g. "weapon_ak47", "csm_char_phoenix").
 *
 * The buy menu checks inventory ownership. Players start with a set of
 * default items. New items are acquired through the shop (spending money
 * from the progression system), match rewards, or admin commands.
 *
 * Original code (ISC). DESIGN system - CSNZ's real inventory was
 * server-authoritative. */

/* max items a player can own */
#define INV_MAX_ITEMS 512

/* item categories */
#define INV_CAT_WEAPON		0
#define INV_CAT_CHARACTER	1
#define INV_CAT_EQUIPMENT	2
#define INV_CAT_COSMETIC	3
#define INV_CAT_CONSUMABLE	4

/* per-player inventory stored as a delimited string
   (FTEQW string handling is more practical than arrays for persistence) */
.string inv_items;		/* space-separated list of owned item IDs */
.string inv_equipped_char;	/* currently equipped character class */
.string inv_equipped_primary;	/* equipped primary weapon */
.string inv_equipped_secondary;	/* equipped secondary weapon */

/* --- default inventory for new players --- */
string INV_DEFAULT_ITEMS = "weapon_knife weapon_usp weapon_glock18 weapon_m4a1 weapon_ak47 weapon_mp5 weapon_m3 weapon_hegrenade csm_char_fernando csm_char_phoenix";

/* --- file I/O --- */

string
INV_SavePath(entity p)
{
	string id = p.netname;
	string safe = "";
	for (float i = 0; i < strlen(id) && i < 32; i++) {
		float ch = str2chr(id, i);
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
			safe = strcat(safe, chr2str(ch));
	}
	if (safe == "") safe = "default";
	return sprintf("data/csnz/players/%s_inventory.txt", safe);
}

void
INV_Load(entity p)
{
	string path = INV_SavePath(p);
	filestream f = fopen(path, FILE_READ);

	if (f < 0) {
		/* new player: give defaults */
		p.inv_items = INV_DEFAULT_ITEMS;
		p.inv_equipped_char = "csm_char_fernando";
		p.inv_equipped_primary = "weapon_m4a1";
		p.inv_equipped_secondary = "weapon_usp";
		print(sprintf("inventory: new player %s, giving defaults\n", p.netname));
		return;
	}

	string line;
	while ((line = fgets(f))) {
		float n = tokenize(line);
		if (n < 2) continue;
		string key = argv(0);
		string val = substring(line, strlen(key) + 1, -1);

		if (key == "items") p.inv_items = val;
		else if (key == "char") p.inv_equipped_char = val;
		else if (key == "primary") p.inv_equipped_primary = val;
		else if (key == "secondary") p.inv_equipped_secondary = val;
	}
	fclose(f);
	print(sprintf("inventory: loaded %s (%d items)\n",
		p.netname, (float)INV_Count(p)));
}

void
INV_Save(entity p)
{
	if (clienttype(p) == CLIENTTYPE_BOT) return;

	string path = INV_SavePath(p);
	filestream f = fopen(path, FILE_WRITE);
	if (f < 0) return;

	fputs(f, sprintf("items %s\n", p.inv_items));
	fputs(f, sprintf("char %s\n", p.inv_equipped_char));
	fputs(f, sprintf("primary %s\n", p.inv_equipped_primary));
	fputs(f, sprintf("secondary %s\n", p.inv_equipped_secondary));
	fclose(f);
}

/* --- inventory queries --- */

bool
INV_HasItem(entity p, string itemId)
{
	if (!p.inv_items || p.inv_items == "")
		return false;
	/* check if itemId appears as a whole word in the space-separated list */
	float c = tokenize(p.inv_items);
	for (float i = 0; i < c; i++) {
		if (argv(i) == itemId)
			return true;
	}
	return false;
}

int
INV_Count(entity p)
{
	if (!p.inv_items || p.inv_items == "")
		return 0;
	return (int)tokenize(p.inv_items);
}

/* --- inventory modification --- */

bool
INV_AddItem(entity p, string itemId)
{
	if (INV_HasItem(p, itemId))
		return false;	/* already owned */
	if (INV_Count(p) >= INV_MAX_ITEMS)
		return false;	/* inventory full */

	if (p.inv_items == "" || !p.inv_items)
		p.inv_items = itemId;
	else
		p.inv_items = strcat(p.inv_items, " ", itemId);
	return true;
}

bool
INV_RemoveItem(entity p, string itemId)
{
	if (!INV_HasItem(p, itemId))
		return false;

	string newList = "";
	float c = tokenize(p.inv_items);
	for (float i = 0; i < c; i++) {
		if (argv(i) != itemId) {
			if (newList == "")
				newList = argv(i);
			else
				newList = strcat(newList, " ", argv(i));
		}
	}
	p.inv_items = newList;
	return true;
}

/* --- shop / purchasing --- */

/* weapon shop prices (DESIGN - approximations of CSNZ pricing tiers) */
float
INV_WeaponPrice(string weaponId)
{
	/* tier 1: basic CS weapons (free or cheap) */
	if (weaponId == "weapon_knife" || weaponId == "weapon_usp" ||
		weaponId == "weapon_glock18")
		return 0;

	/* tier 2: standard CS weapons */
	if (weaponId == "weapon_mp5" || weaponId == "weapon_m3" ||
		weaponId == "weapon_tmp" || weaponId == "weapon_mac10" ||
		weaponId == "weapon_ump45" || weaponId == "weapon_p228" ||
		weaponId == "weapon_fiveseven" || weaponId == "weapon_elite")
		return 5000;

	/* tier 3: assault rifles */
	if (weaponId == "weapon_m4a1" || weaponId == "weapon_ak47" ||
		weaponId == "weapon_famas" || weaponId == "weapon_galil" ||
		weaponId == "weapon_aug" || weaponId == "weapon_sg552")
		return 15000;

	/* tier 4: premium weapons */
	if (weaponId == "weapon_awp" || weaponId == "weapon_m249" ||
		weaponId == "weapon_g3sg1" || weaponId == "weapon_sg550" ||
		weaponId == "weapon_p90" || weaponId == "weapon_deagle" ||
		weaponId == "weapon_scout" || weaponId == "weapon_xm1014")
		return 25000;

	/* tier 5: CSNZ exclusive weapons (high price) */
	return 50000;
}

float
INV_CharacterPrice(string charId)
{
	/* base characters are free */
	if (charId == "csm_char_fernando" || charId == "csm_char_phoenix")
		return 0;
	/* standard characters */
	return 30000;
}

bool
INV_BuyItem(entity p, string itemId)
{
	if (INV_HasItem(p, itemId)) {
		sprint(p, PRINT_HIGH, "You already own this item.\n");
		return false;
	}

	float price;
	if (substring(itemId, 0, 7) == "weapon_")
		price = INV_WeaponPrice(itemId);
	else if (substring(itemId, 0, 9) == "csm_char_")
		price = INV_CharacterPrice(itemId);
	else
		price = 10000;	/* default price */

	if (!PROG_SpendMoney(p, price)) {
		sprint(p, PRINT_HIGH, sprintf("Not enough money. Need $%g, have $%g.\n",
			price, p.prog_money));
		return false;
	}

	INV_AddItem(p, itemId);
	INV_Save(p);
	INV_PublishChars(p);
	sprint(p, PRINT_HIGH, sprintf("Purchased %s for $%g!\n", itemId, price));
	return true;
}

/* the owned characters and the equipped one, for the HUD's class menu
   (hud/classmenu.qc): userinfo csnz_chars and csnz_char */
void
INV_PublishChars(entity p)
{
	string chars = "";
	float n = tokenize(p.inv_items);

	for (float i = 0; i < n; i++)
		if (substring(argv(i), 0, 9) == "csm_char_")
			chars = strcat(chars, chars == "" ? "" : " ", argv(i));

	userinfo.SetString(p, "csnz_chars", chars);
	userinfo.SetString(p, "csnz_char", p.inv_equipped_char);
}

/* --- equip system --- */

bool
INV_EquipCharacter(entity p, string charId)
{
	if (!INV_HasItem(p, charId)) {
		sprint(p, PRINT_HIGH, "You don't own this character.\n");
		return false;
	}
	p.inv_equipped_char = charId;
	INV_Save(p);
	INV_PublishChars(p);
	sprint(p, PRINT_HIGH, sprintf("Equipped character: %s\n", charId));
	return true;
}

bool
INV_EquipWeapon(entity p, string weaponId, bool primary)
{
	if (!INV_HasItem(p, weaponId)) {
		sprint(p, PRINT_HIGH, "You don't own this weapon.\n");
		return false;
	}
	if (primary)
		p.inv_equipped_primary = weaponId;
	else
		p.inv_equipped_secondary = weaponId;
	INV_Save(p);
	return true;
}

/* --- client commands --- */

bool
INV_Command(entity p, string cmd)
{
	tokenize(cmd);
	string c = argv(0);

	if (c == "csnz_inventory") {
		sprint(p, PRINT_HIGH, sprintf("=== Inventory (%d items) ===\n", (float)INV_Count(p)));
		float n = tokenize(p.inv_items);
		for (float i = 0; i < n; i++)
			sprint(p, PRINT_HIGH, sprintf("  %s\n", argv(i)));
		sprint(p, PRINT_HIGH, sprintf("Character: %s\n", p.inv_equipped_char));
		sprint(p, PRINT_HIGH, sprintf("Primary: %s\n", p.inv_equipped_primary));
		sprint(p, PRINT_HIGH, sprintf("Secondary: %s\n", p.inv_equipped_secondary));
		return true;
	}

	if (c == "csnz_buy") {
		if (stof(argv(1)) == 0) {
			sprint(p, PRINT_HIGH, "Usage: csnz_buy <item_id>\n");
			return true;
		}
		INV_BuyItem(p, argv(1));
		return true;
	}

	if (c == "csnz_equip_char") {
		INV_EquipCharacter(p, argv(1));
		return true;
	}

	if (c == "csnz_equip_primary") {
		INV_EquipWeapon(p, argv(1), true);
		return true;
	}

	if (c == "csnz_equip_secondary") {
		INV_EquipWeapon(p, argv(1), false);
		return true;
	}

	/* give command (admin/debug) */
	if (c == "csnz_give") {
		INV_AddItem(p, argv(1));
		INV_Save(p);
		INV_PublishChars(p);
		sprint(p, PRINT_HIGH, sprintf("Added %s to inventory.\n", argv(1)));
		return true;
	}

	return false;
}

/* --- hooks for match framework --- */

/* called when a player connects: load their inventory and progression */
void
INV_PlayerConnect(entity p)
{
	PROG_Load(p);
	INV_Load(p);

	/* publish initial state to HUD */
	userinfo.SetInteger(p, "csnz_level", (int)p.prog_level);
	userinfo.SetInteger(p, "csnz_rank", (int)p.prog_rank);
	userinfo.SetInteger(p, "csnz_money", (int)p.prog_money);
	INV_PublishChars(p);
}

/* called when a player disconnects: save */
void
INV_PlayerDisconnect(entity p)
{
	PROG_Save(p);
	INV_Save(p);
}
