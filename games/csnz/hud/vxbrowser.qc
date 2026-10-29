/* Studio block browser (VoxelGUI::Fullbrowser), opened with C in Studio
   Create (mode 38).

   SOURCE-CONFIRMED (client.dll):
   - FUN_02a21d00 is the "radio3" command (C in the default binds). In mode
     0x26 (38) it creates the Fullbrowser once (FUN_02b08680) and shows it;
     in other modes it passes radio3 on to the server.
   - FUN_02b08680, the Fullbrowser constructor: a vgui::Frame titled
     #Vxl_Fullborwser, 1015x739 (0x3f7, 0x2e3). An info button at 958,11
     (25x25, resource/voxel/info_default/over/click). BlockCostUI at
     620,25 (360x20). A "search_label" (#CSO_Search) and a "search" text
     entry. A property sheet at 10,80 (980x600) with six pages in this
     order: page_normal, page_deco, page_char, page_trigger (PageVxlList
     0..3), page_tool (PageTool), page_option (the world settings), titled
     #Vxl_Fullbrowser_Tab_Normal, _Decoration, _Character, _Trigger, _Tool,
     _Setting.
   - FUN_02b14f20 reads voxel/voxel_list (the csv) into four lists; a
     PageVxlList shows the list of its category (FUN_02b1a8a0).
   - FUN_02b0c880, PageVxlList: a grid view of 67x67 cells holding 64x64
     images, and a hidden DetailInfo box (FUN_02b076e0) at 723,23 of
     218x553 with seven rows: #Vxl_Fullbrowser_Size, _Cost, _Type,
     _Collision, _Rotate, _Signal, _Attribute.
   - FUN_02b154a0, a click on a cell: fills the info box (#Vxl_Name_<NAME>,
     #Vxl_Desc_<NAME>, size "%dx%dx%d", cost, the category's tab name, the
     SIGNAL column as _Signal_Out / _In / _InOut / _Property_None, the
     ATTRIBUTE column as _Property_*), shows it and plays
     "events/on mouse.wav".
   - FUN_02b19d50, a double click on a block: "vxl_set <id>" (hw.dll keeps
     it as the block the build guns place) and "events/select.wav".
   - FUN_02b0e000, the search: a block shows when the search text is empty
     or found in its localized name or description.
   - FUN_02b024f0: TAG 1 and 2 draw resource/voxel/newicon and upicon.
   - Tab art: resource/control/TabButton/tab_small_abled_* (the open tab)
     and tab_small_disabled_* (OBSERVED strings).

   OBSERVED: voxel_list.csv columns Id, NAME, CATEGORY, ICON, DETAIL ICON,
   BLOCK COST, SIGNAL, ATTRIBUTE, THEME, HELP QUERY, TAG; its header is
   EUC-KR. Block sizes and costs come from data/studio/blocks.txt, which the
   Studio rules read too.

   INFERRED: the grid fills the page from the left and wraps; it narrows to
   the info box's left edge while the box is shown. Collision is "Exists"
   when the block has contents (FUN_02b11f00 tests a field of the block
   type that we read as its contents).

   DESIGN: the frame art (igframe, as the other HUD menus), where the search
   box sits, the scroll wheel, the 0.4 s double click, closing with C or
   Escape, BlockCostUI showing the rules' one cost total. The Tool and World
   settings pages are in vxtools.qc. Rotate is UNRESOLVED (FUN_02b03fc0
   tests block-type fields not yet mapped), so it shows "-". */

/* vxtools.qc */
void VQ_DrawBar(void);
bool VQ_Input(float evType, float key);
bool VQ_Pick(int n);
void VT_DrawPage(vector po);
void VT_MouseDown(vector po);
void VT_MouseUp(vector po);
void VT_RightClick(vector po);
void VS_DrawPage(vector po);
void VS_Click(vector po, float dir);

#define VB_MAX 1800
#define VB_FW 1015
#define VB_FH 739
#define VB_CELL 67
#define VB_IMG 64

bool g_vbOpen;
bool g_vbLoaded;
int g_vbCount;
/* one row of voxel_list.csv; the table lives in memalloc'd memory so it
   adds no globals (arrays this size push hud.dat's string table up by
   hundreds of KB) */
