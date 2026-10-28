/* csnz_common: the shared match framework for every CSNZ mode in csnz.dat
 * and studio.dat. Original code (ISC), no CSNZ code or data is embedded.
 *
 * What it provides
 *   - mode registration: each mode fills in the hook variables below
 *   - room limits: win/kill/time/round limits from the client's own
 *     resource/gamemodelist.csv + limitlist.csv when present (OBSERVED
 *     values), else the mode's fallback numbers (DESIGN)
 *   - match and round state: wait -> freeze -> live -> roundend -> ... ->
 *     matchend, with the serverinfo keys of INTERFACES.md section 3
 *   - teams, spawn selection, respawn with spawn protection, loadouts,
 *     bot fill, banners and the CSO localisation text
 *   - test hooks (cs_test_*) for the headless runs in tests/modes
 *
 * Team numbers: 1 = TR / zombie / seeker, 2 = CT / human / hider. */

#define CS_TEAM_NONE 0
#define CS_TEAM1 1
#define CS_TEAM2 2

enum
{
	CS_WAIT,	/* not enough players */
	CS_FREEZE,	/* round about to start: spawned, frozen */
	CS_LIVE,
	CS_ROUNDEND,
	CS_MATCHEND
};

/* --- room settings (the Create Room menu writes these) ------------------- */
var float autocvar_csnz_mode = 1;
var float autocvar_mp_winlimit = 0;	/* rounds; 0 = use the mode's table default */
var float autocvar_mp_fraglimit = 0;	/* kills */
var float autocvar_mp_timelimit = 0;	/* minutes */
var float autocvar_mp_roundtime = 0;	/* minutes */
var float autocvar_mp_freezetime = 3;	/* DESIGN: CSO's freeze time is not in the client tables */
var float autocvar_csnz_bots = 0;	/* fill the room with bots up to this many players */
var float autocvar_csnz_min_players = 1;
var float autocvar_csnz_spawnprotect = 3;	/* OBSERVED: "invincible for 3 seconds when you respawn" */
var float autocvar_csnz_end_time = 5;	/* DESIGN: round result display time */
var float autocvar_csnz_intermission = 10;
var bool autocvar_csnz_debug = false;

/* --- mode hooks (set by each mode's init function) ---------------------- */
var void(void) M_Start;			/* once, after StartGameType */
var void(void) M_RoundStart;		/* each round, players already respawned */
var void(void) M_Live;			/* freeze time is over */
var void(void) M_Frame;			/* every frame while live */
var void(entity) M_Equip;		/* class and loadout of a (re)spawning player */
var float(entity) M_PickTeam;		/* team for a joining player */
var void(entity, entity, entity, string) M_Killed;	/* victim, inflictor, attacker, weapon */
var void(entity, entity) M_Damaged;	/* victim, attacker; victim survived */
var float(void) M_CheckRound;		/* 0 = continue, 1/2 = that team won, 3 = draw */
var void(float) M_RoundEnded;		/* winner */
var void(entity) M_Disconnect;
var float(entity, string) M_Command;
var bool(entity) M_LateJoin;		/* a player joining a live round; true if placed */	/* player or world, the full command; true if handled */

/* mode description */
string cs_modeTitle;		/* localisation key, e.g. "CSO_GameMode_DeathMatch" */
string cs_modeFallback;		/* English fallback */
bool cs_teams;			/* two teams (else free for all) */
bool cs_rounds;			/* round based */
float cs_respawnDelay;		/* < 0: dead players wait for the next round */
bool cs_scoreKills;		/* score is kills (else round wins) */
bool cs_oneSideStart;		/* starts with everyone on one team (infection modes) */
float cs_minPlayers;
bool cs_freezeLock;		/* freeze time holds players still (Original family) */
bool cs_scorePoints;		/* score is each player's mode points (.cs_score) */
float cs_winLimitFallback;
float cs_killLimitFallback;
float cs_timeLimitFallback;	/* minutes */
float cs_roundTimeFallback;	/* minutes */
string cs_team1Name;
string cs_team2Name;
bool cs_roundTimeWinsTeam2;	/* time running out is a win for team 2 (else draw) */

