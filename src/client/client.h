/* Bulwark engine - client. ISC licence, see LICENSE. */
#ifndef BW_CLIENT_H
#define BW_CLIENT_H

#include "../common/common.h"
#include "../common/cvar.h"
#include "../net/netchan.h"
#include "../net/protocol.h"
#include "../qcvm/qcvm.h"
#include "../server/bspfile.h"
#include "../common/infoblob.h"

#define CL_MAX_MODELS 4096
#define CL_MAX_SOUNDS 4096
#define CL_MAX_PARTICLES 1024
#define CL_MAX_PLAYERS 256
#define CL_CMD_BACKUP 256 /* input frames remembered for prediction, power of two */

typedef enum {
	ca_disconnected,
	ca_challenging, /* waiting for "challenge" */
	ca_connecting,  /* waiting for "accept" */
	ca_loading,     /* connected, receiving signon data */
	ca_active       /* in the game */
} connstate_t;

typedef struct {
	char *name;
	bspmap_t *bsp;  /* for "*N" submodels and .bsp models, collision */
	int submodel;
	float mins[3], maxs[3];
	void *render;   /* renderer's data */
} clmodel_t;

/* recent network states of a server entity, for render interpolation */
#define CL_INTERP_SAMPLES 16
typedef struct {
	double time;
	float origin[3];
	float angles[3];
	float frame, frame1time;
} interpsample_t;

typedef struct {
	int csqc_ent; /* the csprogs entity standing for this server entity, 0 if none */
	int seat;     /* split screen: the seat whose part of the snapshots it came in */
	interpsample_t samples[CL_INTERP_SAMPLES];
	int numsamples;
} clentity_t;

/* Split screen: what each local player has of their own. The active seat's
 * scalars live in cl (playerslot, playerent, the command frames, view
 * angles) so the rest of the client works on whichever seat is active;
 * CL_SetSeat swaps them. Its arrays (stats, the command ring) are always
 * here, and cl points at the active seat's. docs/spec/splitscreen.md */
typedef enum {
	SEAT_FREE,    /* nobody */
	SEAT_JOINING, /* asked the server for a slot */
	SEAT_LOADING, /* has a slot, waiting to spawn */
	SEAT_ACTIVE   /* playing */
} seatstate_t;

typedef struct {
	seatstate_t state;
	bool guest;
	unsigned device; /* input devices, a bit each: 0 keyboard and mouse, 1..4 controller slot + 1 */
	char name[64];
	double join_time;
	/* swapped with cl's while active */
	int playerslot, playerent;
	uint32_t clientcommandframe, servercommandframe;
	double cmd_start;
	float viewangles[3];
	/* cl points at these while active */
	uint8_t stattype[256];
	uint32_t statval[256];
	char *statstr[256];
	usercmd_t cmds[CL_CMD_BACKUP];
} clseat_t;

extern clseat_t cl_seats[MAX_SEATS];

typedef struct {
	connstate_t state;
	netadr_t server;
	char servername[128];
	netchan_t chan;
	int qport;
	int challenge;
	double connect_time;
	int connect_tries;
	char disconnect_reason[256];

	/* from svc_serverdata */
	int spawncount;
	float tickrate;
	int maxclients;
	int playerslot;
	int playerent;
	char mapname[64];
	char worldpath[128];
	char csprogsname[64];
	uint16_t csprogscrc;
	char serverinfo[8192];
	char playerinfo[CL_MAX_PLAYERS][2048];
	infoblob_t playerblobs[CL_MAX_PLAYERS][MAX_INFOBLOBS];

	clmodel_t models[CL_MAX_MODELS];
	int nummodels;
	char *sounds[CL_MAX_SOUNDS];
	int numsounds;
	char *particles[CL_MAX_PARTICLES];
	char *lightstyles[256];
	/* the active seat's stats (svc_stats): type 0 none, 1 string, 2 float, 8 integer */
	uint8_t *stattype;
	uint32_t *statval;
	char **statstr;
	bspmap_t *world;

	/* timing */
	double realtime;
	double frametime;
	double servertime;       /* time of the newest snapshot */
	double servertime_recv;  /* realtime at which it arrived */
	double time;             /* smoothed estimate of current server time */
	uint32_t servertick;
	uint32_t servercommandframe;
	bool paused;

	/* input */
	usercmd_t *cmds; /* the active seat's */
	uint32_t clientcommandframe; /* the command being built; earlier ones are sent */
	double cmd_start;            /* realtime the current command began */
	double next_cmd;
	float viewangles[3];

	/* csqc */
	qcvm_t *csqc;
	clentity_t *ents;
	int numents;
	double lastsnapshot_real;
	int snapshots_received;
	bool csqc_initialised;
	bool worldloaded;

	int seat; /* the active split screen seat, 0 outside the places that switch */

	/* local server if this process hosts one */
	bool listen;
	bool demoplayback; /* the server messages come from a demo file */
} clstate_t;

