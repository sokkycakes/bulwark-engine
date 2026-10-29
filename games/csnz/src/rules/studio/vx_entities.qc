/* vx_entities: what Studio entity blocks do in play and test mode.
 * Original code (ISC).
 *
 * Evidence
 *   SOURCE-CONFIRMED (mp.dll, the CVoxel* classes made by the entity factory
 *     FUN_10ab2cd0; trace notes in qc-parity/studio-trace.md):
 *     - signals are events: a block that changes sends its output to each of
 *       its (at most 5) receivers, which store the state per input (at most
 *       5) and react; a block sends on every input event, even unchanged
 *     - gates: OR any input on; AND at least 2 inputs, all on; XOR parity;
 *       NOT the inverse of the last input; RELAY the last input; ONCE latches
 *       on; TOGGLE flips on a rising input; DELAY moves the output to the
 *       input after its delay (0-86400 s); BLINK flips every interval (1-86400
 *       s, default 2) while its input is on; RANDOM_RELAY passes the input to
 *       one receiver picked at random and 0 to the others
 *     - SWITCH toggles on use (buttons/lever4.wav); BUTTON goes on for 0.2 s
 *       (buttons/blip1.wav); PRESSURE is on while a player stands on it
 *       (buttons/latchlocked1.wav); DOOR opens with its input or by use when
 *       not locked by its links, and does not close on a player
 *       (weapons/m2_open.wav)
 *     - AREA fires once per round on the first player (or monster) inside;
 *       REPEATABLE_AREA is on while a player is inside, checked every 0.2 s
 *     - COUNTDOWN (0-999 s, default 10) starts on a rising input and turns
 *       its output on when it ends; BREAKABLE breaks with its input or its
 *       hp and sends nothing; HIT_TARGET is raised by its input and sends on
 *       when shot while raised; HIT_BUTTON is on for 0.2 s when shot
 *     - PLAYER_CHECKER makes the block a player's respawn point
 *     - coins: DICE_NUMBER / RANDOM_NUMBER rolls (cooldown 2.3 s), COIN_FARM,
 *       COIN_FLIP, LOTTERY, ROCK_SCISSOR_PAPER, ACCOUNT_CHECKER; a player's
 *       coins are capped at 1e9
 *   OBSERVED (voxelprop.json VoxelEntityType; CSO_VoxelPropEdit_* and Vxl_*
 *     strings): the types, the editable properties and the messages.
 *   DESIGN: the property key names, how shots on blocks are detected (an
 *     invisible hitbox per hittable block), the command line stand-ins for
 *     the coin game dialogs (cmd vx coinflip/lottery/rsp/pay), damage ticks
 *     of traps and fields, monster stand-ins, the open/lit look of doors and
 *     lights, and the start of a hit target with no input (raised). */

#define VE_MAXLINKS	5

.float vb_prev;			/* last input state received (gates, countdown) */
.float vb_pending;		/* delay gate: value waiting for vb_timer */
.float vb_hits;
.entity vb_spawned;		/* monster, item or marker the block made */
.vector vb_mins, vb_maxs;	/* world bounds of the block */
.float vb_tick;
.string vb_in;			/* input states as received: "key=1|key=0" */
.entity vb_pick;		/* random relay: the chosen receiver */

/* the engine gives an added progs 256 spare entity words, so these share
   fields that their entities never use otherwise */
#define vb_hitbox vb_spawned	/* hittable blocks: what shots hit */
#define vb_dmg vb_hits		/* breakable: damage taken */
#define vb_aux vb_inputs	/* per type: cooldown, lottery pot */
#define vb_aux2 vb_pending	/* lottery phase */
#define vb_lot vx_clip		/* lottery: owner entnum per number, "0 0 ..." */
#define vb_act vb_user		/* last activator, for timed sends */

/* players */
#define vx_coins vb_prev	/* Studio coins (player data +0x4c) */
#define vx_check vb_pick	/* PLAYER_CHECKER block: respawn point */
#define vx_game vb_user		/* coin game block the player last used */
#define vx_cfStreak vb_hits	/* coin flip, at vx_game */
#define vx_cfStake vb_pending
#define vx_payAsk vb_timer	/* account checker: asked at this time */

bool vx_live;			/* entity logic running (test or play) */
float vx_result;		/* 1 round clear, 2 round fail, set by exit blocks */
string vx_resultBy;
int vx_depth;

var float autocvar_studio_startcoins = 0;	/* DESIGN: coins at the start of a map */

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

/* a localized Vxl_ string with %s / %d / %02d filled in order */
string
VE_Fmt3(string key, string fallback, string a, string b, string c)
{
	string s = CS_Text(key, fallback);
	string args[3];
	args[0] = a;
	args[1] = b;
	args[2] = c;
	for (int i = 0; i < 3; i++) {
		float at = -1, len = 0;
		float ps = strstrofs(s, "%s"), pd = strstrofs(s, "%d"), p2 = strstrofs(s, "%02d");
		if (ps >= 0) { at = ps; len = 2; }
		if (pd >= 0 && (at < 0 || pd < at)) { at = pd; len = 2; }
		if (p2 >= 0 && (at < 0 || p2 < at)) { at = p2; len = 4; }
		if (at < 0)
			break;
		string v = args[i];
		if (len == 4 && strlen(v) < 2)
			v = strcat("0", v);
		s = strcat(substring(s, 0, at), v, substring(s, at + len, -1));
	}
	return s;
}

string
VE_Fmt(string key, string fallback, string a, string b)
{
	return VE_Fmt3(key, fallback, a, b, "");
}

/* --- coins ----------------------------------------------------------------------- */

void
VE_SetCoins(entity p, float c)
{
	p.vx_coins = bound(0, floor(c), 1000000000);
	forceinfokey(p, "vxcoins", ftos(p.vx_coins));
}

