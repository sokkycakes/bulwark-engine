/* Bulwark engine - the weapon in hand as Nuclide's game code runs it, in C, for
 * the DS link's prediction. ISC licence, see LICENSE.
 *
 * A port of what the PC client runs for its own player each command
 * (nuclide 9qaek1 733a5a1, the co-op game's valve/csprogs.dat):
 * ncPlayer::Physics_Fall's punch (src/shared/physics/player_pmove.qc:37-59),
 * Physics_InputPostMove's timers and punch (player_pmove.qc:704-747) and
 * ncPlayer::ProcessInput's weapon part (src/shared/game/Player.qc:341-462),
 * which runs ncWeapon (src/shared/game/Weapon.qc). Valve's hlWeapon
 * (valve/src/shared/Weapon.qc) overrides none of the weapon logic, so every
 * co-op weapon runs ncWeapon with its decl's values, which WEAPONINFO
 * carries. Names follow the QC; line numbers are Weapon.qc's unless said.
 *
 * Only the CLIENT side of the QC is here: sounds, projectiles, brass,
 * cooking times and the like are the server's. Left out because the device
 * can't know them (the server's state puts them right): DetonateDef (a
 * satchel's charges), the timed ammo regen and deplete (they read `time`),
 * g_infiniteAmmo 2's refill, the zoom (server-only in the QC too), Holster
 * and weapon switches (the server says which weapon is in hand).
 *
 * Floats are single precision and every constant is a float, as in QC, so a
 * command gives the server's result bit for bit (tools/nds/weptest). */
#include "weapon_nds.h"

#include <math.h>
#include <string.h>

/* --- the wire --- */

static unsigned rd16(const uint8_t *d) { return d[0] | d[1] << 8; }
static float rdf(const uint8_t *d)
{
	uint32_t u = rd16(d) | (uint32_t)rd16(d + 2) << 16;
	float f;
	memcpy(&f, &u, 4);
	return f;
}

#define NEED(n)                                                                                                   \
	do {                                                                                                          \
		if (p + (n) > end)                                                                                        \
			return false;                                                                                         \
	} while (0)

static bool parse_fire(wfire_t *f, const uint8_t **pp, const uint8_t *end, uint8_t *wflags)
{
	const uint8_t *p = *pp;
	float *v[25] = {&f->punch[0], &f->punch[1], &f->punch[2], &f->spring[0], &f->spring[1], &f->spring[2],
	                &f->punchkick, &f->firerate, &f->chargetime, &f->overheat, &f->overheatlen, &f->burstcount,
	                &f->bursttime, &f->knockback, &f->knockbackrel, &f->testdist, &f->spinup, &f->reloadtime,
	                &f->reloadstarttime, &f->zoomfov, &f->failrate, &f->sweepangle, &f->sweeprange, &f->regenrate,
	                &f->depleterate};
	int nacts;
	memset(f, 0, sizeof(*f));
	NEED(7 + 25 * 4 + 1);
	f->flags = (uint16_t)rd16(p), f->ammotype = (int8_t)p[2];
	f->ammopershot = (int16_t)rd16(p + 3), f->depleteammo = (int16_t)rd16(p + 5);
	p += 7;
	for (int i = 0; i < 25; i++, p += 4)
		*v[i] = rdf(p);
	nacts = *p++;
	for (int i = 0; i < nacts; i++) {
		int a, n;
		NEED(2);
		a = p[0], n = p[1];
		p += 2;
		NEED(n * 2);
		for (int k = 0; k < n; k++, p += 2)
			if (a < WA_COUNT && k < W_MAX_ACT)
				f->act[a][k] = (int16_t)rd16(p);
		if (a < WA_COUNT)
			f->nact[a] = (uint8_t)(n < W_MAX_ACT ? n : W_MAX_ACT);
		/* GetSubDefAct picks among all of them (activities.qc:116-122): with only some
		 * the pick would differ, so the weapon is left to the server */
		if (a < WA_COUNT && n > W_MAX_ACT)
			*wflags |= WI_TOOMANYACTS;
	}
	*pp = p;
	return true;
}

bool Weapon_ParseInfo(winfo_t *w, const uint8_t *d, int len)
{
	const uint8_t *p = d, *end = d + len;
	uint16_t edict = w->edict, version = w->version;
	memset(w, 0, sizeof(*w));
	w->edict = edict, w->version = version;
	NEED(27);
	w->viewmodel = (uint16_t)rd16(p);
	w->flags = p[2], w->infinite = (int8_t)p[3], w->primammo = (int8_t)p[4], w->secammo = (int8_t)p[5];
	w->primedfuse = rdf(p + 6);
	w->fallmin = rdf(p + 10), w->landmin = rdf(p + 14), w->fallpunch = rdf(p + 18), w->fallspring = rdf(p + 22);
	w->nfi = p[26];
	p += 27;
	if (w->nfi < 1 || w->nfi > 2)
		return false;
	for (int i = 0; i < w->nfi; i++)
		if (!parse_fire(&w->fi[i], &p, end, &w->flags))
			return false;
	NEED(1);
	w->nseq = *p++;
	if (w->nseq > W_MAX_SEQ)
		return false;
	for (int i = 0; i < w->nseq; i++) {
		wseq_t *s = &w->seq[i];
		int nev;
		NEED(12);
		s->dur = rdf(p), s->fps = rdf(p + 4), s->frames = (uint16_t)rd16(p + 8), s->loop = p[10];
		nev = p[11];
		p += 12;
		NEED(nev * 5);
		s->firstev = w->nev;
		for (int k = 0; k < nev; k++, p += 5)
			if (w->nev < W_MAX_EV) {
				wev_t *e = &w->ev[w->nev++];
				e->frame = (uint16_t)rd16(p), e->code = (uint16_t)rd16(p + 2), e->opt = p[4];
				s->nev++;
			}
	}
	return true;
}

