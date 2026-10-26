/* Player-bot tests. "bottest pnav" sends fake clients over the ledge, the
 * stairs and a drop with the engine's player physics. "bottest tf" is a
 * small TF2-style game: two teams of five classes (Scout, Soldier, Heavy,
 * Sniper, Medic) play King of the Hill on one control point. The bots only
 * send player input; the weapons, health and the point are game code here.
 * ISC licence, see LICENSE. */

#define TF_RED	1
#define TF_BLU	2

#define TFC_SCOUT	1
#define TFC_SOLDIER	2
#define TFC_HEAVY	3
#define TFC_SNIPER	4
#define TFC_MEDIC	5

#define TF_POINT	'0 -650 0'
#define TF_POINT_RADIUS	160
#define TF_UNLOCK	10	/* seconds after the start */
#define TF_CAP_TIME	6	/* one capper */
#define TF_HOLD_TO_WIN	60
#define TF_RESPAWN	5
#define TF_TIME_LIMIT	300

.float tf_die;		/* rockets: when they burn out */
.float tf_amount;	/* health packs: fraction of max health */
.float tf_back;		/* health packs: when they come back; 0 when there */

/* --- the game state ---------------------------------------------------------- */

float g_owner, g_cap_team, g_cap_progress, g_unlock_at, g_caps, g_winner, g_locked_told;
float g_hold[3];
float g_kills, g_healed, g_ubers, g_headshots, g_respawns, g_long_stuck, g_pack_uses, g_airshots;
float g_kills_by[6];
float g_snipes, g_snipe_hits;
entity g_players[16];
float g_nplayers;
entity g_packs[4];
float g_npacks;
float g_next_watch;

string tf_team_name(float tm) { return (tm == TF_RED) ? "RED" : (tm == TF_BLU) ? "BLU" : "nobody"; }

string tf_class_name(float c)
{
	switch (c) {
	case TFC_SCOUT: return "Scout";
	case TFC_SOLDIER: return "Soldier";
	case TFC_HEAVY: return "Heavy";
	case TFC_SNIPER: return "Sniper";
	case TFC_MEDIC: return "Medic";
	}
	return "?";
}

float tf_class_health(float c)
{
	switch (c) {
	case TFC_SCOUT: return 125;
	case TFC_SOLDIER: return 200;
	case TFC_HEAVY: return 300;
	case TFC_SNIPER: return 125;
	case TFC_MEDIC: return 150;
	}
	return 100;
}

float tf_class_speed(float c)
{
	switch (c) {
	case TFC_SCOUT: return 400;
	case TFC_SOLDIER: return 240;
	case TFC_HEAVY: return 230;
	case TFC_SNIPER: return 300;
	case TFC_MEDIC: return 320;
	}
	return 300;
}

class TFBot : BWBot {
	float m_class;
	float m_next_attack;
	float m_spun;		/* Heavy: when the minigun is up to speed; 0 when idle */
	float m_zoomed, m_zoom_since;
	float m_uber, m_uber_until;
	entity m_patient;	/* Medic: who the beam is on */
	entity m_last_attacker;
	float m_last_hurt;
	float m_respawn_at;
	float m_prev_buttons;
	float m_stuck_told;
	virtual BWAction InitialAction(void);
};

float tf_is_player(entity e) { return (e.flags & FL_CLIENT) ? TRUE : FALSE; }

/* -1..1 */
float tf_crandom(void) { return 2 * random() - 1; }

/* a medic has its beam on p */
float tf_being_healed(entity p)
{
	float i;
	for (i = 0; i < g_nplayers; i++)
		if (((TFBot)g_players[i]).m_patient == p)
			return TRUE;
	return FALSE;
}

/* --- damage and death ---------------------------------------------------------- */

void tf_kill(TFBot v, entity attacker, entity inflictor, string how)
{
	TFBot a = (TFBot)attacker;
	v.health = 0;
	v.solid = SOLID_NOT;
	v.takedamage = DAMAGE_NO;
	v.velocity = '0 0 0';
	v.m_patient = world;
	v.m_zoomed = FALSE;
	v.m_spun = 0;
	v.m_respawn_at = time + TF_RESPAWN;
	if (attacker != v && tf_is_player(attacker)) {
		g_kills++;
		g_kills_by[a.m_class]++;
		test_line(sprintf("%s killed %s (%s) at %.1f s", a.netname, v.netname, how, time - g_start));
	} else {
		test_line(sprintf("%s died (%s) at %.1f s", v.netname, how, time - g_start));
	}
	bw_notify_killed(v, inflictor, attacker, 0);
}

void tf_damage(entity victim, entity attacker, entity inflictor, float dmg, string how)
{
	TFBot v = (TFBot)victim;
	if (!tf_is_player(victim))
		return;
	if (!v.m_class)
		return;
	if (v.health <= 0 || time < v.m_uber_until)
		return;
	if (attacker != victim && victim.team == attacker.team)
		return;
	v.health -= dmg;
	v.m_last_hurt = time;
	if (attacker != victim)
		v.m_last_attacker = attacker;
	bw_notify_injured(victim, inflictor, attacker, dmg);
	if (v.health <= 0)
		tf_kill(v, attacker, inflictor, how);
}

/* --- weapons ------------------------------------------------------------------------ */

