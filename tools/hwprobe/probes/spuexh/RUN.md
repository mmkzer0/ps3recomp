# RUN.md: spuexh on a real console

spuexh runs the SPU instruction sweep of the host harness (`runtime/spu/tests/test_spu_exhaustive.c`) on the SPUs of a console.
The generator, the domains and the block hash are the host harness' own source file, compiled unchanged for the SPU
(`runtime/spu/tests/spu_exh_gen.h`), so a hash computed here is directly comparable with the host harness' hash of the
same op, domain and block. A console run is the golden reference: where the toolkit disagrees with it, the toolkit is wrong.
The probe only executes SPU instructions inside the SPU's local store and DMAs results to its own memory; it patches
nothing, calls no system function beyond the SPU thread group API and writes only below `/dev_hdd0/tmp/hwprobe/`:
`outdir` must be that directory or below it (no `..` components, checked at start-up), and a result file that cannot
be opened there ends the probe with an error - there is no fallback device.

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
PS3DEV=<prefix> python3 tools/hwprobe/probes/spuexh/verify_encodings.py .      # 198 ops checked against spu-as, 0 mismatches
```

## 2. Dry run without a console (do this first)

The very SPU ELF that goes into the SELF runs in the toolkit's interpreter and writes the file the console run writes:

```
PS3DEV=<prefix> tools/hwprobe/probes/spuexh/dryrun.sh smoke OUT_DIR       # builds the ELF and the driver, runs, compares with scalar.txt
```

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
| `special_a=0\|1`, `a_ops=`, `special_b=0\|1`, `special_b_max=N`, `special_b_from=N`, `sweep=0\|1` | the two special probes and the sweep, each switchable; `special_b_from` resumes a lost probe B; `force_dpcmp=1` runs dfceq/dfcmeq/dfcgt/dfcmgt/dftsv in the sweep without probe A's acceptance |
| `special_b_count=N`, `special_b_patterns=zero\|both` | probe B staging: stop after N words (0 = every word) and use only the zero operand pattern or both; the first B run is `special_b_count=1 special_b_patterns=zero` (one zero-pattern word) |
| `lanes=OP:DOMAIN:BLOCK[:COUNT]` | per-lane dump of the first COUNT (default 64, max 1024) calls of a block, repeatable up to 16 times, see section 6a; set after `preset=` |
| `stall_s` (120), `max_faults` (24), `case_timeout_ms` (2000), `b_timeout_ms` (600), `launch_timeout_ms` (10800000) | hang detection and limits. Every launch kind is bounded end to end: `case_timeout_ms`, `b_timeout_ms` and `launch_timeout_ms` must be positive (the probe rejects 0 and negatives at start-up); `stall_s >= 0` (0 = rely on the launch limit alone) |
| `pad`, `hold_s`, `screen`, `tag`, `outdir`, `fw_version`, `fw_type`, `console_model`, `progress_file` | as the other probes (declare the console so it reaches the result header); `outdir` must stay inside `/dev_hdd0/tmp/hwprobe` |

### Launch and termination policy

Every launch (`launch_timeout_ms`, or `case_timeout_ms`/`b_timeout_ms` for the special probes) is bounded while the
threads run, and the stall detector (`stall_s`) ends a hung group earlier. Everything after the wait is bounded too:
neither a group-exception event nor a thread's final status block proves that lv2 has ended the threads, and
`sysSpuThreadGroupJoin` waits for group completion with no timeout (`sys/spu.h:875-889`). So before any join the probe
polls `sysSpuThreadGetExitStatus` for every thread until all report exited or the launch's remaining deadline passes
(never less than 2 s from the launch start); if an exception event arrived or a thread has not exited then, the group is
terminated with `sysSpuThreadGroupTerminate`, and only terminate 0 (recorded as `META,terminate_join,launchN`, plus
`terminate-join` in the thread's text) allows the join. A terminate error leaves a group of unknown state - and join has
no timeout - so the probe does **not** join: it writes a `FATAL,launch,...` row with the lv2 code (flushed), records
`META,sweep_error,fatal_launch`, shows the error and exits non-zero. The result file holds everything computed so far,
and process exit lets lv2 reclaim the live group. A launch whose exception-queue connect fails is a launch failure: the
unstarted group is destroyed and nothing runs.

What lv2 returns from `sysSpuThreadGetExitStatus` for a running or exception-stopped thread is **unverified in this
tree** (PSL1GHT's `sys/spu.h` documents only "zero if no error occured, nonzero otherwise"): the probe records every
non-zero code in `META,exit_status_pending,0x...` rows (first occurrence per code), so the first hardware run settles the
semantics. Both the FATAL text and those rows are the evidence the strict policy is based on.

The stage configs set `screen=0`: the on-screen status path can wait unboundedly (RSX label wait in `common/hwprobe_ui.c`,
`rsxFinish` at shutdown), so a console stage runs without the screen; the result file is the record.

The dp compares are only run in the sweep when probe A showed that the SPU executes them (an unaccepted op gets a
`SKIP,<op>,rejected_by_special_a` row); with `special_a=0` they are skipped too (`special_a_not_run`).

Plan and cost per configuration. Jobs, hashes and op applications are exact (`spuexh_host --plan`); the time is an
ESTIMATE from the number of SPU instructions the interpreter executes for one job of each domain (summed over the
plan, `estimate_runtime.py`), divided by 6 SPUs and by an assumed 0.5 to 1.0 instructions per cycle at 3.2 GHz. Nothing has been measured on a console.

| config | jobs | hashes | op applications | SPU instructions | estimated sweep time | note |
|---|---|---|---|---|---|---|
| `smoke` (block 0 of everything) | 37 | 315 | 2.7e8 | 8.4e10 | 4 to 9 s | |
| `standard` (every 64th block) | 181 | 1445 | 1.5e9 | 4.2e11 | 22 to 44 s | |
| `priority` (frest frsqest fi and the 48 conversions, every block) | 1056 | 51232 | 5.4e10 | 7.5e12 | 6.5 to 13 min | the ops no independent oracle exists for |
| `full` (everything, every block) | 9988 | 79026 | 8.3e10 | 2.3e13 | 20 to 41 min | |
| `special` (probes A and B, no sweep) | | | | | 1 to 15 min | about 1600 thread groups; the time is the group launch cost, unknown |

(`estimate_runtime.py <dir of per-domain dry-run files> <spuexh_host> smoke standard cfg/priority.cfg full` reproduces the table.)
The interpreter runs about 1.5e8 SPU instructions per second per host core, so `dryrun.sh standard` takes about 15 minutes
on 3 cores; `full` is not practical to dry-run.

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

Recommended order of the first session (inspect the result file between every step; each step only runs when the
previous one was clean - `META,completed,1`, no `FATAL` row, expected hash count):

1. `stage0` (`cfg/stage0.cfg`): one SPU, integer-only domains and ops, block 0 of everything, no special probes.
   Proves the SPU program, the thread groups, the exception queue, the result file and bounded termination on
   known integer code.
2. `specialA` (`cfg/specialA.cfg`): probe A alone (the five DP compares as raw words).
3. `smoke` (`cfg/smoke.cfg`): block 0 of every domain and op, with probe A.
4. `standard` (`cfg/standard.cfg`), then `priority` (`cfg/priority.cfg`) and `full` (`cfg/full.cfg`) - set
   `--timeout` to 3600 or 7200 for these; the run window stays 60 because the probe creates its files at once.
5. Probe B in batches, one `specialb*.cfg` at a time, each word in its own thread group: first exactly one word
   (`cfg/specialb_first.cfg`: prefix 0x004, zero pattern), then 4 prefixes zero, then 16 prefixes zero
   (`special_b_from=0 special_b_count=4|16 special_b_patterns=zero`), then - only after the zero-pattern batches
   are understood - the random patterns (`special_b_patterns=both`), widening `special_b_count` the same way.
   A `SPECB_TRY` row is flushed before each word runs, so a lost batch names the word it was attempting.
While a run is in progress `spuexh_last.txt` reads `(running)` and the summary line (hashes so far); the result file is open
and grows. After a power loss or a lost process the file holds everything computed so far: rerun with a narrower
configuration (`doms=`, `ops=`) for what is missing.

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
more (the old `ref_encodable.py` is gone). `--interp` is the same
probe run by `spuexh_host` (the toolkit's interpreter on the very SPU program). `--rpcs3` is any further file in the same line
format. `--gen` is the output of `spuexh_host --gencheck` (default: the `#gencheck` rows of the `--interp` file); the generator
self-check is the first gate: every domain measured on the hardware needs a digest in the hardware file and in the reference,
and the digests must agree - a measured domain without evidence on either side, a digest disagreement or disjoint evidence
sets fail the comparison (exit 1), because a generator that computes other inputs on the SPU makes every op verdict of that
domain meaningless. Ops of such unvalidated domains carry a `gencheck unvalidated` label in the verdict table. Only blocks
present in both files are compared. The output is a per-op table (blocks compared / differing against
each reference, verdict `MATCH`, `HW!= <reference>`, `NOT RUN`, `NO COMMON`), the first differing block of every difference, the
special probes' reports, and the exit status is 1 when any op differs from a reference or the generator gate failed.

