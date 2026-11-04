#!/usr/bin/env python3
"""Diff two lv2conf result files, typically the console run (A, the golden) and the host-HLE run (B).

    compare_lv2conf.py A.csv B.csv [--divergences FILE]

Both files are parsed through hwprobe_format.py (the versioned header, `HDR,probe,lv2conf`) and carry

    CASE,<case_id>,<key>,<value>      compared
    TIME,<case_id>,<key>,<ticks>      reported as min/median per key, never compared
    SKIP,<case_id>,<reason>           the side did not run the case
    META,<key>,<value>                informational

Per group the tool prints how many cases match, differ, exist only in A or only in B, or are skipped (on
either side), then every differing row with both values. A case's keys are compared as value lists, so a
key recorded twice must be recorded identically.

A divergence file lists the rows that are known to differ, one per line:

    <case_id> <key> #<issue> <short reason>

The key `*` covers every key of the case (a case the host stops or hangs on). Blank lines and lines
starting with `#` are comments.

Exit status: 0 when every difference is listed and every listed entry still differs; 1 when a CASE row
differs and is not listed, when a listed entry no longer differs (stale), or when a case has rows on one
side and nothing, not even a SKIP, on the other; 2 for a file this reader cannot interpret. The C runner
(tests/syscalls/test_lv2conf.c) applies the same rules.

Standard library only.
"""
import argparse
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..")))

import hwprobe_format

CASE_ID_RE = re.compile(r"^[a-z0-9_]+(\.[a-z0-9_]+)+$")
ISSUE_RE = re.compile(r"^#[0-9]+$")


class DivergenceError(ValueError):
    """The divergence file is malformed."""


class CaseRows:
    """What one result file says about one case."""

    def __init__(self):
        self.skipped = None   # the SKIP reason, or None
        self.rows = []        # [(key, value)] in file order
        self.times = {}       # key -> [ticks]

    def values(self, key):
        return [v for k, v in self.rows if k == key]

    def key_order(self):
        seen = []
        for k, _ in self.rows:
            if k not in seen:
                seen.append(k)
        return seen


class Result:
    def __init__(self, path, header):
        self.path = path
        self.header = header
        self.cases = {}   # case_id -> CaseRows, in file order
        self.meta = {}

    def case(self, case_id):
        return self.cases.setdefault(case_id, CaseRows())


def group_of(case_id):
    return case_id.split(".", 1)[0]


def parse_result(path):
    """Read one result file. FormatError (a ValueError) on anything this reader cannot interpret."""
    try:
        with open(path, newline="") as f:
            lines = f.read().splitlines()
    except OSError as e:
        raise hwprobe_format.FormatError(f"{path}: {e.strerror}") from e
    try:
        hdr, pos = hwprobe_format.parse_header(lines)
    except hwprobe_format.FormatError as e:
        raise hwprobe_format.FormatError(f"{path}: {e}") from e
    if hdr["probe"] != "lv2conf":
        raise hwprobe_format.FormatError(f"{path}: HDR,probe is {hdr['probe']!r}, expected 'lv2conf'")
    res = Result(path, hdr)
    for lineno, line in enumerate(lines[pos:], start=pos + 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        kind = line.split(",", 1)[0]

        def bad(why, lineno=lineno, line=line):
            return hwprobe_format.FormatError(f"{path}:{lineno}: {why}: {line[:60]!r}")

        if kind in ("CASE", "TIME"):
            parts = line.split(",", 3)
            if len(parts) != 4:
                raise bad(f"{kind} row needs <case_id>,<key>,<value>")
            _, cid, key, val = parts
            if not CASE_ID_RE.match(cid):
                raise bad("bad case id")
            if not key:
                raise bad("empty key")
            c = res.case(cid)
            if kind == "CASE":
                c.rows.append((key, val))
            else:
                try:
                    c.times.setdefault(key, []).append(int(val))
                except ValueError:
                    raise bad("TIME value is not an integer") from None
        elif kind == "SKIP":
            parts = line.split(",", 2)
            if len(parts) != 3 or not CASE_ID_RE.match(parts[1]) or not parts[2]:
                raise bad("SKIP row needs <case_id>,<reason>")
            res.case(parts[1]).skipped = parts[2]
        elif kind == "META":
            parts = line.split(",", 2)
            if len(parts) != 3:
                raise bad("META row needs <key>,<value>")
            res.meta[parts[1]] = parts[2]
        elif kind == "HDR":
            raise bad("HDR row after the header block")
        else:
            raise bad("unknown row type")
    return res


def parse_divergences(path):
    """-> {(case_id, key): (issue, reason)}. DivergenceError on a malformed or duplicated entry."""
    out = {}
    try:
        with open(path) as f:
            text = f.read()
    except OSError as e:
        raise DivergenceError(f"{path}: {e.strerror}") from e
    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 3)
        if len(parts) != 4 or not ISSUE_RE.match(parts[2]):
            raise DivergenceError(f"{path}:{lineno}: expected '<case_id> <key> #<issue> <reason>', got {line[:60]!r}")
        cid, key, issue, reason = parts
        if (cid, key) in out:
            raise DivergenceError(f"{path}:{lineno}: duplicate entry for {cid} {key}")
        out[(cid, key)] = (issue, reason)
    return out


