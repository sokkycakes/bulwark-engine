/* csnz_achievements: local achievement and mission tracking system.
 *
 * Tracks 30 achievements per player across 5 categories, persisted to
 * data/csnz/players/<id>_achievements.txt. Awards XP bonuses via
 * PROG_AwardXP when thresholds are met.
 *
 * Original code (ISC). DESIGN values throughout - these are approximations. */

/* --- achievement IDs (indices into per-player arrays) --- */

#define ACH_MAX 30

/* Combat (10) */
#define ACH_FIRST_BLOOD		0	/* first kill */
#define ACH_HEADHUNTER		1	/* 100 headshots */
#define ACH_SHARPSHOOTER	2	/* 90% accuracy in a match */
#define ACH_RAMPAGE		3	/* 5 kills without dying */
#define ACH_BOMB_EXPERT		4	/* plant 50 bombs */
#define ACH_DEFUSER		5	/* defuse 50 bombs */
#define ACH_RESCUER		6	/* rescue 50 hostages */
#define ACH_DAMAGE_DEALER	7	/* deal 100000 damage */
#define ACH_KILLSTREAK		8	/* 10 kills in one round */
#define ACH_OVERKILL		9	/* deal 500 damage in one hit */

/* Survival (5) */
#define ACH_SURVIVOR		10	/* survive 100 rounds */
#define ACH_IRONWALL		11	/* take 50000 damage total */
#define ACH_UNTOUCHABLE		12	/* win a round without taking damage */
#define ACH_LAST_STAND		13	/* win a round as last player alive */
#define ACH_ENDURANCE		14	/* play for 100 hours total */

/* Zombie (5) */
#define ACH_ZOMBIE_SLAYER	15	/* kill 500 zombies */
#define ACH_ZOMBIE_KING		16	/* kill 50 zombies in one match */
#define ACH_INFECTION_STOP	17	/* prevent 100 infections (zombie mode kills) */
#define ACH_HORDE_BREAKER	18	/* survive 50 zombie rounds */
#define ACH_EXTERMINATOR	19	/* kill 5000 zombies */

/* Mode (5) */
#define ACH_ALL_STAR		20	/* play every mode (8 modes) */
#define ACH_MASTER_BLASTER	21	/* win 10 deathmatch games */
#define ACH_TACTICIAN		22	/* win 10 bomb/defusal games */
#define ACH_HOSTAGE_HERO	23	/* win 10 hostage games */
#define ACH_MODE_MASTER		24	/* win 50 matches in any single mode */

/* Social (5) */
#define ACH_TEAM_PLAYER		25	/* win 100 team matches */
#define ACH_MEDIC		26	/* heal 1000 HP */
#define ACH_MVP_KING		27	/* earn MVP 50 times */
#define ACH_VETERAN		28	/* play 500 matches */
#define ACH_LEGEND		29	/* reach level 50 */

/* --- achievement metadata --- */

string
ACH_Name(float id)
{
	switch (id) {
	case ACH_FIRST_BLOOD:	return "First Blood";
	case ACH_HEADHUNTER:	return "Headhunter";
	case ACH_SHARPSHOOTER:	return "Sharp Shooter";
	case ACH_RAMPAGE:	return "Rampage";
	case ACH_BOMB_EXPERT:	return "Bomb Expert";
	case ACH_DEFUSER:	return "Defuser";
	case ACH_RESCUER:	return "Rescuer";
	case ACH_DAMAGE_DEALER:	return "Damage Dealer";
	case ACH_KILLSTREAK:	return "Kill Streak";
	case ACH_OVERKILL:	return "Overkill";
	case ACH_SURVIVOR:	return "Survivor";
	case ACH_IRONWALL:	return "Iron Wall";
	case ACH_UNTOUCHABLE:	return "Untouchable";
	case ACH_LAST_STAND:	return "Last Stand";
	case ACH_ENDURANCE:	return "Endurance";
	case ACH_ZOMBIE_SLAYER:	return "Zombie Slayer";
	case ACH_ZOMBIE_KING:	return "Zombie King";
	case ACH_INFECTION_STOP: return "Infection Stop";
	case ACH_HORDE_BREAKER: return "Horde Breaker";
	case ACH_EXTERMINATOR:	return "Exterminator";
	case ACH_ALL_STAR:	return "All-Star";
	case ACH_MASTER_BLASTER: return "Master Blaster";
	case ACH_TACTICIAN:	return "Tactician";
	case ACH_HOSTAGE_HERO:	return "Hostage Hero";
	case ACH_MODE_MASTER:	return "Mode Master";
	case ACH_TEAM_PLAYER:	return "Team Player";
	case ACH_MEDIC:		return "Medic";
	case ACH_MVP_KING:	return "MVP King";
	case ACH_VETERAN:	return "Veteran";
	case ACH_LEGEND:	return "Legend";
	default: return "Unknown";
	}
}

