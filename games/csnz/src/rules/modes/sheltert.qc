/* sheltert: CSO Shelter Team (mode 28), CSOGameRulesSHELTERTeam.

   The trace is docs in /mnt/project-files/csnz/qc-parity/shelterteam-trace.md
   (not in git). Evidence labels as elsewhere: SOURCE-CONFIRMED (read in the
   decompiled mp.dll or in the decrypted .cso tables), OBSERVED (seen in the
   shipped data), INFERRED (follows from the above), DESIGN (ours).

   SOURCE-CONFIRMED, the clock (rules vtable 0x46c, FUN_10838490): a clock
   counter moves one step every 240/40 = 6 seconds (tunables 0x11e58310 and
   0x11e57b30). State 1 counts up from 103; past 121 it turns to state 0,
   which counts down; below 103 it turns to state 2 (night) for 120 seconds
   (tunable 0x11e58e08). A half-day counter goes up when night starts and
   again when day starts, so (half + 1) / 2 is the day. At dawn: minerals
   and ammo boxes reset, resource piles respawn (FUN_1083b1e0), every player
   gets a skill point (INFERRED from FUN_114e58d0(1)), the blackmarket
   (shelter_team_buyzone) moves (FUN_10841be0), and from day 16 (tunable
   0x11e57668) rescue can be requested (FUN_10839050). On day 21 (tunable
   0x11e573c0) "c4_object_break" entities break (FUN_1083b490).

   SOURCE-CONFIRMED, resources: wood (type 0) and iron (type 1), carried by
   each player up to a capacity (base 15, tunable 0x11e57620; skill 9 adds
   params 4-6 to wood and 7-9 to iron, skill 10 params 1-3 / 4-6,
   FUN_1083bc00) and stored per team up to the warehouse capacity (each
   warehouse adds 30 and 30, tunables 0x11e58700 / 0x11e57a10). Overflow is
   lost unless the player has skill 31, which sends it to the team
   (FUN_10837280). Power is a team capacity from generators (40/75/100/130
   by tier, FUN_10887ff0) minus what buildings use.
   Resource piles ("zsht_resource", FUN_1083b1e0) are spread each day over
   the rectangles of maps/zbs_rs_<map>.csv: restype 0 is wood or iron at
   random, 1 wood, 2 iron, count min..max. Using a pile takes one from it and
   gives 1 + skill 9's param (FUN_10895970, FUN_108459e0).
   shelter_team_mineral (CShelterMineral, FUN_10894350) needs skill 10: it
   gives 90% type 1, 10% type 0, in the amounts of skill 10's params, a few
   times a day.

   SOURCE-CONFIRMED, building (zsht_RequestBuild / zsht_BuildConfirm,
   FUN_108396b0 and FUN_10838010): checks in order: class, attribute (static
   buildings only on their spots), max count, skill, building, block areas,
   space, something in the way, resources. Costs come from
   zsht_building.csv; skill 21 takes params 1-3 off wood and iron (min 1),
   skill 22 multiplies power (FUN_1083b3a0). useGlobalRes pays from the
   team, otherwise from the builder (FUN_1083d6c0). Errors are the numbers
   FUN_10843db0 sends; the client's names are in SHT_ErrorKey.

   DESIGN: building health, turrets, traps and zombie damage to buildings
   are ours; the 30 building classes' thinks are not ported one by one.
   Zombies are Nuclide NPCs (decls/def/shelterteam.def) with the health of
   their zbs_zt row; the waves follow zbs_st_wave_<map>.csv with the
   half-day counter as the wave id (INFERRED). */

#define SHT_MAXB 40
#define SHT_MAXS 33
#define SHT_MAXW 9

/* --- tables ----------------------------------------------------------- */

float sht_bRows;
float sht_bTier[SHT_MAXB];
float sht_bCat[SHT_MAXB];
float sht_bDepSkill[SHT_MAXB];
float sht_bDepBuild[SHT_MAXB];
string sht_bClass[SHT_MAXB];
string sht_bModel[SHT_MAXB];
string sht_bKey[SHT_MAXB];
vector sht_bMins[SHT_MAXB];
vector sht_bMaxs[SHT_MAXB];
float sht_bAttr[SHT_MAXB];
float sht_bDur[SHT_MAXB];
float sht_bMax[SHT_MAXB];
float sht_bWood[SHT_MAXB];
float sht_bIron[SHT_MAXB];
float sht_bPower[SHT_MAXB];
float sht_bGlobal[SHT_MAXB];
float sht_bScore[SHT_MAXB];

float sht_sMax[SHT_MAXS];
float sht_sTier[SHT_MAXS];
float sht_sCat[SHT_MAXS];
float sht_sParam[SHT_MAXS * 11];

float sht_wSkill[SHT_MAXW];
float sht_wWood[SHT_MAXW];
float sht_wIron[SHT_MAXW];

/* map tables */
#define SHT_MAXSPOT 16
float sht_spots;
float sht_spotTeam[SHT_MAXSPOT];
float sht_spotAttr[SHT_MAXSPOT];
vector sht_spotOrg[SHT_MAXSPOT];
float sht_spotYaw[SHT_MAXSPOT];
string sht_spotOn[SHT_MAXSPOT];
string sht_spotLadder[SHT_MAXSPOT];
string sht_spotWall[SHT_MAXSPOT];
entity sht_spotUsed[SHT_MAXSPOT];

#define SHT_MAXRS 24
float sht_rsCount;
float sht_rsType[SHT_MAXRS];
vector sht_rsMin[SHT_MAXRS];
vector sht_rsMax[SHT_MAXRS];
float sht_rsLo[SHT_MAXRS];
float sht_rsHi[SHT_MAXRS];

#define SHT_MAXBB 32
float sht_bbCount;
vector sht_bbMin[SHT_MAXBB];
vector sht_bbMax[SHT_MAXBB];

#define SHT_MAXSP 128
float sht_zsCount;
float sht_zsId[SHT_MAXSP];
vector sht_zsOrg[SHT_MAXSP];
float sht_psCount;
vector sht_psOrg[SHT_MAXSP];
float sht_psYaw[SHT_MAXSP];

#define SHT_MAXWAVE 1600
float sht_waveRows;
float sht_waveId[SHT_MAXWAVE];
float sht_waveGap[SHT_MAXWAVE];
float sht_waveZombie[SHT_MAXWAVE];
float sht_waveSpot[SHT_MAXWAVE];
float sht_waveNext[SHT_MAXWAVE];

#define SHT_MAXZT 300
float sht_ztRows;
float sht_ztId[SHT_MAXZT];
string sht_ztLook[SHT_MAXZT];
float sht_ztDamage[SHT_MAXZT];
float sht_ztHealth[SHT_MAXZT];

/* --- state ------------------------------------------------------------ */

float sht_state;		/* 0 evening, 1 morning, 2 night (FUN_10838490) */
float sht_counter;
float sht_tick;
float sht_half;			/* the half-day counter */
float sht_nightEnd;
float sht_rescueFrom;		/* day rescue opens */
float sht_rescueEnd;		/* helicopter arrives */
float sht_rescueWindow;		/* requests close */
bool sht_c4Broken;
float sht_nextHit;
float sht_nextInfo;
float sht_zombies;

float sht_wood[3];
float sht_iron[3];
float sht_woodCap[3];
float sht_ironCap[3];
float sht_powerCap[3];
float sht_powerUsed[3];
float sht_tier[3];
float sht_built[3 * SHT_MAXB];
entity sht_barricade[3];

.float sht_carryWood;
.float sht_carryIron;
.float sht_capWood;
.float sht_capIron;
.float sht_skillA;		/* skill levels, base 4 digits: 0-10, 11-21, 22-32 */
.float sht_skillB;
.float sht_skillC;
.float sht_points;
.float sht_placing;		/* building id + 1 while placing */
.float sht_placeYaw;
.entity sht_ghost;
.float sht_placed;		/* moved to a P spot since the last spawn */
.float sht_rescueAsked;

/* buildings, piles, minerals, zombies */
.float sht_kind;		/* 1 building, 2 pile, 3 mineral, 4 zombie, 5 barricade, 6 ghost */
.float sht_bid;
.float sht_amount;		/* pile count; mineral uses today */
.float sht_resType;
.float sht_nextAct;
.float sht_power;		/* power the building uses, or gives (generators) */
.float sht_spot;		/* static spot + 1 */
.float sht_ztRow;
.float sht_ready;		/* construction done at */
.entity sht_builder;

var float autocvar_sht_tick = 6;		/* SOURCE-CONFIRMED 240 / 40 */
var float autocvar_sht_night = 120;		/* SOURCE-CONFIRMED tunable 0x11e58e08 */
var float autocvar_sht_rescue_day = 16;	/* SOURCE-CONFIRMED tunable 0x11e57668 */
var float autocvar_sht_rescue_time = 180;	/* DESIGN: until the helicopter */
var float autocvar_sht_max_zombies = 28;	/* DESIGN */
var float autocvar_sht_build_time = 3;		/* DESIGN */
var float autocvar_sht_max_days = 31;		/* OBSERVED: 62 half-day waves */
var float autocvar_sht_test_fast = 0;
var float autocvar_sht_debug = 0;		/* tests: a status line every 5 s */
float sht_nextDebug;		/* tests: clock steps per tick */

/* --- helpers ---------------------------------------------------------- */

string
SHT_Model(string m)
{
	return strtolower(m);
}

float
SHT_TeamIdx(float t)
{
	return (t == CS_TEAM1) ? 1 : 2;
}

float
SHT_Day(void)
{
	return floor((sht_half + 1) / 2);
}

float
SHT_SkillLv(entity p, float s)
{
	float v;

	if (s < 11)
		v = p.sht_skillA;
	else if (s < 22) {
		v = p.sht_skillB;
		s -= 11;
	} else {
		v = p.sht_skillC;
		s -= 22;
	}
	return floor(v / pow(4, s)) % 4;
}

void
SHT_SetSkill(entity p, float s, float lv)
{
	float old = SHT_SkillLv(p, s);
	float d = (lv - old);

	if (s < 11)
		p.sht_skillA += d * pow(4, s);
	else if (s < 22)
		p.sht_skillB += d * pow(4, s - 11);
	else
		p.sht_skillC += d * pow(4, s - 22);
}

float
SHT_Param(float s, float n)
{
	return sht_sParam[s * 11 + n - 1];
}

