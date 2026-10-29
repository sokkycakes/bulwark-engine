/* The lobby screens and the navigation between them.

   Flow (navigation-map.md): Server List -> Channel List -> Lobby (room
   list) -> Game Room -> match. The taskbar's back button goes up one level.
   Panel positions come from the layout files (OBSERVED); where a current
   layout is missing from the NARs an older one stands in and is marked.

   Every control either works, says why it can't (MsgBox_Unavailable), or
   is a labelled FIXTURE. */

#define SCR_NONE 0
#define SCR_SERVERS 1
#define SCR_CHANNELS 2
#define SCR_LOBBY 3
#define SCR_ROOM 4

float g_screen;
string g_panelFile;	/* the layout being built, for the custom control hook */
vguiSchemeTheme g_scheme;

vguiWindow g_winServers;
vguiWindow g_winChannels;
vguiWindow g_winPromoLeft;
vguiWindow g_winPromoBottom;
vguiWindow g_winPromoRoom;
vguiWindow g_winClanFriends;
vguiWindow g_winRooms;
vguiWindow g_winRoomInfo;
vguiWindow g_winUsers;
vguiWindow g_winLobbyChat;
vguiWindow g_winTaskbar;
vguiWindow g_winCreate;
vguiWindow g_winRoomPlayers;
vguiWindow g_winRoomOpts;
vguiWindow g_winRoomChat;
vguiWindow g_winExit;
vguiWindow g_winInGame;

vguiResList g_lstServers;
vguiResList g_lstChannels;
vguiResList g_lstRooms;
vguiResList g_lstUsers;
vguiResList g_lstLobbyChat;
vguiResList g_lstRoomChat;
vguiResList g_lstCT;
vguiResList g_lstTR;
vguiLabel g_lblMyInfo;

/* the local game room */
bool g_roomActive;
string g_roomTitle;
float g_roomTeam;	/* 0 CT, 1 TR: which list you are in */
bool g_filtersShown;
int g_chatShown;

void Options_Open(void);
bool Options_Command(string);
void Room_StartGame(void);
bool MapSel_Command(string);
vguiWidget MapSel_Control(vguiResLayout, string);
vguiWindow g_winMapSel;
void Nav_Go(float);

/* --- lists ---------------------------------------------------------------- */

void
List_Setup(vguiResList list, string columns)
{
	float alpha;

	if (!list)
		return;

	list.SetArt("resource/control/listpanel/window_default", "resource/control/listpanel/column",
		"resource/control/selected/selected", "resource/control/selected/over",
		"resource/control/scrollbar/btn_default_scroll_bar");

	if (g_scheme)
		list.SetColors(g_scheme.SchemeColor("FgColor", alpha), g_scheme.SchemeColor("FgColor", alpha), [1, 0.45, 0.35]);

	/* "Title:width;Title:width" with widths in layout units */
	int n = (int)tokenizebyseparator(columns, ";");
	for (int i = 0i; i < n; i++) {
		string col = argv(i);
		float colon = strstrofs(col, ":");
		list.AddColumn(VGUI_Localize(substring(col, 0, colon)), stof(substring(col, colon + 1, -1)) * g_vpScale);
		tokenizebyseparator(columns, ";");
	}
}

static string
Text_Unknown(string fmt)
{
	/* format strings that need account data we don't have show "-" */
	fmt = strreplace("%u", "-", fmt);
	fmt = strreplace("%d", "-", fmt);
	return strreplace("%s", "-", fmt);
}

/* --- service-backed lists -------------------------------------------------- */

static void
Servers_Fill(void)
{
	g_lstServers.ClearRows();

	switch (Svc_State(SVC_SERVERS)) {
	case SVC_LOADING:
		g_lstServers.SetState(VGUI_LIST_LOADING, "Getting the server list...");
		return;
	case SVC_ERROR:
		g_lstServers.SetState(VGUI_LIST_ERROR, "Couldn't get the server list (FIXTURE error). Press Refresh to try again.");
		return;
	}

	g_lstServers.SetState(VGUI_LIST_READY, "");
	for (int i = 0i; i < Svc_ServerCount(); i++)
		g_lstServers.AddRow(Svc_Server(i), ftos(i));
}

static void
Channels_Fill(void)
{
	g_lstChannels.ClearRows();

	switch (Svc_State(SVC_CHANNELS)) {
	case SVC_LOADING:
		g_lstChannels.SetState(VGUI_LIST_LOADING, "Getting the channel list...");
		return;
	case SVC_ERROR:
		g_lstChannels.SetState(VGUI_LIST_ERROR, "Couldn't get the channel list (FIXTURE error). Press Refresh to try again.");
		return;
	}

	g_lstChannels.SetState(VGUI_LIST_READY, "");
	for (int i = 0i; i < Svc_ChannelCount(); i++)
		g_lstChannels.AddRow(Svc_Channel(i), ftos(i));
}

