/* Client tables (OBSERVED files): resource/gamemodelist.csv, limitlist.csv
   and maplist.csv. The meaning of their columns is INFERRED from their
   header lines. Also the room options shared by Create Room and the game
   room's option panel, which use the same panel names. */

/* Zombi first; then the ids csnz.dat implements, then the Studio ids
   (studio.dat: 38, 39, 49, 52, 53). Every id mp.dll's InstallGameRules
   (FUN_11412000) creates rules for, SOURCE-CONFIRMED; 44 and 48 are their
   own classes (SeasonZombieHero, PlayRoomZBS), not copies of 43 and 41.
   60 (PLAYROOM_ZHC) is left out: InstallGameRules has no case for it. */
#define MODES_OFFERED "8 0 1 2 3 4 5 6 7 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 40 41 42 43 44 45 46 47 48 50 51 54 55 56 57 58 59 38 39 49 52 53"

hashtable g_limits; /* limit_id -> "min max default step" */

/* --- client tables ------------------------------------------------------- */

void
Tables_LoadLimits(void)
{
	filestream fh = fopen("resource/limitlist.csv", FILE_READ);
	string line;

	g_limits = hash_createtab(64, EV_STRING);

	if (fh < 0)
		return;

	while (1) {
		line = fgets(fh);

		if not (line)
			break;
		if (substring(line, 0, 1) == "#" || tokenizebyseparator(line, ",") < 5)
			continue;

		hash_add(g_limits, argv(0), strzone(sprintf("%s %s %s %s", argv(1), argv(2), argv(3), argv(4))), EV_STRING);
	}

	fclose(fh);
}

/* fills a combo box with the values a limit allows; returns false for "no limit" (id 0) */
bool
Tables_FillLimit(vguiComboBox combo, string limitId)
{
	float lo, hi, def, step;

	combo.Clear();

	if (stof(limitId) == 0)
		return (false);

	tokenize(hash_get(g_limits, limitId, "0 0 0 0", EV_STRING));
	lo = stof(argv(0));
	hi = stof(argv(1));
	def = stof(argv(2));
	step = stof(argv(3));

	if (step <= 0)
		step = hi - lo + 1;

	for (float v = lo; v <= hi; v += step)
		combo.AddItem(ftos(v), ftos(v));

	combo.SelectValue(ftos(def));
	return (true);
}

/* the row of gamemodelist.csv for a mode, tokenized; returns false if absent */
bool
Tables_ModeRow(string modeId)
{
	filestream fh = fopen("resource/gamemodelist.csv", FILE_READ);
	string line;
	bool found = false;

	if (fh < 0)
		return (false);

	while (!found) {
		line = fgets(fh);

		if not (line)
			break;
		if (substring(line, 0, 1) == "#")
			continue;
		if (tokenizebyseparator(line, ",") > 10 && argv(0) == modeId)
			found = true;
	}

	fclose(fh);
	return (found);
}

/* The maplist.csv column that marks a mode's maps. The column names are
   OBSERVED (the file's header line); which mode each belongs to is INFERRED
   from the names. Modes not listed get every installed map in maplist.csv. */
string
Tables_MapColumnName(string modeId)
{
	switch (modeId) {
	case "0": return "map_original";
	case "1": return "map_deathmode";
	case "2": return "map_teamdeathmode";
	case "3": return "map_bot_origanl";
	case "4": return "map_bot_deathmode";
	case "5": return "map_bot_teamdeathmod";
	case "6": return "official";
	case "7": return "official_tiebreak";
	case "8": return "map_zombi";
	case "9": return "map_zombi_expand";
	case "11": return "map_zombi_team";
	case "12": return "map_challenge";
	case "14": return "map_zombi_3";
	case "16": return "map_soccer";
	case "18": return "PVPVE";
	case "20": return "map_ZombieEscape";
	case "21": return "map_BZM";
	case "22": return "map_GDM";
	case "23": return "map_Basic";
	case "25": return "map_BattleRush";
	case "27": return "map_Snowman";
	case "28": return "map_shelterteam";
	case "29": return "map_Zombie4";
	case "30": return "map_ZombieGiant";
	case "32": return "map_Zombie_Exterminate";
	case "33": return "map_standalone";
	case "35": return "Zombieofficial";
	case "36": return "Zombieofficial_tiebreak";
	case "37": return "ZombieTag";
	}

	return "";
}

