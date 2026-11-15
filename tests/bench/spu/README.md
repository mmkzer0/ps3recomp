# SPU operation benchmark

Operations per second for every SPU operation: lifted (the code the game
runs) and interpreted, on the real SPU runtime (`runtime/spu`).

```sh
tests/bench/spu/run_spu_bench.sh                  # everything (a few minutes)
tests/bench/spu/run_spu_bench.sh --quick          # shorter timing, noisier
tests/bench/spu/run_spu_bench.sh --only fa,fm,dma --no-interp
tests/bench/spu/run_spu_bench.sh --csv results.csv
tests/bench/spu/run_spu_bench.sh --no-barriers    # see what the compiler folds
```

Output goes to `tests/bench/spu/out/` (or `$SPU_BENCH_OUT`).

`gen_spu_bench.py` takes its instruction encodings from `OPCODES`. This tree
carries `spu_ops.py`, the subset the loop control, the memory and branch rows
and the channel, `dma` and `atomic` rows need; the register-operation rows
(adds, shifts, floats, ...) exist only for the mnemonics listed there. Extend
that table with the SPU ISA encodings to measure the rest. The harness is a
single thread: use `--only dma,atomic,wrch,rdch,rchcnt` for the seam-relevant
rows (the effect port adds one load and branch to each of them).

## What is measured

`gen_spu_bench.py` writes one SPU image with a kernel per case: the operation
repeated 32 times inside a counted loop (`ai $2,$2,-1; brnz $2`), each at
its own LS address. `run_spu_bench.sh` lifts the image with
`tools/spu_lifter.py`, compiles it with the game's flags for lifted SPU code
(`-O2 -fwrapv`), and links `spu_bench_main.c` against the real channel, DMA,
dispatch and interpreter sources. Each case doubles its iteration count
until a run takes 5 ms, then keeps the fastest of 25 runs (a preempted or
efficiency-core run is only ever slower, so the minimum is the clean number).

| mode    | meaning |
|---------|---------|
| `lat`   | one dependency chain (`rt = ra = $3`): each op waits for the last -- the op's latency |
| `tput`  | eight independent chains: the host overlaps them -- the op's throughput |
| `issue` | no result to chain (stores, branches, hints, constants, channels) -- independent copies |
| `special` | float ops with an extended-range single (exponent 255) or a double NaN in one lane: the helpers' exact (slow) path |
| `zero` | single float ops with 0.0 in one lane of the second operand (w = 0, padding): zero is not on the fast path either |

The loop control is included, not subtracted; `lnop`/`nop` (lifted to
nothing) are that floor. Channel rows are single channel instructions;
`dma` rows are a whole transfer (5 parameter writes, the command, the tag
wait) and `atomic` rows a command plus its atomic-status read, each
counted as one op.

## Why the barriers

Thirty-two copies of one op on loop-invariant operands are exactly what an
optimizing compiler folds: without help, 32 x `a $3,$3,$4` compile to one
`$3 + 4*$4` and report 48 G op/s. `apply_barriers.py` wraps every result in
`bench_opaque()` (`bench_barrier.h`): an empty asm that claims to change the
value in a vector register. It costs no instruction and forces no memory
traffic; it only stops folding across ops. Checked in the disassembly:
`a lat` is a chain of bare `add.4s` instructions.

## Caveats

- Not-taken halts and branches to the next word come out near-free lifted:
  their condition is loop-invariant here and the compiler hoists it.
- Numbers are single-threaded and sensitive to machine load; on a busy
  machine, compare rows from the same run rather than across runs.
- Skipped: `stop`/`stopd` (end the program), `iret`/`bisled` (interrupt
  return / event branch), the PowerXCell-only double compares and `dftsv`
  (not on the PS3's SPU), and blocking channel reads (inbound mailbox,
  signal notification, event wait with events pending).

## Results

`results/` keeps runs to compare against (`.txt` is the table, `.csv` the
same numbers). `2026-10-06-m1pro-baseline` is the state before any SPU
helper optimisation, on an M1 Pro with the machine otherwise loaded.

## MFC context-lifetime contention

`mfc_lookup_bench.c` runs many short synthetic jobs on 13 host workers. Each
job uses the real channel layer for a 128-byte GET and PUT, reads tag status,
then retires its context engine. The benchmark sums worker CPU time, measures
wall time and verifies both DMA payloads. It is not part of a ctest tier.

Build the baseline binary before the runtime change and retain it; after the
change, build the candidate with the same benchmark source and flags:

```sh
tests/bench/spu/run_mfc_lookup_bench.py --output /tmp/mfc-before --build-only
tests/bench/spu/run_mfc_lookup_bench.py --output /tmp/mfc-after --build-only
tests/bench/spu/run_mfc_lookup_bench.py --compare /tmp/mfc-before /tmp/mfc-after \
    --rounds 3 --threads 13 --iterations 100000
```

`--compare` runs the before binary followed by the after binary in each round
and reports median CPU and wall time. Each executable validates the GET/PUT
payloads; the comparison is specific to this host and build.
