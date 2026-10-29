/* Options: COptionsDialog (OBSERVED RTTI in GameUI.dll) has no layout file
   of its own; its pages do: optionssubvideo.res, optionssubaudio.res,
   optionssubmouse.res and cso_optionssubmultiplayer_v3.res (OBSERVED; the
   current OptionsSubVideo_Ver2.res is missing, the older one stands in).
   The frame, tabs and OK/Cancel/Apply follow stock VGUI2's property dialog;
   their size and placement are DESIGN.

   Each control is tied to a real engine cvar, or is shown disabled when the
   engine has no such setting. OK and Apply set the cvars with "seta" and
   run "writeconfig", so they persist; Cancel puts the controls back. */

.float csnzPage;
.float csnzPageHidden;

vguiWindow g_winOptions;
vguiResButton g_optTabs[4];
int g_optPage;

#define OPT_PAGES 4i
string g_optFiles[4] = {"resource/optionssubvideo.res", "resource/optionssubaudio.res",
	"resource/optionssubmouse.res", "resource/cso_optionssubmultiplayer_v3.res"};
string g_optTitles[4] = {"#GameUI_Video", "#GameUI_Audio", "#GameUI_Mouse", "#GameUI_Multiplayer"};

/* crosshair choices: Counter-Strike 1.6's (STOCK-CS); CSO's lists are UNRESOLVED */
#define CROSSHAIR_SIZES "auto small medium large"

static vguiWidget
Opt_Find(string name)
{
	return Panel_Find(g_winOptions, name);
}

static void
Opt_Disable(string name)
{
	vguiWidget w = Opt_Find(name);

	if (w)
		w.Disable();
}

static void
Opt_ShowPage(int page)
{
	g_optPage = page;

	for (entity e = nextent(__NULL__); e; e = nextent(e)) {
		if (!e.isVGUI || e.csnzPage <= 0)
			continue;

		vguiWidget w = (vguiWidget)e;
		if (w.m_parent != g_winOptions)
			continue;

		Panel_Show(w, (e.csnzPage == page + 1i) && !e.csnzPageHidden);
	}

	for (int i = 0i; i < OPT_PAGES; i++)
		g_optTabs[i].SetLatched(i == page);
}

/* cvars -> controls */
static void
Opt_Load(void)
{
	vguiCheckbox chk;
	vguiComboBox combo;

	chk = (vguiCheckbox)Opt_Find("VSync");
	if (chk)
		chk.SetValue(cvar("vid_vsync") ? 1i : 0i);

	chk = (vguiCheckbox)Opt_Find("ReverseMouse");
	if (chk)
		chk.SetValue(cvar("m_pitch") < 0 ? 1i : 0i);
	chk = (vguiCheckbox)Opt_Find("MouseLook");
	if (chk)
		chk.SetValue(1i);
	chk = (vguiCheckbox)Opt_Find("MouseFilter");
	if (chk)
		chk.SetValue(cvar("m_filter") ? 1i : 0i);
	chk = (vguiCheckbox)Opt_Find("Joystick");
	if (chk)
		chk.SetValue(cvar("joy_enable") ? 1i : 0i);

	vguiResSlider sens = (vguiResSlider)Opt_Find("Slider");
	if (sens)
		sens.SetValue(cvar("sensitivity"));
	vguiTextBox sensText = (vguiTextBox)Opt_Find("SensitivityLabel");
	if (sensText)
		sensText.SetText(sprintf("%.1f", cvar("sensitivity")));

	vguiResSlider vol = (vguiResSlider)Opt_Find("VolumeSlider");
	if (vol)
		vol.SetValue(cvar("volume"));
	vguiResSlider music = (vguiResSlider)Opt_Find("MusicSlider");
	if (music)
		music.SetValue(cvar("bgmvolume"));

	combo = (vguiComboBox)Opt_Find("CrosshairSizeComboBox");
	if (combo)
		if (!combo.SelectValue(cvar_string("cl_crosshair_size")))
			combo.SelectValue("auto");
	combo = (vguiComboBox)Opt_Find("CrosshairColorComboBox");
	if (combo)
		if (!combo.SelectValue(cvar_string("cl_crosshair_color")))
			combo.SetSelected(0i);
	chk = (vguiCheckbox)Opt_Find("CrosshairTranslucencyCheckbox");
	if (chk)
		chk.SetValue(cvar("cl_crosshair_translucent") ? 1i : 0i);
}

