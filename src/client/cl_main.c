/* Bulwark engine - client connection, server message parsing and timing.
 * ISC licence, see LICENSE. */
#include "client.h"
#include "console.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../render/render.h"
#include "../sound/sound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

clstate_t cl;

static cvar_t *cl_name, *cl_updaterate, *cl_rate, *cl_interp, *cl_cmdrate, *cl_timeout, *cl_shownet;
static cvar_t *net_fakelag, *net_fakeloss;
static char sent_userinfo[2048];
static infoblob_t localblobs[MAX_INFOBLOBS]; /* setinfoblob keys, sent once connected */
static bool blobsent[MAX_INFOBLOBS];

/* --- info strings (same format as the server's) ------------------------- */

static const char *info_get(const char *info, const char *key, char *out, size_t outsize)
{
	const char *p = info;
	size_t klen = strlen(key);
	out[0] = 0;
	while (*p == '\\') {
		const char *k = p + 1, *ke = strchr(k, '\\'), *v, *ve;
		if (!ke)
			break;
		v = ke + 1;
		ve = strchr(v, '\\');
		if (!ve)
			ve = v + strlen(v);
		if ((size_t)(ke - k) == klen && !strncmp(k, key, klen)) {
			size_t n = BW_MIN((size_t)(ve - v), outsize - 1);
			memcpy(out, v, n);
			out[n] = 0;
			return out;
		}
		p = ve;
	}
	return out;
}

static void info_set(char *info, size_t size, const char *key, const char *value)
{
	char rebuilt[8192];
	const char *p = info;
	size_t klen = strlen(key);
	if (strchr(key, '\\') || strchr(value, '\\'))
		return;
	rebuilt[0] = 0;
	while (*p == '\\') {
		const char *k = p + 1, *ke = strchr(k, '\\'), *v, *ve;
		if (!ke)
			break;
		v = ke + 1;
		ve = strchr(v, '\\');
		if (!ve)
			ve = v + strlen(v);
		if (!((size_t)(ke - k) == klen && !strncmp(k, key, klen))) {
			size_t l = strlen(rebuilt), n = (size_t)(ve - p);
			if (l + n < sizeof(rebuilt)) {
				memcpy(rebuilt + l, p, n);
				rebuilt[l + n] = 0;
			}
		}
		p = ve;
	}
	if (*value) {
		Str_Cat(rebuilt, "\\", sizeof(rebuilt));
		Str_Cat(rebuilt, key, sizeof(rebuilt));
		Str_Cat(rebuilt, "\\", sizeof(rebuilt));
		Str_Cat(rebuilt, value, sizeof(rebuilt));
	}
	Str_Copy(info, rebuilt, size);
}

const char *CL_PlayerKey(int slot, const char *key, char *out, size_t size)
{
	out[0] = 0;
	if (slot < 0 || slot >= CL_MAX_PLAYERS)
		return out;
	return info_get(cl.playerinfo[slot], key, out, size);
}

const char *CL_ServerKey(const char *key, char *out, size_t size) { return info_get(cl.serverinfo, key, out, size); }

static void build_userinfo(char *out, size_t size)
{
	out[0] = 0;
	for (cvar_t *v = Cvar_First(); v; v = v->next)
		if (v->flags & CVAR_USERINFO)
			info_set(out, size, v->name, v->string);
}

/* --- models -------------------------------------------------------------- */

void CL_SetModelBounds(int index)
{
	clmodel_t *m = &cl.models[index];
	const char *name = m->name;
	if (!name)
		return;
	if (name[0] == '*' && cl.world) {
		int sub = atoi(name + 1);
		if (sub > 0 && sub < cl.world->nummodels) {
			m->bsp = cl.world;
			m->submodel = sub;
			VectorCopy(cl.world->models[sub].mins, m->mins);
			VectorCopy(cl.world->models[sub].maxs, m->maxs);
		}
	} else if (!strcmp(name, cl.worldpath) && cl.world) {
		m->bsp = cl.world;
		VectorCopy(cl.world->models[0].mins, m->mins);
		VectorCopy(cl.world->models[0].maxs, m->maxs);
	}
}

int CL_ModelIndex(const char *name, bool add)
{
	if (!name || !*name)
		return 0;
	for (int i = 1; i < cl.nummodels; i++)
		if (cl.models[i].name && !strcmp(cl.models[i].name, name))
			return i;
	/* client-side precaches use negative indices in FTE's scheme; we keep a
	 * single table and simply append past the server's entries */
	if (!add || cl.nummodels >= CL_MAX_MODELS)
		return 0;
	cl.models[cl.nummodels].name = Mem_Strdup(name);
	CL_SetModelBounds(cl.nummodels);
	R_PrecacheModel(cl.nummodels);
	return cl.nummodels++;
}

