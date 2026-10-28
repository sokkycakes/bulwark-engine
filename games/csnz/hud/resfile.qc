/* VGUI2 .res layouts for the HUD's own menus.

   A .res file is KeyValues: the file name, then one block per control with
   "ControlName", "fieldName", "xpos", "ypos", "wide", "tall" and the rest.
   Each file loads into a hashtable keyed "<control>.<key>" (lower case),
   with the control names in load order under " names".

   Positions use the VGUI2 forms N, cN (from the parent's centre) and rN
   (from the parent's far edge). Nuclide's own reader (src/vgui/ResFile.qc)
   is not used: it needs the client's VGUI and font code, which the HUD
   progs doesn't have. */

hashtable
Res_Load(string file)
{
	filestream fh = fopen(file, FILE_READ);
	hashtable tab;
	string line, ctl = "", key, names = "";
	int depth = 0i;

	if (fh < 0) {
		print(sprintf("^1HUD: can't open %s\n", file));
		return (hashtable)0;
	}

	tab = hash_createtab(64, HASH_REPLACE);

	while (1) {
		line = fgets(fh);

		if not (line)
			break;

		int n = (int)tokenize_console(line);

		for (int i = 0i; i < n; i++) {
			key = argv(i);

			if (key == "{") {
				depth++;
				continue;
			}
			if (key == "}") {
				depth--;
				continue;
			}
			/* platform conditionals ([$WIN32] and the like) */
			if (substring(key, 0, 1) == "[")
				continue;
			if (substring(key, 0, 2) == "//")
				break;

			if (depth == 1i) {
				/* a control's name; its block follows */
				ctl = strtolower(key);
				names = strcat(names, " ", ctl);
			} else if (depth == 2i && i + 1i < n) {
				hash_add(tab, strcat(ctl, ".", strtolower(key)), argv(i + 1i), EV_STRING);
				i++;
			}
		}
	}

	fclose(fh);
	hash_add(tab, " names", names, EV_STRING);
	return tab;
}

string
Res_Get(hashtable tab, string ctl, string key)
{
	if (!tab)
		return "";

	return hash_get(tab, strcat(strtolower(ctl), ".", strtolower(key)), "", EV_STRING);
}

bool
Res_Has(hashtable tab, string ctl)
{
	return Res_Get(tab, ctl, "ControlName") != "";
}

/* one coordinate in a parent of the given size */
float
Res_Coord(string val, float parentSize)
{
	string c = substring(val, 0, 1);

	if (c == "c" || c == "C")
		return parentSize / 2 + stof(substring(val, 1, -1));
	if (c == "r" || c == "R")
		return parentSize - stof(substring(val, 1, -1));

	return stof(val);
}

vector
Res_Pos(hashtable tab, string ctl, vector parentSize)
{
	return [Res_Coord(Res_Get(tab, ctl, "xpos"), parentSize[0]),
		Res_Coord(Res_Get(tab, ctl, "ypos"), parentSize[1])];
}

vector
Res_Size(hashtable tab, string ctl)
{
	return [stof(Res_Get(tab, ctl, "wide")), stof(Res_Get(tab, ctl, "tall"))];
}

bool
Res_Visible(hashtable tab, string ctl)
{
	return Res_Get(tab, ctl, "visible") != "0";
}

/* a #token through the HUD's localization table; anything else as is.
   VGUI2's & hotkey marker is dropped. */
string
Res_Text(string txt)
{
	if (substring(txt, 0, 1) == "#")
		txt = Loc(substring(txt, 1, -1), substring(txt, 1, -1));

	return strreplace("&", "", txt);
}

/* --- drawing in layout space (art.qc's 1024x768, scaled) ------------------ */

/* one piece of a frame: base + part, or base with %s replaced by part
   (for names like output_default_%s@n) */
string
Res_Piece(string base, string part)
{
	if (strstrofs(base, "%s") >= 0)
		return strreplace("%s", part, base);
	return strcat(base, part);
}