/* controls -> cvars, saved */
static void
Opt_Apply(void)
{
	vguiCheckbox chk;
	vguiComboBox combo;

	chk = (vguiCheckbox)Opt_Find("VSync");
	if (chk)
		localcmd(sprintf("seta vid_vsync %i\n", chk.GetValue()));

	chk = (vguiCheckbox)Opt_Find("ReverseMouse");
	if (chk)
		localcmd(sprintf("seta m_pitch %g\n", fabs(cvar("m_pitch")) * (chk.GetValue() ? -1 : 1)));
	chk = (vguiCheckbox)Opt_Find("MouseFilter");
	if (chk)
		localcmd(sprintf("seta m_filter %i\n", chk.GetValue()));
	chk = (vguiCheckbox)Opt_Find("Joystick");
	if (chk)
		localcmd(sprintf("seta joy_enable %i\n", chk.GetValue()));

	vguiResSlider sens = (vguiResSlider)Opt_Find("Slider");
	if (sens)
		localcmd(sprintf("seta sensitivity %g\n", sens.GetValue()));
	vguiResSlider vol = (vguiResSlider)Opt_Find("VolumeSlider");
	if (vol)
		localcmd(sprintf("seta volume %g\n", vol.GetValue()));
	vguiResSlider music = (vguiResSlider)Opt_Find("MusicSlider");
	if (music)
		localcmd(sprintf("seta bgmvolume %g\n", music.GetValue()));

	combo = (vguiComboBox)Opt_Find("CrosshairSizeComboBox");
	if (combo)
		localcmd(sprintf("seta cl_crosshair_size %s\n", combo.GetValue()));
	combo = (vguiComboBox)Opt_Find("CrosshairColorComboBox");
	if (combo)
		localcmd(sprintf("seta cl_crosshair_color \"%s\"\n", combo.GetValue()));
	chk = (vguiCheckbox)Opt_Find("CrosshairTranslucencyCheckbox");
	if (chk)
		localcmd(sprintf("seta cl_crosshair_translucent %i\n", chk.GetValue()));

	localcmd("writeconfig\n");
}

static void
Opt_SensitivityMoved(entity sliderEnt)
{
	vguiTextBox sensText = (vguiTextBox)Opt_Find("SensitivityLabel");

	if (sensText)
		sensText.SetText(sprintf("%.1f", ((vguiResSlider)sliderEnt).GetValue()));
}