static void set_precache(int kind, int index, const char *name)
{
	switch (kind) {
	case PRECACHE_MODEL:
		if (index <= 0 || index >= CL_MAX_MODELS)
			return;
		Mem_Free(cl.models[index].name);
		memset(&cl.models[index], 0, sizeof(cl.models[index]));
		cl.models[index].name = Mem_Strdup(name);
		if (index >= cl.nummodels)
			cl.nummodels = index + 1;
		CL_SetModelBounds(index);
		if (cl.state == ca_active)
			R_PrecacheModel(index);
		break;
	case PRECACHE_SOUND:
		if (index <= 0 || index >= CL_MAX_SOUNDS)
			return;
		Mem_Free(cl.sounds[index]);
		cl.sounds[index] = Mem_Strdup(name);
		if (index >= cl.numsounds)
			cl.numsounds = index + 1;
		if (cl.state == ca_active)
			S_PrecacheSound(name);
		break;
	case PRECACHE_PARTICLE:
		if (index <= 0 || index >= CL_MAX_PARTICLES)
			return;
		Mem_Free(cl.particles[index]);
		cl.particles[index] = Mem_Strdup(name);
		break;
	}
}

static void clear_map_state(void)
{
	CSQC_Shutdown();
	R_ClearMap();
	for (int i = 0; i < CL_MAX_MODELS; i++) {
		Mem_Free(cl.models[i].name);
		memset(&cl.models[i], 0, sizeof(cl.models[i]));
	}
	for (int i = 0; i < CL_MAX_SOUNDS; i++) {
		Mem_Free(cl.sounds[i]);
		cl.sounds[i] = NULL;
	}
	for (int i = 0; i < CL_MAX_PARTICLES; i++) {
		Mem_Free(cl.particles[i]);
		cl.particles[i] = NULL;
	}
	for (int i = 0; i < 256; i++) {
		Mem_Free(cl.lightstyles[i]);
		cl.lightstyles[i] = NULL;
	}
	for (int i = 0; i < CL_MAX_PLAYERS; i++)
		Blob_Clear(cl.playerblobs[i]);
	memset(blobsent, 0, sizeof(blobsent));
	BSP_Free(cl.world);
	cl.world = NULL;
	Mem_Free(cl.ents);
	cl.ents = NULL;
	cl.numents = 0;
	cl.nummodels = 1;
	cl.numsounds = 1;
	cl.worldloaded = false;
	cl.snapshots_received = 0;
}

/* --- connection ------------------------------------------------------------ */

void CL_Disconnect(const char *reason)
{
	if (cl.demoplayback)
		CL_DemoAbort();
	else if (cl.state >= ca_loading) {
		uint8_t b[2] = {clc_disconnect, 0};
		for (int i = 0; i < 3; i++)
			Netchan_Transmit(&cl.chan, b, 1);
		Netchan_Free(&cl.chan);
	}
	if (cl.state != ca_disconnected)
		Con_Printf("disconnected%s%s\n", reason ? ": " : "", reason ? reason : "");
	CL_DemoDisconnected();
	clear_map_state();
	S_StopAllSounds();
	cl.state = ca_disconnected;
	cl.paused = false;
	memset(cl.serverinfo, 0, sizeof(cl.serverinfo));
	memset(cl.playerinfo, 0, sizeof(cl.playerinfo));
	Str_Copy(cl.disconnect_reason, reason ? reason : "", sizeof(cl.disconnect_reason));
}

void CL_Connect(const char *address)
{
	CL_Disconnect(NULL);
	if (!NET_StringToAdr(address, &cl.server, PORT_SERVER)) {
		Con_Printf("bad server address %s\n", address);
		return;
	}
	Str_Copy(cl.servername, address, sizeof(cl.servername));
	cl.qport = (rand() & 0x7fff) + 1;
	cl.state = cl.server.type == NA_LOOPBACK ? ca_connecting : ca_challenging;
	cl.connect_time = -999;
	cl.connect_tries = 0;
	cl.challenge = 0;
	Con_Printf("connecting to %s...\n", NET_AdrToString(&cl.server));
}

