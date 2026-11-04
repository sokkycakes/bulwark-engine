/* Bulwark engine - the DS as a link player: it joins a game running on a PC
 * over WiFi, like a GBA on a link cable. ISC licence, see LICENSE.
 *
 * docs/spec/link.md has the protocol. The DS runs none of the game. It sends
 * its buttons and gets back a private screen (drawn on the bottom screen) and
 * where its player is (the top screen draws the map from there, when the DS
 * has a copy of it, and the other players as coloured markers).
 *
 * WiFi comes from the DS's own settings, or from link.txt in a base folder:
 *   ssid=  key=  security=open|wep|wpa|wpa2  server=ip[:port]  name=  autojoin=1
 */
#include <arpa/inet.h>
#include <dswifi9.h>
#include <nds.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <wfc.h>

#include "nds.h"

#define LINK_PORT 27901
#define MAX_SERVERS 8
#define MAX_MARKERS 64
#define HZ 60

enum { L_DISCOVER = 1, L_JOIN, L_INPUT, L_LEAVE };
enum { L_INFO = 0x81, L_WELCOME, L_REFUSE, L_SCREEN, L_VIEW, L_KICK, L_ENTS };

#define MAX_LINKMODELS 1024 /* brush models the server can move */
#define MAX_THINGS 48
enum { SC_CLEAR = 1, SC_FILL, SC_TEXT };

extern const u8 default_font_bin[]; /* libnds' console font: 256 characters, 8x8, 1 bit */

typedef struct {
	char ssid[33], key[65], security[8], name[16];
	uint32_t server; /* network order, 0 for none */
	uint16_t port;
	bool autojoin;
} linkcfg_t;

typedef struct {
	struct sockaddr_in addr;
	char host[33], map[33];
	int players, max, free;
} server_t;

typedef struct {
	int16_t org[3];
	uint8_t kind;
	uint16_t rgb;
} marker_t;

typedef struct {
	int16_t org[3]; /* the bottom middle, map units */
	uint8_t yaw, half, height, kind;
} thing_t;

/* each brush model as the server last had it, and as drawn (easing toward it) */
typedef struct {
	bool moved;
	int32_t target[3], cur[3]; /* 1/8 units */
	int16_t tang[3], cang[3];  /* 65536 to a turn */
} linkmodel_t;

static linkcfg_t cfg;
static int sock = -1;
static uint16_t *sub; /* the bottom screen, 256x256 RGB15 (192 rows shown) */
static server_t servers[MAX_SERVERS];
static int numservers;
static const uint16_t *palette;

static struct {
	int fps, minfps, peakpolys; /* the last second, sent with the input */
	int loadms;                 /* the last map load */
	struct sockaddr_in to;
	uint32_t session;
	uint16_t seq, screen_ack, spawncount;
	int lastrecv; /* frame */
	char map[65];
	int32_t target[3];
	bool haveview;
	marker_t markers[MAX_MARKERS];
	int nummarkers;
	int health;
	uint16_t entsseq;
	bool haveents;
	thing_t things[MAX_THINGS];
	int numthings;
} link;

static linkmodel_t models[MAX_LINKMODELS];

/* --- link.txt ------------------------------------------------------------------------ */

static void read_config(void)
{
	char path[64], line[160];
	memset(&cfg, 0, sizeof(cfg));
	cfg.port = LINK_PORT;
	snprintf(cfg.name, sizeof(cfg.name), "DS");
	for (int b = 0; b < NDS_NUMBASEDIRS; b++) {
		FILE *f;
		snprintf(path, sizeof(path), "%slink.txt", nds_basedirs[b]);
		if (!(f = fopen(path, "r")))
			continue;
		while (fgets(line, sizeof(line), f)) {
			char *v = strchr(line, '=');
			line[strcspn(line, "\r\n")] = 0;
			if (!v)
				continue;
			*v++ = 0;
			if (!strcmp(line, "ssid"))
				snprintf(cfg.ssid, sizeof(cfg.ssid), "%s", v);
			else if (!strcmp(line, "key"))
				snprintf(cfg.key, sizeof(cfg.key), "%s", v);
			else if (!strcmp(line, "security"))
				snprintf(cfg.security, sizeof(cfg.security), "%s", v);
			else if (!strcmp(line, "name"))
				snprintf(cfg.name, sizeof(cfg.name), "%s", v);
			else if (!strcmp(line, "autojoin"))
				cfg.autojoin = atoi(v) != 0;
			else if (!strcmp(line, "server")) {
				char *colon = strchr(v, ':');
				if (colon)
					*colon = 0, cfg.port = (uint16_t)atoi(colon + 1);
				cfg.server = inet_addr(v);
				if (cfg.server == INADDR_NONE)
					cfg.server = 0;
			}
		}
		fclose(f);
		return;
	}
}

