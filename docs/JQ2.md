# JobQueue2 ring codec and reference scheduler

`libs/spurs/jq2/` holds two pieces of the `ps3recomp_spurs_jq2` library. Both are
pure C++20, deterministic, and free of runtime, thread and clock dependencies.

- `ring.hpp/.cpp`: the 8-byte command-ring entry codec.
- `sched.hpp/.cpp`: a queue-level model of the SPU policy module's (PM's) intake,
  used as the oracle for the HLE executor.

Tests: `tests/spurs_jq2/` (`ctest --preset t0 -R jq2`). Evidence ids (`jq2fw-*`,
`m1r-*`, `jq2co-*`) refer to the research repository's fact files; the codec and
scheduler headers carry the per-field citations.

## Codec

An entry is two big-endian u32 words. JOB (command 0) and COPY (command 2):
`w0 = portEA | tag << 3 | command`, `w1 = dataEA | sizeCode | bit0`. SYNC:
`w0 = mask << 16 | 7`, `w1 = bit0`.

`w1` bit 0 is **writer metadata**, never an API flag or a mask. It is the queue's
phase bit (`queue+4` bit 5, toggled on ring wrap) for JOB, COPY and SYNC alike
(jq2fw-050, m1r-003, m1r-017). The API `flags & 1` does not reach `w1`: PushBodyInternal
uses it only to include the port EA in `w0` (0x02004B14/0x02004BC8) and for the
atomic increment of `port+0x10` (0x02004B88-0x02004BB0); `w1` is the descriptor EA
alone (0x02004B38-0x02004B5C) and the writer ORs in only the phase
(0x02001538-0x02001548).

| Call | Meaning |
|---|---|
| `encode(entry, writer_phase)` | JOB/COPY/SYNC `w1` bit 0 = phase; Unknown copied verbatim |
| `decode(bytes)` -> `Decoded{entry, writer_bit}` | `writer_bit` is the raw `w1` bit 0 (the phase) |
| `JobEntry::api_flag()`, `CopyEntry::api_flag()` | `flags & 1`, recovered as `port_ea != 0` |

A SYNC whose `w1` has a bit above bit 0 set decodes to `UnknownEntry`. Round trip
`encode(d.entry, d.writer_bit)` is byte-exact for every 8-byte input.

## Scheduler contract (M1-SCOPE Amendment 2, F2)

State shared by all PM instances: ring, read/write index, expected phase, per-tag
in-flight counts, per-tag barrier set. Each instance has N job slots (the PM has
two coroutine slots). Operations: `push`, `intake_pass(instance)`, `complete(id)`.

- **C1'**: SYNC(mask) is consumed at admission. It bars every masked tag that is
  not already barred (recording the mask as that tag's barrier). It never fences
  other tags.
- **C2**: a barred tag is released when every tag of its barrier mask has no job
  in flight.
- **C3'**: a JOB of a barred tag is retained in the ring and not dispatched;
  later entries are still examined. At most one JOB is dispatched per pass.
  The read index advances over consumed entries and stops at the first retained
  one.
- **C4**: jobs of different tags admitted before a SYNC may run together.
- **Pass end**: an entry whose `w1` bit 0 does not match the expected phase ends
  the pass. A SYNC that does not complete at admission (or that covered an
  already-barred tag) ends the pass after being consumed (static handler
  0x24B4-0x2584, `r2 = -1`); the OQ-A read-index values 3 versus 4 depend on it.
  A SYNC whose masked tags are all barred is retained and re-examined.

### Measured on the firmware PM (m1r harness, `B2Golden`)

- Out-of-order consumption: a JOB dispatched behind a retained entry is recorded
  in queue state (bitmap at `queue+0x180`); ring bytes are not rewritten. Read
  index trajectory for ring `A(tag1) SYNC(2) C(tag1) D(tag0)`: 0, 1, 2 (held while
  D runs and ends), 3 (C dispatched), 4 (the next pass skips D). Same-tag jobs
  behind a barrier dispatch FIFO. Isolated in `Scheduler::mark_consumed_out_of_order`.
- Wrap, PM side of the phase rule: the expected phase flips on wrap and is
  compared strictly with `w1` bit 0 (PM 0x22B0-0x22D0). An entry whose bit 0
  differs from the expected phase ends the pass at 0x22D0 without dispatching.
  Smallest depth the PM accepted: 4 (4/8/16/32 identical). Isolated in
  `pm_accepts_phase`.
- OQ-F is closed (m1r-017): the writer stores only the phase in `w1` bit 0. With
  bit 0 = phase, JOBs with `flags&1` of 0 and 1 pass a ring wrap identically
  (depths 4, 8, 16); there is no wrap stall.
- One SPU, three unbarred jobs: one JOB per pass, read index +1 per pass, job
  slots rotate (JobContext2 0x68D0, 0x6900, 0x68D0, ...).

### Marked assumptions

- `[static only]` The API `flags & 1` is carried by the presence of the port EA in
  w0 (`JobEntry::api_flag()` is `port_ea != 0`). Every traced JOB/COPY entry has a
  port EA and every traced push has `flags&1 = 1`, so the "no port EA when
  `flags&1 = 0`" half rests on the static reading of PushBodyInternal
  (0x02004B14/0x02004BC8) alone; no traced entry exercises it.
- `[assumption]` A SYNC consumed behind a retained entry uses the same
  out-of-order mark as a JOB (only the JOB case was measured). A SYNC records a
  barrier only for masked tags that were not already barred, so a tag whose
  earlier JOB is still retained is held only by the older barrier.
- `[assumption]` The writer refuses a push when `depth` entries are outstanding;
  the firmware's AGAIN rule (jq2fw-029) is unmeasured.
- `[assumption]` A pass scans only up to the write index.
- `push()` rejects COPY, Unknown and SYNC(0): no scheduling semantics are
  documented for them. `maxGrab` and the PM's secondary busy sets (`r36`/`r37`)
  are not modelled; in-flight counts are exact over all instances.

### Depth geometries

The smallest depth the firmware PM accepted is 4 (m1r-015; 4/8/16/32 behaved
identically), and that is the only range with firmware evidence. `Scheduler::create`
nevertheless accepts any depth >= 1: depths 1-3 are **model-only geometries**,
kept deliberately because a ring that wraps and flips its phase every one to three
pushes is the sharpest stress of the wrap/phase bookkeeping (the property tests run
depths 1-12). Nothing about the PM's behaviour at depths 1-3 is known; do not read
a passing model run at those depths as firmware behaviour. A consumer that models
a guest queue (B4) must take the depth the guest configured, and a guest depth
below 4 is outside what was measured.

### How the model is tested

The property tests (`tests/spurs_jq2/test_jq2_sched.cpp`) do not trust the
scheduler to report on itself. An independent oracle, written from this contract
and sharing nothing with `Scheduler`, numbers the pushed entries by absolute
position and derives the read position, the barred tags, what each SYNC barred,
the out-of-order marks and the phase bit the writer stored from the pushed
sequence and the dispatch/complete events alone; every `Scheduler` observer is
compared with it after every operation.

`push()` always stores the phase the PM expects, so the wrong-phase path needs two
test hooks that add no scheduling semantics: `push_with_phase(entry, phase)` (a
writer that stored the wrong w1 bit 0, or has not finished the write) and
`rewrite_entry(index, entry, phase)` (the writer completing it). An entry whose
bit 0 differs from the expected phase ends the pass without being dispatched or
consumed, and is not stepped over, until it is rewritten with the right phase.