static void check_connect(void)
{
	char userinfo[2048];
	if (cl.state != ca_challenging && cl.state != ca_connecting)
		return;
	if (cl.realtime - cl.connect_time < 1.0)
		return;
	if (++cl.connect_tries > 10) {
		CL_Disconnect("no response from server");
		return;
	}
	cl.connect_time = cl.realtime;
	if (cl.state == ca_challenging) {
		NET_OutOfBandPrint(NS_CLIENT, &cl.server, "getchallenge");
		return;
	}
	build_userinfo(userinfo, sizeof(userinfo));
	Str_Copy(sent_userinfo, userinfo, sizeof(sent_userinfo));
	NET_OutOfBandPrint(NS_CLIENT, &cl.server, "connect %d %d %d \"%s\"", PROTOCOL_VERSION, cl.qport, cl.challenge, userinfo);
}

static void connectionless(const netadr_t *from, uint8_t *data, size_t len)
{
	cmd_tokens_t *t = Mem_Alloc(sizeof(*t));
	char *text = Mem_Alloc(len - 3);
	memcpy(text, data + 4, len - 4);
	Cmd_TokenizeInto(t, text);
	if (!t->argc)
		goto done;
	if (!strcmp(t->argv[0], "challenge") && cl.state == ca_challenging && t->argc > 1) {
		cl.challenge = atoi(t->argv[1]);
		cl.state = ca_connecting;
		cl.connect_time = -999;
		cl.connect_tries = 0;
	} else if (!strcmp(t->argv[0], "accept") && cl.state == ca_connecting && NET_CompareAdr(from, &cl.server)) {
		Netchan_Setup(&cl.chan, NS_CLIENT, &cl.server, cl.qport, true);
		cl.chan.rate = cl_rate->ivalue;
		cl.state = ca_loading;
		Con_Printf("connected\n");
	} else if (!strcmp(t->argv[0], "reject")) {
		const char *why = strchr(text, ' ');
		Con_Printf("server refused connection:%s\n", why ? why : "");
		cl.state = ca_disconnected;
	} else if (!strcmp(t->argv[0], "print")) {
		/* rcon replies and other server messages */
		const char *body = strchr(text, '\n');
		Con_Printf("%s", body ? body + 1 : "");
	} else if (!strcmp(t->argv[0], "infoResponse")) {
		Con_Printf("%s: %s\n", NET_AdrToString(from), text + strlen("infoResponse"));
	}
done:
	Mem_Free(text);
	Mem_Free(t);
}

void CL_SendStringCmd(const char *s)
{
	uint8_t buf[4096];
	msg_t m;
	if (cl.state < ca_loading || cl.demoplayback)
		return;
	MSG_Init(&m, buf, sizeof(buf));
	MSG_WriteByte(&m, clc_stringcmd);
	MSG_WriteString(&m, s);
	if (!m.overflowed)
		Netchan_WriteReliable(&cl.chan, m.data, m.cursize);
}

void CL_SendEvent(const msg_t *body)
{
	uint8_t *buf;
	msg_t m;
	if (cl.state != ca_active || cl.demoplayback)
		return;
	buf = Mem_Alloc(body->cursize + 1);
	MSG_Init(&m, buf, body->cursize + 1);
	MSG_WriteByte(&m, clc_event);
	MSG_WriteData(&m, body->data, body->cursize);
	Netchan_WriteReliable(&cl.chan, m.data, m.cursize);
	Mem_Free(buf);
}

/* unknown commands go to the server, like the console in Quake and Source */
bool CL_ForwardToServer(const char *line, int argc, const char **argv)
{
	if (cl.state < ca_loading || !argc || cl.demoplayback)
		return false;
	CL_SendStringCmd(line);
	return true;
}

/* --- server messages ------------------------------------------------------- */

static void load_world(void)
{
	BSP_Free(cl.world);
	cl.world = BSP_Load(cl.worldpath);
	if (!cl.world)
		Con_Printf("^3couldn't load %s for collision\n", cl.worldpath);
	for (int i = 1; i < cl.nummodels; i++)
		CL_SetModelBounds(i);
}

