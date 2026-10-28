/* vx_tools: the Studio Create tools held in the hands (block guns, the
 * connector, the spinner, the scanner), the five quick slots, the block
 * pick sent by the browser, and the World settings commands.
 * Original code (ISC).
 *
 * Evidence
 *   SOURCE-CONFIRMED (mp.dll):
 *     - weapon_vxllonggun (weapon id 300) primary FUN_112578b0: an eye trace
 *       of 2000 units (DAT_11cd3150), next shot 0.2 s (DAT_11cd3140); the
 *       weapon's mode (+0x1c0) picks placing (FUN_11257d50) or removing
 *       (FUN_11258360). Secondary FUN_11257aa0 flips the mode, next 1.0 s.
 *     - weapon_vxlshortgun (335) FUN_1125a860: the same with 100 units
 *       (DAT_11cd4f28), and FUN_11257d50's last argument 1, which with the
 *       item text "Can also create in the air" (CSO_Item_Desc_vxlshortgun)
 *       places at the end of the reach when nothing is hit (that reading of
 *       the flag is INFERRED).
 *     - weapon_vxlminigun (363) FUN_112596f0: 2000 units; the delay starts
 *       at 0.2 s (DAT_11cd4050) and drops 0.03 s a shot (DAT_11cd4060) to
 *       0.05 s (DAT_11cd4058); letting go resets it (FUN_11259ac0).
 *     - FUN_11257d50, placing: only in mode 38 (0x26); refused during a test
 *       (#CSO_Script_ImpossibleOnTest); the block is the player's voxel
 *       selection (hw.dll vxl_set); the cell in front of the hit face; the
 *       rotation from the player's yaw (FUN_1064fa70: <45 3, <135 1, <225 2,
 *       <315 0, else 3, i.e. quarter turns) unless the block carries one;
 *       errors in the order overlap, block count, cost, other
 *       (#CSO_Voxel_CantPlaceVoxelSomethingOverlap, _BlockCountOver,
 *       _BlockCostOver, _CantPlaceVoxel), shown as centre text (type 4).
 *     - FUN_11258360, removing: the hit cell's block goes.
 *     - weapon_vxlconnector (301) FUN_112555a0: 200 units (DAT_11cd1af8),
 *       next 0.7 s. Mode 0: the first block hit that sends is remembered;
 *       the next hit that receives, if another block, is linked from it
 *       (FUN_10a643b0); either way the memory clears. Mode 1: a block that
 *       sends or receives loses all its links (FUN_10a5f260). Secondary
 *       FUN_11255940 flips the mode and forgets the sender, next 1.0 s.
 *     - weapon_vxleditor (334, "Spinner") FUN_112560d0: 200 units
 *       (DAT_11cd2260), next 0.2 s; primary and secondary rotate the hit
 *       block opposite ways (hw.dll FUN_0224a2d0 with the tables at
 *       02babfe0/02babff8: primary clockwise seen from above, secondary
 *       counter-clockwise), #CSO_Voxel_CantRotateVoxel on failure.
 *     - weapon_vxlscanner (364) FUN_1125a1a0: 1000 units (DAT_11cd47e8),
 *       next 0.7 s (DAT_11cd47e0); hands the hit cell to the engine copy
 *       (vtable 0x168) with the attack as argument. CSO_Item_Desc_vxlscanner:
 *       left copies the look, right copies properties and rotation.
 *     - ClientCommand FUN_10856b50 "vxl_wpn <slot> <id>" -> FUN_10855810:
 *       slot 0..4, id 1..0x1020; ids 0x21f-0x2e6, 0x807-0x9c3, 0xfef-0x1021
 *       are refused in create mode (#CSO_Restrict_VoxelCreateMode); the
 *       slot's weapon is given and drawn and the slot becomes current, the
 *       old current one becomes the last (for lastinv).
 *   SOURCE-CONFIRMED (client.dll): five weapon quick slots (DAT_03584c5c),
 *     defaulting to 300 335 301 334 (DAT_031744cc) when the .vxlqs registry
 *     value is missing; keys slot1-5 in mode 38 send "vxl_wpn <n-1> <id>"
 *     (FUN_02a21e90 -> FUN_02b1a110); the browser's block double click
 *     sends "vxl_set <id>" (FUN_02b19d50) and a Tool page double click
 *     "vxl_wpn 0 <id>" (FUN_02b17290).
 *   SOURCE-CONFIRMED (hw.dll FUN_021f0e60): "vxl_set <id>" in mode 38 checks
 *     the block type exists, then the theme (UNRESOLVED here: no theme
 *     ownership), and stores the id as the player's selection.
 *   INFERRED: that our rotation 0..3 (counter-clockwise quarter turns from
 *     +X) faces the same way as CSO's for the same yaw; the minigun places
 *     like the long gun (it shares the ignore-the-shooter rule for ids 300
 *     and 363); the tools never fire bullets during a test.
 *   DESIGN: tools run from the server frame, not through Nuclide's weapon
 *     code; the HUD forwards the +attack / +attack2 keys as "vxl_fire <1|2>
 *     <0|1>" because Nuclide never fills button0 for the rules progs; the
 *     marker (485) sets the two corners of vx fill/copy; the scanner's
 *     "properties" copy keeps rotation and props for the next placements;
 *     slots are kept per player for the session (the client keeps its own
 *     copy in the vx_quickslots cvar and sends it with vxl_qsload).
 *   UNRESOLVED: the tool table behind ids 4000-4129 (mp DAT_11e5acfc+0x11c),
 *     block rotation presets, themes, junk gun (366), grenade (2038).
 */