/* --- state --------------------------------------------------------------- */
int cs_state;
int cs_round;
float cs_stateEnd;
float cs_matchEnd;
float cs_score1, cs_score2;
float cs_goal;			/* win target of the match: rounds or kills */
float cs_roundTime;		/* seconds */
float cs_timeLimit;		/* minutes, 0 = none */
float cs_nextInfo;
int cs_msgseq;
entity cs_banner;
entity cs_center;
hashtable cs_text;
.float cs_protectEnd;
.float cs_respawnAt;
.float cs_lastTeam;
.float cs_kills;		/* kills this match (frags can be changed by modes) */
.float cs_money;
.float cs_score;		/* mode points (soccer goals, gun game level, ...) */
.float cs_deaths;

/* --- localisation: the CSO text the client already ships ----------------- */

/* Reads every "key" "value" pair; values may span lines (span mode). */
void
CS_LoadText(void)
{
	filestream f;
	string s, pending = "";
	int loaded = 0;

	cs_text = hash_createtab(4096, EV_STRING);
	f = fopen("resource/cso_na_en.utf8.txt", FILE_READ);

	if (f < 0) {
		print("csnz: resource/cso_na_en.utf8.txt not found, using English fallbacks\n");
		return;
	}

	while (1) {
		s = fgets(f);
		if not (s)	/* end of file; "" is a blank line */
			break;
		if (pending != "") {
			s = strcat(pending, "\n", s);
			pending = "";
		}

		/* a value still open at the end of the line continues on the next */
		if (tokenize_console(s) < 2) {
			float quotes = 0;
			for (float i = 0; i < strlen(s); i++)
				if (str2chr(s, i) == '"')
					quotes++;
			if (quotes == 3)
				pending = s;
			continue;
		}

		string key = strtolower(argv(0));
		if (substring(key, 0, 4) != "cso_" && substring(key, 0, 4) != "vxl_")
			continue;
		/* the first definition wins (some keys are defined twice) */
		if (hash_get(cs_text, key, "", EV_STRING) == "") {
			hash_add(cs_text, key, strzone(argv(1)), EV_STRING);
			loaded++;
		}
	}

	fclose(f);
	printf("csnz: %i localisation keys loaded\n", loaded);
}

string
CS_Text(string key, string fallback)
{
	string s;

	if (!cs_text)
		return fallback;

	s = hash_get(cs_text, strtolower(key), "", EV_STRING);
	if (s == "")
		return fallback;
	return strreplace("\\n", "\n", s);
}

/* --- client tables: room limits ---------------------------------------- */

/* Returns the default value of a limit id in limitlist.csv, or -1. */
float
CS_LimitDefault(float limitId)
{
	filestream f;
	string s;
	float val = -1;

	if (limitId <= 0)
		return -1;

	f = fopen("resource/limitlist.csv", FILE_READ);
	if (f < 0)
		return -1;

	while (1) {
		s = fgets(f);
		if not (s)	/* end of file; "" is a blank line */
			break;
		if (substring(s, 0, 1) == "#")
			continue;
		if (tokenizebyseparator(s, ",") < 4)
			continue;
		if (stof(argv(0)) == limitId) {
			val = stof(argv(3));
			break;
		}
	}

	fclose(f);
	return val;
}

/* Reads the mode's row of gamemodelist.csv into the four limit ids. */
bool
CS_ModeLimitIds(float modeId, __out float winId, __out float killId, __out float timeId, __out float roundId)
{
	filestream f;
	string s;
	bool found = false;

	winId = killId = timeId = roundId = 0;
	f = fopen("resource/gamemodelist.csv", FILE_READ);
	if (f < 0)
		return false;

	while (1) {
		s = fgets(f);
		if not (s)	/* end of file; "" is a blank line */
			break;
		if (substring(s, 0, 1) == "#")
			continue;
		if (tokenizebyseparator(s, ",") < 10)
			continue;
		if (stof(argv(0)) == modeId) {
			winId = stof(argv(6));
			killId = stof(argv(7));
			timeId = stof(argv(8));
			roundId = stof(argv(9));
			found = true;
			break;
		}
	}

	fclose(f);
	return found;
}