void
VE_AddCoins(entity p, float c)
{
	VE_SetCoins(p, p.vx_coins + c);
	printf("studio %.1f: %s coins %+g = %g\n", time, p.netname, c, p.vx_coins);
}

/* --- signal network ---------------------------------------------------------- */

void VE_Input(entity t, entity from, float st, entity act);
void VE_Roll(entity r, entity p);
bool VE_Hittable(string t);

/* the input state r last received from the sender with key k */
float
VE_InState(entity r, string k)
{
	float n = tokenizebyseparator(r.vb_in, "|");
	for (float i = 0; i < n; i++) {
		string e = argv(i);
		float eq = strstrofs(e, "=");
		if (substring(e, 0, eq) == k)
			return stof(substring(e, eq + 1, -1));
	}
	return 0;
}

void
VE_InStore(entity r, string k, float st)
{
	string out = "";
	float n = tokenizebyseparator(r.vb_in, "|");
	for (float i = 0; i < n; i++) {
		string e = argv(i);
		if (substring(e, 0, strstrofs(e, "=")) != k)
			out = out == "" ? e : strcat(out, "|", e);
	}
	out = out == "" ? sprintf("%s=%g", k, st) : sprintf("%s|%s=%g", out, k, st);
	if (r.vb_in)
		strunzone(r.vb_in);
	r.vb_in = strzone(out);
}

void
VE_InClear(entity r)
{
	if (r.vb_in)
		strunzone(r.vb_in);
	r.vb_in = "";
}

bool
VE_LinksTo(entity s, entity r)
{
	return strstrofs(strcat("|", s.vb_links, "|"), strcat("|", VX_Key(r.vb_cell), "|")) >= 0;
}

/* how many senders feed r and how many of the states they sent are on */
void
VE_Inputs(entity r, __out float total, __out float on)
{
	total = on = 0;
	for (entity s = world; (s = find(s, ::classname, "vxblock"));) {
		if (s == r || !VE_LinksTo(s, r))
			continue;
		total++;
		if (VE_InState(r, VX_Key(s.vb_cell)))
			on++;
	}
}

float
VE_InputCount(entity r)
{
	float total, on;
	VE_Inputs(r, total, on);
	return total;
}

/* XOR: parity of the stored input states */
float
VE_InParity(entity r)
{
	float total, on;
	VE_Inputs(r, total, on);
	return mod(on, 2);
}

/* the state r sends to receiver t: a random relay sends only to its pick */
float
VE_OutputFor(entity r, entity t)
{
	string ty = VE_Type(r);
	if (ty == "RANDOM_RELAY_GATE")
		return (t == r.vb_pick) ? r.vb_state : 0;
	if (ty == "HIT_TARGET")
		return r.vb_state == 2;
	return r.vb_state;
}

/* slot 24: every receiver gets this block's output, with the activator */
void
VE_Send(entity r, entity act)
{
	if (vx_depth > 32)
		return;	/* a loop of links */
	vx_depth++;
	int n = tokenizebyseparator(r.vb_links, "|");
	string targets[VE_MAXLINKS * 2];
	n = min(n, VE_MAXLINKS * 2);
	for (int i = 0; i < n; i++)
		targets[i] = argv(i);
	for (int j = 0; j < n; j++) {
		entity t = VX_RecordAt(VX_Parse(targets[j]));
		if (t)
			VE_Input(t, r, VE_OutputFor(r, t), act);
	}
	vx_depth--;
}

void
VE_SetOut(entity r, float on, entity act)
{
	r.vb_state = on;
	VE_Send(r, act);
}

/* random relay: one of the receivers, picked when needed (FUN_10a627b0) */
entity
VE_PickReceiver(entity r)
{
	int n = tokenizebyseparator(r.vb_links, "|");
	if (n <= 0)
		return world;
	return VX_RecordAt(VX_Parse(argv((int)floor(random() * n))));
}

/* --- block looks ------------------------------------------------------------------ */

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
			{
				voxel_define(VX_OPEN_BASE + ct, strcat("LIT_", voxel_typename(ct)), VXS_FIELD, 0, [1, 0.95, 0.7], 0.6, "", VXF_FULLBRIGHT);
				/* switched on: the light the block's own type gives (hw.dll
				   keeps a switch block's light at 0 until its on bit is set) */
				string sl = hash_get(vx_switchLight, ftos(ct), "", EV_STRING);
				if (sl == "")
					sl = hash_get(vx_switchLight, ftos(r.vb_type), "", EV_STRING);
				if (sl != "" && checkbuiltin(voxel_setlight))
					voxel_setlight(VX_OPEN_BASE + ct, VXL_POINT, stov(sl));
			}
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

/* every cell of the block present or gone (breakable, used-up coin farm) */
void
VE_SetPresent(entity r, bool present)
{
	float n = VX_GroupCount(r.vb_type);
	for (float i = 0; i < n; i++) {
		vector c = VX_GroupCell(r.vb_type, r.vb_rot, r.vb_cell, i);
		voxel_set(c, present ? VX_GroupType(r.vb_type, i) : 0, present ? r.vb_rot : 0);
	}
	if (VE_Hittable(VE_Type(r)) && r.vb_hitbox)
		r.vb_hitbox.solid = present ? SOLID_BBOX : SOLID_NOT;
}

/* a player inside the block's cells (doors and breakables do not close on one) */
bool
VE_Occupied(entity r)
{
	for (entity p = world; (p = next.Player(p));)
		if (p.health > 0 && p.classname != "spectator" && VE_Touches(r, p, -1))
			return true;
	return false;
}

