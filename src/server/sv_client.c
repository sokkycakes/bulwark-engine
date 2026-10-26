/* Bulwark engine - server side of the network: client slots, connection
 * handshake, signon, usercmds, CSQC entity snapshots and the QC builtins
 * that talk to clients. ISC licence, see LICENSE.
 *
 * Entity updates follow the CSQC SendEntity model: each entity accumulates
 * changed bits per client and its SendEntity writes them. Loss recovery
 * borrows from Source: every packet's contents are remembered until the
 * client's ack bits say it arrived or was lost, and lost bits are re-queued.
 * An entity's updates always carry every bit until its creation is acked,
 * so the client never builds an entity from a partial update. */
#include "sv_local.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../common/digest.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define F sv.vm->fofs
#define G sv.vm->gofs

serverstatic_t svs;

static cvar_t *sv_maxclients, *sv_timeout, *sv_unlag, *sv_maxunlag, *sv_minupdaterate, *sv_maxupdaterate;
static cvar_t *sv_port, *hostname, *sv_maxrate;

/* the buffers QC's Write* builtins target */
static strbuf_t msg_entity_buf; /* MSG_ENTITY, inside SendEntity */
static strbuf_t msg_multicast;  /* MSG_MULTICAST, until multicast() */
static strbuf_t msg_broadcast;  /* MSG_BROADCAST, unreliable to everyone this frame */
static strbuf_t msg_init;       /* MSG_INIT, sent to every client at signon */

/* input_* globals, looked up once per map */
static struct {
	int sequence, timelength, angles, movevalues, buttons, impulse, servertime, clienttime, weapon;
	int cursor_screen, cursor_start, cursor_end, cursor_ent, lightlevel;
	int servercommandframe;
} IG;

/* --- helpers ----------------------------------------------------------- */

client_t *SV_ClientForEdict(int e)
{
	if (e < 1 || e > svs.maxclients)
		return NULL;
	return &svs.clients[e - 1];
}

static int client_slot(const client_t *c) { return (int)(c - svs.clients); }

static void sb_write(strbuf_t *sb, const msg_t *m) { SB_Append(sb, (const char *)m->data, m->cursize); }

/* flushes QC-written reliable bytes ahead of any engine reliable message */
static void flush_qcrel(client_t *c)
{
	uint8_t hdr[8];
	msg_t m;
	if (!c->qcrel.len || c->bot)
		goto done;
	MSG_Init(&m, hdr, sizeof(hdr));
	MSG_WriteByte(&m, svc_qcmessage);
	MSG_WriteVarUInt(&m, (uint32_t)c->qcrel.len);
	{
		uint8_t *tmp = Mem_Alloc(m.cursize + c->qcrel.len);
		memcpy(tmp, m.data, m.cursize);
		memcpy(tmp + m.cursize, c->qcrel.data, c->qcrel.len);
		Netchan_WriteReliable(&c->chan, tmp, m.cursize + c->qcrel.len);
		Mem_Free(tmp);
	}
done:
	c->qcrel.len = 0;
}

static void client_reliable(client_t *c, const msg_t *m)
{
	if (c->state == CS_FREE || c->bot)
		return;
	flush_qcrel(c);
	Netchan_WriteReliable(&c->chan, m->data, m->cursize);
}

static void broadcast_reliable(const msg_t *m, bool spawned_only)
{
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state == CS_FREE || (spawned_only && c->state != CS_SPAWNED))
			continue;
		client_reliable(c, m);
	}
}

/* a message with a small scratch buffer */
#define LOCALMSG(name, size)                                                                                           \
	uint8_t name##_buf[size];                                                                                          \
	msg_t name;                                                                                                        \
	MSG_Init(&name, name##_buf, sizeof(name##_buf))

static void client_print(client_t *c, int level, const char *text)
{
	LOCALMSG(m, 4096);
	MSG_WriteByte(&m, svc_print);
	MSG_WriteByte(&m, level);
	MSG_WriteString(&m, text);
	if (c->bot)
		return;
	client_reliable(c, &m);
}

void SV_ClientPrint(client_t *c, int level, const char *text) { client_print(c, level, text); }

void SV_StuffText(client_t *c, const char *text)
{
	LOCALMSG(m, 4200);
	if (!c || c->state == CS_FREE || c->bot)
		return;
	MSG_WriteByte(&m, svc_stufftext);
	MSG_WriteString(&m, text);
	client_reliable(c, &m);
}

void SV_BroadcastPrint(int level, const char *text)
{
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].state == CS_SPAWNED)
			client_print(&svs.clients[i], level, text);
	Con_Print(text);
}

void SV_SetPaused(bool paused)
{
	LOCALMSG(m, 4);
	sv.paused = paused;
	MSG_WriteByte(&m, svc_setpause);
	MSG_WriteByte(&m, sv.paused);
	broadcast_reliable(&m, true);
}

void SV_BroadcastPrecache(int kind, int index, const char *name)
{
	LOCALMSG(m, 512);
	MSG_WriteByte(&m, svc_precache);
	MSG_WriteByte(&m, kind);
	MSG_WriteVarUInt(&m, (uint32_t)index);
	MSG_WriteString(&m, name);
	broadcast_reliable(&m, false);
}

void SV_BroadcastLightstyle(int index, const char *style)
{
	LOCALMSG(m, 512);
	MSG_WriteByte(&m, svc_lightstyle);
	MSG_WriteByte(&m, index);
	MSG_WriteString(&m, style);
	broadcast_reliable(&m, false);
}

void SV_BroadcastServerinfo(const char *key, const char *value)
{
	LOCALMSG(m, 2048);
	MSG_WriteByte(&m, svc_serverinfo);
	MSG_WriteString(&m, key);
	MSG_WriteString(&m, value);
	broadcast_reliable(&m, false);
}

static void broadcast_playerinfo(client_t *c, const char *key, const char *value)
{
	LOCALMSG(m, 4096);
	MSG_WriteByte(&m, svc_playerinfo);
	MSG_WriteByte(&m, client_slot(c));
	MSG_WriteString(&m, key);
	MSG_WriteString(&m, value);
	broadcast_reliable(&m, false);
}

static void set_playerkey(client_t *c, const char *key, const char *value, bool broadcast)
{
	char old[1024];
	SV_InfoGet(c->userinfo, key, old, sizeof(old));
	if (!strcmp(old, value))
		return;
	SV_InfoSet(c->userinfo, sizeof(c->userinfo), key, value);
	if (!strcmp(key, "name"))
		Str_Copy(c->name, value, sizeof(c->name));
	if (broadcast)
		broadcast_playerinfo(c, key, value);
}

/* --- entity send state ------------------------------------------------- */

static void reset_entity_state(client_t *c)
{
	int n = sv.vm ? sv.vm->max_edicts : 0;
	if (c->estate_count != n) {
		Mem_Free(c->pending);
		Mem_Free(c->estate);
		c->pending = n ? Mem_Alloc(sizeof(uint32_t) * (size_t)n) : NULL;
		c->estate = n ? Mem_Alloc((size_t)n) : NULL;
		c->estate_count = n;
	} else if (n) {
		memset(c->pending, 0, sizeof(uint32_t) * (size_t)n);
		memset(c->estate, 0, (size_t)n);
	}
	for (int i = 0; i < SNAP_HISTORY; i++)
		c->snaps[i].num = 0;
	c->ent_rotor = 0;
}

static void free_client_state(client_t *c)
{
	Mem_Free(c->pending);
	Mem_Free(c->estate);
	c->pending = NULL;
	c->estate = NULL;
	c->estate_count = 0;
	for (int i = 0; i < SNAP_HISTORY; i++) {
		Mem_Free(c->snaps[i].recs);
		c->snaps[i].recs = NULL;
		c->snaps[i].num = c->snaps[i].cap = 0;
	}
	SB_Free(&c->qcrel);
	SB_Free(&c->datagram);
	Blob_Clear(c->blobs);
}

static void write_blob(msg_t *m, int slot, const infoblob_t *b)
{
	MSG_WriteByte(m, svc_playerblob);
	MSG_WriteByte(m, slot);
	MSG_WriteString(m, b->key);
	MSG_WriteVarUInt(m, (uint32_t)b->len);
	MSG_WriteData(m, b->data, (size_t)b->len);
}

/* a client's binary info key; keys starting with _ stay on the server */
static void parse_userblob(client_t *c, msg_t *m)
{
	char key[64];
	uint32_t len;
	const infoblob_t *b;
	msg_t out;
	Str_Copy(key, MSG_ReadString(m), sizeof(key));
	len = MSG_ReadVarUInt(m);
	if (len > MAX_INFOBLOB_SIZE || m->readpos + len > m->cursize) {
		m->badread = true;
		return;
	}
	Blob_Set(c->blobs, key, m->data + m->readpos, (int)len);
	m->readpos += len;
	if (!key[0] || key[0] == '_' || !(b = Blob_Get(c->blobs, key)))
		return;
	MSG_Init(&out, Mem_Alloc(len + 128), len + 128);
	write_blob(&out, client_slot(c), b);
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].state >= CS_CONNECTED && !svs.clients[i].bot)
			client_reliable(&svs.clients[i], &out);
	Mem_Free(out.data);
}

static void snap_add(snaprec_t *s, uint32_t ent, uint32_t flags, bool remove, bool create)
{
	if (s->num == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 64;
		s->recs = Mem_Realloc(s->recs, sizeof(sentrec_t) * (size_t)s->cap);
	}
	s->recs[s->num].ent = ent;
	s->recs[s->num].flags = flags;
	s->recs[s->num].remove = remove;
	s->recs[s->num].create = create;
	s->num++;
}

