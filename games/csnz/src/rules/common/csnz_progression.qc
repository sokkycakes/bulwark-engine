/* csnz_progression: local player progression system.
 *
 * Provides offline XP, leveling, rank, and basic unlock tracking.
 * All data persisted to data/csnz/players/<steamid>.txt or a hash of
 * the player's name when no Steam ID is available.
 *
 * Original code (ISC). DESIGN numbers throughout - CSNZ's real progression
 * was server-authoritative and these are approximations. */

/* --- constants --- */

#define PROG_MAX_LEVEL 100
#define PROG_MAX_RANK 18

/* XP required per level: base 1000 + 500 per level (DESIGN) */
float
PROG_XPForLevel(int level)
{
	return 1000 + level * 500;
}

/* rank names (DESIGN approximation of CSO rank tiers) */
string
PROG_RankName(int rank)
{
	switch (rank) {
	case 0: return "Trainee";
	case 1: return "Amateur";
	case 2: return "Regular";
	case 3: return "Semi-Pro";
	case 4: return "Professional";
	case 5: return "Veteran";
	case 6: return "Expert";
	case 7: return "Elite";
	case 8: return "Master";
	case 9: return "Grand Master";
	case 10: return "Champion";
	case 11: return "Hero";
	case 12: return "Legend";
	case 13: return "Mythic";
	case 14: return "Immortal";
	case 15: return "Titan";
	case 16: return "God";
	case 17: return "Transcendent";
	default: return "Unknown";
	}
}

/* rank thresholds by level (DESIGN: rank up every ~6 levels) */
int
PROG_RankForLevel(int level)
{
	return min(PROG_MAX_RANK - 1, (int)floor(level / 6));
}

/* --- per-player progression data (entity fields) --- */
.float prog_level;
.float prog_xp;
.float prog_xp_total;
.float prog_rank;
.float prog_kills_total;
.float prog_deaths_total;
.float prog_wins_total;
.float prog_matches_total;
.float prog_playtime;		/* seconds */
.float prog_money;		/* in-game currency */

/* --- file I/O --- */

/* generate a save filename for a player */
string
PROG_SavePath(entity p)
{
	/* use netname hash as fallback ID */
	string id = p.netname;
	/* sanitize: only keep alphanumeric */
	string safe = "";
	for (float i = 0; i < strlen(id) && i < 32; i++) {
		float ch = str2chr(id, i);
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
			safe = strcat(safe, chr2str(ch));
	}
	if (safe == "") safe = "default";
	return sprintf("data/csnz/players/%s.txt", safe);
}

void
PROG_Defaults(entity p)
{
	p.prog_level = 1;
	p.prog_xp = 0;
	p.prog_xp_total = 0;
	p.prog_rank = 0;
	p.prog_kills_total = 0;
	p.prog_deaths_total = 0;
	p.prog_wins_total = 0;
	p.prog_matches_total = 0;
	p.prog_playtime = 0;
	p.prog_money = 10000;	/* DESIGN: starting money */
}

/* one "key value" line of the save file (or of the SpacetimeDB profile) */
void
PROG_ParseLine(entity p, string line)
{
	float n = tokenize(line);
	if (n < 2) return;
	string key = argv(0);
	string val = argv(1);

	if (key == "level") p.prog_level = stof(val);
		else if (key == "xp") p.prog_xp = stof(val);
		else if (key == "xp_total") p.prog_xp_total = stof(val);
		else if (key == "rank") p.prog_rank = stof(val);
		else if (key == "kills") p.prog_kills_total = stof(val);
		else if (key == "deaths") p.prog_deaths_total = stof(val);
		else if (key == "wins") p.prog_wins_total = stof(val);
		else if (key == "matches") p.prog_matches_total = stof(val);
		else if (key == "playtime") p.prog_playtime = stof(val);
	else if (key == "money") p.prog_money = stof(val);
}

void
PROG_Load(entity p)
{
	string path = PROG_SavePath(p);
	filestream f = fopen(path, FILE_READ);

	PROG_Defaults(p);
	if (f < 0) {
		print(sprintf("progression: new player %s\n", p.netname));
		return;
	}

	string line;
	while ((line = fgets(f)))
		PROG_ParseLine(p, line);
	fclose(f);
	print(sprintf("progression: loaded %s (level %g, rank %s)\n",
		p.netname, p.prog_level, PROG_RankName((int)p.prog_rank)));
}

/* the save file's text */
string
PROG_Text(entity p)
{
	return strcat(sprintf("level %g\nxp %g\nxp_total %g\nrank %g\n", p.prog_level, p.prog_xp, p.prog_xp_total, p.prog_rank),
		sprintf("kills %g\ndeaths %g\nwins %g\n", p.prog_kills_total, p.prog_deaths_total, p.prog_wins_total),
		sprintf("matches %g\nplaytime %g\nmoney %g\n", p.prog_matches_total, p.prog_playtime, p.prog_money));
}

