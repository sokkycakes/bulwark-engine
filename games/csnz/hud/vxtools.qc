/* Studio Create: the in-game quick slot bar, and the Fullbrowser's Tool and
   World settings pages (vxbrowser.qc draws the frame and the block pages).

   SOURCE-CONFIRMED (client.dll):
   - Five weapon quick slots (DAT_03584c5c = 5). Without a valid .vxlqs
     registry value they hold 300 335 301 334 (DAT_031744cc: long gun, short
     gun, connector, spinner) and the fifth is empty (FUN_02b14530).
   - In mode 38 the slot1..slot5 keys go to FUN_02b1a110: "vxl_wpn <n-1>
     <id>", then the CurrentWeaponQuickSlot bar is shown for
     max(0.5, x + 1.0) seconds (x is DAT_03a83dbc, UNRESOLVED; read as 0).
   - CurrentWeaponQuickSlot (FUN_02b07320, FUN_02b1a5c0): slots side by
     side; the current one is weapon_effect_ingame 196x82 with its image at
     13,12 (172x59) and the number box at 7,52; the others are
     weapon_bg_ingame 150x65, image at 9,10 (132x46), number box at 9,33.
     Number boxes are q_slot_number_box 22x23 with the slot number.
   - PageTool (FUN_02b0c3d0): an "itemlist" grid of 168x78 cells holding
     154x53 images, a WeaponQuickSlot strip, and a "desc" box at 720,17
     (218x470). A double click sends "vxl_wpn 0 <id>" and plays
     events/select.wav (FUN_02b17290). Items drag onto the strip
     (GridViewPanelBeginDrag / EndDrag; what a drop sends is INFERRED to be
     "vxl_wpn <slot> <id>").
   - The World settings page (FUN_02b09a80): [Rules settings] with Play
     mode (the 21 modes at DAT_02fb6d38), Player respawn, Playtime 10 20 30
     40 50 60 120 300 or Permanent, Number of players, Reset device upon game
     start, Collision between players, 3rd person, Minimum starting number,
     Victory criteria (#Vxl_Fullbrowser_EndRule_0..2), Restore destroyed
     map, Enable destruction (human / zombie), Zombie detecting, Respawn
     wait 0..60, Match lost when all dead, Attackable player (Enemy, All,
     None), Seeker damage 0..100, Restrict object list; [Map settings] with
     Sky (voxel/voxel_skybox.csv), Weather (None, Rain, Snow), Color (day),
     Remove default key light, Color (back), Fog color, Fog amount and
     Visibility range sliders, Turn on mist.
   - FUN_02b10390 sends one command per changed row: vxl_gamemode %d,
     vxl_respawn %d, vxl_playtime %d, vxl_skyname %s, vxl_fogcolor %d %d %d,
     vxl_daylight %d %d %d, vxl_invlight %d %d %d, vxl_weather %d,
     vxl_maxplayernum %d, vxl_fogdensity %f, vxl_resettrigger %d,
     vxl_playercollision %d, vxl_enabletps %d, vxl_minplayernum %d,
     vxl_endrule %d, vxl_initback %d, vxl_humandestroy %d,
     vxl_zombidestroy %d, vxl_zombiseethru %d, vxl_fogon %d,
     vxl_respawntime %d, vxl_defeatbywipe %d, vxl_friendlyfire %d (0 only
     for "None") with vxl_enemyfire %d, vxl_seekerpenalty %d,
     vxl_hiderproplimit %d, vxl_remove_dim_light %d.

   INFERRED: vxl_enemyfire is 1 for "Enemy team" only; vxl_visdist carries
   the Visibility slider (hw.dll handles a vxl_visdist).
   DESIGN: where the bar sits (bottom centre), its 0.5 s fade, the page
   layouts inside the 980x600 sheet, list rows stepped with the arrows, the
   colour rows as three bars, the Number of players lists (1..32), the
   defaults shown before the map sets a value, right click to empty a quick
   slot, sending the tool slots to the server with vxl_qsload, and the
   registry file name. The .vxlqs registry itself follows client.dll. The Shelter, Gun Deathmatch,
   level and difficulty rows are not ported. */

#define VQ_SLOTS 5
#define VQ_DEFAULTS "300 335 301 334 0"

float g_vqShowUntil;
var int g_vqCur = 0i;
bool g_vqSynced;
var float g_vqHeld = 0;
var float g_vqHeldSince = 0;
var float g_vqLastCmd = 0;
var float g_vtSelId = 0;	/* the tool in the desc box */
var float g_vtDrag = 0;		/* the tool being dragged */
float g_vtLastClick;
var float g_vtLastId = -1;

/* --- the tools ------------------------------------------------------------------- */

/* weapon ids (item.csv column 5) the Tool page lists; the player owns them all */
var float g_vtIds[7] = {300, 335, 363, 301, 334, 364, 485};
#define VT_COUNT 7i

