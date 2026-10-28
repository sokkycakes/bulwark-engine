/* Vote kick windows (handoff M3): CIGVoteMenu and the kick-call dialog.

   SOURCE-CONFIRMED (client.dll FUN_029d2920, the CIGVoteMenu constructor,
   and FUN_029d41f0, its layout):
   - The vote layout is a 355x356 frame titled #CSO_Vote_Title. It has a
     line at 12,38 (321x15), #CSO_VoteKick_KickUser at 12,61 (74x15) with
     the name at 89,61 (240x15), and #CSO_VoteKick_Reason at 12,82 (74x15)
     with the reason at 89,82 (240x64). There is a 321x30 line at 12,158,
     #CSO_Vote_Kick_Info at 12,275 (321x15), and the keys at 12,310 /
     121,310 / 228,310 (105x15, left / centred / right). The keys are
     "F3: Agree", "F4: Disagree" and "ESC: Withdraw", built from
     #CSO_Vote_Yes, #CSO_Vote_No and #CSO_Vote_Cancel around
     #CSO_VoteYes2, #CSO_VoteNo2 and #CSO_Vote_Giveup. Labels are c0c0c0,
     and the kick-user row is 9eaccc.
   - The notice layout (for the player being voted on) is a 357x327 frame
     titled #CSO_Notice_Title, with #CSO_Vote_Notice_Title and
     #CSO_Vote_Notice_Info, and "ESC: Close" (#CSO_ClosePopup). Its
     defense text entry is not ported.
   - The kick-call dialog is titled #CSO_VoteKick_Title and sends
     "votekick2" with a player index (FUN_029fc550). Its layout lives in
     code this port hasn't traced, so here it is DESIGN: a player list, the
     four #CSO_VoteReason texts, and OK / Cancel. It opens with "votekick"
     (no arguments) and sends "cmd votekick <entity number> <reason>".

   DESIGN: the window sits at the right edge, centred vertically, and
   doesn't take the mouse. The 12,38 line shows #CSO_Vote_Kick_Title with
   the proposer. The 12,158 line shows the yes / no counts. Esc withdraws
   for the proposer and hides the window for everyone else. */

bool g_vkHidden;		/* Esc hid the window for this vote */
float g_vkShownEnd;
bool g_vkCallOpen;
var int g_vkCallPlayer = -1i;	/* player number (0-based) */
int g_vkCallReason;

void
VK_Init(void)
{
	Loc_Load("resource/cso_na_en.utf8.txt",
		"CSO_Vote_ CSO_VoteKick_ CSO_VoteYes2 CSO_VoteNo2 CSO_VoteReason CSO_Notice_Title CSO_ClosePopup");
	registercommand("votekick");
}

bool
VK_Active(void)
{
	return serverkeyfloat("csnz_vote_end") > time && serverkey("csnz_vote_target") != "";
}

/* the rules publish the reason's number; the text is the client's own
   (cso_na_en.txt defines CSO_VoteReason0-3 twice, OBSERVED) */
string
VK_ReasonText(void)
{
	int r = stoi(serverkey("csnz_vote_reason"));

	return Loc(sprintf("CSO_VoteReason%i", r), sprintf("Reason %i", r + 1i));
}

/* "F3: %s" and the like */
string
VK_Key(string fmt, string fmtFallback, string word, string wordFallback)
{
	return strreplace("%s", Loc(word, wordFallback), Loc(fmt, fmtFallback));
}

vector
VK_Origin(vector size)
{
	vector area = screen.HUDSize() / g_hs;

	return [area[0] - size[0] - 16, (area[1] - size[1]) / 2];
}

void
VK_Text(vector p, vector s, string txt, int align, vector rgb)
{
	float w = font.StringWidth(txt, false, g_hudFont) / g_hs;

	if (align == 4i)
		p[0] += (s[0] - w) / 2;
	else if (align == 5i)
		p[0] += s[0] - w;
	draw.Text_RGB(Art_Pos(p), txt, rgb, g_hudFont);
}

/* a label and its value; the value moves right when the label is wider
   than its 74-unit box (the HUD font isn't CSO's) */
void
VK_Row(vector o, vector lp, vector vp, vector vs, string label, string value, vector rgb)
{
	float w = font.StringWidth(label, false, g_hudFont) / g_hs;

	VK_Text(o + lp, [74, 15], label, 0i, rgb);
	VK_Text(o + [max(vp[0], lp[0] + w + 6), vp[1]], vs, value, 0i, rgb);
}

