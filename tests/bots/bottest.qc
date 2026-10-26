/* Headless acceptance tests for the bot framework (docs/spec/bots.md).
 * Run by tests/bots/run.sh: "bottest basic" or "bottest l4d" picks the
 * scenario. Prints TEST lines and finally PASS or FAIL. ISC licence. */

#define KIND_MOVETO	1
#define KIND_ROAMER	2
#define KIND_CHASER	3
#define KIND_SURVIVOR	4
#define KIND_HUNTER	5
#define KIND_TANK	6

float g_started, g_start, g_finished, g_next_tell, g_status_at;
string g_scenario;

/* basic */
float g_ledge_ok, g_stairs_ok, g_ledge_fail, g_stairs_fail, g_saw, g_caught;
float g_gap_ok, g_drop_ok;
/* l4d */
float g_pounces, g_punches, g_fled, g_best_pounce;

void test_line(string s) { print(strcat("TEST ", s, "\n")); }

class TestBot : BWBot {
	float m_kind;
	vector m_goal;
	entity m_pinned_by;
	float m_cooldown;
	float m_report;
	virtual BWAction InitialAction(void);
};

/* feet position of a waypoint */
vector wp_feet(float i) { return bw_snap_down(bw_wps[i].pos, world); }

/* --- shared actions ---------------------------------------------------------- */

/* prints a line once and then idles */
class TestReport : BWAction {
	float m_id;
	virtual string GetName(void) { return "Report"; };
	virtual float OnStart(BWAction prior)
	{
		float t = time - g_start;
		switch (m_id) {
		case 1: g_ledge_ok = t; test_line(sprintf("ledge MoveTo succeeded at %.1f s", t)); break;
		case 2: g_ledge_fail = t; test_line(sprintf("ledge MoveTo failed at %.1f s, at %v", t, m_bot.origin)); break;
		case 3: g_stairs_ok = t; test_line(sprintf("stairs MoveTo succeeded at %.1f s", t)); break;
		case 4: g_stairs_fail = t; test_line(sprintf("stairs MoveTo failed at %.1f s, at %v", t, m_bot.origin)); break;
		case 5: g_gap_ok = t; test_line(sprintf("gap jump MoveTo succeeded at %.1f s", t)); break;
		case 6: test_line(sprintf("gap jump MoveTo failed at %.1f s, at %v", t, m_bot.origin)); break;
		case 7: g_drop_ok = t; test_line(sprintf("drop down MoveTo succeeded at %.1f s", t)); break;
		case 8: test_line(sprintf("drop down MoveTo failed at %.1f s, at %v", t, m_bot.origin)); break;
		}
		m_bot.m_loco.Stop();
		return Continue();
	};
};

/* walks to random waypoints */
class TestRoam : BWAction {
	virtual string GetName(void) { return "Roam"; };
	virtual float Update(float interval)
	{
		float i = floor(random() * bw_wp_count);
		return SuspendFor(bw_moveto(wp_feet(i), __NULL__, __NULL__), sprintf("off to waypoint %g", i));
	};
};

/* follows a visible or known enemy with a chase path; otherwise wanders */
class TestChaser : BWAction {
	BWChasePath m_chase;
	BWPathFollower m_wander;
	float m_next_wander;
	virtual string GetName(void) { return "Chaser"; };
	virtual float OnStart(BWAction prior)
	{
		m_chase = spawn(BWChasePath, m_lead: 500);
		m_wander = spawn(BWPathFollower);
		Own(m_chase);
		Own(m_wander);
		return Continue();
	};
	virtual float Update(float interval)
	{
		BWKnown k = m_bot.m_vision.GetPrimaryKnownThreat(FALSE);
		if (k) {
			m_chase.Chase(m_bot, k.m_subject);
			m_bot.m_body.AimHeadTowardsEntity(k.m_subject, BWLOOK_IMPORTANT, 0.3, __NULL__, "chasing");
			if (!g_caught && vlen(k.m_subject.origin - m_bot.origin) < 48) {
				g_caught = time - g_start;
				test_line(sprintf("chaser caught the roamer at %.1f s", g_caught));
			}
			return Continue();
		}
		if (!m_wander.IsValid() || time > m_next_wander) {
			m_wander.Compute(m_bot, wp_feet(floor(random() * bw_wp_count)), 0);
			m_next_wander = time + 15;
		}
		m_wander.Follow(m_bot);
		return Continue();
	};
	virtual float OnSight(entity subject)
	{
		if (m_bot.IsEnemy(subject) && !g_saw) {
			g_saw = time - g_start;
			test_line(sprintf("chaser saw %s at %.1f s (sighting event)", ((BWBot)subject).BotName(), g_saw));
		}
		return BWR_CONTINUE;
	};
	virtual float OnStuck(void) { return TrySuspendFor(bw_backup(m_bot.origin + v_forward * 32), BWP_TRY, "stuck"); };
};

