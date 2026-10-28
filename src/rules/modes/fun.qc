/* fun: the FUN-category and other single-rule modes of csnz.dat.
 * Original code (ISC). Evidence labels as in csnz_common.qc.
 *
 *   22 GunDeath      weapon ladder
 *   10 Hidden        near-invisible Hiddens against soldiers
 *   16 Soccer        ball, goals, possession penalty, goal-line penalty
 *   12 Challenge     timed runs from start to finish, best times kept
 *   41/48 Playroom   Hide-Out: decoders hand out weapons to try
 *   27 Snowman       TDM sub-type with ampoules and the beast form */

/* --- gun deathmatch (22) ------------------------------------------------------
   OBSERVED tooltip: kills change your weapon to the next, harder one; dying
   interrupts the streak and resets the progress toward the next weapon.
   OBSERVED GDM_02: after the MAX level a kill with the final grenade.
   DESIGN: two kills per level, the ladder of placeholder weapons, and the
   win as the first kill made at the final level. */

string gd_ladder = "weapon_m249 weapon_m4a1 weapon_m3 weapon_usp weapon_awp weapon_ak47 weapon_hegrenade";
var float autocvar_gd_kills_per_level = 2;
.float gd_level;
.float gd_progress;

float
GD_Levels(void)
{
	return tokenize(gd_ladder);
}

void
GD_Arm(entity p)
{
	int n = GD_Levels();
	string w = argv((int)min(p.gd_level, n - 1));
	ents.ChangeToClass(p, "csm_knifeonly");
	ents.Input(p, "GiveItem", w, world);
	CS_FillAmmo(p);
	ents.Input(p, "SwitchToWeapon", w, world);
	userinfo.SetInteger(p, "csnz_level", (int)p.gd_level + 1);
	userinfo.SetInteger(p, "csnz_levels", n);
}

void
GD_Equip(entity p)
{
	GD_Arm(p);
}

void
GD_Killed(entity p, entity inflictor, entity attacker, string weapon)
{
	p.gd_progress = 0;	/* OBSERVED: dying resets the progress */
	if (!(attacker.flags & FL_CLIENT) || attacker == p)
		return;

	float last = GD_Levels() - 1;
	if (attacker.gd_level >= last) {
		attacker.cs_score = last + 1;
		CS_Banner(sprintf("%s made the final kill", attacker.netname));
		return;
	}
	attacker.gd_progress++;
	if (attacker.gd_progress >= autocvar_gd_kills_per_level) {
		attacker.gd_progress = 0;
		attacker.gd_level++;
		attacker.cs_score = attacker.gd_level;
		printf("csnz %.1f: %s reached level %g\n", time, attacker.netname, attacker.gd_level + 1);
		if (CS_Playing(attacker))
			GD_Arm(attacker);
	}
}

/* test hook: cs_test_gdkills <n> credits the first player with n kills */
void
GD_Frame(void)
{
	float n = cvars.GetFloat("cs_test_gdkills");
	if (n <= 0)
		return;
	cvars.SetString("cs_test_gdkills", "0");
	entity p = next.Player(world);
	entity v = next.Player(p);
	if (!p || !v)
		return;
	for (float i = 0; i < n; i++)
		GD_Killed(v, world, p, "");
}

void
GD_Start(void)
{
	/* the score is the level reached; the final kill scores the level count */
	cs_goal = GD_Levels();
}

void
CSM_GunDeath(void)
{
	CSM_Deathmatch(false);
	cs_modeTitle = "CSO_GameMode_GunDeath";
	cs_modeFallback = "Gun Deathmatch";
	cs_scoreKills = false;
	cs_scorePoints = true;
	cs_timeLimitFallback = 30;	/* OBSERVED limitlist id 27 default */
	M_Equip = GD_Equip;
	M_Killed = GD_Killed;
	M_Start = GD_Start;
	M_Frame = GD_Frame;
}

/* --- hidden (10) ----------------------------------------------------------------
   OBSERVED tip Hidden_01: kill Hiddens to reach the victory condition;
   Hiddens become more visible as time passes; whoever kills a Hidden is a
   Hidden at their next respawn. Team score: each side scores the other
   side's deaths. DESIGN: one Hidden per 5 players, the visibility ramp. */

