/* shelter: CSO Shelter (26) and Shelter Team (28) modes.
 *
 * Evidence from decompiled mp.dll (CSOGameRulesSHELTER, CSOGameRulesSHELTERTeam):
 *   OBSERVED  humans build a shelter from voxel blocks and defend it against
 *             zombie waves; shelter has HP that the zombies drain; humans
 *             survive rounds by keeping the shelter alive; zombies respawn.
 *   OBSERVED  Shelter Team: two teams compete to destroy the other's shelter
 *             while defending their own; both sides build shelters.
 *   BINARY    sounds: zsht_upgradeshelter, zsht_healthrestore, shelterteam/build2.wav
 *   BINARY    sprites: shelter models (shelter_b_shelter01-05, wall, metaldoor,
 *             electric_defensefx, laser_towerfx, ammobox01)
 *   BINARY    string: zsht_healthrestore %d %d (health restore notification)
 *   BINARY    EGM_SHELTER=26, EGM_SHELTER_TEAM=28, EGM_VOXEL_SHELTER=52
 *
 * Values without OBSERVED citation are DESIGN approximations from binary
 * evidence patterns and common CSO conventions. */

#define SH_TEAM_HUMAN CS_TEAM2
#define SH_TEAM_ZOMBIE CS_TEAM1

/* --- shelter mode cvars (DESIGN numbers) --- */
var float autocvar_sh_shelter_hp = 5000;	/* DESIGN: shelter starting HP */
var float autocvar_sh_round_time = 3;		/* DESIGN: minutes per round */
var float autocvar_sh_build_time = 30;		/* DESIGN: seconds of build phase */
var float autocvar_sh_zombie_respawn = 5;	/* DESIGN: seconds before zombie respawns */
var float autocvar_sh_wave_count = 5;		/* DESIGN: zombie waves per round */
var float autocvar_sh_wave_interval = 30;	/* DESIGN: seconds between waves */
var float autocvar_sh_repair_amount = 100;	/* DESIGN: HP restored per repair action */
var float autocvar_sh_repair_cooldown = 5;	/* DESIGN: seconds between repairs */

/* per-player fields */
.float sh_lastRepair;

/* match state */
float sh_shelterHP;
float sh_shelterMaxHP;
float sh_buildEnd;
float sh_waveNum;
float sh_nextWave;
bool sh_isTeam;		/* true for Shelter Team variant */
float sh_shelterHP2;	/* team 2's shelter in Shelter Team */

void
SH_EquipHuman(entity p)
{
	ents.ChangeToClass(p, "csm_rifleman");
	CS_FillAmmo(p);
}

void
SH_EquipZombie(entity p)
{
	ZA_BecomeZombie(p, "csm_zombie");
	ents.Input(p, "SetHealth", "2000", world);		/* DESIGN */
	ents.Input(p, "SetMaxHealth", "2000", world);
}

void
SH_Equip(entity p)
{
	if (p.team == SH_TEAM_ZOMBIE)
		SH_EquipZombie(p);
	else
		SH_EquipHuman(p);
}

float
SH_PickTeam(entity p)
{
	if (sh_isTeam) {
		/* Shelter Team: balance between two human+shelter teams */
		return CS_TeamSize(CS_TEAM2) <= CS_TeamSize(CS_TEAM1) ? CS_TEAM2 : CS_TEAM1;
	}
	/* Shelter: humans vs AI zombies */
	if (clienttype(p) == CLIENTTYPE_BOT)
		return SH_TEAM_ZOMBIE;
	return SH_TEAM_HUMAN;
}

void
SH_RoundStart(void)
{
	sh_shelterHP = autocvar_sh_shelter_hp;
	sh_shelterMaxHP = autocvar_sh_shelter_hp;
	sh_buildEnd = time + autocvar_sh_build_time;
	sh_waveNum = 0;
	sh_nextWave = sh_buildEnd;
	if (sh_isTeam)
		sh_shelterHP2 = autocvar_sh_shelter_hp;

	serverinfo.SetInteger("csnz_shelter_hp", (int)sh_shelterHP);
	serverinfo.SetInteger("csnz_shelter_max", (int)sh_shelterMaxHP);
	CS_Banner("Build your shelter!");
}

