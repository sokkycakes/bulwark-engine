/* vx_core: Studio editing core. Block placement with multi-cell groups,
 * entity block records, limits, permissions, undo/redo, clipboard.
 * Original code (ISC).
 *
 * Evidence
 *   OBSERVED (voxelprop.json, voxel_list.csv via blocks.txt): block ids and
 *     names, per-block size in cells and the group list naming every cell
 *     of a multi-cell block (fillers start with "__"), block cost, entity
 *     type, SIGNAL column (0 none, 1 send, 2 receive, 3 both).
 *   OBSERVED (client strings): "CSO_Voxel_CantRotateVoxel" and
 *     "...SomethingOverlap": rotation is refused when the rotated block would
 *     overlap something. Tool names VxlEditor, VxlConnector, VxlMarker,
 *     VxlScanner.
 *   INFERRED: group cells are listed x fastest, then y, then z.
 *   DESIGN: the world size, block and cost limits, undo depth, the fill and
 *     clipboard caps, and that a group's minimum corner goes at the cell
 *     being pointed at. */

var float autocvar_studio_maxblocks = 40000;	/* DESIGN */
var float autocvar_studio_maxcost = 20000;	/* DESIGN */
var float autocvar_studio_openbuild = 0;	/* 1: everyone may build */
var float autocvar_studio_reach = 512;		/* DESIGN: how far a builder reaches */

#define VX_UNDO 64
#define VX_FILLMAX 32768
#define VX_CLIPMAX 4096
#define VX_OPEN_BASE 2048	/* runtime-only "open"/"lit" variants: id + 2048 */

.vector vb_cell;		/* record: minimum cell */
.float vb_type;
.float vb_rot;
.vector vb_dims;		/* rotated size in cells */
.string vb_props;		/* "key=value;key=value" */
.string vb_links;		/* receivers: "x y z|x y z" (anchor cells) */
.float vb_state;		/* signal output, door open, ... */
.float vb_inputs;		/* inputs currently on */
.float vb_timer;
.float vb_used;
.entity vb_user;

hashtable vx_records;		/* "x y z" of every covered cell -> record */
float vx_blocks;		/* cells in use */
float vx_costTotal;
bool vx_dirty;
float vx_lastChange;

/* per player */
.float vx_select;		/* selected block type */
.float vx_selRot;
.vector vx_mark1, vx_mark2;	/* Marker tool corners */
.float vx_marks;
.vector vx_linkFrom;		/* Connector tool: chosen sender */
.float vx_linking;
.float vx_undoPos, vx_undoTop;	/* records live in vx_undoTab, "<player>:<slot>" */
.string vx_clip;
.vector vx_clipSize;
.string vx_tool;
.float vx_allowed;

hashtable vx_undoTab;
string vx_owner;		/* the room's creator (first human), DESIGN */

/* --- helpers ------------------------------------------------------------- */

string
VX_Key(vector c)
{
	return sprintf("%d %d %d", c[0], c[1], c[2]);
}

vector
VX_Parse(string s)
{
	tokenize(s);
	return [stof(argv(0)), stof(argv(1)), stof(argv(2))];
}

/* long lists (undo records, the clipboard, map metadata) are walked with
   this instead of tokenize, which stops at 256 tokens */
float vx_iterPos;

bool
VX_More(string list)
{
	return vx_iterPos >= 0 && vx_iterPos < strlen(list);
}

string
VX_NextItem(string list, string sep)
{
	float e = strstrofs(list, sep, vx_iterPos);
	string item;
	if (e < 0) {
		item = substring(list, vx_iterPos, -1);
		vx_iterPos = -1;
	} else {
		item = substring(list, vx_iterPos, e - vx_iterPos);
		vx_iterPos = e + strlen(sep);
	}
	return item;
}

bool
VX_InBounds(vector c)
{
	vector sz = voxel_size();
	return c[0] >= 0 && c[1] >= 0 && c[2] >= 0 && c[0] < sz[0] && c[1] < sz[1] && c[2] < sz[2];
}