static bool
Rooms_Passes(void)
{
	/* fields are tokenized: 0 no, 1 title, 2 mode, 3 map, 4 players, 5 max, 6 password, 7 playing */
	string mode = RoomOpts_Value(g_winRooms, "FilterGameMode");
	string map = RoomOpts_Value(g_winRooms, "FilterMap");
	vguiTextBox find = (vguiTextBox)Panel_Find(g_winRooms, "FindKeyword");
	vguiCheckbox slot = (vguiCheckbox)Panel_Find(g_winRooms, "FilterSlot");
	vguiCheckbox pass = (vguiCheckbox)Panel_Find(g_winRooms, "FilterPassword");

	if (mode != "" && mode != "all" && argv(2) != mode)
		return (false);
	if (map != "" && map != "all" && argv(3) != map)
		return (false);
	/* (QC's && evaluates both sides: no method calls on a null widget) */
	if (find) {
		string key = find.GetText();
		if (key != "" && strstrofs(strtolower(argv(1)), strtolower(key)) < 0)
			return (false);
	}
	if (slot)
		if (slot.GetValue() && stof(argv(4)) >= stof(argv(5)))
			return (false);
	if (pass)
		if (pass.GetValue() && stof(argv(6)) != 0)
			return (false);

	return (true);
}

static void
Rooms_Fill(void)
{
	g_lstRooms.ClearRows();

	switch (Svc_State(SVC_ROOMS)) {
	case SVC_LOADING:
		g_lstRooms.SetState(VGUI_LIST_LOADING, "Getting the room list...");
		return;
	case SVC_ERROR:
		g_lstRooms.SetState(VGUI_LIST_ERROR, "Couldn't get the room list (FIXTURE error).");
		return;
	}

	g_lstRooms.SetState(VGUI_LIST_READY, "");
	for (int i = 0i; i < Svc_RoomCount(); i++) {
		string fields = Svc_RoomFields(i);
		tokenizebyseparator(fields, "\t");

		if (!Rooms_Passes())
			continue;

		tokenizebyseparator(fields, "\t");
		string no = strzone(argv(0));
		string title = strzone(argv(1));
		string mode = strzone(argv(2));
		string map = strzone(argv(3));
		string count = sprintf("%s/%s", argv(4), argv(5));
		string state = VGUI_Localize(stof(argv(7)) ? "#CSO_Room_State_Playing" : "#CSO_Room_State_Waiting");
		string lock = stof(argv(6)) ? "*" : "";

		g_lstRooms.AddRow(sprintf("%s\t%s%s\t%s\t%s\t%s\t%s", no, lock, title, Tables_ModeTitle(mode), Tables_MapTitle(map), count, state), ftos(i));
		strunzone(no);
		strunzone(title);
		strunzone(mode);
		strunzone(map);
	}
}

static void
Users_Fill(void)
{
	g_lstUsers.ClearRows();
	g_lstUsers.SetState(VGUI_LIST_READY, "");

	for (int i = 0i; i < Svc_UserCount(); i++)
		g_lstUsers.AddRow(Svc_User(i), ftos(i));
}

static void
Chat_Fill(vguiResList list)
{
	if (!list)
		return;

	list.ClearRows();
	for (int i = 0i; i < g_chatCount; i++)
		list.AddRow(g_chatLines[i], "");

	/* keep the newest line in view */
	if (g_chatCount > 0i)
		list.SetSelected(-1i);
}

/* the lobby's room info panel shows the selected room */
static void
Rooms_Selected(entity listEnt, int row)
{
	vguiResList list = (vguiResList)listEnt;
	string fields = Svc_RoomFields(stof(list.RowKey(row)));
	vguiTextBox name = (vguiTextBox)Panel_Find(g_winRoomInfo, "RoomName");
	vguiTextBox mode = (vguiTextBox)Panel_Find(g_winRoomInfo, "GameMode");
	vguiTextBox map = (vguiTextBox)Panel_Find(g_winRoomInfo, "MapName");

	tokenizebyseparator(fields, "\t");
	string t = strzone(argv(1));
	string m = strzone(argv(2));
	string f = strzone(argv(3));

	if (name)
		name.SetText(t);
	if (mode)
		mode.SetText(Tables_ModeTitle(m));
	if (map)
		map.SetText(Tables_MapTitle(f));

	strunzone(t);
	strunzone(m);
	strunzone(f);
}

static void
Rooms_Activated(entity listEnt, int row)
{
	Nav_Command(__NULL__, "EnterRoom");
}

