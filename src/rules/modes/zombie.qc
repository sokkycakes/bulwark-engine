/* zombie: the infection modes of csnz.dat. Zombi (8) itself is zombi.dat,
 * owned by the Zombi thread; these are the other ids, built on one core.
 * Original code (ISC).
 *
 * mp.dll rules class of each id (SOURCE-CONFIRMED: InstallGameRules,
 * FUN_11412000, and the constructor chains under it):
 *   9 Zombi2, 58 Zombi3Classic (VoxelPlay<Zombi2>)          classic infection
 *   24 ZombiBot (base Zombi3)                               classic infection, bots
 *   14 Zombi3, 44 SeasonZombieHero (base Zombi3)            regen, revives, heroes, ATT bonus
 *   45 ZombieZMode (base Zombi3)                            the Zombi3 set + mutations
 *   32/35/36 Zombi3ExRenewal (base Zombi3),
 *   43 SeasonZombieEx (base Zombi3ExRenewal)                the Zombi3 set in equal teams
 *   29 Zombie4Reboot (base Zombi2)                          day and night
 *   20 ZombiEscape (base Zombi3)                            escape zone, endless host zombie
 *   30 ZombieBoss (base DM, not Zombi)                      one giant boss, points
 *   37 Mugoonhwa (base CSOGameRules, not Zombi)             real and decoy zombies race to the goal
 *   46/47 ZombieTouchdown (base Zombi3)                     flag to the touchdown zone, sides rotate
 * The QC flags below stand for what each class adds; which of the Zombi3
 * features the subclasses keep or override is not decompiled yet.
 *
 * Evidence: the OBSERVED tips are quoted where used (resource/cso_na_en).
 * Numbers without a citation are DESIGN. */

#define ZV_HUMAN CS_TEAM2
#define ZV_ZOMBIE CS_TEAM1

var float autocvar_zv_origin_ratio = 10;	/* DESIGN: one origin zombie per 10 players */
var float autocvar_zv_knockback = 220;		/* DESIGN; OBSERVED: zombies are pushed back */
var float autocvar_zv_stun_time = 0.3;		/* DESIGN; OBSERVED: momentarily stunned */

/* variant switches */
bool zv_regen;			/* OBSERVED ZM2/ZM3: regain HP standing still, unhit, 3 s */
float zv_revives;		/* ZM3: zombies revive (DESIGN count, headshots are not known) */
bool zv_heroes;			/* ZM3: hero humans */
bool zv_attBonus;		/* ZM3: +10% ATT per zombie kill (OBSERVED) */
bool zv_dayNight;		/* ZM4 */
bool zv_equalTeams;		/* Exterminate */
bool zv_noInfect;		/* SeasonZombie: "Zombies: Kill all humans" */
bool zv_escape;
bool zv_giant;
bool zv_tag;
bool zv_touchdown;
bool zv_mutate;			/* Zombi_3z */
bool zv_endlessHost;		/* ZE: "The Host Zombie has unlimited health" (OBSERVED) */
bool zv_zombieRespawn;		/* ZE: "You will respawn if you die as a normal zombie" */

.float zv_isOrigin;
.float zv_still;		/* time the zombie last moved or was hit */
.vector zv_lastPos;
.float zv_revivesLeft;
.float zv_attLevel;
.float zv_lastHealth;
.float zv_stunEnd;
.float zv_real;			/* zombie tag: the real one */
.float zv_virus;		/* Zombi_3z points */
.float zv_virusSent;		/* last zv_virus published as csnz_virus */
.float zv_mutations;
.float zv_mutSpeed;
.float zv_hasFlag;
.float zv_infectAt;
.vector zv_infectOrigin;

float zv_night;
float zv_nextCycle;
entity zv_flag;
vector zv_flagHome;
float zv_flagReturnAt;
float zv_attackers;		/* touchdown: the team attacking this round */
entity zv_giantPlayer;

bool zv_hasFlagDrop(entity p);
vector zv_goalSpot;		/* fallback goal when the map has no escape zone */
bool zv_testCarry;