/* room value if set, else the table default, else the mode's fallback */
float
CS_Limit(float roomValue, float limitId, float fallback, string what)
{
	float tableValue;

	if (roomValue > 0)
		return roomValue;

	tableValue = CS_LimitDefault(limitId);
	if (tableValue >= 0) {
		printf("csnz: %s %g (limitlist.csv id %g)\n", what, tableValue, limitId);
		return tableValue;
	}

	return fallback;
}

/* --- messages ------------------------------------------------------------ */

void
CS_Banner(string msg)
{
	printf("csnz %.1f: %s\n", time, msg);
	cs_msgseq++;
	serverinfo.SetString("csnz_msg", msg);
	serverinfo.SetInteger("csnz_msgseq", cs_msgseq);
	ents.Input(cs_banner, "SetText", msg, world);
	ents.Input(cs_banner, "Display", "", world);
}

void
CS_Tell(entity p, string msg)
{
	if (autocvar_csnz_debug)
		printf("csnz %.1f: to %s: %s\n", time, p.netname, msg);
	ents.Input(cs_center, "SetText", msg, world);
	ents.Input(cs_center, "Display", "", p);
}

/* --- players and teams ---------------------------------------------------- */

bool
CS_Playing(entity p)
{
	return (p.classname != "spectator" && p.health > 0);
}

int
CS_Players(void)
{
	int n = 0;
	for (entity p = world; (p = next.Player(p));)
		n++;
	return n;
}

int
CS_Alive(float whichTeam)
{
	int n = 0;
	for (entity p = world; (p = next.Player(p));)
		if (CS_Playing(p) && (!cs_teams || p.team == whichTeam))
			n++;
	return n;
}

int
CS_TeamSize(float whichTeam)
{
	int n = 0;
	for (entity p = world; (p = next.Player(p));)
		if (p.team == whichTeam)
			n++;
	return n;
}

/* the smaller team, ties broken at random */
float
CS_AutoTeam(entity p)
{
	int a = 0, b = 0;

	for (entity o = world; (o = next.Player(o));) {
		if (o == p)
			continue;
		if (o.team == CS_TEAM1)
			a++;
		else if (o.team == CS_TEAM2)
			b++;
	}

	if (a == b)
		return random() < 0.5 ? CS_TEAM1 : CS_TEAM2;
	return a < b ? CS_TEAM1 : CS_TEAM2;
}

void
CS_SetTeam(entity p, float whichTeam)
{
	p.cs_lastTeam = whichTeam;
	ents.Input(p, "SetTeam", ftos(whichTeam), world);
	p.team = whichTeam;
}

/* --- money (Original family). STOCK-CS amounts: CSO's own values are not
   in the client tables. Published to the HUD as userinfo csnz_money. --- */
var float autocvar_mp_startmoney = 800;	/* OBSERVED: "initial possession amount of $800" */
#define CS_MONEY_MAX 16000

void
CS_SetMoney(entity p, float amount)
{
	p.cs_money = bound(0, amount, CS_MONEY_MAX);
	userinfo.SetInteger(p, "csnz_money", (int)p.cs_money);
}

void
CS_GiveMoney(entity p, float amount)
{
	CS_SetMoney(p, p.cs_money + amount);
}

void
CS_TeamMoney(float whichTeam, float amount)
{
	for (entity p = world; (p = next.Player(p));)
		if (p.team == whichTeam)
			CS_GiveMoney(p, amount);
}

/* --- spawning -------------------------------------------------------------- */

/* spawn point classes for a team, most specific first. CS maps use
   info_player_deathmatch for terrorists and info_player_start for CTs. */
