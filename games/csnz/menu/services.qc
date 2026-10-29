/* Lobby services: server list, channel list, room list, users and chat.

   The original gets these from Nexon's servers. This build never contacts
   them. Two providers stand in, chosen by the cvar csnz_service:

   - "local" (default): real data from this computer only. One server and
     one channel for the listen server, no other rooms or players, and the
     room you create. Nothing here is invented.
   - "fixture": fixed sample data for testing layouts and states. Every
     name says FIXTURE, the screen shows a FIXTURE tag outside the 1024x768
     viewport, and joining a fixture room or server action is refused with
     a message. csnz_fixture_error servers|channels|rooms makes that request
     fail, to show the error state.

   With the SpacetimeDB backend set up (csnz_stdb 1, stdb_url and
   csnz_stdb_db, which run-csnz-stdb.sh sets from stdb.cfg), "local" also
   lists the database's lobby after this computer: its channel servers,
   their channels, and in a channel the rooms, which are game servers that
   registered there (csnz_stdb_address). Joining one connects to it. It is
   modelled on CSNZ_Server's server/channel/room layout and needs no token.

   Requests go through a loading state first (fixture: 0.6 s, local: one
   frame, the database: until it answers or csnz_service_timeout), so the
   loading state is the one the screens really show. */

#define SVC_READY 0
#define SVC_LOADING 1
#define SVC_ERROR 2

#define SVC_SERVERS 0
#define SVC_CHANNELS 1
#define SVC_ROOMS 2
#define SVC_USERS 3

var float autocvar_csnz_service_fixture_delay = 0.6f;

float g_svcState[4];
float g_svcReadyAt[4];
float g_svcServer;	/* the server picked on the server list */

/* chat lines, newest last */
string g_chatLines[64];
int g_chatCount;

bool
Svc_IsFixture(void)
{
	return (cvar_string("csnz_service") == "fixture");
}

string
Svc_Label(void)
{
	return Svc_IsFixture() ? "FIXTURE DATA" : "LOCAL DATA";
}

/* --- the SpacetimeDB lobby ---------------------------------------------- */

#define SVC_URI_BASE 300000
#define SVC_MAX_CHANNELS 64
#define SVC_MAX_ROOMS 128
var float autocvar_csnz_service_timeout = 6.0f;

/* list_channels lines: server id, server title, channel id, channel name,
   max players, players, rooms */
string g_dbChannels[SVC_MAX_CHANNELS];
int g_dbChannelCount;
/* list_rooms lines: the room fields (see Svc_RoomFields), then the address */
string g_dbRooms[SVC_MAX_ROOMS];
int g_dbRoomCount;
float g_dbReq[4];	/* the request each list waits for, 0 when none */
float g_dbSeq;

float g_svcServerId;	/* the database channel server picked, 0 for this computer */
float g_svcChannelId;	/* the database channel picked, 0 for this computer's */
bool g_svcFailed;	/* the last room list request failed */

bool
Svc_DbEnabled(void)
{
	return !Svc_IsFixture() && cvar("csnz_stdb") && cvar_string("stdb_url") != "" && cvar_string("csnz_stdb_db") != "";
}

/* the first JSON string in body, decoded ("[0,\"...\"]" is a procedure's Ok) */
string
Svc_JsonString(string body)
{
	float start = strstrofs(body, "\"");
	if (start < 0)
		return "";

	string out = "";
	float len = strlen(body);
	float from = start + 1;
	for (float i = start + 1; i < len; i++) {
		float c = str2chr(body, i);
		if (c == '"')
			return strcat(out, substring(body, from, i - from));
		if (c != '\\')
			continue;
		out = strcat(out, substring(body, from, i - from));
		i++;
		c = str2chr(body, i);
		if (c == 'n') out = strcat(out, "\n");
		else if (c == 't') out = strcat(out, "\t");
		else if (c == 'u') {
			out = strcat(out, "?");
			i += 4;
		} else if (c != 'r') out = strcat(out, chr2str(c));
		from = i + 1;
	}
	return strcat(out, substring(body, from, -1));
}

/* asks a lobby procedure; the answer lands in URI_Get_Callback */
bool
Svc_DbAsk(float what, string procedure, string args)
{
	string base = cvar_string("stdb_url");
	if (substring(base, strlen(base) - 1, 1) == "/")
		base = substring(base, 0, strlen(base) - 1);

	g_dbSeq = (g_dbSeq + 1) % 1000;
	g_dbReq[what] = SVC_URI_BASE + what * 1000 + g_dbSeq;
	if (uri_post(sprintf("%s/v1/database/%s/call/%s", base, cvar_string("csnz_stdb_db"), procedure), g_dbReq[what], "application/json", args))
		return (true);

	g_dbReq[what] = 0;
	print(sprintf("csnz: couldn't ask the lobby database (%s)\n", procedure));
	return (false);
}