/* what the client's acks say about our earlier packets */
static void process_acks(client_t *c)
{
	for (int i = 0; i < c->chan.numacked; i++) {
		snaprec_t *s = &c->snaps[c->chan.acked[i] % SNAP_HISTORY];
		if (s->seq != c->chan.acked[i])
			continue;
		for (int r = 0; r < s->num; r++) {
			sentrec_t *rec = &s->recs[r];
			if (rec->ent >= (uint32_t)c->estate_count)
				continue;
			if (rec->create && (c->estate[rec->ent] & ES_KNOWN))
				c->estate[rec->ent] |= ES_CONFIRMED;
		}
		s->num = 0;
	}
	for (int i = 0; i < c->chan.numlost; i++) {
		snaprec_t *s = &c->snaps[c->chan.lost[i] % SNAP_HISTORY];
		if (s->seq != c->chan.lost[i])
			continue;
		for (int r = 0; r < s->num; r++) {
			sentrec_t *rec = &s->recs[r];
			uint8_t *st;
			if (rec->ent >= (uint32_t)c->estate_count)
				continue;
			st = &c->estate[rec->ent];
			if (rec->remove) {
				if (!(*st & ES_KNOWN))
					*st |= ES_NEEDREMOVE;
			} else if (*st & ES_KNOWN)
				c->pending[rec->ent] |= rec->flags;
		}
		s->num = 0;
	}
	c->chan.numacked = c->chan.numlost = 0;
}

/* moves every entity's SendFlags into each client's pending bits */
static void collect_sendflags(void)
{
	qcvm_t *vm = sv.vm;
	if (F.sendflags < 0)
		return;
	for (int e = 0; e < vm->num_edicts; e++) {
		uint32_t fl;
		if (vm->edicts[e].free)
			continue;
		fl = (uint32_t)QC_EFloat(vm, e, F.sendflags);
		if (!fl)
			continue;
		for (int i = 0; i < svs.maxclients; i++) {
			client_t *c = &svs.clients[i];
			if (c->state == CS_SPAWNED && !c->bot && e < c->estate_count)
				c->pending[e] |= fl;
		}
		QC_SetEFloat(vm, e, F.sendflags, 0);
	}
}

/* Calls ent.SendEntity(client, flags). Returns false if the entity chose not
 * to be sent; otherwise msg_entity_buf holds its bytes. */
static bool call_sendentity(int e, int client_edict, uint32_t flags)
{
	qcvm_t *vm = sv.vm;
	func_t f = QC_EInt(vm, e, F.sendentity);
	int oldself = QC_GInt(vm, G.self);
	bool ok;
	msg_entity_buf.len = 0;
	if (!f)
		return false;
	QC_SetGInt(vm, G.self, e);
	vm->globals[OFS_PARM(0)].i = client_edict;
	vm->globals[OFS_PARM(1)].f = (float)flags;
	vm->argc = 2;
	ok = QC_Execute(vm, f) && vm->globals[OFS_RETURN].f != 0;
	QC_SetGInt(vm, G.self, oldself);
	return ok;
}

/* Writes as many entity updates as fit in 'budget' bytes. */
static void write_entities(client_t *c, msg_t *m, size_t budget, snaprec_t *rec)
{
	qcvm_t *vm = sv.vm;
	int n = vm->num_edicts;
	uint32_t start = c->ent_rotor % (uint32_t)BW_MAX(n, 1);
	bool full = false;
	for (int k = 0; k < n && !full; k++) {
		int e = (int)((start + (uint32_t)k) % (uint32_t)n);
		uint8_t *st = &c->estate[e];
		bool alive = !vm->edicts[e].free && F.sendentity >= 0 && QC_EInt(vm, e, F.sendentity);
		size_t before = m->cursize;

		if (!alive) {
			if ((*st & ES_KNOWN) || (*st & ES_NEEDREMOVE)) {
				if (m->cursize + 6 > budget) {
					full = true;
					break;
				}
				MSG_WriteVarUInt(m, ((uint32_t)(e + 1) << 1) | 1);
				snap_add(rec, (uint32_t)e, 0, true, false);
				*st = 0;
				c->pending[e] = 0;
			}
			continue;
		}

		{
			uint32_t flags = c->pending[e];
			bool create = !(*st & ES_KNOWN);
			if (create || !(*st & ES_CONFIRMED))
				flags = flags || create ? SENDFLAGS_ALL : 0;
			if (!flags)
				continue;
			if (!call_sendentity(e, c->edict, flags)) {
				/* hidden from this client: remove it if it was known */
				if (*st & ES_KNOWN) {
					MSG_WriteVarUInt(m, ((uint32_t)(e + 1) << 1) | 1);
					snap_add(rec, (uint32_t)e, 0, true, false);
				}
				*st = 0;
				c->pending[e] = 0;
				continue;
			}
			if (m->cursize + msg_entity_buf.len + 10 > budget) {
				m->cursize = before;
				full = true;
				c->ent_rotor = (uint32_t)e; /* start here next time */
				break;
			}
			MSG_WriteVarUInt(m, (uint32_t)(e + 1) << 1);
			MSG_WriteVarUInt(m, (uint32_t)msg_entity_buf.len);
			MSG_WriteData(m, msg_entity_buf.data, msg_entity_buf.len);
			snap_add(rec, (uint32_t)e, flags, false, create);
			*st = (uint8_t)((*st | ES_KNOWN) & ~ES_NEEDREMOVE);
			c->pending[e] = 0;
		}
	}
	if (!full)
		c->ent_rotor = 0;
	MSG_WriteVarUInt(m, 0);
}

static void send_snapshot(client_t *c)
{
	uint8_t buf[NET_MAX_PACKET];
	msg_t m;
	snaprec_t *rec;
	uint32_t seq = c->chan.outseq + 1; /* the sequence Netchan_Transmit will use */
	size_t budget = NET_MAX_PACKET - 48;
	if (Netchan_HasPendingReliable(&c->chan))
		budget -= NETCHAN_FRAGSIZE + 8;

	MSG_Init(&m, buf, sizeof(buf));
	m.allowoverflow = true;
	/* unreliable extras first; dropped if they would not fit */
	if (c->datagram.len && c->datagram.len < budget / 2)
		MSG_WriteData(&m, c->datagram.data, c->datagram.len);
	c->datagram.len = 0;

	rec = &c->snaps[seq % SNAP_HISTORY];
	rec->seq = seq;
	rec->num = 0;

	if (c->state == CS_SPAWNED) {
		MSG_WriteByte(&m, svc_snapshot);
		MSG_WriteLong(&m, sv.frames);
		MSG_WriteDouble(&m, sv.time);
		MSG_WriteLong(&m, (int32_t)c->lastcmd);
		write_entities(c, &m, budget, rec);
	}
	if (m.overflowed) {
		Con_DPrintf("snapshot for %s overflowed\n", c->name);
		m.cursize = 0;
		rec->num = 0;
	}
	Netchan_Transmit(&c->chan, m.data, m.cursize);
}

/* --- signon ------------------------------------------------------------ */

static void send_serverdata(client_t *c)
{
	msg_t m;
	uint8_t *big;
	size_t cssize = 0;
	char *cs = FS_LoadFile("csprogs.dat", &cssize);
	uint16_t crc = cs ? Digest_CRC16(cs, cssize) : 0;
	Mem_Free(cs);

	big = Mem_Alloc(65536);
	MSG_Init(&m, big, 65536);
	MSG_WriteByte(&m, svc_serverdata);
	MSG_WriteVarUInt(&m, PROTOCOL_VERSION);
	MSG_WriteLong(&m, svs.spawncount);
	MSG_WriteFloat(&m, 1.0f / sv.frametime);
	MSG_WriteByte(&m, svs.maxclients);
	MSG_WriteByte(&m, client_slot(c));
	MSG_WriteVarUInt(&m, (uint32_t)c->edict);
	MSG_WriteString(&m, sv.mapname);
	MSG_WriteString(&m, sv.worldpath);
	MSG_WriteString(&m, cs ? "csprogs.dat" : "");
	MSG_WriteShort(&m, crc);
	MSG_WriteLong(&m, (int32_t)cssize);
	MSG_WriteString(&m, sv.serverinfo);
	client_reliable(c, &m);
	Mem_Free(big);

	{
		LOCALMSG(p, 600);
		for (int i = 1; i < sv.nummodels; i++) {
			MSG_Clear(&p);
			MSG_WriteByte(&p, svc_precache);
			MSG_WriteByte(&p, PRECACHE_MODEL);
			MSG_WriteVarUInt(&p, (uint32_t)i);
			MSG_WriteString(&p, sv.models[i]);
			client_reliable(c, &p);
		}
		for (int i = 1; i < sv.numsounds; i++) {
			MSG_Clear(&p);
			MSG_WriteByte(&p, svc_precache);
			MSG_WriteByte(&p, PRECACHE_SOUND);
			MSG_WriteVarUInt(&p, (uint32_t)i);
			MSG_WriteString(&p, sv.sounds[i]);
			client_reliable(c, &p);
		}
		for (int i = 0; i < sv.numparticles; i++) {
			MSG_Clear(&p);
			MSG_WriteByte(&p, svc_precache);
			MSG_WriteByte(&p, PRECACHE_PARTICLE);
			MSG_WriteVarUInt(&p, (uint32_t)i + 1);
			MSG_WriteString(&p, sv.particles[i]);
			client_reliable(c, &p);
		}
		for (int i = 0; i < SV_MAX_LIGHTSTYLES; i++) {
			if (!sv.lightstyles[i])
				continue;
			MSG_Clear(&p);
			MSG_WriteByte(&p, svc_lightstyle);
			MSG_WriteByte(&p, i);
			MSG_WriteString(&p, sv.lightstyles[i]);
			client_reliable(c, &p);
		}
	}
	/* everyone's player info, as whole info strings (empty key) */
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *o = &svs.clients[i];
		LOCALMSG(p, 2200);
		if (o->state == CS_FREE)
			continue;
		MSG_WriteByte(&p, svc_playerinfo);
		MSG_WriteByte(&p, i);
		MSG_WriteString(&p, "");
		MSG_WriteString(&p, o->userinfo);
		client_reliable(c, &p);
		for (int k = 0; k < MAX_INFOBLOBS; k++) {
			const infoblob_t *b = &o->blobs[k];
			msg_t bm;
			if (!b->key[0] || b->key[0] == '_')
				continue;
			MSG_Init(&bm, Mem_Alloc((size_t)b->len + 128), (size_t)b->len + 128);
			write_blob(&bm, i, b);
			client_reliable(c, &bm);
			Mem_Free(bm.data);
		}
	}
	if (svs.signonlen) {
		MSG_Init(&m, Mem_Alloc(svs.signonlen + 16), svs.signonlen + 16);
		MSG_WriteByte(&m, svc_qcmessage);
		MSG_WriteVarUInt(&m, (uint32_t)svs.signonlen);
		MSG_WriteData(&m, svs.signon, svs.signonlen);
		client_reliable(c, &m);
		Mem_Free(m.data);
	}
	{
		LOCALMSG(p, 8);
		MSG_WriteByte(&p, svc_signon);
		MSG_WriteByte(&p, 1);
		client_reliable(c, &p);
	}
	c->spawncount = svs.spawncount;
	c->state = CS_CONNECTED;
}

