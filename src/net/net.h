/* Bulwark engine - packet transport (UDP and in-process loopback).
 * ISC licence, see LICENSE. */
#ifndef BW_NET_H
#define BW_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_MAX_PACKET 1400  /* payload size we never exceed, below typical MTU */
#define NET_MAX_RECV 65536

typedef enum { NA_NONE, NA_LOOPBACK, NA_IP4, NA_IP6 } netadrtype_t;

typedef struct {
	netadrtype_t type;
	uint8_t ip[16];
	uint16_t port; /* host order */
} netadr_t;

/* Which end of the loopback pair a socket is. */
typedef enum { NS_CLIENT, NS_SERVER } netsrc_t;

void NET_Init(void);
void NET_Shutdown(void);

/* Opens (or reopens) the server's UDP socket on 'port'. 0 picks any port.
 * Returns false if binding failed; loopback still works. */
bool NET_OpenServer(int port);
bool NET_OpenClient(void);
void NET_CloseServer(void);
int NET_ServerPort(void);

bool NET_StringToAdr(const char *s, netadr_t *a, int defport);
const char *NET_AdrToString(const netadr_t *a);
bool NET_CompareAdr(const netadr_t *a, const netadr_t *b);
bool NET_CompareBaseAdr(const netadr_t *a, const netadr_t *b); /* ignores the port */

void NET_SendPacket(netsrc_t src, const void *data, size_t len, const netadr_t *to);
/* Returns the length of the next queued packet, or 0 when there is none. */
size_t NET_GetPacket(netsrc_t src, void *buf, size_t bufsize, netadr_t *from);

/* Simulated network conditions, applied to outgoing packets (like Source's
 * net_fakelag / net_fakeloss). */
void NET_SetFakeConditions(float lag_ms, float loss_percent);
void NET_RunDelayed(void);

/* Sends "\xff\xff\xff\xff" followed by text. */
void NET_OutOfBandPrint(netsrc_t src, const netadr_t *to, const char *fmt, ...);

#endif
