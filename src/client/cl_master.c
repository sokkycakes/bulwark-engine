/* Bulwark engine - finding servers and the ways of connecting to them:
 * the server browser's list (localservers, globalservers, quickconnect),
 * cl_transfer, connectbr and the other connect variants, qwurl and extra
 * client ports.
 *
 * Masters are asked with the DarkPlaces master protocol: "getservers
 * <game> <protocol> empty full" answered by "getserversResponse" and six
 * byte addresses after backslashes, ending with "\EOT". Each server is then
 * asked "getstatus <challenge>"; the time to its statusResponse is its
 * ping. The menu and client QC read the list through the hostcache
 * builtins, filtered and sorted as they ask. ISC licence, see LICENSE. */
#include "client.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/json.h"
#include "../net/protocol.h"
#include "../server/sv_local.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_MASTERS 8
#define MAX_HOSTS 1024

typedef struct {
	netadr_t adr;
	char info[1024];
	char name[64], map[64], game[64], mod[64];
	int players, maxplayers, bots, protocol;
	int ping; /* -1 until it answers */
	double sent;
	double sent_wall; /* Sys_Seconds() when asked: pings finer than a frame */
	char challenge[16];
	bool printed;
	char playerlines[1024]; /* the status lines: frags ping "name" */
	int src;                /* SRC_*: which of the browser's lists it's on */
	bool status;            /* the last answer was a status reply (has players) */
} host_t;

/* the server browser's lists (docs/spec/serverbrowser.md) */
enum { SRC_INTERNET = 1, SRC_LAN = 2, SRC_FAVORITE = 4, SRC_HISTORY = 8 };

static host_t *hosts;
static int numhosts;
static bool hostcache_dirty; /* answers came since the view was sorted */
static int masters_asked, masters_answered;
static bool print_replies; /* the console asked, so answers are printed */
static double lan_sent, lan_sent_wall;
static cvar_t *net_master[NUM_MASTERS];

static const char *const default_masters[] = {"master.frag-net.com:27950", "dpmaster.deathmask.net:27950",
                                              "dpmaster.tchr.no:27950", NULL};

static void CL_InitServerBrowser(void);

static struct {
	bool active;
	netadr_t adr;
	char name[256];
	double until;
} transfer;

static struct {
	bool active;
	char name[256];
} bestroute;

static struct {
	bool active;
	double at;
} quick;

/* --- the list ------------------------------------------------------------- */

static host_t *find_host(const netadr_t *a)
{
	for (int i = 0; i < numhosts; i++)
		if (NET_CompareAdr(&hosts[i].adr, a))
			return &hosts[i];
	return NULL;
}

static void query(host_t *h)
{
	snprintf(h->challenge, sizeof(h->challenge), "%08x", (unsigned)rand());
	h->sent = cl.realtime;
	h->sent_wall = Sys_Seconds();
	NET_OutOfBandPrint(NS_CLIENT, &h->adr, "getstatus %s", h->challenge);
}

static host_t *add_host(const netadr_t *a)
{
	host_t *h = find_host(a);
	if (h)
		return h;
	if (numhosts >= MAX_HOSTS)
		return NULL;
	if (!hosts)
		hosts = Mem_Alloc(sizeof(*hosts) * MAX_HOSTS);
	h = &hosts[numhosts++];
	memset(h, 0, sizeof(*h));
	h->adr = *a;
	h->ping = -1;
	return h;
}

static const char *ikey(const char *info, const char *key, char *out, size_t size)
{
	return SV_InfoGet(info, key, out, size);
}

static void parse_info(host_t *h, const char *info)
{
	char tmp[64];
	Str_Copy(h->info, info, sizeof(h->info));
	ikey(info, "hostname", h->name, sizeof(h->name));
	if (!ikey(info, "mapname", h->map, sizeof(h->map))[0])
		ikey(info, "map", h->map, sizeof(h->map));
	ikey(info, "gamename", h->game, sizeof(h->game));
	ikey(info, "modname", h->mod, sizeof(h->mod));
	h->players = atoi(ikey(info, "clients", tmp, sizeof(tmp)));
	h->bots = atoi(ikey(info, "bots", tmp, sizeof(tmp)));
	h->maxplayers = atoi(ikey(info, "sv_maxclients", tmp, sizeof(tmp)));
	if (!h->maxplayers)
		h->maxplayers = atoi(ikey(info, "maxclients", tmp, sizeof(tmp)));
	h->protocol = atoi(ikey(info, "protocol", tmp, sizeof(tmp)));
}

static void print_host(const host_t *h)
{
	Con_Printf("%-24s %-24.24s %-12.12s %2d/%-2d %4dms\n", NET_AdrToString(&h->adr), h->name[0] ? h->name : "?",
	           h->map, h->players, h->maxplayers, h->ping);
}

/* --- asking ---------------------------------------------------------------------- */

static int ask_masters(int argc, const char **argv)
{
	const char *game = Cvar_String("com_protocolname");
	int asked = 0;
	bool any = false;
	for (int i = 0; i < NUM_MASTERS; i++)
		any |= net_master[i]->string[0] != 0;
	const char *list[NUM_MASTERS + 8];
	int n = 0;
	if (argc > 1)
		for (int i = 1; i < argc && n < (int)BW_ARRAY_COUNT(list); i++)
			list[n++] = argv[i];
	else if (any)
		for (int i = 0; i < NUM_MASTERS; i++)
			if (net_master[i]->string[0])
				list[n++] = net_master[i]->string;
	if (!n)
		for (int i = 0; default_masters[i]; i++)
			list[n++] = default_masters[i];
	for (int i = 0; i < n; i++) {
		netadr_t a;
		if (!NET_StringToAdr(list[i], &a, 27950)) {
			Con_Printf("Failed to resolve master address \"%s\"\n", list[i]);
			continue;
		}
		NET_OutOfBandPrint(NS_CLIENT, &a, "getservers %s %d empty full", game, PROTOCOL_VERSION);
		NET_OutOfBandPrint(NS_CLIENT, &a, "getserversExt %s %d empty full ipv6", game, PROTOCOL_VERSION);
		asked++;
		masters_asked++;
	}
	return asked;
}

static char lan_challenge[16];

/* a broadcast getinfo; servers answer from their own addresses, matched by
 * the challenge */
