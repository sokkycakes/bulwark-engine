/* Bulwark engine - the DS as a link player: it joins a game running on a PC
 * over WiFi, like a GBA on a link cable. ISC licence, see LICENSE.
 *
 * docs/spec/link.md has the protocol. The DS runs none of the game. It sends
 * its buttons and gets back a private screen (drawn on the bottom screen) and
 * where its player is (the top screen draws the map from there, when the DS
 * has a copy of it, and the other players as coloured markers).
 *
 * WiFi comes from the DS's own settings, or from link.txt in a base folder:
 *   ssid=  key=  security=open|wep|wpa|wpa2  server=ip[:port]  name=  autojoin=1
 * and the controls: controls=touch|classic  hand=right|left  sens=100 (percent)
 * diag=0 (hides the frame rate and counters along the top screen's top edge)
 * fps=30 (holds the frame rate to 30, for testing)
 * predict=0 (shows where the server says the player is, as before link v2)
 * cmdvb=2 (a movement command every 2 refreshes instead of every one, for testing)
 * botwalk=1 (walks, strafes, jumps and crouches by itself in a fixed pattern, for testing)
 *
 * The bottom screen is a touch pad, as in Metroid Prime Hunters: drag in the
 * middle to aim, double-tap to jump, and buttons below for weapons, reload,
 * use, jump, duck, flashlight and secondary fire. Above the pad are the
 * player's score and weapon, and the top screen shows health and Barrier
 * (armour) in the HUD font (STATS), and the weapon's clip and reserve. The tab at the top right
 * swaps it for the game's own bottom screen (SCREEN packets) and back.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <dswifi9.h>
#include <nds.h>
#include <netinet/in.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <wfc.h>

#include "nds.h"
#include "hull_nds.h"
#include "weapon_nds.h"
#include "hud_nds.h"
#include "events_nds.h"

#define LINK_PORT 27901
#define MAX_SERVERS 8
#define MAX_MARKERS 64
#define HZ 60

enum { L_DISCOVER = 1, L_JOIN, L_INPUT, L_LEAVE };
enum { L_INFO = 0x81, L_WELCOME, L_REFUSE, L_SCREEN, L_VIEW, L_KICK, L_ENTS, L_MODELS, L_STATS, L_FX,
       L_PSTATE, L_PMOVEVARS, L_BRUSHINFO, L_WEAPONINFO, L_HUDINFO, L_EVENT };
#define LINK_VER 2 /* JOIN and WELCOME's linkver: 2 sends movement commands (docs/spec/link.md) */

#define MAX_LINKMODELS 1024 /* brush models the server can move */
#define MAX_THINGS 48
/* the PC draws other entities cl_interp (0.03 s) behind its newest snapshot,
 * which comes 72 times a second: always between two. ENTS come 20 times a
 * second, so the DS stays one ENTS interval (50 ms) further behind */
#define LINK_INTERP_MS 80
#define MAX_IMOVERS 32 /* moved brush models drawn between updates; more snap */
#define MAX_STUDIO 1032     /* model indexes the server can name; 1000 on are players' own */
enum { SC_CLEAR = 1, SC_FILL, SC_TEXT };
#define MAX_FX 24
enum { FX_OWN = 1, FX_SHOT = 2 }; /* FX: our own shot; someone's shot (it has a muzzle) */
#define FX_LIFE 20                /* refreshes an impact puff lasts */

extern const u8 default_font_bin[]; /* libnds' console font: 256 characters, 8x8, 1 bit */

typedef struct {
	char ssid[33], key[65], security[8], name[16];
	uint32_t server; /* network order, 0 for none */
	uint16_t port;
	bool autojoin;
	bool classic; /* D-pad turns, as Quake and Half-Life on a pad with one stick */
	bool lefty;   /* move with A B X Y, fire with R */
	bool diag;
	bool fps30; /* fps=30: hold to 30 frames a second */
	bool nopredict; /* predict=0 */
	int cmdvb;      /* refreshes a movement command covers: 1, or 2 with cmdvb=2 */
	bool botwalk;   /* botwalk=1 */
	int sens;     /* aim speed, percent: 100 turns 120 degrees across the screen */
	int interp;   /* interp=: ms other entities are drawn behind the server's newest, 0 as sent */
} linkcfg_t;

/* what the game reads in input_buttons (Nuclide's INPUT_BUTTON*) */
enum { B_ATTACK = 0x01, B_JUMP = 0x02, B_RELOAD = 0x08, B_USE = 0x10, B_ATTACK2 = 0x20, B_SPRINT = 0x40, B_CROUCH = 0x80 };
/* Quake's next and previous weapon, Half-Life's flashlight */
enum { IMP_NEXT = 10, IMP_PREV = 12, IMP_LIGHT = 100 };

typedef struct {
	struct sockaddr_in addr;
	char host[33], map[33];
	int players, max, free;
} server_t;

typedef struct {
	int16_t org[3];
	uint8_t kind;
	uint16_t rgb;
} marker_t;

typedef struct {
	int16_t org[3]; /* the bottom middle, map units */
	uint8_t yaw, half, height, kind;
	uint16_t ent, model; /* entity number; studio model index, 0 for none */
	uint8_t seq, lift;   /* the sequence it plays; its origin above the bottom */
	int8_t anim;         /* its slot in anim[], -1 for none */
	uint16_t f1t;        /* the time into seq (frame1time), 1/100 s; F1T_NONE: not sent */
	/* as drawn this frame (lerp_things): where, which way (32768 a turn), the
	 * sequence, how long it has played, and how fast it goes (units a second) */
	int16_t dorg[3];
	int16_t dyaw;
	uint8_t dseq;
	uint32_t dms;
	int16_t speed;
} thing_t;
#define F1T_NONE 0xffff

/* the entities we were last sent, by entity number: the last few updates of
 * each (when, where, and what it played), so the DS draws them between two
 * of them as the PC does (lerp_things); a small table, the oldest slot
 * reused. It is on the heap from the first ENTS after a map loads to the
 * next map (ents_alloc), so it doesn't take from what loading a map has */
#define MAX_ANIM 64
#define ISAMPLES 3 /* enough for LINK_INTERP_MS behind 20 updates a second */
typedef struct {
	uint32_t time;  /* the server's, ms */
	int16_t org[3]; /* units */
	uint8_t yaw, seq;
	uint16_t f1t;
} isample_t;
typedef struct {
	uint16_t ent;
	uint8_t seq;
	int start, seen; /* frame the sequence started; frame last sent */
	int16_t prev[3];
	int16_t speed; /* map units a second, eased */
	int16_t lightat[3]; /* where the light was looked up */
	/* light -1: not yet */
	int8_t light;
	uint8_t ns;      /* samples held, oldest first */
	bool hasvel;     /* a player whose velocity the last ENTS gave */
	int16_t vel[3];  /* units a second */
	uint16_t model;  /* the model of its updates */
	uint16_t entsseq; /* the ENTS it was last in */
	uint8_t legs;    /* a player's: 0 idle, 1 walk, 2 run (UpdatePlayerAnimation keeps it from 150 to 200) */
	isample_t s[ISAMPLES];
} ianim_t;
#define ENTS_GONE 3 /* not in this many ENTS in a row: back, it starts again with no updates */
static ianim_t *anim; /* MAX_ANIM, NULL until ents_alloc */

static int anim_slot(int ent, int frame)
{
	int old = 0;
	for (int i = 0; i < MAX_ANIM; i++) {
		if (anim[i].ent == ent)
			return i;
		if (anim[i].seen < anim[old].seen)
			old = i;
	}
	memset(&anim[old], 0, sizeof(anim[old]));
	anim[old].ent = (uint16_t)ent;
	anim[old].seen = frame;
	anim[old].light = -1;
	return old;
}

/* the server's studio model names, by model index, and what loading gave */
static char *studioname[MAX_STUDIO];
static int16_t studiomdl[MAX_STUDIO]; /* -2 not tried, -1 can't, else Ents_Model's */
static int linkframe;
static uint32_t vb_ms(int vb);

/* the server's clock as the DS reckons it (ENTS carries the server's time,
 * docs/spec/link.md), run as the PC client's cl.time is (update_clock,
 * src/client/cl_main.c): at real-time speed, nudged toward the newest
 * update's time plus how long ago it came */
static struct {
	bool have;
	uint32_t srv;  /* the newest ENTS's time, ms */
	uint32_t recv; /* our ms clock when it came */
	uint32_t now;  /* the reckoned server time, ms */
	uint32_t last; /* our ms clock at the last step */
	bool servertime; /* the server sends its time (else ENTS are stamped when they come) */
} eclock;

/* each brush model as the server last had it, and as drawn (easing toward it) */
typedef struct {
	bool moved;
	bool wasmoved;             /* in the ENTS before (handle_ents) */
	int8_t slot;               /* its updates in imover[], -1 for none */
	int32_t target[3], cur[3]; /* 1/8 units */
	int16_t tang[3], cang[3];  /* 65536 to a turn */
} linkmodel_t;

/* the moved brush models' last few updates, drawn between two as the things
 * are; with anim, on the heap */
typedef struct {
	uint16_t model;
	uint8_t ns;
	bool resting;              /* left the list: its last update is home, drawn till then */
	uint16_t entsseq;          /* the ENTS it was last in */
	struct {
		uint32_t time;
		int16_t org[3]; /* 1/4 units, as sent */
		int16_t ang[3];
	} s[ISAMPLES];
} imover_t;
static imover_t *imover; /* MAX_IMOVERS */
static bool ents_prev;          /* an ENTS came before on this clock (handle_ents) */
static uint32_t ents_prevstamp; /* and its time */
static uint32_t ents_ms;        /* our ms clock when the newest ENTS came */


static linkcfg_t cfg;
static int sock = -1;
static uint16_t *sub; /* the bottom screen, 256x256 RGB15 (192 rows shown) */
static server_t servers[MAX_SERVERS];
static int numservers;
static const uint16_t *palette;

static struct {
	int fps, minfps, peakpolys; /* the last second, sent with the input */
	int loadms;                 /* the last map load */
	struct sockaddr_in to;
	uint32_t session;
	uint16_t seq, screen_ack, spawncount;
	int lastrecv; /* frame */
	char map[65];
	char loaderr[40];           /* why the last map didn't load */
	int32_t target[3];
	int32_t vel[3];   /* how fast target moves, 1/8 units a refresh, from the last two VIEWs */
	int viewframe;    /* when the last VIEW came */
	bool haveview;
	float pvel[3];        /* the player's velocity, units a second (VIEW) */
	bool onground, crouch; /* VIEW flags 2 and 4 */
	marker_t markers[MAX_MARKERS];
	int nummarkers;
	int health;
	int armor, frags;           /* from STATS */
	char weapon[25];            /* the weapon in hand, by class name without "weapon_" */
	int statsver;               /* bumped when any of the above changes */
	int vmodel, vseq, vplays;   /* the view model (a model index), its sequence and plays */
	int vmstart;                /* the frame its sequence started */
	uint16_t entsseq;
	bool haveents;
	thing_t things[MAX_THINGS];
	int numthings;
	struct {
		int32_t to[3], from[3]; /* the impact; the muzzle its tracer starts at */
		int born;               /* frame */
		uint8_t flags;          /* FX_ */
	} fx[MAX_FX];
	int nextfx;
} link;

static linkmodel_t models[MAX_LINKMODELS];

/* movement prediction (link v2, docs/spec/link.md "Prediction"): the DS makes
 * movement commands, numbered, and runs each through pmove_nds.c (a port of
 * the Nuclide movement code the PC runs) against the map's clip hulls
 * (hull_nds.c); the server runs each once and says, in PSTATE, which it ran
 * last and where that left the player. Commands the server hasn't run yet
 * are run again from its state whenever ours turns out to differ, as
 * ncPlayer::PreFrame does on the PC (src/shared/game/Player.qc). */
#define PR_CMDS 64   /* commands kept, by seq & 63; more unacked than 63 and the view snaps */
#define PR_RING 32   /* states predicted after them, by seq & 31 */
#define PR_REPLAY 16 /* the most a frame runs again; a longer backlog goes on next frame */
#define PR_REPLAY_MS 8 /* and for no longer than this, once PR_REPLAY_MIN have run (melonDS: a command
                        * and its weapon part take 0.5 to 1.2 ms, so 16 fit on a light map, and a
                        * heavy one keeps its frame rate) */
#define PR_REPLAY_MIN 4 /* the most commands a frame makes: a replay always gains on them */
#define PR_TBOX 64 /* turned brush entities with their own box, in one BRUSHINFO version */
#define PR_MOVERS 8
static struct {
	int srvver;            /* WELCOME's linkver: 0 an old server */
	uint32_t seq;          /* the newest command */
	uint32_t floor;        /* commands up to here were made for another map: never sent */
	uint32_t ack;          /* the newest the server ran (or dropped) */
	int cmdvb;             /* the refresh the newest command ends at */
	uint32_t cmdms;        /* and its millisecond clock */
	uint8_t impseq;        /* the pad's one-shot count the commands have taken */
	pmcmd_t cmd[PR_CMDS];
	pmstate_t pred[PR_RING];
	uint32_t predseq[PR_RING]; /* the command each holds the state after, 0 none */
	pmstate_t cur;         /* after the newest command: what the view shows */
	/* a replay longer than PR_REPLAY, going on over frames: the view stays on cur
	 * (the last prediction, still run on) until it has caught up */
	bool rp;
	bool rpsnap;           /* it ends in a jump of over 64 units */
	uint32_t rpnext;       /* the next command it runs */
	pmstate_t rpstate;
	bool on;               /* predicting; else the view eases toward VIEW */
	pmvars_t vars;
	uint16_t varsver;      /* PMOVEVARS applied, 0 none */
	/* BRUSHINFO: the version being put together from its pages, and the last complete one
	 * (hull_nds.c holds its own copy, with the movers placed); both only grow, and swap */
	hullbrush_t *bnew, *bcur;
	uint8_t *bhave;
	int bnewtotal, bnewgot, bcurn, bnewcap, bcurcap, bhavecap;
	uint16_t bnewver, bcurver;
	/* the turned entries' absmin/absmax (BRUSHINFO's turned block), by entry, for bnew and bcur */
	struct tbox { uint16_t i; float mn[3], mx[3]; } tnew[PR_TBOX], tcur[PR_TBOX];
	int ntnew, ntcur;
	/* its touch block (the same for bnew; ntouchwant the number the version has) */
	pmtouch_t touchnew[HULL_MAX_TOUCH];
	int ntouchnew, ntouchwant;
	int placed[PR_MOVERS]; /* bcur entries hull_nds.c has where the last PSTATE's movers were */
	int nplaced;
	/* the newest PSTATE, kept while packets drain, handled after */
	uint8_t pstate[1400]; /* 180, 8 movers of 26+36, 16 boxes of 27, the weapon block */
	int pstatelen;
	bool havepseq;
	uint16_t pseq;
	uint8_t fixseq;
	struct {
		uint16_t e, model; /* model: its brush model (0 for one BRUSHINFO doesn't list) */
		float org[3], vel[3];
	} movers[PR_MOVERS];
	int nmovers;
	float stairz;          /* ncView::StairSmooth's last eye height */
	bool stair;            /* stairz holds one */
	int botstart;          /* botwalk: the refresh the pattern started */
} pr;

/* weapon prediction (link v2, docs/spec/link.md "Weapon prediction"): each command
 * runs, after its movement, through weapon_nds.c (a port of what the PC client
 * runs: Physics_InputPostMove and ncPlayer::ProcessInput's ncWeapon calls) with
 * the weapon's values from WEAPONINFO, from PSTATE's weapon block as the
 * movement runs from PSTATE's state. Its view model sequence and time, muzzle
 * flash, punch and ammo are what the view and the HUD show. */
typedef struct {
	float health, downforce, eye[3];
	uint32_t seq, vflags, gflags;
	int8_t waterlevel;
	bool landed;
} wenvrec_t; /* what a command's movement left for the weapon code, kept for a replay */
#define LINK_BOXES 16 /* PSTATE's boxes: the server sends at most MAX_BOXES (sv_link.c) */
static struct {
	bool havesrv;              /* the last PSTATE had a weapon block (and wh is there) */
	wstate_t srv;              /* its state, after the acked command */
	wenvrec_t base;            /* and the player's as it left it */
	wstate_t cur;              /* after the newest command: what the view shows */
	wstate_t rpstate;          /* the replay's, with pr.rp or arp */
	bool arp;                  /* without movement prediction: a weapon replay going on over frames */
	uint32_t arpnext;
	bool showing;              /* the view model is ours (cur), not STATS' */
	float showntime;
	int flashframe;            /* the refresh our muzzle flash started; its size */
	float flashscale;
	uint32_t replays;          /* weapon-only replays */
} wp;
/* the weapon's bigger tables, on the heap from the first ENTS after a map has loaded to
 * the next map (with anim), so they don't take from what loading a map has */
static struct {
	winfo_t info;              /* WEAPONINFO; info.version 0 none */
	wstate_t ring[PR_RING];
	uint32_t ringseq[PR_RING];
	wenvrec_t env[PR_CMDS];
	int nboxes;                /* the boxes near the player, for a melee weapon's sweep */
	float bmins[LINK_BOXES][3], bmaxs[LINK_BOXES][3];
	uint8_t bsolid[LINK_BOXES];
} *wh;

/* the weapon state predicted after command seq, for comparing with the server's */
static void wring_put(uint32_t seq, const wstate_t *s)
{
	if (wh)
		wh->ring[seq & (PR_RING - 1)] = *s, wh->ringseq[seq & (PR_RING - 1)] = seq;
}

/* the entity and weapon tables (anim, imover, wh), after a map has loaded: false if there's no room (the
 * entities are then drawn as sent) */
static bool ents_alloc(void)
{
	if (!wh)
		wh = calloc(1, sizeof(*wh));
	if (!anim && (anim = calloc(MAX_ANIM, sizeof(*anim))) != NULL && !(imover = calloc(MAX_IMOVERS, sizeof(*imover))))
		free(anim), anim = NULL;
	return anim != NULL;
}

static void ents_free(void)
{
	free(anim), free(imover), free(wh);
	anim = NULL, imover = NULL, wh = NULL;
	ents_prev = false;
	wp.havesrv = wp.showing = wp.arp = false;
}

