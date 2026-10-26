# Bulwark engine

A permissively licensed (ISC) engine for running [Nuclide](https://code.idtech.space/vera/nuclide)
games. It is a clean-room reimplementation: it speaks the same QuakeC
interface Nuclide targets, but it is not derived from FTEQW or Quake's source.
See [CLEANROOM.md](CLEANROOM.md) for the rules.

Full Quake compatibility is not a goal. The aim is a small, modern base for
new games.

## Status

- **QuakeC VM**: loads FTEQCC v7 progs (16-bit records), the full extended
  opcode set, runtime strings with garbage collection, memalloc heap, and
  about 260 builtins. Self-tests in `tests/qc`.
- **Dedicated server** (`bulwark-ded`): loads Q3-family BSPs (IBSP, RBSP,
  FBSP) with brush collision, and Q1 BSPs for entities only. Runs Nuclide's
  server progs: the map spawns and frames run (think, toss, fly, step, push,
  noclip movement, triggers).
- Not yet: client, renderer, networking, pk3 files, Q1 collision, PVS,
  multiprogs.

## Building

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build        # needs FTEQCC=/path/to/fteqcc to rebuild the test progs
```

## Running Nuclide's server

```sh
./build/bulwark-ded -basedir /path/to/nuclide -game base +map ai
./build/bulwark-ded -basedir /path/to/nuclide -game base -frames 720 +map ai   # 10 s headless, then exit
```

Options: `-basedir dir`, `-game dir` (repeatable, later ones override),
`-frames n`, `-developer`, and `+command args` for console commands.

## Docs

- [docs/spec/progs-format.md](docs/spec/progs-format.md)
- [docs/spec/opcodes.md](docs/spec/opcodes.md)
- [docs/spec/builtins.md](docs/spec/builtins.md)
- [docs/spec/bsp.md](docs/spec/bsp.md)
