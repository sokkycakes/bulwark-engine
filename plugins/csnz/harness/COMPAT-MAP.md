# CSNZ DLL execution — evidence-backed compatibility map

Scope: loading and running the **unmodified** Nexon CSNZ game DLLs (build `0.0.18845.1`) with
Nuclide as the host. Goal is execution of the original machine code behind a compatibility
layer — not reimplementation. Every claim below is labelled **OBSERVED-STATIC** (read directly
from the shipped binary) or **INFERRED** (reasoned, not yet proven by execution).

---

## 1. The binaries (OBSERVED-STATIC)

PE parsed directly from `Bin/*.dll`. All three game DLLs:

| Fact | mp.dll | hw.dll | client.dll |
|------|--------|--------|------------|
| Format / CPU | PE32 **i386** | PE32 **i386** | PE32 **i386** |
| Image base | 0x10000000 | 0x1d00000 | 0x1900000 |
| Relocations | yes (can rebase) | yes | yes |
| TLS directory | **yes, 2 callbacks** | yes, 2 callbacks | yes, 2 callbacks |
| LoadConfig / SEH | yes | yes | yes |
| Authenticode sig | yes (ignored at load) | yes | yes |
| DllCharacteristics | 0x0100 (NX only; **no ASLR**) | 0x0100 | 0x0100 |
| Size | 34.1 MB | 19.2 MB | 32.8 MB |

### Imports — this is the decisive finding

- **`mp.dll` imports only:** `KERNEL32`, `USER32`, `MSVCP140`, `VCRUNTIME140`, and the UCRT
  apiset (`api-ms-win-crt-*`). **No `hw.dll`. No `steam_api`. No anti-cheat.**
  → mp.dll receives its engine services *by table*, not by linking the engine. It can run
  without the engine binary, exactly like a Half-Life game DLL.
- **`client.dll` imports:** `WINMM`, `WSOCK32`, `KERNEL32`, `USER32`, `SHELL32`, `MSVCP140`,
  `VCRUNTIME140`, `SHLWAPI`, `IMM32`, `binkw32`, UCRT. Also clean (no anti-cheat), but needs the
  engine's VGUI2 / studio / client interfaces to do anything useful.
- **`hw.dll` imports (the dirty one):** `OPENGL32`, `DINPUT8`, `vgui.dll`, **`steam_api.dll`**,
  `libcef.dll`, `CrashReporter.dll`, **`nmcogame.dll`**, **`NGClient.aes`**, **`nxgsm.dll`**,
  WININET/WINHTTP/CRYPT32/DNSAPI/WS2_32, `binkw32`, UCRT.
  → `NGClient`/`nmcogame`/`nxgsm` are Nexon game-security / anti-cheat / comms. **We do not load
  hw.dll for the rules milestone**, which keeps anti-cheat entirely out of scope.

## 2. The host (OBSERVED-STATIC, from bulwark-integration/ + bwplug API)

- Nuclide/Bulwark is **Linux x86_64 ELF**; native extensions are `bwplug_*.so` (ELF, file-format
  hooks). There is no PE loader and no 32-bit execution path in the engine process.
- Bulwark already ships a GoldSrc/Source asset plugin (`bwplug_source.so`) and a `bwplug_csnz.so`
  that launches the retail client out-of-process under Wine (tier A — already working).

## 3. Execution model — decision (INFERRED, with the reason)

| Option | Runs original code? | Verdict |
|--------|---------------------|---------|
| Load mp.dll **in-process in 64-bit Nuclide** via `dlopen`/PE loader | no | **Impossible.** i386 machine code cannot execute in an x86_64 process; a shim cannot bridge a CPU word-size boundary. |
| **CPU emulation** (qemu-i386 / unicorn) | yes | **Unnecessary.** Linux runs i386 natively; emulation would only add cost and fragility. |
| **Hand-written PE loader in a native i686 ELF** (no Wine) | yes | Possible but **not smallest**: must implement the MSVCP140 + VCRUNTIME140 + UCRT surface that mp.dll's C++ static initializers hit across 2710 exports. Large. |
| **Wine-hosted i686 process** supplying the engine table | yes | **Chosen.** Wine's loader already provides relocations, import resolution, TLS callbacks and SEH; mp.dll's imports resolve against Wine builtins + the game's own shipped VC runtime. Reuses the offline Wine setup already proven on this machine. |