typedef struct {
	float bid;
	float cat;
	float sig;
	float attr;
	float tag;
	string name;
	string icon;
} vbent_t;
vbent_t *g_vb;
hashtable g_vbDims;	/* id -> "x y z" from blocks.txt */
hashtable g_vbCost;	/* id -> cost */
hashtable g_vbSolid;	/* id -> contents */

int *g_vbShown;
int g_vbShownCount;
int g_vbTab;		/* 0..5 */
int g_vbScroll;		/* first row shown */
var int g_vbSel = -1i;	/* index into g_vb*, the block in the info box */
string g_vbSearch;
bool g_vbTyping;
float g_vbLastClick;
var int g_vbLastCell = -1i;

string
VB_StripTga(string s)
{
	if (strtolower(substring(s, strlen(s) - 4, 4)) == ".tga")
		return substring(s, 0, strlen(s) - 4);
	return s;
}

void
VB_LoadBlocks(void)
{
	filestream fh = fopen("data/studio/blocks.txt", FILE_READ);
	string s;

	g_vbDims = hash_createtab(2048, HASH_REPLACE);
	g_vbCost = hash_createtab(2048, HASH_REPLACE);
	g_vbSolid = hash_createtab(2048, HASH_REPLACE);
	if (fh < 0)
		return;
	while ((s = fgets(fh))) {
		if (substring(s, 0, 2) == "//")
			continue;
		if (tokenizebyseparator(s, "\t") < 11)
			continue;
		string key = argv(0);
		hash_add(g_vbDims, key, argv(10), EV_STRING);
		hash_add(g_vbCost, key, argv(7), EV_STRING);
		hash_add(g_vbSolid, key, argv(3), EV_STRING);
	}
	fclose(fh);
}

void VB_Load(void);

/* a block id that is in voxel_list.csv / blocks.txt */
bool
VB_BlockKnown(float bid)
{
	if (bid <= 0)
		return false;
	if (!g_vbLoaded)
		VB_Load();
	return hash_get(g_vbCost, ftos(bid), "", EV_STRING) != "";
}

void
VB_Load(void)
{
	filestream fh = fopen("voxel/voxel_list.csv", FILE_READ);
	string s;
	bool header = true;

	g_vbLoaded = true;
	g_vbCount = 0i;
	g_vb = (vbent_t *)memalloc(sizeof(vbent_t) * VB_MAX);
	g_vbShown = (int *)memalloc(sizeof(int) * VB_MAX);
	VB_LoadBlocks();
	if (fh < 0) {
		print("csnz: voxel/voxel_list.csv not found; the block browser is empty\n");
		return;
	}
	while ((s = fgets(fh))) {
		/* the header's quoted fields hold commas; skip it whole */
		if (header) {
			header = false;
			continue;
		}
		if (tokenizebyseparator(s, ",") < 11)
			continue;
		float bid = stof(argv(0));
		if (bid <= 0 || g_vbCount >= VB_MAX)
			continue;
		g_vb[g_vbCount].bid = bid;
		g_vb[g_vbCount].name = strzone(argv(1));
		g_vb[g_vbCount].cat = stof(argv(2));
		g_vb[g_vbCount].icon = strzone(VB_StripTga(strtolower(argv(3))));
		g_vb[g_vbCount].sig = stof(argv(6));
		g_vb[g_vbCount].attr = stof(argv(7));
		g_vb[g_vbCount].tag = stof(argv(10));
		g_vbCount++;
	}
	fclose(fh);
}

string
VB_LocName(int i)
{
	return Loc(strcat("Vxl_Name_", g_vb[i].name), g_vb[i].name);
}

string
VB_LocDesc(int i)
{
	return Loc(strcat("Vxl_Desc_", g_vb[i].name), "");
}

/* FUN_02b0e000 */
bool
VB_Matches(int i)
{
	if (g_vbSearch == "")
		return true;
	string q = strtolower(g_vbSearch);
	return strstrofs(strtolower(VB_LocName(i)), q) >= 0 || strstrofs(strtolower(VB_LocDesc(i)), q) >= 0;
}

