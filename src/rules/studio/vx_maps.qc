/* vx_maps: Studio map files, autosave and recovery, the test snapshot and
 * the local map library. Original code (ISC).
 *
 * A Studio map is one BWVX file (cells, by type name, written by the engine:
 * docs/spec/voxel.md) whose metadata block holds the rest as text lines:
 *     studio 1
 *     title <text>          author <text>          desc <text>
 *     mode <Vxl_OutUI_Mode name or CSO mode id>
 *     rec <x y z> <TAB> <type name> <TAB> <rot> <TAB> <props> <TAB> <links>
 * "rec" lines are the entity and multi-cell block records (vx_core.qc).
 * A file without them still loads: records are rebuilt from anchor cells.
 *
 * Evidence
 *   OBSERVED: maps are saved to slots and published for others to play and
 *     rate ("After playing a map, you can rate it", CSO_MaploadingTip_VxlPlay);
 *     map title and description ("Read the map description").
 *   OBSERVED (Vxl_OutUI_Mode_*): the modes a Studio map can be made for.
 *   BLOCKED: the online map service (VOXEL_HTTP), accounts, coins and slot
 *     purchases. DESIGN substitute: files under data/studio and a local
 *     library list. Nothing here talks to a network service.
 *   DESIGN: the metadata layout, autosave period, file names. */

var float autocvar_studio_autosave = 120;	/* seconds; 0 = off */
var float autocvar_studio_savealarm = 300;	/* seconds; 0 = off. hw.dll DAT_02dfabd8 */
var string autocvar_studio_map = "";		/* VxlPlay / prop hunt: map to load */

#define VX_MAPDIR "studio/maps/"
#define VX_SAVEDIR "studio/saves/"
#define VX_LIBRARY "studio/library.txt"
#define VX_LIBMAX 256

string vx_title, vx_author, vx_desc, vx_mapMode;
string vx_mapName;		/* name of the loaded or last saved map */
float vx_nextAutosave;

/* names: letters, digits, '_' and '-', 1-32 characters (DESIGN) */
bool
VX_ValidName(string n)
{
	float len = strlen(n);
	if (len < 1 || len > 32)
		return false;
	for (float i = 0; i < len; i++) {
		float c = str2chr(n, i);
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return false;
	}
	return true;
}

string
VX_MapPath(string n)
{
	return strcat(VX_MAPDIR, n, ".bwvx");
}

void
VX_SetText(string key, string value)
{
	/* a metadata line must stay one line */
	value = strreplace("\n", " ", strreplace("\t", " ", value));
	switch (key) {
	case "title": if (vx_title) strunzone(vx_title); vx_title = strzone(value); break;
	case "author": if (vx_author) strunzone(vx_author); vx_author = strzone(value); break;
	case "desc": if (vx_desc) strunzone(vx_desc); vx_desc = strzone(value); break;
	case "mode": if (vx_mapMode) strunzone(vx_mapMode); vx_mapMode = strzone(value); break;
	}
	serverinfo.SetString(strcat("vx_", key), value);
}

/* --- World settings ------------------------------------------------------------
   The Fullbrowser's World settings page (client.dll FUN_02b09a80) sends
   one "vxl_<key> <value>" command per changed row (FUN_02b10390), and
   hw.dll's vxl_ handler (FUN_021f0e60) keeps them with the map.
   SOURCE-CONFIRMED: the command names below. DESIGN: they are kept as
   "opt <key> <value>" lines in the map file and published to clients as
   serverinfo vxo_<key>; vx_tools.qc VXT_ApplyOpts says which ones act. */

#define VX_OPTKEYS "gamemode playtime respawn respawntime maxplayernum minplayernum endrule resettrigger playercollision enabletps initback humandestroy zombidestroy zombiseethru defeatbywipe friendlyfire enemyfire seekerpenalty hiderproplimit skyname weather daylight invlight remove_dim_light fogcolor fogdensity visdist fogon shelter_maxplaydate shelter_maxlevel shelter_maxtier shelter_lastdaywin shelter_buildrange gdmknifekillbonus gdmupperlvkillbonus"

