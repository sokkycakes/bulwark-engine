/* Bulwark engine - starting a dedicated server on this machine from the
 * client (the lobby's "Dedicated server" type). Behaviour in
 * docs/spec/lobby.md, "Dedicated hosting". ISC licence, see LICENSE. */
#ifndef BW_DEDLAUNCH_H
#define BW_DEDLAUNCH_H

#include "../common/common.h"

/* Starts the dedicated server program in the background with this game's
 * directories and the given console commands (one per line, run after its
 * own start-up), listening on a free UDP port. Returns that port, or 0 with
 * the reason in err. A server this client started before is left running. */
int DedLaunch_Start(const char *commands, char *err, size_t errsize);

/* this platform can start programs (false in browsers and on consoles) */
bool DedLaunch_Supported(void);

/* the process this client started last is still running */
bool DedLaunch_Running(void);
int DedLaunch_Port(void);

/* stops the server this client started (SIGTERM), if it still runs */
void DedLaunch_Stop(void);

void DedLaunch_Init(void);

#endif