string
VT_Name(float wid)
{
	switch (wid) {
	case 300: return "vxllonggun";
	case 301: return "vxlconnector";
	case 334: return "vxleditor";
	case 335: return "vxlshortgun";
	case 363: return "vxlminigun";
	case 364: return "vxlscanner";
	case 485: return "vxlmarker";
	}
	return "";
}

string
VT_Image(float wid)
{
	return strcat("gfx/vgui/basket/", VT_Name(wid));
}

string
VT_Label(float wid)
{
	return Loc(strcat("CSO_Item_Name_", VT_Name(wid)), VT_Name(wid));
}

/* --- the quick slots --------------------------------------------------------------- */

/* The .vxlqs registry (client.dll FUN_02b14530 load, FUN_02b19480 save):
   one line of "%04d" ids, the 10 block quick slots (DAT_03584c4c) then the
   5 tool quick slots (DAT_03584c5c), 60 digits in all. */
#define VQ_BLOCKSLOTS 10
#define VQ_REGLEN 15
#define VQ_FILE "studio/quickslot.vxlqs"	/* DESIGN: CSO names it <user>.vxlqs */
float g_vqReg[VQ_REGLEN];
bool g_vqRegLoaded;

bool VB_BlockKnown(float bid);

string
VQ_Pad4(float v)
{
	string n = ftos(bound(0, floor(v), 9999));
	return strcat(substring("0000", 0, 4 - strlen(n)), n);
}

void
VQ_RegSave(void)
{
	string out = "";
	for (int i = 0i; i < VQ_REGLEN; i++)
		out = strcat(out, VQ_Pad4(g_vqReg[i]));
	filestream fh = fopen(VQ_FILE, FILE_WRITE);
	if (fh < 0)
		return;
	fputs(fh, out);
	fclose(fh);
}

/* a saved tool id is kept when it is one of the four defaults
   (DAT_0317444c), one of the item tools (DAT_0317445c: item 970 -> 363,
   971 -> 364, 8412 -> 485, kept only if the item is owned) or a weapon
   client.dll knows. DESIGN: Bulwark has no item inventory, so the item
   tools count as owned and "a known weapon" is a tool on the Tool page. */
bool
VQ_ToolKnown(float wid)
{
	for (int i = 0i; i < VT_COUNT; i++)
		if (g_vtIds[i] == wid)
			return true;
	return false;
}

void
VQ_RegLoad(void)
{
	if (g_vqRegLoaded)
		return;
	g_vqRegLoaded = true;

	string s = "";
	filestream fh = fopen(VQ_FILE, FILE_READ);
	if (fh >= 0) {
		s = fgets(fh);
		fclose(fh);
	}
	bool ok = (strlen(s) == VQ_REGLEN * 4);
	for (int c = 0i; ok && c < strlen(s); c++) {
		float ch = str2chr(s, c);
		if (ch < 48 || ch > 57)
			ok = false;
	}
	if (!ok) {
		/* FUN_02b11da0 twice, the default tools, then save */
		print("Voxel Quickslot : wrong quickslot registry format\n");
		for (int i = 0i; i < VQ_REGLEN; i++)
			g_vqReg[i] = 0;
		tokenize(VQ_DEFAULTS);
		for (int i = 0i; i < 4i; i++)
			g_vqReg[VQ_BLOCKSLOTS + i] = stof(argv(i));
		VQ_RegSave();
		return;
	}
	for (int i = 0i; i < VQ_REGLEN; i++) {
		float v = stof(substring(s, i * 4, 4));
		if (i < VQ_BLOCKSLOTS)
			g_vqReg[i] = VB_BlockKnown(v) ? v : 0;	/* FUN_02b12550 */
		else
			g_vqReg[i] = VQ_ToolKnown(v) ? v : 0;
	}
}

string
VQ_List(void)
{
	VQ_RegLoad();
	string out = "";
	for (int i = 0i; i < VQ_SLOTS; i++)
		out = (i == 0i) ? ftos(g_vqReg[VQ_BLOCKSLOTS + i]) : strcat(out, " ", ftos(g_vqReg[VQ_BLOCKSLOTS + i]));
	return out;
}

float
VQ_Slot(int n)
{
	VQ_RegLoad();
	if (n < 0i || n >= VQ_SLOTS)
		return 0;
	return g_vqReg[VQ_BLOCKSLOTS + n];
}

void
VQ_SetSlot(int n, float wid)
{
	VQ_RegLoad();
	if (n < 0i || n >= VQ_SLOTS)
		return;
	g_vqReg[VQ_BLOCKSLOTS + n] = wid;
	VQ_RegSave();
}

void
VQ_Show(void)
{
	g_vqShowUntil = time + 1.0;
}

