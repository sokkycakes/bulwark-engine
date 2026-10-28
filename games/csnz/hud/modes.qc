/* Which CSO mode the server runs, for the HUD.

   csnz.dat and studio.dat publish csnz_moderun, the id whose rules actually
   run (csnz_mode is the requested id, which may have fallen back to
   Deathmatch). zombi.dat publishes only csnz_mode "8". */

float
HUD_ModeId(void)
{
	string s = serverkey("csnz_moderun");

	if (s == "")
		s = serverkey("csnz_mode");
	return stof(s);
}

/* The ids whose mp.dll rules class derives from CSOGameRulesZombi
   (SOURCE-CONFIRMED: constructor chains under InstallGameRules,
   FUN_11412000; Zombi -> Zombi2 -> Zombi3 -> the rest). 30 ZombieBoss
   derives from CSOGameRulesDM, 37 Mugoonhwa and 15/48 ZombieScenarioVer2
   from CSOGameRules, so they are not in the list. */
bool
IsZombieMode(float id)
{
	switch (id) {
	case 8: case 9: case 14: case 20: case 24: case 29:
	case 32: case 35: case 36: case 41: case 43: case 44:
	case 45: case 46: case 47: case 51: case 54: case 55:
	case 56: case 58:
		return true;
	}
	return false;
}

/* Zombie/Human team names on the board. Besides the zombie ids, any rules
   whose team 1 the server calls "Zombie"/"Zombies" (csnz_t1): the QC still
   runs 30 and 37 as infection modes. */
bool
HUD_ZombieTeams(void)
{
	return IsZombieMode(HUD_ModeId()) || strstrofs(serverkey("csnz_t1"), "Zombie") == 0;
}

/* The scoreboard set client.dll shows for a mode (SOURCE-CONFIRMED: the
   CHudScoreBoard constructor FUN_02a71ab0 fills a table of {mode, sub-mode,
   mask}; FUN_02a781d0 looks the running mode up, sub-mode first, then
   "any" (-1); each board draws only when its bit is set). Sub-modes are
   left out here (15 sub 3/6, 17 sub 5, 40 sub 0/1): the server publishes
   none. Ids not in the table (16, 60) get 0. Not used for drawing yet:
   only the generic board (board.qc) is ported (handoff H6). */
/* bits 1-0x10 and 0x2000000 are CHudMultiplayScoreBoard; its background
   (FUN_02a77aa0) is SBOriginalBG for 1|8, SBTeamDeathBG for 2|4 and
   SBUnitehBG for 0x10. What else bit 8 changes is not traced. */
#define SB_ORIGINAL	0x1
#define SB_TEAMDEATH	0x2
#define SB_DEATH	0x4
#define SB_ZOMBIE	0x8
#define SB_UNITE	0x10
#define SB_CHALLENGE	0x20
#define SB_ZBS		0x40
#define SB_TEAMZBS	0x80
#define SB_TOP3		0x100
#define SB_ZBSKILLMSG	0x200
#define SB_PVPVE	0x400
#define SB_PVPVEKILLMSG	0x800
#define SB_BAZOOKA	0x1000
#define SB_GUNDEATH	0x2000
#define SB_LIGHTORI	0x4000
#define SB_SHELTER	0x8000
#define SB_BATTLERUSH	0x10000
#define SB_ZBSMINIMAP	0x20000
#define SB_SNOWMAN	0x40000
#define SB_SHELTERTEAM	0x80000
#define SB_SCENARIO	0x100000	/* ZombieScenarioVer2Hud */
#define SB_BOSS		0x200000	/* owner not identified */
#define SB_STANDALONE	0x400000
#define SB_VOXELCREATE	0x800000
#define SB_VOXELPVE	0x1000000
#define SB_TOUCHDOWN	0x2000000	/* CHudMultiplayScoreBoard */
#define SB_ZHE		0x4000000	/* owner not identified */
#define SB_CONQUEST	0x8000000
#define SB_ZSRIFT	0x10000000

float
Board_Mask(float id)
{
	switch (id) {
	case 0: case 3: case 6: case 7: case 13: case 31: case 42: case 50:
		return SB_ORIGINAL;
	case 2: case 5: case 19: case 40: case 57:
		return SB_TEAMDEATH;
	case 1: case 4: case 10: case 34:
		return SB_DEATH;
	case 8: case 9: case 14: case 20: case 24: case 29: case 32: case 35:
	case 36: case 37: case 41: case 43: case 44: case 45: case 49: case 53:
	case 54: case 55: case 58:
		return SB_ZOMBIE;
	case 11: return SB_UNITE;
	case 12: return SB_CHALLENGE;
	case 15: case 48: return SB_SCENARIO;
	case 17: return SB_ZBS | SB_ZBSKILLMSG;
	case 18: return SB_PVPVE | SB_PVPVEKILLMSG;
	case 21: return SB_BAZOOKA;
	case 22: return SB_GUNDEATH;
	case 23: return SB_LIGHTORI;
	case 25: return SB_BATTLERUSH;
	case 26: return SB_SHELTER | SB_ZBSKILLMSG;
	case 27: return SB_SNOWMAN | SB_TEAMDEATH;
	case 28: return SB_SHELTERTEAM | SB_ZBSKILLMSG;
	case 30: return SB_BOSS;
	case 33: return SB_STANDALONE;
	case 38: return SB_VOXELCREATE;
	case 39: return SB_VOXELPVE;
	case 46: case 47: return SB_TOUCHDOWN;
	case 51: return SB_ZHE;
	case 52: return SB_SHELTERTEAM;
	case 56: return SB_CONQUEST;
	case 59: return SB_ZSRIFT;
	}
	return 0;
}