/* --- Left 4 Dead: survivors ------------------------------------------------------ */

class SurvivorFlee : BWAction {
	entity m_threat;
	BWPathFollower m_path;
	float m_next_repath;
	virtual string GetName(void) { return "Flee"; };
	virtual float OnStart(BWAction prior)
	{
		m_path = spawn(BWPathFollower);
		Own(m_path);
		g_fled++;
		test_line(sprintf("%s flees from %s, %.0f away", m_bot.BotName(), ((BWBot)m_threat).BotName(), vlen(m_threat.origin - m_bot.origin)));
		return Continue();
	};
	virtual float Update(float interval)
	{
		float i, best = -1, bd = -1, d;
		if (((TestBot)m_bot).m_pinned_by)
			return Done("pinned");
		if (wasfreed(m_threat) || m_threat.health <= 0 || vlen(m_threat.origin - m_bot.origin) > 600)
			return Done("safe");
		if (time >= m_next_repath) {
			/* run for the waypoint farthest from the threat */
			for (i = 0; i < bw_wp_count; i++) {
				d = vlen(bw_wps[i].pos - m_threat.origin);
				if (d > bd) {
					bd = d;
					best = i;
				}
			}
			m_path.Compute(m_bot, wp_feet(best), 0);
			m_next_repath = time + 1;
		}
		m_path.Follow(m_bot);
		return Continue();
	};
};

class SurvivorMain : BWAction {
	float m_next_shot;
	virtual string GetName(void) { return "Survivor"; };
	virtual float Update(float interval)
	{
		BWKnown k;
		entity t;
		float d;
		if (((TestBot)m_bot).m_pinned_by) {
			m_bot.m_loco.Stop();
			return Continue();	/* pinned: can't move or shoot */
		}
		k = m_bot.m_vision.GetPrimaryKnownThreat(TRUE);
		if (!k)
			return Continue();
		t = k.m_subject;
		d = vlen(t.origin - m_bot.origin);
		if (d < 350)
			return SuspendFor(spawn(SurvivorFlee, m_threat: t), "too close");
		if (d <= 800) {
			/* stand and shoot */
			m_bot.m_body.AimHeadTowardsEntity(t, BWLOOK_IMPORTANT, 0.5, __NULL__, "shooting");
			m_bot.m_loco.FaceTowards(t.origin);
			if (time >= m_next_shot && m_bot.m_body.IsHeadAimingOnTarget()) {
				m_next_shot = time + 0.5;
				bw_notify_weapon_fired(m_bot, m_bot);
				bw_damage(t, m_bot, m_bot, 4);
			}
		}
		return Continue();
	};
};

/* --- Left 4 Dead: Hunter ------------------------------------------------------------ */

class HunterPinned : BWAction {
	TestBot m_target;
	float m_next_hit;
	virtual string GetName(void) { return "Pinned"; };
	virtual float OnStart(BWAction prior)
	{
		m_target.m_pinned_by = m_bot;
		m_target.m_loco.Stop();
		m_bot.m_loco.Stop();
		m_next_hit = time;
		return Continue();
	};
	virtual float Update(float interval)
	{
		if (wasfreed(m_target) || !m_target.IsAlive())
			return Done("victim dead");
		m_bot.m_loco.Stop();
		m_bot.m_body.AimHeadTowardsEntity(m_target, BWLOOK_CRITICAL, 0.3, __NULL__, "pinning");
		if (time >= m_next_hit) {
			m_next_hit = time + 0.5;
			bw_damage(m_target, m_bot, m_bot, 8);
		}
		return Continue();
	};
	virtual void OnEnd(BWAction next)
	{
		if (!wasfreed(m_target) && m_target.m_pinned_by == m_bot)
			m_target.m_pinned_by = world;
	};
};