/* The weapon in the hands is chosen on the client in Nuclide (the input
   frame carries it), so the HUD selects the slot's tool itself once the
   server has given it, and reports switches made some other way. */
var float g_vqWant = 0;
float g_vqWantUntil;

float
VQ_TitleId(string t)
{
	for (int i = 0i; i < VT_COUNT; i++)
		if (VT_Name(g_vtIds[i]) == t)
			return g_vtIds[i];
	return 0;
}

void
VQ_Want(float wid)
{
	g_vqWant = wid;
	g_vqWantUntil = time + 2;
	g_vqLastCmd = time;
}

void
VQ_Follow(void)
{
	entity cur = weapon.GetActiveWeapon();
	float held = cur ? VQ_TitleId(weapon.GetTitle(cur)) : 0;

	if (g_vqWant) {
		if (held == g_vqWant || time > g_vqWantUntil) {
			g_vqWant = 0;
			return;
		}
		entity first = weapon.GetFirstWeaponInInventory();
		entity w = first;
		for (int i = 0i; i < 64i && w; i++) {
			if (VQ_TitleId(weapon.GetTitle(w)) == g_vqWant) {
				weapon.SelectWeapon(w);
				return;
			}
			w = weapon.GetNextWeaponRelativeTo(w);
			if (w == first)
				break;
		}
		return;
	}
	/* the wheel or another key changed the tool: follow it to its slot,
	   once the hands have settled on it (the active weapon flickers
	   while a server switch and the local prediction disagree) */
	if (held != g_vqHeld) {
		g_vqHeld = held;
		g_vqHeldSince = time;
	}
	if (time < g_vqHeldSince + 0.3 || time < g_vqLastCmd + 0.6)
		return;
	if (held && held != VQ_Slot(g_vqCur))
		for (int s = 0i; s < VQ_SLOTS; s++)
			if (VQ_Slot(s) == held) {
				g_vqCur = s;
				g_vqLastCmd = time;
				localcmd(sprintf("cmd vxl_wpn %i %d held\n", s, held));
				VQ_Show();
				return;
			}
}

/* FUN_02b1a110 */
bool
VQ_Pick(int n)
{
	float wid = VQ_Slot(n);
	if (n < 0i || n >= VQ_SLOTS || wid <= 0)
		return false;
	localcmd(sprintf("cmd vxl_wpn %i %d\n", n, wid));
	g_vqCur = n;
	VQ_Want(wid);
	VQ_Show();
	return true;
}

void
VQ_Sync(void)
{
	if (g_vqSynced || HUD_ModeId() != 38)
		return;
	/* the server has equipped us once it published our slots */
	if (getplayerkeyvalue(player_localnum, "vx_qs") == "")
		return;
	g_vqSynced = true;
	localcmd(sprintf("cmd vxl_qsload %s\n", VQ_List()));
	VQ_Want(VQ_Slot(g_vqCur));
}

void
VQ_DrawBar(void)
{
	VQ_Sync();
	if (HUD_ModeId() != 38)
		return;
	VQ_Follow();

	float a = bound(0, (g_vqShowUntil + 0.5 - time) / 0.5, 1);
	if (g_vbOpen)
		a = 1;
	if (a <= 0)
		return;

	vector hs = screen.HUDSize() / g_hs;
	float w = VQ_SLOTS * 150 + 46;
	vector o = [(hs[0] - w) / 2, hs[1] - 82 - 72];
	float x = 0;

	for (int i = 0i; i < VQ_SLOTS; i++) {
		float wid = VQ_Slot(i);
		vector p = o + [x, 0];
		vector ip, is, np;
		if (i == g_vqCur) {
			drawpic(Art_Pos(p), "resource/voxel/weapon_effect_ingame", [196, 82] * g_hs, [1,1,1], a, 0);
			ip = p + [13, 12];
			is = [172, 59];
			np = p + [7, 52];
			x += 196;
		} else {
			drawpic(Art_Pos(p), "resource/voxel/weapon_bg_ingame", [150, 65] * g_hs, [1,1,1], a, 0);
			ip = p + [9, 10];
			is = [132, 46];
			np = p + [9, 33];
			x += 150;
		}
		if (wid > 0)
			drawpic(Art_Pos(ip), VT_Image(wid), is * g_hs, [1,1,1], a, 0);
		drawpic(Art_Pos(np), "resource/voxel/q_slot_number_box", [22, 23] * g_hs, [1,1,1], a, 0);
		string num = itos(i + 1i);
		float tw = font.StringWidth(num, false, g_hudFont) / g_hs;
		draw.Text_RGBA(Art_Pos(np + [(22 - tw) / 2, (23 - font.GetHeight(g_hudFont) / g_hs) / 2]), num, [1,1,1], a, g_hudFont);
	}
}

