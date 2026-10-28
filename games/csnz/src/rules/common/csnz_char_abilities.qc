/* csnz_char_abilities: character buff class ability system.
 *
 * Each character class from characters.def has passive and/or active abilities
 * beyond their base stat modifiers. This file implements the runtime effects.
 *
 * Original code (ISC). DESIGN ability effects - approximations of CSNZ's
 * buff class system based on binary evidence (ClassMod.csv, buffclass* strings). */

/* --- character ability IDs --- */
#define CHAR_NONE		0
#define CHAR_FERNANDO	1
#define CHAR_BLAIR		2
#define CHAR_PHOENIX	3
#define CHAR_YURI		4
#define CHAR_LYCAN		5
#define CHAR_HUNTER		6
#define CHAR_PIRATE		7
#define CHAR_HEAVY_SOL	8
#define CHAR_SPECIAL	9
#define CHAR_ASSASSIN	10
#define CHAR_COMMANDO	11
#define CHAR_OPERATOR	12
#define CHAR_MEDIC		13
#define CHAR_ENGINEER	14
#define CHAR_SNIPER_PRO	15
#define CHAR_DEMOLITION	16
#define CHAR_RANGER		17
#define CHAR_SHADOW		18
#define CHAR_GUARDIAN	19
#define CHAR_BERSERKER	20

/* per-player ability state */
.float ca_type;			/* character ability ID */
.float ca_cooldown;		/* ability cooldown timer */
.float ca_active_end;		/* active ability duration end */
.float ca_passive_timer;	/* passive tick timer */
.float ca_heal_pool;		/* medic heal pool remaining */
.float ca_kills_streak;		/* kill streak for streak-based abilities */

/* --- passive abilities (run every frame) --- */

void
CA_PassiveTick(entity p)
{
	if (p.health <= 0 || !CS_Playing(p))
		return;

	switch ((int)p.ca_type) {
	case CHAR_MEDIC:
		/* passive: regenerate 2 HP/sec, regenerate nearby allies 1 HP/sec */
		if (time >= p.ca_passive_timer) {
			p.ca_passive_timer = time + 1;
			if (p.health < 100)
				ents.Input(p, "SetHealth", ftos(min(100, p.health + 2)), world);
			/* heal nearby allies */
			for (entity ally = world; (ally = next.Player(ally));) {
				if (ally == p || ally.team != p.team || !CS_Playing(ally))
					continue;
				if (vlen(ally.origin - p.origin) <= 200 && ally.health < 100) {
					ents.Input(ally, "SetHealth", ftos(min(100, ally.health + 1)), world);
					p.ca_heal_pool += 1;
					ACH_OnHeal(p, 1);
				}
			}
		}
		break;

	case CHAR_RANGER:
		/* passive: regenerate 1 HP/sec in the field */
		if (time >= p.ca_passive_timer) {
			p.ca_passive_timer = time + 1;
			if (p.health < 110)
				ents.Input(p, "SetHealth", ftos(min(110, p.health + 1)), world);
		}
		break;

	case CHAR_ASSASSIN:
	case CHAR_SHADOW:
		/* passive: reduced visibility when crouching (alpha) */
		if (p.flags & FL_ONGROUND && vlen(p.velocity) < 10) {
			ents.Input(p, "Alpha", "0.3", world);
		} else {
			ents.Input(p, "Alpha", "1", world);
		}
		break;

	case CHAR_GUARDIAN:
		/* passive: damage reduction aura for nearby allies (tracked via userinfo) */
		if (time >= p.ca_passive_timer) {
			p.ca_passive_timer = time + 2;
			int nearby = 0;
			for (entity ally = world; (ally = next.Player(ally));) {
				if (ally == p || ally.team != p.team || !CS_Playing(ally))
					continue;
				if (vlen(ally.origin - p.origin) <= 300)
					nearby++;
			}
			userinfo.SetInteger(p, "csnz_aura_allies", nearby);
		}
		break;
	}
}