#define VXT_SLOTS 5
#define VXT_DEFAULTS "300 335 301 334 0"

.string vxt_slots;		/* "id id id id id" */
.float vxt_cur, vxt_last;
.float vxt_next;		/* next tool use */
.float vxt_mode;		/* block guns / connector: 0 primary use, 1 the other */
.float vxt_modeNext;
.float vxt_rate;		/* minigun: current delay */
.float vxt_from;		/* connector: a sender is chosen */
.vector vxt_fromCell;
.float vxt_copyRot;		/* scanner (properties): rotation for placements, -1 none */
.string vxt_copyProps;
.float vxt_b1, vxt_b2;		/* held buttons, from the HUD's vxl_fire */

/* weapon id (item.csv column 5) -> entityDef */
string
VXT_Class(float wid)
{
	switch (wid) {
	case 300: return "weapon_vxllonggun";
	case 301: return "weapon_vxlconnector";
	case 334: return "weapon_vxleditor";
	case 335: return "weapon_vxlshortgun";
	case 363: return "weapon_vxlminigun";
	case 364: return "weapon_vxlscanner";
	case 485: return "weapon_vxlmarker";
	}
	return "";
}

float
VXT_Slot(entity p, float s)
{
	tokenize(p.vxt_slots);
	return stof(argv(s));
}

void
VXT_SetSlot(entity p, float s, float wid)
{
	string out = "";
	tokenize(p.vxt_slots);
	for (float i = 0; i < VXT_SLOTS; i++) {
		string v = (i == s) ? ftos(wid) : (argv(i) == "" ? "0" : argv(i));
		out = (i == 0) ? v : strcat(out, " ", v);
	}
	if (p.vxt_slots)
		strunzone(p.vxt_slots);
	p.vxt_slots = strzone(out);
	userinfo.SetString(p, "vx_qs", out);
}

/* the tool in the hands: the current quick slot's */
float
VXT_Held(entity p)
{
	if (p.vxt_slots == "")
		return 0;
	return VXT_Slot(p, p.vxt_cur);
}

bool
VXT_HoldsTool(entity p)
{
	return VXT_Class(VXT_Held(p)) != "";
}

void
VXT_Draw(entity p, float slot)
{
	string cls = VXT_Class(VXT_Slot(p, slot));
	if (slot != p.vxt_cur) {
		p.vxt_last = p.vxt_cur;
		p.vxt_cur = slot;
	}
	userinfo.SetInteger(p, "vx_qscur", (int)p.vxt_cur);
	p.vxt_mode = 0;
	p.vxt_from = 0;
	p.vxt_rate = 0.2;
	if (cls != "") {
		ents.Input(p, "GiveItem", cls, world);
		ents.Input(p, "SwitchToWeapon", cls, world);
	}
}

