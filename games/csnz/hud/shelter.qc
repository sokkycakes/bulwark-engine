/* Shelter Team (mode 28): the status board, the inventory gauge, the
   placement hints and the five menus (handoff M3).

   Data comes from the rules (src/rules/modes/sheltert.qc):
   - serverinfo zsht_res<t> "wood iron woodCap ironCap freePower powerCap
     tier", zsht_built<t> (one 0/1 per zsht_building.csv row),
     zsht_clock "day state endTime counter" (state 2 is night),
     zsht_rescue "fromDay windowEnd heliTime", zsht_rescueask "asked humans",
     zsht_barricade "hp max", zsht_zombies.
   - userinfo zsht_res "carryWood carryIron capWood capIron points",
     zsht_skills (three base-4 packed level words, 11 skills each) and
     zsht_placing (building row + 1 while placing).
   The tables are the decrypted .cso files the rules read too
   (shelterteam/zsht_buildmenu.csv, zsht_skilltree.csv,
   zsht_worktablemenu.csv).

   Layouts:
   - Housing, its sub menu, warehouse, rescue and skill tree are the
     build's own .res files (OBSERVED, resource/res/popup_zshelterteam*.res),
     drawn the way resfile.qc reads them. Their RoundPanel controls are
     drawn as dark fills (DESIGN: RoundPanel's art is not traced).
   - The housing buttons map to zsht_buildmenu.csv rows 0-11 (button k is
     row k-1: OBSERVED, the .res commands and the table's ids line up);
     "zsht_opensubmenu N" opens the rows whose submenu column is N, in tier
     order (INFERRED). A row is greyed (@d art) when the shelter tier is
     too low or its skill or building dependency is missing (INFERRED from
     the DisableTooltip texts, which name exactly those three).
   - The worktable dialog's .res holds only its 710x398 frame and title;
     the nine crafts are laid out here (DESIGN) with the build's
     zsht_upgrade_btn_* art, three columns for the craft skills 27/28/29
     (zsht_worktablemenu.csv's dependskill).
   - The skill tree slot of a skill is UIPos + 1 (OBSERVED: the 36 slots
     are three categories of four tiers of three, which UIPos 0-35 fills
     in that order). Its art is zsht_st_btn_<ImageName>@n/@d.
   - The board (zsht_board_pve: wood, iron, power, a centre cell and the
     zombie count), the timer strip, the inventory gauge
     (zsht_ingame_gungergauge_bg) and the key hints (ingame_housingkey B,
     ingame_skillkey N, ingame_rkey R, ingame_mouse_left/right) are the
     build's art; where they sit and what goes in each cell is DESIGN, as
     CSO's HUD layout code for them is not traced. The gauge's segmented
     bar is the mental gauge (zsht_mentalrestore, max 100, SOURCE-CONFIRMED
     in mp.dll), which the rules don't model yet, so it is drawn full.

   Keys (DESIGN, after the key art): B (the buy menu command) opens the
   housing menu, N the skill tree. While placing, mouse 1 builds, R
   rotates and mouse 2 cancels. The rules open the warehouse, worktable
   and rescue menus with "zsht_open <name>" when the player uses them. */

#define SHH_MAXM 64
#define SHH_MAXS 40
#define SHH_MAXW 12

float shh_mId[SHH_MAXM];
float shh_mSub[SHH_MAXM];
float shh_mTier[SHH_MAXM];
float shh_mSkill[SHH_MAXM];
float shh_mBld[SHH_MAXM];
float shh_mWood[SHH_MAXM];
float shh_mIron[SHH_MAXM];
float shh_mPow[SHH_MAXM];
string shh_mCmd[SHH_MAXM];
string shh_mName[SHH_MAXM];
string shh_mTip[SHH_MAXM];
string shh_mTipOff[SHH_MAXM];
int shh_mCount;

float shh_sMax[SHH_MAXS];
float shh_sTier[SHH_MAXS];
float shh_sCat[SHH_MAXS];
float shh_sPos[SHH_MAXS];
string shh_sImg[SHH_MAXS];
int shh_sCount;

float shh_wSkill[SHH_MAXW];
float shh_wWood[SHH_MAXW];
float shh_wIron[SHH_MAXW];
string shh_wName[SHH_MAXW];
string shh_wTip[SHH_MAXW];
int shh_wCount;

bool shh_loaded;
hashtable shh_resHousing;
hashtable shh_resSub;
hashtable shh_resUpgrade;
hashtable shh_resWarehouse;
hashtable shh_resRescue;
hashtable shh_resSkill;

/* 0 none, 1 housing, 2 worktable, 3 warehouse, 4 rescue, 5 skill tree */
int g_shhOpen;
float shh_sub;			/* the open sub menu, 0 for none */
float shh_subCol;		/* the housing column it opened from */
string shh_tip;			/* tooltip for this frame */

#define SHH_ART "resource/shelterteam/"

bool
SHH_Active(void)
{
	return HUD_ModeId() == 28;
}

/* --- tables ------------------------------------------------------------ */