void
VB_Filter(void)
{
	g_vbShownCount = 0i;
	if (g_vbTab > 3i)
		return;
	for (int i = 0i; i < g_vbCount; i++)
		if (g_vb[i].cat == g_vbTab && VB_Matches(i))
			g_vbShown[g_vbShownCount++] = i;
}

void
VB_Init(void)
{
	registercommand("radio3");
	Loc_Load("resource/cso_na_en.utf8.txt", "Vxl_Name_ Vxl_Desc_ Vxl_Fullbrowser_ Vxl_Fullborwser CSO_Search");
	Loc_Load("resource/cso_na_en.utf8.txt", "CSO_Item_Name_vxl CSO_Item_Desc_vxl Vxl_OutUI_Mode_ Vxl_AttackType_ CSO_Permanent");
	registercommand("vxl_lastinv");
	precache_pic("resource/voxel/weapon_bg_ingame");
	precache_pic("resource/voxel/weapon_effect_ingame");
	precache_pic("resource/voxel/q_slot_number_box");
	precache_pic("resource/voxel/weapon_slot_box");
	precache_pic("resource/voxel/info_default");
	precache_pic("resource/voxel/info_studio_default");
	precache_pic("resource/voxel/cost_bg_left");
	precache_pic("resource/control/tabbutton/tab_small_abled_left_n");
	precache_pic("resource/control/tabbutton/tab_small_disabled_left_n");
	precache_pic("resource/control/textentry/output_default_top_left@n");
}

void
VB_Open(void)
{
	if (!g_vbLoaded)
		VB_Load();
	g_vbOpen = true;
	g_vbTyping = false;
	VB_Filter();
}

void
VB_Close(void)
{
	g_vbOpen = false;
	g_vbTyping = false;
}

vector
VB_Origin(void)
{
	vector hs = screen.HUDSize() / g_hs;
	return [(hs[0] - VB_FW) / 2, (hs[1] - VB_FH) / 2];
}

/* the page area under the tab strip, frame-relative */
#define VB_SHEET_X 10
#define VB_SHEET_Y 80
#define VB_SHEET_W 980
#define VB_SHEET_H 600
#define VB_TAB_H 26
#define VB_TAB_W 150

vector
VB_PageOrg(vector fo)
{
	return fo + [VB_SHEET_X, VB_SHEET_Y + VB_TAB_H];
}

float
VB_GridW(void)
{
	return (g_vbSel >= 0i) ? 713 : 959;
}

int
VB_Cols(void)
{
	return (int)floor(VB_GridW() / VB_CELL);
}

int
VB_Rows(void)
{
	return (int)floor((VB_SHEET_H - VB_TAB_H - 8) / VB_CELL);
}

/* the grid cell under the mouse, as an index into g_vbShown, or -1 */
int
VB_CellAt(vector fo)
{
	vector po = VB_PageOrg(fo) + [4, 4];
	vector m = g_resMouse - po;
	int cols = VB_Cols();

	if (g_vbTab > 3i || m[0] < 0 || m[1] < 0 || m[0] >= cols * VB_CELL || m[1] >= VB_Rows() * VB_CELL)
		return -1i;
	int n = (int)floor(m[1] / VB_CELL) * cols + (int)floor(m[0] / VB_CELL) + g_vbScroll * cols;
	return (n < g_vbShownCount) ? n : -1i;
}

string
VB_TabName(int t)
{
	switch (t) {
	case 0i: return Loc("Vxl_Fullbrowser_Tab_Normal", "Basic block");
	case 1i: return Loc("Vxl_Fullbrowser_Tab_Decoration", "Decorative block");
	case 2i: return Loc("Vxl_Fullbrowser_Tab_Character", "Character block");
	case 3i: return Loc("Vxl_Fullbrowser_Tab_Trigger", "Device block");
	case 4i: return Loc("Vxl_Fullbrowser_Tab_Tool", "Tool/Weapon");
	}
	return Loc("Vxl_Fullbrowser_Tab_Setting", "World settings");
}

string
VB_SignalName(float s)
{
	switch (s) {
	case 1: return Loc("Vxl_Fullbrowser_Signal_Out", "Sent");
	case 2: return Loc("Vxl_Fullbrowser_Signal_In", "Receiving");
	case 3: return Loc("Vxl_Fullbrowser_Signal_InOut", "Sending and receiving");
	}
	return Loc("Vxl_Fullbrowser_Property_None", "Doesn't exist");
}