extern clstate_t cl;

/* screen effects and readouts the engine draws over the game (cl_cmds.c,
 * drawn by r_overlay.c) */
typedef struct {
	float flash[4], flash_fade; /* "bf": fades out by flash_fade alpha per second */
	float cshift[4];            /* "v_cshift": steady tint */
	float fade_color[3];        /* "fadein"/"fadeout" */
	double fade_start;
	float fade_time;
	bool fade_out, fading;
	char centerprint[1024];     /* only when the game doesn't draw centre prints */
	double centerprint_time;
	cvar_t *showfps, *showpos, *netgraph, *drawnotify, *notifytime, *notifylines, *drawhud, *crosshair;
} clscreen_t;
extern clscreen_t cl_screen;
void CL_InitCommands(void);
bool SV_ConsoleSay(const char *msg);

/* cl_seat.c: split screen seats */
void CL_SetSeat(int seat);
int CL_NumSeats(void);        /* numclientseats: the highest playing seat + 1, 0 while loading */
bool CL_SeatPlaying(int seat);
void CL_SeatRect(int seat, float r[4]); /* the seat's view: x, y, width, height as fractions of the screen */
int CL_SeatForDevice(int device); /* -1 for none */
void CL_SeatUserinfo(int seat, char *out, size_t size); /* cl_main.c */
void CL_InitSeatCommands(void);
void IN_ReleaseSeat(int seat);    /* cl_input.c: lets go of the seat's buttons */
extern int in_cmdseat;            /* cl_input.c: the seat console commands act for (p1..p4) */
void CL_InitSeats(void);
void CL_SeatsConnected(void);  /* a new connection: seats 1..3 ask again */
void CL_SeatsDisconnected(void);
void CL_SeatsFrame(void);
void CL_SeatInfo(int seat, int slot, int ent);
void CL_SeatSpawned(int seat);
void CL_SeatDevicePressed(int device, int key); /* an unassigned controller's button: press Start to join */

/* cl_main.c */
void CL_Init(void);
void CL_Frame(double dt);
void CL_Disconnect(const char *reason);
void CL_Connect(const char *address);
void CL_SendStringCmd(const char *s);
void CL_SetLocalBlob(const char *key, const void *data, int len);
const infoblob_t *CL_GetLocalBlob(const char *key);
void CL_SendEvent(const msg_t *m); /* clc_event body, written by sendevent */
bool CL_ForwardToServer(const char *line, int argc, const char **argv);
const char *CL_PlayerKey(int slot, const char *key, char *out, size_t size);
const char *CL_ServerKey(const char *key, char *out, size_t size);
float CL_StatFloat(int i);
int CL_StatInt(int i);
const char *CL_StatString(int i);
bool CL_DemoMultiview(void);
/* cl_tp.c: teamplay macros, locations, triggers */
void TP_Init(void);
void TP_Frame(void);
void TP_NewMap(void);
void TP_CheckTriggers(const char *text, int level);
void TP_ParseSay(const char *in, char *out, size_t size);
/* cl_hud.c: the engine's scoreboards and weapon wheel */
void HUD_Init(void);
bool HUD_MouseMove(float dx, float dy);
/* cl_chat.c: ignore lists and message filters */
void CL_InitChat(void);
bool CL_ChatAllowed(const char *text, int level);
int CL_ModelIndex(const char *name, bool add);
void CL_SetModelBounds(int index);
void CL_PrintServerinfo(void);
void CL_ParseServerMessage(msg_t *m, bool reliable);
/* cl_demo.c */
void CL_InitDemos(void);
bool CL_DemoPlaying(void);
bool CL_DemoRecording(void);
void CL_DemoServerMessage(const uint8_t *data, size_t len, bool reliable);
void CL_DemoFrame(void);
void CL_DemoUsercmd(const usercmd_t *u);
void CL_DemoReadPackets(double dt);
void CL_DemoDisconnected(void);
void CL_DemoAbort(void);

