# Scripting

Bulwark runs map and game scripts in three languages through one layer:

| Language | Files | Runtime |
| --- | --- | --- |
| Lua 5.4 | `.lua` | `third_party/lua-5.4.7` (MIT) |
| JavaScript | `.js` | QuickJS 0.15, already vendored for the UI (MIT) |
| QuakeC | `.dat` | the server VM, as an extra progs (multiprogs) |

The game itself stays in QuakeC (Nuclide). Scripts sit next to it, the way
Valve's VScript sits next to Source's C++ game code. The design follows
VScript as Valve's public wiki documents it: per-entity script scopes, the
`vscripts` and `thinkfunction` keys, the `RunScriptCode` family of inputs,
`Input<Name>` hooks, `OnGameEvent_<name>` callbacks and the `EntFire` and
`Entities` API. Everything here was written from that public description
against Bulwark's own server. No Source or FTEQW code was read for it.

The code is in `src/script/`:

- `script.c`: scopes, thinks, inputs, the EntFire queue, events, the QC
  builtins and the console commands.
- `script_api.c`: the native API every language sees.
- `script_lua.c`, `script_js.c`, `script_qc.c`: the language backends.

Lua and JavaScript can each be left out at build time (`BULWARK_SCRIPT_LUA`,
`BULWARK_SCRIPT_JS`). Both compile for WebAssembly as they are, so web
builds keep them.

## Scopes

Every entity can own a scope: the variables and functions its scripts
define. A scope has a part for each language that has code in it, so one
entity can mix a `.lua` and a `.js` script. The world (entity 0) is the root
scope. The console's `script` command and `mapspawn` run there.

- **Lua:** one `lua_State` per level. The root scope is `_G`. An entity's
  scope is a table whose metatable falls back to `_G`, and it is the `_ENV`
  of every chunk run in it. A script reads root globals, and the globals it
  defines stay in its own entity.
- **JavaScript:** one runtime per level, and each scope is its own context
  with its own global object. Contexts cost more memory than Lua tables
  (about the size of a fresh QuickJS context each), so this suits dozens of
  scripted entities, not thousands.
- **QuakeC:** a scope's QC part is a progs added to the server VM, and its
  functions run with `self` set to the scope's entity. QC is compiled ahead
  of time, so it can't run code strings. `RunScriptCode` on an entity whose
  only scripts are QC goes to the default language.

Each scope gets `self`, plus `activator` and `caller` before an input
reaches it. When an entity is removed, its scope goes with it. A scope that
is still running a function is freed when that function returns.

Function lookups (`CallScriptFunction`, thinks, hooks, events) only see a
scope's own functions, not the root's, so a root function is never every
entity's hook.

## Map entities

These keys are handled by Nuclide's `ncIO`, through the builtins below:

| Key | Meaning |
| --- | --- |
| `vscripts` | space separated script files, loaded into the entity's scope when it spawns |
| `thinkfunction` | script function to call every 0.1 s |

Script files are looked up in `scripts/vscripts/` first, then as given. A
name with no extension tries `.lua`, `.js` and `.dat` in that order. After a
scope's scripts load, `Precache()` runs. `OnPostSpawn()` runs once every map
entity exists, or straight away for entities spawned later.

`scripts/vscripts/mapspawn.lua` and `mapspawn.js` run in the root scope just
before the map's entities spawn.

`maps/<name>.ent`, when it exists, replaces the map's entity lump on the
server. This makes it possible to add scripts to a map without recompiling
it. The tests use it.

## Inputs

Every input that reaches an entity through Nuclide's I/O first goes to the
script layer. That covers outputs, `sendInput`, the `input` command and
`EntFire`. The layer handles it as follows:

1. If the scope defines `Input<InputName>`, it runs first. Returning `false`
   swallows the input. QC functions return numbers, so a QC hook can't
   swallow one.
2. These inputs belong to the layer:

