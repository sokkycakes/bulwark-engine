/* Buy menu (handoff M3): CBuyMenu with its CBuySubMenu page.

   What the build does (SOURCE-CONFIRMED, client.dll):
   - CBuySubMenu's constructor (FUN_02936650) loads
     Resource/UI/CSO_BuySubMenu_ver7.res, then builds its own panels in
     code: BuyLeftSlotPanel, BuySearchPanel, BuyBasketPanel,
     BuyRightFavoritePanel, account / buytime / remain labels and the
     RebuyButton (art resource/Control/Button_Glow/btn_glow).
   - CBuySubMenu_DM loads CSO_BuySubMenu_DM.res, CBuySubMenu_PVPVE
     CSO_BuySubMenu_PVPVE.res.
   - The search filter's categories: Primary, Pistol, Knife, Equipment
     (FUN_0292b500) and, under Primary, ShotGun, SubMachineGun,
     AssaultRifle, Rifle, MachineGun, Special (FUN_0292bd10), shown as
     #CSO_Category*.

   What this file uses instead (sokky chose the .nar layouts, 2026-10-28):
   ver7 is not in any extracted .nar layer (it is presumably in the
   encrypted Data/Packer archives: UNKNOWN). The layouts here are the
   newest ones on disk: Resource/UI/CSO_BuyMenu.res (the 606x420
   WizardPanel frame) and CSO_BuySubMenu_ver5.res (fstrike). They are an
   older version of the menu, so ver7's code-built panels (search, left
   slot panel, favourites) are left out rather than mixed in.

   Behaviour, all DESIGN unless marked:
   - slot0..slot9 (the ver5 res's MouseOverPanelButtons) list the
     categories, then a category's items, ten to a page with prevBtn /
     nextBtn. Number keys 1-9 and 0 press slot0..slot9.
   - Choosing an item sends "buy <name>" to the server at once (the
     rules' CS_Buy decides; the reply is printed as before). CSO's basket
     (pick several, then BasketBuy) is drawn but disabled, as are
     RebuyButton, the favourites set picker and BasketClear.
   - ItemInfo shows the hovered item's gfx/vgui/basket picture (OBSERVED
     path format "gfx/vgui/basket/%s" in client.dll), name and price.
   - account_num shows this round's money (userinfo csnz_cash), buytime_num
     the seconds of buy time left (serverinfo csnz_buyend).
   - The frame is the in-game window art resource/Control/IGFrame/
     window_default_* (OBSERVED string "resource/Control/IGFrame/
     window_default_%s"; which frame CBuyMenu uses is INFERRED). Buttons
     use resource/Control/Button/btn_default (OBSERVED string).

   The item table matches the rules' cs_buyList (modes/objectives.qc):
   STOCK-CS names, teams and prices. Flash and smoke grenades and night
   vision are missing because the rules have no entityDefs for them. */

bool BM_Enabled(string);
#define BM_PAGE 10i

hashtable g_bmFrameRes;
hashtable g_bmRes;
bool g_bmOpen;
int g_bmCat;		/* -1 categories, else the category shown */
int g_bmPage;
int g_bmHover;		/* the item under the mouse, -1 none */

/* categories: #CSO_Category* tokens (SOURCE-CONFIRMED names) */
#define BM_NCATS 7i
string g_bmCatToken[BM_NCATS] = {
	"CSO_CategoryPistol",
	"CSO_CategoryShotGun",
	"CSO_CategorySubMachineGun",
	"CSO_CategoryAssaultRifle",
	"CSO_CategoryRifle",
	"CSO_CategoryMachineGun",
	"CSO_CategoryEquipment"
};

/* items: buy name, category, team (0 any, 1 T, 2 CT), price, name token,
   picture under gfx/vgui/basket/ */
