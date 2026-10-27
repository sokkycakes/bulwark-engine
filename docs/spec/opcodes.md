# Opcode semantics

The opcode numbering in `src/qcvm/opcodes.h` comes from FTEQCC's
`-Fdumpopcodes` output. Operand roles below were confirmed by compiling probe
programs and reading the generated statements with `-Fwasm`, then checking
results in `tests/qc/vmtest.qc`.

Notation: `G[x]` is the global at word x, `E(e).f` is field f of entity e.

## Memory

| op | effect |
|---|---|
| `STORE_x a b` | G[b] = G[a] |
| `STOREP_x a b c` | \*(G[b] + G[c] words) = G[a] |
| `LOADP_x a b c` | G[c] = \*(G[a] + G[b] words) |
| `STOREF_x a b c` | E(G[a]).G[b] = G[c] |
| `LOAD_x a b c` | G[c] = E(G[a]).G[b] |
| `ADDRESS a b c` | G[c] = pointer to E(G[a]).G[b] |
| `GLOBALADDRESS a b c` | G[c] = (a + G[b]) × 4 (byte pointer) |
| `ADD_PIW a b c` | G[c] = G[a] + G[b] × 4 |
| `LOADA_x a b c` | G[c] = G[a + G[b]] |
| `BOUNDCHECK a b c` | error unless c ≤ G[a] < b; b and c are immediates |
| `LOADP_C a b c` | G[c] = byte at string G[a], float index G[b] |
| `STOREP_C a b c` | byte at string G[b], int index G[c], = G[a] |
| `LOADP_U8` | as LOADP_C with an int index |

## Compound assignment

`*STORE_x` / `*STOREP_x` (ADDSTORE, SUBSTORE, MULSTORE, DIVSTORE, BITSET,
BITCLR and friends) compute `b op= a`. When c is non-zero the result is also
written to G[c]. Compiler quirk: for `x &~= 2` on a plain variable FTEQCC puts
the constant's own slot in c, so the result overwrites that constant. The VM
keeps this behaviour; the test uses a field instead.

## Control flow

- `IF`/`IFNOT` test the integer bits; `IF_F`/`IFNOT_F` test the float value
  (so -0.0 is false).
- `SWITCH_x a b` stores G[a] in a switch register and jumps by b.
  `CASE a b` jumps by b if the register equals G[a]. `CASERANGE a b c` jumps
  by c if G[a] ≤ register ≤ G[b].
- `CALLn` / `CALLnH`: see progs-format.md.

## Arithmetic and comparisons

- Any comparison with an int operand produces an int (0/1); pure float
  comparisons produce a float.
- `RAND0` writes to c; `RAND1` is a → c; `RAND2` is a, b → c.
- Mixed int/float ops (`ADD_IF`, `MUL_FI`, …) convert the int to float first
  and produce a float unless the op name ends in `_I`.
