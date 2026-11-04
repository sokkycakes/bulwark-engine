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
 * and the controls: controls=touch|classic  hand=right|left  sens=100 (percent)
 * diag=1 (frame rate and polygons on the bottom screen's top bar)
 *
 * The bottom screen is a touch pad, as in Metroid Prime Hunters: drag in the
 * middle to aim, double-tap to jump, and buttons below for weapons, reload,
 * use, jump, duck, flashlight and secondary fire. The tab at the top right
 * swaps it for the game's own bottom screen (SCREEN packets) and back.
 */
#include <arpa/inet.h>
#include <dswifi9.h>
#include <nds.h>
#include <netinet/in.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <strings.h>
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
enum { L_INFO = 0x81, L_WELCOME, L_REFUSE, L_SCREEN, L_VIEW, L_KICK, L_ENTS, L_MODELS };

#define MAX_LINKMODELS 1024 /* brush models the server can move */
#define MAX_THINGS 48
#define MAX_STUDIO 1032     /* model indexes the server can name; 1000 on are players' own */
enum { SC_CLEAR = 1, SC_FILL, SC_TEXT };

extern const u8 default_font_bin[]; /* libnds' console font: 256 characters, 8x8, 1 bit */

typedef struct {
	char ssid[33], key[65], security[8], name[16];
	uint32_t server; /* network order, 0 for none */
	uint16_t port;
	bool autojoin;
	bool classic; /* D-pad turns, as Quake and Half-Life on a pad with one stick */
	bool lefty;   /* move with A B X Y, fire with R */
	bool diag;
	int sens;     /* aim speed, percent: 100 turns 120 degrees across the screen */
} linkcfg_t;

/* what the game reads in input_buttons (Nuclide's INPUT_BUTTON*) */
enum { B_ATTACK = 0x01, B_JUMP = 0x02, B_RELOAD = 0x08, B_ATTACK2 = 0x10, B_USE = 0x20, B_CROUCH = 0x80 };
/* Quake's next and previous weapon, Half-Life's flashlight */
enum { IMP_NEXT = 10, IMP_PREV = 12, IMP_LIGHT = 100 };

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
	uint16_t ent, model; /* entity number; studio model index, 0 for none */
	uint8_t seq, lift;   /* the sequence it plays; its origin above the bottom */
	int8_t anim;         /* its slot in anim[], -1 for none */
} thing_t;

/* the animation of the entities we were last sent, as we saw it: what each
 * played and since when; a small table, the oldest slot reused */
#define MAX_ANIM 96
static struct {
	uint16_t ent;
	uint8_t seq;
	int start, seen; /* frame the sequence started; frame last sent */
	int16_t prev[3];
	int16_t speed; /* map units a second, eased */
	int16_t lightat[3]; /* where the light was looked up */
	/* light -1: not yet */
	int8_t light;
} anim[MAX_ANIM];

static int anim_slot(int ent, int frame)
{
	int old = 0;
	for (int i = 0; i < MAX_ANIM; i++) {
		if (anim[i].ent == ent)
			return i;
		if (anim[i].seen < anim[old].seen)
			old = i;
	}
	memset(&anim[old], 0, sizeof(anim[old]));
	anim[old].ent = (uint16_t)ent;
	anim[old].seen = frame;
	anim[old].light = -1;
	return old;
}

/* the server's studio model names, by model index, and what loading gave */
static char *studioname[MAX_STUDIO];
static int16_t studiomdl[MAX_STUDIO]; /* -2 not tried, -1 can't, else Ents_Model's */
static int linkframe;

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
	char loaderr[40];           /* why the last map didn't load */
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
	cfg.sens = 100;
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
			else if (!strcmp(line, "controls"))
				cfg.classic = !strcmp(v, "classic");
			else if (!strcmp(line, "hand"))
				cfg.lefty = !strcmp(v, "left");
			else if (!strcmp(line, "sens"))
				cfg.sens = BW_NDS_CLAMP(atoi(v), 10, 1000);
			else if (!strcmp(line, "diag"))
				cfg.diag = atoi(v) != 0;
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
	/* bank H holds the console at the same address: take it off, or both
	 * banks answer there and the top 64 rows show the two mixed */
	vramSetBankH(VRAM_H_LCD);
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