void
VK_DrawVote(void)
{
	vector c0 = [0xc0, 0xc0, 0xc0] / 255;
	vector cu = [0x9e, 0xac, 0xcc] / 255;
	vector ck = [0xa5, 0xa5, 0xa5] / 255;
	bool amTarget = (stof(serverkey("csnz_vote_targetnum")) == player_localnum + 1);
	string by = serverkey("csnz_vote_by");
	int ballot = stoi(getplayerkeyvalue(player_localnum, "csnz_vote_ballot"));
	bool amProposer = (by != "" && by == getplayerkeyvalue(player_localnum, "name"));

	vector o;

	if (amTarget) {
		o = VK_Origin([357, 327]);
		Res_Frame9("resource/control/igframe/window_default_", o, [357, 327], 1.0f);
		draw.Text_RGB(Art_Pos(o + [12, 8]), Loc("CSO_Notice_Title", "Vote"), [1,1,1], g_hudFont);
		VK_Text(o + [17, 37], [313, 15], Loc("CSO_Vote_Notice_Title", "A vote against you will be started."), 0i, c0);
		VK_Row(o, [17, 109], [90, 109], [240, 15], Loc("CSO_VoteKick_Proposer", "Propose vote kick:"), by, c0);
		VK_Row(o, [17, 130], [90, 130], [240, 64], Loc("CSO_VoteKick_Reason", "Reasons for kick:"), VK_ReasonText(), c0);
		VK_Text(o + [17, 213], [313, 15], Loc("CSO_Vote_Notice_Info", "You can explain your stance against the reason."), 0i, c0);
		VK_Text(o + [183, 272], [147, 15], VK_Key("CSO_Vote_Cancel", "ESC: %s", "CSO_ClosePopup", "Close"), 4i, ck);
		return;
	}

	o = VK_Origin([355, 356]);
	Res_Frame9("resource/control/igframe/window_default_", o, [355, 356], 1.0f);
	draw.Text_RGB(Art_Pos(o + [12, 8]), Loc("CSO_Vote_Title", "Vote submitted"), [1,1,1], g_hudFont);
	VK_Text(o + [12, 38], [321, 15], strreplace("%s", by, Loc("CSO_Vote_Kick_Title", "Vote is underway for the vote kick initiated by %s.")), 0i, c0);
	VK_Row(o, [12, 61], [89, 61], [240, 15], Loc("CSO_VoteKick_KickUser", "User to kick:"), serverkey("csnz_vote_target"), cu);
	VK_Row(o, [12, 82], [89, 82], [240, 64], Loc("CSO_VoteKick_Reason", "Reasons for kick:"), VK_ReasonText(), c0);
	VK_Text(o + [12, 158], [321, 30], sprintf("%s %s   %s %s   (%i)",
		Loc("CSO_VoteYes2", "Agree"), serverkey("csnz_vote_yes"),
		Loc("CSO_VoteNo2", "Disagree"), serverkey("csnz_vote_no"),
		(int)ceil(serverkeyfloat("csnz_vote_end") - time)), 0i, c0);
	VK_Text(o + [12, 275], [321, 15], Loc("CSO_Vote_Kick_Info", "Do you agree on this kick?"), 0i, c0);

	VK_Text(o + [12, 310], [105, 15], VK_Key("CSO_Vote_Yes", "F3: %s", "CSO_VoteYes2", "Agree"), 3i,
		ballot == 1i ? [1, 1, 1] : ck);
	VK_Text(o + [121, 310], [105, 15], VK_Key("CSO_Vote_No", "F4: %s", "CSO_VoteNo2", "Disagree"), 4i,
		ballot == 2i ? [1, 1, 1] : ck);
	VK_Text(o + [228, 310], [105, 15], amProposer ? VK_Key("CSO_Vote_Cancel", "ESC: %s", "CSO_Vote_Giveup", "Withdraw")
		: VK_Key("CSO_Vote_Cancel", "ESC: %s", "CSO_ClosePopup", "Close"), 5i, ck);
}

/* --- the kick-call dialog (DESIGN layout) -------------------------------- */

vector
VK_CallOrigin(void)
{
	return (screen.HUDSize() / g_hs - [420, 360]) / 2;
}

/* row i of the player list or the reasons */
vector
VK_PlayerRow(int i)
{
	return VK_CallOrigin() + [16, 64 + i * 18];
}

vector
VK_ReasonRow(int i)
{
	return VK_CallOrigin() + [216, 64 + i * 18];
}

