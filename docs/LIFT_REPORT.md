# Lift report

`tools/ppu_lifter.py` writes `ppu_lift_report.json` next to its output: what the
lift could not do, as data, so real gaps can be told from data the function list
mistook for code. Schema version 1.

An instruction the lifter has no lowering for used to become a `/* TODO */` no-op.
It is now `ppu_trap_unknown_instruction(ctx, pc, word, "mnemonic");`, a call that
stops the process (kind `unknown-instruction`, `docs/STOPS.md`) if the instruction
is ever *executed*. The generated code still compiles, so every trap can be
counted per function before anything runs.

Only instructions the lifted C can reach are lowered at all. A lifted function is
entered at its first instruction and moves only by falling through or by
`goto loc_X`, so an instruction no such path reaches (a literal pool branched
over, padding after a return, anything after a trap call, which never returns)
is dead in the generated program whatever its bytes are. It is left out and
listed as a `dead` range instead of becoming a trap, and its call and branch
targets are not registered, so data does not seed functions of its own. Only
the lowerings that certainly end a block (`return`, `goto`, a tail call, a
jump-table switch, a trap) stop the walk; every other lowering is taken to fall
through, so an unrecognised one can only keep code, never drop it.

A live `goto loc_X` whose target has no emitted instruction (an in-range
address inside a too-large boundary, or past the bytes the disassembler saw)
used to get an empty tail label and fall off the function — a guest path
returned although it should have stopped. The label now calls
`ppu_unlifted_stub(0xX, ctx)` (stop kind `unlifted-function`, naming the
target), and the target is listed under `undecoded_targets`. The same window
rule applies to discovery: no function entry is created outside
`[code_lo, code_hi)` (lowest executable section for an ELF, load base for a
`--raw` image). Import stubs named by `--hle-stubs` stay callable by
declaration and are exempt, as before.

```json
{
  "schema": 1,
  "tool": "ppu_lifter.py",
  "input_sha256": "<hex SHA-256 of the lifted file>",
  "summary": {
    "functions": 0,               "functions_with_traps": 0,
    "trapped_instructions": 0,    "functions_with_dead": 0,
    "dead_instructions": 0,       "functions_with_undecoded_targets": 0,
    "undecoded_targets": 0,       "capped_mid_tails": 0,
    "unresolved_targets": 0
  },
  "functions": [                  // only functions with a trap, a dead range or
                                  // an undecoded target
    { "name": "func_00010008", "start": "0x00010008", "end": "0x00010014",
      "traps": [ ["0x0001000C", "0x00000000", ".word 0x00000000"] ],
                                  //  [pc, raw word, decoded "mnemonic operands"]
      "dead": [ ["0x00010010", "0x00010010"] ],
                                  //  [first, last] address of each unreachable run
      "undecoded_targets": [ "0x00010054" ] }
                                  //  live goto targets with no emitted instruction;
                                  //  the label stops (unlifted-function)
  ],
  "capped_mid_tails": [           // tails truncated at _MAX_MID_TAIL (0x6000 bytes)
    { "start": "0x...", "wanted_end": "0x...", "capped_end": "0x..." } ],
  "unresolved_targets": [ "0x..." ]   // call/branch targets no lifted function starts at
}
```

All addresses and words are `0x`-prefixed upper-case hex strings.

Classifying a trap: `.word` is the disassembler's "not an instruction" fallback, so
it is almost always data inside a function the discovery pass got wrong; the word
itself (a pointer, a string, `0x00000000`) usually says so. A named mnemonic
(`vmx_*`, `op31_*`, `rld??`) with a plausible encoding is a real instruction the
lifter lacks and needs a lowering. `unresolved_targets` are registered call or
branch targets with no lifted function: they reach `ppu_unlifted_stub` (stop
kind `unlifted-function`) at run time, or fail to link. A branch outside the
executable code window `[code_lo, code_hi)` (derived automatically from the ELF's
executable sections/segments, or overridden with `--code-end`) or a live goto to
an undecoded in-range address stops the same way but is not counted there (see
`undecoded_targets`). A function list that is wrong shows as many `.word` traps in
the same functions; fix discovery (`--code-end`, a better function list), not the traps.

`tools/lift_audit.py` counts the traps in the generated source as well.
