# Milestone 2 — runtime init path of mp.dll's pfnGameInit (OBSERVED-RUNTIME)

Method: the harness builds the real 180-slot engine table using the authoritative hw.dll
names (`analysis/hw_enginefuncs.csv`), installs a distinct **logging stub per slot**, loads the
unmodified `mp.dll`, calls `GiveFnptrsToDll` + `GetEntityAPI(140)`, then calls
`DLL_FUNCTIONS[0]` = **`pfnGameInit`** and records every engine call until it faults. This turns
the earlier static (noisy) slot scan into ground truth for the init path. Tool:
`csnz_mp_harness2.c` (run under Wine).

## What GameInit actually calls, in order

| order | slot | engine fn | notes |
|------|------|-----------|-------|
| 1 | 104 | `pfnRunPlayerMove`* | single call at entry (*name per CSV; may be a CSO-order quirk) |
| 2 | 122 | `pfnPrecacheEvent` x6 | precaches 6 events |
| 3 | 61 | `pfnAlertMessage` x~135 | init logging (banner / version / config dump) |
| 4 | 169 | `pfnGetSharedDataMgr` | **FIRST HARD CSO DEPENDENCY — see below** |

Total: 143 engine calls, then fault (before harness4). `GiveFnptrsToDll` and `GetEntityAPI`
themselves make **zero** engine calls (confirming the smallest milestone's call was
side-effect-free).

## The first blocker: pfnGetSharedDataMgr (slot 169) returns a C++ interface object

Disassembly of `mp.dll` at the GameInit fault site (`0x1147f596`):

```
call ds:0x121d3cfc      ; slot 169 = pfnGetSharedDataMgr -> returns eax
push 0x11972f04         ; arg
push 0x11b41780         ; arg (config)
mov  ecx, eax           ; eax used as a C++ 'this' pointer
mov  edx, [eax]         ; edx = object's vtable           <-- faults: dummy return has null vtable
call [edx+0x1c0]        ; virtual call, vtable slot 0x1C0 / 4 = method #112
```

So `pfnGetSharedDataMgr` returns a **singleton interface object** (`ISharedDataMgr*`) with a large
vtable (>=113 methods). `GameInit` fetches it and calls its method at **vtable+0x1C0** with two
pushed args (`"MPDLL"` key + config).

## UPDATE — pfnGameInit now runs to completion (OBSERVED-RUNTIME)

With sokky's slot-169 analysis (`evidence/HW-SLOT169-SHAREDDATA.md`), `csnz_mp_harness4.c`
replaced the four blocking stubs with real stand-in implementations:

- **`eng_alert`** (slot 61) — real variadic `__cdecl` that prints the message.
- **`eng_precache_event`** (slot 122) — returns a monotonically increasing nonzero index.
- **`eng_runplayermove`** (slot 104) — no-op.
- **`eng_getsharedmgr`** (slot 169) — returns a **stand-in `ISharedDataMgr`**: an object whose
  first dword points at a 160-entry vtable; entry #112 is a `thiscall` handler that receives
  `(this, key, cfg)`, logs them, and returns a non-null pointer; all other entries are a safe
  default stub.

**Result:** `pfnGameInit` **returns cleanly** — 143 engine calls total
(`pfnAlertMessage`x135, `pfnPrecacheEvent`x6, `pfnRunPlayerMove`x1, `pfnGetSharedDataMgr`x1), and
the log confirms `vmethod112 key='MPDLL' cfg=11b41780`, then `pfnGameInit returned`. The whole
GameInit init path is now traversable with the engine table supplied out-of-process — no fault.

Caveats (honest state):
- The `ISharedDataMgr` is a **stand-in**, not the real hw.dll semantics. It satisfies GameInit's
  shape (object -> vtable -> method #112 returns non-null) but does not yet store/return real
  shared config. The real method is hw.dll `0x025c23d0` (constructor) + the class method at
  vtable+0x1C0; recovering its true behaviour is the next Ghidra task before relying on any value
  it hands back.
- The `pfnAlertMessage` **text prints as garbage** — a cosmetic va_args/format mismatch in the
  stand-in formatter, not a structural problem. The call counts and control flow are correct.

Next fault-driven step: advance past GameInit to the next entry point (`pfnServerActivate` /
spawn / map load) to discover the next CSO dependency, OR harden the ISharedDataMgr to real
semantics. Tool: `csnz_mp_harness4.c`.

## Reproduce
```
i686-w64-mingw32-gcc -O2 -o csnz_mp_harness2.exe csnz_mp_harness2.c
CSNZ_ROOT=/path/to/CSNZ wine ./csnz_mp_harness2.exe --gameinit

i686-w64-mingw32-gcc -O2 -o csnz_mp_harness4.exe csnz_mp_harness4.c
CSNZ_ROOT=/path/to/CSNZ wine ./csnz_mp_harness4.exe --gameinit
```
