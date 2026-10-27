# Milestone result — mp.dll loaded, initialized, and called (verified)

Run in a cloud container: Ubuntu Wine 9.0 (win32 prefix), harness built with i686 mingw-w64,
against the **unmodified** `Bin/mp.dll` (build 0.0.18845.1) staged from the device. Native VC++
runtime from the game (`msvcp140*`, `vcruntime140`, `ucrtbase`, `concrt140`, `vccorlib140`) via
`WINEDLLOVERRIDES`; KERNEL32/USER32 + UCRT apiset forwarders from Wine.

```
step 1: LoadLibraryA("mp.dll")  (runs relocations, imports, TLS callbacks, DllMain, static init)
  OK: mp.dll mapped at 10000000  (its init machine code has now executed)
step 2: resolved exports  GiveFnptrsToDll=11490720  GetEntityAPI=113e86e0  GetNewDLLFunctions=113e87f0
step 3: GiveFnptrsToDll(engfuncs [213 slots], globals)
  OK: returned cleanly. Nexon's rep-movs of 213 dwords executed; engine table + gpGlobals installed.
step 4: GetNewDLLFunctions(&v=1) => 1 (ver now 1); slot0=113e6040 slot4=00000000
step 5: GetEntityAPI(table, 140) => 1; first slot=1147f0f0; engine-stub callbacks during registration: 0
```

## What this proves

- **Load + required initialization**: the PE loader mapped mp.dll at its preferred base
  0x10000000, ran its 2 TLS callbacks, DllMain and MSVC C++ static constructors. Original code ran.
- **Export addresses match the static analysis exactly** (0x11490720 / 0x113e86e0 / 0x113e87f0),
  confirming the disassembly the contract was derived from.
- **Three verified calls into Nexon's own machine code, all succeeding**:
  - `GiveFnptrsToDll` — the 852-byte (213-ptr) engine-table copy executed; no fault.
  - `GetNewDLLFunctions(v=1)` — returned 1, filled real mp.dll pointers.
  - `GetEntityAPI(v=140)` — returned 1, filled the DLL_FUNCTIONS table, and needed **0** engine
    callbacks during entity-class registration (so even this is safe with a minimal table).

## What it does NOT yet prove

- No game tick (`pfnGameInit`/`pfnServerActivate`/frame) — those read `globalvars_t`/`entvars_t`
  and call engine functions for real; needs the CSO layouts + working engine-func implementations.
- Nothing is wired into Nuclide yet (this is the standalone 32-bit helper).
- hw.dll / client.dll / anti-cheat untouched by design.

## Reproduce
```
make                      # builds csnz_mp_harness.exe (needs i686-w64-mingw32-gcc)
CSNZ_ROOT=/path/to/CSNZ ./run-harness.sh            # core milestone
CSNZ_ROOT=/path/to/CSNZ ./run-harness.sh --entityapi # + entity registration
```
