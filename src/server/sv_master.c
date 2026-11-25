/* Bulwark engine - being found: answering server browser queries,
 * heartbeats to master servers, extra ports, NAT-PMP port forwarding, and
 * the pinned messages players see when they join.
 *
 * Masters speak the DarkPlaces master protocol (as dpmaster documents it):
 * a server sends "heartbeat DarkPlaces", the master asks "getinfo
 * <challenge>" and the server answers "infoResponse" with an info string
 * that echoes the challenge. ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../net/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../net/net_compat.h"

#define NUM_MASTERS 8
#define HEARTBEAT_SECONDS 300

static cvar_t *sv_public, *sv_master[NUM_MASTERS], *sv_port_tcp, *sv_port_natpmp, *hostname;
static double next_heartbeat;

/* used when sv_public is on and no sv_master is set */
#ifdef __EMSCRIPTEN__
static const char *const default_masters[] = {NULL}; /* see cl_master.c */
#else
static const char *const default_masters[] = {"master.frag-net.com:27950", "dpmaster.deathmask.net:27950",
                                              "dpmaster.tchr.no:27950", NULL};
#endif

/* --- answering queries ---------------------------------------------------- */

static int count_clients(int *bots)
{
	int n = 0;
	*bots = 0;
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].state != CS_FREE) {
			n++;
			*bots += svs.clients[i].bot;
		}
	return n;
}

static void build_info(char *out, size_t size, const char *challenge)
{
	int bots, n = count_clients(&bots);
	snprintf(out, size,
	         "\\gamename\\%s\\modname\\%s\\protocol\\%d\\hostname\\%s\\mapname\\%s\\clients\\%d\\bots\\%d"
	         "\\sv_maxclients\\%d\\map\\%s\\maxclients\\%d\\engine\\%s\\password\\%d",
	         Cvar_String("com_protocolname"), FS_GameDir(), PROTOCOL_VERSION, hostname->string, sv.mapname, n, bots,
	         svs.maxclients, sv.mapname, svs.maxclients, PROTOCOL_NAME,
	         Cvar_String("sv_password")[0] && strcmp(Cvar_String("sv_password"), "none") ? 1 : 0);
	if (challenge && *challenge && !strchr(challenge, '\\') && !strchr(challenge, '"')) {
		size_t len = strlen(out);
		snprintf(out + len, size - len, "\\challenge\\%s", challenge);
	}
}

static int frags_of(client_t *c)
{
	qcdef_t *d = sv.vm ? QC_FindField(sv.vm, "frags") : NULL;
	return d && c->edict ? (int)QC_EdictFields(sv.vm, c->edict)[d->ofs].f : 0;
}

/* getinfo, info, getstatus, status; false for anything else */
bool SV_MasterConnectionless(const netadr_t *from, int argc, const char **argv)
{
	char info[2048];
	const char *c = argv[0];
	if (!strcmp(c, "getinfo") || !strcmp(c, "info")) {
		build_info(info, sizeof(info), argc > 1 ? argv[1] : NULL);
		NET_OutOfBandPrint(NS_SERVER, from, "infoResponse\n%s", info);
		return true;
	}
	if (!strcmp(c, "getstatus") || !strcmp(c, "status")) {
		strbuf_t sb = {0};
		build_info(info, sizeof(info), argc > 1 ? argv[1] : NULL);
		SB_Printf(&sb, "statusResponse\n%s\n", info);
		for (int i = 0; i < svs.maxclients && sb.len < NET_MAX_PACKET - 100; i++) {
			client_t *cl = &svs.clients[i];
			if (cl->state != CS_FREE)
				SB_Printf(&sb, "%d %d \"%s\"\n", frags_of(cl), cl->bot ? 0 : (int)(cl->chan.rtt * 1000), cl->name);
		}
		NET_OutOfBandPrint(NS_SERVER, from, "%s", sb.data);
		SB_Free(&sb);
		return true;
	}
	return false;
}

/* --- heartbeats ----------------------------------------------------------- */

static void send_heartbeats(bool verbose)
{
	int sent = 0;
	bool any = false;
	if (!NET_ServerPort()) {
		if (verbose)
			Con_Printf("heartbeat: the server isn't on the network (single player)\n");
		return;
	}
	for (int i = 0; i < NUM_MASTERS; i++)
		any |= sv_master[i]->string[0] != 0;
	for (int i = 0; any ? i < NUM_MASTERS : default_masters[i] != NULL; i++) {
		const char *m = any ? sv_master[i]->string : default_masters[i];
		netadr_t a;
		if (!*m)
			continue;
		if (!NET_StringToAdr(m, &a, 27950)) {
			Con_Printf("Failed to resolve master address \"%s\"\n", m);
			continue;
		}
		if (verbose || developer)
			Con_Printf("^9Sending heartbeat to %s (%s)\n", NET_AdrToString(&a), m);
		NET_OutOfBandPrint(NS_SERVER, &a, "heartbeat DarkPlaces\n");
		sent++;
	}
	if (verbose && !sent)
		Con_Printf("heartbeat: no master servers\n");
}