void
PROG_Save(entity p)
{
	if (clienttype(p) == CLIENTTYPE_BOT) return;

	string text = PROG_Text(p);
	STDB_SaveSection(p, "save_progression", text);

	string path = PROG_SavePath(p);
	filestream f = fopen(path, FILE_WRITE);
	if (f < 0) {
		print(sprintf("progression: ERROR saving %s\n", path));
		return;
	}
	fputs(f, text);
	fclose(f);
}

/* --- XP and leveling --- */

/* award XP and check for level up */
void
PROG_AwardXP(entity p, float amount)
{
	if (clienttype(p) == CLIENTTYPE_BOT) return;
	if (amount <= 0) return;

	p.prog_xp += amount;
	p.prog_xp_total += amount;

	/* check for level up */
	while (p.prog_level < PROG_MAX_LEVEL) {
		float needed = PROG_XPForLevel((int)p.prog_level);
		if (p.prog_xp < needed) break;

		p.prog_xp -= needed;
		p.prog_level++;

		int newRank = PROG_RankForLevel((int)p.prog_level);
		if (newRank > p.prog_rank) {
			p.prog_rank = newRank;
			sprint(p, PRINT_HIGH, sprintf("RANK UP! You are now %s!\n",
				PROG_RankName(newRank)));
		} else {
			sprint(p, PRINT_HIGH, sprintf("LEVEL UP! Level %g\n", p.prog_level));
		}
	}

	/* publish to HUD */
	userinfo.SetInteger(p, "csnz_level", (int)p.prog_level);
	userinfo.SetInteger(p, "csnz_rank", (int)p.prog_rank);
}

/* standard XP awards (DESIGN values) */
#define XP_KILL			50
#define XP_ASSIST		25
#define XP_HEADSHOT		20	/* bonus on top of kill XP */
#define XP_ROUND_WIN		100
#define XP_ROUND_LOSE		30
#define XP_MATCH_WIN		300
#define XP_MATCH_LOSE		100
#define XP_BOMB_PLANT		75
#define XP_BOMB_DEFUSE		100
#define XP_HOSTAGE_RESCUE	75
#define XP_MVP			150
#define XP_ZOMBIE_INFECT	40
#define XP_SURVIVE_ROUND	50

/* called when a player gets a kill */
void
PROG_OnKill(entity killer, entity victim)
{
	if (killer == victim) return;
	if (!is.Player(killer)) return;

	killer.prog_kills_total++;
	PROG_AwardXP(killer, XP_KILL);
	PROG_AwardMoney(killer, 300);	/* DESIGN */

	if (is.Player(victim))
		victim.prog_deaths_total++;
}

/* called at round end */
void
PROG_OnRoundEnd(float winningTeam)
{
	for (entity p = world; (p = next.Player(p));) {
		if (clienttype(p) == CLIENTTYPE_BOT) continue;
		if (p.team == winningTeam) {
			PROG_AwardXP(p, XP_ROUND_WIN);
			p.prog_wins_total++;
		} else {
			PROG_AwardXP(p, XP_ROUND_LOSE);
		}
	}
}

/* called at match end */
void
PROG_OnMatchEnd(float winningTeam)
{
	for (entity p = world; (p = next.Player(p));) {
		if (clienttype(p) == CLIENTTYPE_BOT) continue;
		p.prog_matches_total++;
		if (p.team == winningTeam)
			PROG_AwardXP(p, XP_MATCH_WIN);
		else
			PROG_AwardXP(p, XP_MATCH_LOSE);
		PROG_Save(p);
	}
}

/* --- money system --- */

void
PROG_AwardMoney(entity p, float amount)
{
	if (clienttype(p) == CLIENTTYPE_BOT) return;
	p.prog_money = min(999999, p.prog_money + amount);
	userinfo.SetInteger(p, "csnz_money", (int)p.prog_money);
}

bool
PROG_SpendMoney(entity p, float amount)
{
	if (p.prog_money < amount) return false;
	p.prog_money -= amount;
	userinfo.SetInteger(p, "csnz_money", (int)p.prog_money);
	return true;
}

/* --- client commands --- */

bool
PROG_Command(entity p, string cmd)
{
	tokenize(cmd);
	string c = argv(0);

	if (c == "csnz_profile") {
		/* split in two: sprintf takes at most 8 arguments */
		sprint(p, PRINT_HIGH, sprintf(
			"=== %s ===\n"
			"Level: %g | Rank: %s | XP: %g/%g\n",
			p.netname,
			p.prog_level, PROG_RankName((int)p.prog_rank),
			p.prog_xp, PROG_XPForLevel((int)p.prog_level)));
		sprint(p, PRINT_HIGH, sprintf(
			"K/D: %g/%g | Wins: %g | Matches: %g\n"
			"Money: $%g\n",
			p.prog_kills_total, p.prog_deaths_total,
			p.prog_wins_total, p.prog_matches_total,
			p.prog_money));
		return true;
	}
	return false;
}