**"Nuclide as host" therefore means:** Nuclide (64-bit) owns and supervises a **32-bit
Wine-hosted helper**; the compatibility `enginefuncs_t` lives inside that helper so mp.dll's
callbacks stay in-process (no raw pointer crosses the word-size boundary). Only high-level state
crosses to Nuclide, over IPC, with explicit marshaling. The smallest milestone (below) is the
helper proving load + init + one call; wiring it under Nuclide's supervision + IPC is milestone 2.

## 4. Engine-interface negotiation — the contract (OBSERVED-STATIC, from mp.dll disassembly)

Recovered by disassembling the exports (image base 0x10000000):

```
GiveFnptrsToDll   @RVA 0x1490720  __stdcall(enginefuncs_t *pengfuncs, globalvars_t *pGlobals)
    mov ecx, 0xD5        ; 213 dwords
    rep movs             ; copies 213*4 = 852 bytes from pengfuncs into g_engfuncs
    mov ds:gpGlobals, eax; stores pGlobals
    ret 8                ; stdcall, 2 args, no return value
  -> PREREQUISITE: caller MUST pass an enginefuncs_t buffer of >= 852 bytes (213 pointers).
     Passing the stock HL size (0x90=144) under-runs by 69 pointers and corrupts memory.

GetEntityAPI      @RVA 0x13E86E0  __cdecl(DLL_FUNCTIONS *pFunctionTable, int interfaceVersion)
    cmp [arg2], 0x8C     ; requires interfaceVersion == 140
    mov ecx, 0x4A        ; 74 dwords
    rep movs             ; writes 74*4 = 296 bytes into pFunctionTable
    ... then iterates and registers entity classes (calls back through g_engfuncs) ...
    returns 1 on success, 0 on version mismatch / null table.
  -> PREREQUISITE: GiveFnptrsToDll must have run first (registration calls engine funcs);
     interfaceVersion must be exactly 140; table buffer >= 296 bytes.

GetNewDLLFunctions @RVA 0x13E87F0 __cdecl(NEW_DLL_FUNCTIONS *pTable, int *piVersion)
    cmp [*piVersion], 1  ; requires *piVersion == 1
    copies 0x14 = 20 bytes (5 pointers); returns 1, or sets *piVersion=1 and returns 0 on mismatch.
```

mp.dll exports the **old** `GetEntityAPI` (not `GetEntityAPI2`) and `GetNewDLLFunctions`, plus
`Server_GetBlendingInterface`. This is the classic GoldSrc server contract with a CSO-extended
`enginefuncs_t` (213 vs stock 144 slots). The extra 69 slots are CSO additions; their exact
semantics come from `hw.dll`'s fill site (Ghidra project `CSNZ-hw`) and are **not** required to be
correct for the milestone — only the *size* must match so the copy is safe.

## 5. Smallest viable execution milestone

> Load `mp.dll`, let the PE loader run its relocations/imports/TLS/DllMain/static init, resolve
> `GiveFnptrsToDll`, and call it with a 213-slot compatibility `enginefuncs_t` and a `globalvars_t`.

This is one verified call into Nexon's own machine code whose binary prerequisites are all proven
above, and it has **no reentrancy**: `GiveFnptrsToDll` only reads our table and writes mp.dll's own
globals — it never calls our stubs, so nothing downstream can be wrong. Loading alone already
executes a large amount of original code (C++ static constructors across 2710 exports via the TLS
callbacks + DllMain).

Stretch, same harness: `GetNewDLLFunctions(&v=1)` (safe; version check + 20-byte copy) and
`GetEntityAPI(table, 140)` (registers entity classes; *does* call back into our logging stubs, so
it is a diagnostic of how much engine surface the registration path touches).