/* hitscan pellets with a spread in degrees; damage ramps from 150% up close
 * through 100% at 512 units to 50% at 1024 and beyond */
void tf_fire_bullets(TFBot p, float n, float dmg, float spread, string how)
{
	vector eye = p.origin + p.view_ofs, fwd, rgt, up, dir;
	float i, d, k = tan(spread * (M_PI / 180)), amt;
	entity e;
	makevectors(p.v_angle);
	fwd = v_forward;
	rgt = v_right;
	up = v_up;
	bw_notify_weapon_fired(p, p);
	for (i = 0; i < n; i++) {
		dir = normalize(fwd + rgt * tf_crandom() * k + up * tf_crandom() * k);
		traceline(eye, eye + dir * 4096, MOVE_NORMAL, p);
		e = trace_ent;
		if (trace_fraction == 1 || !tf_is_player(e))
			continue;
		d = vlen(trace_endpos - eye);
		amt = dmg * bound(0.5, 1.5 - d / 1024, 1.5);
		tf_damage(e, p, p, amt, how);
	}
}

void tf_sniper_shot(TFBot p)
{
	vector eye = p.origin + p.view_ofs, fwd;
	float charge = 0, dmg;
	entity e;
	string how = "sniper rifle";
	if (p.m_zoomed)
		charge = bound(0, (time - p.m_zoom_since) / 3.3, 1);
	dmg = 50 + 100 * charge;
	g_snipes++;
	makevectors(p.v_angle);
	fwd = v_forward;	/* the notifications below aim other bots' views */
	bw_notify_weapon_fired(p, p);
	traceline(eye, eye + fwd * 8192, MOVE_NORMAL, p);
	e = trace_ent;
	if (trace_fraction < 1 && tf_is_player(e) && e.health > 0) {
		if (p.m_zoomed && trace_endpos_z > e.origin_z + 58) {
			dmg *= 3;
			how = "headshot";
			g_headshots++;
		}
		g_snipe_hits++;
		tf_damage(e, p, p, dmg, how);
	}
	/* the charge starts again after the bolt */
	p.m_zoom_since = time + 1.5;
}

void tf_explode(entity r, vector pos)
{
	entity e;
	vector c;
	float d, dmg;
	for (e = findflags(world, flags, FL_CLIENT); e; e = findflags(e, flags, FL_CLIENT)) {
		if (e.health <= 0)
			continue;
		c = e.origin + (e.mins + e.maxs) * 0.5;
		d = vlen(c - pos);
		if (d > 146)
			continue;
		traceline(pos, c, MOVE_NOMONSTERS, r);
		if (trace_fraction < 1)
			continue;
		dmg = 90 * (1 - 0.5 * d / 146);
		if (e == r.owner)
			dmg *= 0.6;
		else if (e.team != r.owner.team && !(e.flags & FL_ONGROUND) && d < 64)
			g_airshots++;
		/* knocked about: rockets throw people */
		e.velocity += normalize(c - pos + '0 0 16') * dmg * 6;
		if (e.velocity_z > 100)
			e.flags &= ~FL_ONGROUND;
		tf_damage(e, r.owner, r, dmg, "rocket");
	}
	bw_notify_sound(r, pos, 1500);
}

void tf_rocket_fly(void)
{
	entity r = self;
	vector nxt = r.origin + r.velocity * frametime;
	traceline(r.origin, nxt, MOVE_NORMAL, r.owner);
	if (trace_fraction < 1 || time > r.tf_die) {
		tf_explode(r, trace_endpos - normalize(r.velocity) * 2);
		r.customphysics = __NULL__;
		remove(r);
		return;
	}
	setorigin(r, nxt);
}

void tf_fire_rocket(TFBot p)
{
	entity r = spawn();
	makevectors(p.v_angle);
	r.classname = "rocket";
	r.owner = p;
	r.solid = SOLID_NOT;
	r.movetype = MOVETYPE_NONE;
	r.velocity = v_forward * 1100;
	r.tf_die = time + 5;
	setorigin(r, p.origin + p.view_ofs - '0 0 8');
	r.customphysics = tf_rocket_fly;
	bw_notify_weapon_fired(p, r);
}

/* the beam locks on to a teammate in front within 450 units, and holds to 540 */
void tf_medigun(TFBot p, float b, float dt)
{
	entity e, best = world;
	vector eye = p.origin + p.view_ofs, d;
	float i, bd = 0, amt, mx;
	TFBot pt;
	if (!(b & BWIN_ATTACK)) {
		p.m_patient = world;
		return;
	}
	if (p.m_patient) {
		e = p.m_patient;
		traceline(eye, e.origin + e.view_ofs, MOVE_NOMONSTERS, p);
		if (e.health <= 0 || vlen(e.origin - p.origin) > 540 || trace_fraction < 1)
			p.m_patient = world;
	}
	if (!p.m_patient) {
		makevectors(p.v_angle);
		for (i = 0; i < g_nplayers; i++) {
			e = g_players[i];
			if (e == p || e.team != p.team || e.health <= 0)
				continue;
			d = e.origin + '0 0 36' - eye;
			if (vlen(d) > 450)
				continue;
			if (normalize(d) * v_forward < bd || normalize(d) * v_forward < 0.96)
				continue;
			traceline(eye, e.origin + e.view_ofs, MOVE_NOMONSTERS, p);
			if (trace_fraction < 1)
				continue;
			bd = normalize(d) * v_forward;
			best = e;
		}
		p.m_patient = best;
	}
	if (!p.m_patient)
		return;
	pt = (TFBot)p.m_patient;
	/* faster on someone who hasn't been hurt for a while; overheal to 150% */
	amt = ((time - pt.m_last_hurt > 10) ? 72 : 24) * dt;
	mx = pt.max_health * 1.5;
	if (pt.health < mx) {
		amt = min(amt, mx - pt.health);
		pt.health += amt;
		g_healed += amt;
	}
	p.m_uber = min(1, p.m_uber + dt / 20);
	if ((b & BWIN_ATTACK2) && p.m_uber >= 1) {
		p.m_uber = 0;
		p.m_uber_until = time + 8;
		pt.m_uber_until = time + 8;
		g_ubers++;
		test_line(sprintf("%s ubers %s at %.1f s", p.netname, pt.netname, time - g_start));
	}
}

