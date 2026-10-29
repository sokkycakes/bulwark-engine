/* Studio map selection: which map file a Studio room edits or plays.

   SOURCE-CONFIRMED (GameUI.dll strings; the layout is
   resource/voxel/popup_voxeloutui_mapselectdlg.res, CVoxelOutUIMapSelectDlg):
   - A 932x480 panel: the map grid at 64,36 (589x397) on a background at
     53,19, the details on the right (#Vxl_OutUI_MapSelect heading, title
     at 667,47, author at 774,106, description at 667,136) and the
     #Vxl_OutUI_MapSelect_MakeRoom_Btn button "Maps selection complete"
     (command ClickSelectMapBtn) at 683,402.
   - Two tabs, #Vxl_OutUI_MapSelect_TAB_01 "Sample map" and _TAB_02
     "My map". The sample's author is #Vxl_OutUI_MapSelect_SampleAuthor and
     its text #Vxl_OutUI_MapSelect_SampleDesc; an untitled map shows
     #Vxl_OutUI_MapSelect_TitleEmpty "No title".
   - A room can't be made without a map (#Vxl_OutUI_ReqCreateRoom_Err_01
     "Try again after selecting the map"). Slots are copied with the title
     suffix #Vxl_OutUI_SlotCopyTitle "_Copy" (#Vxl_OutUI_All_MapCopy_Btn
     "Copy Map") and removed after #Vxl_OutUI_My_SlotRemove_Check.
   - CSO keeps maps on its servers (OfflineServer VoxelConfigList.json:
     VxlURL .../%s.vxl), one slot per map (Vxl_OutUI_My_MaxCreateSlot).

   DESIGN: maps are the local files data/studio/maps/<name>.bwvx, with the
   details the rules write beside them in <name>.txt (vx_maps.qc). The grid
   is a list (title, file, last saved); the tabs sit above it; Copy Map,
   Delete map and Cancel sit under it. In a Create room the title and
   description can be edited here; the rules apply them when the room
   starts (an unsaved change). Picking the sample makes a new map file. A
   Play room offers only saved maps. The room starts with studio_map set;
   studio_copyfrom, studio_settitle and studio_setdesc carry the rest. */

#define MS_MAX 128

vguiResList g_lstMapSel;
vguiResButton g_msTabSample;
vguiResButton g_msTabMy;
vguiTextBox g_msTitle;
vguiTextBox g_msAuthor;
vguiTextBox g_msDesc;
int g_msTab;
int g_msCount;
string g_msName[MS_MAX];
string g_msTitleS[MS_MAX];
string g_msAuthorS[MS_MAX];
string g_msDescS[MS_MAX];
string g_msSavedS[MS_MAX];
string g_msRoomMode;
var int g_msSel = -1i;
bool g_msChosen;

void Room_StartGame(void);

static string
MapSel_Loc(string token, string fallback)
{
	string s = VGUI_Localize(token);
	return (s == "" || s == token) ? fallback : s;
}

/* the details the rules wrote beside the map: "key value" lines */
static void
MapSel_ReadInfo(int i)
{
	filestream fh = fopen(sprintf("studio/maps/%s.txt", g_msName[i]), FILE_READ);
	string line;

	g_msTitleS[i] = g_msAuthorS[i] = g_msDescS[i] = g_msSavedS[i] = "";
	if (fh < 0)
		return;
	while ((line = fgets(fh))) {
		float sp = strstrofs(line, " ");
		if (sp < 0)
			continue;
		string key = substring(line, 0, sp);
		string val = strzone(substring(line, sp + 1, -1));
		switch (key) {
		case "title": g_msTitleS[i] = val; break;
		case "author": g_msAuthorS[i] = val; break;
		case "desc": g_msDescS[i] = val; break;
		case "saved": g_msSavedS[i] = val; break;
		default: strunzone(val);
		}
	}
	fclose(fh);
}