static void copy_parms_to_globals(client_t *c)
{
	for (int i = 0; i < MAX_SPAWN_PARMS; i++) {
		char name[16];
		snprintf(name, sizeof(name), "parm%d", i + 1);
		QC_SetGFloat(sv.vm, QC_GlobalOfs(sv.vm, name), c->spawn_parms[i]);
	}
}

static void copy_globals_to_parms(client_t *c)
{
	for (int i = 0; i < MAX_SPAWN_PARMS; i++) {
		char name[16];
		snprintf(name, sizeof(name), "parm%d", i + 1);
		c->spawn_parms[i] = QC_GFloat(sv.vm, QC_GlobalOfs(sv.vm, name));
	}
	c->have_parms = true;
}

static void prepare_client_edict(client_t *c)
{
	qcvm_t *vm = sv.vm;
	QC_ClearEdict(vm, c->edict);
	QC_SetEFloat(vm, c->edict, F.dimension_solid, 255);
	QC_SetEFloat(vm, c->edict, F.dimension_hit, 255);
	if (F.netname >= 0)
		QC_EdictFields(vm, c->edict)[F.netname].i = QC_NewString(vm, c->name);
	QC_SetEFloat(vm, c->edict, F.flags, FL_CLIENT);
}

static void spawn_client(client_t *c)
{
	qcvm_t *vm = sv.vm;
	reset_entity_state(c);
	c->lastcmd = 0;
	memset(&c->lastucmd, 0, sizeof(c->lastucmd));
	c->cmd_budget = 0.25f;
	if (SV_SaveTakeRestored(c)) {
		/* a loaded game put the player's entity back as it was */
		c->state = CS_SPAWNED;
		SV_SaveClientSpawned(c);
		return;
	}
	prepare_client_edict(c);
	QC_SetGFloat(vm, G.time, (float)sv.time);
	QC_SetGInt(vm, G.self, c->edict);
	if (!c->have_parms) {
		if (QC_FindFunction(vm, "SetNewParms"))
			QC_ExecuteByName(vm, "SetNewParms");
		copy_globals_to_parms(c);
	}
	copy_parms_to_globals(c);
	QC_SetGInt(vm, G.self, c->edict);
	if (QC_FindFunction(vm, "ClientConnect"))
		QC_ExecuteByName(vm, "ClientConnect");
	QC_SetGInt(vm, G.self, c->edict);
	if (QC_FindFunction(vm, "PutClientInServer"))
		QC_ExecuteByName(vm, "PutClientInServer");
	c->state = CS_SPAWNED;
	SV_SaveClientSpawned(c);
	if (!c->bot) {
		LOCALMSG(p, 8);
		MSG_WriteByte(&p, svc_signon);
		MSG_WriteByte(&p, 2);
		client_reliable(c, &p);
	}
	Con_Printf("%s entered the game\n", c->name);
}

void SV_DropClient(client_t *c, const char *reason)
{
	qcvm_t *vm = sv.vm;
	if (c->state == CS_FREE)
		return;
	if (c->state == CS_SPAWNED && vm) {
		QC_SetGInt(vm, G.self, c->edict);
		if (QC_FindFunction(vm, "ClientDisconnect"))
			QC_ExecuteByName(vm, "ClientDisconnect");
	}
	if (!c->bot) {
		/* tell the client a few times, it is unreliable */
		LOCALMSG(m, 256);
		MSG_WriteByte(&m, svc_disconnect);
		MSG_WriteString(&m, reason ? reason : "");
		for (int i = 0; i < 3; i++)
			Netchan_Transmit(&c->chan, m.data, m.cursize);
		Netchan_Free(&c->chan);
	}
	Con_Printf("%s disconnected%s%s\n", c->name[0] ? c->name : "client", reason ? ": " : "", reason ? reason : "");
	if (vm && c->edict < vm->max_edicts) {
		QC_ClearEdict(vm, c->edict);
	}
	free_client_state(c);
	c->state = CS_FREE;
	c->bot = false;
	c->have_parms = false;
	broadcast_playerinfo(c, "", "");
}

void SV_ClientsMapChanged(void)
{
	svs.spawncount++;
	msg_init.len = 0;
	svs.signon = NULL;
	svs.signonlen = 0;
	memset(&IG, -1, sizeof(IG));
	if (sv.vm) {
		IG.sequence = QC_GlobalOfs(sv.vm, "input_sequence");
		IG.timelength = QC_GlobalOfs(sv.vm, "input_timelength");
		IG.angles = QC_GlobalOfs(sv.vm, "input_angles");
		IG.movevalues = QC_GlobalOfs(sv.vm, "input_movevalues");
		IG.buttons = QC_GlobalOfs(sv.vm, "input_buttons");
		IG.impulse = QC_GlobalOfs(sv.vm, "input_impulse");
		IG.servertime = QC_GlobalOfs(sv.vm, "input_servertime");
		IG.clienttime = QC_GlobalOfs(sv.vm, "input_clienttime");
		IG.weapon = QC_GlobalOfs(sv.vm, "input_weapon");
		IG.cursor_screen = QC_GlobalOfs(sv.vm, "input_cursor_screen");
		IG.cursor_start = QC_GlobalOfs(sv.vm, "input_cursor_trace_start");
		IG.cursor_end = QC_GlobalOfs(sv.vm, "input_cursor_trace_endpos");
		IG.cursor_ent = QC_GlobalOfs(sv.vm, "input_cursor_entitynumber");
		IG.lightlevel = QC_GlobalOfs(sv.vm, "input_lightlevel");
		sv.vm->reserved_edicts = svs.maxclients;
		if (sv.vm->num_edicts <= svs.maxclients)
			sv.vm->num_edicts = svs.maxclients + 1;
	}
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state == CS_FREE)
			continue;
		if (c->bot) {
			spawn_client(c);
			continue;
		}
		reset_entity_state(c);
		send_serverdata(c);
	}
}

/* --- connectionless packets --------------------------------------------- */

static void cl_getchallenge(const netadr_t *from)
{
	int oldest = 0;
	for (int i = 0; i < 64; i++) {
		if (NET_CompareAdr(&svs.challenges[i].adr, from)) {
			NET_OutOfBandPrint(NS_SERVER, from, "challenge %d", svs.challenges[i].challenge);
			return;
		}
		if (svs.challenges[i].time < svs.challenges[oldest].time)
			oldest = i;
	}
	svs.challenges[oldest].adr = *from;
	svs.challenges[oldest].challenge = (rand() << 16) ^ rand();
	svs.challenges[oldest].time = host_time;
	NET_OutOfBandPrint(NS_SERVER, from, "challenge %d", svs.challenges[oldest].challenge);
}

