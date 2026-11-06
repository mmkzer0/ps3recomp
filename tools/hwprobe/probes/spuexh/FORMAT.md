# FORMAT.md: spuexh result files

One file per run, `spuexh_r<NNN>[_<tag>].csv`, written by the PPU layer on a console (`layer1/source/main.c`) and by the
host dry run (`host/spuexh_host`). Both write exactly this format. The file is opened first and grows while the run goes on
(the supervisor flushes about once a second), so an interrupted run leaves everything computed so far.

## Header

The standard hwprobe header (`common/hwprobe.h`, `../../README.md` "Result header"), written first, then the probe's own
rows. The dry run writes `HDR,fw_type,host-interp` and `HDR,console_model,host`.

## Rows

| Row | Meaning |
|---|---|
| `op<TAB>domain<TAB>block<TAB>hash16` | one block hash, in the line format of the host harness (`test_spu_exhaustive`) |
| `#hwdom<TAB>domain<TAB>jobs=N<TAB>ticks=N` | SPU decrementer ticks (79.8 MHz timebase on a console) spent on the jobs of a domain; written at the end |
| `#gencheck<TAB>domain<TAB>digest16` | generator self-check: a digest of 256 calls of the domain's generator (no op involved), one row per domain, written by SPU 0 at the start of a sweep. `spuexh_host --gencheck` prints the same rows computed natively on the host; a hardware digest that differs means the SPU code of the generator, not an op, is wrong |
| `META,key,value` | facts about the run (below) |
| `FAULT,spuN,domain,block,job_K,state_S,text` | an SPU thread did not finish and its job is lost: the job it was running, its state word (0 never started, 1 running) and what lv2 reported (join cause/status, exception event words, `hang:` when the supervisor ended it). Written only when the thread ended by itself or the operator stopped the probe; after a group-wide kill the unfinished workers are re-run alone first (below) |
| `RETRY,spuN,job_K,ok` | a worker that was unfinished when its group died (exception or supervisor kill) re-ran its unfinished jobs alone and completed them: the group's death was collateral, no FAULT, the hashes are kept |
| `FATAL,launch,text` | a launch could not be contained: the exception queue connect failed, or a terminate returned an error after the exception event / a launch that did not end by itself, so the probe did not join (join has no timeout) - it flushed the file, showed the error and stopped; the text names the lv2 error |
| `TIMING_OVERFLOW,domain,block` | the job ran past one decrementer period (~53.8 s at 79.8 MHz): its tick count would undercount, so it is not reported as a number (the job's hash rows are still valid); `META,timing_overflows` counts them |
| `SKIP,op,reason` | an op the configuration enabled was not run: `rejected_by_special_a` (probe A: the SPU did not execute it), `special_a_not_run` |
| `SPECA_VALID,op,executed\|NOT_executed,word_W,started_K,text` | special probe A, first case of an op alone in its own thread group |
| `SPECA,op,word,case,a,b,result` | special probe A case: instruction word (hex), case number, the operands in registers `$3` and `$4` (32 hex digits each, the quadword) and `$rt` after the instruction |
| `SPECA_FAULT,op,word,case,a,b,text` | a case that stopped the SPU when run alone |
| `SPECB_TRY,index,prefix,zero\|rand,word` | special probe B, pre-log: flushed before the word's thread group starts, so a lost run names the word that was being attempted even when no outcome row follows |
| `SPECB,index,prefix,zero\|rand,word,outcome,nchanged,regs,arena,arena_addr,text` | special probe B outcome (below) |

The block hash is the host harness' (`runtime/spu/tests/spu_exh_gen.h`): FNV-1a-64 over the 64-bit words of the outputs of the
2^20 calls of a block, `op` and `domain` are the host harness' names (`cflts@173` is `cflts` with the I8 field 173).
A hash row is in the host harness' line format; `compare_exhaustive.py` takes four tab-separated fields and skips blank and
`#` rows, so a result file is reduced to its hash rows first (`compare_exhaustive.py rows.txt ref.txt --no-localise`);
`compare_spuexh.py` reads a result file unchanged and is the three-way comparison.

Values contain no commas (free text has them replaced by spaces) and no line breaks.

### META keys

`backend` (`ppu`, `host-interp`), `build`, `run`, `preset`, `plan_jobs`, `plan_hashes`, `plan_calls`, `spus_requested`,
`spus` (threads used), `spus_unavailable` (a thread count the system refused), `special_a`, `special_b`, `sweep`, `gen_seed`
(`EXH_SEED`), `special_b_prefixes`, `special_b_from`, `special_b_patterns` (`zero`/`both`), `special_b_count`, per launch and
thread `launchN_spuM_fpscr` (`fscrrd` words read back after the SPU program set the FPSCR to the defined state at thread
start: all status bits clear, DP rounding round-to-nearest-even; one row per launch, the special probes included),
`terminate_join` (`launchN`: the group had to be terminated before it could be joined - it had not ended by itself, or an
exception event arrived), `exit_status_pending` (the non-zero code `sysSpuThreadGetExitStatus` returned for a thread that
had not exited; first occurrence per code, unverified lv2 semantics documented by these rows), per SPU
`spuN_fpscr_end` (the sticky flags the last launch ended with), `spuN_busy_ticks`, `spuN_dead`, and at the end
`hashes`, `jobs_done`, `faults`, `relaunches`, `timing_overflows`, `completed` (every SPU finished), `aborted`, `wall_s`,
`exit_requested`, `sweep_error` (`allocation_failed`, `max_faults_reached`, `stopped_by_operator`, `fatal_launch`,
`backend_run_failed_N`, `spu_init_failed`), and the `ui_*` rows of the hwprobe library. The dry run adds `interp_steps`.

A run is complete when `META,completed,1`, `META,aborted,0` and `META,faults,0`; a `FAULT` row is not an error of the
probe, it is data (an SPU instruction the hardware does not accept, or a hang), and the sweep continues after it. A
`FATAL` row is the opposite: the probe stopped itself and the run is incomplete by design.

## Special probe A

Executes `dfceq`, `dfcmeq`, `dfcgt`, `dfcmgt` and `dftsv` as raw words (the encodings come from the toolkit's decode table and
were checked against the GNU SPU assembler, `verify_encodings.py`). For each op the first case runs alone in its own SPU thread
group; if the SPU executes it (`SPECA_VALID ... executed`), the remaining cases run in batches (one group per at most 96
cases; a batch that does not complete is re-run case by case). The cases are all ordered pairs of 25 double values (both
zeros, the minimum, a middle and the maximum denormal in both signs, the two denormals of the EXHAUSTIVE.md 3.4
repros, the minimum normal, ordinary values, the maximum, both infinities, quiet and signalling NaNs), two lanes per call, and
for `dftsv` every 7-bit mask on the same values.

## Special probe B

For each of the 784 11-bit prefixes the toolkit's decode table maps to `SPU_word` (generated from
`runtime/spu/spu_interp_tables.inc` at build time, `spuexh_badprefix_gen.h`) two words are executed, each in its own thread
group: the prefix with all operand bits zero (`zero`) and with a fixed pseudo-random operand pattern (`rand`). The staging
keys narrow the batch: `special_b_count=N` stops after N words, `special_b_patterns=zero` runs only the zero pattern (the
first run is one zero-pattern word; RUN.md section 4 has the batch sequence). A `SPECB_TRY` row is flushed before each
word's group starts. The word runs
with all 128 registers loaded with known values (word 0 of register n = `0x1E000 + 16n`, so a register used as an address
points into scratch memory) and is followed by code that does not depend on any register: it stores all 128 registers and
returns. Outcome:

| outcome | meaning |
|---|---|
| `executed` | the word fell through; `nchanged` registers differ from their loaded value (the first 8 are listed as `reg:quadword`), `arena` is 1 or 2 when scratch memory was written (with the address) |
| `no_return` | the thread was started and the word did not fall through: invalid-instruction exception, a branch, a stop or a hang; `text` has what lv2 reported |
| `not_started` | the thread did not get to the word |

How the SPU reports an invalid instruction: see `RUN.md` "Reading the special probes".