void
SHH_LoadTables(void)
{
	filestream f;
	string line;
	int n;

	shh_loaded = true;

	f = fopen("shelterteam/zsht_buildmenu.csv", FILE_READ);
	if (f >= 0) {
		fgets(f);
		while ((line = fgets(f)) && shh_mCount < SHH_MAXM) {
			n = (int)tokenizebyseparator(line, ",");
			if (n < 12i)
				continue;
			shh_mId[shh_mCount] = stof(argv(0));
			shh_mSub[shh_mCount] = stof(argv(1));
			shh_mTier[shh_mCount] = stof(argv(2));
			shh_mSkill[shh_mCount] = stof(argv(3));
			shh_mBld[shh_mCount] = stof(argv(4));
			shh_mCmd[shh_mCount] = strzone(argv(5));
			shh_mName[shh_mCount] = strzone(argv(6));
			shh_mTip[shh_mCount] = strzone(argv(7));
			shh_mTipOff[shh_mCount] = strzone(argv(8));
			shh_mWood[shh_mCount] = stof(argv(9));
			shh_mIron[shh_mCount] = stof(argv(10));
			shh_mPow[shh_mCount] = stof(argv(11));
			shh_mCount++;
		}
		fclose(f);
	}

	f = fopen("shelterteam/zsht_skilltree.csv", FILE_READ);
	if (f >= 0) {
		fgets(f);
		while ((line = fgets(f)) && shh_sCount < SHH_MAXS) {
			n = (int)tokenizebyseparator(line, ",");
			if (n < 20i)
				continue;
			int s = (int)stof(argv(1));
			if (s < 0i || s >= SHH_MAXS)
				continue;
			shh_sMax[s] = stof(argv(3));
			shh_sTier[s] = stof(argv(4));
			shh_sCat[s] = stof(argv(2));
			shh_sPos[s] = stof(argv(5));
			shh_sImg[s] = strzone(strtolower(argv(19)));
			if (s + 1i > shh_sCount)
				shh_sCount = s + 1i;
		}
		fclose(f);
	}

	f = fopen("shelterteam/zsht_worktablemenu.csv", FILE_READ);
	if (f >= 0) {
		fgets(f);
		while ((line = fgets(f)) && shh_wCount < SHH_MAXW) {
			n = (int)tokenizebyseparator(line, ",");
			if (n < 8i)
				continue;
			shh_wSkill[shh_wCount] = stof(argv(1));
			shh_wName[shh_wCount] = strzone(argv(3));
			shh_wTip[shh_wCount] = strzone(argv(4));
			shh_wWood[shh_wCount] = stof(argv(6));
			shh_wIron[shh_wCount] = stof(argv(7));
			shh_wCount++;
		}
		fclose(f);
	}

	shh_resHousing = Res_Load("resource/res/popup_zshelterteamhousingdlg.res");
	shh_resSub = Res_Load("resource/res/popup_zshelterteamhousingsubdlg.res");
	shh_resUpgrade = Res_Load("resource/res/popup_zshelterteamupgradedlg.res");
	shh_resWarehouse = Res_Load("resource/res/popup_zshelterteamwarehousedlg.res");
	shh_resRescue = Res_Load("resource/res/popup_zshelterteamrescuedlg.res");
	shh_resSkill = Res_Load("resource/res/popup_zshelterteamskilltreedlg.res");
	print(sprintf("HUD: shelter tables: %i build menu rows, %i skills, %i crafts\n", shh_mCount, shh_sCount, shh_wCount));
}

void
SHH_Init(void)
{
	Loc_Load("resource/cso_na_en.utf8.txt", "CSO_zsht_ CSO_ZSHT_ CSO_Zsht_ CSO_Bulletin_Close CSO_Cancel_Btn");
	registercommand("zsht_open");
	registercommand("zsht_housing");
	registercommand("zsht_skilltree");
}

/* --- state from the rules ------------------------------------------------ */

float
SHH_Field(string s, int i)
{
	tokenize(s);
	return stof(argv(i));
}

float
SHH_Team(void)
{
	return (stof(getplayerkeyvalue(player_localnum, "*team")) == 1) ? 1 : 2;
}

float
SHH_TeamRes(int i)
{
	return SHH_Field(serverkey(sprintf("zsht_res%g", SHH_Team())), i);
}

float
SHH_Mine(int i)
{
	return SHH_Field(getplayerkeyvalue(player_localnum, "zsht_res"), i);
}

float
SHH_Built(float bid)
{
	return SHH_Field(serverkey(sprintf("zsht_built%g", SHH_Team())), (int)bid);
}

/* as the rules' SHT_SkillLv */
float
SHH_SkillLv(float s)
{
	float v = SHH_Field(getplayerkeyvalue(player_localnum, "zsht_skills"), (int)floor(s / 11));
	return floor(v / pow(4, s % 11)) % 4;
}

float
SHH_Placing(void)
{
	return stof(getplayerkeyvalue(player_localnum, "zsht_placing"));
}

/* the table index of build menu row id, -1 for none */
int
SHH_Row(float id)
{
	for (int i = 0i; i < shh_mCount; i++)
		if (shh_mId[i] == id)
			return i;
	return -1i;
}

bool
SHH_RowOk(int r)
{
	if (r < 0i)
		return false;
	if (shh_mTier[r] > SHH_TeamRes(6))
		return false;
	if (shh_mSkill[r] > 0 && SHH_SkillLv(shh_mSkill[r]) <= 0)
		return false;
	if (shh_mBld[r] > 0 && !SHH_Built(shh_mBld[r]))
		return false;
	return true;
}

/* a shelter upgrade that is done */
bool
SHH_RowDone(int r)
{
	tokenize(shh_mCmd[r]);
	if (strtolower(argv(0)) != "zsht_upgradeshelter")
		return false;
	return SHH_Built(stof(argv(1))) > 0;
}

/* the rows of sub menu n in tier order (then table order) */
int
SHH_SubRow(float sub, int k)
{
	int seen = 0i;

	for (float t = 0; t <= 5; t++)
		for (int i = 0i; i < shh_mCount; i++) {
			if (shh_mSub[i] != sub || shh_mTier[i] != t)
				continue;
			if (seen == k)
				return i;
			seen++;
		}
	return -1i;
}

/* --- drawing helpers ----------------------------------------------------- */

vector
SHH_Origin(hashtable tab, string frame)
{
	vector s = Res_Size(tab, frame);
	return [Board_CX() - s[0] / 2, (HUD_H - s[1]) / 2];
}

void
SHH_Pic(vector p, string pic, vector size, float alpha)
{
	drawpic(Art_Pos(p), pic, size * g_hs, [1,1,1], alpha, 0);
}

/* a .res image control: its own art (lower case), or pic if given */
void
SHH_Image(hashtable tab, vector o, vector fs, string ctl, string pic, float alpha)
{
	if (pic == "")
		pic = strtolower(Res_Get(tab, ctl, "image"));
	if (pic == "")
		return;
	SHH_Pic(Res_CtlPos(tab, o, fs, ctl), pic, Res_Size(tab, ctl), alpha);
}

void
SHH_Round(hashtable tab, vector o, vector fs, string ctl)
{
	drawfill(Art_Pos(Res_CtlPos(tab, o, fs, ctl)), Res_Size(tab, ctl) * g_hs, [0.12, 0.12, 0.12], 0.75, 0);
}