hashtable vx_optTab;
void VXT_ApplyOpts(void);

bool
VX_OptKnown(string key)
{
	return strstrofs(strcat(" ", VX_OPTKEYS, " "), strcat(" ", key, " ")) >= 0;
}

string
VX_OptGet(string key, string def)
{
	if (!vx_optTab)
		return def;
	return hash_get(vx_optTab, key, def, EV_STRING);
}

void
VX_OptSet(string key, string value)
{
	if (!vx_optTab)
		vx_optTab = hash_createtab(64, HASH_REPLACE);
	value = strreplace("\n", " ", strreplace("\t", " ", value));
	if (value == "")
		hash_delete(vx_optTab, key);
	else
		hash_add(vx_optTab, key, value, EV_STRING);
	serverinfo.SetString(strcat("vxo_", key), value);
}

void
VX_OptClear(void)
{
	int n = tokenize(VX_OPTKEYS);
	for (int i = 0; i < n; i++) {
		tokenize(VX_OPTKEYS);
		VX_OptSet(argv(i), "");
	}
}

string
VX_OptMeta(void)
{
	string m = "";
	int n = tokenize(VX_OPTKEYS);
	for (int i = 0; i < n; i++) {
		tokenize(VX_OPTKEYS);
		string k = argv(i);
		string v = VX_OptGet(k, "");
		if (v != "")
			m = strcat(m, "opt ", k, " ", v, "\n");
	}
	return m;
}

/* Vxl_OutUI_Mode_* name -> CSO mode id. -1 = Studio play rules (39),
   -2 = a mode with no rules here (BLOCKERS.md) */
float
VX_ModeId(string m)
{
	if (m == "" || m == "Play" || m == "Basic")
		return -1;	/* "Basic": INFERRED to be the plain objective map */
	if (stof(m) > 0 || m == "0")
		return stof(m);
	switch (m) {
	case "Original": return 0;
	case "TeamDeath": return 2;
	case "BotTeamDeath": return 5;
	case "ZombieHero": return 14;
	case "BotZombieHero": return 24;
	case "Soccer": return 16;
	case "ZombiEscape": return 20;
	case "ZombieZ": return 45;
	case "PropHunt": return 49;
	case "AllStar": return 40;
	case "GunDeath": return 22;
	/* INFERRED: the Studio variants mp.dll has rules classes for
	   (VoxelShelter 52, VoxelScenarioTX 53) and Zombi 5 (54) */
	case "Shelter": return 52;
	case "ScenarioTX": return 53;
	case "Zombi5": return 54;
	}
	return -2;
}

/* --- save --------------------------------------------------------------- */

string
VX_BuildMeta(void)
{
	string m = "studio 1\n";
	m = strcat(m, "title ", vx_title, "\n", "author ", vx_author, "\n");
	m = strcat(m, "desc ", vx_desc, "\n", "mode ", vx_mapMode, "\n");
	m = strcat(m, VX_OptMeta());
	for (entity r = world; (r = find(r, ::classname, "vxblock"));)
		m = strcat(m, sprintf("rec %s\t%s\t%g\t%s\t%s\n", VX_Key(r.vb_cell), voxel_typename(r.vb_type),
			r.vb_rot, r.vb_props, r.vb_links));
	return m;
}

/* writes data/<path>; runtime variants (open doors, lit lights) are saved
   as their base type, so a save during a test is still a clean map */
bool
VX_SaveFile(string path)
{
	if (vx_live)
		for (entity r = world; (r = find(r, ::classname, "vxblock"));)
			VE_SetVariant(r, false);
	bool ok = voxel_save(path, VX_BuildMeta());
	if (vx_live)
		for (entity r2 = world; (r2 = find(r2, ::classname, "vxblock"));)
			if ((VE_Type(r2) == "DOOR" || VE_Type(r2) == "LIGHT_ONOFF") && r2.vb_state)
				VE_SetVariant(r2, true);
	if (!ok)
		printf("studio: saving data/%s failed: %s\n", path, voxel_error());
	return ok;
}

