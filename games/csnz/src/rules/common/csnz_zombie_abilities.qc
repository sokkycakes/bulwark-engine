/* csnz_zombie_abilities: special abilities for the 18 zombie host types.
 * Original code (ISC).
 *
 * Each zombie class (csm_zombie_voodoo, csm_zombie_ganymede, etc.) gets a
 * distinct ability. The mode's frame hook calls ZA_Frame(), the damage
 * hook calls ZA_Damaged(), the kill hook calls ZA_Killed(), and the
 * command hook routes "csnz_ability" to ZA_UseAbility().
 *
 * Zombie type is identified by classname. Entity fields prefixed .za_
 * hold per-player cooldowns and effect timers. */

/* --- zombie type identifiers ---------------------------------------------- */

#define ZA_NONE         0
#define ZA_LIGHT        1
#define ZA_HEAVY        2
#define ZA_PSYCHO       3
#define ZA_VOODOO       4
#define ZA_DEIMOS       5
#define ZA_GANYMEDE     6
#define ZA_STAMPER      7
#define ZA_BANSHEE      8
#define ZA_VENOMGUARD   9
#define ZA_STINGFINGER  10
#define ZA_METATRON     11
#define ZA_LILITH       12
#define ZA_CHASER       13
#define ZA_BLOTTER      14
#define ZA_RUSTYWING    15
#define ZA_AKSHA        16
#define ZA_SOLDIER      17

/* --- entity fields -------------------------------------------------------- */

.float za_type;             /* ZA_* constant, set on spawn */
.float za_cooldown;         /* next time the active ability can be used */
.float za_poisonEnd;        /* venomguard: poison wears off at this time */
.float za_poisonSource;     /* entnum of the zombie that poisoned us */
.float za_fireEnd;          /* aksha: fire wears off at this time */
.float za_fireSource;       /* entnum of the zombie that ignited us */
.float za_stillSince;       /* lilith: time we last stopped moving */
.float za_wasOnGround;      /* stamper: were we on the ground last frame? */
.float za_sprintEnd;        /* chaser: sprint wears off at this time */
.float za_sprintSpeed;      /* chaser: speed multiplier during sprint */
.float za_lastOrigin_x;     /* lilith: position tracking */
.float za_lastOrigin_y;
.float za_lastOrigin_z;
.float za_isZombie;         /* set by ZA_BecomeZombie, cleared on a human (re)spawn */
.string za_choice;          /* host type picked with "csnz_zombieclass <type>" */

/* --- type lookup ---------------------------------------------------------- */

float
ZA_TypeForClass(string cls)
{
	if (cls == "csm_zombie_light")      return ZA_LIGHT;
	if (cls == "csm_zombie_heavy")      return ZA_HEAVY;
	if (cls == "csm_zombie_psycho")     return ZA_PSYCHO;
	if (cls == "csm_zombie_voodoo")     return ZA_VOODOO;
	if (cls == "csm_zombie_deimos")     return ZA_DEIMOS;
	if (cls == "csm_zombie_ganymede")   return ZA_GANYMEDE;
	if (cls == "csm_zombie_stamper")    return ZA_STAMPER;
	if (cls == "csm_zombie_banshee")    return ZA_BANSHEE;
	if (cls == "csm_zombie_venomguard") return ZA_VENOMGUARD;
	if (cls == "csm_zombie_stingfinger") return ZA_STINGFINGER;
	if (cls == "csm_zombie_metatron")   return ZA_METATRON;
	if (cls == "csm_zombie_lilith")     return ZA_LILITH;
	if (cls == "csm_zombie_chaser")     return ZA_CHASER;
	if (cls == "csm_zombie_blotter")    return ZA_BLOTTER;
	if (cls == "csm_zombie_rustywing")  return ZA_RUSTYWING;
	if (cls == "csm_zombie_aksha")      return ZA_AKSHA;
	if (cls == "csm_zombie_soldier")    return ZA_SOLDIER;
	return ZA_NONE;
}

