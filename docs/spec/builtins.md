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

## Notes on specific builtins

- `spawn` reuses a free slot only if it has been free for over 0.5 s, or if
  it was freed during the first 2 seconds of the map.
- `fopen` writes are confined to `data/` inside the game directory; reads try
  `data/` first, then the whole file system.
- `digest_hex` supports CRC16 (CRC-16/CCITT-FALSE, the variant Nuclide's
  entityDef code expects), MD5, SHA1 and SHA256. The algorithms are written
  from RFC 1321 and FIPS 180-4 and checked against Python's hashlib.
- `rotate_axis`-style helpers (`rotatevectorsbyangle`, `rotatevectorsbyvectors`)
  follow the doc comments, but have not been checked against content that
  depends on them. *Unverified.*
- `physics_supported` returns 0: there is no rigid-body physics yet.
- Networking (`Write*`, `multicast`, `stuffcmd`, …) builtins are accepted and
  do nothing until the network layer exists.
- `addprogs`, `externvalue`, `externset` and `externcall` work on extra
  progs loaded into the same VM; see the multiprogs section of
  [progs-format.md](progs-format.md).

## Still missing for Nuclide's server

Seen while running all 42 maps in Nuclide's `base` game:
`processmodelevents` and ragdolls (`skel_ragupdate`). The `skel_*` family,
`gettagindex`/`gettaginfo` and the frame queries now work on IQM models in
both VMs (see [models.md](models.md)); `frameforaction` returns -1.
`runstandardplayerphysics` is in place now that there are clients. The `sv`
console command hands a line to the server game's `ConsoleCmd`, as
Nuclide's tools expect.

## Autocvars

A global named `autocvar_<name>` (float, int, vector or string) reads the
cvar `<name>`. When the progs is loaded and the cvar doesn't exist yet, it
is created with the global's initial value as its default. Once any cvar
changes, globals are refreshed before the next top-level QC call. A write
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