/* what prediction did, for the rig's measurement build (it reads and resets it each second) */
typedef struct {
	uint32_t compared, within, snaps;       /* PSTATEs compared; within 0.5 units; corrections over 64 units */
	uint32_t err_sum_cu, err_max_cu;        /* origin error at the acked command, 1/100 units */
	uint32_t verr_max;                      /* velocity error, units a second */
	uint32_t replays, replayed, replay_max; /* PSTATE replays; commands replayed */
	uint32_t rp_skip, rp_exact;             /* replays skipped (matched); of those, bit-identical */
	uint32_t rp_cut;                        /* frames a replay went on past, PR_REPLAY each */
	uint32_t pm_runs, cmds, frames;         /* PM_Run calls; commands made; frames */
	uint64_t pm_ticks;
	uint32_t pm_ticks_max;                  /* per PM_Run (LINK_PM_TICKS) */
	uint64_t fr_ticks;
	uint32_t fr_ticks_max;                  /* prediction per frame */
	uint16_t gaps, clipped;                 /* the server's counters, last PSTATE */
	uint8_t hullrule, on;
	uint16_t hullkb;
} linkpred_stats_t;
linkpred_stats_t Link_PredStats;
/* the top screen's HUD a frame (Hud_Frame), the same way */
typedef struct {
	uint32_t frames, ticks_max;
	uint64_t ticks;
} linkhud_stats_t;
linkhud_stats_t Link_HudStats;
#ifndef LINK_PM_TICKS
#define LINK_PM_TICKS() 0 /* the rig's measurement build makes this tickGetCount() */
#endif
static void pred_newmap(void);

/* --- link.txt ------------------------------------------------------------------------ */

static void read_config(void)
{
	char path[64], line[160];
	memset(&cfg, 0, sizeof(cfg));
	cfg.port = LINK_PORT;
	cfg.sens = 100;
	cfg.interp = LINK_INTERP_MS;
	cfg.diag = true;
	cfg.cmdvb = 1;
	snprintf(cfg.name, sizeof(cfg.name), "DS");
	for (int b = 0; b < NDS_NUMBASEDIRS; b++) {
		FILE *f;
		snprintf(path, sizeof(path), "%slink.txt", nds_basedirs[b]);
		if (!(f = fopen(path, "r")))
			continue;
		while (fgets(line, sizeof(line), f)) {
			char *v = strchr(line, '=');
			line[strcspn(line, "\r\n")] = 0;
			if (!v)
				continue;
			*v++ = 0;
			if (!strcmp(line, "ssid"))
				snprintf(cfg.ssid, sizeof(cfg.ssid), "%s", v);
			else if (!strcmp(line, "key"))
				snprintf(cfg.key, sizeof(cfg.key), "%s", v);
			else if (!strcmp(line, "security"))
				snprintf(cfg.security, sizeof(cfg.security), "%s", v);
			else if (!strcmp(line, "name"))
				snprintf(cfg.name, sizeof(cfg.name), "%s", v);
			else if (!strcmp(line, "autojoin"))
				cfg.autojoin = atoi(v) != 0;
			else if (!strcmp(line, "controls"))
				cfg.classic = !strcmp(v, "classic");
			else if (!strcmp(line, "hand"))
				cfg.lefty = !strcmp(v, "left");
			else if (!strcmp(line, "sens"))
				cfg.sens = BW_NDS_CLAMP(atoi(v), 10, 1000);
			else if (!strcmp(line, "diag"))
				cfg.diag = atoi(v) != 0;
			else if (!strcmp(line, "fps"))
				cfg.fps30 = atoi(v) == 30;
			else if (!strcmp(line, "predict"))
				cfg.nopredict = atoi(v) == 0;
			else if (!strcmp(line, "cmdvb"))
				cfg.cmdvb = BW_NDS_CLAMP(atoi(v), 1, 2);
			else if (!strcmp(line, "botwalk"))
				cfg.botwalk = atoi(v) != 0;
			else if (!strcmp(line, "interp"))
				cfg.interp = BW_NDS_CLAMP(atoi(v), 0, 500);
			else if (!strcmp(line, "lerpframes"))
				Ents_LerpFrames = atoi(v) != 0;
			else if (!strcmp(line, "server")) {
				char *colon = strchr(v, ':');
				if (colon)
					*colon = 0, cfg.port = (uint16_t)atoi(colon + 1);
				cfg.server = inet_addr(v);
				if (cfg.server == INADDR_NONE)
					cfg.server = 0;
			}
		}
		fclose(f);
		return;
	}
}

bool Link_AutoJoin(void)
{
	read_config();
	return cfg.autojoin;
}

/* --- WiFi ---------------------------------------------------------------------------- */

static bool wait_keys_or(int frames, uint32_t keys)
{
	for (int i = 0; i < frames; i++) {
		swiWaitForVBlank();
		scanKeys();
		if (keysDown() & keys)
			return true;
	}
	return false;
}

static int hexval(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* the most secure authentication the network offers that link.txt allows */
static int pick_auth(unsigned mask)
{
	unsigned allow = 0xff;
	if (!strcmp(cfg.security, "open"))
		allow = 1u << WlanBssAuthType_Open;
	else if (!strcmp(cfg.security, "wep"))
		allow = 0x0e;
	else if (!strcmp(cfg.security, "wpa"))
		allow = (1u << WlanBssAuthType_WPA_PSK_TKIP) | (1u << WlanBssAuthType_WPA_PSK_AES);
	else if (!strcmp(cfg.security, "wpa2"))
		allow = (1u << WlanBssAuthType_WPA2_PSK_TKIP) | (1u << WlanBssAuthType_WPA2_PSK_AES);
	mask &= allow;
	for (int t = 7; t >= 0; t--)
		if (mask & (1u << t))
			return t;
	return -1;
}

static bool connect_ssid(void)
{
	static WlanBssDesc bss;
	WlanAuthData auth;
	WlanBssDesc *list;
	unsigned count = 0;
	int t, found = -1;

	iprintf("Looking for %s...\n", cfg.ssid);
	if (!wfcBeginScan(NULL))
		return false;
	for (int i = 0; i < 10 * HZ && wfcGetStatus() == WfcStatus_Scanning; i++)
		swiWaitForVBlank();
	list = wfcGetScanBssList(&count);
	for (unsigned i = 0; list && i < count && found < 0; i++)
		if (list[i].ssid_len == strlen(cfg.ssid) && !memcmp(list[i].ssid, cfg.ssid, list[i].ssid_len))
			found = (int)i;
	if (found < 0) {
		iprintf("%s not found (%u networks seen)\n", cfg.ssid, count);
		return false;
	}
	bss = list[found];
	if ((t = pick_auth(bss.auth_mask)) < 0) {
		iprintf("%s doesn't offer %s\n", cfg.ssid, cfg.security);
		return false;
	}
	memset(&auth, 0, sizeof(auth));
	if (t >= WlanBssAuthType_WPA_PSK_TKIP) {
		if (!wfcDeriveWpaKey(&auth, cfg.ssid, strlen(cfg.ssid), cfg.key, strlen(cfg.key)))
			return false;
	} else if (t != WlanBssAuthType_Open) {
		/* WEP: the key in hex; 10, 26 or 32 digits */
		int n = (int)strlen(cfg.key) / 2;
		for (int i = 0; i < n && i < WLAN_WEP_128_LEN; i++)
			auth.wep_key[i] = (uint8_t)(hexval(cfg.key[i * 2]) << 4 | hexval(cfg.key[i * 2 + 1]));
		t = n <= WLAN_WEP_40_LEN ? WlanBssAuthType_WEP_40 : n <= WLAN_WEP_104_LEN ? WlanBssAuthType_WEP_104 : WlanBssAuthType_WEP_128;
	}
	bss.auth_type = (WlanBssAuthType)t;
	return wfcBeginConnect(&bss, &auth);
}

static bool wifi_connect(void)
{
	static const char *const status[] = {"off", "scanning", "connecting", "getting an address", "connected"};
	int last = -1;
	if (wfcGetStatus() == WfcStatus_Connected)
		return true;
	iprintf("Starting WiFi...\n");
	if (!Wifi_InitDefault(INIT_ONLY)) {
		iprintf("WiFi didn't start\n");
		return false;
	}
	if (cfg.ssid[0]) {
		if (!connect_ssid())
			return false;
	} else {
		iprintf("Using the DS's WiFi settings\n");
		Wifi_AutoConnect();
	}
	for (int i = 0; i < 30 * HZ; i++) {
		int s = wfcGetStatus();
		if (s != last && s >= 0 && s <= WfcStatus_Connected)
			iprintf("WiFi: %s\n", status[s]), last = s;
		if (s == WfcStatus_Connected) {
			struct in_addr ip = {Wifi_GetIP()};
			iprintf("Address %s\n", inet_ntoa(ip));
			return true;
		}
		swiWaitForVBlank();
		scanKeys();
		if (keysDown() & KEY_START)
			return false;
	}
	iprintf("WiFi timed out\n");
	return false;
}

/* --- packets ------------------------------------------------------------------------- */

typedef struct {
	uint8_t d[192]; /* INPUT with 8 commands is 161 */
	int n;
} pkt_t;

static void p_begin(pkt_t *p, int type) { memcpy(p->d, "BWL1", 4), p->d[4] = (uint8_t)type, p->n = 5; }
static void p_u8(pkt_t *p, int v) { p->d[p->n++] = (uint8_t)v; }
static void p_u16(pkt_t *p, int v) { p_u8(p, v), p_u8(p, v >> 8); }
static void p_u32(pkt_t *p, uint32_t v) { p_u16(p, (int)(v & 0xffff)), p_u16(p, (int)(v >> 16)); }
static unsigned rd16(const uint8_t *d) { return d[0] | d[1] << 8; }
static uint32_t rd32(const uint8_t *d) { return rd16(d) | (uint32_t)rd16(d + 2) << 16; }

static void send_to(const struct sockaddr_in *to, const pkt_t *p)
{
	sendto(sock, p->d, p->n, 0, (const struct sockaddr *)to, sizeof(*to));
}

static bool open_socket(void)
{
	int one = 1;
	if (sock >= 0)
		return true;
	if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return false;
	ioctl(sock, FIONBIO, &one);
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	return true;
}

static void close_socket(void)
{
	if (sock >= 0)
		closesocket(sock);
	sock = -1;
}

static struct sockaddr_in addr_of(uint32_t ip, uint16_t port)
{
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = ip;
	return a;
}

/* one packet or 0; type in *type, payload after the type byte */
static int receive(uint8_t *buf, int size, struct sockaddr_in *from, int *type)
{
	socklen_t fl = sizeof(*from);
	int n = recvfrom(sock, buf, size, 0, (struct sockaddr *)from, &fl);
	if (n < 5 || memcmp(buf, "BWL1", 4))
		return 0;
	*type = buf[4];
	return n - 5;
}

static void copy_str(char *dst, int size, const uint8_t *src, int len)
{
	int n = 0;
	while (n < len && n < size - 1 && src[n])
		dst[n] = (char)src[n], n++;
	dst[n] = 0;
}

/* --- the top screen's HUD: hud_nds.c ------------------------------------------------- */

static hudin_t hudin;          /* what the link knows itself, for Hud_Frame */
static hudent_t hudents[MAX_THINGS];
static char huddiag[2][96];

/* --- the bottom screen --------------------------------------------------------------- */

static void screen_init(void)
{
	int bg;
	videoSetModeSub(MODE_5_2D);
	/* bank H holds the console at the same address: take it off, or both
	 * banks answer there and the top 64 rows show the two mixed */
	vramSetBankH(VRAM_H_LCD);
	vramSetBankC(VRAM_C_SUB_BG);
	bg = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	sub = bgGetGfxPtr(bg);
	dmaFillHalfWords(0x8000, sub, 256 * 192 * 2);
	Hud_Init();
}

static void screen_done(void)
{
	Hud_Done();
	glDisable(GL_BLEND); /* Events_Reset turned it on for the link; the rest draws as before */
	sub = NULL;
	Nds_InitConsole();
	Mp_Init();
}

static void fill(int x, int y, int w, int h, uint16_t c)
{
	if (x >= 256 || y >= 192)
		return;
	w = BW_NDS_MIN(w, 256 - x), h = BW_NDS_MIN(h, 192 - y);
	for (int j = 0; j < h; j++) {
		uint16_t *row = sub + (y + j) * 256 + x;
		for (int i = 0; i < w; i++)
			row[i] = c;
	}
}

static void text(int x, int y, uint16_t c, const uint8_t *s, int len)
{
	for (int k = 0; k < len && x + 8 <= 256; k++, x += 8) {
		const u8 *glyph = default_font_bin + s[k] * 8;
		for (int j = 0; j < 8 && y + j < 192; j++) {
			uint16_t *row = sub + (y + j) * 256 + x;
			for (int i = 0; i < 8; i++)
				if (glyph[j] & (1 << i)) /* bit 0 is the left pixel */
					row[i] = c;
		}
	}
}

static void draw_screen(const uint8_t *d, int len)
{
	int i = 0;
	while (i < len) {
		switch (d[i]) {
		case SC_CLEAR:
			if (i + 3 > len)
				return;
			fill(0, 0, 256, 192, (uint16_t)(rd16(d + i + 1) | 0x8000));
			i += 3;
			break;
		case SC_FILL:
			if (i + 7 > len)
				return;
			fill(d[i + 1], d[i + 2], d[i + 3] ? d[i + 3] : 256, d[i + 4] ? d[i + 4] : 256,
			     (uint16_t)(rd16(d + i + 5) | 0x8000));
			i += 7;
			break;
		case SC_TEXT:
			if (i + 6 > len || i + 6 + d[i + 5] > len)
				return;
			text(d[i + 1], d[i + 2], (uint16_t)(rd16(d + i + 3) | 0x8000), d + i + 6, d[i + 5]);
			i += 6 + d[i + 5];
			break;
		default:
			return;
		}
	}
}

/* --- the touch pad on the bottom screen ------------------------------------------------ */

#define PAD_TOP 24     /* rows 0-23: score, weapon and the tab */
#define PAD_BOTTOM 144 /* rows 24-143: drag to aim; 144-191: the buttons */
#define TAB_H 16
#define TAB_X 200      /* the tab that swaps in the game's own screen */

typedef struct {
	uint8_t x, y, w, h;
	const char *label;
	uint16_t button; /* held while pressed */
	uint8_t impulse; /* sent once per press */
	bool toggle;     /* stays on until pressed again */
} padbtn_t;

static const padbtn_t padbtns[] = {
	{0, 144, 64, 24, "< WPN", 0, IMP_PREV, false},    {64, 144, 64, 24, "WPN >", 0, IMP_NEXT, false},
	{128, 144, 64, 24, "RELOAD", B_RELOAD, 0, false}, {192, 144, 64, 24, "USE", B_USE, 0, false},
	{0, 168, 64, 24, "JUMP", B_JUMP, 0, false},       {64, 168, 64, 24, "DUCK", B_CROUCH, 0, true},
	{128, 168, 64, 24, "LIGHT", 0, IMP_LIGHT, false}, {192, 168, 64, 24, "ALT", B_ATTACK2, 0, false},
};
#define NUM_PADBTNS ((int)(sizeof(padbtns) / sizeof(padbtns[0])))

#define C_BAR (RGB15(3, 4, 7) | BIT(15))
#define C_PAD (RGB15(2, 3, 5) | BIT(15))
#define C_GRID (RGB15(4, 6, 10) | BIT(15))
#define C_BTN (RGB15(6, 8, 13) | BIT(15))
#define C_BTNLIT (RGB15(14, 20, 31) | BIT(15))
#define C_EDGE (RGB15(1, 1, 2) | BIT(15))
#define C_TEXT (RGB15(26, 28, 31) | BIT(15))
#define C_DIM (RGB15(10, 12, 17) | BIT(15))
#define C_DOT (RGB15(31, 31, 20) | BIT(15))

static struct {
	bool gamepage;           /* showing the game's screen instead of the pad */
	bool touching, aiming;   /* the stylus is down; it went down in the aim area */
	int lastx, lasty;        /* where it was last frame */
	int dotx, doty;          /* the marker drawn under it, -1 for none */
	int pressed;             /* the button the stylus holds, -1 for none */
	bool ducked;             /* DUCK is on */
	int upframe, upx, upy;   /* when and where the last aim touch ended (double-tap) */
	int jumpframes;          /* JUMP held until this frame */
	uint8_t impulse, impseq; /* the last one-shot press, and its count */
	uint8_t screen[1300];    /* the game's last screen, to show when asked */
	int screenlen;
	int shownstats;          /* link.statsver when the panel was drawn */
	char diagline[33];
} pad;

static uint16_t pad_bg(int x, int y)
{
	return (x & 31) == 16 || ((y - PAD_TOP) & 31) == 16 ? C_GRID : C_PAD;
}

static void text_mid(int x, int y, int w, uint16_t c, const char *s)
{
	int n = (int)strlen(s);
	text(x + (w - n * 8) / 2, y, c, (const uint8_t *)s, n);
}

static void pad_button(int i, bool lit)
{
	const padbtn_t *b = &padbtns[i];
	fill(b->x, b->y, b->w, b->h, C_EDGE);
	fill(b->x + 1, b->y + 1, b->w - 2, b->h - 2, lit ? C_BTNLIT : C_BTN);
	text_mid(b->x, b->y + (b->h - 8) / 2, b->w, lit ? C_EDGE : C_TEXT, b->label);
}

/* the bar: score and the weapon in hand (health and Barrier are on the top screen), and the tab */
static void pad_bar(void)
{
	char n[12], w[sizeof(link.weapon)];
	fill(0, 0, 256, PAD_TOP, C_BAR);
	fill(0, PAD_TOP - 1, 256, 1, C_EDGE);
	text(4, 8, C_DIM, (const uint8_t *)"SCORE", 5);
	snprintf(n, sizeof(n), "%d", link.frags);
	text(48, 8, C_TEXT, (const uint8_t *)n, (int)strlen(n));
	for (int i = 0; i < (int)sizeof(w); i++)
		w[i] = link.weapon[i] == '_' ? ' ' : (char)toupper((unsigned char)link.weapon[i]);
	text(88, 8, C_TEXT, (const uint8_t *)w, BW_NDS_MIN((int)strlen(w), (TAB_X - 92) / 8));
	fill(TAB_X, 1, 256 - TAB_X - 1, TAB_H - 2, C_BTN);
	text_mid(TAB_X, 4, 256 - TAB_X, C_TEXT, pad.gamepage ? "PAD" : "GAME");
}

static void pad_draw(void)
{
	if (pad.gamepage) {
		if (pad.screenlen)
			draw_screen(pad.screen, pad.screenlen);
		else
			fill(0, 0, 256, 192, RGB15(0, 0, 0) | BIT(15));
		fill(TAB_X, 1, 256 - TAB_X - 1, TAB_H - 2, C_BTN);
		text_mid(TAB_X, 4, 256 - TAB_X, C_TEXT, "PAD");
		return;
	}
	for (int y = PAD_TOP; y < PAD_BOTTOM; y++) {
		uint16_t *row = sub + y * 256;
		for (int x = 0; x < 256; x++)
			row[x] = pad_bg(x, y);
	}
	for (int i = 0; i < NUM_PADBTNS; i++)
		pad_button(i, (padbtns[i].toggle && pad.ducked) || i == pad.pressed);
	pad.dotx = pad.doty = -1;
	pad_bar();
}

/* the marker under the stylus while aiming */
static void pad_dot(int x, int y)
{
	if (pad.dotx >= 0)
		for (int j = 0; j < 4; j++)
			for (int i = 0; i < 4; i++)
				sub[(pad.doty + j) * 256 + pad.dotx + i] = pad_bg(pad.dotx + i, pad.doty + j);
	pad.dotx = pad.doty = -1;
	if (x < 0)
		return;
	x = BW_NDS_CLAMP(x - 2, 0, 252), y = BW_NDS_CLAMP(y - 2, PAD_TOP, PAD_BOTTOM - 4);
	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 4; i++)
			sub[(y + j) * 256 + x + i] = C_DOT;
	pad.dotx = x, pad.doty = y;
}

