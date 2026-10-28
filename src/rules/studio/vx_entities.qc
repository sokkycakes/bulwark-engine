/* vx_entities: what Studio entity blocks do in play and test mode.
 * Original code (ISC).
 *
 * Evidence
 *   OBSERVED (voxelprop.json VoxelEntityType): the 105 entity types. The
 *     SIGNAL column says which blocks send and which receive signals; the
 *     Connector tool (CSO_VxlConnector) links a sender to receivers.
 *   OBSERVED (CSO_VoxelPropEdit_* strings): the editable properties used
 *     below: one-time use (switch, button), door lock, piston length and
 *     stickiness, push angle/power/target, damage and target, toggle default
 *     ON/OFF, delay seconds, repeat interval, item/weapon spawner re-spawn
 *     and interval, countdown seconds and title, gravity 0.01-3.00, heal
 *     period, damage field start/end radius and growth, explosion radius and
 *     delay, hit target reuse and hit count, ID number 1-20, probability,
 *     text, colour and size of signposts, passcode of the C4 block.
 *   OBSERVED (voxelprop.json): exit_type ROUND_CLEAR / ROUND_FAIL of the
 *     exit blocks; damage_to / damage_percent of the damage blocks; hp of
 *     breakable blocks.
 *   DESIGN: signal timing (button hold, pulse length), damage ticks, the
 *     open/lit look of doors and lights, trap and field numbers, monster
 *     stand-ins (Nuclide base monsters), the property key names.
 *
 * Signals are levels: a sender is on or off, a receiver is powered while
 * any linked sender is on. Gates compute their output from their inputs. */

.float vb_prev;			/* last powered level seen */
.float vb_pending;		/* delay gate: value waiting for vb_timer */
.float vb_hits;
.entity vb_spawned;		/* monster, item or marker the block made */
.vector vb_mins, vb_maxs;	/* world bounds of the block */
.float vb_tick;

bool vx_live;			/* entity logic running (test or play) */
float vx_result;		/* 1 round clear, 2 round fail, set by exit blocks */
string vx_resultBy;
int vx_depth;

#define VE_ON 1

string
VE_Type(entity r)
{
	return vx_entity[r.vb_type];
}

float
VE_PropF(entity r, string key, float def)
{
	string s = VX_GetProp(r, key, "");
	return s == "" ? def : stof(s);
}

void
VE_Bounds(entity r)
{
	r.vb_mins = voxel_cellorigin(r.vb_cell);
	r.vb_maxs = r.vb_mins + r.vb_dims * VX_BLOCK;
}

bool
VE_Touches(entity r, entity p, float grow)
{
	return p.absmin[0] < r.vb_maxs[0] + grow && p.absmax[0] > r.vb_mins[0] - grow &&
		p.absmin[1] < r.vb_maxs[1] + grow && p.absmax[1] > r.vb_mins[1] - grow &&
		p.absmin[2] < r.vb_maxs[2] + grow && p.absmax[2] > r.vb_mins[2] - grow;
}

/* damage_to: 1 all, 2 humans, 3 zombies (OBSERVED VoxelDamageToType) */
bool
VE_Targets(float to, entity p)
{
	if (to == 1)
		return true;
	if (to == 2)
		return p.team != CS_TEAM1 || !cs_teams;
	if (to == 3)
		return p.team == CS_TEAM1;
	return false;
}

void
VE_Hurt(entity p, float amount, entity by)
{
	if (amount <= 0 || p.health <= 0 || p.takedamage == DAMAGE_NO)
		return;
	if (p.health <= amount) {
		ents.Input(p, "SetHealth", "1", world);
		ents.Input(p, "Kill", "", by);
	} else {
		ents.Input(p, "SetHealth", ftos(p.health - amount), world);
	}
}

/* --- signal network ---------------------------------------------------------- */

void VE_Recompute(entity r);

