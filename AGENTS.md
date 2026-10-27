# Bulwark Engine CSNZ port instructions

Read `CLEANROOM.md` before changing engine code, then consult `docs/status.md`,
`docs/architecture.md`, and the relevant format specification.

## Port boundaries

- Keep engine format, VM, renderer, movement, and networking work generic.
- Put Counter-Strike Nexon: Zombies rules in native QuakeC/game code where the
  existing engine interfaces allow it. Add narrow engine compatibility shims
  only when a reproducible game-side case demonstrates a missing interface.
- Keep asset conversion separate from gameplay rules, and keep emulator/service
  integration behind a separate adapter.
- Preserve the installed client as read-only reference material. Do not access
  credentials, alter the installation, disable anti-cheat/DRM/security, or test
  against production services.
- Do not commit proprietary binaries, extracted client assets, raw decompiler
  output, or credentials. Keep reference material outside distributable source.
- Follow the repository's clean-room rules and record observed behavior,
  provenance, confidence, and unresolved questions in the relevant docs.

## Validation

Use the repository's documented CMake build and CTest entry points when a local
checkout and toolchain are available. Add deterministic gameplay tests for
meaningful rules; do not weaken assertions to hide failures. A successful build
or parser smoke test is not evidence of original-game fidelity. Label fidelity
checks unavailable when no original reference scenario or emulator is available.

## Current evidence limit

The current discovery is based on the GitHub repository only. The installed
CSNZ client, server emulator, target platform, and build environment were not
provided or found in accessible GitHub repositories. Do not fill these gaps
with guessed game behavior.