/* average position of a team's spawn points */
vector
ZV_TeamCentroid(float whichTeam)
{
	vector sum = [0, 0, 0];
	float n = 0;
	string classes = CS_SpawnClasses(whichTeam);
	tokenize(classes);
	string own = argv(0);
	for (entity s = world; (s = find(s, ::classname, own));) {
		sum += s.origin;
		n++;
	}
	return n ? sum / n : [0, 0, 0];
}

bool
ZV_IsZombie(entity p)
{
	return p.team == ZV_ZOMBIE;
}

void
ZV_EquipHuman(entity p)
{
	ents.ChangeToClass(p, "csm_rifleman");
	ents.Input(p, "Alpha", "1", world);
	p.zv_attLevel = 0;
	p.zv_hasFlag = 0;
}

/* turns a player into a zombie where they stand */
void
ZV_MakeZombie(entity p, bool isOrigin, vector where, vector ang)
{
	string cls = isOrigin ? "csm_zombie_origin" : "csm_zombie";
	if (zv_giant)
		cls = "csm_giant";

	ZA_BecomeZombie(p, cls);
	CS_SetTeam(p, ZV_ZOMBIE);
	CS_MoveToSpot(p, where, ang);
	p.zv_isOrigin = isOrigin;
	p.zv_still = time;
	p.zv_lastPos = p.origin;
	p.zv_infectAt = 0;
	p.zv_real = 0;
	p.takedamage = DAMAGE_YES;
	p.cs_protectEnd = 0;
	if (isOrigin && zv_endlessHost) {
		ents.Input(p, "SetMaxHealth", "999999", world);
		ents.Input(p, "SetHealth", "999999", world);
	}
	if (zv_night && !zv_giant)
		ents.Input(p, "SetHealth", ftos(p.health * 1.5), world);
	if (!p.zv_revivesLeft && zv_revives > 0)
		p.zv_revivesLeft = zv_revives;
}

void
ZV_Equip(entity p)
{
	/* a fresh round: everyone starts human (touchdown: by side) */
	if (zv_touchdown && p.team == zv_attackers) {
		ZA_BecomeZombie(p, "csm_zombie");
		return;
	}
	if (zv_equalTeams && p.team == ZV_ZOMBIE) {
		ZA_BecomeZombie(p, "csm_zombie");
		return;
	}
	ZV_EquipHuman(p);
}

float
ZV_PickTeam(entity p)
{
	/* teams are made at selection; joiners during a live round are zombies */
	if (zv_touchdown || zv_equalTeams)
		return CS_AutoTeam(p);
	if (cs_state == CS_LIVE)
		return ZV_ZOMBIE;
	return ZV_HUMAN;
}

void
ZV_RoundStart(void)
{
	zv_night = 0;
	zv_nextCycle = time + 60;
	zv_giantPlayer = world;
	/* DESIGN fallback goal: the far side, where team 1 (TR) spawns sit on
	   CS maps, away from the humans' (CT) spawns */
	zv_goalSpot = ZV_TeamCentroid(CS_TEAM1);
	lightstyle(0, "m");
	serverinfo.SetInteger("csnz_night", 0);

	if (zv_touchdown) {
		/* OBSERVED strings RotationToHuman / RotationToZombie: sides swap */
		zv_attackers = (cs_round & 1) ? CS_TEAM1 : CS_TEAM2;
		zv_goalSpot = ZV_TeamCentroid(zv_attackers == CS_TEAM1 ? CS_TEAM2 : CS_TEAM1);
		serverinfo.SetInteger("csnz_attackers", (int)zv_attackers);
		for (entity p = world; (p = next.Player(p));)
			ZV_Equip(p);
	} else if (!zv_equalTeams) {
		for (entity q = world; (q = next.Player(q));)
			if (q.team != ZV_HUMAN)
				CS_SetTeam(q, ZV_HUMAN);
	}
	for (entity r = world; (r = next.Player(r));) {
		r.zv_revivesLeft = 0;
		r.zv_hasFlag = 0;
	}
}

/* a player joining a live infection round comes in as a zombie (DESIGN,
   as zombi.dat does) */
bool
ZV_LateJoin(entity p)
{
	if (!cs_oneSideStart || zv_giant)
		return false;
	entity s = CS_PickSpawn(ZV_ZOMBIE);
	ents.ChangeToClass(p, "csm_zombie");
	ZV_MakeZombie(p, false, s ? s.origin : p.origin, s ? s.angles : p.angles);
	return true;
}