/* text in a control's box, aligned as its textAlignment says */
void
SHH_Text(hashtable tab, vector o, vector fs, string ctl, string txt, vector rgb)
{
	vector p = Res_CtlPos(tab, o, fs, ctl);
	vector s = Res_Size(tab, ctl);
	string a = strtolower(Res_Get(tab, ctl, "textAlignment"));
	float w = font.StringWidth(txt, false, g_hudFont) / g_hs;
	float h = font.GetHeight(g_hudFont) / g_hs;

	if (substring(a, 0, 4) == "cent")
		p[0] += (s[0] - w) / 2;
	else if (a == "east")
		p[0] += s[0] - w;
	draw.Text_RGB(Art_Pos(p + [0, (s[1] - h) / 2]), txt, rgb, g_hudFont);
}

/* a label with its own text from the .res */
void
SHH_Label(hashtable tab, vector o, vector fs, string ctl, vector rgb)
{
	SHH_Text(tab, o, fs, ctl, Res_Text(Res_Get(tab, ctl, "labelText")), rgb);
}

/* a format string with %d / %s filled in order */
string
SHH_Fmt(string fmt, string a, string b)
{
	fmt = strreplace("ZZZZZZ ", "", fmt);
	int i = strstrofs(fmt, "%");
	if (i >= 0i)
		fmt = strcat(substring(fmt, 0, i), a, substring(fmt, i + 2i, -1));
	i = strstrofs(fmt, "%");
	if (i >= 0i)
		fmt = strcat(substring(fmt, 0, i), b, substring(fmt, i + 2i, -1));
	return fmt;
}

void
SHH_Frame(hashtable tab, vector o, string frame)
{
	vector s = Res_Size(tab, frame);
	Res_Frame9("resource/control/igframe/window_default_", o, s, 1.0f);
}

bool
SHH_Hover(hashtable tab, vector o, vector fs, string ctl)
{
	return Res_MouseIn(Res_CtlPos(tab, o, fs, ctl), Res_Size(tab, ctl));
}

/* the cost strip of a housing button: wood, iron and power */
void
SHH_Costs(hashtable tab, vector o, vector fs, int k, float wood, float iron, float pow)
{
	vector c = [0.85, 0.8, 0.6];

	SHH_Image(tab, o, fs, sprintf("WoodIcon_%i", k), "", 1.0f);
	SHH_Text(tab, o, fs, sprintf("WoodLabel_%i", k), ftos(wood), c);
	SHH_Image(tab, o, fs, sprintf("IronIcon_%i", k), "", 1.0f);
	SHH_Text(tab, o, fs, sprintf("IronLabel_%i", k), ftos(iron), c);
	SHH_Image(tab, o, fs, sprintf("ElectricIcon_%i", k), "", 1.0f);
	SHH_Text(tab, o, fs, sprintf("ElectricLabel_%i", k), ftos(pow), c);
}

/* a tooltip box at the mouse; "\n" in the text breaks lines and long lines
   wrap */
void
SHH_DrawTip(string txt)
{
	float maxW = 300;
	float lh = font.GetHeight(g_hudFont) / g_hs + 2;
	string lines[24];
	int nl = 0i;
	int nseg = (int)tokenizebyseparator(Res_Text(txt), "\\n");
	string segs[24];

	for (int i = 0i; i < nseg && i < 24i; i++)
		segs[i] = argv(i);

	for (int i = 0i; i < nseg && i < 24i && nl < 24i; i++) {
		string cur = "";
		int nw = (int)tokenize(segs[i]);
		for (int w = 0i; w < nw; w++) {
			string next = (cur == "") ? argv(w) : strcat(cur, " ", argv(w));
			if (cur != "" && font.StringWidth(next, false, g_hudFont) / g_hs > maxW && nl < 23i) {
				lines[nl++] = cur;
				cur = argv(w);
			} else {
				cur = next;
			}
		}
		lines[nl++] = cur;
	}

	vector p = g_resMouse + [16, 16];
	vector s = [maxW + 16, nl * lh + 12];
	if (p[0] + s[0] > Board_CX() * 2)
		p[0] = g_resMouse[0] - s[0] - 4;
	if (p[1] + s[1] > HUD_H)
		p[1] = HUD_H - s[1];
	drawfill(Art_Pos(p), s * g_hs, [0.05, 0.05, 0.05], 0.92, 0);
	for (int i = 0i; i < nl; i++)
		draw.Text_RGB(Art_Pos(p + [8, 6 + i * lh]), lines[i], i == 0i ? [1, 0.85, 0.5] : [0.85, 0.85, 0.85], g_hudFont);
}

/* --- housing ------------------------------------------------------------- */

vector
SHH_SubOrigin(void)
{
	vector o = SHH_Origin(shh_resHousing, "ZShelterTeamHousingDialog");
	float colX = Res_Get(shh_resHousing, sprintf("RoundGrayBgImagePanel_%g", shh_subCol + 5), "xpos") != ""
		? stof(Res_Get(shh_resHousing, sprintf("RoundGrayBgImagePanel_%g", shh_subCol + 5), "xpos")) : 43;
	vector so = o + [colX + 192, 85];

	/* the last column opens to its left (DESIGN) */
	if (so[0] + 211 > Board_CX() * 2)
		so[0] = o[0] + colX - 215;
	return so;
}

/* which housing column a main button is in */
float
SHH_Column(int k)
{
	float x = stof(Res_Get(shh_resHousing, sprintf("BoxImagePanel_%i", k), "xpos"));
	return floor((x - 43) / 196);
}

