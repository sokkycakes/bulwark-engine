/* Bulwark engine - server console and admin commands: restarting and
 * changing maps, pausing, player management, address bans, passwords and
 * remote console, the cheat commands players can use when the game leaves
 * them to the engine, and entity tools after Source's ent_* commands.
 * Names and behaviour follow the public documentation of the Quake-family
 * and Source consoles. ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define F sv.vm->fofs
#define G sv.vm->gofs

#define FL_FLY 1
#define FL_GODMODE 64
#define FL_NOTARGET 128

static cvar_t *sv_password, *rcon_password, *sv_cheats, *sv_rcon_maxfailures;

static bool need_server(void)
{
	if (!sv.active)
		Con_Printf("no map running\n");
	return sv.active;
}

/* a client by slot number (1-based), name, or "#userid" */
static client_t *find_client(const char *s)
{
	int slot = atoi(s[0] == '#' ? s + 1 : s);
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state == CS_FREE)
			continue;
		if ((slot == i + 1 && strspn(s[0] == '#' ? s + 1 : s, "0123456789") == strlen(s[0] == '#' ? s + 1 : s)) ||
		    !Str_ICmp(c->name, s))
			return c;
	}
	Con_Printf("no player \"%s\"\n", s);
	return NULL;
}

/* --- maps ----------------------------------------------------------------------------------- */

static void Killserver_f(int argc, const char **argv)
{
	if (!need_server())
		return;
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].state != CS_FREE)
			SV_DropClient(&svs.clients[i], "server shut down");
	SV_Shutdown();
	Con_Printf("server shut down\n");
}

/* map_restart/restart/reload [seconds]: the same map again */
static void MapRestart_f(int argc, const char **argv)
{
	char map[64];
	if (!need_server())
		return;
	if (argc > 1 && atof(argv[1]) > 0) {
		Cbuf_AddDeferred(atof(argv[1]), "map_restart");
		return;
	}
	Str_Copy(map, sv.mapname, sizeof(map));
	for (int i = 0; i < svs.maxclients; i++)
		svs.clients[i].have_parms = false;
	SV_SpawnServer(map);
}

/* devmap turns cheats on, spmap starts a single player game */
static void MapVariant_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: %s <map>\n", argv[0]);
		return;
	}
	if (!Str_ICmp(argv[0], "devmap") || !Str_ICmp(argv[0], "spdevmap"))
		Cvar_Set("sv_cheats", "1");
	if (!Str_ICmp(argv[0], "spmap") || !Str_ICmp(argv[0], "spdevmap"))
		Cvar_Set("maxplayers", "1");
	Cbuf_InsertText(va("map \"%s\"\n", argv[1]));
}

/* changelevel2 <map> [landmark]: Half-Life's transition; the landmark is the game's */
static void Changelevel2_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: changelevel2 <map> [landmark]\n");
		return;
	}
	Cbuf_InsertText(va("changelevel \"%s\"\n", argv[1]));
}

/* --- pause ------------------------------------------------------------------------------------ */

/* asks the game (SV_ShouldPause) before changing; requester is the
 * player's entity, 0 for the console */
bool SV_RequestPause(int requester, bool want)
{
	int f;
	if (!sv.active)
		return false;
	f = QC_FindFunction(sv.vm, "SV_ShouldPause");
	if (f) {
		QC_SetGInt(sv.vm, G.self, requester);
		sv.vm->globals[OFS_PARM(0)].f = want;
		sv.vm->argc = 1;
		QC_Execute(sv.vm, f);
		if (sv.vm->globals[OFS_RETURN].f == 0)
			return false;
	} else if (requester && !Cvar_Value("pausable") && svs.maxclients > 1) {
		return false;
	}
	SV_SetPaused(want);
	SV_BroadcastPrint(2, want ? "game paused\n" : "game unpaused\n");
	return true;
}

static void Pause_f(int argc, const char **argv)
{
	bool want = !Str_ICmp(argv[0], "unpause") ? false
	            : !Str_ICmp(argv[0], "setpause") ? true
	            : !sv.paused;
	if (need_server() && !SV_RequestPause(0, want))
		Con_Printf("the game didn't allow that\n");
}

/* --- players --------------------------------------------------------------------------------- */

static void Users_f(int argc, const char **argv)
{
	int n = 0;
	if (!need_server())
		return;
	Con_Printf("slot name             address               ping  state\n");
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state == CS_FREE)
			continue;
		Con_Printf("%4d %-16s %-21s %4d  %s\n", i + 1, c->name, c->bot ? "bot" : NET_AdrToString(&c->chan.remote),
		           (int)(c->chan.rtt * 1000), c->state == CS_SPAWNED ? "active" : "connecting");
		n++;
	}
	Con_Printf("%d of %d slots used\n", n, svs.maxclients);
}

