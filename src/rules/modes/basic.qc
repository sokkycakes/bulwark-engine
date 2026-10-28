/* basic: the deathmatch, team deathmatch and Original families.
 * Original code (ISC). Evidence labels as in csnz_common.qc.
 *
 *   1 DeathMatch, 4 BotDM            free for all, respawn, kill limit
 *   2 TeamDeathMatch, 5 BotTDM       teams, respawn, team kill limit
 *   34 SuddenDeath                   free for all rounds, last one standing bonus
 *   40 Allstar                       TDM with a random class on every respawn
 *   19 TDM_Item, 11 Zombi_Team       TDM with item boxes (and human/zombie classes)
 *   0 Original, 3 Bot, 23 LightOri, 42 SeasonOriginal, 50 Partner,
 *   6/7 Official, 31 FightYard        round elimination with map missions
 *   21 Bazooka                       rocket-only team rounds, faster players */

/* --- shared ------------------------------------------------------------- */

void
CSM_EquipDefault(entity p)
{
	ents.ChangeToClass(p, "csm_rifleman");
}

/* bots: OBSERVED default_bot_setting.csv row 0: 4 bots with the human team
   and 5 on the bot team. Used when the room asks for bots but sets no count. */
void
CSM_DefaultBots(float count)
{
	if (autocvar_csnz_bots <= 0)
		cvars.SetFloat("csnz_bots", count);
}

/* --- deathmatch ---------------------------------------------------------- */

void
CSM_Deathmatch(bool bots)
{
	cs_modeTitle = bots ? "CSO_GameMode_BotDM" : "CSO_GameMode_DeathMatch";
	cs_modeFallback = bots ? "Bot Deathmatch" : "Deathmatch";
	cs_teams = false;
	cs_rounds = false;
	cs_respawnDelay = 2;		/* DESIGN */
	cs_scoreKills = true;
	cs_killLimitFallback = 40;	/* OBSERVED limitlist id 2 default */
	cs_timeLimitFallback = 20;	/* OBSERVED limitlist id 5 default */
	cs_noBuy = true;
	M_Equip = CSM_EquipDefault;
	if (bots) {
		cs_noStats = true;	/* OBSERVED tip: K/D not accumulated */
		CSM_DefaultBots(9);
	}
}

void
CSM_TeamDeathmatch(bool bots)
{
	cs_modeTitle = bots ? "CSO_GameMode_BotTDM" : "CSO_GameMode_TeamDeathMatch";
	cs_modeFallback = bots ? "Bot Team Deathmatch" : "Team Deathmatch";
	cs_teams = true;
	cs_rounds = false;
	cs_respawnDelay = 2;
	cs_scoreKills = true;
	cs_killLimitFallback = 160;	/* OBSERVED limitlist id 3 default */
	cs_timeLimitFallback = 20;
	cs_noBuy = true;
	M_Equip = CSM_EquipDefault;
	if (bots) {
		cs_noStats = true;
		CSM_DefaultBots(9);
	}
}

/* --- sudden death (34) ------------------------------------------------------
   OBSERVED tip: "Eliminate everyone around you to reach the goal first.
   Bonus kill points are awarded for being the last one alive."
   INFERRED: free for all rounds without respawn; DESIGN: bonus of 2. */

float
CSM_SD_Check(void)
{
	return CS_Alive(CS_TEAM_NONE) <= 1 ? 3 : 0;
}

void
CSM_SD_RoundEnded(float winner)
{
	for (entity p = world; (p = next.Player(p));) {
		if (CS_Playing(p)) {
			p.cs_kills += 2;
			p.frags += 2;
			CS_Banner(sprintf("%s is the last one standing (+2)", p.netname));
			return;
		}
	}
	CS_Banner(CS_Text("CSO_RoundDraw", "Round Draw"));
}

void
CSM_SuddenDeath(void)
{
	CSM_Deathmatch(false);
	cs_modeTitle = "CSO_GameMode_SuddenDeath";
	cs_modeFallback = "Sudden Death";
	cs_rounds = true;
	cs_respawnDelay = -1;
	cs_roundTimeFallback = 2;	/* DESIGN */
	M_CheckRound = CSM_SD_Check;
	M_RoundEnded = CSM_SD_RoundEnded;
}