void
SHH_DrawHousing(void)
{
	hashtable t = shh_resHousing;
	string fr = "ZShelterTeamHousingDialog";
	vector fs = Res_Size(t, fr);
	vector o = SHH_Origin(t, fr);

	SHH_Frame(t, o, fr);
	SHH_Label(t, o, fs, "TitleLabel", [1,1,1]);
	for (int i = 1i; i <= 8i; i++)
		SHH_Round(t, o, fs, sprintf("RoundGrayBgImagePanel_%i", i));
	for (int i = 1i; i <= 4i; i++)
		SHH_Label(t, o, fs, sprintf("CategoryLabel_%i", i), [0.64, 0.75, 0.86]);

	for (int k = 1i; k <= 12i; k++) {
		int r = SHH_Row(k - 1i);
		bool ok = SHH_RowOk(r);
		bool open = (shh_sub > 0 && r >= 0i && strstrofs(shh_mCmd[r], sprintf("zsht_opensubmenu %g ", shh_sub)) == 0i);

		SHH_Image(t, o, fs, sprintf("BoxImagePanel_%i", k), "", 1.0f);
		SHH_Image(t, o, fs, ok ? sprintf("BtnImagePanel_n_%i", k) : sprintf("BtnImagePanel_d_%i", k), "", 1.0f);
		if (ok && (open || SHH_Hover(t, o, fs, sprintf("BtnImagePanel_n_%i", k))))
			drawfill(Art_Pos(Res_CtlPos(t, o, fs, sprintf("BtnImagePanel_n_%i", k))),
				Res_Size(t, sprintf("BtnImagePanel_n_%i", k)) * g_hs, [1, 0.9, 0.6], 0.15, 0);
		SHH_Text(t, o, fs, sprintf("BtnLabel_%i", k), r >= 0i ? Res_Text(shh_mName[r]) : "", ok ? [1,1,1] : [0.55,0.55,0.55]);

		if (r < 0i)
			continue;
		if (SHH_RowDone(r))
			SHH_Image(t, o, fs, sprintf("CompleteImagePanel_%i", k), "", 1.0f);
		else if (shh_mWood[r] + shh_mIron[r] + shh_mPow[r] > 0)
			SHH_Costs(t, o, fs, k, shh_mWood[r], shh_mIron[r], shh_mPow[r]);

		if (SHH_Hover(t, o, fs, sprintf("BoxImagePanel_%i", k)))
			shh_tip = ok ? shh_mTip[r] : shh_mTipOff[r];
	}
	Res_DrawButton(t, o, fs, "Close", Res_Text(Res_Get(t, "Close", "labelText")), true, false);

	if (shh_sub <= 0)
		return;

	/* the sub menu */
	t = shh_resSub;
	fr = "ZShelterTeamHousingSubDialog";
	fs = Res_Size(t, fr);
	o = SHH_SubOrigin();
	SHH_Frame(t, o, fr);
	/* the build has no CSO_zsht_HousingSubTitleLabel string (OBSERVED);
	   the category button's name stands in (DESIGN) */
	string st = Loc("CSO_zsht_HousingSubTitleLabel", "");
	for (int i = 0i; st == "" && i < shh_mCount; i++)
		if (shh_mSub[i] == 0 && strstrofs(shh_mCmd[i], sprintf("zsht_opensubmenu %g ", shh_sub)) == 0i)
			st = Res_Text(shh_mName[i]);
	SHH_Text(t, o, fs, "TitleLabel", st, [1,1,1]);
	SHH_Round(t, o, fs, "RoundGrayBgImagePanel");
	for (int k = 1i; k <= 6i; k++) {
		int r = SHH_SubRow(shh_sub, k - 1i);
		if (r < 0i)
			continue;
		bool ok = SHH_RowOk(r);
		string btn = sprintf("BtnImagePanel_n_%i", k);

		SHH_Image(t, o, fs, sprintf("BoxImagePanel_%i", k), "", 1.0f);
		SHH_Image(t, o, fs, ok ? btn : sprintf("BtnImagePanel_d_%i", k), "", 1.0f);
		if (ok && SHH_Hover(t, o, fs, btn))
			drawfill(Art_Pos(Res_CtlPos(t, o, fs, btn)), Res_Size(t, btn) * g_hs, [1, 0.9, 0.6], 0.15, 0);
		SHH_Text(t, o, fs, sprintf("BtnLabel_%i", k), Res_Text(shh_mName[r]), ok ? [1,1,1] : [0.55,0.55,0.55]);
		if (SHH_RowDone(r))
			SHH_Image(t, o, fs, sprintf("CompleteImagePanel_%i", k), "", 1.0f);
		else
			SHH_Costs(t, o, fs, k, shh_mWood[r], shh_mIron[r], shh_mPow[r]);
		if (SHH_Hover(t, o, fs, sprintf("BoxImagePanel_%i", k)))
			shh_tip = ok ? shh_mTip[r] : shh_mTipOff[r];
	}
}

/* runs a build menu row's command */
void
SHH_RunRow(int r, float col)
{
	tokenize(shh_mCmd[r]);
	if (strtolower(argv(0)) == "zsht_opensubmenu") {
		float sub = stof(argv(1));
		shh_sub = (shh_sub == sub) ? 0 : sub;
		shh_subCol = col;
		return;
	}
	localcmd(sprintf("cmd %s\n", shh_mCmd[r]));
	g_shhOpen = 0i;
	shh_sub = 0;
}

bool
SHH_ClickHousing(void)
{
	hashtable t = shh_resHousing;
	string fr = "ZShelterTeamHousingDialog";
	vector fs = Res_Size(t, fr);
	vector o = SHH_Origin(t, fr);

	if (shh_sub > 0) {
		vector so = SHH_SubOrigin();
		vector sfs = Res_Size(shh_resSub, "ZShelterTeamHousingSubDialog");
		for (int k = 1i; k <= 6i; k++) {
			int r = SHH_SubRow(shh_sub, k - 1i);
			if (r >= 0i && SHH_Hover(shh_resSub, so, sfs, sprintf("BoxImagePanel_%i", k))) {
				if (SHH_RowOk(r))
					SHH_RunRow(r, shh_subCol);
				return true;
			}
		}
	}

	for (int k = 1i; k <= 12i; k++) {
		int r = SHH_Row(k - 1i);
		if (r >= 0i && SHH_Hover(t, o, fs, sprintf("BoxImagePanel_%i", k))) {
			if (SHH_RowOk(r))
				SHH_RunRow(r, SHH_Column(k));
			return true;
		}
	}
	if (SHH_Hover(t, o, fs, "Close")) {
		g_shhOpen = 0i;
		shh_sub = 0;
	}
	return true;
}

/* --- worktable (DESIGN body in the OBSERVED frame) ----------------------- */

vector
SHH_CraftPos(int w)
{
	int col = (int)(shh_wSkill[w] - 27);
	int row = 0i;

	for (int i = 0i; i < w; i++)
		if (shh_wSkill[i] == shh_wSkill[w])
			row++;
	return [22 + col * 226, 84 + row * 84];
}