void
Options_Init(void)
{
	g_winOptions = Panel_New([246, 168], [532, 432], VGUI_Localize("#GameUI_Options"));

	for (int t = 0i; t < OPT_PAGES; t++) {
		g_optTabs[t] = Panel_Button(g_winOptions, [12 + t * 104, 34], [100, 26], VGUI_Localize(g_optTitles[t]), sprintf("opt_tab%i", t));
		g_optTabs[t].SetThreePic("resource/control/tabbutton/tab_small_abled");
		g_optTabs[t].SetThreePicSuffixes("_n", "_o", "_c");
		g_optTabs[t].csnzPage = -1;
	}

	vguiResButton b;
	b = Panel_Button(g_winOptions, [270, 392], [80, 28], VGUI_Localize("#GameUI_OK"), "opt_ok");
	b.csnzPage = -1;
	b = Panel_Button(g_winOptions, [356, 392], [80, 28], VGUI_Localize("#GameUI_Cancel"), "opt_cancel");
	b.csnzPage = -1;
	b = Panel_Button(g_winOptions, [442, 392], [80, 28], VGUI_Localize("#GameUI_Apply"), "opt_apply");
	b.csnzPage = -1;

	/* each page's widgets are built into the frame, then moved below the tabs */
	for (int p = 0i; p < OPT_PAGES; p++) {
		vguiResLayout res = spawn(vguiResLayout);

		if (!res.LoadFile(g_optFiles[p])) {
			print(sprintf("^1menu: %s is missing\n", g_optFiles[p]));
			continue;
		}

		VGUI_ResBuild(g_winOptions, res, "", g_vpScale);

		for (entity e = nextent(__NULL__); e; e = nextent(e)) {
			if (!e.isVGUI || e.csnzPage != 0)
				continue;

			vguiWidget w = (vguiWidget)e;
			if (w.m_parent != g_winOptions)
				continue;

			e.csnzPage = p + 1i;
			e.csnzPageHidden = !w.Visible();
			w.SetPos(w.GetPos() + [8, 64] * g_vpScale);
		}
	}

	/* audio: the page has no volume controls (only EAX/A3D, which the
	   engine has no equivalent for). Volume and music sliders are added
	   (DESIGN) so the engine's volume settings can be reached. */
	vguiLabel volLabel = spawn(vguiLabel);
	volLabel.SetTitle("Volume");
	Panel_AddAt(g_winOptions, volLabel, [51, 240], [120, 20]);
	volLabel.csnzPage = 2;
	vguiResSlider vol = spawn(vguiResSlider);
	vol.vguiResName = "VolumeSlider";
	vol.SetRange(0, 1, 0.05f);
	Panel_AddAt(g_winOptions, vol, [171, 236], [260, 28]);
	vol.csnzPage = 2;
	vguiLabel musicLabel = spawn(vguiLabel);
	musicLabel.SetTitle("Music");
	Panel_AddAt(g_winOptions, musicLabel, [51, 276], [120, 20]);
	musicLabel.csnzPage = 2;
	vguiResSlider music = spawn(vguiResSlider);
	music.vguiResName = "MusicSlider";
	music.SetRange(0, 1, 0.05f);
	Panel_AddAt(g_winOptions, music, [171, 272], [260, 28]);
	music.csnzPage = 2;

	/* settings the engine doesn't have: shown, disabled */
	string none = "Renderer Resolution AspectRatio Windowed Brightness Gamma EAX A3D MouseLook JoystickLook "
		"CrosshairTypeComboBox SpraypaintList SensitivityLabel";
	int n = (int)tokenize(none);
	for (int i = 0i; i < n; i++) {
		string name = strzone(argv(i));
		Opt_Disable(name);
		strunzone(name);
		tokenize(none);
	}
	/* DetailTextures shares VSync's spot; VSync is the one the engine has */
	vguiWidget detail = Opt_Find("DetailTextures");
	if (detail)
		detail.csnzPageHidden = true;

	vguiComboBox renderer = (vguiComboBox)Opt_Find("Renderer");
	if (renderer) {
		renderer.AddItem("OpenGL", "gl");
		renderer.SetSelected(0i);
	}
	vguiComboBox resolution = (vguiComboBox)Opt_Find("Resolution");
	if (resolution) {
		resolution.AddItem(sprintf("%d x %d", g_vidsize[0], g_vidsize[1]), "current");
		resolution.SetSelected(0i);
	}

	vguiResSlider sens = (vguiResSlider)Opt_Find("Slider");
	if (sens) {
		sens.SetRange(1, 20, 0.5f);	/* GoldSrc's range (STOCK) */
		sens.CallOnChange(Opt_SensitivityMoved);
	}

	vguiComboBox size = (vguiComboBox)Opt_Find("CrosshairSizeComboBox");
	if (size) {
		size.AddItem("Auto", "auto");
		size.AddItem("Small", "small");
		size.AddItem("Medium", "medium");
		size.AddItem("Large", "large");
	}
	vguiComboBox color = (vguiComboBox)Opt_Find("CrosshairColorComboBox");
	if (color) {
		color.AddItem("Green", "50 250 50");
		color.AddItem("Red", "250 50 50");
		color.AddItem("Blue", "50 50 250");
		color.AddItem("Yellow", "250 250 50");
		color.AddItem("Light blue", "50 250 250");
	}

	g_winOptions.Hide();
}

void
Options_Open(void)
{
	Opt_Load();
	g_winOptions.Show();
	g_focusNext = g_winOptions;
	Opt_ShowPage(g_optPage);
	g_modal = g_winOptions;
}

static void
Options_Close(void)
{
	g_winOptions.Hide();
	if (g_modal == g_winOptions)
		g_modal = __NULL__;
}

bool
Options_Command(string cmd)
{
	switch (cmd) {
	case "opt_tab0": Opt_ShowPage(0i); return (true);
	case "opt_tab1": Opt_ShowPage(1i); return (true);
	case "opt_tab2": Opt_ShowPage(2i); return (true);
	case "opt_tab3": Opt_ShowPage(3i); return (true);
	case "opt_ok":
		Opt_Apply();
		Options_Close();
		return (true);
	case "opt_apply":
		Opt_Apply();
		return (true);
	case "opt_cancel":
		Opt_Load();
		Options_Close();
		return (true);
	case "Advanced":
	case "ListDemoBtn":
	case "DefaultConsole":
	case "NetworkConfig":
		MsgBox_Unavailable("This options page", "It is not built yet.");
		return (true);
	}

	return (false);
}
