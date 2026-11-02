# Builtins

Builtins are matched to QC by name. Their signatures and intended behaviour
come from the declarations and doc comments in Nuclide's
`src/common/fteextensions.qc`; where a comment is silent, behaviour was
inferred from how Nuclide's QC uses the builtin, and is marked below.

## Where they live

- `src/qcvm/qc_builtins_core.c`: maths, strings, tokenizing, console, cvars,
  entity search, memory, digests. Shared by every VM.
- `src/qcvm/qc_extra.c`: hash tables, files, file searches, JSON.
- `src/server/sv_builtins.c`: server-only (models, traces, physics helpers,
  precaches, info keys, registered commands).
- `src/script/script.c`: the `script_*` builtins, Bulwark's own, that reach
  Lua, JavaScript and QuakeC entity scripts. See `scripting.md`.

## Notes on specific builtins

- `spawn` reuses a free slot only if it has been free for over 0.5 s, or if
  it was freed during the first 2 seconds of the map.
- `fopen` writes are confined to `data/` inside the game directory; reads try
  `data/` first, then the whole file system.
- `digest_hex` supports CRC16 (CRC-16/CCITT-FALSE, the variant Nuclide's
  entityDef code expects), MD5, SHA1 and SHA256. The algorithms are written
  from RFC 1321 and FIPS 180-4 and checked against Python's hashlib.
- `vectoangles` gives Quake's pitch (positive is up) while `r_meshpitch` is
  -1, the default, and a view's pitch (positive is down, so -45 for a vector
  45° up) when it is 1, as Nuclide sets it. Nuclide's weapons turn the view
  into a launch direction with `vectoangles` and fire along it with
  `makevectors`, so with the Quake pitch a Demoman grenade aimed up flew
  down. Checked against FTE run as a black box with Nuclide's settings.
- `rotate_axis`-style helpers (`rotatevectorsbyangle`, `rotatevectorsbyvectors`)
  follow the doc comments, but have not been checked against content that
  depends on them. *Unverified.*
- `tokenize_console` splits on any whitespace, newlines included; double
  quotes group and `//` comments run to the end of their line. Nuclide's
  `ncDict` keeps decl bodies as one `"key" "value"` line per pair and reads
  them back with `tokenize_console`, adding keys by rebuilding the body from
  the tokens. A tokenizer that stopped at the first newline (as the console
  does for a command line) dropped every key after the first line: hitscan
  damage dicts lost their `damage` key and every bullet did 0 damage. Found
  by a probe (`Add` three keys, read one back) and by running Rad-Therapy's
  weapons against monsters. Up to 1024 tokens are kept.
- `sprintf("%S", s)` quotes `s`; when it holds a quote, backslash, tab or
  line break it uses the escaped form `\"..."` (with `\n`, `\t`, `\r`,
  `\"`, `\\` inside) that `tokenize` and `tokenize_console` read back. This
  matches what FTE writes in Nuclide's saved games. See
  [console.md](console.md).
- `findentityfield(name)` gives a field's number for `getentityfieldstring`
  and friends, 0 when there's none; `entityfieldref(n)` gives the field
  itself.
- `SV_PerformSave` and `SV_PerformLoad` are called by `save` and `load`
  with a file handle the game reads or writes; see [console.md](console.md).
- `physics_supported` returns 0: there is no rigid-body physics yet.
- Networking (`Write*`, `multicast`, `stuffcmd`, …) builtins are accepted and
  do nothing until the network layer exists.
- `addprogs`, `externvalue`, `externset` and `externcall` work on extra
  progs loaded into the same VM; see the multiprogs section of
  [progs-format.md](progs-format.md).

- `route_calculate(ent, dest, denylinkflags, callback)` finds a path over
  the progs' waypoint file `data/<map>.way` (the layout Nuclide's node
  editor writes: a node count, then `x y z radius links` per node and
  ` node cost flags` per link). It uses A* from the node nearest the
  entity (preferring one in a clear line) to the node nearest `dest`, and
  skips links whose flags share a bit with `denylinkflags`. The callback
  runs at the start of the next server frame with a `memalloc`ed list of
  `{ vector dest; int linkflags; float radius; }`, ordered last to first
  as the fteextensions comment describes. Element 0 is always `dest`, so a
  failed search hands back a one-element list rather than NULL: Nuclide's
  bots index the list without checking it.

## Added for Stiletto

- `getsurfacenearpoint` and `getsurfacetexture` work in the server game on
  the collision model: Q1 maps keep their faces (polygons and texture names)
  for this, Q3-family maps use brush sides. The client game's versions use
  the renderer's surfaces as before.
- `te_customflash(org, radius, lifetime, colour)` sends `svc_flash`: a light
  that shrinks to nothing over its lifetime.
- `uri_get(url, id, mimetype, body)` (also named `uri_post`) fetches in the
  background and calls `URI_Get_Callback(id, status, body, length)` in the
  VM and progs that asked (an `addprogs` module gets its own answers). A
  body or mimetype makes it a POST; a mimetype of `"METHOD type"`, such as
  `"PUT application/json"`, picks another method. Requests with a method
  report any status the server answers with, error bodies included, and
  don't follow redirects. Status 0 means no answer. All three VMs.