Reading the three-way result:

- hardware == interp == scalar: the op matches everywhere. (Before dev dbb93d6c the immediate domains of word and halfword
  immediates (`ai`, `ahi`, `il`, ...) differed against an old `scalar.txt` alone: the harness passed -1024..1023 and 128 wide
  values where an `ai` has a 10-bit signed field (-512..511). Such old reference files are not comparable any more; rebuild
  the scalar reference from a current toolkit.)
- hardware != interp (== scalar or not): the toolkit's semantics of the op are wrong (or the console is not a plain Cell:
  see the per-launch `META,launchN_spuM_fpscr` rows).
- the five ops of probe A and the 784 words of probe B have their own sections.

`compare_exhaustive.py` (the host harness' own tool) reads the hash rows of a result file with `--no-localise`: its reader
takes four tab-separated fields and skips blank and `#` rows, so reduce a result file to the lines that contain a tab first.

## 6a. Lanes: per-lane dumps of chosen blocks

A hash says that a block differs, not how. `lanes=<op>:<domain>:<block>[:<count>]` (repeatable, at most 16 entries, `count`
1..1024, default 64) makes the probe dump, for the first `count` generator calls of that block, the operands `a` `b` `c`, the
immediate field value patched into the instruction word and the result register of the real instruction, as
`LANE,<op>,<domain>,<block>,<call>,<a>,<b>,<c>,<imm>,<out>` rows (`FORMAT.md`). The op must be one the domain runs and the block
must exist, or the configuration is rejected at start-up (`cflts@0:w32:0`: op names contain `@`, never `:`). Each entry is
one launch on one SPU, bounded by `launch_timeout_ms` like a sweep launch (a lanes job runs for milliseconds: the config sets
60000), with the same FATAL row when a launch cannot be contained; a faulting entry becomes a `LANES_FAULT` row and the next entry
runs. Rows are flushed per entry. The generator, the op table, the immediate patching and the stub are the sweep's
(`spu_main.c`: `gen_chunk`, `op_word`), so the dump sees the inputs the hashes saw. The dp compares (`dfceq` ...) follow the
sweep's rule: only after probe A accepted them (or `force_dpcmp=1`), else a `SKIP` row. Lanes run after the special probes and
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

`lanes_compare.py` runs `<scalar-bin> --dump <op> <domain> <block> --dump-n <calls>` per group (`--dump-n` needs a harness at or
after the commit that added it) and first checks that every dumped call has the generator's operands and immediate; a
difference there (the generator on the SPU, or the op table, is at fault) exits 1 and makes the lanes of that group
meaningless. Then it compares hardware with the harness' `out_fast` (the model the sweep hashes) per 32-bit lane - for `fesd`
per 64-bit result lane, fed by input words 0 and 2 - and prints every lane (`--only-diff`: only the differing ones) with its input
word, and per op how many lanes are equal or differ, with the differing lanes grouped by the input word's class read as an IEEE
single (sign, exponent, fraction). Differences are the data: the exit status is 0 for them, 2 for a usage, file or format error.

## 7. Reading the special probes

**How the SPU reports an invalid instruction.** Not verified here (no hardware, no Sony documentation in this tree), so
the probe records whatever comes back. What is expected: on a Cell SPU an instruction word the decoder does not know stops the
SPU and sets the *invalid instruction* bit (0x20) of its SPU_Status register and raises the class 0 *SPU error* interrupt
(Linux `arch/powerpc/include/asm/spu.h`, `SPU_STATUS_INVALID_INSTR 0x20`; libspe2 `SPE_SPU_INVALID_INSTR`); lv2 turns it into an
event on the exception queue of the SPU thread group (cause `SPU_EXCEPTION_SPU_ERROR` 0x0004 in PSL1GHT's `sys/spu.h`). A thread
group without a connected exception queue loses the whole process on such an event, which is why every group the probe starts has
one (`sysSpuThreadGroupConnectEvent(group, queue, SPU_THREAD_GROUP_EVENT_EXCEPTION)`). A probe-B word that is valid on the hardware
falls through and is recorded with the registers it changed; one that is not stops the thread, and the row's text holds
the join cause and status and the event words (`event[src= d1= d2= d3=]`) exactly as lv2 delivered them. A `SPECB_TRY`
row (prefix index, pattern, exact word) is flushed before each word's group starts, so a lost run names the word that
was being attempted even when no outcome row followed; resume with `special_b_from=<index of the last SPECB_TRY row + 1>`.

**Probe A.** For each of `dfceq dfcmeq dfcgt dfcmgt dftsv` the first case runs alone: `SPECA_VALID,<op>,executed` means the
SPU executed it; the `SPECA` rows hold the operands and the result quadword of every case. The denormal repros of
EXHAUSTIVE.md 3.4 (`dfceq(0x800A56DA8A8D5917, 0x8008000000000000)`, `dfcgt(0x000F6B9D2B8B96A2, 0x8008000000000000)`) are among the
cases; `compare_spuexh.py` prints them with the hardware result. A result that is neither all-ones nor zero per doubleword
means the word did not act as a compare (an SPU that treats an unknown opcode as a no-op returns operand `a` unchanged).

**Probe B.** One word per undecoded prefix and operand pattern; read the table in the comparison output.

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