/* FUN_02b02640 */
string
VB_AttrName(float a)
{
	switch (a) {
	case 1: return Loc("Vxl_Fullbrowser_Property_Light", "Lighting");
	case 2: return Loc("Vxl_Fullbrowser_Property_Water", "Water");
	case 3: return Loc("Vxl_Fullbrowser_Property_Monster", "Monster");
	case 4: return Loc("Vxl_Fullbrowser_Property_Item", "Item");
	case 5: return Loc("Vxl_Fullbrowser_Property_Character", "Character");
	case 6: return Loc("Vxl_Fullbrowser_Property_Circuit", "Logical process");
	case 7: return Loc("Vxl_Fullbrowser_Property_Effect", "Special effects");
	case 8: return Loc("Vxl_Fullbrowser_Property_Slab", "Row");
	case 9: return Loc("Vxl_Fullbrowser_Property_Slope", "Slope");
	case 10: return Loc("Vxl_Fullbrowser_Property_SlopeR", "Reverse slope");
	}
	return Loc("Vxl_Fullbrowser_Property_None", "Doesn't exist");
}

void
VB_Text(vector p, string s, vector rgb)
{
	draw.Text_RGB(Art_Pos(p), s, rgb, g_hudFont);
}

/* word-wrapped text in a column of width w; returns the height used */
float
VB_Wrap(vector p, float w, string s, vector rgb)
{
	float lh = font.GetHeight(g_hudFont) / g_hs + 2;
	float y = 0;
	string line = "";
	int n = (int)tokenize(s);

	for (int i = 0i; i < n; i++) {
		tokenize(s);
		string word = argv(i);
		string tryLine = (line == "") ? word : strcat(line, " ", word);
		if (line != "" && font.StringWidth(tryLine, false, g_hudFont) / g_hs > w) {
			VB_Text(p + [0, y], line, rgb);
			y += lh;
			line = word;
		} else
			line = tryLine;
	}
	if (line != "") {
		VB_Text(p + [0, y], line, rgb);
		y += lh;
	}
	return y;
}

void
VB_DrawTabs(vector fo)
{
	vector p = fo + [VB_SHEET_X, VB_SHEET_Y];

	for (int t = 0i; t < 6i; t++) {
		vector tp = p + [t * VB_TAB_W, 0];
		vector ts = [VB_TAB_W - 2, VB_TAB_H];
		string kind = (t == g_vbTab) ? "abled" : "disabled";
		string st = Res_MouseIn(tp, ts) ? "o" : "n";
		string base = strcat("resource/control/tabbutton/tab_small_", kind, "_");

		drawpic(Art_Pos(tp), strcat(base, "left_", st), [8, ts[1]] * g_hs, [1,1,1], 1, 0);
		drawpic(Art_Pos(tp + [8, 0]), strcat(base, "center_", st), [ts[0] - 16, ts[1]] * g_hs, [1,1,1], 1, 0);
		drawpic(Art_Pos(tp + [ts[0] - 8, 0]), strcat(base, "right_", st), [8, ts[1]] * g_hs, [1,1,1], 1, 0);

		string lbl = VB_TabName(t);
		float lw = font.StringWidth(lbl, false, g_hudFont) / g_hs;
		VB_Text(tp + [(ts[0] - lw) / 2, (ts[1] - font.GetHeight(g_hudFont) / g_hs) / 2], lbl,
			(t == g_vbTab) ? [1,1,1] : [0.7,0.7,0.7]);
	}
}