/* cl_input.c */
void IN_Init(void);
void IN_KeyEvent(int key, bool down, int unicode);
/* a controller's button; device is its slot + 1 (split screen routing) */
void IN_DeviceKeyEvent(int device, int key, bool down, int unicode);
void IN_MouseMove(float dx, float dy);
void IN_MouseAbs(float x, float y);
void IN_ReleaseAll(void);
/* controller axes in SDL's order (left x/y, right x/y, triggers), -1..1 */
void IN_JoyAxis(int dev, int axis, float value);
void IN_ControllerAdded(int dev);
void IN_ControllerRemoved(int dev);
extern float in_mouse_x, in_mouse_y;
void CL_BuildCommand(double now);
void CL_UpdateCurrentCommand(double now);
bool CL_GetInputState(uint32_t seq, usercmd_t *out);
const char *Key_NameForNum(int key);
int Key_NumForName(const char *name);
const char *Key_GetBinding(int key);
void Key_ForEachName(void (*fn)(const char *name, void *ctx), void *ctx);
void Key_SetBinding(int key, const char *binding);
void Key_WriteBindings(strbuf_t *sb);
bool Key_IsDown(int key);
extern bool key_console_active;

/* cl_csqc.c */
bool CSQC_Load(void);
void CSQC_Shutdown(void);
void CSQC_ParseEntities(msg_t *m);
/* where a networked entity is drawn, cl_interp seconds behind the newest
 * snapshot; false when it should be drawn as the client game left it */
bool CSQC_LerpEntity(int entnum, int ent, float origin[3], float angles[3], float *frame, float *frame1time);
void CSQC_ParseEvent(msg_t *m, size_t len);
void CSQC_WorldLoaded(void);
void CSQC_RendererRestarted(void);
void MQC_RendererRestarted(void);
void CSQC_Frame(int width, int height);
void CSQC_SetActiveSeat(int seat); /* setproperty(VF_ACTIVESEAT) */
bool CSQC_ConsoleCommand(const char *line);
bool CSQC_InputEvent(int type, float a, float b, int device);
void CSQC_InputFrame(void);
bool CSQC_Print(const char *text, int level);
bool CSQC_CenterPrint(const char *text);
void CSQC_ServerInfoChanged(void);
void CSQC_PlayerInfoChanged(int slot);
/* menu progs (cl_menu.c) */
/* cl_capture.c */
void CL_InitCapture(void);
void CL_CaptureFrame(void);
bool CL_Capturing(void);
/* cl_cinematic.c */
void CL_InitCinematics(void);
void CL_DrawCinematic(int w, int h); /* the renderer calls it as the frame ends */
bool CIN_KeyEvent(int key, bool down);
bool CIN_Playing(void);
void CIN_Stop(void);
/* cl_download.c */
/* cl_emenu.c: the engine's own menus */
/* cl_editor.c: edit and aliasedit */
void ED_Init(void);
bool ED_Active(void);
void ED_Draw(int w, int h);
bool ED_KeyEvent(int key, bool down, int unicode);
void EM_Init(void);
bool EM_Active(void);
void EM_Draw(int w, int h);
bool EM_KeyEvent(int key, bool down, int unicode);
void EM_Close(void);
void EM_OpenMain(void);

