/* Bulwark engine - finding servers and the ways of connecting to them:
 * the server browser's list (localservers, globalservers, quickconnect),
 * cl_transfer, connectbr and the other connect variants, qwurl and extra
 * client ports.
 *
 * Masters are asked with the DarkPlaces master protocol: "getservers
 * <game> <protocol> empty full" answered by "getserversResponse" and six
 * byte addresses after backslashes, ending with "\EOT". Each server is then
 * asked "getinfo <challenge>"; the time to its infoResponse is its ping.
 * ISC licence, see LICENSE. */
#include "client.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../net/protocol.h"
#include "../server/sv_local.h"

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
	char challenge[16];
	bool printed;
} host_t;

static host_t *hosts;
static int numhosts;
static bool print_replies; /* the console asked, so answers are printed */
static double lan_sent;
static cvar_t *net_master[NUM_MASTERS];

static const char *const default_masters[] = {"master.frag-net.com:27950", "dpmaster.deathmask.net:27950",
                                              "dpmaster.tchr.no:27950", NULL};

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
	NET_OutOfBandPrint(NS_CLIENT, &h->adr, "getinfo %s", h->challenge);
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
	for (int p = PORT_SERVER; p < PORT_SERVER + 4; p++) {
		a.port = (uint16_t)p;
		NET_OutOfBandPrint(NS_CLIENT, &a, "getinfo %s", lan_challenge);
	}
	lan_sent = cl.realtime;
	/* a server in this process answers over loopback */
	if (sv.active) {
		memset(&a, 0, sizeof(a));
		a.type = NA_LOOPBACK;
		host_t *h = add_host(&a);
		if (h)
			query(h);
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
	}
	if (print_replies && added)
		Con_Printf("%d server%s listed\n", added, added == 1 ? "" : "s");
}

/* "infoResponse\n<info>" from a server */
static bool got_info(const netadr_t *from, const char *text)
{
	const char *info = strchr(text, '\n');
	char ch[32];
	info = info ? info + 1 : "";
	host_t *h = find_host(from);
	SV_InfoGet(info, "challenge", ch, sizeof(ch));
	if (!h) {
		if (!lan_challenge[0] || strcmp(ch, lan_challenge))
			return false; /* not ours: the default handler prints it */
		h = add_host(from);
		if (!h)
			return true;
		h->sent = lan_sent;
		Str_Copy(h->challenge, lan_challenge, sizeof(h->challenge));
	}
	if (h->challenge[0] && strcmp(ch, h->challenge) && (!lan_challenge[0] || strcmp(ch, lan_challenge)))
		return true; /* a stale or forged answer */
	h->ping = (int)((cl.realtime - h->sent) * 1000);
	parse_info(h, info);
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
		got_servers((const uint8_t *)text + 18, tlen - 18);
		return true;
	}
	if (tlen >= 21 && !memcmp(text, "getserversExtResponse", 21)) {
		got_servers((const uint8_t *)text + 21, tlen - 21);
		return true;
	}
	if (tlen >= 12 && !memcmp(text, "infoResponse", 12))
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

void CL_MasterFrame(void)
{
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
}
