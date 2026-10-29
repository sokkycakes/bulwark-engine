/* CSNZ front end: menu progs entry points.

   Screens are in screens.qc (lobby flow), options.qc, msgbox.qc and
   loading.qc; the layouts are the client's .res files (OBSERVED), placed
   in a 1024x768 viewport (viewport.qc). Data that the original gets from
   Nexon's servers comes from services.qc: local data or labelled fixtures.

   In-game menu: resource/res/ingamemainmenu.res (OBSERVED). Its buttons
   have no labelText; the original draws images the client code picks
   (UNRESOLVED). The text labels here are DESIGN.
   In Studio Create the original adds a VoxelSaveButton (GameUI.dll
   CInGameMenu: resource/taskbar/btn_save, command "VoxelSave"), listed
   after Report and before Option (string order, INFERRED as the button
   order). Here it opens the HUD's Save map dialog (hud/vxsave.qc). */

#define LOCALIZATION_FILES "resource/cso_na_en.utf8.txt resource/gameui_na_en.utf8.txt resource/cstrike_na_en.utf8.txt"

int g_active;
float lasttime;
bool g_needBuild;
bool g_wasInGame;


/* --- game room: start ------------------------------------------------------ */

/* sets a cvar from a room option, when the mode has that option */
static void
Room_SetFromOption(string cvarName, string option)
{
	string v = RoomOpts_Value(g_winRoomOpts, option);

	if (v != "")
		localcmd(sprintf("set %s %s\n", cvarName, v));
}

/* DESIGN: cvar names the game rules read. They are ours, not CSO's. */
void
Room_StartGame(void)
{
	string mapName = RoomOpts_Value(g_winRoomOpts, "MapName");
	string mode = RoomOpts_Value(g_winRoomOpts, "GameMode");

	if (mapName == "") {
		MsgBox_Show("#CSO_ServerMessage_Title", "No map for this mode is installed.");
		return;
	}

	localcmd(sprintf("set csnz_mode %s\n", mode));
	/* the rules progs for the mode (INTERFACES.md section 2): Zombi has its
	   own, Studio modes theirs, csnz.dat runs every other id */
	if (mode == "8")
		localcmd("set g_gametype zombi\n");
	else if (mode == "38" || mode == "39" || mode == "49" || mode == "52" || mode == "53")
		localcmd("set g_gametype studio\n");
	else
		localcmd("set g_gametype csnz\n");
	Room_SetFromOption("mp_winlimit", "WinLimit");
	Room_SetFromOption("mp_fraglimit", "KillLimit");
	Room_SetFromOption("mp_timelimit", "TimeLimit");
	Room_SetFromOption("mp_roundtime", "RoundTime");
	Room_SetFromOption("maxplayers", "MaxPlayer");
	localcmd(sprintf("set hostname \"%s\"\n", g_roomTitle));
	localcmd(sprintf("map %s\n", mapName));
	m_toggle(0);
}

/* --- create room ------------------------------------------------------------ */

static void
Create_Init(void)
{
	g_winCreate = Screens_Load("resource/popup_makeroom.res", "CreateRoomPopup");

	if (!g_winCreate)
		return;

	g_winCreate.SetStyleMask(vguiWindowTitled);
	/* a popup, centred in the layout */
	g_winCreate.csnzResPos = ([VP_WIDTH, VP_HEIGHT] - g_winCreate.GetSize() / g_vpScale) / 2;
	Panel_Place(g_winCreate);

	/* the short layout isn't offered */
	Panel_Show(Panel_Find(g_winCreate, "Button3"), false);
	RoomOpts_Init(g_winCreate);
}

/* --- in-game menu ----------------------------------------------------------- */

static vguiWidget
InGame_Control(vguiResLayout res, string path)
{
	vguiResButton btn;

	if (g_panelFile != "resource/res/ingamemainmenu.res")
		return Screens_Control(res, path);

	btn = spawn(vguiResButton);
	btn.m_resCommand = strzone(res.GetString(strcat(path, "/command")));

	switch (strtolower(btn.m_resCommand)) {
	case "opendemorecorddialog": btn.SetTitle("Record"); break;
	case "dmrecordstop": btn.SetTitle("Stop"); break;
	case "openreportdialog": btn.SetTitle("Report"); break;
	case "openoptionsdialog": btn.SetTitle("Options"); break;
	case "openconfirmdialog": btn.SetTitle("Leave"); break;
	case "releasemodalwindow": btn.SetTitle("Back"); break;
	}

	return btn;
}

