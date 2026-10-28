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

   Requests go through a loading state first (fixture: 0.6 s, local: one
   frame), so the loading state is the one the screens really show. */

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

void
Svc_Request(float what)
{
	g_svcState[what] = SVC_LOADING;
	g_svcReadyAt[what] = time + (Svc_IsFixture() ? autocvar_csnz_service_fixture_delay : 0.0f);
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
	else
		g_svcState[what] = SVC_READY;

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
	return Svc_IsFixture() ? 4i : 1i;
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

	return sprintf("This computer\t%s", status);
}

/* --- channels ----------------------------------------------------------- */

int
Svc_ChannelCount(void)
{
	return Svc_IsFixture() ? 6i : 1i;
}

/* 0 open, 1 moderate, 2 busy, 3 full (CSO_Channel_Status0-3, OBSERVED strings) */
int
Svc_ChannelLoad(int i)
{
	if (Svc_IsFixture())
		return (i % 4i);

	return 0i;
}

string
Svc_Channel(int i)
{
	string status = VGUI_Localize(sprintf("#CSO_Channel_Status%i", Svc_ChannelLoad(i)));

	if (Svc_IsFixture())
		return sprintf("FIXTURE Channel %i\t%s", i + 1i, status);

	return sprintf("Local\t%s", status);
}

/* --- rooms -------------------------------------------------------------- */

/* fields: number, title, mode id, map file, players, max players, password (0/1), playing (0/1) */
int
Svc_RoomCount(void)
{
	return Svc_IsFixture() ? 12i : 0i;
}

string
Svc_RoomFields(int i)
{
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
