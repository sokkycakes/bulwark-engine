/* scenario: CSO Zombie Scenario TX (53), Human Scenario (17), Zombie Horror
 * Escape (51), Super Soldier (57), Battle Rush (25), Zombi Conquest (56),
 * and ZS Rift (59) modes.
 *
 * Evidence from decompiled mp.dll:
 *   BINARY    EGM_VOXEL_SCENARIOTX=53, EGM_HUMAN_SCENARIO=17,
 *             EGM_ZOMBIE_HORROR_ESCAPE=51, EGM_SUPER_SOLDIER=57,
 *             EGM_BATTLE_RUSH=25, EGM_ZOMBI_CONQUEST=56, EGM_ZSRIFT=59,
 *             EGM_ZOMBI_SURVIVAL=15, EGM_ZOMBI_TEAM_ANN=13
 *   BINARY    CSOGameRulesVoxelScenarioTX with: MainMgr, MonsterMgr,
 *             PlayerMgr, SkillMgr, ItemMgr subclasses
 *   BINARY    Monster specials 1-7, Specialist classes 1-4
 *   BINARY    50+ skill types (Drone, Explosion, SniperScope, etc.)
 *   BINARY    ScenarioTX Item Error string, buymenu, box items
 *   BINARY    HealthBase/HealthIncrease, DamageBase/DamageIncrease scaling
 *
 * These are PvE survival / wave-defense modes where teams of humans fight
 * AI zombies/monsters through rounds of increasing difficulty.
 *
 * mp.dll classes (SOURCE-CONFIRMED, InstallGameRules FUN_11412000 and the
 * constructor chains): 15 ZombieScenarioVer2, 17 HumanScen, 53
 * VoxelScenarioTX and 59 ZSRift each derive from CSOGameRules directly;
 * 48 PlayRoomZBS derives from ZombieScenarioVer2. The wave core below is
 * Zombie Scenario's (15); the others reuse it (DESIGN), they are not its
 * subclasses in the binary. 51 ZHE and 56 ZombiConquest derive from Zombi3,
 * 57 SuperSoldier from TDM2019 and 25 BattleRush from DM: their QC here is
 * a stand-in, not their class. */

/* --- levels: Zombie Scenario v2 (15) and Scenario TX (53) ------------------
   Both modes show the level on the LevelPanel/ExpPanel HUD (client.dll
   ZombieScenarioVer2 FUN_02852ce0, ScenarioTX FUN_02867ba0) and share the
   level-up message panel. The rules publish userinfo csnz_zbslv /
   csnz_zbsexp (percent) / csnz_zbslvup (count) for the HUD.

   Zombie Scenario v2, mp.dll's ZombieScenarioVer2 player (SOURCE-CONFIRMED
   unless marked):
   - Stats (FUN_10935a30): five, health/damage/speed/armor/ammo, each
     starting at 1 with a maximum of the class's zbs_class.csv value + 1.
     Level = 1 + the upgrades taken, max level = 1 + the sum of the maxima.
     The class row is the player's character class; we have no CSO class
     ids, so every player uses the first row, class 40 (DESIGN).
   - AddExp (FUN_10934e90): times the difficulty's ClassEXPRatio (gamerules
     slot 0x44c, FUN_10866a00, column 0x12). While the exp reaches the
     need for the level (zbs_neededexp.csv, slot 0x450, FUN_10865b90; past
     100 the last row), it levels up and pays the need. At max level the
     exp is 0.
   - Level up (FUN_10938790): the next stat in the class's upgrade order
     (the "hdsam" column; FUN_10937380 skips stats at their maximum) goes
     up one, ZBSVer2 case 5 shows the level-up panel and plays
     events/Lvup.wav (client FUN_0284bd40), health rises by 100 up to the
     maximum, and a health upgrade sets the maximum to the table's health
     for that stat level (FUN_10866420). The other stats are sent to the
     client (FUN_1086ac50 and friends); their effects live elsewhere and are
     not ported.
   - Exp source (FUN_109b1aa0, a monster taking damage from a player, not
     from a grenade): min(minExp + dmgExpRatio * damage, maxExp), difficulty
     columns 0x16/0x18/0x17. Nuclide has no NPC damage callback, so the
     rules poll the monsters' health each frame and credit the drop to the
     nearest human that can see the monster (DESIGN stand-in).
   - The percent goes out as ZBSVer2 case 3 every 0.5 s when it changed
     (FUN_10937df0).
   - Late join (FUN_10937c70): after 60 s, a player below joinlevel
     (column 8) times the best other player's level levels up that many
     times minus one.
   - The difficulty table is maps/zbs_difficulty_<map>.csv, else
     maps/zbs_difficulty_default.csv (FUN_1086a5a0); the row is the room's
     difficulty, which we take from zbs_difficulty (DESIGN default 2).
   - Not ported: the per-player bonus levels from account items (slot
     0x3d4 lookups, FUN_109385c0), the EXP_RATIO buff, money from damage.

   Scenario TX, mp.dll's ScenarioTX player (SOURCE-CONFIRMED unless
   marked):
   - AddExp (FUN_108aceb0): only at levels 1-30; exp += amount, and when it
     is over requireexp[level] (scenariotx_dedi.json) it becomes exp mod
     need and the level goes up one, with the level-up effect and
     studio/scenariotx/levelup.wav (FUN_108ad180 0x2a). SetLevel
     (FUN_108b1720) stores the exp as a share of the need in 1/10000.
   - Kill (FUN_108aec30): the killer's combo count goes up; the exp ratio is
     the first comboBonus entry whose max is at least the count. The combo
     drops 5 s after the last kill (FUN_108afc10, globalarg 14 of
     scenariotx_common.json). The killer gets monster exp * ratio. Other
     players get a share by factors not traced (not ported).
   - A monster's exp comes from its type and grade tables; that code is not
     traced and our Scenario TX monsters are stand-ins, so the monster exp
     here is zbs_tx_monsterexp (DESIGN).
   - The starting level is not traced; 1 here (INFERRED). */