void
SHH_DrawWorktable(void)
{
	hashtable t = shh_resUpgrade;
	string fr = "ZShelterTeamUpgradeDlg";
	vector fs = Res_Size(t, fr);
	vector o = SHH_Origin(t, fr);
	string art[3] = {"combat", "survival", "tech"};
	float wood = SHH_Mine(0), iron = SHH_Mine(1);

	SHH_Frame(t, o, fr);
	SHH_Label(t, o, fs, "TitleLabel", [1,1,1]);
	for (int c = 0i; c < 3i; c++) {
		vector hp = o + [22 + c * 226, 50];
		drawfill(Art_Pos(hp), [210, 24] * g_hs, [0.12, 0.12, 0.12], 0.75, 0);
		string cat = Loc(sprintf("CSO_zsht_Upgrade_Category_%i", c + 1i), "");
		draw.Text_RGB(Art_Pos(hp + [(210 - font.StringWidth(cat, false, g_hudFont) / g_hs) / 2, 6]), cat, [0.64, 0.75, 0.86], g_hudFont);
	}

	for (int w = 0i; w < shh_wCount; w++) {
		vector p = o + SHH_CraftPos(w);
		int c = (int)(shh_wSkill[w] - 27);
		bool ok = SHH_SkillLv(shh_wSkill[w]) > 0;
		bool afford = ok && wood >= shh_wWood[w] && iron >= shh_wIron[w];

		if (c < 0i || c > 2i)
			continue;
		SHH_Pic(p, SHH_ART "zsht_upgrade_btn_blackbg", [196, 74], 1.0f);
		SHH_Pic(p + [3, 3], ok ? strcat(SHH_ART "zsht_upgrade_btn_", art[c], "@n") : SHH_ART "zsht_upgrade_btn@d", [190, 48], 1.0f);
		if (afford && Res_MouseIn(p, [196, 74]))
			drawfill(Art_Pos(p + [3, 3]), [190, 48] * g_hs, [1, 0.9, 0.6], 0.15, 0);
		draw.Text_RGB(Art_Pos(p + [12, 20]), Res_Text(shh_wName[w]), ok ? [1,1,1] : [0.55,0.55,0.55], g_hudFont);
		SHH_Pic(p + [8, 53], SHH_ART "zsht_icon_wood", [18, 18], 1.0f);
		draw.Text_RGB(Art_Pos(p + [28, 56]), ftos(shh_wWood[w]), wood >= shh_wWood[w] ? [0.85,0.8,0.6] : [1,0.4,0.4], g_hudFont);
		SHH_Pic(p + [70, 53], SHH_ART "zsht_icon_iron", [18, 18], 1.0f);
		draw.Text_RGB(Art_Pos(p + [90, 56]), ftos(shh_wIron[w]), iron >= shh_wIron[w] ? [0.85,0.8,0.6] : [1,0.4,0.4], g_hudFont);
		if (Res_MouseIn(p, [196, 74]))
			shh_tip = shh_wTip[w];
	}

	vector cp = o + [fs[0] - 120, fs[1] - 46];
	Res_Button3("resource/control/button/btn_default", Res_MouseIn(cp, [98, 32]) ? "o" : "n", cp, [98, 32], 1.0f);
	string cl = Loc("CSO_Bulletin_Close", "Close");
	draw.Text_RGB(Art_Pos(cp + [(98 - font.StringWidth(cl, false, g_hudFont) / g_hs) / 2, 9]), cl, [1,1,1], g_hudFont);
}

bool
SHH_ClickWorktable(void)
{
	vector fs = Res_Size(shh_resUpgrade, "ZShelterTeamUpgradeDlg");
	vector o = SHH_Origin(shh_resUpgrade, "ZShelterTeamUpgradeDlg");

	for (int w = 0i; w < shh_wCount; w++)
		if (Res_MouseIn(o + SHH_CraftPos(w), [196, 74])) {
			localcmd(sprintf("cmd zsht_worktable %i\n", w));
			return true;
		}
	if (Res_MouseIn(o + [fs[0] - 120, fs[1] - 46], [98, 32]))
		g_shhOpen = 0i;
	return true;
}

/* --- warehouse ----------------------------------------------------------- */

string shh_whBtns = "Wood_Plus5_btn_1 Wood_Plus1_btn_1 Iron_Plus5_btn_1 Iron_Plus1_btn_1 Wood_Plus5_btn_2 Wood_Plus1_btn_2 Iron_Plus5_btn_2 Iron_Plus1_btn_2 Close";

void
SHH_DrawWarehouse(void)
{
	hashtable t = shh_resWarehouse;
	string fr = "ZShelterTeamWarehouseDialog";
	vector fs = Res_Size(t, fr);
	vector o = SHH_Origin(t, fr);
	vector c = [0.85, 0.8, 0.6];

	SHH_Frame(t, o, fr);
	SHH_Label(t, o, fs, "TitleLabel", [1,1,1]);
	drawfill(Art_Pos(Res_CtlPos(t, o, fs, "BlackBg_1")), Res_Size(t, "BlackBg_1") * g_hs, [0,0,0], 0.6, 0);
	drawfill(Art_Pos(Res_CtlPos(t, o, fs, "BlackBg_2")), Res_Size(t, "BlackBg_2") * g_hs, [0,0,0], 0.6, 0);
	/* the arrows share a spot: up moves to the warehouse (DESIGN: both
	   drawn faintly, as the direction depends on the button pressed) */
	SHH_Image(t, o, fs, "arrow_up", "", 0.6f);
	SHH_Label(t, o, fs, "WarehouseLabel", [1,1,1]);
	SHH_Label(t, o, fs, "IndividualLabel", [1,1,1]);
	for (int i = 1i; i <= 4i; i++)
		SHH_Image(t, o, fs, sprintf("RoundBox_%i", i), "", 1.0f);
	for (int i = 1i; i <= 2i; i++) {
		SHH_Image(t, o, fs, sprintf("wood_icon_%i", i), "", 1.0f);
		SHH_Image(t, o, fs, sprintf("iron_icon_%i", i), "", 1.0f);
	}
	SHH_Text(t, o, fs, "woodLabel_1", sprintf("%d/%d", SHH_TeamRes(0), SHH_TeamRes(2)), c);
	SHH_Text(t, o, fs, "ironLabel_1", sprintf("%d/%d", SHH_TeamRes(1), SHH_TeamRes(3)), c);
	SHH_Text(t, o, fs, "woodLabel_2", sprintf("%d/%d", SHH_Mine(0), SHH_Mine(2)), c);
	SHH_Text(t, o, fs, "ironLabel_2", sprintf("%d/%d", SHH_Mine(1), SHH_Mine(3)), c);

	int n = (int)tokenize(shh_whBtns);
	string b[9];
	for (int i = 0i; i < n; i++)
		b[i] = argv(i);
	g_resHoverCtl = strtolower(Res_HitTest(t, o, fs, shh_whBtns));
	for (int i = 0i; i < n; i++)
		Res_DrawButton(t, o, fs, b[i], Res_Text(Res_Get(t, b[i], "labelText")), true, false);
}