/* --- allstar (40) -------------------------------------------------------------
   OBSERVED tip: "Classes are randomly assigned, and the game rules are the
   same as Team Deathmatch." Tooltip: classes are re-assigned upon death.
   The CSO character classes are not available: the classes here are DESIGN
   stand-ins with different health, speed and weapons. */

void
CSM_AllstarEquip(entity p)
{
	string classes = "csm_rifleman csm_heavy csm_scout csm_sniper csm_brawler";
	int n = tokenize(classes);
	string c = argv((int)floor(random() * n));
	ents.ChangeToClass(p, c);
	userinfo.SetString(p, "csnz_class", c);
}

void
CSM_Allstar(void)
{
	CSM_TeamDeathmatch(false);
	cs_modeTitle = "CSO_GameMode_Allstar";
	cs_modeFallback = "All Star";
	M_Equip = CSM_AllstarEquip;
}

/* --- item battle (19) and zombie team deathmatch (11) ------------------------
   OBSERVED (CSO_MaploadingTip_ZTDM): "Collect items or kill opposing team
   members"; "use items by pressing E"; "switch classes when you die";
   "some items are used immediately"; "+2 points for a kill, +1 for an item".
   OBSERVED (TDM_ITEM): kill/death points are not accumulated.
   The item list is DESIGN (CSO's item set for these modes is unknown). */

#define ITEM_KINDS 6
string cs_itemNames[ITEM_KINDS] = {"heal", "armor", "speed", "damage", "ammo", "stealth"};
bool cs_itemPoints;		/* +1 per item, +2 per kill (Zombi_Team) */
float cs_nextItem;
.float cs_heldItem;		/* 1 + kind, used with csnz_use */
.float cs_speedEnd;
.float cs_stealthEnd;

void
CSM_SpawnItemBox(void)
{
	entity spot = CS_PickSpawn(random() < 0.5 ? CS_TEAM1 : CS_TEAM2);
	entity box;

	if (!spot)
		return;
	box = spawn();
	box.classname = "csnz_itembox";
	setmodel(box, "models/w_backpack.mdl");
	setsize(box, [-12, -12, 0], [12, 12, 16]);
	/* a little away from the spawn point itself */
	setorigin(box, spot.origin + [random(-96, 96), random(-96, 96), 0]);
	box.movetype = MOVETYPE_TOSS;
	box.solid = SOLID_NOT;
	box.cs_heldItem = 1 + floor(random() * ITEM_KINDS);
	box.colormod = [1, 0.8, 0.2];
}

void
CSM_ApplyItem(entity p, float kind)
{
	switch (kind) {
	case 1:
		ents.Input(p, "SetHealth", ftos(min(p.max_health, p.health + 50)), world);
		break;
	case 2:
		ents.Input(p, "SetArmor", "100", world);
		break;
	case 3:
		p.cs_speedEnd = time + 10;
		break;
	case 4:
		ents.Input(p, "GiveItem", "weapon_m249", world);
		ents.Input(p, "SwitchToWeapon", "weapon_m249", world);
		break;
	case 5:
		CS_FillAmmo(p);
		break;
	case 6:
		p.cs_stealthEnd = time + 10;
		ents.Input(p, "Alpha", "0.15", world);
		break;
	}
	printf("csnz %.1f: %s used item %s\n", time, p.netname, cs_itemNames[kind - 1]);
}

void
CSM_ItemFrame(void)
{
	int boxes = 0;

	for (entity b = world; (b = find(b, ::classname, "csnz_itembox"));) {
		boxes++;
		for (entity p = world; (p = next.Player(p));) {
			if (!CS_Playing(p) || vlen(p.origin - b.origin) > 40)
				continue;
			float kind = b.cs_heldItem;
			remove(b);
			if (cs_itemPoints) {
				p.cs_score++;
				if (p.team == CS_TEAM1)
					cs_score1++;
				else
					cs_score2++;
			}
			/* heal and ammo apply at once; the rest wait for E */
			if (kind == 1 || kind == 5) {
				CSM_ApplyItem(p, kind);
			} else {
				p.cs_heldItem = kind;
				userinfo.SetString(p, "csnz_item", cs_itemNames[kind - 1]);
				CS_Tell(p, sprintf("Picked up %s. Press E to use it.", cs_itemNames[kind - 1]));
			}
			break;
		}
	}

	/* DESIGN: up to 2 boxes per player, a new one every 5 seconds */
	if (time >= cs_nextItem && boxes < 2 * CS_Players()) {
		CSM_SpawnItemBox();
		cs_nextItem = time + 5;
	}

	for (entity q = world; (q = next.Player(q));) {
		if (q.cs_speedEnd > time) {
			q.velocity[0] *= 1.02;
			q.velocity[1] *= 1.02;
		}
		if (q.cs_stealthEnd && time >= q.cs_stealthEnd) {
			q.cs_stealthEnd = 0;
			ents.Input(q, "Alpha", "1", world);
		}
	}
}