static void Kickall_f(int argc, const char **argv)
{
	if (!need_server())
		return;
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].state != CS_FREE && !svs.clients[i].bot)
			SV_DropClient(&svs.clients[i], argc > 1 ? Cmd_Args() : "kicked");
}

/* kickid/clientkick <slot> [reason] */
static void KickId_f(int argc, const char **argv)
{
	client_t *c;
	if (argc < 2) {
		Con_Printf("usage: %s <slot> [reason]\n", argv[0]);
		return;
	}
	if (need_server() && (c = find_client(argv[1])))
		SV_DropClient(c, argc > 2 ? argv[2] : "kicked");
}

/* stuffcmd <player> <text>: run console text on a player's game */
static void Stuffcmd_f(int argc, const char **argv)
{
	client_t *c;
	char text[2048];
	if (argc < 3) {
		Con_Printf("usage: stuffcmd <player> <text>\n");
		return;
	}
	if (!need_server() || !(c = find_client(argv[1])))
		return;
	snprintf(text, sizeof(text), "%s\n", argc == 3 ? argv[2] : Cmd_Args() + strlen(argv[1]) + 1);
	SV_StuffText(c, text);
}

/* tell <player> <text>: a private console message */
static void Tell_f(int argc, const char **argv)
{
	client_t *c;
	if (argc < 3) {
		Con_Printf("usage: tell <player> <text>\n");
		return;
	}
	if (need_server() && (c = find_client(argv[1])))
		SV_ClientPrint(c, 3, va("console (private): %s\n", argv[2]));
}

/* localinfo [key [value]]: server-side settings games can read */
static void Localinfo_f(int argc, const char **argv)
{
	if (argc < 2) {
		const char *p = sv.localinfo;
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
		return;
	}
	if (argc == 2) {
		char v[512];
		Con_Printf("%s\n", SV_InfoGet(sv.localinfo, argv[1], v, sizeof(v)));
		return;
	}
	SV_InfoSet(sv.localinfo, sizeof(sv.localinfo), argv[1], argv[2]);
}

/* sv_serverinfo <key> <value>: publish a key in serverinfo */
bool SV_SetServerinfoKey(const char *key, const char *value)
{
	if (!sv.active)
		return false;
	SV_InfoSet(sv.serverinfo, sizeof(sv.serverinfo), key, value);
	SV_BroadcastServerinfo(key, value);
	return true;
}

static void Serverinfo_f(int argc, const char **argv)
{
	if (argc < 3) {
		Con_Printf("usage: %s <key> <value>\n", argv[0]);
		return;
	}
	if (need_server())
		SV_SetServerinfoKey(argv[1], argv[2]);
}

/* --- bans ---------------------------------------------------------------------------------------- */

typedef struct {
	netadr_t adr;
	int bits;       /* prefix length that must match */
	double expires; /* 0 for permanent, else Sys_Seconds() deadline */
	char reason[64];
} ban_t;

static ban_t *bans;
static int numbans;

/* "a.b.c.d", "a.b.c.d/bits", or Quake's "a.b.c" (missing parts are wildcards) */
static bool parse_mask(const char *s, netadr_t *a, int *bits)
{
	char buf[128];
	const char *slash = strchr(s, '/');
	int parts = 0;
	Str_Copy(buf, s, sizeof(buf));
	if (slash)
		buf[slash - s] = 0;
	memset(a, 0, sizeof(*a));
	if (strchr(buf, ':')) {
		if (!NET_StringToAdr(buf, a, 0))
			return false;
		*bits = slash ? atoi(slash + 1) : 128;
		return a->type == NA_IP6;
	}
	for (char *p = buf; *p && parts < 4;) {
		a->ip[parts++] = (uint8_t)atoi(p);
		p = strchr(p, '.');
		if (!p)
			break;
		p++;
	}
	if (!parts)
		return false;
	a->type = NA_IP4;
	*bits = slash ? atoi(slash + 1) : parts * 8;
	return true;
}

static bool mask_matches(const netadr_t *mask, int bits, const netadr_t *a)
{
	int bytes = bits / 8, rem = bits % 8;
	if (mask->type != a->type)
		return false;
	if (memcmp(mask->ip, a->ip, (size_t)bytes))
		return false;
	if (rem && ((mask->ip[bytes] ^ a->ip[bytes]) & (0xff << (8 - rem))))
		return false;
	return true;
}

static const char *mask_string(const ban_t *b)
{
	static char s[96];
	if (b->adr.type == NA_IP4)
		snprintf(s, sizeof(s), "%d.%d.%d.%d/%d", b->adr.ip[0], b->adr.ip[1], b->adr.ip[2], b->adr.ip[3], b->bits);
	else
		snprintf(s, sizeof(s), "%s/%d", NET_AdrToString(&b->adr), b->bits);
	return s;
}

