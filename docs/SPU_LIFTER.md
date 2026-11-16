# SPU → C Lifter — Status & TODO

Brief for `tools/spu_lifter.py` (added to close the "no SPU lifting" gap noted
in the README's critical gaps). The PPU side had `ppu_lifter.py`; the SPU side
only had `spu_disasm.py`. This document tracks what the lifter does today and
what still needs doing to actually execute SPU code.

## What exists now

`tools/spu_lifter.py` mirrors `ppu_lifter.py`:

- Emits each SPU function as `void spu_func_XXXXXXXX(spu_context* ctx)`.
- Instruction semantics live in `static inline u128 spu_*()` helpers in the
  generated source preamble (readable + unit-testable); the translator just
  wires `ctx->gpr[N]` to them.
- Output: `spu_recomp.h` / `spu_recomp.c`, plus a `spu_function_table[]`.
- CLI prints a coverage summary including a per-mnemonic list of any
  unsupported instructions encountered.

Related pieces:

- `docs/SPU_INTERPRETER_LIFTED_HANDOFF.md` describes how execution returns to
  lifted code after the interpreter ran an unlifted region
  ([SPU_INTERPRETER_LIFTED_HANDOFF.md](SPU_INTERPRETER_LIFTED_HANDOFF.md)).
- `tools/lift_jobmods.py --merge-chunks` (PR #386) passes `--merge-chunks` to
  `spu_lifter.py`: each run of chunks between call targets becomes one C
  function with gotos instead of chunks linked by trampolines.
- `SPU_INTERP_STATS=<seconds>` (PR #394) is read in `runtime/spu/spu_interp.c`
  (`getenv`, about line 454). While set, the interpreter reports how much SPU
  code runs interpreted, by image and 1 KB page of local store.
- SPURS workload registration: `spu_workload_register()` and
  `spu_workload_register_img()` in `runtime/spu/spu_workload.c` take a content
  fingerprint and a lifted entry function; `cellSpurs` calls
  `spu_workload_dispatch()` to run the matching image. `tools/build_spu_workloads.py`
  generates the registration table.

Targets the ABI in `runtime/spu/spu_context.h` (`u128 gpr[128]`, 256 KB
local store, channels, MFC staging) and the `u128` union in
`include/ps3emu/ps3types.h`.

## Generated-code ABI

Each generated header carries `SPU_LIFTER_ABI`; its `spu_recomp_register()`
checks that value against `SPU_RUNTIME_LIFTER_ABI` before adding any functions.
A mismatch stops with `lifter-abi` and identifies the SPU processor, expected
ABI, and generated ABI. The pinned layout signature in
`include/ps3emu/lifter_abi.h` covers the fields accessed by generated code and
its inline helpers/macros: `gpr`, `pc`, `status`, `stop_code`, `image_id`,
`event_status`, `event_mask`, `srr0`, `int_enable`, `host_depth`, `reloc_delta`,
`ls`, `policy_mode`, `steps`, and `budget` (the last member addressed by
generated code; ABI 2). The runtime-only MFC marker and inline engine follow
it without changing any pinned member offset or size.
A changed pinned member offset or size requires
updating the signature pin and bumping the runtime ABI together;
`tests/gates/test_spu_lifter_abi.py` enforces the pin.

The signature is per host data model: `spu_channel.lock` is a `volatile long`,
so LP64 (macOS arm64, Linux x86_64) and Win64/LLP64 lay out every later pinned
field differently. `include/ps3emu/lifter_abi.h` carries a measured pin for each
model and rejects an unverified model at compile time. The always-built SPU
registration runtime (`runtime/spu/spu_workload.c`) asserts the selected pin,
and `tests/gates/probe_spu_lifter_layout_pin.c` is also cross-compiled for
Windows x64 by the gate test. Running the Windows runtime is the Windows t0
job's coverage, not a local macOS check.

Prebuilt lifts are validated before they are compiled:
`tools/check_spu_lifter_abi.py` requires `SPU_LIFTER_ABI` in each generated
header and the `ps3_spu_lifter_abi_check` call before the registration's first
`spu_register_function` call. `tools/build_spu_workloads.py` runs it before
writing a registry, which covers an existing `spu_recomp.c`, `--skip-lift`,
`--relift` and a caller-supplied `--lifter`; the template's `SPU_DIR` helper
runs it at configure time. A lift from before the gate therefore fails the
build instead of linking without a check.

## The budgeted twin

Every back-edge site the lifter emits (a backward `goto`, a cross-function
`SPU_DIRECT`, a return or indirect branch through the trampoline) carries
`SPU_LIFT_BACKEDGE(ctx)`, which is an empty statement unless the source is
compiled with `-DSPU_LIFT_BUDGET`: the production object is unchanged. With
the definition the macro counts `ctx->budget` down, and a lift that finds it at
0 unwinds through `spu_halt` with `SPU_STATUS_BUDGET_EXHAUSTED`
(`docs/SPU_XDIFF.md` section 10). The generated header renames the twin's
symbols (suffix `_budget`) inside the same `#ifdef`, so both compilations link
into one program. `tools/build_spu_workloads.py --budget-twin` (CMake:
`PS3RECOMP_SPU_BUDGET_TWIN=ON`) writes each image's `spu_recomp_budget.c`,
which defines `SPU_LIFT_BUDGET` and includes `spu_recomp.c`, and registers the
twin beside the production entry with `spu_workload_register_twin`. A twin
doubles the lifted code a title compiles, so it is off by default.

## Native overrides (T2 pilot)

`--override 0xADDR=SYMBOL` is repeatable. `SYMBOL` is declared as
`void SYMBOL(spu_context*)` in the generated header and defined by the
consumer. Every `spu_func_ADDR` remains the lifted T1 reference body. The
function-table entry at `E` selects `SYMBOL`; direct call and branch references
from outside its unit also target it. References within an admitted unit stay
directed at lifted member symbols for A/B. The registry stores both selected
and reference pointers (`tools/spu_lifter.py`, `runtime/spu/spu_channels.c`).

The pilot fails closed unless the decoded function is a leaf compute routine:
no call out (`brsl`/`brasl`/`bisl`/`bisled`, excluding PC-getters), channel instruction,
indirect branch other than `bi $r0` with the incoming link value preserved,
stop/halt, cross-bound tail call or fall-through tail call. A stack reload
qualifies only when its dominating save and matching slot are proved under
`"spu-abi-link-slot"`. `override_leaf_problem` refuses any leaf/return proof requiring an additional
assumption: `"spu-abi-link-slot-private"`, `"spu-abi-callee-saved"` or
`"no-unmodelled-indirect-entry"`, and names that reason. The general lifter
may still emit `SPU_RET` under those explicitly recorded assumptions; its
whole-job replay and in-run audit are empirical checks, not T2 admission.
The pipeline recomputes the return proof and calls the lifter's same
`override_unit_admission` gate before registration, even if an alternate
lifter subprocess omitted the gate (`tools/spu_lifter.py`:
`override_leaf_problem`, `override_unit_admission`, `compute_bi_r0_jumps`;
`tools/spu_program_pipeline.py`: registration gate).

An override may name a closed tail-call unit instead of one leaf. Starting at
entry `E`, the unit is the transitive closure of sequential fall-through edges
into detected function starts and direct `br`, `brz`, `brnz`, `brhz` or
`brhnz` edges into detected function starts. Fall-through addresses use SPU
local-store wrapping (`(pc + 4) & 0x3FFFC`), including a transfer from the end
of local store to address zero. The single-function path retains its strict
leaf check and also undergoes the reachable-exit proof below. For a chain, each
member must pass that same check after only those exact tail edges are exempted;
calls (`brsl`/`brasl`/`bisl`/`bisled`), channels, indirect branches, stops and
unproven returns remain refused.

Every reachable control-flow exit from every member must be either a proven
`bi $r0` return or one of the unit's recorded tail edges. This includes both
paths of conditional branches. An exit to a non-member, an undecoded gap, or
past the decoded image is refused with its source address; infinite cycles
inside the unit are allowed. At least one reachable member return must be
proven, using only the already admitted `"spu-abi-link-slot"` assumption.

The unit must have no known foreign call or branch (direct or statically
resolved indirect) into any address in the unit except at `E`. This also
checks sequential fall-through and return continuations after `brsl`, `brasl`,
`bisl` or `bisled`. No independent entry point may land inside a member other
than `E`; an unresolved indirect branch outside the unit also blocks admission
because its target could enter a member. A refusal names the foreign source
edge, unresolved site, or entry point. These checks establish the control-flow
invariant needed for replacement: all known entries enter at `E`, and every
non-return exit stays within the admitted closure until a proven return.
Cycles among members add no escape from the unit. All lifted member functions
remain emitted and registered, and references within the unit continue to
target those lifted functions, so the T1 reference used for A/B stays wholly
lifted while external references to `E` select the native function.


The raw-image manifest carries `overrides` as a list of `ADDR=SYMBOL` strings
and a nonzero `native_rev`; the pipeline validates and canonicalizes them into
`lift_options`, then the workload registry records `SPU_TIER_PARTIAL` and that
revision (`tools/build_spu_workloads.py`; `runtime/spu/spu_workload.c`).

For the pilot, whole-job replay remains strict bit-exact; the per-function
unit masks only proven-dead registers and the function's dead stack extent.
The unit capture/replay commands and their format are in
[`SPU_XDIFF.md`](SPU_XDIFF.md), sections 7.4 and 8.2.


## Liveness (what a T2 override may rely on)

A native override of one function (tier T2, `docs/SPU_XDIFF.md` sections 8.2
and 17 question 10) may leave registers and stack bytes different from the
lifted function's if nobody reads them. `tools/spu_liveness.py` says which, and
the offline runner's poison run checks what it says.

```
python3 tools/spu_liveness.py IMAGE.bin --base 0x7C00 [--code-end N] [--entry N]... [--functions FILE] [--link-base N] -o live.json
python3 tools/spu_liveness.py --elf IMAGE.elf -o live.json          # the ELF's code, bounds and entry
```

The functions are the lifter's: `find_spu_functions.py`'s `raw=True` bounds,
split at each `--entry` and capped at `--code-end` exactly as `spu_lifter.py`
does (`--functions` takes the lifter's own list, `--elf` the
`--auto-functions` one). The output is `ps3recomp.spu-liveness/1`: the image
(`sha256`, `fingerprint` in the registry's FNV form, `ls_base`, `bytes`,
`code_end`) and, per function:

| Member | Meaning |
|---|---|
| `entry`, `end` | the bound, LS addresses as hex strings |
| `exits` | the `bi $r0` (and conditional `biz`..`bihnz $r0`) returns |
| `tail_calls`, `tail_callers` | the other bounds it branches into; the branch sites that reach it |
| `callers` | the `brsl`/`brasl` sites that call it |
| `live_out` | registers some caller may read after the call before writing them; `$r1` always |
| `dead_regs` | `$r0..$r127` minus `$r1` minus `live_out` |
| `dead_stack` | `{"below_sp": n}`: the bytes `[entry_sp - n, entry_sp)` are dead at every return; or `null` |
| `unknown_callers`, `unknown_reasons` | `true` makes `live_out` every register and `dead_regs` empty |

**Model.** No register is assumed dead from the ABI; only what the analysis
proves is claimed, and every doubt makes a register live. The analysis is a
backward dataflow over every instruction of every bound, with exact defs and
uses (an instruction the tool does not know reads every register and kills
none; a test pins that every mnemonic the disassembler prints has a rule).
A call to a function entry E is live-in of E minus the link register, plus
live-in after the call: a callee never kills anything but the link. A call to
anything else, an indirect call (`bisl`), a branch whose target the tool
cannot see (`bi $rN`, a computed `bi $r0` as `spu_lifter.compute_bi_r0_jumps`
classifies it, `iret`, `bisled`, a target outside the image) and a resuming
`stop` (`find_spu_functions.LV2_RESUMING_STOPS`) read every register. A
terminating `stop` reads nothing: the program ends there. `live_out` of a
function is the union, over its call sites, of live-in after the call, plus
`live_out` of every bound that branches or falls into it (a tail call returns
to the caller's callers). Recursion and mutual calls are solved to a fixed
point.

An entry has unknown callers, and every register live-out, when an
`--entry` reaches it, no direct call or branch targets it, its address is
materialised or stored in an aligned word, a call lands inside it, or it is a
call continuation. In addition, any unresolved indirect call/branch leaves
its target set unknown: every function entry could be reached, so each gets
`unresolved-indirect-caller` and no dead-register claims. This covers arithmetic
offset tables and halfword tables the analyzer does not resolve. `--entry`
adds missing function bounds but does not narrow that possible target set.
An image linked at 0 and staged at `--base` forms its addresses image-relative:
`--link-base 0` makes every immediate and data word also name
`value - link_base + base` (the same reading as the raw manifest's `link_base`,
below), so a function whose image-relative address is formed or stored counts
as address-taken. The detected bounds follow it too. `--link-base` must lie
in `[0, --base]`; the output's `image` then carries `link_base`.

`dead_stack` needs a forward analysis of `$r1` to prove a frame: the function
lowers `$r1` only with `ai $r1,$r1,imm`, reaches no indirect branch, no
paths disagree on the offset, every return and every branch out of it finds
`$r1` at its entry value, and `n` is the deepest the pointer goes. Calls are
assumed to return with `$r1` unchanged. A function with no frame, an
unbalanced one, a mid-function entry or an indirect branch claims none. The
claim is the function's own frame, not everything below the stack pointer
(its callees' frames are dead too and are not claimed).

**The poison run** (`spu_job_replay --tier interp --poison live.json FIXTURE`,
`docs/SPU_XDIFF.md` section 8.2) checks the claims on one fixture. T0 replays
it, then replays it again and, at every return of a listed function,
overwrites what the plan calls dead. What is poisoned, and when:

- *A call* is a `brsl`, `brasl` or `bisl` whose target is a listed entry; it
  records the link and `$r1`. *A return* is the first instruction executed
  after a `bi`/`biz`/`binz`/`bihz`/`bihnz` on `$r0` whose address is that link.
  Poison goes in before that instruction runs, so the return has already read
  `$r0`. A function entered by a jump or by the loader (no call seen) is never
  poisoned.
- Registers get four words of `0xDEADBEEF`; `$r1` never does and a plan that
  lists it is refused. `$r0` may be poisoned, after the return consumed it.
- The frame gets `0xA5` at `sp - k` for `k` in `1..n`, `sp` being `$r1` at the
  call. A frame whose return finds `$r1` elsewhere, or that does not fit in
  local store, makes the claim false: the claim said the frame is the `n`
  bytes below the entry stack pointer.
- Poison nobody read stays in the end state; registers and bytes still holding
  the pattern take the fixture's value before the end states are compared, so
  only state the poison reached elsewhere differs.

A run that differs in a transcript record or in the remaining state, or whose
frame claim fails, is `differ` / `poison`. The runner then narrows it with more
replays: the first function (by entry) whose claims fail alone, the lowest dead
register of it that fails alone, else the frame byte (the lowest one that
matters; `exact` when poisoning that byte alone fails). The result names them;
`underlying` is the reason the narrowest failing run would have had. A plan
computed for another image (`fingerprint`) is `malformed` / `fixture-identity`.
`equal` says the claims held on this fixture, for the returns the run poisoned
(`poison.poisoned`, zero for a plan whose functions this job never calls); it
is no proof for another input.

`tools/test_spu_liveness.py` pins the analysis on hand-encoded images;
`runtime/spu/tests/test_spu_xdiff_poison.c` (`spu_xdiff_poison`) records a
caller and callee in four variants, takes the claims from the tool, and checks
that they hold, that a deliberately wrong claim is found and named, and that a
claim the analysis cannot see (the caller reading the callee's frame) is caught
by the poison run.

## Relationship to the PPU fallback approach (read `SPU_FALLBACK.md` first)

The project's **current** SPU strategy is *not* execution — it is the
PPU-side fallback registry (`SPU_FALLBACK.md`): per game, you hand-write a C
function that reproduces each SPU job's output, keyed on the SPU image entry
point. That is pragmatic and gets specific games running fast, but it is
**manual and per-job** — every game re-pays the cost, and jobs whose logic is
non-trivial (decompressors, mixers, physics) are hard to reimplement by hand.

This lifter is the **general, automated** path: translate the SPU ISA once and
run the real job. The two are complementary, and they can coexist:

- Use a **fallback** when the job is simple to reproduce or when the lifted
  output isn't trusted yet.
- Use the **lifted SPU function** when the job is complex or unknown.

A clean integration: have `spu_indirect_branch` / the group-start dispatcher
first consult `spu_lookup_ppu_fallback(entry_point)`, and only fall through to
the lifted `spu_func_*` (via `spu_function_table[]`) when no fallback is
registered. That makes lifting an opt-in upgrade over the existing shim
mechanism rather than a replacement.

### Implemented instructions (Phase 1)

- **Immediates:** il, ila, ilh, ilhu, iohl
- **Integer:** a, sf, ah, sfh (+ ai/ahi/sfi/sfhi); mpy/mpyu/mpyi
- **Logic:** and/or/xor/nand/nor/andc/orc (+ andi/ori/xori)
- **Compare:** ceq/ceqh/ceqb, cgt/cgth/cgtb, clgt/clgth/clgtb (+ ceqi/cgti/clgti)
- **clz, cntb**
- **Select / shuffle:** selb, shufb (faithful, incl. the 0x80/0xC0/0xE0
  selector special cases)
- **Shift/rotate immediate:** shli, shlhi, roti, rothi, rotmi, rotmai, rotmhi,
  shlqbyi, rotqbyi
- **Float (single):** fa, fs, fm, fma, fms, fnms, fceq, fcgt
- **Memory:** lqd, stqd, lqa, stqa, lqx, stqx
- **Branches:** br, bra, brsl, brasl (link reg recovered from raw encoding),
  brz/brnz/brhz/brhnz (internal → `goto`, cross-function → trampoline call),
  bi, bisl, biz/binz/bihz/bihnz (→ `spu_indirect_branch`)
- **stop, nop, lnop, sync, dsync, hbr/hbra/hbrr** (hints dropped)

## TODO — to actually run SPU code

### 1. Runtime glue — **done** (`runtime/spu/spu_channels.c`)

Implements the externs the lifter declares:
- `spu_wrch` / `spu_rdch` / `spu_rchcnt` route MFC channels (`MFC_LSA`,
  `MFC_EAL/EAH`, `MFC_Size`, `MFC_TagID`, `MFC_Cmd`, `MFC_WrTagMask`,
  `MFC_WrTagUpdate`, `MFC_RdTagStat`, `MFC_RdAtomicStat`, ...) through
  `mfc_channel_write`/`mfc_channel_read` from `spu_dma.h`, and mailbox /
  signal / event / decrementer / SRR0 channels through the
  `spu_channel_*` helpers + `spu_context` fields.
- Each `spu_context` embeds its own `mfc_engine`; `mfc_for()` returns that
  storage directly. The optional `SPU_PCSAMPLE` list is diagnostic-only and
  its lock is not part of MFC channel lookup.
- `spu_indirect_branch` resolves `ctx->pc` via a flat address-keyed
  registry of `spu_func_*`. The lifter now emits a `spu_recomp_register()`
  that populates the registry from `spu_function_table[]` — call it once
  at program init.
- Tiny ABI shortcut for the standard SPU calling convention: the lifter
  translates `bi $r0` (return through the link register) directly to a
  host `return;`, so SPU call/return nests as normal C calls.

#### Validation
End-to-end test (two functions; A calls B via `brsl`, B writes outbound
mailbox via `wrch`, returns via `bi $r0`; test main pre-fills inbound
mailbox then reads it via `rdch`):

```
gcc -std=c11 -O2 spu_recomp.c runtime/spu/spu_channels.c test_main.c
```

All seven checks pass (link saved correctly, `wrch` populates
`ctx.ch_out_mbox` with the right value+count, `rdch` consumes
`ctx.ch_in_mbox`, function return restores execution to A).

### 2. Missing instructions (extend `_translate` + add a helper)

Real-image coverage measured against an SPU ELF extracted from Uncharted:
Drake's Fortune (BCUS98103, EBOOT.elf @ 0x845000, 36,608 bytes of code,
~9,150 instructions). **After the Phase 2 pass below: 0 lifter gaps remain
for instructions the disassembler decodes.** The only remaining "unsupported"
entries (683) are `.word` — i.e., bytes the disassembler couldn't decode at
all. That is now §2b's problem, not the lifter's.

Closed in Phase 2:

| Group | Count closed | How |
|---|---|---|
| Variable-count shifts/rotates (`shl`, `shlh`, `rot`, `roth`, `shlqbi`, `rotqbi`, `shlqby`, `rotqby`, `shlqbybi`, `rotqbybi`) | ~590 | Register-variable mirrors of the immediate forms. |
| `rotmahi` | 203 | 16-bit twin of `rotmai`. |
| Immediate compares (`ceqbi`, `ceqhi`, `clgtbi`, `clgthi`) | 69 | Byte/halfword imm twins of the word forms. |
| FP status / estimates (`fscrrd`, `fcmeq`, `fcmgt`, `frsqest`) | 41 | `fscrrd` stubs to 0 (no FPSCR model); the rest implemented IEEE-style. |
| Misc (`gb`, `gbh`, `cg`, `addx`, `mpyh`, `mpyhh`, `mpys`, `mpyui`) | ~30 | One-offs. |
| `shlqbii`, `rotqbii` (quad bit shift/rotate, immediate) | 69 | Found during the build pass — disassembler maps these as 3-reg RR, lifter strips the spurious `$r` prefix to recover the i7 value. |

The "RI7 emitted as RR" wart is a disassembler quirk (§2b territory) handled
defensively in the lifter for now.

Phase 3 — **done**. All mnemonics that the disassembler emits now have
helpers. Verified on the Uncharted SPU image:

- d-form constant generators (`cbd`/`chd`/`cwd`/`cdd`) → 16-byte insertion
  shuffle pattern via `spu_c{b,h,w,d}d_pos()` helpers.
- Indexed constant generators (`cbx`/`chx`/`cwx`/`cdx`) → same patterns,
  position derived from `(ra+rb) & 0xF`.
- Form-select-mask (`fsmbi`/`fsm`/`fsmh`/`fsmb`) → bit→lane expansion.
- Rotate-and-mask family (`rotm`/`rotma`/`rothm`/`rothma`/`rothmi`/`rotqmbi`/
  `rotqmby`/`rotqmbybi`/`rotqmbii`/`rotqmbyi`) → right-shift fills.
- Sign-extension (`xsbh`/`xshw`/`xswd`).
- Halfword/byte immediate logic (`andhi`/`andbi`/`orhi`/`orbi`/`xorhi`).
- OR-across (`orx`).
- `bgx` (borrow-generate extended) — reads `rt` as carry-in like `addx`.
- Halt-conditionals (`hgti`/`hlgti`/`heqi`/`hgt`/`hlgt`/`heq`) → compare the
  preferred word slots and, if the condition holds, print a `HALT-ASSERT`
  line to stderr, set the context status to stopped-by-halt and call
  `spu_halt()` (see "Known stubs" for the details).
- `stopd` → same as `stop`.
- `lqr`/`stqr` → LS load/store at PC-relative target (same as `lqa`/`stqa`).
- `mfspr` → stub (returns 0; SPU SPRs are not modeled).

Result on the Uncharted SPU image after Phase 3:
- **0 lifter gaps remain** for any instruction the disassembler decodes.
- The 1,092 `.word` entries are bytes in the executable segment that are
  not SPU instructions (data tables, padding placed in `.text` by the
  linker). Eliminating them is §4 work — split text from data using the
  ELF section table and function-boundary detection.

Mnemonics once listed here as not yet hit (`fi`, `frest`, `frsqest`, `fcmeq`,
`fcmgt`, `cuflt`/`csflt`/`cflts`/`cfltu`, `fesd`/`frds`, `sfx`/`cg`/`bg`/`cgx`,
`mpyhha`, `gbb`, the double-precision ops) are also handled now. The only
instructions the lifter still does not translate fall through to
`spu_unsupported()` and show up in the coverage summary. See "Known stubs"
below for what is accepted but not modeled.

### 2b. Disassembler — **rewritten against the RPCS3 authoritative table**
The original `spu_disasm.py` had systemic errors: `IL`/`ILH`/`ILHU` listed as
RI18 instead of RI16; `BR`/`BRSL`/`BRA`/`BRASL` likewise; the entire
shift/rotate immediate group (`shli`, `shlhi`, `roti`, `rothi`, `rotmi`,
`rotmai`, `rotmhi`, `rotmahi`, `shlqbii`, `rotqbii`, `shlqbyi`, `rotqbyi`)
sitting at RI10 opcodes that are actually `brz/brnz` prefixes; `lqa`/`stqa`
at the wrong RI16 opcodes; `hbra` occupying `iohl`'s position; etc.
Result on real SPU code: real instructions silently mis-decoded as something
else (`rot` → `shl`, `stqa` → `shlhi`), and the lifter ran *wrong* but
*compilable* C.

Fixed by replacing `RI18_TABLE`, `RI16_TABLE`, `RI10_TABLE`, and `SPU_RR`
with values verified against the SPU decoder table in RPCS3's
`Emu/Cell/SPUOpcodes.h` (`{magn, value, handler}` entries). All RI7
shift/rotate forms now live in `SPU_RR` at their real 11-bit opcodes
(0x078–0x07F and 0x1F8–0x1FF). Operand formatters updated.

After the fix, on the Uncharted SPU image:
- Call targets detected: **13 → 47** — the call graph is now visible.
- 36 new real-SPU mnemonics appear (`cwd`, `fsmbi`, `xsbh`, `rotqmby`,
  `fsm`, `cbd`/`chd`/`cwd`/`cdd`, `hlgti`, `heqi`, `rothmi`, `xorhi`,
  `andhi`, `orhi`, `mfspr`, `xshw`/`xsbh`/`xswd`, etc.). Previously these
  bytes were mis-decoded as something the lifter handled, hiding the gap.
- `.word` count rose (683 → 1092) because the new decoder honestly reports
  bytes it can't identify (data, jump tables embedded in the executable
  segment) instead of matching them to bogus tables.

The original tables are kept as `tools/spu_disasm.py.bak` for reference.

### 3. Correctness items — partially **done**

Refactor + unit tests (`runtime/spu/spu_helpers.h`,
`runtime/spu/tests/test_spu_helpers.c`):

- All semantics helpers extracted from `spu_lifter.py` into the shared
  header — single source of truth for the lifter, the runtime, and tests.
- 63-case unit suite covering: immediate loaders, SIMD arithmetic, multiply
  family, compares, shift/rotate boundaries (sh=0 and sh≥width), shufb
  (incl. the 0x80/0xC0/0xE0 special selectors), selb, constant generators
  (cwd/cbd/chd/cdd/_pos), fsmbi, sign-extension, fused FP multiply.
- Build + run: `gcc -std=c11 -O2 -Wall -Wextra test_spu_helpers.c`; exit
  code = 0 if all pass. Currently **63 passed / 0 failed**.

**Endianness bug found and fixed**: `mpy`/`mpyh`/`mpyhh`/`mpyu`/`mpys`/`mpyi`/
`mpyui` and `xsbh`/`xshw`/`xswd` were accessing sub-lanes via `_s16[2i+1]` /
`_u8[2i+1]` etc., assuming big-endian element order inside `u128`. On the
LE host the recompiler targets this picks the *opposite* sub-lane.
Symptom: silent wrong results for any code using these mnemonics (~16
appearances in the Uncharted SPU image). Fix: invert the sub-lane index on
LE host (`_s16[2i]` etc.). Documented in `spu_helpers.h` with comments
explaining the host-endianness assumption.

Still open:
- `shlqby` direction and the preferred-halfword choice for `brhz`/`brhnz`
  (which `_u16[]` lane is the "halfword preferred slot" in BE semantics
  reinterpreted on LE storage). Both are exercisable by adding test
  vectors; the suite framework now makes it cheap.

### 4. Pipeline integration
- **SPU program discovery — done** (`tools/extract_spu_images.py`). Scans a
  PPU ELF / EBOOT for embedded ELFs with `EM_SPU=23`, computes each image's
  byte-size from its program/section headers, and writes them out. Verified
  on Uncharted: Drake's Fortune (1 image extracted, 39,608 B, code segment
  at va 0x3000).
- **Function-boundary detection for SPU images — done**
  (`tools/find_spu_functions.py`). Seeds from ELF entry point + STT_FUNC
  symbol-table entries + every `brsl`/`brasl` target the disassembler
  resolves. Walks each start forward until `stop`/`stopd`, `bi $r0` (SPU
  ABI return), an unconditional branch with no fallthrough, or the next
  seed. Symbol entries with non-zero `st_size` give exact boundaries
  directly. Emits the `--functions` JSON the lifter already consumes; or
  the lifter can take an ELF directly via `--auto-functions <elf>` and
  run the detector in-process.

  Verified on the Uncharted SPU image (no symbol table — fully stripped):
  **461 functions** detected from 47 `brsl`/`brasl` + other branch targets
  + entry. (Seeding only from calls was the first cut and gave 44; running
  an exploratory harness against the lifted output revealed orphan symbols
  for branch destinations the detector had not promoted to functions —
  `collect_branch_targets` was widened to include `br`/`bra`/`brz`/`brnz`/
  `brhz`/`brhnz` and the count jumped 10×.) Re-lifting with these
  boundaries: 461 functions emitted, only 4 `.word` remaining (the actual
  data tail). Build clean, and the entry function `spu_func_000030F0`
  executes to a natural return in the harness — zero indirect-to-unknown
  branches, zero crashes, zero channel I/O (the bare entry happens to be
  pure setup with no PPU dependency).

  Lifter follow-up: the lifter emits direct `spu_func_X(ctx)` calls for
  every branch target the disassembler resolves, including cross-function
  branches. emit_source now generates a trivial stub for any target the
  detector did NOT promote to a real function — the stub routes through
  `spu_indirect_branch`, which finds the function if registered or halts
  with a clean diagnostic. Without the stubs, those calls were unresolved
  externs and the link failed.

  Harness follow-up (a private test harness, not in this repo): halts
  from `spu_indirect_branch` were not actually terminal — the C call
  returned into its caller's fallthrough, which often re-entered the
  same bad indirect (timeout loop). The harness now `setjmp`s in `main`
  and `longjmp`s from inside the halt path, so a HALT really stops the
  SPU instead of unwinding a stack frame at a time.
- **CMake integration - done.** `cmake/PS3RecompSPU.cmake` provides
  `ps3recomp_generate_spu()`, and `templates/project/CMakeLists.txt` calls it
  when `PORT_ENABLE_SPU` is on and `PORT_SPU_INPUTS` is set (see "Discovery and
  registration coverage" below).

## Known stubs

Instructions the lifter accepts but does not fully model:

- **Halt conditionals** (`hgt`, `hgti`, `heq`, `heqi`, `hlgt`, `hlgti`): not
  no-ops. The lifter emits `if (cond) { fprintf(stderr, "[spu] HALT-ASSERT ...");
  ctx->status = SPU_STATUS_STOPPED_BY_HALT; spu_halt(ctx); return; }`. The
  compare uses word slot 0 of `ra` (and `rb` or the immediate). A taken halt
  stops the SPU through `spu_halt()`; a not-taken one falls through
  (`tools/spu_lifter.py`).
- **`fscrrd`** returns zero (`spu_fscrrd` in `runtime/spu/spu_helpers.h`).
  **`fscrwr`** emits a no-op comment. There is no FPSCR model.
- **`mfspr`** returns zero (`spu_mfspr` in `runtime/spu/spu_helpers.h`);
  **`mtspr`** emits a no-op comment.
- **Double precision** (`dfa`, `dfs`, `dfm`, `dfma`, `dfms`, `dfnma`, `dfnms`,
  `fesd`, `frds`) is lifted to helpers in `runtime/spu/spu_helpers.h`; it is not
  a stub.
- **`dfceq`, `dfcmeq`, `dfcgt`, `dfcmgt`, `dftsv`** are not lifted at all. A
  retail CBE SPU raises an invalid-instruction error for each (hardware probe
  `spuexh` special A, CECHC04 with 4.81 REBUG: the thread group ends with join
  cause 4 and the stopped thread's exit status reads `0x8001000F`; issue #124),
  so the interpreter decodes them as `.word` and the lifter emits the same stop
  at the word's own PC instead of a helper call.
- **`cflts`/`cfltu` at I8 <= 14** lift with a per-lane stop. With
  `sh = be + 23 - I8`, a lane with `sh >= 264` (`be >= 241 + I8`, reachable only
  at these scales) takes the conversion past the hardware's saturating path into
  a region no fit models. Every lane with `sh < 264` follows the rule in
  `spu__cfl_lane`, and the `cfltu` lanes at `sh == 264` and every negative
  `cfltu` lane were measured as well (hardware probe `spuexh` lanes, CECHC04
  with 4.81 REBUG, 129024 lanes; issue #126). Which lane reaches the path
  depends on the data, so the lifter emits the conversion behind a runtime check
  (`spu_cfl_unmodelled_lane`, the interpreter's own predicate): a lane that is
  not covered stops the run at the word's own PC with
  `unmodelled cflts/cfltu lane (op I8=N lane=L in=0x........, #126)`, and no
  result is written. I8 >= 15 cannot reach the path (largest `sh` 263), is exact
  over the whole input space and lifts to the bare helper call with no check.

## Tracing (for §3 diff vs RPCS3)

The lifter now accepts `--trace`. When set, every emitted instruction is
wrapped:

```c
spu_trace_pc(ctx, 0x<PC>);
<lifted instruction>
spu_trace_rt(ctx, <rt>);   // only for instructions that write rt
```

Runtime helpers live in `runtime/spu/spu_channels.c`:
- `spu_trace_init(path)` — `NULL`/empty → stderr; otherwise write to file.
- `spu_trace_pc(ctx, pc)` — one line per executed instruction: `<PC-5hex>`.
- `spu_trace_rt(ctx, rt)` — destination post-state: `  r<rt> <hi64> <lo64>`.

On the RPCS3 side, the equivalent capture is:
- `Core.SPU Decoder = Interpreter (static)` (the recompilers don't emit
  per-instruction trace).
- `Log.SPU = Trace`.
- Boot the SPU ELF directly via CLI: `rpcs3.exe <spu>.elf`. RPCS3 recognises
  standalone SPU ELFs and creates a single SPU thread for the image, avoiding
  the noise of booting a full title.

The two traces line up by PC; a tiny converter (still to be written) can
normalise RPCS3's `SPU.log` lines into the same `<PC>` / `  r<rt> ...` shape
this runtime emits, so plain `diff` finds divergences.

## How to validate

Status: **compile- and run-verified** with GCC 16.1.0 (MinGW-w64, UCRT).
A 7-instruction arithmetic function (il/a/ai/sf/and/cgt) was lifted, compiled
(`gcc -std=c11 -O2 spu_recomp.c test_main.c`) and executed — all register
results matched expected values (incl. `sf` = -2 and `cgt` = 0). Generate
output into a dir two levels under the repo root (e.g. `gen/spu/`) so the
header's relative `#include "../../runtime/spu/spu_context.h"` resolves, then:

```
gcc -std=c11 -Wall -c gen/spu/spu_recomp.c      # compile check
```

Still to do:
1. Per-helper unit tests (`spu_shufb`, compares, rotates) against expected
   vectors from the Cell BE ISA manual — especially the byte-shuffle special
   cases and the shift-direction questions in §3.
2. Diff execution against RPCS3 SPU traces for a real program (e.g. a SPURS
   job from the target game) once SPU images are extracted from the EBOOT.

### Integration tests (runtime/spu/tests/)
End-to-end tests that exercise the **whole pipeline** (encoder → wrap_spu_elf
→ find_spu_functions → spu_lifter --auto-functions → gcc → harness) on
hand-written SPU programs with known input/output. Per-test layout:
`gen_test_<name>.py` (encoder + ELF wrap) + `test_<name>_main.c` (harness
with channel-overriding stubs and asserts). `run_tests.sh` drives the
suite.

Current state (4 tests):

| Test | What it exercises | Result |
|---|---|---|
| `sum` | loop with `rdch` × N, accumulator, `brnz`, `wrch` | PASS |
| `shufb` | shufb special selectors (`0xC0`/`0xE0` magic bytes) | PASS |
| `brsl_return` | function call via `brsl r0`, return via `bi $r0` | PASS |
| `dma` | full MFC DMA round-trip (LSA/EAH/EAL/Size/TagID/Cmd channels, GET into LS, `lqd`, `wrch` out) | PASS |

The original DMA failure was the §3 endianness gap surfacing: on an LE
host the helpers store register lanes in native order so that `_u32[i]`
gives lane i directly, but `spu_ls_read128`/`write128` were doing raw
`memcpy` of LS bytes (which are big-endian, native to the SPU). Loading
a quadword from LS therefore produced byte-swapped lanes.

Fixed by making the two quadword LS helpers byte-swap per 4-byte lane,
mirroring what `spu_ls_read32` already does. The same approach keeps the
helper library untouched (every helper continued to work without
modification) and only adds work at the LS-to-register boundary — i.e.
the same line as the existing `spu_ls_read32` convention.

The alternative — extracting "preferred slot" via explicit BE byte
assembly in every channel consumer — would have required touching
`spu_rdch`, `spu_wrch`, and every harness, plus rewriting `shufb` to
swap indices on LS-loaded data. The per-lane swap in LS access is
strictly less invasive and is the model the helper authors had already
committed to.

## Discovery and registration coverage

The existing workload builder can connect discovery, lifting, and registration
for every SPU ELF candidate in explicitly supplied files:

```bash
python tools/build_spu_workloads.py --input /path/to/EBOOT.ELF \
    --input /path/to/decrypted_module.elf --lifted /path/to/generated_spu \
    --out /path/to/generated_spu/spu_workloads.c --constructor
```

See [the project template](../templates/project/README.md#automatically-discover-and-register-spu-elf-programs)
for automatic CMake integration, output ownership, coverage failures, partial
mode, and limitations. A `registered` status confirms generation and table
coverage; it does not establish correct execution of the program. The standalone
lifter's optional `--report PATH` emits function ranges, unsupported instruction
counts, and direct branch/call targets for tools consuming its output.

### Raw local-store job images

A JobQueue2 job image is not an ELF. The PPU copies `sizeBinary << 4` bytes of
guest memory (the descriptor's `eaBinary`) straight into local store, and the
runtime stages that copy at LS 0x7C00 (`SPU_JQ2_IMAGE_LS` in
`runtime/spu/spu_workload.h`; see `runtime/spu/spurs_job.c`). Such a blob has no
program headers, entry field or symbols, so the pipeline needs a manifest for
the facts the bytes cannot carry:

```bash
python tools/build_spu_workloads.py --raw-images /path/to/raw_images.json \
    --lifted /path/to/generated_spu/raw --out /path/to/generated_spu/raw/spu_workloads.c
```

`--raw-images` takes a non-empty JSON list; relative `path` values resolve
against the manifest's directory:

```json
[
  {
    "path": "images/audio-job.bin",
    "sha256": "0123…(64 hex)…cdef",
    "ls_base": "0x7C00",
    "link_base": "0x0",
    "code_end": "0x16168",
    "extra_entry_points": ["0xA2C"],
    "return_entries": true,
    "promoted": true
  }
]
```

- `sha256` is checked against the blob before anything is lifted. A mismatch is
  a named error and fails the run, `--allow-partial` included: these are the
  bytes the registration key will be computed from, so accepting a different
  file would silently register the wrong image.
- `ls_base` is the local-store address the image is staged at (0x7C00 for a
  JobQueue2 job, 0 for some other loaders). It must be 4-byte aligned and keep
  the whole blob inside the 256 KB local store.
- `code_end` is optional and is passed unchanged to the lifter's existing
  `--code-end`: it drops function bounds at or past the address and caps the
  last one, leaving an embedded rodata/const tail undisassembled. It must be a
  4-byte-aligned address inside `(ls_base, ls_base + len(blob)]`; an address past
  the blob is a named error.
- `link_base` is optional (absent: every immediate is an absolute LS address,
  as before). It is for a **position-independent** job image: one linked at
  `link_base` (0 for a JobQueue2 job) and staged at `ls_base`, whose code adds
  the load delta `ls_base - link_base` (a `brsl $rN,.+4` PC-getter, the crt's
  delta in `$r126`) to image-relative immediates: `ila $r14,0x100` ...
  `a $r10,$r14,$r126` is a jump table at LS 0x7D00, whose words are
  table-relative. The toolkit's readers take an immediate as an absolute LS
  address, so such an image's jump-table case bodies and function-pointer
  targets are not function starts: they are never lifted, and a lifted job that
  reaches one interprets silently in production and is `void/unlifted-branch`
  in a shadow. With `link_base` set, an immediate or data word that can name a
  code or data address is read **both** as written and as `imm - link_base +
  ls_base`; nothing local says which one a given `ila` means, so neither is
  dropped. It must be an integer, 16-byte aligned, non-negative and at most
  `ls_base`, or it is a named error; `null` is "absent". `extra_entry_points`
  and `code_end` stay LS addresses.
  What it changes, only for an image that sets it:
  - The detector (`find_spu_functions.py`, `--link-base`) seeds function starts
    from the image-relative reading too: jump-table bases (table-relative
    entries resolve against the relocated base, so the case bodies appear),
    `ila`/`il`/`ilhu`+`iohl` continuations, `$r0` and branch-register
    immediates, and function-pointer words in data. An extra start only lifts
    more, but a start that is data would be refused by the lifter, so an
    image-relative seed is kept only where it begins a run that decodes up to
    its first terminator: the job header's marker text, a jump table or the
    data tail are not seeded. Expect extra starts that are not real functions
    (words that decode, in the header or in data).
  - The static effect inventory counts the same readings as address-taken. The
    MFC resolver's rule is that an entry's address is never an immediate or a
    data word; with `link_base` that also means `address - delta`, so an entry
    that code reaches through `ila` plus the load delta is no longer resolved
    through its call sites. The extra readings only refuse a resolution or
    shorten a straight-line run, never make one: over the same function list
    the sites left unresolved with `link_base` include every site left
    unresolved without it (`tools/test_spu_program_pipeline.py`
    `test_link_base_never_adds_a_resolution`). The MFC effect resolver still
    treats table-relative tables as `no-unmodelled-indirect-entry`; it does
    not rely on the lifter's separate register-branch analysis.
  - The lifter resolves an in-image `lqx`/`rotqby` table only when it can
    derive the table address, branch target expression and index domain.
    `GUARANTEED` additionally requires every reachable index to be bounded,
    every target to be a detected function entry, and no possible store or
    MFC GET to overlap the table. Bounds come from a finite value set, a mask,
    or a `clgti`/`brnz` escape that cannot re-enter the dispatch. The branch
    must test that comparison's destination, and the selector must remain
    unchanged from comparison through dispatch. A selector overwritten after
    scaling cannot bound its earlier value. An unbounded
    index, unproven control flow or possible writer yields `BOUNDED`: the
    generated branch checks its word-aligned 18-bit target and stops with
    `spu-indirect-target-set` if it leaves the enumerated candidates. A
    `BOUNDED` site remains unresolved for native-override admission.
    A mismatch stops before interrupt-enable or PC writes. `bisl` validates
    before writing its link. `bisled` writes its link before the acquire event
    sample, matching the interpreter; a taken mismatch restores the old link
    before stopping, retaining the pre-branch state in the stop snapshot.
    The `GUARANTEED` claim is sound because every reachable selector value
    chooses an enumerated in-image word, all such words name registered
    entries, and the writer scan found no store that can change those bytes.
    `BOUNDED` guards detect an escape but do not prove the table immutable.
  - The lift's options carry `link_base` (below), because the same bytes lift
    to a different body.
  - `tools/spu_liveness.py --link-base` reads the image the same way.
- `extra_entry_points` are optional LS addresses passed to `--extra-funcs`, for
  entries the branch analysis cannot see (interrupt vectors, jump-table
  handlers). They must be 4-byte aligned and below `code_end` when that is set.
- `return_entries` is an optional boolean (default `false`) passed to the
  lifter's `--return-entries`, which makes every call's return address an entry
  point of its own. Code that is *resumed* rather than returned to needs it:
  setjmp/longjmp-style coroutines in a job abandon the lifted call's host frame,
  so the return site is reached through dispatch later.
- `promoted` is an optional boolean (default `false`) copied into the image's
  program record; it changes no lift. It is the port's statement that the
  evidence of `docs/SPU_XDIFF.md` section 12.4 is held for this image, and the
  generated registry marks it promoted (under `spu.job_body=lifted` only
  promoted images run their lifted body).

Both booleans are strict: a JSON number, string or null is a named manifest
error, never an implicit true. They are copied to the program record, so a
reader of `inventory.json` sees what the port decided about the image.

The registration fingerprint is the FNV-1a-64 over the **whole blob** — which is
exactly the staged bytes and therefore exactly what
`spu_workload_fingerprint(image, image_bytes)` hashes at run time. It is not the
ELF-extent rule (`spu_elf_image_size`) and there is no separate text-segment
alias: a raw job is dispatched by those bytes and nothing else. The pipeline
tests compile the runtime's fingerprint function against the generated blob and
assert the C and Python values agree.

Each program record also carries the lift's options, as
`lift_options_string` (the canonical form) and `lift_options` (its hash). The
canonical string lists, in this fixed order, the options that change one image's
generated code: `code_end`, `link_base`, `extra` (the sorted extra entry points
the lifter received), `merge_chunks` and `return_entries`, lowercase hex,
`;`-separated, an absent option omitted (an image without `link_base` keeps the
string, and so the digest, it always had):

```text
code_end=0x16168;link_base=0x0;extra=0xa2c,0xb00;merge_chunks=1;return_entries=1
```

`lift_options` is the FNV-1a-64 of those ASCII bytes as 16 hex digits (the
empty string for a lift that passed none of the five, which still hashes to a
nonzero value, so the field never reads as "unknown lift"). It is what
`spu_xdiff_body_id.lift_options` records and what the registration carries
(`registration_inventory` formats it into `spu_workload_set_inventory`). The
lifter's `--base`, `--functions`, `--symbol-prefix`, `--report` and `--output`
are deliberately not part of it: for one image's bytes they follow from the
image itself (the manifest or the ELF, the detector, the fingerprint) or only
name paths. `promoted` is not part of it either: it records a port decision,
not a generated body.

Function discovery runs `find_spu_functions.py`'s `raw=True` mode at `ls_base`
(with `link_base` when the manifest gives one),
so every seed comes from the entry base and the branch/call analysis. The same
post-lift gates as an ELF apply unchanged: no unsupported instructions, the
entry function generated, every direct branch/call target backed by a generated
function, and generated functions inside the image. A raw image that fails one
of them is `unsupported` and is never registered silently.

### Static effect inventory

Every successfully lifted raw image carries an `effects` object in
`inventory.json` describing what the lifted function ranges contain:

```json
"effects": {
  "channels": [
    {"channel": 21, "name": "MFC_Cmd", "read": 0, "write": 4, "count": 0,
     "sites": {"read": [], "write": [31852, 31948, 32020, 32120], "count": []}}
  ],
  "mfc_commands": [
    {"command": 32, "name": "PUT", "count": 1, "sites": [32020],
     "via": "call-sites", "callers": [32100]},
    {"command": 64, "name": "GET", "count": 1, "sites": [31852]},
    {"command": 64, "name": "GET", "count": 1, "sites": [31948], "via": "function"}
  ],
  "event_branches": [],
  "unresolved": [
    {"channel": 21, "access": "write", "site": 32120, "register": "$r11",
     "reason": "command value is not a same-block immediate",
     "function": "definition at 0x7D10 (lqd) is not a constant",
     "call_sites": "not reached"}
  ]
}
```

Channel instructions are counted by channel number and by `rdch` (read),
`wrch` (write) or `rchcnt` (count). A `wrch MFC_Cmd` is resolved in three steps,
in this order, and each step runs only when the previous one did not resolve
the site:

1. **Same block** (no `via` key): the source register holds an `il` immediate,
   or an `ilhu`+`iohl` pair, from the same straight-line run. No branch, branch
   target, lifted chunk start or other register write may be in between: the
   lifter and the loader's entry list can enter a chunk start without the
   instructions before it running.
2. **Region** (`"via": "function"`): reaching values over the branch-connected
   region the site belongs to, not over one lifted chunk. A raw lift generates
   one chunk per branch target, so a procedure is many ranges; ranges joined by
   a direct branch or by a fall-through are one region (a call is not an edge —
   its callee is another region), and that graph is the procedure's control
   flow. `ori $rD, $rS, 0` and `ai $rD, $rS, 0` copy the source value with its
   provenance, so a copy of the function's argument is still the caller's
   register. A call may write any caller-visible register, so every tracked
   value is unknown at the return. A direct branch out of the region leaves it;
   `bi $r0` and the conditional `biz`/`binz`/`bihz`/`bihnz ... $r0` are ABI
   returns unless the generated code emits an indirect dispatch for that
   address. `compute_bi_r0_jumps` also accepts a saved-link restore only for
   `lqd $r0,off($r1)` when a dominating `stqd $r0,off2($r1)` stores the entry
   link before any `$r0` write on every path, and `$r1` adjustments tracked
   through `ai` prove `off + delta == off2`. Other bases, `lqx`, mismatched
   offsets, or a path that bypasses the save are computed transfers. This
   return classification rests on the named `"spu-abi-link-slot"` assumption.
   The proof follows non-linking direct branches and fall-through across
   finder-created split functions. Its product-lattice join keeps equal stack
   deltas, intersects saved-link slots, and ANDs the predicate that `$r0` still
   holds the entry link. Losing the live-register predicate cannot erase an
   independently proven saved slot; a reload needs only that slot and a
   matching delta. A call target, image/extra entry, immediate or data-word
   reference, recovered dispatch-table member or resolved indirect target
   prevents inheritance. Calls never donate their caller's frame to their
   target.
   At a call-target entry, `$r0` is a live link only when every known call edge
   supplies its continuation in `$r0`. A `brsl $r4` or resolved `bisl $r4`
   therefore cannot turn a saved/reloaded `$r0` into a return link; mixed link
   registers also fail that proof. Non-`$r0` returns remain the responsibility
   of the existing link-register classifier.

   A non-wrapping branch-and-link whose target is its own next instruction is
   a PC-getter, not a call. The shared pipeline `_pc_getter` recognizer compares
   LS-masked PCs but excludes wrapping from the last word to address zero.
   Direct `brsl $rN,.+4` and resolved `bisl` targets of that shape fall through,
   write only their link register and preserve the frame. Such an edge
   does not create a call target or return-entry root. It also does not supply a
   `spu-abi-callee-saved` assumption. A nonzero link register on a PC-getter
   cannot invalidate the still-live `$r0`.
   Emitted `bisl` reads and masks its target before writing the link, captures
   the return PC, and publishes `ctx->pc` on both paths. `_tg != _ret` is guarded
   only when the sequential next instruction is represented in the same lifted
   function: equality then falls through without a host call; inequality
   retains ordinary indirect-call/drain/image-restore semantics. At a lifted
   range end, a gap or LS wrap, even equality transfers as an ordinary indirect
   call. The static proof uses the PC-getter fall-through only when its
   target proof establishes that guarded path. An unexpected entry therefore
   cannot erase the fallback call. A `bisl` PC-getter proof in an image with
   unresolved indirect entries is not admitted for T2 overrides.

   A return-entry continuation after a known in-image `brsl` or `bisl` instead
   receives its call site's frame fact, with live-`$r0` false, under
   `"spu-abi-callee-saved"`. Both normal return and a context-restoring dispatch
   preserve the caller's `$r1` and stack frame. The call site's delta and saved
   slot must be known; another independent way into that continuation still
   vetoes the fact. Conflicting call-site/body-path deltas remain unproven.
   Identity moves are looked through, so the interrupt-window idiom
   (`ila $r0,<resume>`, `ai $r0,$r0,0`, `bid $r0`) remains an indirect dispatch
   even where this pass cannot enumerate its target set. Branch hints are
   no-ops; a non-resuming stop ends the path. The inventory uses the same
   `compute_link_returns` classification as emitted `SPU_RET_REG` for known
   non-`$r0` returns. `iret` and other unclassified register-targeted branches
   remain indirect dispatches with unknown successors.
   The step resolves only when every path arrives with one constant. Two
   constants on two paths, a non-constant definition, or a reachable indirect
   dispatch keep the site unresolved.

   A register-directed `bi`/conditional branch is **modelled exactly** when its
   target register is built only from a `brsl $rN, .+4` link constant,
   `il`/`ila` immediates, `andi` with a constant mask, constant
   `shli`/`roti`/`rotmi` shifts, and `ai`/`a` sums of those over the
   straight-line run ending at the branch — the PC-getter idiom a Duff's-device
   dispatch uses. The run is cut image-wide at every target of a branch other
   than a PC-getter, at every chunk start no reference names, and at every
   extra entry point; a PC-getter's own target is exempt, because the link it
   reads flows straight on into the run. `andi` with an unknown source is exact
   over the 32-bit domain:
   the image is the set of subsets of the mask's bits, so a mask with at most
   six bits stays within the 64-value bound. Every target must be word-aligned
   and inside the region; then the branch's successors are exactly those
   blocks, and a resolution in a region that used one carries
   `"dispatch": "masked-pc-relative"`. A register mask, a set over 64 values,
   an unaligned target or one outside the region keeps the branch opaque.

   An **entry** is a block that code outside the region can enter. Entries are
   image-wide, not a property of one lifted range: a chunk start is an entry
   when the image entry is at its address, a call (`brsl`/`brasl`) targets it,
   a direct branch from another region targets it, it is a return site, it is
   an extra entry point, its address is materialised as an immediate anywhere
   in the blob, or the address appears as an aligned 32-bit data word anywhere
   in the blob. A block with a predecessor is an entry too: a call can land on
   it, so its live atoms join the incoming state instead of being dropped.
   Every other no-predecessor block is dead code or is reachable only through
   a register-targeted transfer, which is either modelled exactly or covered
   by `assumes`.
3. **Call sites** (`"via": "call-sites"`), for a value still live-in: it is the
   register the region was entered with, and the image's own callers prove it.
   Every entry such a path came through must be provable: every reference to it
   is a direct `brsl`/`brasl` inside the lifted ranges; it is not also
   reachable indirectly (its address is not materialised as an immediate, does
   not appear as an aligned 32-bit word anywhere in the blob, and is neither
   the image entry nor an extra entry point, because an external caller could
   supply any argument); and every call site sets the register with an
   immediate in its own straight-line run — the same block-local resolver,
   applied to the caller, one level only. A caller that computes the argument,
   loads it, or received it from its own caller proves nothing, and callers'
   callers are not followed. The call's own link register never proves an
   argument: `brsl $rD, target` writes the return address into `$rD` before the
   callee runs, so the caller snapshot drops it. The proven set must be
   non-empty and agree with any constant the region's own paths produce.

Each resolved site is one entry per command, with `count` 1 and `sites`
holding the site. `callers` lists the `brsl`/`brasl` sites that prove a
`call-sites` entry; same-block entries keep their original shape (no `via`, no
`callers`), so the schema change is additive. Entries are sorted by command,
then by `via` (`""` before `"call-sites"` before `"function"`).

Everything not proven is listed under `unresolved` with its site and register
and is still included in the channel-21 write count. `reason` keeps the
block-local outcome, and `function` / `call_sites` say which later pass refused
the site and why, as data:

- `function` examples: `"two values: 0x20 at 0x7C4C, 0x40 at 0x7C44"`,
  `"definition at 0x7D10 (lqd) is not a constant"`,
  `"call at 0x7E20 clobbers $r14"`, `"opaque successor at 0x9A50: bi $r3"`,
  `"no path from a region entry reaches the site"`, `"live-in"` or
  `"live-in via $r7"`.
- `call_sites` examples: `"entry 0x7C64 is materialised by ila at 0x7C50"`,
  `"entry 0x7C64 appears as a data word at 0x94"`,
  `"caller 0x7C4C passes a computed value"`,
  `"entry 0x7C40 has no direct brsl/brasl caller"`; `"not reached"` means the
  region pass had already refused.

A field named `assumes` names an assumption a resolution rests on. A single
assumption is a string; multiple assumptions form an ordered list of
strings. Resolved entries carry `"assumes": "no-unmodelled-indirect-entry"`
when the image contains a register-targeted transfer that the region pass did
**not** model — a `bi` or conditional `biz`/`binz`/`bihz`/`bihnz` that is not
a known link return, a `$r0` target the generated code turns into a dispatch
(including one the lifter's identity-move look-through finds), a non-PC-getter
`bisl`/`bisled`, or `iret`. Such a transfer can enter a generated function at its start, a
lifted chunk start in the middle of a block, or any instruction at all (a
computed `biz` straight to the `wrch`, past the immediate the block set),
without appearing among the call sites, so the resolution assumes it does not.
Same-block entries carry the tag as well: a block-local outcome is never
presented as unconditionally proven while an unmodelled transfer could land
inside it.

When the lifter accepts an `lqd` as a saved-link restore, effect resolutions
also carry `"assumes": "spu-abi-link-slot"`: `compute_bi_r0_jumps` relies on
the caller's link being saved to the matching stack slot, using the `$r1`
adjustments it can track through `ai` (`tools/spu_lifter.py`). If both this
rule and an unmodelled transfer apply, both names are present in the ordered
list. An image whose transfers are all modelled exactly by the
`masked-pc-relative` rule (or that has none) carries no
`no-unmodelled-indirect-entry` assumption. A dispatch through an `lqx`-loaded
jump table is still opaque, so those sites stay unresolved; modelling its
recovered targets would need a named completeness assumption, and the
detector's recovered set is a heuristic. A resolution that used a modelled
dispatch says so with `"dispatch": "masked-pc-relative"`.

Inherited saved-link returns use the same completeness assumption: an opaque
register-targeted transfer elsewhere in the image could enter a split
epilogue or land after its save without appearing in the direct predecessor
graph. Each inherited return therefore records
`"no-unmodelled-indirect-entry"` when any register-target branch remains
unresolved, alongside `"spu-abi-link-slot"`. Return-entry facts also name
`"spu-abi-callee-saved"` when used. With no unresolved branch the completeness
assumption is absent. These are assumptions, not proof that unknown indirect
targets are absent; whole-job differential replay and the T0 audit remain the
empirical checks.

`"spu-abi-link-slot-private"` assumes compiled guest code never forms a pointer
to an active frame's link-save slot. Consequently a store through an argument,
loaded pointer or immediate-built address that is not a placeable `$r1`-relative
address cannot modify that slot under this assumption. A pointer to the slot
spilled to memory and reloaded violates the premise by construction: the
classifier still lowers the return, but visibly records this assumption; T2
override admission refuses it.

The assumption follows the saved value through reloads and copies into another
link slot, and is recorded on every same-function or inherited return that
uses it. A store through `$r1` with a known displacement is checked against the
slot exactly. An unplaceable stack-derived store (`stqx` with a stack-derived
base or index, or a copied/offset stack pointer) still kills the slot. Stack
derivation propagates through copies and arithmetic such as `ori`, `ai`, `a`
and `sf`, and joins by union. The privacy assumption never excuses a store
the classifier knows may be an unplaceable stack-relative overwrite.

`spu_lifter.py --report` records these decisions per site in `r0_returns`
(`site`, `load`, `inherited`, `assumes`), rejected sites and reasons in
`r0_dispatches`, and the image's `unresolved_register_branches`. The pipeline
passes the raw manifest's existing `link_base` to the lifter as `--link-base`
for this entry-veto inventory only: omitted means no relocation; an explicit
value uses `ls_base - link_base` for relative immediates and table members.
It does not change emitted addresses or the canonical `lift_options` string,
which already records the manifest field. The finder, lifter and liveness
thus use the same relocation convention, including explicit `link_base: 0`.

Resolution is deliberately conservative: a false "resolved" could hide an
atomic or mailbox access, while an unresolved site is visible and reviewable.
Command names mirror the `MFC_*_CMD` constants in
`runtime/spu/spu_context.h`.

`event_branches` names every instruction that transfers control on an SPU event
or interrupt: `bisled`, `iret`, and any `bi`, `bisl`, `biz`, `binz`, `bihz` or
`bihnz` whose E (enable) or D (disable) interrupt bit is set. Each entry is
`{"site": <LS address>, "mnemonic": <decoded name>, "enable": <E bit>,
"disable": <D bit>}`. The key is always present; an image with no such site has
`"event_branches": []`. `bisled` and `iret` are listed even with both bits
false. An image with a nonempty list cannot be shadowed or recorded faithfully
(`docs/SPU_XDIFF.md` section 12.3), so it is not eligible for xdiff; it may
still run as an authority.

`tools/gen_test_raw_job.py` builds the synthetic image these tests use: a
job-header-shaped blob with an immediate prefix, branch stub, `JOBCRT Ver14`
marker, and code carrying a GET, a PUT, a GETLLAR/PUTLLC pair and a mailbox
write, plus one command write that is deliberately not statically resolvable.
`build(..., variant=...)` additionally emits a `bisled`, a D-bit `bi` return
(`bid`) or an `iret` at the end of that code for the event-branch tests; the
`calls*` variants issue both transferred commands from one helper that takes the
command word in `$r8`, with callers that pass an immediate, load the argument,
materialise the helper's address, list it as an extra entry point or append it
as a data word; `calls-forward*` instead pass the command in `$r7` and have the
helper copy it with `ori $r14, $r7, 0`, so the proof has to keep the copy's
provenance (and the computed variant loads the caller's argument instead); the
`flow*` variants put a command immediate and its site in different blocks and
chunks (a dominating preheader, a loop head behind a back edge, two constants on
two paths, a definition on one path, a definition in a sibling branch, one
behind a register-targeted transfer, and the PC-getter/masked-offset dispatch
idiom in exact, register-mask, oversized-set and out-of-region target forms);
`flow-loop-live` adds a live back edge that redefines the same constant, so the
reaching state has to converge; `flow-tail-call` enters one helper by a direct
call and by a tail branch; `flow-link-arg` calls through the register the
helper reads, so the caller's block never set it; `flow-extra-entry` lists the
site's chunk start as an extra entry point; `flow-ila-landing` seeds the site's
chunk start through an `ila` and reaches it with two different commands;
`flow-mid-landing` reaches the `wrch` itself through a computed `biz`; and
`calls-dispatch` appends a register-targeted `bi $r5` (listed as an extra entry
point) next to a resolvable site, for the `assumes` tag. `default-dispatch` and
`flow-r0-identity` append their transfer past the default image's return (a
`bi $r5`, and the `ila`/`ai`/`bi $r0` computed-dispatch idiom), listed as an
extra entry point, so the block-local commands in the default code must carry
that assumption too. The default variant is unchanged. No SPU toolchain and no
guest bytes are involved.
