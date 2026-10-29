/* Studio world effects drawn by the HUD: the target cell of the build
   tools and the map's weather.

   Ghost (DESIGN; no CSO evidence was found for how its build tools mark
   the cell, so this is ours): while a build gun is in the hands in mode
   38, the cell it would fill (Create mode) is outlined in white and the
   block it would remove (Remove mode) in red. The aim follows the rules'
   VX_AimReach (studio_main.qc): a trace from the eye, the cell hit, or the
   cell in front of the face hit; the short gun also builds in the air at
   its reach. The rules publish the mode as userinfo vx_mode (vx_tools.qc).

   Weather (SOURCE-CONFIRMED setting, DESIGN look): CSO's World settings
   Weather is None, Rain or Snow (client.dll FUN_02b09a80, "vxl_weather",
   hw.dll world settings +0x35). The rules publish it as serverinfo
   vxo_weather; here particles from particles/vxweather.cfg are spawned in
   the open air around the view: each column is traced down so nothing
   starts under a roof, and each particle dies before it falls further
   than its own drop. */

/* the engine's voxel builtins (src/shared/voxel_api.h); the client
   world answers them in the HUD as well */
vector voxel_cellforpoint(vector point) = #0:voxel_cellforpoint;
vector voxel_cellorigin(vector cell) = #0:voxel_cellorigin;
float voxel_blocksize(void) = #0:voxel_blocksize;
vector voxel_tracecell(void) = #0:voxel_tracecell;

float g_vxwAcc;

void
VXW_Edge(vector a, vector b, vector rgb)
{
	vector sa = project(a), sb = project(b);
	if (sa[2] <= 1 || sb[2] <= 1)
		return;
	drawline(2, [sa[0], sa[1]], [sb[0], sb[1]], rgb, 0.9f, 0);
}

void
VXW_Box(vector mins, vector maxs, vector rgb)
{
	vector c[8];
	for (int i = 0i; i < 8i; i++)
		c[i] = [(i & 1i) ? maxs[0] : mins[0], (i & 2i) ? maxs[1] : mins[1], (i & 4i) ? maxs[2] : mins[2]];
	VXW_Edge(c[0], c[1], rgb); VXW_Edge(c[2], c[3], rgb); VXW_Edge(c[4], c[5], rgb); VXW_Edge(c[6], c[7], rgb);
	VXW_Edge(c[0], c[2], rgb); VXW_Edge(c[1], c[3], rgb); VXW_Edge(c[4], c[6], rgb); VXW_Edge(c[5], c[7], rgb);
	VXW_Edge(c[0], c[4], rgb); VXW_Edge(c[1], c[5], rgb); VXW_Edge(c[2], c[6], rgb); VXW_Edge(c[3], c[7], rgb);
}

void
VXW_DrawGhost(void)
{
	float wid = g_vqHeld;
	if (HUD_ModeId() != 38 || HUD_WantsCursor() || player.GetHealth() <= 0)
		return;
	if (wid != 300 && wid != 335 && wid != 363 && wid != 485)
		return;
	/* the Build Marker places at 100 units, in the air too (vx_tools.qc) */
	bool marker = (wid == 485);
	float bs = voxel_blocksize();
	if (bs <= 0)
		return;

	bool remove = !marker && stof(getplayerkeyvalue(player_localnum, "vx_mode")) != 0;
	float reach = (wid == 335 || marker) ? 100 : 2000;
	vector src = player.GetCameraPosition();
	makevectors(player.GetCameraAngles());
	traceline(src, src + v_forward * reach, MOVE_NOMONSTERS, world);
	vector hit = voxel_tracecell();
	vector cell;

	if (hit[0] >= 0 && trace_fraction < 1)
		cell = remove ? hit : voxel_cellforpoint(trace_endpos + trace_plane_normal * (bs * 0.5));
	else if (!remove && (wid == 335 || marker))
		cell = voxel_cellforpoint(src + v_forward * reach);
	else
		return;
	if (cell[0] < 0)
		return;

	vector o = voxel_cellorigin(cell);
	vector rgb = remove ? [1, 0.3, 0.25] : [1, 1, 1];
	if (marker)
		rgb = [0.3, 1, 0.4];
	/* the rules refuse a cell that would trap a player (VX_CanPlace,
	   vx_core.qc); the HUD only knows the eye, so our own hull is taken as
	   16 units around it, from 64 below (30 ducked) to 10 above, and such
	   a cell is outlined in grey */
	if (!remove && !marker) {
		vector lo = src - [16, 16, player.IsCrouched() ? 30 : 64], hi = src + '16 16 10';
		if (lo[0] < o[0] + bs && hi[0] > o[0] && lo[1] < o[1] + bs && hi[1] > o[1] && lo[2] < o[2] + bs && hi[2] > o[2])
			rgb = [0.45, 0.45, 0.45];
	}
	/* a hair outside the cell so the lines sit on top of its faces */
	VXW_Box(o - '0.3 0.3 0.3', o + [bs, bs, bs] + '0.3 0.3 0.3', rgb);
}

