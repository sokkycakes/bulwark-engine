/* zombi5: CSO Zombi 5 mode (54) - virus-based zombie mode.
 *
 * Evidence from decompiled mp.dll (CSOGameRulesZombi5, CZombi5PlayerModule):
 *   BINARY    EGM_ZOMBI_5 = 54 (0x36)
 *   BINARY    classes: CSOGameRulesZombi5, CSOGameRulesZombi5RandomWeapon
 *   BINARY    CVirusObject@Zombi5 - virus spread entity
 *   BINARY    Human abilities (from sprite icons): Normal, Speed, Heavy, Heal,
 *             Undertaker, China, Revival, Resident, Pass, Booster, Flying,
 *             Aksha, Psycho, Deimos, Ganymede, Boomer, Witch
 *   BINARY    Zombie abilities: DogShoot, Rush, WebShooter, WebBomb, Protect,
 *             ChargeSlash
 *   BINARY    Data files: ZombiVirusBonus_Dedi.csv, VirusFactorReq_Dedi.csv,
 *             HumanAbilityData_Dedi.csv, HumanAbilityProbData_Dedi.csv,
 *             SpecialZombieProb_Dedi.csv
 *   BINARY    speed formula: base 0.4 + 0.0996 * level, cap 250
 *
 * This mode extends the zombie infection core with a virus-point system:
 * zombies earn virus points by damaging humans, which they spend on abilities.
 * Humans pick an ability class at round start. */

#define Z5_HUMAN CS_TEAM2
#define Z5_ZOMBIE CS_TEAM1

/* --- cvars (DESIGN unless cited) --- */
var float autocvar_z5_origin_ratio = 10;
var float autocvar_z5_origin_health = 3000;
var float autocvar_z5_host_health = 2000;
var float autocvar_z5_knockback = 250;
var float autocvar_z5_stun_time = 0.2;
var float autocvar_z5_select_time = 20;
var float autocvar_z5_virus_per_hit = 10;	/* DESIGN: virus points earned per hit */
var float autocvar_z5_ability_cost = 50;	/* DESIGN: virus points to use an ability */

/* human ability types (from binary sprite evidence) */
enum {
	Z5H_NORMAL,
	Z5H_SPEED,
	Z5H_HEAVY,
	Z5H_HEAL,
	Z5H_UNDERTAKER,
	Z5H_REVIVAL,
	Z5H_BOOSTER,
	Z5H_FLYING,
	Z5H_PSYCHO,
	Z5H_DEIMOS,
	Z5H_GANYMEDE,
	Z5H_BOOMER,
	Z5H_WITCH
};

/* zombie ability types (from binary evidence) */
enum {
	Z5Z_DOGSHOOT,
	Z5Z_RUSH,
	Z5Z_WEBSHOOTER,
	Z5Z_WEBBOMB,
	Z5Z_PROTECT,
	Z5Z_CHARGESLASH
};

/* per-player fields */
.float z5_ability;		/* chosen human ability */
.float z5_virus;		/* zombie virus points */
.float z5_stunEnd;
.float z5_infectAt;
.vector z5_infectOrigin;
.vector z5_infectAngles;

/* match state */
int z5_state;
float z5_selectEnd;

enum {
	Z5_WAIT,
	Z5_SELECT,
	Z5_LIVE,
	Z5_END
};

bool
Z5_IsZombie(entity p)
{
	return p.team == Z5_ZOMBIE;
}

void
Z5_MakeZombie(entity p, bool isOrigin, vector where, vector ang)
{
	p.team = Z5_ZOMBIE;
	ZA_BecomeZombie(p, "csm_zombie");
	CS_MoveToSpot(p, where, ang);
	float hp = isOrigin ? autocvar_z5_origin_health : autocvar_z5_host_health;
	ents.Input(p, "SetHealth", ftos(hp), world);
	ents.Input(p, "SetMaxHealth", ftos(hp), world);
	p.z5_virus = 0;
	userinfo.SetInteger(p, "csnz_team", Z5_ZOMBIE);
	userinfo.SetInteger(p, "csnz_virus", 0);
}

void
Z5_EquipHuman(entity p)
{
	ents.ChangeToClass(p, "csm_rifleman");
	CS_FillAmmo(p);

	/* apply ability bonuses */
	switch ((int)p.z5_ability) {
	case Z5H_SPEED:
		ents.Input(p, "SetMaxHealth", "80", world);
		ents.Input(p, "SetHealth", "80", world);
		/* speed bonus handled in movement hooks */
		break;
	case Z5H_HEAVY:
		ents.Input(p, "SetMaxHealth", "150", world);
		ents.Input(p, "SetHealth", "150", world);
		ents.Input(p, "SetArmor", "100", world);
		break;
	case Z5H_HEAL:
		/* heal ability: passive HP regen */
		break;
	default:
		break;
	}
	userinfo.SetInteger(p, "csnz_team", Z5_HUMAN);
}

void
Z5_Equip(entity p)
{
	if (Z5_IsZombie(p))
		Z5_MakeZombie(p, false, p.origin, p.v_angle);
	else
		Z5_EquipHuman(p);
}

float
Z5_PickTeam(entity p)
{
	return Z5_HUMAN;	/* everyone starts human, zombies selected later */
}

void
Z5_SelectZombies(void)
{
	int total = CS_Players();
	int origins = max(1, (int)floor(total / autocvar_z5_origin_ratio));
	int chosen = 0;

	/* random selection */
	for (int attempt = 0; attempt < 100 && chosen < origins; attempt++) {
		entity p;
		int pick = (int)floor(random() * total);
		int idx = 0;
		for (p = world; (p = next.Player(p));) {
			if (!CS_Playing(p)) continue;
			if (idx == pick && !Z5_IsZombie(p)) {
				Z5_MakeZombie(p, true, p.origin, p.v_angle);
				chosen++;
				CS_Banner(sprintf("%s is an Origin Zombie!", p.netname));
				break;
			}
			idx++;
		}
	}
}

