# Milestone 2 — runtime init path of mp.dll's pfnGameInit (OBSERVED-RUNTIME)

Method: the harness builds the real 180-slot engine table using the authoritative hw.dll
names (`analysis/hw_enginefuncs.csv`), installs a distinct **logging stub per slot**, loads the
unmodified `mp.dll`, calls `GiveFnptrsToDll` + `GetEntityAPI(140)`, then calls
`DLL_FUNCTIONS[0]` = **`pfnGameInit`** and records every engine call until it faults. This turns
the earlier static (noisy) slot scan into ground truth for the init path.

## What GameInit actually calls, in order

| order | slot | engine fn | notes |
|------|------|-----------|-------|
| 1 | 104 | `pfnRunPlayerMove`* | single call at entry (*name per CSV; may be a CSO-order quirk) |
| 2 | 122 | `pfnPrecacheEvent` ×6 | precaches 6 events |
| 3 | 61 | `pfnAlertMessage` ×~135 | init logging (banner / version / config dump) |
| 4 | 169 | `pfnCSO_ext_10` | **FIRST HARD CSO DEPENDENCY — see below** |

Total: 143 engine calls, then fault. `GiveFnptrsToDll` and `GetEntityAPI` themselves make **zero**
engine calls (confirming the smallest milestone's call was side-effect-free).

## The first blocker: pfnCSO_ext_10 (slot 169) returns a C++ interface object

Disassembly of `mp.dll` at the GameInit fault site (`0x1147f596`):

```
call ds:0x121d3cfc      ; slot 169 = pfnCSO_ext_10  -> returns eax
push 0x11972f04         ; arg
push 0x11b41780         ; arg
mov  ecx, eax           ; eax used as a C++ 'this' pointer
mov  edx, [eax]         ; edx = object's vtable           <-- faults: dummy return has null vtable
call [edx+0x1c0]        ; virtual call, vtable slot 0x1C0 / 4 = method #112
```

So `pfnCSO_ext_10` is a CSO engine accessor returning a **singleton interface object** with a large
vtable (≥113 methods). `GameInit` fetches it and calls its method at **vtable+0x1C0** with two
pushed args. Returning `0` → null deref; returning a zeroed buffer → the `[eax]` vtable read is
null → deref at `+0x1C0` faults one step later (observed: fault moved `0x1147f5a8`→`0x1147f5aa`).
This confirms it's a real object+vtable contract, not an int.

## Milestone-2 priority list (now evidence-based)

To drive `mp.dll` through `GameInit`, the compat layer must implement, in dependency order:

1. **`pfnAlertMessage`** (slot 61) — variadic `__cdecl`; forward the formatted text to Nuclide's
   console (or no-op). ~135 calls; trivial.
2. **`pfnPrecacheEvent`** (slot 122) — return a stable nonzero event index; back it with a table.
3. **`pfnRunPlayerMove`** (slot 104) — verify the call (single, at entry); likely safe no-op at init.
4. **`pfnCSO_ext_10`** (slot 169) — the real work: return a CSO interface object whose vtable has a
   valid method at **+0x1C0**. Recover its shape from hw.dll:
   - hw.dll handler for slot 169 is at **`0x025c23d0`** (constructs/returns the object).
   - Identify the object's class and the method at vtable **+0x1C0** in the `CSNZ-hw` Ghidra
     project; that method (called during GameInit with args `0x11972f04`, `0x11b41780` from mp.dll)
     is what the compat layer must emulate or forward.

After slot 169 is satisfied, re-run the harness to reveal the next dependency — this
**fault-driven discovery loop** maps the exact engine surface GameInit needs, with no guessing.

## Tooling

`csnz_mp_harness2.c` (in the private workspace) is the instrumented harness: 213 generated
per-slot logging stubs + `--gameinit`. It is a research tool, not engine code; it is kept in the
private analysis workspace rather than committed here. The slot→name table it uses is
`analysis/hw_enginefuncs.csv` (hw.dll-derived).
