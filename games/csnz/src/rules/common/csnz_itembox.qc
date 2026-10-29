/* csnz_itembox: gacha / item box / supply box system.
 *
 * Players can open item boxes that grant random weapons from weighted
 * drop tables. Box tiers range from free daily boxes to premium boxes.
 *
 * Original code (ISC). DESIGN drop rates and prices. */

/* box tier costs (DESIGN, progression money) */
#define IB_COST_BASIC		1000
#define IB_COST_STANDARD	5000
#define IB_COST_PREMIUM		10000
#define IB_COST_ELITE		25000

/* drop grade weights (out of 1000) */
#define IB_GRADE_COMMON		600
#define IB_GRADE_UNCOMMON	250
#define IB_GRADE_RARE		100
#define IB_GRADE_EPIC		40
#define IB_GRADE_LEGENDARY	10

.float ib_lastDaily;	/* timestamp of last daily box */

/* weapon pools by grade (representative sample from Item.csv data) */
/* Common (grade 1): stock CS weapons and basic CSNZ */
string ib_pool_common = "weapon_mp5 weapon_m4a1 weapon_ak47 weapon_famas weapon_galil weapon_m3 weapon_tmp weapon_mac10 weapon_ump45 weapon_p228 weapon_fiveseven weapon_elite weapon_scout weapon_sg550 weapon_sg552 weapon_aug weapon_p90 weapon_m249 weapon_anaconda weapon_k1a weapon_mp7a1 weapon_qbb95 weapon_vsk94";

/* Uncommon (grade 2) */
string ib_pool_uncommon = "weapon_deserteagleg weapon_scar weapon_xm8 weapon_svd weapon_usas12 weapon_tar21 weapon_mk48 weapon_as50 weapon_wa2000 weapon_an94 weapon_aw50 weapon_arx160 weapon_hk416 weapon_m14ebr weapon_spas12";

/* Rare (grade 3) */
string ib_pool_rare = "weapon_balrog1 weapon_balrog3 weapon_balrog5 weapon_balrog7 weapon_janus1 weapon_janus3 weapon_janus5 weapon_janus7 weapon_buffak47 weapon_buffm4a1 weapon_buffm249 weapon_buffawp weapon_buffsg552 weapon_thanatos1 weapon_thanatos3 weapon_thanatos5";

/* Epic (grade 4) */
string ib_pool_epic = "weapon_balrog11 weapon_janus11 weapon_thanatos7 weapon_thanatos9 weapon_thanatos11 weapon_railgun weapon_coilgun weapon_kronos1 weapon_kronos3 weapon_kronos5 weapon_laserminigun weapon_bloodhunter";

/* Legendary (grade 5) */
string ib_pool_legendary = "weapon_gungnir weapon_ethereal weapon_volcano weapon_stormgiant weapon_skull1 weapon_skull3 weapon_skull5 weapon_skull7 weapon_skull9 weapon_skull11";

/* pick a random weapon from a space-separated pool */
string
IB_RandomFromPool(string pool)
{
	float n = tokenize(pool);
	if (n <= 0) return "weapon_m4a1";
	float pick = floor(random() * n);
	tokenize(pool);
	return argv(pick);
}

/* roll a grade based on weights */
int
IB_RollGrade(float tierBonus)
{
	float roll = random() * 1000;
	float threshold = 0;

	/* tier bonus shifts weights toward rarer drops */
	float legendary = IB_GRADE_LEGENDARY + tierBonus * 5;
	float epic = IB_GRADE_EPIC + tierBonus * 10;
	float rare = IB_GRADE_RARE + tierBonus * 15;
	float uncommon = IB_GRADE_UNCOMMON;
	float common = 1000 - legendary - epic - rare - uncommon;

	threshold += legendary;
	if (roll < threshold) return 5;
	threshold += epic;
	if (roll < threshold) return 4;
	threshold += rare;
	if (roll < threshold) return 3;
	threshold += uncommon;
	if (roll < threshold) return 2;
	return 1;
}

string
IB_GradeName(int grade)
{
	switch (grade) {
	case 1: return "Common";
	case 2: return "Uncommon";
	case 3: return "Rare";
	case 4: return "Epic";
	case 5: return "Legendary";
	default: return "Unknown";
	}
}

string
IB_GradePool(int grade)
{
	switch (grade) {
	case 1: return ib_pool_common;
	case 2: return ib_pool_uncommon;
	case 3: return ib_pool_rare;
	case 4: return ib_pool_epic;
	case 5: return ib_pool_legendary;
	default: return ib_pool_common;
	}
}

