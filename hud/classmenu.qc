/* Class menu (handoff M3): CCSClassMenu.

   SOURCE-CONFIRMED (client.dll): CCSClassMenu loads
   Resource/UI/CSO_ClassMenu.res (FUN_029e6770) or CSO_ClassMenu_ver2.res
   (FUN_029e8850). ver2 adds slotPin (resource/zombi/pin), a
   CurrentSelectedBG / CurrentSelected label (#CSO_ZI_Current_Selected)
   and looks up the CS classes militia and spetsnaz ("czero"). Which of the
   two this build shows, and the list it fills, are UNRESOLVED.

   This uses CSO_ClassMenu_ver2.res (cstrike, OBSERVED): a 606x420 frame,
   slot0..slot9 at 16,50 (200x25 each), prevBtn / nextBtn and a ClassInfo
   panel at 263,50 (400x380).

   DESIGN: the list is the CSNZ characters the player owns (userinfo
   csnz_chars, published by the rules' inventory) and picking one sends
   csnz_equip_char, which the rules apply from the next spawn. The CS
   model classes (terror, leet, urban, ...) are not listed: the rules have
   no player model choice. ClassInfo shows gfx/vgui/<name> when the build
   has a picture for that character. The title is #Cstrike_Join_Class.
   Opened after joining a team from the team menu, and by csnz_classmenu. */

hashtable g_cmRes;
bool g_cmOpen;
int g_cmPage;
int g_cmHover;
int g_cmCount;
string g_cmChars[64];

void
CM_Init(void)
{
	g_cmRes = Res_Load("resource/ui/cso_classmenu_ver2.res");
	Loc_Load("resource/cstrike_na_en.utf8.txt", "Cstrike_Join_Class");
	Loc_Load("resource/cso_na_en.utf8.txt", "CSO_ZI_Current_Selected");
	registercommand("csnz_classmenu");
}

/* csm_char_fernando -> Fernando */
string
CM_CharName(string id)
{
	string n = id;

	if (substring(n, 0, 9) == "csm_char_")
		n = substring(n, 9, -1);
	return strcat(strtoupper(substring(n, 0, 1)), substring(n, 1, -1));
}

void
CM_Open(void)
{
	int n = (int)tokenize(getplayerkeyvalue(player_localnum, "csnz_chars"));

	g_cmCount = 0i;
	for (int i = 0i; i < n && g_cmCount < 64i; i++) {
		if (g_cmChars[g_cmCount])
			strunzone(g_cmChars[g_cmCount]);
		g_cmChars[g_cmCount] = strzone(argv(i));
		g_cmCount++;
	}

	g_cmPage = 0i;
	g_cmHover = -1i;
	g_cmOpen = true;
}

void
CM_Close(void)
{
	g_cmOpen = false;
}

vector
CM_FrameSize(void)
{
	if (Res_Has(g_cmRes, "ClassMenu"))
		return Res_Size(g_cmRes, "ClassMenu");

	return [606, 420];
}

vector
CM_FrameOrigin(void)
{
	return Res_Pos(g_cmRes, "ClassMenu", screen.HUDSize() / g_hs);
}

/* the character in slot i, -1 for none */
int
CM_SlotChar(int i)
{
	int idx = g_cmPage * 10i + i;

	if (i < 0i || i > 9i || idx >= g_cmCount)
		return -1i;
	return idx;
}

bool
CM_Enabled(string ctl)
{
	switch (ctl) {
	case "prevbtn":
		return (g_cmPage > 0i);
	case "nextbtn":
		return ((g_cmPage + 1i) * 10i < g_cmCount);
	}

	if (substring(ctl, 0, 4) == "slot")
		return CM_SlotChar(stoi(substring(ctl, 4, -1))) >= 0i;
	return false;
}

void
CM_Press(int i)
{
	int c = CM_SlotChar(i);

	if (c < 0i)
		return;

	localcmd(sprintf("cmd csnz_equip_char %s\n", g_cmChars[c]));
	CM_Close();
}

void
CM_Command(string ctl)
{
	switch (ctl) {
	case "prevbtn":
		if (CM_Enabled(ctl))
			g_cmPage--;
		break;
	case "nextbtn":
		if (CM_Enabled(ctl))
			g_cmPage++;
		break;
	default:
		if (substring(ctl, 0, 4) == "slot")
			CM_Press(stoi(substring(ctl, 4, -1)));
	}
}