/* --- the touch pad on the bottom screen ------------------------------------------------ */

#define PAD_TOP 16     /* rows 0-15: the bar (health, map or diagnostics, the tab) */
#define PAD_BOTTOM 144 /* rows 16-143: drag to aim; 144-191: the buttons */
#define TAB_X 200      /* the tab that swaps in the game's own screen */

typedef struct {
	uint8_t x, y, w, h;
	const char *label;
	uint16_t button; /* held while pressed */
	uint8_t impulse; /* sent once per press */
	bool toggle;     /* stays on until pressed again */
} padbtn_t;

static const padbtn_t padbtns[] = {
	{0, 144, 64, 24, "< WPN", 0, IMP_PREV, false},    {64, 144, 64, 24, "WPN >", 0, IMP_NEXT, false},
	{128, 144, 64, 24, "RELOAD", B_RELOAD, 0, false}, {192, 144, 64, 24, "USE", B_USE, 0, false},
	{0, 168, 64, 24, "JUMP", B_JUMP, 0, false},       {64, 168, 64, 24, "DUCK", B_CROUCH, 0, true},
	{128, 168, 64, 24, "LIGHT", 0, IMP_LIGHT, false}, {192, 168, 64, 24, "ALT", B_ATTACK2, 0, false},
};
#define NUM_PADBTNS ((int)(sizeof(padbtns) / sizeof(padbtns[0])))

#define C_BAR (RGB15(3, 4, 7) | BIT(15))
#define C_PAD (RGB15(2, 3, 5) | BIT(15))
#define C_GRID (RGB15(4, 6, 10) | BIT(15))
#define C_BTN (RGB15(6, 8, 13) | BIT(15))
#define C_BTNLIT (RGB15(14, 20, 31) | BIT(15))
#define C_EDGE (RGB15(1, 1, 2) | BIT(15))
#define C_TEXT (RGB15(26, 28, 31) | BIT(15))
#define C_DIM (RGB15(10, 12, 17) | BIT(15))
#define C_DOT (RGB15(31, 31, 20) | BIT(15))

static struct {
	bool gamepage;           /* showing the game's screen instead of the pad */
	bool touching, aiming;   /* the stylus is down; it went down in the aim area */
	int lastx, lasty;        /* where it was last frame */
	int dotx, doty;          /* the marker drawn under it, -1 for none */
	int pressed;             /* the button the stylus holds, -1 for none */
	bool ducked;             /* DUCK is on */
	int upframe, upx, upy;   /* when and where the last aim touch ended (double-tap) */
	int jumpframes;          /* JUMP held for a few more frames */
	uint8_t impulse, impseq; /* the last one-shot press, and its count */
	uint8_t screen[1300];    /* the game's last screen, to show when asked */
	int screenlen;
	int shownhealth;
	char diagline[24];
} pad;

static uint16_t pad_bg(int x, int y)
{
	return (x & 31) == 16 || ((y - PAD_TOP) & 31) == 16 ? C_GRID : C_PAD;
}

static void text_mid(int x, int y, int w, uint16_t c, const char *s)
{
	int n = (int)strlen(s);
	text(x + (w - n * 8) / 2, y, c, (const uint8_t *)s, n);
}

static void pad_button(int i, bool lit)
{
	const padbtn_t *b = &padbtns[i];
	fill(b->x, b->y, b->w, b->h, C_EDGE);
	fill(b->x + 1, b->y + 1, b->w - 2, b->h - 2, lit ? C_BTNLIT : C_BTN);
	text_mid(b->x, b->y + (b->h - 8) / 2, b->w, lit ? C_EDGE : C_TEXT, b->label);
}