/* The build tools' buttons. Nuclide reads input_buttons inside its own
   player code and never fills button0/1 for the rules progs, so the HUD
   forwards the keys bound to +attack and +attack2 while a tool is in the
   hands in mode 38 (DESIGN transport for mp.dll's PrimaryAttack /
   SecondaryAttack), and swallows them so the placeholder weapon code does
   not fire. */
bool HUD_WantsCursor(void);
bool g_vqHeld1, g_vqHeld2;

bool
VQ_Input(float evType, float key)
{
	if (evType != IE_KEYDOWN && evType != IE_KEYUP)
		return false;
	if (HUD_ModeId() != 38 || HUD_WantsCursor())
		return false;
	string b = getkeybind(key);
	float which = (b == "+attack") ? 1 : ((b == "+attack2") ? 2 : 0);
	if (!which)
		return false;
	bool down = (evType == IE_KEYDOWN);
	if (which == 1) {
		if (down == g_vqHeld1)
			return true;
		g_vqHeld1 = down;
	} else {
		if (down == g_vqHeld2)
			return true;
		g_vqHeld2 = down;
	}
	localcmd(sprintf("cmd vxl_fire %d %d\n", which, down));
	return true;
}

/* --- the Tool page ----------------------------------------------------------------- */

#define VT_CELLW 168
#define VT_CELLH 78
#define VT_STRIPY 486

float
VT_GridW(void)
{
	/* fixed: the grid stops where the desc box starts (720), so cells
	   never reflow under the cursor when a tool gets selected */
	return 712;
}

/* the tool under the mouse, or 0 */
float
VT_ItemAt(vector po)
{
	vector m = g_resMouse - (po + [4, 4]);
	int cols = (int)floor(VT_GridW() / VT_CELLW);
	if (m[0] < 0 || m[1] < 0 || m[0] >= cols * VT_CELLW || m[1] >= VT_STRIPY - 12)
		return 0;
	int n = (int)floor(m[1] / VT_CELLH) * cols + (int)floor(m[0] / VT_CELLW);
	return (n < VT_COUNT) ? g_vtIds[n] : 0;
}

vector
VT_SlotPos(vector po, int i)
{
	return po + [8 + i * 186, VT_STRIPY];
}

/* the quick slot box under the mouse, or -1 */
int
VT_SlotAt(vector po)
{
	for (int i = 0i; i < VQ_SLOTS; i++)
		if (Res_MouseIn(VT_SlotPos(po, i), [178, 64]))
			return i;
	return -1i;
}

void
VT_DrawPage(vector po)
{
	int cols = (int)floor(VT_GridW() / VT_CELLW);
	float hover = VT_ItemAt(po);

	for (int n = 0i; n < VT_COUNT; n++) {
		float wid = g_vtIds[n];
		vector cp = po + [4, 4] + [(n % cols) * VT_CELLW, floor(n / cols) * VT_CELLH];
		drawfill(Art_Pos(cp), [VT_CELLW - 4, VT_CELLH - 4] * g_hs, [0.12, 0.12, 0.12], 0.8, 0);
		drawpic(Art_Pos(cp + [5, 4]), VT_Image(wid), [154, 53] * g_hs, [1,1,1], 1, 0);
		string lbl = VT_Label(wid);
		float lw = font.StringWidth(lbl, false, g_hudFont) / g_hs;
		VB_Text(cp + [(VT_CELLW - 4 - lw) / 2, 58], lbl, [0.85, 0.85, 0.85]);
		if (wid == g_vtSelId || wid == hover)
			drawfill(Art_Pos(cp), [VT_CELLW - 4, VT_CELLH - 4] * g_hs, [1, 0.8, 0.4], (wid == g_vtSelId) ? 0.18 : 0.08, 0);
	}

	/* WeaponQuickSlot */
	VB_Text(po + [8, VT_STRIPY - 20], Loc("Vxl_Fullbrowser_QuickSlot", "Quick slots"), [1, 1, 1]);
	int over = VT_SlotAt(po);
	for (int i = 0i; i < VQ_SLOTS; i++) {
		vector sp = VT_SlotPos(po, i);
		float sid = VQ_Slot(i);
		drawpic(Art_Pos(sp), "resource/voxel/weapon_slot_box", [178, 64] * g_hs, [1,1,1], 1, 0);
		if (sid > 0)
			drawpic(Art_Pos(sp + [12, 6]), VT_Image(sid), [154, 53] * g_hs, [1,1,1], 1, 0);
		if (i == over && g_vtDrag)
			drawfill(Art_Pos(sp), [178, 64] * g_hs, [1, 0.8, 0.4], 0.25, 0);
		drawpic(Art_Pos(sp + [4, 4]), "resource/voxel/q_slot_number_box", [22, 23] * g_hs, [1,1,1], 1, 0);
		VB_Text(sp + [11, 8], itos(i + 1i), [1, 1, 1]);
	}

	/* desc */
	if (g_vtSelId) {
		vector bp = po + [720, 17];
		vector bs = [218, 470];
		drawfill(Art_Pos(bp), bs * g_hs, [0, 0, 0], 0.55, 0);
		drawpic(Art_Pos(bp + [(bs[0] - 154) / 2, 10]), VT_Image(g_vtSelId), [154, 53] * g_hs, [1,1,1], 1, 0);
		float y = 72;
		y += VB_Wrap(bp + [8, y], bs[0] - 16, VT_Label(g_vtSelId), [1, 0.85, 0.5]) + 6;
		string desc = strreplace("\\n", "\n", Loc(strcat("CSO_Item_Desc_", VT_Name(g_vtSelId)), ""));
		int np = (int)tokenizebyseparator(desc, "\n");
		string para[8];
		for (int k = 0i; k < np && k < 8i; k++)
			para[k] = argv(k);
		for (int k = 0i; k < np && k < 8i; k++)
			y += VB_Wrap(bp + [8, y], bs[0] - 16, para[k], [0.85, 0.85, 0.85]);
	}

	if (g_vtDrag)
		drawpic(Art_Pos(g_resMouse - [77, 26]), VT_Image(g_vtDrag), [154, 53] * g_hs, [1,1,1], 0.7, 0);
}