class HunterPounce : BWAction {
	TestBot m_target;
	float m_phase, m_until, m_touched, m_launch_dist;
	virtual string GetName(void) { return "Pounce"; };
	virtual float OnStart(BWAction prior)
	{
		m_bot.m_loco.Stop();
		m_bot.m_body.SetDesiredPosture(BWPOSTURE_CROUCH);
		m_bot.m_body.StartActivity(BWACT_CROUCH_IDLE, 0);
		m_until = time + 0.6;
		return Continue();
	};
	virtual float Update(float interval)
	{
		vector pred, v;
		float d, t, g = cvar("sv_gravity");
		if (wasfreed(m_target) || !m_target.IsAlive())
			return Done("target gone");
		switch (m_phase) {
		case 0:	/* crouch, then leap at where the target will be */
			m_bot.m_loco.Stop();
			m_bot.m_loco.FaceTowards(m_target.origin);
			m_bot.m_body.AimHeadTowardsEntity(m_target, BWLOOK_IMPORTANT, 0.3, __NULL__, "pounce");
			if (time < m_until)
				return Continue();
			d = bw_dist2d(m_bot.origin, m_target.origin);
			pred = m_target.origin + bw_flat(m_target.velocity) * (d / 700);
			t = max(0.1, bw_dist2d(m_bot.origin, pred) / 700);
			v = normalize(bw_flat(pred - m_bot.origin)) * 700;
			v_z = bound(100, ((pred_z - m_bot.origin_z) + 0.5 * g * t * t) / t, 450);
			m_launch_dist = d;
			m_bot.m_body.SetDesiredPosture(BWPOSTURE_STAND);
			m_bot.m_body.StartActivity(BWACT_JUMP, 0);
			m_bot.m_loco.Leap(v);
			m_phase = 1;
			m_until = time + 3;
			return Continue();
		case 1:	/* in the air */
			if (m_touched || (m_bot.m_loco.IsOnGround() && vlen(m_target.origin - m_bot.origin) < 48)) {
				g_pounces++;
				g_best_pounce = max(g_best_pounce, m_launch_dist);
				test_line(sprintf("hunter pounce landed on %s from %.0f units%s", m_target.BotName(), m_launch_dist, m_touched ? " (touch)" : ""));
				return ChangeTo(spawn(HunterPinned, m_target: m_target), "pinned");
			}
			if (m_bot.m_loco.IsOnGround() || time > m_until) {
				test_line(sprintf("hunter pounce missed (%.0f away)", vlen(m_target.origin - m_bot.origin)));
				m_phase = 2;
				m_until = time + 1.5;
			}
			return Continue();
		}
		/* missed: wait before trying again */
		if (time < m_until)
			return Continue();
		return Done("missed");
	};
	virtual float OnContact(entity touched)
	{
		if (m_phase == 1 && touched == m_target)
			m_touched = TRUE;
		return BWR_CONTINUE;
	};
	virtual void OnEnd(BWAction next) { m_bot.m_body.SetDesiredPosture(BWPOSTURE_STAND); };
};

class HunterMain : BWAction {
	BWChasePath m_chase;
	virtual string GetName(void) { return "HunterAttack"; };
	virtual float OnStart(BWAction prior)
	{
		m_chase = spawn(BWChasePath);
		Own(m_chase);
		return Continue();
	};
	virtual float Update(float interval)
	{
		BWKnown k = m_bot.m_vision.GetPrimaryKnownThreat(FALSE);
		entity t;
		if (!k) {
			m_bot.m_loco.Stop();
			return Continue();
		}
		t = k.m_subject;
		if (k.m_visible && vlen(t.origin - m_bot.origin) < 600 && m_bot.m_loco.IsOnGround() &&
		    !((TestBot)t).m_pinned_by)
			return SuspendFor(spawn(HunterPounce, m_target: t), "target in range");
		m_chase.Chase(m_bot, t);
		m_bot.m_body.AimHeadTowardsEntity(t, BWLOOK_IMPORTANT, 0.3, __NULL__, "hunting");
		return Continue();
	};
};

/* --- Left 4 Dead: Tank ---------------------------------------------------------------- */

class TankPunch : BWAction {
	TestBot m_target;
	float m_hit_at;
	virtual string GetName(void) { return "Punch"; };
	virtual float OnStart(BWAction prior)
	{
		m_hit_at = time + 0.4;
		m_bot.m_body.StartActivity(BWACT_ATTACK, 0);
		return Continue();
	};
	virtual float Update(float interval)
	{
		vector dir;
		if (wasfreed(m_target) || !m_target.IsAlive())
			return Done("target gone");
		m_bot.m_loco.Stop();
		m_bot.m_loco.FaceTowards(m_target.origin);
		if (time < m_hit_at)
			return Continue();
		((TestBot)m_bot).m_cooldown = time + 1.5;
		if (vlen(m_target.origin - m_bot.origin) > 100) {
			test_line("tank punch whiffed");
			return Done("out of reach");
		}
		dir = normalize(bw_flat(m_target.origin - m_bot.origin));
		g_punches++;
		test_line(sprintf("tank punched %s", m_target.BotName()));
		m_target.m_pinned_by = world;
		m_target.m_loco.Leap(dir * 450 + '0 0 300');
		bw_damage(m_target, m_bot, m_bot, 24);
		return Done("punched");
	};
};

