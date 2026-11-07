#!/usr/bin/env python3
"""Analyze special-B result rows without scalar or interpreter references."""
import csv
import io
import re
import sys
from pathlib import Path

import gen_tables

ROOT = Path(__file__).resolve().parents[4]
_HEX = re.compile(r"[0-9a-fA-F]+\Z")
_DECIMAL = re.compile(r"[0-9]+\Z")
_META_KEYS = {
    "special_b_prefixes",
    "special_b_from",
    "special_b_count",
    "special_b_patterns",
    "special_b_prefixes_done",
    "special_b_completed",
}


class ReportError(ValueError):
    """A result file has malformed or inconsistent special-B rows."""


def _decimal(value, label):
    if not _DECIMAL.fullmatch(value):
        raise ReportError(f"{label} must be an unsigned decimal integer, got {value!r}")
    return int(value)


def _hex(value, width, label):
    if len(value) != width or not _HEX.fullmatch(value):
        raise ReportError(f"{label} must be {width} hexadecimal digits, got {value!r}")
    return int(value, 16)


def _identity(row, offset, line_number):
    label = f"line {line_number}"
    index = _decimal(row[offset], f"{label} prefix index")
    prefix = _hex(row[offset + 1], 3, f"{label} prefix")
    pattern = row[offset + 2]
    word = _hex(row[offset + 3], 8, f"{label} word")
    if prefix > 0x7FF or (word >> 21) != prefix:
        raise ReportError(f"{label} prefix does not match the 11-bit prefix of its word")
    if pattern not in ("zero", "rand"):
        raise ReportError(f"{label} pattern must be zero or rand")
    return index, prefix, pattern, word


def _expected_attempts(metadata):
    required = ("special_b_prefixes", "special_b_count", "special_b_patterns")
    if any(key not in metadata for key in required):
        return None, None
    total = _decimal(metadata["special_b_prefixes"], "special_b_prefixes")
    start = _decimal(metadata.get("special_b_from", "0"), "special_b_from")
    count = _decimal(metadata["special_b_count"], "special_b_count")
    patterns = metadata["special_b_patterns"]
    if total < 1 or start >= total:
        raise ReportError("special-B metadata has an empty prefix range")
    if patterns not in ("zero", "both"):
        raise ReportError("special_b_patterns must be zero or both")
    selected = min(count, total - start) if count else total - start
    return selected * (2 if patterns == "both" else 1), selected


def _validate_attempt_range(metadata, attempts, selected):
    if selected is None:
        return
    start = _decimal(metadata.get("special_b_from", "0"), "special_b_from")
    end = start + selected
    patterns = metadata["special_b_patterns"]
    for attempt in attempts:
        index, _, pattern, _ = attempt["identity"]
        if not start <= index < end:
            raise ReportError(f"prefix index {index} is outside the configured batch")
        if patterns == "zero" and pattern != "zero":
            raise ReportError(f"prefix index {index} includes an unconfigured {pattern} pattern")



def _classify(attempt):
    if attempt["fatal"] is not None:
        return "FATAL: " + attempt["fatal"]
    result = attempt["result"]
    if result is None:
        return "suspected freeze"
    outcome = result["outcome"]
    text = result["text"]
    if outcome == "executed":
        return "normal return"
    if outcome == "not_started":
        return "not started"
    lower = text.lower()
    if "event[" in lower or "fault" in lower:
        cause = re.search(r"\bcause=([^\s]+)", text)
        return f"SPU error event + cause={cause.group(1) if cause else 'unknown'}"
    if "terminate-join" in lower:
        return "timeout+terminate"
    cause = re.search(r"\bcause=(0x[0-9a-f]+|[0-9]+)", lower)
    group_exit = cause and int(cause.group(1), 16 if cause.group(1).startswith("0x") else 10) == 1
    if group_exit or "halt" in lower or "stop" in lower:
        return "halt"
    return "unknown no-return"


def _verdict(expectation, observed):
    if observed.startswith("FATAL:") or observed in ("suspected freeze", "not started", "unknown no-return", "timeout+terminate", "halt"):
        return "unknown"
    if expectation == "invalid->stop":
        return "agree" if observed.startswith("SPU error event + cause=") else "differ"
    if observed.startswith("SPU error event + cause="):
        return "differ"
    if observed == "normal return":
        return "agree"
    return "unknown"