/* open a box and get a random weapon */
bool
IB_OpenBox(entity p, string tier)
{
	float cost;
	float tierBonus;

	if (tier == "basic") { cost = IB_COST_BASIC; tierBonus = 0; }
	else if (tier == "standard") { cost = IB_COST_STANDARD; tierBonus = 1; }
	else if (tier == "premium") { cost = IB_COST_PREMIUM; tierBonus = 2; }
	else if (tier == "elite") { cost = IB_COST_ELITE; tierBonus = 3; }
	else {
		sprint(p, PRINT_HIGH, "Unknown box tier. Use: basic, standard, premium, elite\n");
		return false;
	}

	if (!PROG_SpendMoney(p, cost)) {
		sprint(p, PRINT_HIGH, sprintf("Not enough money. Need $%g, have $%g.\n",
			cost, p.prog_money));
		return false;
	}

	/* roll */
	int grade = IB_RollGrade(tierBonus);
	string pool = IB_GradePool(grade);
	string weapon = IB_RandomFromPool(pool);
	string gradeName = IB_GradeName(grade);

	/* check if already owned */
	int attempts = 0;
	while (INV_HasItem(p, weapon) && attempts < 20) {
		weapon = IB_RandomFromPool(pool);
		attempts++;
	}

	if (INV_HasItem(p, weapon)) {
		/* duplicate: refund partial and give money bonus instead */
		float refund = cost * 0.3;
		PROG_AwardMoney(p, refund);
		sprint(p, PRINT_HIGH, sprintf("[%s Box] Duplicate item! Refunded $%g.\n",
			tier, refund));
		return true;
	}

	/* award the weapon */
	INV_AddItem(p, weapon);
	INV_Save(p);
	PROG_AwardXP(p, grade * 25);

	sprint(p, PRINT_HIGH, sprintf("[%s Box] You got: %s (%s)!\n",
		tier, weapon, gradeName));

	/* announce rare+ drops to all players */
	if (grade >= 3) {
		string msg = sprintf("%s opened a %s box and got %s (%s)!",
			p.netname, tier, weapon, gradeName);
		for (entity q = world; (q = next.Player(q));)
			sprint(q, PRINT_HIGH, strcat("[LOOT] ", msg, "\n"));
	}

	return true;
}

/* daily free box */
bool
IB_DailyBox(entity p)
{
	/* check if 24 hours have passed since last daily */
	if (p.ib_lastDaily > 0 && (time - p.ib_lastDaily) < 86400) {
		float remaining = 86400 - (time - p.ib_lastDaily);
		float hours = floor(remaining / 3600);
		float mins = floor((remaining - hours * 3600) / 60);
		sprint(p, PRINT_HIGH, sprintf("Daily box not ready. %gh %gm remaining.\n",
			hours, mins));
		return false;
	}

	p.ib_lastDaily = time;

	/* daily box: guaranteed at least uncommon */
	int grade = IB_RollGrade(1);
	if (grade < 2) grade = 2;	/* floor at uncommon */

	string pool = IB_GradePool(grade);
	string weapon = IB_RandomFromPool(pool);
	string gradeName = IB_GradeName(grade);

	int attempts = 0;
	while (INV_HasItem(p, weapon) && attempts < 20) {
		weapon = IB_RandomFromPool(pool);
		attempts++;
	}

	if (INV_HasItem(p, weapon)) {
		PROG_AwardMoney(p, 2000);
		sprint(p, PRINT_HIGH, "[Daily] Duplicate! Awarded $2000 instead.\n");
		return true;
	}

	INV_AddItem(p, weapon);
	INV_Save(p);
	sprint(p, PRINT_HIGH, sprintf("[Daily Box] You got: %s (%s)!\n",
		weapon, gradeName));

	if (grade >= 3) {
		for (entity q = world; (q = next.Player(q));)
			sprint(q, PRINT_HIGH, sprintf("[DAILY] %s got %s (%s)!\n",
				p.netname, weapon, gradeName));
	}

	return true;
}

/* command handler */
bool
IB_Command(entity p, string cmd)
{
	tokenize(cmd);
	string c = argv(0);

	if (c == "csnz_openbox") {
		return IB_OpenBox(p, argv(1));
	}
	if (c == "csnz_daily") {
		return IB_DailyBox(p);
	}
	return false;
}
