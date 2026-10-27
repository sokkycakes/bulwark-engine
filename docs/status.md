# CSNZ native port status

Updated: 2026-10-27.

## State

Discovery is based on GitHub repository content only. A dedicated branch was
created from the existing engine source branch
`claude/project-thread-upkyrb`. The default branch `main` contains only a
README, so this branch preserves the usable engine baseline.

The engine already documents QuakeC game execution, GoldSrc/Quake map and
model support, Source BSP and asset readers, client-game support, and a local
networked server. No CSNZ-specific gameplay module was found in the inspected
engine tree.

## Work completed

- Inspected the current repository branch and build/docs structure.
- Recorded an initial architecture, compatibility matrix, provenance rules,
  prioritized playable-slice plan, and repository working instructions.
- Reviewed open PR #8. It is a standalone `mp.dll` execution harness; it does
  not implement native CSNZ rules or connect them to Bulwark gameplay.
- Searched accessible GitHub repositories for a CSNZ server emulator. No
  compatible emulator repository was found. CSO2 server results refer to a
  different title.

## Blockers

- No local engine checkout is available in the active workspace; build and
  runtime tests cannot be run in this GitHub-only task.
- No read-only CSNZ client/map/assets or emulator repository/binary is available
  here. The prompt's path fields were literal placeholders.
- Target OS/architecture and build tools are unspecified.
- Original gameplay rules and reference scenarios therefore remain unverified.

## Next actions

1. Provide an accessible GitHub emulator/reference repository, or make its
   source/API description available in GitHub.
2. Establish the target map and asset versions using permitted repository
   evidence; keep proprietary files local.
3. Build the engine baseline with the repository README commands in a local
   checkout and record results.
4. Implement and validate the first map/player/weapon/zombie/round slice from
   observed behavior. Do not claim CSNZ fidelity until compared with a permitted
   reference scenario.
