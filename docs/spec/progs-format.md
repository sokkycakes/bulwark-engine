# Progs format

What the loader in `src/qcvm/qcvm.c` accepts, and how each part was confirmed.

## Header

Version 7 progs (FTE's extended format) begin with the classic 15-int header:

| int | field |
|---|---|
| 0 | version (6 or 7) |
| 1 | crc (54730 for Nuclide's server, client and menu modules) |
| 2–3 | statements offset, count |
| 4–5 | global defs offset, count |
| 6–7 | field defs offset, count |
| 8–9 | functions offset, count |
| 10–11 | strings offset, size |
| 12–13 | globals offset, count |
| 14 | entity fields (words per entity) |

Version 7 adds more ints after these. The loader only reads `entityfields`
(byte offset 56) and otherwise sizes entities from the field defs, taking the
larger of the two.

## Record widths

Statements are either 16-bit (`op, a, b, c` as 4 × uint16 = 8 bytes) or 32-bit
(4 × uint32 = 16 bytes). Defs are either 16-bit (`type, ofs` as uint16 plus a
string index, 8 bytes) or 32-bit (12 bytes). The width is detected by checking
which size makes each section fit between its offset and the next section.
Every Nuclide module checked (progs.dat, csprogs.dat, menu.dat, hud.dat) uses
16-bit records. 32-bit statements are rejected for now.

## Functions

36 bytes each: first statement, parm start, locals count, profile, name,
file, numparms, then 8 parm sizes (one byte each). A negative or zero first
statement marks a builtin.

Builtins are bound **by name**, not by number. menu.dat numbers differ from
progs.dat, and many extensions are `#0:name`. A `prior` prefix is stripped.
Unknown builtins get a stub that warns once per name and returns 0.

## Memory layout

Each VM has one arena: `[globals | edicts | memalloc heap]`.

- Globals are padded to at least 65540 words for 16-bit progs so any 16-bit
  operand is in range.
- Pointers handed to QC are byte offsets into the arena.
- Entity values are entity numbers (not byte offsets).
- Field values are word offsets inside an entity.

## Strings

`0` is the empty/null string; positive values index the progs string table.
Runtime strings use `0x80000000 | slot` into a process-wide table (slot 0 is
skipped since its bit pattern is -0.0f). A conservative mark-and-sweep
collector scans globals, entity fields, the local stack, heap blocks, the
tokenizer and hash tables; it runs only while no VM is executing. `strzone`
behaves like `strcat` and `strunzone` does nothing.

## Calling convention

Return in globals 1–3, parameters at 4 + 3n for n < 8. Arguments past the
eighth live in globals named `$parm8`, `$parm9`, … (found by name). The
`CALLnH` opcodes pass the first two arguments in operands b and c instead.
