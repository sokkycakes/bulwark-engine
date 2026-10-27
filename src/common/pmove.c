/* Bulwark engine - player movement. ISC licence, see LICENSE. */
#include <math.h>
#include <string.h>

#include "common.h"
#include "pmove.h"

#define FL_ONGROUND 512
#define FL_WATERJUMP 2048
#define FL_JUMPRELEASED 4096
#define MOVETYPE_WALK 3
#define MOVETYPE_FLY 5
#define MOVETYPE_NOCLIP 8
#define CONTENT_EMPTY -1
#define CONTENT_SOLID -2
#define CONTENT_WATER -3
#define CONTENT_LADDER -16
#define STOP_EPSILON 0.1f
#define MIN_STEP_NORMAL 0.7f

void PM_DefaultVars(pmvars_t *v)
{
	v->gravity = 800;
	v->maxspeed = 320;
	v->accelerate = 10;
	v->airaccelerate = 10;
	v->wateraccelerate = 10;
	v->friction = 4;
	v->waterfriction = 1;
	v->stopspeed = 100;
	v->stepheight = 18;
	v->jumpspeed = 268.3f; /* sqrt(2 * 800 * 45): a 45 unit jump */
	v->spectatormaxspeed = 500;
	v->aircap = 30;
}

typedef struct {
	pmstate_t *s;
	const pmcmd_t *cmd;
	const pmvars_t *v;
	const pmworld_t *w;
	float dt;
	float forward[3], right[3], up[3];
} pm_t;

static void trace(pm_t *pm, const float *start, const float *end, pmtrace_t *tr)
{
	pm->w->trace(pm->w->ctx, start, pm->s->mins, pm->s->maxs, end, tr);
}

static void clip_velocity(const float *in, const float *normal, float *out, float overbounce)
{
	float backoff = DotProduct(in, normal) * overbounce;
	for (int i = 0; i < 3; i++) {
		out[i] = in[i] - normal[i] * backoff;
		if (out[i] > -STOP_EPSILON && out[i] < STOP_EPSILON)
			out[i] = 0;
	}
}

/* Moves along velocity for dt, sliding along whatever it hits. Returns
 * a bitmask: 1 = hit a floor, 2 = hit a wall/step. */
static int slide_move(pm_t *pm)
{
	float planes[5][3], primal[3], original[3], end[3];
	float timeleft = pm->dt;
	int numplanes = 0, blocked = 0;
	pmstate_t *s = pm->s;
	VectorCopy(s->velocity, original);
	VectorCopy(s->velocity, primal);
	for (int bump = 0; bump < 4; bump++) {
		pmtrace_t tr;
		int i, j;
		if (!s->velocity[0] && !s->velocity[1] && !s->velocity[2])
			break;
		for (i = 0; i < 3; i++)
			end[i] = s->origin[i] + timeleft * s->velocity[i];
		trace(pm, s->origin, end, &tr);
		if (tr.allsolid) {
			VectorClear(s->velocity);
			return 3;
		}
		if (tr.fraction > 0) {
			VectorCopy(tr.endpos, s->origin);
			VectorCopy(s->velocity, original);
			numplanes = 0;
		}
		if (tr.fraction == 1)
			break;
		blocked |= tr.normal[2] > MIN_STEP_NORMAL ? 1 : 2;
		timeleft -= timeleft * tr.fraction;
		if (numplanes >= 5) {
			VectorClear(s->velocity);
			return blocked;
		}
		VectorCopy(tr.normal, planes[numplanes]);
		numplanes++;
		/* find a velocity that satisfies every plane hit this move */
		for (i = 0; i < numplanes; i++) {
			clip_velocity(original, planes[i], s->velocity, 1.0f);
			for (j = 0; j < numplanes; j++)
				if (j != i && DotProduct(s->velocity, planes[j]) < 0)
					break;
			if (j == numplanes)
				break;
		}
		if (i == numplanes) {
			/* in a crease: slide along the line between two planes */
			float dir[3], d;
			if (numplanes != 2) {
				VectorClear(s->velocity);
				return blocked;
			}
			dir[0] = planes[0][1] * planes[1][2] - planes[0][2] * planes[1][1];
			dir[1] = planes[0][2] * planes[1][0] - planes[0][0] * planes[1][2];
			dir[2] = planes[0][0] * planes[1][1] - planes[0][1] * planes[1][0];
			d = DotProduct(dir, s->velocity);
			for (int k = 0; k < 3; k++)
				s->velocity[k] = dir[k] * d;
		}
		/* never bounce back into where we started */
		if (DotProduct(s->velocity, primal) <= 0) {
			VectorClear(s->velocity);
			break;
		}
	}
	if (s->waterjumptime > 0)
		VectorCopy(primal, s->velocity);
	return blocked;
}

