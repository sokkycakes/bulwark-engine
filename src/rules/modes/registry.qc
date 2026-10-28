/* registry: CSO mode id -> rules, shared by csnz.dat and studio.dat (a Studio
 * map can be played with the rules of a regular mode). Original code (ISC).
 * MODE_INVENTORY.md lists every id's disposition. */

float cs_modeRun;		/* the id whose rules actually run */

void CSM_ShelterTeam(void);	/* modes/sheltert.qc, after this file (it uses getentitytoken) */

/* partner mode: two AI partners per human on the human's side (DESIGN count) */
float
CSM_PartnerPickTeam(entity p)
{
	if (clienttype(p) != CLIENTTYPE_BOT)
		return CS_TEAM2;
	/* partners join the side with fewer bots per human */
	return CS_TeamSize(CS_TEAM2) <= CS_TeamSize(CS_TEAM1) ? CS_TEAM2 : CS_TEAM1;
}

/* --- Partner spawn-point picker (handoff M3) -------------------------------
   SOURCE-CONFIRMED (mp.dll): the client command "ptn_spawn N" (FUN_10743990)
   runs FUN_107458e0 for the player and then for the player's partner bot.
   For a player on team 1 or 2 it frees the spot the player claimed before,
   looks up spawn group N of the player's team, claims the first unclaimed
   spot in it and moves the player there: origin + 1 up, the spot's angles,
   no velocity, view angles reset (fixangle). It has no freeze-time or
   round-state check. A human's pick is also broadcast (bytes 7, player, N);
   what the client does with that is UNRESOLVED and it isn't sent here.
   OBSERVED: ptn_villa.bsp gives its spawns an "id" key (T: info_player_
   deathmatch 0-3, CT: info_player_start 0-1, 16 spots each), matching the
   point count in maps/ptn_villa_partner.json, so group N is the spots whose
   id is N.
   SOURCE-CONFIRMED (client.dll FUN_02971d80): the picker counts down from 15
   seconds; the text says the player starts where they picked when it runs
   out. INFERRED: the picker opens at round start and freeze time lasts its
   15 seconds; claims are cleared each round. DESIGN: csnz_ptn_end publishes
   the picker's end for the HUD; a human's partner is the first bot on the
   same team that no other human has. */

.float cs_ptnClaimed;
.entity cs_ptnSpot;
.entity cs_ptnPartner;

/* The spawns' "id" keys, read once from the map's entity lump: Nuclide keeps
   each entity's spawn keys in a class field (ncIO::m_rawSpawnData) that the
   rules progs can't reach on Bulwark (OBSERVED: findentityfield finds
   nothing from an added progs, and a same-named field reads empty), so the
   lump is walked with getentitytoken (which Bulwark's server side has,
   OBSERVED) and each spawn's id is stored on the spawn entity at the same
   origin. */
.float cs_ptnId;
#ifndef CSQC
string(optional string resetstring) getentitytoken = #355;	/* fteextensions declares it for CSQC only */
#endif
bool cs_ptnIdsRead;

void
CSM_PartnerReadIds(void)
{
	string tok, kname, cname = "", org = "", spotId = "";

	cs_ptnIdsRead = true;
	for (entity s = world; (s = nextent(s));)
		s.cs_ptnId = -1;

	getentitytoken(0);
	while ((tok = getentitytoken()) != "") {
		if (tok == "{") {
			cname = org = spotId = "";
			continue;
		}
		if (tok == "}") {
			if (spotId != "" && (cname == "info_player_deathmatch" || cname == "info_player_start")) {
				vector o = stov(org);
				for (entity s = world; (s = find(s, ::classname, cname));)
					if (vlen(s.origin - o) < 1)
						s.cs_ptnId = stof(spotId);
			}
			continue;
		}
		kname = tok;
		tok = getentitytoken();
		if (kname == "classname")
			cname = tok;
		else if (kname == "origin")
			org = tok;
		else if (kname == "id")
			spotId = tok;
	}
}