float
ACH_Threshold(float id)
{
	switch (id) {
	case ACH_FIRST_BLOOD:	return 1;
	case ACH_HEADHUNTER:	return 100;
	case ACH_SHARPSHOOTER:	return 1;	/* boolean: achieved in a match */
	case ACH_RAMPAGE:	return 1;	/* boolean: achieved in a life */
	case ACH_BOMB_EXPERT:	return 50;
	case ACH_DEFUSER:	return 50;
	case ACH_RESCUER:	return 50;
	case ACH_DAMAGE_DEALER:	return 100000;
	case ACH_KILLSTREAK:	return 1;	/* boolean: achieved in a round */
	case ACH_OVERKILL:	return 1;	/* boolean: achieved in a hit */
	case ACH_SURVIVOR:	return 100;
	case ACH_IRONWALL:	return 50000;
	case ACH_UNTOUCHABLE:	return 1;
	case ACH_LAST_STAND:	return 1;
	case ACH_ENDURANCE:	return 360000;	/* 100 hours in seconds */
	case ACH_ZOMBIE_SLAYER:	return 500;
	case ACH_ZOMBIE_KING:	return 1;
	case ACH_INFECTION_STOP: return 100;
	case ACH_HORDE_BREAKER: return 50;
	case ACH_EXTERMINATOR:	return 5000;
	case ACH_ALL_STAR:	return 255;	/* all 8 mode bits set */
	case ACH_MASTER_BLASTER: return 10;
	case ACH_TACTICIAN:	return 10;
	case ACH_HOSTAGE_HERO:	return 10;
	case ACH_MODE_MASTER:	return 50;
	case ACH_TEAM_PLAYER:	return 100;
	case ACH_MEDIC:		return 1000;
	case ACH_MVP_KING:	return 50;
	case ACH_VETERAN:	return 500;
	case ACH_LEGEND:	return 1;	/* boolean: reached level 50 */
	default: return 1;
	}
}

/* XP bonus awarded when an achievement is completed (DESIGN) */
float
ACH_XPReward(float id)
{
	switch (id) {
	case ACH_FIRST_BLOOD:	return 100;
	case ACH_HEADHUNTER:	return 2000;
	case ACH_SHARPSHOOTER:	return 500;
	case ACH_RAMPAGE:	return 500;
	case ACH_BOMB_EXPERT:	return 1500;
	case ACH_DEFUSER:	return 1500;
	case ACH_RESCUER:	return 1500;
	case ACH_DAMAGE_DEALER:	return 3000;
	case ACH_KILLSTREAK:	return 750;
	case ACH_OVERKILL:	return 300;
	case ACH_SURVIVOR:	return 2500;
	case ACH_IRONWALL:	return 2000;
	case ACH_UNTOUCHABLE:	return 500;
	case ACH_LAST_STAND:	return 750;
	case ACH_ENDURANCE:	return 5000;
	case ACH_ZOMBIE_SLAYER:	return 2000;
	case ACH_ZOMBIE_KING:	return 1000;
	case ACH_INFECTION_STOP: return 2000;
	case ACH_HORDE_BREAKER: return 1500;
	case ACH_EXTERMINATOR:	return 5000;
	case ACH_ALL_STAR:	return 3000;
	case ACH_MASTER_BLASTER: return 1000;
	case ACH_TACTICIAN:	return 1000;
	case ACH_HOSTAGE_HERO:	return 1000;
	case ACH_MODE_MASTER:	return 2000;
	case ACH_TEAM_PLAYER:	return 3000;
	case ACH_MEDIC:		return 2000;
	case ACH_MVP_KING:	return 2500;
	case ACH_VETERAN:	return 5000;
	case ACH_LEGEND:	return 10000;
	default: return 500;
	}
}

string
ACH_Category(float id)
{
	if (id < 10) return "Combat";
	if (id < 15) return "Survival";
	if (id < 20) return "Zombie";
	if (id < 25) return "Mode";
	return "Social";
}

/* --- per-player achievement state (entity fields) --- */

