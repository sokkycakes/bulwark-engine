/* Self-checking VM test. Compile with FTEQCC (-Tfte) and run with qcrun.
 * Every check prints FAIL with its name; main() errors if any failed. */
#pragma target fte

entity self, other, world;
float time;
vector v_forward, v_up, v_right;
void end_sys_globals;
.float frame, nextthink;
.void() think;
.string classname;
.vector origin;
.entity chain;
.float health;
.int ival;
.float flags;
void end_sys_fields;
.float modelindex, frame2, lerpfrac, frame1time, frame2time, skeletonindex, scale;
.vector angles;

void(string s, ...) print = #0:print;
void(string s, ...) error = #0:error;
string(float f) ftos = #0:ftos;
string(vector v) vtos = #0:vtos;
string(string s, ...) strcat = #0:strcat;
float(string s) strlen = #0:strlen;
string(string s, float start, float len) substring = #0:substring;
string(string fmt, ...) sprintf = #0:sprintf;
float(string s) tokenize = #0:tokenize;
float(string s, string sep, ...) tokenizebyseparator = #0:tokenizebyseparator;
float(string s) tokenize_console = #0:tokenize_console;
string(float n) argv = #0:argv;
entity() spawn = #0:spawn;
void(entity e) remove = #0:remove;
entity(entity start, .string fld, string match) find = #0:find;
entity(entity start, .float fld, float match) findfloat = #0:findfloat;
float(float f) floor = #0:floor;
float(float a, float b) pow = #0:pow;
float(float a, ...) min = #0:min;
float(float a, ...) max = #0:max;
float(string a, string b, optional float len) strncmp = #0:strncmp;
float(string s, string sub, optional float start) strstrofs = #0:strstrofs;
string(string a, string b, string c) strreplace = #0:strreplace;
float(string s, float i) str2chr = #0:str2chr;
int(string s) stoi = #0:stoi;
string(int i) itos = #0:itos;
__variant*(int size) memalloc = #0:memalloc;
void(__variant *p) memfree = #0:memfree;
float(float size, optional float type) hash_createtab = #0:hash_createtab;
void(float tab, string name, __variant value, optional float flags) hash_add = #0:hash_add;
__variant(float tab, string name, optional __variant deflt, optional float type, optional float index) hash_get = #0:hash_get;
int(string s) json_parse = #0:json_parse;
int(int n, string name) json_find_object_child = #0:json_find_object_child;
string(int n) json_get_string = #0:json_get_string;
float(int n) json_get_float = #0:json_get_float;
int(int n) json_get_length = #0:json_get_length;
void(string name, ...) callfunction = #0:callfunction;
float(string s) isfunction = #0:isfunction;
float(__variant f) checkbuiltin = #0:checkbuiltin;
void() not_a_real_builtin = #0:not_a_real_builtin;
void(vector ang) makevectors = #0:makevectors;
float(float modlindex, optional float useabstransforms) skel_create = #0:skel_create;
float(float skel, entity ent, float modelindex, float retainfrac, float firstbone, float lastbone, optional float addfrac) skel_build = #0:skel_build;
float(float skel) skel_get_numbones = #0:skel_get_numbones;
string(float skel, float bonenum) skel_get_bonename = #0:skel_get_bonename;
float(float skel, float bonenum) skel_get_boneparent = #0:skel_get_boneparent;
float(float skel, string tagname) skel_find_bone = #0:skel_find_bone;
vector(float skel, float bonenum) skel_get_bonerel = #0:skel_get_bonerel;
vector(float skel, float bonenum) skel_get_boneabs = #0:skel_get_boneabs;
void(float skel, float bonenum, vector org, optional vector fwd, optional vector right, optional vector up) skel_set_bone = #0:skel_set_bone;
void(float skel, float bonenum, vector org, optional vector fwd, optional vector right, optional vector up) skel_premul_bone = #0:skel_premul_bone;
void(float skel, float bonenum, vector org, optional vector fwd, optional vector right, optional vector up) skel_postmul_bone = #0:skel_postmul_bone;
void(float skeldst, float skelsrc, float startbone, float entbone) skel_copybones = #0:skel_copybones;
void(float skel) skel_delete = #0:skel_delete;
float(entity ent, string tagname) gettagindex = #0:gettagindex;
vector(entity ent, float tagindex) gettaginfo = #0:gettaginfo;

float failures;
float checks;
void(float ok, string name) check = {
	checks++;
	if (!ok) {
		print("FAIL: ", name, "\n");
		failures++;
	}
};