static void pad_impulse(int n)
{
	pad.impulse = (uint8_t)n;
	pad.impseq++;
}

static void pad_reset(void)
{
	memset(&pad, 0, sizeof(pad));
	pad.pressed = -1;
	pad.dotx = pad.doty = -1;
	pad.upframe = -100;
	pad.shownstats = -1;
}

/* the stylus, once a frame. Returns the buttons the pad holds; aiming turns
 * the camera directly. *totouch is set when the touch belongs to the game's
 * screen (sent on to the server as it is) */
static int pad_frame(int frame, uint32_t held, ndscam_t *cam, bool *togame, touchPosition *tp)
{
	bool down = (held & KEY_TOUCH) != 0, began = down && !pad.touching;
	int buttons = 0;
	*togame = false;
	if (down)
		touchRead(tp);
	if (began && tp->py < TAB_H && tp->px >= TAB_X) {
		pad.gamepage = !pad.gamepage;
		pad.aiming = false, pad.pressed = -2; /* the rest of this touch is the tab's */
		pad_draw();
	} else if (pad.gamepage) {
		*togame = down && !(pad.touching && !pad.aiming && pad.pressed == -2);
	} else if (began) {
		if (tp->py >= PAD_TOP && tp->py < PAD_BOTTOM) {
			pad.aiming = true;
			pad.lastx = tp->px, pad.lasty = tp->py;
			if (frame - pad.upframe < 18 && abs(tp->px - pad.upx) < 24 && abs(tp->py - pad.upy) < 24)
				pad.jumpframes = frame + 8; /* double-tap: held until then */
			pad_dot(tp->px, tp->py);
		} else if (tp->py >= PAD_BOTTOM) {
			for (int i = 0; i < NUM_PADBTNS; i++) {
				const padbtn_t *b = &padbtns[i];
				if (tp->px >= b->x && tp->px < b->x + b->w && tp->py >= b->y && tp->py < b->y + b->h) {
					pad.pressed = i;
					if (b->impulse)
						pad_impulse(b->impulse);
					if (b->toggle)
						pad.ducked = !pad.ducked;
					pad_button(i, true);
				}
			}
		}
	} else if (down && pad.aiming) {
		int dx = tp->px - pad.lastx, dy = tp->py - pad.lasty;
		/* the touch screen jumps now and then: ignore a big leap */
		if (abs(dx) < 48 && abs(dy) < 48) {
			cam->yaw -= dx * cfg.sens * 7 / 16;
			cam->pitch -= dy * cfg.sens * 7 / 16;
			cam->pitch = BW_NDS_CLAMP(cam->pitch, -8000, 8000);
			cam->yaw = (int16_t)cam->yaw;
			pad.lastx = tp->px, pad.lasty = tp->py;
			pad_dot(tp->px, tp->py);
		}
	}
	if (!down && pad.touching) {
		if (pad.aiming) {
			pad.upframe = frame, pad.upx = pad.lastx, pad.upy = pad.lasty;
			pad_dot(-1, -1);
		}
		if (pad.pressed >= 0 && !pad.gamepage)
			pad_button(pad.pressed, padbtns[pad.pressed].toggle && pad.ducked);
		pad.aiming = false, pad.pressed = -1;
	}
	pad.touching = down;
	if (pad.pressed >= 0)
		buttons |= padbtns[pad.pressed].button & ~B_CROUCH;
	if (pad.ducked)
		buttons |= B_CROUCH;
	if (frame < pad.jumpframes)
		buttons |= B_JUMP;
	if (link.statsver != pad.shownstats) {
		if (!pad.gamepage)
			pad_bar();
		pad.shownstats = link.statsver;
	}
	return buttons;
}

/* --- finding and joining a server ---------------------------------------------------- */

static void add_server(const struct sockaddr_in *from, const uint8_t *d, int len)
{
	server_t *s = NULL;
	if (len < 67)
		return;
	for (int i = 0; i < numservers; i++)
		if (servers[i].addr.sin_addr.s_addr == from->sin_addr.s_addr && servers[i].addr.sin_port == from->sin_port)
			s = &servers[i];
	if (!s) {
		if (numservers >= MAX_SERVERS)
			return;
		s = &servers[numservers++];
	}
	s->addr = *from;
	copy_str(s->host, sizeof(s->host), d, 32);
	copy_str(s->map, sizeof(s->map), d + 32, 32);
	s->players = d[64], s->max = d[65], s->free = d[66];
}

static void list_servers(int sel)
{
	consoleClear();
	iprintf("Bulwark DS - join a game\n\n");
	if (!numservers)
		iprintf("Looking for games...\n");
	for (int i = 0; i < numservers; i++) {
		const server_t *s = &servers[i];
		iprintf("%c %.20s\n    %.14s %d/%d %s\n", i == sel ? '>' : ' ', s->host, s->map, s->players, s->max,
		        s->free ? "" : "full");
	}
	iprintf("\nA: join  START: back\n");
}

/* broadcasts DISCOVER (and asks the server in link.txt); returns the server picked or -1 */
static int find_server(void)
{
	uint8_t buf[256];
	struct sockaddr_in from, bcast = addr_of(INADDR_BROADCAST, cfg.port), direct = addr_of(cfg.server, cfg.port);
	int sel = 0, shown = -1;
	numservers = 0;
	for (int frame = 0;; frame++) {
		int len, type;
		if (frame % HZ == 0) {
			pkt_t p;
			p_begin(&p, L_DISCOVER);
			send_to(&bcast, &p);
			if (cfg.server)
				send_to(&direct, &p);
		}
		while ((len = receive(buf, sizeof(buf), &from, &type)) > 0)
			if (type == L_INFO)
				add_server(&from, buf + 5, len);
		if (cfg.autojoin && numservers)
			return 0;
		if (shown != numservers * 16 + sel) {
			list_servers(sel);
			shown = numservers * 16 + sel;
		}
		swiWaitForVBlank();
		scanKeys();
		uint32_t down = keysDown();
		if (down & KEY_START)
			return -1;
		if ((down & KEY_A) && numservers)
			return sel;
		if ((down & KEY_DOWN) && sel + 1 < numservers)
			sel++;
		if ((down & KEY_UP) && sel > 0)
			sel--;
		if (down & KEY_TOUCH) {
			touchPosition tp;
			int row;
			touchRead(&tp);
			row = (tp.py / 8 - 2) / 2;
			if (tp.py >= 16 && row < numservers)
				return row;
		}
	}
}

static void handle_welcome(const uint8_t *d, int len, ndscam_t *cam);

static bool join(const server_t *s, ndscam_t *cam)
{
	uint8_t buf[256];
	struct sockaddr_in from;
	pkt_t p;
	memset(&link, 0, sizeof(link));
	pred_newmap(); /* frees what the last game left */
	memset(&pr, 0, sizeof(pr));
	link.to = s->addr;
	/* different on every DS and every join: the time, how long since power-on and the name */
	link.session = (uint32_t)rand() ^ (uint32_t)time(NULL) << 8 ^ Wifi_GetIP() ^ (uint32_t)Nds_Vblanks() * 2654435761u;
	for (const char *n = cfg.name; *n; n++)
		link.session = link.session * 31 + (uint8_t)*n;
	if (!link.session)
		link.session = 1;
	iprintf("Joining %s...\n", s->host);
	p_begin(&p, L_JOIN);
	p_u32(&p, link.session);
	p_u16(&p, 1); /* a DS */
	for (int i = 0; i < 16; i++)
		p_u8(&p, i < (int)strlen(cfg.name) ? cfg.name[i] : 0);
	p_u8(&p, LINK_VER);
	for (int frame = 0; frame < 6 * HZ; frame++) {
		int len, type;
		if (frame % HZ == 0)
			send_to(&link.to, &p);
		while ((len = receive(buf, sizeof(buf), &from, &type)) > 0) {
			const uint8_t *d = buf + 5;
			if (type == L_WELCOME && len >= 7 && rd32(d) == link.session) {
				handle_welcome(d, len, cam);
				return true;
			}
			if (type == L_REFUSE) {
				char reason[65];
				copy_str(reason, sizeof(reason), d, len);
				iprintf("Refused: %s\n", reason);
				return false;
			}
		}
		swiWaitForVBlank();
	}
	iprintf("No answer\n");
	return false;
}

/* --- linked -------------------------------------------------------------------------- */

static void reset_studio(void)
{
	Ents_Free();
	for (int i = 0; i < MAX_STUDIO; i++) {
		free(studioname[i]);
		studioname[i] = NULL;
		studiomdl[i] = -2;
	}
	ents_free();
}

/* names for model indexes (docs/spec/link.md, MODELS) */
/* STATS: the player's numbers for the panel */
static void handle_stats(const uint8_t *d, int len)
{
	char w[sizeof(link.weapon)];
	int armor, frags;
	if (len < 11)
		return;
	armor = (int16_t)rd16(d + 6), frags = (int16_t)rd16(d + 8);
	copy_str(w, sizeof(w), d + 11, BW_NDS_MIN(d[10], len - 11));
	if (armor != link.armor || frags != link.frags || strcmp(w, link.weapon)) {
		link.armor = armor, link.frags = frags;
		memcpy(link.weapon, w, sizeof(w));
		link.statsver++;
	}
	/* then the view model, from servers that send it */
	if (len >= 11 + d[10] + 4) {
		const uint8_t *v = d + 11 + d[10];
		link.vmodel = rd16(v);
		if (v[2] != link.vseq || v[3] != link.vplays)
			link.vmstart = linkframe;
		link.vseq = v[2], link.vplays = v[3];
	}
}

static void handle_models(const uint8_t *d, int len)
{
	int pos = 7, count;
	if (len < 7 || (uint16_t)rd16(d + 4) != link.spawncount)
		return;
	count = d[6];
	for (int i = 0; i < count && pos + 3 <= len; i++) {
		int idx = rd16(d + pos), n = d[pos + 2];
		pos += 3;
		if (pos + n > len)
			break;
		if (idx > 0 && idx < MAX_STUDIO && studioname[idx] && (strlen(studioname[idx]) != (size_t)n || strncasecmp(studioname[idx], (const char *)d + pos, (size_t)n))) {
			free(studioname[idx]); /* a player picked another model */
			studioname[idx] = NULL;
			studiomdl[idx] = -2;
		}
		if (idx > 0 && idx < MAX_STUDIO && !studioname[idx] && (studioname[idx] = malloc((size_t)n + 1))) {
			for (int k = 0; k < n; k++) /* the card's copies are in lower case */
				studioname[idx][k] = (char)(d[pos + k] >= 'A' && d[pos + k] <= 'Z' ? d[pos + k] + 32 : d[pos + k]);
			studioname[idx][n] = 0;
		}
		pos += n;
	}
}

static void handle_welcome(const uint8_t *d, int len, ndscam_t *cam)
{
	char map[65], path[300], err[96];
	uint16_t spawncount = (uint16_t)rd16(d + 5);
	copy_str(map, sizeof(map), d + 7, BW_NDS_MIN(len - 7, 64));
	link.spawncount = spawncount;
	Events_MapChange();
	pr.srvver = len >= 72 ? d[71] : 0;
	pred_newmap();
	eclock.have = false; /* the next ENTS starts the clock and the entities' updates again */
	if (World_Loaded() && !strcmp(map, link.map))
		return;
	snprintf(link.map, sizeof(link.map), "%s", map);
	reset_studio(); /* the models' textures go before VRAM is reset, not after */
	World_Free();
	Nds_ResetTextures();
	link.haveview = false;
	link.haveents = false;
	link.numthings = 0;
	memset(models, 0, sizeof(models));
	snprintf(path, sizeof(path), "maps/%s.bsp", map);
	{
		int start = Nds_Vblanks();
		link.loaderr[0] = 0;
		if (World_Load(path, palette, err, sizeof(err)))
			World_StartPosition(cam);
		else /* shown on the bottom line */
			snprintf(link.loaderr, sizeof(link.loaderr), "%s", err);
		link.loadms = (Nds_Vblanks() - start) * 1000 / 60;
	}
}

static void handle_view(const uint8_t *d, int len, ndscam_t *cam)
{
	int count;
	if (len < 4 + 2 + 12 + 6 + 2)
		return;
	{
		int since = linkframe - link.viewframe;
		for (int k = 0; k < 3; k++) {
			int32_t t = (int32_t)rd32(d + 6 + k * 4);
			/* a jump of more than 2 units a refresh is a teleport, not motion */
			link.vel[k] = link.haveview && since > 0 && since < 30 && abs(t - link.target[k]) < 16 * since
			                  ? (t - link.target[k]) / since
			                  : 0;
			link.target[k] = t;
		}
		link.viewframe = linkframe;
	}
	if ((int16_t)rd16(d + 22) != link.health)
		link.health = (int16_t)rd16(d + 22), link.statsver++;
	if (d[24] & 1) { /* the game turned us */
		cam->pitch = (int16_t)rd16(d + 18) / 2;
		cam->yaw = (int16_t)rd16(d + 20) / 2;
	}
	if (!link.haveview)
		memcpy(cam->org, link.target, sizeof(cam->org));
	link.haveview = true;
	count = d[25];
	link.nummarkers = 0;
	for (int i = 0; i < count && i < MAX_MARKERS && 26 + (i + 1) * 9 <= len; i++) {
		const uint8_t *m = d + 26 + i * 9;
		marker_t *mk = &link.markers[link.nummarkers++];
		for (int k = 0; k < 3; k++)
			mk->org[k] = (int16_t)rd16(m + k * 2);
		mk->kind = m[6];
		mk->rgb = (uint16_t)rd16(m + 7);
	}
	link.onground = d[24] & 2, link.crouch = d[24] & 4;
	if (26 + count * 9 + 6 <= len)
		for (int k = 0; k < 3; k++)
			link.pvel[k] = (int16_t)rd16(d + 26 + count * 9 + k * 2);
	else /* an older server: from the last two views */
		for (int k = 0; k < 3; k++)
			link.pvel[k] = link.vel[k] * 7.5f;
}

/* bullet impacts (docs/spec/link.md, FX): a puff where each hit, and for a
 * shot a muzzle flash and a tracer from the muzzle: someone else's a little
 * ahead of their eye toward the hit, ours at the view model's barrel (draw_fx) */
static void handle_fx(const uint8_t *d, int len)
{
	for (int i = 0; i < d[4] && 5 + (i + 1) * 25 <= len; i++) {
		const uint8_t *f = d + 5 + i * 25;
		int32_t to[3], from[3];
		for (int k = 0; k < 3; k++)
			to[k] = (int32_t)rd32(f + 1 + k * 4), from[k] = (int32_t)rd32(f + 13 + k * 4);
		/* a hit point far beyond the map's reach on the DS is skipped */
		if (abs(to[0]) > 32000 || abs(to[1]) > 32000 || abs(to[2]) > 32000)
			continue;
		int fl = f[0];
		if (fl & FX_SHOT) {
			float dx = (float)(to[0] - from[0]), dy = (float)(to[1] - from[1]), dz = (float)(to[2] - from[2]);
			float dist = sqrtf(dx * dx + dy * dy + dz * dz);
			if (dist < 72 * 8) /* a melee hit: no flash, no tracer */
				fl &= ~(FX_SHOT | FX_OWN);
			else {
				float ahead = 20 * 8 / dist; /* someone else's: their barrel, toward the hit */
				from[0] += (int32_t)(dx * ahead), from[1] += (int32_t)(dy * ahead), from[2] += (int32_t)(dz * ahead);
			}
		}
		/* a shotgun's pellets share a flash: only the first of a volley keeps one */
		if (fl & FX_SHOT)
			for (int j = 0; j < MAX_FX; j++)
				if (link.fx[j].born == linkframe && link.fx[j].flags & FX_SHOT && (link.fx[j].flags & FX_OWN) == (fl & FX_OWN) &&
				    abs(link.fx[j].from[0] - from[0]) < 256 && abs(link.fx[j].from[1] - from[1]) < 256)
					fl |= 4; /* 4: no flash of its own */
		int n = link.nextfx++ % MAX_FX;
		memcpy(link.fx[n].to, to, sizeof(to));
		memcpy(link.fx[n].from, from, sizeof(from));
		link.fx[n].born = linkframe;
		link.fx[n].flags = (uint8_t)fl;
	}
}

/* a quad facing the camera, centre c and half size h, in 1/8 units; a
 * diamond (turned 45 degrees) when h is negative */
static void fx_sprite(const ndscam_t *cam, const int32_t *c, int h)
{
	bool diamond = h < 0;
	h = abs(h);
	int rx = sinLerp(cam->yaw), ry = -cosLerp(cam->yaw); /* the camera's right, 4.12 */
	int dx = rx * h >> 12, dy = ry * h >> 12;
	int x = BW_NDS_CLAMP(c[0], -32000, 32000), y = BW_NDS_CLAMP(c[1], -32000, 32000), z = BW_NDS_CLAMP(c[2], -32000, 32000);
	glBegin(GL_QUADS);
	if (diamond) {
		glVertex3v16(x, y, z - h * 3 / 2);
		glVertex3v16(x + dx * 3 / 2, y + dy * 3 / 2, z);
		glVertex3v16(x, y, z + h * 3 / 2);
		glVertex3v16(x - dx * 3 / 2, y - dy * 3 / 2, z);
	} else {
		glVertex3v16(x - dx, y - dy, z - h);
		glVertex3v16(x + dx, y + dy, z - h);
		glVertex3v16(x + dx, y + dy, z + h);
		glVertex3v16(x - dx, y - dy, z + h);
	}
	glEnd();
}

