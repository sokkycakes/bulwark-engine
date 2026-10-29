/* Studio "Save map" dialog and the save notices, in Studio Create (mode 38).

   SOURCE-CONFIRMED (GameUI.dll strings; the layout is
   resource/voxel/popup_voxelsave.res):
   - The in-game menu's VoxelSaveButton (resource/taskbar/btn_save) sends
     "VoxelSave", which opens CVoxelSaveDlg: a 382x164 frame titled
     #CSO_VoxelSave_Title, SaveHelp_Label (0,60 382x32, centred,
     #Vxl_OutUI_Save_OK "This will overwrite the original file. Save?"),
     Button1 (87,110 98x32, #CSO_OKl_Btn, the default, command VoxelSave)
     and Button2 (189,110, #CSO_Cancel, command Close).
   - #Vxl_OutUI_Save_On_Test sits with the dialog's strings: no saving
     while a test runs. #Vxl_Draft_Upload_OK / _Failed / _FailedSaveWorld
     are the upload's outcomes.
   - hw.dll FUN_02150c60: #Vxl_SaveAlarm shows for 5 s (0x40a00000) on the
     creator's screen, every 300 s while there are unsaved changes.

   DESIGN: the menu progs can't see the game, so its Save button runs
   "vxl_savedlg" and this HUD draws the dialog. While a test runs the dialog
   shows the On_Test text and only Cancel. The rules' answers come as
   serverinfo vx_saveres and vx_alarm (vx_maps.qc); a notice is drawn under
   the round messages for 5 s, for the creator only. Saving runs
   "cmd vx save", which writes the room's map file. */

bool g_vsvOpen;
bool g_vsvTest;			/* opened while a test runs */
hashtable g_vsvRes;
string g_vsvNotice;
float g_vsvNoticeEnd;
var float g_vsvAlarmSeen = -1;
var float g_vsvResSeen = -1;

#define VSV_CTLS "button1 button2"

void
VSV_Init(void)
{
	Loc_Load("resource/cso_na_en.utf8.txt", "CSO_VoxelSave_Title Vxl_OutUI_Save_ Vxl_Draft_Upload Vxl_SaveAlarm CSO_OKl_Btn CSO_Cancel");
	g_vsvRes = Res_Load("resource/voxel/popup_voxelsave.res");
	registercommand("vxl_savedlg");
}

bool
VSV_IsOwner(void)
{
	return serverkey("vx_owner") == getplayerkeyvalue(player_localnum, "name");
}

vector
VSV_FrameSize(void)
{
	if (Res_Has(g_vsvRes, "VoxelSaveDlg"))
		return Res_Size(g_vsvRes, "VoxelSaveDlg");
	return [382, 164];
}

vector
VSV_FrameOrigin(void)
{
	vector hs = screen.HUDSize() / g_hs;
	return (hs - VSV_FrameSize()) / 2;
}

void
VSV_Open(void)
{
	if (HUD_ModeId() != 38)
		return;
	g_vsvTest = serverkey("vx_testing") == "1";
	g_vsvOpen = true;
	g_resPressCtl = "";
}

void
VSV_Close(void)
{
	g_vsvOpen = false;
}

bool
VSV_Enabled(string ctl)
{
	return ctl != "button1" || !g_vsvTest;
}

void
VSV_Command(string ctl)
{
	if (ctl == "button1" && !g_vsvTest)
		localcmd("cmd vx save\n");
	VSV_Close();
}

void
VSV_Notice(string token, string fallback)
{
	g_vsvNotice = Loc(token, fallback);
	g_vsvNoticeEnd = time + 5.0f;
}

/* the rules' answers; the first look only records where the counts are,
   so joining a room shows nothing old */
void
VSV_Watch(void)
{
	float alarm = serverkeyfloat("vx_alarm");
	tokenize(serverkey("vx_saveres"));
	float res = stof(argv(0));
	string code = argv(1);

	if (g_vsvAlarmSeen < 0 || g_vsvResSeen < 0) {
		g_vsvAlarmSeen = alarm;
		g_vsvResSeen = res;
		return;
	}
	if (!VSV_IsOwner()) {
		g_vsvAlarmSeen = alarm;
		g_vsvResSeen = res;
		return;
	}
	if (alarm != g_vsvAlarmSeen) {
		g_vsvAlarmSeen = alarm;
		VSV_Notice("Vxl_SaveAlarm", "There are recent changes in the map. Please save!");
	}
	if (res != g_vsvResSeen) {
		g_vsvResSeen = res;
		if (code == "ok")
			VSV_Notice("Vxl_Draft_Upload_OK", "Saved!");
		else if (code == "test")
			VSV_Notice("Vxl_OutUI_Save_On_Test", "You can't save while testing the script");
		else
			VSV_Notice("Vxl_Draft_Upload_FailedSaveWorld", "Failed to save studio world.");
	}
}