/* heroes: OBSERVED "unique Heroes for Humans"; DESIGN: one per 8 players */
void
ZV_PickHeroes(int players)
{
	if (!zv_heroes || players < 4)
		return;
	int heroes = max(1, floor(players / 8));
	for (entity h = world; (h = next.Player(h)) && heroes > 0;) {
		if (CS_Playing(h) && !ZV_IsZombie(h) && random() < 0.5) {
			ents.ChangeToClass(h, "csm_hero");
			CS_Tell(h, "You are a Hero.");
			heroes--;
		}
	}
}

/* picks the origin zombies when the countdown (freeze time) ends */
void
ZV_Live(void)
{
	int players = 0, want, picked = 0;
	string forced = cvars.GetString("cs_test_origin");

	if (zv_touchdown) {
		/* the flag waits at the attackers' spawn */
		entity home = CS_PickSpawn(zv_attackers);
		zv_flagHome = home ? home.origin : [0, 0, 0];
		if (zv_flag)
			remove(zv_flag);
		zv_flag = spawn();
		zv_flag.classname = "csnz_flag";
		setmodel(zv_flag, "models/w_backpack.mdl");
		setsize(zv_flag, [-8, -8, 0], [8, 8, 16]);
		setorigin(zv_flag, zv_flagHome);
		zv_flag.colormod = [1, 0.3, 0.3];
		zv_flag.movetype = MOVETYPE_TOSS;
		return;
	}
	for (entity p = world; (p = next.Player(p));)
		if (CS_Playing(p))
			players++;

	if (zv_equalTeams) {
		ZV_PickHeroes(players);
		return;
	}

	want = zv_giant ? 1 : max(1, ceil(players / max(1, autocvar_zv_origin_ratio)));
	if (zv_tag)
		want = max(2, ceil(players / 3));	/* DESIGN: a third are zombies */
	if (want >= players)
		want = players - 1;

	/* test hook: cs_test_origin <name> is picked first */
	if (forced != "")
		for (entity f = world; (f = next.Player(f));)
			if (f.netname == forced && CS_Playing(f) && picked < want) {
				ZV_MakeZombie(f, true, f.origin, f.v_angle);
				picked++;
			}

	while (picked < want) {
		int pool = 0;
		for (entity c = world; (c = next.Player(c));)
			if (CS_Playing(c) && !ZV_IsZombie(c))
				pool++;
		if (pool <= 1)
			break;
		int pick = (int)floor(random() * pool);
		for (entity d = world; (d = next.Player(d));) {
			if (!CS_Playing(d) || ZV_IsZombie(d))
				continue;
			if (pick-- == 0) {
				ZV_MakeZombie(d, true, d.origin, d.v_angle);
				picked++;
				break;
			}
		}
	}

	if (zv_giant)
		for (entity g = world; (g = next.Player(g));)
			if (ZV_IsZombie(g))
				zv_giantPlayer = g;

	/* zombie tag: half of the zombies are real (DESIGN), the rest decoys */
	if (zv_tag) {
		int z = 0;
		for (entity t = world; (t = next.Player(t));)
			if (ZV_IsZombie(t)) {
				t.zv_real = (z++ & 1) == 0;
				userinfo.SetInteger(t, "csnz_real", (int)t.zv_real);
			}
	}

	ZV_PickHeroes(players);

	CS_Banner(CS_Text("CSO_ZombiAppear", "The zombies have appeared!"));
	printf("csnz %.1f: %i origin zombies\n", time, picked);
}

void
ZV_Infect(entity victim, entity attacker)
{
	if (zv_noInfect || zv_touchdown || zv_giant || zv_tag)
		return;
	attacker.frags++;
	attacker.cs_kills++;
	printf("csnz %.1f: %s infected %s\n", time, attacker.netname, victim.netname);
	ZV_MakeZombie(victim, false, victim.origin, victim.v_angle);
}