class TankMain : BWAction {
	BWChasePath m_chase;
	virtual string GetName(void) { return "TankAttack"; };
	virtual float OnStart(BWAction prior)
	{
		m_chase = spawn(BWChasePath, m_lead: 500);
		Own(m_chase);
		return Continue();
	};
	virtual float Update(float interval)
	{
		BWKnown k = m_bot.m_vision.GetPrimaryKnownThreat(FALSE);
		entity t;
		if (!k) {
			m_bot.m_loco.Stop();
			return Continue();
		}
		t = k.m_subject;
		if (vlen(t.origin - m_bot.origin) < 70 && time >= ((TestBot)m_bot).m_cooldown)
			return SuspendFor(spawn(TankPunch, m_target: t), "in reach");
		m_chase.Chase(m_bot, t);
		m_bot.m_body.AimHeadTowardsEntity(t, BWLOOK_IMPORTANT, 0.3, __NULL__, "tank");
		return Continue();
	};
};

BWAction TestBot::InitialAction(void)
{
	switch (m_kind) {
	case KIND_MOVETO:
		return bw_moveto(m_goal, spawn(TestReport, m_id: m_report), spawn(TestReport, m_id: m_report + 1));
	case KIND_ROAMER: return spawn(TestRoam);
	case KIND_CHASER: return spawn(TestChaser);
	case KIND_SURVIVOR: return spawn(SurvivorMain);
	case KIND_HUNTER: return spawn(HunterMain);
	case KIND_TANK: return spawn(TankMain);
	}
	return __NULL__;
}

/* -1..1 */
float crandom(void) { return 2 * random() - 1; }

TestBot test_bot(string name, float kind, float tm, vector org, float yaw)
{
	TestBot b = spawn(TestBot, m_kind: kind, netname: name, team: tm);
	b.classname = "testbot";
	b.angles = [0, yaw, 0];
	b.origin = org;
	b.BotStart();
	return b;
}

/* --- scenarios ---------------------------------------------------------------------- */

void basic_start(void)
{
	TestBot b;
	b = test_bot("ledge", KIND_MOVETO, 2, '-900 900 0', 0);
	b.m_goal = '600 450 48';
	b.m_report = 1;
	b = test_bot("stairs", KIND_MOVETO, 2, '-900 -500 0', 0);
	b.m_goal = '-620 -940 64';
	b.m_report = 3;
	b = test_bot("gap", KIND_MOVETO, 2, '330 -850 64', 0);
	b.m_goal = '570 -850 64';
	b.m_report = 5;
	b = test_bot("drop", KIND_MOVETO, 2, '700 600 48', 90);
	b.m_goal = '700 900 0';
	b.m_report = 7;
	test_bot("roamer", KIND_ROAMER, 1, '700 -700 0', 90);
	test_bot("chaser", KIND_CHASER, 2, '900 900 0', 270);
}

void basic_check(void)
{
	float t = time - g_start, pass;
	if (!(g_ledge_ok && g_stairs_ok && g_saw && g_gap_ok && g_drop_ok) && t < 60)
		return;
	pass = (g_ledge_ok && g_ledge_ok <= 50 && g_stairs_ok && g_saw && g_gap_ok && g_drop_ok) ? TRUE : FALSE;
	test_line(sprintf("summary: ledge %s, stairs %s, sighting %s, catch %s",
	          g_ledge_ok ? sprintf("%.1f s", g_ledge_ok) : "no", g_stairs_ok ? sprintf("%.1f s", g_stairs_ok) : "no",
	          g_saw ? sprintf("%.1f s", g_saw) : "no", g_caught ? sprintf("%.1f s", g_caught) : "no (not required)"));
	test_line(sprintf("summary: gap jump %s, drop down %s", g_gap_ok ? sprintf("%.1f s", g_gap_ok) : "no",
	          g_drop_ok ? sprintf("%.1f s", g_drop_ok) : "no"));
	print(pass ? "PASS\n" : "FAIL\n");
	g_finished = TRUE;
}

entity g_surv[3];
TestBot g_hunter, g_tank;