bool Link_AutoJoin(void)
{
	read_config();
	return cfg.autojoin;
}

/* --- WiFi ---------------------------------------------------------------------------- */

static bool wait_keys_or(int frames, uint32_t keys)
{
	for (int i = 0; i < frames; i++) {
		swiWaitForVBlank();
		scanKeys();
		if (keysDown() & keys)
			return true;
	}
	return false;
}

static int hexval(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* the most secure authentication the network offers that link.txt allows */
static int pick_auth(unsigned mask)
{
	unsigned allow = 0xff;
	if (!strcmp(cfg.security, "open"))
		allow = 1u << WlanBssAuthType_Open;
	else if (!strcmp(cfg.security, "wep"))
		allow = 0x0e;
	else if (!strcmp(cfg.security, "wpa"))
		allow = (1u << WlanBssAuthType_WPA_PSK_TKIP) | (1u << WlanBssAuthType_WPA_PSK_AES);
	else if (!strcmp(cfg.security, "wpa2"))
		allow = (1u << WlanBssAuthType_WPA2_PSK_TKIP) | (1u << WlanBssAuthType_WPA2_PSK_AES);
	mask &= allow;
	for (int t = 7; t >= 0; t--)
		if (mask & (1u << t))
			return t;
	return -1;
}

static bool connect_ssid(void)
{
	static WlanBssDesc bss;
	WlanAuthData auth;
	WlanBssDesc *list;
	unsigned count = 0;
	int t, found = -1;

	iprintf("Looking for %s...\n", cfg.ssid);
	if (!wfcBeginScan(NULL))
		return false;
	for (int i = 0; i < 10 * HZ && wfcGetStatus() == WfcStatus_Scanning; i++)
		swiWaitForVBlank();
	list = wfcGetScanBssList(&count);
	for (unsigned i = 0; list && i < count && found < 0; i++)
		if (list[i].ssid_len == strlen(cfg.ssid) && !memcmp(list[i].ssid, cfg.ssid, list[i].ssid_len))
			found = (int)i;
	if (found < 0) {
		iprintf("%s not found (%u networks seen)\n", cfg.ssid, count);
		return false;
	}
	bss = list[found];
	if ((t = pick_auth(bss.auth_mask)) < 0) {
		iprintf("%s doesn't offer %s\n", cfg.ssid, cfg.security);
		return false;
	}
	memset(&auth, 0, sizeof(auth));
	if (t >= WlanBssAuthType_WPA_PSK_TKIP) {
		if (!wfcDeriveWpaKey(&auth, cfg.ssid, strlen(cfg.ssid), cfg.key, strlen(cfg.key)))
			return false;
	} else if (t != WlanBssAuthType_Open) {
		/* WEP: the key in hex; 10, 26 or 32 digits */
		int n = (int)strlen(cfg.key) / 2;
		for (int i = 0; i < n && i < WLAN_WEP_128_LEN; i++)
			auth.wep_key[i] = (uint8_t)(hexval(cfg.key[i * 2]) << 4 | hexval(cfg.key[i * 2 + 1]));
		t = n <= WLAN_WEP_40_LEN ? WlanBssAuthType_WEP_40 : n <= WLAN_WEP_104_LEN ? WlanBssAuthType_WEP_104 : WlanBssAuthType_WEP_128;
	}
	bss.auth_type = (WlanBssAuthType)t;
	return wfcBeginConnect(&bss, &auth);
}

static bool wifi_connect(void)
{
	static const char *const status[] = {"off", "scanning", "connecting", "getting an address", "connected"};
	int last = -1;
	if (wfcGetStatus() == WfcStatus_Connected)
		return true;
	iprintf("Starting WiFi...\n");
	if (!Wifi_InitDefault(INIT_ONLY)) {
		iprintf("WiFi didn't start\n");
		return false;
	}
	if (cfg.ssid[0]) {
		if (!connect_ssid())
			return false;
	} else {
		iprintf("Using the DS's WiFi settings\n");
		Wifi_AutoConnect();
	}
	for (int i = 0; i < 30 * HZ; i++) {
		int s = wfcGetStatus();
		if (s != last && s >= 0 && s <= WfcStatus_Connected)
			iprintf("WiFi: %s\n", status[s]), last = s;
		if (s == WfcStatus_Connected) {
			struct in_addr ip = {Wifi_GetIP()};
			iprintf("Address %s\n", inet_ntoa(ip));
			return true;
		}
		swiWaitForVBlank();
		scanKeys();
		if (keysDown() & KEY_START)
			return false;
	}
	iprintf("WiFi timed out\n");
	return false;
}

/* --- packets ------------------------------------------------------------------------- */

typedef struct {
	uint8_t d[96];
	int n;
} pkt_t;

static void p_begin(pkt_t *p, int type) { memcpy(p->d, "BWL1", 4), p->d[4] = (uint8_t)type, p->n = 5; }
static void p_u8(pkt_t *p, int v) { p->d[p->n++] = (uint8_t)v; }
static void p_u16(pkt_t *p, int v) { p_u8(p, v), p_u8(p, v >> 8); }
static void p_u32(pkt_t *p, uint32_t v) { p_u16(p, (int)(v & 0xffff)), p_u16(p, (int)(v >> 16)); }
static unsigned rd16(const uint8_t *d) { return d[0] | d[1] << 8; }
static uint32_t rd32(const uint8_t *d) { return rd16(d) | (uint32_t)rd16(d + 2) << 16; }

static void send_to(const struct sockaddr_in *to, const pkt_t *p)
{
	sendto(sock, p->d, p->n, 0, (const struct sockaddr *)to, sizeof(*to));
}

static bool open_socket(void)
{
	int one = 1;
	if (sock >= 0)
		return true;
	if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return false;
	ioctl(sock, FIONBIO, &one);
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	return true;
}

static void close_socket(void)
{
	if (sock >= 0)
		closesocket(sock);
	sock = -1;
}

static struct sockaddr_in addr_of(uint32_t ip, uint16_t port)
{
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = ip;
	return a;
}

/* one packet or 0; type in *type, payload after the type byte */
static int receive(uint8_t *buf, int size, struct sockaddr_in *from, int *type)
{
	socklen_t fl = sizeof(*from);
	int n = recvfrom(sock, buf, size, 0, (struct sockaddr *)from, &fl);
	if (n < 5 || memcmp(buf, "BWL1", 4))
		return 0;
	*type = buf[4];
	return n - 5;
}

static void copy_str(char *dst, int size, const uint8_t *src, int len)
{
	int n = 0;
	while (n < len && n < size - 1 && src[n])
		dst[n] = (char)src[n], n++;
	dst[n] = 0;
}

/* --- the bottom screen --------------------------------------------------------------- */

static void screen_init(void)
{
	int bg;
	videoSetModeSub(MODE_5_2D);
	vramSetBankC(VRAM_C_SUB_BG);
	bg = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	sub = bgGetGfxPtr(bg);
	dmaFillHalfWords(0x8000, sub, 256 * 192 * 2);
}

static void screen_done(void)
{
	sub = NULL;
	Nds_InitConsole();
	Mp_Init();
}

static void fill(int x, int y, int w, int h, uint16_t c)
{
	if (x >= 256 || y >= 192)
		return;
	w = BW_NDS_MIN(w, 256 - x), h = BW_NDS_MIN(h, 192 - y);
	for (int j = 0; j < h; j++) {
		uint16_t *row = sub + (y + j) * 256 + x;
		for (int i = 0; i < w; i++)
			row[i] = c;
	}
}

static void text(int x, int y, uint16_t c, const uint8_t *s, int len)
{
	for (int k = 0; k < len && x + 8 <= 256; k++, x += 8) {
		const u8 *glyph = default_font_bin + s[k] * 8;
		for (int j = 0; j < 8 && y + j < 192; j++) {
			uint16_t *row = sub + (y + j) * 256 + x;
			for (int i = 0; i < 8; i++)
				if (glyph[j] & (1 << i)) /* bit 0 is the left pixel */
					row[i] = c;
		}
	}
}

static void draw_screen(const uint8_t *d, int len)
{
	int i = 0;
	while (i < len) {
		switch (d[i]) {
		case SC_CLEAR:
			if (i + 3 > len)
				return;
			fill(0, 0, 256, 192, (uint16_t)(rd16(d + i + 1) | 0x8000));
			i += 3;
			break;
		case SC_FILL:
			if (i + 7 > len)
				return;
			fill(d[i + 1], d[i + 2], d[i + 3] ? d[i + 3] : 256, d[i + 4] ? d[i + 4] : 256,
			     (uint16_t)(rd16(d + i + 5) | 0x8000));
			i += 7;
			break;
		case SC_TEXT:
			if (i + 6 > len || i + 6 + d[i + 5] > len)
				return;
			text(d[i + 1], d[i + 2], (uint16_t)(rd16(d + i + 3) | 0x8000), d + i + 6, d[i + 5]);
			i += 6 + d[i + 5];
			break;
		default:
			return;
		}
	}
}

/* --- finding and joining a server ---------------------------------------------------- */

static void add_server(const struct sockaddr_in *from, const uint8_t *d, int len)
{
	server_t *s = NULL;
	if (len < 67)
		return;
	for (int i = 0; i < numservers; i++)
		if (servers[i].addr.sin_addr.s_addr == from->sin_addr.s_addr && servers[i].addr.sin_port == from->sin_port)
			s = &servers[i];
	if (!s) {
		if (numservers >= MAX_SERVERS)
			return;
		s = &servers[numservers++];
	}
	s->addr = *from;
	copy_str(s->host, sizeof(s->host), d, 32);
	copy_str(s->map, sizeof(s->map), d + 32, 32);
	s->players = d[64], s->max = d[65], s->free = d[66];
}

static void list_servers(int sel)
{
	consoleClear();
	iprintf("Bulwark DS - join a game\n\n");
	if (!numservers)
		iprintf("Looking for games...\n");
	for (int i = 0; i < numservers; i++) {
		const server_t *s = &servers[i];
		iprintf("%c %.20s\n    %.14s %d/%d %s\n", i == sel ? '>' : ' ', s->host, s->map, s->players, s->max,
		        s->free ? "" : "full");
	}
	iprintf("\nA: join  START: back\n");
}

/* broadcasts DISCOVER (and asks the server in link.txt); returns the server picked or -1 */
static int find_server(void)
{
	uint8_t buf[256];
	struct sockaddr_in from, bcast = addr_of(INADDR_BROADCAST, cfg.port), direct = addr_of(cfg.server, cfg.port);
	int sel = 0, shown = -1;
	numservers = 0;
	for (int frame = 0;; frame++) {
		int len, type;
		if (frame % HZ == 0) {
			pkt_t p;
			p_begin(&p, L_DISCOVER);
			send_to(&bcast, &p);
			if (cfg.server)
				send_to(&direct, &p);
		}
		while ((len = receive(buf, sizeof(buf), &from, &type)) > 0)
			if (type == L_INFO)
				add_server(&from, buf + 5, len);
		if (cfg.autojoin && numservers)
			return 0;
		if (shown != numservers * 16 + sel) {
			list_servers(sel);
			shown = numservers * 16 + sel;
		}
		swiWaitForVBlank();
		scanKeys();
		uint32_t down = keysDown();
		if (down & KEY_START)
			return -1;
		if ((down & KEY_A) && numservers)
			return sel;
		if ((down & KEY_DOWN) && sel + 1 < numservers)
			sel++;
		if ((down & KEY_UP) && sel > 0)
			sel--;
		if (down & KEY_TOUCH) {
			touchPosition tp;
			int row;
			touchRead(&tp);
			row = (tp.py / 8 - 2) / 2;
			if (tp.py >= 16 && row < numservers)
				return row;
		}
	}
}

static void handle_welcome(const uint8_t *d, int len, ndscam_t *cam);

static bool join(const server_t *s, ndscam_t *cam)
{
	uint8_t buf[256];
	struct sockaddr_in from;
	pkt_t p;
	memset(&link, 0, sizeof(link));
	link.to = s->addr;
	link.session = (uint32_t)rand() ^ (uint32_t)time(NULL) << 8 ^ Wifi_GetIP();
	if (!link.session)
		link.session = 1;
	iprintf("Joining %s...\n", s->host);
	p_begin(&p, L_JOIN);
	p_u32(&p, link.session);
	p_u16(&p, 1); /* a DS */
	for (int i = 0; i < 16; i++)
		p_u8(&p, i < (int)strlen(cfg.name) ? cfg.name[i] : 0);
	for (int frame = 0; frame < 6 * HZ; frame++) {
		int len, type;
		if (frame % HZ == 0)
			send_to(&link.to, &p);
		while ((len = receive(buf, sizeof(buf), &from, &type)) > 0) {
			const uint8_t *d = buf + 5;
			if (type == L_WELCOME && len >= 7 && rd32(d) == link.session) {
				handle_welcome(d, len, cam);
				return true;
			}
			if (type == L_REFUSE) {
				char reason[65];
				copy_str(reason, sizeof(reason), d, len);
				iprintf("Refused: %s\n", reason);
				return false;
			}
		}
		swiWaitForVBlank();
	}
	iprintf("No answer\n");
	return false;
}

/* --- linked -------------------------------------------------------------------------- */

static void handle_welcome(const uint8_t *d, int len, ndscam_t *cam)
{
	char map[65], path[300], err[96];
	uint16_t spawncount = (uint16_t)rd16(d + 5);
	copy_str(map, sizeof(map), d + 7, len - 7);
	link.spawncount = spawncount;
	if (World_Loaded() && !strcmp(map, link.map))
		return;
	snprintf(link.map, sizeof(link.map), "%s", map);
	World_Free();
	Nds_ResetTextures();
	link.haveview = false;
	link.haveents = false;
	link.numthings = 0;
	memset(models, 0, sizeof(models));
	snprintf(path, sizeof(path), "maps/%s.bsp", map);
	{
		int start = Nds_Vblanks();
		if (World_Load(path, palette, err, sizeof(err)))
			World_StartPosition(cam);
		link.loadms = (Nds_Vblanks() - start) * 1000 / 60;
	}
}

static void handle_view(const uint8_t *d, int len, ndscam_t *cam)
{
	int count;
	if (len < 4 + 2 + 12 + 6 + 2)
		return;
	for (int k = 0; k < 3; k++)
		link.target[k] = (int32_t)rd32(d + 6 + k * 4);
	link.health = (int16_t)rd16(d + 22);
	if (d[24] & 1) { /* the game turned us */
		cam->pitch = (int16_t)rd16(d + 18) / 2;
		cam->yaw = (int16_t)rd16(d + 20) / 2;
	}
	if (!link.haveview)
		memcpy(cam->org, link.target, sizeof(cam->org));
	link.haveview = true;
	count = d[25];
	link.nummarkers = 0;
	for (int i = 0; i < count && i < MAX_MARKERS && 26 + (i + 1) * 9 <= len; i++) {
		const uint8_t *m = d + 26 + i * 9;
		marker_t *mk = &link.markers[link.nummarkers++];
		for (int k = 0; k < 3; k++)
			mk->org[k] = (int16_t)rd16(m + k * 2);
		mk->kind = m[6];
		mk->rgb = (uint16_t)rd16(m + 7);
	}
}

/* which brush models are there and where the moved ones are, then the things
 * near us (docs/spec/link.md, ENTS) */
static void handle_ents(const uint8_t *d, int len)
{
	uint16_t seq = (uint16_t)rd16(d + 4);
	int n, pos, count;
	if (len < 9 || (link.haveents && (int16_t)(seq - link.entsseq) <= 0))
		return;
	n = BW_NDS_MIN((int)rd16(d + 6), MAX_LINKMODELS);
	pos = 8 + ((int)rd16(d + 6) + 7) / 8;
	if (pos >= len)
		return;
	link.entsseq = seq;
	link.haveents = true;
	for (int m = 1; m < n; m++)
		models[m].moved = false;
	count = d[pos++];
	for (int i = 0; i < count && pos + 14 <= len; i++, pos += 14) {
		int m = rd16(d + pos);
		linkmodel_t *lm;
		if (m <= 0 || m >= MAX_LINKMODELS)
			continue;
		lm = &models[m];
		for (int c = 0; c < 3; c++) {
			lm->target[c] = (int16_t)rd16(d + pos + 2 + c * 2) * 2;
			lm->tang[c] = (int16_t)rd16(d + pos + 8 + c * 2);
		}
		if (!lm->moved && !(lm->cur[0] | lm->cur[1] | lm->cur[2]))
			memcpy(lm->cur, lm->target, sizeof(lm->cur)), memcpy(lm->cang, lm->tang, sizeof(lm->cang));
		lm->moved = true;
	}
	for (int m = 1; m < n; m++) {
		bool shown = (d[8 + (m >> 3)] >> (m & 7)) & 1;
		if (!models[m].moved)
			memset(models[m].cur, 0, sizeof(models[m].cur)), memset(models[m].cang, 0, sizeof(models[m].cang));
		World_ModelState(m, shown, NULL, NULL);
	}
	link.numthings = 0;
	if (pos >= len)
		return;
	count = d[pos++];
	for (int i = 0; i < count && link.numthings < MAX_THINGS && pos + 10 <= len; i++, pos += 10) {
		thing_t *t = &link.things[link.numthings++];
		for (int c = 0; c < 3; c++)
			t->org[c] = (int16_t)rd16(d + pos + c * 2);
		t->yaw = d[pos + 6], t->half = d[pos + 7], t->height = d[pos + 8], t->kind = d[pos + 9];
	}
}

/* ease the moved brush models toward where the server has them; it updates at 20 Hz */
static void move_models(void)
{
	int n = BW_NDS_MIN(World_NumModels(), MAX_LINKMODELS);
	for (int m = 1; m < n; m++) {
		linkmodel_t *lm = &models[m];
		int ang[3];
		if (!lm->moved)
			continue;
		for (int c = 0; c < 3; c++) {
			int32_t delta = lm->target[c] - lm->cur[c];
			int16_t da = (int16_t)(lm->tang[c] - lm->cang[c]);
			lm->cur[c] = delta > 1024 || delta < -1024 ? lm->target[c] : lm->cur[c] + delta / 3;
			lm->cang[c] = (int16_t)(lm->cang[c] + (da > 8192 || da < -8192 ? da : da / 3));
			ang[c] = lm->cang[c] / 2;
		}
		World_ModelState(m, true, lm->cur, ang);
	}
}

/* everything else the server sends: boxes, turned to face the way the thing does */
static void draw_things(void)
{
	static const uint16_t colours[] = {RGB15(20, 20, 20), RGB15(28, 12, 8), RGB15(28, 26, 8), RGB15(16, 18, 22), RGB15(12, 4, 4)};
	glBindTexture(GL_TEXTURE_2D, 0);
	for (int i = 0; i < link.numthings; i++) {
		const thing_t *t = &link.things[i];
		int16_t mins[3], maxs[3];
		int32_t h = t->half * 8, z = t->kind == 4 ? 4 * 8 : t->height * 8;
		int32_t c4[4][2];
		int s, c;
		for (int k = 0; k < 2; k++) {
			mins[k] = (int16_t)(t->org[k] - t->half), maxs[k] = (int16_t)(t->org[k] + t->half);
		}
		mins[2] = t->org[2], maxs[2] = (int16_t)(t->org[2] + (z >> 3));
		if (!World_BoxVisible(mins, maxs))
			continue;
		s = sinLerp((int16_t)(t->yaw * 128)), c = cosLerp((int16_t)(t->yaw * 128));
		for (int k = 0; k < 4; k++) {
			int32_t x = (k == 0 || k == 3) ? -h : h, y = k < 2 ? -h : h;
			c4[k][0] = t->org[0] * 8 + ((c * x - s * y) >> 12);
			c4[k][1] = t->org[1] * 8 + ((s * x + c * y) >> 12);
		}
		{
			int32_t z0 = t->org[2] * 8, z1 = z0 + z;
			uint16_t col = colours[t->kind < 5 ? t->kind : 0];
			glBegin(GL_QUADS);
			for (int k = 0; k < 4; k++) {
				int n = (k + 1) & 3;
				/* the sides a little darker than the top */
				glColor((uint16_t)((col >> 1) & 0x3def));
				glVertex3v16(c4[k][0], c4[k][1], z0);
				glVertex3v16(c4[n][0], c4[n][1], z0);
				glColor(col);
				glVertex3v16(c4[n][0], c4[n][1], z1);
				glVertex3v16(c4[k][0], c4[k][1], z1);
			}
			glColor(col);
			for (int k = 0; k < 4; k++)
				glVertex3v16(c4[k][0], c4[k][1], z1);
			glEnd();
		}
	}
	glColor(RGB15(31, 31, 31));
}

/* players and the game's markers: upright quads that face the camera */
static void draw_markers(const ndscam_t *cam)
{
	int rx = sinLerp(cam->yaw), ry = -cosLerp(cam->yaw); /* the camera's right, 4.12 */
	glBindTexture(GL_TEXTURE_2D, 0);
	for (int i = 0; i < link.nummarkers; i++) {
		const marker_t *m = &link.markers[i];
		int hw = m->kind ? 8 : 16, lo = m->kind ? -8 : -24, hi = m->kind ? 8 : 32;
		int x = BW_NDS_CLAMP(m->org[0], -4000, 4000) * 8, y = BW_NDS_CLAMP(m->org[1], -4000, 4000) * 8;
		int z = BW_NDS_CLAMP(m->org[2], -4000, 4000) * 8;
		int dx = rx * hw * 8 >> 12, dy = ry * hw * 8 >> 12;
		glColor(m->rgb);
		glBegin(GL_QUADS);
		glVertex3v16(x - dx, y - dy, z + lo * 8);
		glVertex3v16(x + dx, y + dy, z + lo * 8);
		glVertex3v16(x + dx, y + dy, z + hi * 8);
		glVertex3v16(x - dx, y - dy, z + hi * 8);
		glEnd();
	}
	glColor(RGB15(31, 31, 31));
}

static void send_input(const ndscam_t *cam, uint32_t held)
{
	pkt_t p;
	int fwd = 0, side = 0;
	touchPosition tp = {0};
	if (held & KEY_TOUCH)
		touchRead(&tp);
	if (held & KEY_L) {
		side = held & KEY_RIGHT ? 127 : held & KEY_LEFT ? -127 : 0;
	} else
		fwd = held & KEY_UP ? 127 : held & KEY_DOWN ? -127 : 0;
	p_begin(&p, L_INPUT);
	p_u32(&p, link.session);
	p_u16(&p, ++link.seq);
	p_u16(&p, (int)(held & 0xffff));
	p_u8(&p, held & KEY_TOUCH ? 1 : 0);
	p_u8(&p, tp.px > 255 ? 255 : tp.px);
	p_u8(&p, tp.py > 191 ? 191 : tp.py);
	p_u8(&p, fwd), p_u8(&p, side), p_u8(&p, 0);
	p_u16(&p, cam->pitch * 2);
	p_u16(&p, cam->yaw * 2);
	p_u16(&p, link.screen_ack);
	p_u8(&p, link.fps), p_u8(&p, link.minfps), p_u16(&p, link.peakpolys);
	p_u16(&p, link.loadms / 10);
	send_to(&link.to, &p);
}

static void turn(ndscam_t *cam, uint32_t held)
{
	const int step = 32768 / 180;
	if (held & KEY_L) {
		if (held & KEY_UP)
			cam->pitch += step;
		if (held & KEY_DOWN)
			cam->pitch -= step;
	} else {
		if (held & KEY_LEFT)
			cam->yaw += step;
		if (held & KEY_RIGHT)
			cam->yaw -= step;
	}
	cam->pitch = BW_NDS_CLAMP(cam->pitch, -8000, 8000);
	cam->yaw = (int16_t)cam->yaw;
}

/* the linked loop; returns why it ended */
static const char *play(ndscam_t *cam)
{
	static char reason[96];
	uint8_t buf[1500];
	struct sockaddr_in from;
	ndsstats_t st = {0};
	pkt_t p;
	int secstart = Nds_Vblanks(), lastv = secstart, frames = 0, worst = 0, peak = 0, debugview = 0;
	screen_init();
	link.lastrecv = 0;
	for (int frame = 1;; frame++) {
		int len, type;
		uint32_t held;
		scanKeys();
		held = keysHeld();
		if (keysDown() & KEY_START) {
			snprintf(reason, sizeof(reason), "You left the game.");
			break;
		}
		/* SELECT: leave out the server's things, then its moved models too, to see what is what */
		if (keysDown() & KEY_SELECT)
			debugview = (debugview + 1) % 3;
		turn(cam, held);
		if (frame % 3 == 0)
			send_input(cam, held);

		while ((len = receive(buf, sizeof(buf), &from, &type)) > 0) {
			const uint8_t *d = buf + 5;
			if (from.sin_addr.s_addr != link.to.sin_addr.s_addr || len < 4 ||
			    (type != L_REFUSE && rd32(d) != link.session))
				continue;
			link.lastrecv = frame;
			if (type == L_WELCOME && len >= 7 && rd16(d + 5) != link.spawncount)
				handle_welcome(d, len, cam);
			else if (type == L_SCREEN && len >= 6) {
				uint16_t version = (uint16_t)rd16(d + 4);
				if (version != link.screen_ack) {
					draw_screen(d + 6, len - 6);
					link.screen_ack = version;
				}
			} else if (type == L_VIEW)
				handle_view(d, len, cam);
			else if (type == L_ENTS)
				handle_ents(d, len);
			else if (type == L_KICK) {
				char why[65];
				copy_str(why, sizeof(why), d + 4, len - 4);
				snprintf(reason, sizeof(reason), "The server dropped you: %s", why);
				goto done;
			}
		}
		if (frame - link.lastrecv > 5 * HZ) {
			snprintf(reason, sizeof(reason), "Lost the connection.");
			break;
		}

		/* ease toward where the server says we are; it updates at 20 Hz */
		for (int k = 0; k < 3; k++) {
			int32_t delta = link.target[k] - cam->org[k];
			cam->org[k] = delta > 2048 || delta < -2048 ? link.target[k] : cam->org[k] + delta / 3;
		}
		if (debugview == 2)
			World_ResetModels();
		else
			move_models();
		if (World_Loaded())
			World_Draw(cam, &st);
		else
			World_SetView(cam);
		if (debugview == 0)
			draw_things();
		draw_markers(cam);
		{
			int polys, verts;
			Gpu_Counts(&polys, &verts);
			peak = polys > peak ? polys : peak;
		}
		glFlush(0);
		swiWaitForVBlank();
		/* how fast this runs, on the bottom row: frames a second, the slowest frame, peak polygons */
		{
			int now = Nds_Vblanks();
			char line[33];
			worst = now - lastv > worst ? now - lastv : worst;
			lastv = now;
			frames++;
			if (now - secstart >= 60) {
				link.fps = frames * 60 / (now - secstart), link.minfps = 60 / (worst ? worst : 1);
				link.peakpolys = peak;
				int n = snprintf(line, sizeof(line), "%2d fps min %2d %4dp lf%d%s", link.fps, link.minfps, peak, st.leaf,
				                 debugview == 1 ? " -T" : debugview == 2 ? " -TM" : "");
				fill(0, 184, 256, 8, RGB15(2, 2, 4) | BIT(15));
				text(0, 184, RGB15(20, 31, 20) | BIT(15), (const uint8_t *)line, n);
				secstart = now, frames = 0, worst = 0, peak = 0;
			}
		}
	}
	p_begin(&p, L_LEAVE);
	p_u32(&p, link.session);
	for (int i = 0; i < 3; i++)
		send_to(&link.to, &p);
done:
	screen_done();
	return reason;
}

void Link_Run(const uint16_t *pal)
{
	ndscam_t cam;
	int pick;
	palette = pal;
	read_config();
	consoleClear();
	if (!wifi_connect() || !open_socket()) {
		iprintf("\nA or START: back\n");
		wait_keys_or(1 << 30, KEY_A | KEY_START);
		return;
	}
	if ((pick = find_server()) < 0) {
		close_socket();
		return;
	}
	consoleClear();
	memset(&cam, 0, sizeof(cam));
	if (!join(&servers[pick], &cam)) {
		close_socket();
		iprintf("\nA or START: back\n");
		wait_keys_or(1 << 30, KEY_A | KEY_START);
		return;
	}
	iprintf("%s\n", play(&cam));
	close_socket();
	World_Free();
	Nds_ResetTextures();
	iprintf("A or START: back\n");
	wait_keys_or(1 << 30, KEY_A | KEY_START);
}