string
CS_SpawnClasses(float whichTeam)
{
	/* Studio PLAYER_SPAWN blocks (csnz_vxspawn*, made while blocks are live) win */
	if (!cs_teams)
		return "csnz_vxspawn csnz_vxspawn_t1 csnz_vxspawn_t2 info_player_deathmatch info_player_start info_player_terrorist info_player_counterterrorist";
	if (whichTeam == CS_TEAM1)
		return "csnz_vxspawn_t1 csnz_vxspawn info_player_terrorist info_player_deathmatch info_player_start";
	return "csnz_vxspawn_t2 csnz_vxspawn info_player_counterterrorist info_player_start info_player_deathmatch";
}

/* a random free spot of the first class that exists */
entity
CS_PickSpawn(float whichTeam)
{
	string classes = CS_SpawnClasses(whichTeam);
	int n = tokenize(classes);

	for (int c = 0; c < n; c++) {
		string cname = argv(c);
		int count = 0, freeCount = 0;
		entity e;

		for (e = world; (e = find(e, ::classname, cname));)
			count++;
		if (!count)
			continue;

		/* prefer spots no living player stands near */
		for (e = world; (e = find(e, ::classname, cname));) {
			bool busy = false;
			for (entity p = world; (p = next.Player(p));)
				if (CS_Playing(p) && vlen(p.origin - e.origin) < 48)
					busy = true;
			if (!busy)
				freeCount++;
		}

		int pick = (int)floor(random() * (freeCount ? freeCount : count));
		for (e = world; (e = find(e, ::classname, cname));) {
			if (freeCount) {
				bool busy2 = false;
				for (entity p2 = world; (p2 = next.Player(p2));)
					if (CS_Playing(p2) && vlen(p2.origin - e.origin) < 48)
						busy2 = true;
				if (busy2)
					continue;
			}
			if (pick-- == 0)
				return e;
		}
	}

	return world;
}

/* Some GoldSrc spawn points sit inside the world for Nuclide's player box;
   move up or down to the nearest spot where the box fits (as zombi.qc). */
void
CS_Unstick(entity p)
{
	tracebox(p.origin, p.mins, p.maxs, p.origin, MOVE_NOMONSTERS, p);
	if (!trace_startsolid)
		return;

	for (float dz = 4; dz <= 128; dz += 4) {
		for (float sign = -1; sign <= 1; sign += 2) {
			vector test = p.origin + [0, 0, dz * sign];
			tracebox(test, p.mins, p.maxs, test, MOVE_NOMONSTERS, p);
			if (!trace_startsolid) {
				tracebox(test, p.mins, p.maxs, test - [0, 0, 256], MOVE_NOMONSTERS, p);
				setorigin(p, trace_endpos);
				return;
			}
		}
	}
}

void
CS_MoveToSpot(entity p, vector org, vector ang)
{
	setorigin(p, org);
	p.angles = p.v_angle = ang;
	p.fixangle = 1;
	p.velocity = [0, 0, 0];

	WriteByte(MSG_MULTICAST, SVC_CGAMEPACKET);
	WriteByte(MSG_MULTICAST, EV_ANGLE);
	WriteFloat(MSG_MULTICAST, ang[0]);
	WriteFloat(MSG_MULTICAST, ang[1]);
	WriteFloat(MSG_MULTICAST, ang[2]);
	msg_entity = p;
	multicast([0, 0, 0], MULTICAST_ONE_R);
	CS_Unstick(p);
}

/* class, team, loadout, position */
void
CS_PutInGame(entity p)
{
	entity spot;

	if (cs_teams && p.team != CS_TEAM1 && p.team != CS_TEAM2)
		CS_SetTeam(p, M_PickTeam ? M_PickTeam(p) : CS_AutoTeam(p));
	else if (!cs_teams && p.team != CS_TEAM_NONE)
		CS_SetTeam(p, CS_TEAM_NONE);

	CSNZ_PreEquip(p);
	if (M_Equip)
		M_Equip(p);
	else
		ents.ChangeToClass(p, "csm_rifleman");
	CSNZ_PostEquip(p);

	ents.Input(p, "SetTeam", ftos(p.team), world);
	spot = CS_PickSpawn(p.team);
	if (spot)
		CS_MoveToSpot(p, spot.origin, spot.angles);
	else
		game.TeleportToSpawn(p);

	p.cs_respawnAt = 0;
	p.zv_lastHealth = p.health;
	if (autocvar_csnz_spawnprotect > 0) {
		p.cs_protectEnd = time + autocvar_csnz_spawnprotect;
		p.takedamage = DAMAGE_NO;
	}
}