void
VT_MouseDown(vector po)
{
	float wid = VT_ItemAt(po);
	if (wid) {
		if (wid == g_vtLastId && time - g_vtLastClick < 0.4) {
			/* FUN_02b17290 */
			localcmd(sprintf("cmd vxl_wpn 0 %d\n", wid));
			VQ_SetSlot(0i, wid);
			g_vqCur = 0i;
			VQ_Want(wid);
			localsound("events/select.wav");
			g_vtLastId = -1;
			g_vtDrag = 0;
			return;
		}
		g_vtLastId = wid;
		g_vtLastClick = time;
		g_vtSelId = wid;
		g_vtDrag = wid;
		localsound("events/on mouse.wav");
		return;
	}
	int s = VT_SlotAt(po);
	if (s >= 0i)
		VQ_Pick(s);
}

void
VT_MouseUp(vector po)
{
	if (!g_vtDrag)
		return;
	int s = VT_SlotAt(po);
	if (s >= 0i) {
		VQ_SetSlot(s, g_vtDrag);
		localcmd(sprintf("cmd vxl_wpn %i %d\n", s, g_vtDrag));
		g_vqCur = s;
		VQ_Want(g_vtDrag);
		localsound("events/select.wav");
	}
	g_vtDrag = 0;
}

void
VT_RightClick(vector po)
{
	int s = VT_SlotAt(po);
	if (s < 0i)
		return;
	VQ_SetSlot(s, 0);
	localcmd(sprintf("cmd vxl_qsload %s\n", VQ_List()));
}

/* --- the World settings page ----------------------------------------------------------- */

#define VS_ROWS 27
#define VS_ROWH 26

typedef struct {
	string key;	/* the vxl_ command without its prefix */
	string kind;	/* c check, l list, s slider, rgb colour */
	string label;	/* localization token */
	string fallback;
	string opts;	/* list rows: "value label|value label" (labels may be tokens, starting with #) */
	string def;
	float col;
} vsrow_t;
vsrow_t *g_vs;
int g_vsCount;
string g_vsSkies;	/* the sky list: "value label|..." */
int g_vsRuleRows;

void
VS_Add(string key, string kind, string label, string fallback, string opts, string def, float col)
{
	int i = g_vsCount++;
	g_vs[i].key = key;
	g_vs[i].kind = kind;
	g_vs[i].label = label;
	g_vs[i].fallback = fallback;
	g_vs[i].opts = opts;
	g_vs[i].def = def;
	g_vs[i].col = col;
}

string
VS_Range(float lo, float hi)
{
	string s = "";
	for (float v = lo; v <= hi; v++)
		s = strcat(s, (s == "") ? "" : "|", ftos(v), " ", ftos(v));
	return strzone(s);
}

void
VS_LoadSkies(void)
{
	filestream fh = fopen("voxel/voxel_skybox.csv", FILE_READ);
	string s, out = "";
	bool header = true;

	if (fh >= 0) {
		while ((s = fgets(fh))) {
			if (header) {
				header = false;
				continue;
			}
			if (tokenizebyseparator(s, ",") < 2 || argv(1) == "")
				continue;
			out = strcat(out, (out == "") ? "" : "|", argv(1), " ", argv(0));
		}
		fclose(fh);
	}
	if (out == "")
		out = "none none";
	g_vsSkies = strzone(out);
}