static void cl_connect(const netadr_t *from, int argc, const char **argv)
{
	int proto, qport, challenge;
	client_t *c = NULL;
	if (argc < 5) {
		NET_OutOfBandPrint(NS_SERVER, from, "reject bad connect request");
		return;
	}
	proto = atoi(argv[1]);
	qport = atoi(argv[2]);
	challenge = atoi(argv[3]);
	if (proto != PROTOCOL_VERSION) {
		NET_OutOfBandPrint(NS_SERVER, from, "reject server uses protocol %d", PROTOCOL_VERSION);
		return;
	}
	if (from->type != NA_LOOPBACK) {
		bool ok = false;
		for (int i = 0; i < 64; i++)
			if (NET_CompareAdr(&svs.challenges[i].adr, from) && svs.challenges[i].challenge == challenge)
				ok = true;
		if (!ok) {
			NET_OutOfBandPrint(NS_SERVER, from, "reject bad challenge");
			return;
		}
	}
	if (!sv.active) {
		NET_OutOfBandPrint(NS_SERVER, from, "reject no map running");
		return;
	}
	{
		char why[96], pass[128];
		if (SV_CheckBanned(from, why, sizeof(why))) {
			NET_OutOfBandPrint(NS_SERVER, from, "reject %s", why);
			return;
		}
		/* a server password is matched against the "password" userinfo key */
		if (Cvar_String("sv_password")[0] && strcmp(Cvar_String("sv_password"), "none") && from->type != NA_LOOPBACK &&
		    strcmp(SV_InfoGet(argv[4], "password", pass, sizeof(pass)), Cvar_String("sv_password"))) {
			NET_OutOfBandPrint(NS_SERVER, from, "reject server needs a password (setinfo password ...)");
			return;
		}
	}
	/* a reconnect from the same address takes over its old slot */
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *o = &svs.clients[i];
		if (o->state != CS_FREE && !o->bot && NET_CompareBaseAdr(&o->chan.remote, from) && o->chan.qport == qport) {
			SV_DropClient(o, "reconnecting");
			c = o;
			break;
		}
	}
	for (int i = 0; !c && i < svs.maxclients; i++)
		if (svs.clients[i].state == CS_FREE)
			c = &svs.clients[i];
	if (!c) {
		NET_OutOfBandPrint(NS_SERVER, from, "reject server is full");
		return;
	}
	memset(c, 0, sizeof(*c));
	c->edict = client_slot(c) + 1;
	Netchan_Setup(&c->chan, NS_SERVER, from, qport, false);
	Str_Copy(c->userinfo, argv[4], sizeof(c->userinfo));
	SV_InfoGet(c->userinfo, "name", c->name, sizeof(c->name));
	if (!c->name[0]) {
		Str_Copy(c->name, "player", sizeof(c->name));
		SV_InfoSet(c->userinfo, sizeof(c->userinfo), "name", c->name);
	}
	{
		char tmp[32];
		c->updaterate = (float)atof(SV_InfoGet(c->userinfo, "cl_updaterate", tmp, sizeof(tmp)));
		c->chan.rate = atoi(SV_InfoGet(c->userinfo, "rate", tmp, sizeof(tmp)));
		c->interp = (float)atof(SV_InfoGet(c->userinfo, "cl_interp", tmp, sizeof(tmp)));
	}
	if (sv_maxrate->ivalue > 0 && (c->chan.rate <= 0 || c->chan.rate > sv_maxrate->ivalue))
		c->chan.rate = sv_maxrate->ivalue;
	c->connect_time = host_time;
	c->state = CS_CONNECTED;
	NET_OutOfBandPrint(NS_SERVER, from, "accept %d", svs.spawncount);
	Con_Printf("%s connected from %s\n", c->name, NET_AdrToString(from));
	broadcast_playerinfo(c, "", c->userinfo);
	reset_entity_state(c);
	send_serverdata(c);
}

static void cl_info(const netadr_t *from)
{
	char info[2048];
	int n = 0;
	for (int i = 0; i < svs.maxclients; i++)
		n += svs.clients[i].state != CS_FREE;
	snprintf(info, sizeof(info), "\\hostname\\%s\\map\\%s\\clients\\%d\\maxclients\\%d\\protocol\\%d\\engine\\%s",
	         hostname->string, sv.mapname, n, svs.maxclients, PROTOCOL_VERSION, PROTOCOL_NAME);
	NET_OutOfBandPrint(NS_SERVER, from, "infoResponse\n%s", info);
}

static void connectionless(const netadr_t *from, uint8_t *data, size_t len)
{
	cmd_tokens_t *t = Mem_Alloc(sizeof(*t));
	char *text = Mem_Alloc(len - 3);
	memcpy(text, data + 4, len - 4);
	Cmd_TokenizeInto(t, text);
	if (t->argc) {
		const char *c = t->argv[0];
		if (!strcmp(c, "getchallenge"))
			cl_getchallenge(from);
		else if (!strcmp(c, "connect"))
			cl_connect(from, t->argc, t->argv);
		else if (!strcmp(c, "getinfo") || !strcmp(c, "info"))
			cl_info(from);
		else if (!strcmp(c, "rcon"))
			SV_HandleRcon(from, text);
	}
	Mem_Free(text);
	Mem_Free(t);
}

/* --- client messages ---------------------------------------------------- */

static void set_input_globals(const usercmd_t *u)
{
	qcvm_t *vm = sv.vm;
	QC_SetGFloat(vm, IG.sequence, (float)u->sequence);
	QC_SetGFloat(vm, IG.timelength, u->msec);
	QC_SetGVec(vm, IG.angles, u->angles);
	QC_SetGVec(vm, IG.movevalues, u->movevalues);
	QC_SetGFloat(vm, IG.buttons, (float)u->buttons);
	QC_SetGFloat(vm, IG.impulse, u->impulse);
	QC_SetGFloat(vm, IG.servertime, u->servertime_ms / 1000.0f);
	QC_SetGFloat(vm, IG.clienttime, (float)u->clienttime);
	if (IG.weapon >= 0) {
		vm->globals[IG.weapon].u = u->weapon;
		vm->globals[IG.weapon + 1].u = 0;
	}
	if (IG.cursor_screen >= 0) {
		float v[3] = {u->cursor_screen[0], u->cursor_screen[1], 0};
		QC_SetGVec(vm, IG.cursor_screen, v);
	}
	QC_SetGVec(vm, IG.cursor_start, u->cursor_start);
	QC_SetGVec(vm, IG.cursor_end, u->cursor_end);
	QC_SetGFloat(vm, IG.cursor_ent, (float)u->cursor_entity);
}

/* --- lag compensation (after Source's CLagCompensationManager) ---------- */

#define LAG_HISTORY 256
typedef struct {
	double time;
	struct {
		bool valid;
		float origin[3], mins[3], maxs[3];
	} ent[256];
} lagframe_t;
static lagframe_t *lag_frames;
static int lag_head; /* next slot to write */

void SV_LagCompensationRecord(void)
{
	lagframe_t *f;
	int n = BW_MIN(svs.maxclients, 256);
	if (!sv_unlag->ivalue || svs.maxclients < 2)
		return;
	if (!lag_frames)
		lag_frames = Mem_Alloc(sizeof(lagframe_t) * LAG_HISTORY);
	f = &lag_frames[lag_head++ % LAG_HISTORY];
	f->time = sv.time;
	for (int i = 0; i < n; i++) {
		client_t *c = &svs.clients[i];
		f->ent[i].valid = c->state == CS_SPAWNED && QC_EFloat(sv.vm, c->edict, F.solid) != SOLID_NOT;
		if (!f->ent[i].valid)
			continue;
		memcpy(f->ent[i].origin, &QC_EdictFields(sv.vm, c->edict)[F.origin], sizeof(float) * 3);
		memcpy(f->ent[i].mins, &QC_EdictFields(sv.vm, c->edict)[F.mins], sizeof(float) * 3);
		memcpy(f->ent[i].maxs, &QC_EdictFields(sv.vm, c->edict)[F.maxs], sizeof(float) * 3);
	}
}

typedef struct {
	bool moved;
	float saved[3], rewound[3];
} lagrestore_t;

/* Moves every other player back to where 'c' saw them when it issued the
 * command: the command's server time minus the client's interpolation. */
static void lag_rewind(client_t *c, const usercmd_t *u, lagrestore_t *restore)
{
	double target;
	int n = BW_MIN(svs.maxclients, 256);
	memset(restore, 0, sizeof(lagrestore_t) * (size_t)n);
	if (!sv_unlag->ivalue || svs.maxclients < 2 || !lag_frames || !u->servertime_ms)
		return;
	target = u->servertime_ms / 1000.0 - BW_MAX(c->interp, 0.0f);
	if (sv.time - target > sv_maxunlag->value)
		target = sv.time - sv_maxunlag->value;
	if (target >= sv.time)
		return;
	for (int i = 0; i < n; i++) {
		client_t *o = &svs.clients[i];
		lagframe_t *a = NULL, *b = NULL;
		float *org;
		if (o == c || o->state != CS_SPAWNED)
			continue;
		/* newest frame at or before the target, and the one after it */
		for (int k = 1; k <= LAG_HISTORY && k <= lag_head; k++) {
			lagframe_t *fr = &lag_frames[(lag_head - k) % LAG_HISTORY];
			if (!fr->ent[i].valid)
				break;
			if (fr->time <= target) {
				a = fr;
				break;
			}
			b = fr;
		}
		if (!a)
			continue;
		org = &QC_EdictFields(sv.vm, o->edict)[F.origin].f;
		VectorCopy(org, restore[i].saved);
		for (int k = 0; k < 3; k++) {
			float frac = (b && b->time > a->time) ? (float)((target - a->time) / (b->time - a->time)) : 0;
			restore[i].rewound[k] = a->ent[i].origin[k] + (b ? (b->ent[i].origin[k] - a->ent[i].origin[k]) * frac : 0);
		}
		VectorCopy(restore[i].rewound, org);
		SV_LinkEdict(o->edict, false);
		restore[i].moved = true;
	}
}

static void lag_restore(lagrestore_t *restore)
{
	int n = BW_MIN(svs.maxclients, 256);
	for (int i = 0; i < n; i++) {
		float *org;
		if (!restore[i].moved)
			continue;
		org = &QC_EdictFields(sv.vm, svs.clients[i].edict)[F.origin].f;
		/* if the command moved them (knockback etc.) keep the new position */
		if (org[0] == restore[i].rewound[0] && org[1] == restore[i].rewound[1] && org[2] == restore[i].rewound[2])
			VectorCopy(restore[i].saved, org);
		SV_LinkEdict(svs.clients[i].edict, false);
	}
}

void SV_RunStandardPlayerPhysics(int e);