/* the skill's param for the player's level (params lv, lv+off) */
float
SHT_LvParam(entity p, float s, float first)
{
	float lv = SHT_SkillLv(p, s);

	if (lv <= 0)
		return 0;
	return SHT_Param(s, first + lv - 1);
}

string
SHT_ErrorKey(float e)
{
	/* SOURCE-CONFIRMED client.dll FUN_0285cd50 */
	switch (e) {
	case 1: return "CSO_ZSHT_ERROR_FULL_Wood_Inventory";
	case 2: return "CSO_ZSHT_ERROR_FULL_Iron_Inventory";
	case 3: return "CSO_ZSHT_ERROR_NOT_ENOUGH_GLOBAL_RESOURCE_WOOD";
	case 4: return "CSO_ZSHT_ERROR_NOT_ENOUGH_GLOBAL_RESOURCE_IRON";
	case 5: return "CSO_ZSHT_ERROR_NOT_ENOUGH_RESOURCE_WOOD";
	case 6: return "CSO_ZSHT_ERROR_NOT_ENOUGH_RESOURCE_IRON";
	case 7: return "CSO_ZSHT_ERROR_NOT_ENOUGH_RESOURCE_POWER";
	case 8: return "CSO_ZSHT_ERROR_SOMETHING_IN";
	case 9: return "CSO_ZSHT_ERROR_NOT_ENOUGH_SPACE";
	case 10: return "CSO_ZSHT_ERROR_BUILDING_BLOCK";
	case 11: return "CSO_ZSHT_ERROR_WRONG_STATIC_BUILDING_LOCATION";
	case 12: return "CSO_ZSHT_ERROR_CANT_REPLACE_STATIC_BUILDING";
	case 13: return "CSO_ZSHT_ERROR_MINERAL_EMPTY";
	case 14: return "CSO_ZSHT_ERROR_WaitCoolTime";
	case 16: return "CSO_ZSHT_ERROR_REQUIRE_BUILDING";
	case 17: return "CSO_ZSHT_ERROR_REQUIRE_SKILL";
	case 18: return "CSO_ZSHT_ERROR_ALREADY_HAS_ACTIVE_SKILL";
	case 19: return "CSO_ZSHT_ERROR_ONLY_HAS_ONE_ACTIVE_SKILL";
	case 20: return "CSO_ZSHT_ERROR_NOT_ENOUGH_MONEY";
	case 21: return "CSO_ZSHT_ERROR_OUT_OF_RANGE";
	case 22: return "CSO_ZSHT_ERROR_UNKNOWN_WORKTABLE";
	case 25: return "CSO_ZSHT_ERROR_Duplicate_Build";
	case 28: return "CSO_ZSHT_ERROR_ALREADY_REQUEST_RESCUE";
	case 29: return "CSO_ZSHT_ERROR_Max_Build";
	}
	return "";
}

void
SHT_Error(entity p, float e)
{
	string k = SHT_ErrorKey(e);

	sound(p, CHAN_AUTO, "shelterteam/build_impossible.wav", 1, ATTN_NORM);
	CS_Tell(p, CS_Text(k, sprintf("Can't do that (%g)", e)));
	printf("sht %.1f: %s error %g %s\n", time, p.netname, e, k);
}

/* --- CSV loading ------------------------------------------------------ */

/* one data line split on commas into argv; 0 at the end of the file */
float
SHT_ReadRow(filestream f)
{
	string s;

	while ((s = fgets(f))) {
		s = strreplace("\r", "", s);
		if (s == "")
			continue;
		return tokenizebyseparator(s, ",");
	}
	return 0;
}

void
SHT_LoadTables(void)
{
	filestream f;
	float n, i;

	f = fopen("shelterteam/zsht_building.csv", FILE_READ);
	if (f < 0) {
		print("sht: shelterteam/zsht_building.csv missing\n");
	} else {
		SHT_ReadRow(f);	/* header */
		while ((n = SHT_ReadRow(f))) {
			i = stof(argv(0));
			if (i < 0 || i >= SHT_MAXB || n < 23)
				continue;
			sht_bTier[i] = stof(argv(1));
			sht_bCat[i] = stof(argv(2));
			sht_bDepSkill[i] = stof(argv(3));
			sht_bDepBuild[i] = stof(argv(4));
			sht_bClass[i] = strzone(argv(5));
			sht_bModel[i] = strzone(SHT_Model(argv(6)));
			sht_bKey[i] = strzone(strreplace("#", "", argv(7) != "" ? argv(7) : argv(8)));
			sht_bMins[i] = [stof(argv(9)), stof(argv(10)), stof(argv(11))];
			sht_bMaxs[i] = [stof(argv(12)), stof(argv(13)), stof(argv(14))];
			sht_bAttr[i] = stof(argv(15));
			sht_bDur[i] = stof(argv(16));
			sht_bMax[i] = stof(argv(17));
			sht_bWood[i] = stof(argv(18));
			sht_bIron[i] = stof(argv(19));
			sht_bPower[i] = stof(argv(20));
			sht_bGlobal[i] = stof(argv(21));
			sht_bScore[i] = stof(argv(22));
			sht_bRows = max(sht_bRows, i + 1);
		}
		fclose(f);
	}

	f = fopen("shelterteam/zsht_skilltree.csv", FILE_READ);
	if (f >= 0) {
		SHT_ReadRow(f);
		while ((n = SHT_ReadRow(f))) {
			i = stof(argv(1));
			if (i < 0 || i >= SHT_MAXS || n < 19)
				continue;
			sht_sCat[i] = stof(argv(2));
			sht_sMax[i] = stof(argv(3));
			sht_sTier[i] = stof(argv(4));
			for (float k = 0; k < 11; k++)
				sht_sParam[i * 11 + k] = stof(argv(8 + k));
		}
		fclose(f);
	}

	f = fopen("shelterteam/zsht_worktable.csv", FILE_READ);
	if (f >= 0) {
		SHT_ReadRow(f);
		while ((n = SHT_ReadRow(f))) {
			i = stof(argv(0));
			if (i < 0 || i >= SHT_MAXW || n < 4)
				continue;
			sht_wSkill[i] = stof(argv(1));
			sht_wWood[i] = stof(argv(2));
			sht_wIron[i] = stof(argv(3));
		}
		fclose(f);
	}
	printf("sht: %g buildings, skill 9 params %g %g %g\n", sht_bRows, SHT_Param(9, 1), SHT_Param(9, 2), SHT_Param(9, 3));
}

/* maps/zbs_{sb,rs,bb,sp,st_wave,zt}_<map>.csv, decrypted from the .cso */
void
SHT_LoadMapTables(void)
{
	filestream f;
	float n;

	f = fopen(sprintf("maps/zbs_sb_%s.csv", mapname), FILE_READ);
	if (f >= 0) {
		SHT_ReadRow(f);
		while ((n = SHT_ReadRow(f)) && sht_spots < SHT_MAXSPOT) {
			float k = sht_spots++;
			sht_spotTeam[k] = stof(argv(1));
			sht_spotAttr[k] = stof(argv(2));
			sht_spotOrg[k] = [stof(argv(3)), stof(argv(4)), 0];
			sht_spotYaw[k] = stof(argv(5)) * 90;
			sht_spotOn[k] = strzone(argv(6));
			sht_spotLadder[k] = strzone(argv(8));
			sht_spotWall[k] = strzone(argv(9));
		}
		fclose(f);
	}

	f = fopen(sprintf("maps/zbs_rs_%s.csv", mapname), FILE_READ);
	if (f >= 0) {
		SHT_ReadRow(f);
		while ((n = SHT_ReadRow(f)) && sht_rsCount < SHT_MAXRS) {
			if (n < 7)
				continue;
			float r = sht_rsCount++;
			sht_rsType[r] = stof(argv(0));
			sht_rsMin[r] = [min(stof(argv(1)), stof(argv(3))), min(stof(argv(2)), stof(argv(4)))];
			sht_rsMax[r] = [max(stof(argv(1)), stof(argv(3))), max(stof(argv(2)), stof(argv(4)))];
			sht_rsLo[r] = stof(argv(5));
			sht_rsHi[r] = stof(argv(6));
		}
		fclose(f);
	}

	f = fopen(sprintf("maps/zbs_bb_%s.csv", mapname), FILE_READ);
	if (f >= 0) {
		SHT_ReadRow(f);
		while ((n = SHT_ReadRow(f)) && sht_bbCount < SHT_MAXBB) {
			if (n < 5)
				continue;
			float b = sht_bbCount++;
			sht_bbMin[b] = [min(stof(argv(1)), stof(argv(3))), min(stof(argv(2)), stof(argv(4)))];
			sht_bbMax[b] = [max(stof(argv(1)), stof(argv(3))), max(stof(argv(2)), stof(argv(4)))];
		}
		fclose(f);
	}

	f = fopen(sprintf("maps/zbs_sp_%s.csv", mapname), FILE_READ);
	if (f >= 0) {
		while ((n = SHT_ReadRow(f))) {
			if (n < 6)
				continue;
			vector o = [stof(argv(2)), stof(argv(3)), stof(argv(4))];
			if (argv(0) == "P" && sht_psCount < SHT_MAXSP) {
				sht_psOrg[sht_psCount] = o;
				sht_psYaw[sht_psCount] = stof(argv(6));
				sht_psCount++;
			} else if (argv(0) == "Z" && sht_zsCount < SHT_MAXSP) {
				sht_zsId[sht_zsCount] = stof(argv(1));
				sht_zsOrg[sht_zsCount] = o;
				sht_zsCount++;
			}
		}
		fclose(f);
	}

	f = fopen(sprintf("maps/zbs_st_wave_%s.csv", mapname), FILE_READ);
	if (f >= 0) {
		SHT_ReadRow(f);
		while ((n = SHT_ReadRow(f)) && sht_waveRows < SHT_MAXWAVE) {
			if (n < 3)
				continue;
			float w = sht_waveRows++;
			sht_waveId[w] = stof(argv(0));
			sht_waveGap[w] = stof(argv(1));
			sht_waveZombie[w] = stof(argv(2));
			sht_waveSpot[w] = stof(argv(3));
		}
		fclose(f);
	}

	f = fopen(sprintf("maps/zbs_zt_%s.csv", mapname), FILE_READ);
	if (f >= 0) {
		SHT_ReadRow(f);
		while ((n = SHT_ReadRow(f)) && sht_ztRows < SHT_MAXZT) {
			if (n < 6)
				continue;
			float z = sht_ztRows++;
			sht_ztId[z] = stof(argv(0));
			sht_ztLook[z] = strzone(strtolower(argv(1)));
			sht_ztDamage[z] = stof(argv(2));
			sht_ztHealth[z] = stof(argv(5));
		}
		fclose(f);
	}

	printf("sht: %s: %g static spots, %g resource areas, %g block areas, ", mapname, sht_spots, sht_rsCount, sht_bbCount);
	printf("%g/%g spawns, %g wave rows, %g zombie types\n", sht_psCount, sht_zsCount, sht_waveRows, sht_ztRows);
}