void
SH_DamageShelter(float amount, float whichTeam)
{
	if (whichTeam == SH_TEAM_HUMAN || !sh_isTeam) {
		sh_shelterHP = max(0, sh_shelterHP - amount);
		serverinfo.SetInteger("csnz_shelter_hp", (int)sh_shelterHP);
	} else if (sh_isTeam) {
		sh_shelterHP2 = max(0, sh_shelterHP2 - amount);
		serverinfo.SetInteger("csnz_shelter_hp2", (int)sh_shelterHP2);
	}
}

void
SH_Killed(entity victim, entity inflictor, entity attacker, string weapon)
{
	if (victim.team == SH_TEAM_ZOMBIE) {
		/* zombies respawn after a delay, even though this is a round mode:
		   CS_Frame brings anyone with cs_respawnAt set back in */
		victim.cs_respawnAt = time + max(0.1, autocvar_sh_zombie_respawn);
	}
	/* shelter damage from zombie melee (DESIGN: 50 per zombie kill near shelter) */
	if (attacker.team == SH_TEAM_ZOMBIE) {
		SH_DamageShelter(50, SH_TEAM_HUMAN);
	}
}

void
SH_Damaged(entity victim, entity attacker)
{
	/* zombies attacking near shelter deal shelter damage */
	if (victim.team == SH_TEAM_HUMAN && attacker.team == SH_TEAM_ZOMBIE) {
		SH_DamageShelter(10, SH_TEAM_HUMAN);	/* DESIGN: gradual damage */
	}
}

float
SH_CheckRound(void)
{
	if (sh_shelterHP <= 0)
		return SH_TEAM_ZOMBIE;	/* zombies destroyed the shelter */
	if (sh_isTeam && sh_shelterHP2 <= 0)
		return SH_TEAM_HUMAN;
	if (sh_waveNum >= autocvar_sh_wave_count && !sh_isTeam) {
		/* survived all waves */
		return SH_TEAM_HUMAN;
	}
	return 0;
}

bool
SH_Command(entity p, string cmd)
{
	tokenize(cmd);
	if (argv(0) != "csnz_use")
		return false;

	/* repair shelter */
	if (p.team == SH_TEAM_HUMAN && time >= p.sh_lastRepair + autocvar_sh_repair_cooldown) {
		if (sh_shelterHP < sh_shelterMaxHP) {
			sh_shelterHP = min(sh_shelterMaxHP, sh_shelterHP + autocvar_sh_repair_amount);
			p.sh_lastRepair = time;
			serverinfo.SetInteger("csnz_shelter_hp", (int)sh_shelterHP);
			sprint(p, PRINT_HIGH, sprintf("Shelter repaired! HP: %d/%d\n",
				floor(sh_shelterHP), floor(sh_shelterMaxHP)));
		}
	}
	return true;
}

void
SH_Frame(void)
{
	if (time < sh_buildEnd) {
		/* build phase: publish countdown */
		serverinfo.SetString("csnz_phase", "BUILD");
		return;
	}

	serverinfo.SetString("csnz_phase", "DEFEND");

	/* spawn zombie waves */
	if (sh_waveNum < autocvar_sh_wave_count && time >= sh_nextWave) {
		sh_waveNum++;
		sh_nextWave = time + autocvar_sh_wave_interval;
		CS_Banner(sprintf("Wave %g / %g!", sh_waveNum, autocvar_sh_wave_count));

		/* respawn all dead zombies */
		for (entity p = world; (p = next.Player(p));) {
			if (p.team == SH_TEAM_ZOMBIE && p.health <= 0)
				CS_PutInGame(p);
		}
	}
}

void
CSM_Shelter(bool isTeam)
{
	cs_modeTitle = isTeam ? "CSO_GameMode_ShelterTeam" : "CSO_GameMode_Shelter";
	cs_modeFallback = isTeam ? "Shelter Team" : "Shelter";
	cs_teams = true;
	cs_rounds = true;
	cs_oneSideStart = false;
	cs_respawnDelay = -1;
	cs_roundTimeFallback = autocvar_sh_round_time;
	cs_winLimitFallback = 7;
	cs_freezeLock = false;
	cs_team1Name = "Zombies";
	cs_team2Name = "Humans";
	sh_isTeam = isTeam;

	M_Equip = SH_Equip;
	M_PickTeam = SH_PickTeam;
	M_RoundStart = SH_RoundStart;
	M_Frame = SH_Frame;
	M_Killed = SH_Killed;
	M_Damaged = SH_Damaged;
	M_CheckRound = SH_CheckRound;
	M_Command = SH_Command;
}