/* Tries the plain slide and a stepped-up slide, keeping whichever went farther. */
static void step_slide_move(pm_t *pm)
{
	pmstate_t *s = pm->s;
	float start_o[3], start_v[3], down_o[3], down_v[3], up[3], down[3];
	pmtrace_t tr;
	int blocked;
	VectorCopy(s->origin, start_o);
	VectorCopy(s->velocity, start_v);
	blocked = slide_move(pm);
	if (!(blocked & 2))
		return; /* nothing to step over */
	VectorCopy(s->origin, down_o);
	VectorCopy(s->velocity, down_v);

	VectorCopy(start_o, up);
	up[2] += pm->v->stepheight;
	trace(pm, start_o, up, &tr);
	if (tr.allsolid)
		return;
	VectorCopy(tr.endpos, s->origin);
	VectorCopy(start_v, s->velocity);
	slide_move(pm);

	/* come back down by however far the step-up went */
	VectorCopy(s->origin, down);
	down[2] = s->origin[2] - (tr.endpos[2] - start_o[2]);
	trace(pm, s->origin, down, &tr);
	if (!tr.allsolid)
		VectorCopy(tr.endpos, s->origin);
	if (tr.fraction < 1 && tr.normal[2] < MIN_STEP_NORMAL) {
		/* stepped onto a slope too steep to stand on: undo */
		VectorCopy(down_o, s->origin);
		VectorCopy(down_v, s->velocity);
		return;
	}
	{
		float d1 = (down_o[0] - start_o[0]) * (down_o[0] - start_o[0]) + (down_o[1] - start_o[1]) * (down_o[1] - start_o[1]);
		float d2 = (s->origin[0] - start_o[0]) * (s->origin[0] - start_o[0]) +
		           (s->origin[1] - start_o[1]) * (s->origin[1] - start_o[1]);
		if (d1 > d2) {
			VectorCopy(down_o, s->origin);
			VectorCopy(down_v, s->velocity);
		} else
			s->velocity[2] = down_v[2];
	}
}

static void categorize(pm_t *pm)
{
	pmstate_t *s = pm->s;
	float p[3], h;
	pmtrace_t tr;
	int c;
	/* ground */
	VectorCopy(s->origin, p);
	p[2] -= 0.25f;
	if (s->velocity[2] > 180) {
		s->flags &= ~FL_ONGROUND;
		s->groundent = -1;
	} else {
		trace(pm, s->origin, p, &tr);
		if (tr.fraction < 1 && tr.normal[2] >= MIN_STEP_NORMAL && !tr.startsolid) {
			s->flags |= FL_ONGROUND;
			s->groundent = tr.ent;
			VectorCopy(tr.endpos, s->origin);
		} else {
			s->flags &= ~FL_ONGROUND;
			s->groundent = -1;
		}
	}
	/* water at feet, waist and eyes */
	s->waterlevel = 0;
	s->watertype = CONTENT_EMPTY;
	h = s->maxs[2] - s->mins[2];
	p[0] = s->origin[0];
	p[1] = s->origin[1];
	p[2] = s->origin[2] + s->mins[2] + 1;
	c = pm->w->contents(pm->w->ctx, p);
	if (c <= CONTENT_WATER && c > -10) {
		s->watertype = c;
		s->waterlevel = 1;
		p[2] = s->origin[2] + s->mins[2] + h * 0.5f;
		c = pm->w->contents(pm->w->ctx, p);
		if (c <= CONTENT_WATER && c > -10) {
			s->waterlevel = 2;
			p[2] = s->origin[2] + s->mins[2] + h * 0.9f;
			c = pm->w->contents(pm->w->ctx, p);
			if (c <= CONTENT_WATER && c > -10)
				s->waterlevel = 3;
		}
	}
}