entity
VX_RecordAt(vector c)
{
	float n = hash_get(vx_records, VX_Key(c), 0, EV_FLOAT);
	if (n <= 0)
		return world;
	entity e = edict_num(n);
	if (wasfreed(e) || e.classname != "vxblock")
		return world;
	return e;
}

void
VX_ResetRecords(void)
{
	for (entity e = world; (e = find(e, ::classname, "vxblock"));)
		remove(e);
	if (vx_records)
		hash_destroytab(vx_records);
	vx_records = hash_createtab(4096, EV_FLOAT);
}

/* rotated size of a type */
vector
VX_Dims(float type, float rot)
{
	vector d = vx_dims[VX_T(type)];
	if (rot & 1)
		return [d[1], d[0], d[2]];
	return d;
}

/* cell of group index i for a block at base with rotation rot */
vector
VX_GroupCell(float type, float rot, vector base, float i)
{
	vector d = vx_dims[VX_T(type)];
	float lx = mod(i, d[0]);
	float ly = mod(floor(i / d[0]), d[1]);
	float lz = floor(i / (d[0] * d[1]));
	float rx = lx, ry = ly;
	switch ((int)rot & 3) {
	case 1: rx = d[1] - 1 - ly; ry = lx; break;
	case 2: rx = d[0] - 1 - lx; ry = d[1] - 1 - ly; break;
	case 3: rx = ly; ry = d[0] - 1 - lx; break;
	}
	return base + [rx, ry, lz];
}

/* the type stored in group cell i: the named group entry, else the block */
float
VX_GroupType(float type, float i)
{
	if (vx_group[VX_T(type)] == "")
		return type;
	int n = tokenizebyseparator(vx_group[VX_T(type)], ",");
	if (i >= n)
		return type;
	float t = voxel_typeforname(argv(i));
	return t < 0 ? type : t;
}

/* the group entry that is the block itself (the anchor) */
float
VX_AnchorIndex(float type)
{
	if (vx_group[VX_T(type)] == "")
		return 0;
	int n = tokenizebyseparator(vx_group[VX_T(type)], ",");
	string own = voxel_typename(type);
	for (int i = 0; i < n; i++)
		if (argv(i) == own)
			return i;
	return 0;
}

float
VX_GroupCount(float type)
{
	vector d = vx_dims[VX_T(type)];
	return d[0] * d[1] * d[2];
}

bool
VX_CanEdit(entity p)
{
	if (!p)
		return true;	/* the server console */
	if (autocvar_studio_openbuild || p.vx_allowed)
		return true;
	return p.netname == vx_owner;
}

void
VX_Publish(void)
{
	serverinfo.SetInteger("vx_blocks", (int)vx_blocks);
	serverinfo.SetInteger("vx_limit", (int)autocvar_studio_maxblocks);
	serverinfo.SetInteger("vx_cost", (int)vx_costTotal);
	serverinfo.SetInteger("vx_costlimit", (int)autocvar_studio_maxcost);
}

void
VX_Tell(entity p, string msg)
{
	if (p)
		sprint(p, PRINT_HIGH, strcat(msg, "\n"));
	else
		print(substring(msg, 0, 7) == "studio:" ? "" : "studio: ", msg, "\n");
}

/* --- low level cell writes (counted, recorded for undo) --------------------- */

string vx_opBuf;		/* undo record being built: "x y z ot or nt nr;" */
string vx_opRev;		/* the same, newest first: undo replays this one */
float vx_opCells;

void
VX_SetCell(vector c, float type, float rot)
{
	float old = voxel_get(c), oldRot = voxel_getrot(c);
	if (old == type && oldRot == rot)
		return;
	if (old > 0 && old < VX_OPEN_BASE) {
		vx_blocks--;
		vx_costTotal -= vx_cost[old];
	}
	if (type > 0 && type < VX_OPEN_BASE) {
		vx_blocks++;
		vx_costTotal += vx_cost[type];
	}
	voxel_set(c, type, rot);
	if (vx_opCells < 2048) {
		string item = sprintf("%d %d %d %g %g %g %g;", c[0], c[1], c[2], old, oldRot, type, rot);
		vx_opBuf = strcat(vx_opBuf, item);
		vx_opRev = strcat(item, vx_opRev);
	}
	vx_opCells++;
	vx_dirty = true;
	vx_lastChange = time;
}