static void
Servers_Activated(entity listEnt, int row)
{
	int i = (int)stof(((vguiResList)listEnt).RowKey(row));

	if (!Svc_ServerOnline(i)) {
		MsgBox_Show("#CSO_ServerMessage_Title", "This server is offline (FIXTURE data).");
		return;
	}

	g_svcServer = i;
	Nav_Go(SCR_CHANNELS);
}

static void
Channels_Activated(entity listEnt, int row)
{
	int i = (int)stof(((vguiResList)listEnt).RowKey(row));

	if (Svc_ChannelLoad(i) == 3i) {
		MsgBox_Show("#CSO_ServerMessage_Title", "This channel is full (FIXTURE data).");
		return;
	}

	Nav_Go(SCR_LOBBY);
}

/* --- chat ----------------------------------------------------------------- */

static void
Chat_Enter(void)
{
	vguiTextBox boxes[2];
	boxes[0] = (vguiTextBox)Panel_Find(g_winLobbyChat, "IMETextEntry1");
	boxes[1] = (vguiTextBox)Panel_Find(g_winRoomChat, "ChatInput");

	for (int i = 0i; i < 2i; i++) {
		vguiTextBox box = boxes[i];

		if (!box)
			continue;
		if (!box.HasFlag(TEXTBOX_FOCUS))
			continue;

		if (!Svc_IsFixture() && g_chatShown == 0i) {
			Svc_ChatAdd("[Local] Nobody else is connected; chat is shown only to you.");
			g_chatShown = 1i;
		}

		Svc_ChatSend(box.GetText());
		box.SetText("");
	}

	Chat_Fill(g_lstLobbyChat);
	Chat_Fill(g_lstRoomChat);
}

/* --- custom controls --------------------------------------------------------- */

/* taskbar art per button: resource/taskbar/btn_<name>@n/@o/@c.tga. The art
   carries its own English label (OBSERVED), which is how buttons are
   matched to images (INFERRED). */
static string
Taskbar_Art(string panel)
{
	switch (strtolower(panel)) {
	case "serverbutton": return "server";
	case "channelbutton": return "channel";
	case "rommlistbutton": return "roomlist";
	case "gameroombutton": return "room";
	case "gamematchbutton": return "room";
	case "tutorialbutton": return "tutorial";
	case "bulletinbutton": return "bulletin";
	case "shopbutton": return "store";
	case "inventorybutton": return "myinfo";
	case "itemfarmbutton": return "weaponmaking";
	case "questbutton": return "quest";
	case "newquestbutton": return "quest_new";
	case "clanbutton": return "clan";
	case "statisticbutton": return "statistic";
	case "optionbutton": return "option";
	case "exitbutton": return "exit";
	case "pcbangbutton": return "pcbangregistration";
	case "pcbanginfobutton": return "pcbanginfo";
	case "pcbanginfohomebutton": return "home";
	}

	return "";
}

vguiWidget
Screens_Control(vguiResLayout res, string path)
{
	string ctl = strtolower(res.GetString(strcat(path, "/ControlName")));
	string name = substring(path, strstrofs(path, "/") + 1, -1);

	if (g_panelFile == "resource/voxel/popup_voxeloutui_mapselectdlg.res")
		return MapSel_Control(res, path);

	/* web pages (CEF); the original loads them from Nexon */
	if (ctl == "html") {
		vguiLabel web = spawn(vguiLabel);
		web.SetTitle("Promotion page: web content from the official service, not loaded in this build.");
		web.SetAlignment(AF_LEFT | AF_RIGHT | AF_TOP | AF_BOTTOM);
		return web;
	}

	if (ctl == "winorkilllimitlabel") {
		vguiLabel lbl = spawn(vguiLabel);
		lbl.SetTitle(VGUI_Localize(res.GetString(strcat(path, "/labelText"))));
		return lbl;
	}

	if (g_panelFile == "resource/res/taskbarver7.res") {
		if (strtolower(name) == "myinfolabel") {
			g_lblMyInfo = spawn(vguiLabel);
			return g_lblMyInfo;
		}

		string art = Taskbar_Art(name);
		if (art == "")
			return __NULL__;

		vguiResButton btn = spawn(vguiResButton);
		string base = strcat("resource/taskbar/btn_", art);
		btn.SetImages(strcat(base, "@n"), strcat(base, "@o"), strcat(base, "@c"), "");
		btn.SetTextHidden(true);
		btn.m_resCommand = strzone(res.GetString(strcat(path, "/command")));
		return btn;
	}

	/* the room chat's input has no ControlName in its layout */
	if (g_panelFile == "resource/res/room_chattingmanagerdlgver2.res" && strtolower(name) == "chatinput") {
		vguiTextBox box = spawn(vguiTextBox);
		box.CallOnEnter(Chat_Enter);
		return box;
	}

	return __NULL__;
}

