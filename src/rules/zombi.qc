/* zombi: rules for CSNZ mode 8 ("Zombie Classic").
 *
 * Behaviour comes from client text (evidence/ZOMBI-CLIENT-EVIDENCE.md, Z1-Z3):
 *   OBSERVED  each round some players are randomly made zombies after a
 *             selection countdown; zombies turn humans into zombies; humans
 *             win by killing every zombie or surviving the round time;
 *             zombies win when every human is a zombie; zombies have more
 *             HP and speed and are pushed back and stunned when hit.
 *   INFERRED  mp_winlimit counts round wins of one team, mp_roundtime and
 *             mp_timelimit are minutes (stock CS conventions).
 *   APPROX    every zb_* number below is a temporary approximation, not
 *             CSNZ data. See specifications/zombi-rules.md for the list.
 */

/* temporary approximations (U1-U7 in the parity matrix) */
var float autocvar_zb_select_time = 20;		/* U3 selection countdown, seconds */
var float autocvar_zb_origin_ratio = 10;	/* U2 one origin zombie per this many players */
var float autocvar_zb_origin_health = 2000;	/* U1 */
var float autocvar_zb_origin_armor = 0;	/* U1; Nuclide armor absorbs 80% of each hit, which made zombies unkillable */
var float autocvar_zb_host_health = 1000;	/* U1 */
var float autocvar_zb_host_armor = 0;		/* U1 */
var float autocvar_zb_knockback = 250;		/* U5 units/s per hit */
var float autocvar_zb_stun_time = 0.15;		/* U5 */
var float autocvar_zb_stun_scale = 0.4;		/* U5 horizontal speed kept while stunned */
var bool autocvar_zb_zombie_respawn = false;	/* U6 Z3 says "permanently killed" */
var float autocvar_zb_end_time = 5;
var int autocvar_zb_min_players = 2;		/* Z1 min players (table value) */
var bool autocvar_zb_debug = false;

/* room settings written by the Create Room menu (Z2 defaults) */
var float autocvar_mp_winlimit = 13;
var float autocvar_mp_timelimit = 150;
var float autocvar_mp_roundtime = 2;

enum
{
	ZB_WAIT,	/* not enough players */
	ZB_SELECT,	/* everyone human, countdown to zombie selection */
	ZB_LIVE,	/* zombies chosen */
	ZB_END,		/* round result shown */
	ZB_MATCHEND
};

#define ZB_TEAM_ZOMBIE 1
#define ZB_TEAM_HUMAN 2

.float zb_stunEnd;
.float zb_infectAt;	/* set when a dead human is waiting to rise as a zombie */
.vector zb_infectOrigin;
.vector zb_infectAngles;
.float zb_infecting;	/* guards the PlayerKilled callback during a strip */

int zb_state;
int zb_round;
int zb_humanWins;
int zb_zombieWins;
float zb_stateEnd;
float zb_matchEnd;
float zb_nextStatus;
float zb_lastCount;
entity zb_banner;
entity zb_status;
entity zb_message;
hashtable zb_text;

/* --- localization: read the CSO text file the client already ships --- */

void
ZB_LoadText(void)
{
	filestream f;
	string s;
	int loaded = 0;

	zb_text = hash_createtab(64, EV_STRING);
	f = fopen("resource/cso_na_en.utf8.txt", FILE_READ);

	if (f < 0) {
		print("zombi: resource/cso_na_en.utf8.txt not found, using fallback text\n");
		return;
	}

	/* only the keys this file uses; the first definition wins (Z3: some
	   keys are defined more than once, and which one CSO shows is unresolved) */
	while (1) {
		s = fgets(f);

		if not (s)
			break;

		if (tokenize_console(s) < 2)
			continue;

		string key = strtolower(argv(0));

		if (substring(key, 0, 4) != "cso_")
			continue;

		switch (key) {
		case "cso_humanwin":
		case "cso_zombiwin":
		case "cso_zombiselectcount":
		case "cso_humanteam":
		case "cso_zombiteam":
		case "cso_zb_msg_h1_1":
		case "cso_zb_msg_h1_2":
		case "cso_zb_msg_z1_1":
			if (hash_get(zb_text, key, "", EV_STRING) == "") {
				hash_add(zb_text, key, strzone(argv(1)), EV_STRING);
				loaded++;
			}
			break;
		}
	}

	fclose(f);
	printf("zombi: %i CSO text keys loaded\n", loaded);
}