/* --- serverinfo for the HUD (INTERFACES.md section 3) --------------------- */

string
CS_StateName(void)
{
	switch (cs_state) {
	case CS_WAIT: return "wait";
	case CS_FREEZE: return "freeze";
	case CS_LIVE: return "live";
	case CS_ROUNDEND: return "roundend";
	default: return "matchend";
	}
}

void
CS_PublishInfo(void)
{
	serverinfo.SetString("csnz_state", CS_StateName());
	serverinfo.SetFloat("csnz_stateend", cs_stateEnd);
	serverinfo.SetInteger("csnz_round", cs_round);
	serverinfo.SetString("csnz_t1", cs_teams ? cs_team1Name : "");
	serverinfo.SetString("csnz_t2", cs_teams ? cs_team2Name : "");
	serverinfo.SetInteger("csnz_s1", (int)cs_score1);
	serverinfo.SetInteger("csnz_s2", (int)cs_score2);
	serverinfo.SetInteger("csnz_a1", CS_Alive(cs_teams ? CS_TEAM1 : CS_TEAM_NONE));
	serverinfo.SetInteger("csnz_a2", cs_teams ? CS_Alive(CS_TEAM2) : 0);
	serverinfo.SetInteger("csnz_goal", (int)cs_goal);
}

/* --- rounds and the match ------------------------------------------------ */

void CS_EndMatch(string why);
void CS_StatsRecordMatch(float winnerTeam, entity ffaWinner);

void
CS_StartRound(void)
{
	cs_round++;
	cs_state = CS_FREEZE;
	cs_stateEnd = time + autocvar_mp_freezetime;

	for (entity p = world; (p = next.Player(p));) {
		/* a fresh body each round in round modes */
		if (p.health > 0 && cs_rounds) {
			p.cs_respawnAt = 0;
		}
		CS_PutInGame(p);
	}

	if (M_RoundStart)
		M_RoundStart();
	ACH_OnRoundStart();

	printf("csnz %.1f: round %i, %i players\n", time, cs_round, CS_Players());
	CS_PublishInfo();
}

void
CS_GoLive(void)
{
	cs_state = CS_LIVE;
	cs_stateEnd = cs_roundTime > 0 ? time + cs_roundTime : 0;
	if (M_Live)
		M_Live();
	CS_PublishInfo();
}

void
CS_EndRound(float winner, string why)
{
	if (cs_state != CS_LIVE)
		return;

	cs_state = CS_ROUNDEND;
	cs_stateEnd = time + autocvar_csnz_end_time;

	if (!cs_scoreKills) {
		if (winner == CS_TEAM1)
			cs_score1++;
		else if (winner == CS_TEAM2)
			cs_score2++;
	}

	printf("csnz %.1f: round %i over (%s), winner %g, score %g:%g\n", time, cs_round, why, winner, cs_score1, cs_score2);
	CSNZ_RoundEnded(winner);

	if (M_RoundEnded)
		M_RoundEnded(winner);
	else if (winner == CS_TEAM1)
		CS_Banner(CS_Text("CSO_TeamWin_TR", strcat(cs_team1Name, " Win")));
	else if (winner == CS_TEAM2)
		CS_Banner(CS_Text("CSO_TeamWin_CT", strcat(cs_team2Name, " Win")));
	else
		CS_Banner(CS_Text("CSO_RoundDraw", "Round Draw"));

	CS_PublishInfo();
}

/* kills-based score for FFA (best player) or teams */
float
CS_BestKills(void)
{
	float best = 0;
	for (entity p = world; (p = next.Player(p));)
		if (p.cs_kills > best)
			best = p.cs_kills;
	return best;
}