/* every receiver linked from r recomputes its inputs */
void
VE_Propagate(entity r)
{
	if (vx_depth > 32)
		return;	/* a loop of links */
	vx_depth++;
	int n = tokenizebyseparator(r.vb_links, "|");
	string targets[64];
	n = min(n, 64);
	for (int i = 0; i < n; i++)
		targets[i] = argv(i);
	for (int j = 0; j < n; j++) {
		entity t = VX_RecordAt(VX_Parse(targets[j]));
		if (t)
			VE_Recompute(t);
	}
	vx_depth--;
}

void
VE_SetOutput(entity r, float on)
{
	if (r.vb_state == on)
		return;
	r.vb_state = on;
	VE_Propagate(r);
}

/* inputs: how many linked senders exist and how many are on */
void
VE_Inputs(entity r, __out float total, __out float on)
{
	string key = VX_Key(r.vb_cell);
	total = on = 0;
	for (entity s = world; (s = find(s, ::classname, "vxblock"));) {
		if (s == r || strstrofs(strcat("|", s.vb_links, "|"), strcat("|", key, "|")) < 0)
			continue;
		total++;
		if (s.vb_state)
			on++;
	}
}

void VE_Act(entity r, float powered, float rising);

void
VE_Recompute(entity r)
{
	float total, on;
	VE_Inputs(r, total, on);
	float any = on > 0;
	float rising = any && !r.vb_prev;
	string t = VE_Type(r);
	r.vb_prev = any;

	switch (t) {
	case "AND_GATE": VE_SetOutput(r, total > 0 && on == total); break;
	case "OR_GATE": VE_SetOutput(r, any); break;
	case "XOR_GATE": VE_SetOutput(r, mod(on, 2) == 1); break;
	case "NOT_GATE": VE_SetOutput(r, !any); break;
	case "RELAY_GATE": VE_SetOutput(r, any); break;
	case "ONCE_GATE":
		if (rising && !r.vb_used) {
			r.vb_used = 1;
			VE_SetOutput(r, 1);
		}
		break;
	case "TOGGLE_GATE":
		if (rising)
			VE_SetOutput(r, !r.vb_state);
		break;
	case "DELAY_GATE":
		r.vb_pending = any;
		r.vb_timer = time + VE_PropF(r, "delay", 1);
		break;
	case "BLINK_GATE":
		if (!any)
			VE_SetOutput(r, 0);
		else if (rising)
			r.vb_timer = time;	/* starts blinking in the frame */
		break;
	case "RANDOM_RELAY_GATE":
		if (rising) {
			/* one random receiver gets a pulse */
			int n = tokenizebyseparator(r.vb_links, "|");
			if (n > 0) {
				entity pick = VX_RecordAt(VX_Parse(argv((int)floor(random() * n))));
				string old = r.vb_links;
				if (pick) {
					r.vb_links = strzone(VX_Key(pick.vb_cell));
					r.vb_state = 1;
					VE_Propagate(r);
					r.vb_state = 0;
					VE_Propagate(r);
					strunzone(r.vb_links);
					r.vb_links = old;
				}
			}
		}
		break;
	default:
		VE_Act(r, any, rising);
	}
}

/* a sender pulse: on now, off after hold seconds (0 = next frame) */
void
VE_Pulse(entity r, float hold)
{
	VE_SetOutput(r, 1);
	r.vb_timer = time + max(0.05, hold);
	r.vb_pending = 0;
}

/* --- receivers ------------------------------------------------------------------ */

/* runtime-only variants: open doors (passable, faint), lit lights */
void
VE_DefineVariants(void)
{
	for (entity r = world; (r = find(r, ::classname, "vxblock"));) {
		string t = VE_Type(r);
		float n = VX_GroupCount(r.vb_type);
		for (float i = 0; i < n; i++) {
			float ct = VX_GroupType(r.vb_type, i);
			if (ct <= 0 || ct >= VX_OPEN_BASE || voxel_typename(VX_OPEN_BASE + ct) != "")
				continue;
			if (t == "DOOR")
				voxel_define(VX_OPEN_BASE + ct, strcat("OPEN_", voxel_typename(ct)), VXS_FIELD, 0, [0.6, 0.6, 0.6], 0.15, "", 0);
			else if (t == "LIGHT_ONOFF")
				voxel_define(VX_OPEN_BASE + ct, strcat("LIT_", voxel_typename(ct)), VXS_FIELD, 0, [1, 0.95, 0.7], 0.6, "", VXF_FULLBRIGHT);
		}
	}
}