/* --- building ---------------------------------------------------------------- */

vguiWindow
Screens_Load(string file, string root)
{
	g_panelFile = file;
	vguiWindow win = Panel_Load(file, root);
	g_panelFile = "";
	return win;
}

static vguiResList
Screens_List(vguiWindow win, string name, string columns)
{
	vguiResList list = (vguiResList)Panel_Find(win, name);
	List_Setup(list, columns);
	return list;
}

/* The quick/custom entry buttons: their labels (CustomStartLabel,
   QuickStartLabel, ModeLabel) sit inside the buttons (OBSERVED layout), so
   the buttons' own text is not drawn and ModeLabel names the quick entry
   mode (INFERRED). */
static void
Screens_StartButtons(vguiWindow win)
{
	vguiButton b;
	vguiLabel l;

	if (!win)
		return;

	b = (vguiButton)Panel_Find(win, "CustomStartBtn");
	if (b)
		b.SetTitle("");
	l = (vguiLabel)Panel_Find(win, "ModeLabel");
	if (l)
		l.SetTitle(Tables_ModeTitle(cvar_string("csnz_mode")));
}

void
Screens_Init(void)
{
	/* Server List and Channel List screens: promotions left and bottom,
	   clan/friends bottom left */
	g_winPromoLeft = Screens_Load("resource/res/promotionleft.res", "PromotionLeftDlg");
	g_winPromoBottom = Screens_Load("resource/res/promotionbottom.res", "PromotionBottomDlg");
	g_winClanFriends = Screens_Load("resource/res/serverlist_clanfriendlist_ver2.res", "MainClanFriendListDlg");

	g_winServers = Screens_Load("resource/res/serverlist_serverlistver2.res", "MainServerListDlg");
	g_lstServers = Screens_List(g_winServers, "ListPanel1", "#CSO_Server_Lable:460;#CSO_ServerStatus_Lable:236");
	g_lstServers.CallOnActivate(Servers_Activated);

	g_winChannels = Screens_Load("resource/res/channellist_channellistver2.res", "MainChannelListDlg");
	g_lstChannels = Screens_List(g_winChannels, "ListPanel1", "#CSO_Channel_Lable:460;#CSO_ServerStatus_Lable:236");
	g_lstChannels.CallOnActivate(Channels_Activated);
	Screens_StartButtons(g_winServers);
	Screens_StartButtons(g_winChannels);

	/* Lobby: room list with filters, room info, users, chat, promotion */
	g_winRoomInfo = Screens_Load("resource/res/roomlist_roominfo_ver2.res", "GameRoomInfoDlg");
	g_winUsers = Screens_Load("resource/res/roomlist_userlist_ver2.res", "LobbyUserListDlg");
	g_winPromoRoom = Screens_Load("resource/res/promotionroomlist.res", "PromotionRoomListDlg");
	g_winRooms = Screens_Load("resource/res/roomlist_roomlist_filterchangever2.res", "MainRoomListDlg");
	Screens_StartButtons(g_winRooms);
	g_lstRooms = Screens_List(g_winRooms, "ListPanel1",
		" :40;#CSO_RoomName_Lable:230;#CSO_GameMode_Lable:130;#CSO_Map_Lable:110;#CSO_Status_Lable:96;#CSO_State_Label:90");
	g_lstRooms.SetEmptyText("There are no rooms. Use Create Room to make one.");
	g_lstRooms.CallOnSelect(Rooms_Selected);
	g_lstRooms.CallOnActivate(Rooms_Activated);
	g_filtersShown = true;

	/* filter choices: modes and maps the client knows */
	vguiComboBox fmode = (vguiComboBox)Panel_Find(g_winRooms, "FilterGameMode");
	if (fmode) {
		fmode.AddItem("All", "all");
		for (int m = 0i; m < 60i; m++)
			if (Tables_ModeRow(ftos(m)))
				fmode.AddItem(VGUI_Localize(argv(1)), ftos(m));
		fmode.SetSelected(0i);
	}
	vguiComboBox fmap = (vguiComboBox)Panel_Find(g_winRooms, "FilterMap");
	if (fmap) {
		Tables_FillMaps(fmap, "");
		fmap.AddItem("All", "all");
		fmap.SelectValue("all");
	}
	/* ping needs a real connection and level needs an account: unavailable */
	vguiWidget fping = Panel_Find(g_winRooms, "FilterPing");
	if (fping)
		fping.Disable();
	vguiWidget flevel = Panel_Find(g_winRooms, "FilterLevel");
	if (flevel)
		flevel.Disable();

	/* the room info fields are read-only displays */
	string infoFields[3] = {"RoomName", "GameMode", "MapName"};
	for (int f = 0i; f < 3i; f++) {
		vguiWidget field = Panel_Find(g_winRoomInfo, infoFields[f]);
		if (field)
			field.Disable();
	}

	g_lstUsers = spawn(vguiResList);
	Panel_AddAt(g_winUsers, g_lstUsers, [9, 30], [264, 124]);
	List_Setup(g_lstUsers, "#CSO_NickName_Lable:180;#CSO_Level_Lable:80");

	/* Lobby chat. DESIGN placement: the current layout
	   (Modaless_ChattingManagerDlg.res) is missing; roomlist_chat.res is an
	   older one whose own spot overlaps the user list. It goes in the free
	   space between the room list and the room promotion. */
	g_winLobbyChat = Screens_Load("resource/roomlist_chat.res", "LobbyChatDlg");
	if (g_winLobbyChat) {
		g_winLobbyChat.csnzResPos = [300, 424];
		g_winLobbyChat.SetSize([498, 251] * g_vpScale);
		Panel_Place(g_winLobbyChat);
		g_lstLobbyChat = (vguiResList)Panel_Find(g_winLobbyChat, "ListPanel1");
		List_Setup(g_lstLobbyChat, "");
		g_lstLobbyChat.SetPos([9, 12] * g_vpScale);
		g_lstLobbyChat.SetSize([480, 200] * g_vpScale);
		vguiTextBox lobbyInput = (vguiTextBox)Panel_Find(g_winLobbyChat, "IMETextEntry1");
		lobbyInput.SetPos([9, 218] * g_vpScale);
		lobbyInput.SetSize([480, 24] * g_vpScale);
		lobbyInput.CallOnEnter(Chat_Enter);
	}

	/* Game room: players by team, room options, chat */
	g_winRoomPlayers = Screens_Load("resource/res/room_roomlist_list.res", "GameRoomPlayerDlg");
	g_lstCT = Screens_List(g_winRoomPlayers, "CTListPanel", "#CSO_Level_Lable:60;#CSO_NickName_Lable:288");
	g_lstTR = Screens_List(g_winRoomPlayers, "TRListPanel", "#CSO_Level_Lable:60;#CSO_NickName_Lable:288");
	vguiLabel title = (vguiLabel)Panel_Find(g_winRoomPlayers, "PlayerListTitle");
	if (title)
		title.SetTitle(VGUI_Localize("#CSO_PlayerList_Label"));
	/* the host starts; Ready is for the others (INFERRED from the shared spot) */
	Panel_Show(Panel_Find(g_winRoomPlayers, "ReadyButton"), false);
	Panel_Show(Panel_Find(g_winRoomPlayers, "InviteClanButton"), false);
	Panel_Show(Panel_Find(g_winRoomPlayers, "InviteClanMemberButton"), false);

	/* Room options: Room_RoomInformationV9.res is missing; V7 stands in */
	g_winRoomOpts = Screens_Load("resource/mapmodev2/room_roominformationv7.res", "GameRoomInfoDlg");
	RoomOpts_Init(g_winRoomOpts);
	/* panels sharing a spot with the ones above, for other modes */
	string other[6] = {"AllStarGameRule", "AllStarGameRuleLabel", "BallNumber", "Money", "MoneyLabel", "ZombieRestrict"};
	for (int o = 0i; o < 6i; o++)
		Panel_Show(Panel_Find(g_winRoomOpts, other[o]), false);
	Panel_Show(Panel_Find(g_winRoomOpts, "ZombieRestrictLabel"), false);
	/* the map rotation list is filled by the original's code (UNRESOLVED);
	   its labels hold a placeholder token (#CSO_kakaka) until then */
	for (int ml = 1i; ml <= 5i; ml++) {
		Panel_Show(Panel_Find(g_winRoomOpts, sprintf("MapListLabel%i", ml)), false);
		Panel_Show(Panel_Find(g_winRoomOpts, sprintf("MapListLabelBg%i", ml)), false);
	}
	vguiLabel infoTitle = (vguiLabel)Panel_Find(g_winRoomOpts, "RoomInfoTitle");
	if (infoTitle)
		infoTitle.SetTitle(VGUI_Localize("#CSO_RoomInfo_Label"));
	/* options with no table or rules behind them yet: shown, unavailable */
	string off[4] = {"WeaponRestrict", "MannerLimit", "Password", "UsingPassword"};
	for (int u = 0i; u < 4i; u++) {
		vguiWidget w = Panel_Find(g_winRoomOpts, off[u]);
		if (w)
			w.Disable();
	}

	/* Room chat: the layout's input sits below its own height, so the
	   window is made tall enough for it (INFERRED: 528 + 174 meets the
	   taskbar at 704); the message list is placed by code (DESIGN) */
	g_winRoomChat = Screens_Load("resource/res/room_chattingmanagerdlgver2.res", "ChattingManagerDlg");
	if (g_winRoomChat) {
		g_winRoomChat.SetSize([433, 174] * g_vpScale);
		g_lstRoomChat = spawn(vguiResList);
		Panel_AddAt(g_winRoomChat, g_lstRoomChat, [8, 8], [417, 130]);
		List_Setup(g_lstRoomChat, "");
		vguiComboBox recv = (vguiComboBox)Panel_Find(g_winRoomChat, "ReceiverList");
		if (recv) {
			recv.AddItem("All", "all");
			recv.SetSelected(0i);
		}
	}

	/* Taskbar: layout without a root panel; 1024x64 at the bottom (INFERRED) */
	g_panelFile = "resource/res/taskbarver7.res";
	vguiResLayout tres = spawn(vguiResLayout);
	if (tres.LoadFile(g_panelFile)) {
		g_winTaskbar = spawn(vguiWindow);
		g_winTaskbar.SetMaxSize([8192, 8192]);
		g_winTaskbar.SetStyleMask(vguiWindowBorderless);
		g_uiDesktop.Add(g_winTaskbar);
		g_winTaskbar.SetSize([1024, 64] * g_vpScale);
		vguiResImage bar = spawn(vguiResImage);
		bar.SetImage("resource/taskbar");
		bar.SetScaleImage(true);
		Panel_AddAt(g_winTaskbar, bar, [0, 0], [1024, 64]);
		VGUI_ResBuild(g_winTaskbar, tres, "", g_vpScale);
		g_winTaskbar.csnzResPos = [0, 704];
		Panel_Place(g_winTaskbar);
		g_winTaskbar.Hide();
		if (g_panelCount < 48i)
			g_panels[g_panelCount++] = g_winTaskbar;
	} else {
		print("^1menu: resource/res/taskbarver7.res is missing\n");
	}
	g_panelFile = "";

	/* features of the account service: shown, and say why they can't open */
	string account[7] = {"BulletinButton", "ShopButton", "InventoryButton", "ItemFarmButton", "QuestButton", "ClanButton", "StatisticButton"};
	for (int a = 0i; a < 7i; a++) {
		vguiResButton b = (vguiResButton)Panel_Find(g_winTaskbar, account[a]);
		if (b)
			b.SetAlpha(0.45f);
	}

	/* Exit Game popup */
	g_winExit = Screens_Load("resource/res/popup_logout.res", "ExitPopupDlg");
	/* account statistics: the format strings show "-" (no account data) */
	string exitLabels = "lbl_WinLose #CSO_WinLoseNum lbl_KillDeath #CSO_KillDeathNum lbl_ZombieKillSurvival #CSO_ZombieKillSurvivalNum "
		"lbl_PointChange #CSO_PointChangeNum lbl_CurrentLevel #CSO_CurrentLevel lbl_NextLevel #CSO_NextLevel "
		"lbl_GetExp #CSO_GetExp_Label lbl_RemainExp #CSO_RemainExp_Label lbl_GetExpPercent #CSO_GetExpPercent";
	int pairs = (int)tokenize(exitLabels);
	for (int k = 0i; k < pairs; k += 2i) {
		vguiLabel l = (vguiLabel)Panel_Find(g_winExit, argv(k));
		string token = strzone(argv(k + 1i));
		if (l)
			l.SetTitle(Text_Unknown(VGUI_Localize(token)));
		strunzone(token);
		tokenize(exitLabels);
	}
}