string
ZB_Text(string key, string fallback)
{
	string s = hash_get(zb_text, strtolower(key), "", EV_STRING);
	return (s == "") ? fallback : s;
}

/* --- helpers --- */

/* the HUD shows the banner from serverinfo (INTERFACES.md section 3) */
int zb_msgSeq;

void
ZB_Banner(string msg)
{
	zb_nextStatus = 0;	/* state changed: refresh the HUD keys next frame */
	printf("zombi %.1f: %s\n", time, msg);
	serverinfo.SetString("csnz_msg", msg);
	serverinfo.SetString("csnz_msgseq", sprintf("%i", ++zb_msgSeq));
}

void
ZB_Tell(entity p, string msg)
{
	ents.Input(zb_message, "SetText", msg, world);
	ents.Input(zb_message, "Display", "", p);
}

bool
ZB_InGame(entity p)
{
	return (p.classname != "spectator" && p.health > 0);
}

bool
ZB_IsZombie(entity p)
{
	return (p.team == ZB_TEAM_ZOMBIE);
}

int
ZB_Count(float wantTeam)
{
	int count = 0;

	for (entity p = world; (p = next.Player(p));) {
		if (ZB_InGame(p) && p.team == wantTeam)
			count++;
	}

	return (count);
}

/* zombies that are dead but will rise again still count as alive */
int
ZB_PendingZombies(void)
{
	int count = 0;

	for (entity p = world; (p = next.Player(p));) {
		if (p.zb_infectAt > 0)
			count++;
	}

	return (count);
}

int
ZB_Players(void)
{
	int count = 0;

	for (entity p = world; (p = next.Player(p));)
		count++;

	return (count);
}

/* Some zm_origin spawn points sit inside the world for Nuclide's player
   box (OBSERVED: bots spawned there never moved). Move the player down,
   then up, to the nearest spot where the box fits, and drop to the floor. */
void
ZB_Unstick(entity p)
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
ZB_MakeHuman(entity p)
{
	p.zb_infectAt = 0;
	p.zb_stunEnd = 0;
	ents.ChangeToClass(p, "player_csnz_human");
	ents.Input(p, "SetTeam", itos(ZB_TEAM_HUMAN), world);
	game.TeleportToSpawn(p);
	ZB_Unstick(p);
}

/* Turns a player into a zombie where they stand. Origin zombies are the
   ones picked at the end of the countdown; the rest are hosts. */
void
ZB_MakeZombie(entity p, bool isOrigin, vector where, vector ang)
{
	float hp = isOrigin ? autocvar_zb_origin_health : autocvar_zb_host_health;
	float ap = isOrigin ? autocvar_zb_origin_armor : autocvar_zb_host_armor;

	p.zb_infectAt = 0;
	p.zb_stunEnd = 0;

	/* The human loadout must not carry over. Nuclide's death path is the
	   one that reliably empties an inventory (a player_weaponstrip left
	   stale links in the item list), so the human dies quietly and the
	   zombie class spawns in their place. */
	if (p.health > 0) {
		p.zb_infecting = 1;
		ents.Input(p, "Kill", "", world);
		p.zb_infecting = 0;
	}
	ents.ChangeToClass(p, "player_csnz_zombie");
	ents.Input(p, "SwitchToWeapon", "weapon_csnz_claw", world);

	ents.Input(p, "SetTeam", itos(ZB_TEAM_ZOMBIE), world);
	setorigin(p, where);
	p.angles = p.v_angle = ang;
	p.fixangle = 1;
	ents.Input(p, "SetMaxHealth", ftos(hp), world);
	ents.Input(p, "SetHealth", ftos(hp), world);
	ents.Input(p, "SetMaxArmor", ftos(ap), world);
	ents.Input(p, "SetArmor", ftos(ap), world);

	if (autocvar_zb_debug) {
		string inv = "";
		for (entity w = world; (w = findentity(w, ::owner, p));)
			inv = strcat(inv, " ", w.classname);
		printf("zombi %.1f: %s is now a%s zombie (hp %d, holds%s)\n", time, p.netname, isOrigin ? "n origin" : " host", p.health, inv);
	}
}

