/* Bulwark engine - Bulwark Live client: room codes, invites, matchmaking,
 * hole punching and relaying through a bwlive server (docs/spec/live.md).
 * ISC licence, see LICENSE. */
#ifndef BW_LIVE_H
#define BW_LIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "net.h"

/* virtual ports: one UDP socket carries several streams */
#define LIVE_VPORT_LOBBY 1
#define LIVE_VPORT_CLIENT 2
#define LIVE_VPORT_SERVER 3

void Live_Init(void);
void Live_Shutdown(void);
/* reads the live socket, keeps hello and punching going; once a frame */
void Live_Frame(void);

/* Tells Live whether the game is in a match's game right now (a server
 * running, or a client connected), so it can report leaving a match. */
void Live_SetGameProbe(bool (*probe)(void));

bool Live_Connected(void);
/* a Bulwark Live server is named (live_server); none is deployed by default */
bool Live_Configured(void);
uint32_t Live_MyId(void);
/* the party's room code, "" when not in a party */
const char *Live_PartyCode(void);
/* the party leader's player id, 0 when not in a party */
uint32_t Live_PartyLeader(void);

/* Game packets to and from "live:<id>" addresses; net.c calls these for
 * NA_LIVE. A client's packets go to the peer's server port, a server's to
 * the peer's client port. */
void Live_SendGame(netsrc_t src, const void *data, size_t len, const netadr_t *to);
size_t Live_GetGame(netsrc_t src, void *buf, size_t bufsize, netadr_t *from);

/* One seat of a finished set, for the fighter records (outcome: 0 win,
 * 1 loss, 2 quit, 3 disconnect). */
typedef struct {
	uint32_t id;
	int slot, outcome, games;
	bool behind;
	char hero[33];
} live_seat_report_t;

/* The match's host reports a finished set; false when this game isn't
 * the host of a Live match. */
bool Live_ReportSet(int result, int first_to, bool runback, const live_seat_report_t *seats, int n);
uint64_t Live_MatchId(void);
uint32_t Live_MatchHost(void);

/* the pre-game lobby's stream (virtual port 1) */
bool Live_SendLobby(uint32_t peer, const void *data, size_t len);
size_t Live_GetLobby(void *buf, size_t bufsize, uint32_t *from);

#endif