.float hd_spawnTime;
.float hd_next;			/* become a Hidden at the next respawn */

void
HD_Equip(entity p)
{
	if (p.hd_next) {
		p.hd_next = 0;
		CS_SetTeam(p, CS_TEAM1);
	}
	if (p.team == CS_TEAM1) {
		ents.ChangeToClass(p, "csm_hidden");
		ents.Input(p, "SetRenderMode", "2", world);
		ents.Input(p, "Alpha", "0.05", world);
	} else {
		ents.ChangeToClass(p, "csm_rifleman");
		ents.Input(p, "Alpha", "1", world);
	}
	p.hd_spawnTime = time;
}

float
HD_PickTeam(entity p)
{
	int players = CS_Players();
	int want = max(1, floor(players / 5));
	return CS_TeamSize(CS_TEAM1) < want ? CS_TEAM1 : CS_TEAM2;
}

void
HD_Killed(entity p, entity inflictor, entity attacker, string weapon)
{
	if (!(attacker.flags & FL_CLIENT) || attacker == p)
		return;
	if (p.team == CS_TEAM1 && attacker.team == CS_TEAM2) {
		/* the Hidden's killer takes over; the Hidden becomes a soldier */
		attacker.hd_next = 1;
		attacker.cs_respawnAt = time + 2;	/* swap at once (DESIGN) */
		CS_SetTeam(p, CS_TEAM2);
		CS_Banner(sprintf("%s killed the Hidden", attacker.netname));
	}
}

void
HD_Frame(void)
{
	/* test hook: cs_test_killhidden has a soldier kill the Hidden */
	if (cvars.GetBool("cs_test_killhidden")) {
		cvars.SetString("cs_test_killhidden", "0");
		entity hid = world, sol = world;
		for (entity c = world; (c = next.Player(c));) {
			if (CS_Playing(c) && c.team == CS_TEAM1)
				hid = c;
			if (CS_Playing(c) && c.team == CS_TEAM2)
				sol = c;
		}
		if (hid && sol) {
			HD_Killed(hid, world, sol, "");
			hid.takedamage = DAMAGE_YES;
			ents.Input(hid, "Kill", "", world);
		}
	}

	for (entity p = world; (p = next.Player(p));) {
		if (!CS_Playing(p) || p.team != CS_TEAM1)
			continue;
		/* fully transparent at spawn, half visible after two minutes */
		float a = min(0.6, 0.05 + (time - p.hd_spawnTime) / 120 * 0.55);
		ents.Input(p, "Alpha", ftos(a), world);
	}
	/* the killer that is to become a Hidden: put them in at the respawn */
	for (entity q = world; (q = next.Player(q));)
		if (q.hd_next && CS_Playing(q) && q.cs_respawnAt > 0 && time >= q.cs_respawnAt) {
			q.cs_respawnAt = 0;
			CS_PutInGame(q);
		}
}

void
CSM_Hidden(void)
{
	CSM_TeamDeathmatch(false);
	cs_modeTitle = "CSO_GameMode_Hidden";
	cs_modeFallback = "Hidden";
	cs_team1Name = "Hidden";
	cs_team2Name = "Soldier";
	cs_killLimitFallback = 60;	/* OBSERVED limitlist id 21 default */
	M_Equip = HD_Equip;
	M_PickTeam = HD_PickTeam;
	M_Killed = HD_Killed;
	M_Frame = HD_Frame;
}

/* --- soccer (16) ----------------------------------------------------------------
   OBSERVED tips Soccer_01..06: the team with the target goals, or most goals
   at the end, wins; holding the ball 15 s or more is a penalty; standing near
   the opposing goal causes continuous damage; right-click passes; Shift
   sprints. OBSERVED maplist.csv: sc_soccer01 has ball_max 2, ball_default 2.
   Goals: Studio GOALNET_SOCCER blocks or info_target "goal1"/"goal2", else
   each team's spawn area (DESIGN). Kicks, speeds and damage are DESIGN. */