/* called when a player becomes a zombie to set their type from class */
void
ZA_Init(entity p, string cls)
{
	p.za_type = ZA_TypeForClass(cls);
	p.za_cooldown = 0;
	p.za_poisonEnd = 0;
	p.za_fireEnd = 0;
	p.za_stillSince = time;
	p.za_wasOnGround = (p.flags & FL_ONGROUND) ? 1 : 0;
	p.za_sprintEnd = 0;
	p.za_lastOrigin_x = p.origin[0];
	p.za_lastOrigin_y = p.origin[1];
	p.za_lastOrigin_z = p.origin[2];

	/* lilith starts visible */
	if (p.za_type == ZA_LILITH)
		ents.Input(p, "Alpha", "1", world);

	/* soldier: publish health for enemy HUD reads */
	if (p.za_type == ZA_SOLDIER)
		userinfo.SetInteger(p, "csnz_showhealth", 1);
}

/* DESIGN: 1 gives every infected player a host type (their pick, else random
   for this infection); 0 keeps the plain csm_zombie classes */
var float autocvar_csnz_zombie_types = 1;
string za_typeNames = "light heavy psycho voodoo deimos ganymede stamper banshee venomguard stingfinger metatron lilith chaser blotter rustywing aksha soldier";

/* the host type class this player would become, or "" */
string
ZA_ClassFor(entity p)
{
	if (autocvar_csnz_zombie_types <= 0)
		return "";
	if (p.za_choice != "")
		return strcat("csm_zombie_", p.za_choice);
	float n = tokenize(za_typeNames);
	return strcat("csm_zombie_", argv(floor(random() * n)));
}

/* every mode turns a player into a zombie through here. A plain csm_zombie
   becomes the player's host type; an origin keeps its double health
   (DESIGN: csm_zombie_origin has 2000 to csm_zombie's 1000) */
void
ZA_BecomeZombie(entity p, string baseClass)
{
	string cls = baseClass;
	bool origin = (baseClass == "csm_zombie_origin");

	if (baseClass == "csm_zombie" || origin) {
		string typed = ZA_ClassFor(p);
		if (typed != "")
			cls = typed;
	}
	ents.ChangeToClass(p, cls);
	if (origin && cls != baseClass) {
		ents.Input(p, "SetMaxHealth", ftos(p.max_health * 2), world);
		ents.Input(p, "SetHealth", ftos(p.health * 2), world);
	}
	p.za_isZombie = 1;
	CA_SetCharacter(p, "");	/* human character abilities stop */
	ZA_Init(p, cls);
	p.zv_lastHealth = p.health;
	printf("csnz %.1f: %s is a %s (type %g)\n", time, p.netname, cls, p.za_type);
}

/* --- poison cloud (blotter death cloud) ----------------------------------- */

/* a temporary entity that deals area damage each frame */
.float za_cloudEnd;
.float za_cloudDamage;
.float za_cloudRadius;
.float za_cloudTeam;

void
ZA_CloudThink(void)
{
	if (time >= self.za_cloudEnd) {
		remove(self);
		return;
	}

	for (entity p = world; (p = next.Player(p));) {
		if (!CS_Playing(p))
			continue;
		if (p.team == self.za_cloudTeam)
			continue;
		if (vlen(p.origin - self.origin) > self.za_cloudRadius)
			continue;
		float dmg = self.za_cloudDamage * frametime;
		ents.Input(p, "SetHealth", ftos(max(1, p.health - dmg)), world);
	}

	self.nextthink = time + frametime;
}

void
ZA_SpawnCloud(vector where, float teamNum, float damage, float duration, float radius)
{
	entity cloud = spawn();
	cloud.classname = "csnz_poisoncloud";
	setorigin(cloud, where);
	cloud.za_cloudEnd = time + duration;
	cloud.za_cloudDamage = damage;
	cloud.za_cloudRadius = radius;
	cloud.za_cloudTeam = teamNum;
	cloud.think = ZA_CloudThink;
	cloud.nextthink = time + frametime;
}