static void parse_serverdata(msg_t *m)
{
	uint32_t proto = MSG_ReadVarUInt(m);
	int spawncount;
	if (proto != PROTOCOL_VERSION) {
		CL_Disconnect("server uses a different protocol version");
		return;
	}
	spawncount = MSG_ReadLong(m);
	clear_map_state();
	cl.spawncount = spawncount;
	cl.tickrate = MSG_ReadFloat(m);
	cl.maxclients = MSG_ReadByte(m);
	cl.playerslot = MSG_ReadByte(m);
	cl.playerent = MSG_ReadEntity(m);
	Str_Copy(cl.mapname, MSG_ReadString(m), sizeof(cl.mapname));
	Str_Copy(cl.worldpath, MSG_ReadString(m), sizeof(cl.worldpath));
	Str_Copy(cl.csprogsname, MSG_ReadString(m), sizeof(cl.csprogsname));
	cl.csprogscrc = (uint16_t)MSG_ReadUShort(m);
	MSG_ReadLong(m); /* size, for a future download */
	Str_Copy(cl.serverinfo, MSG_ReadString(m), sizeof(cl.serverinfo));
	cl.state = ca_loading;
	cl.servercommandframe = 0;
	cl.time = 0;
	Con_Printf("map %s\n", cl.mapname);
}

static void finish_signon(void)
{
	char cmd[64];
	load_world();
	R_NewMap();
	for (int i = 1; i < cl.numsounds; i++)
		if (cl.sounds[i])
			S_PrecacheSound(cl.sounds[i]);
	if (!CSQC_Load()) {
		CL_Disconnect("couldn't load the client game (csprogs.dat)");
		return;
	}
	CSQC_WorldLoaded();
	snprintf(cmd, sizeof(cmd), "begin %d", cl.spawncount);
	CL_SendStringCmd(cmd);
}

static void parse_snapshot(msg_t *m)
{
	uint32_t tick = (uint32_t)MSG_ReadLong(m);
	double t = MSG_ReadDouble(m);
	uint32_t ack = (uint32_t)MSG_ReadLong(m);
	if (tick < cl.servertick && cl.snapshots_received) {
		/* older than what we have (reordered): its entity data still
		 * applies, but must not move the clock backwards */
	} else {
		cl.servertick = tick;
		cl.servertime = t;
		cl.servertime_recv = cl.realtime;
		if (ack > cl.servercommandframe)
			cl.servercommandframe = ack;
	}
	cl.snapshots_received++;
	cl.lastsnapshot_real = cl.realtime;
	CSQC_ParseEntities(m);
}

static void parse_sound(msg_t *m)
{
	int ent = MSG_ReadEntity(m);
	int chan = MSG_ReadByte(m);
	uint32_t idx = MSG_ReadVarUInt(m);
	float vol = MSG_ReadByte(m) / 255.0f;
	float atten = MSG_ReadByte(m) / 64.0f;
	float pitch = (float)MSG_ReadByte(m);
	int flags = MSG_ReadByte(m);
	float org[3];
	for (int i = 0; i < 3; i++)
		org[i] = MSG_ReadCoord(m);
	if (idx > 0 && idx < (uint32_t)cl.numsounds && cl.sounds[idx])
		S_StartSound(ent, chan, cl.sounds[idx], org, vol, atten, pitch, flags);
}

static void parse_server_message(msg_t *m, bool reliable);
void CL_ParseServerMessage(msg_t *m, bool reliable) { parse_server_message(m, reliable); }