static void draw_fx(const ndscam_t *cam)
{
	/* our own gun's barrel, in front of the view model's tip: 10 units ahead,
	 * a third of the way to the right edge and down (fov 70, the tip near
	 * pixel 160,140), so it moves with the view and nothing covers it */
	int32_t muzzle[3];
	{
		float sy = sinLerp(cam->yaw) / 4096.0f, cy = cosLerp(cam->yaw) / 4096.0f;
		float sp = sinLerp(cam->pitch) / 4096.0f, cp = cosLerp(cam->pitch) / 4096.0f;
		float f[3] = {cy * cp, sy * cp, sp}, r[3] = {sy, -cy, 0}, u[3] = {-cy * sp, -sy * sp, cp};
		for (int k = 0; k < 3; k++)
			muzzle[k] = cam->org[k] + (int32_t)((f[k] * 10 + r[k] * 3.2f - u[k] * 1.7f) * 8);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	/* our predicted flash: env_muzzleflash fades by 16 a second (62.5 ms), and its
	 * size is the event's option (0.1 for a 1x; the sizes below are that) */
	if (wp.showing && linkframe - wp.flashframe >= 0 && linkframe - wp.flashframe < 4) {
		int age = linkframe - wp.flashframe, a = BW_NDS_MAX(26 - age * 26 * 16 / HZ, 1);
		float sc = wp.flashscale;
		glPolyFmt(POLY_ALPHA(a) | POLY_CULL_NONE | POLY_ID(5));
		glColor(RGB15(31, 22, 6));
		fx_sprite(cam, muzzle, -(int)(5 * sc + 0.5f));
		glPolyFmt(POLY_ALPHA(a) | POLY_CULL_NONE | POLY_ID(6));
		glColor(RGB15(31, 27, 12));
		fx_sprite(cam, muzzle, (int)(4 * sc + 0.5f));
		glPolyFmt(POLY_ALPHA(a) | POLY_CULL_NONE | POLY_ID(7));
		glColor(RGB15(31, 31, 24));
		fx_sprite(cam, muzzle, (int)(3 * sc + 0.5f));
	}
	for (int i = 0; i < MAX_FX; i++) {
		int age = linkframe - link.fx[i].born, id = 8 + i;
		const int32_t *to = link.fx[i].to, *from = link.fx[i].flags & FX_OWN ? muzzle : link.fx[i].from;
		if (!link.fx[i].flags && !to[0] && !to[1] && !to[2])
			continue;
		if (age < 0 || age >= FX_LIFE)
			continue;
		/* the muzzle flash, two refreshes; a cross of two quads */
		if ((link.fx[i].flags & (FX_SHOT | 4)) == FX_SHOT && age < 3 && !(wp.showing && link.fx[i].flags & FX_OWN)) {
			glPolyFmt(POLY_ALPHA(26) | POLY_CULL_NONE | POLY_ID(id));
			glColor(RGB15(31, 22, 6));
			fx_sprite(cam, from, -(link.fx[i].flags & FX_OWN ? 5 : 3 * 8));
			glPolyFmt(POLY_ALPHA(26) | POLY_CULL_NONE | POLY_ID(id + 16));
			glColor(RGB15(31, 27, 12));
			fx_sprite(cam, from, link.fx[i].flags & FX_OWN ? 4 : 2 * 8);
			glPolyFmt(POLY_ALPHA(26) | POLY_CULL_NONE | POLY_ID(id + 24));
			glColor(RGB15(31, 31, 24));
			fx_sprite(cam, from, link.fx[i].flags & FX_OWN ? 3 : 10);
		}
		/* the tracer: a streak a third of the way long, crossing in 4 refreshes */
		if (link.fx[i].flags & FX_SHOT && age < 4) {
			float t1 = (age + 1) / 4.0f, t0 = BW_NDS_MAX(t1 - 0.35f, 0.0f);
			float a[3], b[3], d[3], v[3], side[3], len;
			for (int k = 0; k < 3; k++) {
				a[k] = from[k] + (to[k] - from[k]) * t0, b[k] = from[k] + (to[k] - from[k]) * t1;
				d[k] = b[k] - a[k], v[k] = a[k] - cam->org[k];
			}
			/* across the streak and the line of sight, so it faces us */
			side[0] = d[1] * v[2] - d[2] * v[1], side[1] = d[2] * v[0] - d[0] * v[2], side[2] = d[0] * v[1] - d[1] * v[0];
			len = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
			if (len > 0) {
				for (int k = 0; k < 3; k++)
					side[k] = side[k] / len * 4; /* half a unit */
				glPolyFmt(POLY_ALPHA(20) | POLY_CULL_NONE | POLY_ID(id + 32));
				glColor(RGB15(31, 29, 18));
				glBegin(GL_QUADS);
				glVertex3v16((int)(a[0] - side[0]), (int)(a[1] - side[1]), (int)(a[2] - side[2]));
				glVertex3v16((int)(a[0] + side[0]), (int)(a[1] + side[1]), (int)(a[2] + side[2]));
				glVertex3v16((int)(b[0] + side[0]), (int)(b[1] + side[1]), (int)(b[2] + side[2]));
				glVertex3v16((int)(b[0] - side[0]), (int)(b[1] - side[1]), (int)(b[2] - side[2]));
				glEnd();
			}
		}
		/* the impact: a spark, then dust that grows and fades */
		if (age < 3) {
			glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(id));
			glColor(RGB15(31, 28, 14));
			fx_sprite(cam, to, 3 * 8);
		} else {
			int fade = 24 - age * 24 / FX_LIFE;
			glPolyFmt(POLY_ALPHA(BW_NDS_MAX(fade, 1)) | POLY_CULL_NONE | POLY_ID(id));
			glColor(RGB15(20, 19, 17));
			fx_sprite(cam, to, 3 * 8 + age * 4);
		}
	}
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);
	glColor(RGB15(31, 31, 31));
}

/* the updates of an entity: ISAMPLES records of size bytes, oldest first,
 * each starting with its time (ms) */
#define TIME_AT(i) (*(const uint32_t *)((const uint8_t *)base + (i) * size))

/* which record a new update at time t goes in, the oldest dropped when they
 * are all used: -1 when it is older than the newest (it came late) */
static int sample_at(void *base, size_t size, uint8_t *ns, uint32_t t)
{
	int n = *ns;
	if (n && (int32_t)(t - TIME_AT(n - 1)) <= 0)
		return t == TIME_AT(n - 1) ? n - 1 : -1; /* the same again: replaced */
	if (n == ISAMPLES)
		memmove(base, (uint8_t *)base + size, size * (ISAMPLES - 1)), n--;
	*ns = (uint8_t)(n + 1);
	return n;
}

/* model m's slot in imover[], or one for it: the one longest out of ENTS, never one this
 * ENTS (seq) uses; -1 when all are (more than MAX_IMOVERS moving: the rest ease) */
static int mover_slot(int m, uint16_t seq)
{
	int old = -1;
	for (int i = 0; i < MAX_IMOVERS; i++) {
		if (imover[i].model == m && imover[i].ns)
			return i;
		if (!imover[i].ns) { /* a free one first */
			if (old < 0 || imover[old].ns)
				old = i;
		} else if (imover[i].entsseq != seq && (old < 0 || (imover[old].ns && (uint16_t)(seq - imover[i].entsseq) >
		                                                                     (uint16_t)(seq - imover[old].entsseq))))
			old = i;
	}
	if (old < 0)
		return -1;
	if (imover[old].ns && imover[old].model < MAX_LINKMODELS && models[imover[old].model].slot == old)
		models[imover[old].model].slot = -1;
	memset(&imover[old], 0, sizeof(imover[old]));
	imover[old].model = (uint16_t)m;
	return old;
}

/* which brush models are there and where the moved ones are, then the things
 * near us (docs/spec/link.md, ENTS) */
static void handle_ents(const uint8_t *d, int len)
{
	uint16_t seq = (uint16_t)rd16(d + 4);
	int n, pos, count, movers;
	uint32_t now = vb_ms(Nds_Vblanks()), stamp = now;
	imover_t *im;
	if (len < 9 || (link.haveents && (int16_t)(seq - link.entsseq) <= 0))
		return;
	n = BW_NDS_MIN((int)rd16(d + 6), MAX_LINKMODELS);
	pos = 8 + ((int)rd16(d + 6) + 7) / 8;
	if (pos >= len)
		return;
	link.entsseq = seq;
	link.haveents = true;
	ents_ms = now;
	for (int m = 1; m < n; m++)
		models[m].wasmoved = models[m].moved, models[m].moved = false;
	if (!ents_alloc())
		eclock.have = false;
	movers = d[pos++];
	/* the server's time comes after the things: find it first */
	{
		int tp = pos + movers * 14, things, size;
		if (tp + 2 <= len && (size = d[tp + 1]) >= 10 && (things = d[tp], tp + 2 + things * size + 4 <= len)) {
			stamp = rd32(d + tp + 2 + things * size);
			eclock.servertime = true;
		} else
			eclock.servertime = false; /* an older server: when it came */
	}
	{
		int32_t ahead = (int32_t)(stamp - eclock.srv);
		if (!anim)
			;
		else if (!eclock.have || ahead > 1000 || ahead < -1000) {
			/* the first, or another clock (a new map, a restarted server):
			 * the updates kept go by the old one */
			for (int i = 0; i < MAX_ANIM; i++)
				anim[i].ns = 0;
			for (int i = 0; i < MAX_IMOVERS; i++)
				imover[i].ns = 0, imover[i].resting = false;
			ents_prev = false;
			eclock.now = stamp, eclock.last = now;
			eclock.srv = stamp, eclock.recv = now, eclock.have = true;
		} else if (ahead > 0)
			eclock.srv = stamp, eclock.recv = now;
	}
	for (int i = 0; i < movers && pos + 14 <= len; i++, pos += 14) {
		int m = rd16(d + pos), k;
		linkmodel_t *lm;
		if (m <= 0 || m >= MAX_LINKMODELS)
			continue;
		lm = &models[m];
		for (int c = 0; c < 3; c++) {
			lm->target[c] = (int16_t)rd16(d + pos + 2 + c * 2) * 2;
			lm->tang[c] = (int16_t)rd16(d + pos + 8 + c * 2);
		}
		if (!lm->moved && !(lm->cur[0] | lm->cur[1] | lm->cur[2]))
			memcpy(lm->cur, lm->target, sizeof(lm->cur)), memcpy(lm->cang, lm->tang, sizeof(lm->cang));
		lm->moved = true;
		if (!imover)
			continue;
		if (lm->slot < 0 || imover[lm->slot].model != m || !imover[lm->slot].ns)
			lm->slot = (int8_t)mover_slot(m, seq);
		if (lm->slot < 0)
			continue; /* eased (move_models) */
		im = &imover[lm->slot];
		im->entsseq = seq;
		im->resting = false;
		/* leaving home, where the last ENTS had it: from there, as the PC's entity was */
		if (!im->ns && !lm->wasmoved && ents_prev && (int32_t)(stamp - ents_prevstamp) > 0) {
			memset(&im->s[0], 0, sizeof(im->s[0]));
			im->s[0].time = ents_prevstamp, im->ns = 1;
		}
		k = sample_at(im->s, sizeof(im->s[0]), &im->ns, stamp);
		if (k >= 0) {
			im->s[k].time = stamp;
			for (int c = 0; c < 3; c++)
				im->s[k].org[c] = (int16_t)rd16(d + pos + 2 + c * 2);
			memcpy(im->s[k].ang, lm->tang, sizeof(lm->tang));
		}
	}
	/* back home (the server leaves a brush model at its origin out): drawn on toward it
	 * until the drawn moment passes this update, as the PC draws its entity between
	 * snapshots, then at home (move_models) */
	for (int m = 1; m < n && imover; m++) {
		linkmodel_t *lm = &models[m];
		if (lm->moved || !lm->wasmoved || lm->slot < 0 || imover[lm->slot].model != m || !imover[lm->slot].ns)
			continue;
		im = &imover[lm->slot];
		memset(lm->target, 0, sizeof(lm->target)), memset(lm->tang, 0, sizeof(lm->tang));
		lm->moved = true;
		if (!im->resting) {
			int k = sample_at(im->s, sizeof(im->s[0]), &im->ns, stamp);
			im->resting = true;
			if (k >= 0)
				memset(&im->s[k], 0, sizeof(im->s[k])), im->s[k].time = stamp;
		}
	}
	ents_prev = anim != NULL, ents_prevstamp = stamp;
	for (int m = 1; m < n; m++) {
		bool shown = (d[8 + (m >> 3)] >> (m & 7)) & 1;
		if (!models[m].moved)
			memset(models[m].cur, 0, sizeof(models[m].cur)), memset(models[m].cang, 0, sizeof(models[m].cang));
		World_ModelState(m, shown, NULL, NULL);
	}
	link.numthings = 0;
	if (pos >= len)
		return;
	count = d[pos++];
	{
		int size = pos < len ? d[pos++] : 10;
		if (size < 10)
			return;
		for (int i = 0; i < count && link.numthings < MAX_THINGS && pos + size <= len; i++, pos += size) {
			thing_t *t = &link.things[link.numthings++];
			for (int c = 0; c < 3; c++)
				t->org[c] = (int16_t)rd16(d + pos + c * 2);
			t->yaw = d[pos + 6], t->half = d[pos + 7], t->height = d[pos + 8], t->kind = d[pos + 9];
			t->ent = t->model = 0, t->seq = t->lift = 0;
			t->f1t = F1T_NONE;
			if (size >= 16) {
				t->ent = (uint16_t)rd16(d + pos + 10), t->model = (uint16_t)rd16(d + pos + 12);
				t->seq = d[pos + 14], t->lift = d[pos + 15];
			}
			if (size >= 18)
				t->f1t = (uint16_t)rd16(d + pos + 16);
			t->anim = -1;
			if (t->ent && anim) {
				/* a new sequence starts its clip again; speed from how far it went since the last packet (20 a second) */
				typeof(anim[0]) *a = &anim[t->anim = (int8_t)anim_slot(t->ent, linkframe)];
				int k;
				a->seen = linkframe;
				/* back after a while, or another model on its edict: the PC makes a new
				 * entity of it, with no updates to draw between (cl_csqc.c:428) */
				if (a->ns && (a->model != t->model || (uint16_t)(seq - a->entsseq) > ENTS_GONE))
					a->ns = 0, a->legs = 0;
				a->model = t->model, a->entsseq = seq;
				int dx = t->org[0] - a->prev[0], dy = t->org[1] - a->prev[1];
				int sp = (int)sqrtf((float)(dx * dx + dy * dy)) * 20;
				if (a->seq != t->seq || !a->start)
					a->seq = t->seq, a->start = linkframe;
				a->speed = (int16_t)(sp > 2000 ? a->speed : (a->speed + sp) / 2);
				memcpy(a->prev, t->org, sizeof(a->prev));
				a->hasvel = false;
				k = sample_at(a->s, sizeof(a->s[0]), &a->ns, stamp);
				if (k >= 0) {
					isample_t *sm = &a->s[k];
					sm->time = stamp;
					memcpy(sm->org, t->org, sizeof(sm->org));
					sm->yaw = t->yaw, sm->seq = t->seq, sm->f1t = t->f1t;
				}
			}
		}
		/* after the things: the time (read above), then the players' velocities */
		if (pos + 5 <= len && eclock.servertime) {
			int nv = d[pos + 4];
			pos += 5;
			for (int i = 0; i < nv && pos + 7 <= len; i++, pos += 7) {
				int k = d[pos];
				if (k < link.numthings && link.things[k].anim >= 0) {
					typeof(anim[0]) *a = &anim[link.things[k].anim];
					a->hasvel = true;
					for (int c = 0; c < 3; c++)
						a->vel[c] = (int16_t)rd16(d + pos + 1 + c * 2);
				}
			}
		}
	}
}

/* the reckoned server time, stepped once a frame */
static void step_clock(void)
{
	uint32_t now = vb_ms(Nds_Vblanks());
	int32_t dt = (int32_t)(now - eclock.last), diff;
	if (!eclock.have)
		return;
	eclock.last = now;
	eclock.now += (uint32_t)BW_NDS_CLAMP(dt, 0, 250);
	diff = (int32_t)(eclock.srv + (now - eclock.recv) - eclock.now);
	if (diff > 250 || diff < -250)
		eclock.now += (uint32_t)diff;
	else
		eclock.now += (uint32_t)(diff * BW_NDS_CLAMP(dt, 0, 250) * 4 / 1000);
}

/* between which two updates the drawn moment t falls, and how far: as
 * CSQC_LerpEntity (src/client/cl_csqc.c:467-520). Returns false when t is
 * past the newest (drawn as sent). A long gap between two updates means the
 * entity stood still, so the change is spread over the last tenth of a
 * second only */
static bool lerp_pick(const void *base, size_t size, int ns, uint32_t t, int *ia, int *f256)
{
	int i;
	uint32_t ta, tb, start;
	if (ns < 2 || (int32_t)(t - TIME_AT(ns - 1)) >= 0)
		return false;
	for (i = ns - 2; i > 0 && (int32_t)(TIME_AT(i) - t) > 0; i--)
		;
	ta = TIME_AT(i), tb = TIME_AT(i + 1);
	*ia = i;
	if ((int32_t)(t - ta) <= 0) {
		*f256 = 0;
		return true;
	}
	start = (int32_t)(tb - ta) > 100 ? tb - 100 : ta;
	*f256 = (int32_t)(t - start) <= 0 ? 0 : (int)((t - start) * 256 / (tb - start));
	return true;
}
#undef TIME_AT

static int lerp_angle16(int a, int b, int f256)
{
	int d = (int16_t)(b - a);
	return a + d * f256 / 256;
}

/* where and how each thing is drawn this frame: cfg.interp ms behind the
 * reckoned server time, between the two updates around then, as the PC
 * draws every networked entity; other players moved on from the newest
 * update at their velocity instead, as Nuclide's ncPlayer::predraw does
 * with cg_extrapolateClients 1 (src/shared/game/Player.qc:740-756) */