void
VX_BeginOp(void)
{
	vx_opBuf = vx_opRev = "";
	vx_opCells = 0;
}

string
VX_UndoKey(entity p, float pos)
{
	return sprintf("%d:%d", num_for_edict(p), mod(pos, VX_UNDO));
}

void
VX_EndOp(entity p)
{
	if (!p || vx_opBuf == "")
		return;
	if (vx_opCells > 2048) {
		VX_Tell(p, "This change is too large to undo.");
		return;
	}
	if (!vx_undoTab)
		vx_undoTab = hash_createtab(256, EV_STRING);
	hash_add(vx_undoTab, VX_UndoKey(p, p.vx_undoPos), vx_opBuf, EV_STRING | HASH_REPLACE);
	hash_add(vx_undoTab, strcat(VX_UndoKey(p, p.vx_undoPos), "r"), vx_opRev, EV_STRING | HASH_REPLACE);
	p.vx_undoPos++;
	p.vx_undoTop = p.vx_undoPos;
}

/* --- records -------------------------------------------------------------- */

void
VX_IndexRecord(entity r, bool add)
{
	float n = VX_GroupCount(r.vb_type);
	for (float i = 0; i < n; i++) {
		string k = VX_Key(VX_GroupCell(r.vb_type, r.vb_rot, r.vb_cell, i));
		if (add)
			hash_add(vx_records, k, num_for_edict(r), EV_FLOAT | HASH_REPLACE);
		else
			hash_delete(vx_records, k);
	}
}

entity
VX_NewRecord(vector base, float type, float rot)
{
	entity r = spawn();
	r.classname = "vxblock";
	r.vb_cell = base;
	r.vb_type = type;
	r.vb_rot = rot;
	r.vb_dims = VX_Dims(type, rot);
	/* the block's centre, for distance checks and effects */
	setorigin(r, voxel_cellorigin(base) + r.vb_dims * VX_BLOCK * 0.5);
	VX_IndexRecord(r, true);
	return r;
}

void
VX_FreeRecord(entity r)
{
	VX_IndexRecord(r, false);
	if (r.vb_props)
		strunzone(r.vb_props);
	if (r.vb_links)
		strunzone(r.vb_links);
	remove(r);
}

/* a block needs a record if it spans several cells or has an entity type */
bool
VX_NeedsRecord(float type)
{
	type = VX_T(type);
	return VX_GroupCount(type) > 1 || (vx_entity[type] != "" && vx_entity[type] != "NONE");
}

string
VX_GetProp(entity r, string key, string def)
{
	int n = tokenizebyseparator(r.vb_props, ";");
	for (int i = 0; i < n; i++) {
		string kv = argv(i);
		float eq = strstrofs(kv, "=");
		if (eq > 0 && substring(kv, 0, eq) == key)
			return substring(kv, eq + 1, -1);
	}
	return def;
}

void
VX_SetProp(entity r, string key, string value)
{
	string out = "";
	int n = tokenizebyseparator(r.vb_props, ";");
	for (int i = 0; i < n; i++) {
		string kv = argv(i);
		float eq = strstrofs(kv, "=");
		if (eq <= 0 || substring(kv, 0, eq) == key)
			continue;
		out = strcat(out, kv, ";");
	}
	/* ';' and '=' would break the list; values keep everything else */
	value = strreplace(";", ",", strreplace("=", ":", value));
	if (value != "")
		out = strcat(out, key, "=", value, ";");
	if (r.vb_props)
		strunzone(r.vb_props);
	r.vb_props = strzone(out);
	vx_dirty = true;
}

/* --- placement --------------------------------------------------------------- */