bool
CSM_ItemCommand(entity p, string cmd)
{
	tokenize(cmd);
	if (argv(0) == "csnz_use" && p.cs_heldItem > 0 && CS_Playing(p)) {
		float kind = p.cs_heldItem;
		p.cs_heldItem = 0;
		userinfo.SetString(p, "csnz_item", "");
		CSM_ApplyItem(p, kind);
		return true;
	}
	if (argv(0) == "csnz_class") {
		/* applies on the next respawn (OBSERVED: switch classes when you die) */
		userinfo.SetString(p, "csnz_wantclass", argv(1));
		sprint(p, PRINT_HIGH, sprintf("Class %s from your next respawn.\n", argv(1)));
		return true;
	}
	return false;
}

void
CSM_ItemKilled(entity p, entity inflictor, entity attacker, string weapon)
{
	p.cs_heldItem = 0;
	p.cs_stealthEnd = 0;
	if (cs_itemPoints && (attacker.flags & FL_CLIENT) && attacker != p && attacker.team != p.team) {
		/* kills count 2 points: CS_PlayerKilled added 1 */
		attacker.cs_score += 2;
		if (attacker.team == CS_TEAM1)
			cs_score1++;
		else
			cs_score2++;
	}
}

void
CSM_ZTEquip(entity p)
{
	string want = userinfo.GetString(p, "csnz_wantclass");
	if (want == "zombie")
		ZA_BecomeZombie(p, "csm_zombie");
	else
		ents.ChangeToClass(p, "csm_rifleman");
	ents.Input(p, "Alpha", "1", world);
}

void
CSM_ItemBattle(void)
{
	CSM_TeamDeathmatch(false);
	cs_modeTitle = "CSO_GameMode_TDM_Item";
	cs_modeFallback = "Team Deathmatch Item Battle";
	cs_noStats = true;	/* OBSERVED TDM_ITEM tip */
	M_Frame = CSM_ItemFrame;
	M_Command = CSM_ItemCommand;
	M_Killed = CSM_ItemKilled;
}

void
CSM_ZombieTeam(void)
{
	CSM_ItemBattle();
	cs_modeTitle = "CSO_GameMode_Zombi_Team";
	cs_modeFallback = "Zombie Team Deathmatch";
	cs_noStats = false;
	cs_itemPoints = true;
	cs_killLimitFallback = 600;	/* OBSERVED limitlist id 13 default */
	M_Equip = CSM_ZTEquip;
}

/* --- Original family -------------------------------------------------------- */

bool cs_partnerBots;
bool cs_fightYard;
float cs_extraGrenade;		/* LightOri: one more HE (OBSERVED tip) */

void
CSM_OriginalEquip(entity p)
{
	/* weapons carry over between rounds for the living (STOCK-CS); a fresh
	   spawn gets the knife and pistol */
	ents.ChangeToClass(p, cs_fightYard ? "csm_knifeonly" : "csm_basic");
	if (cs_extraGrenade)
		ents.Input(p, "GiveItem", "weapon_hegrenade", world);
	if (p.cs_money <= 0 && cs_round <= 1)
		CS_SetMoney(p, autocvar_mp_startmoney);
}

/* FightYard: OBSERVED "weapons on the ground will be spawned when the round
   starts". Positions: the map's armoury_entity points when present (they
   are CS's ground weapons), else around the spawn points (DESIGN). */
