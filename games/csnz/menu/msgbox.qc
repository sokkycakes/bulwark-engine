/* Message box: vgui2::MessageBox is in GameUI.dll and client.dll (OBSERVED
   RTTI) and has no layout file; it is built by code. This one follows stock
   VGUI2: a title, text, OK, and Cancel when asking. Its size is DESIGN. */

vguiWindow g_msgWin;
vguiLabel g_msgText;
vguiResButton g_msgOk;
vguiResButton g_msgCancel;
string g_msgOkCmd;
vguiWindow g_msgPrevModal;

void
MsgBox_Init(void)
{
	g_msgWin = Panel_New([332, 289], [360, 170], " ");
	g_msgText = spawn(vguiLabel);
	g_msgText.SetAlignment(AF_LEFT | AF_RIGHT | AF_TOP | AF_BOTTOM);
	Panel_AddAt(g_msgWin, g_msgText, [16, 36], [328, 80]);
	g_msgOk = Panel_Button(g_msgWin, [96, 126], [80, 28], VGUI_Localize("#GameUI_OK"), "msgbox_ok");
	g_msgCancel = Panel_Button(g_msgWin, [184, 126], [80, 28], VGUI_Localize("#GameUI_Cancel"), "msgbox_cancel");
}

void
MsgBox_Close(void)
{
	g_msgWin.Hide();
	g_modal = g_msgPrevModal;
	g_msgPrevModal = __NULL__;
}

static void
MsgBox_Open(string title, string text, string okCmd, bool ask, string okLabel)
{
	g_msgWin.SetTitle(VGUI_Localize(title));
	g_msgOk.SetTitle(VGUI_Localize(okLabel != "" ? okLabel : "#GameUI_OK"));
	g_msgText.SetTitle(VGUI_Localize(text));
	g_msgOkCmd = strzone(okCmd);

	if (ask) {
		g_msgOk.SetPos([96, 126] * g_vpScale);
		g_msgCancel.Show();
	} else {
		g_msgOk.SetPos([140, 126] * g_vpScale);
		g_msgCancel.Hide();
	}

	/* on top of everything */
	g_msgWin.Show();
	g_focusNext = g_msgWin;

	if (g_modal != g_msgWin)
		g_msgPrevModal = g_modal;
	g_modal = g_msgWin;
}

void
MsgBox_Show(string title, string text)
{
	MsgBox_Open(title, text, "", false, "");
}

/* OK runs okCmd through Nav_Command */
void
MsgBox_Ask(string title, string text, string okCmd)
{
	MsgBox_Open(title, text, okCmd, true, "");
}

/* the same, with the OK button's label given (a localization token) */
void
MsgBox_AskWith(string title, string text, string okCmd, string okLabel)
{
	MsgBox_Open(title, text, okCmd, true, okLabel);
}

/* returns true if the command was the message box's */
bool
MsgBox_Command(string cmd)
{
	if (cmd == "msgbox_ok") {
		string next = g_msgOkCmd;
		MsgBox_Close();
		if (next != "")
			Nav_Command(__NULL__, next);
		return (true);
	}

	if (cmd == "msgbox_cancel") {
		MsgBox_Close();
		return (true);
	}

	return (false);
}

/* a feature this build can't offer: says why instead of doing nothing */
void
MsgBox_Unavailable(string what, string why)
{
	MsgBox_Show("#CSO_ServerMessage_Title", sprintf("%s is not available.\n%s", what, why));
}