int Weapon_ParseState(wstate_t *s, const uint8_t *d, int len)
{
	const uint8_t *p = d;
	int n;
	memset(s, 0, sizeof(*s));
	if (len < 84 || d[0] != 'W' || d[1] != 1)
		return 0;
	s->edict = (uint16_t)rd16(p + 2), s->infover = (uint16_t)rd16(p + 4);
	s->state = p[6], s->fi = p[7], s->flags = p[8], n = p[9];
	s->clip = (int16_t)rd16(p + 10), s->clipsize = (int16_t)rd16(p + 12);
	p += 14;
	s->burst = rdf(p), s->fiv = rdf(p + 4), s->overheat = rdf(p + 8), s->overheatlen = rdf(p + 12);
	s->reloadstep = rdf(p + 16), s->spin = rdf(p + 20);
	s->atk = rdf(p + 24), s->idle = rdf(p + 28), s->rel = rdf(p + 32);
	s->animseq = (int16_t)rd16(p + 36), s->animtime = rdf(p + 38);
	p += 42;
	for (int k = 0; k < 3; k++)
		s->punch[k] = rdf(p + k * 4), s->punchvel[k] = rdf(p + 12 + k * 4);
	s->firescale = rdf(p + 24);
	p += 28;
	if (84 + n * 3 > len || n > W_AMMO)
		return 0;
	s->nammo = (uint8_t)n;
	for (int i = 0; i < n; i++, p += 3)
		s->ammotype[i] = (int8_t)p[0], s->ammo[i] = (int16_t)rd16(p + 1);
	return 84 + n * 3;
}

bool Weapon_Usable(const wstate_t *s, const winfo_t *w)
{
	return w && s->edict && s->edict == w->edict && s->infover && s->infover == w->version && s->fi < w->nfi &&
	       !(w->flags & WI_TOOMANYACTS);
}

int Weapon_Ammo(const wstate_t *s, int type)
{
	for (int i = 0; i < s->nammo; i++)
		if (s->ammotype[i] == type)
			return s->ammo[i];
	return -1;
}

bool Weapon_Matches(const wstate_t *a, const wstate_t *b)
{
	const float e = 1e-3f;
	if (a->edict != b->edict || a->state != b->state || a->fi != b->fi || a->flags != b->flags || a->clip != b->clip ||
	    a->clipsize != b->clipsize || a->animseq != b->animseq || a->nammo != b->nammo ||
	    ((a->vflags ^ b->vflags) & W_VFL_BITS) || ((a->gflags ^ b->gflags) & W_GF_BITS))
		return false;
	for (int i = 0; i < a->nammo; i++)
		if (a->ammotype[i] != b->ammotype[i] || a->ammo[i] != b->ammo[i])
			return false;
	if (fabsf(a->burst - b->burst) > e || fabsf(a->fiv - b->fiv) > e || fabsf(a->overheat - b->overheat) > e ||
	    fabsf(a->reloadstep - b->reloadstep) > e || fabsf(a->spin - b->spin) > e || fabsf(a->atk - b->atk) > e ||
	    fabsf(a->idle - b->idle) > e || fabsf(a->rel - b->rel) > e || fabsf(a->animtime - b->animtime) > e ||
	    a->firescale != b->firescale)
		return false;
	for (int k = 0; k < 3; k++)
		if (fabsf(a->punch[k] - b->punch[k]) > 0.01f || fabsf(a->punchvel[k] - b->punchvel[k]) > 0.05f)
			return false;
	return true;
}

/* --- the QC, ported --- */

/* one command's run: the QC's `this`, its owner and the input_ globals */
typedef struct {
	wstate_t *s;
	const winfo_t *w;
	const wfire_t *fi;      /* m_weaponLastFireInfo's cache */
	const pmcmd_t *c;
	const wenv_t *env;
	float dt;               /* input_timelength */
	float angles[3];        /* input_angles: the owner's v_angle */
} wrun_t;

/* src/shared/defs.h:258-268 */
static float pseudorandom(uint32_t seq)
{
	float s = (float)seq, seed;
	seed = fmodf(s, 5.0f);
	seed += fmodf(s, 8.0f);
	seed += fmodf(s, 4.0f);
	seed += fmodf(s, 13.0f);
	seed += fmodf(s, 70.0f);
	seed = seed / 100.0f;
	return seed < 0.01f ? 0.01f : seed > 0.99f ? 0.99f : seed;
}

/* GetSubDefAct (src/shared/system/activities.qc:98-128): one value, or one of a list */
static float act(wrun_t *r, int fi, int a)
{
	const wfire_t *f = &r->w->fi[fi < r->w->nfi ? fi : 0];
	int n = f->nact[a];
	if (!n)
		return -1;
	if (n == 1)
		return (float)f->act[a][0];
	return (float)f->act[a][(int)floorf(pseudorandom(r->c->seq) * (float)n)];
}
static float lastact(wrun_t *r, int a) { return act(r, r->s->fi, a); }

/* frameduration of the view model (studio: Studio_SeqDuration) */
static float frameduration(wrun_t *r, float seq)
{
	int i = (int)seq;
	return seq >= 0 && i < r->w->nseq ? r->w->seq[i].dur : 0.0f;
}

