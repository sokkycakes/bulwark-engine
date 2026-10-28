/* zombi_nav: builds a waypoint graph for bots by walking the map.
 *
 * DESIGN, test tooling. Bots route over data/<map>.way. Nuclide only makes
 * unlinked nodes from spawn points, which gives bots nowhere to go, so this
 * flood-fills from every spawn point on a grid: a step is kept when a
 * player-sized box can move to the next cell (with a stair step) and finds
 * floor under it. Set zb_buildnav 1 on a running map, then restart the map
 * so Nuclide reads the new file.
 */

#define ZBN_MAX 4096
#define ZBN_LINKS 8
#define ZBN_GRID 64
#define ZBN_STEP 18
#define ZBN_DROP 512

vector zbn_org[ZBN_MAX];
int zbn_link[ZBN_MAX * ZBN_LINKS];
int zbn_linkCount[ZBN_MAX];
int zbn_count;
hashtable zbn_cells;

string
ZBN_Key(vector p)
{
	return sprintf("%d %d %d", rint(p[0] / ZBN_GRID), rint(p[1] / ZBN_GRID), rint(p[2] / 32));
}

int
ZBN_Add(vector p)
{
	string key = ZBN_Key(p);
	float idx = hash_get(zbn_cells, key, -1, EV_FLOAT);

	if (idx >= 0)
		return ((int)idx);

	if (zbn_count >= ZBN_MAX)
		return (-1);

	zbn_org[zbn_count] = p;
	zbn_linkCount[zbn_count] = 0;
	hash_add(zbn_cells, key, (float)zbn_count, EV_FLOAT);
	zbn_count++;
	return (zbn_count - 1);
}

void
ZBN_Link(int a, int b)
{
	if (a == b || zbn_linkCount[a] >= ZBN_LINKS)
		return;

	for (int i = 0; i < zbn_linkCount[a]; i++)
		if (zbn_link[a * ZBN_LINKS + i] == b)
			return;

	zbn_link[a * ZBN_LINKS + zbn_linkCount[a]] = b;
	zbn_linkCount[a]++;
}

/* where a player standing at from ends up after walking toward to, or
   '0 0 0' if the way is blocked or there is no floor. Walks in short
   moves so a staircase counts as a run of single steps. */
vector
ZBN_Walk(vector from, vector to, vector hullMin, vector hullMax)
{
	vector up = [0, 0, ZBN_STEP];
	vector at = from;
	vector stride = (to - from) / 4;
	float startZ = from[2];

	for (int i = 0; i < 4; i++) {
		vector next = at + stride;

		tracebox(at + up, hullMin, hullMax, next + up, MOVE_NOMONSTERS, world);
		if (trace_fraction < 1.0 || trace_startsolid)
			return ([0, 0, 0]);

		tracebox(next + up, hullMin, hullMax, next - [0, 0, ZBN_DROP], MOVE_NOMONSTERS, world);
		if (trace_fraction == 1.0 || trace_startsolid || trace_plane_normal[2] < 0.7)
			return ([0, 0, 0]);

		at = trace_endpos;
	}

	/* no water or slime cells */
	if (pointcontents(at) != CONTENT_EMPTY)
		return ([0, 0, 0]);

	return (at);
}

void
ZBN_Build(void)
{
	vector hullMin = [-16, -16, -36];
	vector hullMax = [16, 16, 36];
	entity pl = next.Player(world);
	int head = 0;
	int links = 0;
	vector dirs[8] = {[1,0,0], [-1,0,0], [0,1,0], [0,-1,0], [1,1,0], [1,-1,0], [-1,1,0], [-1,-1,0]};

	if (pl) {
		hullMin = pl.mins;
		hullMax = pl.maxs;
	}

	zbn_count = 0;
	zbn_cells = hash_createtab(ZBN_MAX, EV_FLOAT);

	/* seeds: every spawn point, dropped to the floor */
	for (entity s = world; (s = find(s, ::classname, "info_player_deathmatch"));) {
		tracebox(s.origin + [0, 0, 8], hullMin, hullMax, s.origin - [0, 0, ZBN_DROP], MOVE_NOMONSTERS, world);
		if (!trace_startsolid && trace_fraction < 1.0)
			ZBN_Add(trace_endpos);
	}

	/* breadth-first over the grid */
	while (head < zbn_count) {
		vector at = zbn_org[head];

		for (int d = 0; d < 8; d++) {
			vector to = at + dirs[d] * ZBN_GRID;
			vector land = ZBN_Walk(at, to, hullMin, hullMax);
			int n;

			if (land == [0, 0, 0])
				continue;

			n = ZBN_Add(land);
			if (n >= 0)
				ZBN_Link(head, n);
		}
		head++;
	}

	/* save in Nuclide's .way layout */
	filestream f = fopen(sprintf("data/%s.way", mapname), FILE_WRITE);
	if (f < 0) {
		printf("zombi: cannot write data/%s.way\n", mapname);
		return;
	}

	fputs(f, sprintf("%i\n", zbn_count));
	for (int i = 0; i < zbn_count; i++) {
		fputs(f, sprintf("%v %f %i\n", zbn_org[i], ZBN_GRID / 2, zbn_linkCount[i]));
		for (int k = 0; k < zbn_linkCount[i]; k++) {
			int to = zbn_link[i * ZBN_LINKS + k];
			fputs(f, sprintf(" %i %f %i\n", to, vlen(zbn_org[to] - zbn_org[i]), 0));
			links++;
		}
	}
	fclose(f);
	hash_destroytab(zbn_cells);
	printf("zombi: wrote data/%s.way, %i nodes, %i links (hull %v %v)\n", mapname, zbn_count, links, hullMin, hullMax);
}