/* --- publishing ------------------------------------------------------- */

void
SHT_PublishTeam(float t)
{
	string s = "";

	serverinfo.SetString(sprintf("zsht_res%g", t), sprintf("%g %g %g %g %g %g %g",
		sht_wood[t], sht_iron[t], sht_woodCap[t], sht_ironCap[t],
		sht_powerCap[t] - sht_powerUsed[t], sht_powerCap[t], sht_tier[t]));
	for (float i = 0; i < sht_bRows; i++)
		s = strcat(s, ftos(sht_built[t * SHT_MAXB + i]), " ");
	serverinfo.SetString(sprintf("zsht_built%g", t), s);
}

void
SHT_PublishPlayer(entity p)
{
	userinfo.SetString(p, "zsht_res", sprintf("%g %g %g %g %g", p.sht_carryWood, p.sht_carryIron,
		p.sht_capWood, p.sht_capIron, p.sht_points));
	userinfo.SetString(p, "zsht_skills", sprintf("%g %g %g", p.sht_skillA, p.sht_skillB, p.sht_skillC));
	userinfo.SetInteger(p, "zsht_placing", (int)p.sht_placing);
}

void
SHT_PublishClock(void)
{
	float left;

	if (sht_state == 2)
		left = sht_nightEnd - time;
	else if (sht_state == 1)
		left = ((122 - sht_counter) + 20) * autocvar_sht_tick;
	else
		left = (sht_counter - 102) * autocvar_sht_tick;
	serverinfo.SetString("zsht_clock", sprintf("%g %g %g %g", SHT_Day(), sht_state, time + left, sht_counter));
	serverinfo.SetString("zsht_rescue", sprintf("%g %g %g", sht_rescueFrom, sht_rescueWindow, sht_rescueEnd));
}

/* --- capacities ------------------------------------------------------- */

void
SHT_UpdateCaps(entity p)
{
	/* SOURCE-CONFIRMED FUN_1083bc00: base 15, plus skill 9 / 10 params */
	p.sht_capWood = 15 + SHT_LvParam(p, 9, 4) + SHT_LvParam(p, 10, 1);
	p.sht_capIron = 15 + SHT_LvParam(p, 9, 7) + SHT_LvParam(p, 10, 4);
	/* skills 0 and 4 lower the carry (FUN_10841ff0) */
	if (SHT_SkillLv(p, 0) == 1) {
		p.sht_capWood -= SHT_Param(0, 4);
		p.sht_capIron -= SHT_Param(0, 5);
	}
	if (SHT_SkillLv(p, 4) > 0) {
		p.sht_capWood -= SHT_Param(4, 2);
		p.sht_capIron -= SHT_Param(4, 2);
	}
	p.sht_carryWood = min(p.sht_carryWood, p.sht_capWood);
	p.sht_carryIron = min(p.sht_carryIron, p.sht_capIron);
}

/* add to the team, up to its capacity; returns what didn't fit */
float
SHT_TeamAdd(float t, float type, float amount)
{
	float over;

	if (type == 0) {
		sht_wood[t] += amount;
		over = sht_wood[t] - sht_woodCap[t];
		if (over > 0)
			sht_wood[t] = sht_woodCap[t];
	} else {
		sht_iron[t] += amount;
		over = sht_iron[t] - sht_ironCap[t];
		if (over > 0)
			sht_iron[t] = sht_ironCap[t];
	}
	SHT_PublishTeam(t);
	return max(0, over);
}

/* FUN_10837280: carry what fits; with skill 31 the rest goes to the team */
float
SHT_Give(entity p, float type, float amount)
{
	float over, given;

	if (type == 0) {
		p.sht_carryWood += amount;
		over = p.sht_carryWood - p.sht_capWood;
		if (over > 0)
			p.sht_carryWood = p.sht_capWood;
	} else {
		p.sht_carryIron += amount;
		over = p.sht_carryIron - p.sht_capIron;
		if (over > 0)
			p.sht_carryIron = p.sht_capIron;
	}
	over = max(0, over);
	given = amount - over;
	if (over > 0 && SHT_SkillLv(p, 31) > 0)
		over = SHT_TeamAdd(SHT_TeamIdx(p.team), type, over);
	if (given <= 0 && over > 0)
		SHT_Error(p, type == 0 ? 1 : 2);
	SHT_PublishPlayer(p);
	return given;
}

/* --- costs (FUN_1083b3a0, FUN_1083d6c0, FUN_10844e60) ----------------- */

float
SHT_Cost(entity p, float bid, float what)
{
	float c;

	if (what == 0)
		c = sht_bWood[bid];
	else if (what == 1)
		c = sht_bIron[bid];
	else
		c = sht_bPower[bid];

	if (c <= 0)
		return 0;
	if (what < 2 && SHT_SkillLv(p, 21) > 0)
		c = max(1, c - SHT_LvParam(p, 21, 1));
	if (what == 2 && SHT_SkillLv(p, 22) > 0)
		c = max(1, floor(c * SHT_LvParam(p, 22, 1)));
	return c;
}

float
SHT_CanPay(entity p, float bid)
{
	float t = SHT_TeamIdx(p.team);
	float w = SHT_Cost(p, bid, 0), i = SHT_Cost(p, bid, 1), pw = SHT_Cost(p, bid, 2);

	if (sht_bGlobal[bid]) {
		if (sht_wood[t] < w)
			return 3;
		if (sht_iron[t] < i)
			return 4;
	} else {
		if (p.sht_carryWood < w)
			return 5;
		if (p.sht_carryIron < i)
			return 6;
	}
	if (sht_powerCap[t] - sht_powerUsed[t] < pw)
		return 7;
	return 0;
}

void
SHT_Pay(entity p, float bid)
{
	float t = SHT_TeamIdx(p.team);

	if (sht_bGlobal[bid]) {
		sht_wood[t] -= SHT_Cost(p, bid, 0);
		sht_iron[t] -= SHT_Cost(p, bid, 1);
	} else {
		p.sht_carryWood -= SHT_Cost(p, bid, 0);
		p.sht_carryIron -= SHT_Cost(p, bid, 1);
	}
	sht_powerUsed[t] += SHT_Cost(p, bid, 2);
	SHT_PublishTeam(t);
	SHT_PublishPlayer(p);
}

/* --- checks (FUN_10838010 order) -------------------------------------- */

float
SHT_PreCheck(entity p, float bid)
{
	float t = SHT_TeamIdx(p.team);

	if (bid < 0 || bid >= sht_bRows || (sht_bClass[bid] == "" && sht_bCat[bid] != 11))
		return 32;
	if (sht_bMax[bid] > 0 && sht_built[t * SHT_MAXB + bid] >= sht_bMax[bid])
		return 29;
	if (sht_bDepSkill[bid] > 0 && SHT_SkillLv(p, sht_bDepSkill[bid]) <= 0)
		return 17;
	if (sht_bDepBuild[bid] > 0 && sht_built[t * SHT_MAXB + sht_bDepBuild[bid]] <= 0)
		return 16;
	return SHT_CanPay(p, bid);
}

bool
SHT_Blocked(vector o)
{
	for (float b = 0; b < sht_bbCount; b++)
		if (o[0] >= sht_bbMin[b][0] && o[0] <= sht_bbMax[b][0] &&
			o[1] >= sht_bbMin[b][1] && o[1] <= sht_bbMax[b][1])
			return true;
	return false;
}

vector
SHT_Rotate(vector v, float yaw)
{
	float s = sin(yaw * (M_PI / 180)), c = cos(yaw * (M_PI / 180));

	return [v[0] * c - v[1] * s, v[0] * s + v[1] * c, v[2]];
}

/* the building's box at o, turned by yaw (90 degree steps) */
void
SHT_Box(float bid, float yaw, __out vector mins, __out vector maxs)
{
	vector a = SHT_Rotate(sht_bMins[bid], yaw), b = SHT_Rotate(sht_bMaxs[bid], yaw);

	mins = [min(a[0], b[0]), min(a[1], b[1]), sht_bMins[bid][2]];
	maxs = [max(a[0], b[0]), max(a[1], b[1]), max(sht_bMaxs[bid][2], 8)];
}

float
SHT_PlaceCheck(entity p, float bid, vector o, float yaw)
{
	vector mins, maxs;

	if (SHT_Blocked(o))
		return 10;
	SHT_Box(bid, yaw, mins, maxs);
	tracebox(o + [0, 0, 1], mins, maxs, o + [0, 0, 1], MOVE_NOMONSTERS, p);
	if (trace_startsolid || trace_allsolid)
		return 9;
	for (entity e = findradius(o, 256); e; e = e.chain) {
		if (e == p.sht_ghost || e.solid == SOLID_NOT || e.solid == SOLID_TRIGGER)
			continue;
		if (!(is.Player(e) || e.sht_kind == 1 || e.sht_kind == 4))
			continue;
		if (e.absmin[0] < o[0] + maxs[0] && e.absmax[0] > o[0] + mins[0] &&
			e.absmin[1] < o[1] + maxs[1] && e.absmax[1] > o[1] + mins[1] &&
			e.absmin[2] < o[2] + maxs[2] && e.absmax[2] > o[2] + mins[2])
			return 8;
	}
	return 0;
}

/* --- map pieces ------------------------------------------------------- */

void
SHT_Fire(string name, string input)
{
	if (name == "")
		return;
	for (entity e = world; (e = find(e, ::targetname, name));)
		ents.Input(e, input, "", world);
}

/* --- buildings -------------------------------------------------------- */

float
SHT_GeneratorPower(float bid)
{
	/* SOURCE-CONFIRMED FUN_10887ff0 tunables, by tier */
	switch (sht_bTier[bid]) {
	case 1: return 40;
	case 2: return 75;
	case 3: return 100;
	}
	return 130;
}