void
CSM_FightYardWeapons(void)
{
	string guns = "weapon_usp weapon_m3 weapon_m4a1 weapon_m249 weapon_hegrenade weapon_ak47";
	int n;
	float placed = 0;

	for (entity old = world; (old = find(old, ::classname, "csnz_groundgun"));)
		remove(old);

	for (entity a = world; (a = find(a, ::classname, "armoury_entity"));) {
		n = tokenize(guns);
		entity g = ents.Create(argv((int)floor(random() * n)), a.origin);
		if (g) {
			g.classname = "csnz_groundgun";
			placed++;
		}
	}

	if (placed == 0) {
		for (entity s = world; (s = find(s, ::classname, "info_player_deathmatch"));) {
			n = tokenize(guns);
			entity g2 = ents.Create(argv((int)floor(random() * n)), s.origin + [random(-64, 64), random(-64, 64), 16]);
			if (g2)
				placed++;
		}
		for (entity s2 = world; (s2 = find(s2, ::classname, "info_player_start"));) {
			n = tokenize(guns);
			entity g3 = ents.Create(argv((int)floor(random() * n)), s2.origin + [random(-64, 64), random(-64, 64), 16]);
			if (g3)
				placed++;
		}
	}
	printf("csnz %.1f: %g ground weapons\n", time, placed);
}

void
CSM_OriginalRoundStart(void)
{
	CS_MissionRoundStart();
	if (cs_fightYard)
		CSM_FightYardWeapons();
}

void
CSM_OriginalKilled(entity p, entity inflictor, entity attacker, string weapon)
{
	CS_DropBomb(p);
	p.cs_action = 0;
	if ((attacker.flags & FL_CLIENT) && attacker != p) {
		if (attacker.team != p.team)
			CS_GiveMoney(attacker, 300);
		else
			CS_GiveMoney(attacker, -3300);	/* STOCK-CS teamkill penalty */
	}
}

void
CSM_OriginalRoundEnded(float winner)
{
	CS_RoundMoney(winner);
	if (winner == CS_TEAM1)
		CS_Banner(CS_Text("CSO_Terrorists_Win", "Terrorists Win!"));
	else if (winner == CS_TEAM2)
		CS_Banner(CS_Text("CSO_CTs_Win", "Counter-Terrorists Win!"));
	else
		CS_Banner(CS_Text("CSO_Round_Draw", "Round Draw!"));
}

bool
CSM_OriginalCommand(entity p, string cmd)
{
	tokenize(cmd);
	if (argv(0) == "csnz_use") {
		CS_Use(p);
		return true;
	}
	return false;
}

/* bots carry out the mission when they happen to be in position */
void
CSM_OriginalFrame(void)
{
	for (entity b = world; (b = next.Player(b));) {
		if (clienttype(b) != CLIENTTYPE_BOT || !CS_Playing(b) || b.cs_action)
			continue;
		if (b.cs_hasC4 && CS_InBombSite(b))
			CS_Use(b);
		else if (b.team == CS_TEAM2 && cs_bombPlanted && vlen(b.origin - cs_bomb.origin) < 72)
			CS_Use(b);
	}
}

/* test hooks for the mission (one shot): cs_test_plant, cs_test_defuse,
   cs_test_rescue move the right player into place and press E for them */