static void run_usercmd(client_t *c, const usercmd_t *u)
{
	qcvm_t *vm = sv.vm;
	lagrestore_t restore[256];
	usercmd_t cmd = *u;
	func_t f;
	/* speedhack guard: a client cannot submit more movement than real time allows */
	if (cmd.msec > 0.1f)
		cmd.msec = 0.1f;
	if (cmd.msec < 0)
		cmd.msec = 0;
	if (cmd.msec > c->cmd_budget)
		cmd.msec = BW_MAX(c->cmd_budget, 0.0f);
	c->cmd_budget -= cmd.msec;

	set_input_globals(&cmd);
	QC_SetGFloat(vm, G.time, (float)sv.time);
	QC_SetGFloat(vm, G.frametime, cmd.msec);
	QC_SetGInt(vm, G.self, c->edict);
	QC_SetGInt(vm, G.other, 0);
	lag_rewind(c, &cmd, restore);
	f = QC_FindFunction(vm, "SV_RunClientCommand");
	if (f)
		QC_Execute(vm, f);
	else
		SV_RunStandardPlayerPhysics(c->edict);
	lag_restore(restore);
	QC_SetGFloat(vm, G.frametime, sv.frametime);
	c->lastucmd = cmd;
	c->lastcmd = cmd.sequence;
}

static void read_usercmd(msg_t *m, const usercmd_t *from, usercmd_t *to)
{
	int bits;
	*to = *from;
	to->sequence = (uint32_t)MSG_ReadLong(m);
	to->msec = MSG_ReadByte(m) / 1000.0f;
	bits = MSG_ReadByte(m);
	if (bits & UCMD_ANGLES)
		for (int i = 0; i < 3; i++)
			to->angles[i] = MSG_ReadAngle(m);
	if (bits & UCMD_MOVE)
		for (int i = 0; i < 3; i++)
			to->movevalues[i] = (float)MSG_ReadShort(m);
	if (bits & UCMD_BUTTONS)
		to->buttons = MSG_ReadVarUInt(m);
	if (bits & UCMD_IMPULSE)
		to->impulse = (uint8_t)MSG_ReadByte(m);
	else
		to->impulse = 0;
	if (bits & UCMD_WEAPON)
		to->weapon = MSG_ReadVarUInt(m);
	if (bits & UCMD_CURSOR) {
		to->cursor_screen[0] = MSG_ReadFloat(m);
		to->cursor_screen[1] = MSG_ReadFloat(m);
		for (int i = 0; i < 3; i++)
			to->cursor_start[i] = MSG_ReadFloat(m);
		for (int i = 0; i < 3; i++)
			to->cursor_end[i] = MSG_ReadFloat(m);
		to->cursor_entity = MSG_ReadEntity(m);
	}
}

static void parse_move(client_t *c, msg_t *m)
{
	usercmd_t cmds[1 + MAX_BACKUP_CMDS], base;
	int count = MSG_ReadByte(m);
	uint32_t servertime_ms = (uint32_t)MSG_ReadLong(m);
	double clienttime = MSG_ReadDouble(m);
	if (count < 1 || count > 1 + MAX_BACKUP_CMDS) {
		m->badread = true;
		return;
	}
	memset(&base, 0, sizeof(base));
	for (int i = 0; i < count; i++) {
		read_usercmd(m, i ? &cmds[i - 1] : &base, &cmds[i]);
		cmds[i].servertime_ms = servertime_ms;
		cmds[i].clienttime = clienttime;
	}
	if (m->badread || c->state != CS_SPAWNED)
		return;
	/* oldest first; anything already executed is a resend of a lost packet */
	for (int i = 0; i < count; i++)
		if (cmds[i].sequence > c->lastcmd)
			run_usercmd(c, &cmds[i]);
}

static void execute_clientcommand(client_t *c, const char *s);

static void parse_stringcmd(client_t *c, const char *s)
{
	cmd_tokens_t *t = Mem_Alloc(sizeof(*t));
	Cmd_TokenizeInto(t, s);
	if (!t->argc)
		goto done;
	if (!strcmp(t->argv[0], "begin")) {
		if (c->state == CS_CONNECTED && t->argc >= 2 && atoi(t->argv[1]) == svs.spawncount) {
			if (c->fullupdate) {
				/* back in the game where the player was */
				LOCALMSG(p, 8);
				c->fullupdate = false;
				c->state = CS_SPAWNED;
				reset_entity_state(c);
				MSG_WriteByte(&p, svc_signon);
				MSG_WriteByte(&p, 2);
				client_reliable(c, &p);
			} else {
				spawn_client(c);
			}
		}
		goto done;
	}
	/* "fullupdate": everything from serverdata on again, without a
	 * respawn, so a demo recording can start mid-game */
	if (!strcmp(t->argv[0], "fullupdate")) {
		if (c->state == CS_SPAWNED) {
			c->fullupdate = true;
			reset_entity_state(c);
			send_serverdata(c);
		}
		goto done;
	}
	if (!strcmp(t->argv[0], "disconnect") || !strcmp(t->argv[0], "drop")) {
		SV_DropClient(c, NULL);
		goto done;
	}
	if (!strcmp(t->argv[0], "setinfo") && t->argc >= 3) {
		set_playerkey(c, t->argv[1], t->argv[2], true);
		if (!strcmp(t->argv[1], "cl_updaterate"))
			c->updaterate = (float)atof(t->argv[2]);
		else if (!strcmp(t->argv[1], "rate")) {
			c->chan.rate = atoi(t->argv[2]);
			if (sv_maxrate->ivalue > 0 && (c->chan.rate <= 0 || c->chan.rate > sv_maxrate->ivalue))
				c->chan.rate = sv_maxrate->ivalue;
		} else if (!strcmp(t->argv[1], "cl_interp"))
			c->interp = (float)atof(t->argv[2]);
		if (sv.vm && c->state == CS_SPAWNED && QC_FindFunction(sv.vm, "UserInfo_Changed")) {
			/* not part of the documented API; harmless when absent */
		}
		goto done;
	}
	if (c->state != CS_SPAWNED)
		goto done;
	/* everything else is the game's to handle */
	{
		func_t f = QC_FindFunction(sv.vm, "SV_ParseClientCommand");
		if (f) {
			QC_SetGInt(sv.vm, G.self, c->edict);
			QC_SetGFloat(sv.vm, G.time, (float)sv.time);
			sv.vm->globals[OFS_PARM(0)].i = QC_NewString(sv.vm, s);
			sv.vm->argc = 1;
			QC_Execute(sv.vm, f);
		} else
			execute_clientcommand(c, s);
	}
done:
	Mem_Free(t);
}

/* The engine's own handling of a client command (clientcommand builtin). */
static void execute_clientcommand(client_t *c, const char *s)
{
	cmd_tokens_t *t = Mem_Alloc(sizeof(*t));
	Cmd_TokenizeInto(t, s);
	if (!t->argc)
		goto done;
	if ((!strcmp(t->argv[0], "say") || !strcmp(t->argv[0], "say_team")) && t->argc > 1) {
		char text[1024];
		snprintf(text, sizeof(text), "%s: %s\n", c->name, Cmd_Args());
		for (int i = 0; i < svs.maxclients; i++)
			if (svs.clients[i].state == CS_SPAWNED)
				client_print(&svs.clients[i], 3, text);
		Con_Print(text);
	} else if (!strcmp(t->argv[0], "kill")) {
		if (QC_FindFunction(sv.vm, "ClientKill")) {
			QC_SetGInt(sv.vm, G.self, c->edict);
			QC_ExecuteByName(sv.vm, "ClientKill");
		}
	} else if (!strcmp(t->argv[0], "name") && t->argc > 1) {
		set_playerkey(c, "name", t->argv[1], true);
	} else if (!strcmp(t->argv[0], "pause")) {
		if (!SV_RequestPause(c->edict, !sv.paused))
			client_print(c, 2, "Pausing isn't allowed now.\n");
	} else if (SV_ClientCheatCommand(c, t->argc, t->argv)) {
		/* god, noclip and the other cheats */
	} else if (!strcmp(t->argv[0], "setinfo") || !strcmp(t->argv[0], "begin") || !strcmp(t->argv[0], "disconnect")) {
		/* handled by parse_stringcmd */
	} else {
		Con_DPrintf("unknown client command from %s: %s\n", c->name, s);
	}
done:
	Mem_Free(t);
}

static void parse_event(client_t *c, msg_t *m)
{
	char fname[256];
	const char *name = MSG_ReadString(m);
	const char *types;
	char typebuf[16];
	func_t f;
	qcvm_t *vm = sv.vm;
	int argc = 0;
	Str_Copy(fname, name, sizeof(fname));
	types = MSG_ReadString(m);
	Str_Copy(typebuf, types, sizeof(typebuf));
	for (int i = 0; typebuf[i] && argc < 8; i++, argc++) {
		qcval_t *p = &vm->globals[OFS_PARM(argc)];
		switch (typebuf[i]) {
		case 'f':
			p->f = MSG_ReadFloat(m);
			break;
		case 'i':
			p->i = MSG_ReadLong(m);
			break;
		case 'e':
			p->i = MSG_ReadEntity(m);
			if (p->i >= vm->num_edicts)
				p->i = 0;
			break;
		case 'v':
			for (int k = 0; k < 3; k++)
				p[k].f = MSG_ReadFloat(m);
			break;
		case 's':
			p->i = QC_NewString(vm, MSG_ReadString(m));
			break;
		default:
			m->badread = true;
			return;
		}
	}
	if (m->badread || c->state != CS_SPAWNED)
		return;
	{
		char full[300];
		if (typebuf[0])
			snprintf(full, sizeof(full), "CSEv_%s_%s", fname, typebuf);
		else
			snprintf(full, sizeof(full), "CSEv_%s", fname);
		f = QC_FindFunction(vm, full);
		if (!f) {
			Con_DPrintf("client event %s has no handler\n", full);
			return;
		}
	}
	QC_SetGInt(vm, G.self, c->edict);
	QC_SetGFloat(vm, G.time, (float)sv.time);
	vm->argc = argc;
	QC_Execute(vm, f);
}