#define BM_NITEMS 30i
string g_bmItem[BM_NITEMS * 6i] = {
	"glock18", "0", "0", "400", "CSO_Glock18", "glock18",
	"usp", "0", "0", "500", "CSO_USP45", "usp45",
	"p228", "0", "0", "600", "CSO_P228", "p228",
	"deagle", "0", "0", "650", "CSO_DesertEagle", "deserteagle",
	"fiveseven", "0", "2", "750", "CSO_FiveSeven", "fiveseven",
	"elite", "0", "1", "800", "CSO_Elites", "elites",
	"m3", "1", "0", "1700", "CSO_m3", "m3",
	"xm1014", "1", "0", "3000", "CSO_xm1014", "xm1014",
	"mac10", "2", "1", "1400", "CSO_MAC10", "mac10",
	"tmp", "2", "2", "1250", "CSO_Tmp", "tmp",
	"mp5navy", "2", "0", "1500", "CSO_MP5", "mp5",
	"ump45", "2", "0", "1700", "CSO_UMP45", "ump45",
	"p90", "2", "0", "2350", "CSO_P90", "p90",
	"galil", "3", "1", "2000", "CSO_Galil", "galil",
	"famas", "3", "2", "2250", "CSO_Famas", "famas",
	"ak47", "3", "1", "2500", "CSO_AK47", "ak47",
	"m4a1", "3", "2", "3100", "CSO_M4A1", "m4a1",
	"sg552", "3", "1", "3500", "CSO_SG552", "sg552",
	"aug", "3", "2", "3500", "CSO_Aug", "aug",
	"scout", "4", "0", "2750", "CSO_Scout", "scout",
	"awp", "4", "0", "4750", "CSO_AWP", "awp",
	"g3sg1", "4", "1", "5000", "CSO_G3SG1", "g3sg1",
	"sg550", "4", "2", "4200", "CSO_SG550", "sg550",
	"m249", "5", "0", "5750", "CSO_M249", "m249",
	"primammo", "6", "0", "60", "CSO_Prim_Ammo", "pammo",
	"secammo", "6", "0", "50", "CSO_Sec_Ammo", "sammo",
	"kevlar", "6", "0", "650", "CSO_Kevlar", "kevlar",
	"kevlar_helmet", "6", "0", "1000", "CSO_Kevlar_Helmet", "kevlar_helmet",
	"hegrenade", "6", "0", "300", "CSO_HEGrenade_Holder", "hegrenade",
	"defuser", "6", "2", "200", "CSO_Defuser", "defuser"
};

string BM_ItemField(int item, int field) { return g_bmItem[item * 6i + field]; }

/* the items of the current category that this team can buy, in order */
int g_bmList[BM_NITEMS];
int g_bmListCount;

float
BM_MyTeam(void)
{
	return stof(getplayerkeyvalue(player_localnum, "*team"));
}

void
BM_BuildList(void)
{
	float team = BM_MyTeam();

	g_bmListCount = 0i;
	for (int i = 0i; i < BM_NITEMS; i++) {
		float t = stof(BM_ItemField(i, 2i));

		if (stoi(BM_ItemField(i, 1i)) != g_bmCat)
			continue;
		if (t && team && t != team)
			continue;
		g_bmList[g_bmListCount] = i;
		g_bmListCount++;
	}
}