/* --- 1. Voodoo: passive heal aura ---------------------------------------- */

void
ZA_VoodooAura(entity p)
{
	for (entity ally = world; (ally = next.Player(ally));) {
		if (ally == p || !CS_Playing(ally))
			continue;
		if (ally.team != CS_TEAM1)
			continue;
		if (vlen(ally.origin - p.origin) > 200)
			continue;
		if (ally.health < ally.max_health) {
			float heal = 5 * frametime;
			ents.Input(ally, "SetHealth", ftos(min(ally.max_health, ally.health + heal)), world);
		}
	}
}

/* --- 2. Ganymede: glide (reduced gravity in air) -------------------------- */

void
ZA_GanymedeGlide(entity p)
{
	if (!(p.flags & FL_ONGROUND)) {
		/* reduce downward velocity to simulate 0.3 gravity */
		if (p.velocity[2] < 0)
			p.velocity[2] *= 0.3;
	}
}

/* --- 3. Stamper: ground stomp on landing ---------------------------------- */

void
ZA_StamperCheck(entity p)
{
	bool onGround = (p.flags & FL_ONGROUND) ? true : false;
	bool wasOnGround = p.za_wasOnGround ? true : false;

	if (onGround && !wasOnGround && time >= p.za_cooldown) {
		/* just landed: stomp */
		for (entity victim = world; (victim = next.Player(victim));) {
			if (victim == p || !CS_Playing(victim))
				continue;
			if (victim.team == CS_TEAM1)
				continue;
			if (vlen(victim.origin - p.origin) > 150)
				continue;
			ents.Input(victim, "SetHealth", ftos(max(1, victim.health - 30)), world);
			/* small knockup */
			vector push = normalize(victim.origin - p.origin);
			push[2] = 0.5;
			ents.Input(victim, "AddVelocity", vtos(push * 200), world);
		}
		p.za_cooldown = time + 3;
	}

	p.za_wasOnGround = onGround ? 1 : 0;
}

/* --- 4. Banshee: ranged scream (use key) ---------------------------------- */

void
ZA_BansheeScream(entity p)
{
	if (time < p.za_cooldown) {
		CS_Tell(p, sprintf("Scream recharging: %is", (int)ceil(p.za_cooldown - time)));
		return;
	}

	makevectors(p.v_angle);

	for (entity victim = world; (victim = next.Player(victim));) {
		if (victim == p || !CS_Playing(victim))
			continue;
		if (victim.team == CS_TEAM1)
			continue;

		vector toVictim = normalize(victim.origin - p.origin);
		float dist = vlen(victim.origin - p.origin);

		if (dist > 300)
			continue;

		/* cone check: 45 degree half-angle */
		if (toVictim * v_forward < 0.707)
			continue;

		ents.Input(victim, "SetHealth", ftos(max(1, victim.health - 50)), world);
		/* push victims away */
		ents.Input(victim, "AddVelocity", vtos(toVictim * 300), world);
	}

	p.za_cooldown = time + 5;
	CS_Tell(p, "Scream!");
}

/* --- 5. Venomguard: poison on melee hit ----------------------------------- */

void
ZA_VenomguardApply(entity victim, entity attacker)
{
	victim.za_poisonEnd = time + 5;
	victim.za_poisonSource = num_for_edict(attacker);
}

void
ZA_PoisonTick(entity p)
{
	if (p.za_poisonEnd <= 0 || time >= p.za_poisonEnd) {
		p.za_poisonEnd = 0;
		return;
	}

	/* 3 damage per second */
	float dmg = 3 * frametime;
	ents.Input(p, "SetHealth", ftos(max(1, p.health - dmg)), world);
}

/* --- 6. Stingfinger: speed burst on kill ---------------------------------- */

void
ZA_StingfingerBurst(entity p)
{
	/* 50% speed boost for 2 seconds after a kill */
	p.za_sprintEnd = time + 2;
	p.za_sprintSpeed = 1.5;
}