void
ZV_Damaged(entity victim, entity attacker)
{
	float dealt = victim.zv_lastHealth - victim.health;
	victim.zv_lastHealth = victim.health;

	if (cs_state != CS_LIVE || !(attacker.flags & FL_CLIENT) || attacker == victim)
		return;

	if (ZV_IsZombie(attacker) && !ZV_IsZombie(victim)) {
		ZV_Infect(victim, attacker);
		return;
	}
	if (!ZV_IsZombie(attacker) && ZV_IsZombie(victim)) {
		victim.zv_still = time;
		vector dir = normalize(victim.origin - attacker.origin);
		dir[2] = max(dir[2], 0.1);
		if (!zv_giant)
			ents.Input(victim, "AddVelocity", vtos(dir * autocvar_zv_knockback), world);
		victim.zv_stunEnd = time + autocvar_zv_stun_time;

		/* OBSERVED ZM3: ATT +10% per zombie killed: the extra share is
		   taken off the zombie's health here */
		if (zv_attBonus && attacker.zv_attLevel > 0 && dealt > 0)
			ents.Input(victim, "SetHealth", ftos(max(1, victim.health - dealt * 0.1 * attacker.zv_attLevel)), world);
		if (zv_mutate)
			attacker.zv_virus += dealt;
		if (zv_giant)
			attacker.cs_score += dealt / 1000;	/* DESIGN: a point per 1000 damage */
		victim.zv_lastHealth = victim.health;
	}
}

void
ZV_Killed(entity p, entity inflictor, entity attacker, string weapon)
{
	if (cs_state != CS_LIVE)
		return;

	if (ZV_IsZombie(p)) {
		if ((attacker.flags & FL_CLIENT) && !ZV_IsZombie(attacker)) {
			attacker.zv_attLevel++;
			if (zv_giant) {
				attacker.cs_score += 5;
				CS_Banner(sprintf("%s brought down the Giant", attacker.netname));
			}
		}
		zv_hasFlagDrop(p);
		/* revives (ZM3) and normal zombie respawn (ZE) */
		if (p.zv_revivesLeft > 0) {
			p.zv_revivesLeft--;
			p.zv_infectAt = time + 3;
			p.zv_infectOrigin = p.origin;
		} else if (zv_zombieRespawn && !p.zv_isOrigin) {
			p.zv_infectAt = time + 3;
			p.zv_infectOrigin = [0, 0, 0];
		}
		return;
	}

	/* a human killed by a zombie rises as one (classic) */
	if ((attacker.flags & FL_CLIENT) && ZV_IsZombie(attacker)) {
		if (zv_giant) {
			attacker.cs_score += 1;
			return;
		}
		if (zv_noInfect || zv_touchdown || zv_tag)
			return;
		p.zv_infectAt = time + 0.5;
		p.zv_infectOrigin = p.origin;
	}
}

/* the touchdown flag carrier drops it where they fall */
bool
zv_hasFlagDrop(entity p)
{
	if (!p.zv_hasFlag)
		return false;
	p.zv_hasFlag = 0;
	if (zv_flag) {
		setorigin(zv_flag, p.origin);
		zv_flag.solid = SOLID_NOT;
		zv_flagReturnAt = time + 10;	/* DESIGN; OBSERVED Recovery_Start/End */
		CS_Banner(CS_Text("CSO_Touchdown_Recovery_Start", "The flag was dropped"));
	}
	return true;
}

/* test hooks: cs_test_hescape (living humans to the escape zone),
   cs_test_tagescape (a real zombie to the goal), cs_test_touchdown (an
   attacker takes the flag to the goal) */
vector
ZV_GoalPoint(void)
{
	entity z = find(world, ::classname, "func_escapezone");
	if (!z)
		z = find(world, ::classname, "csnz_escape_goal");
	if (z)
		return (z.absmin != z.absmax ? (z.absmin + z.absmax) * 0.5 : z.origin) + [0, 0, 36];
	return zv_goalSpot + [0, 0, 36];
}

