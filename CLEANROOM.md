# Clean-room rules

Bulwark is licensed under ISC. To keep it that way, no code from FTEQW, Quake,
Darkplaces or any other GPL engine goes into this repository, and nobody
writing engine code here reads those engines' source.

## Allowed inputs

- **Written specs and docs.** The `specs/` folder of FTEQW (for example
  `multiprogs.txt`), file-format write-ups, RFCs and similar prose.
- **Builtin declarations and their doc comments**, as shipped in the QC
  headers that games compile against (Nuclide's `src/common/fteextensions.qc`).
  These describe the interface QC code expects, not how an engine does it.
- **Compiler output.** FTEQCC is used as a build tool only. Its
  `-Fdumpopcodes` and `-Fwasm` outputs, and the `.dat` files it produces, show
  which opcodes exist and what each operand means.
- **Probes.** Small QC programs compiled with FTEQCC and run through our VM,
  plus inspection of real content (Nuclide's progs, maps and decls).
- **Game code.** Nuclide's QC source is read to learn what it expects from the
  engine. It is a client of the engine, not part of it.

## Not allowed

- Reading FTEQW's `engine/` tree (other than to build the compiler), or any
  other GPL engine's source.
- Pasting code from those engines, including "small" helpers.

## How findings are recorded

Whenever behaviour was learned from a probe or from content rather than a
written spec, write it down in `docs/spec/` with how it was established. Code
comments point at those files. If something is a guess, say so.

## Third-party code

Only public domain (or equally permissive) libraries go in `src/thirdparty/`,
unmodified, with their licence text kept at the end of each file:

- `stb_image.h`, `stb_image_write.h`, `stb_truetype.h`, `stb_vorbis.c` by
  Sean Barrett and contributors: public domain (or MIT, at your choice).
- `font8x8_basic.h` by Daniel Hepper, from Marcel Sondaar's and IBM's 8x8
  fonts: public domain.
