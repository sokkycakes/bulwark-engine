/* Bulwark engine - sequenced channel over unreliable packets.
 * ISC licence, see LICENSE.
 *
 * Design follows Source's model rather than QuakeWorld's: every packet acks
 * the peer's latest sequence plus a 32-bit history, so the layer above can
 * learn exactly which of its packets arrived or were lost (used for entity
 * delta recovery), and reliable data is a fragmented stream with a sliding
 * window instead of one message in flight. See docs/spec/protocol.md. */
#ifndef BW_NETCHAN_H
#define BW_NETCHAN_H

#include "msg.h"
#include "net.h"

#define NETCHAN_HISTORY 1024 /* outgoing packets remembered, power of two */
#define NETCHAN_WINDOW 256    /* reliable fragments in flight */
#define NETCHAN_FRAGSIZE 1024
#define NETCHAN_MAXLOST 256

typedef struct {
	uint32_t relseq;
	uint16_t len;
	double lastsent; /* 0 = never sent */
	uint8_t data[NETCHAN_FRAGSIZE];
} netfrag_t;

typedef struct {
	netsrc_t sock;
	netadr_t remote;
	int qport;
	bool is_client; /* writes qport in the header */

	/* outgoing */
	uint32_t outseq;           /* last sequence sent */
	double sendtime[NETCHAN_HISTORY];
	uint8_t outstate[NETCHAN_HISTORY]; /* 0 unknown, 1 acked, 2 lost */
	uint32_t lost[NETCHAN_MAXLOST];
	int numlost;
	uint32_t acked[NETCHAN_MAXLOST];
	int numacked;

	/* incoming */
	uint32_t inseq;   /* highest sequence received */
	uint32_t inbits;  /* received history below inseq */
	uint32_t peerack; /* highest of our packets the peer has seen */
	uint32_t dropped; /* incoming packets skipped over */

	/* reliable stream out */
	uint8_t *relout;
	size_t reloutlen, reloutcap;
	netfrag_t frags[NETCHAN_WINDOW];
	uint32_t relnext;   /* next fragment sequence to create */
	uint32_t relacked;  /* every fragment below this is acknowledged */

	/* reliable stream in */
	uint32_t relexpect;
	netfrag_t *pending[NETCHAN_WINDOW];
	uint8_t *relin;
	size_t relinlen, relincap;

	/* timing and stats */
	double last_received;
	float rtt; /* smoothed, seconds */
	int rate;  /* bytes per second, 0 = unlimited */
	double cleartime;
	float bytes_in_sec, bytes_out_sec;
	float loss; /* fraction of our packets lost, smoothed */
	size_t frame_in, frame_out;
	double stat_time;
} netchan_t;

void Netchan_Setup(netchan_t *c, netsrc_t sock, const netadr_t *remote, int qport, bool is_client);
void Netchan_Free(netchan_t *c);

/* Queues one reliable message; delivered whole, in order. */
void Netchan_WriteReliable(netchan_t *c, const void *data, size_t len);
bool Netchan_HasPendingReliable(const netchan_t *c);

/* Sends a packet carrying any due reliable fragments followed by 'data'.
 * Returns the sequence number used. */
uint32_t Netchan_Transmit(netchan_t *c, const void *data, size_t len);

/* Parses a received packet's header. Returns false for stale or duplicate
 * packets. On success msg->readpos points at the unreliable payload. */
bool Netchan_Process(netchan_t *c, msg_t *msg);

/* Pops the next complete reliable message, or returns false. 'out' is valid
 * until the next call. */
bool Netchan_NextReliable(netchan_t *c, msg_t *out);

/* Rate limiting: false when sending now would exceed c->rate. */
bool Netchan_CanSend(const netchan_t *c);

/* Peeks at the qport of a client packet without processing it. */
int Netchan_PeekQPort(const uint8_t *data, size_t len);

#endif
