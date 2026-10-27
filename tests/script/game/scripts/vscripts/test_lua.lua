-- lua_ent's scope
local function ok(name, cond) printl((cond and "ok " or "FAIL ") .. name) end

ok("lua self", self ~= nil and self:GetName() == "lua_ent")
ok("lua classname", self:GetClassname() == "logic_relay")

thinks = 0
function Think()
	thinks = thinks + 1
	if thinks < 3 then return 0.05 end
	printl("ok lua think")
	CheckKilled()
	AddThinkToEnt(self, "")
end

function Double(x) return x * 2 end

function InputFireUser1() return false end -- swallowed
function InputFireUser2() return true end

function OnGameEvent_test_event(params)
	ok("lua event " .. tostring(params.who), params.value == 42)
end

function OnPostSpawn()
	printl("ok lua postspawn")
	ok("lua sees root globals", root_value == 7) -- set by mapspawn.lua, found through _G
	local js = Entities.FindByName(nil, "js_ent")
	ok("lua find js", js ~= nil)
	ok("lua->js call", js:CallScriptFunction("Add", 2, 3) == 5)
	ok("lua->qc call", Entities.FindByName(nil, "qc_ent"):CallScriptFunction("Triple", 4) == 12)
	ok("lua handle equality", Entities.FindByName(nil, "js_ent") == js)
	local v = Vector(1, 2, 3) + Vector(1, 1, 1)
	ok("lua vector", v.x == 2 and v.z == 4 and tostring(v) == "Vector(2, 3, 4)")
	ok("lua qc builtin", QC.CallString("ftos", 5) == "5")
	ok("lua getfield", self:GetField("targetname") == "lua_ent")
	self:SetField("netname", "scripted")
	ok("lua setfield", self:GetField("netname") == "scripted")
	ok("lua no such field", self:GetField("no_such_field") == nil)

	local t = Entities.CreateByClassname("info_target")
	ok("lua create", t ~= nil and t:IsValid())
	t:Destroy()
	ok("lua stale handle", not t:IsValid())
	local okcall = pcall(function() return t:GetOrigin() end)
	ok("lua stale handle errors", not okcall)

	-- through Nuclide's sendInput, a frame later
	EntFire("lua_ent", "RunScriptCode", "printl('ok lua entfire runscriptcode')")
	EntFire("lua_ent", "FireUser1")
	EntFire("lua_ent", "FireUser2")
	EntFire("relay", "Trigger", "", 0.1, self)
	local k = Entities.CreateByClassname("info_target")
	k:SetName("doomed")
	EntFire("doomed", "Kill")
	doomed = k
end

function CheckKilled()
	ok("lua entfire kill", not doomed:IsValid())
end