void l4d_start(void)
{
	vector j = [crandom() * 40, crandom() * 40, 0];	/* each seed starts a little differently */
	g_surv[0] = test_bot("Bill", KIND_SURVIVOR, 1, '-700 600 0' + j, 270);
	g_surv[1] = test_bot("Zoey", KIND_SURVIVOR, 1, '-600 700 0' - j, 270);
	g_surv[2] = test_bot("Louis", KIND_SURVIVOR, 1, '-760 760 0' + [j_y, j_x, 0], 270);
	g_hunter = test_bot("Hunter", KIND_HUNTER, 3, '600 -700 0' + [j_y, -j_x, 0] * 2, 180);
	g_hunter.health = 250;
	g_hunter.m_loco.m_run_speed = 250;
	g_hunter.m_loco.Run();
	g_tank = test_bot("Tank", KIND_TANK, 3, '800 -850 0', 180);
	g_tank.health = 1500;
	g_tank.m_loco.m_run_speed = 210;
	g_tank.m_loco.m_accel = 800;
	g_tank.m_loco.Run();
}

void l4d_tick(void)
{
	float i, alive = 0, t = time - g_start, pass;
	/* the infected are told where the survivors are */
	if (time >= g_next_tell) {
		g_next_tell = time + 1;
		for (i = 0; i < 3; i++)
			if (((TestBot)g_surv[i]).IsAlive()) {
				if (g_hunter.IsAlive())
					g_hunter.m_vision.AddKnownEntity(g_surv[i]);
				if (g_tank.IsAlive())
					g_tank.m_vision.AddKnownEntity(g_surv[i]);
			}
	}
	for (i = 0; i < 3; i++)
		if (((TestBot)g_surv[i]).IsAlive())
			alive++;
	if (alive && (g_hunter.IsAlive() || g_tank.IsAlive()) && t < 120)
		return;
	if (!alive)
		test_line(sprintf("the infected won at %.1f s (Hunter %s, Tank %g hp)", t, g_hunter.IsAlive() ? "alive" : "dead", g_tank.health));
	else if (t >= 120)
		test_line(sprintf("time up with %g survivors alive", alive));
	else
		test_line(sprintf("the survivors won at %.1f s with %g alive", t, alive));
	test_line(sprintf("summary: %g pounces (longest %.0f units), %g punches, %g flights", g_pounces, g_best_pounce, g_punches, g_fled));
	pass = (g_pounces > 0 && g_punches > 0 && g_fled > 0) ? TRUE : FALSE;
	print(pass ? "PASS\n" : "FAIL\n");
	g_finished = TRUE;
}

/* --- game entry points ------------------------------------------------------------------ */

void worldspawn(void)
{
	bw_init();
	registercvar("bottest", "basic");
	registercvar("bottest_debug", "");
	registercvar("bottest_filter", "");
	registercvar("bottest_seed", "0");
}

void info_player_start(void) { }

float ConsoleCmd(string line) { return bw_console_command(line); }

void StartFrame(void)
{
	if (g_finished)
		return;
	if (!g_started) {
		if (time < 1.5)
			return;
		g_started = TRUE;
		g_start = time;
		g_scenario = cvar_string("bottest");
		/* vary the run: skip this many random numbers */
		for (float i = cvar("bottest_seed") * 7; i > 0; i--)
			random();
		test_line(sprintf("scenario %s on %s", g_scenario, mapname));
		/* debug settings for a run: +set bottest_debug "path vision" */
		if (cvar_string("bottest_debug") != "")
			bw_console_command(strcat("bot_debug ", cvar_string("bottest_debug")));
		if (cvar_string("bottest_filter") != "")
			bw_console_command(strcat("bot_debug_filter ", cvar_string("bottest_filter")));
		if (!bw_wp_load()) {
			print("FAIL\n");
			g_finished = TRUE;
			return;
		}
		if (g_scenario == "l4d")
			l4d_start();
		else
			basic_start();
		g_status_at = time + 10;
		return;
	}
	if (time >= g_status_at) {
		g_status_at = time + 10;
		bw_status();
	}
	if (g_scenario == "l4d")
		l4d_tick();
	else
		basic_check();
	if (g_finished)
		localcmd("quit\n");
}

/* unused engine entry points (no players in these tests) */
void main(void) { }
void PlayerPreThink(void) { }
void PlayerPostThink(void) { }
void ClientKill(void) { }
void ClientConnect(void) { }
void PutClientInServer(void) { }
void ClientDisconnect(void) { }
void SetNewParms(void) { }
void SetChangeParms(void) { }