var float autocvar_zbs_difficulty = 2;		/* DESIGN: the room's difficulty */
var float autocvar_zbs_tx_monsterexp = 200;	/* DESIGN: see above */

#define ZBS_ROWS 101
#define TX_MAX_LEVEL 30

bool zbs_v2;
bool zbs_tx;
float zbs_need[ZBS_ROWS];
float zbs_hpTab[ZBS_ROWS];
float zbs_statMax[5];
string zbs_order;		/* stat indices in upgrade order, "01243" */
var float zbs_diffEXP = 1;		/* ClassEXPRatio */
float zbs_diffJoin;		/* joinlevel */
var float zbs_minExp = 200, zbs_maxExp = 1000, zbs_dmgExpRatio = 0.8;
float zbs_nextPct;
float tx_need[TX_MAX_LEVEL + 1];
float tx_comboMax[16];
float tx_comboRatio[16];
float tx_comboCount;

.string zbs_stats;	/* current stat levels, "h d s a m" */
.float zbs_hpLast;	/* monsters: health seen last frame */
.float tx_combo;
.float tx_comboTime;

float
ZBS_ReadRow(filestream f)
{
	string s;
	while ((s = fgets(f))) {
		s = strreplace("\r", "", s);
		if (s == "" || substring(s, 0, 1) == ";")
			continue;
		return tokenizebyseparator(s, ",");
	}
	return 0;
}

void
ZBS_LoadTables(void)
{
	filestream f = fopen("zbsver2/zbs_neededexp.csv", FILE_READ);
	if (f >= 0) {
		ZBS_ReadRow(f);	/* header */
		while (ZBS_ReadRow(f)) {
			float lv = stof(argv(0));
			if (argv(0) == "" || lv < 0 || lv >= ZBS_ROWS)
				continue;
			zbs_need[lv] = stof(argv(1));
			zbs_hpTab[lv] = stof(argv(3));
		}
		fclose(f);
	} else {
		print("csnz: zbsver2/zbs_neededexp.csv missing\n");
	}

	f = fopen("zbsver2/zbs_class.csv", FILE_READ);
	if (f >= 0) {
		ZBS_ReadRow(f);
		if (ZBS_ReadRow(f) >= 7) {
			string order = "";
			for (float i = 0; i < 5; i++)
				zbs_statMax[i] = stof(argv(i + 1)) + 1;
			string o = argv(6);
			for (float c = 0; c < strlen(o); c++)
				order = strcat(order, ftos(strstrofs("hdsam", substring(o, c, 1))));
			zbs_order = strzone(order);
		}
		fclose(f);
	}

	f = fopen(sprintf("maps/zbs_difficulty_%s.csv", strtolower(mapname)), FILE_READ);
	if (f < 0)
		f = fopen("maps/zbs_difficulty_default.csv", FILE_READ);
	if (f >= 0) {
		while (ZBS_ReadRow(f) >= 25) {
			if (stof(argv(0)) != autocvar_zbs_difficulty)
				continue;
			zbs_diffJoin = stof(argv(8));
			zbs_diffEXP = stof(argv(18));
			zbs_minExp = stof(argv(22));
			zbs_maxExp = stof(argv(23));
			zbs_dmgExpRatio = stof(argv(24));
			break;
		}
		fclose(f);
	}
	printf("csnz: Zombie Scenario v2 levels, difficulty %d: exp x%g, damage exp %g..%g (x%g), join level x%g\n",
		autocvar_zbs_difficulty, zbs_diffEXP, zbs_minExp, zbs_maxExp, zbs_dmgExpRatio, zbs_diffJoin);
}

