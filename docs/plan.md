# CSNZ native gameplay port plan

## Milestone 0 — establish evidence and build baseline

- Identify the exact engine source revision to use and inspect its clean-room
  rules.
- Obtain the read-only client/reference build, local emulator source or binary,
  target OS/architecture, and compiler/test environment.
- Build Bulwark and its existing tests before changes; record failures and
  commands.
- Inventory one representative map and the asset versions needed for a small
  mode.

**Acceptance:** the inputs and baseline are reproducible, or each unavailable
input is documented as a blocker. No gameplay behavior is inferred from
unrelated CSO2 repositories or the DLL harness.

## Milestone 1 — map and player slice

- Load one selected map through the existing format path.
- Spawn a player at an evidence-backed spawn point.
- Verify camera, movement, floor collision, and input in a local session.

**Acceptance:** deterministic map-load and movement/collision checks pass.

## Milestone 2 — one human weapon and one zombie

- Implement weapon firing, reload, ammunition, cadence, traces, and damage in
  native game code.
- Implement one zombie type with the minimum evidence-backed movement and
  attack behavior.
- Add fixed-tick tests for ammo, reload, damage, death, and zombie attacks.

**Acceptance:** scripted local scenarios exercise each transition and pass.

## Milestone 3 — complete playable round

- Add evidence-backed death/respawn or round lifecycle, role assignment or
  transformation behavior, a round-end condition, and restart.
- Connect to a local client/server session and check replicated state.

**Acceptance:** a scripted local round starts, reaches its defined outcome,
  restarts, and passes state-consistency checks.

## Milestone 4 — fidelity and service integration

- Compare repeatable local reference scenarios with the permitted client or
  emulator, separating original behavior from emulator approximations.
- Add only the service integration shown to be necessary by the emulator.
- Add soak/stability checks and crash artifacts.

**Acceptance:** all supported features are reported with measured coverage;
unknown or unsupported behavior remains explicit.

## Current priority

GitHub-only discovery can establish engine capabilities and repository
architecture, but no CSNZ map, installed client, emulator, target platform, or
build environment is available in this task. The first code milestone is
blocked until representative gameplay evidence and a buildable checkout are
available; do not invent game rules to bypass that gap.
