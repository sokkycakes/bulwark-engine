/* objectives: the Original-family map missions (bomb, hostage, VIP,
 * escape), buy zones and the buy command. Original code (ISC).
 *
 * Evidence
 *   OBSERVED (CSO_MaploadingTip_ORIGINAL_*): T plant C4 at the bomb site,
 *     only the carrier can; CT prevent or defuse; 'E' plants and defuses;
 *     a defuse kit shortens defusing; hostages follow a CT who presses 'E'
 *     and stop on a second press; the VIP has only a pistol and 200 armor.
 *   OBSERVED (maplist.csv "Orginal_Mission" column): maps carry one of four
 *     mission types. The column legend is garbled; the type is detected
 *     from the map's own entities instead, as CS does.
 *   OBSERVED (item.csv): buy names m4a1 (CT), ak47 (TR), kevlar,
 *     kevlar_helmet, defuser (CT).
 *   STOCK-CS: every timer, distance and price below (CS 1.6 defaults);
 *     CSO's values are not in the client files.
 *
 * Interaction: the 'E' key must send "cmd csnz_use" next to +use (contract
 * in INTERFACES.md). Bots and the test hooks use the same entry point. */

#define MISSION_NONE 0
#define MISSION_BOMB 1
#define MISSION_HOSTAGE 2
#define MISSION_VIP 3
#define MISSION_ESCAPE 4

var float autocvar_mp_c4timer = 35;		/* STOCK-CS */
var float autocvar_mp_buytime = 1.5;		/* minutes, STOCK-CS */
var float autocvar_csnz_plant_time = 3;		/* STOCK-CS */
var float autocvar_csnz_defuse_time = 10;	/* STOCK-CS, 5 with a kit */

float cs_mission;
bool cs_giveAllC4;		/* LightOri: every T carries C4 (OBSERVED tip) */
bool cs_giveAllKits;		/* LightOri: every CT has a kit (OBSERVED tip) */
float cs_buyEnd;
bool cs_noBuy;			/* modes without the buy menu */

.bool cs_hasC4;
.bool cs_hasKit;
.bool cs_isVip;
.float cs_actionEnd;		/* plant or defuse completes at this time */
.vector cs_actionOrigin;
.float cs_action;		/* 1 plant, 2 defuse */
.entity cs_follow;		/* hostage: the CT it follows */
.float cs_rescued;
.float cs_lossStreak;

entity cs_bomb;			/* planted or dropped C4 */
bool cs_bombPlanted;
float cs_bombBlowAt;
float cs_hostagesTotal;
float cs_hostagesRescued;
float cs_lossStreak1, cs_lossStreak2;

/* --- zones ------------------------------------------------------------ */

bool
CS_InEntity(entity p, entity z, float pointRadius)
{
	vector size = z.absmax - z.absmin;
	/* a real brush zone; a point entity's trigger box is only a few units */
	if (size[0] > 8 && size[1] > 8 && size[2] > 8) {
		return (p.absmin[0] <= z.absmax[0] && p.absmax[0] >= z.absmin[0] &&
			p.absmin[1] <= z.absmax[1] && p.absmax[1] >= z.absmin[1] &&
			p.absmin[2] <= z.absmax[2] && p.absmax[2] >= z.absmin[2]);
	}
	return vlen(p.origin - z.origin) <= pointRadius;
}

/* in any entity of the class; brush zones by bounds, point zones by radius
   (a func_ zone placed as a point, as on Studio and test maps, uses 128) */
bool
CS_InZone(entity p, string cname, float pointRadius)
{
	for (entity z = world; (z = find(z, ::classname, cname));)
		if (CS_InEntity(p, z, pointRadius))
			return true;
	return false;
}

bool
CS_ZoneExists(string cname)
{
	return find(world, ::classname, cname) != world;
}

bool
CS_InBombSite(entity p)
{
	return CS_InZone(p, "func_bomb_target", 128) || CS_InZone(p, "info_bomb_target", 96);
}

/* STOCK-CS: maps without buy zones let each team buy near its spawns */
bool
CS_InBuyZone(entity p)
{
	if (CS_ZoneExists("func_buyzone")) {
		for (entity z = world; (z = find(z, ::classname, "func_buyzone"));)
			if ((!z.team || z.team == p.team) && CS_InEntity(p, z, 128))
				return true;
		return false;
	}

	string classes = CS_SpawnClasses(p.team);
	tokenize(classes);
	string own = argv(0);
	for (entity s = world; (s = find(s, ::classname, own));)
		if (vlen(s.origin - p.origin) < 256)
			return true;
	return false;
}

