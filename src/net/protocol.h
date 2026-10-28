/* Bulwark engine - wire protocol constants. ISC licence, see LICENSE.
 * The layout of every message is described in docs/spec/protocol.md. */
#ifndef BW_PROTOCOL_H
#define BW_PROTOCOL_H

#define PROTOCOL_VERSION 2
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
	svc_download,     /* byte op (DL_), then by op: see below */
	svc_pointparticles, /* varuint effect, coord x3 origin, float x3 direction, float count */
	svc_trailparticles, /* varuint effect, entity, coord x3 start, coord x3 end */
	svc_playerblob,   /* byte slot, string key, varuint length, bytes */
	svc_stats,        /* varuint count, then per stat: byte index, byte type (EV_FLOAT 2, EV_STRING 1, EV_INTEGER 8), value */
	svc_voice,        /* byte sender slot, then a clc_voice body */
	svc_voxel,        /* byte subcommand (VXN_*), see docs/spec/voxel.md */
	svc_flash,        /* coord x3 origin, float radius, float lifetime, float x3 colour (te_customflash) */
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
	clc_userblob,  /* string key, varuint length, bytes */
	clc_upload,    /* byte op (UL_), then by op: see below */
	clc_voice,     /* byte codec (VOICE_), byte generation, varuint sequence, byte frames, per frame short length + bytes */
	clc_count
};

/* svc_voxel subcommands */
enum {
	VXN_INIT,  /* varuint size x3, float blocksize, float origin x3: a new empty world */
	VXN_FREE,  /* no voxel world any more */
	VXN_DEF,   /* varuint type, byte shape, long contents, byte rgba x4, byte flags, string name, string texture */
	VXN_CHUNK, /* varuint cx, cy, cz, varuint length, run-length bytes (Vox_EncodeChunk) */
	VXN_SET    /* varuint x, y, z, short type, byte rot */
};
/* svc_download ops: START string name, long size; DATA long offset, short
 * length, bytes; DONE string sha256 hex; REFUSED string name, string why */
enum { DL_START, DL_DATA, DL_DONE, DL_REFUSED };
/* clc_upload ops: START string name, long size; DATA long offset, short
 * length, bytes; WINDOW (waiting for "nextul"); DONE */
enum { UL_START, UL_DATA, UL_WINDOW, UL_DONE };

/* voice codecs, numbered as FTE's cl_voip_codec */
enum { VOICE_SPEEX11 = 0, VOICE_RAW, VOICE_OPUS, VOICE_SPEEX8, VOICE_SPEEX16, VOICE_SPEEX32, VOICE_PCMA, VOICE_PCMU };

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