/* the maps of a mode whose .bsp is here */
void
Tables_FillMaps(vguiComboBox combo, string modeId)
{
	filestream fh = fopen("resource/maplist.csv", FILE_READ);
	string line;
	hashtable seen = hash_createtab(64, EV_FLOAT);
	string colName = strtolower(Tables_MapColumnName(modeId));
	float col = -1;

	combo.Clear();

	/* DESIGN: a Studio room plays on a voxel world (maps/*.vxmap, a map with
	   no BSP); its map file is picked afterwards in Select map (studio.qc) */
	if (modeId == "38" || modeId == "39") {
		if (fh >= 0)
			fclose(fh);
		float sh = search_begin("maps/*.vxmap", TRUE, TRUE);
		if (sh < 0)
			return;
		for (float k = 0; k < search_getsize(sh); k++) {
			string vf = search_getfilename(sh, k);
			vf = substring(vf, 5, strlen(vf) - 11);
			if ((float)hash_get(seen, vf, 0, EV_FLOAT))
				continue;
			hash_add(seen, vf, 1, EV_FLOAT);
			combo.AddItem(vf, vf);
		}
		search_end(sh);
		hash_destroytab(seen);
		return;
	}

	if (fh < 0)
		return;

	while (1) {
		line = fgets(fh);

		if not (line)
			break;

		/* the header line names the columns */
		if (substring(line, 0, 1) == ";") {
			if (colName != "" && col < 0) {
				float n = tokenizebyseparator(substring(line, 1, -1), ",");
				for (float i = 0; i < n; i++)
					if (strtolower(strtrim(argv(i))) == colName)
						col = i;
			}
			continue;
		}

		if (tokenizebyseparator(line, ",") < 12)
			continue;
		if (col >= 0 && stof(argv(col)) == 0)
			continue;

		string file = strtrim(argv(2));
		string title = VGUI_Localize(strtrim(argv(1)));

		if ((float)hash_get(seen, file, 0, EV_FLOAT))
			continue;
		if (whichpack(strcat("maps/", file, ".bsp")) == "")
			continue;

		hash_add(seen, file, 1, EV_FLOAT);
		combo.AddItem(title, file);
	}

	fclose(fh);
	hash_destroytab(seen);
}

/* the display name of a map file, from maplist.csv; the file name if absent */
string
Tables_MapTitle(string file)
{
	filestream fh = fopen("resource/maplist.csv", FILE_READ);
	string line;
	string title = file;

	if (fh < 0)
		return file;

	while (1) {
		line = fgets(fh);

		if not (line)
			break;
		if (substring(line, 0, 1) == ";" || tokenizebyseparator(line, ",") < 3)
			continue;
		if (strtrim(argv(2)) == file) {
			title = VGUI_Localize(strtrim(argv(1)));
			break;
		}
	}

	fclose(fh);
	return title;
}

/* the map_id of a map file (maplist.csv column 0), or -1 */
float
Tables_MapId(string file)
{
	filestream fh = fopen("resource/maplist.csv", FILE_READ);
	string line;
	float mapId = -1;

	if (fh < 0)
		return -1;

	while (1) {
		line = fgets(fh);

		if not (line)
			break;
		if (substring(line, 0, 1) == ";" || tokenizebyseparator(line, ",") < 3)
			continue;
		if (strtrim(argv(2)) == file) {
			mapId = stof(argv(0));
			break;
		}
	}

	fclose(fh);
	return mapId;
}

/* names of the modes that this build's gamemodelist.csv lacks: the tokens
   the na locale's gamemodelist.csv, maploadingtip.csv and cso_na_en.txt use
   (OBSERVED) */