bool
CS_InRescueZone(entity h)
{
	if (CS_ZoneExists("func_hostage_rescue") || CS_ZoneExists("info_hostage_rescue"))
		return CS_InZone(h, "func_hostage_rescue", 128) || CS_InZone(h, "info_hostage_rescue", 256);
	/* STOCK-CS: no rescue zone means the CT spawn area */
	for (entity s = world; (s = find(s, ::classname, "info_player_counterterrorist"));)
		if (vlen(s.origin - h.origin) < 256)
			return true;
	for (entity s2 = world; (s2 = find(s2, ::classname, "info_player_start"));)
		if (vlen(s2.origin - h.origin) < 256)
			return true;
	return false;
}

float
CS_DetectMission(void)
{
	if (CS_ZoneExists("func_bomb_target") || CS_ZoneExists("info_bomb_target"))
		return MISSION_BOMB;
	if (CS_ZoneExists("hostage_entity") || CS_ZoneExists("monster_scientist"))
		return MISSION_HOSTAGE;
	if (CS_ZoneExists("func_vip_safetyzone"))
		return MISSION_VIP;
	if (CS_ZoneExists("func_escapezone"))
		return MISSION_ESCAPE;
	return MISSION_NONE;
}

string
CS_MissionName(float m)
{
	switch (m) {
	case MISSION_BOMB: return "bomb";
	case MISSION_HOSTAGE: return "hostage";
	case MISSION_VIP: return "vip";
	case MISSION_ESCAPE: return "escape";
	default: return "elimination";
	}
}

/* --- C4 --------------------------------------------------------------- */

void
CS_PublishCarrier(entity p)
{
	userinfo.SetInteger(p, "csnz_c4", p.cs_hasC4 ? 1 : 0);
	userinfo.SetInteger(p, "csnz_kit", p.cs_hasKit ? 1 : 0);
}

void
CS_RemoveBomb(void)
{
	if (cs_bomb)
		remove(cs_bomb);
	cs_bomb = world;
	cs_bombPlanted = false;
	cs_bombBlowAt = 0;
	serverinfo.SetFloat("csnz_c4blow", 0);
}

void
CS_DropBomb(entity p)
{
	if (!p.cs_hasC4)
		return;
	p.cs_hasC4 = false;
	CS_PublishCarrier(p);

	/* with every T carrying C4 (LightOri) nothing is left behind */
	if (cs_giveAllC4)
		return;

	CS_RemoveBomb();
	cs_bomb = spawn();
	cs_bomb.classname = "csnz_c4";
	setmodel(cs_bomb, "models/w_c4.mdl");
	setsize(cs_bomb, [-8, -8, 0], [8, 8, 8]);
	setorigin(cs_bomb, p.origin + [0, 0, 8]);
	cs_bomb.movetype = MOVETYPE_TOSS;
	cs_bomb.solid = SOLID_NOT;
	printf("csnz %.1f: %s dropped the C4\n", time, p.netname);
}

void
CS_GiveBombToRandomT(void)
{
	int n = CS_Alive(CS_TEAM1), pick;

	if (cs_giveAllC4) {
		for (entity q = world; (q = next.Player(q));)
			if (CS_Playing(q) && q.team == CS_TEAM1) {
				q.cs_hasC4 = true;
				CS_PublishCarrier(q);
			}
		return;
	}

	if (n <= 0)
		return;
	pick = (int)floor(random() * n);
	for (entity p = world; (p = next.Player(p));) {
		if (!CS_Playing(p) || p.team != CS_TEAM1)
			continue;
		if (pick-- == 0) {
			p.cs_hasC4 = true;
			CS_PublishCarrier(p);
			CS_Tell(p, CS_Text("CSO_Have_Bomb", "You have the C4 bomb. Plant it at a bomb site."));
			printf("csnz %.1f: %s carries the C4\n", time, p.netname);
			return;
		}
	}
}