/* why a block cannot go there, or "" */
string
VX_CheckPlace(float type, float rot, vector base, entity ignore)
{
	float n;

	if (type <= 0 || type >= VX_OPEN_BASE || voxel_typename(type) == "")
		return "Unknown block.";
	n = VX_GroupCount(type);
	if (vx_blocks + n > autocvar_studio_maxblocks)
		return "The block limit has been reached.";
	if (vx_costTotal + vx_cost[type] > autocvar_studio_maxcost)
		return "The cost limit has been reached.";

	for (float i = 0; i < n; i++) {
		vector c = VX_GroupCell(type, rot, base, i);
		if (!VX_InBounds(c))
			return "Out of the map.";
		if (voxel_get(c) != 0) {
			entity r = VX_RecordAt(c);
			if (!ignore || r != ignore)
				return "Something is in the way.";
		}
		/* a solid block may not trap a player */
		vector lo = voxel_cellorigin(c), hi = lo + [VX_BLOCK, VX_BLOCK, VX_BLOCK];
		for (entity p = world; (p = next.Player(p));) {
			if (p.health <= 0 || p.classname == "spectator")
				continue;
			if (p.absmin[0] < hi[0] && p.absmax[0] > lo[0] && p.absmin[1] < hi[1] &&
			    p.absmax[1] > lo[1] && p.absmin[2] < hi[2] && p.absmax[2] > lo[2])
				return "A player is in the way.";
		}
	}
	return "";
}

entity
VX_Place(entity p, float type, float rot, vector base, bool record)
{
	string why = VX_CheckPlace(type, rot, base, world);
	float n;
	entity r = world;

	if (why != "") {
		VX_Tell(p, why);
		return world;
	}
	if (record)
		VX_BeginOp();
	n = VX_GroupCount(type);
	for (float i = 0; i < n; i++)
		VX_SetCell(VX_GroupCell(type, rot, base, i), VX_GroupType(type, i), rot);
	if (VX_NeedsRecord(type))
		r = VX_NewRecord(base, type, rot);
	if (record)
		VX_EndOp(p);
	VX_Publish();
	return r;
}

/* removes the block covering the cell, whole group included */
bool
VX_Remove(entity p, vector c, bool record)
{
	entity r = VX_RecordAt(c);
	if (voxel_get(c) == 0)
		return false;
	if (record)
		VX_BeginOp();
	if (r) {
		float n = VX_GroupCount(r.vb_type);
		for (float i = 0; i < n; i++)
			VX_SetCell(VX_GroupCell(r.vb_type, r.vb_rot, r.vb_cell, i), 0, 0);
		/* links pointing at it are dropped with it */
		string key = VX_Key(r.vb_cell);
		for (entity o = world; (o = find(o, ::classname, "vxblock"));)
			if (o != r && strstrofs(strcat("|", o.vb_links, "|"), strcat("|", key, "|")) >= 0) {
				string kept = strreplace(strcat("|", key, "|"), "|", strcat("|", o.vb_links, "|"));
				kept = substring(kept, 1, max(0, strlen(kept) - 2));
				if (o.vb_links)
					strunzone(o.vb_links);
				o.vb_links = strzone(kept);
			}
		VX_FreeRecord(r);
	} else {
		VX_SetCell(c, 0, 0);
	}
	if (record)
		VX_EndOp(p);
	VX_Publish();
	return true;
}

/* rotates the block in place; refused if the rotated block overlaps (OBSERVED) */
bool
VX_Rotate(entity p, vector c)
{
	entity r = VX_RecordAt(c);
	float type, rot;
	vector base;

	if (voxel_get(c) == 0)
		return false;
	if (!r) {
		VX_BeginOp();
		VX_SetCell(c, voxel_get(c), mod(voxel_getrot(c) + 1, 4));
		VX_EndOp(p);
		return true;
	}
	type = r.vb_type;
	rot = mod(r.vb_rot + 1, 4);
	base = r.vb_cell;
	string why = VX_CheckPlace(type, rot, base, r);
	if (why != "") {
		VX_Tell(p, CS_Text("CSO_Voxel_CantRotateVoxelSomethingOverlap", "Cannot rotate: something overlaps."));
		return false;
	}
	string props = r.vb_props, links = r.vb_links;
	VX_BeginOp();
	float n = VX_GroupCount(type);
	for (float i = 0; i < n; i++)
		VX_SetCell(VX_GroupCell(type, r.vb_rot, base, i), 0, 0);
	VX_IndexRecord(r, false);
	for (float j = 0; j < n; j++)
		VX_SetCell(VX_GroupCell(type, rot, base, j), VX_GroupType(type, j), rot);
	r.vb_rot = rot;
	r.vb_dims = VX_Dims(type, rot);
	VX_IndexRecord(r, true);
	VX_EndOp(p);
	return true;
}