#define SC_BALLS_MAX 4
entity sc_balls[SC_BALLS_MAX];
float sc_ballCount;
vector sc_goal[3];		/* by team */
.entity sc_holder;		/* on a ball: who dribbles it */
.float sc_holdSince;
.float sc_kickAt;

/* maplist.csv: ball_default of this map (column 56 of the data rows) */
float
SC_BallsForMap(void)
{
	filestream f = fopen("resource/maplist.csv", FILE_READ);
	string s;
	float balls = 1;
	if (f < 0)
		return 1;
	while (1) {
		s = fgets(f);
		if not (s)	/* end of file; "" is a blank line */
			break;
		if (tokenizebyseparator(s, ",") < 57)
			continue;
		if (strtolower(argv(2)) == strtolower(game.GetMap())) {
			balls = bound(1, stof(argv(56)), SC_BALLS_MAX);
			break;
		}
	}
	fclose(f);
	return balls;
}

vector
SC_FindGoal(float whichTeam)
{
	string name = whichTeam == CS_TEAM1 ? "goal1" : "goal2";
	entity e = find(world, ::targetname, name);
	if (e)
		return e.origin;
	/* Studio goal nets: the first is team 1's, the second team 2's */
	int n = 0;
	for (entity g = world; (g = find(g, ::classname, "csnz_goalnet"));)
		if (++n == whichTeam)
			return g.origin;
	return ZV_TeamCentroid(whichTeam);
}

void
SC_ResetBalls(void)
{
	vector mid = (sc_goal[1] + sc_goal[2]) * 0.5;
	/* Studio BALLSPAWN_SOCCER blocks, else between the goals */
	entity spot = find(world, ::classname, "csnz_ballspawn");
	for (int i = 0; i < sc_ballCount; i++) {
		entity b = sc_balls[i];
		if (spot) {
			setorigin(b, spot.origin + [0, 0, 48]);
			printf("csnz %.1f: ball %d at the Studio ball spawn %v\n", time, i + 1, spot.origin);
			spot = find(spot, ::classname, "csnz_ballspawn");
			if (!spot)
				spot = find(world, ::classname, "csnz_ballspawn");
		} else {
			setorigin(b, mid + [i * 48 - (sc_ballCount - 1) * 24, 0, 48]);
		}
		b.velocity = [0, 0, 0];
		b.sc_holder = world;
	}
}

void
SC_Start(void)
{
	sc_goal[1] = SC_FindGoal(CS_TEAM1);
	sc_goal[2] = SC_FindGoal(CS_TEAM2);
	sc_ballCount = SC_BallsForMap();
	for (int i = 0; i < sc_ballCount; i++) {
		entity b = spawn();
		b.classname = "csnz_ball";
		setmodel(b, "models/w_backpack.mdl");
		setsize(b, [-10, -10, -10], [10, 10, 10]);
		b.movetype = MOVETYPE_BOUNCE;
		b.solid = SOLID_TRIGGER;
		b.gravity = 1;
		b.colormod = [1, 1, 1];
		sc_balls[i] = b;
	}
	SC_ResetBalls();
	printf("csnz: soccer with %g balls, goals at %v and %v\n", sc_ballCount, sc_goal[1], sc_goal[2]);
}

void
SC_Equip(entity p)
{
	ents.ChangeToClass(p, "csm_brawler");
}

void
SC_Kick(entity p, entity b, float power)
{
	makevectors(p.v_angle);
	b.velocity = v_forward * power + [0, 0, power * 0.25];
	b.sc_holder = world;
	b.sc_kickAt = time;
}