void
CS_PlantBomb(entity p)
{
	p.cs_hasC4 = false;
	CS_PublishCarrier(p);
	if (cs_giveAllC4)
		for (entity q = world; (q = next.Player(q));) {
			q.cs_hasC4 = false;
			CS_PublishCarrier(q);
		}

	CS_RemoveBomb();
	cs_bomb = spawn();
	cs_bomb.classname = "csnz_c4";
	setmodel(cs_bomb, "models/w_c4.mdl");
	setsize(cs_bomb, [-8, -8, 0], [8, 8, 8]);
	setorigin(cs_bomb, p.origin);
	cs_bomb.movetype = MOVETYPE_TOSS;
	cs_bombPlanted = true;
	cs_bombBlowAt = time + autocvar_mp_c4timer;
	serverinfo.SetFloat("csnz_c4blow", cs_bombBlowAt);
	CS_GiveMoney(p, 300);	/* STOCK-CS plant bonus */
	ACH_OnBombPlant(p);
	PROG_AwardXP(p, XP_BOMB_PLANT);
	CS_Banner(CS_Text("CSO_Bomb_Planted", "The bomb has been planted!"));
	printf("csnz %.1f: %s planted the C4, detonation in %g s\n", time, p.netname, autocvar_mp_c4timer);

	/* STOCK-CS: a planted bomb keeps the round going past the round timer */
	cs_stateEnd = 0;
}

void
CS_ExplodeBomb(void)
{
	vector at = cs_bomb.origin;
	CS_RemoveBomb();
	/* STOCK-CS: 500 unit blast */
	combat.RadiusDamage(at, 500, 100, 500, world, "");
	CS_Banner(CS_Text("CSO_Target_Bombed", "Target Successfully Bombed!"));
	CS_EndRound(CS_TEAM1, "target bombed");
}

void
CS_DefuseBomb(entity p)
{
	CS_RemoveBomb();
	CS_GiveMoney(p, 300);	/* STOCK-CS defuse bonus */
	ACH_OnBombDefuse(p);
	PROG_AwardXP(p, XP_BOMB_DEFUSE);
	CS_Banner(CS_Text("CSO_Bomb_Defused", "The bomb has been defused!"));
	printf("csnz %.1f: %s defused the C4\n", time, p.netname);
	CS_EndRound(CS_TEAM2, "bomb defused");
}

/* --- hostages ----------------------------------------------------------- */

void
CS_SpawnHostages(void)
{
	cs_hostagesTotal = cs_hostagesRescued = 0;

	for (entity old = world; (old = find(old, ::classname, "csnz_hostage"));)
		remove(old);

	for (entity mark = world; (mark = find(mark, ::classname, "hostage_entity"));) {
		entity h = spawn();
		h.classname = "csnz_hostage";
		setmodel(h, "models/hostage.mdl");
		setsize(h, [-16, -16, -24], [16, 16, 32]);
		setorigin(h, mark.origin);
		h.angles = mark.angles;
		h.solid = SOLID_SLIDEBOX;
		h.movetype = MOVETYPE_STEP;
		h.cs_follow = world;
		h.cs_rescued = 0;
		h.colormod = [1.0, 0.9, 0.6];
		cs_hostagesTotal++;
	}
	serverinfo.SetInteger("csnz_hostages", (int)cs_hostagesTotal);
	serverinfo.SetInteger("csnz_rescued", 0);
}

/* the follower moves toward its CT with the engine's step movement */
void
CS_HostageThink(entity h)
{
	entity oldself = self;
	entity leader = h.cs_follow;

	if (h.cs_rescued)
		return;

	if (leader && (!CS_Playing(leader) || leader.team != CS_TEAM2))
		h.cs_follow = leader = world;

	if (leader) {
		vector d = leader.origin - h.origin;
		float dist = vlen([d[0], d[1], 0]);
		if (dist > 80) {
			self = h;
			h.angles[1] = vectoyaw(d);
			walkmove(h.angles[1], min(dist - 64, 250 * frametime));
			self = oldself;
		}
	}

	if (CS_InRescueZone(h)) {
		h.cs_rescued = 1;
		h.solid = SOLID_NOT;
		h.model = "";
		cs_hostagesRescued++;
		serverinfo.SetInteger("csnz_rescued", (int)cs_hostagesRescued);
		if (leader) {
			CS_GiveMoney(leader, 1000);	/* STOCK-CS per rescue */
			ACH_OnHostageRescue(leader);
			PROG_AwardXP(leader, XP_HOSTAGE_RESCUE);
		}
		printf("csnz %.1f: hostage rescued (%g of %g)\n", time, cs_hostagesRescued, cs_hostagesTotal);
	}
}

/* --- VIP -------------------------------------------------------------- */