bool
SHH_ClickWarehouse(void)
{
	hashtable t = shh_resWarehouse;
	string fr = "ZShelterTeamWarehouseDialog";
	string ctl = Res_HitTest(t, SHH_Origin(t, fr), Res_Size(t, fr), shh_whBtns);

	if (ctl == "")
		return true;
	if (ctl == "Close") {
		g_shhOpen = 0i;
		return true;
	}
	localcmd(sprintf("cmd %s\n", Res_Get(t, ctl, "command")));
	return true;
}

/* --- rescue -------------------------------------------------------------- */

void
SHH_DrawRescue(void)
{
	hashtable t = shh_resRescue;
	string fr = "ZShelterTeamRescueMenu";
	vector fs = Res_Size(t, fr);
	vector o = SHH_Origin(t, fr);
	vector c = [0.85, 0.85, 0.85];
	float windowEnd = SHH_Field(serverkey("zsht_rescue"), 1);

	SHH_Frame(t, o, fr);
	SHH_Label(t, o, fs, "TitleLabel", [1,1,1]);
	drawfill(Art_Pos(Res_CtlPos(t, o, fs, "BlackBgImagePanel")), Res_Size(t, "BlackBgImagePanel") * g_hs, [0,0,0], 0.6, 0);
	SHH_Round(t, o, fs, "RoundPanelBg");
	SHH_Image(t, o, fs, "RescueImage", "", 1.0f);
	for (int i = 1i; i <= 4i; i++)
		SHH_Label(t, o, fs, sprintf("RescueDetailLabel%i", i), c);
	SHH_Label(t, o, fs, "RescueLabel", [1,1,1]);
	SHH_Text(t, o, fs, "RescueUserInfo", SHH_Fmt(Loc("CSO_zsht_RescueUserInfo", "%d/%d"),
		ftos(SHH_Field(serverkey("zsht_rescueask"), 0)), ftos(SHH_Field(serverkey("zsht_rescueask"), 1))), [1,1,1]);
	if (windowEnd > time)
		SHH_Text(t, o, fs, "RescueRemainTime", SHH_Fmt(Loc("CSO_zsht_RescueRemainTime", "Time until requesting ends: %d sec."),
			ftos(ceil(windowEnd - time)), ""), [1, 0.85, 0.5]);
	g_resHoverCtl = strtolower(Res_HitTest(t, o, fs, "RequestRescue Close"));
	Res_DrawButton(t, o, fs, "RequestRescue", Res_Text(Res_Get(t, "RequestRescue", "labelText")), true, false);
	Res_DrawButton(t, o, fs, "Close", Res_Text(Res_Get(t, "Close", "labelText")), true, false);
}

bool
SHH_ClickRescue(void)
{
	hashtable t = shh_resRescue;
	string fr = "ZShelterTeamRescueMenu";
	string ctl = Res_HitTest(t, SHH_Origin(t, fr), Res_Size(t, fr), "RequestRescue Close");

	if (ctl == "RequestRescue")
		localcmd("cmd zsht_RequestRescue\n");
	if (ctl != "")
		g_shhOpen = 0i;
	return true;
}

/* --- skill tree ---------------------------------------------------------- */

/* the skill in slot k (1-36), -1 for none */
int
SHH_SkillAt(int k)
{
	for (int s = 0i; s < shh_sCount; s++)
		if (shh_sImg[s] != "" && shh_sPos[s] + 1 == k)
			return s;
	return -1i;
}

