# JobQueue2 ring codec, reference scheduler, port line, descriptor pool and HLE executor

`libs/spurs/jq2/` holds five pieces. The codec and the scheduler are pure C++20,
deterministic, and free of runtime, thread and clock dependencies: they are the
target `ps3recomp_spurs_jq2_model`. The port model's logic is too, and only its
guest binding reaches into the runtime (see Port line); the port model, the
descriptor pool and the executor are the target `ps3recomp_spurs_jq2`, which
links the runtime explicitly (the runtime's `cellSpursJq.c` calls `jq2/jq2.h`,
so the two static libraries depend on each other and CMake repeats the cycle on
the link line).

- `ring.hpp/.cpp`: the 8-byte command-ring entry codec. Pure C++20,
  deterministic, no runtime dependency.
- `sched.hpp/.cpp`: a queue-level model of the SPU policy module's (PM's) intake,
  used as the oracle for the HLE executor and, over a guest-memory ring, as its
  scheduling rules.
- `port.hpp/.cpp`: the port line, the PPU/SPU completion accounting a Port2 push
  and Port2Sync share with the PM.
- `pool.hpp/.cpp`: the `CopyPushJobBody` descriptor pool in the queue's guest
  memory, with the acquire/release/snapshot verbs and the blocking wait hook.
  It bounds snapshot ranges by `ppu_vm_size` and writes through the A2(b)
  reservation model.
- `executor.hpp/.cpp`, `jq2.h`: the HLE executor and the C surface the PPU
  exports call (see Executor).

Tests: `tests/spurs_jq2/` (`ctest --preset t0 -R jq2`); the port, pool and
executor tests are also labelled `tsan`. Evidence ids (`jq2fw-*`, `jq2c-*`,
`m1r-*`, `jq2co-*`) refer to the research repository's fact files; the headers
carry the per-field citations.

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

## Descriptor pool

`pool.hpp/.cpp` is the pool `_cellSpursJobQueuePort2CopyPushJobBody` uses. The
firmware asks `JobQueueSemaphoreAcquire` (EA 0x0200204C) for a block of the class
selected by `copySize`, copies exactly `descriptorSize` bytes with the vector
helper 0x020063E8, and only then enqueues the returned block EA
(0x02004C9C-0x02004DD0; jq2fw-024-027). The snapshot therefore completes before
the command exists (contract S3), and the caller may reuse the source as soon as
the call returns.

### Guest layout

The queue EA is 128-byte aligned. All fields below are big-endian.

| Guest field | Meaning | Firmware evidence |
|---|---|---|
| `queue+0x600 + 4*class` | u32 first block of the class; 0 = unconfigured | class initializer 0x020008A8; Acquire rejects a 0 at 0x02002170-0x02002184 |
| `queue+0x620 + 4*class` | u32 end of the class's block region | 0x020008C8 |
| `queue+0x640 + 4*class` | u32 EA of the class's zeroed 0x400-byte control area (the firmware's per-class waiter table) | 0x02000960; wait path 0x02002218 |
| `queue+0x660` | u32 descriptor-pool base (the create argument) | 0x02000938 |
| `queue+0x664` | u32 end of the descriptor-block area, before the control areas | 0x0200093C |
| `queue+0x680 + 16*class` | 16-byte free-queue record (below) | 0x020008A8-0x0200090C, pop 0x0200217C-0x020023F0 |

Each 16-byte record:

| Offset | Field |
|---|---|
| +0x00 | u32 head: first free block EA, 0 = empty (the high word of the 64-bit atomically-updated state) |
| +0x04 | u16 free count |
| +0x06 | u16 reserved; byte +0x06 is preserved, byte +0x07 is the blocked-waiter count. The firmware increments it before blocking and refuses a new waiter once it is 0x7F (0x020021A4-0x020021AC, 0x020023DC-0x020023F0). |
| +0x08 | firmware per-class wait word; a configured class has byte +0x0D = 1 and byte +0x0E = 1 (0x020008AC, 0x020008B4), the rest is left as initialized. |