void
CS_PickVip(void)
{
	int n = CS_Alive(CS_TEAM2);
	int pick;
	entity reserved = world;

	for (entity q = world; (q = next.Player(q));)
		q.cs_isVip = false;
	if (n <= 0)
		return;

	/* OBSERVED: a CT can reserve the VIP with 'M' (csnz_vip command) */
	for (entity r = world; (r = next.Player(r));)
		if (CS_Playing(r) && r.team == CS_TEAM2 && userinfo.GetBool(r, "csnz_wantvip"))
			reserved = r;

	pick = (int)floor(random() * n);
	for (entity p = world; (p = next.Player(p));) {
		if (!CS_Playing(p) || p.team != CS_TEAM2)
			continue;
		if (reserved ? p == reserved : pick-- == 0) {
			p.cs_isVip = true;
			ents.ChangeToClass(p, "csm_vip");
			ents.Input(p, "SetTeam", ftos(CS_TEAM2), world);
			ents.Input(p, "SetArmor", "200", world);	/* OBSERVED */
			userinfo.SetInteger(p, "csnz_vip", 1);
			CS_Tell(p, "You are the VIP. Reach the escape zone.");
			printf("csnz %.1f: %s is the VIP\n", time, p.netname);
			return;
		}
	}
}

/* --- use: plant, defuse, hostages --------------------------------------- */

void
CS_Use(entity p)
{
	if (cs_state != CS_LIVE || !CS_Playing(p) || p.cs_action)
		return;

	if (cs_mission == MISSION_BOMB) {
		if (p.team == CS_TEAM1 && p.cs_hasC4 && !cs_bombPlanted && CS_InBombSite(p)) {
			p.cs_action = 1;
			p.cs_actionEnd = time + autocvar_csnz_plant_time;
			p.cs_actionOrigin = p.origin;
			userinfo.SetFloat(p, "csnz_action", p.cs_actionEnd);
			return;
		}
		if (p.team == CS_TEAM2 && cs_bombPlanted && vlen(p.origin - cs_bomb.origin) < 72) {
			p.cs_action = 2;
			p.cs_actionEnd = time + (p.cs_hasKit ? autocvar_csnz_defuse_time / 2 : autocvar_csnz_defuse_time);
			p.cs_actionOrigin = p.origin;
			userinfo.SetFloat(p, "csnz_action", p.cs_actionEnd);
			printf("csnz %.1f: %s is defusing (%s kit)\n", time, p.netname, p.cs_hasKit ? "with" : "no");
			return;
		}
	}

	if (cs_mission == MISSION_HOSTAGE) {
		for (entity h = world; (h = find(h, ::classname, "csnz_hostage"));) {
			if (h.cs_rescued || vlen(h.origin - p.origin) > 96)
				continue;
			if (p.team == CS_TEAM2) {
				/* OBSERVED: E starts following; E again stops it */
				h.cs_follow = (h.cs_follow == p) ? world : p;
				printf("csnz %.1f: hostage %s %s\n", time, h.cs_follow ? "follows" : "stops for", p.netname);
			} else {
				/* OBSERVED: a T can send a moved hostage back (it stops) */
				h.cs_follow = world;
			}
			return;
		}
	}
}

void
CS_ActionFrame(entity p)
{
	if (!p.cs_action)
		return;

	/* bot AI keeps walking; a bot that started an action holds still */
	if (clienttype(p) == CLIENTTYPE_BOT && CS_Playing(p)) {
		setorigin(p, [p.cs_actionOrigin[0], p.cs_actionOrigin[1], p.origin[2]]);
		p.velocity = [0, 0, p.velocity[2]];
	}

	vector moved = p.origin - p.cs_actionOrigin;
	moved[2] = 0;	/* settling onto the floor is not moving away */
	if (!CS_Playing(p) || vlen(moved) > 24 || cs_state != CS_LIVE) {
		p.cs_action = 0;
		userinfo.SetFloat(p, "csnz_action", 0);
		return;
	}
	if (time < p.cs_actionEnd)
		return;

	float what = p.cs_action;
	p.cs_action = 0;
	userinfo.SetFloat(p, "csnz_action", 0);
	if (what == 1 && p.cs_hasC4 && !cs_bombPlanted)
		CS_PlantBomb(p);
	else if (what == 2 && cs_bombPlanted)
		CS_DefuseBomb(p);
}

/* --- buying ------------------------------------------------------------- */