void
VE_Sound(entity r, string key, string def)
{
	sound(r, CHAN_AUTO, VX_GetProp(r, key, def), 1, ATTN_NORM);
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
	vector at = r.origin + [0, 0, r.vb_dims[2] * VX_BLOCK * 0.5 + 8];
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
	if (r.vb_used)
		return;
	float radius = VE_PropF(r, "radius", 3) * VX_BLOCK;
	r.vb_used = 1;
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
	VE_SetPresent(r, false);
	printf("studio %.1f: explosive block went off\n", time);
}

/* BREAKABLE (FUN_10a79d80): broken or whole; never sends */
void
VE_Break(entity r, bool broken)
{
	if (!broken && VE_Occupied(r))
		return;
	r.vb_state = broken;
	if (!broken)
		r.vb_dmg = 0;
	VE_SetPresent(r, !broken);
	if (broken)
		printf("studio %.1f: breakable %s broke\n", time, VX_Key(r.vb_cell));
}

/* DOOR (FUN_10a7a2a0): false when it cannot close on a player */
bool
VE_Door(entity r, float open, entity act)
{
	if (!open && VE_Occupied(r))
		return false;
	r.vb_state = open;
	VE_SetVariant(r, open);
	VE_Sound(r, "sound", "weapons/m2_open.wav");
	VE_Send(r, act);
	return true;
}

void
VE_CountdownStart(entity r, entity act)
{
	r.vb_act = act;
	r.vb_timer = time + bound(0, VE_PropF(r, "seconds", 10), 999);
	CS_Banner(sprintf("%s: %d", VX_GetProp(r, "title", "Countdown"), bound(0, VE_PropF(r, "seconds", 10), 999)));
}

/* --- inputs (slot 25) ------------------------------------------------------------ */

void
VE_Input(entity r, entity from, float st, entity act)
{
	string k = VX_Key(from.vb_cell);
	float was = VE_InState(r, k);
	VE_InStore(r, k, st);
	float total, on;
	VE_Inputs(r, total, on);
	string t = VE_Type(r);

	switch (t) {
	case "OR_GATE":
		VE_SetOut(r, on > 0, act);
		break;
	case "AND_GATE":
		VE_SetOut(r, total >= 2 && on == total, act);
		break;
	case "XOR_GATE":
		VE_SetOut(r, VE_InParity(r), act);
		break;
	case "NOT_GATE":
		VE_SetOut(r, !st, act);
		break;
	case "RELAY_GATE":
		VE_SetOut(r, st, act);
		break;
	case "ONCE_GATE":
		if (st && !r.vb_state)
			r.vb_state = 1;
		VE_Send(r, act);
		break;
	case "TOGGLE_GATE":
		if (st && !r.vb_prev)
			r.vb_state = !r.vb_state;
		r.vb_prev = st;
		VE_Send(r, act);
		break;
	case "DELAY_GATE":
		/* FUN_10a77c70: inputs while a change is pending are only recorded */
		r.vb_prev = st;
		if (!r.vb_timer && st != r.vb_state) {
			r.vb_pending = st;
			r.vb_act = act;
			if (VE_PropF(r, "immediate", 0)) {
				VE_SetOut(r, st, act);
				break;
			}
			r.vb_timer = time + bound(0, VE_PropF(r, "delay", 1), 86400);
		}
		break;
	case "BLINK_GATE":
		r.vb_prev = st;
		if (!st) {
			r.vb_timer = 0;
			VE_SetOut(r, 0, act);
		} else if (!r.vb_timer) {
			r.vb_act = act;
			r.vb_timer = time;	/* first flip in this frame */
		}
		break;
	case "RANDOM_RELAY_GATE":
		r.vb_prev = st;
		if (!r.vb_pick || wasfreed(r.vb_pick))
			r.vb_pick = VE_PickReceiver(r);
		VE_SetOut(r, st, act);
		break;
	case "DOOR":
		VE_Door(r, st, act);
		break;
	case "LIGHT_ONOFF":
		VE_SetVariant(r, st);
		r.vb_state = st;
		break;
	case "PISTON":
	case "SPIKE_PISTON":
		VE_Piston(r, st);
		r.vb_state = st;
		break;
	case "BREAKABLE":
		VE_Break(r, st);
		break;
	case "COUNTDOWN":
		if (!was && st && !r.vb_state)
			VE_CountdownStart(r, act);
		break;
	case "HIT_TARGET":
		/* 0 down, 1 raised, 2 hit (FUN_10a77ee0) */
		if (!st) {
			if ((r.vb_state == 2 && VE_PropF(r, "reuse", 1)) || r.vb_state == 1)
				r.vb_state = 0;
		} else if (r.vb_state == 0) {
			r.vb_state = 1;
		}
		VE_Send(r, act);
		break;
	case "DICE_NUMBER":
	case "RANDOM_NUMBER":
		if (st)
			VE_Roll(r, act);
		break;
	case "ANNOUNCE":
		if (st && !was)
			CS_Banner(VX_GetProp(r, "text", "..."));
		break;
	case "EXIT":
		if (st && !was)
			VE_Exit(r, act);
		break;
	case "MONSTER":
	case "MONSTER_BOSS":
	case "MONSTER_TARGET":
		if (st && !was)
			VE_SpawnMonster(r);
		break;
	case "LINK_EXPLOSIVE":
		if (st && !was && !r.vb_used && !r.vb_timer)
			r.vb_timer = time + VE_PropF(r, "delay", 0.5);
		break;
	case "RESET":
		if (st && !was)
			localcmd("sv vx reset\n");
		break;
	case "MP3":
	case "EMITSOUND":
		if (st && !was)
			sound(r, CHAN_AUTO, VX_GetProp(r, "sound", "misc/talk.wav"), 1, ATTN_NORM);
		break;
	case "TRANSIT":
		if (st && !was && VX_GetProp(r, "map", "") != "")
			localcmd(sprintf("sv vx play %s\n", VX_GetProp(r, "map", "")));
		break;
	default:
		/* area-type receivers (push, damage, heal, gravity, traps, fields)
		   are active while an input is on, or always when nothing is linked */
		r.vb_state = on > 0;
		break;
	}
}