static void friction(pm_t *pm, float fric)
{
	float *v = pm->s->velocity;
	float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), control, drop, newspeed;
	if (speed < 1) {
		v[0] = v[1] = 0;
		return;
	}
	control = speed < pm->v->stopspeed ? pm->v->stopspeed : speed;
	drop = control * fric * pm->dt;
	newspeed = speed - drop;
	if (newspeed < 0)
		newspeed = 0;
	newspeed /= speed;
	for (int i = 0; i < 3; i++)
		v[i] *= newspeed;
}

static void accelerate(pm_t *pm, const float *wishdir, float wishspeed, float accel, float cap)
{
	float current, addspeed, accelspeed, wish = wishspeed;
	if (cap > 0 && wish > cap)
		wish = cap;
	current = DotProduct(pm->s->velocity, wishdir);
	addspeed = wish - current;
	if (addspeed <= 0)
		return;
	accelspeed = accel * pm->dt * wishspeed;
	if (accelspeed > addspeed)
		accelspeed = addspeed;
	for (int i = 0; i < 3; i++)
		pm->s->velocity[i] += accelspeed * wishdir[i];
}

static float wish(pm_t *pm, bool flat, bool useup, float maxspeed, float *dir)
{
	float f[3], r[3], len;
	VectorCopy(pm->forward, f);
	VectorCopy(pm->right, r);
	if (flat) {
		f[2] = r[2] = 0;
		VectorNormalize(f);
		VectorNormalize(r);
	}
	for (int i = 0; i < 3; i++)
		dir[i] = f[i] * pm->cmd->movevalues[0] + r[i] * pm->cmd->movevalues[1];
	if (useup)
		dir[2] += pm->cmd->movevalues[2];
	len = VectorNormalize(dir);
	return len > maxspeed ? maxspeed : len;
}

static void noclip_move(pm_t *pm, float maxspeed)
{
	float dir[3], speed;
	friction(pm, pm->v->friction * 1.5f);
	speed = wish(pm, false, true, maxspeed, dir);
	accelerate(pm, dir, speed, pm->v->accelerate, 0);
	for (int i = 0; i < 3; i++)
		pm->s->origin[i] += pm->s->velocity[i] * pm->dt;
}

static void fly_move(pm_t *pm, float maxspeed)
{
	float dir[3], speed;
	friction(pm, pm->v->friction * 1.5f);
	speed = wish(pm, false, true, maxspeed, dir);
	accelerate(pm, dir, speed, pm->v->accelerate, 0);
	slide_move(pm);
}

static void water_move(pm_t *pm, float maxspeed)
{
	float dir[3], speed;
	pmstate_t *s = pm->s;
	friction(pm, pm->v->waterfriction);
	speed = wish(pm, false, true, maxspeed, dir);
	if (!pm->cmd->movevalues[0] && !pm->cmd->movevalues[1] && !pm->cmd->movevalues[2]) {
		dir[0] = dir[1] = 0;
		dir[2] = -1;
		speed = 60; /* sink slowly */
	}
	speed *= 0.7f;
	accelerate(pm, dir, speed, pm->v->wateraccelerate, 0);
	if (s->flags & FL_ONGROUND)
		step_slide_move(pm);
	else
		slide_move(pm);
}