void
CS_EndMatch(string why)
{
	cs_state = CS_MATCHEND;
	cs_stateEnd = time + autocvar_csnz_intermission;

	if (cs_teams) {
		CS_Banner(sprintf("Match over (%s): %s %g - %g %s", why, cs_team1Name, cs_score1, cs_score2, cs_team2Name));
		CS_StatsRecordMatch(cs_score1 > cs_score2 ? CS_TEAM1 : (cs_score2 > cs_score1 ? CS_TEAM2 : 0), world);
		CSNZ_MatchEnded(cs_score1 > cs_score2 ? CS_TEAM1 : (cs_score2 > cs_score1 ? CS_TEAM2 : 0), world);
	} else {
		entity best = world;
		for (entity p = world; (p = next.Player(p));)
			if (!best || p.cs_kills + p.cs_score > best.cs_kills + best.cs_score)
				best = p;
		CS_Banner(sprintf("Match over (%s): %s wins with %g kills", why, best ? best.netname : "nobody", best ? best.cs_kills : 0));
		CS_StatsRecordMatch(0, best);
		CSNZ_MatchEnded(0, best);
	}
	serverinfo.SetString("csnz_winner", cs_teams ? ftos(cs_score1 > cs_score2 ? CS_TEAM1 : (cs_score2 > cs_score1 ? CS_TEAM2 : 0)) : "");

	printf("csnz %.1f: match over (%s)\n", time, why);
	CS_PublishInfo();
}

bool
CS_GoalReached(void)
{
	if (cs_goal <= 0)
		return false;
	if (cs_scorePoints) {
		for (entity q = world; (q = next.Player(q));)
			if (q.cs_score >= cs_goal)
				return true;
		return false;
	}
	if (cs_scoreKills && !cs_teams)
		return CS_BestKills() >= cs_goal;
	return cs_score1 >= cs_goal || cs_score2 >= cs_goal;
}

/* --- bots ---------------------------------------------------------------- */

float cs_nextBotCheck;
int cs_botsWanted;		/* players expected once the last request lands */
float cs_botRequestTime;

void
CS_FillBots(void)
{
	int bots = 0, players = 0;
	string names = "marco gethyn axel dale eric fabian gustav hayden ivan jules kurt leon morgan nils oscar pavel quinn rolf stan tomas";

	if (autocvar_csnz_bots <= 0 || time < cs_nextBotCheck)
		return;
	cs_nextBotCheck = time + 1;

	for (entity p = world; (p = next.Player(p));) {
		players++;
		if (clienttype(p) == CLIENTTYPE_BOT)
			bots++;
	}

	/* Nuclide bot profiles exist only for marco and gethyn; the alternate
	   name argument gives each bot its own name */
	/* one request at a time: wait for it to land (or 5 s) before the next */
	if (players < cs_botsWanted && time < cs_botRequestTime + 5)
		return;

	if (players < autocvar_csnz_bots) {
		cs_botsWanted = players + 1;
		cs_botRequestTime = time;
		tokenize(names);
		localcmd(sprintf("sv addBot %s 3 0 0 %s\n", (bots & 1) ? "gethyn" : "marco", argv(bots % 20)));
	}
}

/* --- test hooks: one shot, set the cvar to 1 --------------------------------- */

/* cs_test_at "<time> <console command>; <time> <command>; ...": runs each
   command once when server time passes <time>. Lets headless runs script a
   match without depending on console wait timing. */
int cs_testStep;

void
CS_TestScript(void)
{
	string script = cvars.GetString("cs_test_at");
	int n;

	if (script == "")
		return;
	n = tokenizebyseparator(script, ";");
	if (cs_testStep >= n)
		return;

	string step = strtrim(argv(cs_testStep));
	float at = stof(step);
	if (time < at)
		return;

	string cmd = substring(step, strstrofs(step, " ") + 1, -1);
	printf("csnz %.1f: test step %i: %s\n", time, cs_testStep, cmd);
	localcmd(strcat(cmd, "\n"));
	cs_testStep++;
}

