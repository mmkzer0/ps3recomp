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
  new lifter gap cannot hide behind a declared one.

## Roadmap (HARNESS_ROADMAP.md stages 1-9)

| # | Stage | State |
|---|---|---|
| 1 | Case-based: one call, declared expectation | V1, this document |
| 2 | Per function and function x case coverage, matrix, report per NID | V1: per-function verdict aggregate keyed by name and NID, export coverage line |
| 3 | Call chains: A->B->C->A, state between calls (fiber Create->Run->Join->Create) | sequences exist in the case model; a lifted fiber context switch is still blocked (F21b) |
| 4 | Composition: whole subsystems (Sync2 + Fiber, JQ2 queue life cycle) | not started |
| 5 | Integration (forwarding/linking): imports HLE or LLE, closing the `_sys_memset`/`_sys_strncpy` blind spot | V1 flags and counts common mode through forwarded imports |
| 6 | Fault normalisation: guest fault, named stop, hang; comparable instead of declared | V1 result model |
| 7 | CI without firmware: synthetic PPU module (C, C++, hand-written assembly), which also tests the lifter | V1 synthetic fixture above |
| 8 | Per-NID backend switch, explicit instead of the export-registration side effect | not started |
| 9 | Formal methods: differential fuzzing -> KLEE -> CBMC/ESBMC bounded equivalence -> SMT on the IL | future |

Stage 9 results are only ever **"equivalent under constraints C up to bound k"** or a
counterexample; a verifier run that cannot state both is not evidence.

## Adding a module

1. Lift the PRX outside the repository (`lift_prx.py`, then `ppu_lifter.py --raw --base --toc`).
2. Add `tests/diff_harness/cases_<lib>.cpp` with `register_cases()` and `import_policy()`.
3. Add `dh_add_module(<mod> <prefix> <stem> cases_<lib>.cpp <hle-lib>...)` to
   `tests/diff_harness/CMakeLists.txt`.
4. Declare each case's expectation and, for anything but a plain match, its causal witness.
5. Run it locally; the summary's `unexpected` count is the gate.