static void ask_lan(void)
{
	netadr_t a;
	snprintf(lan_challenge, sizeof(lan_challenge), "%08x", (unsigned)rand());
	NET_EnableBroadcast();
	memset(&a, 0, sizeof(a));
	a.type = NA_IP4;
	memset(a.ip, 255, 4);
	/* a second server on a machine takes the next free port (net.c) */
	for (int p = PORT_SERVER; p < PORT_SERVER + 10; p++) {
		a.port = (uint16_t)p;
		NET_OutOfBandPrint(NS_CLIENT, &a, "getinfo %s", lan_challenge);
	}
	lan_sent = cl.realtime;
	lan_sent_wall = Sys_Seconds();
	/* a server in this process answers over loopback */
	if (sv.active) {
		memset(&a, 0, sizeof(a));
		a.type = NA_LOOPBACK;
		host_t *h = add_host(&a);
		if (h) {
			h->src |= SRC_LAN;
			query(h);
		}
	}
}

/* localservers: the servers on this network */
static void Localservers_f(int argc, const char **argv)
{
	print_replies = true;
	ask_lan();
	Con_Printf("asking the local network...\n");
}

/* globalservers [master...]: the servers the master servers know */
static void Globalservers_f(int argc, const char **argv)
{
	print_replies = true;
	if (ask_masters(argc, argv))
		Con_Printf("asking the master servers...\n");
}

/* quickconnect: asks around, then joins the best answer */
static void Quickconnect_f(int argc, const char **argv)
{
	print_replies = false;
	numhosts = 0;
	ask_masters(1, NULL);
	ask_lan();
	quick.active = true;
	quick.at = cl.realtime + 2.5;
	Con_Printf("looking for a server...\n");
}

static void quick_pick(void)
{
	host_t *best = NULL;
	int bestscore = 0;
	quick.active = false;
	for (int i = 0; i < numhosts; i++) {
		host_t *h = &hosts[i];
		if (h->ping < 0 || h->protocol != PROTOCOL_VERSION || (h->maxplayers && h->players >= h->maxplayers))
			continue;
		/* people to play with first, then the quickest */
		int score = (h->players - h->bots > 0 ? 100000 : 0) + 10000 - BW_MIN(h->ping, 9999);
		if (score > bestscore)
			best = h, bestscore = score;
	}
	if (!best) {
		Con_Printf("quickconnect: no servers answered\n");
		return;
	}
	Con_Printf("quickconnect: %s (%s, %dms)\n", best->name, NET_AdrToString(&best->adr), best->ping);
	CL_Connect(NET_AdrToString(&best->adr));
}

/* --- replies ------------------------------------------------------------------------ */

static void got_servers(const uint8_t *p, size_t len)
{
	int added = 0;
	size_t i = 0;
	while (i < len) {
		netadr_t a;
		memset(&a, 0, sizeof(a));
		if (p[i] == '\\' && i + 7 <= len) {
			if (!memcmp(p + i + 1, "EOT", 3))
				break;
			a.type = NA_IP4;
			memcpy(a.ip, p + i + 1, 4);
			a.port = (uint16_t)(p[i + 5] << 8 | p[i + 6]);
			i += 7;
		} else if (p[i] == '/' && i + 19 <= len) {
			a.type = NA_IP6;
			memcpy(a.ip, p + i + 1, 16);
			a.port = (uint16_t)(p[i + 17] << 8 | p[i + 18]);
			i += 19;
		} else {
			break;
		}
		if (!a.port)
			continue;
		host_t *h = find_host(&a);
		if (!h && (h = add_host(&a))) {
			query(h);
			added++;
		}
		if (h)
			h->src |= SRC_INTERNET;
	}
	if (print_replies && added)
		Con_Printf("%d server%s listed\n", added, added == 1 ? "" : "s");
}

/* "infoResponse\n<info>", or "statusResponse\n<info>\n<players>", from a server */
static bool got_info(const netadr_t *from, const char *text)
{
	const char *info = strchr(text, '\n'), *players;
	char ch[32], infobuf[1024];
	info = info ? info + 1 : "";
	players = strchr(info, '\n');
	Str_Copy(infobuf, info, BW_MIN(sizeof(infobuf), (size_t)(players ? players - info + 1 : (ptrdiff_t)sizeof(infobuf))));
	info = infobuf;
	host_t *h = find_host(from);
	SV_InfoGet(info, "challenge", ch, sizeof(ch));
	if (!h) {
		if (!lan_challenge[0] || strcmp(ch, lan_challenge))
			return false; /* not ours: the default handler prints it */
		h = add_host(from);
		if (!h)
			return true;
		h->sent = lan_sent;
		h->sent_wall = lan_sent_wall;
		Str_Copy(h->challenge, lan_challenge, sizeof(h->challenge));
	}
	if (lan_challenge[0] && !strcmp(ch, lan_challenge))
		h->src |= SRC_LAN;
	if (h->challenge[0] && strcmp(ch, h->challenge) && (!lan_challenge[0] || strcmp(ch, lan_challenge)))
		return true; /* a stale or forged answer */
	h->ping = (int)(((h->sent_wall > 0 ? Sys_Seconds() - h->sent_wall : cl.realtime - h->sent)) * 1000);
	parse_info(h, info);
	h->status = !memcmp(text, "statusResponse", 14);
	if (players)
		Str_Copy(h->playerlines, players + 1, sizeof(h->playerlines));
	else if (h->status)
		h->playerlines[0] = 0;
	hostcache_dirty = true;
	if (print_replies && !h->printed) {
		print_host(h);
		h->printed = true;
	}
	return true;
}

bool CL_MasterConnectionless(const netadr_t *from, const uint8_t *data, size_t len)
{
	static char text[NET_MAX_RECV + 1];
	size_t tlen = len - 4;
	/* packets aren't terminated */
	memcpy(text, data + 4, tlen);
	text[tlen] = 0;
	if (tlen >= 18 && !memcmp(text, "getserversResponse", 18)) {
		masters_answered++;
		got_servers((const uint8_t *)text + 18, tlen - 18);
		return true;
	}
	if (tlen >= 21 && !memcmp(text, "getserversExtResponse", 21)) {
		got_servers((const uint8_t *)text + 21, tlen - 21);
		return true;
	}
	if (tlen >= 12 && !memcmp(text, "infoResponse", 12))
		return got_info(from, text);
	if (tlen >= 14 && !memcmp(text, "statusResponse", 14))
		return got_info(from, text);
	/* the server cl_transfer asked answered: leave the old one for it */
	if (transfer.active && tlen > 10 && !memcmp(text, "challenge ", 10) && NET_CompareAdr(from, &transfer.adr)) {
		int challenge = atoi(text + 10);
		transfer.active = false;
		CL_Connect(transfer.name);
		cl.challenge = challenge;
		cl.state = ca_connecting;
		cl.connect_time = -999;
		cl.connect_tries = 0;
		return true;
	}
	return false;
}