/* --- dice and random numbers (CVoxelRandomNumberGenerator) ------------------------ */

void
VE_Roll(entity r, entity p)
{
	bool dice = VE_Type(r) == "DICE_NUMBER";
	if (time < r.vb_aux) {
		if (p)
			CS_Tell(p, CS_Text("Vxl_Dice_Cooltime", "Rolling the dice..."));
		return;
	}
	float lo = 1, hi = 6;
	if (!dice) {
		lo = bound(0, VE_PropF(r, "min", 1), 9999);
		hi = bound(lo, VE_PropF(r, "max", 100), 9999);
	}
	float roll = lo + floor(random() * (hi - lo + 1));
	r.vb_hits = roll;
	if (dice) {
		CS_Banner(VE_Fmt("Vxl_DiceNumber_Announce", "You roll the dice and get [%d].", ftos(roll), ""));
		r.vb_aux = time + 2.3;
	} else {
		CS_Banner(VE_Fmt("Vxl_RandomNumber_Announce", "This time, the number is [%d].", ftos(roll), ""));
		r.vb_aux = time;
	}
	printf("studio %.1f: %s rolled %g (%g-%g)\n", time, VE_Type(r), roll, lo, hi);
}

/* --- coin games ------------------------------------------------------------------- */

/* COIN_FLIP (CVoxelCoinFlip): choice 0 heads, 1 tails, 2 continue, 3 stop */
void
VE_CoinFlip(entity r, entity p, float choice)
{
	float base = max(1, VE_PropF(r, "bet", 100));
	if (p.vx_game != r || p.vx_cfStake <= 0) {
		p.vx_game = r;
		p.vx_cfStreak = 0;
		p.vx_cfStake = base * 2;
	}
	if (choice == 0 || choice == 1) {
		float side = floor(random() * 2);
		if (side == choice) {
			p.vx_cfStreak++;
			CS_Tell(p, sprintf("%s: %s (%s %g)", CS_Text("Vxl_CoinFlip_Title", "Coin Toss"),
				CS_Text("Vxl_RockScissorPaper_Win", "Win"), CS_Text("Vxl_CoinFlip_Dividend", "Amount Gained"), p.vx_cfStake));
		} else {
			VE_AddCoins(p, -base);
			p.vx_cfStake = base;
			p.vx_cfStreak = 0;
			CS_Tell(p, sprintf("%s: %s (-%g)", CS_Text("Vxl_CoinFlip_Title", "Coin Toss"),
				CS_Text("Vxl_RockScissorPaper_Lose", "Lose"), base));
		}
		printf("studio %.1f: coin flip %s guessed %g, coin %g, streak %g stake %g\n", time, p.netname, choice, side, p.vx_cfStreak, p.vx_cfStake);
	} else if (choice == 2) {
		p.vx_cfStake *= 2;
		CS_Tell(p, sprintf("%s %g", CS_Text("Vxl_CoinFlip_BattingCost", "Amount Wagered"), p.vx_cfStake));
	} else if (choice == 3) {
		if (p.vx_cfStreak > 0) {
			VE_AddCoins(p, p.vx_cfStake);
			CS_Banner(VE_Fmt("Vxl_CoinFlip_Announce", "%s won the coin toss [%d] time(s).", p.netname, ftos(p.vx_cfStreak)));
			if (VE_PropF(r, "signal", 1)) {
				r.vb_state = 1;
				r.vb_act = p;
				r.vb_timer = time + 0.2;
				VE_Send(r, p);
			}
		}
		p.vx_cfStreak = 0;
		p.vx_cfStake = base * 2;
	}
}

/* ROCK_SCISSOR_PAPER (CVoxelRockScissorPaper): 0 rock, 1 scissors, 2 paper */
void
VE_RSP(entity r, entity p, float choice)
{
	string names[3];
	names[0] = CS_Text("Vxl_RockScissorPaper_Rock", "Rock");
	names[1] = CS_Text("Vxl_RockScissorPaper_Scissor", "Scissors");
	names[2] = CS_Text("Vxl_RockScissorPaper_Paper", "Paper");
	float bet = max(1, VE_PropF(r, "bet", 100));
	float srv = floor(random() * 3);
	if (srv == choice) {
		CS_Tell(p, sprintf("%s: %s / %s", CS_Text("Vxl_RockScissorPaper_Title", "Rock-Paper-Scissors"), names[choice], names[srv]));
		printf("studio %.1f: rsp %s %g vs %g draw\n", time, p.netname, choice, srv);
		return;
	}
	/* rock beats scissors, scissors beat paper, paper beats rock */
	if (mod(srv - choice + 3, 3) == 1) {
		float won = bet * (1 + floor(random() * 4));
		VE_AddCoins(p, won);
		CS_Banner(VE_Fmt3("Vxl_RockScissorPaper_Announce_Win", "%s won rock-paper-scissors with [%s] and received %d.", p.netname, names[choice], ftos(won)));
		CS_Tell(p, sprintf("+%g", won));
		printf("studio %.1f: rsp %s %g vs %g win %g\n", time, p.netname, choice, srv, won);
		if (VE_PropF(r, "signal", 1)) {
			r.vb_state = 1;
			r.vb_act = p;
			r.vb_timer = time + 0.2;
			VE_Send(r, p);
		}
	} else {
		VE_AddCoins(p, -bet);
		CS_Banner(VE_Fmt("Vxl_RockScissorPaper_Announce_Lose", "%s lost rock paper scissors with [%s].", p.netname, names[choice]));
		printf("studio %.1f: rsp %s %g vs %g lose\n", time, p.netname, choice, srv);
	}
}