/* a three-by-three frame: base + top_left, top_center, ... bottom_right */
void
Res_Frame9(string base, vector p, vector s, float alpha)
{
	vector tl = drawgetimagesize(Res_Piece(base, "top_left"));
	vector br = drawgetimagesize(Res_Piece(base, "bottom_right"));
	float l = tl[0], t = tl[1], r = br[0], bt = br[1];

	drawpic(Art_Pos(p), Res_Piece(base, "top_left"), [l, t] * g_hs, [1,1,1], alpha, 0);
	drawpic(Art_Pos(p + [l, 0]), Res_Piece(base, "top_center"), [s[0] - l - r, t] * g_hs, [1,1,1], alpha, 0);
	drawpic(Art_Pos(p + [s[0] - r, 0]), Res_Piece(base, "top_right"), [r, t] * g_hs, [1,1,1], alpha, 0);
	drawpic(Art_Pos(p + [0, t]), Res_Piece(base, "center_left"), [l, s[1] - t - bt] * g_hs, [1,1,1], alpha, 0);
	drawpic(Art_Pos(p + [l, t]), Res_Piece(base, "center_center"), [s[0] - l - r, s[1] - t - bt] * g_hs, [1,1,1], alpha, 0);
	drawpic(Art_Pos(p + [s[0] - r, t]), Res_Piece(base, "center_right"), [r, s[1] - t - bt] * g_hs, [1,1,1], alpha, 0);
	drawpic(Art_Pos(p + [0, s[1] - bt]), Res_Piece(base, "bottom_left"), [l, bt] * g_hs, [1,1,1], alpha, 0);
	drawpic(Art_Pos(p + [l, s[1] - bt]), Res_Piece(base, "bottom_center"), [s[0] - l - r, bt] * g_hs, [1,1,1], alpha, 0);
	drawpic(Art_Pos(p + s - [r, bt]), Res_Piece(base, "bottom_right"), [r, bt] * g_hs, [1,1,1], alpha, 0);
}

/* a three-part button: base + left@x, center@x, right@x with x n(ormal),
   o(ver) or c(licked) */
void
Res_Button3(string base, string state, vector p, vector s, float alpha)
{
	string l = strcat(base, "_left@", state);
	string c = strcat(base, "_center@", state);
	string r = strcat(base, "_right@", state);
	vector ls = drawgetimagesize(l), rs = drawgetimagesize(r);
	float lw = ls[0], rw = rs[0];

	/* the end caps keep their width and take the button's height */
	drawpic(Art_Pos(p), l, [lw, s[1]] * g_hs, [1,1,1], alpha, 0);
	drawpic(Art_Pos(p + [lw, 0]), c, [s[0] - lw - rw, s[1]] * g_hs, [1,1,1], alpha, 0);
	drawpic(Art_Pos(p + [s[0] - rw, 0]), r, [rw, s[1]] * g_hs, [1,1,1], alpha, 0);
}

/* the mouse position, kept in layout space by HUD_InputEvent */
vector g_resMouse;

bool
Res_MouseIn(vector p, vector s)
{
	return (g_resMouse[0] >= p[0] && g_resMouse[0] < p[0] + s[0] &&
		g_resMouse[1] >= p[1] && g_resMouse[1] < p[1] + s[1]);
}

/* --- controls of a menu whose .res is tab, laid out from org ------------- */

/* the control under the mouse and the one the mouse went down on, for
   whichever HUD menu is open (only one is) */
string g_resHoverCtl;
string g_resPressCtl;

vector
Res_CtlPos(hashtable tab, vector org, vector frameSize, string ctl)
{
	return org + Res_Pos(tab, ctl, frameSize);
}

/* a button with btn_default art (OBSERVED string
   "resource/Control/Button/btn_default"); the label centred, or from the
   left for list slots */
void
Res_DrawButton(hashtable tab, vector org, vector frameSize, string ctl, string label, bool on, bool leftAlign)
{
	vector p = Res_CtlPos(tab, org, frameSize, ctl);
	vector s = Res_Size(tab, ctl);
	string state = "n";
	float alpha = on ? 1.0f : 0.4f;

	if (on && g_resHoverCtl == strtolower(ctl))
		state = (g_resPressCtl == strtolower(ctl)) ? "c" : "o";

	Res_Button3("resource/control/button/btn_default", state, p, s, alpha);

	if (label == "")
		return;

	vector ts = [font.StringWidth(label, false, g_hudFont), font.GetHeight(g_hudFont)] / g_hs;
	vector tp = p + [(s[0] - ts[0]) / 2, (s[1] - ts[1]) / 2];

	if (leftAlign)
		tp[0] = p[0] + 8;

	draw.Text_RGBA(Art_Pos(tp), label, on ? [1,1,1] : [0.6,0.6,0.6], alpha, g_hudFont);
}

/* text in a control's box, vertically centred */
void
Res_DrawLabel(hashtable tab, vector org, vector frameSize, string ctl, string txt, vector rgb, bool right)
{
	vector p = Res_CtlPos(tab, org, frameSize, ctl);
	vector s = Res_Size(tab, ctl);
	float w = font.StringWidth(txt, false, g_hudFont) / g_hs;

	if (right)
		p[0] += s[0] - w;

	draw.Text_RGB(Art_Pos(p + [0, (s[1] - font.GetHeight(g_hudFont) / g_hs) / 2]), txt, rgb, g_hudFont);
}

/* the first of the space-separated controls under the mouse, "" for none */
string
Res_HitTest(hashtable tab, vector org, vector frameSize, string ctls)
{
	int n = (int)tokenize(ctls);

	for (int i = 0i; i < n; i++) {
		string ctl = argv(i);

		if (Res_MouseIn(Res_CtlPos(tab, org, frameSize, ctl), Res_Size(tab, ctl)))
			return ctl;
	}

	return "";
}
