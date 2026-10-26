/* Bulwark engine - server internals. ISC licence, see LICENSE. */
#ifndef BW_SV_LOCAL_H
#define BW_SV_LOCAL_H

#include "../qcvm/qcvm.h"
#include "bspfile.h"
#include "../net/netchan.h"
#include "../net/protocol.h"
#include "../common/infoblob.h"

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
	struct vxworld_s *voxel;
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
	bool paused;
	int frames;
} server_t;

extern server_t sv;

/* --- clients (sv_client.c) --- */
typedef enum { CS_FREE, CS_CONNECTED, CS_SPAWNED } clientstate_t;

/* per client, per entity send state */
#define ES_KNOWN 1      /* the client has (or is being sent) this entity */
#define ES_CONFIRMED 2  /* its creation was acknowledged, so deltas are safe */
#define ES_NEEDREMOVE 4 /* a removal was lost and must be resent */
#define SENDFLAGS_ALL 0xffffffu

typedef struct {
	uint32_t ent;
	uint32_t flags;
	uint8_t remove, create;
} sentrec_t;

#define SNAP_HISTORY 256 /* outgoing packets whose entity contents we remember */
typedef struct {
	uint32_t seq;
	int num, cap;
	sentrec_t *recs;
} snaprec_t;

#define MAX_SPAWN_PARMS 16

typedef struct client_s {
	clientstate_t state;
	bool bot;
	netchan_t chan;
	char name[64];
	char userinfo[2048];
	infoblob_t blobs[MAX_INFOBLOBS];
	int edict;
	int spawncount; /* map the client was last told about */
	double connect_time, last_ping_update;
	float spawn_parms[MAX_SPAWN_PARMS];
	bool have_parms;

	uint32_t lastcmd; /* servercommandframe: last usercmd sequence executed */
	usercmd_t lastucmd;
	float cmd_budget; /* seconds of movement the client may still submit (speedhack guard) */

	uint32_t *pending;
	uint8_t *estate;
	int estate_count;
	uint32_t ent_rotor; /* where the next snapshot starts scanning */
	snaprec_t snaps[SNAP_HISTORY];

	/* QC writes to MSG_ONE/MSG_ALL/reliable multicasts, flushed as one svc_qcmessage */
	strbuf_t qcrel;
	/* unreliable extras for the next packet (prints, multicasts, sounds) */
	strbuf_t datagram;

	double next_snapshot;
	float updaterate; /* snapshots per second (cl_updaterate) */
	float interp;     /* client's interpolation delay, seconds (for lag compensation) */
} client_t;

typedef struct {
	client_t *clients;
	int maxclients;
	int spawncount;
	struct {
		netadr_t adr;
		int challenge;
		double time;
	} challenges[64];
	char *signon; /* MSG_INIT bytes */
	size_t signonlen;
} serverstatic_t;

extern serverstatic_t svs;

void SV_InitClients(void);
void SV_ClientsMapChanged(void); /* after a new map is spawned */
void SV_ReadPackets(void);
void SV_ClientsPreThink(void);
void SV_ClientsPostThink(void);
void SV_SendClientMessages(void);
void SV_ClientsAfterPhysics(void);
void SV_BroadcastPrecache(int kind, int index, const char *name);
void SV_BroadcastLightstyle(int index, const char *style);
void SV_BroadcastServerinfo(const char *key, const char *value);
client_t *SV_ClientForEdict(int e);
void SV_DropClient(client_t *c, const char *reason);
void SV_LagCompensationRecord(void);
void SV_SetupClientSlots(void);
void SV_SaveSpawnParms(void);
void SV_RunStandardPlayerPhysics(int e);
void SV_InitPMove(void);
struct pmvars_s;
void W_RunPlayerPhysics(qcvm_t *vm, int e, const struct pmvars_s *vars);
void SV_SyncServerinfoCvars(bool broadcast);
extern const qc_builtin_t sv_client_builtins[];
void SV_ClientReliable(client_t *c, const msg_t *m);
void SV_BroadcastReliable(const msg_t *m);
/* voxel worlds (sv_voxel.c) */
void SV_VoxelSignon(client_t *c);
void SV_VoxelShutdown(void);
extern const qc_builtin_t sv_voxel_builtins[];
extern struct vxworld_s *vx_clientworld;
const int *SV_LastVoxelCell(void); /* x, y, z, type of the last trace's voxel hit; type -1 if none */
void VX_bi_voxel_cellforpoint(qcvm_t *vm);
void VX_bi_voxel_cellorigin(qcvm_t *vm);
void VX_bi_voxel_size(qcvm_t *vm);
void VX_bi_voxel_blocksize(qcvm_t *vm);
void VX_bi_voxel_count(qcvm_t *vm);
void VX_bi_voxel_typeforname(qcvm_t *vm);
void VX_bi_voxel_typename(qcvm_t *vm);
void VX_bi_voxel_tracecell(qcvm_t *vm);
void VX_bi_voxel_tracetype(qcvm_t *vm);
void VX_bi_voxel_client_get(qcvm_t *vm);
void VX_bi_voxel_client_getrot(qcvm_t *vm);

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
	int voxelcell[4]; /* x, y, z, type of a voxel hit; [3] is -1 when none */
} svtrace_t;

typedef struct worldctx_s {
	qcvm_t *vm;
	bspmap_t *world;
	bool (*brushmodel)(int modelindex, bspmap_t **bsp, int *submodel);
	struct vxworld_s *voxel; /* the voxel grid, part of the world (ent 0) */
} worldctx_t;
/* Points the world functions (SV_Move and friends) at another module's
 * entities, e.g. the client's; NULL restores the server. Returns the previous
 * context (NULL for the server). */
worldctx_t *SV_SetWorldContext(worldctx_t *w);
bspmap_t *SV_CurrentWorld(void); /* the map of the current context */

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

/* sv_route.c: route_calculate over data/<map>.way */
extern const qc_builtin_t sv_route_builtins[];
void SV_RunRoutes(void);
void SV_FreeRoutes(void);
const char *SV_ModelName(qcvm_t *vm, int index);

/* world builtins that work on whichever VM and world context is current;
 * the client calls them with its own context set */
void W_bi_setorigin(qcvm_t *vm);
void W_bi_setsize(qcvm_t *vm);
void W_bi_traceline(qcvm_t *vm);
void W_bi_tracebox(qcvm_t *vm);
void W_bi_pointcontents(qcvm_t *vm);
void W_bi_droptofloor(qcvm_t *vm);
void W_bi_checkbottom(qcvm_t *vm);
void W_bi_walkmove(qcvm_t *vm);
void W_bi_changeyaw(qcvm_t *vm);
void W_bi_touchtriggers(qcvm_t *vm);
void W_bi_checkpvs(qcvm_t *vm);

#endif
