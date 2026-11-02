/* Bulwark engine - sequenced channel over unreliable packets.
 * ISC licence, see LICENSE. */
#include <string.h>

#include "../common/common.h"
#include "netchan.h"

/* header: seq u32, ack u32, ackbits u32, relack u32, [qport u16], flags u8 */
#define FLAG_RELIABLE 1

void Netchan_Setup(netchan_t *c, netsrc_t sock, const netadr_t *remote, int qport, bool is_client)
{
	memset(c, 0, sizeof(*c));
	c->sock = sock;
	c->remote = *remote;
	c->qport = qport;
	c->is_client = is_client;
	c->last_received = host_time;
	c->stat_time = c->last_received;
	c->rtt = 0.1f;
}

void Netchan_Free(netchan_t *c)
{
	Mem_Free(c->relout);
	Mem_Free(c->relin);
	for (int i = 0; i < NETCHAN_WINDOW; i++)
		Mem_Free(c->pending[i]);
	memset(c, 0, sizeof(*c));
}

void Netchan_WriteReliable(netchan_t *c, const void *data, size_t len)
{
	size_t need = c->reloutlen + len + 4;
	if (need > c->reloutcap) {
		c->reloutcap = need * 2 + 1024;
		c->relout = Mem_Realloc(c->relout, c->reloutcap);
	}
	/* messages are framed in the stream by a 32-bit little endian length */
	for (int i = 0; i < 4; i++)
		c->relout[c->reloutlen++] = (uint8_t)(len >> (8 * i));
	memcpy(c->relout + c->reloutlen, data, len);
	c->reloutlen += len;
}

bool Netchan_HasPendingReliable(const netchan_t *c) { return c->reloutlen || c->relnext != c->relacked; }

size_t Netchan_ReliableBytesDue(const netchan_t *c)
{
	double resend = c->rtt * 1.5 + 0.03;
	size_t due = 0;
	for (uint32_t s = c->relacked; s != c->relnext; s++) {
		const netfrag_t *f = &c->frags[s % NETCHAN_WINDOW];
		if (!f->lastsent || host_time - f->lastsent >= resend)
			due += 6 + f->len;
	}
	/* stream bytes not cut into fragments yet go out as new ones */
	due += c->reloutlen + 6 * ((c->reloutlen + NETCHAN_FRAGSIZE - 1) / NETCHAN_FRAGSIZE);
	if (due)
		due += 2; /* the reliable flag's count byte, and slack */
	return BW_MIN(due, (size_t)NETCHAN_FRAGSIZE + 8);
}

/* moves stream bytes into fragments while the window has room */
static void make_fragments(netchan_t *c)
{
	size_t used = 0;
	while (used < c->reloutlen && c->relnext - c->relacked < NETCHAN_WINDOW) {
		netfrag_t *f = &c->frags[c->relnext % NETCHAN_WINDOW];
		size_t n = c->reloutlen - used;
		if (n > NETCHAN_FRAGSIZE)
			n = NETCHAN_FRAGSIZE;
		f->relseq = c->relnext++;
		f->len = (uint16_t)n;
		f->lastsent = 0;
		memcpy(f->data, c->relout + used, n);
		used += n;
	}
	if (used) {
		memmove(c->relout, c->relout + used, c->reloutlen - used);
		c->reloutlen -= used;
	}
}

bool Netchan_CanSend(const netchan_t *c) { return c->rate <= 0 || c->cleartime <= host_time; }

