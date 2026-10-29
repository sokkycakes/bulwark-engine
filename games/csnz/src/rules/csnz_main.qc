/* csnz_main: the Nuclide rule callbacks of csnz.dat (registry: modes/registry.qc).
 * Original code (ISC).
 *
 * csnz_mode (the CSO mode id, as in resource/gamemodelist.csv) picks the
 * rules. Ids routed to other progs (INTERFACES.md section 2): 8 -> zombi.dat,
 * 38/39/49/52/53 -> studio.dat (mp.dll: VoxelCreate, VoxelPVE,
 * VoxelPropHunt, VoxelShelter, VoxelScenarioTX). An id without rules here falls back to
 * Deathmatch and says so. MODE_INVENTORY.md lists every id's disposition. */

/* --- callbacks ------------------------------------------------------------- */

void
CodeCallback_Precache(void)
{
	precache_sound("events/lvup.wav");
	precache_sound("studio/scenariotx/levelup.wav");
	/* player classes */
	precache.Entity("csm_rifleman");
	precache.Entity("csm_basic");
	precache.Entity("csm_knifeonly");
	precache.Entity("csm_heavy");
	precache.Entity("csm_scout");
	precache.Entity("csm_sniper");
	precache.Entity("csm_brawler");
	precache.Entity("csm_vip");
	precache.Entity("csm_bazooka");
	precache.Entity("csm_zombie");
	precache.Entity("csm_zombie_origin");
	precache.Entity("csm_hidden");
	precache.Entity("csm_giant");
	precache.Entity("csm_hero");
	precache.Entity("weapon_csm_giantclaw");

	/* zombie host types */
	precache.Entity("csm_zombie_light");
	precache.Entity("csm_zombie_heavy");
	precache.Entity("csm_zombie_psycho");
	precache.Entity("csm_zombie_voodoo");
	precache.Entity("csm_zombie_deimos");
	precache.Entity("csm_zombie_ganymede");
	precache.Entity("csm_zombie_stamper");
	precache.Entity("csm_zombie_banshee");
	precache.Entity("csm_zombie_venomguard");
	precache.Entity("csm_zombie_stingfinger");
	precache.Entity("csm_zombie_metatron");
	precache.Entity("csm_zombie_lilith");
	precache.Entity("csm_zombie_chaser");
	precache.Entity("csm_zombie_blotter");
	precache.Entity("csm_zombie_rustywing");
	precache.Entity("csm_zombie_aksha");
	precache.Entity("csm_zombie_soldier");

	CS_PrecacheWeapons();
	precache_model("models/w_backpack.mdl");
	precache_model("models/hostage.mdl");
}

string
CodeCallback_Title(void)
{
	return ("csnz");
}

void
CodeCallback_StartGameType(void)
{
	cs_modeRun = autocvar_csnz_mode;

	if (cs_modeRun == 8 || cs_modeRun == 38 || cs_modeRun == 39 || cs_modeRun == 49)
		printf("csnz: mode %g belongs to %s.dat (g_gametype); running Deathmatch here\n",
			cs_modeRun, cs_modeRun == 8 ? "zombi" : "studio");
	/* the registry has rules for 52/53 (studio.dat uses them), so they run,
	   but without the Studio map layer */
	if (cs_modeRun == 52 || cs_modeRun == 53)
		printf("csnz: mode %g is a Studio mode (g_gametype studio); running its rules without a Studio map\n", cs_modeRun);

	if (!CS_SelectMode(cs_modeRun)) {
		printf("csnz: mode %g has no rules (see MODE_INVENTORY.md); falling back to Deathmatch\n", cs_modeRun);
		cs_modeRun = 1;
		CS_SelectMode(1);
	}

	CS_Init();
	VX_Boot([128, 128, 64]);
	CS_ApplyLimits(cs_modeRun);
	serverinfo.SetInteger("csnz_moderun", (int)cs_modeRun);
	printf("csnz: mode %g (%s), %s, %s\n", cs_modeRun, CS_Text(cs_modeTitle, cs_modeFallback),
		cs_teams ? "teams" : "free for all", cs_rounds ? "rounds" : "continuous");

	if (M_Start)
		M_Start();
}

void
CodeCallback_FrameStart(void)
{
	CS_Frame();
	CSNZ_Frame();
}