/* switches every cell of the block between its type and the variant */
void
VE_SetVariant(entity r, bool variant)
{
	float n = VX_GroupCount(r.vb_type);
	for (float i = 0; i < n; i++) {
		vector c = VX_GroupCell(r.vb_type, r.vb_rot, r.vb_cell, i);
		float base = VX_GroupType(r.vb_type, i);
		float want = variant ? VX_OPEN_BASE + base : base;
		if (voxel_typename(want) != "")
			voxel_set(c, want, r.vb_rot);
	}
}

void
VE_Piston(entity r, bool out)
{
	float len = bound(1, VE_PropF(r, "len", 1), 8);
	vector dir = [1, 0, 0];
	switch ((int)r.vb_rot & 3) {
	case 0: dir = [1, 0, 0]; break;
	case 1: dir = [0, 1, 0]; break;
	case 2: dir = [-1, 0, 0]; break;
	default: dir = [0, -1, 0]; break;
	}
	vector tip = r.vb_cell + [dir[0] > 0 ? r.vb_dims[0] - 1 : 0, dir[1] > 0 ? r.vb_dims[1] - 1 : 0, 0];
	for (float i = 1; i <= len; i++) {
		vector c = tip + dir * i;
		if (!VX_InBounds(c))
			break;
		if (out) {
			if (voxel_get(c) != 0)
				break;	/* blocked */
			voxel_set(c, r.vb_type, r.vb_rot);
		} else if (voxel_get(c) == r.vb_type) {
			voxel_set(c, 0, 0);
		}
	}
}

void
VE_SpawnMonster(entity r)
{
	/* DESIGN stand-ins: CSO monsters (voxel_monster.csv) are not acquired */
	string cls = VE_Type(r) == "MONSTER_BOSS" ? "csm_vxboss" : "csm_vxmonster";
	if (r.vb_spawned && !wasfreed(r.vb_spawned) && r.vb_spawned.health > 0)
		return;
	vector at = r.origin + [0, 0, 8];
	/* MONSTER_AREA (INFERRED from the name): anywhere on the area's floor */
	if (VE_Type(r) == "MONSTER_AREA")
		at = [r.vb_mins[0] + random() * (r.vb_maxs[0] - r.vb_mins[0]),
			r.vb_mins[1] + random() * (r.vb_maxs[1] - r.vb_mins[1]), r.vb_mins[2] + 8];
	r.vb_spawned = ents.Create(cls, at);
	printf("studio %.1f: %s spawned %s at %v\n", time, VE_Type(r), cls, at);
}

void
VE_GiveWeapon(entity r, entity p)
{
	string w = VX_GetProp(r, "weapon", "weapon_m3");
	ents.Input(p, "GiveItem", w, world);
	ents.Input(p, "SwitchToWeapon", w, world);
}

void
VE_Exit(entity r, entity by)
{
	if (vx_result)
		return;
	vx_result = vx_exit[r.vb_type] ? vx_exit[r.vb_type] : VE_PropF(r, "result", 1);
	vx_resultBy = by ? by.netname : "";
	printf("studio %.1f: exit block: round %s\n", time, vx_result == 1 ? "clear" : "fail");
}