/* the Build Marker's markers and area (DESIGN look: CSO's drawing of
   them is not traced), from the rules' userinfo vx_marks */
void
VXW_DrawMarks(void)
{
	if (HUD_ModeId() != 38)
		return;
	int n = (int)tokenize(getplayerkeyvalue(player_localnum, "vx_marks"));
	if (n < 3)
		return;
	float bs = voxel_blocksize();
	if (bs <= 0)
		return;
	vector a = [stof(argv(0)), stof(argv(1)), stof(argv(2))];
	vector oa = voxel_cellorigin(a);
	VXW_Box(oa - '0.6 0.6 0.6', oa + [bs, bs, bs] + '0.6 0.6 0.6', [0.3, 1, 0.4]);
	if (n < 6)
		return;
	vector b = [stof(argv(3)), stof(argv(4)), stof(argv(5))];
	vector lo = [min(a[0], b[0]), min(a[1], b[1]), min(a[2], b[2])];
	vector hi = [max(a[0], b[0]), max(a[1], b[1]), max(a[2], b[2])];
	vector ob = voxel_cellorigin(b);
	VXW_Box(ob - '0.6 0.6 0.6', ob + [bs, bs, bs] + '0.6 0.6 0.6', [1, 0.6, 0.2]);
	VXW_Box(voxel_cellorigin(lo) - '1 1 1', voxel_cellorigin(hi) + [bs, bs, bs] + '1 1 1', [0.35, 0.8, 1]);
}

void
VXW_DrawWeather(void)
{
	float w = stof(serverkey("vxo_weather"));
	float mode = HUD_ModeId();
	if ((mode != 38 && mode != 39) || (w != 1 && w != 2))
		return;

	float eff = particleeffectnum(w == 1 ? "vxweather.rain" : "vxweather.snow");
	/* how far one particle falls in its life (vxweather.cfg) */
	float drop = (w == 1) ? 165 : 110;
	float rate = (w == 1) ? 700 : 220;
	vector cam = player.GetCameraPosition();

	g_vxwAcc += rate * clframetime;
	if (g_vxwAcc > 200)
		g_vxwAcc = 200;
	while (g_vxwAcc >= 1) {
		g_vxwAcc -= 1;
		float ang = random() * 6.2832, r = sqrt(random()) * 800;
		vector top = cam + [cos(ang) * r, sin(ang) * r, 500];
		traceline(top, top - '0 0 1400', MOVE_NOMONSTERS, world);
		if (trace_startsolid)
			continue;
		float low = trace_endpos[2] + drop;
		if (low >= top[2])
			continue;
		pointparticles(eff, [top[0], top[1], low + random() * (top[2] - low)], '0 0 0', 1);
	}
}

void
VXW_Draw(void)
{
	VXW_DrawMarks();
	VXW_DrawGhost();
	VXW_DrawWeather();
}