void
Z5_RoundStart(void)
{
	z5_state = Z5_SELECT;
	z5_selectEnd = time + autocvar_z5_select_time;

	for (entity p = world; (p = next.Player(p));) {
		if (CS_Playing(p)) {
			p.team = Z5_HUMAN;
			Z5_EquipHuman(p);
			p.z5_ability = Z5H_NORMAL;
		}
	}
	CS_Banner("Zombie selection in progress...");
}

void
Z5_Live(void)
{
	/* called when freeze ends - select zombies */
	Z5_SelectZombies();
	z5_state = Z5_LIVE;
}

void
Z5_Killed(entity victim, entity inflictor, entity attacker, string weapon)
{
	if (!Z5_IsZombie(victim) && Z5_IsZombie(attacker)) {
		/* human killed by zombie: infect */
		victim.z5_infectAt = time + 3;
		victim.z5_infectOrigin = victim.origin;
		victim.z5_infectAngles = victim.v_angle;
	}
}

void
Z5_Damaged(entity victim, entity attacker)
{
	if (!Z5_IsZombie(victim) && Z5_IsZombie(attacker)) {
		/* zombie hit human: earn virus points */
		attacker.z5_virus += autocvar_z5_virus_per_hit;
		userinfo.SetInteger(attacker, "csnz_virus", (int)attacker.z5_virus);
	}
	/* knockback and stun */
	if (Z5_IsZombie(victim) && !Z5_IsZombie(attacker)) {
		vector dir = normalize(victim.origin - attacker.origin);
		dir[2] = 0.1;
		ents.Input(victim, "AddVelocity",
			sprintf("%g %g %g", dir[0] * autocvar_z5_knockback,
				dir[1] * autocvar_z5_knockback, dir[2] * autocvar_z5_knockback), world);
		victim.z5_stunEnd = time + autocvar_z5_stun_time;
	}
}

float
Z5_CheckRound(void)
{
	if (z5_state != Z5_LIVE)
		return 0;

	int humans = 0, zombies = 0;
	for (entity p = world; (p = next.Player(p));) {
		if (!CS_Playing(p)) continue;
		if (Z5_IsZombie(p)) zombies++;
		else humans++;
	}
	if (humans == 0 && zombies > 0) return Z5_ZOMBIE;
	if (zombies == 0 && humans > 0) return Z5_HUMAN;
	return 0;
}

void
Z5_Frame(void)
{
	/* selection countdown */
	if (z5_state == Z5_SELECT && time >= z5_selectEnd) {
		Z5_Live();
	}

	/* infection timer: dead humans rise as zombies */
	for (entity p = world; (p = next.Player(p));) {
		if (p.z5_infectAt && time >= p.z5_infectAt && p.health <= 0) {
			Z5_MakeZombie(p, false, p.z5_infectOrigin, p.z5_infectAngles);
			p.z5_infectAt = 0;
		}
	}

	/* passive heal for HEAL ability humans */
	for (entity p = world; (p = next.Player(p));) {
		if (!Z5_IsZombie(p) && CS_Playing(p) && p.z5_ability == Z5H_HEAL) {
			if (p.health < 100)
				ents.Input(p, "SetHealth", ftos(min(100, p.health + 1)), world);
		}
	}

	/* publish state */
	serverinfo.SetInteger("csnz_z5_state", z5_state);
}

bool
Z5_Command(entity p, string cmd)
{
	tokenize(cmd);
	string c = argv(0);

	/* choose human ability before zombie selection */
	if (c == "csnz_ability" && z5_state == Z5_SELECT && !Z5_IsZombie(p)) {
		float ability = stof(argv(1));
		if (ability >= Z5H_NORMAL && ability <= Z5H_WITCH) {
			p.z5_ability = ability;
			sprint(p, PRINT_HIGH, sprintf("Ability set to %g\n", ability));
		}
		return true;
	}

	/* zombie ability use */
	if (c == "csnz_use" && Z5_IsZombie(p) && CS_Playing(p)) {
		if (p.z5_virus >= autocvar_z5_ability_cost) {
			p.z5_virus -= autocvar_z5_ability_cost;
			userinfo.SetInteger(p, "csnz_virus", (int)p.z5_virus);
			/* DESIGN: basic speed boost for 3 seconds */
			ents.Input(p, "AddVelocity",
				sprintf("%g %g 0", v_forward[0] * 500, v_forward[1] * 500), world);
			sprint(p, PRINT_HIGH, "Ability activated!\n");
		}
		return true;
	}
	return false;
}

void
CSM_Zombi5(bool randomWeapon)
{
	cs_modeTitle = "CSO_GameMode_Zombi5";
	cs_modeFallback = "Zombi 5";
	cs_teams = true;
	cs_rounds = true;
	cs_oneSideStart = true;
	cs_respawnDelay = -1;
	cs_roundTimeFallback = 3;
	cs_winLimitFallback = 13;
	cs_freezeLock = false;
	cs_team1Name = "Zombies";
	cs_team2Name = "Humans";
	cs_roundTimeWinsTeam2 = true;

	M_Equip = Z5_Equip;
	M_PickTeam = Z5_PickTeam;
	M_RoundStart = Z5_RoundStart;
	M_Live = Z5_Live;
	M_Frame = Z5_Frame;
	M_Killed = Z5_Killed;
	M_Damaged = Z5_Damaged;
	M_CheckRound = Z5_CheckRound;
	M_Command = Z5_Command;
}