void
ZV_TestHooks(void)
{
	if (cvars.GetBool("cs_test_hescape")) {
		cvars.SetString("cs_test_hescape", "0");
		float k = 0;
		for (entity h = world; (h = next.Player(h));)
			if (CS_Playing(h) && !ZV_IsZombie(h))
				setorigin(h, ZV_GoalPoint() + [0, 40 * k++, 0]);
	}
	if (cvars.GetBool("cs_test_tagescape")) {
		cvars.SetString("cs_test_tagescape", "0");
		for (entity t = world; (t = next.Player(t));)
			if (CS_Playing(t) && t.zv_real) {
				setorigin(t, ZV_GoalPoint());
				break;
			}
	}
	if (cvars.GetBool("cs_test_touchdown") && zv_flag) {
		cvars.SetString("cs_test_touchdown", "0");
		for (entity a = world; (a = next.Player(a));)
			if (CS_Playing(a) && a.team == zv_attackers) {
				setorigin(a, zv_flag.origin + [0, 0, 16]);
				zv_testCarry = true;
				break;
			}
	}
	/* the carrier then runs to the goal */
	if (zv_testCarry)
		for (entity c = world; (c = next.Player(c));)
			if (c.zv_hasFlag && CS_Playing(c)) {
				setorigin(c, ZV_GoalPoint());
				zv_testCarry = false;
			}
}

void
ZV_Frame(void)
{
	ZV_TestHooks();
	for (entity p = world; (p = next.Player(p));) {
		if (p.zv_infectAt > 0 && time >= p.zv_infectAt) {
			vector at = p.zv_infectOrigin;
			p.zv_infectAt = 0;
			if (at == [0, 0, 0]) {
				entity s = CS_PickSpawn(ZV_ZOMBIE);
				at = s ? s.origin : p.origin;
			}
			ZV_MakeZombie(p, false, at, p.v_angle);
		}
		if (!CS_Playing(p))
			continue;

		if (p.zv_stunEnd > time) {
			p.velocity[0] *= 0.5;
			p.velocity[1] *= 0.5;
		}

		if (ZV_IsZombie(p)) {
			/* OBSERVED ZM2/ZM3: still and unhit for 3 s regains HP */
			if (vlen(p.origin - p.zv_lastPos) > 4) {
				p.zv_still = time;
				p.zv_lastPos = p.origin;
			}
			if ((zv_regen || zv_night) && time - p.zv_still >= 3 && p.health < p.max_health)
				ents.Input(p, "SetHealth", ftos(min(p.max_health, p.health + 200 * frametime)), world);
			if (zv_night && p.zv_stunEnd < time) {
				p.velocity[0] *= 1.01;
				p.velocity[1] *= 1.01;
			}
		}
		p.zv_lastHealth = p.health;

		if (zv_mutate && p.zv_virus >= 1000 * (p.zv_mutations + 1)) {
			userinfo.SetInteger(p, "csnz_canmutate", 1);
		}
		/* the HUD's "Virus" line (hud/progression.qc), as Zombi 5 does */
		if (zv_mutate && floor(p.zv_virus) != p.zv_virusSent) {
			p.zv_virusSent = floor(p.zv_virus);
			userinfo.SetInteger(p, "csnz_virus", (int)p.zv_virusSent);
		}
		if (p.zv_mutSpeed > 0) {
			p.velocity[0] *= 1 + 0.002 * p.zv_mutSpeed;
			p.velocity[1] *= 1 + 0.002 * p.zv_mutSpeed;
		}
	}

	/* Zombi_4: OBSERVED "When it gets dark, zombies will be much stronger";
	   DESIGN cycle of 60 s day and 30 s night, drawn with lightstyle 0 */
	if (zv_dayNight && time >= zv_nextCycle) {
		zv_night = !zv_night;
		zv_nextCycle = time + (zv_night ? 30 : 60);
		lightstyle(0, zv_night ? "c" : "m");
		serverinfo.SetInteger("csnz_night", (int)zv_night);
		CS_Banner(zv_night ? "Night falls. The zombies grow stronger." : "Day breaks.");
	}

	/* touchdown flag */
	if (zv_touchdown && zv_flag) {
		entity carrier = world;
		for (entity c = world; (c = next.Player(c));)
			if (c.zv_hasFlag && CS_Playing(c))
				carrier = c;
		if (carrier) {
			setorigin(zv_flag, carrier.origin + [0, 0, 64]);
		} else {
			for (entity t = world; (t = next.Player(t));) {
				if (!CS_Playing(t) || t.team != zv_attackers || vlen(t.origin - zv_flag.origin) > 48)
					continue;
				t.zv_hasFlag = 1;
				zv_flagReturnAt = 0;
				CS_Banner(sprintf("%s %s", t.netname, CS_Text("CSO_Touchdown_GetFlag", "has the flag")));
				break;
			}
			if (zv_flagReturnAt > 0 && time >= zv_flagReturnAt) {
				setorigin(zv_flag, zv_flagHome);
				zv_flagReturnAt = 0;
				CS_Banner(CS_Text("CSO_Touchdown_Recovery_End", "The flag was returned"));
			}
		}
	}
}