void CS_StatsTest(void);

void
CS_TestHooks(void)
{
	CS_TestScript();
	CS_StatsTest();

	if (cvars.GetBool("cs_test_killteam1") || cvars.GetBool("cs_test_killteam2")) {
		float t = cvars.GetBool("cs_test_killteam1") ? CS_TEAM1 : CS_TEAM2;
		cvars.SetString("cs_test_killteam1", "0");
		cvars.SetString("cs_test_killteam2", "0");
		for (entity p = world; (p = next.Player(p));)
			if (CS_Playing(p) && p.team == t) {
				p.takedamage = DAMAGE_YES;
				ents.Input(p, "SetHealth", "1", world);
				ents.Input(p, "Kill", "", world);
			}
		printf("csnz %.1f: test hook killed team %g\n", time, t);
	}

	if (cvars.GetBool("cs_test_endround")) {
		cvars.SetString("cs_test_endround", "0");
		CS_EndRound(cvars.GetFloat("cs_test_winner"), "test hook");
	}

	/* cs_test_addkills <n>: credits the first player with n kills */
	if (cvars.GetFloat("cs_test_addkills") > 0) {
		float n = cvars.GetFloat("cs_test_addkills");
		cvars.SetString("cs_test_addkills", "0");
		entity p1 = next.Player(world);
		if (p1) {
			p1.cs_kills += n;
			p1.frags += n;
			if (cs_scoreKills && cs_teams) {
				if (p1.team == CS_TEAM1)
					cs_score1 += n;
				else
					cs_score2 += n;
			}
			printf("csnz %.1f: test hook gave %s %g kills\n", time, p1.netname, n);
		}
	}
}

/* --- the frame ------------------------------------------------------------ */

void
CS_Frame(void)
{
	float verdict;

	if (!cs_banner)
		return;

	CS_FillBots();
	CS_TestHooks();

	for (entity p = world; (p = next.Player(p));) {
		if (p.cs_protectEnd && time >= p.cs_protectEnd) {
			p.cs_protectEnd = 0;
			if (p.health > 0)
				p.takedamage = DAMAGE_YES;
		}
		/* freeze time holds everyone in place (STOCK-CS) */
		if (cs_state == CS_FREEZE && cs_freezeLock && p.health > 0) {
			p.velocity[0] = 0;
			p.velocity[1] = 0;
		}
		/* respawn modes bring dead players back after the delay */
		if (p.cs_respawnAt > 0 && time >= p.cs_respawnAt && (cs_state == CS_LIVE || cs_state == CS_FREEZE))
			CS_PutInGame(p);
	}

	if (time >= cs_nextInfo) {
		CS_PublishInfo();
		cs_nextInfo = time + 1;
	}

	switch (cs_state) {
	case CS_WAIT:
		/* team modes start once both sides have someone (STOCK-CS) */
		if (CS_Players() >= max(autocvar_csnz_min_players, cs_minPlayers) && (!cs_teams || cs_oneSideStart || (CS_TeamSize(CS_TEAM1) > 0 && CS_TeamSize(CS_TEAM2) > 0))) {
			/* the match clock starts with the first round */
			if (cs_round == 0)
				cs_matchEnd = cs_timeLimit > 0 ? time + cs_timeLimit * 60 : 0;
			CS_StartRound();
		}
		break;
	case CS_FREEZE:
		if (time >= cs_stateEnd)
			CS_GoLive();
		break;
	case CS_LIVE:
		if (M_Frame)
			M_Frame();
		if (cs_state != CS_LIVE)
			break;

		if (CS_GoalReached()) {
			CS_EndMatch("goal reached");
			break;
		}
		if (cs_matchEnd > 0 && time >= cs_matchEnd) {
			CS_EndMatch("time limit");
			break;
		}

		verdict = M_CheckRound ? M_CheckRound() : 0;
		if (verdict > 0) {
			CS_EndRound(verdict == 3 ? 0 : verdict, "objective");
		} else if (cs_stateEnd > 0 && time >= cs_stateEnd) {
			if (cs_rounds)
				CS_EndRound(cs_roundTimeWinsTeam2 ? CS_TEAM2 : 0, "round time over");
			else
				CS_EndMatch("time limit");
		}
		break;
	case CS_ROUNDEND:
		if (time < cs_stateEnd)
			break;
		if (CS_GoalReached())
			CS_EndMatch("goal reached");
		else if (cs_matchEnd > 0 && time >= cs_matchEnd)
			CS_EndMatch("time limit");
		else
			CS_StartRound();
		break;
	case CS_MATCHEND:
		if (time >= cs_stateEnd && cvars.GetBool("csnz_restart")) {
			/* a fresh match on the same map */
			cs_score1 = cs_score2 = 0;
			cs_round = 0;
			for (entity q = world; (q = next.Player(q));) {
				q.cs_kills = q.cs_deaths = 0;
				q.frags = q.deaths = 0;
			}
			cs_state = CS_WAIT;
		}
		break;
	}
}

