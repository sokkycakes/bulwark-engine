/* Bulwark engine - wire protocol constants. ISC licence, see LICENSE.
 * The layout of every message is described in docs/spec/protocol.md. */
#ifndef BW_PROTOCOL_H
#define BW_PROTOCOL_H

#define PROTOCOL_VERSION 1
#define PROTOCOL_NAME "BULWARK"
#define PORT_SERVER 27500

/* server to client */
enum {
	svc_bad = 0,
	svc_nop,
	svc_disconnect,   /* string reason */
	svc_print,        /* byte level, string */
	svc_centerprint,  /* string */
	svc_stufftext,    /* string */
	svc_serverdata,   /* see protocol.md */
	svc_precache,     /* byte kind, varuint index, string name */
	svc_lightstyle,   /* byte index, string */
	svc_serverinfo,   /* string key, string value */
	svc_playerinfo,   /* byte slot, string key, string value */
	svc_signon,       /* byte stage */
	svc_snapshot,     /* see protocol.md */
	svc_qcmessage,    /* varuint length, bytes written by QC to MSG_ONE/ALL/MULTICAST */
	svc_setpause,     /* byte paused */
	svc_sound,        /* see protocol.md */
	svc_setangle,     /* angle16 x3 */
	svc_download,     /* reserved */
	svc_count
};

/* client to server */
enum {
	clc_bad = 0,
	clc_nop,
	clc_disconnect,
	clc_move,      /* see protocol.md */
	clc_stringcmd, /* string */
	clc_event,     /* string function, then typed arguments */
	clc_count
};

/* svc_precache kinds */
enum { PRECACHE_MODEL = 0, PRECACHE_SOUND = 1, PRECACHE_PARTICLE = 2 };

/* first byte of a QC message that the client hands to CSQC_Parse_Event */
#define SVC_CGAMEPACKET 83

/* usercmd field bits in clc_move */
#define UCMD_ANGLES 1
#define UCMD_MOVE 2
#define UCMD_BUTTONS 4
#define UCMD_IMPULSE 8
#define UCMD_WEAPON 16
#define UCMD_CURSOR 32

#define MAX_BACKUP_CMDS 8 /* earlier commands repeated in each clc_move */

typedef struct {
	uint32_t sequence;   /* client input frame number */
	uint32_t servertime_ms; /* server time the client was viewing (lag compensation) */
	float msec;          /* time this command covers, seconds */
	float angles[3];
	float movevalues[3];
	uint32_t buttons;
	uint8_t impulse;
	uint32_t weapon;
	float cursor_screen[2];
	float cursor_start[3], cursor_end[3];
	int cursor_entity;
	double clienttime;
} usercmd_t;

#endif
