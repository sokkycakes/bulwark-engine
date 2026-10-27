# CSNZ engine-table field map — stock GoldSrc vs CSO-added

Cross-reference of the CSO-extended `enginefuncs_t` (the table `GiveFnptrsToDll` copies into `mp.dll`) against the public HLSDK, plus which slots `mp.dll` actually calls. Lets milestone 2 implement the stock engine functions from known semantics and target only the CSO additions for Ghidra recovery.

## Sizes & deltas (OBSERVED-STATIC — measured from the binaries)

| Interface | Stock HLSDK | CSO (measured) | Nexon delta |
|---|---|---|---|
| `enginefuncs_t` (engine→game) | 157 | **213** (0xD5 dwords) | **+56** (slots 157–212) |
| `DLL_FUNCTIONS` (game→engine) | 50 | **74** (0x4A dwords) | **+24** (slots 50–73) |
| `NEW_DLL_FUNCTIONS` | 5 | **5** (0x14 bytes) | 0 — unchanged |

Sizes are from the `rep movs` counts in `GiveFnptrsToDll`/`GetEntityAPI`/`GetNewDLLFunctions`; stock counts from HLSDK `engine/eiface.h`. `NEW_DLL_FUNCTIONS` is byte-identical to stock, so no work there.

## ⚠ Confidence note on slot→name

Slot **sizes and the used/unused split are solid** (from the binary). Slot→name uses modern-HLSDK ordering as a reference, and it is reliable only for the **early, historically-stable GoldSrc functions**. The reference counts below come from a linear disassembly (they include some data-misread-as-code), so treat them as *approximate hotness*, not exact call counts — and note the tell-tale: the slot that would be `pfnAnimationAutomove` (76) in stock order is among the hottest, which is implausible for that function. So CSO's order **diverges from modern HLSDK somewhere in the mid-range**. The authoritative slot→name source is `hw.dll`'s table-fill site (Ghidra project `CSNZ-hw`), where the engine assigns each slot its function; names here are a hypothesis to confirm against it.

## CSO-added engine functions that mp.dll calls (36 of 56)

These are slots ≥157 — **not in any GoldSrc/HLSDK**, so they must be recovered from `hw.dll`. Ranked by approximate hotness (top = do first).

| slot | ~refs | priority |
|---|---|---|
| 165 | 4998 | ★ hot |
| 169 | 1636 | ★ hot |
| 176 | 1519 | ★ hot |
| 200 | 745 | ★ hot |
| 207 | 288 | ★ hot |
| 164 | 216 | ★ hot |
| 199 | 131 | · mid |
| 167 | 126 | · mid |
| 171 | 90 | · mid |
| 166 | 70 | · mid |
| 191 | 38 | · mid |
| (20 more minor slots) | <20 | minor |

The five hot ones (165, 169, 176, 200, 207) are the first to identify in `hw.dll` — they're engine services `mp.dll` leans on heavily (likely CSO memory/string/entity or networking helpers).

## Stock engine functions mp.dll calls (119 of 157)

Reference mapping (modern-HLSDK order; confirm mid/high indices against `hw.dll`). These are implementable in the compat layer from known GoldSrc semantics. Hottest (approximate):

| slot | stock name (hypothesis) | ~refs |
|---|---|---|
| 2 | pfnSetModel | 3372 |
| 1 | pfnPrecacheSound | 3248 |
| 58 | pfnCVarGetString | 2359 |
| 68 | pfnGetVarsOfEnt | 1892 |
| 23 | pfnCreateNamedEntity | 1873 |
| 95 | pfnLoadFileForMe | 1331 |
| 3 | pfnModelIndex | 1254 |
| 96 | pfnFreeFile | 1156 |
| 74 | pfnGetModelPtr | 831 |
| 75 | pfnRegUserMsg | 685 |
| 56 | pfnCVarRegister | 601 |
| 54 | pfnWriteString | 466 |
| 66 | pfnSzFromIndex | 240 |
| 64 | pfnPvEntPrivateData | 158 |
| 72 | pfnPEntityOfEntIndex | 133 |

(The `pfnWrite*` message slots 48–55 and `pfnAnimationAutomove` show counts inconsistent with those stock names — see the confidence note; confirm mid-range order against `hw.dll`.) The remaining ~100 stock slots are lower-frequency. Full ordered stock list for slots 0–156 is HLSDK `enginefuncs_t`.

## globalvars_t (gpGlobals) — stock layout, 31 fields

`mp.dll` reads `gpGlobals` constantly (its pointer is stored right after the 213-slot table). Stock field order (byte offsets, 32-bit): `time`(0) `frametime`(4) `force_retouch`(8) `mapname`(12) `startspot`(16) `deathmatch`(20) `coop`(24) `teamplay`(28) `serverflags`(32) `found_secrets`(36) `v_forward`(40) `v_up`(52) `v_right`(64) `trace_*`… `maxClients` `maxEntities` `pStringBase` `pSaveData` `vecLandmarkOffset`. CSO may append; confirm the trailing offsets from `hw.dll` before relying on anything past `vecLandmarkOffset`.

## entvars_t — stock layout known; CSO extensions TBD

Stock `entvars_t` (HLSDK `progdefs.h`) ends with the mod slots `iuser1-4`, `fuser1-4`, `vuser1-4`, `euser1-4`. CSO Zombi almost certainly uses those plus its own appended fields (zombie class, infection state, skill). Layout past the stock tail must come from `hw.dll`/`mp.dll` in Ghidra before any entity struct is exchanged across the process boundary.

## What this means for milestone 2

1. Implement the ~119 stock engine functions `mp.dll` calls as real forwards into Nuclide (model/sound precache, entity create/vars, tracelines, cvars, messages, string alloc). Semantics are public GoldSrc.
2. Stub all 56 CSO-added slots with logging first (the harness already does this), then identify the 36 it actually calls from `hw.dll`, hot ones first (165/169/176/200/207).
3. Recover `globalvars_t`/`entvars_t` CSO tails from Ghidra before exchanging any entity state.
4. The slot→name order for CSO must be read off `hw.dll`'s fill site, not assumed from HLSDK.
