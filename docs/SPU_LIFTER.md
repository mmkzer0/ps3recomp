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
`ls`, `policy_mode`, `steps`, and `budget` (the last member of the context; ABI 2).
A changed member offset or size requires
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

## Liveness (what a T2 override may rely on)

A native override of one function (tier T2, `docs/SPU_XDIFF.md` sections 8.2
and 17 question 10) may leave registers and stack bytes different from the
lifted function's if nobody reads them. `tools/spu_liveness.py` says which, and
the offline runner's poison run checks what it says.

```
python3 tools/spu_liveness.py IMAGE.bin --base 0x7C00 [--code-end N] [--entry N]... [--functions FILE] -o live.json
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

A function has unknown callers, and every register live-out, when anything but
a direct call or branch can reach it: it is an `--entry`; no call or branch in
the image targets it; its address is an `ila`, an `il` or an `ilhu`+`iohl`
constant, or an aligned word anywhere in the image (a function pointer table);
a call lands inside it; or it is the return continuation of a call. What the
tool does not see is an entry reached through arithmetic on a table base
(offset tables, `ila` plus `ai`) or a halfword table: name such entries with
`--entry`.

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
- A small `spu_context* → mfc_engine` registry holds per-SPU DMA state
  (the engine isn't embedded in `spu_context`).
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
    "code_end": "0x16168",
    "extra_entry_points": ["0xA2C"]
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
- `extra_entry_points` are optional LS addresses passed to `--extra-funcs`, for
  entries the branch analysis cannot see (interrupt vectors, jump-table
  handlers). They must be 4-byte aligned and below `code_end` when that is set.

The registration fingerprint is the FNV-1a-64 over the **whole blob** — which is
exactly the staged bytes and therefore exactly what
`spu_workload_fingerprint(image, image_bytes)` hashes at run time. It is not the
ELF-extent rule (`spu_elf_image_size`) and there is no separate text-segment
alias: a raw job is dispatched by those bytes and nothing else. The pipeline
tests compile the runtime's fingerprint function against the generated blob and
assert the C and Python values agree.

Function discovery runs `find_spu_functions.py`'s `raw=True` mode at `ls_base`,
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
    {"channel": 21, "name": "MFC_Cmd", "read": 0, "write": 5, "count": 0,
     "sites": {"read": [], "write": [31852, 31900, 31948, 32000, 32020], "count": []}}
  ],
  "mfc_commands": [
    {"command": 64, "name": "GET", "count": 1, "sites": [31852]}
  ],
  "event_branches": [],
  "unresolved": [
    {"channel": 21, "access": "write", "site": 32020, "register": "$r11",
     "reason": "command value is not a same-block immediate"}
  ]
}
```

Channel instructions are counted by channel number and by `rdch` (read),
`wrch` (write) or `rchcnt` (count). A `wrch MFC_Cmd` counts as a resolved
command constant only when its source register holds an `il` immediate from the
same straight-line run — no branch, branch target or other register write in
between. Everything else is listed under `unresolved` with its site and register
and is still included in the channel-21 write count. Resolution is deliberately
block-local: a false "resolved" could hide an atomic or mailbox access, while an
unresolved site is visible and reviewable. Command names mirror the
`MFC_*_CMD` constants in `runtime/spu/spu_context.h`.

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
default variant is unchanged. No SPU toolchain and no guest bytes are involved.