/* Create mode loadout: every quick slot's tool, the first one drawn */
void
VXT_Equip(entity p)
{
	if (p.vxt_slots == "") {
		p.vxt_slots = strzone(VXT_DEFAULTS);
		userinfo.SetString(p, "vx_qs", p.vxt_slots);
	}
	p.vxt_copyRot = -1;
	for (float i = VXT_SLOTS - 1; i >= 0; i--) {
		string cls = VXT_Class(VXT_Slot(p, i));
		if (cls != "")
			ents.Input(p, "GiveItem", cls, world);
	}
	VXT_Draw(p, p.vxt_cur);
}

/* --- the tools ----------------------------------------------------------------- */

float
VXT_YawRot(float yaw)
{
	/* FUN_1064fa70's quarters, in our counter-clockwise numbering */
	yaw = mod(mod(yaw, 360) + 360, 360);
	return mod(floor((yaw + 45) / 90), 4);
}

void
VXT_Fail(entity p, string why)
{
	CS_Tell(p, why);
	printf("studio %.1f: %s: %s\n", time, p.netname, why);
}

void
VXT_Place(entity p, float reach, bool inAir)
{
	float t = p.vx_select;
	vector cell;

	if (vx_testing) {
		VXT_Fail(p, CS_Text("CSO_Script_ImpossibleOnTest", "You can't do that while testing the script"));
		return;
	}
	if (t <= 0 || !vx_placeable[VX_T(t)]) {
		VXT_Fail(p, CS_Text("CSO_Voxel_CantPlaceVoxel", "Block cannot be placed."));
		return;
	}
	if (VX_AimReach(p, reach)) {
		cell = vx_aimFront;
	} else if (inAir) {
		makevectors(p.v_angle);
		cell = voxel_cellforpoint(p.origin + p.view_ofs + v_forward * reach);
	} else {
		return;
	}
	float rot = (p.vxt_copyRot >= 0) ? p.vxt_copyRot : VXT_YawRot(p.v_angle[1]);
	string why = VX_CheckPlace(t, rot, cell, world);
	if (why != "") {
		VXT_Fail(p, why);
		return;
	}
	entity r = VX_Place(p, t, rot, cell, true);
	if (r && p.vxt_copyProps != "") {
		int n = (int)tokenizebyseparator(p.vxt_copyProps, ";");
		for (int i = 0; i < n; i++) {
			tokenizebyseparator(p.vxt_copyProps, ";");
			string kv = argv(i);
			float eq = strstrofs(kv, "=");
			if (eq > 0)
				VX_SetProp(r, substring(kv, 0, eq), substring(kv, eq + 1, -1));
		}
	}
	sound(p, CHAN_WEAPON, "weapons/vxllonggun-1.wav", 1, ATTN_NORM);
	printf("studio %.1f: %s placed %s at %s rot %g\n", time, p.netname, voxel_typename(t), VX_Key(cell), rot);
}

void
VXT_Remove(entity p, float reach)
{
	if (vx_testing) {
		VXT_Fail(p, CS_Text("CSO_Script_ImpossibleOnTest", "You can't do that while testing the script"));
		return;
	}
	if (!VX_AimReach(p, reach))
		return;
	vector c = vx_aimCell;
	if (VX_Remove(p, c, true)) {
		sound(p, CHAN_WEAPON, "weapons/vxllonggun-2.wav", 1, ATTN_NORM);
		printf("studio %.1f: %s removed %s\n", time, p.netname, VX_Key(c));
	}
}

/* FUN_10a5f260: a block's links both ways */
void
VXT_UnlinkAll(entity r)
{
	string key = VX_Key(r.vb_cell);
	if (r.vb_links)
		strunzone(r.vb_links);
	r.vb_links = "";
	for (entity o = world; (o = find(o, ::classname, "vxblock"));)
		if (o != r && strstrofs(strcat("|", o.vb_links, "|"), strcat("|", key, "|")) >= 0) {
			string kept = strreplace(strcat("|", key, "|"), "|", strcat("|", o.vb_links, "|"));
			kept = substring(kept, 1, max(0, strlen(kept) - 2));
			if (o.vb_links)
				strunzone(o.vb_links);
			o.vb_links = strzone(kept);
		}
	vx_dirty = true;
}