/* LOTTERY (CVoxelLottery): 24 numbers, 2 per player, draw 60 s after a
   second player joins or when all are sold */
float
VE_LotOwner(entity r, float n)
{
	tokenize(r.vb_lot);
	return stof(argv(n));
}

void
VE_LotSet(entity r, float n, float owner)
{
	string out = "";
	tokenize(r.vb_lot);
	for (float i = 0; i < 24; i++) {
		string v = (i == n) ? ftos(owner) : argv(i);
		if (v == "")
			v = "0";
		out = i ? strcat(out, " ", v) : v;
	}
	if (r.vb_lot)
		strunzone(r.vb_lot);
	r.vb_lot = strzone(out);
}

void
VE_LotClear(entity r)
{
	if (r.vb_lot)
		strunzone(r.vb_lot);
	r.vb_lot = strzone("0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0");
}

void
VE_Lottery(entity r, entity p, float n)
{
	float price = max(1, VE_PropF(r, "price", 1000));
	float me = num_for_edict(p);
	float mine = 0, sold = 0, owners = 0;
	string seen = " ";

	if (r.vb_aux2 == 2) {
		CS_Tell(p, CS_Text("Vxl_Lottery_CantClientPlay_Restart", "Currently selecting number. Please use later."));
		return;
	}
	if (n < 0 || n > 23)
		return;
	if (VE_LotOwner(r, n)) {
		CS_Tell(p, CS_Text("Vxl_Lottery_CantSelect", "This number has already been chosen."));
		return;
	}
	for (float i = 0; i < 24; i++) {
		float o = VE_LotOwner(r, i);
		if (o == me)
			mine++;
	}
	if (mine >= max(1, VE_PropF(r, "max", 2))) {
		CS_Tell(p, CS_Text("Vxl_Lottery_CantMaxCnt", "You exceeded the max allowed number of entries."));
		return;
	}
	if (p.vx_coins < price) {
		CS_Tell(p, CS_Text("CSO_Voxel_NotEnoughMoney", "Insufficient coins!"));
		return;
	}
	VE_AddCoins(p, -price);
	r.vb_aux += price;	/* the pot */
	VE_LotSet(r, n, me);
	printf("studio %.1f: lottery %s took %02d, pot %g\n", time, p.netname, n + 1, r.vb_aux);

	for (float j = 0; j < 24; j++) {
		float o2 = VE_LotOwner(r, j);
		if (!o2)
			continue;
		sold++;
		if (strstrofs(seen, sprintf(" %g ", o2)) < 0) {
			seen = sprintf("%s%g ", seen, o2);
			owners++;
		}
	}
	if (sold >= 24) {
		r.vb_timer = time;
	} else if (owners >= 2 && !r.vb_aux2) {
		r.vb_aux2 = 1;
		r.vb_timer = time + 60;
		CS_Banner(VE_Fmt("Vxl_Lottery_RemainTime", "%d sec. left until the winner is announced.", "60", ""));
	} else if (owners < 2) {
		CS_Tell(p, CS_Text("Vxl_Lottery_WaittingTime", "Roulette will begin once 2 or more players enter."));
	}
}

/* the draw: the winner gets the pot; nobody holding the number carries it over */
void
VE_LotteryDraw(entity r)
{
	float n = floor(random() * 24);
	float o = VE_LotOwner(r, n);
	entity w = o ? edict_num(o) : world;
	r.vb_aux2 = 2;
	r.vb_timer = time + 5;
	CS_Banner(VE_Fmt("Vxl_Lottery_Announce", "Winning Number: %02d", ftos(n + 1), ""));
	if (w && (w.flags & FL_CLIENT)) {
		VE_AddCoins(w, r.vb_aux);
		CS_Banner(VE_Fmt("Vxl_Lottery_Announce_ForUser", "%s guessed the winning number %02d and won the roulette.", w.netname, ftos(n + 1)));
		r.vb_aux = 0;
	} else {
		CS_Banner(CS_Text("Vxl_Lottery_Announce_HoldOver", "No one guessed the winning number. Continuing to the next round."));
	}
	printf("studio %.1f: lottery draw %02d owner %g, pot now %g\n", time, n + 1, o, r.vb_aux);
}

/* ACCOUNT_CHECKER: pay the block's coins once per round, then it stays on */
void
VE_Pay(entity r, entity p)
{
	float cost = max(0, VE_PropF(r, "coin", 1));
	if (r.vb_state)
		return;
	if (p.vx_payAsk < time - 5 || p.vx_game != r) {
		p.vx_game = r;
		p.vx_payAsk = time;
		CS_Tell(p, sprintf("%s (%s %g) - use again or: cmd vx pay", VE_Fmt("Vxl_BattingUI_Query", "Pay %d coins?", ftos(cost), ""),
			CS_Text("Vxl_BattingUI_CurrentCoin", "Current Coins"), p.vx_coins));
		return;
	}
	p.vx_payAsk = 0;
	if (p.vx_coins < cost) {
		CS_Tell(p, CS_Text("CSO_Voxel_NotEnoughMoney", "Insufficient coins!"));
		return;
	}
	VE_AddCoins(p, -cost);
	VE_SetOut(r, 1, p);
	printf("studio %.1f: %s paid %g at the coin checker\n", time, p.netname, cost);
}

/* --- shots on blocks ----------------------------------------------------------------- */

#define VE_HITHP 100000

bool
VE_Hittable(string t)
{
	return t == "HIT_TARGET" || t == "HIT_BUTTON" || t == "HIT_EXPLOSIVE" || t == "BREAKABLE" || t == "COIN_FARM";
}