/* runs the weapon of a player for one input frame */
void tf_weapon_frame(TFBot p, float b, float dt)
{
	float pressed = b & ~p.m_prev_buttons;
	p.m_prev_buttons = b;
	if (p.health <= 0)
		return;
	if (time < p.m_uber_until && p.m_class != TFC_MEDIC)
		p.m_uber = 0;
	switch (p.m_class) {
	case TFC_SCOUT:
		if ((b & BWIN_ATTACK) && time >= p.m_next_attack) {
			tf_fire_bullets(p, 10, 6, 5, "scattergun");
			p.m_next_attack = time + 0.625;
		}
		break;
	case TFC_SOLDIER:
		if ((b & BWIN_ATTACK) && time >= p.m_next_attack) {
			tf_fire_rocket(p);
			p.m_next_attack = time + 0.8;
		}
		break;
	case TFC_HEAVY:
		if (b & (BWIN_ATTACK | BWIN_ATTACK2)) {
			if (!p.m_spun)
				p.m_spun = time + 0.87;
			p.maxspeed = 110;
			if ((b & BWIN_ATTACK) && time >= p.m_spun && time >= p.m_next_attack) {
				tf_fire_bullets(p, 4, 9, 4, "minigun");
				p.m_next_attack = time + 0.105;
			}
		} else {
			p.m_spun = 0;
			p.maxspeed = tf_class_speed(TFC_HEAVY);
		}
		break;
	case TFC_SNIPER:
		if (pressed & BWIN_ATTACK2) {
			p.m_zoomed = !p.m_zoomed;
			p.m_zoom_since = time;
		}
		p.maxspeed = p.m_zoomed ? 80 : tf_class_speed(TFC_SNIPER);
		if ((b & BWIN_ATTACK) && time >= p.m_next_attack) {
			tf_sniper_shot(p);
			p.m_next_attack = time + 1.5;
		}
		break;
	case TFC_MEDIC:
		tf_medigun(p, b, dt);
		break;
	}
}

/* --- spawning ------------------------------------------------------------------------ */

/* a standing player's box, origin at the feet (the bots' convention) */
void tf_place(entity p, vector org, float yaw)
{
	p.classname = "player";
	p.solid = SOLID_SLIDEBOX;
	p.movetype = MOVETYPE_WALK;
	p.flags = FL_CLIENT | FL_JUMPRELEASED;
	p.takedamage = DAMAGE_AIM;
	p.velocity = '0 0 0';
	p.view_ofs = '0 0 64';
	p.angles = [0, yaw, 0];
	p.v_angle = p.angles;
	/* as in TF2, teammates pass through each other and their own team's
	 * shots: each team is solid in its own dimension bit and hits the other */
	p.dimension_solid = (p.team == TF_RED) ? 1 : 2;
	p.dimension_hit = (p.team == TF_RED) ? 2 : 1;
	setsize(p, '-16 -16 0', '16 16 72');
	setorigin(p, org + '0 0 1');
}

void tf_spawn(TFBot b)
{
	vector base = (b.team == TF_RED) ? '-850 850 0' : '850 850 0';
	float n = num_for_edict(b);
	tf_place(b, base + [(n % 3 - 1) * 60, -(floor(n / 3) % 3) * 60, 0], 270);
	b.health = b.max_health = tf_class_health(b.m_class);
	b.maxspeed = tf_class_speed(b.m_class);
	b.m_zoomed = FALSE;
	b.m_spun = 0;
	b.m_patient = world;
	b.m_uber = 0;
	b.m_uber_until = 0;
	b.m_last_attacker = world;
	b.m_last_hurt = 0;
}

/* aiming skill by class and the tf_bot_skill cvar (0 easy, 1 normal, 2
 * hard, 3 expert): degrees of error, how quickly aim catches up, and how
 * long it takes to react to someone coming into view */
void tf_skill(TFBot b)
{
	float sk = bound(0, cvar("tf_bot_skill"), 3), err = 2.5 - 0.5 * sk;
	b.m_body.m_aim_error = (b.m_class == TFC_SNIPER) ? err * 0.4 : err * 1.5;
	b.m_body.m_track_rate = 5 + 2 * sk;
	b.m_body.m_head_rate = 300 + 100 * sk;
	b.m_vision.m_reaction = ((b.m_class == TFC_SNIPER) ? 0.25 : 0.1) + 0.3 - 0.1 * sk;
	b.m_loco.m_run_speed = tf_class_speed(b.m_class);
	b.m_loco.m_walk_speed = b.m_loco.m_run_speed * 0.5;
	b.m_loco.Run();
}