class Diff:
    def __init__(self, case_id, key, a, b, listed):
        self.case_id, self.key, self.a, self.b, self.listed = case_id, key, a, b, listed  # listed: (issue, reason) or None


class Report:
    def __init__(self):
        self.groups = {}      # group -> {"match","differs","only_a","only_b","skipped"} counts
        self.diffs = []       # [Diff]
        self.only_a = []      # case ids with rows in A and nothing in B
        self.only_b = []
        self.skipped = []     # (case_id, side, reason)
        self.stale = []       # [(case_id, key, issue, reason)]
        self.times = []       # [(case_id, key, (min, median, n) of A or None, same for B)]

    def counts(self, case_id):
        return self.groups.setdefault(group_of(case_id), dict.fromkeys(("match", "differs", "only_a", "only_b", "skipped"), 0))

    @property
    def unlisted(self):
        return [d for d in self.diffs if d.listed is None]

    @property
    def ok(self):
        return not (self.unlisted or self.stale or self.only_a or self.only_b)


def _has_rows(c):
    return c is not None and c.skipped is None and bool(c.rows)


def _time_stats(c, key):
    xs = c.times.get(key) if c is not None else None
    return (min(xs), statistics.median(xs), len(xs)) if xs else None


def compare(a, b, divergences):
    rep = Report()
    used = set()
    ids = list(a.cases) + [i for i in b.cases if i not in a.cases]
    for cid in ids:
        ca, cb = a.cases.get(cid), b.cases.get(cid)
        counts = rep.counts(cid)
        for side, c in (("A", ca), ("B", cb)):
            if c is not None and c.skipped is not None:
                rep.skipped.append((cid, side, c.skipped))
        if any(c is not None and c.skipped is not None for c in (ca, cb)):
            counts["skipped"] += 1
        elif _has_rows(ca) and not _has_rows(cb):
            counts["only_a"] += 1
            rep.only_a.append(cid)
        elif _has_rows(cb) and not _has_rows(ca):
            counts["only_b"] += 1
            rep.only_b.append(cid)
        elif _has_rows(ca) and _has_rows(cb):
            diffs = []
            for key in ca.key_order() + [k for k in cb.key_order() if k not in ca.key_order()]:
                va, vb = ca.values(key), cb.values(key)
                if va != vb:
                    entry = None
                    for k in (key, "*"):
                        if (cid, k) in divergences:
                            used.add((cid, k))
                            entry = divergences[(cid, k)]
                            break
                    diffs.append(Diff(cid, key, va, vb, entry))
            rep.diffs.extend(diffs)
            counts["differs" if diffs else "match"] += 1
        else:
            counts["match"] += 1  # nothing but TIME rows on either side: nothing to compare
        # TIME rows are reported for any case a side recorded them for, whatever its other rows did
        for key in sorted(set(ca.times if ca else ()) | set(cb.times if cb else ())):
            rep.times.append((cid, key, _time_stats(ca, key), _time_stats(cb, key)))
    for (cid, key), (issue, reason) in divergences.items():
        if (cid, key) not in used:
            rep.stale.append((cid, key, issue, reason))
    return rep