/* --- the save button ------------------------------------------------------------
   SOURCE-CONFIRMED (GameUI.dll strings, cstrike/resource/voxel/popup_voxelsave.res):
   the in-game menu has a VoxelSaveButton (resource/taskbar/btn_save) whose
   command "VoxelSave" opens CVoxelSaveDlg: "Save map" (#CSO_VoxelSave_Title),
   "This will overwrite the original file. Save?" (#Vxl_OutUI_Save_OK), OK
   and Cancel. #Vxl_OutUI_Save_On_Test refuses it during a test; the upload
   ends in #Vxl_Draft_Upload_OK "Saved!", _Failed or _FailedSaveWorld.
   SOURCE-CONFIRMED (hw.dll FUN_02150c60, FUN_02150d70, FUN_0214c240 case
   0x13): for the map's creator, #Vxl_SaveAlarm "There are recent changes in
   the map.. Please save!" shows for 5 s, 300 s after a change and every
   300 s after that, until a save clears it.
   INFERRED: the alarm is armed by an edit (FUN_02150d70's caller is not
   traced). DESIGN: the save goes to a local file, not CSO's upload; the
   rules publish the state the HUD's dialog needs as serverinfo keys:
   vx_owner, vx_testing, vx_alarm (a count) and vx_saveres ("<count> ok",
   "fail" or "test"). */

float vx_saveAlarm;		/* when the next alarm is due; 0 = not armed */
float vx_alarmSeq, vx_saveSeq;

void
VX_SaveResult(string code)
{
	vx_saveSeq++;
	serverinfo.SetString("vx_saveres", sprintf("%g %s", vx_saveSeq, code));
}

void
VX_SaveAlarmFrame(void)
{
	if (!vx_unsaved || autocvar_studio_savealarm <= 0) {
		vx_saveAlarm = 0;
		return;
	}
	if (vx_testing || vx_owner == "")
		return;
	if (vx_saveAlarm <= 0) {
		vx_saveAlarm = time + autocvar_studio_savealarm;
		return;
	}
	if (time < vx_saveAlarm)
		return;
	vx_saveAlarm = time + autocvar_studio_savealarm;
	vx_alarmSeq++;
	serverinfo.SetString("vx_alarm", ftos(vx_alarmSeq));
	printf("studio %.1f: save alarm\n", time);
}

bool
VX_SaveMap(entity p, string n)
{
	if (vx_testing) {
		VX_Tell(p, "You can't save while testing the script.");
		VX_SaveResult("test");
		return false;
	}
	if (p && !VX_CanEdit(p)) {
		VX_Tell(p, "Only the creator can save this map.");
		return false;
	}
	if (!VX_ValidName(n)) {
		VX_Tell(p, "Map names use letters, digits, _ and -, up to 32 characters.");
		return false;
	}
	if (vx_author == "" && p)
		VX_SetText("author", p.netname);
	if (vx_title == "")
		VX_SetText("title", n);
	if (!VX_SaveFile(VX_MapPath(n))) {
		VX_Tell(p, sprintf("Could not save: %s", voxel_error()));
		VX_SaveResult("fail");
		return false;
	}
	if (vx_mapName)
		strunzone(vx_mapName);
	vx_mapName = strzone(n);
	serverinfo.SetString("vx_map", n);
	vx_dirty = false;
	vx_unsaved = false;
	vx_saveAlarm = 0;
	VX_SaveResult("ok");
	printf("studio %.1f: saved %s (%g blocks, cost %g)\n", time, n, vx_blocks, vx_costTotal);
	VX_Tell(p, sprintf("Saved %s.", n));
	return true;
}

/* --- load ---------------------------------------------------------------- */