/* heartbeat: tells the master servers about this server now */
static void Heartbeat_f(int argc, const char **argv)
{
	if (!sv.active) {
		Con_Printf("heartbeat: no server running\n");
		return;
	}
	if (!sv_public->ivalue)
		Con_Printf("heartbeat: sv_public is 0, sending anyway\n");
	send_heartbeats(true);
	next_heartbeat = host_time + HEARTBEAT_SECONDS;
}

/* setmaster <address>...: replaces the master servers */
static void Setmaster_f(int argc, const char **argv)
{
	if (!sv_public->ivalue)
		Con_Printf("setmaster used on private server (sv_public is \"%s\")\n", sv_public->string);
	if (argc < 2) {
		for (int i = 0; i < NUM_MASTERS; i++)
			if (sv_master[i]->string[0])
				Con_Printf("%s: %s\n", sv_master[i]->name, sv_master[i]->string);
		return;
	}
	for (int i = 0; i < NUM_MASTERS; i++)
		Cvar_Set(sv_master[i]->name, i + 1 < argc ? argv[i + 1] : "");
	if (sv.active && sv_public->ivalue)
		send_heartbeats(true);
}

/* --- extra ports ------------------------------------------------------------ */

static void list_port(const char *desc, void *ctx) { Con_Printf("  %s\n", desc); }

/* sv_addport [port]: listens on another UDP port too */
static void SvAddport_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("Active Server ports:\n");
		NET_ListPorts(NS_SERVER, list_port, NULL);
		Con_Printf("end of list\n");
		return;
	}
	if (!NET_ServerPort()) {
		Con_Printf("sv_addport: the server isn't on the network\n");
		return;
	}
	if (NET_AddPort(NS_SERVER, atoi(argv[1])))
		Con_Printf("also listening on port %s\n", argv[1]);
}

/* --- NAT-PMP (RFC 6886): asking the router to forward our ports ----------------- */

static struct {
	int fd;
	struct sockaddr_in gw;
	int stage, tries; /* 0 idle, 1 public address, 2 udp map, 3 tcp map */
	double next, renew;
	int port;
} pmp = {.fd = -1};

static bool default_gateway(struct in_addr *out)
{
	FILE *f = fopen("/proc/net/route", "r");
	char line[256];
	bool ok = false;
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		char iface[64];
		unsigned dest, gw, flags;
		if (sscanf(line, "%63s %x %x %x", iface, &dest, &gw, &flags) == 4 && dest == 0 && (flags & 2)) {
			out->s_addr = gw; /* already in network order as the kernel prints it */
			ok = true;
			break;
		}
	}
	fclose(f);
	return ok;
}

static void pmp_send(void)
{
	uint8_t req[12] = {0};
	size_t len = 2;
	req[1] = pmp.stage == 1 ? 0 : pmp.stage == 2 ? 1 : 2;
	if (pmp.stage >= 2) {
		uint32_t life = htonl(7200);
		req[4] = req[6] = (uint8_t)(pmp.port >> 8);
		req[5] = req[7] = (uint8_t)pmp.port;
		memcpy(req + 8, &life, 4);
		len = 12;
	}
	sendto(pmp.fd, req, len, 0, (struct sockaddr *)&pmp.gw, sizeof(pmp.gw));
	pmp.next = host_time + 0.25 * (1 << pmp.tries);
}

/* starts mapping port through the router at gateway ("" finds it) */
static bool pmp_start(const char *gateway, int port)
{
	struct in_addr gw;
	int gwport = 5351;
	if (gateway && *gateway && strcmp(gateway, "1")) {
		char host[64], *colon;
		Str_Copy(host, gateway, sizeof(host));
		if ((colon = strchr(host, ':')))
			*colon = 0, gwport = atoi(colon + 1);
		if (!inet_pton(AF_INET, host, &gw)) {
			if (atoi(gateway) > 1 && !strchr(gateway, '.')) {
				gwport = atoi(gateway);
				if (!default_gateway(&gw))
					return false;
			} else {
				Con_Printf("NAT-PMP: bad router address %s\n", gateway);
				return false;
			}
		}
	} else if (!default_gateway(&gw)) {
		Con_Printf("NAT-PMP: no default route to find the router by\n");
		return false;
	}
	if (pmp.fd < 0) {
		pmp.fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (pmp.fd < 0)
			return false;
		fcntl(pmp.fd, F_SETFL, O_NONBLOCK);
	}
	memset(&pmp.gw, 0, sizeof(pmp.gw));
	pmp.gw.sin_family = AF_INET;
	pmp.gw.sin_addr = gw;
	pmp.gw.sin_port = htons((uint16_t)gwport);
	pmp.port = port;
	pmp.stage = 1;
	pmp.tries = 0;
	pmp_send();
	return true;
}