- Web API token: requests the server's QC makes to addresses under the
  `stdb_url` cvar carry `Authorization: Bearer <stdb_token>`, added by the
  engine (`src/server/sv_uri.c`). Client and menu code never get it. Both
  cvars are never saved to config.cfg; set them from a cfg kept out of git.
  Made for SpacetimeDB's HTTP API, but any bearer-token API works.
- Named consoles (`con_getset`, `con_printf`, `con_draw`, `con_input`) for
  Nuclide's VGUI text views, in the client game and the menu:
  `src/client/cl_altcon.c`.
- `infokey`, `getsoundindex` (sounds the client game precaches itself get
  negative numbers), `soundnameforindex`, `makestatic` (the entity is drawn
  with every scene's engine entities until the map changes),
  `getlightstylergb`, `getchannellevel` (RMS of the next 1/30 s of the
  channel's sample), `R_EndPolygonRibbon` (each segment a quad facing the
  view) in the client game.
- `setcustomskin`, `loadcustomskin`, `applycustomskin`,
  `releasecustomskin`: `.skin` text; `geomset <group> <value>` picks a
  Half-Life model's sub-model per body part (counted from 1; 0 is the
  first), `replace` (or the legacy
  `surface,shader`) swaps a surface's material. `src/render/r_skin.c`.
- `remapshader` swaps a material's look until the map changes;
  `r_readimage` returns an image's RGBA pixels in the game's heap (the
  `__out` sizes come back through the parameter slots).
- `dynamiclight_spawnstatic`, `dynamiclight_set`, `dynamiclight_get`: lights
  the client game owns, with fteextensions' `LFIELD_*` numbers; origin,
  colour, radius, style (or style string), diffuse scale and die time
  affect the scene, the rest are kept.
- `gecko_*` and `hud_drawnative`: see [webcore.md](webcore.md).
- `netaddress_resolve` in the menu and client game.
- No-ops, since nothing is simulated or drawn for them: `physics_enable`,
  `physics_addforce`, `physics_addtorque`, and the server game's debug
  polygons (`R_BeginPolygon` and friends).
- `pr_missing [ssqc|csqc|menuqc]` lists what a loaded progs declares that
  Bulwark lacks.

Still missing: FTE_RAW_MAP's `brush_*` (editing the map's brushes at run
time; Nuclide's ncBreakableBrush, which no Stiletto content uses) and
ragdolls (`skel_ragupdate`, used only with `bodyque_forceRagdolls`).

## Autocvars

A global named `autocvar_<name>` of type float, int, bool, vector or string
mirrors cvar `<name>`, as fteextensions describes. When a progs loads (the
main progs or one added with `addprogs`), a missing cvar is created with
the global's initial value as its default; an existing one (from the
command line or a config) keeps its value. The globals are refreshed when
any cvar has changed since the last copy, checked on each entry into QC
and after each builtin returns, so `cvar_set` followed by a read of the
autocvar sees the new value.

## Bots

Clients made with `spawnclient` send no packets. Each server frame, after
`PlayerPreThink`, every bot gets one empty input frame (its current view
angles, frame time as `input_timelength`) run through
`SV_RunClientCommand`, so the progs can fill in the `input_*` globals from
its AI and move the bot like any other player.

## Traces

`traceline`, `tracebox` and friends take the usual `MOVE_*` flags.
`MOVE_OTHERONLY` follows its fteextensions comment: the trace tests only
the entity in the `other` global (owner, `SOLID_NOT` and dimension rules
don't apply to it), or only the map when `other` is the world. Nuclide's
monsters use it to check a clear line to their enemy before attacking:
without it every such check failed and no monster attacked.

## Still missing for Nuclide's server

Seen while running all 42 maps in Nuclide's `base` game: ragdolls
(`skel_ragupdate`). The `skel_*` family, `gettagindex`/`gettaginfo` and
the frame queries work on IQM models in both VMs, and the frame queries,
`frameforaction` and the model event builtins on studio models (see
[models.md](models.md)). `runstandardplayerphysics` is in place now that
there are clients. The `sv` console command hands a line to the server
game's `ConsoleCmd`, as Nuclide's tools expect.

## Autocvars

A global named `autocvar_<name>` (float, int, vector or string) reads the
cvar `<name>`. When the progs is loaded and the cvar doesn't exist yet, it
is created with the global's initial value as its default. Once any cvar
changes, globals are refreshed before the next top-level QC call and
after any builtin call that changed one (so `cvar_set` is seen at once). A write
from QC changes only the global. Bound in `src/qcvm/qc_autocvar.c`.

`random()` is seeded from the clock when a VM loads, so each run differs.

## Bots and routes

- Bots made by `spawnclient()` send no input. Each server frame, after
  PlayerPreThink, the engine runs one command per bot: its last command
  again, with msec set to the frame time, the next sequence number, and
  impulse cleared. That command goes through `SV_RunClientCommand` like a
  player's, so the game can fill `input_*` from its AI there, as Nuclide's
  ncBot::RunAI does.
- `route_calculate(ent, dest, denylinkflags, callback)` loads the waypoint
  graph from `data/<map>.way`, in the text layout of Nuclide's node editor.
  It runs A* from the node nearest the entity to the node nearest dest
  (preferring nodes the point can see), skipping links whose flags meet
  denylinkflags.
- The callback runs at the start of the next server frame with a memalloc'd
  `nodeslist_t` array (dest, linkflags, radius; 20 bytes each). Entry 0 is
  dest itself and the last entry is the first node to reach. With no route,
  the count is 0. Done in `src/server/sv_route.c`.