static void parse_server_message(msg_t *m, bool reliable)
{
	while (!m->badread && MSG_Remaining(m)) {
		int c = MSG_ReadByte(m);
		if (cl_shownet->ivalue)
			Con_Printf("svc %d at %zu\n", c, m->readpos - 1);
		switch (c) {
		case svc_nop:
			break;
		case svc_disconnect:
			CL_Disconnect(MSG_ReadString(m));
			return;
		case svc_print: {
			int level = MSG_ReadByte(m);
			const char *s = MSG_ReadString(m);
			if (!CSQC_Print(s, level))
				Con_Print(s);
			break;
		}
		case svc_centerprint: {
			const char *s = MSG_ReadString(m);
			if (!CSQC_CenterPrint(s))
				Con_Printf("%s\n", s);
			break;
		}
		case svc_stufftext:
			Cbuf_AddText(MSG_ReadString(m));
			break;
		case svc_serverdata:
			parse_serverdata(m);
			break;
		case svc_precache: {
			int kind = MSG_ReadByte(m);
			uint32_t idx = MSG_ReadVarUInt(m);
			set_precache(kind, (int)idx, MSG_ReadString(m));
			break;
		}
		case svc_lightstyle: {
			int i = MSG_ReadByte(m);
			Mem_Free(cl.lightstyles[i]);
			cl.lightstyles[i] = Mem_Strdup(MSG_ReadString(m));
			break;
		}
		case svc_serverinfo: {
			char key[256];
			Str_Copy(key, MSG_ReadString(m), sizeof(key));
			info_set(cl.serverinfo, sizeof(cl.serverinfo), key, MSG_ReadString(m));
			CSQC_ServerInfoChanged();
			break;
		}
		case svc_playerblob: {
			int slot = MSG_ReadByte(m);
			char key[64];
			uint32_t len;
			Str_Copy(key, MSG_ReadString(m), sizeof(key));
			len = MSG_ReadVarUInt(m);
			if (len > MAX_INFOBLOB_SIZE || m->readpos + len > m->cursize) {
				m->badread = true;
				break;
			}
			Blob_Set(cl.playerblobs[slot], key, m->data + m->readpos, (int)len);
			m->readpos += len;
			CSQC_PlayerInfoChanged(slot);
			break;
		}
		case svc_playerinfo: {
			int slot = MSG_ReadByte(m);
			char key[256];
			Str_Copy(key, MSG_ReadString(m), sizeof(key));
			if (!key[0]) {
				Str_Copy(cl.playerinfo[slot], MSG_ReadString(m), sizeof(cl.playerinfo[slot]));
				if (!cl.playerinfo[slot][0])
					Blob_Clear(cl.playerblobs[slot]); /* the player left */
			} else
				info_set(cl.playerinfo[slot], sizeof(cl.playerinfo[slot]), key, MSG_ReadString(m));
			CSQC_PlayerInfoChanged(slot);
			break;
		}
		case svc_signon: {
			int stage = MSG_ReadByte(m);
			if (stage == 1)
				finish_signon();
			else if (stage == 2 && cl.state == ca_loading) {
				cl.state = ca_active;
				Con_SetActive(false); /* the console gets out of the way once in game */
				cl.next_cmd = cl.realtime;
				cl.cmd_start = cl.realtime;
			}
			break;
		}
		case svc_snapshot:
			parse_snapshot(m);
			break;
		case svc_qcmessage: {
			uint32_t len = MSG_ReadVarUInt(m);
			if (MSG_Remaining(m) < len) {
				m->badread = true;
				break;
			}
			CSQC_ParseEvent(m, len);
			break;
		}
		case svc_setpause:
			cl.paused = MSG_ReadByte(m) != 0;
			break;
		case svc_sound:
			parse_sound(m);
			break;
		case svc_pointparticles: {
			int fx = (int)MSG_ReadVarUInt(m);
			float org[3], dir[3], count;
			for (int i = 0; i < 3; i++)
				org[i] = MSG_ReadCoord(m);
			for (int i = 0; i < 3; i++)
				dir[i] = MSG_ReadFloat(m);
			count = MSG_ReadFloat(m);
			R_PointParticles(fx, org, dir, count);
			break;
		}
		case svc_trailparticles: {
			int fx = (int)MSG_ReadVarUInt(m);
			float start[3], end[3];
			MSG_ReadEntity(m);
			for (int i = 0; i < 3; i++)
				start[i] = MSG_ReadCoord(m);
			for (int i = 0; i < 3; i++)
				end[i] = MSG_ReadCoord(m);
			R_TrailParticles(fx, start, end);
			break;
		}
		case svc_setangle:
			for (int i = 0; i < 3; i++)
				cl.viewangles[i] = MSG_ReadAngle(m);
			break;
		default:
			Con_Printf("^1bad server message %d\n", c);
			m->badread = true;
			break;
		}
		if (cl.state == ca_disconnected)
			return;
	}
	if (m->badread)
		Con_DPrintf("^3server message ended early (%s)\n", reliable ? "reliable" : "unreliable");
}

static void read_packets(void)
{
	static uint8_t buf[NET_MAX_RECV];
	netadr_t from;
	size_t len;
	while ((len = NET_GetPacket(NS_CLIENT, buf, sizeof(buf), &from)) > 0) {
		msg_t m;
		if (len >= 4 && !memcmp(buf, "\xff\xff\xff\xff", 4)) {
			connectionless(&from, buf, len);
			continue;
		}
		if (cl.state < ca_loading || !NET_CompareAdr(&from, &cl.chan.remote))
			continue;
		MSG_Init(&m, buf, sizeof(buf));
		m.cursize = len;
		if (!Netchan_Process(&cl.chan, &m))
			continue;
		cl.chan.numacked = cl.chan.numlost = 0;
		{
			msg_t rel;
			while (cl.state >= ca_loading && Netchan_NextReliable(&cl.chan, &rel)) {
				CL_DemoServerMessage(rel.data + rel.readpos, rel.cursize - rel.readpos, true);
				parse_server_message(&rel, true);
			}
		}
		if (cl.state >= ca_loading) {
			CL_DemoServerMessage(m.data + m.readpos, m.cursize - m.readpos, false);
			parse_server_message(&m, false);
		}
	}
}