void
ZB_Infect(entity victim, entity attacker)
{
	printf("zombi %.1f: %s infected %s\n", time, attacker.netname, victim.netname);
	combat.Obituary(victim.netname, attacker.netname, "weapon_csnz_claw", "");
	attacker.frags++;
	ZB_MakeZombie(victim, false, victim.origin, victim.v_angle);
	ZB_Tell(victim, ZB_Text("CSO_ZB_MSG_Z1_1", "You have been infected!"));

	for (entity p = world; (p = next.Player(p));) {
		if (p != victim && ZB_InGame(p) && !ZB_IsZombie(p))
			ZB_Tell(p, ZB_Text("CSO_ZB_MSG_H1_1", "One of us has been infected!"));
	}
}

/* --- round flow --- */

void
ZB_StartRound(void)
{
	zb_round++;
	zb_state = ZB_SELECT;
	zb_stateEnd = time + autocvar_zb_select_time;
	zb_lastCount = -1;

	for (entity p = world; (p = next.Player(p));)
		ZB_MakeHuman(p);

	printf("zombi %.1f: round %i, %i players, selection in %d s\n", time, zb_round, ZB_Players(), autocvar_zb_select_time);
}

void
ZB_SelectZombies(void)
{
	int players = 0;
	int want;
	entity pick;

	for (entity p = world; (p = next.Player(p));) {
		if (ZB_InGame(p))
			players++;
	}

	want = max(1, (int)floor(players / max(1, autocvar_zb_origin_ratio)));

	/* never leave the humans empty */
	if (want >= players)
		want = players - 1;

	/* test hook: zb_test_origin <name> picks that player first */
	string forced = cvars.GetString("zb_test_origin");
	if (forced != "") {
		for (entity p = world; (p = next.Player(p));) {
			if (p.netname == forced && ZB_InGame(p) && want > 0) {
				printf("zombi %.1f: %s was chosen as an origin zombie (forced)\n", time, p.netname);
				ZB_MakeZombie(p, true, p.origin, p.v_angle);
				want--;
				players--;
			}
		}
	}

	for (int i = 0; i < want; i++) {
		int n = (int)floor(random() * (players - i));

		pick = world;
		for (entity p = world; (p = next.Player(p));) {
			if (!ZB_InGame(p) || ZB_IsZombie(p))
				continue;
			if (n == 0) {
				pick = p;
				break;
			}
			n--;
		}

		if (pick) {
			printf("zombi %.1f: %s was chosen as an origin zombie\n", time, pick.netname);
			ZB_MakeZombie(pick, true, pick.origin, pick.v_angle);
		}
	}

	zb_state = ZB_LIVE;
	zb_stateEnd = time + autocvar_mp_roundtime * 60;
	ZB_Banner(ZB_Text("CSO_ZB_MSG_H1_2", "We don't know who that is! Be careful!"));
}

void
ZB_EndRound(bool humansWon, string why)
{
	zb_state = ZB_END;
	zb_stateEnd = time + autocvar_zb_end_time;

	if (humansWon)
		zb_humanWins++;
	else
		zb_zombieWins++;

	printf("zombi %.1f: round %i over (%s), humans %i zombies %i\n", time, zb_round, why, zb_humanWins, zb_zombieWins);
	ZB_Banner(humansWon ? ZB_Text("CSO_HumanWin", "Human Victory") : ZB_Text("CSO_ZombiWin", "Zombie Victory"));
	serverinfo.SetString("zb_score", sprintf("%i:%i", zb_humanWins, zb_zombieWins));
}

