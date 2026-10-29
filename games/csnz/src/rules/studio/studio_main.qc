/* studio_main: the Studio progs (studio.dat): Create (38), Play (39),
 * Hide and Seek (49), Studio Zombie Shelter (52) and Scenario TX (53).
 * Original code (ISC).
 *
 * Evidence
 *   OBSERVED (maploadingtip ids 38, 39, 49; Vxl_OutUI_Mode_Create/Play):
 *     a creation mode where the map is built and a play mode where a
 *     finished map is played for its own objective ("Read the map
 *     description and achieve the objective for that map"); maps are rated
 *     after play.
 *   OBSERVED (CSO_Voxel_Start_TestGame "Begin test", tooltip "Bot added,
 *     starts automatically with 1 player"): the creator tests the map from
 *     the editor and a bot is added when alone.
 *   OBSERVED (Vxl_OutUI_Mode_*): a Studio map names the mode it is played
 *     in (Team deathmatch, Zombie, Soccer, Zombie Escape, Zombie Z, Original,
 *     All-Star, Gun Deathmatch, Hide and Seek, ...); vx_maps.qc maps them to
 *     CSO mode ids and the regular rules of csnz.dat run on the Studio map.
 *   OBSERVED (client strings): tool names VxlEditor, VxlConnector,
 *     VxlMarker, VxlScanner.
 *   DESIGN: the command set (cmd vx ...), that editing is peaceful, that a
 *     test replaces the world with the saved snapshot when it ends, the
 *     plain Play rules (co-op, cleared by an exit block).
 *
 * Commands: INTERFACES.md section 5. Every editing command is also reachable
 * from the server console as "sv vx as <player> <command>" for tests. */

float studio_kind;		/* 38 create, 39 play, 49 hide and seek, 52 shelter, 53 scenario TX */
bool vx_testing;		/* create mode: a test is running */
entity vx_tester;
float vx_playedMarked;
bool sm_ownRules;		/* the plain Studio play rules run (not a regular mode) */

/* vx_tools.qc */
void VXT_Frame(void);
bool VXT_Command(entity p, string cmd);
void VXT_Equip(entity p);
bool VXT_HoldsTool(entity p);
void VXT_SendEnv(entity to);

/* the rules the Studio map's mode would have had (vx_maps.qc VX_ModeId) */
var void(void) SM_InnerStart;
var void(void) SM_InnerRoundStart;
var void(void) SM_InnerFrame;
var float(void) SM_InnerCheck;

/* --- targeting ----------------------------------------------------------- */

vector vx_aimCell;		/* last eye trace: cell hit, '-1 -1 -1' if none */
vector vx_aimFront;		/* the empty cell in front of the hit face */

/* the first occupied cell along a ray, solid or not, closer than dist.
   The engine trace only stops at solid contents, but every tool also
   targets the non-solid blocks (gates, triggers, volumes). OBSERVED: CSNZ
   tools hit gate blocks; DESIGN: a grid walk (DDA) over voxel_get. */
bool
VX_GridWalk(vector src, vector dir, float dist)
{
	float bs = VX_BLOCK;
	vector o = voxel_cellorigin([0, 0, 0]);
	vector sz = voxel_size();
	vector c = voxel_cellforpoint(src);
	vector rel = src - o;
	vector st = [0, 0, 0], tmax = [0, 0, 0], tdelta = [0, 0, 0];
	vector prev = c;

	for (float a = 0; a < 3; a++) {
		float d = dir[a];
		if (d > 0) {
			st[a] = 1;
			tmax[a] = ((c[a] + 1) * bs - rel[a]) / d;
			tdelta[a] = bs / d;
		} else if (d < 0) {
			st[a] = -1;
			tmax[a] = (c[a] * bs - rel[a]) / d;
			tdelta[a] = -bs / d;
		} else {
			st[a] = 0;
			tmax[a] = tdelta[a] = 999999;
		}
	}

	for (float i = 0; i < 128; i++) {
		float t;
		prev = c;
		if (tmax[0] <= tmax[1] && tmax[0] <= tmax[2]) {
			t = tmax[0]; c[0] += st[0]; tmax[0] += tdelta[0];
		} else if (tmax[1] <= tmax[2]) {
			t = tmax[1]; c[1] += st[1]; tmax[1] += tdelta[1];
		} else {
			t = tmax[2]; c[2] += st[2]; tmax[2] += tdelta[2];
		}
		if (t > dist)
			return false;
		if (c[0] < 0 || c[1] < 0 || c[2] < 0 || c[0] >= sz[0] || c[1] >= sz[1] || c[2] >= sz[2])
			continue;
		if (voxel_get(c) > 0) {
			vx_aimCell = c;
			vx_aimFront = prev;
			return true;
		}
	}
	return false;
}