/* per-player tables, by client slot: field arrays would take 60 of the 256
   spare entity words an added progs gets (studio.dat ran out) */
#define ACH_SLOTS 64
float ach_progressTab[ACH_SLOTS * ACH_MAX];	/* current progress toward each achievement */
float ach_completedTab[ACH_SLOTS * ACH_MAX];	/* 1 if completed, 0 otherwise */
float
ACH_Idx(entity pl, float n)
{
	return bound(0, num_for_edict(pl) - 1, ACH_SLOTS - 1) * ACH_MAX + n;
}
#define ach_progress(pl, n) ach_progressTab[ACH_Idx(pl, n)]
#define ach_completed(pl, n) ach_completedTab[ACH_Idx(pl, n)]

/* per-match / per-life tracking counters */
.float ach_kills_life;		/* kills in current life (for Rampage) */
.float ach_kills_round;		/* kills in current round (for Kill Streak) */
.float ach_shots_fired;		/* shots fired this match (for accuracy) */
.float ach_shots_hit;		/* shots hit this match (for accuracy) */
.float ach_damage_taken_round;	/* damage taken this round (for Untouchable) */
.float ach_zombie_kills_match;	/* zombie kills this match (for Zombie King) */

/* cumulative stat counters */
.float ach_kills;
.float ach_headshots;
.float ach_zombie_kills;
.float ach_rounds_survived;
.float ach_bombs_planted;
.float ach_bombs_defused;
.float ach_hostages_rescued;
.float ach_modes_played;	/* bitfield: bit N = mode N played */
.float ach_matches_won;
.float ach_damage_dealt;
.float ach_damage_healed;

/* --- file I/O --- */

string
ACH_SavePath(entity p)
{
	return CSNZ_PlayerFile(p, "_achievements.txt", FALSE);
}

void
ACH_Reset(entity p)
{
	/* zero everything */
	for (float i = 0; i < ACH_MAX; i++) {
		ach_progress(p, i) = 0;
		ach_completed(p, i) = 0;
	}
	p.ach_kills = 0;
	p.ach_headshots = 0;
	p.ach_zombie_kills = 0;
	p.ach_rounds_survived = 0;
	p.ach_bombs_planted = 0;
	p.ach_bombs_defused = 0;
	p.ach_hostages_rescued = 0;
	p.ach_modes_played = 0;
	p.ach_matches_won = 0;
	p.ach_damage_dealt = 0;
	p.ach_damage_healed = 0;

	/* per-life / per-match */
	p.ach_kills_life = 0;
	p.ach_kills_round = 0;
	p.ach_shots_fired = 0;
	p.ach_shots_hit = 0;
	p.ach_damage_taken_round = 0;
	p.ach_zombie_kills_match = 0;
}

/* one line of the save file (or of the SpacetimeDB profile) */
void
ACH_ParseLine(entity p, string line)
{
	float n = tokenize(line);
	if (n < 2) return;
	string key = argv(0);
	string val = argv(1);

	/* achievement progress/completion lines: "ach <id> <progress> <completed>" */
	if (key == "ach") {
		if (n >= 4) {
			float id = stof(val);
			if (id >= 0 && id < ACH_MAX) {
				ach_progress(p, id) = stof(argv(2));
				ach_completed(p, id) = stof(argv(3));
			}
		}
		return;
	}

	/* cumulative stat counters */
	if (key == "kills") p.ach_kills = stof(val);
	else if (key == "headshots") p.ach_headshots = stof(val);
	else if (key == "zombie_kills") p.ach_zombie_kills = stof(val);
	else if (key == "rounds_survived") p.ach_rounds_survived = stof(val);
	else if (key == "bombs_planted") p.ach_bombs_planted = stof(val);
	else if (key == "bombs_defused") p.ach_bombs_defused = stof(val);
	else if (key == "hostages_rescued") p.ach_hostages_rescued = stof(val);
	else if (key == "modes_played") p.ach_modes_played = stof(val);
	else if (key == "matches_won") p.ach_matches_won = stof(val);
	else if (key == "damage_dealt") p.ach_damage_dealt = stof(val);
	else if (key == "damage_healed") p.ach_damage_healed = stof(val);
}

void
ACH_Load(entity p)
{
	ACH_Reset(p);

	string path = CSNZ_PlayerFile(p, "_achievements.txt", TRUE);
	filestream f = fopen(path, FILE_READ);
	if (f < 0) {
		print(sprintf("achievements: new player %s\n", p.netname));
		return;
	}

	string line;
	while ((line = fgets(f)))
		ACH_ParseLine(p, line);
	fclose(f);

	float completed = 0;
	for (float i = 0; i < ACH_MAX; i++)
		if (ach_completed(p, i))
			completed++;
	print(sprintf("achievements: loaded %s (%g/%g completed)\n",
		p.netname, completed, (float)ACH_MAX));
}

