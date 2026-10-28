/* Partner spawn-point picker (handoff M3): CIGSelectSpawnPointMenu.

   SOURCE-CONFIRMED (client.dll):
   - FUN_0296f8b0, the constructor: a 961x672 frame. A 882x512 minimap
     panel at 38,80 drawn with resource/Control/TextEntry/Output_default_%s@n.
     The title #CSO_Partner_SelectSpawnPoint at 37,13 (882x18, centred).
     The countdown label at 521,520 (386x41, right-aligned) and
     #CSO_Partner_RemainTime_SelectSpawnPoint under it at 521,569 (386x13,
     right-aligned). #CSO_Partner_RemainTime_SelectSpawnPoint_Desc at
     37,614 (715x13, centred). The #CSO_Partner_SelectSpawnPoint_Confirm
     button at 794,606 (130x30) with the command "Close".
   - FUN_02972010 reads maps/<map>_partner.json, the local team's array
     ("TR" for team 1, else "CT"). Each entry is one floor, with a tab
     ("spawnpoint_floor_tab", 110x25 tabs at 37,55) named floorName, and
     floorImage drawn in the minimap panel. Each spawnPos [x, y] is a 39x39
     point image (resource/Partner/tr_point_n or ct_point_n) at x, y in the
     panel, with tr_dot or ct_dot at x+17, y-10. Each bombTargets [x, y] is
     resource/Partner/c4 at x, y. Points are numbered 0, 1, 2... across all
     floors in order.
   - FUN_02972e60 / FUN_02972f20: the cursor over a point not yet picked
     shows the _c image. FUN_02972c30: clicking a point not yet picked
     marks it (the _c image and the team colour, a1 27 30 for team 1, else
     39 50 ff), sends "ptn_spawn <n>" and un-marks the previous pick.
   - FUN_02971d80 starts the countdown at 15 seconds; FUN_029733b0 counts
     it down by the frame time and writes it as "%d" (the format string's
     bytes are INFERRED).

   DESIGN: the menu frame is centred and drawn with window_default art.
   The countdown is drawn with the HUD's big white digits, because the
   ResultBig font isn't loaded. The floor tabs use tab_small_abled art.
   The three name lines over each point and the 39x39 label on it are not
   drawn, because their text is unknown. The menu opens by itself when
   the rules publish a picker end (csnz_ptn_end) and the map has a
   _partner.json. It closes at that end, on Confirm, or on Esc.
   "ptn_spawnmenu" toggles it by hand. */

#define PTN_MAXPTS 32
#define PTN_MAXFLOORS 4

bool g_ptnOpen;
bool g_ptnAuto;		/* opened for the rules' picker, so it closes at its end */
float g_ptnShownEnd;		/* the csnz_ptn_end the menu last opened for */
int g_ptnFloors;
int g_ptnFloor;
string g_ptnFloorName[PTN_MAXFLOORS];
string g_ptnFloorImage[PTN_MAXFLOORS];
int g_ptnCount;
int g_ptnPtFloor[PTN_MAXPTS];
vector g_ptnPt[PTN_MAXPTS];
int g_ptnBombs;
int g_ptnBombFloor[PTN_MAXPTS];
vector g_ptnBomb[PTN_MAXPTS];
var int g_ptnPicked = -1i;
var int g_ptnHover = -1i;
string g_ptnTeam;		/* "tr" or "ct": the art prefix */

void
PTN_Init(void)
{
	Loc_Load("resource/cso_na_en.utf8.txt", "CSO_Partner_SelectSpawnPoint CSO_Partner_RemainTime_SelectSpawnPoint");
	registercommand("ptn_spawnmenu");
}

string
PTN_MapName(void)
{
	string map = mapname;

	if (substring(map, 0, 5) == "maps/")
		map = substring(map, 5, -1);
	if (substring(map, strlen(map) - 4, 4) == ".bsp")
		map = substring(map, 0, strlen(map) - 4);
	return map;
}