/* --- navigation -------------------------------------------------------------- */

static void
Nav_HideAll(void)
{
	for (int i = 0i; i < g_panelCount; i++)
		g_panels[i].Hide();

	if (g_winCreate)
		g_winCreate.Hide();
	if (g_winInGame)
		g_winInGame.Hide();
	g_modal = __NULL__;
}

static void
Nav_TaskbarFor(float scr)
{
	Panel_Show(Panel_Find(g_winTaskbar, "ServerButton"), scr == SCR_CHANNELS);
	Panel_Show(Panel_Find(g_winTaskbar, "ChannelButton"), scr == SCR_LOBBY);
	Panel_Show(Panel_Find(g_winTaskbar, "RommListButton"), scr == SCR_ROOM);
	g_winTaskbar.Show();

	if (g_lblMyInfo)
		g_lblMyInfo.SetTitle(sprintf("%s %s   %s", VGUI_Localize("#CSO_Level_Lable"), "-", cvar_string("name")));
}

void
Room_FillPlayers(void)
{
	g_lstCT.ClearRows();
	g_lstTR.ClearRows();

	vguiResList mine = (g_roomTeam == 0) ? g_lstCT : g_lstTR;
	mine.AddRow(sprintf("-\t%s (host)", cvar_string("name")), "me");

	vguiLabel state = (vguiLabel)Panel_Find(g_winRoomPlayers, "RoomState");
	if (state)
		state.SetTitle(VGUI_Localize("#CSO_Room_State_Waiting"));

	vguiLabel small = (vguiLabel)Panel_Find(g_winRoomPlayers, "PlayerListTitleSmall");
	if (small)
		small.SetTitle("(1)");

	vguiLabel rtitle = (vguiLabel)Panel_Find(g_winRoomOpts, "Title");
	if (rtitle)
		rtitle.SetTitle(g_roomTitle);
}