static void walk_move(pm_t *pm, float maxspeed)
{
	pmstate_t *s = pm->s;
	float dir[3], speed, grav = pm->v->gravity * (s->gravity ? s->gravity : 1.0f);
	speed = wish(pm, true, false, maxspeed, dir);
	if (s->flags & FL_ONGROUND) {
		s->velocity[2] = 0;
		friction(pm, pm->v->friction);
		accelerate(pm, dir, speed, pm->v->accelerate, 0);
		s->velocity[2] = 0;
		step_slide_move(pm);
	} else {
		/* half the gravity before the move and half after keeps jump
		 * height independent of the tick rate */
		s->velocity[2] -= grav * pm->dt * 0.5f;
		accelerate(pm, dir, speed, pm->v->airaccelerate, pm->v->aircap);
		slide_move(pm);
		s->velocity[2] -= grav * pm->dt * 0.5f;
	}
}

/* a box that starts partly inside the world (a spawn point set into the
 * floor, say) would slide through it, since traces leave a starting solid
 * behind: lift it out, or shift it a little sideways, when that frees it */
static void unstick(pm_t *pm)
{
	static const float side[9][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
	pmtrace_t tr;
	float p[3];
	trace(pm, pm->s->origin, pm->s->origin, &tr);
	if (!tr.startsolid)
		return;
	for (int up = 0; up <= 18; up++)
		for (int sd = 0; sd < 9; sd++)
			for (int dist = 1; dist <= (sd ? 4 : 1); dist *= 2) {
				p[0] = pm->s->origin[0] + side[sd][0] * dist;
				p[1] = pm->s->origin[1] + side[sd][1] * dist;
				p[2] = pm->s->origin[2] + up;
				trace(pm, p, p, &tr);
				if (!tr.startsolid) {
					VectorCopy(p, pm->s->origin);
					return;
				}
			}
}

void PM_Move(pmstate_t *s, const pmcmd_t *cmd, const pmvars_t *vars, const pmworld_t *world)
{
	pm_t pm;
	float maxspeed;
	memset(&pm, 0, sizeof(pm));
	pm.s = s;
	pm.cmd = cmd;
	pm.v = vars;
	pm.w = world;
	pm.dt = cmd->msec;
	if (pm.dt <= 0)
		return;
	AngleVectors(cmd->angles, pm.forward, pm.right, pm.up);
	maxspeed = s->maxspeed > 0 ? s->maxspeed : vars->maxspeed;

	if (s->movetype == MOVETYPE_NOCLIP) {
		noclip_move(&pm, BW_MAX(maxspeed, vars->spectatormaxspeed));
		s->flags &= ~FL_ONGROUND;
		s->groundent = -1;
		return;
	}
	if (s->movetype == MOVETYPE_FLY) {
		fly_move(&pm, BW_MAX(maxspeed, vars->spectatormaxspeed));
		categorize(&pm);
		return;
	}

	unstick(&pm);
	categorize(&pm);
	if (s->waterjumptime > 0) {
		s->waterjumptime -= pm.dt;
		if (s->waterjumptime <= 0)
			s->flags &= ~FL_WATERJUMP;
	}
	/* jumping needs the button to be released in between */
	if (cmd->buttons & PM_BUTTON_JUMP) {
		if ((s->flags & FL_ONGROUND) && (s->flags & FL_JUMPRELEASED) && s->waterlevel < 2) {
			s->velocity[2] = vars->jumpspeed;
			s->flags &= ~(FL_ONGROUND | FL_JUMPRELEASED);
			s->groundent = -1;
		} else if (s->waterlevel >= 2) {
			s->velocity[2] = 100; /* swim up */
		}
	} else
		s->flags |= FL_JUMPRELEASED;

	if (s->waterlevel >= 2)
		water_move(&pm, maxspeed);
	else
		walk_move(&pm, maxspeed);
	categorize(&pm);
}