/* the save file's text */
string
ACH_Text(entity p)
{
	string s = "";
	for (float i = 0; i < ACH_MAX; i++)
		s = strcat(s, sprintf("ach %g %g %g\n", i, ach_progress(p, i), ach_completed(p, i)));
	s = strcat(s, sprintf("kills %g\n", p.ach_kills));
	s = strcat(s, sprintf("headshots %g\n", p.ach_headshots));
	s = strcat(s, sprintf("zombie_kills %g\n", p.ach_zombie_kills));
	s = strcat(s, sprintf("rounds_survived %g\n", p.ach_rounds_survived));
	s = strcat(s, sprintf("bombs_planted %g\n", p.ach_bombs_planted));
	s = strcat(s, sprintf("bombs_defused %g\n", p.ach_bombs_defused));
	s = strcat(s, sprintf("hostages_rescued %g\n", p.ach_hostages_rescued));
	s = strcat(s, sprintf("modes_played %g\n", p.ach_modes_played));
	s = strcat(s, sprintf("matches_won %g\n", p.ach_matches_won));
	s = strcat(s, sprintf("damage_dealt %g\n", p.ach_damage_dealt));
	s = strcat(s, sprintf("damage_healed %g\n", p.ach_damage_healed));
	return s;
}

void
ACH_Save(entity p)
{
	if (clienttype(p) == CLIENTTYPE_BOT) return;

	string text = ACH_Text(p);
	STDB_SaveSection(p, "save_achievements", text);

	string path = ACH_SavePath(p);
	filestream f = fopen(path, FILE_WRITE);
	if (f < 0) {
		print(sprintf("achievements: ERROR saving %s\n", path));
		return;
	}
	fputs(f, text);
	fclose(f);
}

/* --- core achievement logic --- */

/* award a completed achievement */
void
ACH_Award(entity p, float achId)
{
	if (achId < 0 || achId >= ACH_MAX) return;
	if (ach_completed(p, achId)) return;	/* already earned */
	if (clienttype(p) == CLIENTTYPE_BOT) return;

	ach_completed(p, achId) = 1;
	float xp = ACH_XPReward(achId);

	sprint(p, PRINT_HIGH, sprintf(
		"\x02ACHIEVEMENT UNLOCKED: %s! (+%g XP)\n",
		ACH_Name(achId), xp));

	PROG_AwardXP(p, xp);
	ACH_Save(p);
}

/* increment progress toward an achievement, auto-award when threshold met */
void
ACH_Check(entity p, float achId, float progress)
{
	if (achId < 0 || achId >= ACH_MAX) return;
	if (ach_completed(p, achId)) return;
	if (clienttype(p) == CLIENTTYPE_BOT) return;

	ach_progress(p, achId) += progress;

	float threshold = ACH_Threshold(achId);
	if (ach_progress(p, achId) >= threshold)
		ACH_Award(p, achId);
}

/* set progress to exact value (for bitfield-style achievements) */
void
ACH_SetProgress(entity p, float achId, float value)
{
	if (achId < 0 || achId >= ACH_MAX) return;
	if (ach_completed(p, achId)) return;
	if (clienttype(p) == CLIENTTYPE_BOT) return;

	ach_progress(p, achId) = value;

	float threshold = ACH_Threshold(achId);
	if (ach_progress(p, achId) >= threshold)
		ACH_Award(p, achId);
}

/* --- event hooks (called from match framework) --- */

void
ACH_OnKill(entity killer, entity victim)
{
	if (killer == victim) return;
	if (!is.Player(killer)) return;
	if (clienttype(killer) == CLIENTTYPE_BOT) return;

	killer.ach_kills++;
	killer.ach_kills_life++;
	killer.ach_kills_round++;

	/* First Blood */
	ACH_Check(killer, ACH_FIRST_BLOOD, 1);

	/* Rampage: 5 kills without dying */
	if (killer.ach_kills_life >= 5)
		ACH_Check(killer, ACH_RAMPAGE, 1);

	/* Kill Streak: 10 kills in one round */
	if (killer.ach_kills_round >= 10)
		ACH_Check(killer, ACH_KILLSTREAK, 1);
}

