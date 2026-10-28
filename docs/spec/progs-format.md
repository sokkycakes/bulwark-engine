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

A collection runs once 65536 strings, or twice the number that survived the
last one, are live. It visits only slots in use (they are kept in a list),
not the whole table: a map load runs without collections and can leave
millions of slots behind, and sweeping all of them each time cost a 32-bot
co-op server 15 ms a frame. Nuclide makes about 25,000 temporary strings a
frame in that test, so collecting every frame was most of the server's time.

`pr_profile start` counts the statements each server QC function runs, and
`pr_profile [n]` prints the n busiest and starts over. Builtins are not
timed; their cost shows up in the engine's own profile instead.

## Calling convention

Return in globals 1–3, parameters at 4 + 3n for n < 8. Arguments past the
eighth live in globals named `$parm8`, `$parm9`, … (found by name). The
`CALLnH` opcodes pass the first two arguments in operands b and c instead.

Open issue: the fteqcc used to build Nuclide (both Rad-Therapy's progs and
the rules progs) gives those extra-argument globals no definitions. A
probe with a nine-argument `sprintf` stored the ninth argument in an
unnamed global right after the constants, so the VM can't find it and a
builtin reads 0 or an empty string there. Builtin calls with more than
eight arguments lose the extra ones until the compiler's convention is
known from its documentation or more probes.

## Multiprogs

FTEQW's `specs/multiprogs.txt` describes `addprogs`: extra progs files
loaded into an already running VM, sharing its entities. Nuclide uses it for
`progs/singleplayer.dat` (game rules) on the server and `hud.dat` on the
client. This VM implements it like this:

- Each progs keeps its own functions, strings and globals. Its globals live
  in the VM's heap, so pointers between progs stay valid.
- Function values and static string values carry the progs number in bits
  24-30 (0 for the main progs). A call switches the active progs and
  switches back on return; strings decode against their own progs.
- Fields of an added progs are matched by name onto the main progs' entity
  layout. Fields the main progs lacks get one of 256 spare words reserved
  past the main layout. Nuclide's added progs needed none.
- The system globals (up to `end_sys_globals`, or the first 28 words) are
  copied across on every switch, so `self`, `time` and the rest are shared.
  Extra parameters (`$parm8` and up) are copied by name.
- An added progs' `init(float prevprogs)` runs once it is linked.
- Overlapping fields: FTEQCC gives fields of unrelated classes the same
  entity slot. Nuclide's csprogs has 2733 field definitions on 616 distinct
  offsets, for example `ncView::__mSetupView` shares a slot with
  `ncIO::__mSpawned`. So a method call's target depends on `self` being the
  right entity. That is why engine code must give `self` back after it
  runs QC from inside a builtin (see client-game.md).
