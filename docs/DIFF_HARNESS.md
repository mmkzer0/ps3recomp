# Call-level differential harness (HLE vs lifted firmware)

A library function has two implementations in this tree: the hand-written HLE module under
`libs/`, and the same function lifted from the real firmware PRX. The differential harness runs
one call through both and compares what they do. It is variant **a** of the three differentials
in the M3 contract's §3 (that contract is project-local, outside this repository):

| Variant | Compares | Proves |
|---|---|---|
| **a** (this harness) | one call through the HLE against the same call through the lifted firmware library | firmware parity of an HLE handler |
| b (RSX capture/replay, H1) | an RSX capture replayed against our own baseline | determinism and regression of *our* implementation, not PS3 correctness |
| c (frame/reference, S1-S3) | our frame against RPCS3 at the same frame identity | correctness against an independent reference |

Variant a is a **call-level oracle**: it can say an HLE handler diverges from the firmware on a
specific argument, with the return value, the written bytes, the import calls or the fault as the
witness. It cannot say whether a whole frame is right; variants b and c answer that.

## Result model

Two independent dimensions. A case's record carries both.

Per side, the execution state:

| state | meaning |
|---|---|
| `returned` | the call returned; `r3` (and `lr`, `r1`) are recorded |
| `guest-fault{ea,size}` | the side touched a guest EA no mapping backs; `ea` is guest-relative |
| `named-stop{kind}` | the runtime stopped by name ([STOPS.md](STOPS.md)); the canonical machine-line fields are kept and compared |
| `host-crash{signal}` | the side died on a host signal |
| `timeout` | the side hit its per-side alarm |
| `harness-blocked{reason}` | the side could not run at all: no HLE handler, no lifted export, an import the policy forbids, an instruction the lifter did not lower, a truncated result record, ... |

Per case, the comparator's verdict:

| verdict | rule |
|---|---|
| `match` | both sides returned with equal return/memory (and, where applicable, import calls), or an equal named stop, or a guest-fault paired with an `hle-guest-range` stop at the same step with overlapping spans |
| `diverge` | any other observable difference: return, memory, import call, state (`timeout` vs returned, a stop vs returned, ...) |
| `no-hle` | the HLE has no handler for the NID; there is nothing to compare |
| `inconclusive` | a pair the comparator must not call a match: two timeouts, two host crashes, a fault pair that cannot be normalised, an empty (zero-length) span, a lifted result that did not restore the ABI, or named stops that share a kind but not their fields |
| `lifted-blocked` | the lifted side hit a harness gap; the case declares which one (`blocker` + step), and any other gap is unexpected |

`inconclusive` never counts as passing evidence, and a declared `match`/`diverge`/`no-hle`/
`inconclusive`/`lifted-blocked` is checked against the observed verdict. A mismatch is
`unexpected` and fails the run. Cases may also declare a **causal witness** -- cause kind
(`return`, `memory`, `imports`, `status`, `fault`, `fault-span`, `stop-fields`, `abi`, `hang`,
`blocked`), the call step, a memory range the first differing byte must lie in, and a stop kind
-- so a case cannot pass for an unrelated reason.

### Common mode

When a `match` (or an equal named stop) passed through an import with `Policy::Forward`, the
lifted side executed the *same HLE handler* as the HLE side. That is weaker evidence: a bug in
the shared helper cancels out. The case is marked `common_mode: true` with the forwarded NIDs,
and the summary and per-function aggregate count these cases separately. A run whose matches are
all common mode proves the surrounding code, not the forwarded helper.

### Exit codes

| code | meaning |
|---|---|
| 0 | at least one case was actually compared (match/diverge/inconclusive with both sides running) and none is unexpected |
| 1 | a case is unexpected |
| 2 | harness error, a report that cannot be written, a filter that matches nothing, or no compared case |
| 77 | visible `SKIP <module>: <reason>` (configured module absent, missing image) |

## Report (schema `ps3recomp.diffharness/1`)

One JSON document per run, written to `<out>.partial` and renamed into place (never left
half-written). It contains:

