/* Score board (held Tab): Resource/UI/CSO_ScoreBoard.res, which
   client.dll names for CClientScoreBoardDialog (OBSERVED). The layout below
   is that file's (OBSERVED): a 630x430 frame centred on the screen, the
   server name at c-250,30, PlayerListTER at 30,45 and PlayerListCT at
   320,45 (280x340 each), PlayerListSPEC at 60,305 (560x90) and a row of
   buttons at y 390.

   The lists are SectionedListPanels whose columns client.dll builds in
   code: UNRESOLVED. The columns here are name, kills, deaths and ping with
   CSO's own words for them (CSO_Kill, CSO_DeathCount, CSO_Ping_Lable,
   OBSERVED strings; their use here is INFERRED). The section header uses
   CSO_ZombiTeam / CSO_HumanTeam in Zombi and Cstrike_ScoreBoard_Ter / _CT
   otherwise (OBSERVED strings).

   The buttons (vote kick, ban, friend, family invite and so on) act on
   Nexon accounts, so they are drawn disabled (explicit unavailable state). */

#define SB_W 630
#define SB_H 430

hashtable g_loc;

/* reads the few tokens the HUD needs from a VGUI2 localization file */
void
Loc_Load(string file, string prefixes)
{
	filestream fh = fopen(file, FILE_READ);
	string line;
	int np = (int)tokenize(prefixes);
	string pre[16];

	for (int i = 0i; i < np && i < 16i; i++)
		pre[i] = strzone(argv(i));

	if (fh < 0)
		return;

	while (1) {
		line = fgets(fh);

		/* a blank line is "" too, which is false: test for null only */
		if not (line)
			break;
		if (tokenize_console(line) < 2)
			continue;

		string key = argv(0);
		for (int i = 0i; i < np && i < 16i; i++) {
			if (substring(key, 0, strlen(pre[i])) == pre[i]) {
				hash_add(g_loc, strtolower(key), argv(1), EV_STRING);
				break;
			}
		}
	}

	fclose(fh);
}

string
Loc(string token, string fallback)
{
	string s = hash_get(g_loc, strtolower(token), "", EV_STRING);
	return (s != "") ? s : fallback;
}

void
Scores_Init(void)
{
	g_loc = hash_createtab(256, HASH_REPLACE);
	Loc_Load("resource/cso_na_en.utf8.txt",
		"CSO_Kill CSO_DeathCount CSO_Ping_Lable CSO_ZombiTeam CSO_HumanTeam CSO_Spectator CSO_VoteKick CSO_AddBanButton CSO_QuickKick CSO_SpectKick CSO_AddFriend CSO_ClanInvite CSO_VoiceBlock CSO_ShowInfo");
	Loc_Load("resource/cstrike_na_en.utf8.txt", "Cstrike_ScoreBoard_");
	/* names of the Nuclide base items that stand in for CSO weapons */
	Loc_Load("resource/base_english.txt", "WEAPON_");
}

/* a three-by-three frame from the scheme's window art (OBSERVED images) */
void
Scores_Frame(vector p, vector s)
{
	string b = "resource/control/frame/window_default_";
	vector tl = drawgetimagesize(strcat(b, "top_left"));
	vector br = drawgetimagesize(strcat(b, "bottom_right"));
	float l = tl[0], t = tl[1], r = br[0], bt = br[1];

	drawpic(Art_Pos(p), strcat(b, "top_left"), [l, t] * g_hs, [1,1,1], 1, 0);
	drawpic(Art_Pos(p + [l, 0]), strcat(b, "top_center"), [s[0] - l - r, t] * g_hs, [1,1,1], 1, 0);
	drawpic(Art_Pos(p + [s[0] - r, 0]), strcat(b, "top_right"), [r, t] * g_hs, [1,1,1], 1, 0);
	drawpic(Art_Pos(p + [0, t]), strcat(b, "center_left"), [l, s[1] - t - bt] * g_hs, [1,1,1], 1, 0);
	drawpic(Art_Pos(p + [l, t]), strcat(b, "center_center"), [s[0] - l - r, s[1] - t - bt] * g_hs, [1,1,1], 1, 0);
	drawpic(Art_Pos(p + [s[0] - r, t]), strcat(b, "center_right"), [r, s[1] - t - bt] * g_hs, [1,1,1], 1, 0);
	drawpic(Art_Pos(p + [0, s[1] - bt]), strcat(b, "bottom_left"), [l, bt] * g_hs, [1,1,1], 1, 0);
	drawpic(Art_Pos(p + [l, s[1] - bt]), strcat(b, "bottom_center"), [s[0] - l - r, bt] * g_hs, [1,1,1], 1, 0);
	drawpic(Art_Pos(p + s - [r, bt]), strcat(b, "bottom_right"), [r, bt] * g_hs, [1,1,1], 1, 0);
}

/* the players of one team; team -1 lists spectators */
int
Scores_Count(float team)
{
	int n = 0i;

	for (int i = 0i; i < maxclients; i++) {
		if (getplayerkeyvalue(i, "name") == "")
			continue;
		if (team < 0 ? getplayerkeyfloat(i, "*spectator") != 0 : (getplayerkeyfloat(i, "*team") == team && getplayerkeyfloat(i, "*spectator") == 0))
			n++;
	}

	return n;
}