/* the zone humans escape to (ZE) or zombies run to (tag, touchdown):
   func_escapezone on CS maps, the ESCAPE_GOAL Studio block's marker, else
   the far side: the defenders' spawn (DESIGN) */
bool
ZV_InGoal(entity p, float defenders)
{
	if (CS_ZoneExists("func_escapezone") || CS_ZoneExists("csnz_escape_goal"))
		return CS_InZone(p, "func_escapezone", 128) || CS_InZone(p, "csnz_escape_goal", 64);
	vector d = zv_goalSpot - p.origin;
	d[2] = 0;
	return vlen(d) < 160;
}

float
ZV_Check(void)
{
	int humans = CS_Alive(ZV_HUMAN), zombies = CS_Alive(ZV_ZOMBIE);
	int pendingZombies = 0;

	for (entity q = world; (q = next.Player(q));)
		if (q.zv_infectAt > 0)
			pendingZombies++;

	if (zv_touchdown) {
		float defenders = zv_attackers == CS_TEAM1 ? CS_TEAM2 : CS_TEAM1;
		for (entity c = world; (c = next.Player(c));)
			if (c.zv_hasFlag && CS_Playing(c) && ZV_InGoal(c, defenders)) {
				CS_Banner(CS_Text("CSO_Touchdown_Touchdown", "Touchdown!"));
				return zv_attackers;
			}
		if (CS_Alive(zv_attackers) == 0)
			return defenders;
		return 0;
	}

	if (zv_tag) {
		int real = 0;
		for (entity t = world; (t = next.Player(t));) {
			if (!CS_Playing(t) || !t.zv_real)
				continue;
			real++;
			if (ZV_InGoal(t, ZV_HUMAN)) {
				CS_Banner("A real zombie escaped!");
				return ZV_ZOMBIE;
			}
		}
		if (real == 0) {
			CS_Banner("All real zombies were found!");
			return ZV_HUMAN;
		}
		return 0;
	}

	if (zv_escape) {
		int inZone = 0, zombiesInZone = 0;
		for (entity e = world; (e = next.Player(e));) {
			if (!CS_Playing(e) || !ZV_InGoal(e, ZV_HUMAN))
				continue;
			if (ZV_IsZombie(e))
				zombiesInZone++;
			else
				inZone++;
		}
		/* OBSERVED ZE: humans reach the escape area; zombies reaching it
		   prevent the escape */
		if (inZone > 0 && zombiesInZone == 0) {
			CS_Banner(CS_Text("CSO_ZE_Escape_Success", "The humans have escaped!"));
			return ZV_HUMAN;
		}
	}

	if (zv_giant) {
		/* the boss died: another random player becomes the Giant (OBSERVED) */
		if (!zv_giantPlayer || !CS_Playing(zv_giantPlayer)) {
			for (entity g = world; (g = next.Player(g));)
				if (ZV_IsZombie(g) && !CS_Playing(g))
					CS_SetTeam(g, ZV_HUMAN);
			ZV_Live();
		}
		return 0;
	}

	if (humans == 0 && pendingZombies == 0 && zombies > 0)
		return ZV_ZOMBIE;
	if (humans == 0 && zombies == 0)
		return 3;
	if (zombies == 0 && pendingZombies == 0) {
		/* revives pending also keep the round going */
		for (entity r = world; (r = next.Player(r));)
			if (r.zv_revivesLeft > 0 && ZV_IsZombie(r))
				return 0;
		if (zv_zombieRespawn)
			return 0;
		return ZV_HUMAN;
	}
	return 0;
}

