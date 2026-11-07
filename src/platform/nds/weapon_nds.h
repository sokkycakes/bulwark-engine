/* Bulwark engine - the weapon in hand as Nuclide's game code runs it (ncWeapon,
 * src/shared/game/Weapon.qc, from ncPlayer::Physics_InputPostMove and
 * ProcessInput), in C, for the DS link's prediction. ISC licence, see LICENSE.
 * Plain C99 without libnds: tools/nds/weptest.c builds it on the host. */
#ifndef BW_WEAPON_NDS_H
#define BW_WEAPON_NDS_H

#include <stdbool.h>
#include <stdint.h>

#include "pmove_nds.h"

#define W_MAX_SEQ 64 /* view model sequences WEAPONINFO carries */
#define W_MAX_EV 32  /* muzzle flash events, all sequences */
#define W_MAX_ACT 8  /* sequences one activity picks among (co-op's machine pistols have 6); a
                        * longer list makes the weapon one the device doesn't run (WI_TOOMANYACTS) */
#define W_AMMO 4     /* ammo types PSTATE carries */

/* Weapon.qc's activities ("act_<name>"), in WEAPONINFO's numbering */
enum { WA_FIRE, WA_FIRELAST, WA_FIREFAILED, WA_FIRESTART, WA_FIRESTOP, WA_RELOAD, WA_RELOADEMPTY, WA_RELOADSTART,
       WA_RELOADEND, WA_DRAW, WA_DRAWEMPTY, WA_IDLE, WA_IDLEEMPTY, WA_COCK, WA_LOOP, WA_DELAY, WA_RELEASE,
       WA_COOKING, WA_DETONATE, WA_MODEON, WA_MODEOFF, WA_COUNT };
/* nsweapon_state_t (src/shared/game/Weapon.h) */
enum { WS_IDLE, WS_COCK, WS_CHARGING, WS_FIRELOOP, WS_RELEASED, WS_OVERHEATED, WS_DRAW, WS_RELOAD_START,
       WS_RELOAD, WS_RELOAD_END, WS_BURSTFIRING };
/* a fireInfo's switches (wfire_t.flags) */
enum { WF_AMMOREQ = 1, WF_SEMIAUTO = 2, WF_UNDERWATER = 4, WF_LOOPING = 8, WF_COCKS = 16, WF_DRAWAFTER = 32,
       WF_RELOADTIMED = 64, WF_ONFIRE = 128, WF_ONRELEASE = 256, WF_DETONATE = 512, WF_SERVERFIRE = 1024 };
/* the weapon decl's (winfo_t.flags) */
enum { WI_ALTALTERNATES = 1, WI_REMOVEONEMPTY = 2, WI_POWERAMMO = 4, WI_ALTMODE = 8, WI_BUGGYIDLE = 16,
       WI_SECONDARY = 32,
       WI_TOOMANYACTS = 128 /* the parser's own: an activity lists more than W_MAX_ACT sequences */ };
/* wstate_t.flags */
enum { WST_FIRING = 1, WST_WILLRELEASE = 2 };
/* the player's flags Weapon.qc reads or sets (src/common/c_flags.h, valve/src/shared/flags.h) */
#define W_VFL_INVEHICLE (1u << 3)
#define W_VFL_PRIMEDFUSE (1u << 11)
#define W_VFL_FIRING (1u << 13)
#define W_VFL_HOLDING (1u << 16)
#define W_GF_SEMI_TOGGLED (1u << 0)
#define W_VFL_BITS (W_VFL_PRIMEDFUSE | W_VFL_FIRING | W_VFL_HOLDING) /* what the weapon code owns */
#define W_GF_BITS W_GF_SEMI_TOGGLED

/* one fireInfo, as ncWeapon::UpdateFireInfoCache keeps it (Weapon.qc:126-256) */
typedef struct {
	uint16_t flags;          /* WF_ */
	int8_t ammotype;         /* m_fiAmmoType, -1 none */
	int16_t ammopershot, depleteammo;
	float punch[3], spring[3], punchkick, firerate, chargetime, overheat, overheatlen, burstcount, bursttime;
	float knockback, knockbackrel, testdist, spinup, reloadtime, reloadstarttime, zoomfov, failrate;
	float sweepangle, sweeprange, regenrate, depleterate;
	uint8_t nact[WA_COUNT];
	int16_t act[WA_COUNT][W_MAX_ACT];
} wfire_t;

