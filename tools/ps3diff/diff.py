"""Per-context record alignment and the first-divergence report.

Records are aligned by `ctx_seq` inside a context, never by global order: two
runs of the same workload interleave PPU threads and SPU jobs differently on
every boot, and that difference is not a divergence. Timing (`t`,
`latency_ms`) and sequence identity (`seq`, `call_seq`) are never compared;
guest addresses that move between runs are either excluded (`pc`, `lr`) or
normalisable through the mask.

event/1 input is projected to event/2 with the same rule the converter uses,
so `ps3diff diff trace.ndjson trace.ndjson` works on the producer's output
without a prior conversion.
"""

from __future__ import annotations

from dataclasses import dataclass

from .convert import project_v1_stream
from .schema import (
    HEX_ANY,
    REPORT_SCHEMA,
    SCHEMA_V1,
    Ps3DiffError,
    canonical_json,
    check_stream_v2,
    ctx_key,
    parse_ctx_key,
    read_json_object,
    read_ndjson,
    validate_entries,
)

MISSING = object()

#: Compared categories in report order; anything else sorts after them.
_CATEGORY = {name: rank for rank, name in enumerate((
    "event", "label", "ret", "args", "intervened", "dumps", "inputs",
    "outputs", "state_hash", "image", "descriptor", "dir", "lsa", "ea",
    "len", "tag", "detail"))}


@dataclass(frozen=True)
class Divergence:
    ctx: str
    ctx_seq: int
    label: str
    path: str
    a: object
    b: object
    order: int


@dataclass(frozen=True)
class Side:
    path: str
    records: tuple[dict, ...]
    contexts: dict[str, tuple[dict, ...]]


@dataclass(frozen=True)
class Mask:
    source: str = ""
    ignore: tuple[tuple[str, ...], ...] = ()
    normalize: tuple[tuple[tuple[str, ...], str], ...] = ()


@dataclass(frozen=True)
class CtxMap:
    source: str
    forward: dict[str, str]


EMPTY_MASK = Mask()


# --------------------------------------------------------------------------- loading


def load_side(path: str) -> Side:
    entries = read_ndjson(path)
    schema = validate_entries(path, entries)
    if schema == SCHEMA_V1:
        records = project_v1_stream(entries)
        check_stream_v2(path, [
            (lineno, "", rec)
            for (lineno, _, _), rec in zip(entries, records)
        ])
    else:
        records = [rec for _, _, rec in entries]
    contexts: dict[str, list[dict]] = {}
    for rec in records:
        contexts.setdefault(ctx_key(rec["ctx"]), []).append(rec)
    return Side(str(path), tuple(records),
                {key: tuple(value) for key, value in contexts.items()})


def _compile_path(text: object, source: str, what: str) -> tuple[str, ...]:
    if not isinstance(text, str) or not text:
        raise Ps3DiffError(f"{source}: {what} path must be a non-empty string, "
                           f"found {text!r}")
    parts = tuple(text.split("."))
    if any(not part for part in parts):
        raise Ps3DiffError(f"{source}: {what} path {text!r} has an empty segment")
    return parts


def load_mask(path: str) -> Mask:
    data = read_json_object(path, "mask")
    unknown = sorted(set(data) - {"ignore", "normalize"})
    if unknown:
        raise Ps3DiffError(f"{path}: mask: unknown key {unknown[0]!r}",
                           'a mask holds "ignore" and optional "normalize"')
    raw_ignore = data.get("ignore", [])
    if not isinstance(raw_ignore, list):
        raise Ps3DiffError(f"{path}: mask.ignore: must be an array")
    raw_normalize = data.get("normalize", {})
    if not isinstance(raw_normalize, dict):
        raise Ps3DiffError(f"{path}: mask.normalize: must be an object")

    ignore = tuple(_compile_path(item, str(path), "ignore") for item in raw_ignore)
    normalize = []
    for item, name in raw_normalize.items():
        if name not in ("hex", "addr"):
            raise Ps3DiffError(
                f"{path}: mask.normalize.{item}: unknown normaliser {name!r}",
                "defined normalisers: hex (canonical 0x form), addr (page-aligned)")
        normalize.append((_compile_path(item, str(path), "normalize"), name))
    for pattern in ignore:
        if any(pattern == other for other, _ in normalize):
            text = ".".join(pattern)
            raise Ps3DiffError(
                f"{path}: mask: '{text}' is both ignored and normalised",
                "one path has one meaning; drop it from one of the two lists")
    return Mask(str(path), ignore, tuple(normalize))