void
SC_Frame(void)
{
	for (int i = 0; i < sc_ballCount; i++) {
		entity b = sc_balls[i];

		/* dribbling: the nearest player within reach carries the ball */
		entity near = world;
		float best = 48;
		for (entity p = world; (p = next.Player(p));) {
			if (!CS_Playing(p))
				continue;
			float d = vlen(p.origin - b.origin);
			if (d < best && time - b.sc_kickAt > 0.4) {
				best = d;
				near = p;
			}
		}
		if (near != b.sc_holder) {
			b.sc_holder = near;
			b.sc_holdSince = time;
		}
		if (b.sc_holder) {
			makevectors([0, b.sc_holder.v_angle[1], 0]);
			setorigin(b, b.sc_holder.origin + v_forward * 32 + [0, 0, -14]);
			b.velocity = b.sc_holder.velocity;
			/* OBSERVED: 15 s of possession is a penalty: the ball is lost */
			if (time - b.sc_holdSince >= 15) {
				CS_Tell(b.sc_holder, "Possession penalty!");
				SC_Kick(b.sc_holder, b, -300);
			}
		}

		/* goals: a ball in a team's goal scores for the other team */
		for (float t = CS_TEAM1; t <= CS_TEAM2; t++) {
			vector d2 = b.origin - sc_goal[t];
			if (vlen([d2[0], d2[1], 0]) < 64 && fabs(d2[2]) < 96) {
				float scorer = t == CS_TEAM1 ? CS_TEAM2 : CS_TEAM1;
				if (scorer == CS_TEAM1)
					cs_score1++;
				else
					cs_score2++;
				CS_Banner(sprintf("GOAL! %s %g - %g %s", cs_team1Name, cs_score1, cs_score2, cs_team2Name));
				printf("csnz %.1f: goal for team %g\n", time, scorer);
				SC_ResetBalls();
				CS_PublishInfo();
				break;
			}
		}
	}

	/* OBSERVED: standing near the opposing goal causes continuous damage */
	for (entity q = world; (q = next.Player(q));) {
		if (!CS_Playing(q) || !q.team)
			continue;
		float other = q.team == CS_TEAM1 ? CS_TEAM2 : CS_TEAM1;
		if (vlen(q.origin - sc_goal[other]) < 160 && random() < frametime * 2)
			ents.Input(q, "SetHealth", ftos(q.health - 5), world);
	}
}

/* "cmd csnz_shoot": the attack button of the soccer HUD; "cmd csnz_pass":
   right click, OBSERVED to pass to the team mate in front */
bool
SC_Command(entity p, string cmd)
{
	tokenize(cmd);
	if (!p || (argv(0) != "csnz_shoot" && argv(0) != "csnz_pass"))
		return false;
	for (int i = 0; i < sc_ballCount; i++) {
		entity b = sc_balls[i];
		if (b.sc_holder != p)
			continue;
		if (argv(0) == "csnz_shoot") {
			SC_Kick(p, b, 900);
		} else {
			entity mate = world;
			float best = 0.5;
			makevectors(p.v_angle);
			for (entity m = world; (m = next.Player(m));) {
				if (m == p || m.team != p.team || !CS_Playing(m))
					continue;
				float dot = normalize(m.origin - p.origin) * v_forward;
				if (dot > best) {
					best = dot;
					mate = m;
				}
			}
			if (mate) {
				b.velocity = normalize(mate.origin - b.origin) * min(700, vlen(mate.origin - b.origin) * 2) + [0, 0, 80];
				b.sc_holder = world;
				b.sc_kickAt = time;
			} else {
				SC_Kick(p, b, 450);
			}
		}
		return true;
	}
	return true;
}

/* test hook: cs_test_goal 1|2 puts a ball into that team's goal */
void
SC_TestHooks(void)
{
	float t = cvars.GetFloat("cs_test_goal");
	if (t == CS_TEAM1 || t == CS_TEAM2) {
		cvars.SetString("cs_test_goal", "0");
		if (sc_ballCount > 0) {
			sc_balls[0].sc_holder = world;
			sc_balls[0].sc_kickAt = time;
			setorigin(sc_balls[0], sc_goal[t] + [0, 0, 16]);
		}
	}
}

void
SC_FrameAll(void)
{
	SC_TestHooks();
	SC_Frame();
}

void
CSM_Soccer(void)
{
	CSM_TeamDeathmatch(false);
	cs_modeTitle = "CSO_GameMode_Soccer";
	cs_modeFallback = "Soccer";
	cs_scoreKills = false;
	cs_winLimitFallback = 13;	/* limitlist id 1 default, used as the goal target */
	M_Start = SC_Start;
	M_Equip = SC_Equip;
	M_Frame = SC_FrameAll;
	M_Command = SC_Command;
}