/* a join from the server browser: back to the browser if it fails */
static struct {
	bool active;
	char addr[80];
	double at;
} sb_join;

static void sb_join_frame(void)
{
	if (!sb_join.active || cl.realtime - sb_join.at < 0.5)
		return;
	if (cl.state == ca_active) {
		sb_join.active = false;
	} else if (cl.state == ca_disconnected) {
		char reason[256], url[512];
		sb_join.active = false;
		/* the reason travels in the page's query: keep it to safe characters */
		Str_Copy(reason, cl.disconnect_reason[0] ? cl.disconnect_reason : "no answer", sizeof(reason));
		for (char *p = reason; *p; p++)
			if (!isalnum((unsigned char)*p) && !strchr(" .,:()-", *p))
				*p = ' ';
		for (char *p = reason; *p; p++)
			if (*p == ' ')
				*p = '+';
		Con_Printf("server browser: couldn't join %s (%s)\n", sb_join.addr,
		           cl.disconnect_reason[0] ? cl.disconnect_reason : "no answer");
		snprintf(url, sizeof(url), "fte://data/web/server-browser/index.html?failed=%s&reason=%s", sb_join.addr, reason);
		WC_OpenPage(url);
	}
}

void CL_MasterFrame(void)
{
	sb_join_frame();
	if (quick.active && cl.realtime >= quick.at)
		quick_pick();
	if (transfer.active) {
		static double last;
		if (cl.realtime > transfer.until) {
			transfer.active = false;
			Con_Printf("cl_transfer: no answer from %s\n", transfer.name);
		} else if (cl.realtime - last >= 1) {
			last = cl.realtime;
			NET_OutOfBandPrint(NS_CLIENT, &transfer.adr, "getchallenge");
		}
	}
	/* connectbr: no answer over UDP after a few tries, so try TCP */
	if (bestroute.active) {
		if (cl.state != ca_challenging)
			bestroute.active = false;
		else if (cl.connect_tries >= 3) {
			char name[300];
			bestroute.active = false;
			snprintf(name, sizeof(name), "tcp://%s", bestroute.name);
			Con_Printf("no answer over UDP, trying %s\n", name);
			CL_Connect(name);
		}
	}
}

/* --- connecting -------------------------------------------------------------------------- */

/* cl_transfer <server>: moves to another server, staying on this one until
 * the new one answers */
static void ClTransfer_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: cl_transfer <server>\n");
		return;
	}
	if (cl.state < ca_loading) {
		CL_Connect(argv[1]);
		return;
	}
	if (!NET_StringToAdr(argv[1], &transfer.adr, PORT_SERVER)) {
		Con_Printf("bad server address %s\n", argv[1]);
		return;
	}
	Str_Copy(transfer.name, argv[1], sizeof(transfer.name));
	transfer.active = true;
	transfer.until = cl.realtime + 10;
	NET_OutOfBandPrint(NS_CLIENT, &transfer.adr, "getchallenge");
	Con_Printf("transferring to %s...\n", NET_AdrToString(&transfer.adr));
}

/* connectbr <server>: UDP when it answers, TCP when it doesn't */
static void Connectbr_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: connectbr <server>\n");
		return;
	}
	CL_Connect(argv[1]);
	if (strstr(argv[1], "://") || cl.server.type == NA_LOOPBACK)
		return;
	Str_Copy(bestroute.name, argv[1], sizeof(bestroute.name));
	bestroute.active = true;
}

static void connect_with(const char *prefix, const char *server, int port)
{
	char name[300];
	const char *host = strstr(server, "://") ? strstr(server, "://") + 3 : server;
	bool hasport = strrchr(host, ':') && (host[0] != '[' || strstr(host, "]:"));
	snprintf(name, sizeof(name), "%s%s", prefix, host);
	if (!hasport && port)
		snprintf(name + strlen(name), sizeof(name) - strlen(name), ":%d", port);
	CL_Connect(name);
}

/* connecttcp/tcpconnect <server>: packets over a TCP stream */
static void Connecttcp_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: %s <server>\n", argv[0]);
		return;
	}
	connect_with("tcp://", argv[1], PORT_SERVER);
}

/* connectnq/connectqe/connectq2e: those games' default ports. Bulwark
 * servers speak one protocol, so only the port differs. */
static void ConnectPort_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: %s <server>\n", argv[0]);
		return;
	}
	connect_with("", argv[1], !strcmp(argv[0], "connectq2e") ? 5069 : 26000);
}

/* qwurl qw://server[:port][/join|/observe|/qtvplay]: ezQuake's url form */
static void Qwurl_f(int argc, const char **argv)
{
	char host[256], *slash;
	if (argc < 2) {
		Con_Printf("usage: connect <server>\n");
		return;
	}
	const char *u = argv[1];
	if (!Str_NICmp(u, "qw://", 5))
		u += 5;
	Str_Copy(host, u, sizeof(host));
	const char *action = "";
	if ((slash = strchr(host, '/'))) {
		*slash = 0;
		action = slash + 1;
	}
	if (!Str_ICmp(action, "qtvplay") || !Str_ICmp(action, "qtv")) {
		Cbuf_AddText(va("qtvplay \"%s\"\n", host));
		return;
	}
	if (!Str_ICmp(action, "observe") || !Str_ICmp(action, "spectate"))
		Cbuf_AddText("setinfo spectator 1\n");
	else if (!Str_ICmp(action, "join"))
		Cbuf_AddText("setinfo spectator 0\n");
	Cbuf_AddText(va("connect \"%s\"\n", host));
}

static void list_port(const char *desc, void *ctx) { Con_Printf("  %s\n", desc); }

/* cl_addport [port]: another UDP port for the client */
static void ClAddport_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("Active Client ports:\n");
		NET_ListPorts(NS_CLIENT, list_port, NULL);
		Con_Printf("end of list\n");
		return;
	}
	if (NET_AddPort(NS_CLIENT, atoi(argv[1])))
		Con_Printf("client also listening on port %s\n", argv[1]);
}

