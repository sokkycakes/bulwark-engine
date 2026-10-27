# CSNZ port inventory

Last checked: 2026-10-27 (UTC).

## Engine repository

- Repository: `sokkycakes/bulwark-engine` (private; GitHub access available).
- Default branch `main` currently contains only `README.md`.
- Usable engine source baseline: `claude/project-thread-upkyrb`, head
  `1195fcd1a325eea4ad4e19224b9273536b1ac736`. A separate discovery branch is
  based on that source baseline so the empty default branch is not mistaken for
  the full engine.
- Relevant source language/build: C11 and C++, CMake (minimum 3.16), with
  CTest tests. The repository README documents
  `cmake -S . -B build`, `cmake --build build`, and
  `ctest --test-dir build`. Those commands have not been run in this
  GitHub-only session.
- The source baseline includes a QuakeC VM, dedicated server and SDL client,
  renderer, sound mixer, client-game VM support, and game-side Nuclide loading.
  See the repository README and `CMakeLists.txt`.

## Relevant documented compatibility

- Q1/GoldSrc BSP29/BSP30 and Q3-family BSP loading are documented in
  `README.md` and `docs/spec/bsp.md`.
- Source VBSP v19–21 parsing, collision, entities, and rendering are documented
  in `docs/spec/vbsp.md`; some prop collision paths are approximate.
- GoldSrc Studio IDST v10 and CSO `.seq` data are documented in
  `docs/spec/studio-mdl.md`. That document says encrypted CSO model versions
  20/21 are not handled.
- Source VTF/VMT and MDL v44–49 support is documented in
  `docs/spec/source-formats.md`.
- The built-in network protocol is Bulwark's own protocol, not wire-compatible
  with GoldSrc, QuakeWorld, or FTE; see `docs/spec/protocol.md`.
- Nuclide's client-game expectations and engine integration are documented in
  `docs/spec/client-game.md`. No CSNZ-specific game rules or playable CSNZ
  module were found in the inspected engine tree.

## External references and environment

- Installed CSNZ client: no path or files were supplied. No client files were
  inspected.
- Server emulator: no repository/path was supplied. GitHub searches available
  to this task found no CSNZ emulator repository. Search results for CSO2
  servers are for a different title and are not treated as compatible evidence.
- Target OS/architecture, compilers, FTEQCC version, available maps/assets, and
  build/test environment: unspecified.
- Active workspace: projectless GitHub task; no Bulwark checkout is mounted in
  the workspace. This run therefore cannot compile or run engine tests.

## Related existing work

Open PR #8, `plugins/csnz: execute unmodified mp.dll behind a compat layer
(milestone 1)`, adds a standalone 32-bit DLL harness and evidence notes. Its
description reports successful DLL loading and interface registration. It does
not add native CSNZ game rules or integrate gameplay into Bulwark, so it is not
counted as implementation of this native-port objective.
