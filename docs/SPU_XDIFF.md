# SPU job tiers and differential execution (xdiff)

One SPU job image can run in several implementation tiers: interpreted, lifted,
partly native, fully native. xdiff compares any two tiers on the same real
inputs, in a test or during a live run, without the compared side ever
producing an effect. This document explains the design and the evidence it
rests on. The shared types are `runtime/spu/spu_effect_port.h` and
`runtime/spu/spu_xdiff.h`; neither is in the tree yet (see Status).

**Status.** Design. Nothing in this document is implemented in the runtime.
Two prerequisites of the commit plan have landed on `dev`: the SPU lifter ABI
gate (`1b4eac28`) and raw job images with a JSON effects inventory in the SPU
pipeline (`3d4f0596`); section 12 says what they cover and what is left. This
document lands alone. The text of `runtime/spu/spu_effect_port.h` and
`runtime/spu/spu_xdiff.h` is a draft on the branch `m4/xdiff-design`; the
layouts of section 7 (sizes and fields) are the specification, and where the
draft and this text differ (the checksum range in 7.1, the payload of the
`WRCH` of `MFC_WrListStallAck` in 7.5) this text rules. Each header lands with
its first consumer: `spu_effect_port.h` with commit 4, which defines and tests
`spu_port_*` (without the draft's bitmask `spu_effect_inventory`, which
duplicates the pipeline's JSON inventory and has no producer, section 12.3);
`spu_xdiff.h` with commit 7 (the fixture layout and the tier enum its
identity section needs) and commit 8 (the verdict and reason enums). Each
section is marked *decided* (the commit plan builds it as written),
*proposed* (built later, may change with its first consumer) or *open* (needs
the named evidence). A claim about existing code carries a `file:line` anchor
at toolkit `dev` `e53652e2`. `[INFERENCE]` marks a number or a property that is
reasoned and not yet measured or tested.

Contents: [why](#1-why) · [terms](#2-terms) · [tiers](#3-tiers) ·
[identity](#4-identity) · [effect port](#5-the-effect-port) ·
[transcript](#6-the-transcript) · [fixture](#7-the-fixture-ps3recompspujob1) ·
[matching](#8-matching) · [shadow contexts](#9-shadow-contexts) ·
[termination](#10-bounded-execution-and-fault-isolation) ·
[modes](#11-modes-and-roles) · [build-time contract](#12-build-time-contract) ·
[offline](#13-offline-replay-synthetic-worlds-corpora) ·
[traces](#14-trace-records) · [PPU](#15-ppu-context-only) ·
[commit plan](#16-commit-plan) · [open questions](#17-the-open-questions) ·
[measurements owed](#18-measurements-owed)

---

## 1. Why

- **The only production body of a JobQueue2 job is the interpreter.** A worker
  asks `ExecutorHooks::select_body` and otherwise takes `interpreter_body()`
  (`libs/spurs/jq2/executor.cpp:1151-1153`, `:1574`); `docs/JQ2.md:670-672`
  calls the hook "the plug for lifted bodies once the job-image registry
  exists". In a title that drives its audio through JobQueue2 this is where
  the process CPU goes (fork issues #87, #106).
- **Lifting is not the obstacle.** The lifter takes a raw job image and a
  `--code-end` (`tools/spu_lifter.py:1241`), and the function finder has a raw
  mode (`tools/find_spu_functions.py:608-621`).
- **Trust is.** A job that uses lock-line atomics or a mailbox is not a closed
  function of its local store and its GET data, so comparing two runs of it
  against live memory compares two different worlds. The audit design of #87
  therefore refused every such job, which refuses the audio jobs it was
  written for.

The missing piece is a way to give a second execution of a job exactly the
world the first one saw, and to take from it exactly what the first one
produced. That is what the rest of this document builds.

## 2. Terms

| Term | Meaning |
|---|---|
| tier | one implementation of a job image (section 3) |
| authority | the tier whose result the guest sees; it runs against the real world |
| shadow | a second tier run in-process on the authority's recorded world |
| subject | the body under comparison, whether a shadow in a run or a body under the offline runner |
| effect | anything that crosses the boundary of an SPU context (section 5.4) |
| transcript | the ordered effects of one run of the authority, with every value it read |
| fixture | entry state, transcript and authoritative end state of one unit, as a file |
| unit | what one fixture covers: one job, from the loader's entry state to the end of its run |
| verdict | `equal`, `differ`, `void`, `malformed` or `not-found`, with a reason id |

## 3. Tiers

*Decided.* `spu_job_tier`, `runtime/spu/spu_xdiff.h`.

| Tier | Body | Registered by | Compared under |
|---|---|---|---|
| T0 | the SPU interpreter | nothing: it runs any bytes | same program |
| T1 | the whole image, lifted by `tools/spu_lifter.py` | image fingerprint | same program |
| T2 | T1 with native overrides of single functions | image fingerprint; each override by its LS entry | contract |
| T3 | a native reimplementation of the whole job | image fingerprint | contract |
| tweak | any body that changes behaviour on purpose | as its base tier, flag `SPU_TIER_TWEAK` | never: not comparable |

Every tier reaches the world through the same three functions (section 5), so
every tier records and replays the same transcript, and one fixture corpus
serves correctness, performance and reverse engineering.

`spu_xdiff_match_for(authority, subject)` is the whole rule for which
comparison applies: T0 and T1 execute the same SPU instructions and are held
to the same program; a pair with native code in it is held to the contract;
a tweak has no rule and yields `malformed` / `not-comparable`.

## 4. Identity

*Decided.*

- **An image is the bytes that were staged.** Its identity is
  `spu_workload_fingerprint`, FNV-1a-64 (`runtime/spu/spu_workload.c:56-65`),
  over exactly the `sizeBinary << 4` bytes the loader copied into local store
  (`runtime/spu/spurs_job.c:481`, `:745`, `:912`).
  A raw job image is registered under the same value: the pipeline hashes the
  whole manifest blob, which is the staged bytes (`tools/spu_program_pipeline.py:393`;
  `docs/SPU_LIFTER.md`, "Raw local-store job images").
- **An address is never identity.** One EA holds different binaries over
  time, which is why the trace-side image table re-checks the content of an
  `(ea, bytes)` it has seen before (`executor.cpp:1102-1109`), and one image
  is staged from several EAs.
- **The body is chosen after staging, from the staged bytes.** The loader
  decodes `eaBinary` and `sizeBinary` from the descriptor (`spurs_job.c:477-481`)
  and copies the image later (`:745`); a body picked from the descriptor is
  picked from bytes that may no longer be the ones staged. The loader's start
  hook runs after staging with the context and the layout
  (`spurs_job.c:965-972`, `spu_workload.h:148-156`) and may write the context,
  so selection happens there: fingerprint `ctx->ls + image_ls`, look the
  fingerprint up, set `ctx->image_id`, and let a dispatching entry call the
  body found. There is no candidate to mismatch: an image without a
  registered body runs T0, as today.
  `select_body` remains a test seam for forcing a body; production image
  selection uses the staged fingerprint. A forced test body is intentionally
  not checked against the staged bytes.
- **Finding the fingerprint is cheap; a cache hit is a claim, not a proof.**
  Hashing the staged bytes byte by byte costs about a millisecond for a large
  image, so two places remember a result by address and size and reuse it
  while a word-wise content hash of the bytes still matches: the dispatcher
  (eight slots, `spu_workload.c:347-380`) and, for traces, the per-worker table
  of `identify_image` (`executor.cpp:1119-1137`), which runs only while a
  trace is recorded (`:1170-1173`, `:1198`). The content hash is a change
  detector, never an identity (`spu_workload.h:42-45`, `spu_workload.c:332-346`):
  it is 64 bits of a non-cryptographic fold. A hit says the bytes are
  probably the ones the fingerprint was taken from; it does not say they are.
  - *A sampled job verifies the hit.* The worker already copies the entry
    local store of a sampled job (section 11). It takes the byte-serial
    fingerprint over that copy and compares it with the one the cache gave.
    A difference is a hash collision: the sample is dropped and counted as
    `ineligible`, the cache entry is evicted, and a diagnostic names it. A
    fixture is never written under a fingerprint that was not computed from
    its own bytes.
  - *An unsampled job that runs a registered body trusts the hit.* This is the
    accepted risk: a different image restaged at the same address and size,
    with an equal content hash, would run the previous image's body. The
    dispatcher takes the same risk today; this design does not widen it, and
    it does not claim collision resistance. `[INFERENCE]` for unrelated bytes
    the chance is about 2^-64 per restage; nothing but the fold's mixing
    excludes a structured collision. Whether a byte-serial check on every hit
    that selects a registered body is affordable is a measurement (section
    18); if it is, the rule becomes "every such hit verifies".
  The cost of the identification itself is already in every trace as the
  span of kind 29 (`executor.cpp:1237`, `ps3_ftrace.h:154-157`), so "measure
  first" is a report over an existing trace (section 18).
- **A fixture also names who recorded it and what ran.** `spu_xdiff_identity`
  (section 7.3) carries the effect-port ABI, the context census, the host and
  the toolkit commit; `spu_xdiff_body_id` carries a body's tier and, for a
  lifted body, the lifter ABI, `--code-end` and a hash of the lifter options.
  The lift identity describes a body, so it sits with the body: in the
  fixture for the authority, in the verdict for the subject.

## 5. The effect port

### 5.1 The observation

*Code.* SPU code reaches the world outside its context only through channel
instructions. The interpreter and lifted code issue all of them as three
runtime calls:

| Call | Definition | Interpreter | Lifter |
|---|---|---|---|
| `spu_wrch` | `runtime/spu/spu_channels.c:1111` | `spu_interp.c:355` | `tools/spu_lifter.py:949` |
| `spu_rdch` | `spu_channels.c:1385` | `spu_interp.c:356` | `spu_lifter.py:942` |
| `spu_rchcnt` | `spu_channels.c:1484` | `spu_interp.c:358` | `spu_lifter.py:946` |

MFC DMA, lock-line atomics, mailboxes, signals, events and the decrementer are
all channel operations. Local store is part of the context
(`runtime/spu/spu_context.h:223`). So an SPU job needs no copy-on-write of
guest memory: the boundary to mediate is the channel layer of one context.

`stop` and `halt` are not effects at this boundary. The interpreter sets
`status` and `stop_code` and returns from its run (`spu_interp.c:414-420`); it
has no host frame per guest call, so a stop ends the run (but for `stop 0x110`
with a process-wide event pending, which it services and resumes; section
5.5 skips that for a served context). Lifted code
sets the same fields, calls `spu_stop` and then `return`s from the generated
function (`spu_lifter.py:689-693`). `spu_stop` by default only sets `status`
again; it longjmps to `spu_run_with_halt` only when `YDKJ_STOP_HALT` is set
(`spu_channels.c:236-250`). A lifted stop therefore ends the run when it is
executed in the entry function, and otherwise leaves one host frame and lets
its caller go on. What the run ended on is read from the finished context. It
is part of the end state (section 7.6), and neither `spu_stop` nor `spu_halt`
needs a hook.

**Known T0/T1 divergence: a stop inside a nested lifted call.** The
interpreter ends its run at the stop; the lift returns into the caller and
executes instructions, and possibly effects, the interpreter never reached.
A comparison of such a pair reports `differ` (`effect-extra`, `end-class`,
`end-regs` or `end-ls`), never a false `equal`: `[INFERENCE]` the lift's end
state is that of a run that went on, and a continuation that ends in the
same class with the same state would be the interpreter's own run. The
class is documented, not fixed; commit 8 pins it with a synthetic job.

### 5.2 Why the seam is the three functions and nothing lower

A lower seam (one transfer, one line operation) would give a neater
vocabulary. It is the wrong place, because the code between the channel
functions and guest memory is full of effects a replaying side must not
produce:

- named stops that end the process: a malformed transfer
  (`runtime/spu/spu_dma.h:187-209`), a PUT into the label window (`:491-496`,
  `spu_channels.c:760-771`), a repeated DMA (`spu_dma.h:1455`);
- hooks into other subsystems: the GCM put hook (`spu_dma.h:822-823`), the
  outbound-mailbox and user-event hooks (`spu_channels.c:1197-1199`), the
  line-commit hook (`:1065`);
- locks and shared registries: the per-context MFC engine table
  (`spu_channels.c:549`, `:562`), the lock-line lock (`:838`, `:958`), the
  reserver list (`:842`), peer wake-ups (`:1009`);
- blocking: a wait on an empty channel (`spu_channels.c:1341-1380`).

A subject that diverged may issue any request at all. If the seam were below
this code, a wrong EA from a wrong lift would stop the game by name. With the
seam at the three functions, nothing below it runs for a served context, and
that holds by construction instead of by an audit of every stop site.

### 5.3 Two halves: serve and tap

*Decided.* `spu_effect_port`, `runtime/spu/spu_effect_port.h`.

| Backend | `serve` | `tap` | The context's world |
|---|---|---|---|
| real | none | none | the runtime; no port installed |
| record | none | recorder | the runtime, written down |
| replay | transcript | none | one recorded run |
| synthetic | seeded model | recorder | a model; its run becomes a fixture |

- **`serve` replaces the channel bodies.** Its three members are required.
  A served context touches no guest memory, reservation, mailbox, lock or
  clock. A serve callback never blocks: a mailbox read the transcript does
  not hold is a divergence at once, not a wait. When it cannot answer it
  keeps the reason and ends the run through the seam's `spu_port_serve_halt`,
  which calls `spu_halt`, the path every lifted job already ends on
  (`spu_channels.c:192-209`, `:3010`). It never invents a value, and the halt
  has to be real: `spu_halt` leaves only while a landing pad is armed
  (`s_spu_halt_armed`, `:209`) and otherwise returns. The seam therefore
  aborts the process, with a diagnostic naming the context, if `spu_halt`
  returns: a served context without a pad crashes instead of going on with a
  value nobody recorded. The shadow driver runs under `spu_run_with_halt`,
  which arms the pad (`:301-349`).
- **`tap` is told and cannot answer.** Its callbacks return `void` and take a
  `const` context, so a recorder has no way to change what the authority
  does. The body that runs is the one that would have run without it.

The draft of this design described `record` as a port that wraps `real`. A
wrapper must remember to call the real body and to return its value
unchanged; a tap cannot get either wrong. The existing observer
(`runtime/spu/spu_observe.h:50-64`) is the same idea, and it is not reused
for two reasons. It lacks three things a transcript needs (channel counts,
the single transfers of a list command, their bytes), and a context has one
observer slot, which JobQueue2's job observer hands to tools at job start
(`docs/JQ2.md:756-765`, `spu_workload.h:148-151`): a recorder in that slot
would be silently replaced by the first tool that installs its own.

`synthetic` composes the halves: the model serves, the recorder taps, and a
serve table that moves bytes reports them through `spu_port_report_xfer`,
exactly as the runtime's MFC engine does.

### 5.4 What is an effect

*Decided.* Every `spu_rdch`, every `spu_rchcnt` and every `spu_wrch` except
the five MFC parameter latches: `MFC_LSA`, `MFC_EAH`, `MFC_EAL`, `MFC_Size`,
`MFC_TagID` (`spu_port_is_latch`). The runtime handles them first and returns
(`spu_channels.c:1130-1138`), because they only store a value in the context.
They stay there for every backend. Their values reach a port as the request
of the `MFC_Cmd` write that consumes them, which is also when a wrong one
first matters, and no channel reads them back. `spu_port_is_latch` spells the
five channel numbers (16 to 20) because `spu_effect_port.h` is included where
`spu_context.h` is not. The commit that lands it asserts at compile time, in
`spu_channels.c`, which includes both, that `MFC_LSA`, `MFC_EAH`, `MFC_EAL`,
`MFC_Size` and `MFC_TagID` are the channels 16 to 20 it tests. `spu_xdiff.h`
repeats `SPU_LS_SIZE` as `SPU_XDIFF_LS_BYTES` and gets the same assertion
with it (commit 7).

`MFC_WrTagMask` sits in the same early switch (`:1136`) but is an effect.
The mask decides what `MFC_RdTagStat` returns (`spu_dma.h:1834`, `:1788`),
`MFC_RdTagMask` reads it back (`:1864-1865`), and guest code compares the
status with the mask it wrote (`:1836-1837`). Were it a latch, a replay would
serve the authority's status and mask to a subject that wrote another mask,
and neither the transcript nor the end state (section 8.1) would hold the
difference: a false `equal`. As a `WRCH` it costs one record per tag wait.
Commit 4 moves it below the port test and keeps today's inline store for
`port == NULL` (section 5.5), so the real path does not reach the MFC engine
table and its registry lock for it (`spu_channels.c:549`, `:562`).

Reads are not split into "private" and "world". `MFC_RdTagMask` returns
context state and could bypass the port, but one rule without exceptions is
worth more than a few saved records per job: a replay then needs no model of
the MFC at all (section 8.1).

Not yet an effect: `MFC_SNDSIG`. The runtime has no path for a signal
notification write: `MFC_SNDSIG_CMD` is only a constant (`spu_context.h:146`),
and the command decode classifies by opcode bits, which put 0xA0 in the PUT
class (`spu_dma.h:155-163`). When the runtime implements it, it is an effect
to add to section 6 and to the signals of section 8.2's epoch table, and
`SPU_EFFECT_PORT_ABI` moves with it.

### 5.5 The seam, and the crossings it does not cover

*Decided; commit 4.*

```
spu_wrch    latch?            -> store, return                 (five channels)
            MFC_WrTagMask, ctx->port == NULL
                              -> inline store, return          (today's store, no registry lock)
            ctx->port == NULL -> today's body                  (one branch)
            else              -> tap->wrch; serve->wrch or today's body;
                                 tap->xfer from inside either
spu_rdch    ctx->port == NULL -> today's body
            else              -> serve->rdch or today's body; tap->rdch(value)
spu_rchcnt  as spu_rdch
```

The runtime reports transfers at its two funnels: after each
`mfc_do_transfer` that moved bytes (the plain command, `spu_dma.h:1695`, and
each list element, `:906`, which also covers a list resumed inside
`MFC_WrListStallAck`, `:999`), and where an atomic command completes
(`spu_mfc_atomic_done`, `spu_channels.c:659-664`).

Three functions do not cover everything a context can reach. Each remaining
crossing, found by reading the paths a job context runs through:

| Crossing | Where | Closed by |
|---|---|---|
| `bisled` reads the event status without a channel read | `spu_interp.c:392-397`, `spu_lifter.py:1066-1074` | the pipeline inventory's event-branch list (section 12.3, detection owed): an image with one is not eligible |
| interrupt delivery at a transfer | `spu_context.h:1087-1089`, `:1127-1130`; `spu_lifter.py:570` | the same list: a job that can enable interrupts is not eligible |
| the interpreter serves `stop 0x110` from a process-wide pending event | `spu_interp.c:858-869` | skipped for a served context; the stop ends its run |
| the interpreter's JobQueue2 taskset HLE at PC `0xA70` | `spu_interp.c:780-790` | a served context bypasses that HLE and ends as an unsupported dispatch |
| the branch resolver: kernel services, taskset syscalls, overlay matching, interpretation of unlifted code, diagnostics | `spu_channels.c:2843-2911`, `:3068-3120`, `:3392` | a served context takes a closed path: lifted lookup, the planted return (`:3005-3012`), the job runtime area (`:3020-3025`), anything else ends the run as `unlifted-branch`; implemented in commit 4 |
| the dispatcher recognizes resident overlays after a GET | `spu_dma.h:695-699`, `spu_channels.c:2340` | not reproduced for a served context. A body that streams code and branches to it ends `unlifted-branch`, never a false `equal` |
| the lockstep ring joins every run | `spu_channels.c:344` | no shadow runs while the gate is armed; the samples count as dropped |
| the host float mode of a run | `spu_channels.c:303` | an input, not an effect: a shadow runs under `spu_run_with_halt` like the authority, and the fixture records the mode (`float_rz`). The mode is latched once per process from `SPU_FLOAT_RZ`, arm64 only (`:259-269`), so the runner compares it with the fixture's instead of assuming it (section 8.1) |
| an external stop request | `spu_context.h:474`, `spu_channels.c:292-297` | nobody else holds a pointer to a shadow context (section 9) |

Not closed: capped diagnostics on stderr. A shadow passes some of the same
`fprintf` sites as a real job. Their caps are per thread
(`SPU_THREAD_LOCAL`), a shadow runs on its own thread, and the closed
resolver path removes most of them, but a line from a shadow can still look
like one from the game. Commit 10 prefixes the audit thread's stderr lines;
until then this is a known wart.

### 5.6 The real path

With no port, each channel operation pays one load of `ctx->port` and one
predictable branch; the five latch writes of every DMA
(`spu_channels.c:1126-1135`) pay nothing, and `MFC_WrTagMask`, written once
per tag wait, pays the branch and keeps its inline store (`:1136`), so the
registry lock stays off the real path. The pointer
sits behind `observer` (`spu_context.h:458`), which `spu_wrch` and `spu_rdch`
already load (`spu_channels.c:1124`, `:1477`). `spu_rchcnt` does not use the
observer, so its context-line cost is not covered by that argument. No field
that lifted code addresses moves (section 12.1). All hot-path cost claims
are `[INFERENCE]` until the bench rows in section 18 are measured.

## 6. The transcript

*Decided.* `spu_xdiff_effect`, 40 bytes, one record per tap callback, in the
order the callbacks arrive.

| Kind | Recorded | Source |
|---|---|---|
| `WRCH` | channel, value; for `MFC_WrListStallAck` also the bytes of the list still parked on the tag | the write |
| `RDCH` | channel, value returned | after the read |
| `RCHCNT` | channel, count returned | after the count |
| `MFC_CMD` | command word, LSA, EAH:EAL, size, tag; for a list command the list bytes | `MFC_Cmd` write and the latches |
| `XFER` | opcode, direction, EA, LSA, size, tag, the bytes; for an atomic the status | one `spu_port_xfer` |

- An `XFER` belongs to the nearest `WRCH` or `MFC_CMD` before it. A plain GET
  is one `MFC_CMD` and one `XFER`. A list command is one `MFC_CMD` and one
  `XFER` per element that moved bytes; a list that stalls yields the rest
  under the `WRCH` of `MFC_WrListStallAck` that resumes it.
- **The recorder never walks a list and never reads guest memory.** It takes
  the bytes from the context's local store at the reported `lsa`, after a
  transfer to local store landed and while the source of a transfer from it
  is intact. What it records is therefore what the runtime did, including the
  cases where the runtime moved nothing or zeroed a line
  (`spu_channels.c:738-750`). A recorder that re-derived the element list
  could compare the wrong bytes without anyone noticing.
- **The list itself is part of the request.** Two lists that move the same
  bytes are two different programs; the list bytes are the `MFC_CMD`
  record's payload. The same holds on resume: a parked list's remaining
  elements are read from local store again when `MFC_WrListStallAck` resumes
  it (`spu_dma.h:883-889`, `:999-1001`), after the guest's stall handler may
  have rewritten them. The `WRCH` of `MFC_WrListStallAck` therefore carries
  the bytes of the elements still parked on its tag (`list_stall_remaining`
  times 8 bytes at `list_stall_elem_lsa`, `spu_context.h:432-433`), which the
  recorder reads from the context before the write takes effect.
- Atomics: `GETLLAR` is an `XFER` to local store of 128 bytes with status 4;
  `PUTLLC` an `XFER` from local store with status 0 (stored) or 1 (lost
  reservation); `PUTLLUC` and `PUTQLLUC` status 2 (`spu_observe.h:56-58`).
  The later `MFC_RdAtomicStat` read is its own `RDCH`.
- Every nondeterministic input is a recorded value: the decrementer, which
  the runtime derives from the host clock (`spu_channels.c:1445-1462`), the
  tag status, the mailbox count.
- `pc` is `ctx->pc` at the effect. The interpreter stores it before every
  channel instruction (`spu_interp.c:355-358`); lifted code stores it at
  transfers only. It locates a divergence and is never compared. When T0 is
  one side of a pair, its `pc` is exact.

Example, a job that reads 256 bytes, bumps a counter under a reservation and
reports through the outbound mailbox:

```
#  kind     ch  value  ea          lsa      size  status
0  MFC_CMD  21  0x40   0x00A01000  0x08000  256           GET
1  XFER         0x40   0x00A01000  0x08000  256           -> 256 bytes
2  WRCH     23  2                                         tag update: all
3  RDCH     24  0x1                                       tag status
4  MFC_CMD  21  0xD0   0x00A02000  0x09000  128           GETLLAR
5  XFER         0xD0   0x00A02000  0x09000  128   4       -> the line
6  RDCH     27  4                                         atomic status
7  MFC_CMD  21  0xB4   0x00A02000  0x09000  128           PUTLLC
8  XFER         0xB4   0x00A02000  0x09000  128   0       <- the line, stored
9  RDCH     27  0
10 WRCH     28  0x1                                       out mailbox
```

## 7. The fixture: `ps3recomp.spujob/1`

*Decided.* All types are in `runtime/spu/spu_xdiff.h` (a draft on the branch
`m4/xdiff-design`, landing with commits 7 and 8), with their sizes
asserted there. All integers are little-endian. Guest data (register and
local-store bytes, payloads) is stored in guest byte order, untouched.

### 7.1 Container

One file per unit: a divergence artefact has to be one thing that can be
attached, copied and replayed. A header, a section table, then the sections
in any order, each at an 8-byte boundary, none overlapping, padding zero.

`spu_xdiff_file_header`, offset 0, 40 bytes:

| Off | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u8[8]` | `magic` | `"SPUJOB\r\n"` |
| 8 | `u32` | `version` | 1 |
| 12 | `u32` | `header_bytes` | 40; the section table starts here |
| 16 | `u32` | `section_count` | at most 32 |
| 20 | `u32` | `section_bytes` | 24 |
| 24 | `u64` | `file_bytes` | the whole file; a shorter file is truncated |
| 32 | `u64` | `checksum` | FNV-1a-64 over bytes `[0, file_bytes)`, this field read as zero: the header is covered too |

`spu_xdiff_section`, 24 bytes: `u32 tag`, `u32 flags` (0), `u64 offset` (from
the start of the file), `u64 bytes`.

### 7.2 Sections

| Tag | Content | Required |
|---|---|---|
| `IDEN` | `spu_xdiff_identity` | yes |
| `UNIT` | `spu_xdiff_unit` | yes |
| `REG0` | entry registers, 128 x 16 bytes | yes |
| `LS_0` | entry local store, extents over a zero-filled store | yes |
| `EFCT` | `spu_xdiff_effect[]` | yes, may be empty |
| `BLOB` | payload bytes the effects refer to | yes, may be empty |
| `REG1` | the authority's end registers | yes |
| `LS_1` | the authority's end local store, extents over the entry state | yes |
| `END_` | `spu_xdiff_end` | yes |
| `VRDT` | `spu_xdiff_verdict_record`: the verdict that made the runtime write the fixture | no |

- **Registers** are in guest byte order: byte *k* of register *r* is the byte
  a `stqd` of *r* stores at offset *k*, the conversion `spu_ls_write128`
  makes (`spu_context.h:811-835`). The file does not depend on a host's
  `u128` layout.
- **Local store** is a sequence of `spu_xdiff_ls_extent { u32 lsa; u32 bytes; }`,
  each followed by its bytes padded to 8, sorted, not overlapping. `LS_0`
  skips zero runs; `LS_1` contains the end-state bytes that differ from the
  entry state, not bytes that were written and later restored. Any exact
  cover is valid.
- **Payloads** are deduplicated inside a fixture: the writer keeps a table of
  payloads it has stored, keyed by length and content hash and confirmed by
  comparing bytes, and reuses the offset. A retry loop that reads one line
  eight times stores it once. Nothing is deduplicated across fixtures: a
  fixture stays self-contained, and a corpus is compressed as a whole.
- **Compression** is not part of the format. A reader accepts a file that
  starts with the zstd frame magic and decodes it, as the trace tools do
  (`docs/TOOLS.md:1015-1026`); the runtime writes plain files and needs no
  library. The extents already remove most of the two 256 KiB stores.
- **Writing.** To `<path>.partial`, then renamed, as the toolkit's other
  artefacts are (`include/ps3emu/ps3_ftrace.h:26-28`). The name is
  `<fingerprint as 16 hex digits>-q<queue>-s<seq>.spujob`.

### 7.3 `IDEN`: `spu_xdiff_identity`, 80 bytes

| Off | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u64` | `image_fingerprint` | `spu_workload_fingerprint` over the staged image |
| 8 | `spu_xdiff_body_id` | `authority` | the body whose run this is |
| 40 | `u32` | `effect_port_abi` | `SPU_EFFECT_PORT_ABI` of the recording runtime |
| 44 | `u32` | `context_census` | that runtime's `spu_context` census (section 9) |
| 48 | `u32` | `host_arch` | 1 arm64, 2 x86-64 |
| 52 | `u32` | `host_isa` | `SPU_XDIFF_ISA_*`: the helper paths the build compiled |
| 56 | `u8[20]` | `toolkit_commit` | git commit of the recording toolkit, raw; zero: unknown |
| 76 | `u8` | `toolkit_dirty` | 1: built from a modified tree |
| 77 | `u8` | `float_rz` | 1: the run used the native round-toward-zero float mode |
| 78 | `u8[2]` | `reserved` | 0 |

`spu_xdiff_body_id`, 32 bytes:

| Off | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u8` | `tier` | `spu_job_tier`, with `SPU_TIER_TWEAK` when it applies |
| 1 | `u8[3]` | `reserved` | 0 |
| 4 | `u32` | `lifter_abi` | the SPU lifter ABI the body was generated for; 0 for T0, T3 |
| 8 | `u32` | `code_end` | the lifter's `--code-end`, an LS address; 0: none |
| 12 | `u32` | `reserved2` | 0 |
| 16 | `u64` | `lift_options` | FNV-1a-64 of the lifter's canonical option string; 0 for T0, T3 |
| 24 | `u64` | `native_rev` | T2, T3: the revision the registrant declares; else 0 |

On `dev` the registration check stops a lift whose `SPU_LIFTER_ABI` differs
from the runtime's (section 12.1), so every registered lifted body was
generated for `SPU_RUNTIME_LIFTER_ABI`, and that is the value a recorder
writes; the per-image value is not carried into registration. `code_end` and
`lift_options` are known at lift time (section 12.2) and stay 0 until a
registration carries them (section 12.3).

The image is not named by address here. The fingerprint covers bytes
`[unit.image_ls, +unit.image_bytes)` of the entry local store; a reader
recomputes it and answers `malformed` / `fixture-identity` on a mismatch.

### 7.4 `UNIT`: `spu_xdiff_unit`, 104 bytes, 26 x `u32`

| Off | Field | Meaning |
|---|---|---|
| 0 | `kind` | 1: one job |
| 4 | `profile` | 1 jobchain loader, 2 JobQueue2 loader; selects the end classification |
| 8 | `entry_pc` | `ctx->pc` at entry |
| 12 | `return_ls` | the link the loader planted (`spurs_job.c:921`, `:949`) |
| 16 | `job_rt_ls` | `ctx->job_rt_ls` (`spurs_job.c:910`) |
| 20 | `job_rt_ls2` | `ctx->job_rt_ls2` (`spurs_job.c:911`) |
| 24 | `image_ea` | guest EA the image was staged from |
| 28 | `image_ls` | LS base of the staged image |
| 32 | `image_bytes` | staged bytes: the descriptor's `sizeBinary << 4` |
| 36 | `bss_bytes` | the `eaBinary` high word x 16 |
| 40, 44 | `desc_ls`, `desc_bytes` | the descriptor copy |
| 48, 52 | `io_ls`, `io_bytes` | the in/out buffer |
| 56, 60 | `out_ls`, `out_bytes` | the out buffer |
| 64, 68 | `scratch_ls`, `scratch_bytes` | the scratch block |
| 72 | `stack_bytes` | the stack's size |
| 76 | `r1` | the stack pointer at entry |
| 80 | `ctx_ls` | the `CellSpursJobContext2` |
| 84 | `origin_queue` | queue index; `origin_*` join the fixture with the trace of its run and are 0 for a synthetic fixture |
| 88 | `origin_worker` | worker index |
| 92 | `origin_seq` | the job's per-queue sequence number |
| 96 | `origin_descriptor_ea` | the descriptor's guest EA |
| 100 | `reserved` | 0 |

Offsets 24 to 80 are `spu_job_layout`, its 15 members in order, as the
loader's start hook received it (`spu_workload.h:132-146`).

This section is complete on purpose. A job context at entry is
`spu_context_init`, then `image_id`, `job_rt_ls`, `job_rt_ls2`, local store,
registers and `pc` (`spurs_job.c:908-936`). Everything else is zero. That is
what lets a replaying context be built from a fixture (section 9).

### 7.5 `EFCT`: `spu_xdiff_effect`, 40 bytes

| Off | Type | Field | `WRCH` | `RDCH` | `RCHCNT` | `MFC_CMD` | `XFER` |
|---|---|---|---|---|---|---|---|
| 0 | `u8` | `kind` | 1 | 2 | 3 | 4 | 5 |
| 1 | `u8` | `channel` | channel | channel | channel | 21 | 0 |
| 2 | `u8` | `tag` | 0 | 0 | 0 | tag group | tag group |
| 3 | `u8` | `dir` | 0 | 0 | 0 | 0 | 1 to LS, 2 from LS, 3 zero EA |
| 4 | `u32` | `pc` | `ctx->pc` | | | | |
| 8 | `u32` | `value` | written | returned | count | command word | opcode, list bit clear |
| 12 | `u32` | `status` | 0 | 0 | 0 | 0 | atomic status, else 0 |
| 16 | `u64` | `ea` | 0 | 0 | 0 | EAH:EAL | first EA |
| 24 | `u32` | `lsa` | 0 | 0 | 0 | `MFC_LSA` | first LS byte, masked |
| 28 | `u32` | `size` | 0 | 0 | 0 | `MFC_Size` | bytes moved |
| 32 | `u32` | `blob` | none | none | none | list commands: the list | the bytes; none for zero EA |
| 36 | `u32` | `reserved` | 0 | | | | |

"none" is `SPU_XDIFF_NO_BLOB`. A payload is `size` bytes at offset `blob` of
the `BLOB` section. One `WRCH` uses fields its column leaves empty: for
`MFC_WrListStallAck` with a list parked on the tag, `lsa` is the address of
the next element, `size` the bytes of the elements left (8 each) and `blob`
their bytes; with no list parked they stay 0, 0 and none. The record layout
does not change.

### 7.6 `END_`: `spu_xdiff_end`, 16 bytes

| Off | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `end_class` | 1 `return`, 2 `stop`, 3 `halt`, 4 `jm-call`, 5 `other` |
| 4 | `u32` | `stop_code` | `ctx->stop_code` |
| 8 | `u32` | `pc` | `ctx->pc & SPU_LS_MASK`, as the tier left it |
| 12 | `u32` | `status` | `ctx->status`, as the tier left it |

The class exists because the raw fields depend on the tier. An interpreted
job returns by executing the `stop 0` at the planted link (status STOP, pc
link + 4); a lifted one is halted by the resolver when it branches there (pc
the link, status neither STOP nor HALT). `require_normal_return` already
states both (`executor.cpp:1053-1079`), but it stops the process on anything
else; commit 5 splits a pure classifier out of it. The class, and for `stop`
the code, are compared; `pc` and `status` only locate.

### 7.7 `VRDT`: `spu_xdiff_verdict_record`, 144 bytes

| Off | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | `verdict` | `spu_xdiff_verdict` |
| 4 | `u32` | `reason` | `spu_xdiff_reason` |
| 8 | `u32` | `match` | 1 same program, 2 contract |
| 12 | `u32` | `effect_index` | the record the subject failed at; the record count for `effect-extra`; `SPU_XDIFF_NO_EFFECT` for an end-state reason |
| 16 | `spu_xdiff_body_id` | `subject_body` | the body that replayed |
| 48 | `spu_xdiff_effect` | `subject` | what the subject issued there; kind 0: nothing |
| 88 | `u32` | `where` | first differing payload byte, register index or LS address |
| 92 | `u32` | `subject_pc` | the subject's `ctx->pc` at the verdict |
| 96 | `u8[16]` | `want` | the authority's bytes from `where` |
| 112 | `u8[16]` | `got` | the subject's |
| 128 | `u64` | `budget` | the subject's budget in the tier's unit (instructions for T0, backward transfers for T1); 0: none |
| 136 | `u64` | `budget_spent` | |

The subject's full end state is not stored. A replay is deterministic, so the
offline runner regenerates it from the fixture.

### 7.8 What a reader rejects

A fixture is an input file: it may be truncated, corrupt or hostile. A reader
answers `malformed` / `fixture-format` and reads no further when: the magic,
version or sizes are not the ones above; `file_bytes` is not the file's
size; the checksum fails; a section lies outside the file, overlaps another
or is misaligned; a required section is missing or duplicated; a section's
size is not the one its type requires; an effect count exceeds
`SPU_XDIFF_MAX_EFFECTS` or the blob exceeds `SPU_XDIFF_MAX_BLOB_BYTES`; an
effect has an unknown kind, a nonzero unused field, a `blob + size` outside
`BLOB`, or an `lsa + size` outside local store; an `XFER` has no command
before it; an extent is unsorted, overlapping or outside local store.

### 7.9 Size

`[INFERENCE]`, to be replaced by a measured row (section 18): an audio job
that moves about 16 KiB by DMA stores its image (tens of KiB) and descriptor
in `LS_0`, the local-store bytes whose final value differs in `LS_1`, and
its payloads once. That is on the order of 100 KiB per fixture before
compression, against 0.5 MiB for two raw local stores.

## 8. Matching

### 8.1 Same program: T0 against T1

*Decided.* The subject must issue the same effects in the same order with the
same arguments. A replay is a cursor over `EFCT`, with no model of the MFC:

1. `wrch` of a non-`MFC_Cmd` channel: the next record must be a `WRCH` with
   that channel and value. For `MFC_WrListStallAck` the bytes at the record's
   `lsa` in the subject's local store must equal the recorded remaining list
   (`effect-request`, as for a list at issue): the runtime reads the elements
   again at the resume, so a subject that rewrote them would still be served
   the authority's transfers, and only the end state could show it. Then
   apply the `XFER` records that follow it (a list resume).
2. `wrch` of `MFC_Cmd`: the next record must be an `MFC_CMD` with that
   command word and the context's latches; for a list command the list bytes
   in the subject's local store must equal the recorded ones. Then, for each
   `XFER` that follows: to local store, copy the recorded bytes in; from
   local store, compare the subject's bytes with the recorded ones; zero EA,
   nothing.
3. `rdch`, `rchcnt`: the next record must be that kind and channel; return
   its value.
4. At the end of the run every record must have been consumed.

The first failure is the verdict, by the reason ids of `spu_xdiff.h`:
`effect-kind` (another kind or channel), `effect-request` (other arguments or
another list), `effect-payload` (other bytes sent), `effect-extra` (an effect
past the last record), `effect-missing` (records left over). The report names
the record index, both requests and both `pc`.

**End state: every register, all of local store, the end class.** Not a
declared subset. The argument, from what both tiers write:

- Both execute the same instructions on the same entry state with the same
  values read, and every instruction's effect on registers and local store is
  defined bit for bit; the per-instruction differential already holds the
  interpreter and the lifter's output to it
  (`runtime/spu/tests/spu_interp_diff_test.c`). A lift has no host state that
  could stand in for a guest register: `brsl` writes the link register in
  both tiers, and host call frames are not in local store.
- The C compiler cannot drop a store that matters: `ctx->gpr` and `ctx->ls`
  are memory reachable from every runtime call, so every store is done before
  the next effect and before the end.
- Nothing the lift omits is architectural. `fscrrd` reads zero and `fscrwr`
  is empty in the interpreter (`spu_interp.c:368-370`); there is no hidden
  FPSCR to disagree about.
- State the runtime keeps for a channel (event mask, tag mask, decrementer,
  mailboxes) is covered by the transcript: every write that formed it is a
  `WRCH` record matched in order, and every read of it is served from a
  record. The five parameter latches are not records, but every value of
  theirs that can matter is part of the `MFC_CMD` request that consumes it,
  and no channel reads them (section 5.4).

A declared subset would hide exactly the defect this comparison exists for: a
lift that computes a wrong value into a register nobody reads on this input.

Across hosts the same rule is a test of the helpers. A fixture recorded on
one host and replayed on another that differs shows a helper whose result
depends on the host; `host_isa` says which paths recorded it.

**Process-latched knobs.** The runtime latches some behaviour from the
environment on first use, and a fixture does not carry it. The native float
mode is one: `SPU_FLOAT_RZ=1` runs SPU code in round-toward-zero with
flush-to-zero, on arm64 only (`spu_channels.c:259-269`; elsewhere the exact
forms always run). Others change what a run does at a stop, a return or a
branch: `YDKJ_STOP_HALT` (a lifted `stop` halts, `spu_channels.c:244-247`),
`SPU_TAILRET` (`spu_drain.c:526-531`), `SPU_NO_LS0_END` (`spu_drain.c:185`)
and `SPU_INTERP_UNLIFTED` (the resolver interprets live bytes,
`spu_channels.c:3098-3109`). A replay under other values than the recording
compares different programs, so the runner, and the live driver for its
shadow, fail closed:

- it compares the effective float mode with the fixture's `float_rz` before
  anything runs and answers `malformed` / `fixture-identity` on a difference;
- it refuses to run while any of the other knobs is set: exit status 1 and a
  diagnostic naming the variable, no verdict.

Long-term these knobs move into typed configuration (section 11), and the
fixture identity then carries the ones that matter.

### 8.2 Contract: a pair with native code

*Proposed; built with the first T2 or T3 consumer.* A native body need not
issue the authority's commands, use its local-store addresses or wait where
it waited. What it owes is the job's observable behaviour.

**What is observable.** A job's writes to the world and its signals, in an
order the hardware would have guaranteed, and how it ended. For the
JobQueue2 profile its local store and registers are not: the context is on
the loader's stack and the staging store is freed when the job returns
(`spurs_job.c:904`, `:1068-1071`), and the queue's loader keeps no local
store (`retain_ls` 0, `:1106`). The jobchain loader does keep it (`:1078`,
`:1013-1016`), so for that profile local store stays part of the contract.

**Epochs.** The runtime's MFC is synchronous: every command completes at its
`MFC_Cmd` write (`spu_dma.h:149-152`, `:1682-1756`). The hardware's is not,
and guarantees order only at synchronisation points. A program correct on
hardware cannot depend on more, so the authority's transcript is cut into
epochs at:

| Boundary | Kind |
|---|---|
| a read of `MFC_RdTagStat` after a nonzero `MFC_WrTagUpdate` | ordering point, for the tag groups in the mask last written |
| a command with the fence or barrier bit; `0xC0`, `0xC8`, `0xCC` | ordering point |
| an atomic command | signal |
| a write to the outbound or interrupt mailbox; a read or count of the inbound mailbox, a signal channel or the event status | signal |
| a list element with stall-and-notify | signal |
| the end of the run | signal |

**The rule.**

1. Each epoch has a write image: for every guest byte the authority wrote in
   it, the last value.
2. The subject's writes are matched in its own order against the current
   epoch's image, byte by byte. A byte of a later epoch while the current one
   is incomplete is `epoch-order`; a byte in no epoch, or a wrong value, is
   `epoch-writes`. A complete image followed by an ordering point advances
   the epoch. A tag-status ordering point counts only when the subject's
   mask covers every tag group it used for a write of the epoch; a wait on
   other tags is `epoch-order`.
3. A signal of the subject must be the authority's next signal (same kind,
   channel or opcode, EA, value or bytes), and the epoch before it must be
   complete. Atomics are matched one for one: a subject receives the recorded
   line and the recorded status, so it repeats a lost reservation as often as
   the authority did.
4. A read is served from the recorded reads of the same bytes: in the epoch
   the authority read them in, or in any epoch when the authority read them
   in two or more epochs and always saw the same value. Anything else is
   `void` / `uncovered-read`, never served from live memory.
   A channel read of state the subject keeps is answered from that state,
   as the runtime's synchronous MFC would answer it: `MFC_RdTagMask`
   returns the subject's last `MFC_WrTagMask`, `MFC_RdTagStat` that mask
   without the tags of a stalled list (`spu_dma.h:145`, `:1002-1003`,
   `:1755-1756`).
5. The end classes must be equal.

An uncovered read is `void`, not `differ`: the transcript cannot say what the
read would have returned, so it proves nothing. It blocks promotion all the
same (section 12.4).

**Each relaxation and what withdraws it.**

| Relaxation | Rests on | Falsifier: seen in a transcript, it withdraws the relaxation for that epoch |
|---|---|---|
| a transfer may be split or merged | an MFC transfer is not atomic beyond a lock line; the runtime commits a PUT line by line (`spu_dma.h:748-800`) | the authority reads back bytes it wrote in the same epoch and sees another value: the range is contended, commands are matched one for one |
| writes of one epoch may be reordered | no order within a tag group without a fence, a barrier or a wait | two writes of the epoch overlap; or the job never waits, so the whole run is one epoch: both are matched in order |
| a stable byte may be read at another time | all recorded reads of it agree, in at least two epochs | two recorded reads of the byte disagree |
| private end state is not compared | the JobQueue2 loader destroys it | the profile retains local store |
| registers and stack a native function leaves dead may differ (T2) | lifter liveness (section 17, question 10) | the poison run below changes the transcript |

**The poison run** checks a "dead" claim instead of trusting it. Under the
offline runner, T0 runs the fixture twice with an instruction observer
(`spu_observe.h:53`). At the override's return address the second run
overwrites every register and stack byte claimed dead with a second pattern.
If its transcript or its remaining state differs from the first run, the
claim is false for that fixture and the override is not comparable.

A T2 body is judged twice: as a whole job under the contract, which is what
makes it sound, and per overridden function with the masks, which is what
says where it went wrong. The second needs a function as a unit; `kind` in
`UNIT` leaves room for it and version 1 does not define it.

**Open, before any T2/T3 promotion:** an MFC PUT can also invalidate a
reservation held by a peer context. Plain PUT calls
`spu_coh_notify_write_except` per reserved line
(`spu_dma.h:779-800`); the helper raises `SPU_EVENT_LR`, clears the
reservation and wakes the holder (`spu_coherency.c:286-308`). It also wakes
registered line watchers (`spu_coherency.c:286-306`); a taskset watcher can
broadcast its `WAIT_SIGNAL` condition (`spu_workload.c:1468-1484`,
`:1670-1674`). Atomics call `spu_coh_notify_write` and may run the line
commit hooks (`spu_channels.c:1009-1010`, `:1063-1086`). The transcript
records transfer bytes and atomic status, not the peer recipients or watcher
wakes. Reordering, splitting or merging a write cannot be declared
contract-equivalent until these notifications are represented. Close this
with a synthetic concurrent-peer fixture: reserve a line and register a
watcher, then vary an overlapping write and verify the recipient set and
event order. Until then, the contract rule is a proposal, not a promotion
gate. For same-program T0/T1, exact command and XFER matching invokes the
same runtime path; the shadow intentionally does not issue live peer wakes.

### 8.3 Verdicts

*Decided.* The values are the offline runner's exit status and follow the
shader differential (fork #137): 0 `equal`, 2 `malformed` (or not
comparable), 3 `not-found`, 4 `differ`, 5 `void`; 1 is a usage error, which
includes a run refused for a set knob (section 8.1). `void`
is never folded into `equal`. Reason ids are stable; each belongs to one
verdict (`SPU_XDIFF_REASONS`).

| Verdict | Reasons |
|---|---|
| `equal` | `none` |
| `differ` | `effect-kind`, `effect-request`, `effect-payload`, `effect-extra`, `effect-missing`, `end-class`, `end-regs`, `end-ls`; contract only: `epoch-order`, `epoch-writes` |
| `void` | `budget`, `depth-guard`, `capture-limit`, `unsupported-effect`, `uncovered-read`, `unlifted-branch`, `host-fault` |
| `malformed` | `fixture-format`, `fixture-identity`, `not-comparable` |
| `not-found` | `no-fixture`, `no-body` |

## 9. Shadow contexts

*Decided.* A shadow context is **built, never copied.** It is
`spu_context_init` (`spu_context.h:539-545`), then the fixture's registers,
local store, `pc`, `job_rt_ls` and `job_rt_ls2`. Set `image_id` to the
registered id for T1 and to `-1` for T0, so the interpreter cannot rejoin a
lifted function (`spu_interp.c:793-798`). Install the replay port. Nothing is
copied from the authority's context, so there is nothing to sanitise: the
wait handles (`spu_context.h:393-394`), `irq_frame` (`:414`), `observer`
(`:458`), `stop_request` (`:474`) and `coh_listed` (`:480`) are zero because
the context is new, and `ls` points at its own store (`:542`).

A whitelist goes stale in the other direction: someone adds a field to
`spu_context` that a job's entry state does set, and a shadow starts without
it. That is enforced, not remembered:

- **Census test** (commit 8): `tools/test_spu_context_census.py` asks the
  compiler for the record layout of `spu_context` and compares the member
  list with a table that gives every member one class: *carried by a
  fixture*, *zero at job entry*, or *host-only*. A new member fails the test
  until it has a class. A compiler that cannot print the layout skips the
  test visibly.
- **Census value**: an integer constant over `sizeof(spu_context)` and the
  member offsets, stored in every fixture (`context_census`). The offline
  runner reports a fixture recorded under another census; it is still
  replayable when `SPU_EFFECT_PORT_ABI` and the carried members are
  unchanged.
- **Driver check**: after a shadow run the driver requires `coh_listed == 0`
  and that no MFC engine was created for the context. Either would mean the
  subject reached the runtime below the seam.

A shadow context is about 270 KiB, mostly its local store. The audit thread
owns one and reuses it.

**A reused audit thread keeps interpreter state across runs.** The
interpreter counts its host nesting depth in a thread-local
(`s_interp_depth`, `spu_interp.c:494`) and raises it around every run
(`:698-700`). Only the outermost run may stop at an observer yield; a nested
one prints a diagnostic and aborts the process (`:805-812`). A serve table
ends a run with `spu_halt`, which `longjmp`s to `spu_run_with_halt`
(`spu_channels.c:209`, `:345-349`) over `spu_interp_run_until` and skips the
decrement at `:700`. After one T0 shadow that ends by serve-halt, the audit
thread's depth stays raised: the next T0 shadow on that thread is not the
outermost run, and its first budget yield aborts the process instead of
answering `void` / `budget`. Commit 8 fixes it at the landing pad:
`spu_run_with_halt` saves the interpreter's depth before `setjmp` and
restores it in the halt cases (an accessor pair exported by `spu_interp.c`).
Ending the interpreter's run through the observer yield instead would let
the channel instruction finish with a value nobody recorded first; the
landing pad does not. Acceptance: a T0 shadow ends by serve-halt, then a
second T0 shadow on the same thread exhausts its budget and gets `void` /
`budget`, not an abort.

## 10. Bounded execution and fault isolation

- **The transcript ends.** An effect past the last record is
  `effect-extra`; the serve table ends the run.
- **Loops without effects need a budget.** *Decided for the mechanism; its
  cost is owed (section 18).*
  - *T0 as subject:* the instruction observer. `on_insn` returning nonzero
    stops the outermost interpreter run with nothing consumed
    (`spu_observe.h:15-26`, `spu_interp.c:803-814`). A step counter there
    costs the production interpreter nothing, because an observer already
    moves a run off its fast path (`spu_interp.c:773-776`). The budget is in
    instructions.
  - *T1 as subject:* a budgeted twin, added in commit 9. The current lifter
    emits plain conditional and direct `goto`s at the back-edge sites
    (`spu_lifter.py:1007`, `:1121`); commit 9 wraps these with
    `SPU_LIFT_BACKEDGE(ctx)`. It expands to nothing in the production source;
    the twin counts down `ctx->budget` at these sites, in `SPU_DIRECT`
    (`spu_context.h:1124-1133`) and at drain hops (`:1084-1086`). The registry
    carries both entries of an image. The budget is in backward transfers.
  - **Depth guard:** `spu_depth_guard` calls `spu_halt` without setting an
    architectural status (`runtime/spu/spu_drain.c:487-523`). The shadow
    driver recognizes this runtime exit and reports `void` / `depth-guard`,
    not an end-state `differ`.
  - **Unwinding** is `spu_halt`, a `longjmp` to `spu_run_with_halt`
    (`spu_channels.c:209`, `:345-349`). Lifted code holds no resources, and
    a serve table takes no locks. The JobQueue2 executor tests run
    lifted-style bodies that end in `spu_halt`
    (`tests/spurs_jq2/test_jq2_executor.cpp:35-39`, `:429-447`) under the
    ThreadSanitizer preset (`tests/spurs_jq2/CMakeLists.txt:128`, `:143`);
    the `asan` preset runs the same tier. Commit 8 restores the interpreter's
    nesting depth at the landing pad after a halt (section 9); commit 9 adds
    the budget unwind.
  - *Over budget on one side only is `void`, never `equal`.* The authority
    has no budget: it is the game's own run.
- **The budgeted twin does not prove the production object.** Offline replay
  runs the production body in a child process under a timeout; a hang or
  signal becomes `void`, and the parent still emits one JSON verdict. During
  live audit, T0 and generated T1 shadows are in process: their only guest
  data is in the validated context and their only effects are served, but a
  runtime or compiler defect remains a process-level risk. Native overrides
  can execute arbitrary host code, so T2 and T3 shadows run out of process
  (the title binary in a worker mode, fixtures over shared memory); a fault
  becomes `void` / `host-fault`.

## 11. Modes and roles

*Decided.* Typed configuration only (`include/ps3emu/config_options.def`);
an enumeration is a `PS3_CFG_STR` with a validator, as `ppu.null_page` is
(`config_options.def:66`, `include/ps3emu/config.h:52-60`). Each key is one
line there and one row in `docs/CONFIG.md` (`:63`, `:185-186`).

| Key | Type | Default | Meaning |
|---|---|---|---|
| `spu.job_body` | `interp` \| `lifted` | `interp` | which tier is the authority: T0, or the image's registered body |
| `spu.xdiff` | `off` \| `sample` \| `all` | `off` | whether the other tier shadows |
| `spu.xdiff_period` | u32, 1.. | 64 | `sample`: every N-th job of an image, counted per image from its first job |
| `spu.xdiff_on_divergence` | `report` \| `stop` | `report` | what a `differ` does to the run |
| `spu.xdiff_budget` | u32, 1.. | 2^28 | the shadow's budget (section 10) |
| `spu.xdiff_dir` | path or empty | empty | where fixtures are written; empty: none |
| `spu.xdiff_capture` | u32 | 0 | also write the first N sampled fixtures of each image, for a corpus |

**Roles are symmetric.** The authority is tapped; the other tier replays.
`job_body=interp, xdiff=all` is the audit mode of #87; `job_body=lifted,
xdiff=sample` is "fast with a sampled check". The same machinery serves
both. A `guarded` mode, which would commit the fast result only when both
agree, is not built: under same-program matching it is observably the audit
mode, and it buys no speed because both sides always run. It returns with a
contract-matched consumer.

**One job, in a run:**

```
worker thread                                   audit thread
-------------                                   ------------
loader stages the job
start hook: fingerprint staged bytes,
  choose the authority, decide the sample
sampled: copy the entry state into a
  bounded capture record; install the tap
authority runs against the real world
end hook: copy registers and local store
credit the job                     -- queue -->  build the shadow context
(the guest sees this result)                     run the subject on the transcript
                                                 compare; count; report
```

- **The worker copies two local stores.** The start hook copies the entry
  local store; the end hook copies registers and the end local store. There
  is no ownership hand-off in the existing `spu_job_hooks` API
  (`spu_workload.h:148-156`), so these copies are deliberate. `[INFERENCE]`
  about 26 µs at 20 GB/s, around 3% of a 0.9 ms job; the tap also copies
  effect payloads. Section 18 measures it.
- **The queue is byte-bounded.** It has at most
  `SPU_XDIFF_LIVE_QUEUE_SLOTS` (8) samples and
  `SPU_XDIFF_LIVE_QUEUE_BYTES` (32 MiB) total; one sample is at most
  `SPU_XDIFF_MAX_LIVE_SAMPLE_BYTES` (4 MiB). A larger recording produces
  `void` / `capture-limit` and is counted, never partially compared.
  - `sample`: the worker never waits. A full queue drops the sample and
    counts it. The audit thread runs below normal priority.
  - `all`: the worker waits for queue room and attempts every job; an
    oversized job still has an explicit `capture-limit` void verdict. The
    audit thread runs at normal priority, since a worker now depends on it.
    With T0 as the shadow this slows the run to the interpreter's speed;
    that is what `sample` is for.
- **A dropped sample stays visible:** per image the run counts jobs, sampled,
  dropped, ineligible, capture-limit and each verdict by reason; every job's
  disposition is a trace record (section 14); the report prints the sampled
  share. Drop-when-full under-samples busy phases, and the share says by how
  much.
- **The authority's result is always the one the guest sees.** A divergence
  never changes the run. It is reported with a named diagnostic, a counter
  and, when `spu.xdiff_dir` is set, a fixture with a `VRDT` section; at most
  16 fixtures per image are written, further divergences are counted.
- **`stop`** ends the run on a `differ` with a new stop kind,
  `spu-xdiff-divergence`, from the audit thread and so after the job was
  credited. Fields: `image reason effect authority subject queue seq
  fixture`. Stops from any thread are serialised (`include/ps3emu/stop.h:31-35`).
  The kind takes a row in both tables of `docs/STOPS.md` (`:45`, `:96`) and
  its id at the end of `KINDS` in `tests/gates/test_stop.py` (`:13-29`). A
  `void` never stops a run; it is counted.
- **Shutdown** drops what is queued and counts it. The audit thread never
  delays an exit.
- **Benchmarks run with `xdiff=off`.** Recording perturbs timing.

## 12. Build-time contract

### 12.1 The SPU lifter ABI and the context layout

*Landed: `1b4eac28`, merged as `fd3e043f`.* The PPU side had this already
(`tools/ppu_lifter.py:91`, `:4508-4509`, `include/ps3emu/lifter_abi.h:34`,
stop `lifter-abi`, `docs/STOPS.md:48`); a lift regenerated against a changed
`spu_context.h` no longer links and runs silently on the SPU side either.

- The lifter writes `#define SPU_LIFTER_ABI 1u` into every generated header
  (`tools/spu_lifter.py:73`), and the generated `spu_recomp_register()` calls
  `ps3_spu_lifter_abi_check(SPU_LIFTER_ABI)` before its first
  `spu_register_function` (`:1186`). The runtime compares the value with
  `SPU_RUNTIME_LIFTER_ABI` (`include/ps3emu/lifter_abi.h:47`) and stops with
  `lifter-abi`, whose fields are `processor expected got`
  (`runtime/diag/lifter_abi.c:26-30`, `docs/STOPS.md:48`, `:99`). The kind is
  shared with the PPU. There is no image field, and no symbol to link
  against: a lift from before the gate simply has no call, so it cannot be
  refused at run time. `tools/check_spu_lifter_abi.py` refuses it before it is
  compiled: `tools/build_spu_workloads.py` runs it before it writes a
  registry, and the template's `SPU_DIR` helper runs it at configure time
  (`cmake/PS3RecompSPU.cmake`).
- **The layout is pinned next to the number.**
  `SPU_RUNTIME_LIFTER_CONTEXT_LAYOUT_SIGNATURE`
  (`include/ps3emu/lifter_abi.h:89`) is an integer constant expression over
  the `offsetof` and `sizeof` of 14 members of `spu_context`: `gpr`, `pc`,
  `status`, `stop_code`, `image_id`, `event_status`, `event_mask`, `srr0`,
  `int_enable`, `host_depth`, `reloc_delta`, `ls`, `policy_mode` and `steps`.
  There is one pin per data model, LP64 and Win64 (`:45-46`), because
  `spu_channel.lock` is a `volatile long` (`spu_context.h:198`) and moves the
  later members where `long` is 32 bits; an unlisted model is a compile
  error. `runtime/spu/spu_workload.c` asserts the pin at build time on every
  host (`:27-35`), and `tests/gates/test_spu_lifter_abi.py` holds the pair
  (ABI, pin) and cross-compiles the Win64 pin.
- **`spu_id` is not in the pin, and xdiff does not need it there.** Its only
  inline reader is the `SPU_PCHIST` diagnostic (`spu_pchist_tick`,
  `spu_context.h:1029-1038`); generated code does not address it. Every job
  context is built with `spu_id` 0 (`spurs_job.c:905-912`), so a shadow built
  by `spu_context_init(ctx, 0)` agrees with its authority and the fixture
  needs no field for it (section 7.4). It would have to join the pin, with an
  ABI bump, if generated or inline code began to read it for behaviour; and
  it would become a member a fixture carries (section 9) if a loader gave
  jobs distinct ids.
- The effect port's pointer, and `lift_miss`, are added behind `observer`
  (`spu_context.h:458`). None of the 14 pinned members lies behind that
  offset, so commit 4 needs no ABI bump, and its acceptance is that the pin
  is unchanged. `ctx->budget` is read by generated code of the budgeted
  twin, and commit 9 therefore adds it to the member list, raises
  `SPU_RUNTIME_LIFTER_ABI` and `SPU_LIFTER_ABI`, **recomputes both data-model
  pins**, updates `EXPECTED_ABI` in `tests/gates/test_spu_lifter_abi.py`, and
  regenerates the checked-in lifts `runtime/spu/tests/out_*` (eleven
  directories whose `spu_recomp.h` stamps `SPU_LIFTER_ABI` and whose
  registration calls the check): with the old stamp each would stop by
  `lifter-abi`.

### 12.2 Raw job images in the pipeline

*Landed: `3d4f0596`, merged as `682d040f`.* `tools/build_spu_workloads.py
--raw-images MANIFEST` takes a JSON list of `{path, sha256, ls_base,
code_end?, extra_entry_points?}` (`tools/spu_program_pipeline.py:71`;
`docs/SPU_LIFTER.md`, "Raw local-store job images"). The pipeline checks the
`sha256` before it lifts anything; registers the image under the FNV-1a-64 of
the whole blob, which is the staged bytes that `spu_workload_fingerprint`
hashes at run time (`:393`; a test compares the C and Python values); runs
the function finder in raw mode at `ls_base` (`:454`); passes `code_end` to
the lifter's `--code-end` (`:477`); and applies the existing lift gates
unchanged. An image that fails a gate is `unsupported` in `inventory.json`,
and the run exits nonzero unless `--allow-partial` is given (`:528`, `:553`).
`tools/gen_test_raw_job.py` builds the synthetic image of the tests. The
`code_end` that `spu_xdiff_body_id` records is therefore known at lift time;
registration does not carry it yet (section 12.3).

### 12.3 Static effect inventory

*Landed in part (`3d4f0596`); the rest is commit 3b.* For every raw image
that lifts, the pipeline adds an `effects` object to `inventory.json`
(`effects_inventory`, `tools/spu_program_pipeline.py:175`; schema in
`docs/SPU_LIFTER.md`, "Static effect inventory"): per channel the counts and
sites of `rdch`, `wrch` and `rchcnt`; per constant MFC command its name and
sites (`name: null` for a word the table does not name); and an `unresolved`
list of the `wrch MFC_Cmd` sites whose command word is not a same-block
immediate. Resolution is block-local on purpose: an unresolved site is
visible, while a false "resolved" could hide an atomic or a mailbox access.
This record replaces the bitmask `spu_effect_inventory` of the draft header,
which duplicated it and had no producer. Same-program replay does not depend
on knowing a command's class, so an unresolved site, a channel number of 32
or more or an unnamed command is reported with the image and refuses
nothing.

What is still missing, and what section 5.5 relies on:

- **Event-branch detection.** The inventory does not see the instructions
  that use the event facility without a channel read: `bisled`, the bi-family
  forms with the E or D bit (`spu_lifter.py:1014-1020`) and `iret`. Only
  `iret` appears in the pipeline, in the control-flow set that clears its
  constant tracker (`spu_program_pipeline.py:162-164`); the others are not
  looked at. The inventory gains an `event_branches` list (the key is
  proposed), one entry per site with its mnemonic, and an image with a
  nonempty list is not eligible for xdiff; it may still run as an authority.
  Commit 3b.
- **A compact inventory in the registration.** Today the report is a file
  for tools, and a registration carries only fingerprint, entry and image
  id. For the offline runner, which can read `inventory.json`, carrying it is
  optional. For in-run xdiff (commit 10) it is required: the runtime can
  refuse an image before its first job only for what its registration told
  it. The same record would give `spu_xdiff_body_id` its `code_end` and
  `lift_options`.

An inventory is an upper bound on a run. It never excuses a check at run
time.

### 12.4 Coverage and promotion

A lifter does not cover every byte of an image, and what a lifted job does
at an unlifted branch depends on where it lands: the resolver interprets
live bytes, or halts the run (`spu_channels.c:3068-3120`, `:3392`), and a
halted JobQueue2 job is the named stop `jq2-job-end` (`executor.cpp:1021-1034`).
Neither is acceptable as a surprise. A context counts its resolver misses
(`lift_miss`, commit 4), and a shadow ends on the first one with `void` /
`unlifted-branch`.

The toolkit keeps no promotion state: registering a body is the port's
statement that it is promoted. The evidence a port should hold before it
sets `spu.job_body=lifted` for an image:

1. sessions with `job_body=interp, xdiff=all` in which every job of the
   image was sampled, with no `differ`, no `void`, no drop;
2. the corpus captured in those sessions replays `equal` offline against the
   production object;
3. afterwards `job_body=lifted, xdiff=sample` stays on in development builds.

## 13. Offline replay, synthetic worlds, corpora

*Decided for the runner's contract; commit 8.*

```
spu_job_replay [--tier interp|registered] [--budget N] [--json] FIXTURE
```

- One fixture, one verdict. Standard output carries one JSON object, schema
  `ps3recomp.spujob-verdict/1`: `schema`, `tool`, `version`, `verdict`,
  `exit_code`, `reason`, `match`, `fixture {path, image_fingerprint,
  authority_tier, effects, toolkit_commit}`, `subject {tier, lifter_abi,
  code_end, lift_options, native_rev}`, `first {effect_index, authority,
  subject, where, want, got}`, `budget {limit, spent}`, `seconds`.
  Diagnostics go to standard error. The exit status is the verdict
  (section 8.3); 1 is a usage error.
- The CLI is a supervisor: it runs the subject in a child process and emits
  the JSON verdict after the child exits. A timeout or signal becomes `void`
  / `host-fault`; it does not kill the corpus driver.
- Lifted bodies are a title's code, so the subject worker is a library entry
  linked with the title's registry. The toolkit's own executable knows T0
  and the synthetic test images.
- A corpus is a directory of fixtures. A corpus driver in `tools/` runs the
  supervisor per fixture and sums verdicts by image and reason.
- **Fuzzing.** A synthetic serve table (a seeded memory image and mailbox
  queues) with a recorder tap lets T0 run as the authority on mutated
  inputs; the fixture it produces drives the other tier. It needs no title
  and no firmware.
- **Performance.** A corpus recorded in play makes tier-against-tier
  benchmarks deterministic and portable across hosts: the same fixtures,
  interpreter against lift against native, on each host and each helper
  path. Host features are chosen at run time by CPU feature, never by a
  global `-march`.
- **Reverse engineering.** A transcript is a job's observable contract.
  `LS_1` lists local-store bytes whose final value differs; `UNIT` names the
  regions they fall in.

Fixtures recorded from a title hold its code and data. They live in the
title's local tree and never enter this repository; toolkit tests assemble
synthetic images (`runtime/spu/tests/gen_test_workload.py:1-33`,
`tools/wrap_spu_elf.py:1-21`).

## 14. Trace records

*Decided; commits 10 and 11.* Two new kinds after the last one in use
(`PS3_FT_AUDIO_RING_VIEW = 31`, `ps3_ftrace.h:167`), in the existing 32-byte
record (`:191`), both never filtered by duration, like kind 29:

| Kind | Emitted by | Span | `who` | `a` | `b` |
|---|---|---|---|---|---|
| 32 `JQ2_BODY` | the worker, once per job that has a `JOB` span, when `spu.job_body` is not `interp` or `spu.xdiff` is not `off` | the end hook's work for the sample; zero length when not sampled | `q << 8 \| w` | `seq` | `tier \| disposition << 8 \| sat(lift_miss, 255) << 16`; disposition 0 not sampled, 1 queued, 2 dropped, 3 not eligible |
| 33 `JQ2_XDIFF` | the audit thread, once per shadow run | the subject's run | `q << 8 \| w` of the job | `seq` | `verdict \| reason << 8 \| subject tier << 16` |

Kind 32 joins kind 29 on `(who, a)`, which gives the image. Kind 33 gives the
shadow's cost for free. `tools/ftrace_report.py` appends both names to its
kind tuple (`:129-133`) and extends the per-image rows (`jq2_image_rows`,
`:1656`) with jobs per tier, sampled, dropped, not eligible, verdicts by
reason, resolver misses and the sampled share; a trace without the kinds
prints the columns as absent, not as zero. Trace bundles are unaffected: the
kinds live in the trace. Fixtures are not bundled; a divergence is named in
the trace by `(who, seq)` and on disk by the file name of section 7.2.

## 15. PPU (context only)

Lifted PPU code addresses guest memory directly, so the channel boundary
does not exist there. The workable order is the reverse: run the
replacement first against a write overlay, so its reads see unmodified
memory and need no log; then the original for real; then compare the overlay
with the original's writes. That still needs concurrent writers excluded for
the duration of the call, so the first PPU consumers are leaf functions
with inputs proven thread-local. Nothing in this document builds it; the
verdict vocabulary and the fixture container are meant to be shared.

## 16. Commit plan

Each commit builds and passes on its own. "Fails without" is the test that
goes red when the commit is reverted. Every commit that adds a test adds its
row to `CMakeLists.txt` with an area label and `t0`; the Python tests run
standalone.

| # | Commit | Files | Test that fails without it | Acceptance |
|---|---|---|---|---|
| 1 | dropped | | | The headers land with their consumers (commits 4, 7 and 8). Real consumers compile them in C and C++, so a compile-check unit would test nothing they do not. The number stays so that the others do not move. |
| 2 | **landed** `1b4eac28`, merged as `fd3e043f`: `spu: gate lifted code with a pinned ABI` | `tools/spu_lifter.py`, `tools/build_spu_workloads.py`, `tools/check_spu_lifter_abi.py`, `include/ps3emu/lifter_abi.h`, `runtime/diag/{lifter_abi.c,stop.cpp}`, `include/ps3emu/stop.h`, `runtime/spu/spu_workload.c`, `cmake/PS3RecompSPU.cmake`, `runtime/spu/tests/out_*`, `docs/STOPS.md`, `docs/SPU_LIFTER.md` | `tests/gates/test_spu_lifter_abi.py`: a matching ABI runs; a mismatched one stops with `lifter-abi` during registration; the (ABI, pin) pair holds; the Win64 pin matches a cross-compiled layout | section 12.1 |
| 3 | **landed** `3d4f0596`, merged as `682d040f`: `tools: register raw SPU job images from a manifest` | `tools/spu_program_pipeline.py`, `tools/build_spu_workloads.py`, `tools/find_spu_functions.py`, `tools/gen_test_raw_job.py`, `tools/test_spu_program_pipeline.py`, `docs/SPU_LIFTER.md` | `tools/test_spu_program_pipeline.py`: a synthetic raw image registers under its whole-blob fingerprint with its channel and MFC inventory; a wrong `sha256` and a `code_end` past the blob are named errors; `code_end` reaches the lifter | sections 12.2 and 12.3 |
| 3b | `tools: spu_program_pipeline reports event branches` | `tools/spu_program_pipeline.py`, `tools/gen_test_raw_job.py` (variants), `tools/test_spu_program_pipeline.py`, `docs/SPU_LIFTER.md`; before commit 10 also `tools/build_spu_workloads.py` (registry writer) and `runtime/spu/spu_workload.{c,h}` (a compact inventory in the registration, with the lift's `code_end` and options hash) | a synthetic raw image with `bisled`, one with a bi-family E or D branch and one with `iret` each list the site under `event_branches`; an image with none lists none | the tool test; the raw-image tests of commit 3 unchanged; with the registration part, a registered image reports its flags to the runtime |
| 4 | `spu: effect port seam in the channel runtime` | `runtime/spu/spu_effect_port.h` (new: the draft without its `spu_effect_inventory` block), `runtime/spu/spu_context.h` (`port`, `lift_miss`), `spu_channels.c` (seam; `MFC_WrTagMask` below the port test, today's inline store kept for `port == NULL`; `spu_port_*` and `spu_port_serve_halt`; closed resolver path; the latch static assertions), `spu_dma.h`, `spu_interp.c`, `runtime/spu/tests/test_spu_effect_port.c` (new), `tests/bench/spu/` (driver) | `spu_effect_port`: a tap sees every effect of a synthetic job in order, list elements, an atomic and `MFC_WrTagMask` included; a served context completes with `vm_base == NULL` and no MFC engine created; a served context that branches to unlifted local store ends its run with `lift_miss == 1` and nothing interpreted; a serve callback that cannot answer ends the run under `spu_run_with_halt`, and with no landing pad armed the process aborts instead of continuing with a value (a child-process test) | the test, interpreted and lifted; all `spu` and `spurs` suites; the bench rows of section 18 within noise of the baseline; `tests/gates/test_spu_lifter_abi.py` green with the pin unchanged; with no port `MFC_WrTagMask` does not take the MFC registry lock |
| 5 | `jq2: classify a job's end without stopping` | `libs/spurs/jq2/executor.cpp`, `runtime/spu/spurs_job.c` (shared classifier), `tests/spurs_jq2/test_jq2_executor.cpp` | each end (return in both tiers, stop, halt, jm call) yields its class; the existing `jq2-job-end` and `jq2-jm-call` stop tests stay green | `ctest -R jq2_exec`; `--preset tsan -R jq2_exec` |
| 6 | `jq2: select a job body by its staged bytes` | `libs/spurs/jq2/executor.{cpp,hpp}`, `include/ps3emu/config_options.def`, `include/ps3emu/config.h`, `runtime/config/`, `docs/CONFIG.md`, `docs/JQ2.md` | a registered image runs lifted under `spu.job_body=lifted`; other bytes staged from the same EA and size run interpreted; `interp` never runs a registered body | the executor tests; kind-29 cost reported from a synthetic trace; `gcc-16`; tsan |
| 7 | `spu: record a job's transcript and write its fixture` | `runtime/spu/spu_xdiff.h` (new: the container, section, identity, unit, effect and end layouts of 7.1 to 7.6, and `spu_job_tier`), `runtime/spu/xdiff/` (new: fixture reader and writer, recorder tap), `spu_channels.c` (a query for the latched float mode), tests | a synthetic job with GET, PUT, a list, a stalled list resumed by `MFC_WrListStallAck`, GETLLAR/PUTLLC and a mailbox write records the expected transcript, the remaining list bytes at the ack included; the fixture reads back the expected record and payload bytes; each rejection of section 7.8 is reported as `fixture-format` (a `VRDT` section is bounds-checked only until commit 8) | the test, interpreted and lifted; `asan` preset over the reader with truncated and corrupt fixtures, among them one with a flipped header byte (the checksum covers the whole header); `gcc-16`; `SPU_XDIFF_LS_BYTES` equals `SPU_LS_SIZE` by static assertion |
| 8 | `spu: replay port, shadow contexts and spu_job_replay` | `runtime/spu/spu_xdiff.h` (verdict, reason and match enums, `VRDT`), `runtime/spu/xdiff/` (replay serve table, shadow context, runner), `spu_channels.c` and `spu_interp.c` (`spu_run_with_halt` saves the interpreter's nesting depth before `setjmp` and restores it in the halt cases), `tools/test_spu_context_census.py` (new), `tools/spu_xdiff_corpus.py` (new), tests | T0 against the lift on the fixture of commit 7: `equal`; a lift with one changed instruction: `differ` naming the first effect; a lift that writes another tag mask before a wait and then overwrites the register: `differ` / `effect-request`; a lift that rewrites the remaining elements of a stalled list before the ack and later overwrites them: `differ` / `effect-request`; a stop inside a nested lifted call: `differ` (section 5.1); T0 over its budget by the instruction observer: `void`; a T0 shadow that ends by serve-halt, then a second T0 shadow on the same thread that exhausts its budget: `void` / `budget`, not an abort (section 9); a fixture whose `float_rz` is not the runner's mode: `malformed` / `fixture-identity`; a behaviour-changing knob set: exit status 1; a new `spu_context` member without a class: the census test fails | runner exit codes 0, 1, 2, 3, 4, 5; `gcc-16`; `asan` |
| 9 | `spu_lifter: count backward transfers under SPU_LIFT_BUDGET` | `tools/spu_lifter.py`, `tools/build_spu_workloads.py` (twin entry), `runtime/spu/spu_context.h`, `spu_drain.c`, and the ABI bump of section 12.1: `budget` joins the member list of `include/ps3emu/lifter_abi.h`, `SPU_RUNTIME_LIFTER_ABI` and `SPU_LIFTER_ABI` go up, both data-model pins are recomputed, `EXPECTED_ABI` in `tests/gates/test_spu_lifter_abi.py` follows, and `runtime/spu/tests/out_*` are regenerated | a lifted loop without effects: `void` / `budget` and the process goes on; the same source without the definition contains no check (object comparison) | `asan` and `tsan` presets with `--repeat until-fail:10`; SPU bench unchanged for the production object; `tests/gates/test_spu_lifter_abi.py` and the SPU runtime tests over the regenerated lifts |
| 10 | `jq2: in-run xdiff with sampled shadows` | `libs/spurs/jq2/` (audit thread, queue), `runtime/spu/spurs_job.c` (staging store hand-over), `include/ps3emu/stop.h`, `docs/STOPS.md`, `tests/gates/test_stop.py`, `include/ps3emu/ps3_ftrace.h`, config keys, `docs/CONFIG.md` | a seeded divergence is reported, its fixture written and replayable, the guest-visible result unchanged; `stop` exits by `spu-xdiff-divergence`; a full queue drops and counts in `sample` and blocks in `all` (ordered by hooks, never by sleeps) | `tsan` preset `-R jq2_xdiff --repeat until-fail:10`; the stop gate; `gcc-16` |
| 11 | `ftrace_report: per-image tier and xdiff counters` | `tools/ftrace_report.py`, `tools/test_ftrace_report.py`, `docs/TOOLS.md` | a synthetic trace with kinds 32 and 33 yields the per-image counts and the sampled share; a trace without them reports the columns absent | the tool test; `ftrace_bundle.py verify` on a bundle with the new kinds |

The order carries two constraints. Event-branch detection (3b) comes before
anything that shadows a job: the offline runner reads its report, and the
in-run mode (10) needs it in the registration. And the lifted budget (9)
comes before any shadow runs inside a game (10): until then a lifted subject
runs only under the offline runner, in its own process, where a loop without
effects is a timeout and not a hung title.

After commit 10, in the title's own repository: register the first image, run
an `xdiff=all` session, replay the corpus offline, then compare power and
frame time with `xdiff=off`.

## 17. The open questions

| # | Question | Answer | State |
|---|---|---|---|
| 1 | Fixture encoding | one self-contained file; header, section table, typed sections; extents for local store; payloads deduplicated inside a fixture; no compression in the format, zstd accepted around it (section 7) | closed |
| 2 | Contract matching for T2 and T3 | epoch rule and falsifiers in section 8.2 | proposed, and not a promotion gate until a synthetic peer-reservation test captures notification recipients and event order, then a T3 corpus replay confirms the relaxations |
| 3 | Equivalence of T0 and T1 | every register, all of local store, the end class; the argument is in section 8.1 | closed |
| 4 | Sanitising a shadow context | nothing is copied, so nothing is sanitised; the census test and the driver check enforce it (section 9) | closed |
| 5 | Budget for lifted code | a back-edge macro that exists only in a second compilation; unwinding by the existing halt; evidence for `longjmp` under the sanitizers in section 10 | closed for soundness; the twin's cost is owed |
| 6 | Audit thread | one thread; queue of 8 slots/32 MiB total, maximum 4 MiB per sample; `sample` drops and counts on queue pressure, `all` blocks for queue room; oversized samples are explicit `capture-limit` voids; drops and voids appear in counters, trace records and the report (section 11) | closed |
| 7 | Fingerprint check | on the staged bytes, through the existing content-hash cache; a sampled job verifies the hit byte-serially, an unsampled one trusts it, an accepted and stated risk (section 4) | closed; its cost is read from existing traces |
| 8 | Cost of the real path | one load and branch per operation, none per latch write; `wrch`/`rdch` already read `observer`, `rchcnt` does not; line-placement and branch costs await measurement (section 5.6) | open until the bench rows are measured |
| 9 | Where xdiff records go | two trace kinds, 32 and 33; fixtures beside the trace, never in a bundle (section 14) | closed |
| 10 | What T2 needs from the lifter | per function: the registers that may be read after each exit before they are written (live-out), over all callers, with an unknown caller meaning all; the extent of the stack below the entry pointer that is dead at exit; and a function as a fixture unit | open. Closes with that lifter pass and the poison run confirming its masks on a corpus |

## 18. Measurements owed

No performance claim in this document is measured. Each one, and what
settles it:

| Claim | Measurement | Where |
|---|---|---|
| the real path is unchanged by the seam | the channel, `dma` and `atomic` rows of the SPU operation benchmark, lifted and interpreted, before and after commit 4; fastest of 25 runs each, as the bench already takes (`tests/bench/spu/README.md:21-39`) against `results/2026-10-06-m1pro-baseline.csv` | commit 4. The driver script the README names is not in the tree; the commit restores one |
| identifying the staged image is cheap | the kind-29 spans of an existing play trace, per image: mean and p95 against the job's own span | `ftrace_report.py`, before commit 6 |
| recording costs a few percent of a job | job spans with `xdiff=off` against `xdiff=all`, same scene, same build; and kind 32 spans; the byte-serial check of a sampled job's image is part of this cost | after commit 10 |
| a fixture is about 100 KiB | sizes of a captured corpus, raw and zstd | after commit 10 |
| the budgeted twin is close to the production object | the SPU bench over both objects | commit 9 |
| T0 under an instruction observer is fast enough to shadow | interpreter steps per second with and without the counting observer | commit 8 |

## 19. References

- Fork issues: #87 (lifted JobQueue2 dispatch and audit), #95 (differential
  execution contract), #42 (HLE against lifted firmware), #53, #106, #137.
- Toolkit: `docs/JQ2.md` ("Job observer"), `docs/SPU_LIFTER.md`,
  `docs/SPU_INTERPRETER_LIFTED_HANDOFF.md`, `docs/SPU_CONTEXT_REGISTRIES.md`,
  `docs/DIFF_HARNESS.md`, `docs/PS3DIFF.md`, `docs/STOPS.md`,
  `docs/CONFIG.md`, `docs/TOOLS.md` ("ftrace_report.py", "Compressed
  traces").
- Existing differentials: `runtime/spu/tests/spu_interp_diff_test.c` (per
  instruction: interpreter, lift, RPCS3), `tests/diff_harness/`.
- Evidence order for a behaviour: firmware RE, console probe, RPCS3 at the
  pinned oracle, inference. Everything this document says about the SPU
  runtime rests on the toolkit's source at the cited lines; the hardware's
  ordering guarantees in section 8.2 rest on the architecture documents and
  are inference until a console probe confirms them.
