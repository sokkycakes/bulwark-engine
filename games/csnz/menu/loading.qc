/* Map loading screen, driven by the engine's load steps.

   Layout: resource/ui/cso_loadingdialog_v2.res (OBSERVED): a 1024x768
   frame, ServerLabel at the top, LoadingTipLabel, StatusLabel and the
   progress bar at the bottom.
   Background: resource/maploading/maploading.csv names each map's image
   (OBSERVED); a 512x384 image drawn over the whole 1024x768 frame
   (INFERRED). Tip: resource/maploading/maploadingtip.csv rows for the map
   and mode, picked by weight (OBSERVED table; the pick rule is INFERRED);
   tip images vary in size (256x128 up to 446x161 and 386x376, OBSERVED).
   Where the original draws them is UNRESOLVED (no control for it in the
   .res; capture needed). Here: native size, shrunk to fit 266x300, its
   bottom-left at 64,560, so it sits left of the tip text and above the
   map name the background art carries at the bottom-left (DESIGN).
   Progress: the engine reports each step with a fraction measured from
   counted items (cl_loadstage, cl_loadprogress); nothing here is timed.
   The bar's art is UNRESOLVED (no progress-bar images found); it is drawn
   plain (DESIGN). */

string g_loadMap;
string g_loadBg;
string g_loadTipImage;
string g_loadTipText;

static string
Loading_Background(string map)
{
	filestream fh = fopen("resource/maploading/maploading.csv", FILE_READ);
	string line;
	string bg = "";

	if (fh >= 0) {
		while (1) {
			line = fgets(fh);

			if not (line)
				break;
			if (tokenizebyseparator(line, ",") < 3)
				continue;
			if (strtolower(argv(1)) == strtolower(map)) {
				bg = strcat("resource/maploading/", strtolower(argv(2)));
				break;
			}
		}

		fclose(fh);
	}

	if (bg == "" || drawgetimagesize(bg)[0] <= 0)
		bg = "resource/background/csnzloading";

	return bg;
}

static void
Loading_PickTip(string map)
{
	filestream fh = fopen("resource/maploading/maploadingtip.csv", FILE_READ);
	float mapId = Tables_MapId(map);
	string mode = cvar_string("csnz_mode");
	string line;
	float total = 0;
	float pick;

	g_loadTipImage = "";
	g_loadTipText = "";

	if (fh < 0)
		return;

	/* two passes: sum the weights, then walk to the picked row */
	pick = random();
	for (int pass = 0i; pass < 2i; pass++) {
		float acc = 0;

		while (1) {
			line = fgets(fh);

			if not (line)
				break;
			if (substring(line, 0, 1) == "#" || tokenizebyseparator(line, ",") < 6)
				continue;
			if (stof(argv(0)) == 0 || stof(argv(1)) != mapId || argv(2) != mode)
				continue;

			if (pass == 0i) {
				total += stof(argv(3));
				continue;
			}

			acc += stof(argv(3));
			if (acc >= pick * total) {
				g_loadTipImage = strzone(strcat("resource/maploading/tip/", strtolower(argv(4))));
				g_loadTipText = strzone(VGUI_Localize(strtrim(argv(5))));
				break;
			}
		}

		if (pass == 0i) {
			fclose(fh);
			if (total <= 0)
				return;
			fh = fopen("resource/maploading/maploadingtip.csv", FILE_READ);
			if (fh < 0)
				return;
		}
	}

	fclose(fh);
}

static string
Loading_StageText(string stage)
{
	switch (stage) {
	case "connecting": return VGUI_Localize("#CSO_LoadingWait");
	case "server_world": return "Starting the server: loading the map";
	case "server_progs": return "Starting the server: game rules";
	case "server_entities": return "Starting the server: map entities";
	case "precache": return "Receiving the game data";
	case "world": return "Loading the map";
	case "world_render": return "Preparing the map";
	case "models": return "Loading models";
	case "sounds": return "Loading sounds";
	case "csprogs": return "Loading the client game";
	case "spawning": return VGUI_Localize("#CSO_LoadingReady");
	}

	return VGUI_Localize("#CSO_LoadingWait");
}

void
Loading_Draw(vector screen)
{
	string map = cvar_string("cl_loadmap");
	string stage = cvar_string("cl_loadstage");
	float frac = bound(0, cvar("cl_loadprogress"), 1);

	if (map != g_loadMap) {
		g_loadMap = strzone(map);
		g_loadBg = strzone(Loading_Background(map));
		Loading_PickTip(map);
	}

	Viewport_Update(screen);
	drawfill([0,0], screen, [0,0,0], 1.0f, 0);
	drawpic(Viewport_Map([0,0]), g_loadBg, [VP_WIDTH, VP_HEIGHT] * g_vpScale, [1,1,1], 1.0f, 0);

	/* ServerLabel 13,10 1000x30: the room title */
	string title = g_roomTitle != "" ? g_roomTitle : cvar_string("hostname");
	Font_DrawField(Viewport_Map([13, 10]), [1000, 30] * g_vpScale, title, g_fntDefault, AF_LEFT | AF_TOP | AF_BOTTOM);

	/* tip image and LoadingTipLabel 340,565 620x150 */
	if (g_loadTipImage != "") {
		vector sz = drawgetimagesize(g_loadTipImage);
		float k = 1.0f;
		if (sz[0] > 266)
			k = 266 / sz[0];
		if (sz[1] * k > 300)
			k = 300 / sz[1];
		sz *= k;
		drawpic(Viewport_Map([64, 560 - sz[1]]), g_loadTipImage, sz * g_vpScale, [1,1,1], 1.0f, 0);
	}
	if (g_loadTipText != "")
		Font_DrawField(Viewport_Map([340, 565]), [620, 150] * g_vpScale, g_loadTipText, g_fntDefault, AF_LEFT | AF_TOP);

	/* StatusLabel 64,695 200x30 (drawn wider: our step names are longer) */
	Font_DrawField(Viewport_Map([64, 695]), [600, 30] * g_vpScale, Loading_StageText(stage), g_fntDefault, AF_LEFT | AF_TOP | AF_BOTTOM);

	/* progress 64,729 896x24 */
	vector bp = Viewport_Map([64, 729]);
	vector bs = [896, 24] * g_vpScale;
	drawfill(bp, bs, [0,0,0], 0.6f, 0);
	drawfill(bp + [2,2], [(bs[0] - 4) * frac, bs[1] - 4], [0.85, 0.55, 0.15], 1.0f, 0);
	Font_DrawField(bp, bs, sprintf("%d%%", rint(frac * 100)), g_fntDefault, AF_LEFT | AF_RIGHT | AF_TOP | AF_BOTTOM);

	Viewport_DrawMargins(screen);
}