bool
VX_AimReach(entity p, float reach)
{
	makevectors(p.v_angle);
	vector src = p.origin + p.view_ofs;
	traceline(src, src + v_forward * reach, MOVE_NOMONSTERS, p);
	vector hit = voxel_tracecell();
	vector end = trace_endpos, nrm = trace_plane_normal;
	float frac = trace_fraction;

	if (VX_GridWalk(src, v_forward, reach * frac))
		return true;
	vx_aimCell = hit;
	if (vx_aimCell[0] < 0 || frac >= 1) {
		vx_aimCell = vx_aimFront = [-1, -1, -1];
		return false;
	}
	vx_aimFront = voxel_cellforpoint(end + nrm * (VX_BLOCK * 0.5));
	return true;
}

bool
VX_Aim(entity p)
{
	return VX_AimReach(p, autocvar_studio_reach);
}

/* a cell from the arguments (x y z at first) or from where p looks */
vector
VX_ArgCell(entity p, float first, bool front)
{
	if (argv(first + 2) != "")
		return [stof(argv(first)), stof(argv(first + 1)), stof(argv(first + 2))];
	if (!p)
		return [-1, -1, -1];
	if (!VX_Aim(p))
		return [-1, -1, -1];
	return front ? vx_aimFront : vx_aimCell;
}

float
VX_ArgType(string s)
{
	if (s == "")
		return -1;
	float t = stof(s);
	if (t > 0 || s == "0")
		return t;
	return voxel_typeforname(strtoupper(s));
}

/* --- test (create mode) ------------------------------------------------------ */

void
SM_RespawnAll(void)
{
	for (entity p = world; (p = next.Player(p));)
		CS_PutInGame(p);
}

void
SM_StartTest(entity p)
{
	if (vx_testing)
		return;
	if (!VX_SaveFile(VX_SnapshotPath())) {
		VX_Tell(p, "The test could not start: the map could not be saved.");
		return;
	}
	vx_testing = true;
	vx_tester = p;
	VE_Activate();
	/* OBSERVED: a bot is added when the tester is alone; SOURCE-CONFIRMED
	   (CSOGameRulesVoxelPVE "vxl_starttest", FUN_10859420): it stands still
	   ("bot_stop 1" there, bot_pause in Nuclide) */
	if (CS_Players() < 2) {
		localcmd("sv addBot marco 2 0 0 tester_bot\n");
		localcmd("set bot_pause 1\n");
	}
	SM_RespawnAll();
	CS_Banner(CS_Text("CSO_Voxel_Start_TestGame", "Begin test"));
	printf("studio %.1f: test started\n", time);
}

void
SM_EndTest(string why)
{
	if (!vx_testing)
		return;
	VE_Deactivate();
	vx_testing = false;
	localcmd("set bot_pause 0\n");
	/* pistons, explosions, doors: back to the map as it was saved */
	if (!VX_LoadFile(VX_SnapshotPath()))
		print("studio: the test snapshot could not be restored; the world is as the test left it\n");
	vx_dirty = true;
	SM_RespawnAll();
	CS_Banner(sprintf("Test over: %s", why));
	printf("studio %.1f: test over (%s)\n", time, why);
}

/* --- the vx command set ------------------------------------------------------ */

void
VX_Status(entity p)
{
	float recs = 0;
	for (entity r = world; (r = find(r, ::classname, "vxblock"));)
		recs++;
	string head = sprintf("studio: blocks %g/%g cost %g/%g records %g", vx_blocks, autocvar_studio_maxblocks,
		vx_costTotal, autocvar_studio_maxcost, recs);
	VX_Tell(p, sprintf("%s map %s title \"%s\" mode %s %s%s", head, vx_mapName, vx_title,
		vx_mapMode == "" ? "Play" : vx_mapMode, vx_live ? "live" : "editing", vx_dirty ? " (unsaved)" : ""));
}

void
VX_Scan(entity p, vector c)
{
	float t = voxel_get(c);
	if (!VX_InBounds(c) || t <= 0) {
		VX_Tell(p, "studio: scan: empty");
		return;
	}
	entity r = VX_RecordAt(c);
	if (r)
		VX_Tell(p, sprintf("%s entity %s props [%s] links [%s] state %g",
			sprintf("studio: scan %s %s (%s) rot %g cell %s", VX_Key(c), voxel_typename(r.vb_type), VX_Key(r.vb_cell), r.vb_rot, voxel_typename(t)),
			vx_entity[r.vb_type], r.vb_props, r.vb_links, r.vb_state));
	else
		VX_Tell(p, sprintf("studio: scan %s %s rot %g cost %g", VX_Key(c), voxel_typename(t), voxel_getrot(c), vx_cost[t]));
}