def load_ctx_map(path: str) -> CtxMap:
    data = read_json_object(path, "context map")
    forward = {}
    for source_key, target_key in data.items():
        parse_ctx_key(source_key)
        parse_ctx_key(target_key)
        forward[source_key] = target_key
    return CtxMap(str(path), forward)


# --------------------------------------------------------------------------- leaves


def _label_of(rec: dict) -> str:
    return rec.get("label", rec["event"])


def record_leaves(rec: dict) -> dict[str, object]:
    """The fields compared for `rec`, keyed by a dotted path."""
    leaves: dict[str, object] = {"event": rec["event"]}
    if "label" in rec:
        leaves["label"] = rec["label"]
    if "ret" in rec:
        leaves["ret"] = rec["ret"]
    for name, value in rec.get("args", {}).items():
        leaves[f"args.{name}"] = value
    for name, value in rec.get("intervened", {}).items():
        leaves[f"intervened.{name}"] = value
    for name, value in rec.get("inputs", {}).items():
        leaves[f"inputs.{name}"] = value
    for name, value in rec.get("outputs", {}).items():
        leaves[f"outputs.{name}"] = value
    if "state_hash" in rec:
        for name in ("algo", "scope", "value"):
            leaves[f"state_hash.{name}"] = rec["state_hash"][name]
    if "image" in rec:
        leaves["image"] = rec["image"]
    if "descriptor" in rec:
        for name in ("ea", "size", "sha256"):
            leaves[f"descriptor.{name}"] = rec["descriptor"][name]
    if rec["event"] == "dma":
        for name in ("dir", "lsa", "ea", "len", "tag"):
            leaves[name] = rec[name]
    if "detail" in rec:
        leaves["detail"] = rec["detail"]
    for dump in sorted(rec.get("dumps", []), key=lambda item: item["name"]):
        base = f"dumps.{dump['name']}"
        if "error" in dump:
            leaves[f"{base}.kind"] = "error"
            leaves[f"{base}.error"] = dump["error"]
        else:
            leaves[f"{base}.kind"] = "read"
            leaves[f"{base}.sha256"] = dump["sha256"]
            leaves[f"{base}.len"] = dump["len"]
    return leaves


def _path_order(path: str) -> tuple[int, str]:
    head = path.split(".", 1)[0]
    return (_CATEGORY.get(head, len(_CATEGORY)), path)


def _leaf_signature(value: object) -> str:
    """Type-aware equality: 1, 1.0, true and "1" are four different values."""
    return canonical_json(value)


def _display(value: object) -> str:
    return value if isinstance(value, str) else canonical_json(value)


# --------------------------------------------------------------------------- masks


def _matches(pattern: tuple[str, ...], parts: tuple[str, ...]) -> bool:
    return len(pattern) <= len(parts) and all(
        want == "*" or want == part for want, part in zip(pattern, parts))


def _classify(parts: tuple[str, ...], mask: Mask) -> tuple[int | None, int | None]:
    ignored = next((index for index, pattern in enumerate(mask.ignore)
                    if _matches(pattern, parts)), None)
    normalised = next((index for index, (pattern, _) in enumerate(mask.normalize)
                       if _matches(pattern, parts)), None)
    if ignored is not None and normalised is not None:
        raise Ps3DiffError(
            f"{mask.source}: field '{'.'.join(parts)}' is both ignored and normalised")
    return ignored, normalised


def mark_mask(leaves: dict[str, object], mask: Mask, used: set[tuple[str, int]]) -> None:
    for path in leaves:
        ignored, normalised = _classify(tuple(path.split(".")), mask)
        if ignored is not None:
            used.add(("ignore", ignored))
        if normalised is not None:
            used.add(("normalize", normalised))


def _parse_hex_value(path: str, value: object) -> int:
    if isinstance(value, bool):
        raise Ps3DiffError(f"normalise: {path}: a boolean is not an address")
    if isinstance(value, int):
        number = value
    elif isinstance(value, str) and HEX_ANY.match(value):
        number = int(value, 16)
    else:
        raise Ps3DiffError(
            f"normalise: {path}: expected an integer or 0x hex string, found "
            f"{_show_value(value)}")
    if number < 0:
        raise Ps3DiffError(f"normalise: {path}: address {number} is negative")
    return number