TFBot tf_new_bot(float cls, float tm)
{
	entity os = self;
	TFBot b;
	self = spawnclient();
	if (!self) {
		self = os;
		return __NULL__;
	}
	spawnfunc_TFBot();
	b = (TFBot)self;
	self = os;
	b.m_class = cls;
	b.team = tm;
	b.netname = sprintf("%s %s", tf_team_name(tm), tf_class_name(cls));
	forceinfokey(b, "name", b.netname);
	tf_spawn(b);
	b.BotStart();
	tf_skill(b);
	g_players[g_nplayers++] = b;
	return b;
}

void tf_pack(vector org, float amount)
{
	entity e = spawn();
	e.classname = "item_healthkit";
	e.tf_amount = amount;
	setorigin(e, org);
	g_packs[g_npacks++] = e;
}

/* --- behaviour ------------------------------------------------------------------------ */

/* the spot a class wants to shoot at: heads for the Sniper, the feet ahead
 * of a walking target for rockets, the chest otherwise */
vector tf_aim_point(TFBot b, entity t)
{
	float d = vlen(t.origin - b.origin);
	switch (b.m_class) {
	case TFC_SNIPER:
		return t.origin + [0, 0, t.maxs_z - 6];
	case TFC_SOLDIER:
		if (t.flags & FL_ONGROUND)
			return t.origin + '0 0 4' + bw_flat(t.velocity) * (d / 1100);
		return t.origin + '0 0 36' + t.velocity * (d / 1100);
	}
	return t.origin + '0 0 40';
}

/* the bot fires when its crosshair is on where it believes the target is
 * (its aim error included), with nothing solid in the way */
float tf_on_target(TFBot b, vector p, float radius)
{
	vector aim = b.m_body.m_aim_pos;
	if (vlen(aim - p) > 150)
		return FALSE;	/* still swinging over from somewhere else */
	if (!b.m_body.IsAimingAt(aim, radius))
		return FALSE;
	traceline(b.m_body.GetEyePosition(), p, MOVE_NOMONSTERS, b);
	return (trace_fraction == 1) ? TRUE : FALSE;
}

/* every think: look at and shoot the most dangerous visible enemy */
void tf_combat(TFBot b)
{
	BWKnown k;
	entity t;
	float d, radius = 24, range = 1200, close = 0;
	vector p;
	if (b.m_class == TFC_MEDIC)
		return;	/* the Medic's behaviour aims the beam */
	k = b.m_vision.GetPrimaryKnownThreat(TRUE);
	if (!k) {
		/* keep the minigun turning for a few seconds after a fight */
		if (b.m_class == TFC_HEAVY && b.m_vision.GetTimeSinceVisible(b.team == TF_RED ? TF_BLU : TF_RED) < 3)
			b.PressButton(BWIN_ATTACK2, 0.3);
		/* shot by someone not in view: turn to look */
		if (b.m_last_attacker && time - b.m_last_hurt < 1 && b.m_last_attacker.health > 0)
			b.m_body.AimHeadTowards(b.m_last_attacker.origin + '0 0 40', BWLOOK_IMPORTANT, 0.5, __NULL__, "who shot me");
		return;
	}
	t = k.m_subject;
	d = vlen(t.origin - b.origin);
	b.m_body.AimHeadTowardsEntity(t, BWLOOK_IMPORTANT, 0.3, __NULL__, "combat");
	switch (b.m_class) {
	case TFC_SCOUT: range = 900; radius = 30; break;
	case TFC_SOLDIER: range = 1500; radius = 40; close = 120; break;
	case TFC_HEAVY: range = 1300; radius = 24;
		b.PressButton(BWIN_ATTACK2, 0.3);
		break;
	case TFC_SNIPER: range = 4000; radius = 7; break;
	}
	if (d > range || d < close)
		return;
	/* a Sniper waits for some charge unless the target is close */
	if (b.m_class == TFC_SNIPER && b.m_zoomed && time - b.m_zoom_since < 1.2 && d > 500)
		return;
	p = b.m_intention.SelectTargetPoint(t);
	if (tf_on_target(b, p, radius))
		b.PressButton(BWIN_ATTACK, 0.15);
}

class TFMain : BWAction {
	virtual string GetName(void) { return "TFMain"; };
	virtual BWAction InitialChild(void);
	virtual float Update(float interval);
	virtual vector SelectTargetPoint(entity subject);
	virtual float OnInjured(entity inflictor, entity attacker, float damage);
};

class TFTactical : BWAction {
	virtual string GetName(void) { return "Tactical"; };
	virtual BWAction InitialChild(void);
	virtual float Update(float interval);
	virtual float OnStuck(void);
};

/* Scout, Soldier and Heavy: take the point, or hold it */
class TFPoint : BWAction {
	BWPathFollower m_path;
	vector m_spot;
	float m_spot_owner, m_next_spot, m_next_repath, m_strafe_until;
	vector m_strafe;
	virtual string GetName(void) { return "Point"; };
	virtual float OnStart(BWAction prior);
	virtual float Update(float interval);
	virtual float OnMoveToFailure(BWPath path, float reason);
	nonvirtual void PickSpot(void);
};

