/* csnz_hooks: connects the progression, inventory, character ability,
 * zombie ability and achievement systems to the CSNZ core (csnz_common)
 * and the game callbacks. Shared by csnz.dat and studio.dat; included after
 * the modes so it can see every mode's globals.
 *
 * Original code (ISC). Award amounts and the achievement mode categories are
 * DESIGN, like the systems they feed. */

/* Nuclide fills these in (fields link by name across progs) */
.float dmg_lastHitBody;
.float stat_shotsFired;
.float stat_shotsHit;
#define CSNZ_BODY_HEAD 1	/* Nuclide bodyType_t BODY_HEAD */

.float ach_seenShotsFired;	/* stat_shots* already passed to ACH_OnShots */
.float ach_seenShotsHit;

bool csnz_zombiesThisMatch;	/* someone was infected: a zombie match */

/* --- connect, disconnect, commands -------------------------------------- */

/* every spawn; the first one loads the player's saved progression */
void
CSNZ_PlayerSpawned(entity p)
{
	VK_PlayerSpawned(p);
	if (p.prog_level <= 0) {
		INV_PlayerConnect(p);
		ACH_PlayerConnect(p);
	}
}

void
CSNZ_Disconnect(entity p)
{
	INV_PlayerDisconnect(p);
	ACH_PlayerDisconnect(p);
	VK_Disconnect(p);
}

/* the meta-game commands every rules progs understands */
bool
CSNZ_Command(entity p, string cmd)
{
	if (PROG_Command(p, cmd))
		return (true);
	if (INV_Command(p, cmd))
		return (true);
	if (ZA_Command(p, cmd))	/* zombies first, then human characters */
		return (true);
	if (CA_Command(p, cmd))
		return (true);
	if (BM_Command(p, cmd))
		return (true);
	if (ACH_Command(p, cmd))
		return (true);
	if (IB_Command(p, cmd))
		return (true);
	if (VK_Command(p, cmd))
		return (true);
	return (false);
}

/* --- spawn ---------------------------------------------------------------- */

/* CS_PutInGame, before the mode equips the player: a fresh body is human
   until the mode says otherwise */
void
CSNZ_PreEquip(entity p)
{
	p.za_isZombie = 0;
	p.za_type = ZA_NONE;
}

/* CS_PutInGame, after the mode equipped the player */
void
CSNZ_PostEquip(entity p)
{
	if (p.za_isZombie)
		csnz_zombiesThisMatch = true;
	else
		CA_ApplyLoadout(p);
}

/* --- damage and kills ----------------------------------------------------- */

/* a hit the victim survived; zv_lastHealth is their health before it */
void
CSNZ_Damaged(entity victim, entity attacker)
{
	float dealt = victim.zv_lastHealth - victim.health;

	if (dealt > 0 && is.Player(attacker) && attacker != victim) {
		/* character passives/actives scale the hit after the fact */
		float mod = CA_DamageModifier(attacker, victim);
		if (mod != 1.0) {
			float hp = victim.health - dealt * (mod - 1.0);
			hp = bound(1, hp, max(victim.health, victim.max_health));
			ents.Input(victim, "SetHealth", ftos(hp), world);
			dealt = victim.zv_lastHealth - victim.health;
		}
		ACH_OnDamageDealt(attacker, dealt);
		ACH_OnDamageTaken(victim, dealt);
	}

	ZA_Damaged(victim, attacker);
	victim.zv_lastHealth = victim.health;
}

void
CSNZ_Killed(entity p, entity inflictor, entity attacker, string weapon)
{
	bool byPlayer = is.Player(attacker) && attacker != p;

	PROG_OnKill(attacker, p);
	ZA_Killed(p, inflictor, attacker, weapon);
	ACH_OnDeath(p);

	if (!byPlayer)
		return;

	/* the killing blow: everything they had left */
	if (p.zv_lastHealth > 0)
		ACH_OnDamageDealt(attacker, p.zv_lastHealth);

	ACH_OnKill(attacker, p);
	CA_OnKill(attacker);

	if (p.dmg_lastHitBody == CSNZ_BODY_HEAD) {
		ACH_OnHeadshot(attacker);
		PROG_AwardXP(attacker, XP_HEADSHOT);
	}
	if (p.za_isZombie && !attacker.za_isZombie)
		ACH_OnZombieKill(attacker);
	else if (!p.za_isZombie && attacker.za_isZombie)
		PROG_AwardXP(attacker, XP_ZOMBIE_INFECT);
}