/* --- frame ------------------------------------------------------------------ */

/* Keeps cl.time close to the server's clock: it runs at real-time speed,
 * nudged toward the newest snapshot's time projected forward. */
static void update_clock(double dt)
{
	double target;
	if (cl.state != ca_active || !cl.snapshots_received) {
		cl.time = cl.servertime;
		return;
	}
	if (cl.paused)
		return;
	target = cl.servertime + (cl.realtime - cl.servertime_recv);
	cl.time += dt;
	if (fabs(target - cl.time) > 0.25)
		cl.time = target;
	else
		cl.time += (target - cl.time) * BW_MIN(1.0, dt * 4);
}

/* --- binary user info ---------------------------------------------------------- */

static void send_blob(int i)
{
	const infoblob_t *b = &localblobs[i];
	msg_t m;
	if (cl.state < ca_loading || !b->key[0])
		return;
	MSG_Init(&m, Mem_Alloc((size_t)b->len + 128), (size_t)b->len + 128);
	MSG_WriteByte(&m, clc_userblob);
	MSG_WriteString(&m, b->key);
	MSG_WriteVarUInt(&m, (uint32_t)b->len);
	MSG_WriteData(&m, b->data, (size_t)b->len);
	if (!m.overflowed)
		Netchan_WriteReliable(&cl.chan, m.data, m.cursize);
	Mem_Free(m.data);
	blobsent[i] = true;
}

void CL_SetLocalBlob(const char *key, const void *data, int len)
{
	Blob_Set(localblobs, key, data, len);
	for (int i = 0; i < MAX_INFOBLOBS; i++)
		if (localblobs[i].key[0] && !Str_ICmp(localblobs[i].key, key))
			blobsent[i] = false;
}

const infoblob_t *CL_GetLocalBlob(const char *key) { return Blob_Get(localblobs, key); }

/* setinfoblob key file: a file's contents as a user info key (sprays) */
static void CL_SetInfoBlob_f(int argc, const char **argv)
{
	size_t len;
	uint8_t *d;
	if (argc < 3) {
		Con_Printf("usage: setinfoblob <key> <file>\n");
		return;
	}
	d = (uint8_t *)FS_LoadFile(argv[2], &len);
	if (!d) {
		Con_Printf("setinfoblob: couldn't read %s\n", argv[2]);
		return;
	}
	if (len > MAX_INFOBLOB_SIZE)
		Con_Printf("setinfoblob: %s is over %d bytes\n", argv[2], MAX_INFOBLOB_SIZE);
	else
		CL_SetLocalBlob(argv[1], d, (int)len);
	Mem_Free(d);
}

/* setinfo key value: a user info key, sent to the server */
static void CL_SetInfo_f(int argc, const char **argv)
{
	cvar_t *v;
	if (argc < 3) {
		Con_Printf("usage: setinfo <key> <value>\n");
		return;
	}
	v = Cvar_Find(argv[1]);
	if (!v)
		v = Cvar_Get(argv[1], "", CVAR_USERINFO | CVAR_USERCREATED, NULL);
	v->flags |= CVAR_USERINFO;
	Cvar_Set(argv[1], argv[2]);
}

static void check_userinfo(void)
{
	char now[2048];
	if (cl.state < ca_loading)
		return;
	for (int i = 0; i < MAX_INFOBLOBS; i++)
		if (!blobsent[i] && localblobs[i].key[0])
			send_blob(i);
	build_userinfo(now, sizeof(now));
	if (!strcmp(now, sent_userinfo))
		return;
	for (cvar_t *v = Cvar_First(); v; v = v->next) {
		char old[1024];
		if (!(v->flags & CVAR_USERINFO))
			continue;
		info_get(sent_userinfo, v->name, old, sizeof(old));
		if (strcmp(old, v->string)) {
			char cmd[1200];
			snprintf(cmd, sizeof(cmd), "setinfo \"%s\" \"%s\"", v->name, v->string);
			CL_SendStringCmd(cmd);
		}
	}
	Str_Copy(sent_userinfo, now, sizeof(sent_userinfo));
	if (cl.chan.rate != cl_rate->ivalue)
		cl.chan.rate = cl_rate->ivalue;
}

