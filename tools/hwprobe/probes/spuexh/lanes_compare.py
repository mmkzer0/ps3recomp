#!/usr/bin/env python3
"""Set the per-lane dumps of a spuexh result file (lanes= jobs, RUN.md "Lanes") against the toolkit's model.

    lanes_compare.py RESULT.csv --scalar-bin test_spu_exhaustive.scalar [--md OUT.md] [--only-diff]

RESULT.csv     a spuexh result file with `LANES` / `LANE` / `LANES_DONE` / `LANES_FAULT` rows (FORMAT.md), from the console
               or from the dry run (spuexh_host).
--scalar-bin   the host harness' scalar build. For every LANES group (op, domain, block) it runs
               `<bin> --dump <op> <domain> <block> --dump-n <calls>` and takes `out_fast` (what the toolkit's op computes: the
               model the sweep hashes) as the model; `out_ref` is the harness' own reference where it has one.

Step 1, the generator check: every dumped call must have exactly the operands and immediate the harness' generator makes for
that call (the immediate as the field value patched into the word: the harness' value modulo the field width, `-` = no
field). A difference means the SPU generator or the table is at fault, not the instruction: the exit status is 1 and no
verdict about the lanes of that group is worth anything.
Step 2: for each 32-bit lane (for fesd: each 64-bit result lane, fed by input words 0 and 2) hardware and model are compared.
Differences are data, not errors. The summary lists, per op, how many lanes are equal / differ and, per class of the lane's
input word seen as an IEEE single (sign, exponent, fraction), how many of the lanes of that class differ.

Exit status: 1 when a dumped input differs from the generator's (or a call is missing from the harness' output), 0 otherwise,
2 on a usage, file or format error (also: the harness binary cannot be run).
"""
import argparse
import collections
import subprocess
import sys

BLOCK_LOG2 = 20          # EXH_BLOCK_LOG2 of runtime/spu/tests/spu_exh_gen.h

# Result lanes of an op: (input word index, first output word, output words). The default is word i -> word i; the ops listed
# here produce a doubleword from one single: fesd widens words 0 and 2 (the even slots) to two doubles.
LAYOUTS = {"fesd": [(0, 0, 2), (2, 2, 2)]}
DEFAULT_LAYOUT = [(i, i, 1) for i in range(4)]


class FormatError(Exception):
    pass


class Group:
    def __init__(self, op, dom, blk, count=None, imm_bits=None, start=None):
        self.op, self.dom, self.blk = op, dom, blk
        self.count, self.imm_bits, self.start = count, imm_bits, start    # from the LANES header row (None: a file without one)
        self.rows = []                                      # (call, a, b, c, imm, out)
        self.status = None                                  # "done" | "fault: text" | None (no closing row: lost run)

    @property
    def key(self):
        return (self.op, self.dom, self.blk)


def _hex128(s, what, lineno):
    if len(s) != 32:
        raise FormatError(f"line {lineno}: {what} is not 32 hex digits: {s!r}")
    try:
        int(s, 16)
    except ValueError:
        raise FormatError(f"line {lineno}: {what} is not hex: {s!r}") from None
    return s.lower()


def parse_result(path):
    """Groups of the LANES rows of a result file, in file order."""
    groups = []
    cur = None
    with open(path, encoding="utf-8") as f:
        for lineno, line in enumerate(f.read().splitlines(), 1):
            p = line.split(",")
            kind = p[0]
            if kind == "LANES":
                if len(p) not in (6, 7):                     # 7: with the start call (a file of the first lanes mode has 6)
                    raise FormatError(f"line {lineno}: LANES row needs 6 or 7 fields")
                try:
                    cur = Group(p[1], p[2], int(p[3]), int(p[4]), int(p[5]), int(p[6]) if len(p) == 7 else None)
                except ValueError:
                    raise FormatError(f"line {lineno}: bad number in LANES row") from None
                groups.append(cur)
            elif kind == "LANE":
                if len(p) != 10:
                    raise FormatError(f"line {lineno}: LANE row needs 10 fields")
                try:
                    blk, call = int(p[3]), int(p[4])
                    imm = None if p[8] == "-" else int(p[8])
                except ValueError:
                    raise FormatError(f"line {lineno}: bad number in LANE row") from None
                if cur is None or cur.key != (p[1], p[2], blk):
                    cur = Group(p[1], p[2], blk)             # a LANE row without its header row
                    groups.append(cur)
                a, b, c = (_hex128(p[5 + i], n, lineno) for i, n in enumerate("abc"))
                cur.rows.append((call, a, b, c, imm, _hex128(p[9], "out", lineno)))
            elif kind in ("LANES_DONE", "LANES_FAULT") and cur is not None and cur.key == (p[1], p[2], int(p[3])):
                cur.status = "done" if kind == "LANES_DONE" else "fault: " + ",".join(p[5:])
    return groups