/* scenariotx_dedi.json: requireexp and comboBonus */
void
TX_LoadTables(void)
{
	filestream f = fopen("resource/scenariotx/scenariotx_dedi.json", FILE_READ);
	string txt = "", ln;
	if (f < 0) {
		print("csnz: resource/scenariotx/scenariotx_dedi.json missing\n");
		return;
	}
	while ((ln = fgets(f))) {
		float c = strstrofs(ln, "//");
		if (c >= 0)
			ln = substring(ln, 0, c);
		txt = strcat(txt, ln, "\n");
	}
	fclose(f);
	jsonnode root = json_parse(txt);
	if (!root)
		return;
	jsonnode req = json_find_object_child(root, "requireexp");
	for (int i = 0i; req && i < json_get_length(req) && i <= TX_MAX_LEVEL; i++)
		tx_need[i] = json_get_float(json_get_child_at_index(req, i));
	jsonnode combo = json_find_object_child(root, "comboBonus");
	tx_comboCount = 0;
	for (int j = 0i; combo && j < json_get_length(combo) && j < 16; j++) {
		jsonnode e = json_get_child_at_index(combo, j);
		tx_comboMax[tx_comboCount] = json_get_float(json_find_object_child(e, "max"));
		tx_comboRatio[tx_comboCount] = json_get_float(json_find_object_child(e, "expRatio"));
		tx_comboCount++;
	}
	json_free(root);
}

void
ZBS_Publish(entity p)
{
	userinfo.SetInteger(p, "csnz_zbslv", (int)p.zbz_level);
	userinfo.SetInteger(p, "csnz_zbsexp", (int)p.zbz_pct);
	userinfo.SetInteger(p, "csnz_zbslvup", (int)p.zbz_lvup);
}

float
ZBS_Stat(entity p, float s)
{
	tokenize(p.zbs_stats);
	return stof(argv(s));
}

float
ZBS_MaxLevel(void)
{
	float m = 1;
	for (float s = 0; s < 5; s++)
		m += zbs_statMax[s] - 1;
	return m;
}

float
ZBS_Need(float lv)
{
	return zbs_need[min(lv, ZBS_ROWS - 1)];
}

/* FUN_10938790 with FUN_10937380 picking the stat */
void
ZBS_LevelUp(entity p)
{
	float n = strlen(zbs_order);
	float s = -1;
	for (float tries = 0; tries < n; tries++) {
		float k = stof(substring(zbs_order, p.zbz_points, 1));
		if (ZBS_Stat(p, k) < zbs_statMax[k]) {
			s = k;
			break;
		}
		p.zbz_points = (p.zbz_points + 1) % n;
	}
	if (s < 0)
		return;

	float st[5];
	tokenize(p.zbs_stats);
	for (float i = 0; i < 5; i++)
		st[i] = stof(argv(i));
	st[s]++;
	if (p.zbs_stats)
		strunzone(p.zbs_stats);
	p.zbs_stats = strzone(sprintf("%d %d %d %d %d", st[0], st[1], st[2], st[3], st[4]));
	p.zbz_level++;

	if (s == 0 && zbs_hpTab[st[0]] > 0)
		ents.Input(p, "SetMaxHealth", ftos(zbs_hpTab[st[0]]), world);
	if (p.health > 0)
		ents.Input(p, "SetHealth", ftos(min(p.max_health, p.health + 100)), world);

	p.zbz_lvup++;
	sound(p, CHAN_AUTO, "events/lvup.wav", 1, ATTN_NONE);
	printf("csnz %.1f: %s reached scenario level %d (stat %d now %d)\n", time, p.netname, p.zbz_level, s, st[s]);
	ZBS_Publish(p);
}

