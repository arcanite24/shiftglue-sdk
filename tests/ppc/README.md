# PowerPC instruction tests

Each `asm/<suite>.s` holds labelled test cases: a label, `#_ REGISTER_IN` and
`#_ MEMORY_IN` lines, the instructions, `blr`, then `#_ REGISTER_OUT` and
`#_ MEMORY_OUT` lines. The build assembles every file with the bundled
binutils (`tools/binutils`), links it at `0x82010000`, recompiles it with
`rexglue recompile-tests` and runs the result against the expectations as
Catch2 cases (`ppc_tests`, one `<suite>.functions.cpp` and `<suite>.cases.cpp`
per file so a large corpus compiles in parallel). Configure with
`-DREXGLUE_BUILD_TESTS=ON`; the executable lands beside `rexruntime`.

## Directive spellings

Both the original format and xenia's are accepted:

| Value | Spellings |
| --- | --- |
| GPR | `0x1234`, `5678`, `0x0123456789ABCDEFull` |
| FPR as bits | `0x3FF0000000000000`, `[3FF0000000000000]` |
| FPR as a number | `1.0`, `-2.5e3` (anything with a `.`) |
| VR | `[00000000, 00000001, 00000002, 00000003]` (high word first) |
| CR | `0x20000000` (`mfcr` layout) |
| Memory | `10001000 00 01 02` or `0x0000000010001000 [00, 01, 02]`; the address keeps its low 32 bits |

Only `#_` lines are directives. A plain `#` comment anywhere in a case is
skipped, which is how a mnemonic the bundled binutils does not know is kept
beside its hand-encoded `.long` (`mcrxr`, `mftb`, `dcbz128`, `vspltisw128`,
`vpermwi128`, `vpkd3d128`).

A label must be unique within its file; the same label may appear in
another file.

## Skipped cases

`skip.txt` lists cases to emit wrapped in Catch2's `SKIP`, one
`<suite>.<case>  # reason` per line, so a known difference stays visible in
every run without failing the suite. The reasons there fall in two groups:

- an expected value that contradicts the case's own inputs (cases imported
  from xenia-canary that were never valid);
- a documented difference from the hardware: the reciprocal estimates
  return exact values, a store-conditional without a reservation succeeds,
  `mfmsr` reads the runtime's interrupt state, and FPSCR models only the
  rounding and flush modes.

A case that starts passing should leave the list.

## Interpreter run

`ppc_interp_tests` runs the same corpus through the region oracle's
interpreter (`rex/ppc/interpreter.h`) instead of the recompiled code, so
the two implementations are checked against the same expectations
independently. It parses each case's directives at run time and needs no
generated sources. A case with an instruction the interpreter does not
implement is counted as unsupported, not failed; the oracle refuses such a
region the same way. The reciprocal estimates (`fres`, `frsqrte`, `vrefp`,
`vrsqrtefp`) compute the exact value, as the generator does, so a value
difference there is counted apart, but a NaN operand must still come back
as the hardware returns it. With the hardware corpus it passes 63,714
cases with none failing. `PPC_INTERP_FILTER=<substring>` limits the suites;
`PPC_INTERP_VERBOSE=1` prints every failure.

## Provenance

The suite started as the 167 files of xenia's `src/xenia/cpu/ppc/testing`
and took xenia-canary's additions on 2026-10-12 (commit `2372a54105`,
BSD-3-Clause): 254 new files and 22 updated ones. xenia-canary's 151
generated `instr__gen_*.s` files (167,636 cases recorded from hardware, 66
MB) are not in the repository; point `REXGLUE_PPC_TEST_EXTRA_ASM_DIRS` at a
checkout of them to include them in a build.