bool
VX_Link(entity p, vector from, vector to)
{
	entity s = VX_RecordAt(from), r = VX_RecordAt(to);
	if (!s || !r || s == r) {
		VX_Tell(p, "Link a sender block to a receiver block.");
		return false;
	}
	/* SIGNAL column: 1 send, 2 receive, 3 both (OBSERVED) */
	if (!(vx_signal[s.vb_type] & 1) || !(vx_signal[r.vb_type] & 2)) {
		VX_Tell(p, "Those blocks cannot be linked.");
		return false;
	}
	string key = VX_Key(r.vb_cell);
	if (strstrofs(strcat("|", s.vb_links, "|"), strcat("|", key, "|")) >= 0)
		return true;
	/* SOURCE-CONFIRMED (CVoxelEntity link slots 0xa4..0xb4, 0xb8..0xc8): a
	   block has at most 5 outputs and 5 inputs */
	if (tokenizebyseparator(s.vb_links, "|") >= VE_MAXLINKS || VE_InputCount(r) >= VE_MAXLINKS) {
		VX_Tell(p, "That block has too many links.");
		return false;
	}
	string links = s.vb_links == "" ? key : strcat(s.vb_links, "|", key);
	if (s.vb_links)
		strunzone(s.vb_links);
	s.vb_links = strzone(links);
	vx_dirty = true;
	printf("studio %.1f: linked %s -> %s\n", time, VX_Key(s.vb_cell), key);
	return true;
}

void
VX_Unlink(entity p, vector from)
{
	entity s = VX_RecordAt(from);
	if (!s)
		return;
	if (s.vb_links)
		strunzone(s.vb_links);
	s.vb_links = "";
	vx_dirty = true;
}