static void lerp_things(void)
{
	uint32_t t = eclock.now - (uint32_t)cfg.interp;
	/* as sent, the time into a sequence runs on from when the update came (else it
	 * would step at the ENTS rate) */
	uint32_t sent = (uint32_t)BW_NDS_CLAMP((int32_t)(vb_ms(linkframe) - ents_ms), 0, 100);
	for (int i = 0; i < link.numthings; i++) {
		thing_t *th = &link.things[i];
		typeof(anim[0]) *a = th->anim >= 0 ? &anim[th->anim] : NULL;
		const isample_t *sa, *sb, *last;
		int ia, f;
		memcpy(th->dorg, th->org, sizeof(th->dorg));
		th->dyaw = (int16_t)(th->yaw * 128);
		th->dseq = th->seq;
		th->speed = a ? a->speed : 0;
		th->dms = th->f1t != F1T_NONE ? th->f1t * 10u + sent : a ? vb_ms(linkframe - a->start) : 0;
		if (!a || !cfg.interp || !a->ns)
			continue;
		last = &a->s[a->ns - 1];
		if (a->hasvel) {
			/* a player: the newest update moved on by its velocity to the
			 * reckoned server time (for at most a tenth of a second: then it
			 * waits for the next). That time can be a little before the
			 * newest update, which came at the start of a frame rather than
			 * when it arrived: moving it back keeps the steps even. Its
			 * facing and animation are the newest too, run on as far: the
			 * PC's ncPlayer::predraw runs Physics_Run, whose
			 * UpdatePlayerAnimation turns it to the newest v_angle and runs
			 * its animation on (Player.qc:509-519, 740-756) */
			int32_t ahead = BW_NDS_CLAMP((int32_t)(eclock.now - last->time), -100, 100);
			float v2 = (float)a->vel[0] * a->vel[0] + (float)a->vel[1] * a->vel[1] + (float)a->vel[2] * a->vel[2];
			for (int c = 0; c < 3; c++)
				th->dorg[c] = (int16_t)(last->org[c] + a->vel[c] * ahead / 1000);
			th->dyaw = (int16_t)(last->yaw * 128);
			if (last->f1t != F1T_NONE)
				th->dseq = last->seq, th->dms = (uint32_t)BW_NDS_MAX((int32_t)(last->f1t * 10u) + ahead, 0);
			/* the legs as UpdatePlayerAnimation picks them (Player.qc:555-579): idle
			 * under 1 unit a second, walking under 150, running over 200, and between
			 * 150 and 200 what it was; Ents_AddLinked walks over 40 and runs over 200 */
			a->legs = v2 < 1 ? 0 : v2 < 150 * 150 ? 1 : v2 > 200 * 200 ? 2 : a->legs;
			th->speed = a->legs == 0 ? 0 : a->legs == 1 ? 100 : 300;
			continue;
		}
		if (!lerp_pick(a->s, sizeof(a->s[0]), a->ns, t, &ia, &f))
			continue;
		sa = &a->s[ia], sb = &a->s[ia + 1];
		{
			int64_t d2 = 0;
			for (int c = 0; c < 3; c++)
				d2 += (int64_t)(sb->org[c] - sa->org[c]) * (sb->org[c] - sa->org[c]);
			for (int c = 0; c < 3; c++) /* a jump of over 256 units is a teleport */
				th->dorg[c] = d2 > 256 * 256 ? (f < 256 ? sa->org[c] : sb->org[c])
				                             : (int16_t)(sa->org[c] + (sb->org[c] - sa->org[c]) * f / 256);
		}
		th->dyaw = (int16_t)lerp_angle16(sa->yaw * 256, sb->yaw * 256, f) / 2;
		/* the animation: the time into a sequence runs on between updates of
		 * the same sequence; across a change the older one holds until then */
		if (sa->f1t != F1T_NONE && sb->f1t != F1T_NONE) {
			if (sa->seq == sb->seq && sb->f1t >= sa->f1t)
				th->dseq = sa->seq, th->dms = (sa->f1t * 256u + (sb->f1t - sa->f1t) * (uint32_t)f) * 10 / 256;
			else
				th->dseq = f < 256 ? sa->seq : sb->seq, th->dms = (f < 256 ? sa->f1t : sb->f1t) * 10u;
		}
	}
}

/* the moved brush models: between two updates as the things are, or, from a
 * server that doesn't send its time (or one with no room for its updates),
 * eased toward the newest. The ones PSTATE lists are where the predicted
 * player meets them */
static void move_models(int dt)
{
	int n = BW_NDS_MIN(World_NumModels(), MAX_LINKMODELS);
	uint32_t t = eclock.now - (uint32_t)cfg.interp;
	for (int m = 1; m < n; m++) {
		linkmodel_t *lm = &models[m];
		int ang[3], ia, f;
		bool placed = false;
		if (!lm->moved)
			continue;
		if (imover && lm->slot >= 0 && imover[lm->slot].model == m && imover[lm->slot].ns && cfg.interp &&
		    eclock.servertime) {
			imover_t *im = &imover[lm->slot];
			if (lerp_pick(im->s, sizeof(im->s[0]), im->ns, t, &ia, &f)) {
				const int16_t *a = im->s[ia].org, *b = im->s[ia + 1].org;
				int64_t d2 = 0;
				for (int c = 0; c < 3; c++)
					d2 += (int64_t)(b[c] - a[c]) * (b[c] - a[c]);
				for (int c = 0; c < 3; c++) { /* 1/4 units to 1/8; a jump of over 256 units is a teleport */
					lm->cur[c] = 2 * (d2 > (int64_t)1024 * 1024 ? (f < 256 ? a[c] : b[c]) : a[c] + (b[c] - a[c]) * f / 256);
					lm->cang[c] = (int16_t)lerp_angle16(im->s[ia].ang[c], im->s[ia + 1].ang[c], f);
				}
				placed = true;
			} else if (im->resting) { /* drawn home: the slot is free again */
				im->ns = 0, im->resting = false, im->model = 0;
				lm->slot = -1, lm->moved = false;
				memset(lm->cur, 0, sizeof(lm->cur)), memset(lm->cang, 0, sizeof(lm->cang));
				World_ModelState(m, true, NULL, NULL);
				continue;
			} else if (im->ns >= 2) { /* past the newest: as sent */
				memcpy(lm->cur, lm->target, sizeof(lm->cur)), memcpy(lm->cang, lm->tang, sizeof(lm->cang));
				placed = true;
			}
		}
		if (!placed)
			for (int i = 0; i < dt; i++) /* a step for each refresh the last frame took */
				for (int c = 0; c < 3; c++) {
					int32_t delta = lm->target[c] - lm->cur[c];
					int16_t da = (int16_t)(lm->tang[c] - lm->cang[c]);
					lm->cur[c] = delta > 1024 || delta < -1024 ? lm->target[c] : lm->cur[c] + delta / 3;
					lm->cang[c] = (int16_t)(lm->cang[c] + (da > 8192 || da < -8192 ? da : da / 3));
				}
		/* a door or lift the movement prediction collides with (PSTATE's movers) is drawn
		 * where it collides, so the predicted feet stand on what is drawn: that is the
		 * server's place for it at the acknowledged command, which the predicted player,
		 * not carried by it (as on the PC), stands on too */
		if (pr.on)
			for (int i = 0; i < pr.nmovers; i++)
				if (pr.movers[i].model == m)
					for (int c = 0; c < 3; c++)
						lm->cur[c] = (int32_t)lrintf(pr.movers[i].org[c] * 8);
		for (int c = 0; c < 3; c++)
			ang[c] = lm->cang[c] / 2;
		World_ModelState(m, true, lm->cur, ang);
	}
}

/* the thing's model, once the server has named it and it loads (one new one a frame) */
static int thing_model(const thing_t *t, bool *loaded_one)
{
	int16_t *m;
	if (!t->model || t->model >= MAX_STUDIO || !studioname[t->model])
		return -1;
	m = &studiomdl[t->model];
	if (*m == -2 && !*loaded_one) {
		*loaded_one = true;
		*m = (int16_t)Ents_Model(studioname[t->model]);
		if (*m < 0 && t->model >= 1000)
			*m = (int16_t)Ents_Model("models/player.mdl"); /* a model the card doesn't have */
	}
	return *m;
}

/* the weapon in hand; its light is looked up again every half second */
static void set_viewmodel(const ndscam_t *cam, bool *loaded_one)
{
	static int light = -1, lightframe;
	thing_t t = {0};
	int mdl;
	t.model = (uint16_t)(wp.showing ? wh->info.viewmodel : link.vmodel);
	mdl = thing_model(&t, loaded_one);
	if (mdl >= 0 && (light < 0 || linkframe - lightframe > 30)) {
		int16_t at[3];
		for (int k = 0; k < 3; k++)
			at[k] = (int16_t)BW_NDS_CLAMP(cam->org[k] / 8, -4000, 4000);
		light = World_LightAt(at), lightframe = linkframe;
	}
	if (wp.showing) /* the predicted weapon: m_weaponAnimSequence and Time, as the PC's view model reads them */
		Ents_SetViewModel(mdl, wp.cur.animseq, wp.cur.animtime > 0 ? (uint32_t)(wp.cur.animtime * 1000) : 0, light);
	else
		Ents_SetViewModel(mdl, link.vseq, vb_ms(linkframe - link.vmstart), light);
}

/* the things with a model the DS has, into the model list Ents_Draw draws */
static void add_studio_things(const ndscam_t *cam)
{
	bool loaded_one = false;
	Ents_Clear();
	set_viewmodel(cam, &loaded_one);
	for (int i = 0; i < link.numthings; i++) {
		const thing_t *t = &link.things[i];
		int mdl = thing_model(t, &loaded_one);
		if (mdl >= 0) {
			int16_t org[3] = {t->dorg[0], t->dorg[1], (int16_t)(t->dorg[2] + t->lift)};
			int light = -1;
			if (t->anim >= 0) {
				typeof(anim[0]) *a = &anim[t->anim];
				/* look the light up again only after it moves a little */
				if (a->light >= 0 && abs(a->lightat[0] - org[0]) + abs(a->lightat[1] - org[1]) + abs(a->lightat[2] - org[2]) < 32)
					light = a->light;
			}
			if (light < 0) {
				light = Ents_AddLinked(mdl, org, t->dyaw, t->dseq, t->dms, t->speed, -1);
				if (t->anim >= 0)
					anim[t->anim].light = (int8_t)light, memcpy(anim[t->anim].lightat, org, sizeof(org));
			} else
				Ents_AddLinked(mdl, org, t->dyaw, t->dseq, t->dms, t->speed, light);
		}
	}
}

static bool has_model(const thing_t *t)
{
	return t->model && t->model < MAX_STUDIO && studiomdl[t->model] >= 0;
}

/* everything else the server sends: boxes, turned to face the way the thing does */
static void draw_things(void)
{
	static const uint16_t colours[] = {RGB15(20, 20, 20), RGB15(28, 12, 8), RGB15(28, 26, 8), RGB15(16, 18, 22), RGB15(12, 4, 4)};
	glBindTexture(GL_TEXTURE_2D, 0);
	for (int i = 0; i < link.numthings; i++) {
		const thing_t *t = &link.things[i];
		int16_t mins[3], maxs[3];
		int32_t h;
		if (has_model(t) || t->half > 128)
			continue; /* drawn as itself, or too big to be anything but in the way */
		h = t->half * 8;
		int32_t z = t->kind == 4 ? 4 * 8 : t->height * 8;
		int32_t c4[4][2];
		int s, c;
		for (int k = 0; k < 2; k++) {
			mins[k] = (int16_t)(t->dorg[k] - t->half), maxs[k] = (int16_t)(t->dorg[k] + t->half);
		}
		mins[2] = t->dorg[2], maxs[2] = (int16_t)(t->dorg[2] + (z >> 3));
		if (!World_BoxVisible(mins, maxs))
			continue;
		s = sinLerp(t->dyaw), c = cosLerp(t->dyaw);
		for (int k = 0; k < 4; k++) {
			int32_t x = (k == 0 || k == 3) ? -h : h, y = k < 2 ? -h : h;
			c4[k][0] = t->dorg[0] * 8 + ((c * x - s * y) >> 12);
			c4[k][1] = t->dorg[1] * 8 + ((s * x + c * y) >> 12);
		}
		{
			int32_t z0 = t->dorg[2] * 8, z1 = z0 + z;
			uint16_t col = colours[t->kind < 5 ? t->kind : 0];
			glBegin(GL_QUADS);
			for (int k = 0; k < 4; k++) {
				int n = (k + 1) & 3;
				/* the sides a little darker than the top */
				glColor((uint16_t)((col >> 1) & 0x3def));
				glVertex3v16(c4[k][0], c4[k][1], z0);
				glVertex3v16(c4[n][0], c4[n][1], z0);
				glColor(col);
				glVertex3v16(c4[n][0], c4[n][1], z1);
				glVertex3v16(c4[k][0], c4[k][1], z1);
			}
			glColor(col);
			for (int k = 0; k < 4; k++)
				glVertex3v16(c4[k][0], c4[k][1], z1);
			glEnd();
		}
	}
	glColor(RGB15(31, 31, 31));
}

/* players and the game's markers: upright quads that face the camera */
static void draw_markers(const ndscam_t *cam)
{
	int rx = sinLerp(cam->yaw), ry = -cosLerp(cam->yaw); /* the camera's right, 4.12 */
	glBindTexture(GL_TEXTURE_2D, 0);
	for (int i = 0; i < link.nummarkers; i++) {
		const marker_t *m = &link.markers[i];
		const thing_t *on = NULL;
		bool covered = false;
		/* a player the DS draws with a model needs no marker; one it draws
		 * as a box has its marker go where the box is drawn */
		for (int k = 0; k < link.numthings && !covered; k++) {
			const thing_t *t = &link.things[k];
			if (abs(t->org[0] - m->org[0]) < 24 && abs(t->org[1] - m->org[1]) < 24) {
				covered = has_model(t);
				if (!m->kind && t->model >= 1000)
					on = t;
			}
		}
		if (covered)
			continue;
		int hw = m->kind ? 8 : 16, lo = m->kind ? -8 : -24, hi = m->kind ? 8 : 32;
		int16_t o[3];
		for (int c = 0; c < 3; c++)
			o[c] = (int16_t)(m->org[c] + (on ? on->dorg[c] - on->org[c] : 0));
		int x = BW_NDS_CLAMP(o[0], -4000, 4000) * 8, y = BW_NDS_CLAMP(o[1], -4000, 4000) * 8;
		int z = BW_NDS_CLAMP(o[2], -4000, 4000) * 8;
		int dx = rx * hw * 8 >> 12, dy = ry * hw * 8 >> 12;
		glColor(m->rgb);
		glBegin(GL_QUADS);
		glVertex3v16(x - dx, y - dy, z + lo * 8);
		glVertex3v16(x + dx, y + dy, z + lo * 8);
		glVertex3v16(x + dx, y + dy, z + hi * 8);
		glVertex3v16(x - dx, y - dy, z + hi * 8);
		glEnd();
	}
	glColor(RGB15(31, 31, 31));
}

/* the buttons the keys hold, by scheme */
static int key_buttons(uint32_t held)
{
	int b = 0;
	if (cfg.lefty) { /* A B X Y move, R fires, L jumps */
		b |= held & KEY_R ? B_ATTACK : 0;
		b |= held & KEY_L ? B_JUMP : 0;
		return b;
	}
	b |= held & KEY_A ? B_ATTACK : 0;
	b |= held & KEY_B ? B_JUMP : 0;
	b |= held & KEY_X ? B_USE : 0;
	b |= held & KEY_Y ? B_RELOAD : 0;
	if (cfg.classic) /* L is the strafe and look key */
		b |= held & KEY_R ? B_ATTACK2 : 0;
	else /* L fires, R jumps, as the stylus hand has no thumb for A or B */
		b |= (held & KEY_L ? B_ATTACK : 0) | (held & KEY_R ? B_JUMP : 0);
	return b;
}

static void key_move(uint32_t held, int *fwd, int *side)
{
	uint32_t up = KEY_UP, dn = KEY_DOWN, lf = KEY_LEFT, rt = KEY_RIGHT;
	if (cfg.lefty)
		up = KEY_X, dn = KEY_B, lf = KEY_Y, rt = KEY_A;
	*fwd = held & up ? 127 : held & dn ? -127 : 0;
	*side = 0;
	if (!cfg.classic || held & KEY_L) /* classic: left and right turn unless L is held */
		*side = held & rt ? 127 : held & lf ? -127 : 0;
	if (cfg.classic && held & KEY_L)
		*fwd = 0; /* L + up / down looks */
}

/* INPUT: the v1 part for old servers (from the newest command), then the
 * commands the server hasn't run, made on this map, up to the newest 8 */
static void send_input(const ndscam_t *cam, uint32_t held, int buttons, bool togame, const touchPosition *tp)
{
	pkt_t p;
	const pmcmd_t *c = &pr.cmd[pr.seq & (PR_CMDS - 1)];
	int fwd = BW_NDS_CLAMP(c->move[0] * 127 / 400, -127, 127), side = BW_NDS_CLAMP(c->move[1] * 127 / 400, -127, 127);
	uint32_t first = BW_NDS_MAX(pr.ack, pr.floor) + 1;
	p_begin(&p, L_INPUT);
	p_u32(&p, link.session);
	p_u16(&p, ++link.seq);
	p_u16(&p, (int)(held & (togame ? 0xffff : ~KEY_TOUCH & 0xffff)));
	p_u8(&p, togame ? 1 : 0);
	p_u8(&p, togame ? BW_NDS_MIN(tp->px, 255) : 0);
	p_u8(&p, togame ? BW_NDS_MIN(tp->py, 191) : 0);
	p_u8(&p, fwd), p_u8(&p, side), p_u8(&p, 0);
	p_u16(&p, cam->pitch * 2);
	p_u16(&p, cam->yaw * 2);
	p_u16(&p, link.screen_ack);
	p_u8(&p, link.fps), p_u8(&p, link.minfps), p_u16(&p, link.peakpolys);
	p_u16(&p, link.loadms / 10);
	p_u16(&p, link.spawncount); /* the map this device has */
	p_u16(&p, buttons);         /* what the game reads, mapped here */
	p_u8(&p, pad.impulse), p_u8(&p, pad.impseq);
	if (pr.seq > pr.floor) {
		first = BW_NDS_CLAMP(first, pr.seq >= 8 ? pr.seq - 7 : 1, pr.seq);
		first = BW_NDS_MAX(first, pr.floor + 1);
		p_u8(&p, 'C'), p_u8(&p, LINK_VER);
		p_u32(&p, pr.seq);
		p_u8(&p, (int)(pr.seq - first + 1)), p_u8(&p, 14);
		p_u16(&p, pr.varsver), p_u16(&p, pr.bcurver);
		for (uint32_t s = first; s <= pr.seq; s++) {
			c = &pr.cmd[s & (PR_CMDS - 1)];
			p_u8(&p, c->msec);
			p_u16(&p, c->pitch), p_u16(&p, c->yaw);
			for (int k = 0; k < 3; k++)
				p_u16(&p, c->move[k]);
			p_u16(&p, c->buttons);
			p_u8(&p, c->impulse);
		}
		/* the WEAPONINFO we have (0 none yet): it also tells the server we take it */
		p_u8(&p, 'W'), p_u16(&p, wh ? wh->info.version : 0), p_u16(&p, wh ? wh->info.edict : 0);
		/* the HUDINFO we have (0 none yet): it also tells the server we draw the HUD */
		p_u8(&p, 'H'), p_u16(&p, Hud_Version());
	}
	/* the last EVENT record we have in order: it also tells the server we take them */
	p_u8(&p, 'E'), p_u16(&p, Events_Ack());
	send_to(&link.to, &p);
}

