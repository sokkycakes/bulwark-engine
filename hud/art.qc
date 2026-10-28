/* HUD art: the build's resource/hud images and sprites/hud.txt sprites.

   resource/hud/hud_* are the HUD images client.dll names (OBSERVED strings
   "resource/Hud/hud_character_bg_new_top" etc. in client.dll). Digit strips
   hold 0-9 left to right at a fixed pitch, measured from the images'
   alpha (OBSERVED): hud_character_num 7 px, hud_sb_num_big_* 18 px,
   hud_sb_num_center 11 px, hud_sb_num_bottom / small_* 8 px.

   The HUD is laid out in 1024x768 and scaled with the screen height
   (INFERRED, like the menus). */

#define HUD_W 1024
#define HUD_H 768

font_s g_hudFont;
float g_hs;		/* layout scale */
vector g_hmins;		/* layout origin on screen */

void
Art_Frame(void)
{
	vector size = screen.HUDSize();
	g_hs = size[1] / HUD_H;
	g_hmins = screen.HUDMins();
}

/* layout position -> screen; x from the left, or from the right if negative */
vector
Art_Pos(vector p)
{
	vector size = screen.HUDSize();

	if (p[0] < 0)
		p[0] = size[0] / g_hs + p[0];
	return g_hmins + p * g_hs;
}

void
Art_Pic(vector p, string pic, float alpha)
{
	drawpic(Art_Pos(p), pic, drawgetimagesize(pic) * g_hs, [1,1,1], alpha, 0);
}

/* the pic mirrored left-right */
void
Art_PicFlip(vector p, string pic, float alpha)
{
	vector sz = drawgetimagesize(pic);
	drawsubpic(Art_Pos(p), sz * g_hs, pic, [1, 0], [-1, 1], [1,1,1], alpha, 0);
}

/* --- digit strips ------------------------------------------------------------ */

#define NUM_CHAR 0
#define NUM_BIGWHITE 1
#define NUM_BIGRED 2
#define NUM_BIGBLUE 3
#define NUM_CENTER 4
#define NUM_BOTTOM 5

string g_numPic[6];
float g_numPitch[6];
float g_numStripW[6];
float g_numH[6];

void
Art_Strip(int ns, string pic, float pitch)
{
	vector sz;

	precache_pic(pic);
	sz = drawgetimagesize(pic);
	g_numPic[ns] = pic;
	g_numPitch[ns] = pitch;
	g_numStripW[ns] = sz[0];
	g_numH[ns] = sz[1];
}

float
Art_NumWidth(int ns, string digits)
{
	return strlen(digits) * g_numPitch[ns];
}

/* digits only; anything else is skipped. Returns the layout x after the last digit. */
float
Art_Num(vector p, int ns, string digits, vector rgb, float alpha)
{
	int n = strlen(digits);
	float pitch = g_numPitch[ns];

	if (g_numStripW[ns] <= 0)
		return p[0];

	for (int i = 0i; i < n; i++) {
		float d = str2chr(digits, i) - '0';

		if (d < 0 || d > 9)
			continue;

		drawsubpic(Art_Pos(p), [pitch, g_numH[ns]] * g_hs, g_numPic[ns],
			[d * pitch / g_numStripW[ns], 0], [pitch / g_numStripW[ns], 1], rgb, alpha, 0);
		p[0] += pitch;
	}

	return p[0];
}

/* centred on x */
void
Art_NumC(vector p, int ns, string digits, vector rgb, float alpha)
{
	p[0] -= Art_NumWidth(ns, digits) / 2;
	Art_Num(p, ns, digits, rgb, alpha);
}

/* right-aligned to x */
void
Art_NumR(vector p, int ns, string digits, vector rgb, float alpha)
{
	p[0] -= Art_NumWidth(ns, digits);
	Art_Num(p, ns, digits, rgb, alpha);
}

/* --- sprites/hud.txt -------------------------------------------------------- */

/* the 640 set: name -> sprite file and rectangle (OBSERVED table) */
hashtable g_hudSprites;

void
Art_LoadHudTxt(void)
{
	filestream fh = fopen("sprites/hud.txt", FILE_READ);
	string line;

	g_hudSprites = hash_createtab(1024, HASH_REPLACE);

	if (fh < 0) {
		print("^1hud: sprites/hud.txt is missing\n");
		return;
	}

	while (1) {
		line = fgets(fh);

		/* a blank line is "" too, which is false: test for null only */
		if not (line)
			break;
		if (tokenize_console(line) < 7)
			continue;
		if (argv(1) != "640")
			continue;

		hash_add(g_hudSprites, strtolower(argv(0)),
			sprintf("sprites/%s.spr %s %s %s %s", argv(2), argv(3), argv(4), argv(5), argv(6)), EV_STRING);
	}

	fclose(fh);
}

/* a hud.txt sprite's size in layout pixels; [0,0] when unknown */
vector
Art_SpriteSize(string name)
{
	string def = hash_get(g_hudSprites, strtolower(name), "", EV_STRING);

	if (def == "")
		return [0, 0];

	tokenize_console(def);
	return [stof(argv(3)), stof(argv(4))];
}

/* draws a hud.txt sprite with its top-left at p; false when unknown */
bool
Art_Sprite(vector p, string name, vector rgb, float alpha)
{
	string def = hash_get(g_hudSprites, strtolower(name), "", EV_STRING);
	vector sz;
	vector rect;
	vector rsize;

	if (def == "")
		return (false);

	tokenize_console(def);
	string pic = argv(0);
	rect = [stof(argv(1)), stof(argv(2))];
	rsize = [stof(argv(3)), stof(argv(4))];
	sz = drawgetimagesize(pic);

	if (sz[0] <= 0)
		return (false);

	drawsubpic(Art_Pos(p), rsize * g_hs, pic,
		[rect[0] / sz[0], rect[1] / sz[1]], [rsize[0] / sz[0], rsize[1] / sz[1]], rgb, alpha, 0);
	return (true);
}

void
Art_Init(void)
{
	Art_LoadHudTxt();
	Art_Strip(NUM_CHAR, "resource/hud/hud_character_num", 7);
	Art_Strip(NUM_BIGWHITE, "resource/hud/hud_sb_num_big_white", 18);
	Art_Strip(NUM_BIGRED, "resource/hud/hud_sb_num_big_red", 18);
	Art_Strip(NUM_BIGBLUE, "resource/hud/hud_sb_num_big_blue", 18);
	Art_Strip(NUM_CENTER, "resource/hud/hud_sb_num_center", 11);
	Art_Strip(NUM_BOTTOM, "resource/hud/hud_sb_num_bottom", 8);
}