void
ZV_RoundEnded(float winner)
{
	if (zv_flag) {
		remove(zv_flag);
		zv_flag = world;
	}
	lightstyle(0, "m");
	if (zv_touchdown) {
		if (winner == zv_attackers)
			CS_Banner("Attack successful");
		else if (winner)
			CS_Banner("Defense successful");
		else
			CS_Banner(CS_Text("CSO_RoundDraw", "Round Draw"));
		return;
	}
	if (winner == ZV_HUMAN)
		CS_Banner(CS_Text("CSO_Human_Win", "Humans Win"));
	else if (winner == ZV_ZOMBIE)
		CS_Banner(CS_Text("CSO_Zombie_Win", "Zombies Win"));
	else
		CS_Banner(CS_Text("CSO_RoundDraw", "Round Draw"));
}

/* Zombi_3z: OBSERVED "mutate using the 'V' key" for random abilities;
   the client sends "cmd csnz_mutate". The ability list is DESIGN. */
bool
ZV_Command(entity p, string cmd)
{
	tokenize(cmd);
	if (argv(0) != "csnz_mutate" || !zv_mutate || !p)
		return false;
	if (p.zv_virus < 1000 * (p.zv_mutations + 1)) {
		sprint(p, PRINT_HIGH, "Not enough virus points to mutate.\n");
		return true;
	}
	p.zv_mutations++;
	userinfo.SetInteger(p, "csnz_canmutate", 0);
	switch ((int)floor(random() * 3)) {
	case 0:
		ents.Input(p, "SetMaxHealth", ftos(p.max_health + 50), world);
		ents.Input(p, "SetHealth", ftos(p.health + 50), world);
		CS_Tell(p, "Mutation: toughness");
		break;
	case 1:
		p.zv_mutSpeed++;
		CS_Tell(p, "Mutation: speed");
		break;
	default:
		p.zv_attLevel += 2;
		CS_Tell(p, "Mutation: strength");
		break;
	}
	userinfo.SetInteger(p, "csnz_mutations", (int)p.zv_mutations);
	return true;
}

/* equal teams for Exterminate / Season Zombie */
void
ZV_EqualStart(void)
{
	int i = 0;
	for (entity p = world; (p = next.Player(p));)
		CS_SetTeam(p, (i++ & 1) ? ZV_ZOMBIE : ZV_HUMAN);
}

/* what Zombi3 (14) adds to the infection core; every class derived from
   it starts from this set */
void
ZV_Zombi3(void)
{
	zv_regen = true;	/* OBSERVED ZM3 */
	zv_revives = 2;		/* DESIGN count */
	zv_heroes = true;
	zv_attBonus = true;
}