void SHT_BuildingThink(void);

entity
SHT_SpawnBuilding(entity p, float bid, vector o, float yaw, float spot)
{
	float t = SHT_TeamIdx(p.team);
	vector mins, maxs;
	entity b = spawn();

	SHT_Box(bid, yaw, mins, maxs);
	b.classname = sht_bClass[bid];
	b.sht_kind = 1;
	b.sht_bid = bid;
	b.team = p.team;
	b.sht_builder = p;
	b.sht_spot = spot;
	b.angles = [0, yaw, 0];
	b.solid = (bid == 9 || bid == 10 || bid == 11 || bid == 27) ? SOLID_TRIGGER : SOLID_BBOX;
	b.movetype = MOVETYPE_NONE;
	b.takedamage = DAMAGE_NO;	/* the rules hurt it, not Nuclide's combat */
	if (sht_bModel[bid] != "")
		setmodel(b, sht_bModel[bid]);
	setsize(b, mins, maxs);
	setorigin(b, o);
	b.max_health = sht_bDur[bid];
	b.health = b.max_health;
	b.sht_ready = time + autocvar_sht_build_time;
	b.alpha = 0.5;
	b.think = SHT_BuildingThink;
	b.nextthink = time + 0.1;

	sht_built[t * SHT_MAXB + bid]++;
	if (sht_bClass[bid] == "shelter_team_generator")
		b.sht_power = -SHT_GeneratorPower(bid);	/* gives power once built */
	else
		b.sht_power = SHT_Cost(p, bid, 2);

	sound(b, CHAN_AUTO, "shelterteam/build2.wav", 1, ATTN_NORM);
	CS_Banner(sprintf(strreplace("%s", "%s", CS_Text("CSO_zsht_Build_Message", "[%s] has started to build [%s].")),
		p.netname, CS_Text(sht_bKey[bid], sht_bClass[bid])));
	printf("sht %.1f: %s builds %g (%s) at %v\n", time, p.netname, bid, sht_bClass[bid], o);
	SHT_PublishTeam(t);
	return b;
}

void
SHT_BuildingDone(entity b)
{
	float t = SHT_TeamIdx(b.team);

	b.alpha = 1;
	sound(b, CHAN_AUTO, "shelterteam/build_complete.wav", 1, ATTN_NORM);
	if (b.sht_power < 0) {
		/* zsht_generatoractivate team power */
		sht_powerCap[t] += -b.sht_power;
	}
	if (b.classname == "shelter_team_warehouse") {
		/* zsht_builtwarehouse: tunables 0x11e58700 / 0x11e57a10 */
		sht_woodCap[t] += 30;
		sht_ironCap[t] += 30;
	}
	if (b.sht_spot > 0) {
		float s = b.sht_spot - 1;
		SHT_Fire(sht_spotOn[s], "Toggle");
		SHT_Fire(sht_spotLadder[s], "Toggle");
		SHT_Fire(sht_spotWall[s], "Toggle");
		SHT_Fire(strreplace("_wall1", "_wall2", sht_spotWall[s]), "Toggle");
	}
	SHT_PublishTeam(t);
}

void
SHT_RemoveBuilding(entity b, bool destroyed)
{
	float t = SHT_TeamIdx(b.team);

	sht_built[t * SHT_MAXB + b.sht_bid] = max(0, sht_built[t * SHT_MAXB + b.sht_bid] - 1);
	if (b.sht_ready <= time) {
		if (b.sht_power < 0)
			sht_powerCap[t] = max(0, sht_powerCap[t] + b.sht_power);
		if (b.classname == "shelter_team_warehouse") {
			sht_woodCap[t] -= 30;
			sht_ironCap[t] -= 30;
			sht_wood[t] = min(sht_wood[t], sht_woodCap[t]);
			sht_iron[t] = min(sht_iron[t], sht_ironCap[t]);
		}
		if (b.sht_spot > 0) {
			float s = b.sht_spot - 1;
			SHT_Fire(sht_spotOn[s], "Toggle");
			SHT_Fire(sht_spotLadder[s], "Toggle");
			SHT_Fire(sht_spotWall[s], "Toggle");
			SHT_Fire(strreplace("_wall1", "_wall2", sht_spotWall[s]), "Toggle");
		}
	}
	if (b.sht_power > 0)
		sht_powerUsed[t] = max(0, sht_powerUsed[t] - b.sht_power);
	if (b.sht_spot > 0)
		sht_spotUsed[b.sht_spot - 1] = world;
	if (destroyed) {
		sound(b, CHAN_AUTO, "shelterteam/crash.wav", 1, ATTN_NORM);
		CS_Banner(strreplace("%s", CS_Text(sht_bKey[b.sht_bid], b.classname),
			CS_Text("CSO_zsht_Destroy_Build_Message", "[%s] has been destroyed.")));
	}
	printf("sht %.1f: building %g removed (%s)\n", time, b.sht_bid, destroyed ? "destroyed" : "gone");
	SHT_PublishTeam(t);
	remove(b);
}

void
SHT_HurtBuilding(entity b, float amount)
{
	b.health -= amount;
	if (b.health <= 0)
		SHT_RemoveBuilding(b, true);
}

/* --- zombies ---------------------------------------------------------- */

bool
SHT_IsZombie(entity e)
{
	return e.sht_kind == 4 && !wasfreed(e) && e.health > 0;
}

void
SHT_HurtZombie(entity z, float amount, entity by)
{
	if (!SHT_IsZombie(z))
		return;
	if (z.health <= amount) {
		ents.Input(z, "SetHealth", "1", world);
		ents.Input(z, "Kill", "", by);
	} else {
		ents.Input(z, "SetHealth", ftos(z.health - amount), world);
	}
}

/* the nearest zombie in sight within range */
entity
SHT_Target(entity b, float range)
{
	entity best = world;
	float bestD = range;
	vector eye = b.origin + [0, 0, max(16, b.maxs[2] * 0.7)];

	for (entity z = findradius(b.origin, range); z; z = z.chain) {
		if (!SHT_IsZombie(z))
			continue;
		float d = vlen(z.origin - b.origin);
		if (d >= bestD)
			continue;
		traceline(eye, z.origin, MOVE_NOMONSTERS, b);
		if (trace_fraction < 1)
			continue;
		best = z;
		bestD = d;
	}
	return best;
}

void
SHT_Shoot(entity b, entity z, float dmg)
{
	vector eye = b.origin + [0, 0, max(16, b.maxs[2] * 0.7)];
	vector dir = normalize(z.origin - eye);

	b.angles = [0, vectoyaw(dir), 0];
	te_gunshot(z.origin);
	pointsound(eye, "weapons/m249-1.wav", 0.6, ATTN_NORM);
	SHT_HurtZombie(z, dmg, b.sht_builder ? b.sht_builder : b);
}

/* DESIGN behaviour per class; the CSO numbers are in classes not traced */
void
SHT_BuildingThink(void)
{
	entity b = self;
	entity z;

	b.nextthink = time + 0.1;
	if (b.sht_ready > time)
		return;
	if (b.alpha < 1) {
		SHT_BuildingDone(b);
		return;
	}
	if (b.sht_nextAct > time)
		return;

	switch (b.classname) {
	case "shelter_team_turret":
		if ((z = SHT_Target(b, 1200)))
			SHT_Shoot(b, z, b.sht_bid == 14 ? 12 : 18);
		b.sht_nextAct = time + (b.sht_bid == 14 ? 0.1 : 0.25);
		break;
	case "shelter_team_flame_turret":
		if ((z = SHT_Target(b, 400))) {
			b.angles = [0, vectoyaw(z.origin - b.origin), 0];
			for (entity f = findradius(z.origin, 96); f; f = f.chain)
				SHT_HurtZombie(f, 10, b.sht_builder);
		}
		b.sht_nextAct = time + 0.15;
		break;
	case "shelter_team_laser_turret":
		if ((z = SHT_Target(b, 1500)))
			SHT_Shoot(b, z, 60);
		b.sht_nextAct = time + 0.6;
		break;
	case "shelter_team_electronic_turret":
		for (z = findradius(b.origin, 500); z; z = z.chain)
			SHT_HurtZombie(z, 20, b.sht_builder);
		b.sht_nextAct = time + 0.5;
		break;
	case "shelter_team_mortar_turret":
		if ((z = SHT_Target(b, 2400))) {
			te_explosion(z.origin);
			pointsound(z.origin, "weapons/explode3.wav", 1, ATTN_NORM);
			for (entity m = findradius(z.origin, 200); m; m = m.chain)
				SHT_HurtZombie(m, 150, b.sht_builder);
		}
		b.sht_nextAct = time + 3;
		break;
	case "shelter_team_mine":
	case "shelter_team_claymore":
	case "shelter_team_icebomb":
		for (z = findradius(b.origin, 64); z; z = z.chain) {
			if (!SHT_IsZombie(z))
				continue;
			te_explosion(b.origin);
			pointsound(b.origin, "weapons/explode3.wav", 1, ATTN_NORM);
			for (entity v = findradius(b.origin, 220); v; v = v.chain)
				SHT_HurtZombie(v, b.classname == "shelter_team_icebomb" ? 60 : 300, b.sht_builder);
			SHT_RemoveBuilding(b, false);
			return;
		}
		b.sht_nextAct = time + 0.2;
		break;
	case "shelter_team_thron":
		for (z = findradius(b.origin, 48); z; z = z.chain)
			SHT_HurtZombie(z, 15, b.sht_builder);
		b.sht_nextAct = time + 0.5;
		break;
	case "shelter_team_field_bed":
	case "shelter_team_campfire":
		/* zsht_healthrestore: a percentage of max health */
		for (entity h = findradius(b.origin, 128); h; h = h.chain)
			if (is.Player(h) && h.team == b.team && h.health > 0 && h.health < h.max_health)
				ents.Input(h, "SetHealth", ftos(min(h.max_health, h.health + h.max_health * 0.03)), world);
		b.sht_nextAct = time + 1;
		break;
	default:
		b.sht_nextAct = time + 1;
	}
}

/* --- placing (zsht_RequestBuild, zsht_BuildConfirm) ------------------- */

