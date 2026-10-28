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
 * AI zombies/monsters through rounds of increasing difficulty. */

/* --- Zombie Scenario TX (53) --- */

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
CSM_ScenarioTX(void)
{
	cs_modeTitle = "CSO_GameMode_ScenarioTX";
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
}

/* --- Human Scenario (17) ---
 * PvE survival similar to ScenarioTX but without the voxel element.
 * Evidence: EGM_HUMAN_SCENARIO = 17 */
void
CSM_HumanScenario(void)
{
	CSM_ScenarioTX();
	cs_modeTitle = "CSO_GameMode_HumanScenario";
	cs_modeFallback = "Human Scenario";
}

/* --- Zombie Horror Escape (51) ---
 * Evidence: EGM_ZOMBIE_HORROR_ESCAPE = 51
 * Escape-focused PvE: humans must reach the exit while fighting zombies. */
void
CSM_ZombieHorrorEscape(void)
{
	CSM_ScenarioTX();
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
	CSM_ScenarioTX();
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
	/* base on zombie infection core */
	CSM_Zombie(9);	/* use Expand as base */
	cs_modeTitle = "CSO_GameMode_ZombiConquest";
	cs_modeFallback = "Zombi Conquest";
}

/* --- ZS Rift (59) ---
 * Evidence: EGM_ZSRIFT = 59 */
void
CSM_ZSRift(void)
{
	CSM_ScenarioTX();
	cs_modeTitle = "CSO_GameMode_ZSRift";
	cs_modeFallback = "ZS Rift";
}

/* --- Zombi Survival (15) ---
 * Evidence: EGM_ZOMBI_SURVIVAL = 15 */
void
CSM_ZombiSurvival(void)
{
	CSM_ScenarioTX();
	cs_modeTitle = "CSO_GameMode_ZombiSurvival";
	cs_modeFallback = "Zombi Survival";
	sctx_maxRounds = 50;	/* DESIGN: longer survival */
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