typedef struct {
	float dur, fps;       /* frameduration as the server's game gets it; frames a second */
	uint16_t frames;
	uint8_t loop, nev, firstev;
} wseq_t;

typedef struct {
	uint16_t frame, code; /* 5001, 5011, 5021 or 5031 */
	uint8_t opt;          /* the event's options as a number (size and sprite) */
} wev_t;

/* WEAPONINFO: the weapon decl as Weapon.qc reads it, and its view model */
typedef struct {
	uint16_t edict, version, viewmodel;
	uint8_t flags;               /* WI_ */
	int8_t infinite;             /* g_infiniteAmmo */
	int8_t primammo, secammo;    /* m_primaryAmmoType, m_secondaryAmmoType */
	float primedfuse;
	float fallmin, landmin, fallpunch, fallspring; /* falldamage decl: Physics_Fall's punch */
	uint8_t nfi;                 /* 1, or 2 with a def_altFireInfo */
	wfire_t fi[2];
	uint8_t nseq, nev;
	wseq_t seq[W_MAX_SEQ];
	wev_t ev[W_MAX_EV];
} winfo_t;

/* what Weapon.qc and Physics_InputPostMove keep between commands: the player's
 * timers, view model and punch, and the weapon's own (PSTATE's weapon block) */
typedef struct {
	uint16_t edict, infover;     /* the weapon in hand (0 none) and its WEAPONINFO version (0 unknown) */
	uint8_t state, fi, flags;    /* WS_; m_weaponLastFireInfo (0 primary, 1 secondary, 255 other); WST_ */
	int16_t clip, clipsize;
	float burst, fiv, overheat, overheatlen, reloadstep, spin;
	float atk, idle, rel;        /* m_timeUntilNextAttack, NextIdle, Reloaded */
	int16_t animseq;             /* m_weaponAnimSequence */
	float animtime;              /* m_weaponAnimTime */
	float punch[3], punchvel[3];
	float firescale;             /* the "*fireScale" player key, 1 without */
	uint32_t vflags, gflags;     /* the player's: the W_VFL_BITS and W_GF_BITS are the weapon's */
	uint8_t nammo;
	int8_t ammotype[W_AMMO];
	int16_t ammo[W_AMMO];
} wstate_t;

/* the rest of the player as the command left it (the movement runs first) */
typedef struct {
	uint32_t seq;          /* input_sequence */
	float health;
	int waterlevel;
	bool landed;           /* Physics_Fall ran: this command landed */
	float downforce;       /* and with what */
	float eye[3];          /* origin + view_ofs, for a melee weapon's test */
	float *velocity;       /* knockback goes here; may be NULL */
	const pmtrace_if *trace; /* a melee weapon's test traces; may be NULL (no hulls) */
	int nboxes;            /* the server's boxes near the player (PSTATE), for a melee sweep */
	const float (*boxmins)[3], (*boxmaxs)[3];
	const uint8_t *boxsolid;
} wenv_t;

/* WEAPONINFO's body after the version (docs/spec/link.md); false if it doesn't parse */
bool Weapon_ParseInfo(winfo_t *w, const uint8_t *d, int len);
/* PSTATE's weapon block, from its 'W'; returns its length, 0 if it doesn't parse */
int Weapon_ParseState(wstate_t *s, const uint8_t *d, int len);
/* the info can run this state: the same weapon, the version PSTATE names, a fireInfo it knows */
bool Weapon_Usable(const wstate_t *s, const winfo_t *w);
/* one command: Physics_Fall's punch, Physics_InputPostMove's timers and punch, then
 * ncPlayer::ProcessInput's weapon part. w NULL: only the player's part (no weapon,
 * or one the device can't run) */
void Weapon_Run(wstate_t *s, const winfo_t *w, const pmcmd_t *c, const wenv_t *env);
/* every field the weapon code reads, within float rounding */
bool Weapon_Matches(const wstate_t *a, const wstate_t *b);
/* the muzzle flash events seq plays between base and target seconds, as the PC's
 * processmodelevents (src/qcvm/qc_modelevents.c) finds them; returns how many */
int Weapon_Events(const winfo_t *w, int seq, float base, float target, const wev_t **out, int max);
/* ammo of a type the state carries, -1 if it carries none */
int Weapon_Ammo(const wstate_t *s, int type);

#endif