void
VE_MakeHitbox(entity r)
{
	entity h = ents.Create("csm_vxhitbox", r.origin);
	if (!h)
		return;
	setorigin(h, [0, 0, 0]);
	h.solid = SOLID_BBOX;
	h.movetype = MOVETYPE_NONE;
	h.takedamage = DAMAGE_YES;
	h.health = h.max_health = VE_HITHP;
	setsize(h, r.vb_mins - [1, 1, 1], r.vb_maxs + [1, 1, 1]);
	r.vb_hitbox = h;
}

/* who shot: an attacking player whose aim meets the block, else the nearest */
entity
VE_Shooter(entity r)
{
	entity best = world;
	float bestd = 999999999;
	vector mid = (r.vb_mins + r.vb_maxs) * 0.5;
	for (entity p = world; (p = next.Player(p));) {
		if (p.health <= 0)
			continue;
		makevectors(p.v_angle);
		vector src = p.origin + p.view_ofs;
		traceline(src, src + v_forward * 8192, MOVE_NORMAL, p);
		float d = vlen(mid - src);
		if (trace_ent == r.vb_hitbox)
			d -= 1000000;
		if (d < bestd) {
			bestd = d;
			best = p;
		}
	}
	return best;
}

void
VE_Hit(entity r, entity by, float dmg)
{
	string t = VE_Type(r);
	printf("studio %.1f: %s %s hit by %s for %g\n", time, t, VX_Key(r.vb_cell), by ? by.netname : "?", dmg);
	switch (t) {
	case "HIT_TARGET":
		if (r.vb_state == 1) {
			r.vb_state = 2;
			VE_Send(r, by);
		}
		break;
	case "HIT_BUTTON":
		if (!r.vb_state) {
			r.vb_state = 1;
			r.vb_act = world;
			r.vb_timer = time + 0.2;
			VE_Send(r, by);
		}
		break;
	case "HIT_EXPLOSIVE":
		VE_Explode(r, by);
		break;
	case "BREAKABLE":
		if (r.vb_state)
			break;
		r.vb_dmg += dmg;
		if (r.vb_dmg >= max(1, vx_hp[r.vb_type] ? vx_hp[r.vb_type] : VE_PropF(r, "hp", 100)))
			VE_Break(r, true);
		break;
	case "COIN_FARM":
		/* CVoxelCoinFarm slot 43: after the cooldown, coins per hit, up to its count */
		if (dmg <= 0 || time < r.vb_aux || !by || r.vb_hits >= bound(1, VE_PropF(r, "count", 9999), 9999))
			break;
		string tn = voxel_typename(r.vb_type);
		float def = strstrofs(tn, "GOLD") >= 0 ? 100 : (strstrofs(tn, "SILVER") >= 0 ? 10 : 1);
		VE_AddCoins(by, bound(1, VE_PropF(r, "coin", def), 9999));
		r.vb_aux = time + max(0, VE_PropF(r, "cooldown", 0.5));
		r.vb_hits++;
		if (r.vb_hits >= bound(1, VE_PropF(r, "count", 9999), 9999))
			VE_SetPresent(r, false);
		break;
	}
}

/* --- player interaction --------------------------------------------------------- */

/* E on a block (cmd csnz_use); true if something happened */
bool
VE_Use(entity r, entity p)
{
	string t = VE_Type(r);

	if (VE_PropF(r, "once", 0) && r.vb_used)
		return false;

	switch (t) {
	case "SWITCH":
		r.vb_used = 1;
		VE_Sound(r, "sound", "buttons/lever4.wav");
		VE_SetOut(r, !r.vb_state, p);
		return true;
	case "BUTTON":
		if (r.vb_state)
			return true;
		r.vb_used = 1;
		VE_Sound(r, "sound", "buttons/blip1.wav");
		r.vb_act = world;
		r.vb_timer = time + 0.2;
		VE_SetOut(r, 1, p);
		return true;
	case "DOOR":
		/* locked while linked and closed (FUN_10a6f760), unless unlocked */
		if (!r.vb_state && VE_InputCount(r) > 0 && VE_PropF(r, "locked", 1)) {
			CS_Tell(p, CS_Text("CSO_VoxelPropEdit_DoorShowLock", "[Locked]"));
			return true;
		}
		VE_Door(r, !r.vb_state, p);
		return true;
	case "COUNTDOWN":
		/* slot 4 (INFERRED: the use path) starts it too */
		if (!r.vb_state && !r.vb_timer)
			VE_CountdownStart(r, p);
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
		VE_Roll(r, p);
		return true;
	case "COIN_FLIP":
		p.vx_game = r;
		CS_Tell(p, sprintf("%s: %s %g, %s %g - cmd vx coinflip heads|tails|continue|stop",
			CS_Text("Vxl_CoinFlip_Title", "Coin Toss"), CS_Text("Vxl_CoinFlip_BattingCost", "Amount Wagered"),
			max(1, VE_PropF(r, "bet", 100)), CS_Text("Vxl_BattingUI_CurrentCoin", "Current Coins"), p.vx_coins));
		return true;
	case "ROCK_SCISSOR_PAPER":
		p.vx_game = r;
		CS_Tell(p, sprintf("%s: %s %g - cmd vx rsp rock|scissors|paper",
			CS_Text("Vxl_RockScissorPaper_Title", "Rock-Paper-Scissors"),
			CS_Text("Vxl_RockScissorPaper_BattingCost", "Invested Amount"), max(1, VE_PropF(r, "bet", 100))));
		return true;
	case "LOTTERY":
		p.vx_game = r;
		CS_Tell(p, sprintf("%s: %s %g - cmd vx lottery <1-24>", CS_Text("Vxl_Lottery_Title", "Lucky Roulette"),
			CS_Text("Vxl_Lottery_Dividend", "Cumulative amount obtainable"), r.vb_aux));
		return true;
	case "ACCOUNT_CHECKER":
		VE_Pay(r, p);
		return true;
	case "PORTAL":
		return false;	/* by touch */
	}
	return false;
}

