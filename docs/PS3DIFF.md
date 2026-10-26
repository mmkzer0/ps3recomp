# ps3diff: trace schema, conversion and differential comparison

`ps3diff` is the trace contract and comparison tool for M1 slice A4: a
versioned schema, deterministic order per execution context, a
first-divergence report, and a converter from the existing tracer format
rather than a second incompatible format.

It has three jobs:

1. **Define** `ps3diff.event/2`, the successor of
   `rpcs3-gdb-trace.event/1`. Every event/1 field keeps its name, type and
   meaning; event/2 adds the execution context and the observations a
   job-replay slice needs.
2. **Convert** between the two schemas losslessly: a canonical event/1 file
   survives `v1 -> v2 -> v1` byte for byte, or the converter refuses with the
   line number.
3. **Compare** two runs per execution context and report the first divergence
   of each context and the globally first one, with counts.

Non-goals: the runtime-side producer is B6a; `tools/rpcs3_gdb_trace.py` keeps
writing event/1 until it moves to the toolkit in D5; `tools/compare/` is an
unrelated function-matching schema.

---

## Commands

```
python3 tools/ps3diff.py convert --to 2 trace.ndjson -o trace.v2.ndjson
python3 tools/ps3diff.py convert --to 1 trace.v2.ndjson -o trace.v1.ndjson
python3 tools/ps3diff.py validate trace.v2.ndjson
python3 tools/ps3diff.py diff ref.v2.ndjson new.v2.ndjson [--mask MASK.json] [--ctx-map MAP.json] [--json]
```

`--help` on the tool and on every subcommand lists the same examples.
Conversion writes through `<output>.partial` and renames on success, so a
failed conversion never leaves a half-written trace. `-o -` (the default)
writes to stdout.

Exit status:

| Status | Meaning |
|---|---|
| 0 | conversion/validation succeeded, or `diff` found no divergence with at least one record compared |
| 1 | `diff` found a divergence (including a context present on one side only, or a side ending early) |
| 2 | usage, data or schema error |
| 3 | `diff` compared zero records; a comparison that proves nothing is never a pass |

Status 3 takes precedence over 1 when no record could be aligned: two traces
with disjoint contexts report their one-sided contexts, but the result is
"nothing compared", not "divergent" or "identical".

---

## `ps3diff.event/2`

One JSON object per line (NDJSON). Lines are UTF-8 and newline-terminated.

### Envelope