void
ZBS_AddExp(entity p, float amount)
{
	if (!zbs_v2 || amount < 0 || !p.zbz_level)
		return;
	float top = ZBS_MaxLevel();
	if (p.zbz_level >= top) {
		p.zbz_level = top;
		p.zbz_exp = 0;
		return;
	}
	p.zbz_exp += floor(amount * zbs_diffEXP);
	float need = ZBS_Need(p.zbz_level);
	while (need > 0 && need <= p.zbz_exp) {
		if (p.zbz_level >= top) {
			p.zbz_level = top;
			break;
		}
		ZBS_LevelUp(p);
		p.zbz_exp -= need;
		need = ZBS_Need(p.zbz_level);
	}
	if (p.zbz_level >= top)
		p.zbz_exp = 0;
}

/* Scenario TX: FUN_108aceb0 and SetLevel FUN_108b1720 */
void
TX_AddExp(entity p, float amount)
{
	if (!zbs_tx || p.zbz_level < 1 || p.zbz_level > TX_MAX_LEVEL)
		return;
	p.zbz_exp += amount;
	float need = tx_need[p.zbz_level];
	if (need > 0 && p.zbz_exp > need) {
		p.zbz_exp = p.zbz_exp % need;
		p.zbz_level = min(TX_MAX_LEVEL, p.zbz_level + 1);
		p.zbz_lvup++;
		sound(p, CHAN_AUTO, "studio/scenariotx/levelup.wav", 1, ATTN_NORM);
		printf("csnz %.1f: %s reached Scenario TX level %d\n", time, p.netname, p.zbz_level);
	}
	need = tx_need[p.zbz_level];
	p.zbz_pct = need > 0 ? floor(p.zbz_exp / need * 100) : 0;
	ZBS_Publish(p);
}

void
ZBS_InitPlayer(entity p)
{
	if (p.zbz_level)
		return;
	p.zbz_level = 1;
	p.zbz_exp = 0;
	p.zbz_pct = 0;
	p.zbz_points = 0;
	if (zbs_v2) {
		if (p.zbs_stats)
			strunzone(p.zbs_stats);
		p.zbs_stats = strzone("1 1 1 1 1");
		/* FUN_10937c70 */
		if (time > 60) {
			float best = 1;
			for (entity q = world; (q = next.Player(q));)
				if (q != p && q.zbz_level > best)
					best = q.zbz_level;
			float target = floor(zbs_diffJoin * best);
			for (float i = 1; p.zbz_level < target && i < target; i++)
				ZBS_LevelUp(p);
		}
	}
	ZBS_Publish(p);
}

/* test hook: cs_test_zbshit N makes a human hit the nearest monster N
   times through Nuclide's damage path, 5 hits a second */
float zbs_nextTestHit;
string zbs_testDecl;
void
ZBS_TestHit(void)
{
	float n = cvars.GetFloat("cs_test_zbshit");
	if (n <= 0 || time < zbs_nextTestHit)
		return;
	entity p = world;
	for (entity q = world; (q = next.Player(q));)
		if (CS_Playing(q) && q.health > 0) {
			p = q;
			break;
		}
	if (!p)
		return;
	entity best = world;
	for (entity m = world; (m = find(m, ::classname, "monster_zombie"));)
		if (m.health > 0 && m.takedamage && (!best || vlen(m.origin - p.origin) < vlen(best.origin - p.origin)))
			best = m;
	if (!best)
		return;
	if (!zbs_testDecl) {
		zbs_testDecl = strzone(declManager.New());
		declManager.AddKey(zbs_testDecl, "damage", "100");
	}
	zbs_nextTestHit = time + 0.2;
	cvars.SetString("cs_test_zbshit", ftos(n - 1));
	combat.Damage(best, p, p, zbs_testDecl, p.origin, normalize(best.origin - p.origin), best.origin);
}