void
CSM_Zombie(float id)
{
	cs_modeTitle = "CSO_GameMode_Zombi_Expand";
	cs_modeFallback = "Zombie";
	cs_teams = true;
	cs_rounds = true;
	cs_respawnDelay = -1;
	cs_scoreKills = false;
	cs_winLimitFallback = 13;
	cs_timeLimitFallback = 20;
	cs_roundTimeFallback = 3;
	cs_roundTimeWinsTeam2 = true;	/* humans survive the time (OBSERVED ZM tip) */
	cs_team1Name = "Zombie";
	cs_team2Name = "Human";
	cs_noBuy = true;
	cs_minPlayers = 2;
	cs_oneSideStart = true;
	autocvar_mp_freezetime = max(autocvar_mp_freezetime, 10);	/* the infection countdown */
	M_Equip = ZV_Equip;
	M_PickTeam = ZV_PickTeam;
	M_RoundStart = ZV_RoundStart;
	M_Live = ZV_Live;
	M_Frame = ZV_Frame;
	M_Damaged = ZV_Damaged;
	M_Killed = ZV_Killed;
	M_CheckRound = ZV_Check;
	M_RoundEnded = ZV_RoundEnded;
	M_Command = ZV_Command;
	M_LateJoin = ZV_LateJoin;

	switch (id) {
	case 9:
		break;
	case 58:
		/* Zombi3Classic derives from Zombi2 (the class of 9), not Zombi3,
		   so no heroes (SOURCE-CONFIRMED base). Its functions name
		   ZombieRandomWeaponMode and support item drops
		   (#CSO_SupportItemSpawned), not ported yet. No title text in
		   this build (DESIGN name from EGM_ZOMBI_3_CLASSIC). */
		cs_modeTitle = "CSO_GameMode_Zombi_3_Classic";
		cs_modeFallback = "Zombie Hero Classic";
		break;
	case 24:
		cs_modeTitle = "CSO_GameMode_BotZombie";
		cs_modeFallback = "Bot Zombie";
		cs_noStats = true;
		CSM_DefaultBots(9);
		break;
	case 14:
	case 44:
		cs_modeTitle = id == 14 ? "CSO_GameMode_Zombi_3" : "CSO_GameMode_SeasonZombi_3";
		cs_modeFallback = id == 14 ? "Zombie Hero" : "Ranked Mode: Zombie Hero";
		ZV_Zombi3();
		break;
	case 45:
		/* ZombieZMode derives from Zombi3 (SOURCE-CONFIRMED), so it keeps
		   the hero set; its own functions name "herof"/"herom" */
		cs_modeTitle = "CSO_GameMode_Zombi_3z";
		cs_modeFallback = "Zombie Z";
		ZV_Zombi3();
		zv_mutate = true;
		break;
	case 29:
		cs_modeTitle = "CSO_GameMode_Zombi_4";
		cs_modeFallback = "Zombie Darkness";
		zv_regen = true;
		zv_dayNight = true;
		break;
	case 32:
	case 35:
	case 36:
	case 43:
		/* one class, Zombi3ExRenewal, for 32/35/36; 43 SeasonZombieEx
		   derives from it. OBSERVED ZME tooltip: "Zombie Hero in a form of
		   team vs team" */
		switch (id) {
		case 32:
			cs_modeTitle = "CSO_GameMode_Zombi_Exterminate";
			cs_modeFallback = "Zombie War";
			break;
		case 35:
		case 36:
			cs_modeTitle = id == 35 ? "CSO_GameMode_ZombieOfficial" : "CSO_GameMode_ZombieOfficial_TieBreak";
			cs_modeFallback = "Zombie Official";
			cs_noStats = true;
			break;
		default:
			cs_modeTitle = "CSO_GameMode_SeasonZombieEx";
			cs_modeFallback = "Ranked Zombie War";
		}
		ZV_Zombi3();
		zv_equalTeams = true;
		cs_oneSideStart = false;
		cs_roundTimeWinsTeam2 = false;	/* OBSERVED ZME: a draw on time */
		cs_winLimitFallback = 5;	/* OBSERVED limitlist id 34 default */
		cs_roundTimeFallback = 2;	/* OBSERVED limitlist id 33 default */
		M_Start = ZV_EqualStart;
		if (id == 43) {
			/* OBSERVED SeasonZombie tip: "Kill all humans"; 8 rounds,
			   first to 5, 1:45 */
			zv_noInfect = true;
			cvars.SetFloat("mp_winlimit", 5);
			cvars.SetFloat("mp_roundtime", 1.75);
		}
		break;
	case 20:
		cs_modeTitle = "CSO_GameMode_Zombi_Escape";
		cs_modeFallback = "Zombie Escape";
		zv_escape = true;
		zv_endlessHost = true;
		zv_zombieRespawn = true;
		cs_roundTimeWinsTeam2 = false;	/* not escaping in time loses */
		break;
	case 30:
		cs_modeTitle = "CSO_GameMode_ZombieGiant";
		cs_modeFallback = "Zombie Giant";
		zv_giant = true;
		cs_rounds = false;
		cs_scoreKills = false;
		cs_scorePoints = true;
		cs_respawnDelay = 3;
		cs_killLimitFallback = 160;
		cs_timeLimitFallback = 30;	/* OBSERVED limitlist id 32 default */
		break;
	case 37:
		cs_modeTitle = "CSO_GameMode_ZombieTag";
		cs_modeFallback = "Zombie Tag";
		zv_tag = true;
		cs_roundTimeWinsTeam2 = true;	/* OBSERVED: humans win by preventing the escape */
		cs_roundTimeFallback = 4;	/* OBSERVED limitlist id 36 default */
		break;
	case 46:
	case 47:
		cs_modeTitle = "CSO_GameMode_ZombieTouchDown";
		cs_modeFallback = "Zombie Touchdown";
		zv_touchdown = true;
		cs_oneSideStart = false;
		cs_roundTimeWinsTeam2 = false;
		break;
	}
}

