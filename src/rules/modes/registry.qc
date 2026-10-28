/* registry: CSO mode id -> rules, shared by csnz.dat and studio.dat (a Studio
 * map can be played with the rules of a regular mode). Original code (ISC).
 * MODE_INVENTORY.md lists every id's disposition. */

float cs_modeRun;		/* the id whose rules actually run */

/* partner mode: two AI partners per human on the human's side (DESIGN count) */
float
CSM_PartnerPickTeam(entity p)
{
	if (clienttype(p) != CLIENTTYPE_BOT)
		return CS_TEAM2;
	/* partners join the side with fewer bots per human */
	return CS_TeamSize(CS_TEAM2) <= CS_TeamSize(CS_TEAM1) ? CS_TEAM2 : CS_TEAM1;
}

/* every id in gamemodelist.csv and maploadingtip.csv; true if it has rules.
 * Complete mode ID table from decompiled mp.dll EGM enum:
 *   0  Original        1  Deathmatch       2  Team DM          3  Bot Original
 *   4  Bot DM           5  Bot TDM          6  Official         7  Tie Break
 *   8  Zombi (own dat)  9  Zombi_Expand    10  Hidden          11  Zombi_Team
 *  12  Challenge       13  Zombi_Team_Ann  14  Zombi_3         15  Zombi_Survival
 *  16  Soccer          17  Human_Scenario  18  PvPvE           19  TDM_Item
 *  20  Zombi_Escape    21  Bazooka         22  GunDeath        23  Light_Ori
 *  24  Bot_Zombi       25  Battle_Rush     26  Shelter         27  Snowman
 *  28  Shelter_Team    29  Zombi_4         30  Zombie_Boss     31  FightYard
 *  32  Zombie_Exterminate 33 StandAlone    34  Sudden_Death    35  ZombieExOff
 *  36  ZombieExOffTie  37  ZombieTag       38  Voxel_Create    39  Voxel_PvE
 *  40  Allstar         41  Playroom_ZBH    42  Season_Original 43  Season_ZombieEx
 *  44  Season_ZombieHero 45 Zombie_ZMode   46  Zombie_TouchDown 47 Season_TouchDown
 *  48  Playroom_ZBS    49  Voxel_PropHunt  50  Partner         51  Zombie_Horror_Escape
 *  52  Voxel_Shelter   53  Voxel_ScenarioTX 54 Zombi_5         55  Playroom_ZombieZ
 *  56  Zombi_Conquest  57  Super_Soldier   58  Zombi_3_Classic 59  ZSRift
 *  60  Playroom_ZHC */
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
	case 60:
		CSM_Playroom();
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
	case 28: CSM_Shelter(true); break;

	/* --- zombi 5 --- */
	case 54: CSM_Zombi5(false); break;

	/* --- scenario / PvE modes --- */
	case 53: CSM_ScenarioTX(); break;
	case 17: CSM_HumanScenario(); break;
	case 15: CSM_ZombiSurvival(); break;
	case 51: CSM_ZombieHorrorEscape(); break;
	case 56: CSM_ZombiConquest(); break;
	case 57: CSM_SuperSoldier(); break;
	case 59: CSM_ZSRift(); break;

	default:
		return false;
	}
	return true;
}

