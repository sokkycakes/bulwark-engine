# Proposed CSNZ port architecture

This is a provisional architecture based on the inspected engine branch. The
module locations remain subject to the actual game package and emulator inputs.

## Boundaries

1. **Engine compatibility** — Keep Bulwark's general-purpose loaders, QuakeC
   VM, movement/traces, client-game support, renderer, sound, and networking
   reusable. Add narrow engine shims only for a demonstrated missing builtin,
   format feature, or state transition.
2. **Game rules** — Implement CSNZ entities, weapons, zombie behavior,
   team/role rules, round transitions, and HUD state as native game-side code.
   Bulwark already loads QuakeC game programs; the exact package location and
   whether any client-side code is needed must be chosen after examining the
   available game source and content.
3. **Asset pipeline** — Validate map/model/material/sound formats independently
   and add conversion tools only for formats the engine cannot consume. Keep
   proprietary source assets outside the distributable repository; commit
   original conversion code and format tests only.
4. **Emulator/service adapter** — Keep authentication, lobby, inventory, and
   session-service behavior separate from authoritative local gameplay. The
   available emulator's actual scope has not been established. Start with a
   local authoritative server for the playable slice; add a local-only adapter
   only if the emulator provides required behavior and an interface.

## Data flow

The initial target is a local client/server session: map and entities load
through existing engine paths; native game code owns player, weapon, zombie,
and round state; engine movement/traces and networking provide simulation and
replication. Any missing engine service is documented with a minimal
reproduction before changing engine interfaces.

This is an implementation hypothesis, not a claim about original CSNZ
architecture. Verify the flow against the supplied emulator and reference
scenarios before claiming fidelity.