/* cl_master.c: the server list, as the browsers see it */
extern const qc_builtin_t cl_hostcache_builtins[];
extern const qc_builtin_t cl_altcon_builtins[]; /* cl_altcon.c: named consoles */

/* cl_webcore.c: FTE webcore (HTML menu and HUD) games on Panorama views */
void WC_Init(void);
void WC_Frame(void);
bool WC_PluginLoad(const char *name);
bool WC_ToggleMenu(void); /* false: not using these menus */
bool WC_MenuOpen(void);
bool WC_KeyEvent(int key, bool down, int unicode);
void WC_SetStartsGame(bool starts);
void WC_ClientGameShutdown(void);
void WC_OpenPage(const char *url);

/* cl_setflow.c: the match flow around Bulwark Live sets */
void SF_Init(void);
void SF_Frame(void);
bool SF_Query(const char *req, strbuf_t *out);
void SF_Render(void); /* the hero pictures, after R_BeginFrame */
bool SF_InRunningSet(void);
bool SF_SetGame(void); /* in a Live set game or the training map: no team or hero menus */
extern const qc_builtin_t cl_webcore_builtins[];
void CL_HostcacheRefresh(bool purge);
int CL_HostcacheCount(void);
const char *CL_HostcacheField(int n, const char *key, char *out, size_t size);
void CL_HostcacheSort(const char *key, bool descending);

/* cl_voip.c */
void CL_InitVoip(void);
void CL_VoipFrame(void);
void CL_VoipWrite(msg_t *m); /* adds the voice waiting to go to a clc_move packet */
void CL_ParseVoice(msg_t *m);
void CL_VoipDisconnected(void);
void CL_VoipPrintMutes(void);
const char *CL_VoipPlayerKey(int slot, const char *key, char *out, size_t size);
float CL_VoipMeter(void);
bool CL_PlayerIgnored(int slot); /* cl_chat.c */

void CL_InitDownloads(void);
bool CL_StartMissingDownloads(void); /* true: the signon waits for them */
void CL_ParseDownload(msg_t *m);
void CL_DownloadFrame(void);
void CL_DownloadsReset(void);
void CL_FinishSignon(void); /* cl_main.c: loads the map once the files are here */
/* cl_master.c */
void CL_InitMaster(void);
bool CL_MasterConnectionless(const netadr_t *from, const uint8_t *data, size_t len);
void CL_MasterFrame(void);
void MQC_Init(void);
bool MQC_Load(void);
void MQC_LoadWhenActive(void); /* once the game being started is in */
void MQC_PendingLoad(void); /* does the load MQC_LoadWhenActive asked for */
void MQC_Shutdown(void);
qcvm_t *MQC_VM(void); /* the menu progs, NULL when none is loaded */
bool MQC_Active(void);
void MQC_Toggle(int mode);
void MQC_Frame(int width, int height);
bool MQC_Loading(void);   /* a map load is in progress */
bool MQC_InputEvent(int type, float a, float b, int device);
bool MQC_ConsoleCommand(const char *line);
bool MQC_ForceCommand(const char *line);  /* menu_cmd, registered or not */
bool CSQC_ForceCommand(const char *line); /* cl_cmd, registered or not */
void CSQC_SetInputGlobals(qcvm_t *vm, const usercmd_t *u);
void CSQC_ReadInputGlobals(qcvm_t *vm, usercmd_t *u);
extern msg_t *csqc_readmsg;
extern const qc_builtin_t cl_builtins[];

/* cl_world.c */
typedef struct {
	bool allsolid, startsolid, inopen, inwater;
	float fraction;
	float endpos[3], normal[3];
	float dist;
	int ent;
	int contents;
	int surfaceflags;
} cltrace_t;
void CL_Trace(qcvm_t *vm, const float *start, const float *mins, const float *maxs, const float *end, int type,
              int passent, cltrace_t *tr);
int CL_PointContentsBits(qcvm_t *vm, const float *p);
extern const qc_builtin_t cl_world_builtins[];

#endif