static void expire_bans(void)
{
	double now = Sys_Seconds();
	for (int i = 0; i < numbans;)
		if (bans[i].expires && bans[i].expires < now)
			bans[i] = bans[--numbans];
		else
			i++;
}

bool SV_CheckBanned(const netadr_t *from, char *reason, size_t reasonsize)
{
	if (from->type == NA_LOOPBACK)
		return false;
	expire_bans();
	for (int i = 0; i < numbans; i++)
		if (mask_matches(&bans[i].adr, bans[i].bits, from)) {
			Str_Copy(reason, bans[i].reason[0] ? bans[i].reason : "banned", reasonsize);
			return true;
		}
	return false;
}

static void add_ban(const netadr_t *a, int bits, double minutes, const char *reason)
{
	ban_t *b;
	for (int i = 0; i < numbans; i++)
		if (bans[i].bits == bits && mask_matches(&bans[i].adr, bits, a)) {
			b = &bans[i];
			goto fill;
		}
	bans = Mem_Realloc(bans, sizeof(ban_t) * (numbans + 1));
	b = &bans[numbans++];
fill:
	memset(b, 0, sizeof(*b));
	b->adr = *a;
	b->bits = bits;
	b->expires = minutes > 0 ? Sys_Seconds() + minutes * 60 : 0;
	Str_Copy(b->reason, reason ? reason : "", sizeof(b->reason));
	Con_Printf("banned %s%s\n", mask_string(b), minutes > 0 ? va(" for %g minutes", minutes) : "");
	/* anyone already on from there goes */
	for (int i = 0; sv.active && i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state != CS_FREE && !c->bot && mask_matches(a, bits, &c->chan.remote))
			SV_DropClient(c, b->reason[0] ? b->reason : "banned");
	}
}

/* addip <minutes> <ip> (Source), or addip <ip> (Quake) */
static void Addip_f(int argc, const char **argv)
{
	netadr_t a;
	int bits;
	const char *ip = argc > 2 ? argv[2] : argc > 1 ? argv[1] : NULL;
	double minutes = argc > 2 ? atof(argv[1]) : 0;
	if (!ip || !parse_mask(ip, &a, &bits)) {
		Con_Printf("usage: addip <minutes> <ip[/bits]>\n");
		return;
	}
	add_ban(&a, bits, minutes, argc > 3 ? argv[3] : NULL);
}

/* banip <ip[/bits]> [reason] [seconds] (FTE's order) */
static void Banip_f(int argc, const char **argv)
{
	netadr_t a;
	int bits;
	if (argc < 2 || !parse_mask(argv[1], &a, &bits)) {
		Con_Printf("usage: banip <ip[/bits]> [reason] [seconds]\n");
		return;
	}
	add_ban(&a, bits, argc > 3 ? atof(argv[3]) / 60 : 0, argc > 2 ? argv[2] : NULL);
}

/* ban <player> [minutes] [reason], banid <minutes> <slot> [kick] */
static void Ban_f(int argc, const char **argv)
{
	client_t *c;
	bool source = !Str_ICmp(argv[0], "banid");
	if (argc < (source ? 3 : 2)) {
		Con_Printf(source ? "usage: banid <minutes> <slot> [kick]\n" : "usage: ban <player> [minutes] [reason]\n");
		return;
	}
	if (!need_server() || !(c = find_client(argv[source ? 2 : 1])))
		return;
	if (c->bot) {
		Con_Printf("%s is a bot\n", c->name);
		return;
	}
	{
		netadr_t a = c->chan.remote;
		add_ban(&a, a.type == NA_IP6 ? 128 : 32, atof(source ? argv[1] : argc > 2 ? argv[2] : "0"),
		        !source && argc > 3 ? argv[3] : NULL);
	}
}

static void Removeip_f(int argc, const char **argv)
{
	netadr_t a;
	int bits;
	if (argc < 2) {
		Con_Printf("usage: %s <ip[/bits] or list number>\n", argv[0]);
		return;
	}
	/* a list number, as listip prints them */
	if (strspn(argv[1], "0123456789") == strlen(argv[1]) && !strchr(argv[1], '.')) {
		int n = atoi(argv[1]) - 1;
		if (n >= 0 && n < numbans) {
			Con_Printf("removed %s\n", mask_string(&bans[n]));
			bans[n] = bans[--numbans];
			return;
		}
	}
	if (!parse_mask(argv[1], &a, &bits)) {
		Con_Printf("bad address \"%s\"\n", argv[1]);
		return;
	}
	for (int i = 0; i < numbans; i++)
		if (bans[i].bits == bits && mask_matches(&bans[i].adr, bits, &a)) {
			Con_Printf("removed %s\n", mask_string(&bans[i]));
			bans[i] = bans[--numbans];
			return;
		}
	Con_Printf("%s isn't banned\n", argv[1]);
}

