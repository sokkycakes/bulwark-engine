/* Bulwark engine - player movement as Nuclide's game code runs it (PMoveCustom and
 * ncPlayer::Physics_Run), in C, for the DS link's prediction. ISC licence, see LICENSE.
 * Plain C99 without libnds: tools/nds/pmtest.c builds it on the host. Not to be included
 * with src/common/pmove.h (its pmvars_t is another struct). */
#ifndef BW_PMOVE_NDS_H
#define BW_PMOVE_NDS_H

#include <stdbool.h>
#include <stdint.h>

/* Nuclide's numbers: src/common/c_flags.h, src/shared/input/input.h,
 * src/shared/environment/materials.h, valve/src/shared/flags.h */
#define PM_FL_CLIENT (1u << 3)
#define PM_FL_INWATER (1u << 4)
#define PM_FL_ONGROUND (1u << 9)
#define PM_FL_WATERJUMP (1u << 11)
#define PM_FL_JUMPRELEASED (1u << 12)
#define PM_VFL_PRONE (1u << 0)
#define PM_VFL_ONLADDER (1u << 1)
#define PM_VFL_CROUCHING (1u << 2)
#define PM_VFL_INVEHICLE (1u << 3)
#define PM_VFL_FROZEN (1u << 4)
#define PM_VFL_FAKESPEC (1u << 6)
#define PM_VFL_SPRINTING (1u << 8)
#define PM_VFL_NOATTACK (1u << 10)
#define PM_VFL_FIRING (1u << 13)
#define PM_VFL_AIRJUMPS (7u << 17)
#define PM_VFL_AIRJUMP_ONE (1u << 17)
#define PM_GF_FLASHLIGHT (1u << 1)
enum { PM_IN_ATTACK = 0x01, PM_IN_JUMP = 0x02, PM_IN_PRONE = 0x04, PM_IN_RELOAD = 0x08,
       PM_IN_USE = 0x10, PM_IN_SECONDARY = 0x20, PM_IN_SPRINT = 0x40, PM_IN_CROUCH = 0x80 };
#define PM_CB_SOLID 0x00000001u
#define PM_CB_LAVA 0x00000008u
#define PM_CB_SLIME 0x00000010u
#define PM_CB_WATER 0x00000020u
#define PM_CB_LADDER 0x00004000u
#define PM_CB_PLAYERCLIP 0x00010000u
#define PM_CB_BODY 0x02000000u
#define PM_CB_SKY 0x80000000u
#define PM_CB_FLUID (PM_CB_WATER | PM_CB_SLIME | PM_CB_LAVA | PM_CB_SKY) /* CONTENTBITS_FLUID */
enum { PM_MOVETYPE_NONE = 0, PM_MOVETYPE_WALK = 3, PM_MOVETYPE_FLY = 5, PM_MOVETYPE_TOSS = 6,
       PM_MOVETYPE_NOCLIP = 8 };
enum { PM_SOLID_NOT = 0, PM_SOLID_BBOX = 2, PM_SOLID_SLIDEBOX = 3, PM_SOLID_BSP = 4 };
enum { PM_CONTENT_EMPTY = -1, PM_CONTENT_SOLID = -2, PM_CONTENT_WATER = -3,
       PM_CONTENT_SLIME = -4, PM_CONTENT_LAVA = -5 };
enum { PM_MOVE_NORMAL = 0, PM_MOVE_NOMONSTERS = 1, PM_MOVE_EVERYTHING = 32 };
enum { PM_ENT_NONE = -1, PM_ENT_WORLD = 0 };                     /* else: the server's edict number */
enum { PM_R_AUTOJUMP = 1, PM_R_LONGJUMP = 2, PM_R_HL = 4 };       /* pmstate_t.rules */
enum { PM_EV_JUMPED = 1, PM_EV_LANDED = 2 };                      /* pmstate_t.events */
enum { PM_FOCUS_DROPSTUCK = 1 };                                  /* pmtrace_if.focus flags */

/* the QC trace globals after one trace (trace_*) */
typedef struct {
	float fraction;
	float endpos[3];
	float normal[3];               /* trace_plane_normal */
	bool startsolid, allsolid;
	uint32_t contents;             /* trace_endcontentsi, PM_CB_ bits */
	int ent;                       /* trace_ent: edict number, 0 world or nothing hit */
	int entsolid;                  /* trace_ent.solid (world: 4) */
	float entmins[3], entmaxs[3];  /* trace_ent.absmin, .absmax (world: 0 0 0, never linked) */
} pmhit_t;