/* the local team's floors and points from maps/<map>_partner.json */
bool
PTN_Load(void)
{
	filestream fh = fopen(sprintf("maps/%s_partner.json", PTN_MapName()), FILE_READ);
	string txt = "", line;
	float team = stof(getplayerkeyvalue(player_localnum, "*team"));

	if (fh < 0)
		return false;
	while ((line = fgets(fh)))
		txt = strcat(txt, line, "\n");
	fclose(fh);

	jsonnode root = json_parse(txt);
	if (!root)
		return false;

	g_ptnTeam = (team == 1) ? "tr" : "ct";
	jsonnode floors = json_find_object_child(root, (team == 1) ? "TR" : "CT");
	g_ptnFloors = g_ptnCount = g_ptnBombs = 0i;

	for (int f = 0i; floors && f < json_get_length(floors) && f < PTN_MAXFLOORS; f++) {
		jsonnode fl = json_get_child_at_index(floors, f);
		jsonnode pos = json_find_object_child(fl, "spawnPos");
		jsonnode bombs = json_find_object_child(fl, "bombTargets");

		if (g_ptnFloorName[f])
			strunzone(g_ptnFloorName[f]);
		if (g_ptnFloorImage[f])
			strunzone(g_ptnFloorImage[f]);
		g_ptnFloorName[f] = strzone(Res_Text(json_get_string(json_find_object_child(fl, "floorName"))));
		g_ptnFloorImage[f] = strzone(strtolower(json_get_string(json_find_object_child(fl, "floorImage"))));
		precache_pic(g_ptnFloorImage[f]);
		g_ptnFloors++;

		for (int i = 0i; pos && i < json_get_length(pos) && g_ptnCount < PTN_MAXPTS; i++) {
			jsonnode xy = json_get_child_at_index(pos, i);
			g_ptnPt[g_ptnCount] = [json_get_float(json_get_child_at_index(xy, 0i)), json_get_float(json_get_child_at_index(xy, 1i))];
			g_ptnPtFloor[g_ptnCount] = f;
			g_ptnCount++;
		}
		for (int i = 0i; bombs && i < json_get_length(bombs) && g_ptnBombs < PTN_MAXPTS; i++) {
			jsonnode xy = json_get_child_at_index(bombs, i);
			g_ptnBomb[g_ptnBombs] = [json_get_float(json_get_child_at_index(xy, 0i)), json_get_float(json_get_child_at_index(xy, 1i))];
			g_ptnBombFloor[g_ptnBombs] = f;
			g_ptnBombs++;
		}
	}

	json_free(root);
	return (g_ptnCount > 0i);
}

void
PTN_Open(void)
{
	if (!PTN_Load())
		return;
	g_ptnFloor = 0i;
	g_ptnPicked = -1i;
	g_ptnHover = -1i;
	g_ptnOpen = true;
	g_ptnAuto = false;
}

void
PTN_Close(void)
{
	g_ptnOpen = false;
}

vector
PTN_FrameOrigin(void)
{
	return (screen.HUDSize() / g_hs - [961, 672]) / 2;
}

/* the minimap panel's origin */
vector
PTN_MapOrigin(void)
{
	return PTN_FrameOrigin() + [38, 80];
}

vector
PTN_TabPos(int f)
{
	return PTN_FrameOrigin() + [37 + f * 112, 55];
}

/* the point under the mouse on the shown floor, -1 for none */
int
PTN_PointAt(void)
{
	vector mo = PTN_MapOrigin();

	for (int i = 0i; i < g_ptnCount; i++)
		if (g_ptnPtFloor[i] == g_ptnFloor && Res_MouseIn(mo + g_ptnPt[i], [39, 39]))
			return i;
	return -1i;
}

/* the picker's time left, from the rules' csnz_ptn_end */
float
PTN_TimeLeft(void)
{
	return max(0, serverkeyfloat("csnz_ptn_end") - time);
}

/* opens by itself when the rules start a picker (DESIGN) */
void
PTN_Think(void)
{
	float end = serverkeyfloat("csnz_ptn_end");
	float team = stof(getplayerkeyvalue(player_localnum, "*team"));

	if (end > time && end != g_ptnShownEnd && (team == 1 || team == 2) && player.GetHealth() > 0) {
		g_ptnShownEnd = end;
		PTN_Open();
		g_ptnAuto = true;
	}
	if (g_ptnOpen && g_ptnAuto && end <= time)
		PTN_Close();
}

void
PTN_Tab(vector p, vector s, string state, string label, bool on)
{
	string base = "resource/control/tabbutton/tab_small_abled_";
	vector ls = drawgetimagesize(strcat(base, "left_", state));
	vector rs = drawgetimagesize(strcat(base, "right_", state));

	drawpic(Art_Pos(p), strcat(base, "left_", state), [ls[0], s[1]] * g_hs, [1,1,1], 1, 0);
	drawpic(Art_Pos(p + [ls[0], 0]), strcat(base, "center_", state), [s[0] - ls[0] - rs[0], s[1]] * g_hs, [1,1,1], 1, 0);
	drawpic(Art_Pos(p + [s[0] - rs[0], 0]), strcat(base, "right_", state), [rs[0], s[1]] * g_hs, [1,1,1], 1, 0);

	float w = font.StringWidth(label, false, g_hudFont) / g_hs;
	draw.Text_RGB(Art_Pos(p + [(s[0] - w) / 2, (s[1] - font.GetHeight(g_hudFont) / g_hs) / 2]), label,
		on ? [1,1,1] : [0.7,0.7,0.7], g_hudFont);
}

/* text in a box: align 4 centred, 5 right (VGUI2's a_center, a_east) */
void
PTN_Label(vector p, vector s, string txt, int align, vector rgb)
{
	float w = font.StringWidth(txt, false, g_hudFont) / g_hs;

	if (align == 4i)
		p[0] += (s[0] - w) / 2;
	else if (align == 5i)
		p[0] += s[0] - w;
	draw.Text_RGB(Art_Pos(p + [0, (s[1] - font.GetHeight(g_hudFont) / g_hs) / 2]), txt, rgb, g_hudFont);
}