static void parse_client_message(client_t *c, msg_t *m)
{
	while (!m->badread && MSG_Remaining(m)) {
		int cmd = MSG_ReadByte(m);
		if (c->state == CS_FREE)
			return;
		switch (cmd) {
		case clc_nop:
			break;
		case clc_disconnect:
			SV_DropClient(c, NULL);
			return;
		case clc_move:
			parse_move(c, m);
			break;
		case clc_stringcmd:
			parse_stringcmd(c, MSG_ReadString(m));
			break;
		case clc_event:
			parse_event(c, m);
			break;
		case clc_userblob:
			parse_userblob(c, m);
			break;
		default:
			Con_DPrintf("bad message %d from %s\n", cmd, c->name);
			m->badread = true;
			break;
		}
	}
}

void SV_ReadPackets(void)
{
	static uint8_t buf[NET_MAX_RECV];
	netadr_t from;
	size_t len;
	while ((len = NET_GetPacket(NS_SERVER, buf, sizeof(buf), &from)) > 0) {
		msg_t m;
		int qport;
		client_t *c = NULL;
		if (len >= 4 && !memcmp(buf, "\xff\xff\xff\xff", 4)) {
			connectionless(&from, buf, len);
			continue;
		}
		qport = Netchan_PeekQPort(buf, len);
		for (int i = 0; i < svs.maxclients; i++) {
			client_t *o = &svs.clients[i];
			if (o->state == CS_FREE || o->bot || !NET_CompareBaseAdr(&o->chan.remote, &from) || o->chan.qport != qport)
				continue;
			if (o->chan.remote.port != from.port) /* NAT rebinding */
				o->chan.remote.port = from.port;
			c = o;
			break;
		}
		if (!c)
			continue;
		MSG_Init(&m, buf, sizeof(buf));
		m.cursize = len;
		if (!Netchan_Process(&c->chan, &m))
			continue;
		process_acks(c);
		{
			msg_t rel;
			while (c->state != CS_FREE && Netchan_NextReliable(&c->chan, &rel))
				parse_client_message(c, &rel);
		}
		if (c->state != CS_FREE)
			parse_client_message(c, &m);
	}
}

/* --- per frame ---------------------------------------------------------- */

void SV_ClientsPreThink(void)
{
	qcvm_t *vm = sv.vm;
	func_t f = QC_FindFunction(vm, "PlayerPreThink");
	for (int i = 0; f && i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state != CS_SPAWNED)
			continue;
		QC_SetGInt(vm, G.self, c->edict);
		QC_SetGFloat(vm, G.time, (float)sv.time);
		QC_Execute(vm, f);
	}
	/* bots send no input, so each frame runs one command for them, repeating
	 * the last one; the game fills input_* from its AI inside
	 * SV_RunClientCommand, as Nuclide's bots do */
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		usercmd_t cmd;
		if (c->state != CS_SPAWNED || !c->bot)
			continue;
		cmd = c->lastucmd;
		cmd.sequence = c->lastcmd + 1;
		cmd.msec = sv.frametime;
		cmd.servertime_ms = (uint32_t)(sv.time * 1000); /* no lag to rewind */
		cmd.impulse = 0;
		c->cmd_budget = BW_MAX(c->cmd_budget, cmd.msec);
		run_usercmd(c, &cmd);
	}
}

void SV_ClientsPostThink(void)
{
	qcvm_t *vm = sv.vm;
	func_t f = QC_FindFunction(vm, "PlayerPostThink");
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state != CS_SPAWNED)
			continue;
		/* real time passes for everyone, so movement budget refills */
		c->cmd_budget = BW_MIN(c->cmd_budget + sv.frametime, 0.25f);
		if (!f)
			continue;
		QC_SetGInt(vm, G.self, c->edict);
		QC_SetGFloat(vm, G.time, (float)sv.time);
		QC_Execute(vm, f);
	}
}

void SV_ClientsAfterPhysics(void)
{
	collect_sendflags();
	if (msg_broadcast.len) {
		LOCALMSG(h, 8);
		MSG_WriteByte(&h, svc_qcmessage);
		MSG_WriteVarUInt(&h, (uint32_t)msg_broadcast.len);
		for (int i = 0; i < svs.maxclients; i++) {
			client_t *c = &svs.clients[i];
			if (c->state != CS_SPAWNED || c->bot)
				continue;
			sb_write(&c->datagram, &h);
			SB_Append(&c->datagram, msg_broadcast.data, msg_broadcast.len);
		}
		msg_broadcast.len = 0;
	}
	SV_LagCompensationRecord();
}

void SV_SendClientMessages(void)
{
	double now = host_time;
	SV_SyncServerinfoCvars(true);
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		float rate;
		if (c->state == CS_FREE || c->bot)
			continue;
		if (now - c->chan.last_received > sv_timeout->value && c->chan.remote.type != NA_LOOPBACK) {
			SV_DropClient(c, "timed out");
			continue;
		}
		flush_qcrel(c);
		/* connecting clients only need their reliable stream pushed along */
		if (c->state != CS_SPAWNED) {
			if (Netchan_HasPendingReliable(&c->chan) || now - c->chan.last_received < 1.0)
				Netchan_Transmit(&c->chan, NULL, 0);
			continue;
		}
		rate = c->updaterate > 0 ? c->updaterate : 1.0f / sv.frametime;
		rate = BW_MAX(sv_minupdaterate->value, BW_MIN(rate, sv_maxupdaterate->value));
		if (now < c->next_snapshot || !Netchan_CanSend(&c->chan))
			continue;
		c->next_snapshot = BW_MAX(c->next_snapshot + 1.0 / rate, now - 0.5 / rate);
		if (now - c->last_ping_update > 2.0) {
			char ping[16];
			c->last_ping_update = now;
			snprintf(ping, sizeof(ping), "%d", (int)(c->chan.rtt * 1000));
			set_playerkey(c, "ping", ping, true);
		}
		send_snapshot(c);
	}
}

/* --- builtins ----------------------------------------------------------- */

static strbuf_t *write_target(qcvm_t *vm, int to)
{
	switch (to) {
	case 5: /* MSG_ENTITY */
		return &msg_entity_buf;
	case 4: /* MSG_MULTICAST */
		return &msg_multicast;
	case 0: /* MSG_BROADCAST */
		return &msg_broadcast;
	case 1: { /* MSG_ONE */
		client_t *c = SV_ClientForEdict(QC_GInt(vm, G.msg_entity));
		return c && c->state != CS_FREE ? &c->qcrel : NULL;
	}
	case 2: /* MSG_ALL: written to everyone below */
		return NULL;
	case 3: /* MSG_INIT */
		return &msg_init;
	default:
		return NULL;
	}
}

static void write_bytes(qcvm_t *vm, const void *data, size_t len)
{
	int to = (int)QC_PFLOAT(vm, 0);
	strbuf_t *sb;
	if (to == 2) {
		for (int i = 0; i < svs.maxclients; i++)
			if (svs.clients[i].state != CS_FREE && !svs.clients[i].bot)
				SB_Append(&svs.clients[i].qcrel, data, len);
		return;
	}
	sb = write_target(vm, to);
	if (!sb)
		return;
	SB_Append(sb, data, len);
	if (to == 3) {
		svs.signon = sb->data;
		svs.signonlen = sb->len;
	}
}

#define WRITER(fname, body)                                                                                            \
	static void fname(qcvm_t *vm)                                                                                      \
	{                                                                                                                  \
		LOCALMSG(m, 64);                                                                                               \
		body;                                                                                                          \
		write_bytes(vm, m.data, m.cursize);                                                                            \
	}

WRITER(bi_WriteByte, MSG_WriteByte(&m, (int)QC_PFLOAT(vm, 1)))
WRITER(bi_WriteChar, MSG_WriteChar(&m, (int)QC_PFLOAT(vm, 1)))
WRITER(bi_WriteShort, MSG_WriteShort(&m, (int)QC_PFLOAT(vm, 1)))
WRITER(bi_WriteLong, MSG_WriteLong(&m, (int32_t)QC_PFLOAT(vm, 1)))
WRITER(bi_WriteCoord, MSG_WriteCoord(&m, QC_PFLOAT(vm, 1)))
WRITER(bi_WriteAngle, MSG_WriteAngle(&m, QC_PFLOAT(vm, 1)))
WRITER(bi_WriteFloat, MSG_WriteFloat(&m, QC_PFLOAT(vm, 1)))
WRITER(bi_WriteInt, MSG_WriteLong(&m, QC_PINT(vm, 1)))
WRITER(bi_WriteEntity, MSG_WriteEntity(&m, QC_PENT(vm, 1)))
WRITER(bi_WriteInt64, MSG_WriteInt64(&m, (int64_t)QC_Parm(vm, 1)[0].u | (int64_t)QC_Parm(vm, 1)[1].u << 32))
WRITER(bi_WriteDouble, {
	double d;
	uint64_t u = (uint64_t)QC_Parm(vm, 1)[0].u | (uint64_t)QC_Parm(vm, 1)[1].u << 32;
	memcpy(&d, &u, 8);
	MSG_WriteDouble(&m, d);
})

static void bi_WriteString(qcvm_t *vm)
{
	const char *s = QC_PSTR(vm, 1);
	write_bytes(vm, s, strlen(s) + 1);
}

static void bi_WriteUnterminatedString(qcvm_t *vm)
{
	const char *s = QC_PSTR(vm, 1);
	write_bytes(vm, s, strlen(s));
}

/* whether a client's view may see a point */
static bool client_sees(const client_t *c, const float *org)
{
	float view[3];
	const qcval_t *f;
	if (!sv.world || !sv.world->vis)
		return true;
	f = QC_EdictFields(sv.vm, c->edict);
	for (int k = 0; k < 3; k++)
		view[k] = f[F.origin + k].f + (F.view_ofs >= 0 ? f[F.view_ofs + k].f : 0);
	return BSP_CheckPVS(sv.world, view, org);
}