void
VXT_Connector(entity p)
{
	if (vx_testing) {
		VXT_Fail(p, CS_Text("CSO_Script_ImpossibleOnTest", "You can't do that while testing the script"));
		return;
	}
	sound(p, CHAN_WEAPON, "weapons/vxlconnector-1.wav", 1, ATTN_NORM);
	entity r = VX_AimReach(p, 200) ? VX_RecordAt(vx_aimCell) : world;
	if (cvar("studio_debug") > 1)
		printf("studio %.1f: connector aim %v sig %g mode %g\n", time, vx_aimCell, r ? vx_signal[r.vb_type] : -1, p.vxt_mode);
	if (p.vxt_mode == 0) {
		if (r && !p.vxt_from && (vx_signal[r.vb_type] & 1)) {
			p.vxt_from = 1;
			p.vxt_fromCell = r.vb_cell;
			printf("studio %.1f: %s connector from %s\n", time, p.netname, VX_Key(r.vb_cell));
			return;
		}
		if (r && p.vxt_from && (vx_signal[r.vb_type] & 2) && VX_Key(r.vb_cell) != VX_Key(p.vxt_fromCell))
			VX_Link(p, p.vxt_fromCell, r.vb_cell);
	} else if (r && vx_signal[r.vb_type]) {
		VXT_UnlinkAll(r);
		printf("studio %.1f: %s unlinked %s\n", time, p.netname, VX_Key(r.vb_cell));
	}
	p.vxt_from = 0;
}

void
VXT_Spinner(entity p, float d)
{
	if (vx_testing) {
		VXT_Fail(p, CS_Text("CSO_Script_ImpossibleOnTest", "You can't do that while testing the script"));
		return;
	}
	sound(p, CHAN_WEAPON, "weapons/vxleditor-shootleft.wav", 1, ATTN_NORM);
	if (!VX_AimReach(p, 200))
		return;
	vector c = vx_aimCell;
	if (VX_RotateBy(p, c, d))
		printf("studio %.1f: %s rotated %s by %g\n", time, p.netname, VX_Key(c), d);
	else
		VXT_Fail(p, CS_Text("CSO_Voxel_CantRotateVoxel", "Cannot rotate the block."));
}

void
VXT_Scanner(entity p, bool withProps)
{
	if (!VX_AimReach(p, 1000))
		return;
	entity r = VX_RecordAt(vx_aimCell);
	float t = r ? r.vb_type : voxel_get(vx_aimCell);
	if (t <= 0 || !vx_placeable[VX_T(t)])
		return;
	p.vx_select = t;
	if (p.vxt_copyProps)
		strunzone(p.vxt_copyProps);
	p.vxt_copyProps = "";
	p.vxt_copyRot = -1;
	if (withProps) {
		p.vxt_copyRot = r ? r.vb_rot : voxel_getrot(vx_aimCell);
		if (r && r.vb_props != "")
			p.vxt_copyProps = strzone(r.vb_props);
	}
	userinfo.SetInteger(p, "vx_block", (int)t);
	CS_Tell(p, voxel_typename(t));
	printf("studio %.1f: %s scanned %s%s\n", time, p.netname, voxel_typename(t), withProps ? " with rotation and properties" : "");
}

void
VXT_Marker(entity p, float which)
{
	if (!VX_AimReach(p, 2000))
		return;
	VX_Command(p, sprintf("mark%g %g %g %g", which, vx_aimCell[0], vx_aimCell[1], vx_aimCell[2]));
}