void
BM_Init(void)
{
	g_bmFrameRes = Res_Load("resource/ui/cso_buymenu.res");
	g_bmRes = Res_Load("resource/ui/cso_buysubmenu_ver5.res");
	/* Loc_Load takes up to 16 prefixes a call */
	Loc_Load("resource/cso_na_en.utf8.txt",
		"CSO_Category CSO_IGBuyTitle CSO_Buy CSO_PrevBuy CSO_NextBuy CSO_Basket CSO_Fav CSO_RemainTime CSO_Cancel Cstrike_BuyMenuRebuy");
	Loc_Load("resource/cso_na_en.utf8.txt",
		"CSO_Glock18 CSO_USP45 CSO_P228 CSO_DesertEagle CSO_FiveSeven CSO_Elites CSO_m3 CSO_xm1014 CSO_MAC10 CSO_Tmp CSO_MP5 CSO_UMP45 CSO_P90 CSO_Galil CSO_Famas CSO_AK47");
	Loc_Load("resource/cso_na_en.utf8.txt",
		"CSO_M4A1 CSO_SG552 CSO_Aug CSO_Scout CSO_AWP CSO_G3SG1 CSO_SG550 CSO_M249 CSO_Prim_Ammo CSO_Sec_Ammo CSO_Kevlar CSO_HEGrenade_Holder CSO_Defuser");
	registercommand("buy");
	registercommand("buymenu");

	precache_pic("resource/control/igframe/window_default_top_left");
	precache_pic("resource/control/button/btn_default_left@n");
	precache_pic("resource/control/basket/basket_blank_slot");
}

void
BM_Open(void)
{
	g_bmOpen = true;
	g_bmCat = -1i;
	g_bmPage = 0i;
	g_bmHover = -1i;
}

void
BM_Close(void)
{
	g_bmOpen = false;
}

/* the frame's rectangle in layout space */
vector
BM_FrameSize(void)
{
	if (Res_Has(g_bmFrameRes, "BuyMenu"))
		return Res_Size(g_bmFrameRes, "BuyMenu");

	return [606, 420];
}

vector
BM_FrameOrigin(void)
{
	vector screenSize = screen.HUDSize() / g_hs;

	if (Res_Has(g_bmFrameRes, "BuyMenu"))
		return Res_Pos(g_bmFrameRes, "BuyMenu", screenSize);

	return screenSize / 2 - BM_FrameSize() / 2;
}

/* a control's rectangle in layout space */
vector
BM_CtlPos(string ctl)
{
	return Res_CtlPos(g_bmRes, BM_FrameOrigin(), BM_FrameSize(), ctl);
}

void
BM_DrawButton(string ctl, string label)
{
	Res_DrawButton(g_bmRes, BM_FrameOrigin(), BM_FrameSize(), ctl, label,
		BM_Enabled(strtolower(ctl)), substring(ctl, 0, 4) == "slot");
}

void
BM_DrawLabel(string ctl, string txt, vector rgb, bool right)
{
	Res_DrawLabel(g_bmRes, BM_FrameOrigin(), BM_FrameSize(), ctl, txt, rgb, right);
}

/* --- what each slot holds ------------------------------------------------ */

/* the label of slot i, "" for an empty slot */
string
BM_SlotLabel(int i)
{
	if (g_bmCat < 0i) {
		if (i < BM_NCATS)
			return sprintf("%i  %s", i + 1i, Loc(g_bmCatToken[i], g_bmCatToken[i]));
		return "";
	}

	int idx = g_bmPage * BM_PAGE + i;

	if (i == 9i)
		return sprintf("0  %s", Loc("CSO_Cancel", "Cancel"));
	if (idx >= g_bmListCount)
		return "";

	int item = g_bmList[idx];
	return sprintf("%i  %s", i + 1i, Loc(BM_ItemField(item, 4i), BM_ItemField(item, 0i)));
}

/* the item in slot i, -1 for none */
int
BM_SlotItem(int i)
{
	int idx = g_bmPage * BM_PAGE + i;

	if (g_bmCat < 0i || i == 9i || idx >= g_bmListCount)
		return -1i;

	return g_bmList[idx];
}

void
BM_Press(int i)
{
	if (g_bmCat < 0i) {
		if (i < BM_NCATS) {
			g_bmCat = i;
			g_bmPage = 0i;
			BM_BuildList();
		} else if (i == 9i) {
			BM_Close();
		}
		return;
	}

	if (i == 9i) {
		/* back to the categories */
		g_bmCat = -1i;
		return;
	}

	int item = BM_SlotItem(i);

	if (item < 0i)
		return;

	localcmd(sprintf("cmd buy %s\n", BM_ItemField(item, 0i)));
	BM_Close();
}