vector
SHT_AimSpot(entity p)
{
	makevectors([0, p.v_angle[1], 0]);
	vector want = p.origin + v_forward * 96;
	traceline(p.origin + [0, 0, 16], want + [0, 0, 16], MOVE_NOMONSTERS, p);
	want = trace_endpos - v_forward * 8;
	traceline(want + [0, 0, 16], want - [0, 0, 512], MOVE_NOMONSTERS, p);
	return trace_endpos;
}

void
SHT_GhostThink(void)
{
	entity g = self, p = g.owner;
	float bid;

	if (!p || p.sht_placing <= 0 || p.health <= 0) {
		if (p)
			p.sht_ghost = world;
		remove(g);
		return;
	}
	bid = p.sht_placing - 1;
	setorigin(g, SHT_AimSpot(p));
	g.angles = [0, p.v_angle[1] + p.sht_placeYaw, 0];
	g.angles[1] = rint(g.angles[1] / 90) * 90;
	g.colormod = SHT_PlaceCheck(p, bid, g.origin, g.angles[1]) ? [1, 0.2, 0.2] : [0.3, 1, 0.3];
	g.nextthink = time;
}

void
SHT_StopPlacing(entity p)
{
	p.sht_placing = 0;
	if (p.sht_ghost)
		remove(p.sht_ghost);
	p.sht_ghost = world;
	SHT_PublishPlayer(p);
}

float
SHT_FreeSpot(entity p, float bid)
{
	float best = -1, bestD = 768;

	for (float s = 0; s < sht_spots; s++) {
		if (sht_spotAttr[s] != sht_bAttr[bid] || sht_spotUsed[s])
			continue;
		if (sht_spotTeam[s] && sht_spotTeam[s] != p.team && cs_teams && sht_spotTeam[s] != 2)
			continue;
		float d = vlen([sht_spotOrg[s][0], sht_spotOrg[s][1], p.origin[2]] - p.origin);
		if (d < bestD) {
			best = s;
			bestD = d;
		}
	}
	return best;
}

void
SHT_RequestBuild(entity p, float bid)
{
	float e = SHT_PreCheck(p, bid);

	if (e) {
		SHT_Error(p, e);
		return;
	}

	/* static buildings go on their map spots (StaticBuildConfirm) */
	if (sht_bAttr[bid] != 0) {
		float s = SHT_FreeSpot(p, bid);
		if (s < 0) {
			SHT_Error(p, 11);
			return;
		}
		vector o = sht_spotOrg[s];
		traceline(o + [0, 0, 1024], o - [0, 0, 1024], MOVE_NOMONSTERS, p);
		SHT_Pay(p, bid);
		sht_spotUsed[s] = SHT_SpawnBuilding(p, bid, trace_endpos, sht_spotYaw[s], s + 1);
		return;
	}

	SHT_StopPlacing(p);
	p.sht_placing = bid + 1;
	p.sht_placeYaw = 0;
	entity g = spawn();
	g.classname = "sht_ghost";
	g.sht_kind = 6;
	g.owner = p;
	g.solid = SOLID_NOT;
	g.alpha = 0.5;
	g.effects = EF_FULLBRIGHT;
	if (sht_bModel[bid] != "")
		setmodel(g, sht_bModel[bid]);
	g.think = SHT_GhostThink;
	g.nextthink = time;
	p.sht_ghost = g;
	SHT_PublishPlayer(p);
}

void
SHT_BuildConfirm(entity p, float bid, vector o, float yaw, bool fromGhost)
{
	float e;

	if (fromGhost) {
		if (p.sht_placing <= 0 || !p.sht_ghost)
			return;
		bid = p.sht_placing - 1;
		o = p.sht_ghost.origin;
		yaw = p.sht_ghost.angles[1];
	}
	if ((e = SHT_PreCheck(p, bid))) {
		SHT_Error(p, e);
		return;
	}
	if (sht_bAttr[bid] != 0) {
		SHT_Error(p, 11);
		return;
	}
	if (vlen(o - p.origin) > 256) {
		SHT_Error(p, 21);
		return;
	}
	if ((e = SHT_PlaceCheck(p, bid, o, yaw))) {
		SHT_Error(p, e);
		return;
	}
	SHT_Pay(p, bid);
	SHT_SpawnBuilding(p, bid, o, yaw, 0);
	SHT_StopPlacing(p);
}

/* zsht_upgradeshelter: the main shelter's tiers are rows 24-26 and 29 */
void
SHT_Upgrade(entity p, float bid)
{
	float t = SHT_TeamIdx(p.team);
	float e;

	if (sht_bCat[bid] != 11 || sht_bTier[bid] != sht_tier[t] + 1) {
		SHT_Error(p, 15);	/* FUN_10838390 */
		return;
	}
	if ((e = SHT_PreCheck(p, bid))) {
		SHT_Error(p, e);
		return;
	}
	SHT_Pay(p, bid);
	/* "shelter%d_%d" func_wall_toggles (FUN_1088b990), OBSERVED in the
	   map as shelter2_1, shelter2_2 */
	SHT_Fire(sprintf("shelter%g_%g", p.team, sht_tier[t]), "Toggle");
	sht_tier[t] = sht_bTier[bid];
	SHT_Fire(sprintf("shelter%g_%g", p.team, sht_tier[t]), "Toggle");
	sht_built[t * SHT_MAXB + bid] = 1;
	if (sht_barricade[t]) {
		sht_barricade[t].max_health = sht_bDur[bid];
		sht_barricade[t].health = sht_bDur[bid];
	}
	sound(p, CHAN_AUTO, "shelterteam/build_complete.wav", 1, ATTN_NONE);
	CS_Banner(sprintf("%s: %s", p.netname, CS_Text(sht_bKey[bid], "Shelter upgraded")));
	printf("sht %.1f: team %g shelter tier %g\n", time, t, sht_tier[t]);
	SHT_PublishTeam(t);
}

/* --- skills (zsht_RequestSkillup, zsht_SkillInit) --------------------- */

void
SHT_ApplySkills(entity p)
{
	SHT_UpdateCaps(p);
	/* skill 5: health, skill 6: armour (DESIGN: the params as percent / factor) */
	float hp = 100 * (1 + SHT_LvParam(p, 5, 1) / 100);
	ents.Input(p, "SetMaxHealth", ftos(hp), world);
	if (SHT_SkillLv(p, 6) > 0)
		ents.Input(p, "SetArmor", ftos(100 * SHT_LvParam(p, 6, 1)), world);
	SHT_PublishPlayer(p);
}

void
SHT_SkillUp(entity p, float s)
{
	float t = SHT_TeamIdx(p.team);

	if (s < 0 || s >= SHT_MAXS || sht_sMax[s] <= 0)
		return;
	if (p.sht_points < 1) {
		CS_Tell(p, CS_Text("CSO_ZSHT_ERROR_NOT_ENOUGH_SKILLPOINT", "Not enough skill points"));
		return;
	}
	if (SHT_SkillLv(p, s) >= sht_sMax[s])
		return;
	/* DESIGN: a skill's tier needs the shelter's tier */
	if (sht_sTier[s] > max(1, sht_tier[t])) {
		SHT_Error(p, 16);
		return;
	}
	/* tier 4 skills: only one (errors 0x12 / 0x13) */
	if (sht_sTier[s] == 4) {
		for (float k = 0; k < SHT_MAXS; k++)
			if (k != s && sht_sTier[k] == 4 && SHT_SkillLv(p, k) > 0) {
				SHT_Error(p, 19);
				return;
			}
	}
	SHT_SetSkill(p, s, SHT_SkillLv(p, s) + 1);
	p.sht_points--;
	sound(p, CHAN_AUTO, "shelterteam/skill_button.wav", 1, ATTN_NORM);
	SHT_ApplySkills(p);
}

void
SHT_SkillInit(entity p)
{
	float spent = 0;

	for (float s = 0; s < SHT_MAXS; s++)
		spent += SHT_SkillLv(p, s);
	p.sht_skillA = p.sht_skillB = p.sht_skillC = 0;
	p.sht_points += spent;
	SHT_ApplySkills(p);
}

/* --- worktable (zsht_worktable N) ------------------------------------- */

void
SHT_Worktable(entity p, float w)
{
	if (w < 0 || w >= SHT_MAXW) {
		SHT_Error(p, 22);
		return;
	}
	if (SHT_SkillLv(p, sht_wSkill[w]) <= 0) {
		SHT_Error(p, 17);	/* INFERRED: codes 23/24 have no key in FUN_0285cd50 */
		return;
	}
	if (p.sht_carryWood < sht_wWood[w]) {
		SHT_Error(p, 5);
		return;
	}
	if (p.sht_carryIron < sht_wIron[w]) {
		SHT_Error(p, 6);
		return;
	}
	p.sht_carryWood -= sht_wWood[w];
	p.sht_carryIron -= sht_wIron[w];

	/* DESIGN: the crafts' effects (the items are client-side names) */
	switch (w) {
	case 0: ents.Input(p, "GiveItem", "weapon_knife", world); break;
	case 1: CS_FillAmmo(p); break;
	case 2: ents.Input(p, "GiveItem", "weapon_usp", world); break;
	case 3: ents.Input(p, "GiveItem", "weapon_knife", world); break;
	case 4: ents.Input(p, "GiveItem", "weapon_hegrenade", world); break;
	case 5: ents.Input(p, "SetArmor", "100", world); break;
	case 6: ents.Input(p, "GiveItem", "weapon_knife", world); break;
	case 7:
		for (entity b = world; (b = find(b, ::classname, "shelter_team_turret"));)
			if (b.team == p.team)
				b.health = b.max_health;
		break;
	case 8:
		SHT_TeamAdd(SHT_TeamIdx(p.team), 0, 5);
		SHT_TeamAdd(SHT_TeamIdx(p.team), 1, 5);
		break;
	}
	sound(p, CHAN_AUTO, "shelterteam/build_complete.wav", 1, ATTN_NORM);
	SHT_PublishPlayer(p);
}

/* --- warehouse (zsht_warehouse type toTeam toPlayer) ------------------ */