void
CSM_OriginalTestHooks(void)
{
	if (cvars.GetBool("cs_test_plant")) {
		cvars.SetString("cs_test_plant", "0");
		entity site = find(world, ::classname, "info_bomb_target");
		if (!site)
			site = find(world, ::classname, "func_bomb_target");
		for (entity c = world; (c = next.Player(c));)
			if (c.cs_hasC4 && site) {
				vector at = site.absmin != site.absmax ? (site.absmin + site.absmax) * 0.5 : site.origin;
				setorigin(c, at + [0, 0, 36]);
				c.velocity = [0, 0, 0];
				CS_Use(c);
				printf("csnz %.1f: test hook: %s planting\n", time, c.netname);
				break;
			}
	}
	if (cvars.GetBool("cs_test_defuse") && cs_bombPlanted) {
		cvars.SetString("cs_test_defuse", "0");
		for (entity d = world; (d = next.Player(d));)
			if (CS_Playing(d) && d.team == CS_TEAM2) {
				setorigin(d, cs_bomb.origin + [32, 0, 36]);
				d.velocity = [0, 0, 0];
				CS_Use(d);
				break;
			}
	}
	if (cvars.GetBool("cs_test_vipescape")) {
		cvars.SetString("cs_test_vipescape", "0");
		entity zone = find(world, ::classname, "func_vip_safetyzone");
		for (entity v = world; (v = next.Player(v));)
			if (v.cs_isVip && zone) {
				vector zc = zone.absmin != zone.absmax ? (zone.absmin + zone.absmax) * 0.5 : zone.origin;
				setorigin(v, zc + [0, 0, 36]);
			}
	}
	if (cvars.GetBool("cs_test_tescape")) {
		cvars.SetString("cs_test_tescape", "0");
		entity ez = find(world, ::classname, "func_escapezone");
		if (ez)
			for (entity t = world; (t = next.Player(t));)
				if (CS_Playing(t) && t.team == CS_TEAM1)
					setorigin(t, (ez.absmin != ez.absmax ? (ez.absmin + ez.absmax) * 0.5 : ez.origin) + [0, 0, 36]);
	}
	if (cvars.GetBool("cs_test_rescue")) {
		cvars.SetString("cs_test_rescue", "0");
		entity ct = world;
		for (entity q = world; (q = next.Player(q));)
			if (CS_Playing(q) && q.team == CS_TEAM2)
				ct = q;
		if (ct)
			for (entity h = world; (h = find(h, ::classname, "csnz_hostage"));) {
				setorigin(ct, h.origin + [48, 0, 0]);
				CS_Use(ct);
				/* walk the hostage home: the rescue point, else the CT spawn */
				entity home = find(world, ::classname, "info_hostage_rescue");
				if (!home)
					home = find(world, ::classname, "info_player_counterterrorist");
				printf("csnz %.1f: test hook: hostage to %s %v\n", time, home ? home.classname : "nowhere", home.origin);
				if (home)
					setorigin(h, home.origin + [0, 0, 8]);
			}
	}
}

float
CSM_OriginalCheck(void)
{
	CSM_OriginalTestHooks();
	CSM_OriginalFrame();
	return CS_MissionCheck();
}

void
CSM_OriginalStart(void)
{
	cs_mission = CS_DetectMission();
	/* STOCK-CS round timer winners: bomb and escape maps go to the CT, hostage
	   and VIP maps to the T */
	cs_roundTimeWinsTeam2 = (cs_mission == MISSION_BOMB || cs_mission == MISSION_ESCAPE || cs_mission == MISSION_NONE);
	if (cs_mission == MISSION_NONE)
		cs_roundTimeWinsTeam2 = false;	/* OBSERVED ORIGINAL_DM: kill everyone, else a draw */
	serverinfo.SetString("csnz_mission", CS_MissionName(cs_mission));
	printf("csnz: mission %s\n", CS_MissionName(cs_mission));
	for (entity p = world; (p = next.Player(p));)
		CS_SetMoney(p, autocvar_mp_startmoney);
}