void
ZB_Status(void)
{
	string msg;
	float left = max(0, zb_stateEnd - time);

	if (zb_state == ZB_WAIT) {
		msg = sprintf("Waiting for players (%i/%i)", ZB_Players(), autocvar_zb_min_players);
	} else if (zb_state == ZB_SELECT) {
		msg = strreplace("%s1", ftos(ceil(left)), ZB_Text("CSO_ZombiSelectCount", "Time Remaining for Zombie Selection: %s1 Sec"));
	} else {
		msg = sprintf("%s   %i:%02i   %s",
			strreplace("%d", sprintf("%i", ZB_Count(ZB_TEAM_HUMAN)), ZB_Text("CSO_HumanTeam", "Human Team (%d)")),
			(int)floor(left / 60), (int)(floor(left) % 60),
			strreplace("%d", sprintf("%i", ZB_Count(ZB_TEAM_ZOMBIE) + ZB_PendingZombies()), ZB_Text("CSO_ZombiTeam", "Zombie Team (%d)")));
	}

	serverinfo.SetString("zb_status", msg);

	/* the shared HUD keys (INTERFACES.md section 3); team 1 zombies, 2
	   humans. The team names are DESIGN (the HUD draws the HM/ZB art). */
	string states[5] = {"wait", "freeze", "live", "roundend", "matchend"};
	serverinfo.SetString("csnz_mode", "8");
	serverinfo.SetString("csnz_state", states[zb_state]);
	serverinfo.SetString("csnz_stateend", (zb_state == ZB_WAIT || zb_state == ZB_MATCHEND) ? "0" : ftos(zb_stateEnd));
	serverinfo.SetString("csnz_round", sprintf("%i", zb_round));
	serverinfo.SetString("csnz_t1", "Zombie");
	serverinfo.SetString("csnz_t2", "Human");
	serverinfo.SetString("csnz_s1", sprintf("%i", zb_zombieWins));
	serverinfo.SetString("csnz_s2", sprintf("%i", zb_humanWins));
	serverinfo.SetString("csnz_a1", sprintf("%i", ZB_Count(ZB_TEAM_ZOMBIE) + ZB_PendingZombies()));
	serverinfo.SetString("csnz_a2", sprintf("%i", ZB_Count(ZB_TEAM_HUMAN)));
	serverinfo.SetString("csnz_goal", sprintf("%i", (int)autocvar_mp_winlimit));

	if (autocvar_zb_debug)
		printf("zombi %.1f: status %s\n", time, msg);

	if (cvars.GetInteger("zb_debug") >= 2)
		for (entity p = world; (p = next.Player(p));)
		{
			tracebox(p.origin, p.mins, p.maxs, p.origin, MOVE_NORMAL, p);
			printf("zombi %.1f:   %s team %d hp %d at %v%s\n", time, p.netname, p.team, p.health, p.origin,
				trace_startsolid ? sprintf(" STUCK in %s", trace_ent.classname) : "");
		}
}

/* one-shot hooks for the headless tests: set the cvar to 1 */
void
ZB_TestHooks(void)
{
	if (cvars.GetBool("zb_test_killzombies")) {
		cvars.SetString("zb_test_killzombies", "0");
		for (entity p = world; (p = next.Player(p));) {
			if (ZB_InGame(p) && ZB_IsZombie(p))
				ents.Input(p, "Kill", "", world);
		}
		printf("zombi %.1f: test hook killed all zombies\n", time);
	}

	if (cvars.GetBool("zb_test_infectall")) {
		entity z = world;

		cvars.SetString("zb_test_infectall", "0");
		for (entity p = world; (p = next.Player(p));) {
			if (ZB_InGame(p) && ZB_IsZombie(p))
				z = p;
		}
		for (entity p = world; (p = next.Player(p));) {
			if (z && ZB_InGame(p) && !ZB_IsZombie(p))
				ZB_Infect(p, z);
		}
		printf("zombi %.1f: test hook infected all humans\n", time);
	}
}

