/* Bulwark engine - voice chat on the server: each clc_voice from a player
 * goes out as svc_voice in the unreliable part of everyone else's next
 * packet, except to players who asked not to hear that one ("vignore").
 * The server never decodes the audio. ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../net/protocol.h"

#include <stdlib.h>
#include <string.h>

static cvar_t *sv_voip, *sv_voip_echo, *sv_voip_record;

static bool ignores(const client_t *listener, int speaker)
{
	return speaker < 256 && (listener->vignore[speaker >> 5] >> (speaker & 31)) & 1;
}

/* a clc_voice message: forwarded as it came, behind the sender's slot */
void SV_ParseVoice(client_t *c, msg_t *m)
{
	size_t start = m->readpos;
	MSG_ReadByte(m); /* codec */
	MSG_ReadByte(m); /* generation */
	MSG_ReadVarUInt(m);
	int frames = MSG_ReadByte(m);
	for (int i = 0; i < frames && !m->badread; i++) {
		int len = MSG_ReadShort(m) & 0xffff;
		if (m->readpos + (size_t)len > m->cursize) {
			m->badread = true;
			return;
		}
		m->readpos += (size_t)len;
	}
	if (m->badread || !sv_voip->ivalue || c->state != CS_SPAWNED)
		return;
	if (c->silenced_until > Sys_Seconds() || (SV_Penalties(c) & PEN_MUTE))
		return; /* mutes apply to the voice too */
	int slot = (int)(c - svs.clients);
	uint8_t head[2] = {svc_voice, (uint8_t)slot};
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *to = &svs.clients[i];
		if (to->state != CS_SPAWNED || to->bot || to->parent || ignores(to, slot))
			continue;
		if (to == c && !sv_voip_echo->ivalue)
			continue;
		/* sv_voip_record off keeps voices out of server-side recordings */
		strbuf_t *b = sv_voip_record->ivalue ? &to->datagram : &to->voicegram;
		if (b->len > 4096)
			continue; /* already plenty for one packet */
		SB_Append(b, (const char *)head, 2);
		SB_Append(b, (const char *)m->data + start, m->readpos - start);
	}
}

/* "vignore <slot> <0|1>" from a client; false for other commands */
bool SV_VoipCommand(client_t *c, int argc, const char **argv)
{
	if (strcmp(argv[0], "vignore"))
		return false;
	if (argc < 3)
		return true;
	int slot = atoi(argv[1]);
	if (slot < 0 || slot >= 256)
		return true;
	if (atoi(argv[2]))
		c->vignore[slot >> 5] |= 1u << (slot & 31);
	else
		c->vignore[slot >> 5] &= ~(1u << (slot & 31));
	return true;
}

void SV_InitVoip(void)
{
	sv_voip = Cvar_Get("sv_voip", "1", 0, "Pass players' voice chat on to the others");
	sv_voip_echo = Cvar_Get("sv_voip_echo", "0", 0, "Send players their own voice back, to hear the network's latency and loss");
	sv_voip_record = Cvar_Get("sv_voip_record", "0", 0, "Keep voice chat in server-side demo recordings");
}