static void
MapSel_Scan(void)
{
	for (int i = 0i; i < g_msCount; i++)
		if (g_msName[i])
			strunzone(g_msName[i]);
	g_msCount = 0i;

	float sh = search_begin("data/studio/maps/*.bwvx", TRUE, FALSE);
	if (sh < 0)
		return;
	int n = (int)search_getsize(sh);
	for (int k = 0i; k < n && g_msCount < MS_MAX; k++) {
		string f = search_getfilename(sh, k);
		string base = substring(f, strlen("data/studio/maps/"), -1);
		base = substring(base, 0, strlen(base) - 5);
		/* the rules' own files (bad.bwvx from a failed check, test
		   snapshots) aren't maps a player made */
		if (base == "" || base == "bad")
			continue;
		g_msName[g_msCount] = strzone(base);
		MapSel_ReadInfo(g_msCount);
		g_msCount++;
	}
	search_end(sh);
}

static void
MapSel_ShowDetails(void)
{
	string t = "", a = "", d = "";

	if (g_msTab == 0i) {
		t = MapSel_Loc("#Vxl_OutUI_MapSelect_TAB_01", "Sample map");
		a = MapSel_Loc("#Vxl_OutUI_MapSelect_SampleAuthor", "Sample map");
		d = MapSel_Loc("#Vxl_OutUI_MapSelect_SampleDesc", "Default sample map.");
	} else if (g_msSel >= 0i && g_msSel < g_msCount) {
		t = g_msTitleS[g_msSel];
		a = g_msAuthorS[g_msSel];
		d = g_msDescS[g_msSel];
	} else {
		t = MapSel_Loc("#Vxl_OutUI_Detail_Title_Init", "Choose the map.");
		d = MapSel_Loc("#Vxl_OutUI_Detail_Desc_Init", "Choose the map.");
	}
	if (g_msTitle)
		g_msTitle.SetText(t);
	if (g_msAuthor)
		g_msAuthor.SetText(a);
	if (g_msDesc)
		g_msDesc.SetText(d);
}

static void
MapSel_Fill(void)
{
	if (!g_lstMapSel)
		return;
	g_lstMapSel.ClearRows();
	g_lstMapSel.SetState(VGUI_LIST_READY, "");
	g_msTabSample.SetLatched(g_msTab == 0i);
	g_msTabMy.SetLatched(g_msTab == 1i);

	if (g_msTab == 0i) {
		g_lstMapSel.AddRow(sprintf("%s\t-\t-", MapSel_Loc("#Vxl_OutUI_MapSelect_TAB_01", "Sample map")), "sample");
		g_lstMapSel.SetSelected(0i);
		g_msSel = -1i;
	} else {
		for (int i = 0i; i < g_msCount; i++) {
			string t = g_msTitleS[i] != "" ? g_msTitleS[i] : MapSel_Loc("#Vxl_OutUI_MapSelect_TitleEmpty", "No title");
			g_lstMapSel.AddRow(sprintf("%s\t%s\t%s", t, g_msName[i], g_msSavedS[i] != "" ? g_msSavedS[i] : "-"), ftos(i));
		}
		g_msSel = -1i;
		g_lstMapSel.SetSelected(-1i);
	}
	MapSel_ShowDetails();
}

static void
MapSel_Selected(entity listEnt, int row)
{
	if (g_msTab == 1i && row >= 0i)
		g_msSel = (int)stof(((vguiResList)listEnt).RowKey(row));
	MapSel_ShowDetails();
}

static void
MapSel_Activated(entity listEnt, int row)
{
	MapSel_Selected(listEnt, row);
	Nav_Command(__NULL__, "ClickSelectMapBtn");
}

/* ThreePicsButton and the grid's background become a button and a list */
vguiWidget
MapSel_Control(vguiResLayout res, string path)
{
	string ctl = strtolower(res.GetString(strcat(path, "/ControlName")));

	if (ctl == "threepicsbutton") {
		vguiResButton btn = spawn(vguiResButton);
		btn.SetTitle(VGUI_Localize(res.GetString(strcat(path, "/labelText"))));
		btn.SetThreePic(res.GetString(strcat(path, "/ThreePicsTgaPath")));
		btn.m_resCommand = strzone(res.GetString(strcat(path, "/Command")));
		return btn;
	}

	return __NULL__;
}