/* everything after "vx"; p is the player (world for the console) */
bool
VX_Command(entity p, string line)
{
	float argc = tokenize(line);
	string c = argv(0);
	vector cell;
	entity r;

	/* commands anyone may use */
	switch (c) {
	case "status":
		VX_Status(p);
		return true;
	case "scan":
		cell = VX_ArgCell(p, 1, false);
		VX_Scan(p, cell);
		return true;
	case "list":
		VX_LibList(p);
		return true;
	case "rate":
		VX_Rate(p, argv(2) == "" ? vx_mapName : argv(2), stof(argv(1)));
		return true;
	case "code":
		if (p)
			VE_Code(p, argv(1));
		return true;
	case "coinflip":
	case "rsp":
	case "lottery":
	case "pay":
		if (p)
			VE_GameCommand(p, c, argv(1));
		return true;
	case "hit":
		/* tests: sv vx hit x y z, damage through Nuclide on the block's hitbox */
		if (!p && vx_live) {
			entity hr = VX_RecordAt(VX_ArgCell(world, 1, false));
			entity hp = next.Player(world);
			if (hr && VE_Hittable(VE_Type(hr)) && hr.vb_hitbox && hp) {
				if (argv(4) != "")
					combat.Damage(hr.vb_hitbox, hp, hp, argv(4), hp.origin, '0 0 0', hr.origin);
				else
					hr.vb_hitbox.health -= 30;
			}
		}
		return true;
	case "coins":
		/* tests: sv vx coins <player> <amount> */
		if (!p)
			for (entity cq = world; (cq = next.Player(cq));)
				if (cq.netname == argv(1))
					VE_SetCoins(cq, stof(argv(2)));
		return true;
	case "use":
		/* E on the block at x y z (tests; players use csnz_use) */
		r = VX_RecordAt(VX_ArgCell(p, 1, false));
		entity user = p;
		if (!user)
			for (entity uq = world; (uq = next.Player(uq));)
				if (uq.netname == argv(4) || (argv(4) == "" && !user))
					user = uq;
		if (vx_live && r && user)
			printf("studio %.1f: use %s: %s\n", time, VX_Key(r.vb_cell), VE_Use(r, user) ? "yes" : "nothing");
		return true;
	case "tp":
		/* tests: puts a player on top of cell x y z */
		if (!p) {
			for (entity tq = world; (tq = next.Player(tq));)
				if (tq.netname == argv(1)) {
					vector tc = [stof(argv(2)), stof(argv(3)), stof(argv(4))];
					/* facing yaw argv(5) (default +X) */
					CS_MoveToSpot(tq, voxel_cellorigin(tc) + [VX_BLOCK / 2, VX_BLOCK / 2, -tq.mins[2] + 1], [0, stof(argv(5)), 0]);
					printf("studio %.1f: %s moved to %s\n", time, tq.netname, VX_Key(tc));
				}
		}
		return true;
	}

	if (studio_kind != 38 && c != "reset" && c != "play") {
		VX_Tell(p, "Editing is only possible in Create mode.");
		return true;
	}
	if (!VX_CanEdit(p)) {
		printf("studio %.1f: %s may not edit (creator %s)\n", time, p.netname, vx_owner);
		VX_Tell(p, "Only the creator can edit this map.");
		return true;
	}

	switch (c) {
	case "test":
		SM_StartTest(p);
		return true;
	case "edit":
		SM_EndTest("back to editing");
		return true;
	case "reset":
		/* RESET block or console: the map as saved, entity logic restarted */
		if (vx_testing || studio_kind != 38) {
			VE_Deactivate();
			string snap = studio_kind == 38 ? VX_SnapshotPath() : VX_MapPath(vx_mapName);
			if (VX_LoadFile(snap)) {
				VE_Activate();
				SM_RespawnAll();
			}
		}
		return true;
	case "play":
		/* TRANSIT block: another map, through the map list (DESIGN) */
		if (VX_ValidName(argv(1)) && fexists(strcat("data/", VX_MapPath(argv(1))))) {
			cvars.SetString("studio_map", argv(1));
			localcmd(sprintf("map %s\n", game.GetMap()));
		}
		return true;
	}

	if (vx_testing && c != "allow") {
		VX_Tell(p, "Stop the test first (vx edit).");
		return true;
	}

	switch (c) {
	case "select":
		float st = VX_ArgType(argv(1));
		if (st <= 0 || !vx_placeable[VX_T(st)]) {
			VX_Tell(p, "Unknown block.");
			return true;
		}
		if (p) {
			p.vx_select = st;
			p.vx_selRot = mod(stof(argv(2)), 4);
		}
		VX_Tell(p, sprintf("Selected %s.", voxel_typename(st)));
		return true;
	case "place":
		/* vx place [type [x y z [rot]]] */
		float pt = argv(1) != "" ? VX_ArgType(argv(1)) : (p ? p.vx_select : -1);
		cell = VX_ArgCell(p, 2, true);
		float pr = argv(5) != "" ? mod(stof(argv(5)), 4) : (p ? p.vx_selRot : 0);
		if (pt <= 0 || !vx_placeable[VX_T(pt)]) {
			VX_Tell(p, "Choose a block first.");
			return true;
		}
		if (cell[0] < 0) {
			VX_Tell(p, "Nothing to place against.");
			return true;
		}
		string why = VX_CheckPlace(pt, pr, cell, world);
		if (why != "") {
			VX_Tell(p, why);
			printf("studio %.1f: place refused: %s\n", time, why);
			return true;
		}
		VX_Place(p, pt, pr, cell, true);
		printf("studio %.1f: placed %s at %s\n", time, voxel_typename(pt), VX_Key(cell));
		return true;
	case "remove":
		cell = VX_ArgCell(p, 1, false);
		if (VX_Remove(p, cell, true))
			printf("studio %.1f: removed %s\n", time, VX_Key(cell));
		return true;
	case "pick":
		cell = VX_ArgCell(p, 1, false);
		r = VX_RecordAt(cell);
		float pk = r ? r.vb_type : voxel_get(cell);
		if (pk > 0 && p) {
			p.vx_select = pk;
			VX_Tell(p, sprintf("Selected %s.", voxel_typename(pk)));
		}
		return true;
	case "rotate":
		cell = VX_ArgCell(p, 1, false);
		if (VX_Rotate(p, cell))
			printf("studio %.1f: rotated %s\n", time, VX_Key(cell));
		return true;
	case "mark1":
	case "mark2":
		cell = VX_ArgCell(p, 1, false);
		if (!p || !VX_InBounds(cell))
			return true;
		if (c == "mark1") {
			p.vx_mark1 = cell;
			p.vx_marks = 1;
		} else {
			p.vx_mark2 = cell;
			p.vx_marks = max(p.vx_marks, 1) + 1;
		}
		VX_Tell(p, sprintf("Marked %s.", VX_Key(cell)));
		return true;
	case "fill":
		if (p) {
			float ft = argv(1) != "" ? VX_ArgType(argv(1)) : p.vx_select;
			if (ft < 0 || (ft > 0 && !vx_placeable[VX_T(ft)])) {
				VX_Tell(p, "Unknown block.");
				return true;
			}
			printf("studio %.1f: filled %g cells\n", time, VX_Fill(p, ft, p.vx_selRot));
		}
		return true;
	case "clear":
		if (p)
			printf("studio %.1f: cleared %g cells\n", time, VX_Fill(p, 0, 0));
		return true;
	case "copy":
		if (p)
			printf("studio %.1f: copied %g blocks\n", time, VX_Copy(p));
		return true;
	case "paste":
		cell = VX_ArgCell(p, 1, true);
		if (p && VX_InBounds(cell))
			printf("studio %.1f: pasted %g blocks\n", time, VX_Paste(p, cell));
		return true;
	case "undo":
		if (p)
			printf("studio %.1f: undo %s\n", time, VX_Undo(p) ? "done" : "empty");
		return true;
	case "redo":
		if (p)
			printf("studio %.1f: redo %s\n", time, VX_Redo(p) ? "done" : "empty");
		return true;
	case "link":
		/* "vx link x y z x y z", or Connector style: aim at the sender, then
		   at each receiver */
		if (argv(6) != "") {
			VX_Link(p, [stof(argv(1)), stof(argv(2)), stof(argv(3))], [stof(argv(4)), stof(argv(5)), stof(argv(6))]);
		} else if (p ? VX_Aim(p) : false) {
			if (!p.vx_linking || argv(1) == "new") {
				p.vx_linkFrom = vx_aimCell;
				p.vx_linking = 1;
				VX_Tell(p, "Sender chosen; aim at a receiver and link again.");
			} else {
				VX_Link(p, p.vx_linkFrom, vx_aimCell);
			}
		}
		return true;
	case "unlink":
		VX_Unlink(p, VX_ArgCell(p, 1, false));
		if (p)
			p.vx_linking = 0;
		return true;
	case "prop":
		/* vx prop <key> <value...> [at x y z] */
		string pkey = argv(1), pval = "";
		float at = -1;
		for (float i = 2; i < argc; i++) {
			if (argv(i) == "at") {
				at = i;
				break;
			}
			pval = pval == "" ? argv(i) : strcat(pval, " ", argv(i));
		}
		cell = at > 0 ? [stof(argv(at + 1)), stof(argv(at + 2)), stof(argv(at + 3))] : VX_ArgCell(p, 99, false);
		r = VX_RecordAt(cell);
		if (!r || pkey == "") {
			VX_Tell(p, "Aim at an entity block.");
			return true;
		}
		VX_SetProp(r, strreplace(";", "", strreplace("=", "", pkey)), pval);
		VX_Tell(p, sprintf("studio: %s props [%s]", VX_Key(r.vb_cell), r.vb_props));
		return true;
	case "meta":
		/* vx meta title|author|desc|mode <text> */
		string mkey = argv(1);
		if (mkey != "title" && mkey != "author" && mkey != "desc" && mkey != "mode") {
			VX_Tell(p, "vx meta title|desc|author|mode <text>");
			return true;
		}
		VX_SetText(mkey, substring(line, strstrofs(line, mkey) + strlen(mkey) + 1, -1));
		if (mkey == "mode" && VX_ModeId(vx_mapMode) == -2)
			VX_Tell(p, "That mode has no rules here yet; the map will play with the Studio play rules.");
		vx_dirty = true;
		return true;
	case "tool":
		/* VxlEditor, VxlConnector, VxlMarker, VxlScanner (OBSERVED names) */
		if (p) {
			if (p.vx_tool)
				strunzone(p.vx_tool);
			p.vx_tool = strzone(argv(1));
			userinfo.SetString(p, "vx_tool", argv(1));
		}
		return true;
	case "allow":
	case "deny":
		/* only the creator hands out build rights (DESIGN) */
		if (p && p.netname != vx_owner) {
			VX_Tell(p, "Only the creator can do that.");
			return true;
		}
		for (entity q = world; (q = next.Player(q));)
			if (q.netname == argv(1)) {
				q.vx_allowed = c == "allow";
				VX_Tell(p, sprintf("%s %s build.", q.netname, c == "allow" ? "may" : "may not"));
			}
		return true;
	case "save":
		VX_SaveMap(p, argv(1) == "" ? vx_mapName : argv(1));
		return true;
	case "load":
		VX_LoadMap(p, argv(1));
		return true;
	case "recover":
		VX_Recover(p);
		return true;
	case "publish":
		VX_Publish_Map(p, argv(1) == "" ? vx_mapName : argv(1));
		return true;
	case "new":
		/* an empty world with a floor */
		VX_ResetRecords();
		vector sz = voxel_size();
		float fx = bound(1, stof(argv(1)) ? stof(argv(1)) : 40, sz[0]);
		float fy = bound(1, stof(argv(2)) ? stof(argv(2)) : 40, sz[1]);
		voxel_fill([0, 0, 0], sz - [1, 1, 1], 0, 0);
		vector lo = [floor(sz[0] / 2 - fx / 2), floor(sz[1] / 2 - fy / 2), 0];
		voxel_fill(lo, lo + [fx - 1, fy - 1, 0], vx_firstType, 0);
		VX_Rescan();
		for (entity q2 = world; (q2 = next.Player(q2));)
			q2.vx_undoPos = q2.vx_undoTop = 0;
		VX_SetText("title", "");
		VX_SetText("desc", "");
		vx_dirty = true;
		return true;
	}
	VX_Tell(p, "studio: unknown command (status scan list rate code select place remove pick rotate mark1 mark2 fill clear copy paste undo redo link unlink prop meta tool allow deny save load recover publish new test edit)");
	return true;
}