void
VB_DrawInfo(vector fo)
{
	if (g_vbSel < 0i)
		return;

	vector po = fo + [VB_SHEET_X, VB_SHEET_Y + VB_TAB_H];
	vector bp = po + [723, 23 - VB_TAB_H];
	vector bs = [218, 553];
	int i = g_vbSel;
	string key = ftos(g_vb[i].bid);

	drawfill(Art_Pos(bp), bs * g_hs, [0, 0, 0], 0.55, 0);

	/* the block's image, then its name */
	drawpic(Art_Pos(bp + [(bs[0] - VB_IMG) / 2, 10]), g_vb[i].icon, [VB_IMG, VB_IMG] * g_hs, [1,1,1], 1, 0);
	float y = 10 + VB_IMG + 8;
	y += VB_Wrap(bp + [8, y], bs[0] - 16, VB_LocName(i), [1, 0.85, 0.5]);
	y += 6;

	string dims = hash_get(g_vbDims, key, "1 1 1", EV_STRING);
	tokenize(dims);
	string vals[7];
	vals[0] = sprintf("%sx%sx%s", argv(0), argv(1), argv(2));
	vals[1] = hash_get(g_vbCost, key, "-", EV_STRING);
	vals[2] = VB_TabName((int)g_vb[i].cat);
	vals[3] = (stof(hash_get(g_vbSolid, key, "0", EV_STRING)) != 0) ?
		Loc("Vxl_Fullbrowser_Property_Yes", "Exists") : Loc("Vxl_Fullbrowser_Property_None", "Doesn't exist");
	vals[4] = "-";
	vals[5] = VB_SignalName(g_vb[i].sig);
	vals[6] = VB_AttrName(g_vb[i].attr);
	string names = "Size Cost Type Collision Rotate Signal Attribute";
	string fallbacks[7] = {"Size", "Cost", "Type", "Collusion", "Rotate", "Signal", "Properties"};
	float lh = font.GetHeight(g_hudFont) / g_hs + 4;

	for (int r = 0i; r < 7i; r++) {
		tokenize(names);
		string lbl = Loc(strcat("Vxl_Fullbrowser_", argv(r)), fallbacks[r]);
		VB_Text(bp + [8, y], lbl, [0.65, 0.65, 0.65]);
		VB_Text(bp + [80, y], vals[r], [1, 1, 1]);
		y += lh;
	}
	y += 6;
	/* the descriptions break lines with a literal \n */
	string desc = strreplace("\\n", "\n", VB_LocDesc(i));
	int np = (int)tokenizebyseparator(desc, "\n");
	string para[8];
	for (int k = 0i; k < np && k < 8i; k++)
		para[k] = argv(k);
	for (int k = 0i; k < np && k < 8i; k++)
		y += (para[k] == "") ? lh / 2 : VB_Wrap(bp + [8, y], bs[0] - 16, para[k], [0.85, 0.85, 0.85]);
}

void
VB_DrawGrid(vector fo)
{
	vector po = VB_PageOrg(fo) + [4, 4];
	int cols = VB_Cols();
	int rows = VB_Rows();
	int first = g_vbScroll * cols;
	int hover = VB_CellAt(fo);

	for (int n = first; n < g_vbShownCount && n < first + cols * rows; n++) {
		int k = n - first;
		int i = g_vbShown[n];
		vector cp = po + [(k % cols) * VB_CELL, floor(k / cols) * VB_CELL];
		vector ip = cp + [1, 1];

		drawfill(Art_Pos(cp), [VB_CELL - 2, VB_CELL - 2] * g_hs, [0.12, 0.12, 0.12], 0.8, 0);
		drawpic(Art_Pos(ip), g_vb[i].icon, [VB_IMG, VB_IMG] * g_hs, [1,1,1], 1, 0);
		if (g_vb[i].tag == 1)
			drawpic(Art_Pos(ip), "resource/voxel/newicon", drawgetimagesize("resource/voxel/newicon") * g_hs, [1,1,1], 1, 0);
		else if (g_vb[i].tag == 2)
			drawpic(Art_Pos(ip), "resource/voxel/upicon", drawgetimagesize("resource/voxel/upicon") * g_hs, [1,1,1], 1, 0);
		if (i == g_vbSel || n == hover)
			drawpic(Art_Pos(ip), "voxel/icon/icon_select", [VB_IMG, VB_IMG] * g_hs, [1,1,1], (i == g_vbSel) ? 1 : 0.6, 0);
	}

	/* where the list is, when it does not fit */
	int totalRows = (int)ceil(g_vbShownCount / cols);
	if (totalRows > rows) {
		vector sp = po + [cols * VB_CELL + 2, 0];
		float sh = rows * VB_CELL;
		drawfill(Art_Pos(sp), [4, sh] * g_hs, [0.3, 0.3, 0.3], 0.6, 0);
		drawfill(Art_Pos(sp + [0, sh * g_vbScroll / totalRows]), [4, sh * rows / totalRows] * g_hs, [0.9, 0.75, 0.4], 0.9, 0);
	}
}

