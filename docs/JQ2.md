# JobQueue2 ring codec, reference scheduler, port line and HLE executor

`libs/spurs/jq2/` holds four pieces. The codec and the scheduler are pure C++20,
deterministic, and free of runtime, thread and clock dependencies: they are the
target `ps3recomp_spurs_jq2_model`. The port model's logic is too, and only its
guest binding reaches into the runtime (see Port line); the port model and the
executor are the target `ps3recomp_spurs_jq2`, which links the runtime
explicitly (the runtime's `cellSpursJq.c` calls `jq2/jq2.h`, so the two static
libraries depend on each other and CMake repeats the cycle on the link line).

- `ring.hpp/.cpp`: the 8-byte command-ring entry codec.
- `sched.hpp/.cpp`: a queue-level model of the SPU policy module's (PM's) intake,
  used as the oracle for the HLE executor and, over a guest-memory ring, as its
  scheduling rules.
- `port.hpp/.cpp`: the port line, the PPU/SPU completion accounting a Port2 push
  and Port2Sync share with the PM.
- `executor.hpp/.cpp`, `jq2.h`: the HLE executor and the C surface the PPU
  exports call (see Executor).

Tests: `tests/spurs_jq2/` (`ctest --preset t0 -R jq2`). Evidence ids (`jq2fw-*`,
`m1r-*`, `jq2co-*`, `jq2c-*`) refer to the research repository's fact files; the
headers carry the per-field citations.

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

## Port line

A port is a 128-byte, 128-aligned guest block. Four big-endian words carry the
accounting; nothing else in the block is read or written by this module:

| Offset | Word | Writer |
|---|---|---|
| `+0x00` | completion `A`: bits 28:0 count (compared as a signed 29-bit value), bits 31:29 Sync waiter state | PM (`A+1`), Port2Sync (`A-B`, registration) |
| `+0x04` | waiter id (fiber EA or event-flag slot), stale after a wake | Port2Sync |
| `+0x10` | submitted `B` | push (`B+1`, rollback `B-1`), Port2Sync (`B := 0`) |
| `+0x14` | claim, byte 0 only | Port2Sync |

Every operation is one critical section on the line in the reservation model
(`resv_host_rmw`, `runtime/memory/reservation.{h,c}`): the same lock order and
invalidation as a `stdcx.` that lands. An SPU `GETLLAR` reservation or a PPU
`lwarx`/`ldarx` on the line therefore dies when the host stores, and a host
operation that stores nothing (a BUSY, an AGAIN) kills nothing. `port.hpp` layers
the rules as `PortWords` (pure transitions), `LocalPortLine` (single-threaded,
the unit-test and trace-replay oracle), `GuestPortLine` (guest memory through
`resv_host_rmw`) and `BasicPort<Line>` (the operations over either).

| Operation | Effect | Evidence |
|---|---|---|
| `try_submit` (push with `flags&1`) | `B += 1` iff claim byte is 0 and `B <= 0x0FFFFFFE`; else BUSY `0x80410A0A`, port untouched | jq2c-007, `0x02004D70-0x02004DC0` |
| `rollback_submit` (enqueue failed) | claim byte 0: `B -= 1` mod 2^32; claim held: BUSY | jq2c-007, `0x02004B8C-0x02004BC4`, JQ-CLOSURE-REVIEW finding 4 |
| `push_accounted(enqueue)` | submit, enqueue, rollback on failure; a blocked rollback returns BUSY in place of the enqueue error | `0x02004BAC -> 0x02004C10` |
| `credit_completion` (PM, per job without its own semaphore) | `A := (A+1) mod 2^29`; bits 30:29 kept; bit 31 kept unless `A == -1`, which clears it and makes the PM's release call due (`Wakeup{kind = bits 30:29, id = +0x04}`) | jq2c-009/010, PM `0x3404-0x3454` |
| `sync(mode, wait)` (Port2Sync) | `mode & ~7`: INVAL `0x80410A02`. Claim (BUSY if held). Stale waiter bit: BUSY. `B > 0x0FFFFFFF`: INVAL. `sext29(A) >= B`: `A := A-B` (bits 31:29 kept), OK. Else mode bit 2: AGAIN `0x80410A01`; else register and block. Every path clears the claim; only OK zeroes `B`; AGAIN, BUSY and INVAL leave `A` and `B` | jq2c-001..004, `0x020045D8-0x020046F0`, `0x02003018-0x0200339C` |