static void send_move(void)
{
	uint8_t buf[NET_MAX_PACKET];
	msg_t m;
	usercmd_t zero;
	int count;
	uint32_t newest = cl.clientcommandframe - 1;
	MSG_Init(&m, buf, sizeof(buf));
	memset(&zero, 0, sizeof(zero));
	count = (int)BW_MIN((uint32_t)(1 + MAX_BACKUP_CMDS), newest);
	if (count < 1) {
		Netchan_Transmit(&cl.chan, NULL, 0);
		return;
	}
	MSG_WriteByte(&m, clc_move);
	MSG_WriteByte(&m, count);
	MSG_WriteLong(&m, (int32_t)(uint32_t)(cl.time * 1000.0));
	MSG_WriteDouble(&m, cl.realtime);
	for (int i = 0; i < count; i++) {
		const usercmd_t *u = &cl.cmds[(newest - (uint32_t)(count - 1) + (uint32_t)i) % CL_CMD_BACKUP];
		const usercmd_t *from = i ? &cl.cmds[(newest - (uint32_t)(count - 1) + (uint32_t)i - 1) % CL_CMD_BACKUP] : &zero;
		int bits = 0;
		if (memcmp(u->angles, from->angles, sizeof(u->angles)))
			bits |= UCMD_ANGLES;
		if (memcmp(u->movevalues, from->movevalues, sizeof(u->movevalues)))
			bits |= UCMD_MOVE;
		if (u->buttons != from->buttons)
			bits |= UCMD_BUTTONS;
		if (u->impulse)
			bits |= UCMD_IMPULSE;
		if (u->weapon != from->weapon)
			bits |= UCMD_WEAPON;
		if (memcmp(u->cursor_screen, from->cursor_screen, sizeof(u->cursor_screen)) || u->cursor_entity != from->cursor_entity)
			bits |= UCMD_CURSOR;
		MSG_WriteLong(&m, (int32_t)u->sequence);
		MSG_WriteByte(&m, (int)lrintf(u->msec * 1000));
		MSG_WriteByte(&m, bits);
		if (bits & UCMD_ANGLES)
			for (int k = 0; k < 3; k++)
				MSG_WriteAngle(&m, u->angles[k]);
		if (bits & UCMD_MOVE)
			for (int k = 0; k < 3; k++)
				MSG_WriteShort(&m, (int)BW_MAX(-32767.0f, BW_MIN(u->movevalues[k], 32767.0f)));
		if (bits & UCMD_BUTTONS)
			MSG_WriteVarUInt(&m, u->buttons);
		if (bits & UCMD_IMPULSE)
			MSG_WriteByte(&m, u->impulse);
		if (bits & UCMD_WEAPON)
			MSG_WriteVarUInt(&m, u->weapon);
		if (bits & UCMD_CURSOR) {
			MSG_WriteFloat(&m, u->cursor_screen[0]);
			MSG_WriteFloat(&m, u->cursor_screen[1]);
			for (int k = 0; k < 3; k++)
				MSG_WriteFloat(&m, u->cursor_start[k]);
			for (int k = 0; k < 3; k++)
				MSG_WriteFloat(&m, u->cursor_end[k]);
			MSG_WriteEntity(&m, u->cursor_entity);
		}
	}
	Netchan_Transmit(&cl.chan, m.data, m.cursize);
}

void CL_Frame(double dt)
{
	cl.realtime += dt;
	cl.frametime = dt;
	if (cl.demoplayback) {
		CL_DemoReadPackets(dt);
		update_clock(dt);
		return;
	}
	NET_SetFakeConditions(net_fakelag->value, net_fakeloss->value);
	read_packets();
	check_connect();
	update_clock(dt);
	check_userinfo();

	if (cl.state >= ca_loading && cl.chan.remote.type != NA_LOOPBACK &&
	    cl.realtime - cl.chan.last_received > cl_timeout->value) {
		CL_Disconnect("connection timed out");
		return;
	}
	if (cl.state == ca_active) {
		/* commands at cl_cmdrate (the server tick rate by default) */
		double interval = 1.0 / BW_MAX(10.0f, cl_cmdrate->value > 0 ? cl_cmdrate->value : cl.tickrate);
		CL_UpdateCurrentCommand(cl.realtime);
		if (cl.realtime >= cl.next_cmd) {
			CL_BuildCommand(cl.realtime);
			cl.next_cmd = BW_MAX(cl.next_cmd + interval, cl.realtime - interval);
			if (Netchan_CanSend(&cl.chan))
				send_move();
		}
		CL_DemoFrame();
	} else if (cl.state == ca_loading) {
		/* keep acknowledging the signon stream */
		if (cl.realtime >= cl.next_cmd) {
			cl.next_cmd = cl.realtime + 0.05;
			Netchan_Transmit(&cl.chan, NULL, 0);
		}
	}
}