void
VB_DrawCost(vector fo)
{
	vector p = fo + [620, 25];
	string lbl = Loc("Vxl_Fullbrowser_Cost_Label", "Block cost");
	float lw = font.StringWidth(lbl, false, g_hudFont) / g_hs;
	vector l = drawgetimagesize("resource/voxel/cost_bg_left");
	vector r = drawgetimagesize("resource/voxel/cost_bg_right");
	vector bp = p + [lw + 8, 0];
	float bw = 360 - lw - 8;

	VB_Text(p + [0, (20 - font.GetHeight(g_hudFont) / g_hs) / 2], lbl, [1,1,1]);
	drawpic(Art_Pos(bp), "resource/voxel/cost_bg_left", [l[0], 20] * g_hs, [1,1,1], 1, 0);
	drawpic(Art_Pos(bp + [l[0], 0]), "resource/voxel/cost_bg_center", [bw - l[0] - r[0], 20] * g_hs, [1,1,1], 1, 0);
	drawpic(Art_Pos(bp + [bw - r[0], 0]), "resource/voxel/cost_bg_right", [r[0], 20] * g_hs, [1,1,1], 1, 0);
	string v = sprintf("%s / %s", serverkey("vx_cost"), serverkey("vx_costlimit"));
	VB_Text(bp + [10, (20 - font.GetHeight(g_hudFont) / g_hs) / 2], v, [1,1,1]);
}

vector
VB_SearchPos(vector fo)
{
	return fo + [700, 52];
}

void
VB_DrawSearch(vector fo)
{
	vector sp = VB_SearchPos(fo);
	vector ss = [280, 22];
	string lbl = Loc("CSO_Search", "Search");
	float lw = font.StringWidth(lbl, false, g_hudFont) / g_hs;
	float th = font.GetHeight(g_hudFont) / g_hs;

	VB_Text(sp - [lw + 8, -(ss[1] - th) / 2], lbl, [1,1,1]);
	Res_Frame9("resource/control/textentry/output_default_%s@n", sp, ss, 1);
	string txt = g_vbSearch;
	if (g_vbTyping && mod(time, 1) < 0.5)
		txt = strcat(txt, "_");
	VB_Text(sp + [6, (ss[1] - th) / 2], txt, [1,1,1]);
}

void
VB_Draw(void)
{
	VQ_DrawBar();
	if (!g_vbOpen)
		return;

	vector fo = VB_Origin();

	Res_Frame9("resource/control/igframe/window_default_", fo, [VB_FW, VB_FH], 1.0f);
	VB_Text(fo + [12, 8], Loc("Vxl_Fullborwser", "Studio browser"), [1,1,1]);

	string ist = Res_MouseIn(fo + [958, 11], [25, 25]) ? "over" : "default";
	drawpic(Art_Pos(fo + [958, 11]), strcat("resource/voxel/info_", ist), [25, 25] * g_hs, [1,1,1], 1, 0);

	VB_DrawCost(fo);
	VB_DrawSearch(fo);
	VB_DrawTabs(fo);

	vector po = VB_PageOrg(fo);
	drawfill(Art_Pos(po), [VB_SHEET_W, VB_SHEET_H - VB_TAB_H] * g_hs, [0, 0, 0], 0.6, 0);

	if (g_vbTab <= 3i) {
		VB_DrawGrid(fo);
		VB_DrawInfo(fo);
	} else if (g_vbTab == 4i) {
		VT_DrawPage(po);
	} else {
		VS_DrawPage(po);
	}
}

void
VB_SetTab(int t)
{
	g_vbTab = t;
	g_vbScroll = 0i;
	g_vbSel = -1i;
	VB_Filter();
}

void
VB_Scroll(int d)
{
	int cols = VB_Cols();
	int totalRows = (int)ceil(g_vbShownCount / cols);
	int top = max(0i, totalRows - VB_Rows());

	g_vbScroll = (int)bound(0i, g_vbScroll + d, top);
}