/* --- mode setup ----------------------------------------------------------------- */

void
SM_CreateEquip(entity p)
{
	ents.ChangeToClass(p, "csm_builder");
	VXT_Equip(p);
	if (vx_owner == "" && clienttype(p) != CLIENTTYPE_BOT)
		vx_owner = strzone(p.netname);
}

float
SM_CreatePickTeam(entity p)
{
	return CS_TEAM2;
}

void
SM_CreateFrame(void)
{
	VXT_Frame();
	VE_Frame();
	if (vx_testing) {
		if (vx_result)
			SM_EndTest(vx_result == 1 ? "round clear" : "round failed");
		else if (vx_tester && (wasfreed(vx_tester) || !(vx_tester.flags & FL_CLIENT)))
			SM_EndTest("the tester left");
	} else {
		VX_AutosaveFrame();
	}
}

void
SM_CreateStart(void)
{
	/* a fresh world for this room unless the map file came with the map */
	if (autocvar_studio_map != "" && VX_ValidName(autocvar_studio_map))
		VX_LoadMap(world, autocvar_studio_map);
	VX_Rescan();
	vx_nextAutosave = time + autocvar_studio_autosave;
	if (fexists(strcat("data/", VX_AutosavePath())))
		print("studio: an autosave exists for this map (vx recover)\n");
}