void
VS_Init(void)
{
	if (g_vs)
		return;
	g_vs = (vsrow_t *)memalloc(sizeof(vsrow_t) * VS_ROWS);
	VS_LoadSkies();
	/* DAT_02fb6d38 */
	string modes = "39 #Vxl_OutUI_Mode_Play|14 #Vxl_OutUI_Mode_ZombieHero|2 #Vxl_OutUI_Mode_TeamDeath|24 #Vxl_OutUI_Mode_BotZombieHero|5 #Vxl_OutUI_Mode_BotTeamDeath|16 #Vxl_OutUI_Mode_Soccer|20 #Vxl_OutUI_Mode_ZombiEscape|45 #Vxl_OutUI_Mode_ZombieZ|49 #Vxl_OutUI_Mode_PropHunt|0 #Vxl_OutUI_Mode_Original|23 Mode_23|52 #Vxl_OutUI_Mode_Shelter|40 #Vxl_OutUI_Mode_AllStar|22 #Vxl_OutUI_Mode_GunDeath|54 #Vxl_OutUI_Mode_Zombi5|53 #Vxl_OutUI_Mode_ScenarioTX|8 Mode_8|9 Mode_9|58 Mode_58|1 Mode_1|4 Mode_4";
	VS_Add("gamemode", "l", "Vxl_Fullbrowser_PlayMode", "Play mode", modes, "39", 0);
	VS_Add("respawn", "c", "Vxl_Fullbrowser_Respawn", "Player respawn", "", "1", 0);
	VS_Add("playtime", "l", "Vxl_Fullbrowser_PlayTime", "Playtime(minutes)", "10 10|20 20|30 30|40 40|50 50|60 60|120 120|300 300|0 #CSO_Permanent", "0", 0);
	VS_Add("maxplayernum", "l", "Vxl_Fullbrowser_MaxPlayerNum", "Number of players", VS_Range(1, 32), "16", 0);
	VS_Add("resettrigger", "c", "Vxl_Fullbrowser_ResetTrigger", "Reset device upon game start", "", "0", 0);
	VS_Add("playercollision", "c", "Vxl_Fullbrowser_PlayerCollision", "Collision between players", "", "0", 0);
	VS_Add("enabletps", "c", "Vxl_Fullbrowser_EnableTPS", "3rd person", "", "0", 0);
	VS_Add("minplayernum", "l", "Vxl_Fullbrowser_MinPlayerNum", "Minimum starting number", VS_Range(1, 32), "1", 0);
	VS_Add("endrule", "l", "Vxl_Fullbrowser_GameEndRule", "Victory criteria", "0 #Vxl_Fullbrowser_EndRule_0|1 #Vxl_Fullbrowser_EndRule_1|2 #Vxl_Fullbrowser_EndRule_2", "0", 0);
	VS_Add("initback", "c", "Vxl_Fullbrowser_InitBack", "Restore destroyed map at beginning of round", "", "0", 0);
	VS_Add("humandestroy", "c", "Vxl_Fullbrowser_HumanDestroy", "Enable destruction: Human map", "", "0", 0);
	VS_Add("zombidestroy", "c", "Vxl_Fullbrowser_ZombiDestroy", "Enable destruction: Zombie map", "", "0", 0);
	VS_Add("zombiseethru", "c", "Vxl_Fullbrowser_ZombiSeeThru", "Enable Zombie Team's Human Detecting Ability", "", "0", 0);
	VS_Add("respawntime", "l", "Vxl_Fullbrowser_RespawnTime", "Respawn Wait Time", VS_Range(0, 60), "3", 0);
	VS_Add("defeatbywipe", "c", "Vxl_Fullbrowser_DefeatByWipe", "Match lost when all dead", "", "0", 0);
	VS_Add("attacktype", "l", "Vxl_Fullbrowser_AttackType", "Attackable Player", "0 #Vxl_AttackType_Enemy|1 #Vxl_AttackType_All|2 #Vxl_AttackType_None", "0", 0);
	VS_Add("seekerpenalty", "l", "Vxl_Fullbrowser_SeekerPenalty", "Damage taken by the Seeker", VS_Range(0, 100), "10", 0);
	VS_Add("hiderproplimit", "c", "Vxl_Fullbrowser_HiderPropLimit", "Restrict Object transformation list for player", "", "0", 0);
	g_vsRuleRows = g_vsCount;
	VS_Add("skyname", "l", "Vxl_Fullbrowser_Skybox", "Sky settings", g_vsSkies, "none", 1);
	VS_Add("weather", "l", "Vxl_Fullbrowser_Weather", "Weather", "0 #Vxl_Fullbrowser_Weather_None|1 #Vxl_Fullbrowser_Weather_Rain|2 #Vxl_Fullbrowser_Weather_Snow", "0", 1);
	VS_Add("daylight", "rgb", "Vxl_Fullbrowser_Light", "Color (day)", "", "255 255 255", 1);
	VS_Add("remove_dim_light", "c", "Vxl_Fullbrowser_RemoveDimLight", "Remove default key light effects", "", "0", 1);
	VS_Add("invlight", "rgb", "Vxl_Fullbrowser_InvLight", "Color (Back)", "", "128 128 128", 1);
	VS_Add("fogcolor", "rgb", "Vxl_Fullbrowser_Fog", "Fog color", "", "128 128 128", 1);
	VS_Add("fogdensity", "s", "Vxl_Fullbrowser_FogDensity", "Fog Amount", "", "0.5", 1);
	VS_Add("visdist", "s", "Vxl_Fullbrowser_VisDist", "Visibility range", "", "0.5", 1);
	VS_Add("fogon", "c", "Vxl_Fullbrowser_FogOn", "Turn on mist", "", "0", 1);
}