def _fmt_values(vals):
    return ",".join(vals) if vals else "<absent>"


def _fmt_time(s):
    if s is None:
        return "-"
    med = int(s[1]) if s[1] == int(s[1]) else round(s[1], 1)
    return f"min {s[0]} med {med} n={s[2]}"


def render(rep, a, b, out):
    def w(s=""):
        print(s, file=out)

    for tag, r in (("A", a), ("B", b)):
        h = r.header
        w(f"{tag}: {r.path}  (probe_version={h.get('probe_version')} fw_type={h.get('fw_type')} "
          f"console_model={h.get('console_model')})")
    w()
    cols = ("match", "differs", "only_a", "only_b", "skipped")
    labels = {"match": "match", "differs": "differs", "only_a": "only-in-A", "only_b": "only-in-B", "skipped": "skipped"}
    w(f"{'group':<16}" + "".join(f"{labels[c]:>11}" for c in cols))
    total = dict.fromkeys(cols, 0)
    for g in sorted(rep.groups):
        w(f"{g:<16}" + "".join(f"{rep.groups[g][c]:>11}" for c in cols))
        for c in cols:
            total[c] += rep.groups[g][c]
    w(f"{'total':<16}" + "".join(f"{total[c]:>11}" for c in cols))
    if rep.diffs:
        w()
        w("Differing rows:")
        for d in rep.diffs:
            tail = f"listed {d.listed[0]}: {d.listed[1]}" if d.listed else "UNLISTED"
            w(f"  {d.case_id} {d.key}  A={_fmt_values(d.a)}  B={_fmt_values(d.b)}  [{tail}]")
    for label, ids in (("Only in A (rows here, nothing in B)", rep.only_a), ("Only in B (rows here, nothing in A)", rep.only_b)):
        if ids:
            w()
            w(f"{label}:")
            for cid in ids:
                w(f"  {cid}")
    if rep.skipped:
        w()
        w("Skipped:")
        for cid, side, reason in rep.skipped:
            w(f"  {cid} ({side}: {reason})")
    if rep.stale:
        w()
        w("Stale divergences (the rows no longer differ; remove the entry):")
        for cid, key, issue, reason in rep.stale:
            w(f"  {cid} {key} {issue} {reason}")
    if rep.times:
        w()
        w("TIME rows (ticks, never compared):")
        for cid, key, sa, sb in rep.times:
            w(f"  {cid} {key}  A: {_fmt_time(sa)}  B: {_fmt_time(sb)}")
    w()
    w("RESULT: " + ("ok" if rep.ok else
                    f"FAIL ({len(rep.unlisted)} unlisted difference(s), {len(rep.stale)} stale divergence(s), "
                    f"{len(rep.only_a)} only-in-A, {len(rep.only_b)} only-in-B)"))


def main(argv=None, out=None):
    out = out or sys.stdout
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("a", metavar="A.csv", help="first result file, typically the console run (the golden)")
    ap.add_argument("b", metavar="B.csv", help="second result file, typically the host-HLE run")
    ap.add_argument("--divergences", metavar="FILE", help="rows that are known to differ")
    args = ap.parse_args(argv)
    try:
        a, b = parse_result(args.a), parse_result(args.b)
        divs = parse_divergences(args.divergences) if args.divergences else {}
    except (hwprobe_format.FormatError, DivergenceError) as e:
        print(f"compare_lv2conf: {e}", file=sys.stderr)
        return 2
    rep = compare(a, b, divs)
    render(rep, a, b, out)
    return 0 if rep.ok else 1


if __name__ == "__main__":
    sys.exit(main())