typedef struct pmtrace_if {
	void *ctx;
	/* the server's SV_Move (sv_world.c:348-471) for the predicted player as passent:
	 * type PM_MOVE_*, mask the contents that stop the move, 0 for the player's own
	 * (SOLID | BODY | PLAYERCLIP, default_mask sv_world.c:328-337), -1 for all */
	void (*move)(void *ctx, const float start[3], const float mins[3], const float maxs[3],
	             const float end[3], int type, int32_t mask, pmhit_t *tr);
	/* may be NULL. Called once at the start of each PM_Run: the next traces stay within
	 * radius of org (a broadphase that must not change any result); with
	 * PM_FOCUS_DROPSTUCK, boxes that already overlap org+mins..org+maxs are left out
	 * for this PM_Run (stale positions, §6.3). Returns every contents bit (PM_CB_) a
	 * brush entity within the radius can give a trace (its skin's): a ladder or water
	 * probe that can find nothing is then left out */
	uint32_t (*focus)(void *ctx, const float org[3], const float mins[3], const float maxs[3],
	                  float radius, int flags);
} pmtrace_if;

enum { PMV_G_GRAVITY, PMV_ACCELERATE, PMV_AIRACCELERATE, PMV_AIRSTEPSIZE, PMV_BOXCENTER,
       PMV_BOXWIDTH, PMV_CROUCHHEIGHT, PMV_CROUCHSPEED, PMV_CROUCHVIEWHEIGHT, PMV_EDGEFRICTION,
       PMV_FRICTION, PMV_GRAVITY, PMV_JUMPHEIGHT, PMV_AIRJUMPS, PMV_BACKSPEED,
       PMV_GROUNDSPEEDCAP, PMV_JUMPSPEEDCAP, PMV_MAXVIEWPITCH, PMV_MINVIEWPITCH,
       PMV_NOCLIPACCELERATE, PMV_NOCLIPSPEED, PMV_NORMALHEIGHT, PMV_NORMALVIEWHEIGHT,
       PMV_NOSPEEDCAP, PMV_PRONEHEIGHT, PMV_PRONESPEED, PMV_PRONEVIEWHEIGHT, PMV_RUNSPEED,
       PMV_RUNFIRING, PMV_STAMINA, PMV_STAMINARATE, PMV_STAMINATHRESHOLD, PMV_STEPSIZE,
       PMV_STOPSPEED, PMV_WALKSPEED, PMV_WATERACCELERATE, PMV_WATERJUMPHEIGHT, PMV_COUNT };

typedef struct {
	float g[PMV_COUNT]; /* g_pmoveVars: what pmove_custom.qc reads */
	float p[PMV_COUNT]; /* the player's m_pmoveVars: what player_pmove.qc reads */
	/* from p by PM_SetVars, as ncPMoveVars::UpdateBoundingBoxes (pmove.qc:207-239) */
	float standmins[3], standmaxs[3], crouchmins[3], crouchmaxs[3], pronemins[3], pronemaxs[3];
	float standview[3], crouchview[3], proneview[3];
} pmvars_t;

typedef struct {
	float origin[3], velocity[3];
	float basevel[3];          /* m_pmoveBaseVelocity */
	float hookvel[3];          /* m_pmoveHookVelocity (a point) */
	float view_ofs[3], mins[3], maxs[3];
	float v_angle[3];          /* each command sets it to its angles first (entry.qc:377) */
	float angles[3];           /* out: .angles after the move */
	uint32_t flags, vflags, gflags;
	int movetype, solid, waterlevel, watertype;
	int groundent;             /* edict number, 0 world or none */
	float gravity, friction, maxspeed;
	float time_since_jump, time_since_teleport, stamina, speed_target;
	float wsm, wsmfire, wsmzoom, viewzoom; /* PlayerSpeedMod's inputs (§2.4) */
	float health;
	uint8_t rules;             /* PM_R_ */
	float landspeed;           /* out: Physics_Fall's downforce if this command landed, else 0 */
	uint8_t events;            /* out: PM_EV_ of this command */
} pmstate_t;

typedef struct {
	uint32_t seq;
	uint8_t msec;              /* whole milliseconds */
	int16_t pitch, yaw;        /* 65536 a turn, pitch up positive: what the wire carries */
	int16_t move[3];
	uint16_t buttons;          /* PM_IN_ */
	uint8_t impulse;
} pmcmd_t;

void PM_SetVars(pmvars_t *v);                       /* after filling g and p */
void PM_CmdAngles(const pmcmd_t *c, float out[3]);  /* -pitch * (360.0f / 65536.0f), yaw * (360.0f / 65536.0f), 0 */
/* one command: SV_RunClientCommand's v_angle = input_angles, then ncPlayer::Physics_Run
 * (player_pmove.qc:750-818) on s; input_timelength = (float)c->msec / 1000.0f */
void PM_Run(pmstate_t *s, const pmcmd_t *c, const pmvars_t *v, const pmtrace_if *t);
/* sinf(a) and cosf(a) as the server's C library (x86-64 glibc) gives them */
void PM_SinCos(float a, float *s, float *c);
/* QC's degrees to radians: M_PI 3.14159 (fteextensions.qc) * 2 / 360, folded by fteqcc
 * in float; M_PI / 180 folds to the same */
#define PM_QC_DEG2RAD 0x1.1df45cp-6f
/* the QC's anglesToForward (src/common/c_math_vector.h), bit for bit, for the weapon code */
void PM_AnglesToForward(const float angles[3], float fwd[3]);

#endif