/* name, team (0 any), price, item. Names, teams and prices STOCK-CS; the
   guns are decls/def/cs_weapons.def (STOCK-CS stats, not CSNZ parity).
   The HUD's buy menu (hud/buymenu.qc) lists the same items: keep the two
   in step. Flash and smoke grenades and night vision have no entityDefs
   yet, so they are not sold. */
string cs_buyList =
	"glock18 0 400 weapon_glock18 "
	"usp 0 500 weapon_usp "
	"p228 0 600 weapon_p228 "
	"deagle 0 650 weapon_deagle "
	"fiveseven 2 750 weapon_fiveseven "
	"elite 1 800 weapon_elite "
	"m3 0 1700 weapon_m3 "
	"xm1014 0 3000 weapon_xm1014 "
	"mac10 1 1400 weapon_mac10 "
	"tmp 2 1250 weapon_tmp "
	"mp5navy 0 1500 weapon_mp5 "
	"ump45 0 1700 weapon_ump45 "
	"p90 0 2350 weapon_p90 "
	"galil 1 2000 weapon_galil "
	"famas 2 2250 weapon_famas "
	"ak47 1 2500 weapon_ak47 "
	"m4a1 2 3100 weapon_m4a1 "
	"sg552 1 3500 weapon_sg552 "
	"aug 2 3500 weapon_aug "
	"scout 0 2750 weapon_scout "
	"awp 0 4750 weapon_awp "
	"g3sg1 1 5000 weapon_g3sg1 "
	"sg550 2 4200 weapon_sg550 "
	"m249 0 5750 weapon_m249 "
	"hegrenade 0 300 weapon_hegrenade "
	"kevlar 0 650 armor100 "
	"kevlar_helmet 0 1000 armor100 "
	"defuser 2 200 defuser "
	"primammo 0 60 ammo "
	"secammo 0 50 ammo";

bool
CS_Buy(entity p, string what)
{
	int n = tokenize(cs_buyList);

	if (cs_noBuy) {
		sprint(p, PRINT_HIGH, "Buying is not available in this mode.\n");
		return false;
	}
	if (!CS_Playing(p)) {
		sprint(p, PRINT_HIGH, "You must be alive to buy.\n");
		return false;
	}
	if (time > cs_buyEnd && cs_buyEnd > 0) {
		sprint(p, PRINT_HIGH, sprintf("%g seconds have passed. You can't buy anything now.\n", autocvar_mp_buytime * 60));
		return false;
	}
	if (!CS_InBuyZone(p)) {
		sprint(p, PRINT_HIGH, "You are not in a buy zone.\n");
		return false;
	}

	for (int i = 0; i + 3 < n; i += 4) {
		if (argv(i) != what)
			continue;
		float buyTeam = stof(argv(i + 1));
		float price = stof(argv(i + 2));
		string item = argv(i + 3);

		if (buyTeam && buyTeam != p.team) {
			sprint(p, PRINT_HIGH, "Your team cannot buy that.\n");
			return false;
		}
		if (p.cs_money < price) {
			sprint(p, PRINT_HIGH, "You have insufficient funds!\n");
			return false;
		}
		if (item == "defuser") {
			if (p.cs_hasKit)
				return false;
			p.cs_hasKit = true;
			CS_PublishCarrier(p);
		} else if (item == "armor100") {
			ents.Input(p, "SetArmor", "100", world);
		} else if (item == "ammo") {
			CS_FillAmmo(p);
		} else {
			ents.Input(p, "GiveItem", item, world);
			ents.Input(p, "SwitchToWeapon", item, world);
		}
		CS_GiveMoney(p, -price);
		printf("csnz %.1f: %s bought %s for $%g\n", time, p.netname, what, price);
		return true;
	}

	sprint(p, PRINT_HIGH, sprintf("Unknown item %s\n", what));
	return false;
}

/* --- per-round and per-frame mission logic --------------------------------- */

void
CS_MissionRoundStart(void)
{
	CS_RemoveBomb();
	cs_buyEnd = time + autocvar_mp_freezetime + autocvar_mp_buytime * 60;
	/* for the HUD's buy menu (buytime_num) */
	serverinfo.SetFloat("csnz_buyend", cs_buyEnd);

	for (entity p = world; (p = next.Player(p));) {
		p.cs_hasC4 = false;
		p.cs_action = 0;
		if (cs_giveAllKits && p.team == CS_TEAM2)
			p.cs_hasKit = true;
		if (p.cs_isVip && cs_mission != MISSION_VIP)
			p.cs_isVip = false;
		CS_PublishCarrier(p);
	}

	switch (cs_mission) {
	case MISSION_BOMB:
		CS_GiveBombToRandomT();
		break;
	case MISSION_HOSTAGE:
		CS_SpawnHostages();
		break;
	case MISSION_VIP:
		CS_PickVip();
		break;
	}
}