void
VXT_PlayerFrame(entity p)
{
	float wid = VXT_Held(p);
	bool a1 = p.vxt_b1 ? true : false;
	bool a2 = p.vxt_b2 ? true : false;

	if (wid == 363 && !a1)
		p.vxt_rate = 0.2;
	if (time < p.vxt_next)
		return;

	switch (wid) {
	case 300:	/* long gun */
	case 335:	/* short gun */
	case 363:	/* minigun */
		if (a2) {
			p.vxt_mode = !p.vxt_mode;
			p.vxt_next = time + 1.0;
			CS_Tell(p, p.vxt_mode ? "Remove mode" : "Create mode");
			sound(p, CHAN_WEAPON, "weapons/vxllonggun_changea.wav", 1, ATTN_NORM);
			return;
		}
		if (!a1)
			return;
		float reach = 2000;
		if (wid == 335)
			reach = 100;
		if (p.vxt_mode)
			VXT_Remove(p, reach);
		else
			VXT_Place(p, reach, wid == 335);
		if (wid == 363) {
			p.vxt_next = time + p.vxt_rate;
			p.vxt_rate = max(0.05, p.vxt_rate - 0.03);
		} else
			p.vxt_next = time + 0.2;
		return;
	case 301:	/* connector */
		if (a2) {
			p.vxt_mode = !p.vxt_mode;
			p.vxt_from = 0;
			p.vxt_next = time + 1.0;
			CS_Tell(p, p.vxt_mode ? "Remove connections" : "Connect");
			return;
		}
		if (a1) {
			VXT_Connector(p);
			p.vxt_next = time + 0.7;
		}
		return;
	case 334:	/* spinner */
		if (a1 || a2) {
			VXT_Spinner(p, a1 ? -1 : 1);
			p.vxt_next = time + 0.2;
		}
		return;
	case 364:	/* scanner */
		if (a1 || a2) {
			VXT_Scanner(p, a2);
			p.vxt_next = time + 0.7;
		}
		return;
	case 485:	/* marker */
		if (a1 || a2) {
			VXT_Marker(p, a1 ? 1 : 2);
			p.vxt_next = time + 0.3;
		}
		return;
	}
}

void
VXT_Frame(void)
{
	for (entity p = world; (p = next.Player(p));) {
		if (p.health <= 0 || p.classname == "spectator" || !VX_CanEdit(p))
			continue;
		VXT_PlayerFrame(p);
	}
}

/* --- World settings ------------------------------------------------------------- */

void
VXT_SendEnv(entity to)
{
	string sky = VX_OptGet("skyname", "");
	string cmd = "";

	/* DESIGN: CSO's fog density 0..1 and view distance 0..1 as one FTE fog */
	if (stof(VX_OptGet("fogon", "0"))) {
		tokenize(VX_OptGet("fogcolor", "128 128 128"));
		vector rgb = [stof(argv(0)), stof(argv(1)), stof(argv(2))] / 255;
		float d = 0.2 + 1.8 * stof(VX_OptGet("fogdensity", "0.5"));
		d *= 1.5 - stof(VX_OptGet("visdist", "0.5"));
		cmd = sprintf("fog %g %g %g %g\n", d, rgb[0], rgb[1], rgb[2]);
	} else {
		cmd = "fog 0\n";
	}
	if (sky != "" && sky != "none")
		cmd = strcat(cmd, "sky ", sky, "\n");
	for (entity p = world; (p = next.Player(p));)
		if ((!to || p == to) && clienttype(p) == CLIENTTYPE_REAL)
			stuffcmd(p, cmd);
}

/* which settings act here (the rest are kept with the map and shown) */
void
VXT_ApplyOpts(void)
{
	VXT_SendEnv(world);
	string gm = VX_OptGet("gamemode", "");
	if (gm != "" && gm != vx_mapMode)
		VX_SetText("mode", gm);
}

/* "vxl_<key> <args>" from the World settings page */
bool
VXT_Setting(entity p, string key, string args)
{
	if (!VX_OptKnown(key))
		return false;
	if (studio_kind != 38 || !VX_CanEdit(p)) {
		VX_Tell(p, "Only the creator can change the world settings in Create mode.");
		return true;
	}
	args = strreplace(";", "", args);
	VX_OptSet(key, args);
	if (key == "gamemode")
		VX_SetText("mode", args);
	vx_dirty = true;
	VXT_SendEnv(world);
	printf("studio %.1f: %s set %s to %s\n", time, p ? p.netname : "console", key, args);
	return true;
}