/* one "rec" line; false if it does not match the cells */
bool
VX_LoadRecord(string line)
{
	if (tokenizebyseparator(line, "\t") < 3)
		return false;
	string cellS = argv(0), typeS = argv(1), rotS = argv(2), props = argv(3), links = argv(4);
	vector c = VX_Parse(cellS);
	float type = voxel_typeforname(typeS);
	float rot = mod(stof(rotS), 4);

	if (type <= 0 || type >= VX_OPEN_BASE || !VX_InBounds(c) || !VX_NeedsRecord(VX_T(type)))
		return false;
	/* the anchor cell has to hold the block itself */
	vector anchor = VX_GroupCell(type, rot, c, VX_AnchorIndex(type));
	if (voxel_get(anchor) != type)
		return false;
	entity old = VX_RecordAt(anchor);
	if (old)
		VX_FreeRecord(old);
	entity r = VX_NewRecord(c, type, rot);
	r.vb_props = strzone(props);
	r.vb_links = strzone(links);
	return true;
}

/* records for anchor cells that have none; counts blocks and cost */
void
VX_Rescan(void)
{
	vector sz = voxel_size();
	float made = 0;
	vx_blocks = 0;
	vx_costTotal = 0;
	for (float z = 0; z < sz[2]; z++)
		for (float y = 0; y < sz[1]; y++)
			for (float x = 0; x < sz[0]; x++) {
				float t = voxel_get([x, y, z]);
				if (t <= 0)
					continue;
				if (t >= VX_OPEN_BASE)
					t -= VX_OPEN_BASE;	/* a variant left in a world: count the base */
				vx_blocks++;
				vx_costTotal += vx_cost[t];
				if (!VX_NeedsRecord(t) || VX_RecordAt([x, y, z]))
					continue;
				float rot = voxel_getrot([x, y, z]);
				vector base = [x, y, z] - VX_GroupCell(t, rot, [0, 0, 0], VX_AnchorIndex(t));
				if (!VX_InBounds(base))
					continue;
				VX_NewRecord(base, t, rot);
				made++;
			}
	if (made)
		printf("studio: rebuilt %g block records from the cells\n", made);
	VX_Publish();
}

/* replaces the world with data/<path>. On failure the current world stays
   (the engine only swaps it in after the whole file checked out). */
bool
VX_LoadFile(string path)
{
	if (!voxel_load(path)) {
		printf("studio: loading data/%s failed: %s\n", path, voxel_error());
		return false;
	}
	VX_ResetRecords();
	VX_SetText("title", "");
	VX_SetText("author", "");
	VX_SetText("desc", "");
	VX_SetText("mode", "");
	VX_OptClear();

	string meta = voxel_meta();
	float recs = 0, bad = 0;
	bool header = false;
	for (vx_iterPos = 0; VX_More(meta);) {
		string line = VX_NextItem(meta, "\n");
		float sp = strstrofs(line, " ");
		string key = sp < 0 ? line : substring(line, 0, sp);
		string rest = sp < 0 ? "" : substring(line, sp + 1, -1);
		float keep = vx_iterPos;
		switch (key) {
		case "studio":
			header = stof(rest) >= 1;
			break;
		case "title":
		case "author":
		case "desc":
		case "mode":
			VX_SetText(key, rest);
			break;
		case "opt":
			float osp = strstrofs(rest, " ");
			if (osp > 0 && VX_OptKnown(substring(rest, 0, osp)))
				VX_OptSet(substring(rest, 0, osp), substring(rest, osp + 1, -1));
			break;
		case "rec":
			if (VX_LoadRecord(rest))
				recs++;
			else
				bad++;
			break;
		}
		vx_iterPos = keep;
	}
	if (meta != "" && !header)
		print("studio: metadata without a studio header, ignored\n");
	if (bad)
		printf("studio: %g block records did not match the cells and were dropped\n", bad);
	VX_Rescan();
	VXT_ApplyOpts();
	vx_dirty = false;
	printf("studio: loaded data/%s (%g blocks, %g records, %g unknown cells)\n", path, vx_blocks, recs, voxel_unknown());
	return true;
}