uint32_t Netchan_Transmit(netchan_t *c, const void *data, size_t len)
{
	uint8_t buf[NET_MAX_PACKET + 64];
	msg_t m;
	double now = host_time;
	double resend = c->rtt * 1.5 + 0.03;
	size_t flagpos;
	int nfrags = 0;
	size_t countpos = 0;

	MSG_Init(&m, buf, sizeof(buf));
	c->outseq++;
	MSG_WriteLong(&m, (int32_t)c->outseq);
	MSG_WriteLong(&m, (int32_t)c->inseq);
	MSG_WriteLong(&m, (int32_t)c->inbits);
	MSG_WriteLong(&m, (int32_t)c->relexpect);
	if (c->is_client)
		MSG_WriteShort(&m, c->qport);
	flagpos = m.cursize;
	MSG_WriteByte(&m, 0);

	make_fragments(c);
	for (uint32_t s = c->relacked; s != c->relnext; s++) {
		netfrag_t *f = &c->frags[s % NETCHAN_WINDOW];
		if (f->lastsent && now - f->lastsent < resend)
			continue;
		if (m.cursize + 6 + f->len + len > NET_MAX_PACKET)
			break;
		if (!nfrags) {
			m.data[flagpos] |= FLAG_RELIABLE;
			countpos = m.cursize;
			MSG_WriteByte(&m, 0);
		}
		MSG_WriteLong(&m, (int32_t)f->relseq);
		MSG_WriteShort(&m, f->len);
		MSG_WriteData(&m, f->data, f->len);
		f->lastsent = now;
		if (++nfrags == 255)
			break;
	}
	if (nfrags)
		m.data[countpos] = (uint8_t)nfrags;

	if (m.cursize + len <= sizeof(buf))
		MSG_WriteData(&m, data, len);
	else
		Con_DPrintf("netchan: dropped %zu bytes of unreliable data to %s\n", len, NET_AdrToString(&c->remote));

	c->sendtime[c->outseq % NETCHAN_HISTORY] = now;
	c->outstate[c->outseq % NETCHAN_HISTORY] = 0;
	NET_SendPacket(c->sock, m.data, m.cursize, &c->remote);
	c->frame_out += m.cursize + 28;
	if (c->rate > 0) {
		if (c->cleartime < now)
			c->cleartime = now;
		c->cleartime += (m.cursize + 28) / (double)c->rate;
	}
	return c->outseq;
}

int Netchan_PeekQPort(const uint8_t *data, size_t len) { return len >= 18 ? data[16] | data[17] << 8 : -1; }

static void note_ack(netchan_t *c, uint32_t s, uint8_t state)
{
	uint8_t *st = &c->outstate[s % NETCHAN_HISTORY];
	if (*st)
		return;
	*st = state;
	if (state == 1) {
		double sent = c->sendtime[s % NETCHAN_HISTORY];
		if (sent > 0) {
			float sample = (float)(host_time - sent);
			c->rtt = c->rtt * 0.9f + sample * 0.1f;
		}
		c->loss *= 0.98f;
		if (c->numacked < NETCHAN_MAXLOST)
			c->acked[c->numacked++] = s;
	} else {
		c->loss = c->loss * 0.98f + 0.02f;
		if (c->numlost < NETCHAN_MAXLOST)
			c->lost[c->numlost++] = s;
	}
}

static void store_reliable(netchan_t *c, const uint8_t *data, size_t len)
{
	if (c->relinlen + len > c->relincap) {
		c->relincap = (c->relinlen + len) * 2 + 4096;
		c->relin = Mem_Realloc(c->relin, c->relincap);
	}
	memcpy(c->relin + c->relinlen, data, len);
	c->relinlen += len;
}