void
VE_Explode(entity r, entity by)
{
	float radius = VE_PropF(r, "radius", 3) * VX_BLOCK;
	combat.RadiusDamage(r.origin, radius, 20, 150, by, "");
	/* breakable blocks in the blast break */
	vector lo = voxel_cellforpoint(r.origin - [radius, radius, radius]);
	vector hi = voxel_cellforpoint(r.origin + [radius, radius, radius]);
	for (float z = lo[2]; z <= hi[2]; z++)
		for (float y = lo[1]; y <= hi[1]; y++)
			for (float x = lo[0]; x <= hi[0]; x++) {
				float t = voxel_get([x, y, z]);
				if (vx_hp[VX_T(t)] > 0)
					voxel_set([x, y, z], 0, 0);
			}
	VE_SetVariant(r, false);
	float n = VX_GroupCount(r.vb_type);
	for (float i = 0; i < n; i++)
		voxel_set(VX_GroupCell(r.vb_type, r.vb_rot, r.vb_cell, i), 0, 0);
	r.vb_used = 1;
	printf("studio %.1f: explosive block went off\n", time);
}

/* a receiver's input changed */
void
VE_Act(entity r, float powered, float rising)
{
	string t = VE_Type(r);

	switch (t) {
	case "DOOR":
		VE_SetVariant(r, powered);
		r.vb_state = powered;
		break;
	case "LIGHT_ONOFF":
		VE_SetVariant(r, powered);
		r.vb_state = powered;
		break;
	case "PISTON":
	case "SPIKE_PISTON":
		VE_Piston(r, powered);
		r.vb_state = powered;
		break;
	case "ANNOUNCE":
		if (rising)
			CS_Banner(VX_GetProp(r, "text", "..."));
		break;
	case "EXIT":
		if (rising)
			VE_Exit(r, world);
		break;
	case "MONSTER":
	case "MONSTER_BOSS":
	case "MONSTER_TARGET":
	case "MONSTER_AREA":
		if (rising)
			VE_SpawnMonster(r);
		break;
	case "LINK_EXPLOSIVE":
		if (rising && !r.vb_used)
			r.vb_timer = time + VE_PropF(r, "delay", 0.5);
		break;
	case "RESET":
		if (rising)
			localcmd("sv vx reset\n");
		break;
	case "COUNTDOWN":
		if (rising)
			r.vb_timer = time + VE_PropF(r, "seconds", 10);
		break;
	case "MP3":
	case "EMITSOUND":
		if (rising)
			sound(r, CHAN_AUTO, VX_GetProp(r, "sound", "misc/talk.wav"), 1, ATTN_NORM);
		break;
	case "TRANSIT":
		if (rising && VX_GetProp(r, "map", "") != "")
			localcmd(sprintf("sv vx play %s\n", VX_GetProp(r, "map", "")));
		break;
	default:
		/* area-type receivers (push, damage, heal, gravity, traps, fields)
		   are active while powered, or always when nothing is linked */
		r.vb_state = powered;
		break;
	}
}

/* --- player interaction --------------------------------------------------------- */

/* E on a block (cmd csnz_use); true if something happened */
bool
VE_Use(entity r, entity p)
{
	string t = VE_Type(r);
	float once = VE_PropF(r, "once", 0);

	if (once && r.vb_used)
		return false;

	switch (t) {
	case "SWITCH":
		r.vb_used = 1;
		VE_SetOutput(r, !r.vb_state);
		return true;
	case "BUTTON":
	case "HIT_BUTTON":
		r.vb_used = 1;
		VE_Pulse(r, VE_PropF(r, "hold", 3));
		return true;
	case "HIT_TARGET":
		r.vb_hits++;
		if (r.vb_hits >= VE_PropF(r, "hits", 1)) {
			r.vb_hits = 0;
			if (!VE_PropF(r, "reuse", 1))
				r.vb_used = 1;
			VE_Pulse(r, 1);
		}
		return true;
	case "HIT_EXPLOSIVE":
		VE_Explode(r, p);
		return true;
	case "DOOR":
		/* an unlinked, unlocked door opens with E */
		if (VE_PropF(r, "locked", 0)) {
			CS_Tell(p, CS_Text("CSO_VoxelPropEdit_DoorShowLock", "[Locked]"));
			return true;
		}
		VE_SetVariant(r, !r.vb_state);
		r.vb_state = !r.vb_state;
		return true;
	case "READING_MEMO":
	case "READING_BOOK":
	case "READING_COMPUTER":
	case "TEXTBOARD":
		CS_Tell(p, VX_GetProp(r, "text", voxel_typename(r.vb_type)));
		return true;
	case "WEAPON_BOX":
	case "STUDIO_WEAPON_SPAWNER":
	case "WEAPON_SPAWNER":
		VE_GiveWeapon(r, p);
		return true;
	case "WEAPON_REMOVER":
		ents.Input(p, "RemoveActiveWeapon", "", world);
		return true;
	case "C4BOMB":
		CS_Tell(p, "Enter the passcode: cmd vx code <number>");
		p.vx_linkFrom = r.vb_cell;
		return true;
	case "DICE_NUMBER":
	case "RANDOM_NUMBER":
		float lo = VE_PropF(r, "min", 1), hi = VE_PropF(r, "max", 6);
		float roll = lo + floor(random() * (hi - lo + 1));
		CS_Banner(sprintf("%s rolled %g", p.netname, roll));
		/* the output fires when the roll hits the block's target number */
		if (roll == VE_PropF(r, "target", hi))
			VE_Pulse(r, 1);
		return true;
	case "PORTAL":
		return false;	/* by touch */
	}
	return false;
}