/* --- undo / redo ------------------------------------------------------------
   Group records are rebuilt from the cells: an undone multi-cell block is
   re-recorded at its anchor cell, entity properties are not restored. */

void VX_RebuildRecords(void);

void
VX_ApplyOp(string op, bool forward)
{
	/* the cells first, then stale records go, then anchors get records:
	   freeing a record per written cell would also drop records that an
	   earlier entry of the same op just brought back */
	for (vx_iterPos = 0; VX_More(op);) {
		if (tokenize(VX_NextItem(op, ";")) < 7)
			continue;
		vector c = [stof(argv(0)), stof(argv(1)), stof(argv(2))];
		VX_SetCell(c, forward ? stof(argv(5)) : stof(argv(3)), forward ? stof(argv(6)) : stof(argv(4)));
	}
	VX_RebuildRecords();
	for (vx_iterPos = 0; VX_More(op);) {
		if (tokenize(VX_NextItem(op, ";")) < 7)
			continue;
		vector a = [stof(argv(0)), stof(argv(1)), stof(argv(2))];
		float t = voxel_get(a), r = voxel_getrot(a);
		if (t <= 0 || t >= VX_OPEN_BASE || !VX_NeedsRecord(VX_T(t)) || VX_RecordAt(a))
			continue;
		vector base = a - VX_GroupCell(t, r, [0, 0, 0], VX_AnchorIndex(t));
		VX_NewRecord(base, t, r);
	}
}


bool
VX_Undo(entity p)
{
	if (p.vx_undoPos <= 0 || p.vx_undoPos <= p.vx_undoTop - VX_UNDO)
		return false;
	p.vx_undoPos--;
	VX_BeginOp();
	VX_ApplyOp(hash_get(vx_undoTab, strcat(VX_UndoKey(p, p.vx_undoPos), "r"), "", EV_STRING), false);
	VX_RebuildRecords();
	VX_Publish();
	return true;
}

bool
VX_Redo(entity p)
{
	if (p.vx_undoPos >= p.vx_undoTop)
		return false;
	VX_BeginOp();
	VX_ApplyOp(hash_get(vx_undoTab, VX_UndoKey(p, p.vx_undoPos), "", EV_STRING), true);
	p.vx_undoPos++;
	VX_RebuildRecords();
	VX_Publish();
	return true;
}

/* --- area tools (Marker) ----------------------------------------------------- */

void
VX_SortMarks(entity p, __out vector lo, __out vector hi)
{
	lo = [min(p.vx_mark1[0], p.vx_mark2[0]), min(p.vx_mark1[1], p.vx_mark2[1]), min(p.vx_mark1[2], p.vx_mark2[2])];
	hi = [max(p.vx_mark1[0], p.vx_mark2[0]), max(p.vx_mark1[1], p.vx_mark2[1]), max(p.vx_mark1[2], p.vx_mark2[2])];
}

float
VX_Fill(entity p, float type, float rot)
{
	vector lo, hi;
	float placed = 0;

	if (p.vx_marks < 2) {
		VX_Tell(p, "Mark two corners first.");
		return 0;
	}
	VX_SortMarks(p, lo, hi);
	vector d = hi - lo + [1, 1, 1];
	if (d[0] * d[1] * d[2] > VX_FILLMAX) {
		VX_Tell(p, "That area is too large.");
		return 0;
	}
	if (type > 0 && VX_GroupCount(type) > 1) {
		VX_Tell(p, "Only single-cell blocks can fill an area.");
		return 0;
	}
	VX_BeginOp();
	for (float z = lo[2]; z <= hi[2]; z++)
		for (float y = lo[1]; y <= hi[1]; y++)
			for (float x = lo[0]; x <= hi[0]; x++) {
				vector c = [x, y, z];
				if (type == 0) {
					if (voxel_get(c)) {
						entity r = VX_RecordAt(c);
						if (r)
							VX_Remove(world, c, false);
						else
							VX_SetCell(c, 0, 0);
						placed++;
					}
					continue;
				}
				if (voxel_get(c) || vx_blocks >= autocvar_studio_maxblocks ||
				    vx_costTotal + vx_cost[type] > autocvar_studio_maxcost)
					continue;
				VX_SetCell(c, type, rot);
				if (VX_NeedsRecord(type))
					VX_NewRecord(c, type, rot);
				placed++;
			}
	VX_EndOp(p);
	VX_Publish();
	return placed;
}