/* --- frame ---------------------------------------------------------------- */

void
CSNZ_Frame(void)
{
	CA_Frame();
	ZA_Frame();
	VK_Frame();

	for (entity p = world; (p = next.Player(p));) {
		float fired = p.stat_shotsFired - p.ach_seenShotsFired;
		float hit = p.stat_shotsHit - p.ach_seenShotsHit;
		if (fired > 0 || hit > 0) {
			ACH_OnShots(p, max(0, fired), max(0, hit));
			p.ach_seenShotsFired = p.stat_shotsFired;
			p.ach_seenShotsHit = p.stat_shotsHit;
		}
	}
}

/* --- rounds and matches --------------------------------------------------- */

void
CSNZ_RoundEnded(float winner)
{
	bool zombies = false;
	entity mvp = world;
	int winnersAlive = 0, winnersTotal = 0;

	for (entity q = world; (q = next.Player(q));) {
		if (q.za_isZombie)
			zombies = true;
		if (cs_teams && q.team == winner) {
			winnersTotal++;
			if (CS_Playing(q))
				winnersAlive++;
		}
	}
	if (zombies)
		csnz_zombiesThisMatch = true;

	PROG_OnRoundEnd(winner);

	for (entity p = world; (p = next.Player(p));) {
		if (CS_Playing(p)) {
			ACH_OnRoundSurvived(p);
			if (zombies && !p.za_isZombie)
				ACH_OnZombieRoundSurvived(p);
		}
		if (!cs_teams || p.team != winner)
			continue;
		/* the only one of a team of two or more still standing */
		if (CS_Playing(p) && winnersAlive == 1 && winnersTotal >= 2)
			ACH_OnLastStandWin(p);
		/* MVP: most kills this round on the winning side */
		if (p.ach_kills_round > 0 && (!mvp || p.ach_kills_round > mvp.ach_kills_round))
			mvp = p;
	}

	if (mvp) {
		ACH_OnMVP(mvp);
		PROG_AwardXP(mvp, XP_MVP);
	}
}

/* the achievement "mode" categories, 0-7 for the All-Star bitfield (DESIGN):
   0 other, 1 deathmatch, 2 team deathmatch, 3 bomb, 4 hostage, 5 zombie,
   6 points modes, 7 rescue/escort */
float
CSNZ_MatchCategory(void)
{
	if (cs_modeRun == 1 || cs_modeRun == 4)
		return 1;
	if (cs_modeRun == 2 || cs_modeRun == 5)
		return 2;
	if (cs_mission == MISSION_BOMB)
		return 3;
	if (cs_mission == MISSION_HOSTAGE)
		return 4;
	if (csnz_zombiesThisMatch)
		return 5;
	if (cs_scorePoints)
		return 6;
	if (cs_mission == MISSION_VIP || cs_mission == MISSION_ESCAPE)
		return 7;
	return 0;
}

/* winner is a team, or 0 with ffaWinner set in free for all */
void
CSNZ_MatchEnded(float winner, entity ffaWinner)
{
	float category = CSNZ_MatchCategory();

	if (cs_teams) {
		PROG_OnMatchEnd(winner);
	} else {
		/* free for all: only the best player wins (-1 matches no team) */
		PROG_OnMatchEnd(-1);
		if (ffaWinner) {
			ffaWinner.prog_wins_total++;
			PROG_AwardXP(ffaWinner, XP_MATCH_WIN - XP_MATCH_LOSE);
			PROG_Save(ffaWinner);
		}
	}

	for (entity p = world; (p = next.Player(p));) {
		bool won = cs_teams ? (winner != 0 && p.team == winner) : (p == ffaWinner);
		ACH_OnMatchEnd(p, category, won);	/* saves */
	}
	csnz_zombiesThisMatch = false;
}