/* WebRTC/ICE and DTLS aren't part of Bulwark's networking */
static void NetIceShow_f(int argc, const char **argv) { Con_Printf("no ICE connections (Bulwark connects over UDP or TCP)\n"); }

static void Dtls_f(int argc, const char **argv)
{
	Con_Printf("%s: Bulwark has no DTLS, so there are no certificates to trust; tcp:// and plain UDP are its "
	           "transports\n", argv[0]);
}

void CL_InitMaster(void)
{
	for (int i = 0; i < NUM_MASTERS; i++)
		net_master[i] = Cvar_Get(va("net_master%d", i + 1), "", CVAR_ARCHIVE, "A master server to list servers from");
	Cmd_AddD("localservers", Localservers_f, "List the servers on the local network");
	Cmd_AddD("globalservers", Globalservers_f, "globalservers [master...]: list the servers the master servers know");
	Cmd_AddD("quickconnect", Quickconnect_f, "Find a server and join the best one");
	Cmd_AddD("cl_transfer", ClTransfer_f,
	         "Connect to a different server, disconnecting from the current server only when the new server replies");
	Cmd_AddD("connectbr", Connectbr_f, "Connect to a server using the best route we can find (UDP, else TCP)");
	Cmd_AddD("connecttcp", Connecttcp_f, "Connect to a server using the tcp:// prefix");
	Cmd_AddD("tcpconnect", Connecttcp_f, "Connect to a server using the tcp:// prefix");
	Cmd_AddD("connectnq", ConnectPort_f, "Connect to a server, defaulting to port 26000");
	Cmd_AddD("connectqe", ConnectPort_f, "Connect to a server, defaulting to port 26000");
	Cmd_AddD("connectq2e", ConnectPort_f, "Connect to a server, defaulting to port 5069");
	Cmd_AddD("qwurl", Qwurl_f, "qwurl qw://server[/join|/observe|/qtvplay]: for compat with ezquake");
	Cmd_AddD("cl_addport", ClAddport_f, "cl_addport [port]: listen on another UDP port; lists the ports without one");
	Cmd_AddD("net_ice_show", NetIceShow_f, "Show WebRTC/ICE connections (there are none in Bulwark)");
	Cmd_AddD("dtls_importtrust", Dtls_f, "DTLS certificate trust (Bulwark has no DTLS)");
	Cmd_AddD("dtls_untrustall", Dtls_f, "DTLS certificate trust (Bulwark has no DTLS)");
	CL_InitServerBrowser();
}

/* --- the hostcache builtins (menu and client QC) -------------------------------------------- */

enum {
	HC_CNAME, HC_PING, HC_GAME, HC_MOD, HC_MAP, HC_NAME, HC_MAXPLAYERS, HC_NUMPLAYERS, HC_PROTOCOL, HC_NUMBOTS,
	HC_NUMHUMANS, HC_FREESLOTS, HC_GAMEDIR, HC_SERVERINFO, HC_PLAYERS, HC_QCSTATUS, HC_CATEGORY, HC_ISFAVORITE,
	HC_PLAYER0 /* player0, player1, ...: a player's name */
};
#define HC_MAXPLAYERKEYS 64
#define HC_CUSTOM (HC_PLAYER0 + HC_MAXPLAYERKEYS) /* addwantedhostcachekey's, from here */
#define HC_MAXCUSTOM 32

static const char *const hc_keys[] = {"cname", "ping", "game", "mod", "map", "name", "maxplayers", "numplayers",
                                      "protocol", "numbots", "numhumans", "freeslots", "gamedir", "serverinfo",
                                      "players", "qcstatus", "category", "isfavorite"};
static char hc_custom[HC_MAXCUSTOM][32];
static int hc_numcustom;

typedef struct {
	int fld, op;
	bool number;
	char str[64];
	float num;
} hcmask_t;
static hcmask_t hc_masks[32];
static int hc_nummasks;
static int hc_sortfld = HC_PING;
static bool hc_sortdesc;
static int *hc_view;
static int hc_viewcount;

/* the name of player n from the status lines, or "" */
static const char *player_name(const host_t *h, int n, char *out, size_t size)
{
	const char *p = h->playerlines;
	out[0] = 0;
	for (int i = 0; *p; i++) {
		const char *end = strchr(p, '\n');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		if (i == n) {
			const char *q = memchr(p, '"', len);
			if (q) {
				const char *e = memchr(q + 1, '"', len - (size_t)(q + 1 - p));
				Str_Copy(out, q + 1, BW_MIN(size, (size_t)((e ? e : p + len) - q)));
			}
			return out;
		}
		if (!end)
			break;
		p = end + 1;
	}
	return out;
}

static const char *host_string(const host_t *h, int fld, char *out, size_t size)
{
	out[0] = 0;
	switch (fld) {
	case HC_CNAME: Str_Copy(out, NET_AdrToString(&h->adr), size); break;
	case HC_PING: snprintf(out, size, "%d", h->ping); break;
	case HC_GAME: Str_Copy(out, h->game, size); break;
	case HC_MOD: case HC_GAMEDIR: Str_Copy(out, h->mod, size); break;
	case HC_MAP: Str_Copy(out, h->map, size); break;
	case HC_NAME: Str_Copy(out, h->name, size); break;
	case HC_MAXPLAYERS: snprintf(out, size, "%d", h->maxplayers); break;
	case HC_NUMPLAYERS: snprintf(out, size, "%d", h->players); break;
	case HC_PROTOCOL: snprintf(out, size, "%d", h->protocol); break;
	case HC_NUMBOTS: snprintf(out, size, "%d", h->bots); break;
	case HC_NUMHUMANS: snprintf(out, size, "%d", h->players - h->bots); break;
	case HC_FREESLOTS: snprintf(out, size, "%d", BW_MAX(0, h->maxplayers - h->players)); break;
	case HC_SERVERINFO: Str_Copy(out, h->info, size); break;
	case HC_PLAYERS: Str_Copy(out, h->playerlines, size); break;
	case HC_QCSTATUS: ikey(h->info, "qcstatus", out, size); break;
	case HC_CATEGORY: case HC_ISFAVORITE: Str_Copy(out, "0", size); break;
	default:
		if (fld >= HC_PLAYER0 && fld < HC_CUSTOM)
			player_name(h, fld - HC_PLAYER0, out, size);
		else if (fld >= HC_CUSTOM && fld < HC_CUSTOM + hc_numcustom)
			ikey(h->info, hc_custom[fld - HC_CUSTOM], out, size);
	}
	return out;
}