| Field | Type | Required | Meaning |
|---|---|---|---|
| `schema` | string | yes | exactly `"ps3diff.event/2"` |
| `seq` | integer >= 0 | yes | producer sequence number, strictly increasing in file order |
| `ctx_seq` | integer >= 0 | yes | 0-based order of the record **within its context**, assigned by file order |
| `t` | number >= 0 | yes | producer clock in seconds (event/1's field, unchanged) |
| `thread` | non-empty string | yes | event/1's thread id. For a `ppu` context it equals `ctx.id`; for any other context it is the producer's thread label |
| `event` | string | yes | one of `call`, `ret`, `spu-job-start`, `spu-job-end`, `dma`, `stop` |
| `ctx` | object | yes | `{"kind": "ppu"\|"spu", "id": ...}`; a `ppu` id is the thread id string (equal to `thread`), an `spu` id is the SPU index (integer >= 0) |
| `image` | non-empty string | no | SPU image id or fingerprint, or the PPU executable's fingerprint |
| `inputs` | object | no | name -> scalar map of named inputs beyond `args`/`dumps` |
| `outputs` | object | no | name -> scalar map of named outputs beyond `ret`/`dumps` |
| `state_hash` | object | no | `{"algo": "sha256", "scope": <non-empty string>, "value": <64 lowercase hex>}` |
| `source` | object | no | `{"producer": <tool>, "schema": <original schema>}`, so provenance survives conversion |

Unknown top-level fields are an error. The single exception is the extension
namespace: a top-level key starting with `x-` is allowed and preserved
verbatim. Nested objects (`ctx`, `state_hash`, `source`, `descriptor`,
`dumps[]`) are strict.

`ctx_seq` is the alignment key. `seq` is global order and is never compared.
A file may interleave contexts freely; the per-context `ctx_seq` sequence
must still be 0, 1, 2, ... in file order.

### Event kinds

`call` and `ret` are event/1's records with the envelope added.

| Kind | Required fields | Notes |
|---|---|---|
| `call` | `label`, `pc`, `lr`, `args`, `dumps` | optional `intervened` (event/1's field) |
| `ret` | `label`, `call_seq`, `pc`, `ret`, `latency_ms`, `dumps` | `call_seq` names an earlier `call` in the same context |
| `spu-job-start` | `label`, `pc`, `image`, `descriptor`, `dumps` | optional `inputs`; the descriptor was fetched at admission, the dumps hold the DMA-list inputs |
| `spu-job-end` | `label`, `call_seq`, `outputs`, `dumps` | `call_seq` names the earlier `spu-job-start` in the same context; `outputs` and dumps hold the post-Sync observable outputs |
| `dma` | `dir` (`"get"`/`"put"`), `lsa`, `ea`, `len`, `tag` | one observed SPU DMA transfer; optional `image` |
| `stop` | `label`, `pc` | optional `detail`; a named stop observed during the run |

Field types:

- `pc`, `lr`, `ret`, `lsa`, `ea`: `0x` followed by exactly 8 lowercase hex
  digits (`0x00401000`). `ret` is a 32-bit return value formatted the same way,
  matching the event/1 producer.
- `args`, `intervened`: object of register name -> `0x` followed by 1..16
  lowercase hex digits. event/1's producer does not zero-pad register values.
- `latency_ms`, `t`, `len`, `tag`, `call_seq`, `seq`, `ctx_seq`: non-negative
  numbers/integers; `t` and `latency_ms` may be fractional.
- `descriptor`: exactly `{"ea": <0x address>, "size": <integer >= 0>,
  "sha256": <64 lowercase hex>}`.
- `inputs`, `outputs`: exactly `{"<name>": <string|number|boolean|null>}`.

### Dumps

A dump entry is one of the three shapes the event/1 producer emits, unchanged:

```json
{"name": "in", "ea": "0x00100000", "len": 16, "read": 16, "sha256": "...", "hex": "..."}
{"name": "bad_len", "ea": "0x00100010", "len": 0, "error": "length 0 outside 1..4096"}
{"name": "unreadable_ptr", "error": "pointer at 0x00100020 unreadable"}
```

- `hex` is optional and, when present, must be lowercase hex of even length
  that decodes to exactly `read` bytes, and the SHA-256 of those bytes must
  equal the dump's `sha256`.
- `read <= len`; a successful dump has `len >= 1`.
- Dump names are unique within a record.
- An `error` dump is either `{name, error}` or `{name, ea, len, error}`;
  nothing else.

---

## Conversion

`convert --to 2` reads event/1 (or event/2) and writes event/2. For event/1:

- `schema` becomes `ps3diff.event/2`;
- `ctx` becomes `{"kind": "ppu", "id": <thread>}`;
- `ctx_seq` is assigned per thread in file order;
- `source` becomes `{"producer": "rpcs3_gdb_trace", "schema":
  "rpcs3-gdb-trace.event/1"}`.

The added fields are appended after the original fields. Nothing else is
touched: key order, number formatting and string escapes are preserved.

`convert --to 1` removes exactly `ctx`, `ctx_seq` and `source` and restores
the event/1 schema. It refuses, with the file and line, when the record has no
event/1 meaning:

- an event kind other than `call`/`ret`;
- a context that is not a PPU thread, or whose id differs from `thread`;
- any payload field event/1 does not define (`image`, `inputs`, `outputs`,
  `state_hash`, `descriptor`, `dir`, `lsa`, `ea`, `len`, `tag`, `detail`);
- a `source.schema` that is not `rpcs3-gdb-trace.event/1`.

### Canonical JSON

The converter only accepts and only writes the serialisation
`rpcs3_gdb_trace.py` produces:

- no whitespace: `separators=(",", ":")`;
- non-ASCII escaped (`ensure_ascii=True`);
- duplicate object keys and non-finite numbers are rejected.

A line that parses but does not reproduce byte for byte is refused with its
line number and the first differing column. This is deliberate: silently
normalising an input would make "byte-identical round trip" meaningless and
would hide a second producer whose format drifted.

Because conversion preserves key order, the round trip is exact:

```
ps3diff convert --to 2 trace.ndjson -o trace.v2.ndjson
ps3diff convert --to 1 trace.v2.ndjson -o trace.back.ndjson
cmp trace.ndjson trace.back.ndjson
```

The included fixture `tools/ps3diff/fixtures/v1-basic.ndjson` exercises an
intervened call, both error-dump shapes, a hex dump and a non-ASCII label; its
converted form is pinned in `v2-basic.ndjson`.

---

## Validation

`validate FILE` accepts either schema and checks:

1. the record schema (types, required fields, unknown fields, the `x-`
   namespace);
2. `seq` strictly increases across the file;
3. `ctx_seq` is 0, 1, 2, ... per context in file order
   (presence, contiguity and no gaps);
4. `ret.call_seq` names an earlier `call` in the same context, and
   `spu-job-end.call_seq` an earlier `spu-job-start`;
5. dump shapes and hashes as above.

For event/1 the equivalent thread-scoped rules apply (`seq`, and
`ret.call_seq` naming an earlier call in the same thread).

Validation does **not** require canonical formatting; only `convert` does,
because only conversion promises byte identity.

---

## Diff

`diff A B` validates both sides, groups records by context, and pairs them by
`ctx_seq`. Global interleaving is irrelevant. Without `--ctx-map`, a context
is identified by `<kind>:<id>` (`ppu:0000000001000000`, `spu:0`). event/1
input is projected to event/2 in memory with the converter's rule, so a raw
trace can be compared without a prior conversion.

### Compared fields

| Kind | Compared |
|---|---|
| `call` | `event`, `label`, `args.<name>`, `intervened.<name>`, `dumps` |
| `ret` | `event`, `label`, `ret`, `dumps` |
| `spu-job-start` | `event`, `label`, `image`, `descriptor.{ea,size,sha256}`, `inputs.<name>`, `dumps` |
| `spu-job-end` | `event`, `label`, `outputs.<name>`, `dumps` |
| `dma` | `event`, `dir`, `lsa`, `ea`, `len`, `tag` |
| `stop` | `event`, `label`, `detail` |

`image`, `inputs.<name>`, `outputs.<name>` and `state_hash.{algo,scope,value}`
are compared on every kind that carries them, in addition to its row above.

Dumps compare by name, then by:

- `dumps.<name>.kind` (`read` or `error`),
- `dumps.<name>.sha256` and `dumps.<name>.len` for a read,
- `dumps.<name>.error` for an error.

Never compared: `schema`, `seq`, `t`, `thread`, `ctx`, `ctx_seq`,
`call_seq`, `latency_ms`, `source`, `dumps[].ea`, `dumps[].read`,
`dumps[].hex`, and the address fields `pc`/`lr`. The first group is timing or
sequence identity; the second moves when a build or a boot shifts addresses
and needs the mask instead of a fixed value.

A field present on one side only is a divergence with the other side reported
as absent. A context present on one side only is a divergence. If one side
ends before the other inside a context, the divergence is the first record
without a partner (`path: "record"`).

### Mask file

```json
{
  "ignore": ["args.r4", "dumps.scratch"],
  "normalize": {"dma.ea": "addr", "args.r5": "hex"}
}
```

- Paths are dotted segments. `*` matches exactly one segment, so
  `dumps.*.sha256` matches every dump's digest. A path also matches every
  field below it: `args` ignores all arguments, `dumps.scratch` all leaves
  of that dump.
- `ignore` drops the matched leaves from both sides; a presence difference
  disappears with them.
- `normalize` transforms both values before comparing. `hex` renders a JSON
  integer or a `0x` string as `0x` + minimal lowercase hex; `addr` does the
  same and clears the low 12 bits, so a relocation across a page boundary
  does not diverge. A value that is not an integer or `0x` hex string is an
  error.
- A path may not be in both lists. A path that matches no compared field is
  an error: a mask that matches nothing hides a typo.

### Context map

```json
{"ppu:0000000001000000": "ppu:0000000002000000", "spu:0": "spu:1"}
```

Keys are A contexts, values are B contexts. The map is one-to-one, every
source must exist in A and every target in B, or the command exits 2. A
context that is not mapped keeps its own key.

### JSON report

`--json` prints one object:

```json
{
  "schema": "ps3diff.report/1",
  "a": "ref.ndjson",
  "b": "new.ndjson",
  "identical": false,
  "counts": {"compared": 7, "contexts_a": 2, "contexts_b": 2,
             "contexts_matched": 2},
  "first": {"ctx": "ppu:0000000001000000", "ctx_seq": 1,
            "label": "synthetic_call_a", "path": "ret",
            "a": "0x00000001", "b": "0x00000002"},
  "contexts": [
    {"ctx": "ppu:0000000001000000", "compared": 5,
     "first": {"ctx": "ppu:0000000001000000", "ctx_seq": 1,
               "label": "synthetic_call_a", "path": "ret",
               "a": "0x00000001", "b": "0x00000002"},
     "status": "matched"},
    {"ctx": "ppu:000000000100000b", "compared": 2, "first": null,
     "status": "matched"}
  ]
}
```

- `identical` is true only when at least one record was compared and nothing
  diverged.
- `first` is the globally first divergence: the divergence whose A record has
  the lowest `seq`; when A is the short side, the B record's `seq` orders it.
  It is `null` when nothing diverged.
- `contexts` is sorted by context key. `status` is `matched`, `a-only` or
  `b-only`. A one-sided context reports `ctx_seq: 0`, `path: "ctx"` and the
  presence values.
- `a`/`b` values are rendered strings: JSON strings verbatim, numbers and
  booleans in JSON form. `null` means the field or record is absent on that
  side; the string `"null"` is a real JSON null value in `inputs`.

The text report on stdout carries the same information; diagnostics go to
stderr.

---

## Tests

`tools/test_ps3diff.py` (CTest `ps3diff`, labels `tools;t0`) is the gate. It
uses only synthetic fixtures: a v1 file with an intervened call, error dumps,
a hex dump and a non-ASCII label; a pinned v2 conversion; a v2 file with all
new kinds; a non-canonical file; mask, normaliser and context-map files.
Divergence cases (changed ret, masked field, missing tail, interleaving,
zero overlap) are derived from those fixtures in the test.

The local T1 check additionally round-trips every
`odin-sphere-m0/fw/trace/*.ndjson` (3,490 call + 3,485 ret records) and
compares the result byte for byte; those traces never enter this repository.

---

## `ps3diff.rsx/1`: an RSX capture

A sibling schema in the same package for slice H1b (M3): the command stream the RSX
consumed and every guest-memory input it consumed with it, recorded at the moment of
consumption. The envelope rules are event/2's (strict fields, strictly increasing `seq`,
contiguous `ctx_seq`, canonical JSON, `ctx` `{"kind":"ppu","id":"rsx"}`), with one
difference: `t` is the constant `0.0`, because a wall clock would make two captures of the
same inputs differ. Producer: `libs/video/rsx_capture.cpp`, enabled by `video.rsx_capture`
(`docs/CONFIG.md`). Validator and comparison: `tools/ps3diff/rsx.py`.

A capture is a directory: `capture.ndjson` (written through `capture.ndjson.partial` and
renamed when it closes; no tool accepts the `.partial`) and `blobs/sha256/<2 hex>/<sha256>`
(content-addressed, written once). Only a **clean-reset** capture exists: it starts at
`cellGcmSetupContext`, and a FIFO step before any `gcm-init` is the stop `rsx-capture`
(`reason=midstream`); `bootstrap.mode` is `"clean-reset"` and nothing else validates.

### Events

| Event | Input? | Fields beyond the envelope |
|---|---|---|
| `capture-start` | yes | `bootstrap`, `producer` |
| `environment` | no | `machine`: `gpu macos metal build glslang spirv_cross toolkit_sha title_sha` |
| `gcm-init` | yes | `cmd_size io_size io_ea` |
| `gcm-state` | yes | `op`: `io-map`/`io-unmap` (`ea io size`), `display-buffer` (`id offset pitch width height`) |
| `fifo-method` | yes | `get put call_ret subchannel type count method bound binds reads writes` |
| `fifo-branch` | yes | `get put call_ret kind` (`jump`/`call`/`ret`) `target reads writes` |
| `fifo-word` | yes | `get put call_ret reads writes`: a flip marker or a word the walker skips |
| `fifo-nop` | yes | `get put call_ret words reads writes`: a run of zero words |
| `draw` | no | `outcome reason fixed_function state_hash` |
| `frame-complete` | no | `frame capture_hash state_hash frame_hash readback draws unknown_methods resyncs valid invalid` |
| `capture-end` | no | `frames events capture_hash` |

A FIFO event is one step of the walker, in the order the walker took it, never the order of
the ring: `get` is the IO offset the step starts at, `put` the pass's PUT, `call_ret` the
one-deep CALL return slot before the step. The validator walks the steps and refuses a step
that is not where the previous one left the walker, a CALL while one is outstanding, a RET
with none, and a branch target or `get` with no IO mapping. A semaphore acquire that is still
waiting is not an event: the walker reads it again on the next pass.

A blob reference is `{ea, location, size, sha256, consume_seq}` plus optional `offset` (the
RSX offset) and `kind`. `consume_seq` is one global ordinal over reads and writes, strictly
increasing across events. `reads` are what the step consumed (the packet words as `fifo`
reads, a label, vertex, index, texture and fragment-program spans, 2D transfer sources);
`writes` are what the RSX wrote (a label release, a report). Two reads of one event that
disagree on the same bytes are the stop `rsx-capture` (`reason=read-conflict`): a replay
writes every blob of an event before it runs the event. A missing blob, a blob whose bytes do
not hash to its name and a blob of the wrong size are each named by `validate`.

### Three hashes

| Hash | Scope | Meaning |
|---|---|---|
| `capture_hash` | `input-events/v1` | SHA-256 of the canonical lines of the input events so far, newline included: it binds every blob hash. The `environment` event is not in it. |
| `state_hash` | `frame-state/v1` (a draw: `draw-state/v1`) | The register file, transform program, constants, subchannel bindings and fragment-program digest at each draw, chained, then the register file at the flip. |
| `frame_hash` | `raw-readback/v1` | SHA-256 of the unmodified readback bytes. |

The readback descriptor is `{format, channel_order, row_pitch, alpha, colorspace, clamp,
width, height}`: `format` is `rgba8-unorm`, `rgba16-float` or `rgba32-float`, `alpha` is
`kept`, `clamp` is `none`, `colorspace` is the pixel format's transfer function. The bytes are
read only after the command buffer completed. A presented surface in any other format is the
stop `rsx-capture` (`reason=readback-format`), never a skipped frame.

### Frames

A frame is a flip head, identified by `{flip_id, guest_frame, display_buffer}` (plus
`present_seq`). `flip_id` is the zero-based ordinal of `0xE924` heads with bit 31, as C3 counts
`GcmFlipHead`; `guest_frame` is the zero-based ordinal of the `0xE944` queue words
(`GcmQueueFlip`) the head presents, `null` when it had none. A head is presented about three
times; the frame is the first. `draws` obeys `submitted = executed + sum(dropped)` and
`valid` is true exactly when `invalid` is empty: any dropped draw (`dropped-draws`), a draw on
the built-in pipeline (`fixed-function`), a walker recovery that dropped commands
(`fifo-resync`), a head with no queued flip (`no-queued-flip`) or a missing readback makes the
frame invalid. A frame that hides one of these is refused by `validate`.

`unknown_methods` counts the methods the frame consumed that no object class handles and no
allow-list entry covers (`libs/video/rsx_method_class.h`). The capture writes the count and
the reason `unknown-methods`; `validate` and `rsx-compare` refuse such a frame outright, so a
capture with one is an error, not an `invalid` baseline to skip past.

### Commands

```
python3 tools/ps3diff.py validate captures/h1/capture.ndjson
python3 tools/ps3diff.py rsx-compare a/capture.ndjson b/capture.ndjson [--flip N] [--json]
```

`rsx-compare` pairs frames by `flip_id` and compares identity, then `capture_hash`,
`state_hash`, the readback descriptor and `frame_hash`, in that order, so a divergence is
looked for in the inputs, then the state, then the pixels. Exit status: 0 equal on at least one
frame; 1 a frame differs or is invalid; 2 data or schema error; 3 zero frames compared (an
error); 4 the environment records differ: **not comparable**, neither a pass nor a failure.
Byte equality holds only on equal GPU, macOS, Metal, build and translator versions.

### Replay: `rsx_replay`

```
rsx_replay <capture-dir> [--frames a-b] [--out report.json] [--manifest DIR] [--accept-unknown-methods]
```

macOS only (CMake target `rsx_replay`, built with the Metal backend and the shader translator). It
replays the capture through the same FIFO walker, draw engine and headless Metal backend the title
ran, with guest shaders on; a build or run that would fall back to the fixed-function pipeline is
refused by name (`fixed-function`). The replay is itself captured by the same writer: its
**manifest** (`DIR`, default `<report>.manifest`, else a temporary directory) is a `ps3diff.rsx/1`
capture, so `ps3diff rsx-compare live/capture.ndjson DIR/capture.ndjson` works on it, and a replay
that reproduced the inputs has the live capture's input hash frame for frame.

`--frames a-b` (or `a`) names the flips to compare, by `flip_id`. A replay always starts at the
clean reset and runs through the last frame named, because the register file, the surfaces and
the texture cache at flip `a` are what the earlier flips made.

Per recorded step the runner writes the step's blobs to their guest addresses, runs the walker over
exactly that step with the PUT the live pass saw, and checks every recorded `writes` entry (label
RELEASE, report, NV0039 destination) against the guest memory the replay produced. The
walker is not seeked: each step must start where the previous one left it (GET and the CALL return
slot). The one recorded value the replay does not recompute is a GET_REPORT's timer: it is the host
clock on the live side, so the replay feeds the recorded one (`cellGcm_rsx_replay_step`). Nothing
else reads a clock; the replay is one thread, as the title's walker, engine and backend are, and the
readback follows the completed command buffer. The label window's guest address (`ppu_hle_inject_base`)
is not in the capture; it is read from the first label blob's `ea - offset`.

**Named errors** (exit 2; `error.name` in the report, also on stderr as `rsx_replay: error <name>: ...`):

| Name | Meaning |
|---|---|
| `missing-blob` / `altered-blob` | a blob the manifest names is not in `blobs/sha256`, or its bytes are not its name or its declared size |
| `capture-hash` | a frame's recorded input hash is not the hash of the input events before it: the manifest was edited |
| `unknown-method` | a frame up to the last compared one consumed methods no object class handles (see below) |
| `step-discontinuity` | a step does not start where the walker is (or with the same CALL return slot) |
| `step-landing` | the walker did not leave a step where the recording ends it |
| `write-mismatch` | a recorded write was not reproduced (value, or the number of report writes) |
| `frame-identity` | the replay's frames are not the capture's `{flip_id, guest_frame, display_buffer}`; `present_seq` counts the engine's own presents, to which the title's frame clock adds some, so it is reported and not compared |
| `fixed-function` | guest shaders are off in the backend |
| `unfinished-capture`, `truncated-capture`, `no-capture`, `capture-format`, `hle-base`, `bootstrap`, `blob-range`, `gcm-init`, `gcm-state`, `config`, `backend`, `manifest` | the capture or the host cannot be replayed |

Exit status: **0** every compared frame is equal (capture, state and frame hash, readback
descriptor, draw counts) and valid; **1** a compared frame differs or is invalid (`first` names
the flip and the field); **2** a named error; **3** zero frames compared (`zero-frames`);
**4** *not comparable*: the live environment record and this machine's differ (`environment.differs`
lists the fields; nothing is replayed); **5** diagnostic only, see below; **70** a stop raised by
the walker, the engine or the writer.

`--accept-unknown-methods` is a diagnostic for a capture that `validate` refuses because of
`unknown-methods` (until each such method is classified). The replay runs, but the report says
`gate_valid: false` in its header and in every frame, and the exit status for an otherwise equal
replay is 5, never 0. It proves that the replay of those frames is deterministic; it is not a
baseline.

The report is `ps3diff.rsx.replay/1` JSON: `status`, `exit`, `capture_hash`, `gate_valid`,
`environment` (live, replay, differs), `error`, `first`, and per frame the identity,
`present_seq` (live and replay), `result` and the hashes of both sides. The environment's
`title_sha` is carried over from the capture (it names who produced the inputs, not what renders them).
