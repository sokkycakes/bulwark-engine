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

/* recent network origins of a server entity, for render interpolation */
#define CL_INTERP_SAMPLES 16
typedef struct {
	double time;
	float origin[3];
	float angles[3];
} interpsample_t;

typedef struct {
	int csqc_ent; /* the csprogs entity standing for this server entity, 0 if none */
	interpsample_t samples[CL_INTERP_SAMPLES];
	int numsamples;
} clentity_t;

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
	usercmd_t cmds[CL_CMD_BACKUP];
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
void Key_SetBinding(int key, const char *binding);
void Key_WriteBindings(strbuf_t *sb);
bool Key_IsDown(int key);
extern bool key_console_active;

/* cl_csqc.c */
bool CSQC_Load(void);
void CSQC_Shutdown(void);
void CSQC_ParseEntities(msg_t *m);
void CSQC_ParseEvent(msg_t *m, size_t len);
void CSQC_WorldLoaded(void);
void CSQC_RendererRestarted(void);
void MQC_RendererRestarted(void);
void CSQC_Frame(int width, int height);
bool CSQC_ConsoleCommand(const char *line);
bool CSQC_InputEvent(int type, float a, float b, int device);
void CSQC_InputFrame(void);
bool CSQC_Print(const char *text, int level);
bool CSQC_CenterPrint(const char *text);
void CSQC_ServerInfoChanged(void);
void CSQC_PlayerInfoChanged(int slot);
/* menu progs (cl_menu.c) */
void MQC_Init(void);
bool MQC_Load(void);
void MQC_Shutdown(void);
bool MQC_Active(void);
void MQC_Toggle(int mode);
void MQC_Frame(int width, int height);
bool MQC_InputEvent(int type, float a, float b, int device);
bool MQC_ConsoleCommand(const char *line);
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