/* verdict for M_CheckRound: elimination plus the mission */
float
CS_MissionCheck(void)
{
	int t = CS_Alive(CS_TEAM1), ct = CS_Alive(CS_TEAM2);

	for (entity p = world; (p = next.Player(p));)
		CS_ActionFrame(p);
	if (cs_state != CS_LIVE)
		return 0;

	/* the C4 left on the ground goes to the next T who walks over it */
	if (cs_bomb && !cs_bombPlanted) {
		for (entity q = world; (q = next.Player(q));)
			if (CS_Playing(q) && q.team == CS_TEAM1 && vlen(q.origin - cs_bomb.origin) < 48) {
				CS_RemoveBomb();
				q.cs_hasC4 = true;
				CS_PublishCarrier(q);
				printf("csnz %.1f: %s picked up the C4\n", time, q.netname);
				break;
			}
	}

	if (cs_bombPlanted) {
		if (time >= cs_bombBlowAt) {
			CS_ExplodeBomb();
			return 0;
		}
		/* CTs dead with the bomb ticking: T still need the blast; T dead:
		   CT must still defuse (STOCK-CS) */
		if (ct == 0)
			return CS_TEAM1;
		return 0;
	}

	if (cs_mission == MISSION_HOSTAGE) {
		int left = 0;
		for (entity h = world; (h = find(h, ::classname, "csnz_hostage"));) {
			CS_HostageThink(h);
			if (!h.cs_rescued)
				left++;
		}
		if (cs_hostagesTotal > 0 && left == 0) {
			CS_Banner(CS_Text("CSO_All_Hostages_Rescued", "All hostages have been rescued!"));
			return CS_TEAM2;
		}
	}

	if (cs_mission == MISSION_VIP) {
		for (entity v = world; (v = next.Player(v));) {
			if (!v.cs_isVip)
				continue;
			if (!CS_Playing(v)) {
				CS_Banner(CS_Text("CSO_VIP_Assassinated", "The VIP has been assassinated!"));
				return CS_TEAM1;
			}
			if (CS_InZone(v, "func_vip_safetyzone", 128)) {
				CS_Banner(CS_Text("CSO_VIP_Escaped", "The VIP has escaped!"));
				CS_GiveMoney(v, 2500);
				return CS_TEAM2;
			}
		}
	}

	if (cs_mission == MISSION_ESCAPE) {
		int escaped = 0;
		for (entity e = world; (e = next.Player(e));)
			if (CS_Playing(e) && e.team == CS_TEAM1 && CS_InZone(e, "func_escapezone", 128))
				escaped++;
		/* STOCK-CS: half of the terrorists escaping wins */
		if (escaped > 0 && escaped * 2 >= CS_TeamSize(CS_TEAM1)) {
			CS_Banner(CS_Text("CSO_Terrorists_Escaped", "The terrorists have escaped!"));
			return CS_TEAM1;
		}
	}

	/* elimination; with the bomb not planted a dead T team loses */
	if (t == 0 && ct == 0)
		return 3;
	if (t == 0)
		return CS_TEAM2;
	if (ct == 0)
		return CS_TEAM1;
	return 0;
}

/* STOCK-CS round money: win 3250 (3500 by objective), loss 1400 plus 500
   for each further loss in a row up to 3400; 300 per kill. */
void
CS_RoundMoney(float winner)
{
	if (winner == CS_TEAM1 || winner == CS_TEAM2) {
		float loser = winner == CS_TEAM1 ? CS_TEAM2 : CS_TEAM1;
		float streak;
		CS_TeamMoney(winner, cs_mission != MISSION_NONE ? 3500 : 3250);
		if (loser == CS_TEAM1) {
			streak = cs_lossStreak1++;
			cs_lossStreak2 = 0;
		} else {
			streak = cs_lossStreak2++;
			cs_lossStreak1 = 0;
		}
		CS_TeamMoney(loser, min(3400, 1400 + 500 * streak));
	} else {
		CS_TeamMoney(CS_TEAM1, 1400);
		CS_TeamMoney(CS_TEAM2, 1400);
	}
}