/* --- challenge (12) ----------------------------------------------------------
   OBSERVED tip Chall: the shortest time from the starting to the finishing
   line wins; points for arriving; passing the final red line after the
   finish returns you to the beginning; times are taken without limit
   during the game period. OBSERVED maplist: cso_jump1/2, cso_run1/2.
   Lines: info_target "challenge_start"/"challenge_finish", the KZ button
   targets "counter_start"/"counter_off", or Studio EXIT blocks (finish).
   Best times are kept in data/csnz/challenge_<map>.txt. */

.float ch_startTime;
.float ch_best;
vector ch_start, ch_finish;
bool ch_ready;

vector
CH_Find(string a, string b)
{
	entity e = find(world, ::targetname, a);
	if (!e)
		e = find(world, ::target, b);
	if (!e)
		return [0, 0, -99999];
	return e.absmin != e.absmax ? (e.absmin + e.absmax) * 0.5 : e.origin;
}

void
CH_SaveBest(entity p, float t)
{
	string file = sprintf("csnz/challenge_%s.txt", game.GetMap());
	string lines[64];
	int n = 0;
	bool found = false;
	filestream f = fopen(file, FILE_READ);
	string s;

	if (f >= 0) {
		while (n < 64) {
			s = fgets(f);
			if not (s)	/* end of file; "" is a blank line */
				break;
			if (tokenizebyseparator(s, "\t") != 2)
				continue;
			if (argv(0) == p.netname) {
				found = true;
				if (stof(argv(1)) <= t) {
					fclose(f);
					return;		/* not a record */
				}
				s = sprintf("%s\t%.3f", p.netname, t);
			}
			lines[n++] = strzone(s);
		}
		fclose(f);
	}
	if (!found && n < 64)
		lines[n++] = strzone(sprintf("%s\t%.3f", p.netname, t));

	f = fopen(file, FILE_WRITE);
	if (f >= 0) {
		for (int i = 0; i < n; i++)
			fputs(f, strcat(lines[i], "\n"));
		fclose(f);
		printf("csnz: personal best %.3f s for %s saved\n", t, p.netname);
	}
	for (int j = 0; j < n; j++)
		strunzone(lines[j]);
}

void
CH_Start(void)
{
	ch_start = CH_Find("challenge_start", "counter_start");
	ch_finish = CH_Find("challenge_finish", "counter_off");
	if (ch_finish[2] == -99999)
		for (entity x = world; (x = find(x, ::classname, "csnz_exit"));)
			ch_finish = x.origin;
	ch_ready = ch_start[2] != -99999 && ch_finish[2] != -99999;
	if (!ch_ready)
		print("csnz: challenge map has no start or finish line; times are not taken\n");
}

void
CH_Frame(void)
{
	if (!ch_ready)
		return;
	for (entity p = world; (p = next.Player(p));) {
		if (!CS_Playing(p))
			continue;
		if (vlen(p.origin - ch_start) < 96) {
			p.ch_startTime = time;
			userinfo.SetFloat(p, "csnz_runstart", time);
		} else if (p.ch_startTime > 0 && vlen(p.origin - ch_finish) < 96) {
			float t = time - p.ch_startTime;
			p.ch_startTime = 0;
			p.cs_score++;	/* OBSERVED: points for arriving */
			userinfo.SetFloat(p, "csnz_runstart", 0);
			if (!p.ch_best || t < p.ch_best) {
				p.ch_best = t;
				userinfo.SetFloat(p, "csnz_best", t);
				CH_SaveBest(p, t);
			}
			CS_Banner(sprintf("%s finished in %.2f s", p.netname, t));
			/* back to the beginning (OBSERVED) */
			CS_MoveToSpot(p, ch_start + [0, 0, 8], p.v_angle);
		}
	}
}

/* test hook: cs_test_run teleports the first player through the course */
void
CH_TestHooks(void)
{
	float step = cvars.GetFloat("cs_test_run");
	if (!step)
		return;
	cvars.SetString("cs_test_run", "0");
	entity p = next.Player(world);
	if (p)
		setorigin(p, step == 1 ? ch_start + [0, 0, 8] : ch_finish + [0, 0, 8]);
}