float
VX_Copy(entity p)
{
	vector lo, hi;
	string buf = "";
	float n = 0;

	if (p.vx_marks < 2) {
		VX_Tell(p, "Mark two corners first.");
		return 0;
	}
	VX_SortMarks(p, lo, hi);
	for (float z = lo[2]; z <= hi[2]; z++)
		for (float y = lo[1]; y <= hi[1]; y++)
			for (float x = lo[0]; x <= hi[0]; x++) {
				vector c = [x, y, z];
				float t = voxel_get(c);
				if (!t || t >= VX_OPEN_BASE)
					continue;
				/* multi-cell blocks copy from their anchor only */
				entity r = VX_RecordAt(c);
				if (r && r.vb_cell != c)
					continue;
				if (++n > VX_CLIPMAX) {
					VX_Tell(p, "Too many blocks to copy.");
					return 0;
				}
				buf = strcat(buf, sprintf("%d %d %d %g %g;", x - lo[0], y - lo[1], z - lo[2], r ? r.vb_type : t, r ? r.vb_rot : voxel_getrot(c)));
			}
	if (p.vx_clip)
		strunzone(p.vx_clip);
	p.vx_clip = strzone(buf);
	p.vx_clipSize = hi - lo + [1, 1, 1];
	return n;
}

float
VX_Paste(entity p, vector at)
{
	float placed = 0;
	if (!p.vx_clip || p.vx_clip == "")
		return 0;
	VX_BeginOp();
	for (vx_iterPos = 0; VX_More(p.vx_clip);) {
		if (tokenize(VX_NextItem(p.vx_clip, ";")) < 5)
			continue;
		vector c = at + [stof(argv(0)), stof(argv(1)), stof(argv(2))];
		float t = stof(argv(3)), r = stof(argv(4));
		if (VX_CheckPlace(t, r, c, world) != "")
			continue;
		float gn = VX_GroupCount(t);
		for (float k = 0; k < gn; k++)
			VX_SetCell(VX_GroupCell(t, r, c, k), VX_GroupType(t, k), r);
		if (VX_NeedsRecord(t))
			VX_NewRecord(c, t, r);
		placed++;
	}
	VX_EndOp(p);
	VX_Publish();
	return placed;
}

/* --- counting and records after a load ---------------------------------------- */

/* counts blocks and cost from the world and re-creates the group records of
   anchors that have none (after undo or a load without metadata) */
void
VX_Recount(void)
{
	vector sz = voxel_size();
	vx_blocks = voxel_count();
	vx_costTotal = 0;
	/* cost: walk only when the world is small enough to be quick */
	if (vx_blocks < 200000)
		for (float z = 0; z < sz[2]; z++)
			for (float y = 0; y < sz[1]; y++)
				for (float x = 0; x < sz[0]; x++) {
					float t = voxel_get([x, y, z]);
					if (t > 0 && t < VX_OPEN_BASE)
						vx_costTotal += vx_cost[t];
				}
	VX_Publish();
}

void
VX_RebuildRecords(void)
{
	/* a record whose anchor cell no longer holds its block is stale */
	for (entity r = world; (r = find(r, ::classname, "vxblock"));) {
		vector anchor = VX_GroupCell(r.vb_type, r.vb_rot, r.vb_cell, VX_AnchorIndex(r.vb_type));
		if (voxel_get(anchor) != r.vb_type)
			VX_FreeRecord(r);
	}
}
