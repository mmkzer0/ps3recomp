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
| `special_a=0\|1`, `a_ops=`, `special_b=0\|1`, `special_b_max=N`, `special_b_from=N`, `sweep=0\|1` | the two special probes and the sweep, each switchable; `a_ops` narrows probe A to a comma list of its five words; `special_b_from` resumes a lost probe B |
| `special_b_count=N`, `special_b_patterns=zero\|both` | probe B staging: stop after N words (0 = every word) and use only the zero operand pattern or both; the first B run is `special_b_count=1 special_b_patterns=zero` (one zero-pattern word) |
| `lanes=OP:DOMAIN:BLOCK[:COUNT[:START]]`, `lanesum=OP:DOMAIN:BLOCK:CHUNK` | per-lane dump of COUNT (default 64, max 1024) calls of a block from call START (default 0), and one hash per chunk of CHUNK calls over a whole block; each repeatable up to 16 times, conversions at any I8 scale (`cflts@45`), see section 6a; set after `preset=` |
| `stall_s` (120), `max_faults` (24), `case_timeout_ms` (2000), `b_timeout_ms` (600), `launch_timeout_ms` (10800000) | hang detection and limits. Every launch kind is bounded end to end: `case_timeout_ms`, `b_timeout_ms` and `launch_timeout_ms` must be positive (the probe rejects 0 and negatives at start-up); `stall_s >= 0` (0 = rely on the launch limit alone) |
| `pad`, `hold_s`, `screen`, `tag`, `outdir`, `fw_version`, `fw_type`, `console_model`, `progress_file` | as the other probes (declare the console so it reaches the result header); `outdir` must stay inside `/dev_hdd0/tmp/hwprobe` |

**The configuration fails closed.** An unknown key (a typo such as `special_bb=1` or `launch_timeout=...`), a non-blank line
without `=` and a bad value (`special_a`, `special_b` and `sweep` take exactly 0 or 1) stop the probe at start-up, before any SPU
is started; the first error of the file is reported and nothing falls back to a default. On the console the error is shown on
the screen and written as `FATAL,cfg,<text>` (for example `FATAL,cfg,unknown key 'special_bb'`) into a result file of its own
(the screen may be off, `screen=0`); a bad `outdir` is the exception: nothing is written outside `/dev_hdd0/tmp/hwprobe`, so the
error is only on the screen. The host driver exits 2 with the same text on stderr, before it opens the ELF or the output.

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
| `special` (probes A and B, no sweep) | | | | | 1 to 15 min | about 1600 thread groups (5 of probe A, 789 x 2 of probe B); the time is the group launch cost, unknown |

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
2. `specialA` (`cfg/specialA.cfg`): probe A alone (the five DP compares as raw words; expected: none executes).
3. `smoke` (`cfg/smoke.cfg`): block 0 of every domain and op, with probe A as the regression check.
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
(`event[src= d1= d2= d3=]`). A `SPECB_TRY`
row (prefix index, pattern, exact word) is flushed before each word's group starts, so a lost run names the word that
was being attempted even when no outcome row followed; resume with `special_b_from=<index of the last SPECB_TRY row + 1>`.

**Probe A.** The five words `dfceq dfcmeq dfcgt dfcmgt dftsv` are no longer ops of the sweep (the toolkit decodes them as an invalid
instruction, #124), so probe A has them as a fixed list in `common/spuexh_core.c` (`0x78610183 0x79610183 0x58610183
0x59610183 0x77E00183`, the last with the 7-bit mask at bit 14; `verify_encodings.py` assembles each with spu-as and compares).
The first case of each runs alone: `SPECA_VALID,<op>,NOT_executed` is the expected answer (hardware, CECHC04 4.81, r002: all five
end as an invalid instruction, and the interpreter's dry run gives the same). An `executed` row means the hardware executes a word
the toolkit decodes as invalid: the remaining cases then run and their operands and result quadwords are in the `SPECA` rows, and
`compare_spuexh.py` reports a REGRESSION and exits 1. The probe stays in `cfg/specialA.cfg`, `smoke`, `standard` and `full` as that
regression check; it no longer gates anything in the sweep (no `SKIP` rows, no `force_dpcmp`).

**Probe B.** One word per undecoded prefix and operand pattern; read the table in the comparison output. Since #124 the toolkit
decodes the five DP prefixes of probe A (`0x2c3 0x2cb 0x3bf 0x3c3 0x3cb`) as undecoded too, so the list has **789** prefixes (the
784 of before and these five) and probe B runs them as well. The prefix index of `special_b_from=N` and of the `SPECB`/`SPECB_TRY`
rows is the position in that sorted list: indexes of a run from before #124 (784 prefixes) are the same below prefix `0x2c3` and
shifted by one to five above it, so resume such a run by prefix, not by index.

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
