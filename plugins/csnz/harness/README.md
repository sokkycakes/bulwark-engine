# CSNZ DLL host — mp.dll execution (milestone 1)

Executes the **unmodified** Nexon `mp.dll` behind a compatibility layer, with the engine function
table supplied by host code instead of `hw.dll`. Original machine code runs; nothing is
reimplemented or patched.

- `COMPAT-MAP.md` — evidence-backed feasibility map: binaries, host, execution model, the
  engine-interface contract (from disassembly), entity-ownership plan, boundaries, blockers.
- `csnz_mp_harness.c` / `Makefile` / `run-harness.sh` — the i686 Win32 harness (host-side
  implementation for this milestone) and how to build/run it under Wine.
- `RESULTS.md` — the verified run: load + init + 3 successful calls into mp.dll.

Licensing: harness is original ISC-style code. No Nexon source, no decompiler output, no Nexon
binaries are included here. Private use only.