void
VE_Frame(void)
{
	if (!vx_live)
		return;

	for (entity r = world; (r = find(r, ::classname, "vxblock"));) {
		string t = VE_Type(r);
		float total = 0, on = 0;

		/* timers */
		if (r.vb_timer > 0 && time >= r.vb_timer) {
			switch (t) {
			case "DELAY_GATE":
				r.vb_timer = 0;
				VE_SetOutput(r, r.vb_pending);
				break;
			case "BLINK_GATE":
				VE_SetOutput(r, !r.vb_state);
				r.vb_timer = r.vb_prev ? time + VE_PropF(r, "interval", 1) : 0;
				if (!r.vb_prev)
					VE_SetOutput(r, 0);
				break;
			case "COUNTDOWN":
				r.vb_timer = 0;
				CS_Banner(sprintf("%s: 0", VX_GetProp(r, "title", "Countdown")));
				VE_Pulse(r, 1);
				break;
			case "LINK_EXPLOSIVE":
				r.vb_timer = 0;
				VE_Explode(r, world);
				break;
			default:
				/* end of a pulse */
				r.vb_timer = 0;
				VE_SetOutput(r, 0);
				break;
			}
		}

		bool area = (t == "AREA" || t == "REPEATABLE_AREA" || t == "PRESSURE" || t == "PLAYER_CHECKER");
		bool zone = (t == "PUSH" || t == "FIXED_PUSH" || t == "CUSTOM_DAMAGE" || t == "HEAL_AREA" ||
			t == "GRAVITY_AREA" || t == "SPIKE_TRAP" || t == "SPIKE_WALL" || t == "SPINNING_BLADE" ||
			t == "PENDULUM_BLADE" || t == "FALLING_SPIKE" || t == "FLAME_TRAP" || t == "ARROW_TRAP" ||
			t == "DAMAGE_FIELD" || t == "PORTAL" || t == "EXIT" || t == "ITEM_SPAWN" || t == "ZOMBIE_SAFETY_ZONE");
		if (!area && !zone)
			continue;

		/* zones work while powered, or always when nothing drives them */
		if (zone) {
			VE_Inputs(r, total, on);
			if (total > 0 && !r.vb_state && t != "PORTAL" && t != "EXIT" && t != "ITEM_SPAWN")
				continue;
		}

		float inside = 0;
		bool tick = time >= r.vb_tick;
		for (entity p = world; (p = next.Player(p));) {
			if (p.health <= 0 || p.classname == "spectator")
				continue;
			float grow = (t == "DAMAGE_FIELD") ? VE_PropF(r, "radius", 1) * VX_BLOCK : 2;
			if (!VE_Touches(r, p, grow))
				continue;
			inside++;

			switch (t) {
			case "PUSH":
			case "FIXED_PUSH":
				float pitch = VE_PropF(r, "pitch", 45), power = VE_PropF(r, "power", 600);
				if (VE_Targets(VE_PropF(r, "to", 1), p)) {
					makevectors([-pitch, r.vb_rot * 90, 0]);
					p.velocity = v_forward * power;
					p.flags &= ~FL_ONGROUND;
				}
				break;
			case "CUSTOM_DAMAGE":
			case "SPIKE_TRAP":
			case "SPIKE_WALL":
			case "SPINNING_BLADE":
			case "PENDULUM_BLADE":
			case "FALLING_SPIKE":
			case "FLAME_TRAP":
			case "ARROW_TRAP":
			case "DAMAGE_FIELD":
				if (tick && VE_Targets(VE_PropF(r, "to", 1), p))
					VE_Hurt(p, VE_PropF(r, "damage", 10), world);
				break;
			case "HEAL_AREA":
				if (tick)
					ents.Input(p, "SetHealth", ftos(min(p.max_health, p.health + VE_PropF(r, "heal", 10))), world);
				break;
			case "GRAVITY_AREA":
				p.gravity = bound(0.01, VE_PropF(r, "gravity", 0.3), 3);
				break;
			case "PORTAL":
				/* to the portal with the same ID number (1-20) */
				float id = VE_PropF(r, "index", 1);
				for (entity o = world; (o = find(o, ::classname, "vxblock"));)
					if (o != r && VE_Type(o) == "PORTAL" && VE_PropF(o, "index", 1) == id) {
						CS_MoveToSpot(p, o.origin + [0, 0, o.vb_dims[2] * VX_BLOCK * 0.5 + 40], p.v_angle);
						break;
					}
				break;
			case "EXIT":
				VE_Exit(r, p);
				break;
			case "ITEM_SPAWN":
				if (!r.vb_used || (VE_PropF(r, "regen", 1) && time >= r.vb_hits)) {
					r.vb_used = 1;
					r.vb_hits = time + VE_PropF(r, "interval", 10);
					string item = VX_GetProp(r, "item", "health");
					if (item == "health")
						ents.Input(p, "SetHealth", ftos(min(p.max_health, p.health + 25)), world);
					else if (item == "armor")
						ents.Input(p, "SetArmor", "100", world);
					else
						ents.Input(p, "GiveItem", item, world);
				}
				break;
			}
		}
		if (tick)
			r.vb_tick = time + VE_PropF(r, "period", 1);

		/* the damage field grows each second up to its end radius */
		if (t == "DAMAGE_FIELD" && tick) {
			float rad = VE_PropF(r, "radius", VE_PropF(r, "minradius", 1));
			rad = min(VE_PropF(r, "maxradius", 6), rad + VE_PropF(r, "step", 0.5));
			VX_SetProp(r, "radius", ftos(rad));
		}

		if (area) {
			float need = t == "PLAYER_CHECKER" ? VE_PropF(r, "count", 1) : 1;
			float want = inside >= need;
			if (t == "AREA" && r.vb_used && !want)
				continue;
			if (want && t == "AREA")
				r.vb_used = 1;	/* an area fires once; REPEATABLE_AREA every time */
			VE_SetOutput(r, want);
		}
	}

	/* blocks that hurt on contact (KILL_*, DAMAGE_*: OBSERVED damage_to and
	   damage_percent): a share of max health per second (DESIGN tick) */
	for (entity q = world; (q = next.Player(q));) {
		if (q.health <= 0 || q.classname == "spectator" || time < q.vb_tick)
			continue;
		vector lo = voxel_cellforpoint(q.absmin - [1, 1, 2]), hi = voxel_cellforpoint(q.absmax + [1, 1, 0]);
		for (float z = lo[2]; z <= hi[2]; z++)
			for (float y = lo[1]; y <= hi[1]; y++)
				for (float x = lo[0]; x <= hi[0]; x++) {
					float ct = voxel_get([x, y, z]);
					if (vx_dmgTo[VX_T(ct)] && VE_Targets(vx_dmgTo[VX_T(ct)], q)) {
						VE_Hurt(q, q.max_health * vx_dmgPct[ct] / 100, world);
						q.vb_tick = time + 1;
					}
				}
	}
}