static void pmp_frame(void)
{
	uint8_t r[16];
	if (pmp.fd < 0)
		return;
	if (!pmp.stage) {
		if (pmp.renew && host_time > pmp.renew && NET_ServerPort()) {
			pmp.renew = 0;
			pmp_start(sv_port_natpmp->string, NET_ServerPort());
		}
		return;
	}
	ssize_t n = recv(pmp.fd, r, sizeof(r), 0);
	if (n >= 8 && r[0] == 0 && r[1] >= 128) {
		int result = r[2] << 8 | r[3];
		if (result) {
			Con_Printf("NAT-PMP: the router refused (result %d)\n", result);
			pmp.stage = 0;
			return;
		}
		if (r[1] == 128 && n >= 12) {
			char ip[32];
			inet_ntop(AF_INET, r + 8, ip, sizeof(ip));
			Con_Printf("NAT-PMP: public address %s\n", ip);
		} else if (n >= 16) {
			unsigned in = r[8] << 8 | r[9], ext = r[10] << 8 | r[11];
			uint32_t life;
			memcpy(&life, r + 12, 4);
			Con_Printf("NAT-PMP: Local %s port %u publicly available on port %u\n", r[1] == 129 ? "udp" : "tcp", in, ext);
			pmp.renew = host_time + ntohl(life) / 2.0;
		}
		pmp.stage++;
		pmp.tries = 0;
		/* TCP only matters when there's a TCP port */
		if (pmp.stage == 3 && !sv_port_tcp->ivalue)
			pmp.stage = 0;
		if (pmp.stage > 3)
			pmp.stage = 0;
		if (pmp.stage)
			pmp_send();
		return;
	}
	if (host_time >= pmp.next) {
		if (++pmp.tries > 6) {
			Con_Printf("NAT-PMP: no answer from %s\n", inet_ntoa(pmp.gw.sin_addr));
			pmp.stage = 0;
			return;
		}
		pmp_send();
	}
}

/* openroute [router[:port]]: asks the router to forward the server's port */
static void Openroute_f(int argc, const char **argv)
{
	if (!NET_ServerPort()) {
		Con_Printf("openroute: the server isn't on the network\n");
		return;
	}
	pmp_start(argc > 1 ? argv[1] : sv_port_natpmp->string, NET_ServerPort());
}

/* --- pinned messages -------------------------------------------------------- */

typedef struct {
	char *text, *author;
} pin_t;

static pin_t *pins;
static int numpins;

static void free_pins(void)
{
	for (int i = 0; i < numpins; i++) {
		Mem_Free(pins[i].text);
		Mem_Free(pins[i].author);
	}
	Mem_Free(pins);
	pins = NULL;
	numpins = 0;
}

static void add_pin(const char *text, const char *author)
{
	pins = Mem_Realloc(pins, (size_t)(numpins + 1) * sizeof(*pins));
	pins[numpins].text = Mem_Strdup(text);
	pins[numpins].author = Mem_Strdup(author);
	numpins++;
}

/* pinned.txt: each message is its text, a tab-indented author line and a
 * blank line */
static void load_pins(void)
{
	char *data = FS_LoadFile("pinned.txt", NULL);
	free_pins();
	if (!data)
		return;
	char *p = data;
	while (*p) {
		char *text = p, *e = strchr(p, '\n');
		if (!e)
			break;
		*e = 0;
		p = e + 1;
		if (!*text && *p != '\t')
			continue; /* blank separator */
		const char *author = "";
		if (*p == '\t') {
			author = p + 1;
			e = strchr(p, '\n');
			if (e)
				*e = 0, p = e + 1;
			else
				p += strlen(p);
		}
		add_pin(text, author);
	}
	Mem_Free(data);
}

static void save_pins(void)
{
	char path[1024];
	if (!FS_WritePath("pinned.txt", path, sizeof(path)) || !FS_CreatePath(path))
		return;
	FILE *f = fopen(path, "w");
	if (!f) {
		Con_Printf("^&C0couldn't write to %s\n", path);
		return;
	}
	for (int i = 0; i < numpins; i++)
		fprintf(f, "%s\n\t%s\n\n", pins[i].text, pins[i].author);
	fclose(f);
}