bool
VX_LoadMap(entity p, string n)
{
	if (!(VX_ValidName(n) ? VX_LoadFile(VX_MapPath(n)) : false)) {
		VX_Tell(p, sprintf("Could not load %s: %s", n, voxel_error()));
		return false;
	}
	if (vx_mapName)
		strunzone(vx_mapName);
	vx_mapName = strzone(n);
	serverinfo.SetString("vx_map", n);
	vx_unsaved = false;
	for (entity q = world; (q = next.Player(q));) {
		q.vx_undoPos = q.vx_undoTop = 0;	/* history belongs to the old world */
		q.vx_marks = 0;
	}
	VX_Tell(p, sprintf("Loaded %s.", n));
	return true;
}

/* --- autosave, recovery, test snapshot ----------------------------------- */

string
VX_AutosavePath(void)
{
	return strcat(VX_SAVEDIR, "_autosave_", game.GetMap(), ".bwvx");
}

string
VX_SnapshotPath(void)
{
	return strcat(VX_SAVEDIR, "_test_", game.GetMap(), ".bwvx");
}

void
VX_AutosaveFrame(void)
{
	if (autocvar_studio_autosave <= 0 || vx_live || !vx_dirty || time < vx_nextAutosave)
		return;
	/* wait for a quiet moment, at most one period */
	if (time - vx_lastChange < 3 && time < vx_nextAutosave + autocvar_studio_autosave)
		return;
	vx_nextAutosave = time + autocvar_studio_autosave;
	if (VX_SaveFile(VX_AutosavePath())) {
		vx_dirty = false;	/* the autosave holds it; a named save is still needed to publish */
		printf("studio %.1f: autosaved\n", time);
	}
}

bool
VX_Recover(entity p)
{
	if (!fexists(strcat("data/", VX_AutosavePath()))) {
		VX_Tell(p, "There is no autosave for this map.");
		return false;
	}
	if (!VX_LoadFile(VX_AutosavePath())) {
		VX_Tell(p, sprintf("The autosave could not be loaded: %s", voxel_error()));
		return false;
	}
	vx_unsaved = true;	/* the autosave is newer than the named file */
	VX_Tell(p, "Recovered the autosave.");
	return true;
}

/* --- local library (stands in for the online map list) -------------------- */

/* name TAB title TAB author TAB mode TAB plays TAB ratingSum TAB ratings TAB voters */
string vx_lib[VX_LIBMAX];
int vx_libCount;

void
VX_LibLoad(void)
{
	for (int i = 0; i < vx_libCount; i++)
		strunzone(vx_lib[i]);
	vx_libCount = 0;
	filestream f = fopen(VX_LIBRARY, FILE_READ);
	if (f < 0)
		return;
	string s;
	while (vx_libCount < VX_LIBMAX) {
		s = fgets(f);
		if not (s)	/* end of file; "" is a blank line */
			break;
		if (tokenizebyseparator(s, "\t") != 8 || !VX_ValidName(argv(0)))
			continue;	/* damaged line: dropped */
		vx_lib[vx_libCount++] = strzone(s);
	}
	fclose(f);
}

bool
VX_LibSave(void)
{
	filestream f = fopen(VX_LIBRARY, FILE_WRITE);
	if (f < 0) {
		print("studio: cannot write data/" VX_LIBRARY "\n");
		return false;
	}
	for (int i = 0; i < vx_libCount; i++)
		fputs(f, strcat(vx_lib[i], "\n"));
	fclose(f);
	return true;
}

/* sprintf and strcat take at most 8 arguments in this compiler */
string
VX_LibJoin(string a, string b, string c, string d, string e, string f, string g, string h)
{
	return strcat(strcat(a, "\t", b, "\t", c, "\t", d), "\t", strcat(e, "\t", f, "\t", g, "\t", h));
}

