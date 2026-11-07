# RUN.md: spuexh on a real console

spuexh runs the SPU instruction sweep of the host harness (`runtime/spu/tests/test_spu_exhaustive.c`) on the SPUs of a console.
The generator, the domains and the block hash are the host harness' own source file, compiled unchanged for the SPU
(`runtime/spu/tests/spu_exh_gen.h`), so a hash computed here is directly comparable with the host harness' hash of the
same op, domain and block. A console run is the golden reference: where the toolkit disagrees with it, the toolkit is wrong.
The probe only executes SPU instructions inside the SPU's local store and DMAs results to its own memory; it patches nothing.
The PPU SPU backend uses only the assigned SPU-thread/event-queue LV2 calls listed below; the shared hwprobe layer handles
result and UI services. Results are written only below `/dev_hdd0/tmp/hwprobe/`. `outdir` must be that directory or below it
(no `..` components, checked at start-up), and a result file that cannot be opened there ends the probe with an error; no fallback device is used.

The PPU SPU backend's direct LV2 calls use assigned slots only: `sysEventQueueCreate` (128), `sysEventQueueDestroy` (129),
`sysEventQueueReceive` (130), `sysUsleep` (141), `sysSpuThreadGetExitStatus` (165), `sysSpuInitialize` (169),
`sysSpuThreadGroupCreate` (170), `sysSpuThreadGroupDestroy` (171), `sysSpuThreadInitialize` (172),
`sysSpuThreadGroupStart` (173), `sysSpuThreadGroupTerminate` (177), `sysSpuThreadGroupJoin` (178), and
`sysSpuThreadGroupConnectEvent` (185). In particular, exit-status polling uses slot 165 and every launch initializes
its SPU thread through slot 172. The SPU backend does not call slots 6-10, 15, or 1024 and above.

Build, copy and the console-side mechanics are in `tools/hwprobe/README.md`. Commands are run from the repository root with
`export HWPROBE_CONSOLE=ADDR`. Format of the result file: `FORMAT.md`.

## 1. Build

Needs the PPU toolchain and the SPU toolchain (spu-gcc 9.5.0, the last gcc with an SPU backend):

```
tools/hwprobe/toolchain/build-toolchain.sh --with-spu        # container; --check tells whether the prefix is complete
tools/hwprobe/toolchain/build-toolchain.sh --spu-only --native --check     # the SPU part alone, on a matching linux host
tools/hwprobe/build.sh spuexh layer1                          # -> tools/hwprobe/out/spuexh_l1.fake.self
```

`build.sh` first runs `probes/spuexh/prebuild.sh` on the host: it copies `spu_exh_gen.h` next to the probe and generates, from
the repository's own tables (`test_spu_exhaustive.c` for the ops and domains, `spu_interp_tables.inc` for the instruction
words and the undecoded prefixes), the op table, the prefix list and the raw-execution routine (`probes/spuexh/generated/`,
not tracked). The SPU program is built with spu-gcc, embedded in the SELF, and the PPU layer with ppu-gcc.

The SPU build ends with an LS layout guard: the raw-exec arenas sit at fixed local-store addresses (`spuexh.h`: arena A at
0x1E000, arena B at 0x3C000) that the linker does not reserve, and the PSL1GHT SPU script puts the stack at 0x3FFF0 growing
down. Arena B below the stack floor is a `_Static_assert` in `spuexh.h`; the build fails when the loaded image (`_end`)
reaches arena A, and the arenas must be moved (constants in `spuexh.h`, used by SPU and host alike) if a rebuild grows into them.

Independent check of the instruction words (every op is assembled with the GNU SPU assembler and compared):

```
PS3DEV=<prefix> python3 tools/hwprobe/probes/spuexh/verify_encodings.py .      # 193 ops and 5 special-A words checked against spu-as, 0 mismatches
```

## 2. Dry run without a console (do this first)

The very SPU ELF that goes into the SELF runs in the toolkit's interpreter and writes the file the console run writes:

```
PS3DEV=<prefix> tools/hwprobe/probes/spuexh/dryrun.sh smoke OUT_DIR       # builds the ELF and the driver, runs, compares with scalar.txt
```

