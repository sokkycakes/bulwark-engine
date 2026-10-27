# CSNZ compatibility matrix

Status describes engine-repository evidence, not demonstrated fidelity to a
specific CSNZ client build.

| Area | Existing engine evidence | CSNZ status | Next evidence or work |
|---|---|---|---|
| Map container | Q1/GoldSrc, Q3-family, and Source VBSP v19–21 support is documented. | Unknown which format/version the selected CSNZ maps use; no map was supplied. | Inspect one read-only representative map and run the existing loader/smoke test. |
| Map collision | BSP and Source brush collision exist; Source prop collision can be approximate. | Not verified on a CSNZ map. | Check player-sized traces, spawn floor, and representative prop collision. |
| Models and animation | GoldSrc IDST v10 and CSO `.seq` are documented; Source MDL v44–49 conversion is documented. | The format document notes encrypted CSO MDL v20/21 are unsupported. Target asset versions are unknown. | Inventory the target models; add a minimal non-proprietary fixture or local-only test. |
| Materials and textures | VTF/VMT and WAD3 paths are documented. | Target CSNZ texture/material variants are unknown. | Validate real files locally; record unsupported variants without copying assets. |
| Movement and traces | QuakeC VM and movement/trace APIs are present; Nuclide server programs run. | CSNZ movement, hitboxes, and role-specific physics are unmeasured. | Capture reproducible reference scenarios and compare fixed-tick results. |
| Weapons and damage | Game-side QuakeC is supported. | No CSNZ weapon state, cadence, reload, ammo, or damage rules are established in the engine repo. | Implement one evidence-backed weapon and test ammo/reload/damage transitions. |
| Zombie behavior and roles | QuakeC entities and bot/test infrastructure exist. | CSNZ zombie AI, human/zombie roles, and transformation rules are not established. | Inspect emulator/game evidence; implement one type and its smallest attack/role loop. |
| Round lifecycle | Game-side logic can own this state. | Win conditions, respawns, wave/round rules, and restart behavior are unknown. | Capture one complete mode and test an end-to-end local round. |
| Network | Bulwark has an engine-specific UDP/TCP protocol and entity snapshots. | Not wire-compatible with GoldSrc/FTE; emulator protocol compatibility is unknown. | Prefer local Bulwark authoritative play for the first slice; inspect emulator APIs before any adapter. |
| HUD and menus | CSQC, 2D rendering, Panorama UI, and VGUI resource parsing are present in the inspected branch. | Exact CSNZ UI assets and transitions are unknown. | Port only HUD needed to operate the first playable slice; validate from permitted reference captures. |
| Login, lobby, inventory | A generic local lobby exists. | Emulator coverage of these services has not been found. | Keep out of the initial playable slice unless the emulator evidence makes a service necessary. |

“Unknown” means no target asset or observable behavior was available. Existing
format support is not proof that a particular CSNZ file loads correctly.