static void bi_multicast(qcvm_t *vm)
{
	int to = (int)QC_PFLOAT(vm, 1);
	bool reliable = to == 3 || to == 4 || to == 5 || to == 7 || to == 10;
	bool one = to == 6 || to == 7 || to == 9 || to == 10;
	int target = one ? QC_GInt(vm, G.msg_entity) : 0;
	LOCALMSG(h, 8);
	if (!msg_multicast.len)
		return;
	MSG_WriteByte(&h, svc_qcmessage);
	MSG_WriteVarUInt(&h, (uint32_t)msg_multicast.len);
	/* PVS multicasts go to clients that may see the point; PHS ones (sounds
	 * carry around corners) go to everyone */
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state != CS_SPAWNED || c->bot)
			continue;
		if (one && c->edict != target)
			continue;
		if ((to == 2 || to == 5) && !client_sees(c, QC_PVEC(vm, 0)))
			continue;
		if (reliable)
			SB_Append(&c->qcrel, msg_multicast.data, msg_multicast.len);
		else {
			sb_write(&c->datagram, &h);
			SB_Append(&c->datagram, msg_multicast.data, msg_multicast.len);
		}
	}
	msg_multicast.len = 0;
}

static void concat_from(qcvm_t *vm, int first, char *out, size_t size)
{
	out[0] = 0;
	for (int i = first; i < vm->argc; i++)
		Str_Cat(out, QC_PSTR(vm, i), size);
}

static void bi_sprint(qcvm_t *vm)
{
	char text[4096];
	client_t *c = SV_ClientForEdict(QC_PENT(vm, 0));
	int level = 2;
	/* QW signature: (client, level, strings...) */
	concat_from(vm, 2, text, sizeof(text));
	level = (int)QC_PFLOAT(vm, 1);
	if (!c || c->state == CS_FREE) {
		Con_Print(text);
		return;
	}
	client_print(c, level, text);
}

static void bi_bprint(qcvm_t *vm)
{
	char text[4096];
	int level = (int)QC_PFLOAT(vm, 0);
	concat_from(vm, 1, text, sizeof(text));
	Con_Print(text);
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].state == CS_SPAWNED)
			client_print(&svs.clients[i], level, text);
}

static void bi_centerprint(qcvm_t *vm)
{
	char text[4096];
	client_t *c = SV_ClientForEdict(QC_PENT(vm, 0));
	LOCALMSG(m, 4200);
	concat_from(vm, 1, text, sizeof(text));
	if (!c || c->state == CS_FREE || c->bot)
		return;
	MSG_WriteByte(&m, svc_centerprint);
	MSG_WriteString(&m, text);
	client_reliable(c, &m);
}

static void bi_stuffcmd(qcvm_t *vm)
{
	char text[4096];
	client_t *c = SV_ClientForEdict(QC_PENT(vm, 0));
	LOCALMSG(m, 4200);
	concat_from(vm, 1, text, sizeof(text));
	if (!c || c->state == CS_FREE || c->bot)
		return;
	MSG_WriteByte(&m, svc_stufftext);
	MSG_WriteString(&m, text);
	client_reliable(c, &m);
}

static void bi_stuffcmdflags(qcvm_t *vm)
{
	/* (client, flags, text): flags select unreliable delivery, ignored */
	char text[4096];
	client_t *c = SV_ClientForEdict(QC_PENT(vm, 0));
	LOCALMSG(m, 4200);
	concat_from(vm, 2, text, sizeof(text));
	if (!c || c->state == CS_FREE || c->bot)
		return;
	MSG_WriteByte(&m, svc_stufftext);
	MSG_WriteString(&m, text);
	client_reliable(c, &m);
}

static void bi_clientcommand(qcvm_t *vm)
{
	client_t *c = SV_ClientForEdict(QC_PENT(vm, 0));
	if (c && c->state == CS_SPAWNED)
		execute_clientcommand(c, QC_PSTR(vm, 1));
}

static const char *client_key(client_t *c, const char *key, char *buf, size_t size)
{
	if (!strcmp(key, "ip"))
		return c->bot ? "bot" : NET_AdrToString(&c->chan.remote);
	if (!strcmp(key, "ping")) {
		snprintf(buf, size, "%d", c->bot ? 0 : (int)(c->chan.rtt * 1000));
		return buf;
	}
	if (!strcmp(key, "*bot"))
		return c->bot ? "1" : "";
	return SV_InfoGet(c->userinfo, key, buf, size);
}

static void bi_infokey(qcvm_t *vm)
{
	char buf[1024];
	int e = QC_PENT(vm, 0);
	const char *key = QC_PSTR(vm, 1);
	const char *v;
	if (e == 0) {
		SV_InfoGet(sv.serverinfo, key, buf, sizeof(buf));
		if (!buf[0])
			SV_InfoGet(sv.localinfo, key, buf, sizeof(buf));
		v = buf;
	} else {
		client_t *c = SV_ClientForEdict(e);
		v = c && c->state != CS_FREE ? client_key(c, key, buf, sizeof(buf)) : "";
	}
	if (v[0])
		QC_ReturnString(vm, v);
	else
		QC_ReturnInt(vm, 0);
}

static void bi_infokeyf(qcvm_t *vm)
{
	bi_infokey(vm);
	QC_ReturnFloat(vm, (float)atof(QC_GetString(vm, vm->globals[OFS_RETURN].i)));
}

static void bi_forceinfokey(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	const char *key = QC_PSTR(vm, 1), *value = QC_PSTR(vm, 2);
	if (e == 0) {
		SV_InfoSet(sv.serverinfo, sizeof(sv.serverinfo), key, value);
		SV_BroadcastServerinfo(key, value);
		return;
	}
	{
		client_t *c = SV_ClientForEdict(e);
		if (c && c->state != CS_FREE)
			set_playerkey(c, key, value, true);
	}
}

static void bi_setspawnparms(qcvm_t *vm)
{
	client_t *c = SV_ClientForEdict(QC_PENT(vm, 0));
	if (c && c->have_parms)
		copy_parms_to_globals(c);
}

static void bi_clienttype(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	client_t *c = SV_ClientForEdict(e);
	if (!c)
		QC_ReturnFloat(vm, 3); /* CLIENTTYPE_NOTACLIENT */
	else if (c->state == CS_FREE)
		QC_ReturnFloat(vm, 0); /* CLIENTTYPE_DISCONNECTED */
	else
		QC_ReturnFloat(vm, c->bot ? 2 : 1);
}

static void bi_spawnclient(qcvm_t *vm)
{
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state != CS_FREE)
			continue;
		memset(c, 0, sizeof(*c));
		c->bot = true;
		c->edict = i + 1;
		snprintf(c->name, sizeof(c->name), "bot%d", i + 1);
		SV_InfoSet(c->userinfo, sizeof(c->userinfo), "name", c->name);
		SV_InfoSet(c->userinfo, sizeof(c->userinfo), "*bot", "1");
		c->state = CS_SPAWNED;
		reset_entity_state(c);
		prepare_client_edict(c);
		broadcast_playerinfo(c, "", c->userinfo);
		QC_ReturnInt(vm, c->edict);
		return;
	}
	QC_ReturnInt(vm, 0);
}

static void bi_dropclient(qcvm_t *vm)
{
	client_t *c = SV_ClientForEdict(QC_PENT(vm, 0));
	if (c)
		SV_DropClient(c, "dropped by the game");
}

static void bi_checkclient(qcvm_t *vm)
{
	/* without PVS, cycle through spawned clients */
	static int last;
	for (int k = 1; k <= svs.maxclients; k++) {
		int i = (last + k) % BW_MAX(svs.maxclients, 1);
		if (svs.clients[i].state == CS_SPAWNED && QC_EFloat(vm, svs.clients[i].edict, F.health) > 0) {
			last = i;
			QC_ReturnInt(vm, svs.clients[i].edict);
			return;
		}
	}
	QC_ReturnInt(vm, 0);
}

static void bi_setsendneeded(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	uint32_t fl = (uint32_t)QC_PVEC(vm, 1)[0];
	int only = QC_PENT(vm, 2);
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state != CS_SPAWNED || c->bot || e >= c->estate_count)
			continue;
		if (only && c->edict != only)
			continue;
		c->pending[e] |= fl;
	}
}

static void bi_pointparticles(qcvm_t *vm)
{
	const float *org = QC_PVEC(vm, 1), *dir = vm->argc > 2 ? QC_PVEC(vm, 2) : NULL;
	LOCALMSG(m, 64);
	MSG_WriteByte(&m, svc_pointparticles);
	MSG_WriteVarUInt(&m, (uint32_t)BW_MAX(0, (int)QC_PFLOAT(vm, 0)));
	for (int i = 0; i < 3; i++)
		MSG_WriteCoord(&m, org[i]);
	for (int i = 0; i < 3; i++)
		MSG_WriteFloat(&m, dir ? dir[i] : 0);
	MSG_WriteFloat(&m, vm->argc > 3 ? QC_PFLOAT(vm, 3) : 1);
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state == CS_SPAWNED && !c->bot && client_sees(c, org))
			sb_write(&c->datagram, &m);
	}
}

static void bi_trailparticles(qcvm_t *vm)
{
	/* trailparticles(effect, entity, start, end) */
	const float *start = QC_PVEC(vm, 2), *end = QC_PVEC(vm, 3);
	LOCALMSG(m, 64);
	MSG_WriteByte(&m, svc_trailparticles);
	MSG_WriteVarUInt(&m, (uint32_t)BW_MAX(0, (int)QC_PFLOAT(vm, 0)));
	MSG_WriteEntity(&m, QC_PENT(vm, 1));
	for (int i = 0; i < 3; i++)
		MSG_WriteCoord(&m, start[i]);
	for (int i = 0; i < 3; i++)
		MSG_WriteCoord(&m, end[i]);
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state == CS_SPAWNED && !c->bot && (client_sees(c, start) || client_sees(c, end)))
			sb_write(&c->datagram, &m);
	}
}