float(float n) fib = { if (n < 2) return n; return fib(n - 1) + fib(n - 2); };
float(float a, float b, float c, float d, float e, float f, float g, float h, float i, float j) tenargs = {
	return a + b * 2 + c * 3 + j * 10;
};

float called_by_name;
void() named_target = { called_by_name = 1; };

float global_arr[5];
vector global_varr[3];
int global_iarr[4];

void() test_float = {
	float a = 7, b = 2;
	check(a + b == 9, "add_f");
	check(a - b == 5, "sub_f");
	check(a * b == 14, "mul_f");
	check(a / b == 3.5, "div_f");
	check((a & 3) == 3, "bitand_f");
	check((a | 8) == 15, "bitor_f");
	check(!(a < b), "lt_f");
	check(a >= 7 && b <= 2, "ge le f");
	check((a || 0) && !(0 && a), "and or f");
	float c = 5; c += 2; c -= 1; c *= 3; c /= 2;
	check(c == 9, "compound float ops");
	/* note: "x &~= c" on a plain variable makes the compiler write the result
	 * through the constant's slot (see docs/spec/opcodes.md), so it is only
	 * tested on fields here */
	float bits = 1; bits |= 6;
	check(bits == 7, "bitset");
	check(floor(a / b) == 3, "floor");
	check(pow(2, 10) == 1024, "pow");
	check(min(4, 2, 9) == 2 && max(4, 2, 9) == 9, "min max varargs");
	check(7 % 3 == 1, "float modulo");
};

void() test_int = {
	int a = 7, b = 2;
	check(a + b == 9, "add_i");
	check(a - b == 5, "sub_i");
	check(a * b == 14, "mul_i");
	check(a / b == 3, "div_i");
	check(a % b == 1, "mod_i");
	check((a << 2) == 28 && (a >> 1) == 3, "shifts");
	check((a ^ 5) == 2, "xor");
	check((a & b) == 2 && (a | 8) == 15, "bitand/or i");
	float f = a;
	check(f == 7, "int to float");
	int g = (int)3.9;
	check(g == 3, "float to int");
	check(a > 6.5, "int float compare");
	check(2.5 < a, "float int compare");
	check(a * 0.5 == 3.5, "int float mul");
	int z = 0;
	check(!z && a, "int truth");
	int neg = -a;
	check(neg == -7, "negate int");
	check(stoi("0x10") == 16 && itos(-12) == "-12", "stoi itos");
};

void() test_vector = {
	vector v = '1 2 3', w = '4 5 6';
	check(v + w == '5 7 9', "add_v");
	check(w - v == '3 3 3', "sub_v");
	check(v * w == 32, "dot");
	check(v * 2 == '2 4 6' && 2 * v == '2 4 6', "scale");
	check(w / 2 == '2 2.5 3', "div_vf");
	check(v_x == 1 && v_z == 3, "vector component");
	v *= 2; v += '1 1 1'; v -= '1 0 0';
	check(v == '2 5 7', "compound vector ops");
	check(v_x == 2 && v.y == 5, "component access");
	check(!'0 0 0' && '0 0 1', "vector truth");
};

void() test_strings = {
	string s = "hello";
	check(strlen(s) == 5, "strlen");
	check(strcat(s, " ", "world") == "hello world", "strcat");
	check(substring("abcdef", 1, 3) == "bcd", "substring");
	check(substring("abcdef", -2, 2) == "ef", "substring negative start");
	check(sprintf("%d-%s-%i", 4, "x", (int)7) == "4-x-7", "sprintf basic");
	check(sprintf("%.2f|%5s|%v", 1.5, "ab", '1 2 3') == "1.50|   ab|1 2 3", "sprintf formats");
	check(ftos(3) == "3" && ftos(2.5) == "2.5", "ftos");
	check(strncmp("abcd", "abzz", 2) == 0, "strncmp");
	check(strstrofs("hello", "ll") == 2 && strstrofs("hello", "q") == -1, "strstrofs");
	check(strreplace("a", "bb", "banana") == "bbbnbbnbb", "strreplace");
	check(str2chr("A", 0) == 65 && s[1] == 101, "char access");
	string t = sprintf("%s", s);
	t[0] = 72;
	check(t == "Hello", "string char store");
	check(!"" && !(string)0 && s, "string truth");
	float n = tokenize("give \"two words\" { x }");
	check(n == 5 && argv(1) == "two words" && argv(2) == "{" && argv(-1) == "}", "tokenize");
	/* an entity block, as __fullspawndata holds it */
	n = tokenize_console("{\n\"classname\" \"team_round_timer\"\n\"setup_length\" \"70\"\n}");
	check(n == 6 && argv(3) == "setup_length" && argv(4) == "70" && argv(5) == "}", "tokenize_console spans lines");
	n = tokenizebyseparator("a,b;;c", ",", ";");
	check(n == 4 && argv(2) == "" && argv(3) == "c", "tokenizebyseparator");
	/* splitting the same text again gives strings of its own: a store
	 * into the new one leaves the one kept from before alone */
	string tk = sprintf("%s", "alpha beta");
	tokenize_console(tk);
	string a1 = argv(0);
	tokenize_console("x y");
	tokenize_console(tk);
	string a2 = argv(0);
	a2[0] = 65;
	check(a1 == "alpha" && a2 == "Alpha", "argv after a repeated split");
	switch (s) {
	case "nope": check(0, "string switch wrong"); break;
	case "hello": break;
	default: check(0, "string switch default"); break;
	}
};