void
ZA_SprintTick(entity p)
{
	if (p.za_sprintEnd <= 0 || time >= p.za_sprintEnd) {
		p.za_sprintEnd = 0;
		p.za_sprintSpeed = 0;
		return;
	}

	float boost = p.za_sprintSpeed;
	p.velocity[0] *= boost;
	p.velocity[1] *= boost;
}

/* --- 7. Metatron: boss aura ----------------------------------------------- */

void
ZA_MetatronAura(entity p)
{
	/* damage boost is handled in ZA_Damaged via the aura check;
	   here we just mark nearby zombies with a serverinfo flag for HUD */
	for (entity ally = world; (ally = next.Player(ally));) {
		if (ally == p || !CS_Playing(ally))
			continue;
		if (ally.team != CS_TEAM1)
			continue;
		if (vlen(ally.origin - p.origin) <= 400)
			userinfo.SetInteger(ally, "csnz_boosted", 1);
		else
			userinfo.SetInteger(ally, "csnz_boosted", 0);
	}
}

/* check if any Metatron is boosting attacker's damage */
float
ZA_MetatronDamageBoost(entity attacker)
{
	if (attacker.team != CS_TEAM1)
		return 1.0;

	for (entity m = world; (m = next.Player(m));) {
		if (m == attacker || !CS_Playing(m))
			continue;
		if (m.za_type != ZA_METATRON)
			continue;
		if (vlen(m.origin - attacker.origin) <= 400)
			return 1.2;	/* +20% damage */
	}
	return 1.0;
}

/* --- 8. Lilith: stealth --------------------------------------------------- */

void
ZA_LilithStealth(entity p)
{
	vector lastPos;
	lastPos[0] = p.za_lastOrigin_x;
	lastPos[1] = p.za_lastOrigin_y;
	lastPos[2] = p.za_lastOrigin_z;

	float moved = vlen(p.origin - lastPos);

	if (moved > 2) {
		p.za_stillSince = time;
		ents.Input(p, "Alpha", "1", world);
	} else if (time - p.za_stillSince >= 2) {
		/* invisible after standing still for 2 seconds */
		ents.Input(p, "Alpha", "0.15", world);
	}

	p.za_lastOrigin_x = p.origin[0];
	p.za_lastOrigin_y = p.origin[1];
	p.za_lastOrigin_z = p.origin[2];
}

/* lilith becomes visible when attacking */
void
ZA_LilithReveal(entity p)
{
	ents.Input(p, "Alpha", "1", world);
	p.za_stillSince = time;
}

/* --- 9. Chaser: sprint (use key) ------------------------------------------ */

void
ZA_ChaserSprint(entity p)
{
	if (time < p.za_cooldown) {
		CS_Tell(p, sprintf("Sprint recharging: %is", (int)ceil(p.za_cooldown - time)));
		return;
	}

	p.za_sprintEnd = time + 3;
	p.za_sprintSpeed = 2.0;
	p.za_cooldown = time + 8;
	CS_Tell(p, "Sprint!");
}

/* --- 10. Blotter: poison cloud on death ----------------------------------- */

void
ZA_BlotterDeath(entity p)
{
	ZA_SpawnCloud(p.origin, CS_TEAM1, 10, 5, 128);
}

/* --- 11. Rustywing: glide with horizontal boost --------------------------- */

void
ZA_RustywingGlide(entity p)
{
	if (!(p.flags & FL_ONGROUND)) {
		/* slow fall */
		if (p.velocity[2] < 0)
			p.velocity[2] *= 0.3;

		/* horizontal speed boost while airborne */
		float hspeed = sqrt(p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1]);
		if (hspeed > 0 && hspeed < 400) {
			float boost = 1 + (2.0 * frametime);
			p.velocity[0] *= boost;
			p.velocity[1] *= boost;
		}
	}
}

/* --- 12. Aksha: fire on melee hit, spreads -------------------------------- */

void
ZA_AkshaApply(entity victim, entity attacker)
{
	victim.za_fireEnd = time + 4;
	victim.za_fireSource = num_for_edict(attacker);
}

