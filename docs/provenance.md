# CSNZ port provenance and licensing

- The inspected Bulwark source branch identifies the engine as ISC-licensed.
  `CLEANROOM.md` prohibits GPL engine code and describes the accepted inputs:
  public format/API documentation, compiler output, probes, game code as a
  client of the engine, and permissively licensed dependencies.
- The planned gameplay implementation is native original game-side code. Use
  observed behavior and permitted interface/game-code evidence; do not paste
  decompiler output or proprietary implementation code.
- The installed CSNZ client, if later provided, remains a read-only local
  reference. Do not add binaries, extracted proprietary assets, credentials, or
  raw decompiler output to GitHub.
- Record each external code or dependency origin and license before reuse.
  Preserve upstream license texts as required by the existing repository rules.
- Keep test maps/assets that are proprietary out of commits. Use generated or
  permissively licensed fixtures for automated tests where possible.
- Existing PR #8 reports a local-only DLL harness and says its harness is
  original code with no Nexon binaries committed. That work is a separate
  compatibility experiment and is not used here as native gameplay code.