Guards a caller applies first (`check_port_ea`): null `0x80410A11`, not
128-aligned `0x80410A10`.

### Corrections and findings from decoding

- **Credit keeps bits 30:29.** JQ-CLOSURE-STATIC B.2 says the PM "clears bits
  30..29"; jq2c-010 left bits 31..29 unverified. The PM dataflow at LS
  `0x3404-0x3444` (`fw/re/jq_pm.dis`) takes bits 28:0 from `A+1` through the
  `0x1FFFFFFF` select and every other bit from the old word, except bit 31, which
  is masked by `A != -1`. The traced `0x80000005 -> 0x80000006` agrees with both
  readings; a word with bit 30 or 29 set separates them. The tests compare the
  model with a transliteration of the SPU operation sequence over 200000 random
  words and a bit-pattern grid.
- **A blocking Sync registers, then sleeps.** `0x02003104-0x0200337C` stores
  `A-B` (negative) with bit 31 set, bits 30:29 = the wait kind (0 fiber, 2 event
  flag) and `+0x04` = the waiter id in one doubleword update, then waits. Each PM
  credit adds one; when `A` passes from -1 to 0 the PM clears bit 31 and issues the
  release call (`LS 0x3454-0x3488` into helper `0xB28`: mode = bits 30:29, EA =
  `+0x04`). The waiter returns OK without re-reading `A`. `sync()` models this
  with a three-call `SyncWait` environment (`acquire`, `block`, `release`); the
  event-flag or fiber wait itself is the executor's. After a wake `+0x04` stays
  stale and `A`'s bits 30:29 keep the wait kind (`0x40000000` after an
  event-flag wait); neither blocks the next Sync.
- **No traced Sync reached the registration path.** All 4106 traced port dumps
  have bits 31:29 of `A`, the `+0x04` word and the claim word zero. A Sync that
  had to wait would leave its id in `+0x04`.
- **Firmware hazards kept, not fixed.** (1) A rollback that meets a Sync claim
  returns BUSY and leaves the failed push's `B += 1` in place: that `B` can never
  be credited, so a later blocking Sync waits forever. (2) A rollback after a
  Sync reaped the push leaves `B = 0xFFFFFFFF`. Both are the firmware's and are
  pinned by tests; the executor must not create the situations (a push whose
  enqueue can fail should not race a Sync on the same port).
- **Claim and BUSY ownership.** A Sync that loses the claim returns BUSY without
  touching the claim byte; the owner's claim survives. A Sync that claimed
  clears the byte on every return, including BUSY (stale waiter) and INVAL
  (`B` above the bound).

### How the port line is tested

`tests/spurs_jq2/test_jq2_port.cpp` (labels `spurs;t0;tsan`; `jq2_port_race_stress`
is `spurs;stress`):

- `model`: every transition and code on `PortWords`/`LocalPort`: saturation at
  `0x0FFFFFFF`, rollback, `sext29` wrap, modes 0..7 and the INVAL modes, AGAIN,
  BUSY, INVAL, claim release on each path, registration and wake with spurious
  wakes, credits arriving during `acquire`, and the credit against the
  SPU-operation formulation.
- `guest`: big-endian layout, bytes outside the four words, the guards, and the
  reservation model's rule through the real PPU (`resv_*`) and SPU (`spu_wrch`
  MFC) paths.
- `claim`: a Port2Sync holding the claim makes a concurrent push return BUSY and
  `B` stays exact.
- `race N`: one SPU `GETLLAR`/`PUTLLC` credit loop, a pushing thread, a Sync
  thread alternating modes 0 and 4, and a PPU `stwcx.` loop on other words of the
  same line; final `A`, `B` and claim exact, the PPU counters exact, a 20 s
  no-progress watchdog.

The Odin-backed check is `tests/test_jq2_port_replay.cpp` in leifthrasir-recomp:
it replays the `(A, B)` sequences of `jq-boot`, `closure` and `stall2` through
`LocalPort`.

## Executor