- `environment`: toolkit git SHA, module name, image path, image SHA-256, size, base, TOC;
- `options`: filter, enabled/disabled comparator dimensions, fault normalisation;
- `cases[]`: id, function/NID, row, arguments, declared expectation and observed verdict,
  `unexpected`, cause and cause kind, common mode and forwarded NIDs, masked ranges, both side
  states, per-step returns/statuses for sequences, import calls from the lifted side, and the
  memory-difference counters (`missing_bytes` = written by the lifted side but not the HLE side,
  `extra_bytes` = the other direction, `first_missing`/`first_extra`, `diff_ranges`);
- `functions`: name -> NID -> verdict counts (+ `common_mode`);
- `summary`: cases, compared, per-verdict counts, unexpected, common mode, independent matches.

## Running against real firmware (local only)

Firmware material never enters this repository. `run_diff.py` refuses a `--fw-dir`, build dir or
output inside the repository after `realpath` (symlinks included), `gen_tables.py` refuses
repo-contained firmware inputs and outputs, and the CMake hook refuses `PS3RECOMP_DIFF_FW_DIR`
inside it at configure time. The synthetic fixture's runner is the one exception: it is built
with `PS3RECOMP_DIFF_SYNTHETIC` and only ever reads the checked-in synthetic image from the build
tree, never firmware.

```sh
python3 tools/diff_harness/run_diff.py \
    --fw-dir /path/outside/the/repo/fw --module all --jobs 6
```

Layout expected under the firmware directory (see [FIRMWARE_LLE.md](FIRMWARE_LLE.md)):

```
lift/<mod>/<mod>_{image.bin,exports.json,imports.json,functions.json}   # lift_prx.py
lift/<mod>_c/<stem>.c, <stem>.h, <prefix>harness_{tables,layout}.cpp    # ppu_lifter.py + gen_tables.py
```

`run_diff.py` (re)lifts with the toolkit's own `ppu_lifter.py` when the lifted C is missing or
its input fingerprint changed, configures the build every time (so a different `--fw-dir` or
compiler cannot reuse stale tables), builds, runs, and prints the summary and per-function
aggregate. Results are one document per module, `<build-dir>/diff_results.<mod>.json`
(`diff_results.json` for a single module); the ctest firmware tests write
`<fw-dir>/build-diff/diff_results_<mod>.json`, outside the repository like every other
firmware-derived artefact.

A configured module with incomplete metadata is a visible skip:

```
SKIP libsync2: no lifted metadata under /path/fw/lift/libsync2
```

exit 77 unless `--allow-missing` explicitly accepts a partial suite. In ctest the same condition
registers a test that prints that line and returns 77 (`SKIP_RETURN_CODE`); it never passes.

Direct ctest use needs the firmware directory at configure time:

```sh
cmake -S . -B build/diff -DPS3RECOMP_BUILD_TESTS=ON \
      -DPS3RECOMP_DIFF_FW_DIR=/path/outside/the/repo/fw
cmake --build build/diff -j6
ctest --test-dir build/diff -L '^fw$' --output-on-failure
```

Firmware tests are labelled `fw;stress`: they need real firmware, fork twice per case and are
not part of CI. The `--disable-dimension` option of the runner is test-only; production runs
enable every dimension.

### Registered modules

`dh_add_module()` registers a lifted module: its metadata directory, the lifted C stem and
prefix, the case table, and the HLE libraries whose registrations the module needs (the HLE
dispatcher is generated from them exactly as a port generates it). `run_diff.py`'s `MODULES`
table carries the same prefix and stem for the lift.

| module | cases | what it covers | current verdicts (0 unexpected is the gate) |
|---|---|---|---|
| `libsync2` | 61 | semaphore attribute validation, initialize/finalize/wait/signal/try-wait, buffer-size arithmetic | 61 match |
| `libfiber` | 98 | scheduler and fiber lifecycle without a guest context switch | 78 match, 16 diverge, 1 no-hle, 1 inconclusive, 2 lifted-blocked |
| `libspurs_jq` | 242 (155 sequences) | JQ2 attribute, create (10- and 12-argument forms), shutdown/join, Port2 create/destroy, PushJob/CopyPush/PushSync/Sync, ring and descriptor-pool accounting | 175 match (119 common mode, 56 independent), 50 diverge, 6 no-hle, 9 inconclusive, 2 lifted-blocked |