static bool is_number_field(int fld)
{
	return fld == HC_PING || fld == HC_MAXPLAYERS || fld == HC_NUMPLAYERS || fld == HC_PROTOCOL || fld == HC_NUMBOTS ||
	       fld == HC_NUMHUMANS || fld == HC_FREESLOTS || fld == HC_CATEGORY || fld == HC_ISFAVORITE;
}

static const char *Str_IStr(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (!Str_NICmp(hay, needle, n))
			return hay;
	return n ? NULL : hay;
}

static bool mask_passes(const host_t *h, const hcmask_t *m)
{
	char buf[1024];
	const char *v = host_string(h, m->fld, buf, sizeof(buf));
	int c;
	if (m->number || is_number_field(m->fld)) {
		float a = (float)atof(v), b = m->number ? m->num : (float)atof(m->str);
		c = a < b ? -1 : a > b ? 1 : 0;
	} else
		c = Str_ICmp(v, m->str);
	switch (m->op) {
	case 0: return Str_IStr(v, m->str) != NULL;       /* contains */
	case 1: return Str_IStr(v, m->str) == NULL;       /* doesn't contain */
	case 2: return c <= 0;
	case 3: return c < 0;
	case 4: return c == 0;
	case 5: return c > 0;
	case 6: return c >= 0;
	case 7: return c != 0;
	case 8: return !Str_NICmp(v, m->str, strlen(m->str));
	case 9: return Str_NICmp(v, m->str, strlen(m->str)) != 0;
	}
	return true;
}

static int sort_cmp(const void *a, const void *b)
{
	const host_t *x = &hosts[*(const int *)a], *y = &hosts[*(const int *)b];
	char bx[1024], by[1024];
	int c;
	/* servers that haven't answered go last whatever the order */
	if ((x->ping < 0) != (y->ping < 0))
		return x->ping < 0 ? 1 : -1;
	if (is_number_field(hc_sortfld)) {
		float fx = (float)atof(host_string(x, hc_sortfld, bx, sizeof(bx))), fy = (float)atof(host_string(y, hc_sortfld, by, sizeof(by)));
		c = fx < fy ? -1 : fx > fy ? 1 : 0;
	} else
		c = Str_ICmp(host_string(x, hc_sortfld, bx, sizeof(bx)), host_string(y, hc_sortfld, by, sizeof(by)));
	return hc_sortdesc ? -c : c;
}

static void resort(void)
{
	if (!hc_view)
		hc_view = Mem_Alloc(sizeof(int) * MAX_HOSTS);
	hc_viewcount = 0;
	for (int i = 0; i < numhosts; i++) {
		bool ok = hosts[i].ping >= 0; /* only servers that answered */
		for (int k = 0; k < hc_nummasks && ok; k++)
			ok = mask_passes(&hosts[i], &hc_masks[k]);
		if (ok)
			hc_view[hc_viewcount++] = i;
	}
	qsort(hc_view, (size_t)hc_viewcount, sizeof(int), sort_cmp);
	hostcache_dirty = false;
}

static const host_t *view_host(float n)
{
	int i = (int)n;
	return i >= 0 && i < hc_viewcount ? &hosts[hc_view[i]] : NULL;
}

static void bi_gethostcachevalue(qcvm_t *vm)
{
	int queried = 0, answered = 0;
	for (int i = 0; i < numhosts; i++) {
		queried++;
		answered += hosts[i].ping >= 0;
	}
	switch ((int)QC_PFLOAT(vm, 0)) {
	case 0: QC_ReturnFloat(vm, (float)hc_viewcount); break;
	case 1: QC_ReturnFloat(vm, (float)numhosts); break;
	case 2: QC_ReturnFloat(vm, (float)masters_asked); break;
	case 3: QC_ReturnFloat(vm, (float)BW_MIN(masters_answered, masters_asked)); break;
	case 4: QC_ReturnFloat(vm, (float)queried); break;
	case 5: QC_ReturnFloat(vm, (float)answered); break;
	case 6: QC_ReturnFloat(vm, (float)hc_sortfld); break;
	case 7: QC_ReturnFloat(vm, hc_sortdesc ? 1 : 0); break;
	default: QC_ReturnFloat(vm, 0);
	}
}

static void bi_gethostcachestring(qcvm_t *vm)
{
	char buf[1024];
	const host_t *h = view_host(QC_PFLOAT(vm, 1));
	QC_ReturnString(vm, h ? host_string(h, (int)QC_PFLOAT(vm, 0), buf, sizeof(buf)) : "");
}

static void bi_gethostcachenumber(qcvm_t *vm)
{
	char buf[1024];
	const host_t *h = view_host(QC_PFLOAT(vm, 1));
	QC_ReturnFloat(vm, h ? (float)atof(host_string(h, (int)QC_PFLOAT(vm, 0), buf, sizeof(buf))) : 0);
}

static void bi_gethostcacheindexforkey(qcvm_t *vm)
{
	const char *key = QC_PSTR(vm, 0);
	for (int i = 0; i < (int)BW_ARRAY_COUNT(hc_keys); i++)
		if (!Str_ICmp(key, hc_keys[i])) {
			QC_ReturnFloat(vm, (float)i);
			return;
		}
	if (!Str_NICmp(key, "player", 6) && (key[6] == 0 || isdigit((unsigned char)key[6]))) {
		int n = atoi(key + 6);
		QC_ReturnFloat(vm, n < HC_MAXPLAYERKEYS ? (float)(HC_PLAYER0 + n) : -1);
		return;
	}
	for (int i = 0; i < hc_numcustom; i++)
		if (!Str_ICmp(key, hc_custom[i])) {
			QC_ReturnFloat(vm, (float)(HC_CUSTOM + i));
			return;
		}
	QC_ReturnFloat(vm, -1);
}

static void bi_addwantedhostcachekey(qcvm_t *vm)
{
	const char *key = QC_PSTR(vm, 0);
	for (int i = 0; i < hc_numcustom; i++)
		if (!Str_ICmp(key, hc_custom[i]))
			return;
	if (hc_numcustom < HC_MAXCUSTOM)
		Str_Copy(hc_custom[hc_numcustom++], key, sizeof(hc_custom[0]));
}

static void bi_resethostcachemasks(qcvm_t *vm) { hc_nummasks = 0; }