void() test_control = {
	float hits = 0;
	for (float i = 0; i < 10; i++) {
		if (i == 3) continue;
		if (i == 8) break;
		hits++;
	}
	check(hits == 7, "for continue break");
	switch (hits) {
	case 1: hits = 0; break;
	case 5 .. 8: hits = 100; break;
	default: hits = -1; break;
	}
	check(hits == 100, "switch range");
	check(fib(15) == 610, "recursion");
	check(tenargs(1, 1, 1, 0, 0, 0, 0, 0, 0, 2) == 26, "more than 8 args");
	callfunction("named_target");
	check(called_by_name == 1, "callfunction");
	check(isfunction("named_target") && !isfunction("nope"), "isfunction");
	check(checkbuiltin(strlen) && !checkbuiltin(not_a_real_builtin), "checkbuiltin");
};

void() test_arrays = {
	for (float i = 0; i < 5; i++)
		global_arr[i] = i * i;
	check(global_arr[3] == 9, "float array");
	global_varr[2] = '7 8 9';
	check(global_varr[2] == '7 8 9', "vector array");
	for (int k = 0; k < 4; k++)
		global_iarr[k] = k * 3;
	check(global_iarr[3] == 9, "int array");
	float *p = &global_arr[1];
	check(*p == 1 && p[2] == 9, "pointer to global");
	p += 1;
	check(*p == 4, "pointer arithmetic");
	*p = 42;
	check(global_arr[2] == 42, "store through pointer");
	int *mem = memalloc(sizeof(int) * 16);
	for (int j = 0; j < 16; j++)
		mem[j] = j + 100;
	check(mem[15] == 115, "memalloc read/write");
	memfree(mem);
	int *again = memalloc(32);
	check(again[0] == 0, "memalloc zeroed");
	memfree(again);
};

void() thinker = { self.health = 99; };

void() test_entities = {
	entity a = spawn(), b = spawn();
	a.classname = "alpha";
	b.classname = "beta";
	b.health = 5;
	check(find(world, classname, "beta") == b, "find");
	check(findfloat(world, health, 5) == b, "findfloat");
	a.origin = '1 2 3';
	a.origin_z += 1;
	check(a.origin == '1 2 4', "vector field");
	a.ival = 12;
	a.ival += 3;
	check(a.ival == 15, "int field");
	float *hp = &a.health;
	*hp = 50;
	a.health += 1;
	check(a.health == 51, "field pointer");
	a.flags |= 6;
	a.flags &~= 2;
	check(a.flags == 4, "field bitset/bitclr");
	self = a;
	a.think = thinker;
	a.think();
	check(a.health == 99, "call through field");
	time = 10; /* slots freed in the first seconds of a map may be reused at once */
	remove(b);
	check(find(world, classname, "beta") == world, "remove");
	entity c = spawn();
	check(c != b, "removed slot not reused immediately");
};

void() test_builtin_state = {
	float t = hash_createtab(16);
	hash_add(t, "k", "value");
	hash_add(t, "n", 5.5);
	check((string)hash_get(t, "k") == "value", "hash string");
	check((float)hash_get(t, "n") == 5.5, "hash float");
	check((float)hash_get(t, "missing", 7) == 7, "hash default");
	int j = json_parse("{\"a\": [1, 2, 3], \"b\": \"text\", \"c\": 2.5}");
	check(j != 0, "json parse");
	check(json_get_length(json_find_object_child(j, "a")) == 3, "json array length");
	check(json_get_string(json_find_object_child(j, "b")) == "text", "json string");
	check(json_get_float(json_find_object_child(j, "c")) == 2.5, "json number");
};

float(vector a, vector b) near = { vector d = a - b; return d * d < 0.01; };