static void write_sound(int e, int chan, int idx, float vol, float atten, float pitch, int flags, const float *org)
{
	LOCALMSG(m, 64);
	MSG_WriteByte(&m, svc_sound);
	MSG_WriteEntity(&m, e);
	MSG_WriteByte(&m, chan & 0xff);
	MSG_WriteVarUInt(&m, (uint32_t)idx);
	MSG_WriteByte(&m, (int)BW_MAX(0.0f, BW_MIN(vol * 255, 255.0f)));
	MSG_WriteByte(&m, (int)BW_MAX(0.0f, BW_MIN(atten * 64, 255.0f)));
	MSG_WriteByte(&m, (int)BW_MAX(0.0f, BW_MIN(pitch, 255.0f)));
	MSG_WriteByte(&m, flags & 0xff);
	for (int i = 0; i < 3; i++)
		MSG_WriteCoord(&m, org[i]);
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state == CS_SPAWNED && !c->bot)
			sb_write(&c->datagram, &m);
	}
}

static void bi_sound(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	const char *s = QC_PSTR(vm, 2);
	int idx = SV_SoundIndex(s, false);
	float org[3];
	float *eo = &QC_EdictFields(vm, e)[F.origin].f, *mins = &QC_EdictFields(vm, e)[F.mins].f,
	      *maxs = &QC_EdictFields(vm, e)[F.maxs].f;
	if (!idx) {
		if (*s)
			Con_DPrintf("sound: %s was not precached\n", s);
		return;
	}
	for (int i = 0; i < 3; i++)
		org[i] = eo[i] + (mins[i] + maxs[i]) * 0.5f;
	write_sound(e, (int)QC_PFLOAT(vm, 1), idx, vm->argc > 3 ? QC_PFLOAT(vm, 3) : 1, vm->argc > 4 ? QC_PFLOAT(vm, 4) : 1,
	            vm->argc > 5 ? QC_PFLOAT(vm, 5) : 100, vm->argc > 6 ? (int)QC_PFLOAT(vm, 6) : 0, org);
}

static void bi_pointsound(qcvm_t *vm)
{
	int idx = SV_SoundIndex(QC_PSTR(vm, 1), false);
	if (idx)
		write_sound(0, 0, idx, QC_PFLOAT(vm, 2), QC_PFLOAT(vm, 3), 100, 0, QC_PVEC(vm, 0));
}

static void bi_setpause(qcvm_t *vm) { SV_SetPaused(QC_PFLOAT(vm, 0) != 0); }

void SV_RunStandardPlayerPhysics(int e);
static void bi_runstandardplayerphysics(qcvm_t *vm) { SV_RunStandardPlayerPhysics(QC_PENT(vm, 0)); }

const qc_builtin_t sv_client_builtins[] = {
	{"WriteByte", bi_WriteByte}, {"WriteChar", bi_WriteChar}, {"WriteShort", bi_WriteShort},
	{"WriteLong", bi_WriteLong}, {"WriteCoord", bi_WriteCoord}, {"WriteAngle", bi_WriteAngle},
	{"WriteFloat", bi_WriteFloat}, {"WriteInt", bi_WriteInt}, {"WriteUInt", bi_WriteInt},
	{"WriteEntity", bi_WriteEntity}, {"WriteInt64", bi_WriteInt64}, {"WriteUInt64", bi_WriteInt64},
	{"WriteDouble", bi_WriteDouble}, {"WriteString", bi_WriteString},
	{"WriteUnterminatedString", bi_WriteUnterminatedString}, {"multicast", bi_multicast},
	{"pointparticles", bi_pointparticles}, {"trailparticles", bi_trailparticles},
	{"sprint", bi_sprint}, {"bprint", bi_bprint}, {"centerprint", bi_centerprint}, {"stuffcmd", bi_stuffcmd},
	{"stuffcmdflags", bi_stuffcmdflags}, {"clientcommand", bi_clientcommand}, {"infokey", bi_infokey},
	{"infokeyf", bi_infokeyf}, {"forceinfokey", bi_forceinfokey}, {"setspawnparms", bi_setspawnparms},
	{"clienttype", bi_clienttype}, {"spawnclient", bi_spawnclient}, {"dropclient", bi_dropclient},
	{"checkclient", bi_checkclient}, {"setsendneeded", bi_setsendneeded}, {"sound", bi_sound},
	{"pointsound", bi_pointsound}, {"setpause", bi_setpause},
	{"runstandardplayerphysics", bi_runstandardplayerphysics},
	{NULL, NULL}};

/* --- setup -------------------------------------------------------------- */

static void SV_Kick_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: kick <slot|name>\n");
		return;
	}
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state != CS_FREE && (atoi(argv[1]) == i + 1 || !Str_ICmp(c->name, argv[1]))) {
			SV_DropClient(c, "kicked");
			return;
		}
	}
	Con_Printf("no such client\n");
}

static void SV_Clients_f(int argc, const char **argv)
{
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state == CS_FREE)
			continue;
		Con_Printf("%2d %-16s %-8s %s rtt %dms loss %d%% in %.0fB/s out %.0fB/s\n", i + 1, c->name,
		           c->state == CS_SPAWNED ? "spawned" : "loading", c->bot ? "bot" : NET_AdrToString(&c->chan.remote),
		           (int)(c->chan.rtt * 1000), (int)(c->chan.loss * 100), c->chan.bytes_in_sec, c->chan.bytes_out_sec);
	}
}

bool SV_ConsoleSay(const char *msg)
{
	char text[1024];
	if (!sv.active)
		return false;
	snprintf(text, sizeof(text), "console: %s\n", msg);
	Con_Print(text);
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].state == CS_SPAWNED)
			client_print(&svs.clients[i], 3, text);
	return true;
}

static void SV_Say_f(int argc, const char **argv)
{
	char text[1024];
	snprintf(text, sizeof(text), "console: %s\n", Cmd_Args());
	Con_Print(text);
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].state == CS_SPAWNED)
			client_print(&svs.clients[i], 3, text);
}

/* CVAR_SERVERINFO cvars are mirrored into serverinfo, and changes are sent on */
void SV_SyncServerinfoCvars(bool broadcast)
{
	for (cvar_t *v = Cvar_First(); v; v = v->next) {
		char cur[1024];
		if (!(v->flags & CVAR_SERVERINFO))
			continue;
		SV_InfoGet(sv.serverinfo, v->name, cur, sizeof(cur));
		if (!strcmp(cur, v->string))
			continue;
		SV_InfoSet(sv.serverinfo, sizeof(sv.serverinfo), v->name, v->string);
		if (broadcast)
			SV_BroadcastServerinfo(v->name, v->string);
	}
}

void SV_InitClients(void)
{
	sv_maxclients = Cvar_Get("maxplayers", "8", CVAR_SERVERINFO, "Player slots (takes effect on the next map)");
	sv_timeout = Cvar_Get("sv_timeout", "30", 0, "Seconds without packets before a client is dropped");
	sv_unlag = Cvar_Get("sv_unlag", "1", CVAR_SERVERINFO, "Rewind other players to what a client saw when running its commands");
	sv_maxunlag = Cvar_Get("sv_maxunlag", "0.5", 0, "Longest rewind for lag compensation, seconds");
	sv_minupdaterate = Cvar_Get("sv_minupdaterate", "10", 0, "Lowest snapshot rate a client may ask for");
	sv_maxupdaterate = Cvar_Get("sv_maxupdaterate", "100", 0, "Highest snapshot rate a client may ask for");
	sv_maxrate = Cvar_Get("sv_maxrate", "0", 0, "Upper bound on client rate in bytes per second, 0 for none");
	sv_port = Cvar_Get("sv_port", "27500", 0, "UDP port for incoming connections");
	hostname = Cvar_Get("hostname", "Bulwark server", CVAR_SERVERINFO, "Server name shown in browsers");
	SV_InitPMove();
	Cmd_Add("kick", SV_Kick_f);
	Cmd_Add("clients", SV_Clients_f);
	Cmd_Add("say", SV_Say_f);
}

/* Called by SV_SpawnServer before the progs runs, so the slot count is fixed
 * for the whole map. Slots are only resized when nobody is connected. */
void SV_SetupClientSlots(void)
{
	int want = BW_MAX(1, BW_MIN(255, sv_maxclients->ivalue));
	bool anyone = false;
	for (int i = 0; i < svs.maxclients; i++)
		anyone |= svs.clients[i].state != CS_FREE;
	if (!svs.clients || (!anyone && want != svs.maxclients)) {
		Mem_Free(svs.clients);
		svs.clients = Mem_Alloc(sizeof(client_t) * (size_t)want);
		svs.maxclients = want;
	}
	if (svs.maxclients > 1 && !NET_ServerPort())
		NET_OpenServer(sv_port->ivalue);
	/* single player games run over loopback only */
	if (svs.maxclients == 1 && NET_ServerPort())
		NET_CloseServer();
}

/* SetChangeParms for every connected player before a level change */
void SV_SaveSpawnParms(void)
{
	qcvm_t *vm = sv.vm;
	if (!vm)
		return;
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state != CS_SPAWNED)
			continue;
		QC_SetGInt(vm, G.self, c->edict);
		if (QC_FindFunction(vm, "SetChangeParms"))
			QC_ExecuteByName(vm, "SetChangeParms");
		copy_globals_to_parms(c);
	}
}