void
VK_DrawCall(void)
{
	vector o = VK_CallOrigin();
	int row = 0i;

	Res_Frame9("resource/control/igframe/window_default_", o, [420, 360], 1.0f);
	draw.Text_RGB(Art_Pos(o + [12, 8]), Loc("CSO_VoteKick_Title", "Call for a vote to kick"), [1,1,1], g_hudFont);
	draw.Text_RGB(Art_Pos(o + [16, 44]), Loc("CSO_VoteKick_Object", "Target"), [0.8,0.8,0.8], g_hudFont);
	draw.Text_RGB(Art_Pos(o + [216, 44]), Loc("CSO_VoteKick_Type", "Reason"), [0.8,0.8,0.8], g_hudFont);

	for (int i = 0i; i < maxclients && row < 14i; i++) {
		string name = getplayerkeyvalue(i, "name");

		if (name == "" || i == player_localnum)
			continue;
		vector p = VK_PlayerRow(row);
		if (i == g_vkCallPlayer)
			drawfill(Art_Pos(p - [2, 1]), [188, 17] * g_hs, [0.3, 0.4, 0.6], 0.8, 0);
		draw.Text_RGB(Art_Pos(p), name, [1,1,1], g_hudFont);
		row++;
	}

	for (int r = 0i; r < 4i; r++) {
		vector p = VK_ReasonRow(r);
		if (r == g_vkCallReason)
			drawfill(Art_Pos(p - [2, 1]), [188, 17] * g_hs, [0.3, 0.4, 0.6], 0.8, 0);
		draw.Text_RGB(Art_Pos(p), Loc(sprintf("CSO_VoteReason%i", r), sprintf("Reason %i", r + 1i)), [1,1,1], g_hudFont);
	}

	vector okp = o + [200, 316], cp = o + [306, 316];
	Res_Button3("resource/control/button/btn_default", Res_MouseIn(okp, [100, 28]) ? "o" : "n", okp, [100, 28], g_vkCallPlayer >= 0i ? 1.0f : 0.4f);
	VK_Text(okp + [0, 7], [100, 14], Loc("CSO_VoteKick", "Use Vote Kick"), 4i, [1,1,1]);
	Res_Button3("resource/control/button/btn_default", Res_MouseIn(cp, [100, 28]) ? "o" : "n", cp, [100, 28], 1.0f);
	VK_Text(cp + [0, 7], [100, 14], Loc("CSO_ClosePopup", "Close"), 4i, [1,1,1]);
}

/* the player number listed in row r, -1 for none */
int
VK_PlayerInRow(int r)
{
	int row = 0i;

	for (int i = 0i; i < maxclients; i++) {
		if (getplayerkeyvalue(i, "name") == "" || i == player_localnum)
			continue;
		if (row == r)
			return i;
		row++;
	}
	return -1i;
}

void
VK_Draw(void)
{
	float end = serverkeyfloat("csnz_vote_end");

	if (end != g_vkShownEnd) {
		g_vkShownEnd = end;
		g_vkHidden = false;
	}
	if (VK_Active() && !g_vkHidden)
		VK_DrawVote();
	if (g_vkCallOpen)
		VK_DrawCall();
}

bool
VK_Input(float evType, float key, float chr, float dev)
{
	if (g_vkCallOpen) {
		if (evType == IE_MOUSEABS || evType == IE_MOUSEDELTA)
			return true;
		if (evType == IE_KEYDOWN && key == K_ESCAPE) {
			g_vkCallOpen = false;
			return true;
		}
		if (evType == IE_KEYUP && key == K_MOUSE1) {
			vector o = VK_CallOrigin();

			for (int r = 0i; r < 14i; r++)
				if (Res_MouseIn(VK_PlayerRow(r) - [2, 1], [188, 17]) && VK_PlayerInRow(r) >= 0i)
					g_vkCallPlayer = VK_PlayerInRow(r);
			for (int r = 0i; r < 4i; r++)
				if (Res_MouseIn(VK_ReasonRow(r) - [2, 1], [188, 17]))
					g_vkCallReason = r;
			if (Res_MouseIn(o + [200, 316], [100, 28]) && g_vkCallPlayer >= 0i) {
				localcmd(sprintf("cmd votekick %i %i\n", g_vkCallPlayer + 1i, g_vkCallReason));
				g_vkCallOpen = false;
			} else if (Res_MouseIn(o + [306, 316], [100, 28])) {
				g_vkCallOpen = false;
			}
			return true;
		}
		return (evType == IE_KEYDOWN || evType == IE_KEYUP) && (key == K_MOUSE1);
	}

	if (!VK_Active() || evType != IE_KEYDOWN)
		return false;

	switch (key) {
	case K_F3:
		localcmd("cmd vote_yes\n");
		return true;
	case K_F4:
		localcmd("cmd vote_no\n");
		return true;
	case K_ESCAPE:
		if (g_vkHidden)
			return false;
		if (serverkey("csnz_vote_by") == getplayerkeyvalue(player_localnum, "name"))
			localcmd("cmd vote_cancel\n");
		g_vkHidden = true;
		return true;
	}
	return false;
}

bool
VK_ConsoleCommand(string cmd)
{
	tokenize_console(cmd);

	if (argv(0) != "votekick")
		return false;

	/* with arguments it goes straight to the server */
	if (argv(1) != "") {
		localcmd(sprintf("cmd %s\n", cmd));
		return true;
	}
	g_vkCallOpen = !g_vkCallOpen;
	g_vkCallPlayer = -1i;
	g_vkCallReason = 0i;
	return true;
}
