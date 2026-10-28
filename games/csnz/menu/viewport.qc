/* The 1024x768 layout space and the panels placed in it.

   Panels come from .res files (OBSERVED). Their root panel gives their
   place on screen; VGUI_ResBuild lays out the rest inside. Scale and
   centring are DESIGN: the original's handling of other resolutions is
   UNRESOLVED (needs a capture at a widescreen resolution). */

vguiWindow g_panels[48];
int g_panelCount;

void
Viewport_Update(vector screen)
{
	g_vpScale = min(screen[0] / VP_WIDTH, screen[1] / VP_HEIGHT);
	g_vp = (screen - [VP_WIDTH, VP_HEIGHT] * g_vpScale) / 2;
	g_vp[0] = floor(g_vp[0]);
	g_vp[1] = floor(g_vp[1]);
}

vector
Viewport_Map(vector p)
{
	return g_vp + p * g_vpScale;
}

/* letterbox bars: outside the fidelity viewport */
void
Viewport_DrawMargins(vector screen)
{
	if (g_vp[0] > 0) {
		drawfill([0,0], [g_vp[0], screen[1]], [0,0,0], 1.0f, 0);
		drawfill([screen[0] - g_vp[0], 0], [g_vp[0] + 1, screen[1]], [0,0,0], 1.0f, 0);
	}
	if (g_vp[1] > 0) {
		drawfill([0,0], [screen[0], g_vp[1]], [0,0,0], 1.0f, 0);
		drawfill([0, screen[1] - g_vp[1]], [screen[0], g_vp[1] + 1], [0,0,0], 1.0f, 0);
	}
}

void
Panel_Place(vguiWindow win)
{
	win.SetPos(Viewport_Map(win.csnzResPos));
}

void
Panel_PlaceAll(void)
{
	for (int i = 0i; i < g_panelCount; i++)
		Panel_Place(g_panels[i]);
}

/* Loads a layout into a new hidden window placed by its root panel.
   Returns __NULL__ (and says so) when the file is missing. */
vguiWindow
Panel_Load(string file, string root)
{
	vguiResLayout res = spawn(vguiResLayout);

	if (!res.LoadFile(file)) {
		print(sprintf("^1menu: %s is missing\n", file));
		remove(res);
		return __NULL__;
	}

	string rootPath = res.FindPanel(root);
	string title = res.GetString(strcat(rootPath, "/title"));
	bool titled = (title != "" && title != " " && title != "Untitle");

	vguiWindow win = spawn(vguiWindow);
	win.SetMaxSize([8192, 8192]);	/* vguiWindow's default cap is smaller than the layouts */
	win.SetStyleMask(titled ? vguiWindowTitled : 0);
	g_uiDesktop.Add(win);
	VGUI_ResBuild(win, res, root, g_vpScale);

	if (!titled)
		win.SetTitle("");

	win.csnzRes = res;
	win.csnzResPos = [stof(res.GetString(strcat(rootPath, "/xpos"))), stof(res.GetString(strcat(rootPath, "/ypos")))];
	Panel_Place(win);
	win.Hide();

	if (g_panelCount < 48i)
		g_panels[g_panelCount++] = win;

	return win;
}

/* a window made by code, placed in layout space */
vguiWindow
Panel_New(vector pos, vector size, string title)
{
	vguiWindow win = spawn(vguiWindow);
	win.SetMaxSize([8192, 8192]);
	win.SetStyleMask(title != "" ? vguiWindowTitled : 0);
	win.SetTitle(title);
	g_uiDesktop.Add(win);
	win.SetSize(size * g_vpScale);
	win.csnzResPos = pos;
	Panel_Place(win);
	win.Hide();

	if (g_panelCount < 48i)
		g_panels[g_panelCount++] = win;

	return win;
}

vguiWidget
Panel_Find(vguiWindow win, string name)
{
	if (!win)
		return __NULL__;

	return VGUI_ResFindIn(win, name);
}

void
Panel_Show(vguiWidget w, bool show)
{
	if (!w)
		return;

	if (show)
		w.Show();
	else
		w.Hide();
}

/* a widget placed by code inside a panel, in layout units */
void
Panel_AddAt(vguiWindow win, vguiWidget w, vector pos, vector size)
{
	w.SetPos(pos * g_vpScale);
	w.SetSize(size * g_vpScale);
	win.Add(w);
}

/* a button with the default three-piece art and a command */
vguiResButton
Panel_Button(vguiWindow win, vector pos, vector size, string label, string cmd)
{
	vguiResButton btn = spawn(vguiResButton);
	btn.SetTitle(label);
	btn.m_resCommand = strzone(cmd);
	Panel_AddAt(win, btn, pos, size);
	return btn;
}