/* --- active abilities (triggered by csnz_ability command) --- */

bool
CA_UseAbility(entity p)
{
	if (p.health <= 0 || !CS_Playing(p))
		return false;
	if (time < p.ca_cooldown)
		return false;

	switch ((int)p.ca_type) {
	case CHAR_BLAIR:
		/* active: speed burst for 5 seconds, 15s cooldown */
		p.ca_cooldown = time + 15;
		p.ca_active_end = time + 5;
		sprint(p, PRINT_HIGH, "Speed Boost activated!\n");
		return true;

	case CHAR_PHOENIX:
		/* active: damage boost for 5 seconds (+30%), 20s cooldown */
		p.ca_cooldown = time + 20;
		p.ca_active_end = time + 5;
		sprint(p, PRINT_HIGH, "Damage Boost activated!\n");
		return true;

	case CHAR_LYCAN:
	case CHAR_BERSERKER:
		/* active: melee frenzy for 4 seconds (faster melee, +50% melee damage), 12s cooldown */
		p.ca_cooldown = time + 12;
		p.ca_active_end = time + 4;
		sprint(p, PRINT_HIGH, "Melee Frenzy activated!\n");
		return true;

	case CHAR_HUNTER:
		/* active: mark all enemies visible through walls for 3 seconds, 25s cooldown */
		p.ca_cooldown = time + 25;
		p.ca_active_end = time + 3;
		/* publish enemy positions via userinfo */
		for (entity e = world; (e = next.Player(e));) {
			if (e.team != p.team && CS_Playing(e))
				userinfo.SetString(e, "csnz_marked", "1");
		}
		sprint(p, PRINT_HIGH, "Enemy positions revealed!\n");
		return true;

	case CHAR_COMMANDO:
		/* active: armor repair - restore 50 armor, 30s cooldown */
		p.ca_cooldown = time + 30;
		ents.Input(p, "SetArmor", ftos(min(200, p.armorvalue + 50)), world);
		sprint(p, PRINT_HIGH, "Armor repaired!\n");
		return true;

	case CHAR_DEMOLITION:
		/* active: throw an extra grenade, 20s cooldown */
		p.ca_cooldown = time + 20;
		ents.Input(p, "GiveItem", "weapon_hegrenade", world);
		sprint(p, PRINT_HIGH, "Extra grenade acquired!\n");
		return true;

	case CHAR_ENGINEER:
		/* active: deploy ammo supply for team (refill nearby allies), 30s cooldown */
		p.ca_cooldown = time + 30;
		for (entity ally = world; (ally = next.Player(ally));) {
			if (ally.team == p.team && CS_Playing(ally) && vlen(ally.origin - p.origin) <= 200)
				CS_FillAmmo(ally);
		}
		sprint(p, PRINT_HIGH, "Ammo supply deployed!\n");
		return true;

	case CHAR_SNIPER_PRO:
		/* active: steady aim for 5 seconds (no sway), 15s cooldown */
		p.ca_cooldown = time + 15;
		p.ca_active_end = time + 5;
		sprint(p, PRINT_HIGH, "Steady Aim activated!\n");
		return true;

	default:
		sprint(p, PRINT_HIGH, "No active ability for this character.\n");
		return false;
	}
}

/* --- damage modifier hook --- */

float
CA_DamageModifier(entity attacker, entity victim)
{
	float mod = 1.0;

	/* attacker buffs */
	if (attacker.ca_active_end > time) {
		if (attacker.ca_type == CHAR_PHOENIX)
			mod *= 1.3;	/* +30% damage during active */
	}

	/* victim damage reduction */
	if (victim.ca_type == CHAR_GUARDIAN)
		mod *= 0.9;	/* 10% damage reduction */
	if (victim.ca_type == CHAR_HEAVY_SOL)
		mod *= 0.95;	/* 5% damage reduction */

	/* melee bonus for melee classes during frenzy */
	if (attacker.ca_active_end > time &&
		(attacker.ca_type == CHAR_LYCAN || attacker.ca_type == CHAR_BERSERKER))
		mod *= 1.5;

	return mod;
}