vguiResButton g_btnVoxelSave;
var float g_inGameStudio = -1;

/* the layout's buttons are 64 apart from y 20; in Studio Create the Save
   button takes Option's place and the rest move down one */
void
InGame_Layout(bool studio)
{
	if (!g_winInGame || g_inGameStudio == (float)studio)
		return;
	g_inGameStudio = studio;

	string below[3] = {"OptionButton", "ExitButton", "CancelButton"};
	for (int i = 0i; i < 3i; i++) {
		vguiWidget w = Panel_Find(g_winInGame, below[i]);
		if (w)
			w.SetPos([29, 148 + (i + (studio ? 1i : 0i)) * 64] * g_vpScale);
	}
	Panel_Show(g_btnVoxelSave, studio);
	g_winInGame.SetSize([104, studio ? 428 : 364] * g_vpScale);
	g_winInGame.csnzResPos = ([VP_WIDTH, VP_HEIGHT] - g_winInGame.GetSize() / g_vpScale) / 2;
	Panel_Place(g_winInGame);
}

static void
InGame_Init(void)
{
	g_winInGame = Screens_Load("resource/res/ingamemainmenu.res", "InGameMenu");

	if (!g_winInGame)
		return;

	g_winInGame.SetStyleMask(vguiWindowBorderless);
	g_btnVoxelSave = spawn(vguiResButton);
	g_btnVoxelSave.SetImages("resource/taskbar/btn_save@n", "resource/taskbar/btn_save@o", "resource/taskbar/btn_save@c", "");
	g_btnVoxelSave.SetTextHidden(true);
	g_btnVoxelSave.m_resCommand = strzone("VoxelSave");
	Panel_AddAt(g_winInGame, g_btnVoxelSave, [29, 148], [64, 64]);
	g_inGameStudio = -1;
	InGame_Layout(false);
}

/* --- menu progs entry points -------------------------------------------- */

void
m_init(void)
{
	UISystem_Init();

	int n = (int)tokenize(LOCALIZATION_FILES);
	for (int i = 0i; i < n; i++) {
		string file = strzone(argv(i));
		VGUI_LoadLocalization(file);
		strunzone(file);
		tokenize(LOCALIZATION_FILES);
	}
	Tables_LoadLimits();

	/* Bulwark passes a console command to the menu only when it is registered */
	registercommand("csnz_menu_cmd");
	registercvar("csnz_service", "local");
	registercvar("csnz_fixture_error", "");
	registercvar("csnz_mode", "8");

	g_vguiResCommand = Nav_Command;
	g_vguiResImageCommand = Nav_Command;
	g_vguiResCustomControl = InGame_Control;
	g_uiDesktop = spawn(vguiWidget);
	g_vidsize = getresolution(0);
	Viewport_Update(g_vidsize);

	/* GameUI's scheme: VGUI2 menus use resource/trackerscheme.res (OBSERVED
	   file; which scheme GameUI loads is STOCK VGUI2 convention). Check
	   boxes and the combo arrow are not in the scheme; these images are
	   picked by name (INFERRED). */
	g_scheme = spawn(vguiSchemeTheme);
	if (g_scheme.LoadScheme("resource/trackerscheme.res")) {
		g_scheme.SetCheckboxImages("resource/control/checkbox/checkbox_off", "resource/control/checkbox/checkbox_on");
		g_scheme.SetComboArrowImage("resource/control/textentry/pulldownarrow_default@n");
		g_uiDesktop.SetTheme(g_scheme);
	} else {
		print("^1menu: resource/trackerscheme.res is missing, using Nuclide's theme\n");
		g_scheme = __NULL__;
	}

	/* the panels are built on the first frame, when the screen size (and
	   so the layout scale) is known */
	g_needBuild = true;
}

static void
Menu_Build(vector screen)
{
	g_needBuild = false;
	g_vidsize = screen;
	Viewport_Update(screen);
	Screens_Init();
	Create_Init();
	InGame_Init();
	Options_Init();
	MsgBox_Init();
	Nav_Go(SCR_SERVERS);
}

void
m_shutdown(void)
{
}

void
Menu_RendererRestarted(string renderer)
{
}

/* the service tag sits outside the 1024x768 viewport when there is room,
   else in the screen's top-right corner */