def run_scalar(binary, op, dom, blk, start, ncalls):
    """{call: (a, b, c, imm, out_fast, out_ref)} of ncalls calls of the block, from call `start` (counted from the start of the
    block), from the host harness."""
    argv = [binary, "--dump", op, dom, str(blk), "--dump-start", str(start), "--dump-n", str(ncalls)]
    try:
        p = subprocess.run(argv, check=False, capture_output=True, text=True, timeout=600)
    except (OSError, subprocess.SubprocessError) as e:
        raise FormatError(f"cannot run {' '.join(argv)}: {e}") from None
    if p.returncode != 0:
        raise FormatError(f"{' '.join(argv)} exited {p.returncode}: {p.stderr.strip()}")
    out = {}
    for line in p.stdout.splitlines():
        f = line.split()
        if len(f) != 7:
            raise FormatError(f"unexpected --dump line from the harness: {line!r}")
        out[int(f[0])] = (f[1], f[2], f[3], int(f[4]), f[5], f[6])
    return out


def classify(word):
    """(sign, exponent class, fraction class) of a 32-bit word read as an IEEE single."""
    sign, e, frac = word >> 31, (word >> 23) & 0xFF, word & 0x7FFFFF
    if e == 0:
        ec = "zero" if frac == 0 else "denormal"
    elif e == 0xFF:
        ec = "inf" if frac == 0 else "nan"
    else:
        ec = f"2^{e - 127}"
    fc = "0" if frac == 0 else "all-ones" if frac == 0x7FFFFF else "other"
    return ("-" if sign else "+", ec, fc)


def _imm_equal(hw_imm, scalar_imm, bits):
    """hw_imm is the field value patched into the word; the harness prints its own (signed) value."""
    if hw_imm is None:                       # the op has no immediate field: nothing was patched
        return True
    mask = (1 << bits) - 1 if bits else 0xFFFFFFFF
    return hw_imm == (scalar_imm & mask)


class Result:
    def __init__(self, group):
        self.group = group
        self.input_errors = []               # strings (first few) describing generator mismatches
        self.n_input_errors = 0
        self.lanes = []                      # (call, lane, in_word, b_word, c_word, hw, model, ref)
        self.fast_ne_ref = 0

    def equal(self):
        return sum(1 for ln in self.lanes if ln[5] == ln[6])

    def differ(self):
        return len(self.lanes) - self.equal()


def compare_group(binary, group):
    res = Result(group)
    lo = group.blk << BLOCK_LOG2
    if not group.rows:
        return res
    first = min(r[0] for r in group.rows)
    if first < lo:
        res.n_input_errors += 1
        res.input_errors.append(f"call {first} is outside block {group.blk}")
        return res
    if group.start is not None and first - lo != group.start:
        # the entry asked for calls from `start`, the SPU dumped others: its handling of the request is at fault
        res.n_input_errors += len(group.rows)
        res.input_errors.append(f"the entry asked for calls from {group.start} of the block, the dump starts at call {first - lo}")
        return res
    ncalls = max(r[0] for r in group.rows) - first + 1
    sc = run_scalar(binary, group.op, group.dom, group.blk, first - lo, ncalls)
    layout = LAYOUTS.get(group.op, DEFAULT_LAYOUT)
    for call, a, b, c, imm, out in group.rows:
        s = sc.get(call)
        if s is None:
            res.n_input_errors += 1
            if len(res.input_errors) < 5:
                res.input_errors.append(f"call {call}: not in the harness' dump of block {group.blk}")
            continue
        sa, sb, sc_, simm, fast, ref = s
        bad = [n for n, x, y in (("a", a, sa), ("b", b, sb), ("c", c, sc_)) if x != y]
        if not _imm_equal(imm, simm, group.imm_bits):
            bad.append("imm")
        if bad:
            res.n_input_errors += 1
            if len(res.input_errors) < 5:
                res.input_errors.append(f"call {call}: {'/'.join(bad)} differ: hw a={a} b={b} c={c} imm={imm} | "
                                        f"generator a={sa} b={sb} c={sc_} imm={simm}")
            continue
        if fast != ref:
            res.fast_ne_ref += 1
        for lane, (iw, ow, nw) in enumerate(layout):
            res.lanes.append((call, lane, int(a[8 * iw:8 * iw + 8], 16), b[8 * iw:8 * iw + 8], c[8 * iw:8 * iw + 8],
                              out[8 * ow:8 * (ow + nw)], fast[8 * ow:8 * (ow + nw)], ref[8 * ow:8 * (ow + nw)]))
    return res