void
ACH_OnHeadshot(entity killer)
{
	if (clienttype(killer) == CLIENTTYPE_BOT) return;
	killer.ach_headshots++;
	ACH_Check(killer, ACH_HEADHUNTER, 1);
}

void
ACH_OnDeath(entity victim)
{
	if (clienttype(victim) == CLIENTTYPE_BOT) return;
	/* reset per-life counters */
	victim.ach_kills_life = 0;
}

void
ACH_OnZombieKill(entity killer)
{
	if (clienttype(killer) == CLIENTTYPE_BOT) return;
	killer.ach_zombie_kills++;
	killer.ach_zombie_kills_match++;

	ACH_Check(killer, ACH_ZOMBIE_SLAYER, 1);
	ACH_Check(killer, ACH_EXTERMINATOR, 1);
	ACH_Check(killer, ACH_INFECTION_STOP, 1);

	/* Zombie King: 50 zombie kills in one match */
	if (killer.ach_zombie_kills_match >= 50)
		ACH_Check(killer, ACH_ZOMBIE_KING, 1);
}

void
ACH_OnBombPlant(entity planter)
{
	if (clienttype(planter) == CLIENTTYPE_BOT) return;
	planter.ach_bombs_planted++;
	ACH_Check(planter, ACH_BOMB_EXPERT, 1);
}

void
ACH_OnBombDefuse(entity defuser)
{
	if (clienttype(defuser) == CLIENTTYPE_BOT) return;
	defuser.ach_bombs_defused++;
	ACH_Check(defuser, ACH_DEFUSER, 1);
}

void
ACH_OnHostageRescue(entity rescuer)
{
	if (clienttype(rescuer) == CLIENTTYPE_BOT) return;
	rescuer.ach_hostages_rescued++;
	ACH_Check(rescuer, ACH_RESCUER, 1);
}

void
ACH_OnDamageDealt(entity attacker, float amount)
{
	if (clienttype(attacker) == CLIENTTYPE_BOT) return;
	attacker.ach_damage_dealt += amount;
	ACH_Check(attacker, ACH_DAMAGE_DEALER, amount);

	/* Overkill: 500+ damage in a single hit */
	if (amount >= 500)
		ACH_Check(attacker, ACH_OVERKILL, 1);
}

void
ACH_OnDamageTaken(entity victim, float amount)
{
	if (clienttype(victim) == CLIENTTYPE_BOT) return;
	victim.ach_damage_taken_round += amount;
	ACH_Check(victim, ACH_IRONWALL, amount);
}

void
ACH_OnHeal(entity healer, float amount)
{
	if (clienttype(healer) == CLIENTTYPE_BOT) return;
	healer.ach_damage_healed += amount;
	ACH_Check(healer, ACH_MEDIC, amount);
}

void
ACH_OnShot(entity shooter, bool hit)
{
	if (clienttype(shooter) == CLIENTTYPE_BOT) return;
	shooter.ach_shots_fired++;
	if (hit)
		shooter.ach_shots_hit++;
}

/* hitscan attacks since the last call, from Nuclide's stat_shots* counters */
void
ACH_OnShots(entity shooter, float fired, float hit)
{
	if (clienttype(shooter) == CLIENTTYPE_BOT) return;
	shooter.ach_shots_fired += fired;
	shooter.ach_shots_hit += hit;
}

void
ACH_OnMVP(entity player)
{
	if (clienttype(player) == CLIENTTYPE_BOT) return;
	ACH_Check(player, ACH_MVP_KING, 1);
}

void
ACH_OnRoundSurvived(entity player)
{
	if (clienttype(player) == CLIENTTYPE_BOT) return;
	player.ach_rounds_survived++;
	ACH_Check(player, ACH_SURVIVOR, 1);

	/* Untouchable: survived a round with zero damage taken */
	if (player.ach_damage_taken_round == 0)
		ACH_Check(player, ACH_UNTOUCHABLE, 1);
}

void
ACH_OnRoundStart(void)
{
	/* reset per-round counters for all players */
	for (entity p = world; (p = next.Player(p));) {
		p.ach_kills_round = 0;
		p.ach_damage_taken_round = 0;
	}
}

void
ACH_OnZombieRoundSurvived(entity player)
{
	if (clienttype(player) == CLIENTTYPE_BOT) return;
	ACH_Check(player, ACH_HORDE_BREAKER, 1);
}

void
ACH_OnLastStandWin(entity player)
{
	if (clienttype(player) == CLIENTTYPE_BOT) return;
	ACH_Check(player, ACH_LAST_STAND, 1);
}