void
CodeCallback_PlayerSpawn(entity p)
{
	p.cs_kills = p.cs_deaths = p.cs_score = 0;
	if (p.cs_money <= 0)
		CS_SetMoney(p, autocvar_mp_startmoney);

	CSNZ_PlayerSpawned(p);

	/* round modes: a late joiner watches until the next round (STOCK-CS) */
	if (cs_rounds && cs_state == CS_LIVE && cs_respawnDelay < 0) {
		if (M_LateJoin)
			if (M_LateJoin(p))
				return;
		if (cs_teams)
			CS_SetTeam(p, M_PickTeam ? M_PickTeam(p) : CS_AutoTeam(p));
		ents.ChangeToClass(p, "spectator");
		return;
	}
	CS_PutInGame(p);
}

bool
CodeCallback_PlayerRequestRespawn(entity p)
{
	if (cs_state == CS_MATCHEND)
		return (false);
	if (cs_rounds && cs_respawnDelay < 0 && cs_state != CS_WAIT)
		return (false);
	if (p.cs_respawnAt > time)
		return (false);

	CS_PutInGame(p);
	return (true);
}

void
CodeCallback_PlayerDamage(entity victim, entity attacker, entity damageDecl)
{
	CSNZ_Damaged(victim, attacker);
	if (M_Damaged)
		M_Damaged(victim, attacker);
}

void
CodeCallback_PlayerKilled(entity p, entity inflictor, entity attacker, string weapon)
{
	CS_PlayerKilled(p, inflictor, attacker, weapon);
	CSNZ_Killed(p, inflictor, attacker, weapon);
}

/* Nuclide's Monster.qc calls NPCDeath(npc, inflictor, attacker) while the
   delegate names them (npc, attacker, inflictor); take whichever is a player */
void
CodeCallback_NPCKilled(entity npc, entity a, entity b)
{
	entity credit = (b.flags & FL_CLIENT) ? b : a;
	if (M_NPCKilled)
		M_NPCKilled(npc, credit);
}

void
CodeCallback_PlayerDisconnect(entity p)
{
	if (M_Disconnect)
		M_Disconnect(p);
	CS_DropBomb(p);
	CSNZ_Disconnect(p);
}

/* commands every mode understands; the mode sees the rest first */
bool
CodeCallback_ClientCommand(entity p, string cmd)
{
	if (M_Command)
		if (M_Command(p, cmd))
			return (true);

	/* meta-game system commands */
	if (CSNZ_Command(p, cmd))
		return (true);

	tokenize(cmd);
	switch (argv(0)) {
	case "buy":
		CS_Buy(p, argv(1));
		return (true);
	case "jointeam":
	case "csnz_team":
		/* switching sides kills in round modes (STOCK-CS). jointeam is
		   the team menu's command (CSO_TeamMenu.res): 1, 2, 5 auto;
		   3 (VIP) and 6 (spectate) aren't offered by the HUD */
		float t = stof(argv(1));
		if (t == 5 && cs_teams)
			t = CS_AutoTeam(p);
		if (!cs_teams || (t != CS_TEAM1 && t != CS_TEAM2) || t == p.team)
			return (true);
		if (CS_Playing(p) && cs_state == CS_LIVE) {
			p.takedamage = DAMAGE_YES;
			ents.Input(p, "Kill", "", world);
		}
		CS_SetTeam(p, t);
		return (true);
	case "csnz_vip":
		userinfo.SetBool(p, "csnz_wantvip", true);
		sprint(p, PRINT_HIGH, "You will be the VIP next round if you are a CT.\n");
		return (true);
	case "csnz_stats":
		CS_StatsPrint(p, argv(1) == "" ? p.netname : argv(1));
		return (true);
	case "csnz_use":
		return (true);	/* nothing to use in this mode */
	}
	return (false);
}

/* "sv <cmd>" on the server console */
bool
CodeCallback_ConsoleCommand(entity p, string cmd)
{
	if (M_Command)
		if (M_Command(world, cmd))
			return (true);

	tokenize(cmd);
	switch (argv(0)) {
	case "csnz_status":
		printf("csnz: mode %g state %s round %i score %g:%g goal %g players %i\n", cs_modeRun,
			CS_StateName(), cs_round, cs_score1, cs_score2, cs_goal, CS_Players());
		for (entity q = world; (q = next.Player(q));)
			printf("csnz:  %s team %g hp %g kills %g money %g score %g at %v\n", q.netname, q.team,
				q.health, q.cs_kills, q.cs_money, q.cs_score, q.origin);
		return (true);
	case "csnz_stats":
		CS_StatsPrint(world, argv(1));
		return (true);
	}
	return (false);
}