| Input | Data |
| --- | --- |
| `RunScriptCode` | code, run in the entity's scope in its own language (or `script_language`) |
| `RunScriptCodeQuotable` | the same, with `'` turned into `"` (Hammer values can't hold `"`) |
| `RunScriptFile` | a script file to run in the scope |
| `CallScriptFunction` | a function name |
| `SetThinkFunction`, `AddThinkFunction` | a think function name, `""` to stop |

Anything else goes on to the entity's own `Input`.

Nuclide separates output fields with commas, so code in an output can't
contain commas. Put the code in a function and call that function instead.

## Thinks

A think function runs every 0.1 s. If it returns a number above zero, that
is the delay until its next run. `AddThinkToEnt(ent, "")` or
`SetThinkFunction ""` stops it. Thinks run after the server's physics each
frame.

## EntFire

`EntFire` queues an input, and it is delivered at the start of a later
frame even with no delay, as in VScript. Delivery goes through Nuclide's
`sendInput` when the progs has it, and otherwise straight to the script
layer.

The target can be any of these:

- a targetname (a trailing `*` matches a prefix)
- a classname
- `!self` or `!caller` (the scope that fired the input)
- `!activator`

A handle that dies before delivery is dropped.

## Game events

`FireGameEvent(name, params)`, the `script_event` builtin and the
`script_event` command each call `OnGameEvent_<name>(params)` in every scope
that defines it. `params` is a table (Lua) or an object (JS). A QC handler
gets it as a single string of `"key" "value"` pairs.

## The native API

`script_help [filter]` lists everything with its arguments. The groups are:

- **Globals:** `print`, `printl`, `Msg`, `Time`, `FrameTime`, `GetMapName`,
  `RandomInt`, `RandomFloat`, `EntFire`, `DoEntFire`, `EntFireByHandle`,
  `IncludeScript`, `DoIncludeScript`, `AddThinkToEnt`, `FireGameEvent`,
  `SendToConsole`, `SendToServerConsole`, `TraceLine` (returns the
  fraction), `PrecacheModel`, `PrecacheSound`.
- **`Entities`:** `First`, `Next`, `FindByName`, `FindByClassname`,
  `FindInSphere`, `FindByClassnameNearest`, `FindByNameNearest`, and
  `CreateByClassname`, which runs the class's spawn function as map loading
  does.
- **`Convars`:** `GetStr`, `GetFloat`, `GetInt`, `GetBool`, `SetValue`.
- **`QC`**, the bridge into the game's own code:
  - `Call(name, ...)`, plus `CallString`, `CallEntity` and `CallVector` for
    other return types. These call any QC function or builtin, with `self`
    set to the calling scope's entity.
  - `Get(global)` and `Set(global, value)`, typed by the global's
    definition.
  - `HasFunction(name)`.
- **Entity methods:**
  - position and motion: `GetOrigin`/`SetOrigin` (also `GetAbsOrigin`,
    `SetAbsOrigin`), `GetAngles`/`SetAngles`, `GetVelocity`/`SetVelocity`,
    `GetBoundingMins`, `GetBoundingMaxs`, `GetForwardVector`,
    `GetLeftVector`, `GetUpVector`
  - health: `GetHealth`/`SetHealth`, `GetMaxHealth`/`SetMaxHealth`
  - identity: `GetClassname`, `GetName`/`SetName`, `GetModelName`,
    `GetOwner`/`SetOwner`, `IsValid`, `entindex`
  - removal: `Destroy` (also `Kill`)
  - any QC field: `GetField(name)` and `SetField(name, value)`, typed by the
    field's definition
  - scripts and inputs: `CallScriptFunction(name, ...)`, which reaches
    whichever language defines the function, `ValidateScriptScope`, and
    `FireInput(input, value, delay, activator)`

In Lua, methods use `ent:GetOrigin()`. In JS they use `ent.GetOrigin()`.

`Vector(x, y, z)` is a small class in each language. It has `x`, `y` and
`z`, and the methods `Length`, `LengthSqr`, `Length2D`, `Dot`, `Cross`,
`Norm`, `Normalized` and `ToKVString`. Lua adds operators (`+ - * /`, and
`*` between two vectors is the dot product). JS adds `Add`, `Sub` and
`Scale`, and also accepts `[x, y, z]` arrays where a vector is expected.

Entity handles stay valid only while their slot holds the same entity.
Each slot has a generation number that goes up when the entity is freed.
After that, a stale handle's `IsValid()` is false, and its other methods
raise an error. Within one language, the same entity always comes back as
the same handle object, so `==` (Lua) and `===` (JS) work.

### Sandbox

Lua gets `base`, `coroutine`, `table`, `string`, `math` and `utf8`. It
doesn't get `io`, `os`, `package`, `dofile` or `loadfile`, so files are
read only through `IncludeScript` and the engine's filesystem. JS gets
QuickJS's standard objects without its `std` and `os` modules.

A call from the engine into a script is stopped after `script_timeout`
seconds (default 5), and the error goes to the console.

## QC builtins

These are server only and bound by name. `src/common/c_script.h` in Nuclide
declares them, guarded with `checkbuiltin`, so the same progs still runs on
engines that lack them.

```c
float(entity ent, string files) script_attach;
void(entity ent, string func) script_think;
float(entity ent, entity activator, string input, string data, optional entity caller) script_input;
string(entity ent, string func, ...) script_call;     // up to 6 string arguments; result as text
float(entity ent, string code, optional string lang) script_run;  // world = root scope
void(string name, string keyvalues) script_event;
void(string target, string input, string value, float delay, entity activator) script_entfire;
```

## Console

| Command | |
| --- | --- |
| `script <code>` | run code in the root scope, in `script_language` (default `lua`) |
| `script_lua <code>`, `script_js <code>` | the same, in that language |
| `script_execute <file> [ent]` | run a script file in the root scope or an entity's |
| `script_attach <ent> <files>` | load scripts into an entity |
| `script_event <name> [key value ...]` | fire a game event |
| `script_list` | scopes, their languages and think functions |
| `script_help [filter]` | the native API |
| `ent_fire <target> <input> [value] [delay]` | queue an input |

| Cvar | Default | |
| --- | --- | --- |
| `script_language` | `lua` | language for `script` and for `RunScriptCode` on entities with no scripts |
| `script_timeout` | `5` | seconds one call may run |
| `script_debug` | `0` | print each script file as it runs |

## Tests

`tests/run_script_tests.sh <bulwark-ded> <nuclide base>` (with `FTEQCC` set)
loads `tests/script/game` on top of Nuclide's base game. The test map is
`ai.bsp` with its entities replaced by `maps/scripttest.ent`.

Lua, JS and QC entities check each other:

- calls across languages
- thinks
- EntFire through `sendInput`
- outputs from a `logic_relay`
- swallowed and passed-through inputs
- events
- stale handles
- field access
- the runaway guard

Every line in `tests/script/expected.txt` must appear and no `FAIL` may.
With `-DBULWARK_NUCLIDE_BASE=<dir>`, CTest runs it too. It needs Nuclide
with the script hooks (the `c_script.h` change).

## Not done yet

- Scripts on the client game (CSQC) and in the menu.
- Save games don't store script state.
- More of Valve's API: `ListenToGameEvent`, entity sounds, a `TraceLine`
  that returns a full table, and player-specific methods. QC builtins reach
  most of these through `QC.Call` meanwhile, for example
  `QC.Call("setmodel", ent, "models/x.mdl")`.
- Squirrel as a fourth backend.
