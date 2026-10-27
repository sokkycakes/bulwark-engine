/* A QuakeC entity script: compiled to scripts/vscripts/test_qc.dat and
 * added to the server VM as an extra progs. Fields and builtins bind by
 * name; the system globals must start like the game's. */
#pragma target fte

entity self, other, world;
float time;
void end_sys_globals;
.string classname;
.string targetname;
void end_sys_fields;

void(string s, ...) print = #0:print;
string(float f) ftos = #0:ftos;

float thinks;

float(float x) Triple = { return x * 3; };

void() OnPostSpawn =
{
	if (self.targetname == "qc_ent")
		print("ok qc self\n");
	else
		print("FAIL qc self\n");
};

float() QCThink =
{
	thinks = thinks + 1;
	if (thinks == 2)
		print("ok qc think\n");
	return 0.05;
};