void
Scores_List(vector p, vector s, float team, string title, vector col, font_s fnt)
{
	float rowH = 16;
	float th = font.GetHeight(fnt) / g_hs;
	string me = getplayerkeyvalue(player_localnum, "name");
	float cK = p[0] + s[0] - 110, cD = p[0] + s[0] - 64, cP = p[0] + s[0] - 22;
	int n = Scores_Count(team);
	float yEnd = p[1] + s[1];

	/* section header: "Team    -   n Player" (Cstrike_ScoreBoard_Players) */
	string head = strreplace("%s2", sprintf("%i", n), strreplace("%s1", title, Loc("Cstrike_ScoreBoard_Players", "%s1    -   %s2 Player")));
	draw.Text_RGB(Art_Pos(p), head, col, fnt);

	if (team >= 0) {
		draw.RText_RGB(Art_Pos([cK, p[1]]), Loc("CSO_Kill", "Kills"), col, fnt);
		draw.RText_RGB(Art_Pos([cD, p[1]]), Loc("CSO_DeathCount", "Death"), col, fnt);
		draw.RText_RGB(Art_Pos([cP, p[1]]), Loc("CSO_Ping_Lable", "Ping"), col, fnt);
	}

	p[1] += rowH + 2;
	drawfill(Art_Pos(p), [s[0], 1] * g_hs, col, 0.6f, 0);
	p[1] += 4;

	for (int i = 0i; i < maxclients && p[1] + rowH <= yEnd; i++) {
		string name = getplayerkeyvalue(i, "name");

		if (name == "")
			continue;
		if (team < 0 ? getplayerkeyfloat(i, "*spectator") == 0 : (getplayerkeyfloat(i, "*team") != team || getplayerkeyfloat(i, "*spectator") != 0))
			continue;

		vector rc = (name == me) ? [1, 1, 1] : col;

		if (name == me)
			drawfill(Art_Pos(p - [2, 1]), [s[0] + 4, rowH] * g_hs, col, 0.25f, 0);

		draw.Text_RGB(Art_Pos([p[0], p[1] + (rowH - th) / 2 - 1]), name, rc, fnt);
		if (team >= 0) {
			draw.RText_RGB(Art_Pos([cK, p[1]]), sprintf("%i", (int)getplayerkeyfloat(i, "frags")), rc, fnt);
			draw.RText_RGB(Art_Pos([cD, p[1]]), sprintf("%i", (int)getplayerkeyfloat(i, "*deaths")), rc, fnt);
			draw.RText_RGB(Art_Pos([cP, p[1]]), getplayerkeyvalue(i, "ping"), rc, fnt);
		}
		p[1] += rowH;
	}
}

void
HUD_DrawScores(void)
{
	Art_Frame();

	font_s fnt = g_hudFont;
	vector o = [Board_CX() - SB_W / 2, HUD_H / 2 - SB_H / 2];
	bool zombi = serverkey("csnz_mode") == "8";
	/* Counter-Strike's team colours (STOCK-CS) */
	vector red = [1, 0.25, 0.25], blue = [0.6, 0.8, 1];

	Scores_Frame(o, [SB_W, SB_H]);

	/* ServerLabel c-250,30 500x20 */
	string host = serverkey("hostname");
	draw.Text(Art_Pos(o + [SB_W / 2 - font.StringWidth(host, true, fnt) / g_hs / 2, 30]), host, fnt);

	string t1 = zombi ? strreplace("(%d)", "", Loc("CSO_ZombiTeam", "Zombie Team")) : Loc("Cstrike_ScoreBoard_Ter", "Terrorist");
	string t2 = zombi ? strreplace("(%d)", "", Loc("CSO_HumanTeam", "Human Team")) : Loc("Cstrike_ScoreBoard_CT", "Counter-Terrorists");

	Scores_List(o + [30, 52], [280, 240], 1, strtrim(t1), red, fnt);
	Scores_List(o + [320, 52], [280, 240], 2, strtrim(t2), blue, fnt);

	if (Scores_Count(-1) > 0)
		Scores_List(o + [60, 305], [560, 70], -1, Loc("CSO_Spectator", "Spectator"), [0.8, 0.8, 0.8], fnt);

	/* buttons at y 390 (OBSERVED positions and words), disabled */
	string btns[8] = {"CSO_VoteKick", "CSO_AddBanButton", "CSO_QuickKick", "CSO_SpectKick", "CSO_AddFriend", "CSO_ClanInvite", "CSO_VoiceBlock", "CSO_ShowInfo"};
	float bx[8] = {8, 112, 215, 255, 328, 401, 474, 547};
	float bw[8] = {100, 100, 40, 70, 70, 70, 70, 70};

	for (int i = 0i; i < 8i; i++) {
		vector bp = o + [bx[i], 390];
		string w = Loc(btns[i], "");

		drawfill(Art_Pos(bp), [bw[i] - 3, 25] * g_hs, [0.2, 0.2, 0.2], 0.6f, 0);
		/* the English words are wider than some buttons: clipped, as a
		   VGUI2 label clips its text */
		vector cp = Art_Pos(bp);
		drawsetcliparea(cp[0], cp[1], (bw[i] - 3) * g_hs, 25 * g_hs);
		w = strreplace(" (%02d)", "", w);
		float tw = font.StringWidth(w, false, fnt) / g_hs;
		float tx = (tw > bw[i] - 9) ? 3 : (bw[i] - 3) / 2 - tw / 2;
		draw.Text_RGBA(Art_Pos(bp + [tx, 6]), w, [0.5, 0.5, 0.5], 1.0f, fnt);
		drawresetcliparea();
	}
}