void
SM_Create(void)
{
	cs_modeTitle = "CSO_GameMode_VxlCreate";
	cs_modeFallback = "Studio: Create";
	cs_teams = false;
	cs_rounds = false;
	cs_respawnDelay = 1;
	cs_timeLimitFallback = 0;
	cs_noStats = true;
	cs_noBuy = true;
	M_Equip = SM_CreateEquip;
	M_Frame = SM_CreateFrame;
	M_Start = SM_CreateStart;
}

/* Play: the map's own objective (DESIGN rules for "Play"/"Basic" maps):
   everyone on the human side, cleared by an exit block, failed by a fail
   exit block, by everyone dying, or by the round time */
float
SM_PlayCheck(void)
{
	if (vx_result == 1)
		return CS_TEAM2;
	if (vx_result == 2)
		return CS_TEAM1;
	if (CS_Alive(CS_TEAM2) == 0 && CS_TeamSize(CS_TEAM2) > 0)
		return CS_TEAM1;
	return 0;
}

/* every round starts from the map as saved */
void
SM_PlayRoundStart(void)
{
	VE_Deactivate();
	VX_LoadFile(VX_SnapshotPath());
	VE_Activate();
	SM_RespawnAll();
	if (SM_InnerRoundStart)
		SM_InnerRoundStart();
}

void
SM_PlayFrame(void)
{
	VE_Frame();
	if (SM_InnerFrame)
		SM_InnerFrame();
}

float
SM_PlayCheckAll(void)
{
	/* exit blocks decide first, then the mode's own rules */
	if (vx_result == 1)
		return CS_TEAM2;
	if (vx_result == 2)
		return CS_TEAM1;
	if (SM_InnerCheck)
		return SM_InnerCheck();
	return sm_ownRules ? SM_PlayCheck() : 0;
}

void
SM_PlayStart(void)
{
	/* the world as the match starts is what every round goes back to */
	if (!VX_SaveFile(VX_SnapshotPath()))
		print("studio: no round snapshot; rounds keep the world as it is\n");
	/* markers first (goal nets, spawns, bomb sites): the mode's start reads them */
	VE_Activate();
	if (SM_InnerStart)
		SM_InnerStart();
	if (vx_desc != "")
		CS_Banner(vx_desc);
}

void
SM_PlayRoundEnded(float winner)
{
	CS_Banner(winner == CS_TEAM2 ? CS_Text("CSO_Voxel_RoundClear", "Round clear!") : CS_Text("CSO_Voxel_RoundFail", "Round failed"));
	if (!vx_playedMarked && vx_mapName != "") {
		vx_playedMarked = 1;
		VX_LibPlayed(vx_mapName);
		for (entity p = world; (p = next.Player(p));)
			if (clienttype(p) != CLIENTTYPE_BOT)
				CS_Tell(p, "Rate this map: cmd vx rate <1-5>");
	}
}

void
SM_PlayRules(void)
{
	cs_modeTitle = "CSO_GameMode_VxlPlay";
	cs_modeFallback = "Studio: Play";
	cs_teams = true;
	cs_rounds = true;
	cs_oneSideStart = true;
	cs_respawnDelay = -1;
	cs_winLimitFallback = 3;
	cs_roundTimeFallback = 5;
	cs_timeLimitFallback = 30;
	cs_noStats = true;
	cs_noBuy = true;
	cs_team1Name = "Map";
	cs_team2Name = "Players";
	M_PickTeam = SM_CreatePickTeam;
	M_CheckRound = SM_PlayCheck;
	M_RoundEnded = SM_PlayRoundEnded;
}

/* Play (39) or a Studio map with a regular mode */
void
SM_Play(float forcedId)
{
	string n = autocvar_studio_map;
	/* nested: && does not short-circuit, and an empty name means "no map" */
	if (n != "")
		if (!VX_LoadMap(world, n))
			printf("studio: map %s could not be loaded, playing the world this map booted\n", n);
	float id = forcedId >= 0 ? forcedId : VX_ModeId(vx_mapMode);

	if (id == -2)
		printf("studio: the map's mode \"%s\" has no rules here (BLOCKERS.md); playing with the Studio play rules\n", vx_mapMode);
	if (id == 49) {
		CSM_PropHunt();
		cs_modeRun = 49;
	} else if (id >= 0 && id != 38 && id != 39 && CS_SelectMode(id)) {
		printf("studio: playing the map with the rules of mode %g\n", id);
		cs_modeRun = id;
	} else {
		SM_PlayRules();
		sm_ownRules = true;
		cs_modeRun = 39;
	}
	/* the Studio layer goes around whatever rules run */
	SM_InnerStart = M_Start;
	SM_InnerRoundStart = M_RoundStart;
	SM_InnerFrame = M_Frame;
	SM_InnerCheck = M_CheckRound != SM_PlayCheck ? M_CheckRound : __NULL__;
	M_Start = SM_PlayStart;
	M_RoundStart = SM_PlayRoundStart;
	M_Frame = SM_PlayFrame;
	M_CheckRound = SM_PlayCheckAll;
	if (!M_RoundEnded)
		M_RoundEnded = SM_PlayRoundEnded;
}