void
VSV_Draw(void)
{
	if (HUD_ModeId() != 38) {
		g_vsvOpen = false;
		return;
	}
	VSV_Watch();

	if (time < g_vsvNoticeEnd && g_vsvNotice != "") {
		vector np = Art_Pos([Board_CX(), 124]);
		float w = font.StringWidth(g_vsvNotice, false, g_hudFont);
		draw.Text_RGB(np - [w / 2, 0], g_vsvNotice, [1, 0.85, 0.3], g_hudFont);
	}

	if (!g_vsvOpen)
		return;

	vector fo = VSV_FrameOrigin();
	vector fs = VSV_FrameSize();

	Res_Frame9("resource/control/igframe/window_default_", fo, fs, 1.0f);
	draw.Text_RGB(Art_Pos(fo + [12, 8]), Loc("CSO_VoxelSave_Title", "Save map"), [1,1,1], g_hudFont);

	string help = g_vsvTest ? Loc("Vxl_OutUI_Save_On_Test", "You can't save while testing the script")
		: Res_Text(Res_Get(g_vsvRes, "SaveHelp_Label", "labelText"));
	if (help == "")
		help = Loc("Vxl_OutUI_Save_OK", "This will overwrite the original file. Save?");
	vector hp = Res_CtlPos(g_vsvRes, fo, fs, "SaveHelp_Label");
	vector hsz = Res_Size(g_vsvRes, "SaveHelp_Label");
	float tw = font.StringWidth(help, false, g_hudFont) / g_hs;
	float th = font.GetHeight(g_hudFont) / g_hs;
	draw.Text_RGB(Art_Pos(hp + [(hsz[0] - tw) / 2, (hsz[1] - th) / 2]), help, [0.9, 0.9, 0.9], g_hudFont);

	Res_DrawButton(g_vsvRes, fo, fs, "button1", Res_Text(Res_Get(g_vsvRes, "Button1", "labelText")), VSV_Enabled("button1"), false);
	Res_DrawButton(g_vsvRes, fo, fs, "button2", Res_Text(Res_Get(g_vsvRes, "Button2", "labelText")), true, false);
}

bool
VSV_Input(float evType, float key, float chr, float dev)
{
	if (!g_vsvOpen)
		return false;

	if (evType == IE_MOUSEABS || evType == IE_MOUSEDELTA) {
		g_resHoverCtl = Res_HitTest(g_vsvRes, VSV_FrameOrigin(), VSV_FrameSize(), VSV_CTLS);
		return true;
	}

	if (evType == IE_KEYDOWN) {
		if (key == K_MOUSE1) {
			g_resPressCtl = Res_HitTest(g_vsvRes, VSV_FrameOrigin(), VSV_FrameSize(), VSV_CTLS);
			return true;
		}
		if (key == K_ESCAPE) {
			VSV_Close();
			return true;
		}
		/* Button1 is the layout's Default button */
		if (key == K_ENTER) {
			VSV_Command(g_vsvTest ? "button2" : "button1");
			return true;
		}
		return true;
	}

	if (evType == IE_KEYUP && key == K_MOUSE1) {
		string ctl = Res_HitTest(g_vsvRes, VSV_FrameOrigin(), VSV_FrameSize(), VSV_CTLS);

		if (ctl != "" && ctl == g_resPressCtl && VSV_Enabled(ctl))
			VSV_Command(ctl);
		g_resPressCtl = "";
		return true;
	}

	return evType == IE_KEYUP;
}

bool
VSV_ConsoleCommand(string cmd)
{
	tokenize_console(cmd);
	if (argv(0) != "vxl_savedlg")
		return false;
	if (g_vsvOpen)
		VSV_Close();
	else
		VSV_Open();
	return true;
}