A free block's first u32 is the next free block EA; the create path links
`block[i] = block[i] + size` and terminates the last block with 0
(0x020008EC-0x0200090C).

Class sizes are the firmware table at EA 0x02006DE8: 64, 128, 256, 384, 512,
640, 768, 896 bytes (jq2c-014). `copySize` is accepted when it is exactly 64, or
128..0x3FF with `(copySize & 0x7F) == 0`; class = `copySize >> 7`
(0x02002114-0x0200214C, jq2c-015), so the eight sizes are classes 0..7. Any
other size is INVAL; an unconfigured class is NULL_POINTER.

### Create

`DescriptorPool::create({queue_ea, pool_ea, pool_attr_ea}, hook, error)` reads
the eight per-class block counts from `poolAttr` u32[0..7] (CreateCommon arg 7;
jq2c-016) and lays the pool out the way the firmware does:

1. Blocks are carved in **descending** class order from one cursor starting at
   the pool base: class 7 first, class 0 last. The firmware's loop walks classes
   7..0 with that single cursor (0x02000814, 0x02000870-0x0200091C).
2. `queue+0x660` = pool base and `queue+0x664` = the resulting block-area end.
3. Control areas are assigned in **ascending** class order at
   block-area-end + k*0x400, and the whole contiguous region is zeroed
   (0x02000920-0x02000988).
4. Each configured class gets its `queue+0x600/+0x620/+0x640` entries and a
   record with `head = first block`, `count = count`, flags +0x0D/+0x0E = 1.

The descriptor-block bytes themselves are not cleared; only each block's first
u32 is written for the link. The runtime pool (base 0x37649380, class 2 x32)
confirms the image: `q+0x608/0x628/0x648` = 0x37649380/0x3764B380/0x3764B380,
`q+0x660/0x664` = 0x37649380/0x3764B380, and the first block's link = base+0x100
after the one observed pop (jq2c-019, JQ-CLOSURE-STATIC D.4).

`create` rejects a null queue/pool/poolAttr EA (A11), a non-128-byte-aligned EA
(A10), a count above the u16 record field, or a layout that would run past the
32-bit guest address space (A02). A pool with all counts zero is valid (every
class unconfigured) and behaves like the firmware: Acquire returns A11.

### Verbs

- `class_of_copy_size(copy_size)` returns the class or INVAL with the firmware's
  exact size rule.
- `acquire(class, blocking)` pops the class head under the record line lock,
  clears the host free-map bit and decrements both counts. A non-blocking empty
  acquire returns AGAIN; an unconfigured class returns NULL_POINTER; a class
  argument outside 0..7 returns INVAL. Every guest-visible invariant is checked
  before a block is handed out (head is a block start of that class and marked
  free, guest count equals the host free map, the next link is a different live
  block); a mismatch returns INVAL instead of returning a wild EA.
- A blocking empty acquire registers with the injected `PoolWaitHook` and calls
  `wait(class, generation)` with no pool lock held. `release` bumps the class
  generation and calls `notify(class, generation)`. The waiter reads the
  generation under the record lock before waiting, so a release that races the
  wait is never lost; a spurious wake simply re-checks and waits again. The
  pool never spins on its own. The 0x7F waiter saturation rule returns AGAIN
  rather than blocking.
- `release(ea)` locates the class from the immutable class regions, then pushes
  the block under the record line lock: the freed block's first u32 becomes the
  old head, the block becomes the head, both counts increment, and the class
  waiters are notified. A corrupt release is a named stop, never a no-op:
  `jq2-pool-release` with `queue`, `ea`, `class` and
  `reason=foreign|misaligned|double-free|count-mismatch`, exit 70.
