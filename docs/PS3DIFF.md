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

- `hex` is optional and, when present, holds exactly `read` bytes as lowercase
  hex.
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