entity
CSM_PartnerOf(entity p)
{
	if (clienttype(p) == CLIENTTYPE_BOT)
		return world;
	if (p.cs_ptnPartner && p.cs_ptnPartner.team == p.team && clienttype(p.cs_ptnPartner) == CLIENTTYPE_BOT)
		return p.cs_ptnPartner;

	p.cs_ptnPartner = world;
	for (entity b = world; (b = next.Player(b));) {
		bool taken = false;

		if (clienttype(b) != CLIENTTYPE_BOT || b.team != p.team)
			continue;
		for (entity h = world; (h = next.Player(h));)
			if (h != p && h.cs_ptnPartner == b)
				taken = true;
		if (!taken) {
			p.cs_ptnPartner = b;
			break;
		}
	}
	return p.cs_ptnPartner;
}

/* FUN_107458e0 */
void
CSM_PartnerSpawnAt(entity p, float group)
{
	string cname;

	if (!p || (p.team != CS_TEAM1 && p.team != CS_TEAM2))
		return;

	if (p.cs_ptnSpot) {
		p.cs_ptnSpot.cs_ptnClaimed = 0;
		p.cs_ptnSpot = world;
	}

	if (!cs_ptnIdsRead)
		CSM_PartnerReadIds();

	cname = (p.team == CS_TEAM1) ? "info_player_deathmatch" : "info_player_start";
	for (entity s = world; (s = find(s, ::classname, cname));) {
		if (s.cs_ptnId != group || s.cs_ptnClaimed)
			continue;

		s.cs_ptnClaimed = 1;
		p.cs_ptnSpot = s;
		CS_MoveToSpot(p, s.origin + [0, 0, 1], s.angles);
		p.punchangle = [0, 0, 0];
		return;
	}
}

void
CSM_PartnerRoundStart(void)
{
	for (entity s = world; (s = findfloat(s, cs_ptnClaimed, 1));)
		s.cs_ptnClaimed = 0;
	for (entity p = world; (p = next.Player(p));)
		p.cs_ptnSpot = world;

	CSM_OriginalRoundStart();

	/* the picker's 15 seconds are the freeze time (INFERRED) */
	cs_stateEnd = time + 15;
	serverinfo.SetFloat("csnz_ptn_end", cs_stateEnd);
}

float
CSM_PartnerCommand(entity p, string cmd)
{
	tokenize(cmd);
	if (argv(0) == "ptn_spawn" && p) {
		float group = stof(argv(1));

		CSM_PartnerSpawnAt(p, group);
		CSM_PartnerSpawnAt(CSM_PartnerOf(p), group);
		return true;
	}
	return CSM_OriginalCommand(p, cmd);
}

/* every id in gamemodelist.csv and maploadingtip.csv; true if it has rules.
 * Ids and EGM names: mp.dll's EGM registration functions (e.g. FUN_100024d0
 * registers "EGM_SHELTER" as 0x1a); rules classes: InstallGameRules
 * (FUN_11412000). Both SOURCE-CONFIRMED; reports/qc_porting_reference.md.
 *   0  Original        1  (DM)             2  TDeath           3  Bot
 *   4  (BotDM)         5  Bot_TDeath       6  (Official)       7  (TieBreak)
 *   8  Zombi (own dat) 9  Zombi_2         10  Hidden          11  Zombi_Team
 *  12  Challenge       13  Zombi_Team_Ann  14  Zombi_3         15  Zombi_Survival
 *  16  Soccer          17  Human_Scenario  18  PvPvE           19  TDM_Item
 *  20  Zombi_Escape    21  Bazooka         22  GunDeath        23  Light_Ori
 *  24  Bot_Zombi       25  Battle_Rush     26  Shelter         27  Snowman
 *  28  Shelter_Team    29  Zombi_4         30  Zombie_Boss     31  IceWorld (FightYard)
 *  32  Zombie_Exterminate 33 Stand_Alone   34  Sudden_Death    35  Zombie_Ex_Official
 *  36  Zombie_Ex_Official_TieBreak         37  Zombie_Tag_Fun  38  Voxel_Create
 *  39  Voxel_PvE       40  Allstar         41  Playroom_ZBH    42  Season_Original
 *  43  Season_ZombieEx 44  Season_ZombieHero 45 Zombie_ZMode   46  Zombie_TouchDown
 *  47  Season_TouchDown 48 Playroom_ZBS    49  Voxel_PropHunt  50  Partner
 *  51  Zombie_Horror_Escape 52 Voxel_Shelter 53 Voxel_ScenarioTX 54 Zombi_5
 *  55  Playroom_ZombieZ 56 Zombi_Conquest  57  Super_Soldier   58  Zombi_3_Classic
 *  59  ZSRift          60  Playroom_ZHC (no rules: InstallGameRules has no case)
 * (1, 4, 6, 7: no EGM name; gamemodelist.csv names them.) */