void
CH_FrameAll(void)
{
	CH_TestHooks();
	CH_Frame();
}

void
CSM_Challenge(void)
{
	CSM_Deathmatch(false);
	cs_modeTitle = "CSO_GameMode_Challenge";
	cs_modeFallback = "Challenge";
	cs_scoreKills = false;
	cs_scorePoints = false;
	cs_timeLimitFallback = 10;	/* OBSERVED limitlist id 15 default */
	M_Start = CH_Start;
	M_Frame = CH_FrameAll;
	M_Equip = CSM_EquipDefault;
	M_Damaged = __NULL__;
}

/* --- Hide-Out / Playroom (41, 48, 55) -------------------------------------
   mp.dll (SOURCE-CONFIRMED, RTTI templates and constructor chains): each
   playroom wraps a regular mode's rules, CSOGameRulesPlayRoom<base>:
     41 PlayRoomZBH      base Zombi3 (Zombie Hero, 14)
     48 PlayRoomZBS      base ZombieScenarioVer2 (Zombie Scenario, 15)
     55 PlayRoomZombieZ  base ZombieZMode (Zombie Z, 45)
   So the base mode is played and the Hide-Out layer goes on top.
   OBSERVED tooltips: Decoders give items; others who are interested can try
   them; guns can be tested. DESIGN: decoders give a random placeholder
   weapon, and every weapon given in the room can be tried by anyone with
   "cmd csnz_try <weapon>". */

string pr_unlocked;
var float(entity, string) PR_InnerCommand;

bool
PR_Command(entity p, string cmd)
{
	int n = tokenize(cmd);
	if (!p && argv(0) == "csnz_decoder_test") {
		p = next.Player(world);
		cmd = "csnz_decoder";
		n = tokenize(cmd);
	}
	if (!p)
		return false;
	if (argv(0) == "csnz_decoder") {
		int k = tokenize(gd_ladder);
		string w = argv((int)floor(random() * k));
		ents.Input(p, "GiveItem", w, world);
		ents.Input(p, "SwitchToWeapon", w, world);
		if (strstrofs(pr_unlocked, w) < 0) {
			string old = pr_unlocked;
			pr_unlocked = strzone(strcat(old, " ", w));
			if (old)
				strunzone(old);
		}
		serverinfo.SetString("csnz_unlocked", pr_unlocked);
		CS_Banner(sprintf("%s decoded %s", p.netname, w));
		return true;
	}
	if (argv(0) == "csnz_try" && n > 1) {
		string want = argv(1);
		if (strstrofs(pr_unlocked, want) < 0) {
			sprint(p, PRINT_HIGH, "Nobody has decoded that yet.\n");
			return true;
		}
		ents.Input(p, "GiveItem", want, world);
		CS_FillAmmo(p);
		ents.Input(p, "SwitchToWeapon", want, world);
		return true;
	}
	return false;
}

float
PR_CommandAll(entity p, string cmd)
{
	if (PR_Command(p, cmd))
		return true;
	return PR_InnerCommand ? PR_InnerCommand(p, cmd) : false;
}

/* the playroom id's base rules first, then the Hide-Out commands */
void
CSM_Playroom(float id)
{
	switch (id) {
	case 41:
		CSM_Zombie(14);
		cs_modeTitle = "CSO_GameMode_PlayRoom";
		cs_modeFallback = "Hide-Out";
		break;
	case 48:
		CSM_ZombieScenario();
		cs_modeTitle = "CSO_GameMode_PlayRoom2";
		cs_modeFallback = "Hide-Out 2 Scenario";
		break;
	default:
		CSM_Zombie(45);
		zv_zbzPlayroom = true;
		cs_modeTitle = "CSO_GameMode_PlayRoom_ZombieZ";	/* no text in this build */
		cs_modeFallback = "Hide-Out Zombie Z";
	}
	cs_noStats = true;
	PR_InnerCommand = M_Command;
	M_Command = PR_CommandAll;
}