void
BM_Command(string ctl)
{
	switch (ctl) {
	case "prevbtn":
		if (g_bmCat >= 0i && g_bmPage > 0i)
			g_bmPage--;
		break;
	case "nextbtn":
		if (g_bmCat >= 0i && (g_bmPage + 1i) * BM_PAGE < g_bmListCount)
			g_bmPage++;
		break;
	case "quitbutton":
		BM_Close();
		break;
	default:
		if (substring(ctl, 0, 4) == "slot")
			BM_Press(stoi(substring(ctl, 4, -1)));
	}
}

/* --- drawing ------------------------------------------------------------- */

bool
BM_Enabled(string ctl)
{
	switch (ctl) {
	case "prevbtn":
		return (g_bmCat >= 0i && g_bmPage > 0i);
	case "nextbtn":
		return (g_bmCat >= 0i && (g_bmPage + 1i) * BM_PAGE < g_bmListCount);
	case "quitbutton":
		return true;
	}

	if (substring(ctl, 0, 4) == "slot")
		return BM_SlotLabel(stoi(substring(ctl, 4, -1))) != "";

	/* RebuyButton, BasketClear, BasketBuy, the set buttons */
	return false;
}


void
BM_DrawImage(string ctl)
{
	string pic = Res_Get(g_bmRes, ctl, "image");

	if (pic == "" || !Res_Visible(g_bmRes, ctl))
		return;
	pic = strtolower(pic);
	if (drawgetimagesize(pic)[0] <= 0)
		return;

	drawpic(Art_Pos(BM_CtlPos(ctl)), pic, Res_Size(g_bmRes, ctl) * g_hs, [1,1,1], 1, 0);
}


void
BM_DrawItemInfo(void)
{
	int item = g_bmHover;
	vector p = BM_CtlPos("ItemInfo");
	vector s = Res_Size(g_bmRes, "ItemInfo");

	if (item < 0i)
		return;

	string pic = strcat("gfx/vgui/basket/", BM_ItemField(item, 5i));
	vector ps = drawgetimagesize(pic);

	if (ps[0] > 0) {
		/* fit the width, keep the aspect */
		vector fit = ps * (s[0] / ps[0]);
		drawpic(Art_Pos(p), pic, fit * g_hs, [1,1,1], 1, 0);
		p[1] += fit[1] + 4;
	}

	draw.Text_RGB(Art_Pos(p), Loc(BM_ItemField(item, 4i), BM_ItemField(item, 0i)), [1,1,1], g_hudFont);
	p[1] += 16;
	draw.Text_RGB(Art_Pos(p), sprintf("$%s", BM_ItemField(item, 3i)), [0.97, 0.9, 0.13], g_hudFont);
}