/* --- commands -------------------------------------------------------------------- */

bool
VXT_Restricted(float wid)
{
	/* 0x21f + 200, 0x807 + 0x1bd, 0xfef + 0x33 */
	if (wid >= 543 && wid < 743)
		return true;
	if (wid >= 2055 && wid < 2500)
		return true;
	return (wid >= 4079 && wid < 4130);
}

bool
VXT_Command(entity p, string cmd)
{
	float argc = tokenize(cmd);
	string c = argv(0);

	if (substring(c, 0, 4) != "vxl_")
		return false;

	switch (c) {
	case "vxl_fire":
		/* DESIGN: the HUD's +attack / +attack2 while a tool is held */
		if (!p)
			return true;
		if (argv(1) == "1")
			p.vxt_b1 = stof(argv(2));
		else if (argv(1) == "2")
			p.vxt_b2 = stof(argv(2));
		return true;
	case "vxl_set":
		/* hw.dll FUN_021f0e60 */
		if (studio_kind != 38 || !p)
			return true;
		float t = VX_ArgType(argv(1));
		if (t <= 0 || !vx_placeable[VX_T(t)])
			return true;
		p.vx_select = t;
		p.vxt_copyRot = -1;
		if (p.vxt_copyProps)
			strunzone(p.vxt_copyProps);
		p.vxt_copyProps = "";
		userinfo.SetInteger(p, "vx_block", (int)t);
		printf("studio %.1f: %s picked %s\n", time, p.netname, voxel_typename(t));
		return true;
	case "vxl_wpn":
		/* mp.dll FUN_10856b50 -> FUN_10855810 */
		/* "vxl_wpn n id held" (DESIGN): the HUD reports a tool the hands
		   already switched to (mouse wheel), so only the slot is recorded */
		if (studio_kind != 38 || !p || argc < 3)
			return true;
		float slot = stof(argv(1)), wid = stof(argv(2));
		if (slot < 0 || slot > 4 || wid < 1 || wid > 4128)
			return true;
		if (VXT_Restricted(wid)) {
			VXT_Fail(p, CS_Text("CSO_Restrict_VoxelCreateMode", "Can't be used in creation mode."));
			return true;
		}
		if (VXT_Class(wid) == "") {
			printf("studio: vxl_wpn %g: no tool for that wid here\n", wid);
			return true;
		}
		if (p.vxt_slots == "")
			p.vxt_slots = strzone(VXT_DEFAULTS);
		bool held = (argv(3) == "held");
		if (held && slot == p.vxt_cur && VXT_Slot(p, slot) == wid)
			return true;
		VXT_SetSlot(p, slot, wid);
		if (held) {
			p.vxt_last = p.vxt_cur;
			p.vxt_cur = slot;
			p.vxt_mode = p.vxt_from = 0;
			userinfo.SetInteger(p, "vx_qscur", (int)p.vxt_cur);
		} else
			VXT_Draw(p, slot);
		printf("studio %.1f: %s slot %g -> %s\n", time, p.netname, slot + 1, VXT_Class(wid));
		return true;
	case "vxl_qsload":
		/* DESIGN: the client's saved slots, without drawing */
		if (!p || studio_kind != 38)
			return true;
		for (float i = 0; i < VXT_SLOTS; i++) {
			tokenize(cmd);
			float qid = stof(argv(i + 1));
			if (qid == 0 || (VXT_Class(qid) != "" && !VXT_Restricted(qid))) {
				if (p.vxt_slots == "")
					p.vxt_slots = strzone(VXT_DEFAULTS);
				VXT_SetSlot(p, i, qid);
			}
		}
		if (p.health > 0 && p.classname != "spectator")
			VXT_Equip(p);
		return true;
	case "vxl_lastinv":
		if (p && studio_kind == 38 && VXT_Slot(p, p.vxt_last) > 0)
			VXT_Draw(p, p.vxt_last);
		return true;
	}

	return VXT_Setting(p, substring(c, 4, -1), substring(cmd, strlen(c) + 1, -1));
}