void
Nav_Go(float scr)
{
	Nav_HideAll();
	g_screen = scr;

	switch (scr) {
	case SCR_SERVERS:
		g_winPromoLeft.Show();
		g_winClanFriends.Show();
		g_winServers.Show();
		g_winPromoBottom.Show();
		Svc_Request(SVC_SERVERS);
		Servers_Fill();
		break;
	case SCR_CHANNELS:
		g_winPromoLeft.Show();
		g_winClanFriends.Show();
		g_winChannels.Show();
		g_winPromoBottom.Show();
		Svc_Request(SVC_CHANNELS);
		Channels_Fill();
		break;
	case SCR_LOBBY:
		g_roomActive = false;
		g_winRoomInfo.Show();
		g_winUsers.Show();
		g_winRooms.Show();
		g_winPromoRoom.Show();
		if (g_winLobbyChat)
			g_winLobbyChat.Show();
		Svc_Request(SVC_ROOMS);
		Rooms_Fill();
		Users_Fill();
		Chat_Fill(g_lstLobbyChat);
		break;
	case SCR_ROOM:
		g_roomActive = true;
		g_winRoomOpts.Show();
		g_winRoomPlayers.Show();
		if (g_winRoomChat)
			g_winRoomChat.Show();
		Room_FillPlayers();
		Chat_Fill(g_lstRoomChat);
		break;
	}

	Nav_TaskbarFor(scr);
}