class TFSniperLurk : BWAction {
	BWPathFollower m_path;
	vector m_spot;
	float m_next_repath, m_settled, m_move_at;
	virtual string GetName(void) { return "SniperLurk"; };
	virtual float OnStart(BWAction prior);
	virtual float Update(float interval);
	virtual float OnInjured(entity inflictor, entity attacker, float damage);
	virtual float OnMoveToFailure(BWPath path, float reason);
	nonvirtual void PickSpot(void);
};

class TFMedicHeal : BWAction {
	BWChasePath m_chase;
	entity m_who;
	virtual string GetName(void) { return "MedicHeal"; };
	virtual float OnStart(BWAction prior);
	virtual float Update(float interval);
	nonvirtual entity PickPatient(void);
};

class TFGetHealth : BWAction {
	BWPathFollower m_path;
	entity m_pack;
	virtual string GetName(void) { return "GetHealth"; };
	virtual float OnStart(BWAction prior);
	virtual float Update(float interval);
	virtual float OnMoveToFailure(BWPath path, float reason);
};

BWAction TFBot::InitialAction(void) { return spawn(TFMain); }

BWAction TFMain::InitialChild(void) { return spawn(TFTactical); }

float TFMain::Update(float interval)
{
	tf_combat((TFBot)m_bot);
	return Continue();
}

vector TFMain::SelectTargetPoint(entity subject) { return tf_aim_point((TFBot)m_bot, subject); }

/* whoever shot us is someone to know about */
float TFMain::OnInjured(entity inflictor, entity attacker, float damage)
{
	if (tf_is_player(attacker) && attacker != m_bot)
		m_bot.m_vision.AddKnownEntity(attacker);
	return BWR_CONTINUE;
}

BWAction TFTactical::InitialChild(void)
{
	switch (((TFBot)m_bot).m_class) {
	case TFC_SNIPER: return spawn(TFSniperLurk);
	case TFC_MEDIC: return spawn(TFMedicHeal);
	}
	return spawn(TFPoint);
}

entity tf_nearest_pack(vector from)
{
	float i, d, bd = 999999;
	entity best = world;
	for (i = 0; i < g_npacks; i++) {
		if (g_packs[i].tf_back)
			continue;
		d = vlen(g_packs[i].origin - from);
		if (d < bd) {
			bd = d;
			best = g_packs[i];
		}
	}
	return best;
}

float TFTactical::Update(float interval)
{
	/* low and nobody healing us: go for a health pack */
	if (m_bot.health < m_bot.max_health * 0.4 && !tf_being_healed(m_bot) && tf_nearest_pack(m_bot.origin))
		return SuspendFor(spawn(TFGetHealth), "low health");
	return Continue();
}

/* a player who can't get anywhere jumps, as players do */
float TFTactical::OnStuck(void)
{
	m_bot.m_loco.Jump(0);
	return BWR_CONTINUE;
}

/* --- the point ------------------------------------------------------------------ */

/* somewhere open within r of the point */
vector tf_spot_near_point(BWBot b, float r)
{
	float a = random() * 2 * M_PI, d = random() * r;
	vector p = TF_POINT + [cos(a) * d, sin(a) * d, 0];
	tracebox(TF_POINT + '0 0 20', b.mins, b.maxs, p + '0 0 20', MOVE_NOMONSTERS, b);
	return bw_snap_down(trace_endpos, b);
}

float TFPoint::OnStart(BWAction prior)
{
	m_path = spawn(BWPathFollower);
	Own(m_path);
	m_spot_owner = -1;
	return Continue();
}

void TFPoint::PickSpot(void)
{
	/* attackers stand on the point; defenders spread out around it */
	if (g_owner == m_bot.team)
		m_spot = tf_spot_near_point(m_bot, 300);
	else
		m_spot = tf_spot_near_point(m_bot, 90);
	m_spot_owner = g_owner;
	m_next_spot = time + 8 + random() * 6;
	m_path.Invalidate();
	m_next_repath = 0;
}

float TFPoint::Update(float interval)
{
	BWKnown k;
	if (g_owner != m_spot_owner || (time > m_next_spot && g_owner == m_bot.team))
		PickSpot();
	if (bw_dist2d(m_bot.origin, m_spot) > 48 || fabs(m_bot.origin_z - m_spot_z) > 40) {
		if (!m_path.IsValid() || time > m_next_repath) {
			m_path.Compute(m_bot, m_spot, 0);
			m_next_repath = time + 3;
		}
		m_path.Follow(m_bot);
		return Continue();
	}
	/* there: hold the spot, and dodge about while someone is shooting */
	k = m_bot.m_vision.GetPrimaryKnownThreat(TRUE);
	if (!k) {
		m_bot.m_loco.Stop();
		if (g_owner == m_bot.team)
			m_bot.m_body.AimHeadTowards(TF_POINT + '0 0 40' + bw_flat(m_bot.origin - TF_POINT) * 3, BWLOOK_INTERESTING, 1, __NULL__, "watching");
		return Continue();
	}
	if (time > m_strafe_until) {
		vector f = normalize(bw_flat(k.m_subject.origin - m_bot.origin));
		m_strafe = [-f_y, f_x, 0] * ((random() < 0.5) ? 1 : -1);
		m_strafe_until = time + 0.4 + random() * 0.6;
		if (((TFBot)m_bot).m_class == TFC_SCOUT && random() < 0.4)
			m_bot.m_loco.Jump(0);
	}
	/* don't wander off the point while taking it */
	if (bw_dist2d(m_bot.origin + m_strafe * 48, m_spot) > 96)
		m_strafe = -m_strafe;
	m_bot.m_loco.Approach(m_bot.origin + m_strafe * 64, 1);
	m_bot.m_loco.ClearStuck();	/* dodging about one spot isn't being stuck */
	return Continue();
}