int
VX_LibFind(string n)
{
	for (int i = 0; i < vx_libCount; i++) {
		tokenizebyseparator(vx_lib[i], "\t");
		if (argv(0) == n)
			return i;
	}
	return -1;
}

void
VX_LibSet(int i, string line)
{
	if (i < vx_libCount && vx_lib[i])
		strunzone(vx_lib[i]);
	vx_lib[i] = strzone(line);
}

/* publishing lists a saved map; the file must load cleanly first */
bool
VX_Publish_Map(entity p, string n)
{
	if (!VX_ValidName(n) || !fexists(strcat("data/", VX_MapPath(n)))) {
		VX_Tell(p, "Save the map under that name first.");
		return false;
	}
	VX_LibLoad();
	int i = VX_LibFind(n);
	string t = strreplace("\t", " ", vx_title), a = vx_author == "" ? "unknown" : strreplace("\t", " ", vx_author);
	if (i >= 0) {
		tokenizebyseparator(vx_lib[i], "\t");
		VX_LibSet(i, VX_LibJoin(n, t, a, vx_mapMode, argv(4), argv(5), argv(6), argv(7)));
	} else if (vx_libCount < VX_LIBMAX) {
		VX_LibSet(vx_libCount++, VX_LibJoin(n, t, a, vx_mapMode, "0", "0", "0", "-"));
	} else {
		VX_Tell(p, "The library is full.");
		return false;
	}
	VX_LibSave();
	printf("studio %.1f: published %s\n", time, n);
	VX_Tell(p, sprintf("Published %s.", n));
	return true;
}

void
VX_LibPlayed(string n)
{
	VX_LibLoad();
	int i = VX_LibFind(n);
	if (i < 0)
		return;
	tokenizebyseparator(vx_lib[i], "\t");
	VX_LibSet(i, VX_LibJoin(argv(0), argv(1), argv(2), argv(3), ftos(stof(argv(4)) + 1), argv(5), argv(6), argv(7)));
	VX_LibSave();
}

/* one rating of 1-5 per player and map (OBSERVED: maps are rated after play) */
bool
VX_Rate(entity p, string n, float stars)
{
	if (stars < 1 || stars > 5 || stars != floor(stars)) {
		VX_Tell(p, "Ratings are 1 to 5.");
		return false;
	}
	VX_LibLoad();
	int i = VX_LibFind(n);
	if (i < 0) {
		VX_Tell(p, "That map is not published.");
		return false;
	}
	string who = p ? strreplace(",", " ", strreplace("\t", " ", p.netname)) : "console";
	tokenizebyseparator(vx_lib[i], "\t");
	string voters = argv(7);
	if (strstrofs(strcat(",", voters, ","), strcat(",", who, ",")) >= 0) {
		VX_Tell(p, "You already rated this map.");
		return false;
	}
	voters = voters == "-" ? who : strcat(voters, ",", who);
	VX_LibSet(i, VX_LibJoin(argv(0), argv(1), argv(2), argv(3), argv(4),
		ftos(stof(argv(5)) + stars), ftos(stof(argv(6)) + 1), voters));
	VX_LibSave();
	printf("studio %.1f: %s rated %s %g\n", time, who, n, stars);
	return true;
}

void
VX_LibList(entity p)
{
	VX_LibLoad();
	if (!vx_libCount)
		VX_Tell(p, "No published maps.");
	for (int i = 0; i < vx_libCount; i++) {
		tokenizebyseparator(vx_lib[i], "\t");
		float votes = stof(argv(6));
		string head = sprintf("studio: map %s \"%s\" by %s, mode %s", argv(0), argv(1), argv(2), argv(3) == "" ? "Play" : argv(3));
		VX_Tell(p, sprintf("%s, %g plays, rating %.1f (%g)", head, stof(argv(4)), votes ? stof(argv(5)) / votes : 0, votes));
	}
}