void
MapSel_Init(void)
{
	g_panelFile = "resource/voxel/popup_voxeloutui_mapselectdlg.res";
	g_winMapSel = Panel_Load(g_panelFile, "VoxelOutUI_MapSelectDlg");
	g_panelFile = "";
	if (!g_winMapSel)
		return;

	g_winMapSel.SetStyleMask(vguiWindowTitled);
	g_winMapSel.SetTitle(MapSel_Loc("#Vxl_OutUI_MapSelect", "Select map"));
	/* room for the buttons under the grid */
	g_winMapSel.SetSize([932, 500] * g_vpScale);
	g_winMapSel.csnzResPos = ([VP_WIDTH, VP_HEIGHT] - [932, 500]) / 2;
	Panel_Place(g_winMapSel);

	g_msTitle = (vguiTextBox)Panel_Find(g_winMapSel, "MapSelect_DetailInfo_Title_TE");
	g_msAuthor = (vguiTextBox)Panel_Find(g_winMapSel, "MapSelect_DetailInfo_Author_TE");
	g_msDesc = (vguiTextBox)Panel_Find(g_winMapSel, "MapSelect_DetailInfo_Desc_TE");
	Panel_Show(Panel_Find(g_winMapSel, "MapSelectGridView_BG"), false);
	/* a TextEntry used as a backdrop in CSO; here it would take the clicks
	   meant for the tabs and the list */
	Panel_Show(Panel_Find(g_winMapSel, "MapSelectGridViewTotal_Bg"), false);

	g_lstMapSel = spawn(vguiResList);
	Panel_AddAt(g_winMapSel, g_lstMapSel, [64, 48], [589, 385]);
	List_Setup(g_lstMapSel, "Title:300;Map file:150;Saved:125");
	g_lstMapSel.SetEmptyText("No saved maps yet. Pick the sample map to start one.");
	g_lstMapSel.CallOnSelect(MapSel_Selected);
	g_lstMapSel.CallOnActivate(MapSel_Activated);

	g_msTabSample = Panel_Button(g_winMapSel, [64, 22], [120, 24], MapSel_Loc("#Vxl_OutUI_MapSelect_TAB_01", "Sample map"), "ms_tab0");
	g_msTabMy = Panel_Button(g_winMapSel, [188, 22], [120, 24], MapSel_Loc("#Vxl_OutUI_MapSelect_TAB_02", "My map"), "ms_tab1");
	Panel_Button(g_winMapSel, [64, 458], [120, 30], MapSel_Loc("#Vxl_OutUI_All_MapCopy_Btn", "Copy Map"), "ms_copy");
	Panel_Button(g_winMapSel, [188, 458], [120, 30], MapSel_Loc("#Vxl_OutUI_SlotDelete_Btn", "Delete map"), "ms_delete");
	Panel_Button(g_winMapSel, [533, 458], [120, 30], MapSel_Loc("#Vxl_OutUI_Btn_Close", "Cancel"), "ms_cancel");
}

/* from Room_StartGame: a Studio room needs its map first */
bool
MapSel_Needed(string mode)
{
	if (g_msChosen || (mode != "38" && mode != "39"))
		return false;
	if (!g_winMapSel)
		return false;

	g_msRoomMode = mode;
	MapSel_Scan();
	/* Play needs a saved map; Create starts on the maps the player has */
	g_msTab = (mode == "39" || g_msCount) ? 1i : 0i;
	MapSel_Fill();
	g_winMapSel.Show();
	g_focusNext = g_winMapSel;
	g_modal = g_winMapSel;
	return true;
}

static void
MapSel_Close(void)
{
	g_winMapSel.Hide();
	if (g_modal == g_winMapSel)
		g_modal = __NULL__;
}

static string
MapSel_NewName(void)
{
	string n = strcat("map_", strftime(true, "%Y%m%d_%H%M%S"));
	for (int i = 2i; fexists(sprintf("data/studio/maps/%s.bwvx", n)); i++)
		n = sprintf("map_%s_%i", strftime(true, "%Y%m%d_%H%M%S"), i);
	return n;
}