float TFPoint::OnMoveToFailure(BWPath path, float reason)
{
	if (path == m_path) {
		m_spot_owner = -1;	/* try another spot */
		m_bot.m_loco.Jump(0);
	}
	return BWR_CONTINUE;
}

/* --- the Sniper ------------------------------------------------------------------ */

float TFSniperLurk::OnStart(BWAction prior)
{
	m_path = spawn(BWPathFollower);
	Own(m_path);
	PickSpot();
	return Continue();
}

/* a waypoint on our side of the room, 600 to 1400 from the point, that can
 * see it */
void TFSniperLurk::PickSpot(void)
{
	float i, n = 0, d, side = (m_bot.team == TF_RED) ? -1 : 1;
	vector p, eye = '0 0 64', pick = m_bot.origin;
	for (i = 0; i < bw_wp_count; i++) {
		p = bw_snap_down(bw_wps[i].pos, m_bot);
		d = bw_dist2d(p, TF_POINT);
		if (d < 600 || d > 1400 || p_x * side < 0 || p_z > 8)
			continue;
		traceline(p + eye, TF_POINT + '0 0 40', MOVE_NOMONSTERS, m_bot);
		if (trace_fraction < 1)
			continue;
		n++;
		if (random() * n < 1)
			pick = p;
	}
	m_spot = pick;
	m_settled = FALSE;
	m_path.Invalidate();
	m_next_repath = 0;
	m_move_at = time + 40 + random() * 20;
}

float TFSniperLurk::Update(float interval)
{
	TFBot b = (TFBot)m_bot;
	if (time > m_move_at)
		PickSpot();
	if (bw_dist2d(b.origin, m_spot) > 40) {
		if (b.m_zoomed)
			b.PressButton(BWIN_ATTACK2, 0);	/* unzoom to move */
		if (!m_path.IsValid() || time > m_next_repath) {
			m_path.Compute(b, m_spot, 0);
			m_next_repath = time + 3;
		}
		m_path.Follow(b);
		return Continue();
	}
	b.m_loco.Stop();
	if (!m_settled) {
		m_settled = TRUE;
		test_line(sprintf("%s is set up at %v at %.1f s", b.netname, b.origin, time - g_start));
	}
	if (!b.m_zoomed)
		b.PressButton(BWIN_ATTACK2, 0);
	if (!b.m_vision.GetPrimaryKnownThreat(TRUE))
		b.m_body.AimHeadTowards(TF_POINT + '0 0 56', BWLOOK_INTERESTING, 1, __NULL__, "watching the point");
	return Continue();
}

/* shot at while set up: find somewhere else */
float TFSniperLurk::OnInjured(entity inflictor, entity attacker, float damage)
{
	if (m_settled && m_bot.health < m_bot.max_health * 0.6)
		m_move_at = time;
	return BWR_CONTINUE;
}

float TFSniperLurk::OnMoveToFailure(BWPath path, float reason)
{
	if (path == m_path)
		m_move_at = time;
	return BWR_CONTINUE;
}

/* --- the Medic ------------------------------------------------------------------ */

float TFMedicHeal::OnStart(BWAction prior)
{
	m_chase = spawn(BWChasePath, m_lead: 0);
	Own(m_chase);
	return Continue();
}

/* the most hurt teammate, leaning towards those who fight at the front */
entity TFMedicHeal::PickPatient(void)
{
	float i, s, bs = -1;
	entity e, best = world;
	TFBot t;
	for (i = 0; i < g_nplayers; i++) {
		e = g_players[i];
		t = (TFBot)e;
		if (e == m_bot || e.team != m_bot.team || e.health <= 0)
			continue;
		switch (t.m_class) {
		case TFC_HEAVY: s = 3; break;
		case TFC_SOLDIER: s = 2; break;
		case TFC_SCOUT: s = 1; break;
		default: s = 0; break;
		}
		if (t.m_class == TFC_SNIPER && vlen(e.origin - m_bot.origin) > 600)
			continue;	/* not worth the walk */
		s += 4 * (1 - e.health / e.max_health);
		if (e == m_who)
			s += 1.5;	/* stay with the one we have */
		s -= vlen(e.origin - m_bot.origin) / 1000;
		if (s > bs) {
			bs = s;
			best = e;
		}
	}
	return best;
}