/* the damage-exp stand-in and the 0.5 s percent update */
void
ZBS_Frame(void)
{
	ZBS_TestHit();
	if (zbs_v2) {
		for (entity m = world; (m = find(m, ::classname, "monster_zombie"));) {
			if (m.zbs_hpLast < 0)
				continue;	/* a corpse, see ZBS_NPCKilled */
			if (!m.zbs_hpLast || m.health > m.zbs_hpLast) {
				m.zbs_hpLast = m.health;
				continue;
			}
			float dmg = m.zbs_hpLast - m.health;
			m.zbs_hpLast = m.health;
			if (dmg <= 0)
				continue;
			entity best = world;
			float bestDist = 999999;
			for (entity p = world; (p = next.Player(p));) {
				if (!CS_Playing(p))
					continue;
				float d = vlen(p.origin - m.origin);
				traceline(p.origin + p.view_ofs, m.origin, MOVE_NOMONSTERS, p);
				if (trace_fraction < 1 || d >= bestDist)
					continue;
				best = p;
				bestDist = d;
			}
			if (best)
				ZBS_AddExp(best, min(floor(zbs_minExp + zbs_dmgExpRatio * dmg), zbs_maxExp));
		}
	}
	if (time < zbs_nextPct)
		return;
	zbs_nextPct = time + 0.5;
	for (entity q = world; (q = next.Player(q));) {
		if (zbs_tx && q.tx_combo && q.tx_comboTime + 5 < time)
			q.tx_combo = q.tx_comboTime = 0;
		if (!zbs_v2 || !q.zbz_level)
			continue;
		float pct = 0;
		if (q.zbz_level < ZBS_MaxLevel() && ZBS_Need(q.zbz_level) > 0)
			pct = floor(q.zbz_exp * 100 / ZBS_Need(q.zbz_level));
		if (pct != q.zbz_pct) {
			q.zbz_pct = pct;
			ZBS_Publish(q);
		}
	}
}

/* the killing blow: the poll can't see it, since a dead monster's health
   becomes its corpse's (Nuclide Monster.qc) or it is gibbed and removed.
   The blow counts as the health it had left (INFERRED; FUN_109b1aa0 gets
   the damage dealt). */
void
ZBS_KillingBlow(entity npc, entity attacker)
{
	float left = npc.zbs_hpLast ? npc.zbs_hpLast : npc.max_health;	/* not polled yet */
	npc.zbs_hpLast = -1;
	if (!zbs_v2 || !(attacker.flags & FL_CLIENT) || left <= 0)
		return;
	ZBS_AddExp(attacker, min(floor(zbs_minExp + zbs_dmgExpRatio * left), zbs_maxExp));
}

/* FUN_108aec30: a Scenario TX monster killed by a player */
void
ZBS_NPCKilled(entity npc, entity attacker)
{
	ZBS_KillingBlow(npc, attacker);
	if (!zbs_tx || !(attacker.flags & FL_CLIENT))
		return;
	attacker.tx_combo++;
	attacker.tx_comboTime = time;
	float ratio = 1;
	for (float i = 0; i < tx_comboCount; i++)
		if (attacker.tx_combo <= tx_comboMax[i]) {
			ratio = tx_comboRatio[i];
			break;
		}
	TX_AddExp(attacker, floor(autocvar_zbs_tx_monsterexp * ratio));
}


/* --- the wave core: Zombie Scenario (15) --- */

var float autocvar_sctx_rounds = 30;		/* DESIGN: total rounds */
var float autocvar_sctx_round_time = 2;		/* DESIGN: minutes per round */
var float autocvar_sctx_monster_hp_base = 500;	/* DESIGN: base monster HP */
var float autocvar_sctx_monster_hp_scale = 100;	/* DESIGN: HP added per round */
var float autocvar_sctx_monster_dmg_base = 20;	/* DESIGN */
var float autocvar_sctx_monster_dmg_scale = 5;	/* DESIGN */
var float autocvar_sctx_boss_interval = 5;	/* DESIGN: boss every N rounds */
var float autocvar_sctx_supply_interval = 60;	/* DESIGN: seconds between supply drops */

.float sctx_score;
.float sctx_class;	/* specialist class 1-4 */

int sctx_round;
int sctx_maxRounds;
float sctx_nextSupply;
float sctx_monstersAlive;
float sctx_monstersToSpawn;
float sctx_nextSpawn;