static void
MapSel_Start(string map, string copyFrom)
{
	string title = g_msTitle ? g_msTitle.GetText() : "";
	string desc = g_msDesc ? g_msDesc.GetText() : "";
	bool create = g_msRoomMode == "38";

	localcmd(sprintf("set studio_map \"%s\"\n", map));
	localcmd(sprintf("set studio_copyfrom \"%s\"\n", copyFrom));
	/* edits only, and only where the room can save them (fteqcc's && doesn't
	   short-circuit, so the index is guarded on its own) */
	bool unchanged = false;
	if (g_msTab == 1i && g_msSel >= 0i && g_msSel < g_msCount)
		unchanged = title == g_msTitleS[g_msSel] && desc == g_msDescS[g_msSel];
	if (!create || unchanged) {
		title = "";
		desc = "";
	}
	if (g_msTab == 0i && title == MapSel_Loc("#Vxl_OutUI_MapSelect_TAB_01", "Sample map"))
		title = "";
	if (g_msTab == 0i && desc == MapSel_Loc("#Vxl_OutUI_MapSelect_SampleDesc", "Default sample map."))
		desc = "";
	localcmd(sprintf("set studio_settitle \"%s\"\n", strreplace("\"", "'", strreplace("\n", " ", title))));
	localcmd(sprintf("set studio_setdesc \"%s\"\n", strreplace("\"", "'", strreplace("\n", " ", desc))));

	MapSel_Close();
	g_msChosen = true;
	Room_StartGame();
	g_msChosen = false;
}

void
MapSel_DeleteNow(void)
{
	if (g_msSel < 0i || g_msSel >= g_msCount)
		return;
	fremove(sprintf("studio/maps/%s.bwvx", g_msName[g_msSel]));
	fremove(sprintf("studio/maps/%s.txt", g_msName[g_msSel]));
	MapSel_Scan();
	MapSel_Fill();
	g_winMapSel.Show();
	g_modal = g_winMapSel;
}

bool
MapSel_Command(string cmd)
{
	switch (strtolower(cmd)) {
	case "ms_tab0":
		if (g_msRoomMode == "39") {
			MsgBox_Show("#Vxl_OutUI_MapSelect", "A Play room plays a saved map: pick one under My map.");
			return true;
		}
		g_msTab = 0i;
		MapSel_Fill();
		return true;
	case "ms_tab1":
		g_msTab = 1i;
		MapSel_Fill();
		return true;
	case "ms_cancel":
		MapSel_Close();
		return true;
	case "clickselectmapbtn":
		if (g_msTab == 0i) {
			if (g_msRoomMode == "39")
				return true;
			MapSel_Start(MapSel_NewName(), "");
		} else if (g_msSel < 0i || g_msSel >= g_msCount) {
			MsgBox_Show("#Vxl_OutUI_MapSelect", MapSel_Loc("#Vxl_OutUI_ReqCreateRoom_Err_01", "Try again after selecting the map."));
		} else {
			MapSel_Start(g_msName[g_msSel], "");
		}
		return true;
	case "ms_copy":
		if (g_msTab != 1i || g_msSel < 0i || g_msSel >= g_msCount) {
			MsgBox_Show("#Vxl_OutUI_MapSelect", MapSel_Loc("#Vxl_OutUI_My_SelectSlot_Err_01", "No slot has been selected."));
			return true;
		}
		if (g_msRoomMode != "38") {
			MsgBox_Show("#Vxl_OutUI_MapSelect", "Copy a map from a Create room: the copy opens there.");
			return true;
		}
		/* the copy opens in the Create room, saved under a new name */
		g_msTitle.SetText(strcat(g_msTitleS[g_msSel] != "" ? g_msTitleS[g_msSel] : g_msName[g_msSel],
			MapSel_Loc("#Vxl_OutUI_SlotCopyTitle", "_Copy")));
		string src = g_msName[g_msSel];
		g_msSel = -1i;
		MapSel_Start(MapSel_NewName(), src);
		return true;
	case "ms_delete":
		if (g_msTab != 1i || g_msSel < 0i || g_msSel >= g_msCount) {
			MsgBox_Show("#Vxl_OutUI_MapSelect", MapSel_Loc("#Vxl_OutUI_My_SelectSlot_Err_01", "No slot has been selected."));
			return true;
		}
		MsgBox_Ask("#Vxl_OutUI_My_SlotRemove", MapSel_Loc("#Vxl_OutUI_My_SlotRemove_Check", "Would you like to remove this slot?"), "ms_delete_yes");
		return true;
	case "ms_delete_yes":
		MapSel_DeleteNow();
		return true;
	}
	return false;
}