static void Listip_f(int argc, const char **argv)
{
	double now = Sys_Seconds();
	expire_bans();
	if (!numbans)
		Con_Printf("ban list is empty\n");
	for (int i = 0; i < numbans; i++)
		Con_Printf("%3d %-24s %s %s\n", i + 1, mask_string(&bans[i]),
		           bans[i].expires ? va("%.1f min", (bans[i].expires - now) / 60) : "permanent", bans[i].reason);
}

/* writeip: the permanent bans as addip lines, run again at startup */
static void Writeip_f(int argc, const char **argv)
{
	strbuf_t sb = {0};
	char path[1024];
	FILE *f;
	for (int i = 0; i < numbans; i++)
		if (!bans[i].expires)
			SB_Printf(&sb, "addip 0 %s \"%s\"\n", mask_string(&bans[i]), bans[i].reason);
	if (!FS_WritePath("banned_ip.cfg", path, sizeof(path)) || !(f = fopen(path, "wb"))) {
		Con_Printf("can't write banned_ip.cfg\n");
		SB_Free(&sb);
		return;
	}
	if (sb.len)
		fwrite(sb.data, 1, sb.len, f);
	fclose(f);
	SB_Free(&sb);
	Con_Printf("wrote banned_ip.cfg\n");
}

/* --- remote console -------------------------------------------------------------------------------- */

static struct {
	netadr_t adr;
	int failures;
	double until;
} rcon_fail[16];

/* "rcon <password> <command>" from anywhere: runs it here and sends back
 * what it printed. Too many wrong passwords from one address lock it out
 * for a minute. */
bool SV_HandleRcon(const netadr_t *from, const char *text)
{
	cmd_tokens_t *t = Mem_Alloc(sizeof(*t));
	strbuf_t out = {0};
	int slot = -1;
	double now = Sys_Seconds();
	Cmd_TokenizeInto(t, text);
	if (t->argc < 2 || Str_ICmp(t->argv[0], "rcon")) {
		Mem_Free(t);
		return false;
	}
	for (int i = 0; i < 16; i++)
		if (NET_CompareBaseAdr(&rcon_fail[i].adr, from))
			slot = i;
	if (slot >= 0 && rcon_fail[slot].until > now) {
		Mem_Free(t);
		return true;
	}
	if (!rcon_password->string[0] || strcmp(t->argv[1], rcon_password->string)) {
		if (slot < 0) {
			slot = 0;
			for (int i = 1; i < 16; i++)
				if (rcon_fail[i].until < rcon_fail[slot].until)
					slot = i;
			memset(&rcon_fail[slot], 0, sizeof(rcon_fail[slot]));
			rcon_fail[slot].adr = *from;
		}
		if (++rcon_fail[slot].failures >= BW_MAX(1, sv_rcon_maxfailures->ivalue)) {
			rcon_fail[slot].until = now + 60;
			rcon_fail[slot].failures = 0;
		}
		Con_Printf("bad rcon from %s\n", NET_AdrToString(from));
		NET_OutOfBandPrint(NS_SERVER, from, "print\n%s\n", rcon_password->string[0] ? "Bad rcon_password." : "No rcon_password is set on the server.");
		Mem_Free(t);
		return true;
	}
	Con_Printf("rcon from %s: %s\n", NET_AdrToString(from), text + t->argend[1]);
	Con_Redirect(&out);
	Cmd_ExecuteString(text + t->argend[1]);
	Con_Redirect(NULL);
	/* in pieces that fit a packet */
	for (size_t ofs = 0; ofs < out.len || (!out.len && !ofs); ofs += 1200) {
		size_t n = BW_MIN(out.len - ofs, 1200);
		NET_OutOfBandPrint(NS_SERVER, from, "print\n%.*s", (int)n, out.data ? out.data + ofs : "");
		if (!out.len)
			break;
	}
	SB_Free(&out);
	Mem_Free(t);
	return true;
}

/* --- cheats players can use ----------------------------------------------------------------------------- */

static bool cheats_allowed(client_t *c)
{
	if (sv_cheats->ivalue || svs.maxclients == 1)
		return true;
	SV_ClientPrint(c, 2, "Cheats are off on this server (sv_cheats 0).\n");
	return false;
}

static void toggle_flag(client_t *c, int flag, const char *name)
{
	int e = c->edict;
	int flags = (int)QC_EFloat(sv.vm, e, F.flags) ^ flag;
	QC_SetEFloat(sv.vm, e, F.flags, (float)flags);
	SV_ClientPrint(c, 2, va("%s %s\n", name, flags & flag ? "ON" : "OFF"));
}