void
SHH_DrawSkills(void)
{
	hashtable t = shh_resSkill;
	string fr = "ZShelterTeamSkillTreeDlg";
	vector fs = Res_Size(t, fr);
	vector o = SHH_Origin(t, fr);
	float catPts[3] = {0, 0, 0};
	float tier = SHH_TeamRes(6);

	SHH_Frame(t, o, fr);
	SHH_Label(t, o, fs, "TitleLabel", [1,1,1]);
	drawfill(Art_Pos(Res_CtlPos(t, o, fs, "BlackBgImagePanel")), Res_Size(t, "BlackBgImagePanel") * g_hs, [0,0,0], 0.6, 0);
	SHH_Label(t, o, fs, "skillTotalpointLabel_1", [1,1,1]);
	SHH_Text(t, o, fs, "skillTotalpointLabel_2", ftos(SHH_Mine(4)), [1, 0.85, 0.5]);
	for (int i = 1i; i <= 6i; i++)
		SHH_Round(t, o, fs, sprintf("RoundGrayBgImagePanel_%i", i));

	for (int s = 0i; s < shh_sCount; s++)
		if (shh_sCat[s] >= 0 && shh_sCat[s] < 3)
			catPts[(int)shh_sCat[s]] += SHH_SkillLv(s);

	for (int i = 1i; i <= 3i; i++) {
		SHH_Image(t, o, fs, sprintf("CategoryPointBG_%i", i), "", 1.0f);
		SHH_Label(t, o, fs, sprintf("CategoryLabel_%i", i), [0.64, 0.75, 0.86]);
		SHH_Text(t, o, fs, sprintf("CategoryPointLabel_%i", i), ftos(catPts[i - 1i]), [1,1,1]);
	}
	for (int i = 1i; i <= 4i; i++) {
		SHH_Image(t, o, fs, sprintf("Tab_Tier_%i", i), i <= tier ? SHH_ART "zsht_st_tab@sel" : SHH_ART "zsht_st_tab@n", 1.0f);
		SHH_Label(t, o, fs, sprintf("Tier_Label_%i", i), [1,1,1]);
		SHH_Label(t, o, fs, sprintf("TierNumber_Label_%i", i), [1,1,1]);
	}

	for (int k = 1i; k <= 36i; k++) {
		int s = SHH_SkillAt(k);
		if (s < 0i)
			continue;
		float lv = SHH_SkillLv(s);
		bool open = (shh_sTier[s] <= tier);
		string btn = sprintf("BtnImage_n_%i", k);

		drawfill(Art_Pos(Res_CtlPos(t, o, fs, sprintf("btnblackBG_%i", k))), Res_Size(t, sprintf("btnblackBG_%i", k)) * g_hs, [0,0,0], 0.7, 0);
		SHH_Image(t, o, fs, btn, strcat(SHH_ART "zsht_st_btn_", shh_sImg[s], (lv > 0 || open) ? "@n" : "@d"), (lv > 0 || open) ? 1.0f : 0.8f);
		if (lv > 0)
			SHH_Image(t, o, fs, sprintf("BtnImage_c_%i", k), "", 1.0f);
		else if (open && SHH_Hover(t, o, fs, btn))
			SHH_Image(t, o, fs, btn, SHH_ART "zsht_st_btn@o", 1.0f);
		/* an arrow down to the next tier's skill in the same column */
		if (SHH_SkillAt(k + 3i) >= 0i && ((k - 1i) % 12i) < 9i)
			SHH_Image(t, o, fs, sprintf("zsht_st_arrowimg_%i", k), "", 1.0f);
		SHH_Image(t, o, fs, sprintf("zsht_st_skillpointbox_%i", k), SHH_ART "zsht_st_skillpointbox", 1.0f);
		SHH_Text(t, o, fs, sprintf("skillpointLabel_%i", k), sprintf("%d", lv), lv >= shh_sMax[s] ? [1, 0.85, 0.5] : [1,1,1]);
		if (SHH_Hover(t, o, fs, btn))
			shh_tip = sprintf("#CSO_zsht_tooltip_skill_%s", shh_sImg[s]);
	}

	g_resHoverCtl = strtolower(Res_HitTest(t, o, fs, "SkillInit Close"));
	Res_DrawButton(t, o, fs, "SkillInit", Res_Text(Res_Get(t, "SkillInit", "labelText")), true, false);
	Res_DrawButton(t, o, fs, "Close", Res_Text(Res_Get(t, "Close", "labelText")), true, false);
}

bool
SHH_ClickSkills(void)
{
	hashtable t = shh_resSkill;
	string fr = "ZShelterTeamSkillTreeDlg";
	vector fs = Res_Size(t, fr);
	vector o = SHH_Origin(t, fr);
	string ctl = Res_HitTest(t, o, fs, "SkillInit Close");

	if (ctl == "SkillInit") {
		localcmd("cmd zsht_SkillInit\n");
		return true;
	}
	if (ctl == "Close") {
		g_shhOpen = 0i;
		return true;
	}
	for (int k = 1i; k <= 36i; k++) {
		int s = SHH_SkillAt(k);
		if (s >= 0i && SHH_Hover(t, o, fs, sprintf("BtnImage_n_%i", k))) {
			localcmd(sprintf("cmd zsht_RequestSkillUp %i\n", s));
			return true;
		}
	}
	return true;
}

/* --- always-on HUD ------------------------------------------------------- */

void
SHH_DrawBoard(void)
{
	float x0 = Board_CX() - 208;
	vector c = [1, 0.9, 0.7];
	float day = SHH_Field(serverkey("zsht_clock"), 0);
	float state = SHH_Field(serverkey("zsht_clock"), 1);
	float end = SHH_Field(serverkey("zsht_clock"), 2);
	float heli = SHH_Field(serverkey("zsht_rescue"), 2);

	Art_Pic([x0, 0], SHH_ART "zsht_board_pve", 1.0f);
	VK_Text([x0, 42], [70, 15], sprintf("%d/%d", SHH_TeamRes(0), SHH_TeamRes(2)), 4i, c);
	VK_Text([x0 + 70, 42], [70, 15], sprintf("%d/%d", SHH_TeamRes(1), SHH_TeamRes(3)), 4i, c);
	VK_Text([x0 + 140, 42], [69, 15], sprintf("%d/%d", SHH_TeamRes(4), SHH_TeamRes(5)), 4i, c);
	/* centre cell: the shelter tier */
	VK_Text([x0 + 210, 12], [50, 15], Loc("CSO_zsht_TierLabel", "Tier"), 4i, [0.8,0.8,0.8]);
	Art_NumC([x0 + 235, 30], NUM_BIGWHITE, ftos(SHH_TeamRes(6)), [1,1,1], 1.0f);
	VK_Text([x0 + 262, 42], [152, 15], serverkey("zsht_zombies"), 4i, [1, 0.5, 0.5]);

	/* the timer strip: sun or moon, the day and the time left */
	vector tp = [Board_CX() - 98, 66];
	float left = max(0, end - time);
	string clock;

	Art_Pic(tp, SHH_ART "zsht_ingame_timertext_bg", 1.0f);
	if (heli > 0) {
		Art_Pic(tp + [-60, 0], SHH_ART "zsht_ingame_escape_icon", 1.0f);
		left = max(0, heli - time);
	} else {
		Art_Pic(tp + [-60, 0], state == 2 ? SHH_ART "zsht_ingame_moon_icon" : SHH_ART "zsht_ingame_sun_icon", 1.0f);
	}
	clock = sprintf("Day %d   %d:%02d", day, floor(left / 60), floor(left) % 60);
	VK_Text(tp + [0, 6], [197, 15], clock, 4i, state == 2 ? [0.7, 0.8, 1] : [1, 0.95, 0.8]);

	/* the barricade */
	float bhp = SHH_Field(serverkey("zsht_barricade"), 0);
	float bmax = SHH_Field(serverkey("zsht_barricade"), 1);
	if (bmax > 0) {
		vector bp = [Board_CX() - 98, 96];
		drawfill(Art_Pos(bp), [197, 6] * g_hs, [0.15, 0.15, 0.15], 0.8, 0);
		drawpic(Art_Pos(bp), SHH_ART "zsht_gauge", [197 * bound(0, bhp / bmax, 1), 6] * g_hs, [1,1,1], 1.0, 0);
	}
}