/* --- on kill hook --- */

void
CA_OnKill(entity killer)
{
	killer.ca_kills_streak++;

	/* Assassin: temporary speed boost on kill */
	if (killer.ca_type == CHAR_ASSASSIN) {
		killer.ca_active_end = time + 2;
	}

	/* Pirate Girl: money bonus on kill */
	if (killer.ca_type == CHAR_PIRATE) {
		PROG_AwardMoney(killer, 100);	/* extra 100 money per kill */
	}
}

/* --- resolve character class from equipped character --- */

void
CA_SetCharacter(entity p, string charDef)
{
	if (charDef == "csm_char_fernando") p.ca_type = CHAR_FERNANDO;
	else if (charDef == "csm_char_blair") p.ca_type = CHAR_BLAIR;
	else if (charDef == "csm_char_phoenix") p.ca_type = CHAR_PHOENIX;
	else if (charDef == "csm_char_yuri") p.ca_type = CHAR_YURI;
	else if (charDef == "csm_char_lycan") p.ca_type = CHAR_LYCAN;
	else if (charDef == "csm_char_hunter") p.ca_type = CHAR_HUNTER;
	else if (charDef == "csm_char_pirategirl") p.ca_type = CHAR_PIRATE;
	else if (charDef == "csm_char_heavysoldier") p.ca_type = CHAR_HEAVY_SOL;
	else if (charDef == "csm_char_specialagent") p.ca_type = CHAR_SPECIAL;
	else if (charDef == "csm_char_assassin") p.ca_type = CHAR_ASSASSIN;
	else if (charDef == "csm_char_commando") p.ca_type = CHAR_COMMANDO;
	else if (charDef == "csm_char_operator") p.ca_type = CHAR_OPERATOR;
	else if (charDef == "csm_char_medic") p.ca_type = CHAR_MEDIC;
	else if (charDef == "csm_char_engineer") p.ca_type = CHAR_ENGINEER;
	else if (charDef == "csm_char_sniperpro") p.ca_type = CHAR_SNIPER_PRO;
	else if (charDef == "csm_char_demolition") p.ca_type = CHAR_DEMOLITION;
	else if (charDef == "csm_char_ranger") p.ca_type = CHAR_RANGER;
	else if (charDef == "csm_char_shadow") p.ca_type = CHAR_SHADOW;
	else if (charDef == "csm_char_guardian") p.ca_type = CHAR_GUARDIAN;
	else if (charDef == "csm_char_berserker") p.ca_type = CHAR_BERSERKER;
	else p.ca_type = CHAR_NONE;

	p.ca_cooldown = 0;
	p.ca_active_end = 0;
	p.ca_passive_timer = time;
	p.ca_kills_streak = 0;
}

/* the equipped character's abilities for a human (re)spawn */
void
CA_ApplyLoadout(entity p)
{
	CA_SetCharacter(p, p.inv_equipped_char);
}

/* --- frame update for all players --- */

void
CA_Frame(void)
{
	for (entity p = world; (p = next.Player(p));) {
		if (p.ca_type != CHAR_NONE)
			CA_PassiveTick(p);

		/* clear expired active effects */
		if (p.ca_active_end && time >= p.ca_active_end) {
			p.ca_active_end = 0;
			/* reset any visual effects */
			if (p.ca_type == CHAR_HUNTER) {
				for (entity e = world; (e = next.Player(e));)
					if (e.team != p.team)
						userinfo.SetString(e, "csnz_marked", "0");
			}
		}
	}
}

/* --- client command --- */

bool
CA_Command(entity p, string cmd)
{
	tokenize(cmd);
	if (argv(0) == "csnz_ability") {
		return CA_UseAbility(p);
	}
	return false;
}