/* splits a procedure's lines into list (strzoned), returns the count */
int
Svc_DbLines(string text, string list[], int most)
{
	int count = 0i;
	float n = tokenizebyseparator(text, "\n");
	for (float l = 0; l < n && count < most; l++) {
		string line = argv(l);
		if (strstrofs(line, "\t") > 0)
			list[count++] = strzone(line);
	}
	return count;
}

void
URI_Get_Callback(float id, float code, string body, int len)
{
	float what = -1;
	for (float w = 0; w < 4; w++)
		if (id && g_dbReq[w] == id)
			what = w;
	if (what < 0)
		return;	/* an answer we stopped waiting for */
	g_dbReq[what] = 0;

	bool ok = (code == 200 && substring(strreplace(" ", "", body), 0, 3) == "[0,");
	if (!ok)
		print(sprintf("csnz: lobby database answered %d %s\n", code, substring(body, 0, 200)));

	string text = ok ? Svc_JsonString(body) : "";
	if (what == SVC_ROOMS) {
		for (int i = 0i; i < g_dbRoomCount; i++)
			strunzone(g_dbRooms[i]);
		g_dbRoomCount = Svc_DbLines(text, g_dbRooms, SVC_MAX_ROOMS);
	} else {
		for (int i = 0i; i < g_dbChannelCount; i++)
			strunzone(g_dbChannels[i]);
		g_dbChannelCount = Svc_DbLines(text, g_dbChannels, SVC_MAX_CHANNELS);
	}
	if (cvar("csnz_stdb_debug"))
		print(sprintf("csnz: lobby: %i channels, %i rooms\n", g_dbChannelCount, g_dbRoomCount));

	/* only the room list shows a failure: the others still have this computer */
	if (g_svcState[what] == SVC_LOADING)
		g_svcReadyAt[what] = time;
	if (what == SVC_ROOMS && !ok)
		g_svcFailed = true;
}


/* field f of a list_channels line */
string
Svc_DbChannelField(int i, float f)
{
	tokenizebyseparator(g_dbChannels[i], "\t");
	return argv(f);
}

/* the database channel servers, in list order: the n-th distinct server id */
float
Svc_DbServerAt(int n)
{
	float last = -1;
	int seen = -1i;
	for (int i = 0i; i < g_dbChannelCount; i++) {
		float id = stof(Svc_DbChannelField(i, 0));
		if (id == last)
			continue;
		last = id;
		if (++seen == n)
			return id;
	}
	return 0;
}

int
Svc_DbServerCount(void)
{
	int count = 0i;
	while (Svc_DbServerAt(count))
		count++;
	return count;
}

/* the channels of the picked server: the n-th one's line, or -1 */
int
Svc_DbChannelAt(int n)
{
	int seen = -1i;
	for (int i = 0i; i < g_dbChannelCount; i++)
		if (stof(Svc_DbChannelField(i, 0)) == g_svcServerId && ++seen == n)
			return i;
	return -1i;
}

void
Svc_Request(float what)
{
	g_svcState[what] = SVC_LOADING;
	g_svcReadyAt[what] = time + (Svc_IsFixture() ? autocvar_csnz_service_fixture_delay : 0.0f);

	if (!Svc_DbEnabled())
		return;

	bool asked = false;
	if (what == SVC_SERVERS || (what == SVC_CHANNELS && g_svcServerId))
		asked = Svc_DbAsk(what, "list_channels", "[]");
	else if (what == SVC_ROOMS && g_svcChannelId) {
		g_svcFailed = false;
		asked = Svc_DbAsk(what, "list_rooms", sprintf("[%d]", g_svcChannelId));
	}
	if (asked)
		g_svcReadyAt[what] = time + autocvar_csnz_service_timeout;
	else if (what == SVC_ROOMS && g_svcChannelId)
		g_svcFailed = true;
}

/* call every frame; returns true when a request finished this frame */
bool
Svc_Poll(float what)
{
	if (g_svcState[what] != SVC_LOADING || time < g_svcReadyAt[what])
		return (false);

	string fail = cvar_string("csnz_fixture_error");
	string names[4] = {"servers", "channels", "rooms", "users"};

	if (Svc_IsFixture() && fail == names[what])
		g_svcState[what] = SVC_ERROR;
	else if (what == SVC_ROOMS && g_svcChannelId && (g_svcFailed || g_dbReq[what]))
		g_svcState[what] = SVC_ERROR;	/* no rooms from the database */
	else
		g_svcState[what] = SVC_READY;
	g_dbReq[what] = 0;

	return (true);
}

float
Svc_State(float what)
{
	return g_svcState[what];
}

/* --- servers ------------------------------------------------------------ */

int
Svc_ServerCount(void)
{
	return Svc_IsFixture() ? 4i : 1i + Svc_DbServerCount();
}

/* the database channel server behind row i, 0 for this computer */
float
Svc_ServerId(int i)
{
	return (Svc_IsFixture() || i < 1i) ? 0 : Svc_DbServerAt(i - 1i);
}

bool
Svc_ServerOnline(int i)
{
	if (Svc_IsFixture())
		return (i != 3i);

	return (true);
}