static void toggle_movetype(client_t *c, int movetype, const char *name)
{
	int e = c->edict;
	bool on = (int)QC_EFloat(sv.vm, e, F.movetype) != movetype;
	QC_SetEFloat(sv.vm, e, F.movetype, on ? (float)movetype : (float)MOVETYPE_WALK);
	SV_ClientPrint(c, 2, va("%s %s\n", name, on ? "ON" : "OFF"));
}

/* The engine's answers to god, notarget, noclip, fly, give and setpos when
 * the game passes them back with clientcommand (Quake's semantics: flags
 * and movetypes on the player's entity). Returns false if not one of them. */
bool SV_ClientCheatCommand(client_t *c, int argc, const char **argv)
{
	const char *cmd = argv[0];
	int e = c->edict;
	if (Str_ICmp(cmd, "god") && Str_ICmp(cmd, "notarget") && Str_ICmp(cmd, "noclip") && Str_ICmp(cmd, "fly") &&
	    Str_ICmp(cmd, "give") && Str_ICmp(cmd, "setpos") && Str_ICmp(cmd, "buddha"))
		return false;
	if (!cheats_allowed(c))
		return true;
	if (!Str_ICmp(cmd, "god"))
		toggle_flag(c, FL_GODMODE, "godmode");
	else if (!Str_ICmp(cmd, "notarget"))
		toggle_flag(c, FL_NOTARGET, "notarget");
	else if (!Str_ICmp(cmd, "noclip"))
		toggle_movetype(c, MOVETYPE_NOCLIP, "noclip");
	else if (!Str_ICmp(cmd, "fly"))
		toggle_movetype(c, MOVETYPE_FLY, "flymode");
	else if (!Str_ICmp(cmd, "buddha")) {
		/* Source's buddha: damage stops at 1 health; the closest the
		 * engine can do is godmode */
		toggle_flag(c, FL_GODMODE, "buddha (godmode)");
	} else if (!Str_ICmp(cmd, "give")) {
		/* give health/h <amount>; anything else is the game's */
		if (argc > 1 && (!Str_ICmp(argv[1], "h") || !Str_ICmp(argv[1], "health")) && F.health >= 0) {
			float add = argc > 2 ? (float)atof(argv[2]) : 100;
			QC_SetEFloat(sv.vm, e, F.health, QC_EFloat(sv.vm, e, F.health) + add);
		} else {
			SV_ClientPrint(c, 2, "give: only health is known to the engine\n");
		}
	} else if (!Str_ICmp(cmd, "setpos")) {
		float org[3];
		if (argc < 4) {
			SV_ClientPrint(c, 2, "usage: setpos <x> <y> <z>\n");
			return true;
		}
		for (int i = 0; i < 3; i++)
			org[i] = (float)atof(argv[i + 1]);
		memcpy(&QC_EdictFields(sv.vm, e)[F.origin], org, sizeof(org));
		SV_LinkEdict(e, false);
	}
	return true;
}

/* --- entity tools ------------------------------------------------------------------------------------------- */

/* what player 1 is looking at */
static int picker(float *hitpos)
{
	qcval_t *f;
	float start[3], fwd[3], end[3];
	static const float zero[3];
	svtrace_t tr;
	if (svs.maxclients < 1 || svs.clients[0].state != CS_SPAWNED)
		return -1;
	f = QC_EdictFields(sv.vm, 1);
	for (int i = 0; i < 3; i++)
		start[i] = f[F.origin + i].f + (F.view_ofs >= 0 ? f[F.view_ofs + i].f : 0);
	AngleVectors(F.v_angle >= 0 ? &f[F.v_angle].f : &f[F.angles].f, fwd, NULL, NULL);
	for (int i = 0; i < 3; i++)
		end[i] = start[i] + fwd[i] * 8192;
	SV_Move(start, zero, zero, end, MOVE_NORMAL, 1, &tr);
	if (hitpos)
		VectorCopy(tr.endpos, hitpos);
	return tr.ent;
}

static const char *ent_string(int e, const char *field)
{
	qcdef_t *d = QC_FindField(sv.vm, field);
	static char buf[256];
	if (!d)
		return "";
	QC_FieldToString(sv.vm, e, d, buf, sizeof(buf));
	return buf;
}

/* an entity by number, "!picker", or by targetname/classname */
static int find_ent(const char *s, int after)
{
	if (!strcmp(s, "!picker"))
		return after ? -1 : picker(NULL);
	if (!strcmp(s, "!player") || !strcmp(s, "!self"))
		return after ? -1 : 1;
	if (strspn(s, "0123456789") == strlen(s) && *s) {
		int e = atoi(s);
		return !after && e < sv.vm->num_edicts && !sv.vm->edicts[e].free ? e : -1;
	}
	for (int e = after + 1; e < sv.vm->num_edicts; e++) {
		if (sv.vm->edicts[e].free)
			continue;
		if (!Str_ICmp(ent_string(e, "targetname"), s) || !Str_ICmp(ent_string(e, "classname"), s))
			return e;
	}
	return -1;
}