/* --- commands --------------------------------------------------------------- */

static void CL_Connect_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: connect <address>\n");
		return;
	}
	CL_Connect(argv[1]);
}

static void CL_Disconnect_f(int argc, const char **argv) { CL_Disconnect(NULL); }

static void CL_Reconnect_f(int argc, const char **argv)
{
	if (cl.servername[0])
		CL_Connect(cl.servername);
}

static void CL_Cmd_f(int argc, const char **argv)
{
	if (cl.state < ca_loading) {
		Con_Printf("not connected\n");
		return;
	}
	CL_SendStringCmd(Cmd_Args());
}

void CL_PrintServerinfo(void)
{
	const char *p = cl.serverinfo;
	while (*p == '\\') {
		const char *k = p + 1, *ke = strchr(k, '\\'), *v, *ve;
		if (!ke)
			break;
		v = ke + 1;
		ve = strchr(v, '\\');
		if (!ve)
			ve = v + strlen(v);
		Con_Printf("%-20.*s %.*s\n", (int)(ke - k), k, (int)(ve - v), v);
		p = ve;
	}
}

static void CL_Ping_f(int argc, const char **argv)
{
	netadr_t a;
	if (argc < 2 || !NET_StringToAdr(argv[1], &a, PORT_SERVER)) {
		Con_Printf("usage: pingserver <address>\n");
		return;
	}
	NET_OutOfBandPrint(NS_CLIENT, &a, "getinfo");
}

static void CL_Netstats_f(int argc, const char **argv)
{
	if (cl.state < ca_loading) {
		Con_Printf("not connected\n");
		return;
	}
	Con_Printf("rtt %dms, loss %d%%, in %.0f B/s, out %.0f B/s, %u dropped, snapshots %d\n", (int)(cl.chan.rtt * 1000),
	           (int)(cl.chan.loss * 100), cl.chan.bytes_in_sec, cl.chan.bytes_out_sec, cl.chan.dropped, cl.snapshots_received);
}

void CL_Init(void)
{
	memset(&cl, 0, sizeof(cl));
	cl.nummodels = 1;
	cl.numsounds = 1;
	cl_name = Cvar_Get("name", "player", CVAR_ARCHIVE | CVAR_USERINFO, "Your player name");
	cl_updaterate = Cvar_Get("cl_updaterate", "72", CVAR_ARCHIVE | CVAR_USERINFO, "Snapshots per second to ask the server for");
	cl_rate = Cvar_Get("rate", "131072", CVAR_ARCHIVE | CVAR_USERINFO, "Most bytes per second the server should send");
	cl_interp = Cvar_Get("cl_interp", "0.03", CVAR_ARCHIVE | CVAR_USERINFO,
	                     "How far behind the newest snapshot other entities are drawn, in seconds");
	cl_cmdrate = Cvar_Get("cl_cmdrate", "0", CVAR_ARCHIVE, "Commands sent per second, 0 for the server tick rate");
	cl_timeout = Cvar_Get("cl_timeout", "30", 0, "Seconds without server packets before giving up");
	cl_shownet = Cvar_Get("cl_shownet", "0", 0, "Print every server message type as it is parsed");
	net_fakelag = Cvar_Get("net_fakelag", "0", 0, "Added outgoing latency in milliseconds, for testing");
	net_fakeloss = Cvar_Get("net_fakeloss", "0", 0, "Percent of outgoing packets dropped, for testing");
	Cvar_Get("topcolor", "0", CVAR_ARCHIVE | CVAR_USERINFO, NULL);
	Cvar_Get("bottomcolor", "0", CVAR_ARCHIVE | CVAR_USERINFO, NULL);
	Cvar_Get("team", "", CVAR_USERINFO, NULL);
	Cvar_Get("model", "", CVAR_ARCHIVE | CVAR_USERINFO, NULL);
	Cmd_Add("connect", CL_Connect_f);
	Cmd_Add("setinfo", CL_SetInfo_f);
	Cmd_Add("setinfoblob", CL_SetInfoBlob_f);
	Cmd_Add("disconnect", CL_Disconnect_f);
	Cmd_Add("reconnect", CL_Reconnect_f);
	Cmd_Add("cmd", CL_Cmd_f);
	Cmd_Add("pingserver", CL_Ping_f);
	Cmd_Add("netstats", CL_Netstats_f);
	IN_Init();
}