static void add_mask(qcvm_t *vm, bool number)
{
	if (hc_nummasks >= (int)BW_ARRAY_COUNT(hc_masks) || QC_PFLOAT(vm, 1) < 0)
		return;
	hcmask_t *m = &hc_masks[hc_nummasks++];
	m->fld = (int)QC_PFLOAT(vm, 1);
	m->number = number;
	if (number)
		m->num = QC_PFLOAT(vm, 2);
	else
		Str_Copy(m->str, QC_PSTR(vm, 2), sizeof(m->str));
	m->op = (int)QC_PFLOAT(vm, 3);
	/* an empty "equals" mask (an unset cvar) filters nothing */
	if (!number && !m->str[0] && m->op == 4)
		hc_nummasks--;
}

static void bi_sethostcachemaskstring(qcvm_t *vm) { add_mask(vm, false); }
static void bi_sethostcachemasknumber(qcvm_t *vm) { add_mask(vm, true); }
static void bi_resorthostcache(qcvm_t *vm) { resort(); }

static void bi_sethostcachesort(qcvm_t *vm)
{
	hc_sortfld = (int)QC_PFLOAT(vm, 0);
	hc_sortdesc = QC_PFLOAT(vm, 1) != 0;
}

/* refreshhostcache(purge): asks the network and the masters again */
void CL_HostcacheRefresh(bool purge);

static void bi_refreshhostcache(qcvm_t *vm)
{
	CL_HostcacheRefresh(vm->argc > 0 && QC_PFLOAT(vm, 0) != 0);
}

static void bi_getextresponse(qcvm_t *vm) { QC_ReturnString(vm, ""); }

void CL_HostcacheRefresh(bool purge)
{
	print_replies = false;
	if (purge) {
		numhosts = 0;
		hc_viewcount = 0;
	} else
		for (int i = 0; i < numhosts; i++)
			query(&hosts[i]);
	masters_asked = masters_answered = 0;
	ask_lan();
	ask_masters(0, NULL);
}

/* for the engine's own server browser */
int CL_HostcacheCount(void)
{
	if (hostcache_dirty)
		resort();
	return hc_viewcount;
}

const char *CL_HostcacheField(int n, const char *key, char *out, size_t size)
{
	out[0] = 0;
	const host_t *h = view_host((float)n);
	if (!h)
		return out;
	for (int i = 0; i < (int)BW_ARRAY_COUNT(hc_keys); i++)
		if (!Str_ICmp(key, hc_keys[i]))
			return host_string(h, i, out, size);
	return out;
}

void CL_HostcacheSort(const char *key, bool descending)
{
	for (int i = 0; i < (int)BW_ARRAY_COUNT(hc_keys); i++)
		if (!Str_ICmp(key, hc_keys[i]))
			hc_sortfld = i;
	hc_sortdesc = descending;
	resort();
}

/* netaddress_resolve(name, defport): a blocking lookup, "" when it fails */
static void bi_netaddress_resolve(qcvm_t *vm)
{
	netadr_t a;
	int port = vm->argc > 1 ? (int)QC_PFLOAT(vm, 1) : 0;
	if (NET_StringToAdr(QC_PSTR(vm, 0), &a, port > 0 ? port : PORT_SERVER))
		QC_ReturnString(vm, NET_AdrToString(&a));
	else
		QC_ReturnString(vm, "");
}

const qc_builtin_t cl_hostcache_builtins[] = {
	{"netaddress_resolve", bi_netaddress_resolve},
	{"gethostcachevalue", bi_gethostcachevalue}, {"gethostcachestring", bi_gethostcachestring},
	{"gethostcachenumber", bi_gethostcachenumber}, {"gethostcacheindexforkey", bi_gethostcacheindexforkey},
	{"addwantedhostcachekey", bi_addwantedhostcachekey}, {"resethostcachemasks", bi_resethostcachemasks},
	{"sethostcachemaskstring", bi_sethostcachemaskstring}, {"sethostcachemasknumber", bi_sethostcachemasknumber},
	{"resorthostcache", bi_resorthostcache}, {"sethostcachesort", bi_sethostcachesort},
	{"refreshhostcache", bi_refreshhostcache}, {"getextresponse", bi_getextresponse},
	{NULL, NULL}};

/* --- the server browser page's requests ---------------------------------------------------
 *
 * The Panorama server browser (layout/web/server-browser.xml) asks through
 * the webcore bridge with "sb_..." requests; docs/spec/serverbrowser.md
 * lists them. The page keeps the filters and the sort; the engine keeps
 * which list each server is on, favourites and history. */

static cvar_t *sb_favorites, *sb_history, *sb_history_max;
static double sb_refreshed_at;

static bool sb_valid_addr(const char *s)
{
	size_t n = strlen(s);
	if (n < 1 || n > 64)
		return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && !strchr("._:-[]", *s))
			return false;
	return true;
}

/* the canonical "ip:port" of an address a page or player typed, or "" */
static const char *sb_canon(const char *s, char *out, size_t size)
{
	netadr_t a;
	out[0] = 0;
	if (sb_valid_addr(s) && NET_StringToAdr(s, &a, PORT_SERVER) && (a.type == NA_IP4 || a.type == NA_IP6))
		Str_Copy(out, NET_AdrToString(&a), size);
	return out;
}

static bool sb_list_has(const cvar_t *list, const char *addr)
{
	size_t n = strlen(addr);
	for (const char *p = list->string; (p = strstr(p, addr)) != NULL; p += n)
		if ((p == list->string || p[-1] == ' ') && (p[n] == ' ' || !p[n]))
			return true;
	return false;
}

/* rebuilds a space-separated list: addr removed, then put first when add */
static void sb_list_put(cvar_t *list, const char *addr, bool add, int max)
{
	char out[4096] = "", copy[4096];
	int count = 0;
	if (add) {
		Str_Copy(out, addr, sizeof(out));
		count = 1;
	}
	Str_Copy(copy, list->string, sizeof(copy));
	for (char *tok = strtok(copy, " "); tok; tok = strtok(NULL, " ")) {
		if (!strcmp(tok, addr) || (max > 0 && count >= max))
			continue;
		if (strlen(out) + strlen(tok) + 2 >= sizeof(out))
			break;
		if (out[0])
			strcat(out, " ");
		strcat(out, tok);
		count++;
	}
	Cvar_Set(list->name, out);
}