/* ent_dump/ent_info <entity>: every field that isn't empty */
static void EntDump_f(int argc, const char **argv)
{
	int e;
	if (!need_server())
		return;
	e = find_ent(argc > 1 ? argv[1] : "!picker", 0);
	if (e < 0) {
		Con_Printf("no entity \"%s\"\n", argc > 1 ? argv[1] : "under the crosshair");
		return;
	}
	Con_Printf("entity %d:\n", e);
	for (int i = 1; i < sv.vm->numfielddefs; i++) {
		qcdef_t *d = &sv.vm->fielddefs[i];
		char buf[512];
		const char *name = d->name ? d->name : "";
		size_t n = strlen(name);
		/* vectors also have _x _y _z fields; methods are only shown with "all" */
		if (n > 2 && name[n - 2] == '_' && strchr("xyz", name[n - 1]))
			continue;
		if ((d->type & 0xff) == 6 && !(argc > 2 && !Str_ICmp(argv[2], "all")))
			continue;
		if (!strncmp(name, "::", 2)) /* the compiler's names for shared field slots */
			continue;
		QC_FieldToString(sv.vm, e, d, buf, sizeof(buf));
		if (!buf[0] || !strcmp(buf, "0") || !strcmp(buf, "'0 0 0'") || !strcmp(buf, "0 0 0") ||
		    !strcmp(buf, "null") || !strcmp(buf, "world"))
			continue;
		Con_Printf("  %-24s %s\n", name, buf);
	}
}

static void remove_ent(int e)
{
	if (e <= svs.maxclients) {
		Con_Printf("won't remove a player's entity\n");
		return;
	}
	Con_Printf("removed %d (%s)\n", e, ent_string(e, "classname"));
	SV_UnlinkEdict(e);
	QC_FreeEdict(sv.vm, e, (float)sv.time);
}

static void EntRemove_f(int argc, const char **argv)
{
	int e;
	if (!need_server())
		return;
	e = find_ent(argc > 1 ? argv[1] : "!picker", 0);
	if (e <= 0)
		Con_Printf("no entity to remove\n");
	else
		remove_ent(e);
}

static void EntRemoveAll_f(int argc, const char **argv)
{
	int n = 0;
	if (argc < 2) {
		Con_Printf("usage: ent_remove_all <classname or targetname>\n");
		return;
	}
	if (!need_server())
		return;
	for (int e = find_ent(argv[1], 0); e > 0; e = find_ent(argv[1], e)) {
		if (e <= svs.maxclients)
			continue;
		remove_ent(e);
		n++;
	}
	Con_Printf("%d removed\n", n);
}

/* ent_create <classname> [key value ...]: spawned where player 1 is looking */
static void EntCreate_f(int argc, const char **argv)
{
	strbuf_t sb = {0};
	float hit[3] = {0, 0, 0};
	bool has_origin = false;
	int first = 0;
	if (argc < 2) {
		Con_Printf("usage: ent_create <classname> [key value ...]\n");
		return;
	}
	if (!need_server())
		return;
	SB_Printf(&sb, "{\n\"classname\" \"%s\"\n", argv[1]);
	for (int i = 2; i + 1 < argc; i += 2) {
		SB_Printf(&sb, "\"%s\" \"%s\"\n", argv[i], argv[i + 1]);
		has_origin |= !Str_ICmp(argv[i], "origin");
	}
	if (!has_origin) {
		picker(hit);
		SB_Printf(&sb, "\"origin\" \"%g %g %g\"\n", hit[0], hit[1], hit[2] + 1);
	}
	SB_AppendStr(&sb, "}\n");
	if (SV_SpawnEntitiesFromText(sb.data, &first) && first && !sv.vm->edicts[first].free)
		Con_Printf("created %d (%s)\n", first, argv[1]);
	else
		Con_Printf("the game didn't spawn a %s\n", argv[1]);
	SB_Free(&sb);
}

/* ent_teleport <entity>: to where player 1 is looking */
static void EntTeleport_f(int argc, const char **argv)
{
	float hit[3];
	int e;
	if (argc < 2 || !need_server()) {
		if (argc < 2)
			Con_Printf("usage: ent_teleport <entity>\n");
		return;
	}
	picker(hit);
	e = find_ent(argv[1], 0);
	if (e <= 0) {
		Con_Printf("no entity \"%s\"\n", argv[1]);
		return;
	}
	hit[2] += 1;
	memcpy(&QC_EdictFields(sv.vm, e)[F.origin], hit, sizeof(hit));
	SV_LinkEdict(e, false);
}