/* --- markers other systems look for --------------------------------------------- */

void
VE_Marker(entity r, string cname)
{
	entity m = spawn();
	m.classname = cname;
	setorigin(m, r.origin);
	m.team = VE_PropF(r, "team", 0);
	r.vb_spawned = m;
}

/* starts the entity logic: called when a test or play begins */
void
VE_Activate(void)
{
	vx_live = true;
	vx_result = 0;
	VE_DefineVariants();
	for (entity r = world; (r = find(r, ::classname, "vxblock"));) {
		string t = VE_Type(r);
		VE_Bounds(r);
		r.vb_state = r.vb_prev = r.vb_used = r.vb_timer = r.vb_hits = 0;
		switch (t) {
		case "PLAYER_SPAWN":
			float st = VE_PropF(r, "team", 0);
			VE_Marker(r, st == CS_TEAM1 ? "csnz_vxspawn_t1" : (st == CS_TEAM2 ? "csnz_vxspawn_t2" : "csnz_vxspawn"));
			/* stand on top of the block */
			setorigin(r.vb_spawned, r.origin + [0, 0, r.vb_dims[2] * VX_BLOCK * 0.5 + 40]);
			break;
		case "VOXELC4TARGETAREA": VE_Marker(r, "info_bomb_target"); break;
		case "VOXELBUYZONE": VE_Marker(r, "func_buyzone"); break;
		case "ESCAPE_GOAL": VE_Marker(r, "csnz_escape_goal"); break;
		case "GOALNET_SOCCER": VE_Marker(r, "csnz_goalnet"); break;
		case "BALLSPAWN_SOCCER": VE_Marker(r, "csnz_ballspawn"); break;
		case "ZOMBIE_SAFETY_ZONE": VE_Marker(r, "csnz_safezone"); break;
		case "AREA": VE_Marker(r, "csnz_area"); break;
		case "MONSTER":
		case "MONSTER_BOSS":
		case "MONSTER_AREA":
			/* monsters with no link spawn at the start */
			if (!strlen(r.vb_links)) {
				float tot, on;
				VE_Inputs(r, tot, on);
				if (!tot)
					VE_SpawnMonster(r);
			}
			break;
		case "TOGGLE_GATE":
			r.vb_state = VE_PropF(r, "default", 0);
			break;
		case "COUNTDOWN":
			if (VE_PropF(r, "autostart", 1)) {
				float tot2, on2;
				VE_Inputs(r, tot2, on2);
				if (!tot2)
					r.vb_timer = time + VE_PropF(r, "seconds", 10);
			}
			break;
		}
	}
	/* NOT gates and toggles that start on drive their receivers */
	for (entity g = world; (g = find(g, ::classname, "vxblock"));) {
		if (VE_Type(g) == "NOT_GATE") {
			g.vb_state = 0;
			VE_Recompute(g);
		} else if (g.vb_state) {
			VE_Propagate(g);
		}
	}
	printf("studio %.1f: entity blocks live\n", time);
}

void
VE_Deactivate(void)
{
	vx_live = false;
	for (entity r = world; (r = find(r, ::classname, "vxblock"));) {
		if (r.vb_spawned && !wasfreed(r.vb_spawned))
			remove(r.vb_spawned);
		r.vb_spawned = world;
	}
	for (entity q = world; (q = next.Player(q));)
		q.gravity = 1;
}

/* the C4 block opens with its passcode (OBSERVED CSO_VoxelPropEdit_C4Password) */
void
VE_Code(entity p, string code)
{
	entity r = VX_RecordAt(p.vx_linkFrom);
	if (!r || VE_Type(r) != "C4BOMB")
		return;
	if (code == VX_GetProp(r, "code", "0000")) {
		CS_Tell(p, "Correct passcode.");
		VE_Pulse(r, 1);
	} else {
		CS_Tell(p, "Wrong passcode.");
	}
}
