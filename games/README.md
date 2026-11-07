# Bulwark games

Every game made for Bulwark lives here, one folder per game:

- `csnz/`: the CSNZ Reconstruction
- `coop/`: Justice Task Force, the Sven Co-op style mod (with the L4D 360
  title and pause menus in `coop/src/menu/l4d360/`)
- `tftemplate/`: the TF template

They are built on the Nuclide framework, which stays in its own repo
(sokkycakes/nuclide, with its sample games `base/` and `valve/`). Link a
checkout of it first:

    games/link-nuclide.sh /path/to/nuclide

Then build a game's progs from its `src/` folder, for example:

    cd games/coop/src && make QCC=fteqcc

## Where things go

- This repo (private): the engine, and every Bulwark game under `games/`.
- sokkycakes/nuclide (public): changes to the Nuclide framework itself only
  (`src/`, `base/`), nothing derived from another game.
- sokkycakes/bulwark-private-assets (private): third-party game data, as
  `packs/<pack>/`, never in this repo or Nuclide.

Third-party data a game needs at run time (Half-Life's models and sounds,
the L4D 360 menu resources, paid fonts) comes from those packs.