static void sb_mark(const char *addr, int src)
{
	netadr_t a;
	if (!NET_StringToAdr(addr, &a, PORT_SERVER))
		return;
	host_t *h = add_host(&a);
	if (h) {
		h->src |= src;
		query(h);
	}
}

static void sb_ask_list(const cvar_t *list, int src)
{
	char copy[4096];
	Str_Copy(copy, list->string, sizeof(copy));
	for (char *tok = strtok(copy, " "); tok; tok = strtok(NULL, " "))
		sb_mark(tok, src);
}

/* drops a list's mark from every server, and the servers left on none */
static void sb_forget(int src)
{
	int n = 0;
	for (int i = 0; i < numhosts; i++) {
		hosts[i].src &= ~src;
		if (hosts[i].src)
			hosts[n++] = hosts[i];
	}
	numhosts = n;
	hc_viewcount = 0;
	hostcache_dirty = true;
}

static int sb_tab_src(const char *tab)
{
	if (!Str_ICmp(tab, "internet")) return SRC_INTERNET;
	if (!Str_ICmp(tab, "lan")) return SRC_LAN;
	if (!Str_ICmp(tab, "favorites")) return SRC_FAVORITE;
	if (!Str_ICmp(tab, "history")) return SRC_HISTORY;
	return SRC_INTERNET | SRC_LAN | SRC_FAVORITE | SRC_HISTORY;
}

static void sb_refresh(const char *tab)
{
	int src = sb_tab_src(tab);
	print_replies = false;
	sb_forget(src);
	if (src & SRC_INTERNET) {
		masters_asked = masters_answered = 0;
		ask_masters(0, NULL);
	}
	if (src & SRC_LAN)
		ask_lan();
	if (src & SRC_FAVORITE)
		sb_ask_list(sb_favorites, SRC_FAVORITE);
	if (src & SRC_HISTORY)
		sb_ask_list(sb_history, SRC_HISTORY);
	sb_refreshed_at = cl.realtime;
}

/* the player lines of a status reply: frags ping "name" */
static json_t *sb_players(const host_t *h)
{
	json_t *arr = JSON_New(JSON_ARRAY);
	const char *p = h->playerlines;
	while (*p) {
		const char *end = strchr(p, '\n');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		char line[256], name[128] = "";
		Str_Copy(line, p, BW_MIN(sizeof(line), len + 1));
		int frags = 0, ping = 0;
		const char *q = strchr(line, '"');
		sscanf(line, "%d %d", &frags, &ping);
		if (q) {
			const char *e = strchr(q + 1, '"');
			Str_Copy(name, q + 1, BW_MIN(sizeof(name), (size_t)((e ? e : q + 1 + strlen(q + 1)) - q)));
		}
		if (line[0]) {
			json_t *o = JSON_New(JSON_OBJECT);
			JSON_SetString(o, "name", name);
			JSON_SetNumber(o, "frags", frags);
			JSON_SetNumber(o, "ping", ping);
			JSON_SetBool(o, "bot", ping == 0);
			JSON_Append(arr, o);
		}
		if (!end)
			break;
		p = end + 1;
	}
	return arr;
}

static json_t *sb_row(const host_t *h)
{
	char tmp[256];
	const char *addr = NET_AdrToString(&h->adr);
	json_t *o = JSON_New(JSON_OBJECT);
	JSON_SetString(o, "addr", addr);
	JSON_SetString(o, "name", h->name[0] ? h->name : addr);
	JSON_SetString(o, "map", h->map);
	JSON_SetString(o, "game", h->game);
	JSON_SetString(o, "mod", h->mod);
	JSON_SetString(o, "gametype", ikey(h->info, "g_gametype", tmp, sizeof(tmp)));
	JSON_SetString(o, "state", ikey(h->info, "arena_state", tmp, sizeof(tmp)));
	JSON_SetString(o, "engine", ikey(h->info, "engine", tmp, sizeof(tmp)));
	JSON_SetNumber(o, "players", h->players - h->bots);
	JSON_SetNumber(o, "bots", h->bots);
	JSON_SetNumber(o, "max", h->maxplayers);
	JSON_SetNumber(o, "ping", h->ping);
	JSON_SetNumber(o, "protocol", h->protocol);
	JSON_SetBool(o, "password", atoi(ikey(h->info, "password", tmp, sizeof(tmp))) != 0 ||
	                                atoi(ikey(h->info, "needpass", tmp, sizeof(tmp))) != 0);
	JSON_SetBool(o, "compatible", h->protocol == PROTOCOL_VERSION);
	JSON_SetBool(o, "internet", (h->src & SRC_INTERNET) != 0);
	JSON_SetBool(o, "lan", (h->src & SRC_LAN) != 0);
	JSON_SetBool(o, "favorite", sb_list_has(sb_favorites, addr));
	JSON_SetBool(o, "history", sb_list_has(sb_history, addr));
	return o;
}

static void sb_reply(strbuf_t *out, json_t *j)
{
	JSON_Write(out, j, -1);
	JSON_Free(j);
}

/* sb_list[:all]: the servers that answered; other games' servers (a LAN
 * or a shared master has them) only with :all */
static void sb_getlist(strbuf_t *out, bool all)
{
	json_t *j = JSON_New(JSON_OBJECT), *arr = JSON_New(JSON_ARRAY);
	const char *game = Cvar_String("com_protocolname");
	int pending = 0, others = 0;
	for (int i = 0; i < numhosts; i++) {
		if (hosts[i].ping < 0) {
			/* a server that hasn't answered in 3 s isn't coming */
			pending += cl.realtime - hosts[i].sent < 3;
			continue;
		}
		if (!all && hosts[i].game[0] && Str_ICmp(hosts[i].game, game)) {
			others++;
			continue;
		}
		JSON_Append(arr, sb_row(&hosts[i]));
	}
	JSON_SetNumber(j, "other_games", others);
	JSON_SetString(j, "game", Cvar_String("com_protocolname"));
	JSON_SetNumber(j, "protocol", PROTOCOL_VERSION);
	JSON_SetNumber(j, "masters", masters_asked);
	JSON_SetNumber(j, "masters_answered", BW_MIN(masters_answered, masters_asked));
	JSON_SetNumber(j, "pending", pending);
	JSON_SetBool(j, "refreshing", pending > 0 || cl.realtime - sb_refreshed_at < 1.5);
	JSON_SetBool(j, "connected", cl.state == ca_active);
	JSON_SetString(j, "server", cl.state >= ca_loading ? cl.servername : "");
	JSON_Set(j, "servers", arr);
	sb_reply(out, j);
}