The counts are a property of the HLE at the commit; they move when a finding is fixed. The
per-function aggregate in the report is keyed by name and NID, and the summary separates
common-mode matches, so a module whose matches all pass through a forwarded import cannot be
mistaken for independent evidence.

The JQ2 table needs three engine features beyond a single call:

- its 10- and 12-argument creates put arguments 9..12 in the caller's parameter save area
  (`r1+0x70..`), which is what both the ELFv1 lifted code and the HLE adapter read;
- the HLE side owns JQ2 worker threads after a successful create, so that step's `post` hook
  pauses intake on the HLE side only (`on_hle_side()`): the pushes then leave exactly the bytes
  they wrote, and no SPU job runs on either side;
- an import model can stop the lifted call by name (`model_stop`) or wait for good
  (`model_block`; the per-side alarm turns it into a `timeout` outcome, as the HLE's own
  blocking does). The intentionally blocking pushes are declared `inconclusive` with the `hang`
  cause and the step, not hidden as timeouts that happen to agree.

The JQ2 masks are the exact queue-block bytes the firmware writes and the HLE models elsewhere
or not at all: the per-queue state bytes (`+0x0D`), the flag doubleword (`+0x60`), the attribute
copies (`+0x7A`, `+0x9C`, `+0x1C4`, `+0xF18`), the attached-port count word (`+0x70`, a u16 pair
in the firmware, a u32 in the HLE) and the port-slot bitmap (`+0x580`, MSB-first in the firmware,
LSB-first in the HLE; the slot numbers agree). `c00_unmasked_valid_create` is the one unmasked
create and is declared `diverge` at `+0x0D`, which is how those ranges were found; a mask may
not cover the bytes a case is about, and the report lists every masked range.

Two JQ2 cases are declared `lifted-blocked` rather than passed:

- `q16_join_before_shutdown`: the firmware's Join reaches `cellSpursWaitForWorkloadShutdown`,
  whose answer depends on SPURS workload state this harness does not model. The model stops the
  lifted call (`model-stop` at step 1) and the case declares that blocker; the HLE returns its
  own `STAT`.
- `n07c_destroy_slot_not_allocated`: the firmware asserts (`_sys_printf`, then
  `_sys_trap_process`), which the import policy refuses (`import-policy` at step 2).

The firmware-only exports (`cellSpursJobQueueAttributeSetMaxGrab`, the pooled-descriptor push
`0xE1731DF3`) are the six `no-hle` cases: the HLE has no handler, so there is nothing to compare.

## Synthetic fixture (CI, no firmware)

`tests/diff_harness/synth/` builds a synthetic PPC64 module from checked-in sources:

- `module.c` -- C exports, compiled with a PPC64-capable clang
  (`--target=powerpc64-unknown-linux-gnu -mcpu=ppc-cell-be -ffreestanding -fno-builtin -O2`;
  LLVM 23 rejects `-mcpu=cell`, the CMake probe tries `ppc-cell-be`, then `cell`, then no CPU);
- `module_cpp.cpp` -- one `extern "C"` C++ export;
- `module_asm.S` -- a hand-written export and the SCE-shaped import stub;
- `synth.ld` -- image at `0x02000000`, import slots pinned at `0x02020000`.

