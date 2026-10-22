# Lift report

`tools/ppu_lifter.py` writes `ppu_lift_report.json` next to its output: what the
lift could not do, as data, so real gaps can be told from data the function list
mistook for code. Schema version 1.

An instruction the lifter has no lowering for used to become a `/* TODO */` no-op.
It is now `ppu_trap_unknown_instruction(ctx, pc, word, "mnemonic");`, a call that
stops the process (kind `unknown-instruction`, `docs/STOPS.md`) if the instruction
is ever *executed*. Code that is never reached (data decoded as instructions, dead
paths) costs nothing at run time, and the generated code still compiles, so every
trap can be counted per function before anything runs.

```json
{
  "schema": 1,
  "tool": "ppu_lifter.py",
  "input_sha256": "<hex SHA-256 of the lifted file>",
  "summary": {
    "functions": 0,               "functions_with_traps": 0,
    "trapped_instructions": 0,    "capped_mid_tails": 0,
    "unresolved_targets": 0
  },
  "functions": [                  // only functions that contain a trap
    { "name": "func_00010008", "start": "0x00010008", "end": "0x00010014",
      "traps": [ ["0x0001000C", "0x00000000", ".word 0x00000000"] ] }
                                  //  [pc, raw word, decoded "mnemonic operands"]
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
lifter lacks and needs a lowering. `unresolved_targets` are what would reach
`ppu_unlifted_stub` (stop kind `unlifted-function`) at run time, or fail to link.
A function list that is wrong shows as many `.word` traps in the same functions;
fix discovery (`--code-end`, a better function list), not the traps.

`tools/lift_audit.py` counts the traps in the generated source as well.