/* cmd vx coinflip|rsp|lottery|pay: the coin game the player last used */
bool
VE_GameCommand(entity p, string c, string arg)
{
	entity r = p.vx_game;
	if (!vx_live || !r || wasfreed(r) || vlen(r.origin - p.origin) > 256) {
		CS_Tell(p, "Use a coin game block first.");
		return true;
	}
	string t = VE_Type(r);
	switch (c) {
	case "coinflip":
		if (t != "COIN_FLIP")
			break;
		float ch = -1;
		switch (arg) {
		case "heads": case "0": ch = 0; break;
		case "tails": case "1": ch = 1; break;
		case "continue": case "2": ch = 2; break;
		case "stop": case "3": ch = 3; break;
		}
		if (ch >= 0)
			VE_CoinFlip(r, p, ch);
		return true;
	case "rsp":
		if (t != "ROCK_SCISSOR_PAPER")
			break;
		float rc = -1;
		switch (arg) {
		case "rock": case "0": rc = 0; break;
		case "scissors": case "1": rc = 1; break;
		case "paper": case "2": rc = 2; break;
		}
		if (rc >= 0)
			VE_RSP(r, p, rc);
		return true;
	case "lottery":
		if (t != "LOTTERY")
			break;
		VE_Lottery(r, p, stof(arg) - 1);
		return true;
	case "pay":
		if (t != "ACCOUNT_CHECKER")
			break;
		p.vx_payAsk = time;
		VE_Pay(r, p);
		return true;
	}
	CS_Tell(p, "That block does not play that game.");
	return true;
}

/* --- per frame --------------------------------------------------------------------- */

void
VE_Timers(entity r, string t)
{
	if (!r.vb_timer || time < r.vb_timer)
		return;
	switch (t) {
	case "DELAY_GATE":
		/* FUN_10a60130: then follows the input again if it moved meanwhile */
		r.vb_timer = 0;
		VE_SetOut(r, r.vb_pending, r.vb_act);
		if (r.vb_prev != r.vb_state) {
			r.vb_pending = r.vb_prev;
			r.vb_timer = time + bound(0, VE_PropF(r, "delay", 1), 86400);
		}
		break;
	case "BLINK_GATE":
		r.vb_state = !r.vb_state;
		r.vb_timer = time + bound(1, VE_PropF(r, "interval", 2), 86400);
		VE_Send(r, r.vb_act);
		break;
	case "COUNTDOWN":
		r.vb_timer = 0;
		CS_Banner(sprintf("%s: 0", VX_GetProp(r, "title", "Countdown")));
		VE_SetOut(r, 1, r.vb_act);
		break;
	case "LINK_EXPLOSIVE":
		r.vb_timer = 0;
		VE_Explode(r, world);
		break;
	case "LOTTERY":
		if (r.vb_aux2 == 1) {
			VE_LotteryDraw(r);
		} else if (r.vb_aux2 == 2) {
			r.vb_aux2 = 0;
			r.vb_timer = 0;
			VE_LotClear(r);
		} else {
			r.vb_timer = 0;
			VE_LotteryDraw(r);	/* sold out */
		}
		break;
	default:
		/* end of a 0.2 s press (button, hit button, coin game signal) */
		r.vb_timer = 0;
		VE_SetOut(r, 0, world);
		break;
	}
}