/* classic: left and right turn, L + up and down look */
static void turn(ndscam_t *cam, uint32_t held, int dt)
{
	const int step = 32768 / 180 * dt; /* 120 degrees a second */
	if (!cfg.classic)
		return;
	if (held & KEY_L) {
		if (held & KEY_UP)
			cam->pitch += step;
		if (held & KEY_DOWN)
			cam->pitch -= step;
	} else {
		if (held & KEY_LEFT)
			cam->yaw += step;
		if (held & KEY_RIGHT)
			cam->yaw -= step;
	}
	cam->pitch = BW_NDS_CLAMP(cam->pitch, -8000, 8000);
	cam->yaw = (int16_t)cam->yaw;
}

/* the view's motion, as the PC client makes it for the co-op game. This is a
 * port of Nuclide's client code, which is what runs there (valve/csprogs.dat):
 * - valve/src/client/viewmodel.qc Viewmodel_CalcBob: the bob (v_bob 0.01,
 *   v_bobcycle 0.8, v_bobup 0.5, between -8 and 4 units), which
 *   valve/src/client/camera.qc Camera_RunPosBob adds to the camera along its up;
 * - camera.qc Camera_StrafeRoll (v_camroll 1): roll by 0.015 degrees per unit
 *   a second of sideways speed;
 * - src/client/viewbob.qc, cg_viewBobStyle 3 (the default): the view model
 *   sits at the camera and swings with each footstep, drifts slowly, and
 *   kicks down on landing. Nuclide's v_cambob (camera angle bob) is off by
 *   default, so it isn't here. Velocity, on ground and crouching come from
 *   the server's VIEW. */
static struct {
	float bobtime;                  /* Viewmodel_CalcBob */
	float phase;                    /* ViewBob_QuakeCalc */
	bool wasonground;
	float fallspeed, landtime, landdepth;
} vb;

static float land_offset(float now)
{
	float t = now - vb.landtime;
	if (vb.landdepth <= 0 || t < 0)
		return 0;
	if (t < 0.15f)
		return vb.landdepth * (t / 0.15f);
	t -= 0.15f;
	if (t < 0.3f)
		return vb.landdepth * (1 - t / 0.3f);
	vb.landdepth = 0;
	return 0;
}

static void view_motion(const ndscam_t *cam, ndscam_t *view, int dt)
{
	const float pi = 3.14159265f, todeg = 32768.0f / 360.0f;
	float ft = dt / 60.0f, now = Nds_Vblanks() / 60.0f;
	float vx = link.pvel[0], vy = link.pvel[1];
	float speed = sqrtf(vx * vx + vy * vy), bob, cyc;
	float sy = sinLerp(cam->yaw) / 4096.0f, cy = cosLerp(cam->yaw) / 4096.0f;
	float sp = sinLerp(cam->pitch) / 4096.0f, cp = cosLerp(cam->pitch) / 4096.0f;
	int icycle;

	/* Viewmodel_CalcBob */
	icycle = (int)(vb.bobtime / 0.8f);
	vb.bobtime += ft;
	cyc = (vb.bobtime - icycle * 0.8f) / 0.8f;
	cyc = pi * (cyc / 0.5f);
	bob = BW_NDS_CLAMP(speed * 0.01f * sinf(cyc), -8.0f, 4.0f);

	*view = *cam;
	/* Camera_RunPosBob: along the camera's up (pitch up positive here) */
	view->org[0] += (int32_t)(-cy * sp * bob * 8);
	view->org[1] += (int32_t)(-sy * sp * bob * 8);
	view->org[2] += (int32_t)(cp * bob * 8);
	/* Camera_StrafeRoll: the right is (sin yaw, -cos yaw) */
	view->roll = (int)((vx * sy - vy * cy) * 0.015f * todeg);

	/* ViewBob_QuakeCalc */
	if (speed < 5)
		vb.phase = 0;
	else if (link.onground)
		vb.phase += ft * (link.crouch ? 2.0f : speed > 200 ? 1.6f : 1.2f);
	if (!link.onground)
		vb.fallspeed = BW_NDS_MAX(-link.pvel[2], 0.0f);
	else if (!vb.wasonground) {
		if (vb.fallspeed > 100)
			vb.landdepth = BW_NDS_CLAMP(vb.fallspeed * 0.025f, 4.0f, 24.0f), vb.landtime = now;
		vb.fallspeed = 0;
	}
	vb.wasonground = link.onground;

	/* ViewBob_QuakeApply: the gun's origin is the camera's, so only the
	 * swing, the drift and the landing move it against the view */
	{
		float swing = speed * fabsf(sinf(vb.phase * pi)) * 0.005f, side = (int)vb.phase & 1 ? -swing : swing;
		float drift = (speed + 40) * sinf(now) * 0.01f, land = land_offset(now);
		Ents_ViewAng[0] = (int)((swing + drift + land * 0.25f) * todeg);
		Ents_ViewAng[1] = (int)((side + drift) * todeg);
		Ents_ViewAng[2] = (int)((side + drift) * todeg);
		Ents_ViewOfs[0] = Ents_ViewOfs[1] = 0;
		Ents_ViewOfs[2] = (int32_t)(-land * 0.5f * 8);
	}
}

/* --- movement prediction (link v2) ---------------------------------------------------- */