static void
Nav_Back(void)
{
	switch (g_screen) {
	case SCR_CHANNELS: Nav_Go(SCR_SERVERS); break;
	case SCR_LOBBY: Nav_Go(SCR_CHANNELS); break;
	case SCR_ROOM: Nav_Go(SCR_LOBBY); break;
	}
}

/* polls the services; called every frame */
void
Screens_Frame(void)
{
	if (Svc_Poll(SVC_SERVERS))
		Servers_Fill();
	if (Svc_Poll(SVC_CHANNELS))
		Channels_Fill();
	if (Svc_Poll(SVC_ROOMS))
		Rooms_Fill();
}

/* --- popups ------------------------------------------------------------------ */

void
Create_Open(void)
{
	if (!g_winCreate)
		return;

	vguiTextBox name = (vguiTextBox)Panel_Find(g_winCreate, "HostName");
	if (name)
		if (name.GetText() == "")
			name.SetText(VGUI_Localize("#CSO_Room_DefaultTitle00"));

	g_winCreate.Show();
	g_focusNext = g_winCreate;
	g_modal = g_winCreate;
}

static void
Create_Done(void)
{
	vguiTextBox name = (vguiTextBox)Panel_Find(g_winCreate, "HostName");

	if (RoomOpts_Value(g_winCreate, "MapName") == "") {
		MsgBox_Show("#CSO_ServerMessage_Title", "No map for this mode is installed.");
		return;
	}

	g_roomTitle = strzone(name ? name.GetText() : "");
	g_roomTeam = 0;
	RoomOpts_Copy(g_winCreate, g_winRoomOpts);
	g_winCreate.Hide();
	g_modal = __NULL__;
	Nav_Go(SCR_ROOM);
}

static void
Exit_Open(void)
{
	if (!g_winExit) {
		localcmd("quit\n");
		return;
	}

	g_winExit.Show();
	g_focusNext = g_winExit;
	g_modal = g_winExit;
}

/* --- commands -------------------------------------------------------------- */

#define WHY_ACCOUNT "It needs the CSO account service, which this build does not contact."
#define WHY_MATCH "It needs the matchmaking service, which this build does not contact."
#define WHY_ALONE "Nobody else is in this local room."