/* --- callbacks ------------------------------------------------------------------- */

void
CodeCallback_Precache(void)
{
	precache.Entity("csm_rifleman");
	precache.Entity("csm_builder");
	precache.Entity("csm_seeker");
	precache.Entity("csm_prop");
	precache.Entity("csm_zombie");
	precache.Entity("csm_zombie_origin");
	precache.Entity("csm_hero");
	precache.Entity("csm_basic");
	precache.Entity("csm_vip");
	CS_PrecacheWeapons();
	precache.Entity("csm_vxmonster");
	precache.Entity("csm_vxboss");
	precache_model("models/w_backpack.mdl");
	precache_model("models/hostage.mdl");
	precache_sound("misc/talk.wav");
	precache_sound("events/lvup.wav");
	precache_sound("studio/scenariotx/levelup.wav");
	precache_sound("weapons/vxllonggun-1.wav");
	precache_sound("weapons/vxllonggun-2.wav");
	precache_sound("weapons/vxllonggun_changea.wav");
	precache_sound("weapons/vxlconnector-1.wav");
	precache_sound("weapons/vxleditor-shootleft.wav");
	precache.Entity("weapon_vxllonggun");
	precache.Entity("weapon_vxlshortgun");
	precache.Entity("weapon_vxlconnector");
	precache.Entity("weapon_vxleditor");
	precache.Entity("weapon_vxlminigun");
	precache.Entity("weapon_vxlscanner");
	precache.Entity("weapon_vxlmarker");
}

string
CodeCallback_Title(void)
{
	return ("studio");
}

void
CodeCallback_StartGameType(void)
{
	studio_kind = autocvar_csnz_mode;
	if (studio_kind != 38 && studio_kind != 39 && studio_kind != 49 && studio_kind != 52 && studio_kind != 53) {
		printf("studio: mode %g is not a Studio mode (38, 39, 49, 52, 53); using Create\n", studio_kind);
		studio_kind = 38;
	}
	cs_noStats = false;
	cs_respawnDelay = 2;
	cs_modeRun = studio_kind;

	/* the world first: a Studio map's mode decides the rules */
	vx_records = hash_createtab(4096, EV_FLOAT);
	if (!VX_Boot([128, 128, 64])) {
		/* any map can host the Studio: a floor in the middle (DESIGN) */
		voxel_create([128, 128, 64], VX_NEWBLOCK, [-64 * VX_NEWBLOCK, -64 * VX_NEWBLOCK, -64]);
		VX_LoadCatalog();
		voxel_fill([44, 44, 0], [83, 83, 0], vx_firstType, 0);
	}
	VX_Rescan();
	if (vx_mapName == "")
		vx_mapName = strzone(game.GetMap());

	if (studio_kind == 38)
		SM_Create();
	else if (studio_kind == 39)
		SM_Play(-1);
	else
		SM_Play(studio_kind);	/* 49, 52, 53: a Studio map played with that mode's rules */

	CS_Init();
	CS_ApplyLimits(cs_modeRun);
	serverinfo.SetInteger("csnz_moderun", (int)cs_modeRun);
	serverinfo.SetInteger("vx_kind", (int)studio_kind);
	printf("studio: mode %g (%s), %s, %s\n", studio_kind, CS_Text(cs_modeTitle, cs_modeFallback),
		cs_teams ? "teams" : "free for all", cs_rounds ? "rounds" : "continuous");
	if (M_Start)
		M_Start();
}

void
CodeCallback_FrameStart(void)
{
	CS_Frame();
	CSNZ_Frame();
	/* the editor runs outside rounds too */
	if (studio_kind == 38 && cs_state != CS_LIVE)
		SM_CreateFrame();
}

void
CodeCallback_PlayerSpawn(entity p)
{
	p.cs_kills = p.cs_deaths = p.cs_score = 0;
	if (p.cs_money <= 0)
		CS_SetMoney(p, autocvar_mp_startmoney);
	CSNZ_PlayerSpawned(p);
	if (cs_rounds && cs_state == CS_LIVE && cs_respawnDelay < 0) {
		if (M_LateJoin)
			if (M_LateJoin(p))
				return;
		if (cs_teams)
			CS_SetTeam(p, M_PickTeam ? M_PickTeam(p) : CS_AutoTeam(p));
		ents.ChangeToClass(p, "spectator");
		return;
	}
	CS_PutInGame(p);
	VE_Respawned(p);
	VXT_SendEnv(p);
}

