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
| `FATAL,cfg,text` | the configuration was rejected at start-up (unknown key, line without `=`, bad value; RUN.md section 3): the probe wrote this row into a result file that holds only the header and stopped before any SPU launch. Not written when the error is a bad `outdir` |
| `TIMING_OVERFLOW,domain,block` | the job ran past one decrementer period (~53.8 s at 79.8 MHz): its tick count would undercount, so it is not reported as a number (the job's hash rows are still valid); `META,timing_overflows` counts them |
| `SKIP,op,reason` | no longer written. Result files from before #124 carry `SKIP,<op>,rejected_by_special_a` / `special_a_not_run` rows for the five DP compares, which the sweep then skipped; since #124 they are not sweep ops and nothing is skipped. `compare_spuexh.py` still lists them |
| `SPECA_VALID,op,executed\|NOT_executed,word_W,started_K,text` | special probe A, first case of an op alone in its own thread group |
| `SPECA,op,word,case,a,b,result` | special probe A case: instruction word (hex), case number, the operands in registers `$3` and `$4` (32 hex digits each, the quadword) and `$rt` after the instruction |
| `SPECA_FAULT,op,word,case,a,b,text` | a case that stopped the SPU when run alone |
| `SPECB_TRY,index,prefix,zero\|rand,word` | special probe B, pre-log: flushed before the word's thread group starts, so a lost run names the word that was being attempted even when no outcome row follows |
| `SPECB,index,prefix,zero\|rand,word,outcome,nchanged,regs,arena,arena_addr,text` | special probe B outcome (below) |
| `LANES,op,domain,block,count,imm_bits,start` | lanes job (RUN.md "Lanes"), pre-log: flushed before the entry's launch. `count` calls of the block are dumped from call `start` of the block (`start` counts from the start of the block, `LANE` rows carry the call index within the domain, `block << 20` + position), `imm_bits` is the width of the op's immediate field (0 = none). `op` is the name as configured: a conversion at any I8 scale 0..255 (`cflts@45`) is accepted, not only the sweep's twelve scales. The first lanes mode wrote the row without `start`; `lanes_compare.py` reads both |
| `LANE,op,domain,block,call,a,b,c,imm,out` | one dumped call of a lanes job: `call` is the generator call index within the domain (the first column of the host harness' `--dump`, so it includes `block << 20`), `a` `b` `c` the operands the generator produced and `out` the result register after the instruction, each a quadword as 32 hex digits in big-endian word order (word 0 first, as `--dump` prints them); `imm` is the immediate field value actually patched into the instruction word (the call's immediate modulo the field width, decimal), or `-` for an op without an immediate field |
| `LANES_DONE,op,domain,block,rows` | the entry's launch finished and all `count` rows are above |
| `LANES_FAULT,op,domain,block,rows,text` | the entry's launch did not finish cleanly (`rv_`, SPU `state_` and `flags_` words, what lv2 reported); `rows` records are valid (a record is in the file only after its chunk of up to 64 calls reached main memory) and the run goes on with the next entry. A `FATAL` row ends the probe as for the sweep. A lanes file without a closing row was lost mid-entry |
| `LANESUM_BEGIN,op,domain,block,chunk,nchunks` | lanesum job (RUN.md "Lanes"), pre-log: flushed before the entry's launch. The block is hashed in `nchunks` chunks of `chunk` calls (the last one is shorter when `chunk` does not divide the block) |
| `LANESUM,op,domain,block,start,count,hash16` | one chunk of a lanesum job: the sweep's block hash (FNV-1a-64 over the outputs, started from its offset basis for every chunk) restricted to the `count` calls from call `start` of the block (`start` counts from the start of the block, unlike the `call` of a `LANE` row). With `chunk` = the whole block it is the sweep's hash of that block |
| `LANESUM_DONE,op,domain,block,rows` / `LANESUM_FAULT,op,domain,block,rows,text` | as `LANES_DONE` / `LANES_FAULT` for a lanesum entry: `rows` chunk hashes are valid (they reach main memory 64 at a time and at the end of the block) |

The block hash is the host harness' (`runtime/spu/tests/spu_exh_gen.h`): FNV-1a-64 over the 64-bit words of the outputs of the
2^20 calls of a block, `op` and `domain` are the host harness' names (`cflts@173` is `cflts` with the I8 field 173).
A hash row is in the host harness' line format; `compare_exhaustive.py` takes four tab-separated fields and skips blank and
`#` rows, so a result file is reduced to its hash rows first (`compare_exhaustive.py rows.txt ref.txt --no-localise`);
`compare_spuexh.py` reads a result file unchanged and is the three-way comparison.

Values contain no commas (free text has them replaced by spaces) and no line breaks.

### META keys

`backend` (`ppu`, `host-interp`), `build`, `run`, `preset`, `plan_jobs`, `plan_hashes`, `plan_calls`, `spus_requested`, `lanes` and `lanesums` (number of `lanes=` and `lanesum=` entries),
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

Executes `dfceq`, `dfcmeq`, `dfcgt`, `dfcmgt` and `dftsv` as raw words. They are not ops of the sweep (the toolkit decodes them as
an invalid instruction, #124): the five words are a fixed list in `common/spuexh_core.c` (opcodes of `tools/spu_disasm.py`,
registers `$3`/`$4`, result in `$3`), which `verify_encodings.py` checks against the GNU SPU assembler. For each op the first case
runs alone in its own SPU thread group; the expected answer is `SPECA_VALID ... NOT_executed` (hardware and interpreter agree). If
a SPU executes it (`SPECA_VALID ... executed`: a regression of the toolkit's decode, `compare_spuexh.py` reports it), the remaining
cases run in batches (one group per at most 96
cases; a batch that does not complete is re-run case by case). The cases are all ordered pairs of 25 double values (both
zeros, the minimum, a middle and the maximum denormal in both signs, the two denormals of the EXHAUSTIVE.md 3.4
repros, the minimum normal, ordinary values, the maximum, both infinities, quiet and signalling NaNs), two lanes per call, and
for `dftsv` every 7-bit mask on the same values.

## Special probe B

For each of the 789 11-bit prefixes the toolkit's decode table maps to `SPU_word` (784 before #124, plus the five prefixes of
probe A's DP compares, `0x2c3 0x2cb 0x3bf 0x3c3 0x3cb`; generated from
`runtime/spu/spu_interp_tables.inc` at build time, `spuexh_badprefix_gen.h`) two words are executed, each in its own thread
group: the prefix with all operand bits zero (`zero`) and with a fixed pseudo-random operand pattern (`rand`). The staging
keys narrow the batch: `special_b_count=N` stops after N words, `special_b_patterns=zero` runs only the zero pattern (the
first run is one zero-pattern word; RUN.md section 4 has the batch sequence). A `SPECB_TRY` row is flushed before each
word's group starts. The `index` of the `SPECB`/`SPECB_TRY` rows is the position of the prefix in the sorted list of 789 (a file
from before #124 numbers 784: the same below `0x2c3`, shifted above it; compare by prefix, not index). The word runs
with all 128 registers loaded with known values (word 0 of register n = `0x1E000 + 16n`, so a register used as an address
points into scratch memory) and is followed by code that does not depend on any register: it stores all 128 registers and
returns. Outcome:

| outcome | meaning |
|---|---|
| `executed` | the word fell through; `nchanged` registers differ from their loaded value (the first 8 are listed as `reg:quadword`), `arena` is 1 or 2 when scratch memory was written (with the address) |
| `no_return` | the thread was started and the word did not fall through: invalid-instruction exception, a branch, a stop or a hang; `text` has what lv2 reported |
| `not_started` | the thread did not get to the word |

How the SPU reports an invalid instruction: see `RUN.md` "Reading the special probes".