/* the bar: health, then the map or the frame rate, then the tab */
static void pad_bar(void)
{
	char hp[12];
	const char *mid = link.loaderr[0] ? link.loaderr : cfg.diag && pad.diagline[0] ? pad.diagline : link.map;
	fill(0, 0, 256, PAD_TOP, C_BAR);
	snprintf(hp, sizeof(hp), "%d", link.health);
	text(4, 4, C_DIM, (const uint8_t *)"HP", 2);
	text(24, 4, link.health > 25 ? C_TEXT : (RGB15(31, 12, 8) | BIT(15)), (const uint8_t *)hp, (int)strlen(hp));
	text(56, 4, cfg.diag && !link.loaderr[0] ? (RGB15(20, 31, 20) | BIT(15)) : C_DIM, (const uint8_t *)mid,
	     BW_NDS_MIN((int)strlen(mid), (TAB_X - 60) / 8));
	fill(TAB_X, 1, 256 - TAB_X - 1, PAD_TOP - 2, C_BTN);
	text_mid(TAB_X, 4, 256 - TAB_X, C_TEXT, pad.gamepage ? "PAD" : "GAME");
	pad.shownhealth = link.health;
}

static void pad_draw(void)
{
	if (pad.gamepage) {
		if (pad.screenlen)
			draw_screen(pad.screen, pad.screenlen);
		else
			fill(0, 0, 256, 192, RGB15(0, 0, 0) | BIT(15));
		fill(TAB_X, 1, 256 - TAB_X - 1, PAD_TOP - 2, C_BTN);
		text_mid(TAB_X, 4, 256 - TAB_X, C_TEXT, "PAD");
		return;
	}
	for (int y = PAD_TOP; y < PAD_BOTTOM; y++) {
		uint16_t *row = sub + y * 256;
		for (int x = 0; x < 256; x++)
			row[x] = pad_bg(x, y);
	}
	text_mid(0, PAD_BOTTOM - 12, 256, C_DIM, "drag to aim, double-tap to jump");
	for (int i = 0; i < NUM_PADBTNS; i++)
		pad_button(i, (padbtns[i].toggle && pad.ducked) || i == pad.pressed);
	pad.dotx = pad.doty = -1;
	pad_bar();
}

/* the marker under the stylus while aiming */
static void pad_dot(int x, int y)
{
	if (pad.dotx >= 0)
		for (int j = 0; j < 4; j++)
			for (int i = 0; i < 4; i++)
				sub[(pad.doty + j) * 256 + pad.dotx + i] = pad_bg(pad.dotx + i, pad.doty + j);
	pad.dotx = pad.doty = -1;
	if (x < 0)
		return;
	x = BW_NDS_CLAMP(x - 2, 0, 252), y = BW_NDS_CLAMP(y - 2, PAD_TOP, PAD_BOTTOM - 4);
	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 4; i++)
			sub[(y + j) * 256 + x + i] = C_DOT;
	pad.dotx = x, pad.doty = y;
}

static void pad_impulse(int n)
{
	pad.impulse = (uint8_t)n;
	pad.impseq++;
}

static void pad_reset(void)
{
	memset(&pad, 0, sizeof(pad));
	pad.pressed = -1;
	pad.dotx = pad.doty = -1;
	pad.upframe = -100;
	pad.shownhealth = -99999;
}

/* the stylus, once a frame. Returns the buttons the pad holds; aiming turns
 * the camera directly. *totouch is set when the touch belongs to the game's
 * screen (sent on to the server as it is) */