void
PTN_Draw(void)
{
	vector fo, mo;

	PTN_Think();
	if (!g_ptnOpen)
		return;

	fo = PTN_FrameOrigin();
	mo = PTN_MapOrigin();

	Res_Frame9("resource/control/igframe/window_default_", fo, [961, 672], 1.0f);
	PTN_Label(fo + [37, 13], [882, 18], Loc("CSO_Partner_SelectSpawnPoint", "Select Spawn Location"), 4i, [1,1,1]);

	/* the minimap panel and the shown floor */
	Res_Frame9("resource/control/textentry/output_default_%s@n", mo, [882, 512], 1.0f);
	string img = g_ptnFloorImage[g_ptnFloor];
	vector is = drawgetimagesize(img);
	if (is[0] > 0)
		drawpic(Art_Pos(mo), img, is * g_hs, [1,1,1], 1, 0);

	for (int f = 0i; f < g_ptnFloors; f++) {
		vector tp = PTN_TabPos(f);
		string st = (f == g_ptnFloor) ? "c" : (Res_MouseIn(tp, [110, 25]) ? "o" : "n");
		PTN_Tab(tp, [110, 25], st, g_ptnFloorName[f], f == g_ptnFloor);
	}

	for (int i = 0i; i < g_ptnBombs; i++) {
		if (g_ptnBombFloor[i] != g_ptnFloor)
			continue;
		vector bs = drawgetimagesize("resource/partner/c4");
		drawpic(Art_Pos(mo + g_ptnBomb[i]), "resource/partner/c4", bs * g_hs, [1,1,1], 1, 0);
	}

	for (int i = 0i; i < g_ptnCount; i++) {
		if (g_ptnPtFloor[i] != g_ptnFloor)
			continue;
		bool lit = (i == g_ptnPicked || i == g_ptnHover);
		string pt = sprintf("resource/partner/%s_point_%s", g_ptnTeam, lit ? "c" : "n");
		string dot = sprintf("resource/partner/%s_dot", g_ptnTeam);
		vector ds = drawgetimagesize(dot);

		drawpic(Art_Pos(mo + g_ptnPt[i]), pt, [39, 39] * g_hs, [1,1,1], 1, 0);
		drawpic(Art_Pos(mo + g_ptnPt[i] + [17, -10]), dot, ds * g_hs, [1,1,1], 1, 0);
	}

	/* the countdown, its caption and the description */
	string secs = ftos(floor(PTN_TimeLeft()));
	Art_NumR(fo + [521 + 386, 520 + (41 - g_numH[NUM_BIGWHITE]) / 2], NUM_BIGWHITE, secs, [1,1,1], 1.0f);
	PTN_Label(fo + [521, 569], [386, 13], Loc("CSO_Partner_RemainTime_SelectSpawnPoint", "Remaining Time for Spawn Selection"), 5i, [1,1,1]);
	PTN_Label(fo + [37, 614], [715, 13], Loc("CSO_Partner_RemainTime_SelectSpawnPoint_Desc", ""), 4i, [0.8,0.8,0.8]);

	/* Confirm: a plain button, so it is drawn the way the res buttons are */
	vector cp = fo + [794, 606];
	string cs = Res_MouseIn(cp, [130, 30]) ? (g_resPressCtl == "ptn_confirm" ? "c" : "o") : "n";
	Res_Button3("resource/control/button/btn_default", cs, cp, [130, 30], 1.0f);
	PTN_Label(cp, [130, 30], Loc("CSO_Partner_SelectSpawnPoint_Confirm", "Selected"), 4i, [1,1,1]);
}

void
PTN_Pick(int i)
{
	if (i < 0i || i == g_ptnPicked)
		return;
	g_ptnPicked = i;
	localcmd(sprintf("cmd ptn_spawn %i\n", i));
}

bool
PTN_Input(float evType, float key, float chr, float dev)
{
	if (!g_ptnOpen)
		return false;

	if (evType == IE_MOUSEABS || evType == IE_MOUSEDELTA) {
		g_ptnHover = PTN_PointAt();
		return true;
	}

	if (evType == IE_KEYDOWN) {
		if (key == K_MOUSE1) {
			g_resPressCtl = Res_MouseIn(PTN_FrameOrigin() + [794, 606], [130, 30]) ? "ptn_confirm" : "";
			return true;
		}
		if (key == K_ESCAPE) {
			PTN_Close();
			return true;
		}
		return false;
	}

	if (evType == IE_KEYUP && key == K_MOUSE1) {
		if (g_resPressCtl == "ptn_confirm" && Res_MouseIn(PTN_FrameOrigin() + [794, 606], [130, 30])) {
			PTN_Close();
		} else {
			for (int f = 0i; f < g_ptnFloors; f++)
				if (Res_MouseIn(PTN_TabPos(f), [110, 25]))
					g_ptnFloor = f;
			PTN_Pick(PTN_PointAt());
		}
		g_resPressCtl = "";
		return true;
	}

	return false;
}

bool
PTN_ConsoleCommand(string cmd)
{
	tokenize_console(cmd);

	if (argv(0) != "ptn_spawnmenu")
		return false;

	if (g_ptnOpen)
		PTN_Close();
	else
		PTN_Open();
	return true;
}
