/* Team menu (handoff M3): CCSTeamMenu.

   SOURCE-CONFIRMED (client.dll FUN_02d33620, the CTeamMenu constructor):
   a vgui::Frame "TeamMenu" with a "MapInfo" HTML panel and buttons named
   teambutton%i, laid out by Resource/UI/CSO_TeamMenu.res, titled
   #Cstrike_Select_Team. client.dll also names maps/%s.txt and
   maps/default.txt, which CS's team menu shows as the map's description
   (their use here is INFERRED).

   The layout is CSO_TeamMenu.res (cstrike, OBSERVED): a 606x420 frame,
   the map info at 200,50 (316x286), and a column of buttons at x 24 whose
   commands are the res file's own: jointeam 1, 2, 3 (VIP), 5 (auto), 6
   (spectate) and vguicancel.

   DESIGN: the VIP button sits on top of the terrorist button in the res
   file and is hidden; spectating has no rules support yet, so its button
   is drawn disabled; BotGameLabel (#CSO_BotGameMsg) only applies to
   lobby-hosted bot matches and is not drawn. The map info is plain text,
   not HTML. Opened by "chooseteam" (sokky's config binds M to it);
   joining a team opens the class menu next. */

hashtable g_tmRes;
bool g_tmOpen;
string g_tmMapInfo;

void
TM_Init(void)
{
	g_tmRes = Res_Load("resource/ui/cso_teammenu.res");
	Loc_Load("resource/cstrike_na_en.utf8.txt",
		"Cstrike_Select_Team Cstrike_Terrorist_Forces Cstrike_CT_Forces Cstrike_VIP_Team Cstrike_Team_AutoAssign Cstrike_Menu_Spectate Cstrike_cancel");
	registercommand("chooseteam");
}

/* maps/<map>.txt, else maps/default.txt, as plain text */
string
TM_ReadMapInfo(void)
{
	string map = mapname;
	filestream fh;
	string txt = "", line;

	/* mapname can carry the maps/ prefix and the .bsp suffix */
	if (substring(map, 0, 5) == "maps/")
		map = substring(map, 5, -1);
	if (substring(map, strlen(map) - 4, 4) == ".bsp")
		map = substring(map, 0, strlen(map) - 4);

	fh = fopen(sprintf("maps/%s.txt", map), FILE_READ);
	if (fh < 0)
		fh = fopen("maps/default.txt", FILE_READ);
	if (fh < 0)
		return map;

	while (1) {
		line = fgets(fh);
		if not (line)
			break;
		txt = strcat(txt, line, "\n");
	}
	fclose(fh);
	return txt;
}

void
TM_Open(void)
{
	if (g_tmMapInfo)
		strunzone(g_tmMapInfo);
	g_tmMapInfo = strzone(TM_ReadMapInfo());
	g_tmOpen = true;
}

void
TM_Close(void)
{
	g_tmOpen = false;
}

vector
TM_FrameSize(void)
{
	if (Res_Has(g_tmRes, "TeamMenu"))
		return Res_Size(g_tmRes, "TeamMenu");

	return [606, 420];
}

vector
TM_FrameOrigin(void)
{
	return Res_Pos(g_tmRes, "TeamMenu", screen.HUDSize() / g_hs);
}

bool
TM_Enabled(string ctl)
{
	switch (ctl) {
	case "terbutton":
	case "ctbutton":
	case "autobutton":
	case "cancelbutton":
		return true;
	}

	return false;
}

void
TM_Command(string ctl)
{
	string cmd = Res_Get(g_tmRes, ctl, "Command");

	if (cmd == "vguicancel") {
		TM_Close();
		return;
	}
	if (substring(cmd, 0, 9) == "jointeam ") {
		localcmd(sprintf("cmd %s\n", cmd));
		TM_Close();
		/* then the class menu, as in CS (DESIGN) */
		localcmd("csnz_classmenu\n");
	}
}

void
TM_Draw(void)
{
	vector fo, fs;
	string ctls = "terbutton ctbutton autobutton specbutton cancelbutton";

	if (!g_tmOpen)
		return;

	fo = TM_FrameOrigin();
	fs = TM_FrameSize();

	Res_Frame9("resource/control/igframe/window_default_", fo, fs, 1.0f);
	draw.Text_RGB(Art_Pos(fo + [12, 8]), Loc("Cstrike_Select_Team", "Team Selection"), [1,1,1], g_hudFont);

	int n = (int)tokenize(ctls);
	for (int i = 0i; i < n; i++) {
		tokenize(ctls);
		string ctl = argv(i);
		Res_DrawButton(g_tmRes, fo, fs, ctl, Res_Text(Res_Get(g_tmRes, ctl, "labelText")), TM_Enabled(ctl), true);
	}

	/* the map info, line by line in its box */
	vector p = Res_CtlPos(g_tmRes, fo, fs, "MapInfo");
	vector s = Res_Size(g_tmRes, "MapInfo");
	float lh = font.GetHeight(g_hudFont) / g_hs + 2;
	int lines = (int)tokenizebyseparator(g_tmMapInfo, "\n");

	for (int i = 0i; i < lines && (i + 1i) * lh < s[1]; i++)
		draw.Text_RGB(Art_Pos(p + [0, i * lh]), argv(i), [0.9, 0.9, 0.9], g_hudFont);
}

bool
TM_Input(float evType, float key, float chr, float dev)
{
	string ctls = "terbutton ctbutton autobutton specbutton cancelbutton";

	if (!g_tmOpen)
		return false;

	if (evType == IE_MOUSEABS || evType == IE_MOUSEDELTA) {
		g_resHoverCtl = Res_HitTest(g_tmRes, TM_FrameOrigin(), TM_FrameSize(), ctls);
		return true;
	}

	if (evType == IE_KEYDOWN) {
		if (key == K_MOUSE1) {
			g_resPressCtl = Res_HitTest(g_tmRes, TM_FrameOrigin(), TM_FrameSize(), ctls);
			return true;
		}
		/* the labels' hotkeys: &1, &2, &5, &6 and 0 to cancel */
		switch (key) {
		case K_ESCAPE:
		case '0':
			TM_Close();
			return true;
		case '1':
			TM_Command("terbutton");
			return true;
		case '2':
			TM_Command("ctbutton");
			return true;
		case '5':
			TM_Command("autobutton");
			return true;
		}
		return false;
	}

	if (evType == IE_KEYUP && key == K_MOUSE1) {
		string ctl = Res_HitTest(g_tmRes, TM_FrameOrigin(), TM_FrameSize(), ctls);

		if (ctl != "" && ctl == g_resPressCtl && TM_Enabled(ctl))
			TM_Command(ctl);
		g_resPressCtl = "";
		return true;
	}

	return false;
}

bool
TM_ConsoleCommand(string cmd)
{
	tokenize_console(cmd);

	if (argv(0) != "chooseteam")
		return false;

	if (g_tmOpen)
		TM_Close();
	else
		TM_Open();
	return true;
}