/* shown to each player as they join */
void SV_GreetPins(client_t *c)
{
	for (int i = 0; i < numpins; i++)
		SV_ClientPrint(c, 2, va("^3%s%s%s\n", pins[i].text, pins[i].author[0] ? " ^7- " : "", pins[i].author));
}

/* pin_add <author> <message...> */
static void PinAdd_f(int argc, const char **argv)
{
	if (argc < 3) {
		Con_Printf("pin_add <author> <message>\n");
		return;
	}
	char text[1024] = "";
	for (int i = 2; i < argc; i++) {
		if (i > 2)
			Str_Cat(text, " ", sizeof(text));
		Str_Cat(text, argv[i], sizeof(text));
	}
	add_pin(text, argv[1]);
	save_pins();
	Con_Printf("pinned message %d\n", numpins);
}

/* pin_delete <number|all> */
static void PinDelete_f(int argc, const char **argv)
{
	if (argc < 2) {
		for (int i = 0; i < numpins; i++)
			Con_Printf("%d: %s (%s)\n", i + 1, pins[i].text, pins[i].author);
		Con_Printf("pin_delete <number|all>\n");
		return;
	}
	if (!Str_ICmp(argv[1], "all")) {
		free_pins();
	} else {
		int i = atoi(argv[1]) - 1;
		if (i < 0 || i >= numpins) {
			Con_Printf("no pinned message %s\n", argv[1]);
			return;
		}
		Mem_Free(pins[i].text);
		Mem_Free(pins[i].author);
		memmove(pins + i, pins + i + 1, (size_t)(numpins - i - 1) * sizeof(*pins));
		numpins--;
	}
	save_pins();
}

static void PinReload_f(int argc, const char **argv)
{
	load_pins();
	Con_Printf("%d pinned message%s\n", numpins, numpins == 1 ? "" : "s");
}

static void PinSave_f(int argc, const char **argv) { save_pins(); }

/* --- hooks ------------------------------------------------------------------- */

/* the UDP port just opened: the TCP port and the router follow */
void SV_MasterPortsOpened(void)
{
	if (sv_port_tcp->ivalue)
		NET_OpenServerTCP(sv_port_tcp->ivalue);
	if (sv_port_natpmp->string[0] && strcmp(sv_port_natpmp->string, "0"))
		pmp_start(sv_port_natpmp->string, NET_ServerPort());
}

void SV_MasterMapSpawned(void)
{
	load_pins();
	/* a new map is news to the masters */
	if (sv_public->ivalue > 0)
		next_heartbeat = host_time + 2;
}

void SV_MasterFrame(void)
{
	pmp_frame();
	if (sv_public->ivalue > 0 && sv.active && host_time >= next_heartbeat) {
		next_heartbeat = host_time + HEARTBEAT_SECONDS;
		send_heartbeats(false);
	}
}

void SV_InitMaster(void)
{
	hostname = Cvar_Get("hostname", "Bulwark server", CVAR_SERVERINFO, "Server name shown in browsers");
	sv_public = Cvar_Get("sv_public", "0", 0, "1 lists the server on the master servers; 0 keeps it private");
	for (int i = 0; i < NUM_MASTERS; i++)
		sv_master[i] = Cvar_Get(va("sv_master%d", i + 1), "", 0, "A master server to send heartbeats to");
	sv_port_tcp = Cvar_Get("sv_port_tcp", "0", 0, "TCP port for clients that connect with tcp://, 0 for none");
	sv_port_natpmp = Cvar_Get("sv_port_natpmp", "", 0,
	                          "If set (typically to 5351), asks the router to forward the server's port with NAT-PMP; "
	                          "or the router's address, 192.168.1.1:5351 for example");
	Cmd_AddD("heartbeat", Heartbeat_f, "Sends an update or ping to the master servers so the server stays listed");
	Cmd_AddD("setmaster", Setmaster_f, "setmaster <address>...: replace the master servers");
	Cmd_AddD("sv_addport", SvAddport_f, "sv_addport [port]: listen on another UDP port; lists the ports without one");
	Cmd_AddD("openroute", Openroute_f, "Ask the router to forward the server's port (NAT-PMP)");
	Cmd_AddD("pin_add", PinAdd_f, "pin_add <author> <message>: a message shown to players as they join");
	Cmd_AddD("pin_delete", PinDelete_f, "pin_delete <number|all>: remove pinned messages");
	Cmd_AddD("pin_reload", PinReload_f, "Read pinned.txt again");
	Cmd_AddD("pin_save", PinSave_f, "Write the pinned messages to pinned.txt");
}
