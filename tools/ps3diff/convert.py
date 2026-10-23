"""Lossless conversion between rpcs3-gdb-trace.event/1 and ps3diff.event/2.

The v1 -> v2 direction adds the envelope (`ctx`, `ctx_seq`, `source`) and
nothing else; the v2 -> v1 direction removes exactly those and refuses a
record whose payload has no event/1 meaning. Because conversion never
re-serialises a record into a different key order, a canonical event/1 file
survives v1 -> v2 -> v1 byte for byte.

A line that is not canonical JSON, a mixed-schema file, and a v2 payload
field without a v1 meaning are all errors: the tool would otherwise silently
normalise data that the next comparison depends on.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

from .schema import (
    SCHEMA_V1,
    SCHEMA_V2,
    V1_EVENTS,
    V1_PRODUCER,
    Ps3DiffError,
    RecordProblem,
    canonical_json,
    check_stream_v2,
    ctx_key,
    read_ndjson,
    require_canonical,
    validate_entries,
    v1_field_names,
)

#: Envelope fields v2 adds to a converted event/1 record. `--to 1` removes them.
V2_ENVELOPE = ("ctx", "ctx_seq", "source")


def project_v1_record(rec: dict, ctx_seq: int) -> dict:
    """One event/1 record as event/2, in the input's key order plus the envelope."""
    out = dict(rec)
    out["schema"] = SCHEMA_V2
    out["ctx"] = {"kind": "ppu", "id": rec["thread"]}
    out["ctx_seq"] = ctx_seq
    out["source"] = {"producer": V1_PRODUCER, "schema": SCHEMA_V1}
    return out


def project_v1_stream(entries: list[tuple[int, str, dict]]) -> list[dict]:
    """Project event/1 records, numbering ctx_seq per PPU thread in file order."""
    counters: dict[str, int] = {}
    projected = []
    for _, _, rec in entries:
        key = ctx_key({"kind": "ppu", "id": rec["thread"]})
        index = counters.get(key, 0)
        counters[key] = index + 1
        projected.append(project_v1_record(rec, index))
    return projected


def to_v1(rec: dict, path: str, lineno: int) -> dict:
    """One event/2 record as event/1, or a refusal naming the first field in the way."""
    event = rec["event"]
    if event not in V1_EVENTS:
        raise Ps3DiffError(
            f"{path}: line {lineno}: {event}: no {SCHEMA_V1} record exists for this event",
            "new event kinds stay in event/2; they describe observations event/1 "
            "cannot represent")
    ctx = rec["ctx"]
    if ctx["kind"] != "ppu" or ctx["id"] != rec["thread"]:
        raise Ps3DiffError(
            f"{path}: line {lineno}: ctx: {ctx_key(ctx)} has no {SCHEMA_V1} meaning",
            "event/1 derives its context from `thread` and knows only PPU threads")
    allowed = v1_field_names(event)
    extras = [key for key in rec
              if key not in V2_ENVELOPE and key not in allowed]
    if extras:
        raise Ps3DiffError(
            f"{path}: line {lineno}: {extras[0]}: has no {SCHEMA_V1} meaning",
            "dropping it would lose the observation; keep this file as event/2")
    source = rec.get("source")
    if source is not None and source["schema"] != SCHEMA_V1:
        raise Ps3DiffError(
            f"{path}: line {lineno}: source.schema: {source['schema']!r} is not {SCHEMA_V1!r}",
            "refusing to strip provenance that does not describe an event/1 record")
    return {key: value for key, value in rec.items()
            if key not in V2_ENVELOPE} | {"schema": SCHEMA_V1}


def _write_ndjson(path: str, records: list[dict]) -> None:
    text = "".join(canonical_json(rec) + "\n" for rec in records)
    if path == "-":
        sys.stdout.write(text)
        sys.stdout.flush()
        return
    target = Path(path)
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise Ps3DiffError(f"{path}: cannot create directory: {exc.strerror}") from None
    tmp = target.with_name(target.name + ".partial")
    try:
        with open(tmp, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, target)
    except OSError as exc:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise Ps3DiffError(f"{path}: cannot write: {exc.strerror}") from None


def convert_file(src: str, dst: str, target: int) -> tuple[int, int]:
    """Convert one NDJSON file. Returns (records read, records written)."""
    if target not in (1, 2):
        raise Ps3DiffError(f"--to must be 1 or 2, not {target}")
    entries = read_ndjson(src)
    schema = validate_entries(src, entries)
    for lineno, raw, rec in entries:
        require_canonical(src, lineno, raw, rec)

    if target == 2:
        if schema == SCHEMA_V1:
            records = project_v1_stream(entries)
            try:
                check_stream_v2(src, [(lineno, "", rec)
                                      for (lineno, _, _), rec in zip(entries, records)])
            except RecordProblem as problem:
                raise Ps3DiffError(
                    f"{src}: the converted stream is invalid: {problem}") from None
        else:
            records = [rec for _, _, rec in entries]
    else:
        if schema == SCHEMA_V2:
            records = [to_v1(rec, src, lineno) for lineno, _, rec in entries]
        else:
            records = [rec for _, _, rec in entries]

    _write_ndjson(dst, records)
    return len(entries), len(records)