void
VE_Frame(void)
{
	if (!vx_live)
		return;

	for (entity r = world; (r = find(r, ::classname, "vxblock"));) {
		string t = VE_Type(r);
		float total = 0, on = 0;

		VE_Timers(r, t);

		/* shots: the hitbox loses health, the block takes it as a hit */
		if (VE_Hittable(t) && r.vb_hitbox) {
			float dmg = VE_HITHP - r.vb_hitbox.health;
			if (dmg > 0) {
				r.vb_hitbox.health = VE_HITHP;
				VE_Hit(r, VE_Shooter(r), dmg);
			}
		}

		bool area = (t == "AREA" || t == "MONSTER_AREA" || t == "REPEATABLE_AREA" || t == "PRESSURE" || t == "PLAYER_CHECKER");
		bool zone = (t == "PUSH" || t == "FIXED_PUSH" || t == "CUSTOM_DAMAGE" || t == "HEAL_AREA" ||
			t == "GRAVITY_AREA" || t == "SPIKE_TRAP" || t == "SPIKE_WALL" || t == "SPINNING_BLADE" ||
			t == "PENDULUM_BLADE" || t == "FALLING_SPIKE" || t == "FLAME_TRAP" || t == "ARROW_TRAP" ||
			t == "DAMAGE_FIELD" || t == "PORTAL" || t == "EXIT" || t == "ITEM_SPAWN" || t == "ZOMBIE_SAFETY_ZONE");
		if (!area && !zone)
			continue;

		/* zones work while an input is on, or always when nothing drives them */
		if (zone) {
			VE_Inputs(r, total, on);
			if (total > 0 && !on && t != "PORTAL" && t != "EXIT" && t != "ITEM_SPAWN")
				continue;
		}

		float inside = 0;
		entity first = world;
		bool tick = time >= r.vb_tick;

		/* AREA / MONSTER_AREA filter (CVoxelArea 0xd4): 0 players, 1 monsters, 2 both */
		if (t == "AREA" || t == "MONSTER_AREA") {
			float who = VE_PropF(r, "filter", t == "MONSTER_AREA" ? 1 : 0);
			if (who != 0)
				for (entity m = world; (m = findfloat(m, ::flags, FL_MONSTER));)
					if (m.health > 0 && VE_Touches(r, m, 2)) {
						inside++;
						break;
					}
			if (who == 1)
				goto areadone;
		}

		for (entity p = world; (p = next.Player(p));) {
			if (p.health <= 0 || p.classname == "spectator")
				continue;
			float grow = (t == "DAMAGE_FIELD") ? VE_PropF(r, "radius", 1) * VX_BLOCK : 2;
			if (t == "PRESSURE") {
				/* standing on top: the block's columns, feet within 10 units of its top */
				if (!(p.absmin[0] <= r.vb_maxs[0] && p.absmax[0] >= r.vb_mins[0] &&
					p.absmin[1] <= r.vb_maxs[1] && p.absmax[1] >= r.vb_mins[1] &&
					p.absmin[2] <= r.vb_maxs[2] + 10 && p.absmax[2] >= r.vb_maxs[2]))
					continue;
			} else if (!VE_Touches(r, p, grow)) {
				continue;
			}
			inside++;
			if (!first)
				first = p;

			switch (t) {
			case "PLAYER_CHECKER":
				if (p.vx_check != r) {
					p.vx_check = r;
					printf("studio %.1f: %s checkpoint %s\n", time, p.netname, VX_Key(r.vb_cell));
				}
				break;
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
areadone:
		if (tick)
			r.vb_tick = time + VE_PropF(r, "period", 1);

		/* the damage field grows each second up to its end radius */
		if (t == "DAMAGE_FIELD" && tick) {
			float rad = VE_PropF(r, "radius", VE_PropF(r, "minradius", 1));
			rad = min(VE_PropF(r, "maxradius", 6), rad + VE_PropF(r, "step", 0.5));
			VX_SetProp(r, "radius", ftos(rad));
		}

		switch (t) {
		case "AREA":
		case "MONSTER_AREA":
			/* once per round (FUN_10a5a730) */
			if (inside && !r.vb_state) {
				VE_SetOut(r, 1, first);
				printf("studio %.1f: area %s entered\n", time, VX_Key(r.vb_cell));
			}
			break;
		case "REPEATABLE_AREA":
		case "PRESSURE":
			/* on at once; off when nobody is there at a 0.2 s check */
			if (inside && !r.vb_state) {
				if (t == "PRESSURE")
					VE_Sound(r, "sound", "buttons/latchlocked1.wav");
				r.vb_aux = time + 0.2;
				VE_SetOut(r, 1, first);
			} else if (r.vb_state && time >= r.vb_aux) {
				r.vb_aux = time + 0.2;
				if (!inside)
					VE_SetOut(r, 0, world);
			}
			break;
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

/* a respawned player starts at the checkpoint block (FUN_1145ed10) */
void
VE_Respawned(entity p)
{
	entity r = p.vx_check;
	if (!vx_live || !r || wasfreed(r))
		return;
	CS_MoveToSpot(p, r.origin + [0, 0, r.vb_dims[2] * VX_BLOCK * 0.5 + 40], p.v_angle);
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

/* starts the entity logic (round reset, slot 6): when a test, play or round begins */
void
VE_Activate(void)
{
	vx_live = true;
	vx_result = 0;
	VE_DefineVariants();
	for (entity q = world; (q = next.Player(q));) {
		q.vx_check = world;
		q.vx_game = world;
		q.vx_cfStreak = q.vx_cfStake = 0;
		if (!q.vx_coins)
			VE_SetCoins(q, autocvar_studio_startcoins);
	}
	for (entity r = world; (r = find(r, ::classname, "vxblock"));) {
		string t = VE_Type(r);
		VE_Bounds(r);
		VE_InClear(r);
		r.vb_state = r.vb_prev = r.vb_used = r.vb_timer = r.vb_hits = r.vb_dmg = 0;
		r.vb_aux = r.vb_aux2 = 0;
		r.vb_pick = r.vb_act = world;
		if (VE_Hittable(t))
			VE_MakeHitbox(r);
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
			/* monsters with no input spawn at the start */
			if (!VE_InputCount(r))
				VE_SpawnMonster(r);
			break;
		case "TOGGLE_GATE":
			r.vb_state = VE_PropF(r, "default", 0);
			break;
		case "HIT_TARGET":
			/* DESIGN: raised when nothing can raise it */
			r.vb_state = VE_InputCount(r) ? 0 : 1;
			break;
		case "LOTTERY":
			VE_LotClear(r);
			break;
		case "BLINK_GATE":
			/* 0xd0: blinks from the start */
			if (VE_PropF(r, "start", 0))
				r.vb_timer = time;
			break;
		case "COUNTDOWN":
			/* DESIGN: an unlinked countdown starts with the round */
			if (VE_PropF(r, "autostart", 1) && !VE_InputCount(r))
				VE_CountdownStart(r, world);
			break;
		}
	}
	/* the blocks whose reset (slot 6) sends tell their receivers, so a NOT
	   gate behind a switch starts on (CVoxelSwitch FUN_10a6e960, CVoxelButton
	   FUN_10a6d8f0, CVoxelCountDown, CVoxelHitTarget, CVoxelToggleGate ...) */
	for (entity g = world; (g = find(g, ::classname, "vxblock"));) {
		switch (VE_Type(g)) {
		case "SWITCH":
		case "BUTTON":
		case "COUNTDOWN":
		case "HIT_TARGET":
		case "ACCOUNT_CHECKER":
		case "TOGGLE_GATE":
		case "ONCE_GATE":
		case "DELAY_GATE":
			VE_Send(g, world);
			break;
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
		if (r.vb_hitbox && !wasfreed(r.vb_hitbox))
			remove(r.vb_hitbox);
		r.vb_hitbox = world;
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
		r.vb_act = world;
		r.vb_timer = time + 1;
		VE_SetOut(r, 1, p);
	} else {
		CS_Tell(p, "Wrong passcode.");
	}
}
