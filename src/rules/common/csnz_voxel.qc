/* csnz_voxel: the Studio block catalogue and voxel world boot, shared by
 * csnz.dat (any mode can be played on a Studio map) and studio.dat.
 *
 * The catalogue is data/studio/blocks.txt, generated on the user's machine
 * by tools/studio/make_blocks.py from the client's voxelprop.json and
 * voxel_list.csv (OBSERVED ids, names, costs, sizes, entity types). It is
 * never committed. Without it a small built-in colour palette is used
 * (DESIGN), so the code paths still run.
 *
 * Block size: 40 units for new worlds. SOURCE-CONFIRMED: mp.dll and
 * hw.dll put a cell's centre at cell * 40 + origin + 20 (FUN_11257d50,
 * FUN_112555a0). This replaces the earlier 32 (INFERRED from voxelprop.json
 * use_offset values). A saved world keeps the block size it was saved with,
 * so everything else reads the live world's size. */

#define VX_NEWBLOCK 40
#define VX_BLOCK voxel_blocksize()
#define VX_CATALOG "data/studio/blocks.txt"
#define VX_MAXTYPE 2048		/* CSO ids end at 2004; runtime variants use 2048+ */

/* per type, from the catalogue */
float vx_cost[VX_MAXTYPE];
float vx_category[VX_MAXTYPE];
string vx_entity[VX_MAXTYPE];		/* "NONE" or the CSO entity type */
vector vx_dims[VX_MAXTYPE];		/* multi-cell size in cells */
string vx_group[VX_MAXTYPE];		/* filler names of a multi-cell block */
float vx_signal[VX_MAXTYPE];		/* 0 none, 1 send, 2 receive, 3 both */
string vx_display[VX_MAXTYPE];
string vx_model[VX_MAXTYPE];
hashtable vx_switchLight;	/* LIGHT_ONOFF type id -> its light "r g b" when on */
bool vx_placeable[VX_MAXTYPE];		/* shown in the block browser */
float vx_exit[VX_MAXTYPE];		/* 0 playing, 1 round clear, 2 round fail */
float vx_hp[VX_MAXTYPE];		/* breakable blocks */
float vx_dmgTo[VX_MAXTYPE];		/* 0 none, 1 all, 2 human, 3 zombie */
float vx_dmgPct[VX_MAXTYPE];
float vx_typeCount;
float vx_firstType;			/* the default block for new maps */
bool vx_fromCatalog;

/* a type id safe for indexing the arrays above: anything outside the
   catalogue reads as type 0 (QC evaluates both sides of && and ||) */
float
VX_T(float t)
{
	return (t > 0 && t < VX_MAXTYPE) ? t : 0;
}

void
VX_DefineBuiltin(void)
{
	/* DESIGN: a fallback palette so maps can be built without the CSO data */
	string names = "COLORBOX_GRAY_01 COLORBOX_GRAY_05 COLORBOX_RED_01 COLORBOX_GREEN_01 COLORBOX_BLUE_01 COLORBOX_YELLOW_01 COLORBOX_WHITE_01 COLORBOX_BLACK_01";
	vector cols[8] = {[0.53, 0.53, 0.53], [0.33, 0.33, 0.33], [0.8, 0.2, 0.2], [0.2, 0.7, 0.3],
		[0.2, 0.35, 0.85], [0.9, 0.8, 0.2], [0.95, 0.95, 0.95], [0.1, 0.1, 0.1]};
	int n = tokenize(names);

	for (int i = 0; i < n; i++) {
		string nm = argv(i);
		voxel_define(i + 1, nm, VXS_BOX, VXC_SOLID, cols[i], 1, "", 0);
		vx_cost[i + 1] = 1;
		vx_entity[i + 1] = "NONE";
		vx_dims[i + 1] = [1, 1, 1];
		vx_placeable[i + 1] = true;
	}
	voxel_define(9, "WATER_01", VXS_FIELD, VXC_WATER, [0.2, 0.4, 0.9], 0.45, "", 0);
	vx_placeable[9] = true;
	vx_dims[9] = [1, 1, 1];
	vx_entity[9] = "NONE";
	voxel_define(10, "LADDER_01", VXS_THIN, VXC_LADDER | VXC_SOLID, [0.6, 0.45, 0.25], 1, "", 0);
	vx_placeable[10] = true;
	vx_dims[10] = [1, 1, 1];
	vx_entity[10] = "NONE";
	vx_typeCount = 10;
	vx_firstType = 1;
	vx_fromCatalog = false;
}