/* ent_setname <name> [entity]: sets the targetname */
static void EntSetname_f(int argc, const char **argv)
{
	int e, ofs;
	if (argc < 2 || !need_server())
		return;
	e = find_ent(argc > 2 ? argv[2] : "!picker", 0);
	ofs = QC_FieldOfs(sv.vm, "targetname");
	if (e <= 0 || ofs < 0) {
		Con_Printf("no entity, or the game has no targetname field\n");
		return;
	}
	QC_EdictFields(sv.vm, e)[ofs].i = QC_NewString(sv.vm, argv[1]);
	Con_Printf("%d is now named %s\n", e, argv[1]);
}

/* find_ent <text>: entities whose classname or targetname holds the text */
static void FindEnt_f(int argc, const char **argv)
{
	int n = 0;
	if (argc < 2 || !need_server())
		return;
	for (int e = 0; e < sv.vm->num_edicts; e++) {
		const char *cn, *tn;
		char cnb[128];
		if (sv.vm->edicts[e].free)
			continue;
		Str_Copy(cnb, ent_string(e, "classname"), sizeof(cnb));
		cn = cnb;
		tn = ent_string(e, "targetname");
		if (strstr(cn, argv[1]) || strstr(tn, argv[1])) {
			Con_Printf("%5d %-28s %s\n", e, cn, tn);
			n++;
		}
	}
	Con_Printf("%d found\n", n);
}

/* report_entities/edictcount: how many of each class */
static void ReportEntities_f(int argc, const char **argv)
{
	typedef struct {
		char name[64];
		int count;
	} tally_t;
	tally_t *t = NULL;
	int nt = 0, live = 0;
	if (!need_server())
		return;
	for (int e = 0; e < sv.vm->num_edicts; e++) {
		const char *cn;
		int i;
		if (sv.vm->edicts[e].free)
			continue;
		live++;
		cn = ent_string(e, "classname");
		for (i = 0; i < nt && strcmp(t[i].name, cn); i++)
			;
		if (i == nt) {
			t = Mem_Realloc(t, sizeof(tally_t) * (nt + 1));
			Str_Copy(t[nt].name, cn, sizeof(t[nt].name));
			t[nt++].count = 0;
		}
		t[i].count++;
	}
	for (int i = 0; i < nt; i++)
		Con_Printf("%5d %s\n", t[i].count, t[i].name[0] ? t[i].name : "(no classname)");
	Con_Printf("%d entities in use of %d slots (%d high water)\n", live, sv.vm->max_edicts, sv.vm->num_edicts);
	Mem_Free(t);
}

/* precaches: the model, sound and particle lists */
static void Precaches_f(int argc, const char **argv)
{
	if (!need_server())
		return;
	Con_Printf("models:\n");
	for (int i = 1; i < sv.nummodels; i++)
		Con_Printf("  %4d %s\n", i, sv.models[i]);
	Con_Printf("sounds:\n");
	for (int i = 1; i < sv.numsounds; i++)
		Con_Printf("  %4d %s\n", i, sv.sounds[i]);
	Con_Printf("particle effects:\n");
	for (int i = 0; i < sv.numparticles; i++)
		if (sv.particles[i])
			Con_Printf("  %4d %s\n", i, sv.particles[i]);
}

/* sv_saveentfile: the map's entity text to maps/<map>.ent */
static void SaveEntFile_f(int argc, const char **argv)
{
	char rel[128], path[1024];
	FILE *f;
	if (!need_server() || !sv.world || !sv.world->entities)
		return;
	snprintf(rel, sizeof(rel), "maps/%s.ent", sv.mapname);
	if (!FS_WritePath(rel, path, sizeof(path)) || !FS_CreatePath(path) || !(f = fopen(path, "wb"))) {
		Con_Printf("can't write %s\n", rel);
		return;
	}
	fputs(sv.world->entities, f);
	fclose(f);
	Con_Printf("wrote %s\n", rel);
}

/* sv_lightstyle <index> <pattern> */
static void Lightstyle_f(int argc, const char **argv)
{
	int i;
	if (argc < 3 || !need_server()) {
		if (argc < 3)
			Con_Printf("usage: sv_lightstyle <index> <pattern>\n");
		return;
	}
	i = atoi(argv[1]);
	if (i < 0 || i >= SV_MAX_LIGHTSTYLES)
		return;
	Mem_Free(sv.lightstyles[i]);
	sv.lightstyles[i] = Mem_Strdup(argv[2]);
	SV_BroadcastLightstyle(i, argv[2]);
}