/* --- snowman (27) ---------------------------------------------------------
   OBSERVED tooltip: collect 3 ampoules and press E to turn into a beast.
   OBSERVED: the sm_TDM sub-type is Team Deathmatch (tip sm_TDM); the core
   sub-type ("destroy the enemy's core", control device, 20 s restart) is
   not implemented (BLOCKERS.md). Ampoules drop from killed players (DESIGN). */

.float sm_ampoules;
.float sm_beastEnd;

void
SM_Killed(entity p, entity inflictor, entity attacker, string weapon)
{
	entity a = spawn();
	a.classname = "csnz_ampoule";
	setmodel(a, "models/w_backpack.mdl");
	setsize(a, [-8, -8, 0], [8, 8, 12]);
	setorigin(a, p.origin);
	a.movetype = MOVETYPE_TOSS;
	a.colormod = [0.4, 1, 0.6];
	p.sm_ampoules = 0;
	userinfo.SetInteger(p, "csnz_ampoules", 0);
	if (p.sm_beastEnd)
		p.sm_beastEnd = 0;
}

void
SM_Frame(void)
{
	for (entity a = world; (a = find(a, ::classname, "csnz_ampoule"));) {
		for (entity p = world; (p = next.Player(p));) {
			if (!CS_Playing(p) || vlen(p.origin - a.origin) > 40 || p.sm_ampoules >= 3)
				continue;
			p.sm_ampoules++;
			userinfo.SetInteger(p, "csnz_ampoules", (int)p.sm_ampoules);
			remove(a);
			break;
		}
	}
	for (entity q = world; (q = next.Player(q));)
		if (q.sm_beastEnd && time >= q.sm_beastEnd && CS_Playing(q)) {
			q.sm_beastEnd = 0;
			vector o = q.origin, ang = q.v_angle;
			float hp = min(100, q.health);
			ents.ChangeToClass(q, "csm_rifleman");
			CS_MoveToSpot(q, o, ang);
			ents.Input(q, "SetHealth", ftos(hp), world);
		}
}

bool
SM_Command(entity p, string cmd)
{
	tokenize(cmd);
	if (!p || argv(0) != "csnz_use")
		return false;
	if (p.sm_ampoules >= 3 && CS_Playing(p) && !p.sm_beastEnd) {
		vector o = p.origin, ang = p.v_angle;
		p.sm_ampoules = 0;
		userinfo.SetInteger(p, "csnz_ampoules", 0);
		ents.ChangeToClass(p, "csm_giant");
		CS_MoveToSpot(p, o, ang);
		ents.Input(p, "SetHealth", "3000", world);
		p.sm_beastEnd = time + 30;	/* DESIGN */
		CS_Banner(sprintf("%s turned into a beast!", p.netname));
	}
	return true;
}

/* --- snowman core sub-type ---
   OBSERVED tooltip: "destroy the enemy's core". Each team has a core entity;
   destroy the other team's core to win. If a core is destroyed it restarts
   after 20 seconds (DESIGN). Cores are placed at team spawn centroids.
   BINARY evidence: SnowmanCore, SnowmanController, snowman_tr_core,
   snowman_ct_core entities in mp.dll strings. */

entity sm_core1;		/* team 1 core */
entity sm_core2;		/* team 2 core */
float sm_core1HP;
float sm_core2HP;
float sm_coreMaxHP;
float sm_core1Restart;
float sm_core2Restart;
bool sm_coreMode;

var float autocvar_sm_core_hp = 3000;		/* DESIGN: core HP */
var float autocvar_sm_core_restart = 20;	/* DESIGN: restart delay */