def _show_value(value: object) -> str:
    try:
        text = canonical_json(value)
    except (TypeError, ValueError):
        text = repr(value)
    return text if len(text) <= 60 else text[:57] + "..."


def _normalise(kind: str, path: str, value: object) -> str:
    number = _parse_hex_value(path, value)
    if kind == "hex":
        return f"0x{number:x}"
    return f"0x{number & ~0xFFF:08x}"


def apply_mask(leaves: dict[str, object], mask: Mask,
               used: set[tuple[str, int]]) -> dict[str, object]:
    out = {}
    for path, value in leaves.items():
        ignored, normalised = _classify(tuple(path.split(".")), mask)
        if ignored is not None:
            used.add(("ignore", ignored))
            continue
        if normalised is not None:
            used.add(("normalize", normalised))
            value = _normalise(mask.normalize[normalised][1], path, value)
        out[path] = value
    return out


def _check_mask_used(mask: Mask, used: set[tuple[str, int]]) -> None:
    unused = []
    for index, pattern in enumerate(mask.ignore):
        if ("ignore", index) not in used:
            unused.append(".".join(pattern))
    for index, (pattern, _) in enumerate(mask.normalize):
        if ("normalize", index) not in used:
            unused.append(".".join(pattern))
    if unused:
        raise Ps3DiffError(
            f"{mask.source}: mask path(s) match no compared field: "
            f"{', '.join(sorted(unused))}",
            "a mask that matches nothing hides a typo instead of a field")


# --------------------------------------------------------------------------- compare


def compare_records(a_rec: dict, b_rec: dict, ctx: str, index: int,
                    mask: Mask, used: set[tuple[str, int]]) -> Divergence | None:
    a_leaves = apply_mask(record_leaves(a_rec), mask, used)
    b_leaves = apply_mask(record_leaves(b_rec), mask, used)
    for path in sorted(set(a_leaves) | set(b_leaves), key=_path_order):
        a_value = a_leaves.get(path, MISSING)
        b_value = b_leaves.get(path, MISSING)
        if a_value is MISSING and b_value is MISSING:
            continue
        if a_value is not MISSING and b_value is not MISSING \
                and _leaf_signature(a_value) == _leaf_signature(b_value):
            continue
        return Divergence(ctx, index, _label_of(a_rec), path, a_value, b_value,
                          a_rec["seq"])
    return None


def _effective_targets(a: Side, b: Side, mapping: CtxMap | None) -> dict[str, str]:
    """A context key -> B context key, after the optional map; one to one."""
    forward = mapping.forward if mapping is not None else {}
    source = mapping.source if mapping is not None else ""
    unknown = sorted(key for key in forward if key not in a.contexts)
    if unknown:
        raise Ps3DiffError(
            f"{source}: maps context(s) absent from A: {', '.join(unknown)}")
    missing = sorted((key, target) for key, target in forward.items()
                     if target not in b.contexts)
    if missing:
        key, target = missing[0]
        raise Ps3DiffError(
            f"{source}: maps {key} to {target}, which B does not contain")
    effective: dict[str, str] = {}
    claimed: dict[str, str] = {}
    for a_key in a.contexts:
        b_key = forward.get(a_key, a_key)
        if b_key in claimed:
            raise Ps3DiffError(
                f"{source}: {claimed[b_key]} and {a_key} both map to {b_key}",
                "contexts align one to one; a many-to-one map would compare the "
                "same B records twice")
        claimed[b_key] = a_key
        effective[a_key] = b_key
    return effective


def _divergence_json(divergence: Divergence | None) -> dict | None:
    if divergence is None:
        return None
    return {
        "ctx": divergence.ctx,
        "ctx_seq": divergence.ctx_seq,
        "label": divergence.label,
        "path": divergence.path,
        "a": None if divergence.a is MISSING else _display(divergence.a),
        "b": None if divergence.b is MISSING else _display(divergence.b),
    }