void
CSM_Original(float variant)
{
	cs_modeTitle = "CSO_GameMode_Original";
	cs_modeFallback = "Original";
	cs_teams = true;
	cs_rounds = true;
	cs_respawnDelay = -1;
	cs_scoreKills = false;
	cs_winLimitFallback = 13;	/* OBSERVED limitlist id 1 default */
	cs_timeLimitFallback = 20;	/* OBSERVED limitlist id 4 default */
	cs_roundTimeFallback = 3;	/* OBSERVED limitlist id 6 default */
	cs_team1Name = "TR";
	cs_team2Name = "CT";
	cs_freezeLock = true;
	M_Start = CSM_OriginalStart;
	M_Equip = CSM_OriginalEquip;
	M_RoundStart = CSM_OriginalRoundStart;
	M_CheckRound = CSM_OriginalCheck;
	M_Killed = CSM_OriginalKilled;
	M_RoundEnded = CSM_OriginalRoundEnded;
	M_Command = CSM_OriginalCommand;

	switch (variant) {
	case 3:		/* Bot: bots fill empty places (OBSERVED tooltip) */
		cs_modeTitle = "CSO_GameMode_Bot";
		cs_modeFallback = "Bot Original";
		cs_noStats = true;
		CSM_DefaultBots(9);
		break;
	case 6:
	case 7:		/* Official (tie break): OBSERVED "not recorded" */
		cs_modeTitle = variant == 6 ? "CSO_GameMode_Official" : "CSO_GameMode_Official_TieBreak";
		cs_modeFallback = "Official";
		cs_noStats = true;
		break;
	case 23:	/* LightOri (basic mode), OBSERVED tips basic_01..03 */
		cs_modeTitle = "CSO_GameMode_LightOri";
		cs_modeFallback = "Basic";
		cs_giveAllC4 = true;
		cs_giveAllKits = true;
		cs_extraGrenade = 1;
		cs_winLimitFallback = 9;	/* OBSERVED limitlist id 29 default */
		cs_roundTimeFallback = 3;	/* OBSERVED limitlist id 28 default */
		break;
	case 31:	/* FightYard: ground weapons, no buy (OBSERVED tips) */
		cs_modeTitle = "CSO_GameMode_FightYard";
		cs_modeFallback = "Fight Yard";
		cs_fightYard = true;
		cs_noBuy = true;
		break;
	case 42:	/* Season Original, OBSERVED tip: 16 rounds, first to 9,
			   1:45 rounds, minimum $800, no friendly fire */
		cs_modeTitle = "CSO_GameMode_SeasonOriginal";
		cs_modeFallback = "Ranked Original";
		cs_winLimitFallback = 9;
		cs_roundTimeFallback = 1.75;
		cvars.SetFloat("mp_winlimit", 9);
		cvars.SetFloat("mp_roundtime", 1.75);
		cvars.SetFloat("mp_maxrounds", 16);
		cvars.SetFloat("mp_friendlyfire", 0);
		break;
	case 50:	/* Partner: AI partners on the player's side (OBSERVED tooltip) */
		cs_modeTitle = "CSO_GameMode_partnermode";
		cs_modeFallback = "Partner";
		cs_partnerBots = true;
		cs_noStats = true;
		break;
	}
}

/* --- bazooka (21) ----------------------------------------------------------
   OBSERVED tip BK: the bazooka is given in the game; movement speed is
   increased; map objects can be destroyed; the HUD shows hit accuracy;
   assists; combos for continuous hits. Rounds: limitlist 24/25. */

.float cs_combo;
.float cs_lastHit;
.float cs_hits;
.float cs_shotsTaken;

void
CSM_BazookaEquip(entity p)
{
	ents.ChangeToClass(p, "csm_bazooka");
}

void
CSM_BazookaDamaged(entity victim, entity attacker)
{
	if (!(attacker.flags & FL_CLIENT) || attacker == victim)
		return;
	attacker.cs_hits++;
	attacker.cs_combo = (time - attacker.cs_lastHit < 3) ? attacker.cs_combo + 1 : 1;
	attacker.cs_lastHit = time;
	userinfo.SetInteger(attacker, "csnz_combo", (int)attacker.cs_combo);
	userinfo.SetInteger(attacker, "csnz_hits", (int)attacker.cs_hits);
}

void
CSM_BazookaKilled(entity p, entity inflictor, entity attacker, string weapon)
{
	CSM_BazookaDamaged(p, attacker);
}

float
CSM_ElimCheck(void)
{
	int a = CS_Alive(CS_TEAM1), b = CS_Alive(CS_TEAM2);
	if (a == 0 && b == 0)
		return 3;
	if (a == 0)
		return CS_TEAM2;
	if (b == 0)
		return CS_TEAM1;
	return 0;
}

void
CSM_Bazooka(void)
{
	CSM_Original(0);
	cs_modeTitle = "CSO_GameMode_Bazooka";
	cs_modeFallback = "Bazooka";
	cs_noBuy = true;
	cs_winLimitFallback = 3;	/* OBSERVED limitlist id 24 default */
	cs_roundTimeFallback = 10;	/* OBSERVED limitlist id 25 default */
	M_Start = __NULL__;
	M_RoundStart = __NULL__;
	M_Equip = CSM_BazookaEquip;
	M_CheckRound = CSM_ElimCheck;
	M_Damaged = CSM_BazookaDamaged;
	M_Killed = CSM_BazookaKilled;
	M_Command = __NULL__;
	M_RoundEnded = __NULL__;
}
