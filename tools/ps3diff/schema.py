"""The ps3diff trace schema and the checks every command shares.

`ps3diff.event/2` is the successor of `rpcs3-gdb-trace.event/1`: every field
of the older record keeps its name, type and meaning, and the new envelope
fields name the execution context a record belongs to. Two consecutive
`seq` values may come from different contexts; `ctx_seq` is the per-context
order. docs/PS3DIFF.md is the prose contract, this module is its executable
form -- keep the two in step.

The checks are deliberately strict. A record this module accepts is one the
converter can reproduce byte for byte, and a record it rejects is named by
file and line instead of being guessed at downstream.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
import sys
from pathlib import Path

SCHEMA_V1 = "rpcs3-gdb-trace.event/1"
SCHEMA_V2 = "ps3diff.event/2"
REPORT_SCHEMA = "ps3diff.report/1"

#: The tool that writes `rpcs3-gdb-trace.event/1`; recorded as `source.producer`.
V1_PRODUCER = "rpcs3_gdb_trace"

V1_EVENTS = ("call", "ret")
V2_EVENTS = ("call", "ret", "spu-job-start", "spu-job-end", "dma", "stop")

HEX_ADDRESS = re.compile(r"^0x[0-9a-f]{8}$")
HEX_REGISTER = re.compile(r"^0x[0-9a-f]{1,16}$")
HEX_SHA256 = re.compile(r"^[0-9a-f]{64}$")
HEX_ANY = re.compile(r"^0x[0-9a-fA-F]+$")

# Per-kind required and optional payload fields, not counting the envelope.
_FIELDS = {
    SCHEMA_V1: {
        "call": (frozenset("label pc lr args dumps".split()),
                 frozenset({"intervened"})),
        "ret": (frozenset("label call_seq pc ret latency_ms dumps".split()),
                frozenset()),
    },
    SCHEMA_V2: {
        "call": (frozenset("label pc lr args dumps".split()),
                 frozenset({"intervened", "image", "inputs", "outputs",
                            "state_hash", "source"})),
        "ret": (frozenset("label call_seq pc ret latency_ms dumps".split()),
                frozenset({"image", "inputs", "outputs", "state_hash", "source"})),
        "spu-job-start": (frozenset("label pc image descriptor dumps".split()),
                          frozenset({"inputs", "state_hash", "source"})),
        "spu-job-end": (frozenset("label call_seq outputs dumps".split()),
                        frozenset({"image", "state_hash", "source"})),
        "dma": (frozenset("dir lsa ea len tag".split()),
                frozenset({"image", "source"})),
        "stop": (frozenset("label pc".split()),
                 frozenset({"detail", "image", "source"})),
    },
}

_COMMON = {
    SCHEMA_V1: frozenset("schema seq t thread event".split()),
    SCHEMA_V2: frozenset("schema seq t thread event ctx ctx_seq".split()),
}


class Ps3DiffError(Exception):
    """An operator-facing failure. Exit status 2 unless a command overrides it."""

    exit_code = 2

    def __init__(self, message: str, hint: str = ""):
        super().__init__(message)
        self.message = message
        self.hint = hint


class RecordProblem(Exception):
    """One record violates the schema. `where` is a JSON path inside the record."""

    def __init__(self, where: str, message: str):
        super().__init__(f"{where}: {message}")
        self.where = where
        self.message = message


# --------------------------------------------------------------------------- text


def canonical_json(value: object) -> str:
    """The exact serialisation rpcs3_gdb_trace writes, without the newline."""
    return json.dumps(value, ensure_ascii=True, separators=(",", ":"),
                      allow_nan=False)


def _show(value: object) -> str:
    try:
        text = canonical_json(value)
    except (TypeError, ValueError):
        text = repr(value)
    return text if len(text) <= 80 else text[:77] + "..."


def _line_error(path: str, lineno: int, problem: RecordProblem) -> Ps3DiffError:
    return Ps3DiffError(f"{path}: line {lineno}: {problem.where}: {problem.message}")


# --------------------------------------------------------------------------- input


def _no_duplicates(pairs):
    out = {}
    for key, value in pairs:
        if key in out:
            raise ValueError(f"duplicate key {key!r}")
        out[key] = value
    return out


def _reject_constant(text: str):
    raise ValueError(f"non-finite number {text}")


def read_ndjson(path: str) -> list[tuple[int, str, dict]]:
    """Read NDJSON as `(line number, raw line without newline, record)`.

    The file must end in a newline: rpcs3_gdb_trace always terminates a
    record, so a missing one is a truncated write, not a short record.
    """
    path = str(path)
    if path == "-":
        data = sys.stdin.buffer.read()
    else:
        try:
            data = Path(path).read_bytes()
        except OSError as exc:
            raise Ps3DiffError(f"{path}: cannot read: {exc.strerror}") from None
    if not data:
        return []
    if not data.endswith(b"\n"):
        lineno = data.count(b"\n") + 1
        raise Ps3DiffError(
            f"{path}: line {lineno}: missing trailing newline",
            "rpcs3_gdb_trace terminates every record with a newline; a partial "
            "last line is a truncated write, not a record")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        lineno = data[:exc.start].count(b"\n") + 1
        raise Ps3DiffError(f"{path}: line {lineno}: not UTF-8: {exc.reason}") from None

    entries = []
    for lineno, raw in enumerate(text.split("\n")[:-1], 1):
        if not raw:
            raise Ps3DiffError(f"{path}: line {lineno}: empty line")
        try:
            record = json.loads(raw, object_pairs_hook=_no_duplicates,
                                parse_constant=_reject_constant)
        except json.JSONDecodeError as exc:
            raise Ps3DiffError(f"{path}: line {lineno}: invalid JSON: {exc.msg}") from None
        except ValueError as exc:
            raise Ps3DiffError(f"{path}: line {lineno}: {exc}") from None
        if not isinstance(record, dict):
            raise Ps3DiffError(
                f"{path}: line {lineno}: a record is a JSON object, found "
                f"{type(record).__name__}")
        entries.append((lineno, raw, record))
    return entries


def read_json_object(path: str, what: str) -> dict:
    """A JSON object from a config file (mask, context map). Not schema-checked here."""
    path = str(path)
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError as exc:
        raise Ps3DiffError(f"{path}: cannot read {what}: {exc.strerror}") from None
    except UnicodeDecodeError as exc:
        raise Ps3DiffError(f"{path}: {what} is not UTF-8: {exc.reason}") from None
    try:
        value = json.loads(text, object_pairs_hook=_no_duplicates,
                           parse_constant=_reject_constant)
    except json.JSONDecodeError as exc:
        raise Ps3DiffError(f"{path}: {what} is not JSON: {exc.msg}") from None
    except ValueError as exc:
        raise Ps3DiffError(f"{path}: {what}: {exc}") from None
    if not isinstance(value, dict):
        raise Ps3DiffError(f"{path}: {what} must be a JSON object")
    return value


def require_canonical(path: str, lineno: int, raw: str, record: dict) -> None:
    """Refuse a line the converter could not reproduce byte for byte."""
    expected = canonical_json(record)
    if raw == expected:
        return
    offset = next((i for i, (a, b) in enumerate(zip(raw, expected)) if a != b),
                  min(len(raw), len(expected)))
    raise Ps3DiffError(
        f"{path}: line {lineno}: not canonical JSON: first difference at column "
        f"{offset + 1}",
        f"expected {expected[offset:offset + 60]!r}; ps3diff converts only lines "
        f"it can reproduce, so the round trip stays byte-identical")


# --------------------------------------------------------------------------- contexts


def ctx_key(ctx: dict) -> str:
    """`ppu:<thread id>` or `spu:<index>` -- the identity used for alignment."""
    return f"{ctx['kind']}:{ctx['id']}"


def parse_ctx_key(text: str) -> dict:
    """The inverse of ctx_key, for ctx-map files. Raises Ps3DiffError."""
    kind, sep, ident = text.partition(":")
    if not sep or not ident:
        raise Ps3DiffError(f"context key {text!r} is not <ppu|spu>:<id>")
    if kind == "ppu":
        return {"kind": "ppu", "id": ident}
    if kind == "spu":
        if not ident.isdigit() or str(int(ident)) != ident:
            raise Ps3DiffError(
                f"context key {text!r}: an spu id is a decimal index without "
                f"leading zeros")
        return {"kind": "spu", "id": int(ident)}
    raise Ps3DiffError(f"context key {text!r}: unknown kind {kind!r}")


# --------------------------------------------------------------------------- checks


def _is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value: object) -> bool:
    return _is_int(value) or (isinstance(value, float) and math.isfinite(value))


def _need_int(rec: dict, name: str, prefix: str, minimum: int) -> None:
    where = f"{prefix}.{name}" if prefix else name
    value = rec[name]
    if not _is_int(value) or value < minimum:
        raise RecordProblem(where, f"must be an integer >= {minimum}, found {_show(value)}")


def _need_number(rec: dict, name: str, prefix: str, minimum: float) -> None:
    where = f"{prefix}.{name}" if prefix else name
    value = rec[name]
    if not _is_number(value) or value < minimum:
        raise RecordProblem(where, f"must be a finite number >= {minimum}, found {_show(value)}")


def _need_str(rec: dict, name: str, prefix: str, *, nonempty: bool = True) -> None:
    where = f"{prefix}.{name}" if prefix else name
    value = rec[name]
    if not isinstance(value, str) or (nonempty and not value):
        kind = "non-empty string" if nonempty else "string"
        raise RecordProblem(where, f"must be a {kind}, found {_show(value)}")


def _need_hex(rec: dict, name: str, prefix: str, pattern: re.Pattern,
              shape: str) -> None:
    where = f"{prefix}.{name}" if prefix else name
    value = rec[name]
    if not isinstance(value, str) or not pattern.match(value):
        raise RecordProblem(where, f"must be {shape}, found {_show(value)}")


def _check_hex_map(rec: dict, name: str) -> None:
    value = rec[name]
    if not isinstance(value, dict):
        raise RecordProblem(name, f"must be an object, found {type(value).__name__}")
    for key, item in value.items():
        if not key:
            raise RecordProblem(name, "contains an empty key")
        if not isinstance(item, str) or not HEX_REGISTER.match(item):
            raise RecordProblem(f"{name}.{key}",
                                f"must be 0x + 1..16 lowercase hex digits, found "
                                f"{_show(item)}")


def _check_scalar_map(rec: dict, name: str) -> None:
    value = rec[name]
    if not isinstance(value, dict):
        raise RecordProblem(name, f"must be an object, found {type(value).__name__}")
    for key, item in value.items():
        if not key:
            raise RecordProblem(name, "contains an empty key")
        if isinstance(item, float) and not math.isfinite(item):
            raise RecordProblem(f"{name}.{key}", "must be finite")
        if not isinstance(item, (str, int, float, bool, type(None))):
            raise RecordProblem(f"{name}.{key}",
                                f"must be a scalar, found {type(item).__name__}")


def _record_identity(rec: dict) -> str:
    """`seq 3 (ppu:0000000001000000)`, for errors raised inside a record."""
    key = ctx_key(rec["ctx"]) if "ctx" in rec else f"ppu:{rec['thread']}"
    return f"seq {rec['seq']} ({key})"


def _check_dumps(rec: dict) -> None:
    dumps = rec["dumps"]
    if not isinstance(dumps, list):
        raise RecordProblem("dumps", f"must be an array, found {type(dumps).__name__}")
    seen = set()
    for index, dump in enumerate(dumps):
        where = f"dumps[{index}]"
        if not isinstance(dump, dict):
            raise RecordProblem(where, "must be an object")
        name = dump.get("name")
        if not isinstance(name, str) or not name:
            raise RecordProblem(f"{where}.name", f"must be a non-empty string, found {_show(name)}")
        if name in seen:
            raise RecordProblem(f"{where}.name", f"duplicate dump name {name!r}")
        seen.add(name)
        keys = set(dump)
        if "error" in dump:
            if keys == {"name", "error"}:
                pass
            elif keys == {"name", "ea", "len", "error"}:
                _need_hex(dump, "ea", where, HEX_ADDRESS, "0x + 8 lowercase hex digits")
                _need_int(dump, "len", where, 0)
            else:
                extra = sorted(keys - {"name", "ea", "len", "error"})
                raise RecordProblem(where, f"unknown field {extra[0]!r}")
            _need_str(dump, "error", where)
            continue
        required = {"name", "ea", "len", "read", "sha256"}
        missing = sorted(required - keys)
        if missing:
            raise RecordProblem(f"{where}.{missing[0]}", "required field is missing")
        unknown = sorted(keys - required - {"hex"})
        if unknown:
            raise RecordProblem(where, f"unknown field {unknown[0]!r}")
        _need_hex(dump, "ea", where, HEX_ADDRESS, "0x + 8 lowercase hex digits")
        _need_int(dump, "len", where, 1)
        _need_int(dump, "read", where, 0)
        if dump["read"] > dump["len"]:
            raise RecordProblem(f"{where}.read", f"{dump['read']} exceeds len {dump['len']}")
        _need_hex(dump, "sha256", where, HEX_SHA256, "64 lowercase hex digits")
        if "hex" in dump:
            text = dump["hex"]
            subject = f"dump {name!r} in record {_record_identity(rec)}"
            if not isinstance(text, str) or not re.fullmatch(r"[0-9a-f]*", text):
                raise RecordProblem(f"{where}.hex",
                                    f"{subject}: must be lowercase hex digits")
            if len(text) % 2:
                raise RecordProblem(f"{where}.hex",
                                    f"{subject}: holds an odd number of digits "
                                    f"({len(text)}), not whole bytes")
            if len(text) != 2 * dump["read"]:
                raise RecordProblem(f"{where}.hex",
                                    f"{subject}: holds {len(text) // 2} bytes, "
                                    f"read says {dump['read']}")
            digest = hashlib.sha256(bytes.fromhex(text)).hexdigest()
            if digest != dump["sha256"]:
                raise RecordProblem(f"{where}.hex",
                                    f"{subject}: its bytes hash to {digest}, "
                                    f"not the dump's {dump['sha256']}")


def _check_ctx(rec: dict) -> None:
    ctx = rec["ctx"]
    if not isinstance(ctx, dict):
        raise RecordProblem("ctx", f"must be an object, found {type(ctx).__name__}")
    if set(ctx) != {"kind", "id"}:
        extra = sorted(set(ctx) - {"kind", "id"})
        missing = sorted({"kind", "id"} - set(ctx))
        raise RecordProblem("ctx", f"missing {missing[0]!r}" if missing
                            else f"unknown field {extra[0]!r}")
    kind = ctx["kind"]
    if kind == "ppu":
        if not isinstance(ctx["id"], str) or not ctx["id"]:
            raise RecordProblem("ctx.id", "a ppu context id is the thread id string")
        if ctx["id"] != rec["thread"]:
            raise RecordProblem(
                "ctx.id", f"{ctx['id']!r} does not match thread {rec['thread']!r}; "
                          f"event/1 contexts are PPU threads")
    elif kind == "spu":
        if not _is_int(ctx["id"]) or ctx["id"] < 0:
            raise RecordProblem("ctx.id", "an spu context id is a non-negative index")
    else:
        raise RecordProblem("ctx.kind", f"must be 'ppu' or 'spu', found {_show(kind)}")


def _check_state_hash(rec: dict) -> None:
    value = rec["state_hash"]
    if not isinstance(value, dict) or set(value) != {"algo", "scope", "value"}:
        raise RecordProblem("state_hash",
                            'must be exactly {"algo", "scope", "value"}')
    if value["algo"] != "sha256":
        raise RecordProblem("state_hash.algo",
                            f"unsupported algorithm {_show(value['algo'])}; only sha256")
    if not isinstance(value["scope"], str) or not value["scope"]:
        raise RecordProblem("state_hash.scope", "must be a non-empty string")
    if not isinstance(value["value"], str) or not HEX_SHA256.match(value["value"]):
        raise RecordProblem("state_hash.value", "must be 64 lowercase hex digits")


def _check_source(rec: dict) -> None:
    value = rec["source"]
    if not isinstance(value, dict) or set(value) != {"producer", "schema"}:
        raise RecordProblem("source", 'must be exactly {"producer", "schema"}')
    for field in ("producer", "schema"):
        if not isinstance(value[field], str) or not value[field]:
            raise RecordProblem(f"source.{field}", "must be a non-empty string")


def _check_descriptor(rec: dict) -> None:
    value = rec["descriptor"]
    if not isinstance(value, dict) or set(value) != {"ea", "size", "sha256"}:
        raise RecordProblem("descriptor",
                            'must be exactly {"ea", "size", "sha256"}')
    _need_hex(value, "ea", "descriptor", HEX_ADDRESS, "0x + 8 lowercase hex digits")
    _need_int(value, "size", "descriptor", 0)
    _need_hex(value, "sha256", "descriptor", HEX_SHA256, "64 lowercase hex digits")


def _check_record(rec: dict, schema: str) -> None:
    if rec.get("schema") != schema:
        raise RecordProblem("schema", f"expected {schema!r}, found {_show(rec.get('schema'))}")
    event = rec.get("event")
    spec = _FIELDS[schema].get(event)
    if spec is None:
        raise RecordProblem(
            "event", f"unknown event {_show(event)}; {schema} defines "
                     f"{', '.join(_FIELDS[schema])}")
    required, optional = spec
    missing = sorted(field for field in required if field not in rec)
    if missing:
        raise RecordProblem(missing[0], "required field is missing")
    unknown = sorted(
        key for key in rec
        if key not in _COMMON[schema] and key not in required and key not in optional
        and not (schema == SCHEMA_V2 and key.startswith("x-")))
    if unknown:
        raise RecordProblem(unknown[0], "unknown field")

    _need_int(rec, "seq", "", 0)
    _need_number(rec, "t", "", 0)
    _need_str(rec, "thread", "")
    if schema == SCHEMA_V2:
        _need_int(rec, "ctx_seq", "", 0)
        _check_ctx(rec)

    if event == "call":
        _need_str(rec, "label", "")
        _need_hex(rec, "pc", "", HEX_ADDRESS, "0x + 8 lowercase hex digits")
        _need_hex(rec, "lr", "", HEX_ADDRESS, "0x + 8 lowercase hex digits")
        _check_hex_map(rec, "args")
        if "intervened" in rec:
            _check_hex_map(rec, "intervened")
        _check_dumps(rec)
    elif event == "ret":
        _need_str(rec, "label", "")
        _need_hex(rec, "pc", "", HEX_ADDRESS, "0x + 8 lowercase hex digits")
        _need_hex(rec, "ret", "", HEX_ADDRESS, "0x + 8 lowercase hex digits")
        _need_int(rec, "call_seq", "", 0)
        _need_number(rec, "latency_ms", "", 0)
        _check_dumps(rec)
    elif event == "spu-job-start":
        _need_str(rec, "label", "")
        _need_hex(rec, "pc", "", HEX_ADDRESS, "0x + 8 lowercase hex digits")
        _need_str(rec, "image", "")
        _check_descriptor(rec)
        _check_dumps(rec)
    elif event == "spu-job-end":
        _need_str(rec, "label", "")
        _need_int(rec, "call_seq", "", 0)
        _check_scalar_map(rec, "outputs")
        _check_dumps(rec)
    elif event == "dma":
        if rec["dir"] not in ("get", "put"):
            raise RecordProblem("dir", f"must be 'get' or 'put', found {_show(rec['dir'])}")
        _need_hex(rec, "lsa", "", HEX_ADDRESS, "0x + 8 lowercase hex digits")
        _need_hex(rec, "ea", "", HEX_ADDRESS, "0x + 8 lowercase hex digits")
        _need_int(rec, "len", "", 0)
        _need_int(rec, "tag", "", 0)
    elif event == "stop":
        _need_str(rec, "label", "")
        _need_hex(rec, "pc", "", HEX_ADDRESS, "0x + 8 lowercase hex digits")
        if "detail" in rec:
            _need_str(rec, "detail", "", nonempty=False)

    if schema == SCHEMA_V2:
        if "image" in rec:
            _need_str(rec, "image", "")
        if "inputs" in rec:
            _check_scalar_map(rec, "inputs")
        if "outputs" in rec:
            _check_scalar_map(rec, "outputs")
        if "state_hash" in rec:
            _check_state_hash(rec)
        if "source" in rec:
            _check_source(rec)


def v1_field_names(event: str) -> frozenset[str]:
    """Every top-level field `rpcs3-gdb-trace.event/1` defines for `event`."""
    required, optional = _FIELDS[SCHEMA_V1][event]
    return _COMMON[SCHEMA_V1] | required | optional


def _check_call_ref(rec: dict, earlier: dict[int, str], where: str) -> None:
    want = "call" if rec["event"] == "ret" else "spu-job-start"
    ref = rec["call_seq"]
    target = earlier.get(ref)
    if target is None:
        raise RecordProblem(
            "call_seq", f"{ref} is not an earlier record in {where}")
    if target != want:
        raise RecordProblem("call_seq", f"{ref} is a {target}, expected a {want}")


# --------------------------------------------------------------------------- streams


def check_stream_v1(path: str, entries: list[tuple[int, str, dict]]) -> None:
    path = str(path)
    last_seq = -1
    earlier: dict[str, dict[int, str]] = {}
    for lineno, _, rec in entries:
        try:
            _check_record(rec, SCHEMA_V1)
        except RecordProblem as problem:
            raise _line_error(path, lineno, problem) from None
        seq = rec["seq"]
        if seq <= last_seq:
            raise _line_error(path, lineno, RecordProblem(
                "seq", f"{seq} does not increase (previous {last_seq})"))
        last_seq = seq
        thread = rec["thread"]
        seen = earlier.setdefault(thread, {})
        if rec["event"] == "ret":
            try:
                _check_call_ref(rec, seen, f"thread {thread}")
            except RecordProblem as problem:
                raise _line_error(path, lineno, problem) from None
        seen[seq] = rec["event"]


def check_stream_v2(path: str, entries: list[tuple[int, str, dict]]) -> None:
    path = str(path)
    last_seq = -1
    next_ctx: dict[str, int] = {}
    earlier: dict[str, dict[int, str]] = {}
    for lineno, _, rec in entries:
        try:
            _check_record(rec, SCHEMA_V2)
        except RecordProblem as problem:
            raise _line_error(path, lineno, problem) from None
        key = ctx_key(rec["ctx"])
        seq = rec["seq"]
        if seq <= last_seq:
            raise _line_error(path, lineno, RecordProblem(
                "seq", f"{seq} does not increase (previous {last_seq})"))
        last_seq = seq
        expected = next_ctx.get(key, 0)
        if rec["ctx_seq"] != expected:
            raise _line_error(path, lineno, RecordProblem(
                "ctx_seq", f"{rec['ctx_seq']} breaks {key} ordering (expected {expected})"))
        next_ctx[key] = expected + 1
        seen = earlier.setdefault(key, {})
        if rec["event"] in ("ret", "spu-job-end"):
            try:
                _check_call_ref(rec, seen, key)
            except RecordProblem as problem:
                raise _line_error(path, lineno, problem) from None
        seen[seq] = rec["event"]


def validate_entries(path: str, entries: list[tuple[int, str, dict]]) -> str | None:
    """Schema-check a whole file. Returns the schema, or None for an empty file."""
    if not entries:
        return None
    schema = entries[0][2].get("schema")
    if schema not in (SCHEMA_V1, SCHEMA_V2):
        raise Ps3DiffError(
            f"{path}: line {entries[0][0]}: schema: unknown schema {_show(schema)}",
            f"expected {SCHEMA_V1!r} or {SCHEMA_V2!r}")
    for lineno, _, rec in entries:
        found = rec.get("schema")
        if found != schema:
            raise Ps3DiffError(
                f"{path}: line {lineno}: schema: {_show(found)} mixes with "
                f"{schema!r} from line 1",
                "one file holds records of one schema; convert first")
    if schema == SCHEMA_V1:
        check_stream_v1(path, entries)
    else:
        check_stream_v2(path, entries)
    return schema


def context_keys(schema: str | None, records: list[dict]) -> list[str]:
    """Context keys in file order, for the validate summary."""
    if schema == SCHEMA_V1:
        seen: list[str] = []
        for rec in records:
            key = f"ppu:{rec['thread']}"
            if key not in seen:
                seen.append(key)
        return seen
    if schema == SCHEMA_V2:
        seen = []
        for rec in records:
            key = ctx_key(rec["ctx"])
            if key not in seen:
                seen.append(key)
        return seen
    return []