`dryrun.sh` runs the sweep one domain per process with the special probes off; probe A (expected in the interpreter: all five
`NOT_executed`, like the hardware) runs with the driver directly: `host/spuexh_host --elf probes/spuexh/spu/spuexh.elf --cfg
probes/spuexh/cfg/specialA.cfg --out specialA_dry.txt`.

The driver `host/spuexh_host` accepts the same configuration keys (`--cfg FILE`, `--set key=value`, `--preset P`) plus
`--plan` (jobs, hashes and calls of a configuration), `--no-sweep`, `--special-a`, `--special-b`, `--inject SPU:N`
(kill an interpreted SPU at its N-th DMA put, to exercise the fault handling), `--inject group:SPU:N` (end the whole
group there, the console's group-exception model, for the collateral FAULT/RETRY handling) and `--inject-fail
connect|terminate` (make the backend fail a launch the way the fail-closed policy expects: a failed exception-queue
connect, or a failed terminate after a supervisor end).

## 3. Configuration and duration

`cfg/*.cfg` is uploaded as `spuexh.cfg` (`put-cfg spuexh <file>`); the probe reads `/dev_usb000/spuexh.cfg` first. Keys:

| Key | Meaning |
|---|---|
| `preset=smoke\|standard\|full` | first key; sets the stride defaults below |
| `stride_all`, `stride_heavy`, `bmax`, `stride_dom=NAME:S;NAME:S`, `bmax_dom=NAME:N` | a block is hashed when `block % stride == 0` (the host harness' rule; heavy = the 44 conversion scales other than 173/155); `bmax` keeps only blocks below it |
| `ops=a,b*`, `skip_ops=`, `doms=` | comma lists of names; a trailing `*` is a prefix (`cflts*`) |
| `spus=N` | SPU threads (1..6, default 6); fewer are used if the system refuses a group of that size |
| `special_a=0\|1`, `a_ops=`, `special_b=0\|1`, `special_b_max=N`, `special_b_from=N`, `sweep=0\|1` | the two special probes and the sweep, each switchable; `a_ops` narrows probe A; `special_b_from` is an inclusive prefix-list index for resuming |
| `special_b_count=N`, `special_b_patterns=zero\|both` | probe B staging: N counts prefixes (0 = every remaining prefix); `both` runs zero and random words for each selected prefix |
| `lanes=OP:DOMAIN:BLOCK[:COUNT[:START]]`, `lanesum=OP:DOMAIN:BLOCK:CHUNK` | per-lane dump of COUNT (default 64, max 1024) calls of a block from call START (default 0), and one hash per chunk of CHUNK calls over a whole block; each repeatable up to 16 times, conversions at any I8 scale (`cflts@45`), see section 6a; set after `preset=` |
| `stall_s` (120), `max_faults` (24), `case_timeout_ms` (2000), `b_timeout_ms` (600), `launch_timeout_ms` (10800000) | timeout fields are decimal digits only, fit a signed 32-bit integer, and are positive except `stall_s` may be 0 |
| `pad`, `hold_s`, `screen`, `tag`, `outdir`, `fw_version`, `fw_type`, `console_model`, `progress_file` | as the other probes (declare the console so it reaches the result header); `outdir` must stay inside `/dev_hdd0/tmp/hwprobe` |

**The configuration fails closed.** An unknown key (a typo such as `special_bb=1` or `launch_timeout=...`), a non-blank line
without `=`, a non-decimal/overflowing numeric value, or a bad value stops the probe at start-up before any SPU is started.
`special_b_from` must be less than the effective prefix total (`special_b_max`, or the generated total when max is 0 or at
least that total). Switches take exactly 0 or 1. The first error is reported and nothing falls back to a default. On the
console the error is shown on the screen and written as `FATAL,cfg,<text>` (for example `FATAL,cfg,unknown key 'special_bb'`)
into a result file of its own (the screen may be off, `screen=0`); a bad `outdir` is the exception: nothing is written outside
`/dev_hdd0/tmp/hwprobe`, so the error is only on the screen. The host driver exits 2 with the same text on stderr before it
opens the ELF or the output.

### Launch and termination policy

Every launch (`launch_timeout_ms`, or `case_timeout_ms`/`b_timeout_ms` for the special probes) is bounded while the threads
run, and the stall detector (`stall_s`) ends a hung group earlier. Before any join, the probe polls `sysSpuThreadGetExitStatus`
for every thread until all report exited or the launch deadline passes, with a minimum poll window of 2 seconds from launch
start. This is the failure-path floor before termination, not a join timeout. If an exception arrives or a thread remains
unexited, the group is terminated; only a successful `sysSpuThreadGroupTerminate` allows a join.

`sysSpuThreadGroupJoin` has no timeout in lv2 and cannot itself be bounded by this probe. If join fails, or if group destroy
or event-queue destroy fails during cleanup, the backend writes `FATAL,launch,...` with the failing syscall name and rc,
then stops before another launch (and before another B word). A failed join leaves resources for process exit rather than
risking another wait.
A failed terminate leaves the group's state unknown: the probe does not join, writes a flushed `FATAL,launch,...` row
naming `sysSpuThreadGroupTerminate` and rc, and exits non-zero so process exit lets lv2 reclaim the live group. A failed
exception-queue connection is reported as a launch failure; the unstarted group and queue are destroyed with checked results.

What lv2 returns from `sysSpuThreadGetExitStatus` for an exception-stopped thread is **`0x8001000F` (`CELL_ESTAT`)**, measured
on the console (r002, CECHC04 4.81 REBUG; section 7). PSL1GHT's `sys/spu.h` documents only "zero if no error occured, nonzero
otherwise", so the probe keeps recording every non-zero code in `META,exit_status_pending,0x...` rows (first occurrence per
code). Both the FATAL text and those rows are the evidence the strict policy is based on.

The stage configs set `screen=0`: the on-screen status path can wait unboundedly (RSX label wait in `common/hwprobe_ui.c`,
`rsxFinish` at shutdown), so a console stage runs without the screen; the result file is the record.

Probe A (the five DP compares, which the retail SPU does not execute) is independent of the sweep: they are not ops of the sweep
and no row is skipped for them.

Plan and cost per configuration. Jobs, hashes and op applications are exact (`spuexh_host --plan`); the time is an
ESTIMATE from the number of SPU instructions the interpreter executes for one job of each domain (summed over the
plan, `estimate_runtime.py`), divided by 6 SPUs and by an assumed 0.5 to 1.0 instructions per cycle at 3.2 GHz. Nothing has been measured on a console.

| config | jobs | hashes | op applications | SPU instructions | estimated sweep time | note |
|---|---|---|---|---|---|---|
| `smoke` (block 0 of everything) | 36 | 302 | 2.6e8 | 8.5e10 | 4 to 9 s | |
| `standard` (every 64th block) | 180 | 1432 | 1.4e9 | 4.2e11 | 22 to 44 s | |
| `priority` (frest frsqest fi and the 48 conversions, every block) | 1056 | 51232 | 5.4e10 | 7.6e12 | 6.6 to 13 min | the ops no independent oracle exists for |
| `full` (everything, every block) | 9987 | 78989 | 8.3e10 | 2.4e13 | 21 to 41 min | |
| `special` (probe A and full B, no sweep) | | | | | about 52m46s failure-path floor, plus launch overhead | 5 A words and 1,578 B words; no console measurements |

Run `python3 tools/hwprobe/probes/spuexh/estimate_runtime.py <dryrun-dir> <spuexh_host> smoke standard tools/hwprobe/probes/spuexh/cfg/priority.cfg full`
to recompute the sweep estimate from those files; its values depend on measured domains and are not console measurements.
The interpreter runs about 1.5e8 SPU instructions per second per host core, so `dryrun.sh standard` takes about 15 minutes
on 3 cores; `full` is not practical to dry-run.

`estimate_runtime.py` also prints failure-path floors for the five staged B configs and full B. Each attempted word contributes
`max(2 s, b_timeout_ms)` before a forced terminate; these totals exclude group-launch, cleanup, and file-transfer overhead.
Full B is 789 prefixes x 2 patterns = 1,578 words: 3,156 seconds (52m36s, about 53 minutes) on the non-returning failure
path, plus launch and cleanup overhead. Probe A adds five 2-second polling windows to the `special` row above.

A thread group of 6 needs all 6 SPUs; the system may hold one (audio, a running game). The probe then falls back to 5, 4, ...
and says so (`META,spus`).

## 4. Run

The first `/play.ps3` after a finished run often ends in XMB error `8001003A`; `ps3console.py` retries. **Do not unmount
before the result file is fetched.**

```
export HWPROBE_CONSOLE=ADDR
tools/hwprobe/build.sh spuexh layer1
tools/hwprobe/ps3console.py run tools/hwprobe/out/spuexh_l1.fake.self --prefix spuexh --probe spuexh --run-window 60 \
    --cfg spuexh tools/hwprobe/probes/spuexh/cfg/smoke.cfg --timeout 900 --out results
```

1. Run `stage0` (`cfg/stage0.cfg`): one SPU, integer-only domains and ops, block 0, no special probes. Continue only when
   the result is complete, has no `FATAL`, and has the expected hashes.
2. Run `specialA` (`cfg/specialA.cfg`): probe A alone; all five words are expected not to execute.
3. Run `smoke` (`cfg/smoke.cfg`): block 0 of every domain and op, with probe A as the regression check.
4. Run `standard` (`cfg/standard.cfg`), then `priority` (`cfg/priority.cfg`) and `full` (`cfg/full.cfg`); use `--timeout 3600`
   or `7200` for these long sweeps.
5. Run Probe B only after stages 1-3 passed. The failure-path duration below is 2 seconds per word with `b_timeout_ms=600`;
   normal returns can finish earlier, and launch overhead is not included. Use the explicit runner timeout shown:

   | Step | Config | Prefixes | Patterns | Words | Failure-path floor | `ps3console.py --timeout` |
   |---:|---|---:|---|---:|---:|---:|
   | 1 | `cfg/specialb_first.cfg` | 1 | zero | 1 | 2 s | 120 s |
   | 2 | `cfg/specialb_4.cfg` | 4 | zero | 4 | 8 s | 120 s |
   | 3 | `cfg/specialb_16.cfg` | 16 | zero | 16 | 32 s | 180 s |
   | 4 | `cfg/specialb_4_both.cfg` | 4 | both | 8 | 16 s | 180 s |
   | 5 | `cfg/specialb_16_both.cfg` | 16 | both | 32 | 64 s | 300 s |

   Run these one at a time, in order, inspecting each result before the next:

   ```sh
   tools/hwprobe/ps3console.py run tools/hwprobe/out/spuexh_l1.fake.self --prefix spuexh --probe spuexh --run-window 60 --cfg spuexh tools/hwprobe/probes/spuexh/cfg/specialb_first.cfg --timeout 120 --out results
   tools/hwprobe/ps3console.py run tools/hwprobe/out/spuexh_l1.fake.self --prefix spuexh --probe spuexh --run-window 60 --cfg spuexh tools/hwprobe/probes/spuexh/cfg/specialb_4.cfg --timeout 120 --out results
   tools/hwprobe/ps3console.py run tools/hwprobe/out/spuexh_l1.fake.self --prefix spuexh --probe spuexh --run-window 60 --cfg spuexh tools/hwprobe/probes/spuexh/cfg/specialb_16.cfg --timeout 180 --out results
   tools/hwprobe/ps3console.py run tools/hwprobe/out/spuexh_l1.fake.self --prefix spuexh --probe spuexh --run-window 60 --cfg spuexh tools/hwprobe/probes/spuexh/cfg/specialb_4_both.cfg --timeout 180 --out results
   tools/hwprobe/ps3console.py run tools/hwprobe/out/spuexh_l1.fake.self --prefix spuexh --probe spuexh --run-window 60 --cfg spuexh tools/hwprobe/probes/spuexh/cfg/specialb_16_both.cfg --timeout 300 --out results
   ```

   For each fetched CSV, run `python3 tools/hwprobe/probes/spuexh/specialb_report.py <result.csv>`. Continue only when it
   exits 0, reports `Completion marker: complete`, has exactly the listed number of `SPECB_TRY` and `SPECB` rows, and every
   verdict is `agree`. Stop on `FATAL`, `differ`, `unknown`, a suspected freeze, malformed rows, a missing completion marker,
   or a runner timeout/lost process; inspect the result before doing anything else. A batch timeout is not proof that the
   probe stopped, and do not unmount before the result file is fetched.

   If a `SPECB_TRY` has no following outcome, resume at that same row's prefix: copy its `index` into `special_b_from`
   (inclusive, not index+1) and set `special_b_count` to the number of prefixes you still need in that batch. Use the
   hexadecimal `prefix` as the identity: if the generated list changed between builds, map that prefix value to its new
   index. In `both` mode the resumed prefix runs zero and random again, so the attempted pattern and its partner are not
   silently skipped. `spuexh_last.txt` is marked running before Probe B starts and updated after each word.

Fetch and clean up:

```
tools/hwprobe/ps3ftp.py get results --prefix spuexh
tools/hwprobe/ps3ftp.py rm-results --prefix spuexh      # optional
```

## 5. Check a run

A run is complete when `META,completed,1`, `META,aborted,0`, `META,exit_requested,0`. `META,faults` is not zero when an SPU
stopped (`FAULT` rows name the job and what lv2 reported): the sweep continued after it, and the hashes of the job that
was running are missing - except when the stop was a group-wide kill (an exception or a supervisor termination, which
takes healthy workers with it): those workers are re-run alone once, and a `RETRY,spuN,job_K,ok` row says the job
completed there (its hashes are kept); only a job that faults in that single-SPU launch becomes a `FAULT`. A `FATAL,launch,...`
row means the probe stopped itself because a terminated group could not be contained. `META,launchN_spuM_fpscr` is the
FPSCR each thread read back at the start of launch N: the SPU program writes the defined state first (all status bits
clear, DP rounding round-to-nearest-even, SPU ISA v1.2 section 9.3), so the value documents the state every hash was
computed in; `META,spuN_fpscr_end` is what the SPU ended the last launch with (sticky exception flags). A
`TIMING_OVERFLOW,<domain>,<block>` row says a job ran past one decrementer period (~53.8 s) and its tick count is not a
number (`META,timing_overflows` counts them; the job's hashes are still valid).

## 6. Compare

```
python3 tools/hwprobe/probes/spuexh/compare_spuexh.py results/spuexh_r001_full.csv \
    --scalar /path/to/scalar.txt --interp dryrun_full.txt [--rpcs3 rpcs3_precise.txt] [--gen native_gencheck.txt] --md verdicts.md
```

`--scalar` is the host harness' reference (`run_exhaustive.sh scalar`, or any other build's hash file in the same line format).
**The scalar reference must come from a toolkit at or after dev commit dbb93d6c**: from that commit the harness itself
emits encodable hashes for the immediate domains (`immh`/`immw`), so no separate encodable-reference build is needed any
more (the old `ref_encodable.py` is gone). `--interp` is the same probe run by `spuexh_host` (the toolkit's interpreter
on the very SPU program). `--rpcs3` is any further file in the same line format. `--gen` is the output of
`spuexh_host --gencheck` (default: the `#gencheck` rows of the `--interp` file); the generator self-check is the first
gate: every domain measured on the hardware needs a digest in the hardware file and in the reference, and the digests
must agree - a measured domain without evidence on either side, a digest disagreement or disjoint evidence sets fail the
comparison (exit 1), because a generator that computes other inputs on the SPU makes every op verdict of that domain
meaningless. Ops of such unvalidated domains carry a `gencheck unvalidated` label in the verdict table. Only blocks
present in both files are compared. The output is a per-op table (blocks compared / differing against each reference,
verdict `MATCH`, `HW!= <reference>`, `NOT RUN`, `NO COMMON`), the first differing block of every difference, the special
probes' reports, and the exit status is 1 when any op differs from a reference or the generator gate failed.

### Probe B without scalar or interpreter references

`specialb_report.py` reads only the hardware result. It reports each prefix/pattern, the toolkit decode expectation,
observed class and cause, register/memory effects, verdict, and whether `META,special_b_completed,1` confirms the batch.
Run `python3 tools/hwprobe/probes/spuexh/specialb_report.py <hardware-result.csv>`. Exit 0 means the batch marker is
present and every verdict agrees; exit 1 means incomplete or a non-agreeing outcome; exit 2 means malformed input.


Reading the three-way result:

- hardware == interp == scalar: the op matches everywhere. (Before dev dbb93d6c the immediate domains of word and halfword
  immediates (`ai`, `ahi`, `il`, ...) differed against an old `scalar.txt` alone: the harness passed -1024..1023 and 128 wide
  values where an `ai` has a 10-bit signed field (-512..511). Such old reference files are not comparable any more; rebuild
  the scalar reference from a current toolkit.)
- hardware != interp (== scalar or not): the toolkit's semantics of the op are wrong (or the console is not a plain Cell:
  see the per-launch `META,launchN_spuM_fpscr` rows).
- probe A (the five DP compares, expected NOT executed) and the 789 words of probe B have their own sections.

`compare_exhaustive.py` (the host harness' own tool) reads the hash rows of a result file with `--no-localise`: its reader
takes four tab-separated fields and skips blank and `#` rows, so reduce a result file to the lines that contain a tab first.

## 6a. Lanes: per-lane dumps of chosen blocks

A hash says that a block differs, not how. `lanes=<op>:<domain>:<block>[:<count>[:<start>]]` (repeatable, at most 16 entries,
`count` 1..1024, default 64, `start` the first call counted from the start of the block, default 0, `start + count` inside the
block) makes the probe dump, for `count` generator calls of that block from call `start`, the operands `a` `b` `c`, the
immediate field value patched into the instruction word and the result register of the real instruction, as
`LANE,<op>,<domain>,<block>,<call>,<a>,<b>,<c>,<imm>,<out>` rows (`FORMAT.md`). The op must be one the domain runs and the block
must exist, or the configuration is rejected at start-up (`cflts@0:w32:0`: op names contain `@`, never `:`). Each entry is
one launch on one SPU, bounded by `launch_timeout_ms` like a sweep launch (a lanes job runs for milliseconds: the config sets
60000), with the same FATAL row when a launch cannot be contained; a faulting entry becomes a `LANES_FAULT` row and the next entry
runs. Rows are flushed per entry. The generator, the op table, the immediate patching and the stub are the sweep's
(`spu_main.c`: `gen_chunk`, `op_word`), so the dump sees the inputs the hashes saw. The five DP compares are not ops (probe A
has them): a `lanes=dfceq:...` entry is an unknown op and the configuration is rejected at start-up. Lanes run after the special probes and
before the sweep; `cfg/lanes_conv.cfg` runs lanes alone (`sweep=0 special_a=0 special_b=0 spus=1`) for the conversions the sweep
found different from the toolkit: `cflts`/`cfltu` at I8 scale 0, 1, 2 on `w32` block 0 (256 calls), the scale-173 control
(64 calls) and `fesd` on `w32e` block 0 (256 calls).

Console run and comparison:

```
tools/hwprobe/build.sh spuexh layer1
tools/hwprobe/ps3console.py run tools/hwprobe/out/spuexh_l1.fake.self --prefix spuexh --probe spuexh --run-window 60 \
    --cfg spuexh tools/hwprobe/probes/spuexh/cfg/lanes_conv.cfg --timeout 300 --out results
python3 tools/hwprobe/probes/spuexh/lanes_compare.py results/spuexh_r001_lanes_conv.csv \
    --scalar-bin build/dev/spu_exh/test_spu_exhaustive.scalar --md lanes_conv.md
```

The dry run of the same configuration (the interpreter's lanes; needs the SPU ELF that `build.sh` left in `spu/`, and the host
driver built as in `dryrun.sh`):

```
probes/spuexh/host/spuexh_host --elf probes/spuexh/spu/spuexh.elf --cfg probes/spuexh/cfg/lanes_conv.cfg --out lanes_dry.csv
python3 tools/hwprobe/probes/spuexh/lanes_compare.py lanes_dry.csv --scalar-bin <scalar build> --only-diff
```

`lanes_compare.py` runs `<scalar-bin> --dump <op> <domain> <block> --dump-start <first call> --dump-n <calls>` per group (the
options need the harness of the commit that added them) and first checks that every dumped call has the generator's operands and
immediate; a
difference there (the generator on the SPU, or the op table, is at fault) exits 1 and makes the lanes of that group
meaningless. Then it compares hardware with the harness' `out_fast` (the model the sweep hashes) per 32-bit lane - for `fesd`
per 64-bit result lane, fed by input words 0 and 2 - and prints every lane (`--only-diff`: only the differing ones) with its input
word, and per op how many lanes are equal or differ, with the differing lanes grouped by the input word's class read as an IEEE
single (sign, exponent, fraction). Differences are the data: the exit status is 0 for them, 2 for a usage, file or format error.

**Finding which calls differ: `lanesum=`.** A lanes dump covers at most 1024 calls per entry and 16 entries; a block has 2^20.
`lanesum=<op>:<domain>:<block>:<chunk>` (repeatable, at most 16 entries, at most 4096 chunks per block, so `chunk` >= 256 for a
full block) hashes the block in chunks of `chunk` calls on the SPU - with the sweep's own `op_chunk`, so a chunk's hash is the
sweep's block hash restricted to the chunk (`chunk` = 2^20 is the sweep's hash) - and writes one
`LANESUM,<op>,<domain>,<block>,<start>,<count>,<hash>` row per chunk. `lanesum_compare.py` gets the model's chunk hashes from
`<scalar-bin> --dump <op> <domain> <block> --dump-hash <chunk>`, lists the differing chunks with their call ranges and prints the
`lanes=` lines that dump the first differing chunk (exit 0 for differences, 1 when the hardware's chunking is not the harness',
2 for errors). The usual loop: lanesum with chunk 4096, `lanes=...:1024:<start>` entries for a differing chunk, `lanes_compare.py`
on that result.

```
tools/hwprobe/ps3console.py run tools/hwprobe/out/spuexh_l1.fake.self --prefix spuexh --probe spuexh --run-window 60 \
    --cfg spuexh tools/hwprobe/probes/spuexh/cfg/lanesum_fesd.cfg --timeout 300 --out results
python3 tools/hwprobe/probes/spuexh/lanesum_compare.py results/spuexh_r001_lanesum_fesd.csv \
    --scalar-bin build/dev/spu_exh/test_spu_exhaustive.scalar --md lanesum_fesd.md
```

**Any conversion scale.** `cflts@N`, `cfltu@N`, `csflt@N` and `cuflt@N` take any N in 0..255 in `lanes=` and `lanesum=` (the I8
field is patched into the instruction word; the sweep's table only has twelve scales); the host harness' `--dump` takes the same
names. `cfg/lanes_conv2.cfg` dumps `cflts` at 3 10 20 30 40 44 45 46 47 60 100 125 and `cfltu` at 3 45 46 47 (w32 block 0, 256
calls each) to find where the hardware's rule changes; `cfg/lanesum_fesd.cfg` hashes `fesd` on `w32e` block 0 in chunks of 4096 and
holds a commented `lanes=` template for the chunks found.

## 7. Reading the special probes

**How the SPU reports an invalid instruction (measured).** Evidence tier: hardware, direct - the `specialA` run r002 on a CECHC04,
firmware 4.81 REBUG (`CEX REBUG Cobra 7.5`), words `dfceq dfcmeq dfcgt dfcmgt dftsv` (`SPECA_VALID,<op>,NOT_executed` x 5). An
instruction word the SPU does not decode stops that SPU thread and lv2 raises an **SPU error event on the thread group's
exception queue** (`sysSpuThreadGroupConnectEvent(group, queue, SPU_THREAD_GROUP_EVENT_EXCEPTION)`, which every group the probe
starts has; a group without it would lose the whole process). What r002 recorded per word:

- the group did not end by itself: the probe took the terminate-then-join path (`META,terminate_join,launchN`), and the join
  returned 0 with **cause 4** (`SPU_EXCEPTION_SPU_ERROR` in PSL1GHT's `sys/spu.h`), `status=0x7e57` and `exit=0xffffffff` (the
  thread's exit-status field as the probe logged it for a thread that had not exited); the row text:
  `join 0x00000000 cause=4 status=0x7e57 exit=0xffffffff FAULT terminate-join event[src=...]`;
- the terminate itself succeeded (the row text ends `terminate 0x000...`; the text field is cut at 200 characters, so the
  rest of the terminate code is not in the file);
- `sysSpuThreadGetExitStatus` on the stopped thread returned **`0x8001000F` (`CELL_ESTAT`)**: one
  `META,exit_status_pending,0x8001000f` row in the file. A stopped thread therefore does not report an exit status, and the
  strict policy above never reads a non-zero code as a result;
- the event words are `src=ffffffff53505503` and `d1` = `0x0403010000030100` for the first word (`dfceq`; the last bytes count up
  per word), `d2=0x11fb400000004`, `d3=0`, as lv2 delivered them.

Not measured: the SPU_Status register itself (no way to read it from the PPU after lv2 took the thread), and Sony documents no
description of the event words in this tree; the Linux/libspe2 names (`SPU_STATUS_INVALID_INSTR 0x20`, `SPE_SPU_INVALID_INSTR`)
are the analogue, not evidence for this lv2. A probe-B word that is valid on the hardware falls through and is recorded
with the registers it changed; one that is not ends as above and the row's text holds the join cause and status and the event words
`SPECB_TRY` is flushed before each word starts. If no outcome follows, the result is incomplete and the analyzer reports a
suspected freeze. Resume from that same prefix's index, not the next index; `special_b_from` is inclusive. The prefix's
hexadecimal value is its stable identity across generated-list changes, so map it to the current build's index before resuming.

**Probe A.** The five words `dfceq dfcmeq dfcgt dfcmgt dftsv` are no longer ops of the sweep (the toolkit decodes them as an invalid
instruction, #124), so probe A has them as a fixed list in `common/spuexh_core.c` (`0x78610183 0x79610183 0x58610183
0x59610183 0x77E00183`, the last with the 7-bit mask at bit 14; `verify_encodings.py` assembles each with spu-as and compares).
The first case of each runs alone: `SPECA_VALID,<op>,NOT_executed` is the expected answer (hardware, CECHC04 4.81, r002: all five
end as an invalid instruction, and the interpreter's dry run gives the same). An `executed` row means the hardware executes a word
the toolkit decodes as invalid: the remaining cases then run and their operands and result quadwords are in the `SPECA` rows, and
`compare_spuexh.py` reports a REGRESSION and exits 1. The probe stays in `cfg/specialA.cfg`, `smoke`, `standard` and `full` as that
regression check; it no longer gates anything in the sweep (no `SKIP` rows, no `force_dpcmp`).

**Probe B.** `specialb_report.py` reports one row per tested prefix and pattern; the toolkit expectation is
`invalid->stop` for each prefix in the generated list. Since #124 the list contains **789** prefixes (the previous 784 plus
probe A's five DP prefixes). `special_b_count` limits prefixes, and `both` runs zero and random words for every selected
prefix. `special_b_from` is an inclusive index into the sorted generated list and must be below the effective prefix total.
Use the logged hexadecimal prefix as the stable identity; if the generated list changed between builds, map that value to
its index in the current list. After a missing outcome, resume from the same `SPECB_TRY` index, not index+1; in both mode
the resumed prefix runs zero and random again, so neither the attempted word nor its partner is skipped.

## 8. Cautions

- Probe B executes instruction words that no SPU documentation in this tree describes. They run inside the local store of one
  SPU thread; the registers are reloaded afterwards from a copy, and scratch memory is separate from the program. Nothing the
  probe does is aimed at the PPU side, but a word that is valid and has an unexpected side effect (a channel write, a stop with
  a signal) would be contained to that SPU thread's group: the probe ends the group, records the word, and (if the
  group ends cleanly) moves on; it cannot promise what the undecoded word itself did. Run the special probes with the
  console otherwise idle, one staged batch at a time (see section 4), and not before `stage0` and `smoke` passed.
- The sweep is silent on the screen except for a counter (`hashes=`, `jobs=`, elapsed seconds); drawing happens between SPU
  polls and does not influence a hash.
- The first full run is a long unattended run: use `--timeout` generously; the probe ends itself.
