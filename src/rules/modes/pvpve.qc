/* pvpve (18): two teams with AI soldiers fight over bases.
 * Original code (ISC).
 *
 * OBSERVED tips PVPVE_01..08: reach the target team score first; held
 * bases give team points over time in proportion to their number; killing
 * enemy players (or Hector) also scores; a base is stolen by staying within
 * range, faster with more soldiers near it; returning to the start point
 * restores HP and ammo; respawn time grows with distance from the start
 * and shrinks with the bases held; levels, skills and classes exist.
 * OBSERVED limitlist id 18: target score 500 (500-1500).
 *
 * Bases: info_target "base" markers or Studio AREA blocks, else three
 * points between the team spawns (DESIGN). Rates, ranges and the respawn
 * formula are DESIGN; levels, skills, classes and Hector are not
 * implemented (BLOCKERS.md). Bots of each team stand in for the AI soldiers. */

#define PV_BASES 8
entity pv_base[PV_BASES];
float pv_baseCount;
.float pv_owner;		/* on a base: team holding it */
.float pv_progress;		/* -1 .. 1, sign is the capturing team */
float pv_nextTick;

void
PV_AddBase(vector at)
{
	if (pv_baseCount >= PV_BASES)
		return;
	entity b = spawn();
	b.classname = "csnz_base";
	setmodel(b, "models/w_backpack.mdl");
	setsize(b, [-16, -16, 0], [16, 16, 16]);
	setorigin(b, at);
	b.pv_owner = 0;
	b.pv_progress = 0;
	pv_base[pv_baseCount++] = b;
}

void
PV_Start(void)
{
	for (entity m = world; (m = find(m, ::targetname, "base"));)
		PV_AddBase(m.origin);
	for (entity a = world; (a = find(a, ::classname, "csnz_area"));)
		PV_AddBase(a.origin);
	if (!pv_baseCount) {
		vector t1 = ZV_TeamCentroid(CS_TEAM1), t2 = ZV_TeamCentroid(CS_TEAM2);
		for (float i = 1; i <= 3; i++)
			PV_AddBase(t1 + (t2 - t1) * (i / 4));
	}
	printf("csnz: %g bases\n", pv_baseCount);
}

void
PV_Frame(void)
{
	float held1 = 0, held2 = 0;

	for (int i = 0; i < pv_baseCount; i++) {
		entity b = pv_base[i];
		float n1 = 0, n2 = 0;
		for (entity p = world; (p = next.Player(p));) {
			if (!CS_Playing(p) || vlen(p.origin - b.origin) > 192)
				continue;
			if (p.team == CS_TEAM1)
				n1++;
			else if (p.team == CS_TEAM2)
				n2++;
		}
		/* more soldiers capture faster (OBSERVED); 10 s for one (DESIGN) */
		if (n1 != n2 && (n1 == 0 || n2 == 0)) {
			float dir = n1 > n2 ? -1 : 1;
			b.pv_progress = bound(-1, b.pv_progress + dir * max(n1, n2) * frametime / 10, 1);
			float now = b.pv_progress <= -1 ? CS_TEAM1 : (b.pv_progress >= 1 ? CS_TEAM2 : b.pv_owner);
			if (now != b.pv_owner) {
				b.pv_owner = now;
				b.colormod = now == CS_TEAM1 ? [1, 0.3, 0.2] : [0.3, 0.5, 1];
				CS_Banner(sprintf("%s took base %d", now == CS_TEAM1 ? cs_team1Name : cs_team2Name, i + 1));
			}
		}
		if (b.pv_owner == CS_TEAM1)
			held1++;
		else if (b.pv_owner == CS_TEAM2)
			held2++;
	}

	/* a point per held base every 2 s (DESIGN) */
	if (time >= pv_nextTick) {
		pv_nextTick = time + 2;
		cs_score1 += held1;
		cs_score2 += held2;
	}
	serverinfo.SetString("csnz_bases", sprintf("%g %g %g", pv_baseCount, held1, held2));

	/* the start point heals (OBSERVED) */
	for (entity q = world; (q = next.Player(q));) {
		if (!CS_Playing(q))
			continue;
		if (vlen(q.origin - ZV_TeamCentroid(q.team)) < 192 && q.health < q.max_health)
			ents.Input(q, "SetHealth", ftos(min(q.max_health, q.health + 20 * frametime)), world);
	}
}

/* respawn time: longer far from the start, shorter with bases (OBSERVED) */
void
PV_Killed(entity p, entity inflictor, entity attacker, string weapon)
{
	float held = 0;
	for (int i = 0; i < pv_baseCount; i++)
		if (pv_base[i].pv_owner == p.team)
			held++;
	float far = vlen(p.origin - ZV_TeamCentroid(p.team));
	cs_respawnDelay = max(2, 3 + far / 500 - held);
}

/* test hook: cs_test_capture <team> gives that team every base */
void
PV_TestHooks(void)
{
	float t = cvars.GetFloat("cs_test_capture");
	if (t != CS_TEAM1 && t != CS_TEAM2)
		return;
	cvars.SetString("cs_test_capture", "0");
	for (int i = 0; i < pv_baseCount; i++) {
		pv_base[i].pv_owner = t;
		pv_base[i].pv_progress = t == CS_TEAM1 ? -1 : 1;
	}
	printf("csnz %.1f: test hook: team %g holds every base\n", time, t);
}

void
PV_FrameAll(void)
{
	PV_TestHooks();
	PV_Frame();
}

void
CSM_PvPvE(void)
{
	CSM_TeamDeathmatch(false);
	cs_modeTitle = "CSO_GameMode_PVPVE";
	cs_modeFallback = "PvPvE";
	cs_killLimitFallback = 500;	/* OBSERVED limitlist id 18 default */
	M_Start = PV_Start;
	M_Frame = PV_FrameAll;
	M_Killed = PV_Killed;
	CSM_DefaultBots(8);
}