void
SHT_Warehouse(entity p, float type, float toTeam, float toPlayer)
{
	float t = SHT_TeamIdx(p.team);
	float n;

	if (toTeam > 0) {
		n = min(toTeam, type == 0 ? p.sht_carryWood : p.sht_carryIron);
		n = min(n, type == 0 ? sht_woodCap[t] - sht_wood[t] : sht_ironCap[t] - sht_iron[t]);
		if (n <= 0)
			return;
		if (type == 0) {
			p.sht_carryWood -= n;
			sht_wood[t] += n;
		} else {
			p.sht_carryIron -= n;
			sht_iron[t] += n;
		}
	} else if (toPlayer > 0) {
		n = min(toPlayer, type == 0 ? sht_wood[t] : sht_iron[t]);
		n = min(n, type == 0 ? p.sht_capWood - p.sht_carryWood : p.sht_capIron - p.sht_carryIron);
		if (n <= 0) {
			SHT_Error(p, type == 0 ? 1 : 2);
			return;
		}
		if (type == 0) {
			sht_wood[t] -= n;
			p.sht_carryWood += n;
		} else {
			sht_iron[t] -= n;
			p.sht_carryIron += n;
		}
	}
	SHT_PublishTeam(t);
	SHT_PublishPlayer(p);
}

/* --- rescue (OBSERVED CSO_zsht_RescueDetailLabel1-4) ------------------ */

float
SHT_Humans(void)
{
	float n = 0;

	for (entity p = world; (p = next.Player(p));)
		if (CS_Playing(p) && clienttype(p) != CLIENTTYPE_BOT)
			n++;
	return n;
}

float
SHT_RescueAsked(void)
{
	float n = 0;

	for (entity p = world; (p = next.Player(p));)
		if (CS_Playing(p) && clienttype(p) != CLIENTTYPE_BOT && p.sht_rescueAsked)
			n++;
	return n;
}

void
SHT_RequestRescue(entity p)
{
	if (sht_rescueEnd > 0) {
		SHT_Error(p, 28);
		return;
	}
	if (SHT_Day() < sht_rescueFrom) {
		CS_Tell(p, sprintf("Rescue opens on day %g.", sht_rescueFrom));
		return;
	}
	if (p.sht_rescueAsked) {
		SHT_Error(p, 28);
		return;
	}
	/* "all members need to request within 60 seconds" */
	if (sht_rescueWindow < time) {
		for (entity q = world; (q = next.Player(q));)
			q.sht_rescueAsked = 0;
		sht_rescueWindow = time + 60;
	}
	p.sht_rescueAsked = 1;
	serverinfo.SetString("zsht_rescueask", sprintf("%g %g", SHT_RescueAsked(), SHT_Humans()));
	if (SHT_RescueAsked() >= SHT_Humans()) {
		sht_rescueEnd = time + autocvar_sht_rescue_time;
		CS_Banner(CS_Text("CSO_ZSHT_Rescue", "Rescuing"));
		printf("sht %.1f: rescue requested, helicopter at %.1f\n", time, sht_rescueEnd);
	}
	SHT_PublishClock();
}

/* --- piles and minerals ----------------------------------------------- */

void
SHT_SpawnPiles(void)
{
	for (entity e = world; (e = find(e, ::classname, "zsht_resource"));)
		remove(e);

	for (float r = 0; r < sht_rsCount; r++) {
		float n = sht_rsLo[r] + floor(random() * (sht_rsHi[r] - sht_rsLo[r] + 1));
		for (float k = 0; k < n; k++) {
			vector o = [sht_rsMin[r][0] + random() * (sht_rsMax[r][0] - sht_rsMin[r][0]),
				sht_rsMin[r][1] + random() * (sht_rsMax[r][1] - sht_rsMin[r][1]), 2048];
			traceline(o, o - [0, 0, 4096], MOVE_NOMONSTERS, world);
			if (trace_startsolid || trace_fraction >= 1)
				continue;
			entity e = spawn();
			e.classname = "zsht_resource";
			e.sht_kind = 2;
			if (sht_rsType[r] == 0)
				e.sht_resType = random() < 0.5 ? 0 : 1;
			else
				e.sht_resType = (sht_rsType[r] != 1) ? 1 : 0;
			e.sht_amount = 2 + floor(random() * 4);	/* DESIGN: units per pile */
			setmodel(e, e.sht_resType == 0 ? "models/shelter/item_wood.mdl" : "models/shelter/item_hbeam.mdl");
			e.solid = SOLID_TRIGGER;
			setsize(e, [-16, -16, 0], [16, 16, 24]);
			setorigin(e, trace_endpos);
			e.angles = [0, random() * 360, 0];
		}
	}
}

/* E near a pile, a mineral, a door, a warehouse, a worktable or the comm tower */
bool
SHT_Use(entity p)
{
	entity best = world;
	float bestD = 110;

	makevectors(p.v_angle);
	vector eye = p.origin + p.view_ofs;
	traceline(eye, eye + v_forward * 110, MOVE_NORMAL, p);
	if (trace_ent && trace_ent.sht_kind)
		best = trace_ent;
	for (entity e = findradius(p.origin, 110); e && !best; e = e.chain) {
		if (!e.sht_kind || e.sht_kind == 6 || e.sht_kind == 4)
			continue;
		float d = vlen(e.origin - p.origin);
		if (e.sht_kind == 3 || e.sht_kind == 5)
			d = vlen((e.absmin + e.absmax) / 2 - p.origin) - 48;
		if (d < bestD) {
			best = e;
			bestD = d;
		}
	}
	if (!best)
		return false;

	switch (best.sht_kind) {
	case 2: {
		float got = SHT_Give(p, best.sht_resType, 1 + SHT_LvParam(p, 9, 1));
		if (got > 0) {
			best.sht_amount--;
			sound(p, CHAN_AUTO, "shelterteam/point5.wav", 1, ATTN_NORM);
			printf("sht %.1f: %s picks %g of type %g\n", time, p.netname, got, best.sht_resType);
		}
		if (best.sht_amount <= 0)
			remove(best);
		return true;
	}
	case 3:
		/* CShelterMineral::Use needs skill 10 */
		if (SHT_SkillLv(p, 10) <= 0) {
			SHT_Error(p, 17);
			return true;
		}
		if (best.sht_amount >= max(1, SHT_Param(10, 11))) {
			SHT_Error(p, 13);
			return true;
		}
		best.sht_amount++;
		if (random() < 0.9)
			SHT_Give(p, 1, SHT_Param(10, 9) + floor(random() * (SHT_Param(10, 10) - SHT_Param(10, 9) + 1)));
		else
			SHT_Give(p, 0, SHT_Param(10, 7) + floor(random() * (SHT_Param(10, 8) - SHT_Param(10, 7) + 1)));
		return true;
	case 1:
		switch (best.classname) {
		case "shelter_team_warehouse":
			stuffcmd(p, "zsht_open warehouse\n");
			return true;
		case "shelter_team_worktable":
			stuffcmd(p, "zsht_open worktable\n");
			return true;
		case "shelter_team_comm_tower":
			stuffcmd(p, "zsht_open rescue\n");
			return true;
		case "shelter_team_door":
			/* DESIGN: a gate opens for its team for three seconds */
			if (best.team == p.team && best.solid != SOLID_NOT) {
				best.solid = SOLID_NOT;
				best.alpha = 0.35;
				best.sht_nextAct = time + 3;
				best.think = SHT_BuildingThink;
			}
			return true;
		}
		return false;
	case 5:
		stuffcmd(p, "zsht_open warehouse\n");
		return true;
	}
	return false;
}

/* --- map entities from the lump --------------------------------------- */

void
SHT_ReadMap(void)
{
	string k, v, cls, model, tname;
	float type, hp;
	vector org;
	float n = 0;

	getentitytoken(0);
	while ((k = getentitytoken()) != "") {
		if (k != "{")
			continue;
		cls = model = tname = "";
		type = hp = 0;
		org = [0, 0, 0];
		while ((k = getentitytoken()) != "" && k != "}") {
			v = getentitytoken();
			switch (k) {
			case "classname": cls = v; break;
			case "model": model = v; break;
			case "targetname": tname = v; break;
			case "type": type = stof(v); break;
			case "health": hp = stof(v); break;
			case "origin": org = stov(v); break;
			}
		}
		if (cls != "shelter_team_mineral" && cls != "shelter_team_barricade" && cls != "shelter_warehouse_button")
			continue;
		if (substring(model, 0, 1) != "*")
			continue;
		entity e = spawn();
		e.classname = cls;
		e.targetname = tname;
		e.solid = SOLID_BSP;
		e.movetype = MOVETYPE_PUSH;
		setmodel(e, model);
		setorigin(e, org);
		e.sht_resType = type;
		if (cls == "shelter_team_mineral") {
			e.sht_kind = 3;
		} else if (cls == "shelter_team_barricade") {
			e.sht_kind = 5;
			e.max_health = e.health = max(hp, 1000);
			/* DESIGN: humans' barricade (team 2 in the PvE maps) */
			sht_barricade[2] = e;
		} else {
			e.sht_kind = 5;
			e.health = 0;
		}
		n++;
	}
	printf("sht: %g mineral / barricade / warehouse button entities from the map\n", n);
}

/* --- waves ------------------------------------------------------------ */

float
SHT_ZtRow(float id)
{
	for (float z = 0; z < sht_ztRows; z++)
		if (sht_ztId[z] == id)
			return z;
	return -1;
}

void
SHT_SpawnZombie(float id, float spotId)
{
	float zr = SHT_ZtRow(id);
	vector o;
	string cls;

	if (sht_zombies >= autocvar_sht_max_zombies || sht_zsCount <= 0)
		return;

	float s = -1;
	if (spotId > 0)
		for (float k = 0; k < sht_zsCount; k++)
			if (sht_zsId[k] == spotId)
				s = k;
	if (s < 0) {
		/* regular spawn points: ids under 1000 */
		for (float tries = 0; tries < 8 && s < 0; tries++) {
			float c = floor(random() * sht_zsCount);
			if (sht_zsId[c] < 1000)
				s = c;
		}
		if (s < 0)
			s = 0;
	}
	o = sht_zsOrg[s];

	cls = "csm_shz_zombi_host";
	if (zr >= 0) {
		switch (sht_ztLook[zr]) {
		case "zombi_origin":
		case "speed_zombi_host":
		case "heavy_zombi_host":
		case "deimos_zombi_host":
			cls = strcat("csm_shz_", sht_ztLook[zr]);
			break;
		case "zombi_host":
			break;
		default:
			/* boss and other looks this content doesn't have */
			cls = (substring(sht_ztLook[zr], 0, 4) == "zbs_") ? "csm_shz_zombi_origin" : cls;
		}
	}

	entity m = ents.Create(cls, o + [random(-24, 24), random(-24, 24), 40]);
	if (!m)
		return;
	m.sht_kind = 4;
	m.sht_ztRow = zr;
	if (sht_zombies < 3)
		printf("sht %.1f: zombie %g (%s, %s) at %v\n", time, id, cls, zr >= 0 ? sht_ztLook[zr] : "?", m.origin);
	if (zr >= 0) {
		ents.Input(m, "SetHealth", ftos(sht_ztHealth[zr]), world);
		ents.Input(m, "SetMaxHealth", ftos(sht_ztHealth[zr]), world);
	}
	/* OBSERVED: Shelter zombies head for the nearest survivor wherever
	   they are, so they run as a Nuclide horde (always know the target) */
	ents.Input(m, "HordeOn", "", world);
	sht_zombies++;
}