`make_synth.py` extracts the OPDs/imports/function seeds from the linked ELF, writes the flat
image (OPDs normalised to the firmware's two-big-endian-32-bit-word form that the loader reads),
and runs the toolkit's own `ppu_lifter.py` in raw mode with `--toc`, then `gen_tables.py`.
`synth_hle.cpp` registers a test-only HLE under synthetic NIDs; `cases_synth.cpp` compares it
with the lifted module. The fixture includes one deliberate mutation per comparator dimension:

| mutation | witness |
|---|---|
| wrong return | return value |
| missing guest write | `missing_bytes` at the destination |
| extra zero padding (strncpy-vs-memcpy class) | `extra_bytes` at the destination |
| wrong import argument order | import-call record (the helper returns 0 and touches nothing) |
| wrong fault span / zero-length fault | `inconclusive` (`fault-span`) |
| return instead of fault | `diverge` (`status`) |
| hang on both sides | `inconclusive` (`hang`) |

`tools/diff_harness/test_synth.py` (ctest `diff_synth`, labels `diff;t0`) runs the fixture, checks
the schema/aggregate/common-mode, and proves **fail-before** for every dimension: with
`--disable-dimension <dim>` its mutation case must go undetected (the runner exits 1 with the case
`unexpected`). A dimension that stops detecting its mutation fails the test. If no PPC-capable
clang or `ld.lld` is found, CMake registers the same test with a visible skip (77); it never
passes silently. `-DDH_PPC_CLANG=...` / `-DDH_LD_LLD=...` override the probe.

### Cross-compiled synlib variants (the GCC route)

The same directory also holds a second, independent module, `synlib`, written from
[`syn_spec.h`](../tests/diff_harness/synth/syn_spec.h) four ways:

- `syn_c.c` -- C at `-O2`;
- `syn_cpp.cpp` -- C++ at `-O2` (templates, constexpr tables, enum class, a small view class);
- `syn_asm.S` -- hand-written ELFv1 assembly: a jump table fetched through a TOC entry, a CTR
  loop, a call to another export, the import and single/double floating point;
- `syn_c_bug.c` -- `syn_c.c` with three deliberate bugs (check order, off-by-one range limit,
  32-bit product where the spec says 64-bit).

`ref_native.c` is the host reference implementation (the HLE side), registered through
`syn_ref_register.cpp` with `ps3_hle_adapt`, so the real register marshalling is part of what is
exercised. `syn_stubs.S` provides the SCE-shaped import stub, `syn_gcc.ld` links flat at
`0x02000000`, and `syn_module.json` is the export/import manifest.

The build route is the stock `powerpc64-linux-gnu-gcc/g++/ld` (Ubuntu packages
`gcc-powerpc64-linux-gnu`, `g++-powerpc64-linux-gnu`, `binutils-powerpc64-linux-gnu`):

    -m64 -mcpu=cell (probe falls back to -mcpu=powerpc64)
    -mcmodel=small -mno-altivec -mno-vsx -O2 -ffreestanding -fno-builtin
    -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables
    -fno-stack-protector -fno-tree-loop-distribute-patterns -ffp-contract=off
    link: -T syn_gcc.ld --defsym SYN_BASE=0x02000000 -z noexecstack --no-toc-optimize

`-mcmodel=small` with `--no-toc-optimize` keeps TOC entries in the `ld rX,disp(r2)` form the
lifter's jump-table discovery reads. `tools/diff_harness/synth_pack.py` packages the linked ELF
as a PRX-shaped image plus exports/imports/functions/names JSON; `lift_module.py` lifts it with
the toolkit's own `ppu_lifter.py --raw --base --toc --functions`; `gen_tables.py --synthetic
--names` emits the harness glue. Every variant runs the same 138-case table.

| variant | flags | result (GCC 13.3) |
|---|---|---|
| `c` | `-O2` | 138 match |
| `cpp` | `-O2` C++ | 138 match |
| `asm` | hand-written | 138 match |
| `cbug` | `-O2`, three bugs | 132 match, 6 diverge (exactly the declared cases) |
| `c_Os` | `-Os` | 138 match |
| `c_O3` | `-O3 -funroll-loops` | 138 match |
| `c_O0` | `-O0`, the #45 table shape | 101 match, 37 lifted-blocked |
| `c_medium` | `-mcmodel=medium`, linker TOC optimisation | 101 match, 37 lifted-blocked |

The `cbug` table declares the six cases that reach a bug `diverge` with their cause (`return` for
the two check-order/limit cases, `memory` in the PACK wide field for the other four); every other
case must stay `match`. `--expect-diverge 6` and `--min-cases 138` are repeated in
`CMakeLists.txt` on purpose, so editing the table alone cannot make the count check vacuous.

#### Known lifter gaps (issue #45), declared rather than hidden

GCC emits two switch dispatches the lifter does not yet recognise: at `-O0`, the table base plus
index is formed first and the case address read with `lwz rX,0(rY)` (no `lwzx`); at
`-mcmodel=medium` with the linker's default TOC optimisation, the table base is an `addis`/`addi`
pair from `r2`. The case bodies are therefore not lifted and the dispatch hits an unlifted
address. In `c_O0` and `c_medium` exactly the 37 cases that reach `synObjCommand`'s switch are
declared `lifted-blocked` with `blocker = unlifted-function`, their step (0 for a single call, 1
for a sequence) and a note naming #45. They neither pass silently nor turn the suite red today;
when #45 is fixed they return normally, become `match`, and the declarations turn into
`unexpected` so the expectation must be flipped.

A third #45 shape -- a tail branch to a linker-provided `_restgpr0_*` register-restore helper --
is not discovered either. GCC 13.3 does not emit those helpers for this fixture's code, so the
`-Os` variant does not reach it; `synth_pack.py` seeds size-less function symbols (which would
include such helpers) from the symbol table. If a compiler starts emitting the helper as a tail
branch, the affected variant must be declared the same way rather than allowed to fail or pass
silently.

#### ABI choice: `-m64` under 4 GiB with 8-byte OPDs

The stock cross toolchain is ELFv1: a function symbol's descriptor is 24 bytes of 64-bit words
(code, TOC, environment). The PS3 PPU ABI calls through an 8-byte `{u32 code, u32 toc}`
descriptor. `synth_pack.py` bridges the two: code addresses come from the ELFv1 descriptors (or
from a dot symbol in an executable section, for the hand-written import stub), every exported
descriptor must name the same module TOC, and the tool then writes the flat image followed by one
8-byte PS3 descriptor per export. Every address is checked to be word-aligned, below 4 GiB and
32-bit clean. That is why the fixture is `-m64` (the firmware's 64-bit register model) with a
linker script under 4 GiB: a guest effective address is a `u32`, and both the lifted code and the
HLE adapter zero-extend it. The guest TOC is read from the linked descriptors, never invented.

#### What the synthetic fixtures cannot cover

- They are our own code: they prove packaging, the lift, the ABI adaptation and the comparator,
  not that a lifted *firmware* function is correct. A misreading of the ABI shared by the
  reference and the compiled module cancels out.
- No PRX/SCE relocation records or module-entry shape, no lv2 syscalls, no SPU or RSX code; the
  import glue is one host helper. The firmware module is the only place those appear.
- No fiber or thread context switch: a lifted fiber that needs one is still `lifted-blocked`.
  AltiVec/VSX are compiled out; only the scalar integer and floating-point units are exercised.
- Firmware-only writes cannot occur here, so masking them (the JQ2 module) is not tested by this
  fixture; that is `test_synth.py`'s mask-boundary case and the JQ2 table's job.

`PS3RECOMP_DIFF_SYNTHETIC_REQUIRED=ON` turns a missing cross toolchain into a configure error;
the Linux CI job installs it and sets the option, so the variants cannot silently stop running
there. On macOS there is no Homebrew PowerPC64 GCC, so these tests register a visible skip (77)
while the clang/lld fixture above still runs.

## Comparator dimensions and masks

`return`, `writes` (every byte the lifted side wrote, the HLE side wrote too), `extra` (the
opposite direction; catches zero-padding), `imports` (call sequence: NID, step, arguments,
return), `faults` (guest-fault <-> `hle-guest-range` normalisation), `fault-span` (same step and
overlapping non-empty spans) and `hang` (two timeouts/two host crashes are inconclusive).

The compared surface is the **whole guest arena minus the guest stack**. The guest stack is the
one region the lifted side writes by construction (it builds host-visible guest frames there)
while the HLE handler does not, so it is excluded and documented here. The module image is compared: a firmware write into its
own data that the HLE does not reproduce fails the case unless the case masks it.

Masks are per case and must name exactly the bytes an evidence-backed firmware-only artefact
writes; they appear in the report as `masked`. `mask` holds guest EAs, `mask_image` holds offsets
from the module base (reported as `image+<offset>+<length>`). No mask may cover a region the case
is meant to test; the synthetic fixture has a mask-boundary case proving that bytes adjacent to a
masked range still diverge.

The 4 GiB guest space is reserved `PROT_NONE` with the arena mapped inside it, so a lifted access
outside the arena is a deterministic `guest-fault` (not whichever host mapping happens to follow
`vm_base`).

## Known limitations

- One call or a short sequence per case, not a title. No context switch: a lifted fiber that
  needs one ends `lifted-blocked` (declared with its blocker and step).
- Guest stack bytes are not compared (above). FPR/VR/CR state is not part of the comparison;
  integer r3, memory and the ABI nonvolatile GPRs (r14-r31, seeded with sentinels) are.
- The import-call dimension applies when the HLE implementation re-enters the HLE dispatcher for
  a module import (the synthetic fixture does). HLE code that calls a C helper directly has no
  import record, so that dimension is not applicable and says so by staying silent.
- A harness gap on the lifted side is `lifted-blocked`; the case must declare the blocker, so a
  new lifter gap cannot hide behind a declared one. The JQ2 table declares two such blockers
  (`model-stop`, `import-policy`) and the #45 variants declare `unlifted-function`.
- JQ2's HLE side owns host worker threads. A create's `post` hook pauses its intake, so a push
  sequence is compared as the bytes the push wrote; nothing ever drains the ring on that side.
  The firmware side's workers are SPU code this harness does not run (no SPU execution at all).
- A side that must wait on state the harness cannot provide is a declared `hang`
  (`inconclusive`), never a match: `s11b` and the `s15*` full-ring pushes time out on both sides
  at the declared step. This proves they both block, not that they would wake correctly.

## Roadmap (HARNESS_ROADMAP.md stages 1-9)

| # | Stage | State |
|---|---|---|
| 1 | Case-based: one call, declared expectation | V1, this document |
| 2 | Per function and function x case coverage, matrix, report per NID | V1: per-function verdict aggregate keyed by name and NID, export coverage line |
| 3 | Call chains: A->B->C->A, state between calls (fiber Create->Run->Join->Create) | sequences exist in the case model; a lifted fiber context switch is still blocked (F21b) |
| 4 | Composition: whole subsystems (Sync2 + Fiber, JQ2 queue life cycle) | partial: `libspurs_jq` covers the JQ2 queue/port life cycle (242 cases); cross-subsystem composition not started |
| 5 | Integration (forwarding/linking): imports HLE or LLE, closing the `_sys_memset`/`_sys_strncpy` blind spot | V1 flags and counts common mode through forwarded imports |
| 6 | Fault normalisation: guest fault, named stop, hang; comparable instead of declared | V1 result model |
| 7 | CI without firmware: synthetic PPU module (C, C++, hand-written assembly), which also tests the lifter | V1 clang/lld fixture plus the cross-compiled synlib variants and their declared #45 gaps |
| 8 | Per-NID backend switch, explicit instead of the export-registration side effect | not started |
| 9 | Formal methods: differential fuzzing -> KLEE -> CBMC/ESBMC bounded equivalence -> SMT on the IL | future |

Stage 9 results are only ever **"equivalent under constraints C up to bound k"** or a
counterexample; a verifier run that cannot state both is not evidence.

## Adding a module

1. Lift the PRX outside the repository (`lift_prx.py`, then `ppu_lifter.py --raw --base --toc`).
2. Add `tests/diff_harness/cases_<lib>.cpp` with `register_cases()` and `import_policy()`.
3. Add `dh_add_module(<mod> <prefix> <stem> cases_<lib>.cpp <hle-lib>...)` to
   `tests/diff_harness/CMakeLists.txt` and the same prefix/stem to `run_diff.py`'s `MODULES`.
   The HLE libraries must include every NID the lifted code forwards (a `Forward` policy with no
   registered handler is a lifted block, not a match); imports the HLE core does not register
   get a `Model` stand-in, and anything else stops.
4. Declare each case's expectation and, for anything but a plain match, its causal witness.
5. Run it locally; the summary's `unexpected` count is the gate.