void
SCTX_SpawnMonster(void)
{
	float hp = autocvar_sctx_monster_hp_base +
		autocvar_sctx_monster_hp_scale * sctx_round;
	bool isBoss = (sctx_round % (int)autocvar_sctx_boss_interval == 0);

	if (isBoss)
		hp *= 5;	/* DESIGN: bosses have 5x HP */

	/* use Nuclide monster system */
	/* pick a spawn point away from players */
	vector spot = [0, 0, 0];
	entity sp;
	for (sp = world; (sp = find(sp, ::classname, "info_player_start"));)
		spot = sp.origin;	/* use last spawn as fallback */

	entity m = ents.Create("monster_zombie", spot + [random() * 200 - 100, random() * 200 - 100, 32]);
	if (m) {
		ents.Input(m, "SetHealth", ftos(hp), world);
		ents.Input(m, "SetMaxHealth", ftos(hp), world);
		sctx_monstersAlive++;
	}
}

void
SCTX_Equip(entity p)
{
	ents.ChangeToClass(p, "csm_rifleman");
	CS_FillAmmo(p);
	ents.Input(p, "SetHealth", "100", world);
	ents.Input(p, "SetArmor", "100", world);
	ZBS_InitPlayer(p);
}

void
SCTX_RoundStart(void)
{
	sctx_round++;
	sctx_monstersAlive = 0;
	sctx_monstersToSpawn = 3 + sctx_round * 2;	/* DESIGN: scaling wave size */
	sctx_nextSpawn = time + 3;
	sctx_nextSupply = time + autocvar_sctx_supply_interval;

	bool isBoss = (sctx_round % (int)autocvar_sctx_boss_interval == 0);
	if (isBoss) {
		CS_Banner(sprintf("BOSS ROUND %d!", (float)sctx_round));
		sctx_monstersToSpawn = 1;	/* boss rounds: one powerful monster */
	} else {
		CS_Banner(sprintf("Round %d / %d", (float)sctx_round, (float)sctx_maxRounds));
	}
	serverinfo.SetInteger("csnz_sctx_round", sctx_round);
}

void
SCTX_Killed(entity victim, entity inflictor, entity attacker, string weapon)
{
	if (is.Player(victim)) {
		/* human died */
	} else {
		/* monster killed */
		sctx_monstersAlive = max(0, sctx_monstersAlive - 1);
		if (is.Player(attacker)) {
			attacker.sctx_score += 10;
			userinfo.SetInteger(attacker, "csnz_score", (int)attacker.sctx_score);
		}
	}
}

/* Nuclide's NPCDeath reaches the rules as CodeCallback_NPCKilled; monster
   kills never came through M_Killed, which only sees players */
void
SCTX_NPCKilled(entity npc, entity attacker)
{
	SCTX_Killed(npc, attacker, attacker, "");
	ZBS_NPCKilled(npc, attacker);
}

float
SCTX_CheckRound(void)
{
	/* all humans dead? */
	int alive = 0;
	for (entity p = world; (p = next.Player(p));)
		if (CS_Playing(p) && p.health > 0)
			alive++;
	if (alive == 0)
		return CS_TEAM1;	/* zombies win */

	/* all monsters cleared and no more to spawn? */
	if (sctx_monstersAlive <= 0 && sctx_monstersToSpawn <= 0) {
		if (sctx_round >= sctx_maxRounds)
			return CS_TEAM2;	/* humans survived all rounds */
		/* next round starts via RoundStart */
	}
	return 0;
}

void
SCTX_Frame(void)
{
	ZBS_Frame();

	/* spawn monsters over time */
	if (sctx_monstersToSpawn > 0 && time >= sctx_nextSpawn) {
		SCTX_SpawnMonster();
		sctx_monstersToSpawn--;
		sctx_nextSpawn = time + 2;	/* DESIGN: 2s between spawns */
	}

	/* supply drops */
	if (time >= sctx_nextSupply) {
		sctx_nextSupply = time + autocvar_sctx_supply_interval;
		for (entity p = world; (p = next.Player(p));) {
			if (CS_Playing(p)) {
				CS_FillAmmo(p);
				if (p.health < 100)
					ents.Input(p, "SetHealth", "100", world);
			}
		}
		CS_Banner("Supply drop!");
	}
}

bool
SCTX_Command(entity p, string cmd)
{
	tokenize(cmd);
	if (argv(0) == "csnz_class" && stof(argv(1)) >= 1 && stof(argv(1)) <= 4) {
		p.sctx_class = stof(argv(1));
		sprint(p, PRINT_HIGH, sprintf("Specialist class %g selected\n", p.sctx_class));
		return true;
	}
	return false;
}