def diff_files(a_path: str, b_path: str, mask_path: str | None = None,
               ctx_map_path: str | None = None) -> tuple[dict, int]:
    """Compare two traces. Returns (report, exit status: 0, 1 or 3)."""
    a = load_side(a_path)
    b = load_side(b_path)
    mask = load_mask(mask_path) if mask_path else EMPTY_MASK
    mapping = load_ctx_map(ctx_map_path) if ctx_map_path else None
    effective = _effective_targets(a, b, mapping)
    reverse = {b_key: a_key for a_key, b_key in effective.items()}
    common = sorted(b_key for b_key in reverse if b_key in b.contexts)

    used: set[tuple[str, int]] = set()
    if mask is not EMPTY_MASK:
        for side in (a, b):
            for rec in side.records:
                mark_mask(record_leaves(rec), mask, used)
        _check_mask_used(mask, used)

    divergences: list[Divergence] = []
    contexts_report: list[dict] = []
    compared = 0

    for b_key in common:
        a_key = reverse[b_key]
        a_records, b_records = a.contexts[a_key], b.contexts[b_key]
        pairs = min(len(a_records), len(b_records))
        first = None
        for index in range(pairs):
            found = compare_records(a_records[index], b_records[index],
                                    b_key, index, mask, used)
            if found is not None and first is None:
                first = found
        compared += pairs
        if first is None and len(a_records) != len(b_records):
            index = pairs
            present = a_records[index] if len(a_records) > index else b_records[index]
            first = Divergence(
                b_key, index, _label_of(present), "record",
                "present" if len(a_records) > index else MISSING,
                "present" if len(b_records) > index else MISSING,
                present["seq"])
        if first is not None:
            divergences.append(first)
        contexts_report.append({
            "ctx": b_key,
            "compared": pairs,
            "first": _divergence_json(first),
            "status": "matched",
        })

    for a_key in sorted(a_key for a_key, b_key in effective.items()
                        if b_key not in b.contexts):
        rec = a.contexts[a_key][0]
        first = Divergence(a_key, 0, _label_of(rec), "ctx", "present", MISSING,
                           rec["seq"])
        divergences.append(first)
        contexts_report.append({
            "ctx": a_key,
            "compared": 0,
            "first": _divergence_json(first),
            "status": "a-only",
        })

    for b_key in sorted(key for key in b.contexts if key not in reverse):
        rec = b.contexts[b_key][0]
        first = Divergence(b_key, 0, _label_of(rec), "ctx", MISSING, "present",
                           rec["seq"])
        divergences.append(first)
        contexts_report.append({
            "ctx": b_key,
            "compared": 0,
            "first": _divergence_json(first),
            "status": "b-only",
        })

    contexts_report.sort(key=lambda entry: entry["ctx"])
    overall = min(divergences,
                  key=lambda item: (item.order, item.ctx, item.ctx_seq, item.path),
                  default=None)
    report = {
        "schema": REPORT_SCHEMA,
        "a": str(a_path),
        "b": str(b_path),
        "identical": compared > 0 and not divergences,
        "counts": {
            "compared": compared,
            "contexts_a": len(a.contexts),
            "contexts_b": len(b.contexts),
            "contexts_matched": len(common),
        },
        "first": _divergence_json(overall),
        "contexts": contexts_report,
    }
    if compared == 0:
        return report, 3
    return report, (1 if divergences else 0)


def report_text(report: dict) -> str:
    """The human report. Deterministic: contexts arrive sorted."""
    counts = report["counts"]
    lines = [f"ps3diff: A={report['a']} B={report['b']}",
             f"ps3diff: compared {counts['compared']} records in "
             f"{counts['contexts_matched']} matched contexts "
             f"({counts['contexts_a']} in A, {counts['contexts_b']} in B)"]
    if report["identical"]:
        lines.append("ps3diff: identical")
        return "\n".join(lines)
    first = report["first"]
    if first is not None:
        lines.append(f"ps3diff: first divergence: ctx={first['ctx']} "
                     f"ctx_seq={first['ctx_seq']} label={first['label']} "
                     f"field={first['path']}")
        lines.append(f"  A: {_text_value(first['a'])}")
        lines.append(f"  B: {_text_value(first['b'])}")
    if report["contexts"]:
        lines.append("per context:")
        for entry in report["contexts"]:
            detail = entry["first"]
            if detail is None:
                lines.append(f"  {entry['ctx']}: no divergence "
                             f"({entry['compared']} compared)")
            else:
                status = "" if entry["status"] == "matched" else f", {entry['status']}"
                lines.append(
                    f"  {entry['ctx']}: {detail['path']} at ctx_seq "
                    f"{detail['ctx_seq']} label={detail['label']} "
                    f"({entry['compared']} compared{status})")
    return "\n".join(lines)


def _text_value(value: object) -> str:
    return "<absent>" if value is None else str(value)