string
VS_Value(int i)
{
	string k = g_vs[i].key;
	if (k == "attacktype") {
		/* back from vxl_friendlyfire / vxl_enemyfire */
		string ff = serverkey("vxo_friendlyfire");
		if (ff == "")
			return g_vs[i].def;
		if (stof(ff) == 0)
			return "2";
		return stof(serverkey("vxo_enemyfire")) ? "0" : "1";
	}
	string v = serverkey(strcat("vxo_", k));
	if (k == "gamemode" && v == "")
		v = serverkey("vx_mode");
	return (v == "") ? g_vs[i].def : v;
}

string
VS_OptLabel(string lbl)
{
	if (substring(lbl, 0, 1) != "#")
		return strreplace("_", " ", lbl);
	/* the fallback: the token's last part ("Vxl_OutUI_Mode_Soccer" -> "Soccer") */
	string tok = substring(lbl, 1, -1);
	string tail = tok;
	for (float u = strstrofs(tail, "_"); u >= 0; u = strstrofs(tail, "_"))
		tail = substring(tail, u + 1, -1);
	return Loc(tok, tail);
}

/* the shown text of a list row's value, and its index */
float g_vsIdx;
float g_vsCountOpts;
string
VS_ListText(int i, string v)
{
	int n = (int)tokenizebyseparator(g_vs[i].opts, "|");
	string items[110];
	g_vsCountOpts = min(n, 110);
	for (int k = 0i; k < g_vsCountOpts; k++)
		items[k] = argv(k);
	g_vsIdx = -1;
	for (int k = 0i; k < g_vsCountOpts; k++) {
		float sp = strstrofs(items[k], " ");
		if (substring(items[k], 0, sp) == v) {
			g_vsIdx = k;
			return VS_OptLabel(substring(items[k], sp + 1, -1));
		}
	}
	return v;
}

string
VS_ListValue(int i, float k)
{
	tokenizebyseparator(g_vs[i].opts, "|");
	string it = argv(k);
	return substring(it, 0, strstrofs(it, " "));
}

void
VS_Send(int i, string v)
{
	string k = g_vs[i].key;
	if (k == "attacktype") {
		float t = stof(v);
		localcmd(sprintf("cmd vxl_friendlyfire %d\n", (t != 2) ? 1 : 0));
		localcmd(sprintf("cmd vxl_enemyfire %d\n", (t == 0) ? 1 : 0));
	} else if (g_vs[i].kind == "s") {
		localcmd(sprintf("cmd vxl_%s %f\n", k, stof(v)));
	} else {
		localcmd(sprintf("cmd vxl_%s %s\n", k, v));
	}
	localsound("events/on mouse.wav");
}

float
VS_RowH(int i)
{
	return (g_vs[i].kind == "rgb") ? VS_ROWH * 2 : VS_ROWH;
}

vector
VS_RowPos(vector po, int i)
{
	float y = 34;
	for (int k = (g_vs[i].col ? g_vsRuleRows : 0i); k < i; k++)
		y += VS_RowH(k);
	return po + [g_vs[i].col ? 500 : 16, y];
}

#define VS_CTLX 280
#define VS_CTLW 180