string
Tables_ExtraModeToken(string modeId)
{
	switch (modeId) {
	case "13": return "#CSO_GameMode_Zombi_Team_Ann";
	case "26": return "#CSO_GameMode_ZombieShelter";
	case "38": return "#CSO_GameMode_VoxelCreate";
	case "39": return "#CSO_GameMode_VoxelPVE";
	case "40": return "#CSO_GameMode_Allstar";
	case "41": return "#CSO_GameMode_PlayRoom";
	case "42": return "#CSO_GameMode_SeasonOriginal";
	case "43": return "#CSO_GameMode_SeasonZombieEx";
	case "44": return "#CSO_GameMode_SeasonZombi_3";	/* cstrike_na gamemodelist.csv; no text in cso_na_en */
	case "45": return "#CSO_GameMode_Zombi_3z";
	case "48": return "#CSO_GameMode_PlayRoom2";
	case "49": return "#CSO_GameMode_VoxelPropHunt";
	case "50": return "#CSO_GameMode_partnermode";
	case "51": return "#CSO_GameMode_ZHE";
	case "52": return "#CSO_GameMode_vxlzshelter";
	case "53": return "#CSO_GameMode_ScenarioTX";
	case "54": return "#CSO_GameMode_Zombi_5";
	}

	return "";
}

/* English names for the ids with no localised name in this build. DESIGN:
   worded from the mp.dll EGM_* names (SOURCE-CONFIRMED) and the rules
   class names. */
string
Tables_FallbackModeName(string modeId)
{
	switch (modeId) {
	case "13": return "Annihilation";
	case "15": return "Zombie Scenario";
	case "17": return "Human Scenario";
	case "25": return "Battle Rush";
	case "26": return "Zombie Shelter";
	case "28": return "Zombie Shelter Team";
	case "33": return "Zombie Files";
	case "44": return "Ranked Mode: Zombie Hero";
	case "48": return "Hide-Out 2 Scenario";
	case "51": return "Manhunt";
	case "52": return "Studio Zombie Shelter";
	case "53": return "Scenario TX";
	case "54": return "Zombie Evolution";
	case "55": return "Hide-Out Zombie Z";
	case "56": return "Zombie Conquest";
	case "57": return "Super Soldier";
	case "58": return "Zombie Hero Classic";
	case "59": return "ZS Rift";
	}

	return modeId;
}

/* the name of a mode without a gamemodelist.csv row: its token's text, or
   the English fallback when the token or its text is missing */
string
Tables_ExtraModeName(string modeId)
{
	string tok = Tables_ExtraModeToken(modeId);
	string name = (tok != "") ? VGUI_Localize(tok) : "";

	if (name == "" || name == tok)
		return Tables_FallbackModeName(modeId);
	return name;
}

string
Tables_ModeTitle(string modeId)
{
	if (!Tables_ModeRow(modeId))
		return Tables_ExtraModeName(modeId);

	return VGUI_Localize(argv(1));
}

/* --- room options: the GameMode/MapName/limit combo boxes ---------------- */

static vguiComboBox
RoomOpts_Combo(vguiWindow frame, string name)
{
	return (vguiComboBox)Panel_Find(frame, name);
}

string
RoomOpts_Value(vguiWindow frame, string name)
{
	vguiComboBox combo = RoomOpts_Combo(frame, name);

	if (!combo)
		return "";

	return combo.GetValue();
}

static void
RoomOpts_ShowPair(vguiWindow frame, string comboName, string labelName, bool show)
{
	Panel_Show(Panel_Find(frame, comboName), show);
	Panel_Show(Panel_Find(frame, labelName), show);
}

/* false when the mode has no such limit */
static bool
RoomOpts_Fill(vguiWindow frame, string name, string limitId)
{
	vguiComboBox combo = RoomOpts_Combo(frame, name);

	if (!combo)
		return (false);

	return Tables_FillLimit(combo, limitId);
}