static float maxf(float a, float b) { return a > b ? a : b; }
static float boundf(float lo, float v, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void SetWeaponFrame(wrun_t *r, float seq) /* :2562 */
{
	r->s->animseq = (int16_t)(int)seq;
	r->s->animtime = 0.0f;
}
static void SetReloadNext(wrun_t *r, float d) { r->s->rel = d; } /* :2612 */
static void SetAttackNext(wrun_t *r, float d)                   /* :2599 */
{
	r->s->atk = d;
	SetReloadNext(r, d);
}
static void SetIdleNext(wrun_t *r, float d) /* :2624 */
{
	if (r->w->flags & WI_BUGGYIDLE)
		r->s->idle = boundf(0.0f, d - 0.05f, d);
	else
		r->s->idle = d;
}
static bool CanIdle(wrun_t *r) { return !(r->s->idle > 0.0f); } /* :2640 */
static bool CanFire(wrun_t *r)                                  /* :2652 */
{
	if (r->s->atk > 0.0f || r->s->state == WS_OVERHEATED || (r->s->gflags & W_GF_SEMI_TOGGLED))
		return false;
	return true;
}
static bool CanReload(wrun_t *r) { return !(r->s->rel > 0.0f); } /* :2673 */
static float FireScale(wrun_t *r) { return r->s->firescale > 0.0f ? r->s->firescale : 1.0f; } /* :2576 */

/* ncActor's ammo (src/shared/game/Actor.qc:1381-1410) */
static bool HasAmmo(wrun_t *r, int type, int amount)
{
	int n;
	if (type < 0 || type >= 16)
		return false;
	n = Weapon_Ammo(r->s, type);
	return n >= 0 && n - amount >= 0;
}
static bool ActorUseAmmo(wrun_t *r, int type, int amount)
{
	if (r->w->infinite == 1)
		return true;
	if (!HasAmmo(r, type, amount))
		return false;
	for (int i = 0; i < r->s->nammo; i++)
		if (r->s->ammotype[i] == type)
			r->s->ammo[i] = (int16_t)(r->s->ammo[i] - amount);
	return true;
}
static bool HasReserveAmmo(wrun_t *r) /* :2298 */
{
	return HasAmmo(r, r->w->primammo, 1) || HasAmmo(r, r->w->secammo, 1);
}

/* :274, with UpdateFireInfoCache's one carried value (:184-187) */
static void SwitchFireInfo(wrun_t *r, int fi)
{
	r->s->fi = (uint8_t)(fi < r->w->nfi ? fi : 0);
	r->fi = &r->w->fi[r->s->fi];
	if (r->fi->overheat != 0.0f)
		r->s->overheatlen = r->fi->overheatlen;
}

static void SetState(wrun_t *r, int st) { r->s->state = (uint8_t)st; } /* :106 */

static void Idle(wrun_t *r);
static void Release(wrun_t *r);
static void Reload(wrun_t *r);
static void Weapon__Attack(wrun_t *r, int fi);

/* :995 */
static void Draw(wrun_t *r)
{
	float anim = -1, t = 0.0f;
	SwitchFireInfo(r, r->s->fi < r->w->nfi ? r->s->fi : 0);
	if (r->s->clipsize > 0 && r->s->clip == 0)
		anim = lastact(r, WA_DRAWEMPTY);
	if (anim < 0)
		anim = lastact(r, WA_DRAW);
	if (anim >= 0) {
		t = frameduration(r, anim);
		SetWeaponFrame(r, anim);
	}
	SetAttackNext(r, t);
	SetIdleNext(r, t);
}

/* :966 */
static bool IsEmpty(wrun_t *r)
{
	const wfire_t *f = r->fi;
	if (r->w->infinite != 0)
		return false;
	if (f->flags & WF_AMMOREQ) {
		if (r->s->clipsize && f->ammotype == r->w->primammo) {
			if (r->s->clip <= 0 || r->s->clip - f->ammopershot < 0)
				return !HasAmmo(r, f->ammotype, 1);
		} else if (!HasAmmo(r, f->ammotype, f->ammopershot))
			return true;
	}
	return false;
}

/* :1587. A melee weapon's test traces through the device's hulls and sweeps the
 * server's boxes near the player (PSTATE), as the PC's sweeps the entities it knows */
static bool TestFireAbility(wrun_t *r, int fi)
{
	const wfire_t *f = &r->w->fi[fi];
	float dist = f->testdist, fwd[3], end[3];
	bool invert;
	pmhit_t tr;
	static const float zero[3] = {0, 0, 0};
	if (dist == 0.0f)
		return true;
	invert = dist < 0.0f;
	dist = fabsf(dist);
	PM_AnglesToForward(r->angles, fwd); /* anglesToForward(eyeAngles) */
	/* Melee_Sweep (src/shared/game/Projectile.qc:1679-1780): cos(bound(0, coneDeg, 89) *
	 * (M_PI / 180)), fteqcc's M_PI 3.14159 folded to the same constant as anglesToForward's,
	 * through the engine's cosf (bi_cos) */
	if (invert && (f->flags & WF_ONFIRE) && f->sweepangle > 0.0f) {
		float mindot, sn;
		PM_SinCos(boundf(0, f->sweepangle, 89) * PM_QC_DEG2RAD, &sn, &mindot);
		for (int i = 0; i < r->env->nboxes; i++) {
			const float *mn = r->env->boxmins[i], *mx = r->env->boxmaxs[i];
			float near[3], aim[3], aimpt[3], c[3], along = 0, d2 = 0, len = 0, dot;
			if (r->env->boxsolid[i] != 2 && r->env->boxsolid[i] != 3)
				continue;
			for (int k = 0; k < 3; k++) {
				near[k] = boundf(mn[k], r->env->eye[k], mx[k]) - r->env->eye[k];
				d2 += near[k] * near[k];
			}
			if (sqrtf(d2) > f->sweeprange)
				continue;
			for (int k = 0; k < 3; k++)
				c[k] = (mn[k] + mx[k]) * 0.5f, along += (c[k] - r->env->eye[k]) * fwd[k];
			along = boundf(0, along, f->sweeprange);
			for (int k = 0; k < 3; k++) {
				aimpt[k] = boundf(mn[k], r->env->eye[k] + fwd[k] * along, mx[k]);
				aim[k] = aimpt[k] - r->env->eye[k];
				len += aim[k] * aim[k];
			}
			len = sqrtf(len);
			dot = len > 0 ? (aim[0] * fwd[0] + aim[1] * fwd[1] + aim[2] * fwd[2]) / len : 1.0f;
			if (dot < mindot)
				continue;
			/* not through walls (:1745-1756): to the aim point, else to the centre,
			 * MOVE_NOMONSTERS with the player's own contents (its hitcontentsmaski is
			 * the default there; TestFireAbility sets its own only after). Without hulls
			 * nothing can be in the way */
			if (r->env->trace) {
				r->env->trace->move(r->env->trace->ctx, r->env->eye, zero, zero, aimpt, PM_MOVE_NOMONSTERS, 0, &tr);
				if (tr.fraction < 1.0f) {
					r->env->trace->move(r->env->trace->ctx, r->env->eye, zero, zero, c, PM_MOVE_NOMONSTERS, 0, &tr);
					if (tr.fraction < 1.0f)
						continue;
				}
			}
			return true;
		}
	}
	if (!r->env->trace)
		return invert ? false : true; /* nothing to trace against: a miss */
	for (int k = 0; k < 3; k++)
		end[k] = r->env->eye[k] + fwd[k] * dist;
	r->env->trace->move(r->env->trace->ctx, r->env->eye, zero, zero, end, PM_MOVE_NORMAL, (int32_t)(PM_CB_SOLID | PM_CB_BODY), &tr);
	if (tr.fraction >= 1.0f)
		return invert ? false : true;
	return invert ? true : false;
}

/* :1060 */
static bool UseAmmo(wrun_t *r, int fi)
{
	const wfire_t *f = r->fi;
	if (r->w->infinite == 1 || (r->s->clipsize <= 0 && r->w->infinite >= 1))
		return true;
	if (f->flags & WF_AMMOREQ) {
		if (r->s->clipsize && f->ammotype == r->w->primammo) {
			if (r->s->clip <= 0 || r->s->clip - f->ammopershot < 0) {
				if (HasAmmo(r, f->ammotype, 1)) {
					Reload(r);
					return false;
				}
				r->s->flags &= (uint8_t)~WST_FIRING; /* EmptySound: its GF_SEMI_TOGGLED is the server's */
				return false;
			}
			r->s->clip = (int16_t)(r->s->clip - f->ammopershot);
		} else if (!ActorUseAmmo(r, f->ammotype, f->ammopershot)) {
			r->s->flags &= (uint8_t)~WST_FIRING;
			return false;
		}
	}
	if (!TestFireAbility(r, fi)) {
		const wfire_t *g = &r->w->fi[fi];
		float failanim = act(r, fi, WA_FIREFAILED), failrate = g->failrate, faildur = frameduration(r, failanim);
		if (!(failrate != 0.0f))
			failrate = faildur;
		if (failanim >= 0)
			SetWeaponFrame(r, failanim);
		if (failrate != 0.0f)
			SetAttackNext(r, failrate * FireScale(r));
		SetIdleNext(r, faildur);
		return false;
	}
	return true;
}

/* :1220 */
static void FiredWeaponAttack(wrun_t *r)
{
	SetState(r, WS_IDLE);
	if (r->fi->knockback != 0.0f && r->env->velocity) {
		float fwd[3];
		PM_AnglesToForward(r->angles, fwd); /* anglesToForward(ourOwner.v_angle) */
		for (int k = 0; k < 3; k++)
			r->env->velocity[k] += fwd[k] * -r->fi->knockback;
	}
	if (r->w->flags & WI_POWERAMMO)
		r->s->flags &= (uint8_t)~WST_WILLRELEASE;
}

/* :1278 */
static void ReleasedWeaponAttack(wrun_t *r)
{
	SetState(r, WS_RELEASED);
	if (r->fi->knockbackrel != 0.0f && r->env->velocity) {
		float fwd[3];
		PM_AnglesToForward(r->angles, fwd); /* anglesToForward(ourOwner.v_angle) */
		for (int k = 0; k < 3; k++)
			r->env->velocity[k] += fwd[k] * -r->fi->knockbackrel;
	}
}

/* :2182 */
static void TimedReloadStop(wrun_t *r)
{
	if (r->s->state == WS_RELOAD_START || r->s->state == WS_RELOAD || r->s->state == WS_RELOAD_END)
		SetState(r, WS_IDLE);
	r->s->reloadstep = 0.0f;
}

/* :2120 */
static void TimedReloadStart(wrun_t *r, float startact)
{
	float t = r->fi->reloadtime > 0.0f ? r->fi->reloadtime : 1.0f, anim = -1;
	if (startact >= 0) {
		if (r->fi->reloadstarttime > 0.0f)
			t = r->fi->reloadstarttime;
		SetState(r, WS_RELOAD_START);
		anim = startact;
	} else {
		SetState(r, WS_RELOAD);
		if (r->s->clip == 0)
			anim = lastact(r, WA_RELOADEMPTY);
		if (anim < 0)
			anim = lastact(r, WA_RELOAD);
		SetAttackNext(r, t);
	}
	if (anim >= 0)
		SetWeaponFrame(r, anim);
	r->s->burst = 0;
	r->s->reloadstep = t;
	SetIdleNext(r, t);
}

/* :2193 */
static bool TimedReloadFrame(wrun_t *r)
{
	if (r->s->state != WS_RELOAD_START && r->s->state != WS_RELOAD) {
		r->s->reloadstep = 0.0f;
		if (r->s->state == WS_IDLE && r->s->clipsize > 0 && r->s->clip == 0)
			if (CanReload(r) && HasReserveAmmo(r)) {
				TimedReloadStart(r, lastact(r, WA_RELOADSTART));
				return true;
			}
		return false;
	}
	if (!CanIdle(r))
		return true;
	if (lastact(r, WA_RELOADSTART) >= 0) {
		float t = r->fi->reloadtime > 0.0f ? r->fi->reloadtime : 1.0f, anim = lastact(r, WA_RELOAD);
		if (HasReserveAmmo(r)) {
			r->s->clip++;
			ActorUseAmmo(r, r->w->primammo, 1);
		}
		if (r->s->clip >= r->s->clipsize || !HasReserveAmmo(r)) {
			float endact = lastact(r, WA_RELOADEND);
			TimedReloadStop(r);
			if (endact >= 0)
				SetWeaponFrame(r, endact);
			return false;
		}
		SetState(r, WS_RELOAD);
		if (anim >= 0)
			SetWeaponFrame(r, anim);
		r->s->reloadstep = t;
		SetIdleNext(r, t);
		return true;
	}
	{
		int deduct = r->s->clipsize - r->s->clip;
		if (!HasAmmo(r, r->w->primammo, deduct))
			deduct = HasAmmo(r, r->w->primammo, 0) ? Weapon_Ammo(r->s, r->w->primammo) : 0;
		r->s->clip = (int16_t)(r->s->clip + deduct);
		ActorUseAmmo(r, r->w->primammo, deduct);
	}
	TimedReloadStop(r);
	return false;
}

/* :2002 */
static void Reload(wrun_t *r)
{
	float anim = -1, t, startact;
	if (r->s->state >= WS_RELOAD_START && r->s->state <= WS_RELOAD_END) {
		if (r->fi->flags & WF_RELOADTIMED)
			Idle(r);
		return;
	}
	startact = lastact(r, WA_RELOADSTART);
	if (!r->s->clipsize)
		return;
	if (!CanReload(r)) {
		Idle(r);
		return;
	}
	if (r->s->clip == r->s->clipsize) {
		Idle(r);
		return;
	}
	if (!HasAmmo(r, r->w->primammo, 1))
		return;
	if (r->fi->flags & WF_RELOADTIMED) {
		TimedReloadStart(r, startact);
		return;
	}
	if (startact >= 0) {
		SetState(r, WS_RELOAD_START);
		SetIdleNext(r, 0.0f);
		Release(r);
		return;
	}
	if (r->s->clipsize > 0 && r->s->clip == 0)
		anim = lastact(r, WA_RELOADEMPTY);
	if (anim < 0) {
		anim = lastact(r, WA_RELOAD);
		if (lastact(r, WA_RELOADEND) >= 0)
			SetState(r, WS_RELOAD_END);
	}
	t = r->fi->reloadtime == -1.0f ? frameduration(r, anim) : r->fi->reloadtime;
	/* the server fills the clip (_ReloadFinished, a think of its own) */
	r->s->burst = 0;
	SetWeaponFrame(r, anim);
	SetAttackNext(r, t - 0.01f);
	SetIdleNext(r, t);
}

/* :1551 */
static void SwitchedWeaponMode(wrun_t *r)
{
	float anim = -1;
	if (r->s->fiv != 0.0f) {
		anim = lastact(r, WA_MODEON);
		SwitchFireInfo(r, 1);
	} else {
		SwitchFireInfo(r, 0);
		anim = lastact(r, WA_MODEOFF);
	}
	if (anim >= 0) {
		SetWeaponFrame(r, anim);
		SetAttackNext(r, frameduration(r, anim));
		SetIdleNext(r, frameduration(r, anim));
	}
}

/* :1633 */
static void Idle(wrun_t *r)
{
	wstate_t *s = r->s;
	float anim = -1, next;
	s->overheat = maxf(0.0f, s->overheat - r->dt);
	/* depleteAmmo reads `time`: the server's */
	if ((r->w->flags & WI_ALTMODE) && r->fi->flags & WF_AMMOREQ && r->w->secammo != r->w->primammo)
		if (!HasAmmo(r, r->w->secammo, 1) && s->fiv != 0.0f) {
			s->fiv = 0;
			SetAttackNext(r, 0.0f);
			SetIdleNext(r, 0.0f);
			SwitchedWeaponMode(r);
			SetAttackNext(r, 1.0f);
			SetIdleNext(r, 1.0f);
			return;
		}
	if (s->state == WS_BURSTFIRING) {
		if (s->clip <= 0) {
			s->burst = 0;
			SetState(r, WS_IDLE);
		}
		if (CanFire(r) && s->burst < r->fi->burstcount) {
			Weapon__Attack(r, s->fi);
			return;
		}
	}
	if (r->fi->flags & WF_RELOADTIMED)
		if (TimedReloadFrame(r))
			return;
	if (!CanIdle(r))
		return;
	if (s->clipsize > 0 && s->state == WS_IDLE && s->clip == 0)
		if (CanReload(r)) {
			Reload(r);
			return;
		}
	if (s->flags & WST_WILLRELEASE) {
		anim = lastact(r, WA_COOKING);
		if (anim >= 0) {
			SetWeaponFrame(r, anim);
			SetIdleNext(r, frameduration(r, anim));
		}
		return;
	}
	switch (s->state) {
	case WS_RELOAD_START:
		anim = lastact(r, WA_RELOADSTART);
		SetState(r, WS_RELOAD);
		SetAttackNext(r, frameduration(r, anim));
		break;
	case WS_RELOAD: {
		float t;
		anim = lastact(r, WA_RELOAD);
		s->clip++;
		ActorUseAmmo(r, r->w->primammo, 1);
		if (!HasReserveAmmo(r) || s->clip >= s->clipsize)
			SetState(r, WS_RELOAD_END);
		t = r->fi->reloadtime == -1.0f ? frameduration(r, anim) : r->fi->reloadtime;
		SetWeaponFrame(r, anim);
		SetIdleNext(r, t);
		return;
	}
	case WS_RELOAD_END:
		anim = lastact(r, WA_RELOADEND);
		SetState(r, WS_IDLE);
		SetAttackNext(r, frameduration(r, anim));
		break;
	case WS_FIRELOOP:
	case WS_CHARGING:
		anim = lastact(r, WA_LOOP);
		break;
	case WS_COCK:
		anim = lastact(r, WA_COCK);
		SetState(r, WS_IDLE);
		SetAttackNext(r, frameduration(r, anim));
		break;
	case WS_RELEASED:
		if (r->fi->flags & WF_ONRELEASE)
			if (!UseAmmo(r, s->fi))
				break;
		ReleasedWeaponAttack(r);
		s->vflags &= ~W_VFL_PRIMEDFUSE;
		anim = lastact(r, WA_RELEASE);
		SetAttackNext(r, frameduration(r, anim));
		SetState(r, r->fi->flags & WF_DRAWAFTER ? WS_DRAW : WS_IDLE);
		break;
	case WS_DRAW:
		Draw(r);
		SetState(r, WS_IDLE);
		return;
	case WS_OVERHEATED:
		SetState(r, WS_IDLE);
		SetAttackNext(r, s->overheatlen);
		SetReloadNext(r, 0.1f);
		return;
	default:
		if (s->clipsize > 0 && s->clip == 0)
			anim = lastact(r, WA_IDLEEMPTY);
		if (anim < 0)
			anim = lastact(r, WA_IDLE);
	}
	if (anim < 0) {
		SetIdleNext(r, 5.0f);
		return;
	}
	SetWeaponFrame(r, anim);
	next = frameduration(r, anim);
	SetIdleNext(r, next <= 0.0f ? 10.0f : frameduration(r, anim));
}

/* :1300 */
static void Weapon__Attack(wrun_t *r, int fi)
{
	wstate_t *s = r->s;
	float shot = -1, burstdelay = 0.0f, animtime;
	if ((r->fi->flags & WF_RELOADTIMED) && (s->state == WS_RELOAD_START || s->state == WS_RELOAD)) {
		if (lastact(r, WA_RELOADSTART) >= 0 && s->clip > 0) {
			TimedReloadStop(r);
			SetIdleNext(r, 0.0f);
		} else {
			Idle(r);
			return;
		}
	}
	if ((r->fi->flags & WF_RELOADTIMED) && s->clipsize > 0 && s->clip == 0 && s->state == WS_IDLE) {
		Idle(r);
		return;
	}
	if (s->state == WS_RELOAD) {
		SetState(r, WS_RELOAD_END);
		SetIdleNext(r, 0.0f);
		Idle(r);
		return;
	}
	if (r->w->fi[fi].flags & WF_SERVERFIRE) /* CLIENT: the server's game decides when this fires */
		return;
	SwitchFireInfo(r, fi);
	if (r->fi->flags & WF_SEMIAUTO)
		s->gflags |= W_GF_SEMI_TOGGLED;
	/* detonateOnFire: whether there are charges to set off is the server's to know */
	if ((r->w->flags & WI_REMOVEONEMPTY) && !HasReserveAmmo(r) && s->clipsize <= 0)
		return;
	if (s->state != WS_RELEASED) {
		if (r->fi->chargetime <= 0.0f && !(s->vflags & W_VFL_PRIMEDFUSE) && (r->fi->flags & WF_ONRELEASE)) {
			SetState(r, WS_RELEASED);
			s->vflags |= W_VFL_PRIMEDFUSE;
		} else if (r->fi->flags & WF_ONRELEASE)
			s->vflags |= W_VFL_PRIMEDFUSE;
	}
	if (!(r->fi->flags & WF_UNDERWATER) && r->env->waterlevel >= 3)
		return; /* EmptySound */
	if (IsEmpty(r))
		return;
	if (r->fi->chargetime > 0.0f) {
		if (s->state != WS_RELEASED && s->state != WS_CHARGING) {
			shot = act(r, fi, WA_DELAY);
			SetAttackNext(r, r->fi->chargetime);
			SetIdleNext(r, frameduration(r, shot));
			SetWeaponFrame(r, shot);
			SetState(r, WS_CHARGING);
			if (!(r->fi->flags & WF_ONFIRE)) {
				s->gflags |= W_GF_SEMI_TOGGLED;
				SetAttackNext(r, frameduration(r, shot));
			}
			return;
		}
		if (!UseAmmo(r, fi))
			return;
	} else if (!UseAmmo(r, fi))
		return;
	if (r->fi->punchkick > 0.0f) {
		float kick = r->fi->punchkick + (float)((int)r->c->seq % 3) - 1.0f;
		s->punch[0] = -kick, s->punch[1] = 0, s->punch[2] = 0;
		s->punchvel[0] = -0.01f, s->punchvel[1] = 0, s->punchvel[2] = 0;
	} else
		for (int k = 0; k < 3; k++) {
			s->punch[k] += r->fi->punch[k];
			s->punchvel[k] += r->fi->spring[k] * 20.0f;
		}
	FiredWeaponAttack(r);
	if (!(r->fi->flags & WF_LOOPING)) {
		if (s->clipsize > 0 && s->clip == 0)
			shot = act(r, fi, WA_FIRELAST);
		if (shot <= 0)
			shot = act(r, fi, WA_FIRE);
		SetWeaponFrame(r, shot);
		SetIdleNext(r, frameduration(r, shot));
	} else {
		SetState(r, WS_FIRELOOP);
		Idle(r);
	}
	if (s->overheatlen > 0.0f && r->fi->overheat > 0.0f) {
		s->overheat += r->fi->overheat;
		if (s->overheat >= s->overheatlen)
			SetState(r, WS_OVERHEATED);
	} else if (r->fi->flags & WF_COCKS)
		SetState(r, WS_COCK);
	if (r->fi->burstcount > 1.0f) {
		SetState(r, WS_BURSTFIRING);
		if (s->burst + 1.0f >= r->fi->burstcount) {
			s->burst = 0;
			burstdelay = r->fi->bursttime;
			SetState(r, WS_IDLE);
		} else
			s->burst += 1.0f;
	}
	animtime = frameduration(r, shot);
	if (r->fi->firerate != 0.0f)
		SetAttackNext(r, r->fi->firerate * FireScale(r) + burstdelay);
	else
		SetAttackNext(r, animtime + burstdelay);
	SetIdleNext(r, animtime);
	s->flags |= WST_FIRING;
}

/* :2464 */
static void Release(wrun_t *r)
{
	wstate_t *s = r->s;
	s->gflags &= ~W_GF_SEMI_TOGGLED;
	/* regenAmmo reads `time`: the server's */
	if (s->vflags & W_VFL_PRIMEDFUSE) {
		SetState(r, WS_RELEASED);
		SetIdleNext(r, 0.0f);
	}
	if (s->state == WS_CHARGING && CanFire(r) && (r->fi->flags & WF_ONRELEASE))
		SetState(r, WS_RELEASED);
	s->flags &= (uint8_t)~WST_FIRING;
	Idle(r);
}

/* :2315 */
static void WeaponStartedFiring(wrun_t *r)
{
	if (!CanFire(r))
		return;
	if (!(r->s->vflags & W_VFL_FIRING)) {
		float start = lastact(r, WA_FIRESTART);
		if (r->fi->flags & WF_LOOPING) {
			SetState(r, WS_FIRELOOP);
			SetIdleNext(r, 0.0f);
		}
		if (start >= 0) {
			SetWeaponFrame(r, start);
			SetAttackNext(r, frameduration(r, start));
			SetIdleNext(r, frameduration(r, start));
		}
	}
	r->s->vflags |= W_VFL_FIRING;
}

/* :2360 */
static void WeaponStoppedFiring(wrun_t *r)
{
	if (!(r->fi->flags & WF_ONRELEASE) && !(r->fi->flags & WF_LOOPING))
		if (!CanFire(r))
			return;
	if (r->s->vflags & W_VFL_FIRING) {
		float stop = lastact(r, WA_FIRESTOP);
		if (stop >= 0) {
			SetWeaponFrame(r, stop);
			SetAttackNext(r, frameduration(r, stop));
			SetIdleNext(r, frameduration(r, stop));
		}
		if ((r->fi->flags & WF_ONRELEASE) || (r->fi->flags & WF_LOOPING) || stop >= 0)
			SetState(r, WS_RELEASED);
		if (!(r->fi->flags & WF_DETONATE) && (r->w->flags & WI_REMOVEONEMPTY) && !HasReserveAmmo(r) &&
		    r->s->clipsize <= 0)
			return;
	}
	r->s->vflags &= ~W_VFL_FIRING;
}

/* :1839 */
static void PrimaryAttack(wrun_t *r)
{
	if (r->fi->spinup > 0.0f && r->s->spin < r->fi->spinup) {
		Idle(r);
		return;
	}
	if (!CanFire(r)) {
		if (!(r->s->gflags & W_GF_SEMI_TOGGLED)) {
			Idle(r);
			return;
		}
		if (r->fi->flags & WF_LOOPING)
			Release(r);
		else
			Idle(r);
		return;
	}
	if ((r->w->flags & WI_ALTMODE) && !CanFire(r))
		return;
	if (r->w->flags & WI_ALTALTERNATES)
		r->s->fiv = 1.0f - r->s->fiv;
	Weapon__Attack(r, r->s->fiv != 0.0f && r->w->nfi > 1 ? 1 : 0);
}

/* :1883 */
static void SecondaryAttack(wrun_t *r)
{
	if (r->fi->spinup > 0.0f) {
		if (r->c->buttons & PM_IN_ATTACK)
			PrimaryAttack(r);
		else
			Idle(r);
		return;
	}
	if (r->fi->zoomfov > 0.0f) {
		if (!CanFire(r))
			return;
		r->s->gflags |= W_GF_SEMI_TOGGLED; /* the zoom itself is the server's */
		return;
	} else if (r->w->flags & WI_ALTMODE) {
		if (!CanFire(r)) {
			Idle(r);
			return;
		}
		if (r->w->nfi > 1 && r->w->fi[1].depleteammo > 0 && !HasAmmo(r, r->w->secammo, r->w->fi[1].depleteammo)) {
			Idle(r);
			return;
		}
		r->s->fiv = 1.0f - r->s->fiv;
		SetAttackNext(r, 1.0f);
		SetIdleNext(r, 1.0f);
		SwitchedWeaponMode(r);
		return;
	}
	if (!(r->w->flags & WI_SECONDARY) || r->w->nfi < 2) {
		Release(r);
		return;
	}
	if (!CanFire(r)) {
		Idle(r);
		return;
	}
	Weapon__Attack(r, 1);
}

/* player_pmove.qc:37-59, the punch of Physics_Fall */
static void fall_punch(wstate_t *s, const winfo_t *w, const wenv_t *env)
{
	float a = w ? w->fallpunch : 0, sp = w ? w->fallspring : 0, f = env->downforce;
	if (!env->landed || !w)
		return;
	if (a > 0.0f) {
		if (f >= w->fallmin) {
			s->punch[0] += a;
			s->punch[2] += (env->seq & 1) ? a : -a;
		} else if (f >= w->landmin)
			s->punch[0] += a;
	} else if (sp > 0.0f) {
		if (f >= w->fallmin) {
			s->punchvel[0] += sp;
			s->punchvel[2] += (env->seq & 1) ? sp : -sp;
		} else if (f >= w->landmin)
			s->punchvel[0] += sp;
	}
}

void Weapon_Run(wstate_t *s, const winfo_t *w, const pmcmd_t *c, const wenv_t *env)
{
	wrun_t r;
	float dt = (float)c->msec / 1000.0f;
	float *pa = s->punch, *pv = s->punchvel;
	fall_punch(s, w, env);
	/* Physics_InputPostMove (player_pmove.qc:704-747) */
	s->atk = maxf(0.0f, s->atk - dt);
	s->rel = maxf(0.0f, s->rel - dt);
	s->idle = maxf(0.0f, s->idle - dt);
	s->animtime += dt;
	if (sqrtf(pv[0] * pv[0] + pv[1] * pv[1] + pv[2] * pv[2]) > 0.001f) {
		float force, damp;
		for (int k = 0; k < 3; k++)
			pa[k] += pv[k] * dt;
		force = boundf(0.0f, dt * 65.0f, 2.0f);
		damp = 1.0f - dt * 9.0f;
		if (damp < 0)
			damp = 0;
		for (int k = 0; k < 3; k++) {
			pv[k] *= damp;
			pv[k] -= pa[k] * force;
		}
		pa[0] = boundf(-89.0f, pa[0], 89.0f);
		pa[1] = boundf(-179.0f, pa[1], 179.0f);
		pa[2] = boundf(-89.0f, pa[2], 89.0f);
	} else if (sqrtf(pa[0] * pa[0] + pa[1] * pa[1] + pa[2] * pa[2]) > 0.001f) {
		float punch = maxf(0.0f, 1.0f - dt * 4.0f);
		pa[0] *= punch, pa[1] *= punch, pa[2] *= punch;
	}

	/* ncPlayer::ProcessInput (Player.qc:341-462), the weapon's part */
	if (!w || !Weapon_Usable(s, w))
		return;
	memset(&r, 0, sizeof(r));
	r.s = s, r.w = w, r.c = c, r.env = env, r.dt = dt;
	r.fi = &w->fi[s->fi];
	PM_CmdAngles(c, r.angles);
	if (s->vflags & W_VFL_HOLDING) {
		if (c->buttons & PM_IN_ATTACK)
			s->vflags &= ~W_VFL_HOLDING; /* thrown */
		Release(&r);
		return;
	}
	if (env->health <= 0) {
		Release(&r);
		return;
	}
	/* InputFrame (:1255): spin-up weapons */
	if (r.fi->spinup > 0.0f) {
		if (c->buttons & (PM_IN_ATTACK | PM_IN_SECONDARY)) {
			s->spin = s->spin + dt;
			if (s->spin > r.fi->spinup)
				s->spin = r.fi->spinup;
		} else
			s->spin = 0.0f;
	}
	if (c->buttons & PM_IN_SECONDARY) {
		if (!(w->flags & WI_ALTALTERNATES)) { /* _SecondaryAttack (:1536) */
			WeaponStartedFiring(&r);
			SecondaryAttack(&r);
		}
	} else if (c->buttons & PM_IN_ATTACK) { /* _PrimaryAttack (:1525) */
		WeaponStartedFiring(&r);
		PrimaryAttack(&r);
	} else if (c->buttons & PM_IN_RELOAD)
		Reload(&r);
	else {
		WeaponStoppedFiring(&r);
		Release(&r);
	}
}

/* src/qcvm/qc_modelevents.c's collect(): (base, target], a window from 0 taking frame 0's
 * events; target before base means the sequence started over; looping sequences repeat */
int Weapon_Events(const winfo_t *w, int seq, float base, float target, const wev_t **out, int max)
{
	const wseq_t *s;
	int n = 0, first = 0, last = 0;
	if (!w || seq < 0 || seq >= w->nseq)
		return 0;
	s = &w->seq[seq];
	if (target < base)
		base = 0;
	if (target <= base && !(base <= 0 && target >= 0))
		return 0;
	if (s->loop && s->dur > 0) {
		first = (int)(base / s->dur), last = (int)(target / s->dur);
		if (last - first > 4)
			first = last - 4;
	}
	for (int lap = first; lap <= last; lap++)
		for (int i = 0; i < s->nev && n < max; i++) {
			const wev_t *e = &w->ev[s->firstev + i];
			float t = (s->fps > 0 ? e->frame / s->fps : 0) + lap * s->dur;
			if ((t > base || (base <= 0 && t >= 0)) && t <= target)
				out[n++] = e;
		}
	return n;
}