void
ZA_FireTick(entity p)
{
	if (p.za_fireEnd <= 0 || time >= p.za_fireEnd) {
		p.za_fireEnd = 0;
		return;
	}

	/* 5 damage per second */
	float dmg = 5 * frametime;
	ents.Input(p, "SetHealth", ftos(max(1, p.health - dmg)), world);

	/* fire spreads to nearby enemies within 64 units */
	for (entity nearby = world; (nearby = next.Player(nearby));) {
		if (nearby == p || !CS_Playing(nearby))
			continue;
		if (nearby.team == CS_TEAM1)
			continue;
		if (nearby.za_fireEnd > time)
			continue;	/* already burning */
		if (vlen(nearby.origin - p.origin) <= 64) {
			nearby.za_fireEnd = time + 4;
			nearby.za_fireSource = p.za_fireSource;
		}
	}
}

/* --- 13. Soldier: tactical (reduced back damage, health publishing) ------- */

float
ZA_SoldierDamageReduction(entity victim, entity attacker)
{
	if (victim.za_type != ZA_SOLDIER)
		return 1.0;

	/* check if damage came from behind: dot product of facing vs attack dir */
	makevectors(victim.v_angle);
	vector atkDir = normalize(attacker.origin - victim.origin);
	float dot = v_forward * atkDir;

	/* behind: attacker is in the direction the victim faces away from */
	if (dot < -0.3)
		return 0.9;	/* 10% less damage from behind */

	return 1.0;
}

/* publish health so the HUD can show enemy health bars */
void
ZA_SoldierPublish(entity p)
{
	/* soldiers see other players' health via userinfo; here we publish
	   the soldier's own current health for everyone's HUD */
	userinfo.SetInteger(p, "csnz_hp", (int)p.health);
}

/* --- active abilities (use key: "cmd csnz_ability") ----------------------- */

bool
ZA_UseAbility(entity p)
{
	if (!p || !CS_Playing(p) || p.team != CS_TEAM1)
		return false;

	switch ((int)p.za_type) {
	case ZA_BANSHEE:
		ZA_BansheeScream(p);
		return true;
	case ZA_CHASER:
		ZA_ChaserSprint(p);
		return true;
	default:
		return false;
	}
}

/* --- frame hook: called every frame while live ---------------------------- */

void
ZA_Frame(void)
{
	for (entity p = world; (p = next.Player(p));) {
		if (!CS_Playing(p))
			continue;

		p.zv_lastHealth = p.health;

		/* --- effects that apply TO any player (not just zombies) --- */

		/* poison damage tick (venomguard victims) */
		if (p.za_poisonEnd > 0)
			ZA_PoisonTick(p);

		/* fire damage tick (aksha victims) */
		if (p.za_fireEnd > 0)
			ZA_FireTick(p);

		/* sprint/speed burst tick (chaser, stingfinger) */
		if (p.za_sprintEnd > 0)
			ZA_SprintTick(p);

		/* --- zombie-only abilities --- */

		if (p.team != CS_TEAM1)
			continue;

		switch ((int)p.za_type) {
		case ZA_VOODOO:
			ZA_VoodooAura(p);
			break;
		case ZA_GANYMEDE:
			ZA_GanymedeGlide(p);
			break;
		case ZA_STAMPER:
			ZA_StamperCheck(p);
			break;
		case ZA_METATRON:
			ZA_MetatronAura(p);
			break;
		case ZA_LILITH:
			ZA_LilithStealth(p);
			break;
		case ZA_RUSTYWING:
			ZA_RustywingGlide(p);
			break;
		case ZA_SOLDIER:
			ZA_SoldierPublish(p);
			break;
		default:
			break;
		}
	}
}

/* --- damage hook: called when a victim survives a hit --------------------- */