/* defines every catalogue type in the current voxel world */
void
VX_LoadCatalog(void)
{
	filestream f = fopen(VX_CATALOG, FILE_READ);
	string s;
	float n = 0;

	for (int i = 0; i < VX_MAXTYPE; i++) {
		vx_placeable[i] = false;
		vx_dims[i] = [1, 1, 1];
	}

	if (f < 0) {
		print("csnz: " VX_CATALOG " not found (run tools/studio/make_blocks.py); using the built-in palette\n");
		VX_DefineBuiltin();
		return;
	}

	vx_firstType = 0;
	if (!vx_switchLight)
		vx_switchLight = hash_createtab(64, HASH_REPLACE);
	while (1) {
		s = fgets(f);
		if not (s)	/* end of file; "" is a blank line */
			break;
		if (substring(s, 0, 2) == "//")
			continue;
		if (tokenizebyseparator(s, "\t") < 18)
			continue;

		float id = stof(argv(0));
		if (id <= 0 || id >= VX_MAXTYPE)
			continue;
		string nm = argv(1);
		float shape = stof(argv(2));
		float contents = stof(argv(3));
		string rgba = argv(4);
		float flags = stof(argv(5));
		string tex = argv(6);
		float cost = stof(argv(7));
		float cat = stof(argv(8));
		string ent = argv(9);
		string dims = argv(10);
		string grp = argv(11);
		float sig = stof(argv(12));
		string disp = argv(14);
		string mdl = argv(15);
		string exitType = argv(18), hp = argv(19), dmg = argv(20);

		tokenize(rgba);
		vector rgb = [stof(argv(0)) / 255, stof(argv(1)) / 255, stof(argv(2)) / 255];
		float alpha = stof(argv(3)) / 255;
		if (tex == "-")
			tex = "";

		if (!voxel_define(id, nm, shape, contents, rgb, alpha, tex, flags))
			continue;

		vx_cost[id] = cost;
		vx_category[id] = cat;
		vx_entity[id] = strzone(ent);
		tokenize(dims);
		vx_dims[id] = [max(1, stof(argv(0))), max(1, stof(argv(1))), max(1, stof(argv(2)))];
		vx_group[id] = grp == "-" ? "" : strzone(grp);
		vx_signal[id] = sig;
		vx_display[id] = strzone(disp);
		vx_model[id] = mdl == "-" ? "" : strzone(mdl);
		vx_exit[id] = exitType == "ROUND_CLEAR" ? 1 : (exitType == "ROUND_FAIL" ? 2 : 0);
		vx_hp[id] = stof(hp);
		tokenize(dmg);
		vx_dmgTo[id] = argv(0) == "TO_ALL" ? 1 : (argv(0) == "TO_HUMAN" ? 2 : (argv(0) == "TO_ZOMBIE" ? 3 : 0));
		vx_dmgPct[id] = stof(argv(1));
		/* light (hw.dll voxel light, see docs/spec/voxel.md "Light"):
		   a light switch block gives none until it is switched on, when
		   its LIT_ variant (vx_entities.qc) takes the light */
		tokenizebyseparator(s, "\t");
		if (argv(21) != "" && checkbuiltin(voxel_setlight)) {
			float lt = stof(argv(21));
			tokenize(argv(22));
			vector lrgb = [stof(argv(0)), stof(argv(1)), stof(argv(2))];
			if (ent == "LIGHT_ONOFF" && lrgb != '0 0 0') {
				hash_add(vx_switchLight, ftos(id), vtos(lrgb), EV_STRING);
				lrgb = '0 0 0';
			}
			voxel_setlight(id, lt, lrgb);
		}
		/* names starting with "__" are internal (fillers, fakes) */
		vx_placeable[id] = substring(nm, 0, 2) != "__" && cat >= 0;
		if (!vx_firstType && vx_placeable[id] && shape == VXS_BOX)
			vx_firstType = id;
		n++;
	}
	fclose(f);
	vx_typeCount = n;
	vx_fromCatalog = true;
	printf("csnz: %g Studio block types loaded\n", n);
}

/* map markers: an info_target named "voxel:<file>" loads that world, one
   named "voxelfloor:<x> <y>" makes a flat floor of that many cells */
string
VX_MapMarker(string prefix)
{
	for (entity e = world; (e = find(e, ::classname, "info_target"));) {
		if (substring(e.targetname, 0, strlen(prefix)) == prefix)
			return substring(e.targetname, strlen(prefix), -1);
	}
	return "";
}

/* builds a world for this map: a saved Studio file, else a floor, else none */
bool
VX_Boot(vector cells)
{
	string file = VX_MapMarker("voxel:");
	string floorSpec = VX_MapMarker("voxelfloor:");
	string autoFile = sprintf("studio/maps/%s.bwvx", game.GetMap());

	if (file == "" && floorSpec == "" && !fexists(strcat("data/", autoFile)))
		return false;

	if (!voxel_create(cells, VX_NEWBLOCK, [-cells[0] * VX_NEWBLOCK / 2, -cells[1] * VX_NEWBLOCK / 2, -64])) {
		print("csnz: voxel world could not be created\n");
		return false;
	}
	VX_LoadCatalog();

	if (file == "" && fexists(strcat("data/", autoFile)))
		file = autoFile;

	if (file != "") {
		if (voxel_load(file)) {
			printf("csnz: voxel world %s loaded (%g blocks, %g unknown)\n", file, voxel_count(), voxel_unknown());
			return true;
		}
		printf("csnz: voxel world %s failed: %s\n", file, voxel_error());
	}

	float fx = cells[0], fy = cells[1];
	if (floorSpec != "") {
		tokenize(floorSpec);
		fx = bound(1, stof(argv(0)), cells[0]);
		fy = bound(1, stof(argv(1)), cells[1]);
	}
	vector lo = [floor(cells[0] / 2 - fx / 2), floor(cells[1] / 2 - fy / 2), 0];
	voxel_fill(lo, lo + [fx - 1, fy - 1, 0], vx_firstType, 0);
	printf("csnz: flat voxel floor %gx%g of %s\n", fx, fy, voxel_typename(vx_firstType));
	return true;
}