void
BM_Draw(void)
{
	vector fo, fs;
	float money, left;

	if (!g_bmOpen)
		return;

	/* nothing to buy when dead */
	if (player.GetHealth() <= 0) {
		BM_Close();
		return;
	}

	fo = BM_FrameOrigin();
	fs = BM_FrameSize();

	Res_Frame9("resource/control/igframe/window_default_", fo, fs, 1.0f);
	draw.Text_RGB(Art_Pos(fo + [12, 8]), Res_Text(Res_Get(g_bmFrameRes, "BuyMenu", "title")), [1,1,1], g_hudFont);

	/* the basket strip and the favourites panel art (ver5 res images) */
	BM_DrawImage("primaryBG");
	BM_DrawImage("secondaryBG");
	BM_DrawImage("grenadeBG");
	BM_DrawImage("equipBG");
	BM_DrawImage("knifeBG");
	BM_DrawImage("SetSelBg");
	BM_DrawImage("EditDescBg");
	BM_DrawImage("EquipSample");

	for (int i = 0i; i < 10i; i++)
		BM_DrawButton(sprintf("slot%i", i), BM_SlotLabel(i));

	BM_DrawButton("prevBtn", Res_Text(Res_Get(g_bmRes, "prevBtn", "labelText")));
	BM_DrawButton("nextBtn", Res_Text(Res_Get(g_bmRes, "nextBtn", "labelText")));
	BM_DrawButton("RebuyButton", Res_Text(Res_Get(g_bmRes, "RebuyButton", "labelText")));
	BM_DrawButton("BasketClear", Res_Text(Res_Get(g_bmRes, "BasketClear", "labelText")));
	BM_DrawButton("BasketBuy", Res_Text(Res_Get(g_bmRes, "BasketBuy", "labelText")));
	BM_DrawButton("QuitButton", Res_Text(Res_Get(g_bmRes, "QuitButton", "labelText")));

	BM_DrawItemInfo();

	/* account: colour 0xfff8e622 in the constructor (SOURCE-CONFIRMED,
	   read as ARGB); buytime 0xffa8a8a8 */
	money = stof(getplayerkeyvalue(player_localnum, "csnz_cash"));
	BM_DrawLabel("account_num", sprintf("$%d", money), [0xf8, 0xe6, 0x22] / 255, true);

	left = serverkeyfloat("csnz_buyend") - time;
	if (left > 0)
		BM_DrawLabel("buytime_num", sprintf("%s  %d", Loc("CSO_RemainTime", "Time Left"), ceil(left)), [0xa8, 0xa8, 0xa8] / 255, true);

}

/* --- input --------------------------------------------------------------- */

/* the control under the mouse, "" for none */
string
BM_HitTest(void)
{
	return Res_HitTest(g_bmRes, BM_FrameOrigin(), BM_FrameSize(),
		"slot0 slot1 slot2 slot3 slot4 slot5 slot6 slot7 slot8 slot9 prevbtn nextbtn quitbutton rebuybutton basketclear basketbuy");
}

bool
BM_Input(float evType, float key, float chr, float dev)
{
	if (!g_bmOpen)
		return false;

	if (evType == IE_MOUSEABS || evType == IE_MOUSEDELTA) {
		g_resHoverCtl = BM_HitTest();
		g_bmHover = -1i;
		if (substring(g_resHoverCtl, 0, 4) == "slot")
			g_bmHover = BM_SlotItem(stoi(substring(g_resHoverCtl, 4, -1)));
		return true;
	}

	if (evType == IE_KEYDOWN) {
		if (key == K_MOUSE1) {
			g_resPressCtl = BM_HitTest();
			return true;
		}
		if (key == K_ESCAPE) {
			BM_Close();
			return true;
		}
		if (key >= '1' && key <= '9') {
			BM_Press((int)(key - '1'));
			return true;
		}
		if (key == '0') {
			BM_Press(9i);
			return true;
		}
		if (key == '-') {
			BM_Command("prevbtn");
			return true;
		}
		if (key == '=' || key == '+') {
			BM_Command("nextbtn");
			return true;
		}
		/* everything else (movement included) goes to the game */
		return false;
	}

	if (evType == IE_KEYUP && key == K_MOUSE1) {
		string ctl = BM_HitTest();

		if (ctl != "" && ctl == g_resPressCtl && BM_Enabled(ctl))
			BM_Command(ctl);
		g_resPressCtl = "";
		return true;
	}

	return false;
}

bool
BM_ConsoleCommand(string cmd)
{
	tokenize_console(cmd);

	switch (argv(0)) {
	case "buymenu":
		if (g_bmOpen)
			BM_Close();
		else
			BM_Open();
		return true;
	case "buy":
		/* "buy" alone (CS's B key) opens the menu; "buy <item>" goes to
		   the server */
		if (argv(1) != "") {
			localcmd(sprintf("cmd buy %s\n", argv(1)));
			return true;
		}
		if (g_bmOpen)
			BM_Close();
		else
			BM_Open();
		return true;
	}

	return false;
}