void
ZB_CheckRound(void)
{
	int humans = ZB_Count(ZB_TEAM_HUMAN);
	int zombies = ZB_Count(ZB_TEAM_ZOMBIE) + ZB_PendingZombies();

	if (humans == 0)
		ZB_EndRound(false, "all humans infected");
	else if (zombies == 0)
		ZB_EndRound(true, "all zombies killed");
	else if (time >= zb_stateEnd)
		ZB_EndRound(true, "round time over");
}

/* --- callbacks --- */

void
CodeCallback_Precache(void)
{
	precache.Entity("player_csnz_human");
	precache.Entity("player_csnz_zombie");
	precache.Entity("weapon_csnz_claw");
	CS_PrecacheWeapons();
}

string
CodeCallback_Title(void)
{
	return ("zombi");
}

void
CodeCallback_StartGameType(void)
{
	motd.LoadDefault();
	ZB_LoadText();
	game.SetSpawnPoint("info_player_deathmatch");
	teams.SetUp(ZB_TEAM_ZOMBIE, "Zombies", [255, 64, 64], false);
	teams.SetUp(ZB_TEAM_HUMAN, "Humans", [128, 160, 255], false);
	teams.SetSpawnPoint(ZB_TEAM_ZOMBIE, "info_player_deathmatch");
	teams.SetSpawnPoint(ZB_TEAM_HUMAN, "info_player_deathmatch");

	zb_banner = ents.Create("zombi_banner", [0, 0, 0]);
	zb_status = ents.Create("zombi_status", [0, 0, 0]);
	zb_message = ents.Create("zombi_message", [0, 0, 0]);
	if (autocvar_zb_debug)
		printf("zombi: text entities %d %d %d\n", num_for_edict(zb_banner), num_for_edict(zb_status), num_for_edict(zb_message));
	zb_round = 0;
	zb_state = ZB_WAIT;
	zb_matchEnd = time + autocvar_mp_timelimit * 60;

	printf("zombi: winlimit %d, timelimit %d min, roundtime %d min\n", autocvar_mp_winlimit, autocvar_mp_timelimit, autocvar_mp_roundtime);
}

void
CodeCallback_FrameStart(void)
{
	/* the server runs frames before the game type starts */
	if (!zb_status)
		return;

	if (time >= zb_nextStatus) {
		ZB_Status();
		zb_nextStatus = time + 1.0f;
	}

	ZB_TestHooks();

	if (cvars.GetBool("zb_buildnav")) {
		cvars.SetString("zb_buildnav", "0");
		ZBN_Build();
	}

	CSNZ_PublishHudInfo();
	for (entity p = world; (p = next.Player(p));) {
		/* dead humans bitten by a zombie rise where they fell */
		if (p.zb_infectAt > 0 && time >= p.zb_infectAt)
			ZB_MakeZombie(p, false, p.zb_infectOrigin, p.zb_infectAngles);

		/* a stunned zombie keeps only part of its speed. This runs once a
		   server frame; Nuclide never calls the rules' PlayerPreFrame for
		   players (its PlayerPreThink returns early for them). */
		if (p.zb_stunEnd > time) {
			p.velocity[0] *= autocvar_zb_stun_scale;
			p.velocity[1] *= autocvar_zb_stun_scale;
		}

		if (cvars.GetInteger("zb_debug") >= 3 && (p.button0 || floor(time * 2) != floor((time - frametime) * 2)))
			printf("zombi %.2f: DBG %s button0 %d\n", time, p.netname, p.button0);
	}

	switch (zb_state) {
	case ZB_WAIT:
		if (ZB_Players() >= autocvar_zb_min_players)
			ZB_StartRound();
		break;
	case ZB_SELECT:
		if (ZB_Players() < autocvar_zb_min_players) {
			zb_state = ZB_WAIT;
		} else if (time >= zb_stateEnd) {
			ZB_SelectZombies();
		} else if (ceil(zb_stateEnd - time) != zb_lastCount) {
			zb_lastCount = ceil(zb_stateEnd - time);
			ZB_Status();
		}
		break;
	case ZB_LIVE:
		ZB_CheckRound();
		break;
	case ZB_END:
		if (time < zb_stateEnd)
			break;

		if (zb_humanWins >= autocvar_mp_winlimit || zb_zombieWins >= autocvar_mp_winlimit || time >= zb_matchEnd) {
			zb_state = ZB_MATCHEND;
			ZB_Banner(sprintf("Match over: Humans %i - %i Zombies", zb_humanWins, zb_zombieWins));
			printf("zombi %.1f: match over\n", time);
		} else {
			ZB_StartRound();
		}
		break;
	}
}