void
SHT_StartWave(void)
{
	for (float w = 0; w < sht_waveRows; w++) {
		if (sht_waveId[w] != sht_half) {
			sht_waveNext[w] = 0;
			continue;
		}
		if (sht_waveGap[w] >= 9999) {
			/* zombie 9999: an event message; otherwise one spawn */
			if (sht_waveZombie[w] < 9999)
				SHT_SpawnZombie(sht_waveZombie[w], sht_waveSpot[w]);
			sht_waveNext[w] = 0;
		} else {
			sht_waveNext[w] = time + sht_waveGap[w];
		}
	}
}

void
SHT_RunWave(void)
{
	sht_zombies = 0;
	for (entity z = world; (z = nextent(z));)
		if (z.sht_kind == 4) {
			if (wasfreed(z) || z.health <= 0)
				z.sht_kind = 0;
			else
				sht_zombies++;
		}

	for (float w = 0; w < sht_waveRows; w++) {
		if (sht_waveNext[w] <= 0 || sht_waveNext[w] > time)
			continue;
		sht_waveNext[w] = time + sht_waveGap[w];
		SHT_SpawnZombie(sht_waveZombie[w], 0);
	}
}

/* --- the clock -------------------------------------------------------- */

void
SHT_Lights(void)
{
	/* DESIGN: darker towards night */
	string l = "m";

	if (sht_state == 2)
		l = "d";
	else if (sht_counter < 108)
		l = substring("defghijklm", sht_counter - 103, 1);
	lightstyle(0, l);
}

void
SHT_Dawn(void)
{
	sht_state = 1;
	sht_half++;
	sht_counter = 103;
	sht_tick = time + autocvar_sht_tick;

	SHT_SpawnPiles();
	for (entity m = world; (m = find(m, ::classname, "shelter_team_mineral"));)
		m.sht_amount = 0;
	for (entity p = world; (p = next.Player(p));) {
		if (!CS_Playing(p))
			continue;
		p.sht_points++;
		if (SHT_Day() > 1)
			CS_FillAmmo(p);	/* OBSERVED CSO_zsht_ammorefill */
		SHT_PublishPlayer(p);
	}
	if (SHT_Day() >= 21 && !sht_c4Broken) {
		sht_c4Broken = true;
		SHT_Fire("c4_object_break", "Break");
	}
	SHT_StartWave();
	CS_Banner(sprintf("Day %g", SHT_Day()));
	sound(world, CHAN_AUTO, "shelterteam/skill_bonus.wav", 1, ATTN_NONE);
	printf("sht %.1f: day %g (half %g)\n", time, SHT_Day(), sht_half);
	SHT_Lights();
	SHT_PublishClock();
}

void
SHT_Dusk(void)
{
	sht_state = 2;
	sht_half++;
	sht_nightEnd = time + autocvar_sht_night;
	SHT_StartWave();
	CS_Banner(sprintf("Night %g: the zombies are coming", SHT_Day()));
	sound(world, CHAN_AUTO, "shelterteam/siren.wav", 1, ATTN_NONE);
	printf("sht %.1f: night %g (half %g)\n", time, SHT_Day(), sht_half);
	SHT_Lights();
	SHT_PublishClock();
}

void
SHT_Clock(void)
{
	float steps = max(1, autocvar_sht_test_fast);

	if (sht_state == 2) {
		if (time >= sht_nightEnd || autocvar_sht_test_fast > 1)
			SHT_Dawn();
		return;
	}
	if (time < sht_tick)
		return;
	sht_tick = time + autocvar_sht_tick;
	if (sht_state == 1) {
		sht_counter += steps;
		if (sht_counter > 121)
			sht_state = 0;
	} else {
		sht_counter -= steps;
		if (sht_counter < 103)
			SHT_Dusk();
	}
	SHT_Lights();
	SHT_PublishClock();
}

/* --- mode hooks ------------------------------------------------------- */

float
SHT_PickTeam(entity p)
{
	/* PvE maps: every human on team 2 (zbs_sb team 2, OBSERVED) */
	if (sht_psCount > 0 && strstrofs(mapname, "_pvp") < 0)
		return CS_TEAM2;
	return CS_AutoTeam(p);
}

void
SHT_Equip(entity p)
{
	ents.ChangeToClass(p, "csm_rifleman");
	ents.Input(p, "GiveItem", "weapon_usp", world);	/* the zsht pistol, FUN_108396b0 */
	CS_FillAmmo(p);
	p.sht_placed = 0;
	SHT_ApplySkills(p);
}

void
SHT_RoundStart(void)
{
	for (float t = 1; t <= 2; t++) {
		/* INFERRED FUN_10788a70: team capacity starts at 10 and 10 */
		sht_wood[t] = sht_iron[t] = 0;
		sht_woodCap[t] = sht_ironCap[t] = 10;
		sht_powerCap[t] = sht_powerUsed[t] = 0;
		sht_tier[t] = 1;
		for (float b = 0; b < SHT_MAXB; b++)
			sht_built[t * SHT_MAXB + b] = 0;
		sht_built[t * SHT_MAXB + 24] = 1;	/* shelter tier 1 stands */
	}
	for (entity e = world; (e = nextent(e));) {
		if (e.sht_kind == 1 || e.sht_kind == 2 || e.sht_kind == 6)
			remove(e);
		else if (e.sht_kind == 4 && !wasfreed(e))
			ents.Input(e, "Kill", "", world);
	}
	for (float s = 0; s < SHT_MAXSPOT; s++)
		sht_spotUsed[s] = world;
	if (sht_barricade[2])
		sht_barricade[2].health = sht_barricade[2].max_health;
	for (entity p = world; (p = next.Player(p));) {
		p.sht_carryWood = p.sht_carryIron = 0;
		p.sht_skillA = p.sht_skillB = p.sht_skillC = 0;
		p.sht_points = 0;
		p.sht_rescueAsked = 0;
		SHT_StopPlacing(p);
		SHT_ApplySkills(p);
	}
	sht_half = 0;
	sht_rescueFrom = autocvar_sht_rescue_day;
	sht_rescueEnd = sht_rescueWindow = 0;
	sht_c4Broken = false;
	SHT_PublishTeam(1);
	SHT_PublishTeam(2);
	SHT_Dawn();
}

void
SHT_Frame(void)
{
	SHT_Clock();
	SHT_RunWave();

	/* humans to the P spots of zbs_sp (the map's own spawns are a lobby) */
	for (entity p = world; (p = next.Player(p));) {
		if (!CS_Playing(p) || p.health <= 0 || p.sht_placed || sht_psCount <= 0)
			continue;
		float k = floor(random() * sht_psCount);
		CS_MoveToSpot(p, sht_psOrg[k] + [0, 0, 36], [0, sht_psYaw[k], 0]);
		p.sht_placed = 1;
	}

	/* zombies hurt what they stand next to (DESIGN) */
	if (time >= sht_nextHit) {
		sht_nextHit = time + 1;
		for (entity z = world; (z = nextent(z));) {
			if (!SHT_IsZombie(z))
				continue;
			float dmg = (z.sht_ztRow >= 0) ? sht_ztDamage[z.sht_ztRow] : 20;
			for (entity b = findradius(z.origin, 80); b; b = b.chain) {
				if (b.sht_kind == 1 && b.solid != SOLID_TRIGGER) {
					SHT_HurtBuilding(b, dmg);
					break;
				}
				if (b.sht_kind == 5 && b.max_health > 0) {
					b.health -= dmg;
					break;
				}
			}
		}
	}

	if (time >= sht_nextInfo) {
		sht_nextInfo = time + 1;
		if (sht_barricade[2])
			serverinfo.SetString("zsht_barricade", sprintf("%g %g", max(0, sht_barricade[2].health), sht_barricade[2].max_health));
		serverinfo.SetString("zsht_rescueask", sprintf("%g %g", SHT_RescueAsked(), SHT_Humans()));
		serverinfo.SetInteger("zsht_zombies", (int)sht_zombies);
	}
	if (autocvar_sht_debug && time >= sht_nextDebug) {
		sht_nextDebug = time + 5;
		entity h = next.Player(world);
		entity nz = world;
		float nd = 99999;
		for (entity z = world; (z = nextent(z));)
			if (SHT_IsZombie(z) && h && vlen(z.origin - h.origin) < nd) {
				nd = vlen(z.origin - h.origin);
				nz = z;
			}
		printf("sht %.1f: state %g counter %g zombies %g; ", time, sht_state, sht_counter, sht_zombies);
		printf("%s hp %g at %v; ", h ? h.netname : "-", h ? h.health : 0, h ? h.origin : [0,0,0]);
		printf("nearest zombie %g away at %v (hp %g); ", nd, nz ? nz.origin : [0,0,0], nz ? nz.health : 0);
		printf("barricade %g/%g\n", sht_barricade[2] ? sht_barricade[2].health : -1, sht_barricade[2] ? sht_barricade[2].max_health : -1);
	}
}

float
SHT_CheckRound(void)
{
	if (sht_barricade[2] && sht_barricade[2].health <= 0)
		return CS_TEAM1;
	if (sht_rescueEnd > 0 && time >= sht_rescueEnd && CS_Alive(CS_TEAM2) > 0)
		return CS_TEAM2;
	if (SHT_Day() > autocvar_sht_max_days)
		return CS_TEAM2;
	return 0;
}

void
SHT_Killed(entity victim, entity inflictor, entity attacker, string weapon)
{
	if (is.Player(victim)) {
		SHT_StopPlacing(victim);
		victim.cs_respawnAt = time + 5;	/* DESIGN */
	}
}

