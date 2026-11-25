/* Bulwark engine - the pre-game lobby: a host-authoritative, mapless party
 * that agrees on a map and rules, then hands everyone to the game server.
 * Behaviour in docs/spec/lobby.md. ISC licence, see LICENSE. */
#ifndef BW_LOBBY_H
#define BW_LOBBY_H

#include "../common/common.h"
#include "../qcvm/qcvm.h"

typedef enum {
	LOBBY_IDLE,
	LOBBY_CREATING,
	LOBBY_JOINING,
	LOBBY_WAITING,
	LOBBY_COUNTDOWN,
	LOBBY_STARTING,
	LOBBY_INGAME,
	LOBBY_CLOSING
} lobby_phase_t;

void Lobby_Init(void);
void Lobby_Shutdown(void);
void Lobby_Frame(void); /* once per client frame */
lobby_phase_t Lobby_Phase(void);
bool Lobby_Active(void); /* a session exists (any phase but IDLE) */
/* a host turned this guest away for its password: the lobby page asks for
 * it (joinpass:) and stays open meanwhile */
bool Lobby_AskingPassword(void);

/* the WebCore bridge requests: getlobby, getlobbychat, lobby_action:<a>,
 * hostcache_refresh, gethostcache; false for other requests */
bool Lobby_Query(const char *request, strbuf_t *reply);

/* MenuQC: lobby_registermode, lobby_addteam */
extern const qc_builtin_t lobby_builtins[];

#endif