bool
CS_SelectMode(float id)
{
	cs_noStats = false;
	cs_respawnDelay = 2;

	switch (id) {
	/* --- deathmatch family --- */
	case 1: CSM_Deathmatch(false); break;
	case 4: CSM_Deathmatch(true); break;
	case 2: CSM_TeamDeathmatch(false); break;
	case 5: CSM_TeamDeathmatch(true); break;
	case 25: CSM_BattleRush(); break;
	case 34: CSM_SuddenDeath(); break;
	case 40: CSM_Allstar(); break;
	case 19: CSM_ItemBattle(); break;

	/* --- zombie team --- */
	case 11: CSM_ZombieTeam(); break;
	case 13: CSM_ZombiTeamAnn(); break;

	/* --- original family --- */
	case 0:
	case 3:
	case 6:
	case 7:
	case 23:
	case 31:
	case 33:
	case 42:
		CSM_Original(id);
		break;
	case 50:
		CSM_Original(id);
		M_PickTeam = CSM_PartnerPickTeam;
		M_RoundStart = CSM_PartnerRoundStart;
		M_Command = CSM_PartnerCommand;
		break;

	/* --- bazooka --- */
	case 21: CSM_Bazooka(); break;

	/* --- zombie infection modes --- */
	case 9:
	case 14:
	case 20:
	case 24:
	case 29:
	case 30:
	case 32:
	case 35:
	case 36:
	case 37:
	case 43:
	case 44:
	case 45:
	case 46:
	case 47:
	case 58:
		CSM_Zombie(id);
		break;

	/* --- fun modes --- */
	case 22: CSM_GunDeath(); break;
	case 10: CSM_Hidden(); break;
	case 16: CSM_Soccer(); break;
	case 12: CSM_Challenge(); break;
	case 41:
	case 48:
	case 55:
		CSM_Playroom(id);
		break;
	case 27:
		if (autocvar_sm_core_mode || find(world, ::classname, "snowman_tr_core"))
			CSM_SnowmanCore();
		else
			CSM_Snowman();
		break;
	case 18: CSM_PvPvE(); break;

	/* --- shelter modes --- */
	case 26: CSM_Shelter(false); break;
	case 28: CSM_ShelterTeam(); break;
	case 52:
		/* VoxelShelter: VoxelPlay<SHELTERTeam> (SOURCE-CONFIRMED);
		   studio.dat runs it on a Studio map */
		CSM_Shelter(true);
		cs_modeTitle = "CSO_GameMode_vxlzshelter";
		cs_modeFallback = "Studio Zombie Shelter";
		break;

	/* --- zombi 5 --- */
	case 54: CSM_Zombi5(false); break;

	/* --- scenario / PvE modes --- */
	case 53: CSM_ScenarioTX(); break;
	case 17: CSM_HumanScenario(); break;
	case 15: CSM_ZombieScenario(); break;
	case 51: CSM_ZombieHorrorEscape(); break;
	case 56: CSM_ZombiConquest(); break;
	case 57: CSM_SuperSoldier(); break;
	case 59: CSM_ZSRift(); break;

	default:
		return false;
	}
	return true;
}