def _group_title(g):
    return f"{g.op} {g.dom} block {g.blk}"


def _status_note(g, res):
    notes = []
    if g.status is None:
        notes.append("no LANES_DONE row: the run was lost or is still going")
    elif g.status != "done":
        notes.append("LANES_" + g.status.upper())
    if g.count is not None and len(g.rows) != g.count:
        notes.append(f"{len(g.rows)} of {g.count} calls dumped")
    return notes


def lane_lines(res, only_diff):
    """Per-lane lines of a group. b and c are printed only when some lane has a non-zero one."""
    show_b = any(int(ln[3], 16) for ln in res.lanes)
    show_c = any(int(ln[4], 16) for ln in res.lanes)
    out = []
    for call, lane, inw, bw, cw, hw, model, ref in res.lanes:
        eq = hw == model
        if only_diff and eq:
            continue
        extra = (f" b={bw}" if show_b else "") + (f" c={cw}" if show_c else "")
        out.append(f"call {call} lane {lane}: in={inw:08x}{extra} hw={hw} model={model} {'==' if eq else '!='}")
    return out


def class_table(res):
    """{class: [differ, total]} over the lanes of a group."""
    t = collections.defaultdict(lambda: [0, 0])
    for _call, _lane, inw, _b, _c, hw, model, _ref in res.lanes:
        k = classify(inw)
        t[k][1] += 1
        if hw != model:
            t[k][0] += 1
    return t


def render(results, only_diff, md):
    """Text (or markdown) report; returns the list of lines."""
    L = []
    h2 = "## " if md else "== "
    L.append(("# " if md else "") + "lanes: hardware against the toolkit's model")
    L.append("")
    if md:
        L.append("| op | domain | block | calls | lanes | equal | differ | generator check |")
        L.append("|---|---|---|---|---|---|---|---|")
    for res in results:
        g = res.group
        chk = "OK" if not res.n_input_errors else f"FAILED ({res.n_input_errors} calls)"
        row = (g.op, g.dom, g.blk, len(g.rows), len(res.lanes), res.equal(), res.differ(), chk)
        L.append("| " + " | ".join(str(x) for x in row) + " |" if md else
                 f"{g.op:<10} {g.dom:<6} block {g.blk:<4} calls {len(g.rows):<5} lanes {len(res.lanes):<5} "
                 f"equal {res.equal():<5} differ {res.differ():<5} generator check {chk}")
    for res in results:
        g = res.group
        L.append("")
        L.append(h2 + _group_title(g))
        for n in _status_note(g, res):
            L.append(f"NOTE: {n}")
        if res.n_input_errors:
            L.append(f"GENERATOR MISMATCH: {res.n_input_errors} dumped calls differ from the harness' inputs; first ones:")
            L.extend("  " + e for e in res.input_errors)
        if res.fast_ne_ref:
            L.append(f"NOTE: the harness' own fast and reference results differ in {res.fast_ne_ref} calls "
                     "(the model column is the fast one)")
        L.append(f"{len(res.lanes)} lanes: {res.equal()} equal, {res.differ()} differ")
        if res.differ():
            L.append("differing lanes by input class (sign exponent fraction): differ / lanes of that class")
            for k, (d, n) in sorted(class_table(res).items(), key=lambda kv: (-kv[1][0], kv[0])):
                if d:
                    L.append(f"  {k[0]} {k[1]} frac={k[2]}: {d} / {n}")
        lines = lane_lines(res, only_diff)
        if lines:
            if md:
                L.append("")
                L.append("```text")
            L.extend(lines)
            if md:
                L.append("```")
    return L


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("result")
    ap.add_argument("--scalar-bin", required=True, help="the host harness' scalar build (test_spu_exhaustive.scalar)")
    ap.add_argument("--md", help="also write the report as markdown to this file")
    ap.add_argument("--only-diff", action="store_true", help="print only the differing lanes (default: every lane)")
    args = ap.parse_args(argv)
    try:
        groups = parse_result(args.result)
        if not groups:
            raise FormatError(f"{args.result}: no LANES/LANE rows (was the run configured with lanes=?)")
        results = [compare_group(args.scalar_bin, g) for g in groups]
    except (FormatError, OSError) as e:
        print(f"lanes_compare: {e}", file=sys.stderr)
        return 2
    print("\n".join(render(results, args.only_diff, False)))
    if args.md:
        try:
            with open(args.md, "w", encoding="utf-8") as f:
                f.write("\n".join(render(results, args.only_diff, True)) + "\n")
        except OSError as e:
            print(f"lanes_compare: {e}", file=sys.stderr)
            return 2
    return 1 if any(r.n_input_errors for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