static void
Menu_DrawServiceTag(vector screen)
{
	string tag = Svc_Label();
	vector col = Svc_IsFixture() ? [1, 0.8, 0.2] : [0.6, 0.6, 0.6];
	float w = stringwidth(tag, TRUE, [8,8]) + 8;

	if (g_vp[0] >= w)
		drawstring([(g_vp[0] - w) / 2 + 4, 8], tag, [8,8], col, 1.0f, 0);
	else
		drawstring([screen[0] - w, 2], tag, [8,8], col, 0.8f, 0);
}

void
m_draw(vector screensize)
{
	frametime = time - lasttime;
	lasttime = time;

	if (g_needBuild)
		Menu_Build(screensize);

	if (screensize != g_vidsize) {
		g_vidsize = screensize;
		Viewport_Update(screensize);
		Panel_PlaceAll();
	}

	bool inGame = (clientstate() == 2);

	/* back from a match: the room list after Leave (OBSERVED in
	   CSO_GameLeaveConfimation_Text), else the game room (INFERRED) */
	if (g_wasInGame && !inGame) {
		if (g_leftMatch)
			g_roomActive = false;
		g_leftMatch = false;
		Nav_Go(g_roomActive ? SCR_ROOM : SCR_LOBBY);
		m_toggle(1);
	}
	g_wasInGame = inGame;

	if (!g_active)
		return;

	if (g_focusNext) {
		if (g_focusNext.Visible())
			g_focusNext.Focus();
		g_focusNext = __NULL__;
	}

	Screens_Frame();

	if (inGame) {
		/* in a match only the in-game menu (and its popups) show */
		if (g_winInGame && !g_modal) {
			if (!g_winInGame.Visible()) {
				InGame_Layout(cvar_string("g_gametype") == "studio" && cvar("csnz_mode") == 38);
				for (int i = 0i; i < g_panelCount; i++)
					g_panels[i].Hide();
				g_winInGame.Show();
			}
		}
		drawfill([0,0], g_vidsize, [0,0,0], 0.5f);
	} else {
		/* the lobby background: resource/bg.tga stretched (INFERRED) */
		drawfill([0,0], g_vidsize, [0,0,0], 1.0f);
		drawpic(Viewport_Map([0,0]), "resource/bg", [VP_WIDTH, VP_HEIGHT] * g_vpScale, [1,1,1], 1.0f, 0);
	}

	g_uiDesktop.Draw();
	Viewport_DrawMargins(screensize);
	Menu_DrawServiceTag(screensize);
}

void
m_drawloading(vector screensize, float opaque)
{
	Loading_Draw(screensize);
}

float
Menu_InputEvent(float evtype, float scanx, float chary, float devid)
{
	if (!g_active || g_needBuild)
		return (0);

	g_vecMousePos = getmousepos();

	if (evtype == IE_KEYDOWN && scanx == K_ESCAPE) {
		/* Esc closes the top popup, then leaves the in-game menu */
		if (g_modal == g_msgWin) {
			MsgBox_Close();
			return (1);
		}
		if (g_modal) {
			Nav_Command(__NULL__, g_modal == g_winOptions ? "opt_cancel" : (g_modal == g_winExit ? "ok" : "close"));
			return (1);
		}
		if (clientstate() == 2) {
			m_toggle(0);
			return (1);
		}
	}

	if (g_modal)
		g_modal.Input(evtype, scanx, chary, devid);
	else
		g_uiDesktop.Input(evtype, scanx, chary, devid);

	return (1);
}

void
m_display(void)
{
	g_active = TRUE;
	setkeydest(KEY_MENU);
	setmousetarget(TARGET_MENU);
	setcursormode(TRUE);
}

void
m_hide(void)
{
	g_active = FALSE;
	setkeydest(KEY_GAME);
	setmousetarget(TARGET_CLIENT);
	setcursormode(FALSE);

	if (g_winInGame)
		g_winInGame.Hide();
}

void
m_toggle(float wantmode)
{
	if (wantmode)
		m_display();
	else
		m_hide();
}

float
m_consolecommand(string cmd)
{
	tokenize(cmd);

	/* test hook: csnz_menu_cmd <command> runs a menu command as if clicked */
	if (argv(0) == "csnz_menu_cmd") {
		Nav_Command(__NULL__, argv(1));
		return (1);
	}

	return (0);
}