void
CSM_ZombieScenario(void)
{
	cs_modeTitle = "CSO_GameMode_Zombi_Survival";
	cs_modeFallback = "Zombie Scenario";
	cs_teams = true;
	cs_rounds = false;	/* continuous rounds managed internally */
	cs_oneSideStart = false;
	cs_respawnDelay = 5;
	cs_timeLimitFallback = 0;	/* no time limit, round-based */
	cs_winLimitFallback = 0;
	cs_team1Name = "Zombies";
	cs_team2Name = "Humans";
	sctx_round = 0;
	sctx_maxRounds = (int)autocvar_sctx_rounds;

	M_Equip = SCTX_Equip;
	M_RoundStart = SCTX_RoundStart;
	M_Frame = SCTX_Frame;
	M_Killed = SCTX_Killed;
	M_CheckRound = SCTX_CheckRound;
	M_Command = SCTX_Command;
	M_NPCKilled = SCTX_NPCKilled;

	/* ZombieScenarioVer2 is mode 15 (FUN_10862b50) */
	zbs_v2 = (autocvar_csnz_mode == 15);
	zbs_tx = false;
	if (zbs_v2)
		ZBS_LoadTables();
}

/* --- Scenario TX (53), a Studio mode: studio.dat plays it on a Studio map --- */
void
CSM_ScenarioTX(void)
{
	CSM_ZombieScenario();
	cs_modeTitle = "CSO_GameMode_ScenarioTX";
	cs_modeFallback = "Scenario TX";
	zbs_v2 = false;
	zbs_tx = true;
	TX_LoadTables();
}

/* --- Human Scenario (17) ---
 * PvE survival on the Zombie Scenario wave core.
 * Evidence: EGM_HUMAN_SCENARIO = 17 */
void
CSM_HumanScenario(void)
{
	CSM_ZombieScenario();
	cs_modeTitle = "CSO_GameMode_HumanScenario";
	cs_modeFallback = "Human Scenario";
}

/* --- Zombie Horror Escape (51) ---
 * Evidence: EGM_ZOMBIE_HORROR_ESCAPE = 51
 * Escape-focused PvE: humans must reach the exit while fighting zombies. */
void
CSM_ZombieHorrorEscape(void)
{
	CSM_ZombieScenario();
	cs_modeTitle = "CSO_GameMode_ZombieHorrorEscape";
	cs_modeFallback = "Zombie Horror Escape";
	/* escape-focused: shorter rounds, more zombies */
	sctx_maxRounds = 10;
}

/* --- Super Soldier (57) ---
 * Evidence: EGM_SUPER_SOLDIER = 57
 * Humans vs AI zombies with enhanced player abilities. */
void
CSM_SuperSoldier(void)
{
	CSM_ZombieScenario();
	cs_modeTitle = "CSO_GameMode_SuperSoldier";
	cs_modeFallback = "Super Soldier";
}

/* --- Battle Rush (25) ---
 * Evidence: EGM_BATTLE_RUSH = 25
 * Fast-paced team combat mode. */
void
CSM_BattleRush(void)
{
	CSM_TeamDeathmatch(false);
	cs_modeTitle = "CSO_GameMode_BattleRush";
	cs_modeFallback = "Battle Rush";
	cs_respawnDelay = 0;	/* instant respawn */
	cs_timeLimitFallback = 5;
}

/* --- Zombi Conquest (56) ---
 * Evidence: EGM_ZOMBI_CONQUEST = 56
 * Territory control zombie mode. */
void
CSM_ZombiConquest(void)
{
	/* ZombiConquest derives from Zombi3 (SOURCE-CONFIRMED), so the
	   Zombie Hero set; the territory rules are not ported */
	CSM_Zombie(14);
	cs_modeTitle = "CSO_GameMode_ZombiConquest";
	cs_modeFallback = "Zombi Conquest";
}

/* --- ZS Rift (59) ---
 * Evidence: EGM_ZSRIFT = 59 */
void
CSM_ZSRift(void)
{
	CSM_ZombieScenario();
	cs_modeTitle = "CSO_GameMode_ZSRift";
	cs_modeFallback = "ZS Rift";
}

/* --- Zombi Team Anniversary (13) ---
 * Evidence: EGM_ZOMBI_TEAM_ANN = 13 */
void
CSM_ZombiTeamAnn(void)
{
	CSM_ZombieTeam();
	cs_modeTitle = "CSO_GameMode_ZombiTeamAnn";
	cs_modeFallback = "Zombi Team Anniversary";
}
