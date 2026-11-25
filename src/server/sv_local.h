/* Bulwark engine - server internals. ISC licence, see LICENSE. */
#ifndef BW_SV_LOCAL_H
#define BW_SV_LOCAL_H

#include "../qcvm/qcvm.h"
#include "bspfile.h"
#include "../net/netchan.h"
#include "../net/protocol.h"
#include "../common/infoblob.h"
#include "../common/skel.h"

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
extern int sv_spawncount; /* maps spawned so far; the host joins each new one */

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
#define MAX_CL_STATS 256

typedef struct client_s {
	clientstate_t state;
	bool bot;
	int link; /* link player (sv_link.c): its slot + 1, 0 for none; bot is set too */
	bool linkcmds; /* a link player that sends its own commands (link v2): not driven by the bot loop */
	netchan_t chan;
	char name[64];
	char userinfo[2048];
	infoblob_t blobs[MAX_INFOBLOBS];
	int edict;
	int spawncount; /* map the client was last told about */
	double spawn_at; /* a bot carried over to a new map: spawn it once the map has run this long (sv.time) */
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
	bool fullupdate;  /* resent the signon for a demo recording; "begin" doesn't respawn */
	bool sixdof, spiderpig; /* the 6dof and spiderpig cheats */
	int forced_impulse;     /* from bestweapon or sv_impulse, for the next move */
	int rank;               /* ranking entry + 1, 0 for none (sv_admin.c) */
	float rank_frags;       /* frags already counted into the ranking */
	double rank_since;      /* when time on the server was last counted */
	double chat_times[16];  /* recent chat, for flood protection */
	int chat_head;
	double silenced_until;
	uint8_t stattype[MAX_CL_STATS]; /* what the client was last sent (sv_stats.c), 0 none */
	uint32_t statval[MAX_CL_STATS];
	char *statstr[MAX_CL_STATS];
	uint32_t vignore[8];    /* players whose voice this one doesn't want (sv_voip.c) */
	strbuf_t voicegram;     /* voice for the next packet, kept out of recordings */
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
void SV_ClientsDroppedTime(double t);
void SV_SendClientMessages(void);
void SV_ClientsAfterPhysics(void);
void SV_RunUsercmd(client_t *c, const usercmd_t *u); /* one command, as a client's move runs it */
void SV_BroadcastPrecache(int kind, int index, const char *name);
void SV_ClientPrint(client_t *c, int level, const char *text);
void SV_BroadcastPrint(int level, const char *text); /* to every player and the console */
void SV_StuffText(client_t *c, const char *text);    /* console text for the client to run */
void SV_SetPaused(bool paused);
/* sv_cmds.c: server console and admin commands, client fallbacks */
void SV_InitCommands(void);
bool SV_ClientCheatCommand(client_t *c, int argc, const char **argv);
bool SV_CheckBanned(const netadr_t *from, char *reason, size_t reasonsize);
bool SV_HandleRcon(const netadr_t *from, const char *text);
int SV_SpawnEntitiesFromText(const char *data, int *first);
bool SV_RequestPause(int requester, bool want); /* asks SV_ShouldPause first */
bool SV_SetServerinfoKey(const char *key, const char *value);
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
/* One physics frame for entity e of vm: think, then the movetype's movement
 * (only the think when move is false). The client game runs its own
 * entities through this; see docs/spec/client-game.md. */
void W_RunEntityPhysics(qcvm_t *vm, int e, double time, float frametime, float gravity, bool move);
void SV_SyncServerinfoCvars(bool broadcast);
extern const qc_builtin_t sv_client_builtins[];
void SV_ClientReliableMsg(client_t *c, const msg_t *m);
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

/* sv_compat.c: FTE and QuakeWorld server commands */
void SV_InitCompat(void);
bool SV_ClientCompatCommand(client_t *c, int argc, const char **argv);
client_t *SV_FindClient(const char *name_or_slot);
void SV_SetPlayerKey(client_t *c, const char *key, const char *value, bool broadcast);

/* sv_admin.c: moderation, logs and rankings */
enum { PEN_KIND_MUTE, PEN_KIND_STEALTH, PEN_KIND_CUFF, PEN_KIND_CRIPPLE, PEN_KINDS };
#define PEN_MUTE (1 << PEN_KIND_MUTE)
#define PEN_STEALTH (1 << PEN_KIND_STEALTH)
#define PEN_CUFF (1 << PEN_KIND_CUFF)
#define PEN_CRIPPLE (1 << PEN_KIND_CRIPPLE)
enum { CHAT_PASS, CHAT_DROP, CHAT_ONLY_SENDER };
extern client_t *sv_chat_only; /* while set, prints and multicasts reach only this client */
void SV_InitAdmin(void);
void SV_InitRoutes(void);
int SV_Penalties(const client_t *c);
void SV_AdminFilterMove(client_t *c, usercmd_t *cmd);
int SV_AdminFilterChat(client_t *c);
bool SV_AdminCheckConnect(const char *userinfo, char *why, size_t whysize);
void SV_AdminConnected(client_t *c);
void SV_AdminDisconnected(client_t *c);
void SV_AdminRenamed(client_t *c, const char *oldname);
bool SV_AdminClientCommand(client_t *c, int argc, const char **argv, const char *line);
int SV_RankRconLevel(const char *login);
void SV_LogRcon(const netadr_t *from, const char *command);
void SV_LogFrag(int killer, int victim);
void SV_CompatPostThink(client_t *c);
void SV_CompatFrame(void);
bool SV_CompatConnectionless(const netadr_t *from, int argc, const char **argv);

/* sv_save.c: saved games */
void SV_InitSaves(void);
bool SV_SaveGame(const char *name, const char *comment);
bool SV_LoadGame(const char *name);
void SV_SaveLevelChanged(void);        /* after changelevel: autosave once the player is in */
void SV_SaveClientSpawned(client_t *c); /* a player entered the game */
/* true once for each player whose entity a loaded game put back */
bool SV_SaveTakeRestored(client_t *c);

/* sv_main.c */
void SV_Init(void);
bool SV_SpawnServer(const char *mapname);
void SV_Shutdown(void);
void SV_DropClients(const char *reason); /* sv_cmds.c */
void SV_Frame(double dt);
int SV_ModelIndex(const char *name, bool precache);
int SV_SoundIndex(const char *name, bool precache);
void SV_InfoSet(char *info, size_t size, const char *key, const char *value);
const char *SV_InfoGet(const char *info, const char *key, char *out, size_t outsize);
const char *SV_InfoPublic(const char *info, char *out, size_t outsize);
int SV_Spawn(void);
bool SV_ConsoleCommand(const char *line);
/* "say" from the server console; false when no server is running */
bool SV_ConsoleSay(const char *msg);

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
	struct area_s *area; /* sv_world.c's index of linked entities, made on first use */
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
bool SV_InsideBrushEntity(int pusher, int e);
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
const framegroup_t *SV_ModelFramegroups(int modelindex, int *count); /* sv_builtins.c */

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
/* sv_uri.c: uri_get for every module, finished requests are delivered by QC_URIFrame */
extern const qc_builtin_t qc_uri_builtins[];
void QC_URIFrame(void);
void QC_URIDrain(double seconds);
void W_bi_getsurfacenearpoint(qcvm_t *vm);
void W_bi_getsurfacetexture(qcvm_t *vm);

/* sv_stats.c */
extern const qc_builtin_t sv_stat_builtins[];

/* sv_link.c: handhelds as companion players (docs/spec/link.md) */
extern const qc_builtin_t sv_link_builtins[];
void SV_LinkInit(void);
void SV_LinkReadPackets(void);
void SV_LinkSend(void);
void SV_LinkClientDropped(client_t *c, const char *reason);
void SV_LinkMulticast(client_t *c, const uint8_t *data, int len, int cause); /* bullet impacts for a DS */
/* what a link player's client would show (sv_link.c, EVENT): prints, centerprints, the game's messages */
void SV_LinkPrint(client_t *c, int level, const char *text);
void SV_LinkCenterPrint(client_t *c, const char *text);
void SV_LinkGameMessages(client_t *c, const uint8_t *data, int len);
void SV_LinkQCWrite(client_t *c, const void *data, int len);
/* the server's sound() and pointsound() for a link player (svc_sound skips bots) */
void SV_LinkSound(client_t *c, int e, int chan, int idx, float vol, float atten, float pitch, int flags, const float *org);
void SV_LinkSoundLog(int e, int chan, int idx);
void SV_LinkStuffCmd(client_t *c, const char *text); /* play, playvol, music for a link player */
client_t *SV_SpawnLinkClient(const char *name, int link);
void SV_UpdateStats(client_t *c);
void SV_ResetStats(client_t *c);
void SV_ClientReliable(client_t *c, const void *data, size_t len);

/* sv_mvd.c: server demos and QTV streams */
void SV_InitMVD(void);
/* sv_master.c */
void SV_InitMaster(void);
bool SV_MasterConnectionless(const netadr_t *from, int argc, const char **argv);
void SV_MasterPortsOpened(void);
void SV_MasterMapSpawned(void);
void SV_MasterFrame(void);
void SV_GreetPins(client_t *c);
/* sv_download.c */
void SV_InitDownloads(void);
bool SV_DownloadCommand(client_t *c, int argc, const char **argv);
void SV_DownloadFrame(client_t *c);
void SV_ParseUpload(client_t *c, msg_t *m);
void SV_FreeTransfers(client_t *c);

/* sv_sql.c */
extern const qc_builtin_t sv_sql_builtins[];
void SV_InitSQL(void);
void SV_SQLShutdown(void);

/* sv_compile.c */
void SV_InitCompile(void);
bool QC_Decompile(const char *name, const uint8_t *d, size_t size, strbuf_t *out);

/* sv_voip.c */
void SV_InitVoip(void);
void SV_ParseVoice(client_t *c, msg_t *m);
bool SV_VoipCommand(client_t *c, int argc, const char **argv);
void SV_MVDFrame(void);
void SV_MVDMapChanged(void);
void SV_MVDMapSpawned(void);
void SV_MVDShutdown(void);
void SV_MVDReliable(client_t *c, const void *data, size_t len);
void SV_MVDPendingFlags(int e, uint32_t flags);
void SV_MVDSnapshot(client_t *c, const uint8_t *dgram, size_t len);
void SV_MVDUsercmd(client_t *c, const usercmd_t *u);
void SV_MVDClientSpawning(client_t *c);
void SV_MVDClientDropped(client_t *c);
/* sv_client.c, for recordings */
void SV_ShadowSnapshot(const client_t *real, client_t *shadow, msg_t *m);
void SV_ShadowReset(client_t *shadow);
void SV_ShadowFree(client_t *shadow);
void SV_CaptureSignon(client_t *c, strbuf_t *out);

#endif