/* sets the match limits once the mode is known */
void
CS_ApplyLimits(float modeId)
{
	float winId, killId, timeId, roundId;
	float timeLimit;

	CS_ModeLimitIds(modeId, winId, killId, timeId, roundId);

	if (cs_scoreKills || cs_scorePoints)
		cs_goal = CS_Limit(autocvar_mp_fraglimit, killId, cs_killLimitFallback, "kill limit");
	else
		cs_goal = CS_Limit(autocvar_mp_winlimit, winId, cs_winLimitFallback, "win limit");

	timeLimit = CS_Limit(autocvar_mp_timelimit, timeId, cs_timeLimitFallback, "time limit");
	cs_roundTime = CS_Limit(autocvar_mp_roundtime, roundId, cs_roundTimeFallback, "round time") * 60;
	cs_timeLimit = timeLimit;
	cs_matchEnd = 0;

	/* non-round modes: the round is the whole match */
	if (!cs_rounds)
		cs_roundTime = timeLimit * 60;

	printf("csnz: goal %g, time limit %g min, round %g s\n", cs_goal, timeLimit, cs_roundTime);
}

/* the parts of StartGameType every mode shares */
void
CS_Init(void)
{
	motd.LoadDefault();
	CS_LoadText();
	cs_banner = ents.Create("csnz_banner", [0, 0, 0]);
	cs_center = ents.Create("csnz_center", [0, 0, 0]);
	cs_state = CS_WAIT;
	cs_round = 0;
	cs_score1 = cs_score2 = 0;
	serverinfo.SetInteger("csnz_mode", (int)autocvar_csnz_mode);
	if (cs_team1Name == "")
		cs_team1Name = "TR";
	if (cs_team2Name == "")
		cs_team2Name = "CT";

	if (cs_teams) {
		teams.SetUp(CS_TEAM1, cs_team1Name, [255, 96, 64], false);
		teams.SetUp(CS_TEAM2, cs_team2Name, [96, 160, 255], false);
	}
}

/* --- generic callbacks the modes can rely on ------------------------------ */

void
CS_PlayerKilled(entity p, entity inflictor, entity attacker, string weapon)
{
	combat.Obituary(p.netname, attacker.netname, weapon, "");
	p.deaths++;
	p.cs_deaths++;

	if ((attacker.flags & FL_CLIENT) && attacker != p) {
		bool teamkill = cs_teams && attacker.team == p.team;
		if (!teamkill) {
			attacker.cs_kills++;
			attacker.frags++;
			if (cs_scoreKills && cs_teams && cs_state == CS_LIVE) {
				if (attacker.team == CS_TEAM1)
					cs_score1++;
				else if (attacker.team == CS_TEAM2)
					cs_score2++;
			}
		} else {
			attacker.frags--;
		}
	}

	if (M_Killed)
		M_Killed(p, inflictor, attacker, weapon);

	if (cs_respawnDelay >= 0 && cs_state != CS_MATCHEND)
		p.cs_respawnAt = time + max(0.1, cs_respawnDelay);
}