void
VB_Click(vector fo)
{
	/* tabs */
	for (int t = 0i; t < 6i; t++)
		if (Res_MouseIn(fo + [VB_SHEET_X + t * VB_TAB_W, VB_SHEET_Y], [VB_TAB_W - 2, VB_TAB_H])) {
			VB_SetTab(t);
			return;
		}

	g_vbTyping = Res_MouseIn(VB_SearchPos(fo), [280, 22]);
	if (g_vbTyping)
		return;

	if (g_vbTab == 4i) {
		VT_MouseDown(VB_PageOrg(fo));
		return;
	}
	if (g_vbTab == 5i) {
		VS_Click(VB_PageOrg(fo), 1);
		return;
	}

	int n = VB_CellAt(fo);
	if (n < 0i)
		return;

	int i = g_vbShown[n];
	if (n == g_vbLastCell && time - g_vbLastClick < 0.4) {
		/* FUN_02b19d50 */
		localcmd(sprintf("cmd vxl_set %d\n", g_vb[i].bid));
		localsound("events/select.wav");
		g_vbLastCell = -1i;
		return;
	}
	g_vbLastCell = n;
	g_vbLastClick = time;

	/* FUN_02b154a0 */
	g_vbSel = i;
	localsound("events/on mouse.wav");
	/* the grid narrows for the info box; keep the clicked block in view */
	int row = (int)floor(n / VB_Cols());
	if (row < g_vbScroll || row >= g_vbScroll + VB_Rows())
		g_vbScroll = row - (int)floor(VB_Rows() / 2);
	VB_Scroll(0i);
}

bool
VB_Input(float evType, float key, float chr, float dev)
{
	if (!g_vbOpen)
		return VQ_Input(evType, key);

	if (evType == IE_MOUSEABS || evType == IE_MOUSEDELTA)
		return true;

	if (evType == IE_KEYDOWN) {
		if (key == K_ESCAPE) {
			VB_Close();
			return true;
		}
		if (key == K_MOUSE1) {
			VB_Click(VB_Origin());
			return true;
		}
		if (key == K_MOUSE2) {
			if (g_vbTab == 4i)
				VT_RightClick(VB_PageOrg(VB_Origin()));
			else if (g_vbTab == 5i)
				VS_Click(VB_PageOrg(VB_Origin()), -1);
			return true;
		}
		if (key == K_MWHEELUP) {
			VB_Scroll(-1i);
			return true;
		}
		if (key == K_MWHEELDOWN) {
			VB_Scroll(1i);
			return true;
		}
		if (g_vbTyping) {
			if (key == K_BACKSPACE) {
				if (g_vbSearch != "")
					g_vbSearch = strzone(substring(g_vbSearch, 0, strlen(g_vbSearch) - 1));
			} else if (key == K_ENTER) {
				g_vbTyping = false;
			} else if (chr >= 32 && chr < 127) {
				g_vbSearch = strzone(strcat(g_vbSearch, chr2str(chr)));
			} else
				return true;
			g_vbScroll = 0i;
			g_vbSel = -1i;
			VB_Filter();
			return true;
		}
		/* C again closes it */
		if (key == 'c')
			VB_Close();
		return true;
	}

	if (evType == IE_KEYUP && key == K_MOUSE1 && g_vbTab == 4i)
		VT_MouseUp(VB_PageOrg(VB_Origin()));
	return evType == IE_KEYUP;
}

bool
VB_ConsoleCommand(string cmd)
{
	tokenize_console(cmd);

	/* FUN_02a21e90: the slot keys pick quick slots in mode 38 */
	if (HUD_ModeId() == 38) {
		string c = argv(0);
		if (substring(c, 0, 4) == "slot" && strlen(c) == 5) {
			int n = stoi(substring(c, 4, 1)) - 1i;
			if (n >= 0i && n < 5i) {
				VQ_Pick(n);
				return true;
			}
		}
		if (c == "vxl_lastinv") {
			localcmd("cmd vxl_lastinv\n");
			return true;
		}
	}

	if (argv(0) != "radio3")
		return false;

	if (HUD_ModeId() != 38) {
		localcmd("cmd radio3\n");
		return true;
	}
	if (g_vbOpen)
		VB_Close();
	else
		VB_Open();
	return true;
}