`executor.cpp` is the runtime's stand-in for the firmware's job queue: the PPU
half (validation, ring write, port accounting, ports, create/shutdown/join) and
the SPU half (the policy module's intake and the job start), one `Queue` per
guest queue EA. `jq2.h` is its C surface; the 17 `cellSpursJq` exports are thin
adapters that call it (a firmware status in, the same s32 out). The layout of
the guest queue block, the evidence behind each field and the choices the
evidence does not settle are in the header of `executor.hpp`.

**Pushes.** `jq2_port_push_job` / `jq2_port_copy_push` / `jq2_port_push_sync`
apply the firmware guards (null `A11`, alignment `A10`, argument `A02`, port or
queue state `A0F`, full ring `A01`, claim collision `A0A`), then account on the
port line when `flags & 1` (`Port::push_accounted`: `B += 1`, ring write,
rollback if the ring is full) and write one entry through `ring_push()` into the
guest ring, under the reservation model: the index word at `queue+0x00` (read
u16, write u16, flags u32 with the writer phase) moves only through
`resv_host_rmw`, the entry is one 8-byte release store, and a worker that
reaches the slot first sees the previous lap's entry, whose phase bit does not
match (the PM's own protocol). A CopyPush takes a pool slot first (blocking),
snapshots `size` bytes into it and enqueues the slot (contract S3).

**Workers.** `worker_count` host threads (8 MB stacks: the loader keeps a whole
`spu_context` on the stack) are the scheduler's instances, one job slot each.
A worker runs `Scheduler::intake_pass` under the queue lock over `GuestRing`
(`sched.hpp`'s `RingStore`, see below), takes the descriptor of the JOB it
admitted from guest memory right there (contract S2) and runs the job outside
the lock through `spu_run_spurs_job_snapshot`, which reads the header, the DMA
and cache lists and the user words from that copy and never from guest memory
again: an overwrite after admission does not reach the job, one before it does,
exactly as on the PM. The job body is `interpreter_body()` (the SPU interpreter
from LS 0); `ExecutorHooks::select_body` is the plug for lifted bodies once the
job-image registry exists. A job counts as returned only when it executes the
`stop 0` at the link address the loader plants (`0x3FF00`); any other stop, a
halt, or a loader refusal is the named stop `jq2-job-end` and credits nothing.

**Completion.** After the run: pool slot back (COPY), then `credit_completion()`
on the port line, and the `Wakeup` it returns is performed: kind 2 sets bit
`id % 64` of the queue's event flag `+0x80 + 4 * (id / 64)` (LV2 event flags
through `sys_event_flag_*`), any other kind is the named stop `jq2-wake` (fibers
do not exist yet). Last, the scheduler's `complete()` releases barriers. A
`Port2Sync` that has to wait registers a host-allocated id (128 of them) and
blocks on that bit with the firmware's wait (`AND | CLEAR`, no timeout).

**Counters.** `jq2_queue_stats`: `pushed`, `dispatched`, `executed`,
`credited`, `uncredited` (executed jobs pushed without `flags & 1`). `executed`
is counted after the job returned and `credited` just before the port line is
written, so at any instant `credited + uncredited <= executed <= dispatched` and
at quiescence all three agree.

**Shutdown and Join.** `Shutdown` refuses further ports and pushes (`A0F`) and
tells the workers to leave once the ring is drained; it returns `A0A` while a
port is attached. `Join` waits for the workers (so for every outstanding job,
L1), destroys the event flags, sets bit `0x20` of `queue+0x6C` and hands
`*(queue+0xF04)` back. The guest buffers stay the caller's (L2): nothing outside
the queue block, the ring (`2048 + ((depth + 15) * 8 & ~0x7F)` bytes are zeroed
at create, as the firmware does), the pool and the ports is written.

**The PM's record line is not touched.** `queue+0x180..+0x1FF` is one 128-byte
line of 16 per-grab-slot 8-byte records the PM rewrites with GETLLAR/PUTLLUC
(the type-3 path stores at `line + slot * 8`), private scheduling state no PPU
path reads. The executor writes nothing in `+0x180..+0x57F`; the out-of-order
marks live in the `GuestRing` object (host state). m1r-013's "bitmap at
queue+0x180" was a simplification; `Scheduler::mark_consumed_out_of_order`
keeps its contract.

### Scheduler over a ring store

`Scheduler` keeps the model's rules and now runs them over a `RingStore`: the
entries, the read/write index and phase pair, the out-of-order marks, the
writer's `reserve()` and the reader's `advance_read()`. The default store is
private memory, so every B2 test runs unchanged; the executor passes a store
over the guest queue (`GuestRing`). `ring_push()` is the writer over any store
and is safe against a concurrent reader. A COPY is scheduled exactly as a JOB
(the PM's decode sends command types 0 and 2 to one handler, jq2c-021) when the
scheduler is configured with `copy_entries`; `Dispatch::copy` and
`job.descriptor_ea` (the pool slot) carry it. The ring is full when `depth`
entries are outstanding; with equal indices the phases tell empty from full
(`ring_outstanding`).

### Shared SPU state

Two workers run two jobs at once, so state the runtime kept in globals is now
per thread or atomic: `spu_run_spurs_job_snapshot` keeps no retained local store
(`spurs_job_ls_for_handle` is a chain-job notion; the chain walker's
`spu_run_spurs_job` still retains it), the loader's and the MFC layer's
lazily-read `SPU_*` environment gates and debug rings are thread-local
(`SPU_THREAD_LOCAL`), the interpreter's watchdog counters
(`g_spu_interp_steps`, `g_spu_interp_last_pc`) and the SPU local-store gates
are relaxed atomics, and the committed-page cache is updated with an atomic OR.
The `tsan` label of `test_jq2_executor` runs clean; paths the tests do not run
(GETLLAR/PUTLLC jobs, lifted code) keep their unaudited lazy gates.

### Job observer

`jq2_set_job_observer` installs, per queue and off by default, a pair of
callbacks on the worker thread that runs a job: `job_start` with the worker's
`spu_context` after the job is staged and before its first instruction (the
place to install an `spu_observe` observer), and `job_end` with `r3` as it
returned. The `jq2_job_info` they receive carries the admission identity and the
local-store layout the loader chose (`spu_job_layout`: image, descriptor copy,
io, out, scratch/stack, `r1` at entry), passed out by
`spu_run_spurs_job_hooked`, not recomputed.

### How the executor is tested

`tests/spurs_jq2/test_jq2_executor.cpp`, one mode per process, on synthetic SPU
job images (a JOBCRT Ver13 header and 16 hand-encoded instructions: GET input,
add the descriptor's words, PUT, return) and the real interpreter, loader,
channel layer and LV2 event flags; labels `spurs;t0;tsan`:

- `premature`: a store to the descriptor after the push and before admission
  reaches the job; a store from inside the admission hook (after the copy) does
  not.
- `copy`: the CopyPush source can be scribbled at once; a class of two blocks
  makes the third CopyPush block until a job returns its block, nine pushes
  through it; a pool-less queue and a bad class are refused.
- `credit`: 1000 jobs, 2 workers, 3 tags, a PushSync every ten pushes and a
  Sync every hundred: at every Sync `dispatched == executed == credited ==
  pushed`, `uncredited == 0`, every output right, A (count) and B zero.
- `wrap`: a depth-4 ring wraps (and flips its phase) 30 times under two workers.
- `barrier`: the m1r-013 ring `A(tag1) SYNC(tag1) C(tag1) D(tag0) C2(tag1)` with
  A held in flight: D overtakes, the read index holds at the retained C, the
  mark is set, C and C2 start only after A finished and in push order, the ring
  bytes are never rewritten, `+0x180..+0x57F` stays zero.
- `sync`: mode 4 `AGAIN` with jobs outstanding (A, B untouched), a blocked mode
  0 holds the claim (`A` bit 31, claim byte), a second Sync and a push meet
  `BUSY`, the blocked Sync returns 0 when the queue resumes, a mode with bits
  above 7 is `INVAL`.
- `join`: `Shutdown` is `BUSY` with a live port, `Join` before `Shutdown` is
  `STAT`, `Join` returns only after the held job and the other eleven finished,
  guest bytes outside the queue's blocks are untouched; a second run joins with
  30 jobs outstanding.
- `observer`: `job_start` gets the worker's `spu_context` and the loader's layout
  (image at LS 0 with its bss, the descriptor copy, `r1`, the context), an
  `spu_observe` observer installed there sees the job's GET and PUT, `job_end`
  gets `r3`, and after the observer is removed no callback fires.
- `validate`: every guard and status code of `jq2.h`, the guest layout create
  leaves, ports and slots, and a job pushed without `flags & 1` (runs, never
  credited, never counted in B).
- `stop_job`, `stop_crt`, `stop_ring`: forked children (the guest memory is
  shared) end in `jq2-job-end`, `spu-job-crt-version` and `jq2-ring-entry` with
  exit status 70, and the parent finds the port not credited.