void
CM_Draw(void)
{
	vector fo, fs;
	string equipped;

	if (!g_cmOpen)
		return;

	fo = CM_FrameOrigin();
	fs = CM_FrameSize();
	equipped = getplayerkeyvalue(player_localnum, "csnz_char");

	Res_Frame9("resource/control/igframe/window_default_", fo, fs, 1.0f);
	draw.Text_RGB(Art_Pos(fo + [12, 8]), Loc("Cstrike_Join_Class", "Select Class"), [1,1,1], g_hudFont);

	for (int i = 0i; i < 10i; i++) {
		string ctl = sprintf("slot%i", i);
		int c = CM_SlotChar(i);
		string label = "";

		if (c >= 0i) {
			label = sprintf("%i  %s", (i + 1i) % 10i, CM_CharName(g_cmChars[c]));
			if (g_cmChars[c] == equipped)
				label = strcat(label, "  *");
		}
		Res_DrawButton(g_cmRes, fo, fs, ctl, label, c >= 0i, true);
	}

	Res_DrawButton(g_cmRes, fo, fs, "prevBtn", Res_Text(Res_Get(g_cmRes, "prevBtn", "labelText")), CM_Enabled("prevbtn"), false);
	Res_DrawButton(g_cmRes, fo, fs, "nextBtn", Res_Text(Res_Get(g_cmRes, "nextBtn", "labelText")), CM_Enabled("nextbtn"), false);

	/* the hovered (else equipped) character */
	string show = "";
	if (g_cmHover >= 0i)
		show = g_cmChars[g_cmHover];
	else
		show = equipped;
	if (show == "")
		return;

	vector p = Res_CtlPos(g_cmRes, fo, fs, "ClassInfo");
	string pic = strcat("gfx/vgui/", substring(show, 0, 9) == "csm_char_" ? substring(show, 9, -1) : show);
	vector ps = drawgetimagesize(pic);

	draw.Text_RGB(Art_Pos(p), CM_CharName(show), [1,1,1], g_hudFont);
	if (ps[0] > 0)
		drawpic(Art_Pos(p + [0, 18]), pic, ps * g_hs, [1,1,1], 1, 0);
	if (show == equipped)
		draw.Text_RGB(Art_Pos(p + [0, 18 + ps[1] + 6]),
			strcat(Loc("CSO_ZI_Current_Selected", "Current Selection: "), CM_CharName(show)), [0.8, 0.8, 0.8], g_hudFont);
}

bool
CM_Input(float evType, float key, float chr, float dev)
{
	string ctls = "slot0 slot1 slot2 slot3 slot4 slot5 slot6 slot7 slot8 slot9 prevbtn nextbtn";

	if (!g_cmOpen)
		return false;

	if (evType == IE_MOUSEABS || evType == IE_MOUSEDELTA) {
		g_resHoverCtl = Res_HitTest(g_cmRes, CM_FrameOrigin(), CM_FrameSize(), ctls);
		g_cmHover = -1i;
		if (substring(g_resHoverCtl, 0, 4) == "slot")
			g_cmHover = CM_SlotChar(stoi(substring(g_resHoverCtl, 4, -1)));
		return true;
	}

	if (evType == IE_KEYDOWN) {
		if (key == K_MOUSE1) {
			g_resPressCtl = Res_HitTest(g_cmRes, CM_FrameOrigin(), CM_FrameSize(), ctls);
			return true;
		}
		if (key == K_ESCAPE) {
			CM_Close();
			return true;
		}
		if (key >= '1' && key <= '9') {
			CM_Press((int)(key - '1'));
			return true;
		}
		if (key == '0') {
			CM_Press(9i);
			return true;
		}
		return false;
	}

	if (evType == IE_KEYUP && key == K_MOUSE1) {
		string ctl = Res_HitTest(g_cmRes, CM_FrameOrigin(), CM_FrameSize(), ctls);

		if (ctl != "" && ctl == g_resPressCtl && CM_Enabled(ctl))
			CM_Command(ctl);
		g_resPressCtl = "";
		return true;
	}

	return false;
}

bool
CM_ConsoleCommand(string cmd)
{
	tokenize_console(cmd);

	if (argv(0) != "csnz_classmenu")
		return false;

	if (g_cmOpen)
		CM_Close();
	else
		CM_Open();
	return true;
}