void
Nav_Command(vguiWidget w, string cmd)
{
	/* buttons the original handles by name have no command in the layout */
	if (cmd == "" && w)
		cmd = strcat("name:", w.vguiResName);

	if (MsgBox_Command(cmd))
		return;
	if (MapSel_Command(cmd))
		return;

	switch (strtolower(cmd)) {
	/* server and channel lists */
	case "refreshserverlist":
		Svc_Request(SVC_SERVERS);
		Servers_Fill();
		break;
	case "refreshchannellist":
		Svc_Request(SVC_CHANNELS);
		Channels_Fill();
		break;
	case "name:quickstartbtn":
	case "name:quickstartmodebtn":
		MsgBox_Unavailable("Quick Start", WHY_MATCH);
		break;
	case "name:customstartbtn":
		MsgBox_Unavailable("Custom Start", WHY_MATCH);
		break;
	case "playwith":
	case "sendmemo":
	case "openmemodialog":
	case "addfriend":
	case "name:whisper":
		MsgBox_Unavailable("Friends, memos and whispers", WHY_ACCOUNT);
		break;

	/* lobby */
	case "createroom":
		Create_Open();
		break;
	case "enterroom":
		if (g_lstRooms.GetSelected() < 0i)
			MsgBox_Show("#CSO_ServerMessage_Title", "Select a room first.");
		else
			MsgBox_Show("#CSO_ServerMessage_Title", "This room is FIXTURE data. Joining another player's room needs the official service.");
		break;
	case "name:findbtn":
		Rooms_Fill();
		break;
	case "change_filter":
		g_filtersShown = !g_filtersShown;
		string filters[12] = {"Label1", "FilterMap", "Label2", "FilterPing", "Label3", "FilterGameMode",
			"FilterLevel", "FilterSlot", "FilterPassword", "FindKeyword", "FindBtn", "Label1"};
		for (int i = 0i; i < 11i; i++)
			Panel_Show(Panel_Find(g_winRooms, filters[i]), g_filtersShown);
		Rooms_Fill();
		break;

	/* create room popup */
	case "makegameroom":
		Create_Done();
		break;
	case "close":
		if (g_modal == g_winCreate) {
			g_winCreate.Hide();
			g_modal = __NULL__;
		}
		if (g_modal == g_winMapSel)
			MapSel_Command("ms_cancel");
		break;

	/* game room */
	case "name:startbutton":
		Room_StartGame();
		break;
	case "name:teamchangebutton":
		g_roomTeam = 1 - g_roomTeam;
		Room_FillPlayers();
		break;
	case "name:ctbutton":
		g_roomTeam = 0;
		Room_FillPlayers();
		break;
	case "name:trbutton":
		g_roomTeam = 1;
		Room_FillPlayers();
		break;
	case "name:invitebutton":
	case "invite_to_game":
	case "add_ban":
		MsgBox_Show("#CSO_ServerMessage_Title", WHY_ALONE);
		break;
	case "name:botsetting":
	case "name:detailsetting":
	case "name:camall":
	case "name:overview":
	case "name:thirdperson":
	case "name:deathcam":
	case "name:correctpassword":
		MsgBox_Unavailable("This room setting", "The Zombi rules in this build don't read it yet.");
		break;

	/* taskbar */
	case "gotoback":
		Nav_Back();
		break;
	case "openbulletindlg":
	case "openshopdlg":
	case "openinventorydlg":
	case "openitemfarmdlg":
	case "openquestdlg":
	case "openclandialog":
	case "openstatisticdlg":
	case "registercafe":
	case "tutorial":
		MsgBox_Unavailable("This feature", WHY_ACCOUNT);
		break;
	case "openoptionsdialog":
		Options_Open();
		break;
	case "quit":
		Exit_Open();
		break;

	/* exit popup */
	case "ok":
		if (g_modal == g_winExit) {
			g_winExit.Hide();
			g_modal = __NULL__;
		}
		break;
	case "exit":
		localcmd("quit\n");
		break;

	/* in-game menu */
	case "releasemodalwindow":
		m_toggle(0);
		break;
	case "voxelsave":
		/* the dialog is the HUD's: it knows whether a test runs */
		m_toggle(0);
		localcmd("vxl_savedlg\n");
		break;
	case "openconfirmdialog":
		/* title, button and first line OBSERVED (CSO_GameLeave*); the
		   point penalty line is left out: there is no account here */
		MsgBox_AskWith("#CSO_GameLeaveConfirmation_Title",
			sprintf("%s\n(No points are deducted: this build has no account.)",
				substring(VGUI_Localize("#CSO_GameLeaveConfimation_Text"), 0,
					strstrofs(VGUI_Localize("#CSO_GameLeaveConfimation_Text"), "\n"))),
			"leave_match", "#CSO_GameLeave_Btn");
		break;
	case "leave_match":
		/* "you will be returned to the room list screen" (OBSERVED) */
		g_leftMatch = true;
		localcmd("disconnect\n");
		break;

	default:
		if (!Options_Command(cmd))
			MsgBox_Show("#CSO_ServerMessage_Title", sprintf("\"%s\" is not implemented yet.", cmd));
	}
}