void
SHT_Disconnect(entity p)
{
	SHT_StopPlacing(p);
}

bool
SHT_Command(entity p, string cmd)
{
	float n = tokenize(cmd);

	/* the client's commands are mixed case (zsht_RequestBuild) */
	switch (strtolower(argv(0))) {
	case "csnz_use":
		return SHT_Use(p);
	case "zsht_requestbuild":
		SHT_RequestBuild(p, stof(argv(1)));
		return true;
	case "zsht_buildconfirm":
		if (n >= 6)
			SHT_BuildConfirm(p, stof(argv(1)), [stof(argv(2)), stof(argv(3)), stof(argv(4))], stof(argv(5)), false);
		else
			SHT_BuildConfirm(p, 0, [0, 0, 0], 0, true);
		return true;
	case "zsht_rotate":
		p.sht_placeYaw += 90;
		return true;
	case "zsht_buildcancel":
	case "zsht_movebuildcancel":
		SHT_StopPlacing(p);
		return true;
	case "zsht_upgradeshelter":
		SHT_Upgrade(p, stof(argv(1)));
		return true;
	case "zsht_requestskillup":
		SHT_SkillUp(p, stof(argv(1)));
		return true;
	case "zsht_skillinit":
		SHT_SkillInit(p);
		return true;
	case "zsht_worktable":
		SHT_Worktable(p, stof(argv(1)));
		return true;
	case "zsht_warehouse":
		SHT_Warehouse(p, stof(argv(1)), stof(argv(2)), stof(argv(3)));
		return true;
	case "zsht_requestrescue":
		SHT_RequestRescue(p);
		return true;
	case "zsht_test":
		/* tests: zsht_test <wood> <iron> <points> [teamwood teamiron power] */
		p.sht_carryWood = stof(argv(1));
		p.sht_carryIron = stof(argv(2));
		p.sht_points = stof(argv(3));
		if (n > 4) {
			float t = SHT_TeamIdx(p.team);
			sht_woodCap[t] = max(sht_woodCap[t], stof(argv(4)));
			sht_ironCap[t] = max(sht_ironCap[t], stof(argv(5)));
			sht_wood[t] = stof(argv(4));
			sht_iron[t] = stof(argv(5));
			sht_powerCap[t] = stof(argv(6));
			SHT_PublishTeam(t);
		}
		SHT_UpdateCaps(p);
		p.sht_carryWood = stof(argv(1));
		p.sht_carryIron = stof(argv(2));
		SHT_PublishPlayer(p);
		return true;
	}
	return false;
}

/* zombie nav graph. DESIGN: CSNZ ships no .way files; Nuclide's monsters
   route over data/<map>.way and stand still without one. When the file is
   missing this flood-fills the walkable floor from every P and Z spot on a
   grid and writes it in Nuclide's .way layout. It runs before Nodes_Init,
   which reads it in the same map load. */
#define SHTN_MAX 8192
#define SHTN_LINKS 8
#define SHTN_GRID 96
#define SHTN_STEP 18
#define SHTN_DROP 768

/* heap, not global arrays: these would push the progs past 64k globals */
vector *shtn_org;
int *shtn_link;
int *shtn_linkCount;
int shtn_count;
hashtable shtn_cells;

int
SHT_NavAdd(vector p)
{
	string key = sprintf("%d %d %d", rint(p[0] / SHTN_GRID), rint(p[1] / SHTN_GRID), rint(p[2] / 48));
	float idx = hash_get(shtn_cells, key, -1, EV_FLOAT);

	if (idx >= 0)
		return ((int)idx);
	if (shtn_count >= SHTN_MAX)
		return (-1);

	shtn_org[shtn_count] = p;
	shtn_linkCount[shtn_count] = 0;
	hash_add(shtn_cells, key, (float)shtn_count, EV_FLOAT);
	shtn_count++;
	return (shtn_count - 1);
}

void
SHT_NavLink(int a, int b)
{
	if (a == b || shtn_linkCount[a] >= SHTN_LINKS)
		return;
	for (int i = 0; i < shtn_linkCount[a]; i++)
		if (shtn_link[a * SHTN_LINKS + i] == b)
			return;
	shtn_link[a * SHTN_LINKS + shtn_linkCount[a]] = b;
	shtn_linkCount[a]++;
}

vector
SHT_NavWalk(vector from, vector to, vector hullMin, vector hullMax)
{
	vector up = [0, 0, SHTN_STEP];
	vector at = from;
	vector stride = (to - from) / 4;

	for (int i = 0; i < 4; i++) {
		vector nxt = at + stride;

		tracebox(at + up, hullMin, hullMax, nxt + up, MOVE_NOMONSTERS, world);
		if (trace_fraction < 1.0 || trace_startsolid)
			return ([0, 0, 0]);
		tracebox(nxt + up, hullMin, hullMax, nxt - [0, 0, SHTN_DROP], MOVE_NOMONSTERS, world);
		if (trace_fraction == 1.0 || trace_startsolid || trace_plane_normal[2] < 0.7)
			return ([0, 0, 0]);
		at = trace_endpos;
	}

	if (pointcontents(at) != CONTENT_EMPTY)
		return ([0, 0, 0]);
	return (at);
}

void
SHT_NavFree(void)
{
	hash_destroytab(shtn_cells);
	memfree(shtn_org);
	memfree(shtn_link);
	memfree(shtn_linkCount);
}

void
SHT_NavSeed(vector o, vector hullMin, vector hullMax)
{
	tracebox(o + [0, 0, 8], hullMin, hullMax, o - [0, 0, SHTN_DROP], MOVE_NOMONSTERS, world);
	if (!trace_startsolid && trace_fraction < 1.0)
		SHT_NavAdd(trace_endpos);
}

void
SHT_BuildNav(void)
{
	vector hullMin = [-16, -16, -36];
	vector hullMax = [16, 16, 36];
	vector dirs[8] = {[1,0,0], [-1,0,0], [0,1,0], [0,-1,0], [1,1,0], [1,-1,0], [-1,1,0], [-1,-1,0]};
	int head = 0;
	int links = 0;
	float k;

	/* Nuclide saves its spawn-point-only fallback graph, a handful of
	   unlinked nodes, under the same name; treat that as missing */
	filestream old = fopen(sprintf("data/%s.way", mapname), FILE_READ);
	if (old >= 0) {
		float have = stof(fgets(old));
		fclose(old);
		if (have >= 64)
			return;
	}

	shtn_count = 0;
	shtn_org = (vector *)memalloc(sizeof(vector) * SHTN_MAX);
	shtn_link = (int *)memalloc(sizeof(int) * SHTN_MAX * SHTN_LINKS);
	shtn_linkCount = (int *)memalloc(sizeof(int) * SHTN_MAX);
	shtn_cells = hash_createtab(SHTN_MAX, EV_FLOAT);
	for (k = 0; k < sht_psCount; k++)
		SHT_NavSeed(sht_psOrg[k], hullMin, hullMax);
	for (k = 0; k < sht_zsCount; k++)
		SHT_NavSeed(sht_zsOrg[k], hullMin, hullMax);

	while (head < shtn_count) {
		vector at = shtn_org[head];
		for (int d = 0; d < 8; d++) {
			vector land = SHT_NavWalk(at, at + dirs[d] * SHTN_GRID, hullMin, hullMax);
			if (land == [0, 0, 0])
				continue;
			int n = SHT_NavAdd(land);
			if (n >= 0)
				SHT_NavLink(head, n);
		}
		head++;
	}

	filestream f = fopen(sprintf("data/%s.way", mapname), FILE_WRITE);
	if (f < 0) {
		printf("shelterteam: cannot write data/%s.way\n", mapname);
		SHT_NavFree();
		return;
	}
	fputs(f, sprintf("%i\n", shtn_count));
	for (int i = 0; i < shtn_count; i++) {
		fputs(f, sprintf("%v %f %i\n", shtn_org[i], SHTN_GRID / 2, shtn_linkCount[i]));
		for (int j = 0; j < shtn_linkCount[i]; j++) {
			int to = shtn_link[i * SHTN_LINKS + j];
			fputs(f, sprintf(" %i %f 0\n", to, vlen(shtn_org[to] - shtn_org[i])));
			links++;
		}
	}
	fclose(f);
	SHT_NavFree();
	printf("shelterteam: wrote data/%s.way, %i nodes, %i links\n", mapname, shtn_count, links);
}

void
CSM_ShelterTeam(void)
{
	cs_modeTitle = "CSO_GameMode_ShelterTeam";
	cs_modeFallback = "Shelter Team";
	cs_teams = true;
	cs_rounds = true;
	cs_oneSideStart = false;
	cs_respawnDelay = 5;
	cs_roundTimeFallback = 90;	/* OBSERVED zbs_<map>.csv roundTime 5400 */
	cs_winLimitFallback = 1;
	cs_freezeLock = false;
	cs_team1Name = "Zombies";
	cs_team2Name = "Survivors";

	precache_sound("shelterteam/build2.wav");
	precache_sound("shelterteam/build_complete.wav");
	precache_sound("shelterteam/build_impossible.wav");
	precache_sound("shelterteam/crash.wav");
	precache_sound("shelterteam/point5.wav");
	precache_sound("shelterteam/siren.wav");
	precache_sound("shelterteam/skill_bonus.wav");
	precache_sound("shelterteam/skill_button.wav");
	precache_sound("weapons/m249-1.wav");
	precache_sound("weapons/explode3.wav");
	precache_model("models/shelter/item_wood.mdl");
	precache_model("models/shelter/item_hbeam.mdl");

	SHT_LoadTables();
	SHT_LoadMapTables();
	for (float b = 0; b < sht_bRows; b++)
		if (sht_bModel[b] != "")
			precache_model(sht_bModel[b]);
	SHT_ReadMap();
	SHT_BuildNav();

	cs_pve = (sht_psCount > 0 && strstrofs(mapname, "_pvp") < 0);

	M_Equip = SHT_Equip;
	M_PickTeam = SHT_PickTeam;
	M_RoundStart = SHT_RoundStart;
	M_Frame = SHT_Frame;
	M_Killed = SHT_Killed;
	M_CheckRound = SHT_CheckRound;
	M_Command = SHT_Command;
	M_Disconnect = SHT_Disconnect;
}