def analyze_text(text, root=ROOT):
    """Return decoded per-attempt results and completion-marker status."""
    try:
        rows = csv.reader(io.StringIO(text, newline=""), strict=True)
        decoded = gen_tables.load_decode(str(root))[1]
        attempts = []
        pending = None
        metadata = {}
        fatal_seen = False
        for line_number, row in enumerate(rows, 1):
            if not row or not row[0] or row[0].startswith("#"):
                continue
            kind = row[0]
            if kind == "META":
                if len(row) < 3:
                    raise ReportError(f"line {line_number} has a short META row")
                if row[1] in _META_KEYS:
                    if row[1] in metadata:
                        raise ReportError(f"line {line_number} repeats META,{row[1]}")
                    metadata[row[1]] = row[2]
            elif kind == "SPECB_TRY":
                if "special_b_completed" in metadata:
                    raise ReportError(f"line {line_number} SPECB_TRY follows the completion marker")
                if len(row) != 5:
                    raise ReportError(f"line {line_number} SPECB_TRY needs 5 fields")
                identity = _identity(row, 1, line_number)
                pending = {"identity": identity, "result": None, "fatal": None}
                attempts.append(pending)
            elif kind == "SPECB":
                if "special_b_completed" in metadata:
                    raise ReportError(f"line {line_number} SPECB follows the completion marker")
                if len(row) != 11:
                    raise ReportError(f"line {line_number} SPECB needs 11 fields")
                identity = _identity(row, 1, line_number)
                if pending is None or pending["identity"] != identity:
                    raise ReportError(f"line {line_number} SPECB has no matching preceding SPECB_TRY")
                if pending["result"] is not None or pending["fatal"] is not None:
                    raise ReportError(f"line {line_number} duplicates a special-B outcome")
                if row[5] not in ("executed", "no_return", "not_started"):
                    raise ReportError(f"line {line_number} has an unknown SPECB outcome {row[5]!r}")
                result = {
                    "outcome": row[5],
                    "nchanged": _decimal(row[6], f"line {line_number} nchanged"),
                    "regs": row[7] or "-",
                    "arena": _decimal(row[8], f"line {line_number} arena"),
                    "arena_addr": _hex(row[9], 8, f"line {line_number} arena_addr"),
                    "text": ",".join(row[10:]),
                }
                pending["result"] = result
                pending = None
            elif kind == "FATAL":
                if len(row) < 3:
                    raise ReportError(f"line {line_number} has a short FATAL row")
                if "special_b_completed" not in metadata:
                    fatal_seen = True
                if pending is not None:
                    pending["fatal"] = ",".join(row[2:]) or row[1]
                    pending = None
    except csv.Error as error:
        raise ReportError(f"malformed CSV: {error}") from error

    expected_attempts, expected_prefixes = _expected_attempts(metadata)
    _validate_attempt_range(metadata, attempts, expected_prefixes)
    reports = []
    for attempt in attempts:
        index, prefix, pattern, word = attempt["identity"]
        mnemonic = decoded[prefix]
        expectation = "invalid->stop" if mnemonic == "word" else mnemonic
        result = attempt["result"]
        effects = "-"
        if result is not None:
            effects = (f"nchanged={result['nchanged']}; regs={result['regs']}; "
                       f"arena={result['arena']}@0x{result['arena_addr']:08x}")
        observed = _classify(attempt)
        reports.append({
            "index": index,
            "prefix": f"0x{prefix:03x}",
            "pattern": pattern,
            "word": f"0x{word:08x}",
            "expectation": expectation,
            "observed": observed,
            "effects": effects,
            "verdict": _verdict(expectation, observed),
        })

    marker = metadata.get("special_b_completed")
    if marker not in (None, "0", "1"):
        raise ReportError("special_b_completed must be 0 or 1")
    if marker is None:
        completion = "missing"
    elif marker == "0":
        completion = "incomplete (marker=0)"
    else:
        reasons = []
        if expected_attempts is None:
            reasons.append("missing special-B config metadata")
        if fatal_seen:
            reasons.append("FATAL row present")
        if any(attempt["result"] is None and attempt["fatal"] is None for attempt in attempts):
            reasons.append("TRY without outcome")
        if expected_attempts is not None and len(attempts) != expected_attempts:
            reasons.append(f"{len(attempts)} of {expected_attempts} attempts")
        if expected_prefixes is not None:
            if "special_b_prefixes_done" not in metadata:
                reasons.append("missing special_b_prefixes_done")
            else:
                done = _decimal(metadata["special_b_prefixes_done"], "special_b_prefixes_done")
                if done != expected_prefixes:
                    reasons.append(f"{done} of {expected_prefixes} prefixes")
        completion = "incomplete (" + "; ".join(reasons) + ")" if reasons else "complete"

    return {
        "rows": reports,
        "completion": completion,
        "fatal": fatal_seen,
        "expected_attempts": expected_attempts,
    }


def _cell(value):
    return str(value).replace("|", r"\|").replace("\n", " ")


def render_report(report):
    lines = [
        "# Special B report",
        "",
        f"Completion marker: **{_cell(report['completion'])}**",
        "",
        "| Index | Prefix | Pattern | Word | Toolkit expectation | Observed class | Effects | Verdict |",
        "|---:|---:|---|---:|---|---|---|---|",
    ]
    for row in report["rows"]:
        cells = (row["index"], row["prefix"], row["pattern"], row["word"], row["expectation"], row["observed"],
                 row["effects"], row["verdict"])
        lines.append("| " + " | ".join(_cell(value) for value in cells) + " |")
    if not report["rows"]:
        lines.append("| | | | | | No SPECB_TRY rows found | | unknown |")
    return "\n".join(lines) + "\n"


def main(argv=None):
    argv = sys.argv if argv is None else argv
    if len(argv) != 2 or argv[1] in ("-h", "--help"):
        print("usage: specialb_report.py <spuexh-result.csv>", file=sys.stderr)
        return 0 if len(argv) == 2 else 2
    try:
        text = Path(argv[1]).read_text(encoding="utf-8")
        report = analyze_text(text)
    except (OSError, ReportError) as error:
        print(f"specialb_report: {error}", file=sys.stderr)
        return 2
    sys.stdout.write(render_report(report))
    return 0 if report["completion"] == "complete" and all(row["verdict"] == "agree" for row in report["rows"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