- `snapshot(src, size, dst)` copies exactly `size` bytes from the guest source
  to a live block, one destination line at a time under the reservation lock.
  It returns INVAL for a free/foreign destination, a size above the class size,
  or overlapping source and destination, and `PoolError::address` (firmware code
  INVAL, `0x80410A02`) for a source or destination range outside the runtime's
  mapped guest memory (`ppu_vm_size`, or the 32-bit space when the host does not
  size it - the bound the SPURS job loader uses) or wrapping it, checked in
  64 bits before any host pointer is formed. The firmware validates no address
  (its vector copy moves exactly the length it is given, jq2fw-025/026), so
  `address` has no code of its own: invalid arguments are INVAL (jq2fw-022); on
  hardware the copy would take a storage fault [INFERENCE]. Zero bytes is a
  no-op. The copy is byte-exact, so a non-multiple-of-8 `descriptorSize` is
  copied as bytes (the firmware's helper copies exactly its length). The bytes
  are stored as relaxed 64-bit atomics (`resv_copy_in` for a whole aligned
  line, a compare-and-swap merge for an unaligned edge word), because PPU
  `lwarx/ldarx` read guest words lock-free (`reservation.h`, "128-byte line
  copies"): a byte-wise copy would race them and could show a torn word. The
  line lock serializes conditional stores and SPU atomics, not those loads.
- `shutdown()` wakes every waiter and makes the next blocking acquire return
  AGAIN, so the executor can join workers during queue teardown. Releases still
  work.

### Reservation model and observers

Every header update goes through `runtime/memory/reservation.h` with the same
protocol as `resv_stcx32/64` (`runtime/memory/reservation.c:247`): take the SPU
lock-line lock first when the line is SPU-marked, take the line epoch lock,
update, release and advance the epoch, then raise the SPU lost-reservation event
when the line was marked. All eight class records live in the single 128-byte
line at `queue+0x680`, so - as on hardware - class operations conflict on that
reservation line; `queue+0x600..+0x67F` is a second line.

The SPU side does not read the pool header at all: the PM consumes the command
ring and DMA-GETs the descriptor snapshot at the allocated block EA at
admission. It therefore observes exactly the bytes `snapshot()` wrote into an
allocated block, ordered by the executor's release fence after the snapshot and
its acquire before the fetch. The pool never writes the ring; the executor owns
publication.

### Tests

`tests/spurs_jq2/test_jq2_pool.cpp` (`jq2_pool`, labels `spurs;t0;tsan`) is one
binary with a `--child` mode for the stop cases:

- the create layout at byte level against an independent re-derivation of the
  descending block carve and ascending control-area assignment, including the
  runtime pool's class-2 image and the untouched block bytes;
- the class table/selection rule and all four firmware error codes;
- a per-class acquire/release round trip for all eight classes, asserting the
  record head/count/flags and the block links after every step;
- exhaustion (AGAIN non-blocking), the 0x7F waiter saturation, and a blocking
  acquire woken by a release through a counted condition-variable hook
  (threads, entered-handshake, no sleep used as synchronisation);
- `snapshot` copies exactly `descriptorSize` bytes (200 of a 256-byte class),
  the tail stays untouched, and scribbling the source afterwards is invisible;
- `snapshot` refuses a source that starts inside, at, far past or wraps the end
  of the VM, and a destination past a shrunk VM, with `address` and without
  writing the block; the test's VM image is followed by an inaccessible page,
  so an access past it faults instead of passing by luck; the last 16 bytes of
  the VM are a valid source;
- two PPU threads (`resv_ldarx64`, `resv_lwarx32`) read every word of a block
  while it alternates between two uniform images (203 bytes, so a 3-byte merged
  tail word): every word read is one image's, never torn, the bytes after the
  copy keep their old value, and the TSAN run reports no race;
- foreign, misaligned and double-free releases each stop in a subprocess with
  exit 70 and the `jq2-pool-release` machine line naming queue/ea/class/reason;
- a PPU reservation taken before a release cannot commit afterwards, the epoch
  advances, and the SPU lost-reservation event is raised (the test's
  `spu_coh_notify_write` stub counts it);
- four threads acquire/release a class of eight blocks 200 times; a per-slot
  atomic owner asserts every slot is owned at most once (TSAN run).

Every expected byte is derived from the firmware EAs in this section and the
runtime pool dump, not from the implementation, so a layout drift fails the
test even if the pool is self-consistent.

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

### JQ2 job ABI

The executor runs a job through `spu_run_jq2_job`, which stages local store the
way the JobQueue2 policy module (PM) does, not the jobchain layout
(`docs/RUNTIME.md`, "SPURS job start", "Two layouts"). Evidence:
`out/M1/JQ2-JOB-ABI-static.md` (research repository; §1 launch sequence and
placement, §2 kernel context, §3 jm interface, §5 staging spec; perturbation
experiments E0-E13) and the dynamic run of the real PM (`m1r --dbg-pc 0x5834`:
entry registers). ELF Setting is linked for the PM's base: staged at LS 0 the
kernel-view overlay lands on its code and its outputs differ; with this layout
`hle-setting` and `fw-setting` events are identical under `ps3diff` (J5).

| Item | Value | Evidence |
|---|---|---|
| image base | `0x7C00`, `sizeBinary << 4` bytes copied, bss untouched | static 1.1 stage 6, 1.2 |
| image + bss | `A128((sizeBinary + u16@+2) << 4)`; the bss is only `u16@+2 << 4`, the rest is the PM's rounding | static 1.2, E7 |
| caches | `cacheBuffer[i]` = running pointer, each `A128(size)`, before io | E8, E9 |
| io, out | `A128(sizeInOrInOut)`, `A128(sizeOut)`, out follows io, the `+0x10` word is not consulted | E4, E5 |
| scratch + stack | `s` = out end; stack base `s + A128(sizeScratch << 4)`; `T = base + (sizeStack ? sizeStack << 4 : 8192)`; the heap advances by `A128(stackBytes)` | E1-E3, E7 |
| `r1` | `T - 0x60`; words 1..3 `stackBytes - 0x60` (`0x1FA0` for 8 KB). The job's stack-limit helper subtracts word 1 | static 1.4; dynamic entry registers; Setting image `+0x5F40` stores the result |
| descriptor copy | `D0 + k * maxDesc`, `D0 = 0x3F400 - 4 * maxDesc` (`0x3F000` for 256, `0x3E600` for 896) | static 1.1 stage 0, `m1r-007` |
| `JobContext2` | `0x68D0 + 0x30 k`: `+0x00` io, `+0x04..` cache buffers, `+0x14 = ((descSize >> 7) & 0xF) << 28 \| 0x7A10`, `+0x18/+0x1A` numIo/numCache, `+0x1C` o, `+0x20` s, `+0x24 = 0`, `+0x28` 64-bit descriptor EA | static 1.2 |
| jm table | `0x7A10 = {0x1500, 0x5870, 0, 0}`; the CRT copies the words into bss, so they are part of the image+bss hash | static 3.1, 3.5 |
| kernel view | `0x1C0 {0, spursEA}`, `0x1C8` spuNum, `0x1CC = 8`, `0x1D4 = 0xA00`, `0x1D8`/`0x1DC` workload id; all else in `0..0x2FF` zero; nothing is ever written at an image address | static 2, 5.2 step 7 |
| entry state | registers all zero, then `r0 = 0x3FF00`, `r1`, `r3 = ctx`, `r4 = desc`, `r80 = r1 + 0x20`, `r81 = r1 + 0x30`, `r126 = 0x7C00`; pc `0x7C00` | static 1.4, 5.2 step 8 |

**One job per worker.** The PM alternates two Execute slots `k` while it stays
resident; a worker runs one job at a time and has no residency to model, so
`k = 0`: descriptor copy at `D0`, context at `0x68D0`.

**Values from the executor.** `maxDesc` is the queue's `+0xF14`, the SPURS EA
its `+0xF10`, `spuNum` the worker index. The workload id is the low five bits of
`queue+0x6C`; `CreateCommon` writes `spurs EA | wid` there (`m1r-004`) and the
HLE writes it with wid 0, the id the firmware gives the first workload (it has
no workload registry), so the kernel view reads one source. `fw-setting` uses
workload id 0.

**The two jm routines.** Both are authored here, not copied. `0x5870` (the PM's
coroutine swap) is the tag-status poll loop of `s_job_rt_swap` in `spurs_job.c`:
`WrTagMask = -1`, immediate `WrTagUpdate`, `RdTagStat` until every group is
complete, `bi $r0`; it clobbers only `r3`. For a single resident job that is the
swap's whole observable effect (static 3.2); it is one definition shared with
the JobChain Ver14 interface. `0x1500` (the scheduler switch) is a `stop` with
code `0x3B0F` (`SPU_JQ2_JM_CALL_STOP`); a job that ends on it is the named stop
`jq2-jm-call` with the request word `r3`, never executed or credited. Both run
interpreted or lifted (the resolver interprets live bytes at the two addresses,
`job_rt_ls` / `job_rt_ls2`).

**Fail closed.** The staging stops by name, in the one validator, where the
evidence does not decode the layout: `spu-job-dma-list` with `reason=jobtype-bit1`
(descriptor `+0x2C` bit 1, static U1), `binary-bit1` (eaBinary bit 1, the PM's
debugger hold, U6) and `cache-count` (more than the two cache entries the PM was
observed with, U5); a heap that passes `D0` is `spu-job-memory` with `limit` =
`D0` (the PM halts `0x80410A17`). Admission is the loader's: the CRT marker gate
(`JOBCRT Ver13/14`), guest ranges of binary, caches and lists, the input-list
rules. The PM's further checks (the image `+0x10` stub, the bin2 / ELF flag
agreement, the `%SPURS JOB INFO%` trailer) are not repeated: the images the
executor runs are the ones `hle-setting` compares.

**Unresolved** (static §6): U2 second heap pool, U3 `+0x10` semantics, U4
dmaTag use by other jobs, U10 same-EA resident shortcut.

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
  (image at LS `0x7C00` with its bss, the descriptor copy, `r1`, the context), an
  `spu_observe` observer installed there sees the job's GET and PUT, `job_end`
  gets `r3`, and after the observer is removed no callback fires.
- `validate`: every guard and status code of `jq2.h`, the guest layout create
  leaves, ports and slots, and a job pushed without `flags & 1` (runs, never
  credited, never counted in B).
- `stop_job`, `stop_crt`, `stop_ring`: forked children (the guest memory is
  shared) end in `jq2-job-end`, `spu-job-crt-version` and `jq2-ring-entry` with
  exit status 70, and the parent finds the port not credited.
- `layout`, `layout-896`: a descriptor with two caches, an input list, io, out,
  scratch and stack (maxDescriptor 256 and 896, workload ids 5 and 0x11 set in
  `queue+0x6C`): every address of the table above derived by hand, the
  descriptor copy at `0x3F000` / `0x3E600`, every `JobContext2` word, the jm
  table, stop word and poll routine, the kernel view word by word (all else in
  `0..0x2FF` zero), the registers lane by lane, and the loader's report.
- `jm-wait`: a job GETLs a stalled two-element list, calls jm word 1 through the
  table and uses the second element's data; the test acknowledges the stall
  only after the routine has polled `RdTagStat` three times.
- `heap-fit`, `stop-heap`: a heap that ends exactly at `D0` runs; 0x80 more is
  `spu-job-memory` (region `stack`, `p`, `limit = D0`).
- `stop-jm`, `stop-jm-lifted`, `stop-unmodelled`: `jq2-jm-call` (interpreted and
  lifted) with the request word; `jobtype-bit1`, `binary-bit1`, `cache-count`.