## 6. Host-side implementation (this milestone)

- `csnz_mp_harness.c` — the i686 Win32 harness implementing §5, built with mingw, run under Wine.
  It is **ISC-style original code**; it contains no Nexon source and does not patch any Nexon DLL.
- `Makefile`, `run-harness.sh` — build and run (run sets `WINEDLLOVERRIDES` to prefer the game's
  shipped VC++ runtime over Wine builtins, matching the known-good offline launch).

For milestone 2 this harness becomes the 32-bit child that `bwplug_csnz.so` spawns, with the engine
table backed by real forwarding into Nuclide over IPC instead of logging stubs.

## 7. Entity ownership (plan, required before any gameplay call — INFERRED)

Not needed for the milestone, but it governs milestone 3+ and the task asks for it explicitly:

- GoldSrc `edict_t`/`entvars_t` live in the **engine**, handed to the game DLL through the
  engine table (`pfnPEntityOfEntIndex`, `pfnCreateEntity`, …). CSO extends `entvars_t`; its layout
  must be recovered from `hw.dll` before any entity pointer is exchanged — do **not** assume it
  maps onto Nuclide's `entity`/`ncEntity`.
- Proposed split: the **32-bit helper owns the GoldSrc entity array** (because mp.dll creates,
  thinks, moves, damages and frees entities through the table it was handed). Nuclide owns
  rendering/networking/input and receives a **marshaled snapshot** per frame (id, origin, angles,
  model, state), never raw edict pointers. Movement, thinking, damage and removal stay on the
  mp.dll side; Nuclide is the view/transport. Identity = a stable 32-bit entity index mapped to a
  Nuclide handle; lifetime = helper authoritative, Nuclide mirrors create/free events.

## 8. Boundaries, pointers, calling conventions (OBSERVED-STATIC / INFERRED)

- Word size: helper is i386 (4-byte pointers), Nuclide is x86_64 (8-byte). **No pointer, edict, or
  callback crosses raw.** The engine table is implemented *inside* the helper; cross-process traffic
  is value/struct marshaling only, with trampolines if Nuclide ever needs to invoke a DLL callback.
- Calling conventions (from disassembly): `GiveFnptrsToDll` = `__stdcall`; `GetEntityAPI` /
  `GetNewDLLFunctions` = `__cdecl`; GoldSrc `enginefuncs_t` members = `__cdecl`. The harness
  declares these exactly.
- TLS: 2 callbacks per DLL — Wine's loader runs them on `LoadLibrary`. A hand-rolled loader would
  have to invoke them manually (another reason Wine is the smallest path).
- Alignment / SEH / C++ EH: handled by Wine + the shipped `vcruntime140`/`ucrtbase`; not our code.

## 9. Exact blockers & missing artifacts

- **In-process execution inside 64-bit Nuclide: blocked permanently** by the i386/x86_64 word-size
  boundary. The only "in-process" option is building Nuclide itself as a Win32 i686 target, which is
  a different (and much larger) project; the out-of-process helper is the realistic host model.
- **Full client (client.dll):** needs the engine's VGUI2 / GameUI / `VFileSystem` / studio-render /
  Bink interfaces supplied; large. Not started.
- **hw.dll path:** re-introduces `steam_api` + `NGClient`/`nmcogame`/`nxgsm` (anti-cheat/comms).
  Explicitly out of scope; we never load it. No DRM/anti-cheat is bypassed because we don't touch it.
- **CSO enginefuncs_t slot semantics (69 extra slots):** needed for *correct* gameplay, not for the
  load/init/GiveFnptrsToDll milestone. Must be recovered from the `CSNZ-hw` Ghidra project (the
  engine's table-fill site) before `GetEntityAPI` registration or any `pfnGameInit` tick is trusted.
- **globalvars_t / entvars_t CSO layout:** required before milestone 3 (ticking + entities).
- **Toolchain location:** Wine + mingw are on the desktop, not in the cloud sandbox VM; the harness
  runs under Wine (desktop, or a container Wine install).