void SV_InitCommands(void)
{
	sv_password = Cvar_Get("sv_password", "", 0, "Password players must set (setinfo password) to join");
	rcon_password = Cvar_Get("rcon_password", "", 0, "Remote console password; empty turns rcon off");
	sv_rcon_maxfailures = Cvar_Get("sv_rcon_maxfailures", "5", 0, "Wrong rcon passwords before an address is locked out for a minute");
	sv_cheats = Cvar_Find("sv_cheats");
	Cvar_Get("pausable", "1", CVAR_SERVERINFO, "Players may pause the game");

	Cmd_AddD("killserver", Killserver_f, "Stop the running server");
	Cmd_AddD("map_restart", MapRestart_f, "map_restart [seconds]: start the map over");
	Cmd_AddD("restart", MapRestart_f, "Start the map over");
	Cmd_AddD("reload", MapRestart_f, "Start the map over");
	Cmd_AddD("devmap", MapVariant_f, "devmap <map>: start a map with cheats on");
	Cmd_AddD("spmap", MapVariant_f, "spmap <map>: start a single player game");
	Cmd_AddD("spdevmap", MapVariant_f, "spdevmap <map>: single player with cheats");
	Cmd_AddD("gamemap", MapVariant_f, "gamemap <map>: start a map");
	Cmd_AddD("changelevel2", Changelevel2_f, "changelevel2 <map> [landmark]: move on to a map");
	Cmd_AddD("pause", Pause_f, "Pause or unpause the game");
	Cmd_AddD("setpause", Pause_f, "Pause the game");
	Cmd_AddD("unpause", Pause_f, "Unpause the game");
	Cmd_AddD("users", Users_f, "List the players");
	Cmd_AddD("listplayers", Users_f, "List the players");
	Cmd_AddD("kickall", Kickall_f, "kickall [reason]: kick every player");
	Cmd_AddD("kickid", KickId_f, "kickid <slot> [reason]: kick a player");
	Cmd_AddD("clientkick", KickId_f, "clientkick <slot> [reason]: kick a player");
	Cmd_AddD("stuffcmd", Stuffcmd_f, "stuffcmd <player> <text>: run commands on a player's game");
	Cmd_AddD("tell", Tell_f, "tell <player> <text>: a private message");
	Cmd_AddD("localinfo", Localinfo_f, "localinfo [key [value]]: server-side settings");
	Cmd_AddD("sv_serverinfo", Serverinfo_f, "sv_serverinfo <key> <value>: publish a server key");
	Cmd_AddD("addip", Addip_f, "addip <minutes> <ip[/bits]>: ban an address (0 minutes is permanent)");
	Cmd_AddD("banip", Banip_f, "banip <ip[/bits]> [reason] [seconds]: ban an address");
	Cmd_AddD("ban", Ban_f, "ban <player> [minutes] [reason]: ban a player's address");
	Cmd_AddD("banid", Ban_f, "banid <minutes> <slot>: ban a player's address");
	Cmd_AddD("removeip", Removeip_f, "removeip <ip or number>: lift a ban");
	Cmd_AddD("unban", Removeip_f, "unban <ip or number>: lift a ban");
	Cmd_AddD("removeid", Removeip_f, "removeid <number>: lift a ban");
	Cmd_AddD("listip", Listip_f, "List the bans");
	Cmd_AddD("banlist", Listip_f, "List the bans");
	Cmd_AddD("listid", Listip_f, "List the bans");
	Cmd_AddD("writeip", Writeip_f, "Save the permanent bans to banned_ip.cfg");
	Cmd_AddD("writeid", Writeip_f, "Save the permanent bans to banned_ip.cfg");
	Cmd_AddD("ent_dump", EntDump_f, "ent_dump [entity] [all]: an entity's fields (default: under the crosshair)");
	Cmd_AddD("ent_info", EntDump_f, "ent_info [entity]: an entity's fields");
	Cmd_AddD("ent_remove", EntRemove_f, "ent_remove [entity]: remove an entity (default: under the crosshair)");
	Cmd_AddD("ent_remove_all", EntRemoveAll_f, "ent_remove_all <class or name>: remove all matching entities");
	Cmd_AddD("ent_create", EntCreate_f, "ent_create <classname> [key value...]: spawn where you look");
	Cmd_AddD("ent_teleport", EntTeleport_f, "ent_teleport <entity>: move an entity to where you look");
	Cmd_AddD("ent_setname", EntSetname_f, "ent_setname <name> [entity]: set a targetname");
	Cmd_AddD("find_ent", FindEnt_f, "find_ent <text>: entities by classname or targetname");
	Cmd_AddD("report_entities", ReportEntities_f, "Entity counts by class");
	Cmd_AddD("edictcount", ReportEntities_f, "Entity counts by class");
	Cmd_AddD("precaches", Precaches_f, "List the precached models, sounds and effects");
	Cmd_AddD("sv_saveentfile", SaveEntFile_f, "Write the map's entity text to maps/<map>.ent");
	Cmd_AddD("sv_lightstyle", Lightstyle_f, "sv_lightstyle <index> <pattern>: change a light style");
	if (FS_FileExists("banned_ip.cfg"))
		Cbuf_AddText("exec banned_ip.cfg\n");
}
