/* Bulwark engine - server internals. ISC licence, see LICENSE. */
#ifndef BW_SV_LOCAL_H
#define BW_SV_LOCAL_H

#include "../qcvm/qcvm.h"
#include "bspfile.h"

#define SV_MAX_MODELS 4096
#define SV_MAX_SOUNDS 4096
#define SV_MAX_PARTICLES 1024
#define SV_MAX_LIGHTSTYLES 256
#define SV_MAX_COMMANDS 512

/* values shared with QC, from the constants the QC headers define */
enum {
	MOVETYPE_NONE = 0, MOVETYPE_WALK = 3, MOVETYPE_STEP = 4, MOVETYPE_FLY = 5, MOVETYPE_TOSS = 6,
	MOVETYPE_PUSH = 7, MOVETYPE_NOCLIP = 8, MOVETYPE_FLYMISSILE = 9, MOVETYPE_BOUNCE = 10,
	MOVETYPE_BOUNCEMISSILE = 11, MOVETYPE_FOLLOW = 12, MOVETYPE_PHYSICS = 32
};
enum {
	SOLID_NOT = 0, SOLID_TRIGGER = 1, SOLID_BBOX = 2, SOLID_SLIDEBOX = 3, SOLID_BSP = 4,
	SOLID_CORPSE = 5, SOLID_PORTAL = 21, SOLID_BSPTRIGGER = 22
};
enum {
	MOVE_NORMAL = 0, MOVE_NOMONSTERS = 1, MOVE_MISSILE = 2, MOVE_HITMODEL = 4, MOVE_TRIGGERS = 16,
	MOVE_EVERYTHING = 32, MOVE_LAGGED = 64, MOVE_ENTCHAIN = 128, MOVE_OTHERONLY = 256
};
enum { CONTENT_EMPTY = -1, CONTENT_SOLID = -2, CONTENT_WATER = -3, CONTENT_SLIME = -4, CONTENT_LAVA = -5, CONTENT_SKY = -6, CONTENT_LADDER = -16 };
#define FL_ONGROUND 512
#define FL_PARTIALGROUND 1024
#define FL_CLIENT 8

typedef struct {
	bspmap_t *bsp; /* owning map for brush models, or NULL */
	int submodel;
	float mins[3], maxs[3];
} svmodel_t;

typedef struct {
	bool active;
	qcvm_t *vm;
	bspmap_t *world;
	char mapname[64];
	char worldpath[128];
	double time;
	float frametime;

	char *models[SV_MAX_MODELS];
	svmodel_t modelinfo[SV_MAX_MODELS];
	int nummodels;
	char *sounds[SV_MAX_SOUNDS];
	int numsounds;
	char *particles[SV_MAX_PARTICLES];
	int numparticles;
	char *lightstyles[SV_MAX_LIGHTSTYLES];
	char *commands[SV_MAX_COMMANDS];
	int numcommands;

	char serverinfo[8192];
	char localinfo[8192];
	const char *entitytoken_pos; /* getentitytoken cursor */
	bool spawning;
	int frames;
} server_t;

extern server_t sv;

/* sv_main.c */
void SV_Init(void);
bool SV_SpawnServer(const char *mapname);
void SV_Shutdown(void);
void SV_Frame(double dt);
int SV_ModelIndex(const char *name, bool precache);
int SV_SoundIndex(const char *name, bool precache);
void SV_InfoSet(char *info, size_t size, const char *key, const char *value);
const char *SV_InfoGet(const char *info, const char *key, char *out, size_t outsize);
int SV_Spawn(void);
bool SV_ConsoleCommand(const char *line);

/* sv_world.c */
typedef struct {
	bool allsolid, startsolid, inopen, inwater;
	float fraction;
	float endpos[3];
	float normal[3];
	float dist;
	int ent;
	int contents;
	int surfaceflags;
	const char *surfacename;
	int brush, brushside;
} svtrace_t;

void SV_LinkEdict(int e, bool touch_triggers);
void SV_UnlinkEdict(int e);
void SV_Move(const float *start, const float *mins, const float *maxs, const float *end, int type, int passent, svtrace_t *tr);
int SV_PointContentsBits(const float *p);
int SV_ContentsToQuake(int bits);
void SV_TouchTriggers(int e);
void SV_SetTraceGlobals(const svtrace_t *tr);
bool SV_DropToFloor(int e);

/* sv_phys.c (inside sv_main.c for now) */
void SV_Physics(void);

extern const qc_builtin_t sv_builtins[];

#endif