void
VS_DrawPage(vector po)
{
	VS_Init();
	VB_Text(po + [16, 10], Loc("Vxl_Fullbrowser_RuleSetting", "[Rules settings]"), [1, 0.85, 0.5]);
	VB_Text(po + [500, 10], Loc("Vxl_Fullbrowser_EnvSetting", "[Map settings]"), [1, 0.85, 0.5]);

	float th = font.GetHeight(g_hudFont) / g_hs;
	for (int i = 0i; i < g_vsCount; i++) {
		vector rp = VS_RowPos(po, i);
		float ctlx = g_vs[i].col ? 200 : VS_CTLX;
		string v = VS_Value(i);
		VB_Text(rp + [0, (20 - th) / 2], Loc(g_vs[i].label, g_vs[i].fallback), [0.85, 0.85, 0.85]);
		vector cp = rp + [ctlx, 0];
		string kind = g_vs[i].kind;
		if (kind == "c") {
			drawfill(Art_Pos(cp + [0, 2]), [16, 16] * g_hs, [0.2, 0.2, 0.2], 0.9, 0);
			if (stof(v))
				drawfill(Art_Pos(cp + [3, 5]), [10, 10] * g_hs, [1, 0.8, 0.4], 1, 0);
		} else if (kind == "l") {
			drawfill(Art_Pos(cp), [VS_CTLW, 20] * g_hs, [0.15, 0.15, 0.15], 0.9, 0);
			VB_Text(cp + [4, (20 - th) / 2], "<", [1, 0.8, 0.4]);
			VB_Text(cp + [VS_CTLW - 12, (20 - th) / 2], ">", [1, 0.8, 0.4]);
			string t = VS_ListText(i, v);
			float tw = font.StringWidth(t, false, g_hudFont) / g_hs;
			VB_Text(cp + [(VS_CTLW - tw) / 2, (20 - th) / 2], t, [1, 1, 1]);
		} else if (kind == "s") {
			float f = bound(0, stof(v), 1);
			drawfill(Art_Pos(cp + [0, 8]), [VS_CTLW, 4] * g_hs, [0.3, 0.3, 0.3], 0.9, 0);
			drawfill(Art_Pos(cp + [f * (VS_CTLW - 8), 2]), [8, 16] * g_hs, [1, 0.8, 0.4], 1, 0);
			if (g_vs[i].key == "fogdensity") {
				VB_Text(cp + [VS_CTLW + 8, (20 - th) / 2], Loc(f < 0.5 ? "Vxl_Fullbrowser_FogDensity0" : "Vxl_Fullbrowser_FogDensity1", f < 0.5 ? "Little" : "A lot"), [0.7, 0.7, 0.7]);
			} else {
				VB_Text(cp + [VS_CTLW + 8, (20 - th) / 2], Loc(f < 0.5 ? "Vxl_Fullbrowser_VisDistNear" : "Vxl_Fullbrowser_VisDistFar", f < 0.5 ? "Near" : "Far"), [0.7, 0.7, 0.7]);
			}
		} else {
			tokenize(v);
			vector rgb = [stof(argv(0)), stof(argv(1)), stof(argv(2))];
			for (int c = 0i; c < 3i; c++) {
				vector bp = cp + [0, c * 15];
				vector tint = '1 0 0';
				if (c == 1i)
					tint = '0 1 0';
				else if (c == 2i)
					tint = '0 0 1';
				drawfill(Art_Pos(bp + [0, 4]), [VS_CTLW, 6] * g_hs, tint * 0.4, 0.9, 0);
				drawfill(Art_Pos(bp + [rgb[c] / 255 * (VS_CTLW - 6), 1]), [6, 12] * g_hs, [1, 1, 1], 1, 0);
			}
			drawfill(Art_Pos(cp + [VS_CTLW + 8, 2]), [36, 40] * g_hs, rgb / 255, 1, 0);
		}
	}
	VB_Text(po + [16, 548], Loc("Vxl_Fullbrowser_PlayModeDesc", "Some options are deactivated depending on play mode."), [0.6, 0.6, 0.6]);
}

void
VS_Click(vector po, float dir)
{
	VS_Init();
	for (int i = 0i; i < g_vsCount; i++) {
		vector rp = VS_RowPos(po, i);
		float ctlx = g_vs[i].col ? 200 : VS_CTLX;
		vector cp = rp + [ctlx, 0];
		string kind = g_vs[i].kind;
		string v = VS_Value(i);
		float mx = g_resMouse[0] - cp[0];
		if (kind == "c" && Res_MouseIn(cp, [VS_CTLW, 20])) {
			VS_Send(i, stof(v) ? "0" : "1");
			return;
		}
		if (kind == "l" && Res_MouseIn(cp, [VS_CTLW, 20])) {
			VS_ListText(i, v);
			float step = (mx < VS_CTLW / 2) ? -1 : 1;
			if (dir < 0)
				step = -step;
			float k = (g_vsIdx < 0) ? 0 : mod(g_vsIdx + step + g_vsCountOpts, g_vsCountOpts);
			VS_Send(i, VS_ListValue(i, k));
			return;
		}
		if (kind == "s" && Res_MouseIn(cp, [VS_CTLW, 20])) {
			VS_Send(i, ftos(bound(0, mx / VS_CTLW, 1)));
			return;
		}
		if (kind == "rgb" && Res_MouseIn(cp, [VS_CTLW, 45])) {
			tokenize(v);
			vector rgb = [stof(argv(0)), stof(argv(1)), stof(argv(2))];
			int c = (int)bound(0, floor((g_resMouse[1] - cp[1]) / 15), 2);
			rgb[c] = rint(bound(0, mx / VS_CTLW, 1) * 255);
			VS_Send(i, sprintf("%d %d %d", rgb[0], rgb[1], rgb[2]));
			return;
		}
	}
}