bool
CodeCallback_PlayerRequestRespawn(entity p)
{
	if (cs_state == CS_MATCHEND)
		return (false);
	if (cs_rounds && cs_respawnDelay < 0 && cs_state != CS_WAIT)
		return (false);
	if (p.cs_respawnAt > time)
		return (false);
	CS_PutInGame(p);
	VE_Respawned(p);
	return (true);
}

/* editing is peaceful (DESIGN); tests and play are not */
bool
CodeCallback_PlayerCanAttack(entity p)
{
	if (studio_kind == 38 && !vx_testing)
		return (false);
	/* the build tools never shoot (INFERRED) */
	if (studio_kind == 38 && VXT_HoldsTool(p))
		return (false);
	return (true);
}

void
CodeCallback_PlayerDamage(entity victim, entity attacker, entity damageDecl)
{
	CSNZ_Damaged(victim, attacker);
	if (M_Damaged)
		M_Damaged(victim, attacker);
}

void
CodeCallback_PlayerKilled(entity p, entity inflictor, entity attacker, string weapon)
{
	CS_PlayerKilled(p, inflictor, attacker, weapon);
	CSNZ_Killed(p, inflictor, attacker, weapon);
}

/* Nuclide's Monster.qc calls NPCDeath(npc, inflictor, attacker) while the
   delegate names them (npc, attacker, inflictor); take whichever is a player */
void
CodeCallback_NPCKilled(entity npc, entity a, entity b)
{
	entity credit = (b.flags & FL_CLIENT) ? b : a;
	if (M_NPCKilled)
		M_NPCKilled(npc, credit);
}

void
CodeCallback_PlayerDisconnect(entity p)
{
	if (M_Disconnect)
		M_Disconnect(p);
	CS_DropBomb(p);
	CSNZ_Disconnect(p);
}

/* E on whatever block the player looks at, then the mode's own use */
bool
SM_Use(entity p)
{
	if (!vx_live || !VX_Aim(p))
		return false;
	makevectors(p.v_angle);
	if (vlen(trace_endpos - (p.origin + p.view_ofs)) > 96)
		return false;
	entity r = VX_RecordAt(vx_aimCell);
	return r && VE_Use(r, p);
}

bool
CodeCallback_ClientCommand(entity p, string cmd)
{
	tokenize(cmd);
	if (argv(0) == "csnz_use")
		if (SM_Use(p))
			return (true);
	if (M_Command)
		if (M_Command(p, cmd))
			return (true);
	if (CSNZ_Command(p, cmd))
		return (true);

	if (VXT_Command(p, cmd))
		return (true);

	tokenize(cmd);
	switch (argv(0)) {
	case "vx":
		VX_Command(p, substring(cmd, 3, -1));
		return (true);
	case "buy":
		CS_Buy(p, argv(1));
		return (true);
	case "csnz_stats":
		CS_StatsPrint(p, argv(1) == "" ? p.netname : argv(1));
		return (true);
	case "csnz_use":
		return (true);
	}
	return (false);
}

bool
CodeCallback_ConsoleCommand(entity p, string cmd)
{
	if (M_Command)
		if (M_Command(world, cmd))
			return (true);

	tokenize(cmd);
	switch (argv(0)) {
	case "vx":
		VX_Command(world, substring(cmd, 3, -1));
		return (true);
	case "csnz_as":
		/* "csnz_as <player> <client command>": tests drive players with it */
		string who = argv(1);
		string rest = substring(cmd, strstrofs(cmd, who, 7) + strlen(who) + 1, -1);
		for (entity q = world; (q = next.Player(q));)
			if (q.netname == who) {
				if (rest == "csnz_use") {
					bool used = SM_Use(q);
					if (!used && M_Command)
						used = M_Command(q, rest);
					printf("studio %.1f: %s use: %s\n", time, who, used ? "yes" : "nothing");
				} else if (!CodeCallback_ClientCommand(q, rest))
					printf("studio: %s: unknown command %s\n", who, rest);
				return (true);
			}
		printf("studio: no player %s\n", who);
		return (true);
	case "csnz_status":
		printf("csnz: mode %g state %s round %i score %g:%g goal %g players %i\n", cs_modeRun,
			CS_StateName(), cs_round, cs_score1, cs_score2, cs_goal, CS_Players());
		for (entity q2 = world; (q2 = next.Player(q2));)
			printf("csnz:  %s team %g hp %g kills %g money %g score %g at %v\n", q2.netname, q2.team,
				q2.health, q2.cs_kills, q2.cs_money, q2.cs_score, q2.origin);
		VX_Status(world);
		return (true);
	case "csnz_stats":
		CS_StatsPrint(world, argv(1));
		return (true);
	}
	return (false);
}