/* "name<TAB>status" */
string
Svc_Server(int i)
{
	string status = VGUI_Localize(Svc_ServerOnline(i) ? "#CSO_Server_Ready_Status" : "#CSO_Server_NotReady_Status");

	if (Svc_IsFixture())
		return sprintf("FIXTURE Server %i\t%s", i + 1i, status);

	float id = Svc_ServerId(i);
	if (id) {
		for (int c = 0i; c < g_dbChannelCount; c++)
			if (stof(Svc_DbChannelField(c, 0)) == id)
				return sprintf("%s\t%s", Svc_DbChannelField(c, 1), status);
	}

	return sprintf("This computer\t%s", status);
}

/* --- channels ----------------------------------------------------------- */

int
Svc_ChannelCount(void)
{
	if (Svc_IsFixture())
		return 6i;
	if (!g_svcServerId)
		return 1i;

	int count = 0i;
	while (Svc_DbChannelAt(count) >= 0i)
		count++;
	return count;
}

/* the database channel behind row i, 0 for this computer's */
float
Svc_ChannelId(int i)
{
	int line = (Svc_IsFixture() || !g_svcServerId) ? -1i : Svc_DbChannelAt(i);
	return line < 0i ? 0 : stof(Svc_DbChannelField(line, 2));
}

/* 0 open, 1 moderate, 2 busy, 3 full (CSO_Channel_Status0-3, OBSERVED strings) */
int
Svc_ChannelLoad(int i)
{
	if (Svc_IsFixture())
		return (i % 4i);

	int line = g_svcServerId ? Svc_DbChannelAt(i) : -1i;
	if (line >= 0i) {
		/* DESIGN: quarters of the channel's player limit */
		float max = stof(Svc_DbChannelField(line, 4));
		float players = stof(Svc_DbChannelField(line, 5));
		if (max > 0)
			return (int)bound(0, floor(players * 4 / max), 3);
	}

	return 0i;
}

string
Svc_Channel(int i)
{
	string status = VGUI_Localize(sprintf("#CSO_Channel_Status%i", Svc_ChannelLoad(i)));

	if (Svc_IsFixture())
		return sprintf("FIXTURE Channel %i\t%s", i + 1i, status);

	int line = g_svcServerId ? Svc_DbChannelAt(i) : -1i;
	if (line >= 0i)
		return sprintf("%s\t%s", Svc_DbChannelField(line, 3), status);

	return sprintf("Local\t%s", status);
}

/* --- rooms -------------------------------------------------------------- */

/* fields: number, title, mode id, map file, players, max players, password (0/1), playing (0/1) */
int
Svc_RoomCount(void)
{
	if (Svc_IsFixture())
		return 12i;

	return g_svcChannelId ? g_dbRoomCount : 0i;
}

/* "host:port" of room i, "" when it isn't a database room */
string
Svc_RoomAddress(int i)
{
	if (Svc_IsFixture() || !g_svcChannelId || i < 0i || i >= g_dbRoomCount)
		return "";

	tokenizebyseparator(g_dbRooms[i], "\t");
	return argv(8);
}

string
Svc_RoomFields(int i)
{
	if (!Svc_IsFixture())
		return (g_svcChannelId && i >= 0i && i < g_dbRoomCount) ? g_dbRooms[i] : "";

	/* fixture rooms use the client's own default titles, modes and maps */
	string modes[4] = {"8", "2", "3", "9"};
	string maps[4] = {"zm_origin", "de_dust2", "dm_tunnel", "zm_abyss"};
	string title = VGUI_Localize(sprintf("#CSO_Room_DefaultTitle%02i", i % 6i));

	string head = sprintf("%i\tFIXTURE %s\t%s\t%s", i + 1i, title, modes[i % 4i], maps[i % 4i]);
	return sprintf("%s\t%i\t%i\t%i\t%i", head, 1i + (i * 5i) % 16i, 16i, (i % 5i == 4i) ? 1i : 0i, (i % 3i == 1i) ? 1i : 0i);
}

/* --- users -------------------------------------------------------------- */

int
Svc_UserCount(void)
{
	return Svc_IsFixture() ? 9i : 1i;
}

/* "name<TAB>level" ; level "-" where there is no account to read it from */
string
Svc_User(int i)
{
	if (i == 0i)
		return sprintf("%s\t-", cvar_string("name"));

	return sprintf("FIXTURE Player %i\t%i", i, 1i + (i * 7i) % 70i);
}

/* --- chat --------------------------------------------------------------- */

void
Svc_ChatAdd(string line)
{
	if (g_chatCount >= 64i) {
		strunzone(g_chatLines[0]);
		for (int i = 1i; i < 64i; i++)
			g_chatLines[i - 1i] = g_chatLines[i];
		g_chatCount = 63i;
	}

	g_chatLines[g_chatCount++] = strzone(line);
}

/* Sends a chat line. Local: nobody else is here, so it is only shown to
   you, and says so once. */
void
Svc_ChatSend(string text)
{
	if (text == "")
		return;

	Svc_ChatAdd(sprintf("%s : %s", cvar_string("name"), text));
}