void
CodeCallback_PlayerSpawn(entity p)
{
	/* DESIGN: a player joining after selection comes in as a host zombie */
	if (zb_state == ZB_LIVE) {
		ZB_MakeHuman(p);
		ZB_MakeZombie(p, false, p.origin, p.angles);
		return;
	}

	ZB_MakeHuman(p);
}

bool
CodeCallback_PlayerRequestRespawn(entity p)
{
	if (zb_state == ZB_LIVE || zb_state == ZB_MATCHEND)
		return (false);

	ZB_MakeHuman(p);
	return (true);
}

/* Called only when the victim survives the hit. */
void
CodeCallback_PlayerDamage(entity victim, entity attacker, entity damageDecl)
{
	if (autocvar_zb_debug)
		printf("zombi %.1f: %s hit %s (hp %d)\n", time, attacker.netname, victim.netname, victim.health);

	if (zb_state != ZB_LIVE || !(attacker.flags & FL_CLIENT) || attacker == victim)
		return;

	if (ZB_IsZombie(attacker) && !ZB_IsZombie(victim)) {
		/* U4 (armor stopping infection) is not modelled: Nuclide keeps armor
		   in a class-private field this progs cannot read, and humans have
		   no way to get armor in this slice yet */
		ZB_Infect(victim, attacker);
		return;
	}

	if (!ZB_IsZombie(attacker) && ZB_IsZombie(victim)) {
		vector dir = normalize(victim.origin - attacker.origin);
		dir[2] = max(dir[2], 0.1);
		ents.Input(victim, "AddVelocity", vtos(dir * autocvar_zb_knockback), world);
		victim.zb_stunEnd = time + autocvar_zb_stun_time;
	}
}

void
CodeCallback_PlayerKilled(entity p, entity inflictor, entity attacker, string weapon)
{
	if (p.zb_infecting)
		return;

	combat.Obituary(p.netname, attacker.netname, weapon, "");
	p.deaths++;

	if (zb_state != ZB_LIVE)
		return;

	if (!ZB_IsZombie(p) && (attacker.flags & FL_CLIENT) && ZB_IsZombie(attacker)) {
		/* a claw that kills still infects */
		attacker.frags++;
		p.zb_infectAt = time + 0.5f;
		p.zb_infectOrigin = p.origin;
		p.zb_infectAngles = p.v_angle;
		printf("zombi %.1f: %s killed %s, rising as a zombie\n", time, attacker.netname, p.netname);
	} else if (ZB_IsZombie(p)) {
		if (attacker.flags & FL_CLIENT && attacker != p)
			attacker.frags++;

		if (autocvar_zb_zombie_respawn) {
			p.zb_infectAt = time + 3.0f;
			p.zb_infectOrigin = p.origin;
			p.zb_infectAngles = p.v_angle;
		}
	}
}

void
CodeCallback_PlayerDisconnect(entity p)
{
	p.zb_infectAt = 0;
}