void
RoomOpts_ModeChanged(entity comboEnt)
{
	vguiComboBox mode = (vguiComboBox)comboEnt;
	vguiWindow frame = (vguiWindow)mode.m_parent;
	string modeId = mode.GetValue();

	vguiComboBox maps = RoomOpts_Combo(frame, "MapName");
	if (maps)
		Tables_FillMaps(maps, modeId);

	if (!Tables_ModeRow(modeId)) {
		/* no gamemodelist.csv row in this build, so no limits to offer;
		   the player count is DESIGN (the most any row allows) */
		vguiComboBox anyPlayers = RoomOpts_Combo(frame, "MaxPlayer");
		if (anyPlayers) {
			anyPlayers.Clear();
			for (float q = 2; q <= 32; q += 2)
				anyPlayers.AddItem(ftos(q), ftos(q));
			anyPlayers.SelectValue("32");
		}

		string none[4] = {"WinLimit", "KillLimit", "TimeLimit", "RoundTime"};
		string labels[4] = {"WinLabel", "KillLabel", "Label6", "RoundLabel"};
		for (int k = 0i; k < 4i; k++) {
			vguiComboBox c = RoomOpts_Combo(frame, none[k]);
			if (c)
				c.Clear();
			RoomOpts_ShowPair(frame, none[k], labels[k], false);
		}
		return;
	}

	/* columns: 5 max players, 6 win_limit_id, 7 kill_limit_id, 8 time_limit_id, 9 roundtime_id */
	string winId = strzone(argv(6));
	string killId = strzone(argv(7));
	string timeId = strzone(argv(8));
	string roundId = strzone(argv(9));
	float maxPlayers = stof(argv(5));

	vguiComboBox players = RoomOpts_Combo(frame, "MaxPlayer");
	if (players) {
		players.Clear();
		for (float p = 2; p <= maxPlayers; p += 2)
			players.AddItem(ftos(p), ftos(p));
		players.SelectValue(ftos(maxPlayers));
	}

	/* the win and kill limits share a spot; a mode uses one or the other */
	vguiComboBox win = RoomOpts_Combo(frame, "WinLimit");
	vguiComboBox kill = RoomOpts_Combo(frame, "KillLimit");
	bool hasWin = win ? Tables_FillLimit(win, winId) : false;
	bool hasKill = kill ? Tables_FillLimit(kill, killId) : false;

	RoomOpts_ShowPair(frame, "WinLimit", "WinLabel", hasWin);
	RoomOpts_ShowPair(frame, "KillLimit", "KillLabel", hasKill && !hasWin);

	RoomOpts_ShowPair(frame, "TimeLimit", "Label6", RoomOpts_Fill(frame, "TimeLimit", timeId));
	RoomOpts_ShowPair(frame, "RoundTime", "RoundLabel", RoomOpts_Fill(frame, "RoundTime", roundId));

	strunzone(winId);
	strunzone(killId);
	strunzone(timeId);
	strunzone(roundId);
}

/* fills the mode and map combo boxes of a room options panel */
void
RoomOpts_Init(vguiWindow frame)
{
	vguiComboBox mode = RoomOpts_Combo(frame, "GameMode");

	if (!mode)
		return;

	int modes = (int)tokenize(MODES_OFFERED);
	for (int i = 0i; i < modes; i++) {
		string id = strzone(argv(i));
		/* a mode whose name is blank in this build (11, "Zombi_Team",
		   OBSERVED " ") is left out: INFERRED that it is retired */
		if (Tables_ModeRow(id)) {
			if (strtrim(VGUI_Localize(argv(1))) != "")
				mode.AddItem(VGUI_Localize(argv(1)), id);
		} else {
			mode.AddItem(Tables_ExtraModeName(id), id);
		}
		strunzone(id);
		tokenize(MODES_OFFERED);
	}

	mode.CallOnChange(RoomOpts_ModeChanged);
	RoomOpts_ModeChanged(mode);


}

/* copies the choices of one options panel to another */
void
RoomOpts_Copy(vguiWindow from, vguiWindow to)
{
	string names[6] = {"GameMode", "MapName", "MaxPlayer", "WinLimit", "TimeLimit", "RoundTime"};

	for (int i = 0i; i < 6i; i++) {
		vguiComboBox dst = RoomOpts_Combo(to, names[i]);
		string val = RoomOpts_Value(from, names[i]);

		if (dst && val != "") {
			dst.SelectValue(val);
			if (i == 0i)
				RoomOpts_ModeChanged(dst);
		}
	}
}