void
SHH_DrawGauge(void)
{
	vector p = [16, HUD_H - 168];
	vector c = [1, 0.9, 0.7];

	Art_Pic(p, SHH_ART "zsht_ingame_gungergauge_bg", 1.0f);
	/* the mental gauge, full (see the top of the file) */
	drawfill(Art_Pos(p + [36, 1]), [160, 5] * g_hs, [0.85, 0.75, 0.4], 0.6, 0);
	VK_Text(p + [52, 22], [80, 15], sprintf("%d/%d", SHH_Mine(0), SHH_Mine(2)), 4i, c);
	VK_Text(p + [140, 22], [80, 15], sprintf("%d/%d", SHH_Mine(1), SHH_Mine(3)), 4i, c);
	if (SHH_Mine(4) > 0)
		VK_Text(p + [0, -18], [239, 15], sprintf("%s %d", Loc("CSO_zsht_skillTotalpointLabel_1", "Points:"), SHH_Mine(4)), 0i, [1, 0.85, 0.5]);

	/* key hints */
	vector kp = [-220, HUD_H - 230];
	Art_Pic(kp, SHH_ART "ingame_housingkey", 1.0f);
	VK_Text(kp + [52, 15], [160, 15], Loc("CSO_zsht_HousingTitleLabel", "Build"), 0i, [0.9,0.9,0.9]);
	Art_Pic(kp + [0, 50], SHH_ART "ingame_skillkey", 1.0f);
	VK_Text(kp + [52, 65], [160, 15], Loc("CSO_zsht_SkillTreeTitleLabel", "Skills"), 0i, [0.9,0.9,0.9]);
}

void
SHH_DrawPlacing(void)
{
	vector p = [Board_CX() - 170, HUD_H - 250];

	drawfill(Art_Pos(p - [8, 8]), [356, 68] * g_hs, [0, 0, 0], 0.5, 0);
	Art_Pic(p, SHH_ART "ingame_mouse_left", 1.0f);
	VK_Text(p + [50, 18], [60, 15], "Build", 0i, [1,1,1]);
	Art_Pic(p + [120, 3], SHH_ART "ingame_rkey", 1.0f);
	VK_Text(p + [170, 18], [60, 15], "Rotate", 0i, [1,1,1]);
	Art_Pic(p + [236, 0], SHH_ART "ingame_mouse_right", 1.0f);
	VK_Text(p + [286, 18], [60, 15], "Cancel", 0i, [1,1,1]);
}

void
SHH_Draw(void)
{
	if (!SHH_Active()) {
		g_shhOpen = 0i;
		return;
	}
	if (!shh_loaded)
		SHH_LoadTables();

	shh_tip = "";
	SHH_DrawBoard();
	if (player.GetHealth() > 0) {
		SHH_DrawGauge();
		if (SHH_Placing() > 0 && g_shhOpen == 0i)
			SHH_DrawPlacing();
	}

	switch (g_shhOpen) {
	case 1i: SHH_DrawHousing(); break;
	case 2i: SHH_DrawWorktable(); break;
	case 3i: SHH_DrawWarehouse(); break;
	case 4i: SHH_DrawRescue(); break;
	case 5i: SHH_DrawSkills(); break;
	}
	if (g_shhOpen && shh_tip != "")
		SHH_DrawTip(shh_tip);
}

/* the board replaces the standard one in this mode */
bool
SHH_OwnsBoard(void)
{
	return SHH_Active();
}

bool
SHH_Input(float evType, float key, float chr, float dev)
{
	if (!SHH_Active())
		return false;

	if (g_shhOpen) {
		if (evType == IE_MOUSEABS || evType == IE_MOUSEDELTA)
			return true;
		if (evType == IE_KEYDOWN && key == K_ESCAPE) {
			if (shh_sub > 0)
				shh_sub = 0;
			else
				g_shhOpen = 0i;
			return true;
		}
		if (evType == IE_KEYDOWN && ((key == 'b' && g_shhOpen == 1i) || (key == 'n' && g_shhOpen == 5i))) {
			g_shhOpen = 0i;
			shh_sub = 0;
			return true;
		}
		if (evType == IE_KEYUP && key == K_MOUSE1) {
			switch (g_shhOpen) {
			case 1i: return SHH_ClickHousing();
			case 2i: return SHH_ClickWorktable();
			case 3i: return SHH_ClickWarehouse();
			case 4i: return SHH_ClickRescue();
			case 5i: return SHH_ClickSkills();
			}
		}
		return (evType == IE_KEYDOWN || evType == IE_KEYUP) && (key == K_MOUSE1 || key == K_MOUSE2);
	}

	if (player.GetHealth() <= 0 || evType != IE_KEYDOWN)
		return false;

	if (SHH_Placing() > 0) {
		switch (key) {
		case K_MOUSE1:
			localcmd("cmd zsht_BuildConfirm\n");
			return true;
		case K_MOUSE2:
		case K_ESCAPE:
			localcmd("cmd zsht_BuildCancel\n");
			return true;
		case 'r':
			localcmd("cmd zsht_rotate\n");
			return true;
		}
	}

	if (key == 'n') {
		g_shhOpen = 5i;
		return true;
	}
	return false;
}

bool
SHH_WantsCursor(void)
{
	return SHH_Active() && g_shhOpen != 0i;
}

bool
SHH_ConsoleCommand(string cmd)
{
	tokenize_console(cmd);

	switch (argv(0)) {
	case "buymenu":
	case "zsht_housing":
		if (!SHH_Active())
			return false;
		g_shhOpen = (g_shhOpen == 1i) ? 0i : 1i;
		shh_sub = 0;
		return true;
	case "zsht_skilltree":
		if (!SHH_Active())
			return false;
		g_shhOpen = (g_shhOpen == 5i) ? 0i : 5i;
		return true;
	case "zsht_open":
		if (!SHH_Active())
			return true;
		switch (argv(1)) {
		case "housing": g_shhOpen = 1i; break;
		case "worktable": g_shhOpen = 2i; break;
		case "warehouse": g_shhOpen = 3i; break;
		case "rescue": g_shhOpen = 4i; break;
		case "skilltree": g_shhOpen = 5i; break;
		}
		shh_sub = 0;
		return true;
	}
	return false;
}