void
ZA_Damaged(entity victim, entity attacker)
{
	if (!(attacker.flags & FL_CLIENT) || attacker == victim)
		return;

	/* zombie attacking a human */
	if (attacker.team == CS_TEAM1 && victim.team != CS_TEAM1) {
		/* Metatron aura: +20% damage from boosted zombie attackers.
		   We apply extra damage after the main hit. */
		float boost = ZA_MetatronDamageBoost(attacker);
		if (boost > 1.0) {
			float dealt = victim.zv_lastHealth - victim.health;
			if (dealt > 0) {
				float extra = dealt * (boost - 1.0);
				ents.Input(victim, "SetHealth", ftos(max(1, victim.health - extra)), world);
			}
		}

		/* Metatron self: 25% damage reduction (handled below for incoming) */

		/* venomguard: melee hits apply poison */
		if (attacker.za_type == ZA_VENOMGUARD)
			ZA_VenomguardApply(victim, attacker);

		/* aksha: melee hits ignite */
		if (attacker.za_type == ZA_AKSHA)
			ZA_AkshaApply(victim, attacker);

		/* lilith: attacking reveals */
		if (attacker.za_type == ZA_LILITH)
			ZA_LilithReveal(attacker);
	}

	/* human attacking a zombie */
	if (attacker.team != CS_TEAM1 && victim.team == CS_TEAM1) {
		/* Metatron: self takes 25% less damage */
		if (victim.za_type == ZA_METATRON) {
			float dealtM = victim.zv_lastHealth - victim.health;
			if (dealtM > 0) {
				float restoredM = dealtM * 0.25;
				ents.Input(victim, "SetHealth", ftos(min(victim.max_health, victim.health + restoredM)), world);
			}
		}

		/* Soldier: 10% less damage from behind */
		if (victim.za_type == ZA_SOLDIER) {
			float reduction = ZA_SoldierDamageReduction(victim, attacker);
			if (reduction < 1.0) {
				float dealtS = victim.zv_lastHealth - victim.health;
				if (dealtS > 0) {
					float restoredS = dealtS * (1.0 - reduction);
					ents.Input(victim, "SetHealth", ftos(min(victim.max_health, victim.health + restoredS)), world);
				}
			}
		}

		/* Lilith: taking damage reveals */
		if (victim.za_type == ZA_LILITH)
			ZA_LilithReveal(victim);
	}
}

/* --- kill hook: called when a player dies ---------------------------------- */

void
ZA_Killed(entity victim, entity inflictor, entity attacker, string weapon)
{
	/* Blotter: poison cloud on death */
	if (victim.team == CS_TEAM1 && victim.za_type == ZA_BLOTTER)
		ZA_BlotterDeath(victim);

	/* Stingfinger: speed burst on kill */
	if ((attacker.flags & FL_CLIENT) && attacker.team == CS_TEAM1
	    && attacker.za_type == ZA_STINGFINGER && attacker != victim)
		ZA_StingfingerBurst(attacker);

	/* clear effects on the victim */
	victim.za_poisonEnd = 0;
	victim.za_fireEnd = 0;
	victim.za_sprintEnd = 0;

	/* restore alpha for lilith corpse visibility */
	if (victim.za_type == ZA_LILITH)
		ents.Input(victim, "Alpha", "1", world);
}

/* --- command hook: route "csnz_ability" from M_Command -------------------- */

bool
ZA_Command(entity p, string cmd)
{
	tokenize(cmd);
	if (argv(0) == "csnz_zombieclass") {
		string want = argv(1);
		if (want == "" || want == "random") {
			p.za_choice = "";
			sprint(p, PRINT_HIGH, "Zombie class: random\n");
			return true;
		}
		if (ZA_TypeForClass(strcat("csm_zombie_", want)) == ZA_NONE) {
			sprint(p, PRINT_HIGH, sprintf("Zombie classes: %s, random\n", za_typeNames));
			return true;
		}
		p.za_choice = strzone(want);
		sprint(p, PRINT_HIGH, sprintf("Zombie class: %s (from your next infection)\n", want));
		return true;
	}
	if (argv(0) != "csnz_ability")
		return false;
	return ZA_UseAbility(p);
}
