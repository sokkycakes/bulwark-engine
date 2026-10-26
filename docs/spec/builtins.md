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