/* tests/data/skeltest.iqm (tools/mkiqm_test.py): joint "tip" at x=32 turns
 * about +Z by 90 degrees per raw frame; animation 1 ("bend") runs at 10 fps */
void() test_skeletons = {
	float s = skel_create(1), s2;
	entity e = spawn();
	vector o;
	check(s > 0, "skel_create");
	check(skel_create(2) == 0, "skel_create without a model");
	check(skel_get_numbones(s) == 2, "skel_get_numbones");
	check(skel_get_bonename(s, 2) == "tip" && skel_get_boneparent(s, 2) == 1, "bone name and parent");
	check(skel_find_bone(s, "tip") == 2 && skel_find_bone(s, "nope") == 0, "skel_find_bone");
	o = skel_get_boneabs(s, 2);
	check(near(o, '32 0 0') && near(v_forward, '1 0 0') && near(v_right, '0 -1 0'), "bind pose");
	e.modelindex = 1;
	e.frame = 1;
	e.frame1time = 0.1;
	skel_build(s, e, 1, 0, 0, 0, 1);
	o = skel_get_boneabs(s, 2);
	check(near(o, '32 0 0') && near(v_forward, '0 1 0') && near(v_up, '0 0 1'), "skel_build frame");
	e.frame1time = 0.05;
	skel_build(s, e, 1, 0, 0, 0, 1);
	skel_get_bonerel(s, 2);
	check(near(v_forward, '0.7071 0.7071 0'), "skel_build between frames");
	e.frame = 0;
	e.frame2 = 1;
	e.frame2time = 0.1;
	e.lerpfrac = 1;
	skel_build(s, e, 1, 0, 0, 0, 1);
	skel_get_bonerel(s, 2);
	check(near(v_forward, '0 1 0'), "skel_build lerpfrac");
	e.lerpfrac = 0;
	e.frame2 = 0;
	skel_build(s, e, 1, 0, 0, 0, 1);
	makevectors('0 0 0');
	skel_premul_bone(s, 2, '0 0 10');
	check(near(skel_get_boneabs(s, 2), '32 0 10'), "skel_premul_bone");
	skel_set_bone(s, 1, '0 0 5', '0 1 0', '1 0 0', '0 0 1');
	o = skel_get_boneabs(s, 2);
	check(near(o, '0 32 15') && near(v_forward, '0 1 0'), "skel_set_bone moves children");
	s2 = skel_create(1, 1);
	skel_copybones(s2, s, 0, 0);
	check(near(skel_get_boneabs(s2, 2), '0 32 15') && near(skel_get_bonerel(s2, 2), '32 0 10'), "skel_copybones into absolute");
	skel_delete(s2);

	check(gettagindex(e, "tip") == 2 && gettagindex(e, "nope") == 0, "gettagindex");
	e.origin = '100 0 0';
	e.angles = '0 90 0';
	e.frame = 1;
	e.frame1time = 0.1;
	o = gettaginfo(e, 2);
	check(near(o, '100 32 0') && near(v_forward, '-1 0 0'), "gettaginfo from frames");
	e.skeletonindex = s;
	e.angles = '0 0 0';
	o = gettaginfo(e, 2);
	check(near(o, '100 32 15'), "gettaginfo from the skeleton object");
	remove(e);
};

float(string name) cvar = #0:cvar;
string(string name) cvar_string = #0:cvar_string;
void(string name, string value) cvar_set = #0:cvar_set;
var float autocvar_bw_test_af = 5;
var string autocvar_bw_test_as = "abc";
var vector autocvar_bw_test_av = '1 2 3';

/* autocvar_<name> globals create cvar <name> with their value as the
   default and follow it when it changes */
void() test_autocvars = {
	check(cvar("bw_test_af") == 5 && cvar_string("bw_test_as") == "abc", "autocvar creates the cvar");
	check(cvar_string("bw_test_av") == "1 2 3", "vector autocvar default");
	cvar_set("bw_test_af", "7.5");
	cvar_set("bw_test_as", "hello");
	cvar_set("bw_test_av", "4 5 6");
	check(autocvar_bw_test_af == 7.5, "autocvar follows a float cvar");
	check(autocvar_bw_test_as == "hello", "autocvar follows a string cvar");
	check(autocvar_bw_test_av == '4 5 6', "autocvar follows a vector cvar");
};

void() main = {
	test_float();
	test_int();
	test_vector();
	test_strings();
	test_control();
	test_arrays();
	test_entities();
	test_builtin_state();
	test_skeletons();
	test_autocvars();
	print(sprintf("%d checks, %d failures\n", checks, failures));
	if (failures)
		error("vm test failed");
};