float TFMedicHeal::Update(float interval)
{
	TFBot b = (TFBot)m_bot, pt;
	float d;
	BWKnown k;
	m_who = PickPatient();
	if (!m_who) {
		b.m_loco.Stop();
		return Continue();
	}
	pt = (TFBot)m_who;
	d = vlen(m_who.origin - b.origin);
	if (d > 260)
		m_chase.Chase(b, m_who);
	else if (d < 100)
		b.m_loco.Approach(b.origin + normalize(bw_flat(b.origin - m_who.origin)) * 64, 1);
	else
		b.m_loco.Stop();
	b.m_body.AimHeadTowardsEntity(m_who, BWLOOK_IMPORTANT, 0.3, __NULL__, "healing");
	if (d < 440)
		b.PressButton(BWIN_ATTACK, 0.2);
	/* uber when it's going badly and there's a fight */
	k = b.m_vision.GetPrimaryKnownThreat(TRUE);
	if (b.m_uber >= 1 && k && b.m_patient == m_who && (b.health < b.max_health * 0.5 || pt.health < pt.max_health * 0.5))
		b.PressButton(BWIN_ATTACK2, 0);
	return Continue();
}

/* --- health packs -------------------------------------------------------------- */

float TFGetHealth::OnStart(BWAction prior)
{
	m_path = spawn(BWPathFollower);
	Own(m_path);
	m_pack = tf_nearest_pack(m_bot.origin);
	if (!m_pack)
		return Done("no packs");
	m_path.Compute(m_bot, m_pack.origin, 0);
	return Continue();
}

float TFGetHealth::Update(float interval)
{
	if (m_bot.health >= m_bot.max_health * 0.9)
		return Done("healed");
	if (m_pack.tf_back) {
		m_pack = tf_nearest_pack(m_bot.origin);
		if (!m_pack)
			return Done("packs gone");
		m_path.Compute(m_bot, m_pack.origin, 0);
	}
	if (!m_path.IsValid())
		m_path.Compute(m_bot, m_pack.origin, 0);
	m_path.Follow(m_bot);
	return Continue();
}

float TFGetHealth::OnMoveToFailure(BWPath path, float reason)
{
	if (path == m_path)
		return TryDone(BWP_IMPORTANT, "can't reach the pack");
	return BWR_CONTINUE;
}

/* --- the game frame ---------------------------------------------------------------- */

void tf_packs_tick(void)
{
	float i, j;
	entity pk, e;
	for (i = 0; i < g_npacks; i++) {
		pk = g_packs[i];
		if (pk.tf_back) {
			if (time < pk.tf_back)
				continue;
			pk.tf_back = 0;
		}
		for (j = 0; j < g_nplayers; j++) {
			e = g_players[j];
			if (e.health <= 0 || e.health >= e.max_health)
				continue;
			if (bw_dist2d(e.origin, pk.origin) > 40 || fabs(e.origin_z - pk.origin_z) > 30)
				continue;
			e.health = min(e.max_health, e.health + e.max_health * pk.tf_amount);
			pk.tf_back = time + 10;
			g_pack_uses++;
			test_line(sprintf("%s picked up health at %.1f s", e.netname, time - g_start));
			break;
		}
	}
}

/* harmonic: each extra capper adds less */
float tf_cap_rate(float n)
{
	float r = 0, i;
	for (i = 1; i <= n; i++)
		r += 1 / i;
	return r;
}

void tf_point_tick(void)
{
	float i, red = 0, blu = 0, capper, n, w;
	entity e;
	if (time < g_unlock_at)
		return;
	if (!g_locked_told) {
		g_locked_told = TRUE;
		test_line(sprintf("the point unlocks at %.1f s", time - g_start));
	}
	for (i = 0; i < g_nplayers; i++) {
		e = g_players[i];
		if (e.health <= 0)
			continue;
		if (bw_dist2d(e.origin, TF_POINT) > TF_POINT_RADIUS || fabs(e.origin_z) > 40)
			continue;
		w = (((TFBot)e).m_class == TFC_SCOUT) ? 2 : 1;
		if (e.team == TF_RED)
			red += w;
		else
			blu += w;
	}
	if (red && blu) {
		/* contested: nothing moves */
	} else if (red || blu) {
		capper = red ? TF_RED : TF_BLU;
		n = red ? red : blu;
		if (capper == g_owner) {
			g_cap_progress = max(0, g_cap_progress - frametime / TF_CAP_TIME);
		} else {
			if (g_cap_team != capper) {
				g_cap_team = capper;
				g_cap_progress = 0;
			}
			g_cap_progress += frametime * tf_cap_rate(n) / TF_CAP_TIME;
			if (g_cap_progress >= 1) {
				g_owner = capper;
				g_cap_progress = 0;
				g_caps++;
				test_line(sprintf("%s captured the point at %.1f s (RED held it %.0f s, BLU %.0f s)",
				          tf_team_name(capper), time - g_start, g_hold[TF_RED], g_hold[TF_BLU]));
			}
		}
	} else {
		g_cap_progress = max(0, g_cap_progress - frametime / (2 * TF_CAP_TIME));
	}
	if (g_owner) {
		g_hold[g_owner] += frametime;
		if (g_hold[g_owner] >= TF_HOLD_TO_WIN)
			g_winner = g_owner;
	}
}

void tf_start(void)
{
	float tm;
	cvar_set("sv_jumpspeed", "289");	/* TF2's jump: 52 units */
	g_unlock_at = time + TF_UNLOCK;
	tf_pack('-600 -300 0', 0.5);
	tf_pack('500 -300 0', 0.5);
	tf_pack('-900 650 0', 1);
	tf_pack('900 650 0', 1);
	for (tm = TF_RED; tm <= TF_BLU; tm++) {
		tf_new_bot(TFC_SCOUT, tm);
		tf_new_bot(TFC_SOLDIER, tm);
		tf_new_bot(TFC_HEAVY, tm);
		tf_new_bot(TFC_SNIPER, tm);
		tf_new_bot(TFC_MEDIC, tm);
	}
	test_line(sprintf("%g players in the game", g_nplayers));
}