static int pad_frame(int frame, uint32_t held, ndscam_t *cam, bool *togame, touchPosition *tp)
{
	bool down = (held & KEY_TOUCH) != 0, began = down && !pad.touching;
	int buttons = 0;
	*togame = false;
	if (down)
		touchRead(tp);
	if (began && tp->py < PAD_TOP && tp->px >= TAB_X) {
		pad.gamepage = !pad.gamepage;
		pad.aiming = false, pad.pressed = -2; /* the rest of this touch is the tab's */
		pad_draw();
	} else if (pad.gamepage) {
		*togame = down && !(pad.touching && !pad.aiming && pad.pressed == -2);
	} else if (began) {
		if (tp->py >= PAD_TOP && tp->py < PAD_BOTTOM) {
			pad.aiming = true;
			pad.lastx = tp->px, pad.lasty = tp->py;
			if (frame - pad.upframe < 18 && abs(tp->px - pad.upx) < 24 && abs(tp->py - pad.upy) < 24)
				pad.jumpframes = 8; /* double-tap */
			pad_dot(tp->px, tp->py);
		} else if (tp->py >= PAD_BOTTOM) {
			for (int i = 0; i < NUM_PADBTNS; i++) {
				const padbtn_t *b = &padbtns[i];
				if (tp->px >= b->x && tp->px < b->x + b->w && tp->py >= b->y && tp->py < b->y + b->h) {
					pad.pressed = i;
					if (b->impulse)
						pad_impulse(b->impulse);
					if (b->toggle)
						pad.ducked = !pad.ducked;
					pad_button(i, true);
				}
			}
		}
	} else if (down && pad.aiming) {
		int dx = tp->px - pad.lastx, dy = tp->py - pad.lasty;
		/* the touch screen jumps now and then: ignore a big leap */
		if (abs(dx) < 48 && abs(dy) < 48) {
			cam->yaw -= dx * cfg.sens * 7 / 16;
			cam->pitch -= dy * cfg.sens * 7 / 16;
			cam->pitch = BW_NDS_CLAMP(cam->pitch, -8000, 8000);
			cam->yaw = (int16_t)cam->yaw;
			pad.lastx = tp->px, pad.lasty = tp->py;
			pad_dot(tp->px, tp->py);
		}
	}
	if (!down && pad.touching) {
		if (pad.aiming) {
			pad.upframe = frame, pad.upx = pad.lastx, pad.upy = pad.lasty;
			pad_dot(-1, -1);
		}
		if (pad.pressed >= 0 && !pad.gamepage)
			pad_button(pad.pressed, padbtns[pad.pressed].toggle && pad.ducked);
		pad.aiming = false, pad.pressed = -1;
	}
	pad.touching = down;
	if (pad.pressed >= 0)
		buttons |= padbtns[pad.pressed].button & ~B_CROUCH;
	if (pad.ducked)
		buttons |= B_CROUCH;
	if (pad.jumpframes > 0)
		pad.jumpframes--, buttons |= B_JUMP;
	if (!pad.gamepage && link.health != pad.shownhealth)
		pad_bar();
	return buttons;
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
	/* different on every DS and every join: the time, how long since power-on and the name */
	link.session = (uint32_t)rand() ^ (uint32_t)time(NULL) << 8 ^ Wifi_GetIP() ^ (uint32_t)Nds_Vblanks() * 2654435761u;
	for (const char *n = cfg.name; *n; n++)
		link.session = link.session * 31 + (uint8_t)*n;
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

static void reset_studio(void)
{
	Ents_Free();
	for (int i = 0; i < MAX_STUDIO; i++) {
		free(studioname[i]);
		studioname[i] = NULL;
		studiomdl[i] = -2;
	}
	memset(anim, 0, sizeof(anim));
}

/* names for model indexes (docs/spec/link.md, MODELS) */
static void handle_models(const uint8_t *d, int len)
{
	int pos = 7, count;
	if (len < 7 || (uint16_t)rd16(d + 4) != link.spawncount)
		return;
	count = d[6];
	for (int i = 0; i < count && pos + 3 <= len; i++) {
		int idx = rd16(d + pos), n = d[pos + 2];
		pos += 3;
		if (pos + n > len)
			break;
		if (idx > 0 && idx < MAX_STUDIO && studioname[idx] && (strlen(studioname[idx]) != (size_t)n || strncasecmp(studioname[idx], (const char *)d + pos, (size_t)n))) {
			free(studioname[idx]); /* a player picked another model */
			studioname[idx] = NULL;
			studiomdl[idx] = -2;
		}
		if (idx > 0 && idx < MAX_STUDIO && !studioname[idx] && (studioname[idx] = malloc((size_t)n + 1))) {
			for (int k = 0; k < n; k++) /* the card's copies are in lower case */
				studioname[idx][k] = (char)(d[pos + k] >= 'A' && d[pos + k] <= 'Z' ? d[pos + k] + 32 : d[pos + k]);
			studioname[idx][n] = 0;
		}
		pos += n;
	}
}

static void handle_welcome(const uint8_t *d, int len, ndscam_t *cam)
{
	char map[65], path[300], err[96];
	uint16_t spawncount = (uint16_t)rd16(d + 5);
	copy_str(map, sizeof(map), d + 7, len - 7);
	link.spawncount = spawncount;
	if (World_Loaded() && !strcmp(map, link.map))
		return;
	snprintf(link.map, sizeof(link.map), "%s", map);
	reset_studio(); /* the models' textures go before VRAM is reset, not after */
	World_Free();
	Nds_ResetTextures();
	link.haveview = false;
	link.haveents = false;
	link.numthings = 0;
	memset(models, 0, sizeof(models));
	snprintf(path, sizeof(path), "maps/%s.bsp", map);
	{
		int start = Nds_Vblanks();
		link.loaderr[0] = 0;
		if (World_Load(path, palette, err, sizeof(err)))
			World_StartPosition(cam);
		else /* shown on the bottom line */
			snprintf(link.loaderr, sizeof(link.loaderr), "%s", err);
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
	{
		int size = pos < len ? d[pos++] : 10;
		if (size < 10)
			return;
		for (int i = 0; i < count && link.numthings < MAX_THINGS && pos + size <= len; i++, pos += size) {
			thing_t *t = &link.things[link.numthings++];
			for (int c = 0; c < 3; c++)
				t->org[c] = (int16_t)rd16(d + pos + c * 2);
			t->yaw = d[pos + 6], t->half = d[pos + 7], t->height = d[pos + 8], t->kind = d[pos + 9];
			t->ent = t->model = 0, t->seq = t->lift = 0;
			if (size >= 16) {
				t->ent = (uint16_t)rd16(d + pos + 10), t->model = (uint16_t)rd16(d + pos + 12);
				t->seq = d[pos + 14], t->lift = d[pos + 15];
			}
			t->anim = -1;
			if (t->ent && t->model) {
				/* a new sequence starts its clip again; speed from how far it went since the last packet (20 a second) */
				typeof(anim[0]) *a = &anim[t->anim = (int8_t)anim_slot(t->ent, linkframe)];
				a->seen = linkframe;
				int dx = t->org[0] - a->prev[0], dy = t->org[1] - a->prev[1];
				int sp = (int)sqrtf((float)(dx * dx + dy * dy)) * 20;
				if (a->seq != t->seq || !a->start)
					a->seq = t->seq, a->start = linkframe;
				a->speed = (int16_t)(sp > 2000 ? a->speed : (a->speed + sp) / 2);
				memcpy(a->prev, t->org, sizeof(a->prev));
			}
		}
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

/* the thing's model, once the server has named it and it loads (one new one a frame) */
static int thing_model(const thing_t *t, bool *loaded_one)
{
	int16_t *m;
	if (!t->model || t->model >= MAX_STUDIO || !studioname[t->model])
		return -1;
	m = &studiomdl[t->model];
	if (*m == -2 && !*loaded_one) {
		*loaded_one = true;
		*m = (int16_t)Ents_Model(studioname[t->model]);
		if (*m < 0 && t->model >= 1000)
			*m = (int16_t)Ents_Model("models/player.mdl"); /* a model the card doesn't have */
	}
	return *m;
}

/* the things with a model the DS has, into the model list Ents_Draw draws */
static void add_studio_things(void)
{
	bool loaded_one = false;
	Ents_Clear();
	for (int i = 0; i < link.numthings; i++) {
		const thing_t *t = &link.things[i];
		int mdl = thing_model(t, &loaded_one);
		if (mdl >= 0) {
			int16_t org[3] = {t->org[0], t->org[1], (int16_t)(t->org[2] + t->lift)};
			int seq = t->seq, start = 0, speed = 0, light = -1;
			if (t->anim >= 0) {
				typeof(anim[0]) *a = &anim[t->anim];
				start = a->start, speed = a->speed;
				/* look the light up again only after it moves a little */
				if (a->light >= 0 && abs(a->lightat[0] - org[0]) + abs(a->lightat[1] - org[1]) + abs(a->lightat[2] - org[2]) < 32)
					light = a->light;
			}
			if (light < 0) {
				light = Ents_AddLinked(mdl, org, t->yaw * 128, seq, linkframe - start, speed, -1);
				if (t->anim >= 0)
					anim[t->anim].light = (int8_t)light, memcpy(anim[t->anim].lightat, org, sizeof(org));
			} else
				Ents_AddLinked(mdl, org, t->yaw * 128, seq, linkframe - start, speed, light);
		}
	}
}

static bool has_model(const thing_t *t)
{
	return t->model && t->model < MAX_STUDIO && studiomdl[t->model] >= 0;
}

/* everything else the server sends: boxes, turned to face the way the thing does */
static void draw_things(void)
{
	static const uint16_t colours[] = {RGB15(20, 20, 20), RGB15(28, 12, 8), RGB15(28, 26, 8), RGB15(16, 18, 22), RGB15(12, 4, 4)};
	glBindTexture(GL_TEXTURE_2D, 0);
	for (int i = 0; i < link.numthings; i++) {
		const thing_t *t = &link.things[i];
		int16_t mins[3], maxs[3];
		int32_t h;
		if (has_model(t) || t->half > 128)
			continue; /* drawn as itself, or too big to be anything but in the way */
		h = t->half * 8;
		int32_t z = t->kind == 4 ? 4 * 8 : t->height * 8;
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
		bool covered = false;
		/* a player the DS draws with a model needs no marker */
		for (int k = 0; k < link.numthings && !covered; k++) {
			const thing_t *t = &link.things[k];
			covered = has_model(t) && abs(t->org[0] - m->org[0]) < 24 && abs(t->org[1] - m->org[1]) < 24;
		}
		if (covered)
			continue;
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

/* the buttons the keys hold, by scheme */
static int key_buttons(uint32_t held)
{
	int b = 0;
	if (cfg.lefty) { /* A B X Y move, R fires, L jumps */
		b |= held & KEY_R ? B_ATTACK : 0;
		b |= held & KEY_L ? B_JUMP : 0;
		return b;
	}
	b |= held & KEY_A ? B_ATTACK : 0;
	b |= held & KEY_B ? B_JUMP : 0;
	b |= held & KEY_X ? B_USE : 0;
	b |= held & KEY_Y ? B_RELOAD : 0;
	if (cfg.classic) /* L is the strafe and look key */
		b |= held & KEY_R ? B_ATTACK2 : 0;
	else /* L fires, R jumps, as the stylus hand has no thumb for A or B */
		b |= (held & KEY_L ? B_ATTACK : 0) | (held & KEY_R ? B_JUMP : 0);
	return b;
}

static void key_move(uint32_t held, int *fwd, int *side)
{
	uint32_t up = KEY_UP, dn = KEY_DOWN, lf = KEY_LEFT, rt = KEY_RIGHT;
	if (cfg.lefty)
		up = KEY_X, dn = KEY_B, lf = KEY_Y, rt = KEY_A;
	*fwd = held & up ? 127 : held & dn ? -127 : 0;
	*side = 0;
	if (!cfg.classic || held & KEY_L) /* classic: left and right turn unless L is held */
		*side = held & rt ? 127 : held & lf ? -127 : 0;
	if (cfg.classic && held & KEY_L)
		*fwd = 0; /* L + up / down looks */
}

static void send_input(const ndscam_t *cam, uint32_t held, int buttons, bool togame, const touchPosition *tp)
{
	pkt_t p;
	int fwd, side;
	key_move(held, &fwd, &side);
	p_begin(&p, L_INPUT);
	p_u32(&p, link.session);
	p_u16(&p, ++link.seq);
	p_u16(&p, (int)(held & (togame ? 0xffff : ~KEY_TOUCH & 0xffff)));
	p_u8(&p, togame ? 1 : 0);
	p_u8(&p, togame ? BW_NDS_MIN(tp->px, 255) : 0);
	p_u8(&p, togame ? BW_NDS_MIN(tp->py, 191) : 0);
	p_u8(&p, fwd), p_u8(&p, side), p_u8(&p, 0);
	p_u16(&p, cam->pitch * 2);
	p_u16(&p, cam->yaw * 2);
	p_u16(&p, link.screen_ack);
	p_u8(&p, link.fps), p_u8(&p, link.minfps), p_u16(&p, link.peakpolys);
	p_u16(&p, link.loadms / 10);
	p_u16(&p, link.spawncount); /* the map this device has */
	p_u16(&p, buttons);         /* what the game reads, mapped here */
	p_u8(&p, pad.impulse), p_u8(&p, pad.impseq);
	send_to(&link.to, &p);
}

/* classic: left and right turn, L + up and down look */
static void turn(ndscam_t *cam, uint32_t held)
{
	const int step = 32768 / 180;
	if (!cfg.classic)
		return;
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
	pad_reset();
	pad_draw();
	link.lastrecv = 0;
	for (int frame = 1;; frame++) {
		int len, type, buttons;
		uint32_t held;
		bool togame;
		touchPosition tp = {0};
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
		buttons = pad_frame(frame, held, cam, &togame, &tp) | key_buttons(held);
		if (frame % 3 == 0)
			send_input(cam, held, buttons, togame, &tp);

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
					pad.screenlen = BW_NDS_MIN(len - 6, (int)sizeof(pad.screen));
					memcpy(pad.screen, d + 6, (size_t)pad.screenlen);
					if (pad.gamepage)
						pad_draw();
					link.screen_ack = version;
				}
			} else if (type == L_VIEW)
				handle_view(d, len, cam);
			else if (type == L_ENTS)
				handle_ents(d, len);
			else if (type == L_MODELS)
				handle_models(d, len);
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
		linkframe = frame;
		if (World_Loaded()) {
			if (debugview == 0)
				add_studio_things();
			else
				Ents_Clear();
			Ents_Draw(cam, &st);
		} else
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
			worst = now - lastv > worst ? now - lastv : worst;
			lastv = now;
			frames++;
			if (now - secstart >= 60) {
				link.fps = frames * 60 / (now - secstart), link.minfps = 60 / (worst ? worst : 1);
				link.peakpolys = peak;
				int mok, mbad;
				Ents_Counts(&mok, &mbad);

				snprintf(pad.diagline, sizeof(pad.diagline), "%d/%d %dp m%d/%d%s", link.fps, link.minfps, peak, mok, mbad,
				         debugview == 1 ? " -T" : debugview == 2 ? " -TM" : "");
				if (!pad.gamepage && (cfg.diag || link.loaderr[0]))
					pad_bar();
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
	for (int i = 0; i < MAX_STUDIO; i++)
		studiomdl[i] = -2;
	iprintf("%s\n", play(&cam));
	close_socket();
	reset_studio();
	World_Free();
	Nds_ResetTextures();
	iprintf("A or START: back\n");
	wait_keys_or(1 << 30, KEY_A | KEY_START);
}