void
ACH_OnMatchEnd(entity player, float mode, bool won)
{
	if (clienttype(player) == CLIENTTYPE_BOT) return;

	/* track modes played (bitfield) */
	float modeBit = (float)((int)1 << (int)min(mode, 7));
	player.ach_modes_played = (float)((int)player.ach_modes_played | (int)modeBit);
	ACH_SetProgress(player, ACH_ALL_STAR, player.ach_modes_played);

	/* Veteran: play 500 matches */
	ACH_Check(player, ACH_VETERAN, 1);

	if (won) {
		player.ach_matches_won++;

		/* Team Player: win 100 team matches */
		ACH_Check(player, ACH_TEAM_PLAYER, 1);

		/* Mode Master: win 50 in any mode */
		ACH_Check(player, ACH_MODE_MASTER, 1);

		/* mode-specific win achievements */
		if (mode == 1 || mode == 2)	/* DM / TDM */
			ACH_Check(player, ACH_MASTER_BLASTER, 1);
		else if (mode == 3)		/* bomb/defusal */
			ACH_Check(player, ACH_TACTICIAN, 1);
		else if (mode == 4)		/* hostage */
			ACH_Check(player, ACH_HOSTAGE_HERO, 1);
	}

	/* Sharp Shooter: 90% accuracy in a match (min 50 shots) */
	if (player.ach_shots_fired >= 50) {
		float accuracy = player.ach_shots_hit / player.ach_shots_fired;
		if (accuracy >= 0.9)
			ACH_Check(player, ACH_SHARPSHOOTER, 1);
	}

	/* Legend: reached level 50 */
	if (player.prog_level >= 50)
		ACH_Check(player, ACH_LEGEND, 1);

	/* Endurance: cumulative playtime check */
	ACH_SetProgress(player, ACH_ENDURANCE, player.prog_playtime);

	/* reset per-match counters */
	player.ach_shots_fired = 0;
	player.ach_shots_hit = 0;
	player.ach_zombie_kills_match = 0;

	ACH_Save(player);
}

/* --- client commands --- */

bool
ACH_Command(entity p, string cmd)
{
	tokenize(cmd);
	string c = argv(0);

	if (c != "csnz_achievements")
		return false;

	float completed = 0;
	for (float i = 0; i < ACH_MAX; i++)
		if (ach_completed(p, i))
			completed++;

	sprint(p, PRINT_HIGH, sprintf(
		"=== Achievements (%g/%g completed) ===\n",
		completed, (float)ACH_MAX));

	string lastCat = "";
	for (float i = 0; i < ACH_MAX; i++) {
		string cat = ACH_Category(i);
		if (cat != lastCat) {
			sprint(p, PRINT_HIGH, sprintf("\n--- %s ---\n", cat));
			lastCat = cat;
		}

		if (ach_completed(p, i)) {
			sprint(p, PRINT_HIGH, sprintf(
				"  [X] %s\n", ACH_Name(i)));
		} else {
			float threshold = ACH_Threshold(i);
			float progress = ach_progress(p, i);
			float pct = min(100, floor(progress / threshold * 100));
			sprint(p, PRINT_HIGH, sprintf(
				"  [ ] %s (%g/%g - %g%%)\n",
				ACH_Name(i), progress, threshold, pct));
		}
	}

	/* cumulative stats summary */
	/* split in two: sprintf takes at most 8 arguments */
	sprint(p, PRINT_HIGH, sprintf(
		"\n--- Lifetime Stats ---\n"
		"  Kills: %g | Headshots: %g | Zombie Kills: %g\n"
		"  Rounds Survived: %g | Bombs Planted: %g | Bombs Defused: %g\n",
		p.ach_kills, p.ach_headshots, p.ach_zombie_kills,
		p.ach_rounds_survived, p.ach_bombs_planted, p.ach_bombs_defused));
	sprint(p, PRINT_HIGH, sprintf(
		"  Hostages Rescued: %g | Matches Won: %g\n"
		"  Damage Dealt: %g | Damage Healed: %g\n",
		p.ach_hostages_rescued, p.ach_matches_won,
		p.ach_damage_dealt, p.ach_damage_healed));

	return true;
}

/* --- hooks for player connect/disconnect --- */

void
ACH_PlayerConnect(entity p)
{
	ACH_Load(p);
}

void
ACH_PlayerDisconnect(entity p)
{
	ACH_Save(p);
}