void
SM_SpawnCore(float whichTeam)
{
	/* find team spawn centroid */
	vector spot = [0, 0, 0];
	float n = 0;
	string cls = (whichTeam == CS_TEAM1) ? "info_player_deathmatch" : "info_player_start";
	for (entity s = world; (s = find(s, ::classname, cls));) {
		spot += s.origin;
		n++;
	}
	if (n > 0) spot = spot / n;
	spot[2] += 48;	/* raise above ground */

	entity core = spawn();
	core.classname = (whichTeam == CS_TEAM1) ? "snowman_tr_core" : "snowman_ct_core";
	setmodel(core, "models/w_backpack.mdl");	/* placeholder model */
	setsize(core, [-24, -24, 0], [24, 24, 48]);
	setorigin(core, spot);
	core.solid = SOLID_BBOX;
	core.takedamage = DAMAGE_YES;
	core.health = autocvar_sm_core_hp;
	core.colormod = (whichTeam == CS_TEAM1) ? [1, 0.3, 0.3] : [0.3, 0.3, 1];
	core.team = whichTeam;

	if (whichTeam == CS_TEAM1) {
		sm_core1 = core;
		sm_core1HP = autocvar_sm_core_hp;
	} else {
		sm_core2 = core;
		sm_core2HP = autocvar_sm_core_hp;
	}
}

void
SM_CoreRoundStart(void)
{
	sm_coreMaxHP = autocvar_sm_core_hp;
	sm_core1Restart = 0;
	sm_core2Restart = 0;
	SM_SpawnCore(CS_TEAM1);
	SM_SpawnCore(CS_TEAM2);
	serverinfo.SetInteger("csnz_core1_hp", (int)sm_core1HP);
	serverinfo.SetInteger("csnz_core2_hp", (int)sm_core2HP);
	serverinfo.SetInteger("csnz_core_max", (int)sm_coreMaxHP);
}

void
SM_CoreDamaged(entity victim, entity attacker)
{
	/* check if a core was damaged */
	if (victim.classname == "snowman_tr_core" && attacker.team == CS_TEAM2) {
		sm_core1HP = max(0, victim.health);
		serverinfo.SetInteger("csnz_core1_hp", (int)sm_core1HP);
		if (sm_core1HP <= 0) {
			CS_Banner("Team 2 destroyed Team 1's core!");
			remove(victim);
			sm_core1 = world;
			sm_core1Restart = time + autocvar_sm_core_restart;
		}
	} else if (victim.classname == "snowman_ct_core" && attacker.team == CS_TEAM1) {
		sm_core2HP = max(0, victim.health);
		serverinfo.SetInteger("csnz_core2_hp", (int)sm_core2HP);
		if (sm_core2HP <= 0) {
			CS_Banner("Team 1 destroyed Team 2's core!");
			remove(victim);
			sm_core2 = world;
			sm_core2Restart = time + autocvar_sm_core_restart;
		}
	}
}

void
SM_CoreFrame(void)
{
	SM_Frame();	/* run the base ampoule/beast logic too */

	/* respawn destroyed cores */
	if (sm_core1Restart && time >= sm_core1Restart) {
		SM_SpawnCore(CS_TEAM1);
		sm_core1Restart = 0;
		CS_Banner("Team 1's core has been restored!");
	}
	if (sm_core2Restart && time >= sm_core2Restart) {
		SM_SpawnCore(CS_TEAM2);
		sm_core2Restart = 0;
		CS_Banner("Team 2's core has been restored!");
	}
}

float
SM_CoreCheckRound(void)
{
	/* core mode: score-based, no instant win from core destruction
	   (cores respawn). Winner determined by kill/score limit. */
	return 0;
}

void
CSM_Snowman(void)
{
	CSM_TeamDeathmatch(false);
	cs_modeTitle = "CSO_GameMode_Snowman";
	cs_modeFallback = "Snowman";
	M_Killed = SM_Killed;
	M_Frame = SM_Frame;
	M_Command = SM_Command;
}

/* snowman core variant: enabled when map has snowman_*_core entities or
   when explicitly set via cvar */
var bool autocvar_sm_core_mode = false;

void
CSM_SnowmanCore(void)
{
	CSM_TeamDeathmatch(false);
	cs_modeTitle = "CSO_GameMode_Snowman";
	cs_modeFallback = "Snowman (Core)";
	sm_coreMode = true;
	M_Killed = SM_Killed;
	M_RoundStart = SM_CoreRoundStart;
	M_Frame = SM_CoreFrame;
	M_Damaged = SM_CoreDamaged;
	M_CheckRound = SM_CoreCheckRound;
	M_Command = SM_Command;
}