bool Netchan_Process(netchan_t *c, msg_t *msg)
{
	uint32_t seq, ack, ackbits, relack;
	int flags;
	double now = host_time;

	MSG_BeginReading(msg);
	seq = (uint32_t)MSG_ReadLong(msg);
	ack = (uint32_t)MSG_ReadLong(msg);
	ackbits = (uint32_t)MSG_ReadLong(msg);
	relack = (uint32_t)MSG_ReadLong(msg);
	if (!c->is_client)
		MSG_ReadShort(msg); /* qport, already matched by the caller */
	flags = MSG_ReadByte(msg);
	if (msg->badread)
		return false;

	/* stale or duplicate */
	if (seq <= c->inseq && c->inseq) {
		uint32_t back = c->inseq - seq;
		if (back == 0 || back > 32 || (c->inbits & (1u << (back - 1))))
			return false;
		c->inbits |= 1u << (back - 1); /* late but new: accept it */
	} else {
		uint32_t gap = seq - c->inseq;
		if (c->inseq && gap > 1)
			c->dropped += gap - 1;
		c->inbits = gap >= 32 ? 0 : (c->inbits << gap) | (c->inseq ? 1u << (gap - 1) : 0);
		c->inseq = seq;
	}

	/* what the peer says it has from us */
	if (ack > c->outseq)
		return false;
	if (ack > c->peerack || !c->peerack) {
		note_ack(c, ack, 1);
		for (int i = 0; i < 32; i++)
			if ((ackbits & (1u << i)) && ack > (uint32_t)i + 1)
				note_ack(c, ack - 1 - i, 1);
		/* anything older than the history window that never got acked */
		for (uint32_t s = c->peerack + 1; s + 32 < ack; s++)
			note_ack(c, s, 2);
		for (int i = 0; i < 32; i++) {
			uint32_t s = ack - 1 - (uint32_t)i;
			if (s <= c->peerack || s == 0 || s > ack)
				break;
			if (!(ackbits & (1u << i)) && i >= 2) /* allow a little reordering */
				note_ack(c, s, 2);
		}
		c->peerack = ack;
	}
	if (relack > c->relacked && relack <= c->relnext)
		c->relacked = relack;

	if (flags & FLAG_RELIABLE) {
		int n = MSG_ReadByte(msg);
		for (int i = 0; i < n; i++) {
			uint32_t rs = (uint32_t)MSG_ReadLong(msg);
			int len = MSG_ReadUShort(msg);
			const uint8_t *data;
			if (msg->badread || len > NETCHAN_FRAGSIZE || MSG_Remaining(msg) < (size_t)len)
				return false;
			data = msg->data + msg->readpos;
			msg->readpos += (size_t)len;
			if (rs < c->relexpect || rs - c->relexpect >= NETCHAN_WINDOW)
				continue;
			if (rs == c->relexpect) {
				store_reliable(c, data, (size_t)len);
				c->relexpect++;
				/* drain fragments that arrived early */
				for (;;) {
					netfrag_t **slot = &c->pending[c->relexpect % NETCHAN_WINDOW];
					if (!*slot || (*slot)->relseq != c->relexpect)
						break;
					store_reliable(c, (*slot)->data, (*slot)->len);
					Mem_Free(*slot);
					*slot = NULL;
					c->relexpect++;
				}
			} else {
				netfrag_t **slot = &c->pending[rs % NETCHAN_WINDOW];
				if (!*slot)
					*slot = Mem_Alloc(sizeof(netfrag_t));
				(*slot)->relseq = rs;
				(*slot)->len = (uint16_t)len;
				memcpy((*slot)->data, data, (size_t)len);
			}
		}
	}

	c->last_received = now;
	c->frame_in += msg->cursize + 28;
	if (now - c->stat_time >= 1.0) {
		float dt = (float)(now - c->stat_time);
		c->bytes_in_sec = c->frame_in / dt;
		c->bytes_out_sec = c->frame_out / dt;
		c->frame_in = c->frame_out = 0;
		c->stat_time = now;
	}
	return true;
}

bool Netchan_NextReliable(netchan_t *c, msg_t *out)
{
	static uint8_t *buf;
	static size_t bufsize;
	size_t len;
	if (c->relinlen < 4)
		return false;
	len = c->relin[0] | c->relin[1] << 8 | c->relin[2] << 16 | (size_t)c->relin[3] << 24;
	if (c->relinlen < 4 + len)
		return false;
	if (len > bufsize) {
		bufsize = len * 2;
		buf = Mem_Realloc(buf, bufsize);
	}
	memcpy(buf, c->relin + 4, len);
	memmove(c->relin, c->relin + 4 + len, c->relinlen - 4 - len);
	c->relinlen -= 4 + len;
	MSG_Init(out, buf, len);
	out->cursize = len;
	return true;
}