static host_t *sb_find(const char *s)
{
	netadr_t a;
	return sb_valid_addr(s) && NET_StringToAdr(s, &a, PORT_SERVER) ? find_host(&a) : NULL;
}

/* one server's players and rules; asks it again when the list only had
 * its info reply or the status is stale */
static void sb_details(const char *addr, strbuf_t *out)
{
	host_t *h = sb_find(addr);
	if (!h) {
		SB_AppendStr(out, "{\"ok\":false,\"error\":\"unknown server\"}");
		return;
	}
	if ((!h->status || cl.realtime - h->sent > 5) && cl.realtime - h->sent > 1)
		query(h);
	json_t *j = h->ping >= 0 ? sb_row(h) : JSON_New(JSON_OBJECT);
	JSON_SetBool(j, "ok", true);
	JSON_SetBool(j, "answered", h->ping >= 0);
	JSON_Set(j, "player_list", sb_players(h));
	json_t *rules = JSON_New(JSON_OBJECT);
	for (const char *p = h->info; *p == '\\';) {
		const char *k = p + 1, *ke = strchr(k, '\\'), *ve;
		char key[64], val[256];
		if (!ke)
			break;
		ve = strchr(ke + 1, '\\');
		if (!ve)
			ve = ke + 1 + strlen(ke + 1);
		Str_Copy(key, k, BW_MIN(sizeof(key), (size_t)(ke - k + 1)));
		Str_Copy(val, ke + 1, BW_MIN(sizeof(val), (size_t)(ve - ke)));
		if (strcmp(key, "challenge"))
			JSON_SetString(rules, key, val);
		p = ve;
	}
	JSON_Set(j, "rules", rules);
	sb_reply(out, j);
}

void CL_MasterNoteConnect(const netadr_t *a)
{
	if (a->type != NA_IP4 && a->type != NA_IP6)
		return;
	const char *addr = NET_AdrToString(a);
	sb_list_put(sb_history, addr, true, BW_CLAMP(sb_history_max->ivalue, 1, 100));
	host_t *h = find_host(a);
	if (h)
		h->src |= SRC_HISTORY;
}

/* sb_connect:<addr>[ <password>] */
static void sb_connect(const char *arg, strbuf_t *out)
{
	char addr[80], canon[80], pass[128] = "";
	const char *sp = strchr(arg, ' ');
	Str_Copy(addr, arg, sp ? BW_MIN(sizeof(addr), (size_t)(sp - arg + 1)) : sizeof(addr));
	if (sp)
		Str_Copy(pass, sp + 1, sizeof(pass));
	if (!sb_canon(addr, canon, sizeof(canon))[0] || strpbrk(pass, "\\\";\n")) {
		SB_AppendStr(out, "{\"ok\":false,\"error\":\"bad address\"}");
		return;
	}
	Cvar_Set("password", pass);
	Cbuf_AddText(va("connect %s\n", canon));
	sb_join.active = true;
	sb_join.at = cl.realtime;
	Str_Copy(sb_join.addr, canon, sizeof(sb_join.addr));
	SB_AppendStr(out, "{\"ok\":true}");
}

static void sb_favorite(const char *arg, strbuf_t *out, int mode)
{
	char canon[80];
	if (!sb_canon(arg, canon, sizeof(canon))[0]) {
		SB_AppendStr(out, "{\"ok\":false,\"error\":\"bad address\"}");
		return;
	}
	bool on = mode < 0 ? !sb_list_has(sb_favorites, canon) : mode > 0;
	sb_list_put(sb_favorites, canon, on, 200);
	if (on)
		sb_mark(canon, SRC_FAVORITE);
	else {
		host_t *h = sb_find(canon);
		if (h)
			h->src &= ~SRC_FAVORITE;
	}
	SB_Printf(out, "{\"ok\":true,\"favorite\":%s,\"addr\":\"%s\"}", on ? "true" : "false", canon);
}

bool CL_ServerBrowserQuery(const char *req, strbuf_t *out)
{
	if (!strncmp(req, "sb_refresh", 10)) {
		sb_refresh(req[10] == ':' ? req + 11 : "all");
		SB_AppendStr(out, "ok");
	} else if (!strcmp(req, "sb_list") || !strcmp(req, "sb_list:all")) {
		sb_getlist(out, req[7] == ':');
	} else if (!strncmp(req, "sb_details:", 11)) {
		sb_details(req + 11, out);
	} else if (!strncmp(req, "sb_ping:", 8)) {
		host_t *h = sb_find(req + 8);
		if (h)
			query(h);
		SB_AppendStr(out, h ? "ok" : "unknown");
	} else if (!strncmp(req, "sb_connect:", 11)) {
		sb_connect(req + 11, out);
	} else if (!strncmp(req, "sb_favorite:", 12)) {
		sb_favorite(req + 12, out, -1);
	} else if (!strncmp(req, "sb_add:", 7)) {
		sb_favorite(req + 7, out, 1);
	} else if (!strcmp(req, "sb_clearhistory")) {
		Cvar_Set(sb_history->name, "");
		sb_forget(SRC_HISTORY);
		SB_AppendStr(out, "ok");
	} else {
		return false;
	}
	return true;
}

/* sb_query <request>: what the page would get, for tests and the console */
static void SbQuery_f(int argc, const char **argv)
{
	strbuf_t out = {0};
	if (argc < 2 || !CL_ServerBrowserQuery(argv[1], &out))
		Con_Printf("usage: sb_query \"sb_refresh[:tab]|sb_list|sb_details:<addr>|...\"\n");
	else
		Con_Printf("%s\n", out.data ? out.data : "");
	SB_Free(&out);
}

static void CL_InitServerBrowser(void)
{
	sb_favorites = Cvar_Get("sb_favorites", "", CVAR_ARCHIVE, "The server browser's favourite servers (ip:port, space separated)");
	sb_history = Cvar_Get("sb_history", "", CVAR_ARCHIVE, "Servers you joined, newest first (ip:port, space separated)");
	sb_history_max = Cvar_Get("sb_history_max", "30", CVAR_ARCHIVE, "How many servers the browser's history keeps");
	Cvar_Get("password", "", CVAR_USERINFO, "The password sent to a server that asks for one");
	Cmd_AddD("sb_query", SbQuery_f, "sb_query \"<request>\": run a server browser request and print the reply");
}