static float rdf32(const uint8_t *d)
{
	uint32_t u = rd32(d);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

static void rdvec(const uint8_t *d, float *v)
{
	for (int k = 0; k < 3; k++)
		v[k] = rdf32(d + k * 4);
}

static void pred_freebrushes(void)
{
	free(pr.bnew), free(pr.bcur), free(pr.bhave);
	pr.bnew = pr.bcur = NULL, pr.bhave = NULL;
	pr.bnewtotal = pr.bnewgot = pr.bcurn = pr.bnewcap = pr.bcurcap = pr.bhavecap = 0;
	pr.bnewver = pr.bcurver = 0;
	pr.nplaced = 0;
}

/* a WELCOME for another map (or the first): nothing of the last map's holds,
 * and commands made for it are never sent again (the server drops them) */
static void pred_newmap(void)
{
	pred_freebrushes();
	pr.varsver = 0;
	pr.on = false;
	pr.rp = false;
	pr.stair = false;
	pr.havepseq = false;
	pr.pstatelen = 0;
	memset(pr.predseq, 0, sizeof(pr.predseq));
	pr.floor = pr.seq;
	if (wh) {
		wh->info.version = 0, wh->info.edict = 0;
		memset(wh->ringseq, 0, sizeof(wh->ringseq));
	}
	wp.havesrv = false, wp.showing = false, wp.arp = false;
	Hull_SetBrushes(NULL, 0);
	Hull_SetBoxes(NULL, 0);
}

/* PMOVEVARS: the game's movement values, the global set and the player's own */
static void handle_pmovevars(const uint8_t *d, int len)
{
	uint16_t ver;
	if (len < 10 + 2 * PMV_COUNT * 4 || (uint16_t)rd16(d + 4) != link.spawncount || d[8] != PMV_COUNT)
		return;
	ver = (uint16_t)rd16(d + 6);
	if (!ver || ver == pr.varsver)
		return;
	for (int i = 0; i < PMV_COUNT; i++) {
		pr.vars.g[i] = rdf32(d + 10 + i * 4);
		pr.vars.p[i] = rdf32(d + 10 + (PMV_COUNT + i) * 4);
	}
	PM_SetVars(&pr.vars);
	pr.varsver = ver;
}

/* BRUSHINFO: the brush entities that collide, in pages; a version counts once all its entries came */
static void handle_brushinfo(const uint8_t *d, int len)
{
	uint16_t ver;
	int total, first, count;
	if (len < 13 || (uint16_t)rd16(d + 4) != link.spawncount)
		return;
	ver = (uint16_t)rd16(d + 6), total = rd16(d + 8), first = rd16(d + 10), count = d[12];
	if (!ver || ver == pr.bcurver || total > HULL_MAX_BRUSHES || first + count > total || len < 13 + count * 18)
		return;
	if (ver != pr.bnewver || total != pr.bnewtotal) {
		/* the buffers only grow: a door coming to rest is a new version, often */
		if (total > pr.bnewcap) {
			hullbrush_t *nb = realloc(pr.bnew, sizeof(hullbrush_t) * (size_t)total);
			if (!nb) {
				pr.bnewver = 0;
				return;
			}
			pr.bnew = nb, pr.bnewcap = total;
		}
		if (total > pr.bhavecap) {
			uint8_t *nh = realloc(pr.bhave, (size_t)total);
			if (!nh) {
				pr.bnewver = 0;
				return;
			}
			pr.bhave = nh, pr.bhavecap = total;
		}
		if (total)
			memset(pr.bhave, 0, (size_t)total);
		pr.bnewver = ver, pr.bnewtotal = total, pr.bnewgot = 0, pr.ntnew = 0;
		pr.ntouchnew = 0, pr.ntouchwant = 0;
	}
	for (int i = 0; i < count; i++) {
		const uint8_t *e = d + 13 + i * 18;
		hullbrush_t *b = &pr.bnew[first + i];
		b->edict = (uint16_t)rd16(e);
		b->model = (uint16_t)rd16(e + 2);
		b->solid = e[4];
		b->skin = (int8_t)e[5];
		rdvec(e + 6, b->origin);
		/* the page's angles come after its entries (none from an older server) */
		if (len >= 13 + count * 30)
			rdvec(d + 13 + count * 18 + i * 12, b->angles);
		else
			memset(b->angles, 0, sizeof(b->angles));
		if (!pr.bhave[first + i])
			pr.bhave[first + i] = 1, pr.bnewgot++;
	}
	/* then the turned ones' boxes: u8 count, then u8 entry, absmin, absmax each; then the
	 * touches: u16 the version's, u8 this packet's, then 44 bytes each */
	if (len >= 13 + count * 30 + 1) {
		const uint8_t *t = d + 13 + count * 30;
		int nt = t[0], j;
		t++;
		for (j = 0; j < nt && t + 25 <= d + len; j++, t += 25) {
			int idx = first + t[0], k;
			if (t[0] >= count)
				continue;
			for (k = 0; k < pr.ntnew && pr.tnew[k].i != idx; k++)
				;
			if (k == pr.ntnew) {
				if (pr.ntnew >= PR_TBOX)
					continue;
				pr.ntnew++;
			}
			pr.tnew[k].i = (uint16_t)idx;
			rdvec(t + 1, pr.tnew[k].mn);
			rdvec(t + 13, pr.tnew[k].mx);
		}
		if (j == nt && t + 3 <= d + len) {
			int want = rd16(t), ntc = t[2];
			pr.ntouchwant = want < HULL_MAX_TOUCH ? want : HULL_MAX_TOUCH;
			t += 3;
			for (j = 0; j < ntc && t + 44 <= d + len; j++, t += 44) {
				pmtouch_t tc;
				int k;
				tc.edict = (uint16_t)rd16(t);
				tc.kind = t[2], tc.flags = t[3];
				tc.speed = rdf32(t + 4);
				rdvec(t + 8, tc.dir);
				rdvec(t + 20, tc.absmin);
				rdvec(t + 32, tc.absmax);
				for (k = 0; k < pr.ntouchnew && pr.touchnew[k].edict != tc.edict; k++)
					;
				if (k == pr.ntouchnew) {
					if (pr.ntouchnew >= HULL_MAX_TOUCH)
						continue;
					pr.ntouchnew++;
				}
				pr.touchnew[k] = tc;
			}
		}
	}
	if (pr.bnewgot < total || pr.ntouchnew < pr.ntouchwant)
		return;
	/* complete: it becomes the list (the buffers swap), and the scene, once; PSTATE
	 * only places its movers */
	{
		hullbrush_t *t = pr.bcur;
		int cap = pr.bcurcap;
		pr.bcur = pr.bnew, pr.bcurcap = pr.bnewcap;
		pr.bnew = t, pr.bnewcap = cap;
	}
	pr.bcurn = total, pr.bcurver = ver;
	pr.bnewver = 0, pr.bnewtotal = 0;
	memcpy(pr.tcur, pr.tnew, sizeof(pr.tnew[0]) * (size_t)pr.ntnew);
	pr.ntcur = pr.ntnew, pr.ntnew = 0;
	Hull_SetBrushes(pr.bcur, pr.bcurn);
	Hull_SetTouches(pr.touchnew, pr.ntouchnew);
	for (int k = 0; k < pr.ntcur; k++)
		Hull_BrushBox(pr.tcur[k].i, pr.tcur[k].mn, pr.tcur[k].mx);
	pr.nplaced = 0;
}

/* the bcur entry for an edict (the list is sorted by edict), or -1 */
static int find_brush(int e)
{
	int lo = 0, hi = pr.bcurn - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (pr.bcur[mid].edict == e)
			return mid;
		if (pr.bcur[mid].edict < e)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

/* keeps the newest PSTATE for this map; handled once the packets are drained */
static void keep_pstate(const uint8_t *d, int len)
{
	uint16_t pseq;
	if (len < 180 || len > (int)sizeof(pr.pstate) || (uint16_t)rd16(d + 14) != link.spawncount)
		return;
	pseq = (uint16_t)rd16(d + 8);
	if (pr.havepseq && (int16_t)(pseq - pr.pseq) <= 0)
		return;
	pr.havepseq = true, pr.pseq = pseq;
	memcpy(pr.pstate, d, (size_t)len);
	pr.pstatelen = len;
}

/* boxes are where the server had them at the acked command: one that has
 * since walked into us must not pin the prediction (PM_FOCUS_DROPSTUCK) */
static uint32_t focus_dropstuck(void *ctx, const float org[3], const float mins[3], const float maxs[3], float radius,
                                int flags)
{
	return Hull_Focus(ctx, org, mins, maxs, radius, flags | PM_FOCUS_DROPSTUCK);
}

static const pmtrace_if pred_trace = {NULL, Hull_Move, focus_dropstuck, Hull_TouchOf, Hull_Triggers, Hull_Fired};

static void run_cmd(pmstate_t *s, const pmcmd_t *c)
{
	uint64_t t0 = LINK_PM_TICKS();
	uint32_t t;
	Hull_TouchSeq(c->seq);
	PM_Run(s, c, &pr.vars, &pred_trace);
	t = (uint32_t)(LINK_PM_TICKS() - t0);
	Link_PredStats.pm_runs++;
	Link_PredStats.pm_ticks += t;
	Link_PredStats.pm_ticks_max = BW_NDS_MAX(Link_PredStats.pm_ticks_max, t);
}

/* a melee weapon's test: one trace, with a broadphase of its own (PM_Run's
 * covers the move); the next PM_Run sets its own again */
static void weapon_trace(void *ctx, const float start[3], const float mins[3], const float maxs[3], const float end[3],
                         int type, int32_t mask, pmhit_t *tr)
{
	float d = 0;
	for (int k = 0; k < 3; k++)
		d += (end[k] - start[k]) * (end[k] - start[k]);
	Hull_Focus(ctx, start, mins, maxs, sqrtf(d) + 1, 0);
	Hull_Move(ctx, start, mins, maxs, end, type, mask, tr);
}
static const pmtrace_if weapon_trif = {NULL, weapon_trace, NULL};

/* one command's weapon part, after its movement: ps the movement's state after
 * it (its weapon flags are the weapon code's), or NULL when the movement isn't
 * predicted or isn't being run again (what it left last time is kept) */
static void weapon_cmd(wstate_t *ws, const pmcmd_t *c, pmstate_t *ps)
{
	wenvrec_t *r = &wh->env[c->seq & (PR_CMDS - 1)];
	wenv_t e;
	if (ps) {
		r->health = ps->health, r->waterlevel = (int8_t)ps->waterlevel;
		r->landed = ps->landspeed > 0, r->downforce = ps->landspeed;
		for (int k = 0; k < 3; k++)
			r->eye[k] = ps->origin[k] + ps->view_ofs[k];
		r->vflags = ps->vflags, r->gflags = ps->gflags, r->seq = c->seq;
	} else if (r->seq != c->seq)
		*r = wp.base, r->seq = c->seq;
	ws->vflags = (r->vflags & ~W_VFL_BITS) | (ws->vflags & W_VFL_BITS);
	ws->gflags = (r->gflags & ~W_GF_BITS) | (ws->gflags & W_GF_BITS);
	memset(&e, 0, sizeof(e));
	e.seq = c->seq, e.health = r->health, e.waterlevel = r->waterlevel;
	e.landed = r->landed, e.downforce = r->downforce;
	memcpy(e.eye, r->eye, sizeof(e.eye));
	e.velocity = ps ? ps->velocity : NULL;
	e.trace = Hull_Rule() != HULL_NONE ? &weapon_trif : NULL; /* the PC traces whether it predicts movement or not */
	e.nboxes = wh->nboxes, e.boxmins = wh->bmins, e.boxmaxs = wh->bmaxs, e.boxsolid = wh->bsolid;
	Weapon_Run(ws, Weapon_Usable(ws, &wh->info) ? &wh->info : NULL, c, &e);
	if (ps) {
		ps->vflags = (ps->vflags & ~W_VFL_BITS) | (ws->vflags & W_VFL_BITS);
		ps->gflags = (ps->gflags & ~W_GF_BITS) | (ws->gflags & W_GF_BITS);
	}
}

/* PSTATE's weapon block (after the boxes) and what the weapon code reads of the player */
static void weapon_pstate(const uint8_t *d, int len)
{
	int off = 180 + d[21] * 26 + d[178] * 27;
	wenvrec_t *b = &wp.base;
	wp.havesrv = wh && off < len && Weapon_ParseState(&wp.srv, d + off, len - off) > 0;
	if (!wp.havesrv)
		return;
	b->vflags = rd32(d + 114), b->gflags = rd32(d + 118);
	wp.srv.vflags = b->vflags, wp.srv.gflags = b->gflags;
	b->health = (int16_t)rd16(d + 176), b->waterlevel = (int8_t)d[124];
	b->landed = false, b->downforce = 0;
	for (int k = 0; k < 3; k++)
		b->eye[k] = rdf32(d + 26 + k * 4) + rdf32(d + 74 + k * 4);
	wh->nboxes = BW_NDS_MIN(d[178], LINK_BOXES);
	for (int i = 0; i < wh->nboxes; i++) {
		const uint8_t *x = d + 180 + d[21] * 26 + i * 27;
		rdvec(x + 2, wh->bmins[i]);
		rdvec(x + 14, wh->bmaxs[i]);
		wh->bsolid[i] = x[26];
	}
}

/* what we predicted for the acked command holds: every field the weapon code reads
 * when it can run this weapon, else only the punch (the rest is the server's alone) */
static bool weapon_matches(uint32_t ack)
{
	const wstate_t *p;
	if (!wp.havesrv)
		return true;
	p = &wh->ring[ack & (PR_RING - 1)];
	if (!ack || wh->ringseq[ack & (PR_RING - 1)] != ack)
		return false;
	if (Weapon_Usable(&wp.srv, &wh->info))
		return Weapon_Matches(p, &wp.srv);
	for (int k = 0; k < 3; k++)
		if (fabsf(p->punch[k] - wp.srv.punch[k]) > 0.01f || fabsf(p->punchvel[k] - wp.srv.punchvel[k]) > 0.05f)
			return false;
	return true;
}

/* without movement prediction: the weapon runs on alone, from the server's state;
 * the commands it hasn't run are run again in weapon_catchup, within the frame's budget */
static void weapon_alone(uint32_t ack)
{
	if (cfg.nopredict) /* predict=0: the server's, as it comes */
		wp.cur = wp.srv;
	if (cfg.nopredict || !wp.havesrv || weapon_matches(ack))
		return;
	if (pr.seq - ack >= PR_CMDS) {
		wp.cur = wp.srv, wp.arp = false;
		memset(wh->ringseq, 0, sizeof(wh->ringseq));
		return;
	}
	if (ack)
		wring_put(ack, &wp.srv);
	wp.replays++;
	wp.rpstate = wp.srv; /* the last prediction is shown until the replay is done */
	wp.arpnext = BW_NDS_MAX(ack, pr.floor) + 1;
	wp.arp = true;
}

/* WEAPONINFO: the weapon in hand's values, until the server names another version */
static void handle_weaponinfo(const uint8_t *d, int len)
{
	winfo_t *w;
	uint16_t edict, ver;
	if (len < 10 || (uint16_t)rd16(d + 4) != link.spawncount)
		return;
	edict = (uint16_t)rd16(d + 6), ver = (uint16_t)rd16(d + 8);
	if (!wh || !ver || (ver == wh->info.version && edict == wh->info.edict))
		return; /* (no tables yet: not acknowledged, so the server sends it again) */
	/* parsed aside, so a body that doesn't parse leaves the last one; on the heap only meanwhile */
	if (!(w = malloc(sizeof(*w))))
		return;
	w->edict = edict, w->version = ver;
	if (Weapon_ParseInfo(w, d + 10, len - 10))
		wh->info = *w;
	free(w);
}

/* close enough that replaying would change nothing worth seeing (and bit for bit, for the count).
 * Every field PM_Run reads is compared: what isn't would never be taken from the server while
 * nothing mispredicts (the PC starts each frame from the server's state, Player.qc:1287-1320),
 * so a long jump picked up, a weapon's speed or the stamina left must force a replay too */
static bool state_matches(const pmstate_t *a, const pmstate_t *b, bool *exact)
{
	for (int k = 0; k < 3; k++)
		if (fabsf(a->origin[k] - b->origin[k]) > 1.0f / 32 || fabsf(a->velocity[k] - b->velocity[k]) > 0.5f)
			return false;
	if (a->flags != b->flags || a->vflags != b->vflags || a->gflags != b->gflags || a->movetype != b->movetype ||
	    a->solid != b->solid || a->waterlevel != b->waterlevel || a->watertype != b->watertype ||
	    a->groundent != b->groundent || memcmp(a->mins, b->mins, 12) || memcmp(a->maxs, b->maxs, 12) ||
	    memcmp(a->view_ofs, b->view_ofs, 12) || memcmp(a->basevel, b->basevel, 12) ||
	    memcmp(a->hookvel, b->hookvel, 12) || a->gravity != b->gravity || a->friction != b->friction ||
	    a->maxspeed != b->maxspeed || a->wsm != b->wsm || a->wsmfire != b->wsmfire || a->wsmzoom != b->wsmzoom ||
	    a->viewzoom != b->viewzoom || a->health != b->health || a->rules != b->rules)
		return false;
	/* the clocks and stamina run on with each command: what float rounding leaves alone */
	if (fabsf(a->time_since_jump - b->time_since_jump) > 1e-3f ||
	    fabsf(a->time_since_teleport - b->time_since_teleport) > 1e-3f || fabsf(a->stamina - b->stamina) > 1e-3f ||
	    fabsf(a->speed_target - b->speed_target) > 0.5f)
		return false;
	*exact = !memcmp(a->origin, b->origin, 12) && !memcmp(a->velocity, b->velocity, 12);
	return true;
}

/* the movers where the server had them: last PSTATE's back at rest, this one's placed
 * (the list went to hull_nds.c once, when its version came complete) */
static void place_movers(const uint8_t *d, int len, int nmovers)
{
	/* PS_MOVERANGLES: each mover's angles, absmin and absmax, then their count and 'R', at the
	 * end of the packet */
	const uint8_t *ang = (d[179] & 1) && len >= 2 + d[21] * 36 + 180 && d[len - 1] == 'R' && d[len - 2] == d[21]
	                         ? d + len - 2 - d[21] * 36
	                         : NULL;
	for (int i = 0; i < pr.nplaced; i++) {
		int b = pr.placed[i];
		Hull_PlaceBrush(b, pr.bcur[b].origin, pr.bcur[b].angles);
		for (int k = 0; k < pr.ntcur; k++)
			if (pr.tcur[k].i == b)
				Hull_BrushBox(b, pr.tcur[k].mn, pr.tcur[k].mx);
	}
	pr.nplaced = 0;
	pr.nmovers = nmovers;
	for (int i = 0; i < nmovers; i++) {
		const uint8_t *m = d + 180 + i * 26;
		float org[3];
		int b;
		pr.movers[i].e = (uint16_t)rd16(m);
		pr.movers[i].model = 0;
		rdvec(m + 14, pr.movers[i].vel);
		if ((b = find_brush(pr.movers[i].e)) < 0)
			continue;
		rdvec(m + 2, org);
		memcpy(pr.movers[i].org, org, sizeof(org));
		pr.movers[i].model = pr.bcur[b].model;
		{
			float a[3] = {0, 0, 0}, mn[3], mx[3];
			if (ang)
				rdvec(ang + i * 36, a);
			else
				memcpy(a, pr.bcur[b].angles, sizeof(a));
			Hull_PlaceBrush(b, org, a);
			if (ang && (a[0] != 0 || a[1] != 0 || a[2] != 0)) {
				rdvec(ang + i * 36 + 12, mn);
				rdvec(ang + i * 36 + 24, mx);
				Hull_BrushBox(b, mn, mx);
			}
		}
		pr.placed[pr.nplaced++] = b;
	}
}

/* PSTATE (docs/spec/link.md): the server's state after the command it ran
 * last, the movers and boxes round the player; from it the commands it
 * hasn't run are run again, unless what we predicted for that one holds */
static void handle_pstate(ndscam_t *cam)
{
	const uint8_t *d = pr.pstate;
	int len = pr.pstatelen, nmovers, nboxes, status;
	uint32_t ack;
	uint16_t varsver, brushver;
	pmstate_t srv;
	bool had, exact, turned;
	float err = 0;
	if (!len)
		return;
	pr.pstatelen = 0;
	ack = rd32(d + 4), status = d[10];
	Link_PredStats.clipped = d[11], Link_PredStats.gaps = (uint16_t)rd16(d + 12);
	varsver = (uint16_t)rd16(d + 16), brushver = (uint16_t)rd16(d + 18);
	nmovers = BW_NDS_MIN(d[21], PR_MOVERS), nboxes = BW_NDS_MIN(d[178], LINK_BOXES);
	if (180 + d[21] * 26 + d[178] * 27 > len)
		return;
	if (ack > pr.ack && ack <= pr.seq)
		pr.ack = ack;
	weapon_pstate(d, len);

	memset(&srv, 0, sizeof(srv));
	rdvec(d + 26, srv.origin), rdvec(d + 38, srv.velocity), rdvec(d + 50, srv.basevel), rdvec(d + 62, srv.hookvel);
	rdvec(d + 74, srv.view_ofs), rdvec(d + 86, srv.mins), rdvec(d + 98, srv.maxs);
	srv.flags = rd32(d + 110), srv.vflags = rd32(d + 114), srv.gflags = rd32(d + 118);
	srv.movetype = d[122], srv.solid = d[123], srv.waterlevel = (int8_t)d[124], srv.watertype = (int8_t)d[125];
	srv.groundent = rd16(d + 126);
	srv.gravity = rdf32(d + 128), srv.friction = rdf32(d + 132), srv.maxspeed = rdf32(d + 136);
	srv.time_since_jump = rdf32(d + 140), srv.time_since_teleport = rdf32(d + 144);
	srv.stamina = rdf32(d + 148), srv.speed_target = rdf32(d + 152);
	srv.wsm = rdf32(d + 160), srv.wsmfire = rdf32(d + 164), srv.wsmzoom = rdf32(d + 168), srv.viewzoom = rdf32(d + 172);
	srv.health = (int16_t)rd16(d + 176);
	srv.rules = (uint8_t)((status >> 2) & 7);

	/* the game turned us (a teleport, a respawn) */
	if ((turned = d[20] != pr.fixseq)) {
		pr.fixseq = d[20];
		cam->pitch = (int16_t)rd16(d + 22) / 2;
		cam->yaw = (int16_t)rd16(d + 24) / 2;
		pr.stair = false;
	}

	/* nothing to predict with here: the view shows VIEW as before */
	if (cfg.nopredict || pr.srvver < LINK_VER || !(status & 1) || Hull_Rule() == HULL_NONE || !pr.varsver ||
	    !pr.bcurver) {
		pr.on = false;
		pr.rp = false;
		weapon_alone(ack);
		return;
	}
	/* made with movement values or a brush list we don't have yet (a door came to rest, a
	 * page is on its way): predicting goes on from what we have, and the next one counts */
	if ((int16_t)(pr.varsver - varsver) < 0 || pr.bcurver != brushver)
		return;

	/* the scene as the server had it: the movers where they are, and the boxes */
	place_movers(d, len, nmovers);
	{
		static hullbox_t boxes[LINK_BOXES];
		for (int i = 0; i < nboxes; i++) {
			const uint8_t *b = d + 180 + d[21] * 26 + i * 27;
			boxes[i].edict = (uint16_t)rd16(b);
			rdvec(b + 2, boxes[i].mins);
			rdvec(b + 14, boxes[i].maxs);
			boxes[i].solid = b[26];
		}
		Hull_SetBoxes(boxes, nboxes);
		/* one the server's player is in already holds it there, as on the server */
		Hull_PinBoxes(srv.origin, srv.mins, srv.maxs);
	}

	/* turned, it goes on as any other state: the server still runs the commands it hasn't
	 * with the angles they were made with (input_v2), and so do we (the PC's PreFrame) */
	had = !turned && pr.on && pr.predseq[ack & (PR_RING - 1)] == ack && ack && (!pr.rp || ack < pr.rpnext);
	if (had) {
		const pmstate_t *p = &pr.pred[ack & (PR_RING - 1)];
		float dv = 0;
		for (int k = 0; k < 3; k++) {
			float e = p->origin[k] - srv.origin[k], v = p->velocity[k] - srv.velocity[k];
			err += e * e, dv += v * v;
		}
		err = sqrtf(err), dv = sqrtf(dv);
		Link_PredStats.compared++;
		Link_PredStats.within += err <= 0.5f;
		Link_PredStats.err_sum_cu += (uint32_t)BW_NDS_MIN(err * 100, 1e9f);
		Link_PredStats.err_max_cu = BW_NDS_MAX(Link_PredStats.err_max_cu, (uint32_t)BW_NDS_MIN(err * 100, 1e9f));
		Link_PredStats.verr_max = BW_NDS_MAX(Link_PredStats.verr_max, (uint32_t)BW_NDS_MIN(dv, 1e9f));
		if (state_matches(p, &srv, &exact) && weapon_matches(ack)) {
			Link_PredStats.rp_skip++;
			Link_PredStats.rp_exact += exact;
			return;
		}
	}
	/* the server's state at ack holds from now on: a PSTATE with the same ack again (an
	 * idle one, while our INPUT is lost) matches it, and doesn't replay the backlog again */
	if (ack && pr.seq - ack < PR_RING) {
		pr.pred[ack & (PR_RING - 1)] = srv, pr.predseq[ack & (PR_RING - 1)] = ack;
		wring_put(ack, &wp.srv);
	}
	if (pr.seq - ack >= PR_CMDS) { /* older than the commands kept: the server's state, and a snap */
		pr.cur = srv;
		wp.cur = wp.srv;
		memset(pr.predseq, 0, sizeof(pr.predseq));
		if (wh)
			memset(wh->ringseq, 0, sizeof(wh->ringseq));
		Link_PredStats.snaps += pr.on;
		pr.stair = false;
		pr.on = true;
		pr.rp = false;
		return;
	}
	/* run what the server hasn't, from its state (never what was made for another map) */
	Link_PredStats.replays++;
	Link_PredStats.replay_max = BW_NDS_MAX(Link_PredStats.replay_max, pr.seq - ack);
	if (!pr.on)
		pr.cur = srv, wp.cur = wp.srv; /* nothing to show meanwhile but the server's state */
	pr.rp = true;
	pr.rpsnap = had && err > 64;
	pr.rpnext = BW_NDS_MAX(ack, pr.floor) + 1;
	pr.rpstate = srv;
	Hull_ForgetFired(pr.rpnext - 1); /* the one-shot pushes of the commands run again */
	wp.rpstate = wp.srv;
	pr.on = true;
}

/* how long this frame's replays may still take (PR_REPLAY_MS from the start of the
 * frame's prediction), as tickGetCount() counts */
static uint64_t rp_deadline;
static bool rp_more(int n)
{
	return n < PR_REPLAY && (n < PR_REPLAY_MIN || tickGetCount() < rp_deadline);
}

/* the replay handle_pstate started, PR_REPLAY commands a frame at most and no longer
 * than PR_REPLAY_MS: a stall's backlog (up to 63) would otherwise take one frame of 100 ms */
static void pred_catchup(void)
{
	int n = 0;
	if (!pr.rp)
		return;
	for (; pr.rpnext <= pr.seq && rp_more(n); pr.rpnext++, n++) {
		run_cmd(&pr.rpstate, &pr.cmd[pr.rpnext & (PR_CMDS - 1)]);
		if (wp.havesrv) {
			weapon_cmd(&wp.rpstate, &pr.cmd[pr.rpnext & (PR_CMDS - 1)], &pr.rpstate);
			wring_put(pr.rpnext, &wp.rpstate);
		}
		pr.pred[pr.rpnext & (PR_RING - 1)] = pr.rpstate;
		pr.predseq[pr.rpnext & (PR_RING - 1)] = pr.rpnext;
		Link_PredStats.replayed++;
	}
	if (pr.rpnext <= pr.seq) {
		Link_PredStats.rp_cut++; /* frames it went on past */
		return;
	}
	pr.cur = pr.rpstate;
	if (wp.havesrv)
		wp.cur = wp.rpstate;
	pr.rp = false;
	if (pr.rpsnap)
		Link_PredStats.snaps++, pr.stair = false;
}

/* weapon_alone's replay, as pred_catchup goes on with the movement's */
static void weapon_catchup(void)
{
	int n = 0;
	if (!wp.arp)
		return;
	if (pr.on || !wp.havesrv) { /* the movement's replay runs the weapon now */
		wp.arp = false;
		return;
	}
	for (; wp.arpnext <= pr.seq && rp_more(n); wp.arpnext++, n++) {
		weapon_cmd(&wp.rpstate, &pr.cmd[wp.arpnext & (PR_CMDS - 1)], NULL);
		wring_put(wp.arpnext, &wp.rpstate);
	}
	if (wp.arpnext <= pr.seq)
		return;
	wp.cur = wp.rpstate;
	wp.arp = false;
}

/* botwalk=1: a fixed 12-second pattern instead of the pad, so runs repeat */
static void bot_input(int frame, int *fwd, int *side, int *buttons, ndscam_t *cam, int dt)
{
	int t = (frame - pr.botstart) % (12 * HZ);
	*fwd = *side = 0, *buttons = 0;
	if (t < 3 * HZ)
		*fwd = 127; /* walk, and jump twice */
	if ((t >= 2 * HZ && t < 2 * HZ + 12) || (t >= 2 * HZ + 40 && t < 2 * HZ + 52))
		*buttons |= B_JUMP;
	if (t >= 3 * HZ && t < 5 * HZ)
		*side = 127;
	if (t >= 5 * HZ && t < 13 * HZ / 2)
		*fwd = 127, *buttons |= B_CROUCH;
	if (t >= 13 * HZ / 2 && t < 8 * HZ)
		*fwd = -127;
	if (t >= 8 * HZ && t < 9 * HZ) /* a quarter turn, walking */
		*fwd = 127, cam->yaw = (int16_t)(cam->yaw - 8192 / HZ * dt);
	if (t >= 9 * HZ && t < 10 * HZ)
		*side = -127;
	if (t >= 9 * HZ + 30 && t < 9 * HZ + 42)
		*buttons |= B_JUMP;
}

/* the movement keys as the PC makes a command of them: cl_forwardspeed,
 * cl_backspeed and cl_sidespeed are 400 (valve/default_cvar.cfg), then
 * ncPlayer::ClientInputFrame (src/shared/game/Player.qc) normalises the
 * direction, clears sprint when moving forward (the DS has no sprint) and
 * floors the values */
static void make_move(int fwd, int side, int *buttons, int16_t *out)
{
	float mv[3] = {fwd > 0 ? 400.0f : fwd < 0 ? -400.0f : 0, side > 0 ? 400.0f : side < 0 ? -400.0f : 0, 0};
	float dir[3] = {mv[0], mv[1], mv[2]}, len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (len) {
		float inv = 1.0f / len;
		dir[0] *= inv, dir[1] *= inv, dir[2] *= inv;
	}
	mv[0] = dir[0] * fabsf(mv[0]);
	mv[1] = dir[1] * fabsf(mv[1]);
	if (mv[0] > 0)
		*buttons &= ~B_SPRINT;
	for (int k = 0; k < 3; k++)
		out[k] = (int16_t)floorf(mv[k]);
}

/* the millisecond clock of a refresh, at the DS's 59.8261 Hz */
static uint32_t vb_ms(int vb) { return (uint32_t)((uint64_t)(uint32_t)vb * 1000000 / 59826); }

/* the commands due by this refresh, each predicted on from the last; returns how many.
 * Below 30 fps a command covers two refreshes, as with cmdvb=2: a slow map stays at two
 * PM_Run a frame (three at 20 fps would slow it further), and the server runs the same */
static int make_commands(int frame, const ndscam_t *cam, int fwd, int side, int buttons)
{
	int step = cfg.cmdvb, due = (frame - pr.cmdvb) / step, made = 0, left;
	pmcmd_t c;
	memset(&c, 0, sizeof(c));
	if (due > 4) { /* a long frame (a map load): the last 4, the first taking up to 100 ms */
		pr.cmdvb = frame - 4 * step;
		if ((int32_t)(vb_ms(pr.cmdvb) - pr.cmdms) > 100)
			pr.cmdms = vb_ms(pr.cmdvb) - 100;
		due = 4;
	}
	c.pitch = (int16_t)(cam->pitch * 2), c.yaw = (int16_t)(cam->yaw * 2);
	make_move(fwd, side, &buttons, c.move);
	c.buttons = (uint16_t)buttons;
	left = due * step;
	for (; left > 0; made++) {
		int32_t ms, n = step == 1 && due >= 3 && left >= 2 ? 2 : step;
		pr.cmdvb += n, left -= n;
		ms = BW_NDS_CLAMP((int32_t)(vb_ms(pr.cmdvb) - pr.cmdms), 1, 100);
		pr.cmdms += (uint32_t)ms;
		c.seq = ++pr.seq;
		c.msec = (uint8_t)ms;
		c.impulse = 0;
		if (pad.impseq != pr.impseq && pad.impulse) /* a one-shot goes in one command */
			c.impulse = pad.impulse;
		pr.impseq = pad.impseq;
		pr.cmd[c.seq & (PR_CMDS - 1)] = c;
		Link_PredStats.cmds++;
		if (pr.on) {
			run_cmd(&pr.cur, &c);
			if (wp.havesrv)
				weapon_cmd(&wp.cur, &c, &pr.cur);
			if (!pr.rp) { /* else the replay puts its own there */
				pr.pred[c.seq & (PR_RING - 1)] = pr.cur;
				pr.predseq[c.seq & (PR_RING - 1)] = c.seq;
				wring_put(c.seq, &wp.cur);
			}
		} else if (wp.havesrv && !cfg.nopredict) {
			weapon_cmd(&wp.cur, &c, NULL);
			if (!wp.arp) /* else the replay puts its own there */
				wring_put(c.seq, &wp.cur);
		}
	}
	return made;
}

/* ncView::StairSmooth (src/client/View.qc): on the ground, the eye rises at
 * most 100 units a second (cg_viewZSmoothingTime 0.1) and lags at most 16
 * (cg_viewZSmoothingMax); not while standing on something moving up or down,
 * and a jump of more than 64 units is a teleport */
static float stair_smooth(float z, float ft)
{
	float e = z, gvz = 0;
	for (int i = 0; i < pr.nmovers; i++)
		if (pr.movers[i].e == pr.cur.groundent && pr.cur.groundent)
			gvz = pr.movers[i].vel[2];
	if (!pr.stair)
		pr.stair = true, pr.stairz = z;
	if (gvz != 0) {
		pr.stairz = z;
		return z;
	}
	if ((pr.cur.flags & PM_FL_ONGROUND) && z - pr.stairz > 0) {
		e = pr.stairz + ft * 1000 * 0.1f;
		e = BW_NDS_MIN(e, z);
		if (z - e > 16)
			e = z - 16;
	}
	if (fabsf(z - pr.stairz) > 64)
		e = z;
	pr.stairz = e;
	return e;
}

/* the predicted eye as the camera, and what the bob and roll read from it */
static void pred_view(ndscam_t *cam, int dt)
{
	const pmstate_t *s = &pr.cur;
	float z = stair_smooth(s->origin[2] + s->view_ofs[2], dt / 60.0f);
	cam->org[0] = lrintf((s->origin[0] + s->view_ofs[0]) * 8);
	cam->org[1] = lrintf((s->origin[1] + s->view_ofs[1]) * 8);
	cam->org[2] = lrintf(z * 8);
	memcpy(link.pvel, s->velocity, sizeof(link.pvel));
	link.onground = (s->flags & PM_FL_ONGROUND) != 0;
	link.crouch = (s->vflags & PM_VFL_CROUCHING) != 0;
}

/* what the weapon shows this frame: the view model is ours when the info runs the
 * state; a muzzle flash for each event its sequence passed since the last frame
 * (processmodelevents, as the PC's view model calls it each frame); the ammo */
static void weapon_frame(int frame)
{
	const wev_t *ev[4];
	int seq = wp.cur.animseq, n, clip = -1, res = -1;
	float t = wp.cur.animtime;
	bool was = wp.showing;
	wp.showing = wp.havesrv && !cfg.nopredict && Weapon_Usable(&wp.cur, &wh->info);
	if (wp.showing) {
		/* as the PC's View_DrawViewModel (src/client/viewmodel.qc:213-218) calls
		 * processmodelevents: from the last frame's time whatever sequence that was,
		 * and from 0 (frame 0's events with it) only when the time went back
		 * (qc_modelevents.c collect). A last time of exactly 0 already had frame 0 */
		float base = !was ? t : t < wp.showntime ? 0 : BW_NDS_MAX(wp.showntime, 1e-6f);
		n = Weapon_Events(&wh->info, seq, base, t, ev, 4);
		for (int i = 0; i < n; i++)
			if (ev[i]->code == 5001 || ev[i]->code == 5011 || ev[i]->code == 5021 || ev[i]->code == 5031) {
				wp.flashframe = frame;
				wp.flashscale = ev[i]->opt >= 10 ? BW_NDS_CLAMP((float)(ev[i]->opt / 10), 0.5f, 3.0f) : 1.0f;
			}
		wp.showntime = t;
		clip = wp.cur.clipsize > 0 ? wp.cur.clip : -1;
		res = Weapon_Ammo(&wp.cur, wh->info.primammo);
	} else if (wp.havesrv && wp.cur.edict) { /* no info: the server's numbers */
		clip = wp.cur.clipsize > 0 ? wp.cur.clip : -1;
		res = wp.cur.nammo ? wp.cur.ammo[0] : -1;
	}
	/* the HUD's numbers, as jtf.qc reads them: weapon.GetClip(), GetClipSize(), GetAmmo1() */
	hudin.haveweapon = wp.havesrv && wp.cur.edict;
	hudin.clip = wp.cur.clip, hudin.clipsize = wp.cur.clipsize;
	hudin.reserve = res < 0 ? 0 : res;
	(void)clip;
}

/* the punch (m_punchAngle) on the camera, as ncView::AddPunchAngle (src/client/View.qc)
 * adds it: QC pitch is down positive, ours up */
static void weapon_punch(ndscam_t *view)
{
	const float todeg = 32768.0f / 360.0f;
	if (!wp.havesrv)
		return;
	view->pitch -= (int)(wp.cur.punch[0] * todeg);
	view->yaw += (int)(wp.cur.punch[1] * todeg);
	view->roll += (int)(wp.cur.punch[2] * todeg);
}

/* the linked loop; returns why it ended */
/* the top screen's HUD, once a frame (hud_nds.c), from the camera drawn and the
 * entities where they were drawn */
static void hud_frame(const ndscam_t *view)
{
	int n = 0;
	for (int i = 0; i < link.numthings; i++) {
		const thing_t *th = &link.things[i];
		hudent_t *e = &hudents[n++];
		e->ent = th->ent;
		e->slot = (int8_t)(th->model >= 1000 && th->model < MAX_STUDIO ? th->model - 1000 : -1);
		memcpy(e->org, th->dorg, sizeof(e->org));
		e->half = th->half, e->height = th->height;
	}
	hudin.health = link.health, hudin.armor = link.armor, hudin.frags = link.frags;
	hudin.weapon = link.weapon;
	/* the buttons that do it here (key_buttons), as the PC names its bound keys */
	hudin.usekey = cfg.lefty ? "USE" : "X";
	hudin.firekey = cfg.lefty ? "R" : "A";
	hudin.diag1 = huddiag[0], hudin.diag2 = huddiag[1];
	hudin.srvms = eclock.now, hudin.srvclock = eclock.have;
	Hud_Frame(view, hudents, n, &hudin);
}

static const char *play(ndscam_t *cam)
{
	static char reason[96];
	uint8_t buf[1500];
	struct sockaddr_in from;
	ndsstats_t st = {0};
	pkt_t p;
	int secstart = Nds_Vblanks(), lastv = secstart, frames = 0, worst = 0, peak = 0, debugview = 0;
	ndscam_t view;
	screen_init();
	pad_reset();
	Events_Reset();
	pad_draw();
	link.lastrecv = Nds_Vblanks();
	pr.cmdvb = pr.botstart = Nds_Vblanks();
	pr.cmdms = vb_ms(pr.cmdvb);
	/* frame counts vblanks (60 a second) rather than loops, so turning,
	 * easing, timeouts and animations keep their speed when drawing takes
	 * two refreshes; dt is how many the last loop took */
	for (int frame = Nds_Vblanks(), lastframe = frame - 1, lastsend = frame;; lastframe = frame, frame = Nds_Vblanks()) {
		int len, type, buttons, fwd, side, made, dt = BW_NDS_CLAMP(frame - lastframe, 1, 4);
		uint32_t held;
		bool togame;
		touchPosition tp = {0};
		scanKeys();
		held = keysHeld();
		if (keysDown() & KEY_START) {
			snprintf(reason, sizeof(reason), "You left the game.");
			break;
		}
		/* SELECT: leave out the server's things, then its moved models too, to see what is what */
		if (keysDown() & KEY_SELECT)
			debugview = (debugview + 1) % 3;
		turn(cam, held, dt);
		buttons = pad_frame(frame, held, cam, &togame, &tp) | key_buttons(held);
		key_move(held, &fwd, &side);
		if (cfg.botwalk)
			bot_input(frame, &fwd, &side, &buttons, cam, dt);

		/* all that came, before the commands: the newest PSTATE says where they start from */
		while ((len = receive(buf, sizeof(buf), &from, &type)) > 0) {
			const uint8_t *d = buf + 5;
			if (from.sin_addr.s_addr != link.to.sin_addr.s_addr || len < 4 ||
			    (type != L_REFUSE && rd32(d) != link.session))
				continue;
			link.lastrecv = frame;
			if (type == L_WELCOME && len >= 7 && rd16(d + 5) != link.spawncount) {
				handle_welcome(d, len, cam);
				/* a map load takes seconds (17 s for the biggest Half-Life maps) inside
				 * this loop: that time is not silence from the server, and frame was
				 * read before it, so both move on to now (else a load over the 5 s
				 * below ends the link at the next loop without a packet) */
				frame = Nds_Vblanks();
				link.lastrecv = frame;
			}
			else if (type == L_SCREEN && len >= 6) {
				uint16_t version = (uint16_t)rd16(d + 4);
				if (version != link.screen_ack) {
					pad.screenlen = BW_NDS_MIN(len - 6, (int)sizeof(pad.screen));
					memcpy(pad.screen, d + 6, (size_t)pad.screenlen);
					if (pad.gamepage)
						pad_draw();
					link.screen_ack = version;
				}
			} else if (type == L_VIEW)
				handle_view(d, len, cam);
			else if (type == L_ENTS)
				handle_ents(d, len);
			else if (type == L_MODELS)
				handle_models(d, len);
			else if (type == L_STATS)
				handle_stats(d, len);
			else if (type == L_FX)
				handle_fx(d, len);
			else if (type == L_PSTATE)
				keep_pstate(d, len);
			else if (type == L_PMOVEVARS)
				handle_pmovevars(d, len);
			else if (type == L_BRUSHINFO)
				handle_brushinfo(d, len);
			else if (type == L_WEAPONINFO)
				handle_weaponinfo(d, len);
			else if (type == L_HUDINFO)
				Hud_Packet(d, len);
			else if (type == L_EVENT)
				Events_Packet(d, len, vb_ms(frame));
			else if (type == L_KICK) {
				char why[65];
				copy_str(why, sizeof(why), d + 4, len - 4);
				snprintf(reason, sizeof(reason), "The server dropped you: %s", why);
				goto done;
			}
		}
		if (frame - link.lastrecv > 5 * HZ) {
			snprintf(reason, sizeof(reason), "Lost the connection.");
			break;
		}

		/* the server's word on our movement, then this frame's commands, each
		 * predicted, and sent at most 30 times a second */
		{
			uint64_t t0 = LINK_PM_TICKS();
			uint32_t t;
			rp_deadline = tickGetCount() + (uint64_t)PR_REPLAY_MS * TICK_FREQ / 1000;
			handle_pstate(cam);
			pred_catchup();
			weapon_catchup();
			made = make_commands(frame, cam, fwd, side, buttons);
			t = (uint32_t)(LINK_PM_TICKS() - t0);
			Link_PredStats.frames++;
			Link_PredStats.fr_ticks += t;
			Link_PredStats.fr_ticks_max = BW_NDS_MAX(Link_PredStats.fr_ticks_max, t);
		}
		if (made && frame - lastsend >= 2) {
			send_input(cam, held, buttons, togame, &tp);
			lastsend = frame;
		}

		weapon_frame(frame);
		if (pr.on)
			pred_view(cam, dt);
		else {
			/* without prediction: ease toward where the server's last word
			 * says we are by now (moving on at the speed the last two VIEWs
			 * showed, for up to 3 refreshes), rather than where it was */
			int ahead = BW_NDS_CLAMP(frame - link.viewframe, 0, 3);
			for (int i = 0; i < dt; i++)
				for (int k = 0; k < 3; k++) {
					int32_t goal = link.target[k] + link.vel[k] * ahead, delta = goal - cam->org[k];
					cam->org[k] = delta > 2048 || delta < -2048 ? goal : cam->org[k] + delta / 2;
				}
		}
		linkframe = frame;
		step_clock();
		Ents_SetTime(vb_ms(frame));
		lerp_things();
		if (debugview == 2)
			World_ResetModels();
		else
			move_models(dt);
		view_motion(cam, &view, dt);
		weapon_punch(&view);
		Events_Frame(vb_ms(frame), link.health);
		Events_View(&view);
		if (World_Loaded()) {
			if (debugview == 0)
				add_studio_things(&view);
			else
				Ents_Clear();
			Ents_Draw(&view, &st);
		} else
			World_SetView(&view);
		PROF_T(pt);
		if (debugview == 0)
			draw_things();
		draw_markers(&view);
		draw_fx(&view);
		Events_Draw3D(&view);
		Events_Draw2D();
		PROF_LAP(PROF_REST, pt);
		{
			int polys, verts;
			Gpu_Counts(&polys, &verts);
			PROF_LAP(PROF_WAIT2, pt);
			peak = polys > peak ? polys : peak;
		}
		glFlush(0);
		swiWaitForVBlank();
		/* in the blank, while the GPU draws the next frame from what it was given */
		Events_Commit2D();
		{
			uint64_t t0 = LINK_PM_TICKS();
			uint32_t t;
			hud_frame(&view);
			t = (uint32_t)(LINK_PM_TICKS() - t0);
			Link_HudStats.frames++, Link_HudStats.ticks += t;
			Link_HudStats.ticks_max = BW_NDS_MAX(Link_HudStats.ticks_max, t);
		}
		if (cfg.fps30 && Nds_Vblanks() - frame < 2)
			swiWaitForVBlank(); /* fps=30: every frame takes two refreshes */
		/* how fast this runs, on the bottom row: frames a second, the slowest frame, peak polygons */
		{
			int now = Nds_Vblanks();
			worst = now - lastv > worst ? now - lastv : worst;
			lastv = now;
			frames++;
			if (now - secstart >= 60) {
				link.fps = frames * 60 / (now - secstart), link.minfps = 60 / (worst ? worst : 1);
				link.peakpolys = peak;
				int mok, mbad;
				Ents_Counts(&mok, &mbad);

				/* P: predicting */
				snprintf(pad.diagline, sizeof(pad.diagline), "%2d fps (min %d) %dp m%d/%d%s%s", link.fps, link.minfps, peak, mok,
				         mbad, pr.on ? " P" : "", debugview == 1 ? " -T" : debugview == 2 ? " -TM" : "");
				Link_PredStats.hullrule = (uint8_t)Hull_Rule(), Link_PredStats.on = pr.on;
				Link_PredStats.hullkb = (uint16_t)(Hull_Bytes() / 1024);
				snprintf(huddiag[0], sizeof(huddiag[0]), "%s", cfg.diag ? pad.diagline : "");
				snprintf(huddiag[1], sizeof(huddiag[1]), "%s", link.loaderr[0] ? link.loaderr : World_HullError());
				secstart = now, frames = 0, worst = 0, peak = 0;
			}
		}
	}
	p_begin(&p, L_LEAVE);
	p_u32(&p, link.session);
	for (int i = 0; i < 3; i++)
		send_to(&link.to, &p);
done:
	screen_done();
	return reason;
}

void Link_Run(const uint16_t *pal)
{
	ndscam_t cam = {{0}};
	int pick;
	palette = pal;
	read_config();
	consoleClear();
	if (!wifi_connect() || !open_socket()) {
		iprintf("\nA or START: back\n");
		wait_keys_or(1 << 30, KEY_A | KEY_START);
		return;
	}
	if ((pick = find_server()) < 0) {
		close_socket();
		return;
	}
	consoleClear();
	memset(&cam, 0, sizeof(cam));
	if (!join(&servers[pick], &cam)) {
		close_socket();
		iprintf("\nA or START: back\n");
		wait_keys_or(1 << 30, KEY_A | KEY_START);
		return;
	}
	for (int i = 0; i < MAX_STUDIO; i++)
		studiomdl[i] = -2;
	iprintf("%s\n", play(&cam));
	close_socket();
	reset_studio();
	World_Free();
	Nds_ResetTextures();
	iprintf("A or START: back\n");
	wait_keys_or(1 << 30, KEY_A | KEY_START);
}
