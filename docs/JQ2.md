# JobQueue2 ring codec, reference scheduler and descriptor pool

`libs/spurs/jq2/` holds three pieces of the `ps3recomp_spurs_jq2` library.

- `ring.hpp/.cpp`: the 8-byte command-ring entry codec. Pure C++20,
  deterministic, no runtime dependency.
- `sched.hpp/.cpp`: a queue-level model of the SPU policy module's (PM's) intake,
  used as the oracle for the HLE executor. Pure C++20, deterministic, no runtime
  dependency.
- `pool.hpp/.cpp`: the `CopyPushJobBody` descriptor pool in the queue's guest
  memory, with the acquire/release/snapshot verbs and the blocking wait hook.
  It reaches guest memory through the runtime's `vm_base` mapping, bounds
  snapshot ranges by `ppu_vm_size`, and calls the A2(b) reservation model and
  the named-stop implementation, so the runtime library provides those symbols
  (the pool test defines `vm_base`/`ppu_vm_size` and links the rest explicitly).

Tests: `tests/spurs_jq2/` (`ctest --preset t0 -R jq2`); `jq2_pool` is also
labelled `tsan`. Evidence ids (`jq2fw-*`, `jq2c-*`, `m1r-*`, `jq2co-*`) refer
to the research repository's fact files; the headers carry the per-field
citations.

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