void tf_tick(void)
{
	float i, t = time - g_start, classes = 0, pass;
	TFBot b;
	tf_point_tick();
	tf_packs_tick();
	for (i = 0; i < g_nplayers; i++) {
		b = (TFBot)g_players[i];
		if (b.health <= 0 && time >= b.m_respawn_at) {
			tf_spawn(b);
			b.BotRespawn();
			g_respawns++;
		}
	}
	/* watch for players who stay stuck */
	if (time >= g_next_watch) {
		g_next_watch = time + 1;
		for (i = 0; i < g_nplayers; i++) {
			b = (TFBot)g_players[i];
			if (b.health > 0 && b.m_loco.IsStuck() && b.m_loco.GetStuckDuration() > 10) {
				if (!b.m_stuck_told) {
					b.m_stuck_told = TRUE;
					g_long_stuck++;
					test_line(sprintf("%s has been stuck for 10 s at %v", b.netname, b.origin));
				}
			} else {
				b.m_stuck_told = FALSE;
			}
		}
	}
	if (!g_winner && t < TF_TIME_LIMIT)
		return;
	if (g_winner) {
		test_line(sprintf("%s wins at %.1f s", tf_team_name(g_winner), t));
		for (i = 0; i < g_nplayers; i++) {
			b = (TFBot)g_players[i];
			bw_ev_clear();
			bw_send(b, (b.team == g_winner) ? BWE_WIN : BWE_LOSE);
		}
	} else {
		test_line(sprintf("time up at %.1f s, %s holds the point", t, tf_team_name(g_owner)));
	}
	for (i = TFC_SCOUT; i <= TFC_SNIPER; i++)
		if (g_kills_by[i])
			classes++;
	test_line(sprintf("summary: %g captures, %g kills (Scout %g, Soldier %g, Heavy %g, Sniper %g)",
	          g_caps, g_kills, g_kills_by[TFC_SCOUT], g_kills_by[TFC_SOLDIER], g_kills_by[TFC_HEAVY], g_kills_by[TFC_SNIPER]));
	test_line(sprintf("summary: %g headshots, %g air shots, %g of %g sniper shots hit", g_headshots, g_airshots, g_snipe_hits, g_snipes));
	test_line(sprintf("summary: %.0f healed, %g ubers, %g health packs, %g respawns, %g long stucks",
	          g_healed, g_ubers, g_pack_uses, g_respawns, g_long_stuck));
	pass = (g_caps >= 1 && g_kills >= 6 && classes >= 3 && g_healed >= 300 && g_respawns >= 1 && !g_long_stuck) ? TRUE : FALSE;
	print(pass ? "PASS\n" : "FAIL\n");
	g_finished = TRUE;
}

/* --- player-bot navigation --------------------------------------------------------- */

class PNavBot : BWBot {
	vector m_goal;
	float m_report;
	virtual BWAction InitialAction(void)
	{
		return bw_moveto(m_goal, spawn(TestReport, m_id: m_report), spawn(TestReport, m_id: m_report + 1));
	};
};

void pnav_bot(string name, vector org, float yaw, vector goal, float report)
{
	entity os = self;
	PNavBot b;
	self = spawnclient();
	if (!self) {
		self = os;
		return;
	}
	spawnfunc_PNavBot();
	b = (PNavBot)self;
	self = os;
	b.netname = name;
	b.team = 2;
	b.m_goal = goal;
	b.m_report = report;
	tf_place(b, org, yaw);
	b.BotStart();
	g_players[g_nplayers++] = b;
}

void pnav_start(void)
{
	cvar_set("sv_jumpspeed", "289");
	pnav_bot("p-ledge", '-900 900 0', 0, '600 450 48', 1);
	pnav_bot("p-stairs", '-900 -500 0', 0, '-620 -940 64', 3);
	pnav_bot("p-drop", '700 600 48', 90, '700 900 0', 7);
}

void pnav_check(void)
{
	float t = time - g_start, pass;
	if (!(g_ledge_ok && g_stairs_ok && g_drop_ok) && t < 60)
		return;
	pass = (g_ledge_ok && g_stairs_ok && g_drop_ok) ? TRUE : FALSE;
	test_line(sprintf("summary: player ledge %s, stairs %s, drop %s",
	          g_ledge_ok ? sprintf("%.1f s", g_ledge_ok) : "no", g_stairs_ok ? sprintf("%.1f s", g_stairs_ok) : "no",
	          g_drop_ok ? sprintf("%.1f s", g_drop_ok) : "no"));
	print(pass ? "PASS\n" : "FAIL\n");
	g_finished = TRUE;
}

/* --- the engine's client entry point ------------------------------------------------ */

/* every player's input frame: bots fill in their input, then the weapons and
 * the engine's player physics run on it */
void SV_RunClientCommand(void)
{
	entity p = self;
	bw_player_input(p);
	self = p;
	p.v_angle = input_angles;
	if (((TFBot)p).m_class)
		tf_weapon_frame((TFBot)p, input_buttons, input_timelength);
	self = p;
	if (p.health > 0)
		runstandardplayerphysics(p);
}
