/* Bulwark engine - player movement shared by the server and client
 * prediction (runstandardplayerphysics). ISC licence, see LICENSE.
 *
 * The shape is the classic Quake/Source ground and air acceleration model:
 * friction, then acceleration toward the wish direction, then a slide move
 * that clips against up to five planes and steps over small ledges. Default
 * values follow Source (air acceleration capped at 30 units per second). */
#ifndef BW_PMOVE_H
#define BW_PMOVE_H

#include <stdbool.h>

typedef struct {
	bool allsolid, startsolid;
	float fraction;
	float endpos[3];
	float normal[3];
	int ent;
} pmtrace_t;

typedef struct {
	void *ctx;
	void (*trace)(void *ctx, const float *start, const float *mins, const float *maxs, const float *end, pmtrace_t *tr);
	int (*contents)(void *ctx, const float *p); /* Quake CONTENT_* values */
} pmworld_t;

typedef struct pmvars_s {
	float gravity, maxspeed, accelerate, airaccelerate, wateraccelerate, friction, waterfriction;
	float stopspeed, stepheight, jumpspeed, spectatormaxspeed, aircap;
} pmvars_t;

typedef struct {
	float origin[3], velocity[3], mins[3], maxs[3];
	int flags;      /* FL_ONGROUND, FL_JUMPRELEASED, FL_WATERJUMP */
	int movetype;   /* MOVETYPE_WALK, _NOCLIP, _FLY */
	float gravity;  /* entity gravity scale, 0 means 1 */
	float maxspeed; /* entity override, 0 means the default */
	int groundent;  /* -1 when airborne */
	int waterlevel, watertype;
	float waterjumptime;
} pmstate_t;

typedef struct {
	float msec; /* seconds */
	float angles[3];
	float movevalues[3]; /* forward, side, up */
	unsigned buttons;
} pmcmd_t;

#define PM_BUTTON_JUMP 2

void PM_DefaultVars(pmvars_t *v);
void PM_Move(pmstate_t *s, const pmcmd_t *cmd, const pmvars_t *vars, const pmworld_t *world);

#endif
