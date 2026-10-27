// js_ent's scope
function ok(name, c) { printl((c ? "ok " : "FAIL ") + name); }

ok("js self", self.GetName() === "js_ent");

function Add(a, b) { return a + b; }

function OnRelay() {
	ok("js output caller", caller !== null && caller.GetName() === "relay");
	ok("js output activator", activator !== null && activator.GetName() === "lua_ent");
}

function OnGameEvent_test_event(params) {
	ok("js event " + params.who, params.value === 42);
}

function OnPostSpawn() {
	const lua = Entities.FindByName(null, "lua_ent");
	ok("js->lua call", lua.CallScriptFunction("Double", 21) === 42);
	ok("js handle identity", Entities.FindByName(null, "lua_ent") === lua);
	const v = new Vector(3, 4, 0);
	ok("js vector", v.Length() === 5 && v.ToKVString() === "3 4 0");
	ok("js qc global", typeof QC.Get("time") === "number");
	ok("js convars", Convars.GetStr("script_language") === "lua");
	ok("js origin type", self.GetOrigin() instanceof Vector);
	self.SetOrigin(new Vector(1, 2, 3));
	ok("js setorigin", self.GetOrigin().z === 3);
	const t = Entities.CreateByClassname("info_target");
	ok("js create", t !== undefined && t.IsValid());
	t.Destroy();
	ok("js stale handle", !t.IsValid());
	let threw = false;
	try { t.GetOrigin(); } catch (e) { threw = true; }
	ok("js stale handle throws", threw);
	FireGameEvent("test_event", { value: 42, who: "js" });
}
