#!/usr/bin/env python3
"""List the chunks of a block whose hardware hash differs from the toolkit's (lanesum= jobs, RUN.md "Lanes").

    lanesum_compare.py RESULT.csv --scalar-bin test_spu_exhaustive.scalar [--md OUT.md]

RESULT.csv     a spuexh result file with `LANESUM_BEGIN` / `LANESUM` / `LANESUM_DONE` / `LANESUM_FAULT` rows (FORMAT.md).
--scalar-bin   the host harness' scalar build. For every group (op, domain, block) it runs
               `<bin> --dump <op> <domain> <block> --dump-hash <chunk>`, which prints the sweep's block hash (exh_fold, FNV-1a-64
               over the outputs) restricted to each chunk of the block, and sets those hashes against the hardware's.

Prints per group how many chunks are equal, differ or are missing (a faulted or lost run), the differing chunks with their call
ranges, and the `lanes=` lines that dump the calls of the first differing chunk (lanes_compare.py reads that result).
Exit status: 1 when a hardware chunk has another start or length than the harness' chunk (the two disagree about the chunking:
nothing about the hashes means anything), 0 otherwise (differing hashes are the data), 2 on a usage, file or format error.
"""
import argparse
import subprocess
import sys

from lanes_compare import FormatError

LANES_MAX_COUNT = 1024        # SPUEXH_LANES_MAX_COUNT


class Group:
    def __init__(self, op, dom, blk, chunk=None, nchunks=None):
        self.op, self.dom, self.blk = op, dom, blk
        self.chunk, self.nchunks = chunk, nchunks         # from the LANESUM_BEGIN row (None: a file without one)
        self.rows = []                                      # (start, count, hash)
        self.status = None                                  # "done" | "fault: text" | None (no closing row: lost run)

    @property
    def key(self):
        return (self.op, self.dom, self.blk)


def parse_result(path):
    groups = []
    cur = None
    with open(path, encoding="utf-8") as f:
        for lineno, line in enumerate(f.read().splitlines(), 1):
            p = line.split(",")
            kind = p[0]
            try:
                if kind == "LANESUM_BEGIN":
                    if len(p) != 6:
                        raise FormatError(f"line {lineno}: LANESUM_BEGIN row needs 6 fields")
                    cur = Group(p[1], p[2], int(p[3]), int(p[4]), int(p[5]))
                    groups.append(cur)
                elif kind == "LANESUM":
                    if len(p) != 7:
                        raise FormatError(f"line {lineno}: LANESUM row needs 7 fields")
                    if cur is None or cur.key != (p[1], p[2], int(p[3])):
                        cur = Group(p[1], p[2], int(p[3]))           # a row without its BEGIN row
                        groups.append(cur)
                    if len(p[6]) != 16:
                        raise FormatError(f"line {lineno}: hash is not 16 hex digits: {p[6]!r}")
                    cur.rows.append((int(p[4]), int(p[5]), int(p[6], 16)))
                elif kind in ("LANESUM_DONE", "LANESUM_FAULT") and cur is not None and cur.key == (p[1], p[2], int(p[3])):
                    cur.status = "done" if kind == "LANESUM_DONE" else "fault: " + ",".join(p[5:])
            except ValueError:
                raise FormatError(f"line {lineno}: bad number in {kind} row") from None
    return groups


def run_scalar(binary, group, chunk):
    """[(start, count, hash)] of the chunks of the block from the host harness."""
    argv = [binary, "--dump", group.op, group.dom, str(group.blk), "--dump-hash", str(chunk)]
    try:
        p = subprocess.run(argv, check=False, capture_output=True, text=True, timeout=3600)
    except (OSError, subprocess.SubprocessError) as e:
        raise FormatError(f"cannot run {' '.join(argv)}: {e}") from None
    if p.returncode != 0:
        raise FormatError(f"{' '.join(argv)} exited {p.returncode}: {p.stderr.strip()}")
    out = []
    for line in p.stdout.splitlines():
        f = line.split()
        if len(f) != 3:
            raise FormatError(f"unexpected --dump-hash line from the harness: {line!r}")
        out.append((int(f[0]), int(f[1]), int(f[2], 16)))
    return out


class Result:
    def __init__(self, group):
        self.group = group
        self.model = []
        self.layout_errors = []          # strings: hardware chunks that the model does not have
        self.diff = []                   # (start, count, hw, model)
        self.equal = 0
        self.missing = []                # (start, count) of model chunks without a hardware row


def compare_group(binary, group):
    res = Result(group)
    if group.chunk is None:
        # a file without a BEGIN row: the chunk is the length of the first row
        if not group.rows:
            return res
        group.chunk = group.rows[0][1]
    res.model = run_scalar(binary, group, group.chunk)
    by_start = {s: (n, h) for s, n, h in res.model}
    seen = set()
    for start, count, hw in group.rows:
        m = by_start.get(start)
        if m is None or m[0] != count:
            res.layout_errors.append(f"hardware chunk start {start} count {count}: the harness has "
                                     + (f"count {m[0]} there" if m else "no chunk starting there"))
            continue
        seen.add(start)
        if hw == m[1]:
            res.equal += 1
        else:
            res.diff.append((start, count, hw, m[1]))
    res.missing = [(s, n) for s, n, _h in res.model if s not in seen]
    return res


def suggested_lanes(g, start, count):
    """lanes= lines that dump calls [start, start+count) of the block, at most LANES_MAX_COUNT per entry."""
    out = []
    for s in range(start, start + count, LANES_MAX_COUNT):
        out.append(f"lanes={g.op}:{g.dom}:{g.blk}:{min(LANES_MAX_COUNT, start + count - s)}:{s}")
    return out


def render(results, md):
    L = []
    h2 = "## " if md else "== "
    L.append(("# " if md else "") + "lanesum: hardware chunk hashes against the toolkit's")
    L.append("")
    if md:
        L.append("| op | domain | block | chunk | chunks | equal | differ | missing | layout |")
        L.append("|---|---|---|---|---|---|---|---|---|")
    for r in results:
        g = r.group
        lay = "OK" if not r.layout_errors else f"FAILED ({len(r.layout_errors)})"
        row = (g.op, g.dom, g.blk, g.chunk, len(r.model), r.equal, len(r.diff), len(r.missing), lay)
        L.append("| " + " | ".join(str(x) for x in row) + " |" if md else
                 f"{g.op:<10} {g.dom:<6} block {g.blk:<4} chunk {g.chunk:<8} chunks {len(r.model):<5} equal {r.equal:<5} "
                 f"differ {len(r.diff):<5} missing {len(r.missing):<5} layout {lay}")
    for r in results:
        g = r.group
        L.append("")
        L.append(h2 + f"{g.op} {g.dom} block {g.blk} (chunk {g.chunk})")
        if g.status is None:
            L.append("NOTE: no LANESUM_DONE row: the run was lost or is still going")
        elif g.status != "done":
            L.append("NOTE: LANESUM_" + g.status.upper())
        if g.nchunks is not None and len(g.rows) != g.nchunks:
            L.append(f"NOTE: {len(g.rows)} of {g.nchunks} chunks dumped")
        if r.layout_errors:
            L.append("LAYOUT MISMATCH: the hardware's chunks are not the harness' chunks; first ones:")
            L.extend("  " + e for e in r.layout_errors[:5])
        L.append(f"{len(r.model)} chunks: {r.equal} equal, {len(r.diff)} differ, {len(r.missing)} missing")
        if r.diff:
            L.append("differing chunks (calls start..end-1, hardware hash, model hash):")
            for s, n, hw, m in r.diff:
                L.append(f"  {s}..{s + n - 1}  hw={hw:016x}  model={m:016x}")
            sug = suggested_lanes(g, r.diff[0][0], r.diff[0][1])
            L.append(f"lanes= lines for the first differing chunk ({len(sug)} entries of at most {LANES_MAX_COUNT} calls"
                     + (", the first 16 shown: a configuration holds 16; run a lanesum with a smaller chunk to narrow it down" if len(sug) > 16 else "") + "):")
            L.extend("  " + x for x in sug[:16])
        if r.missing:
            L.append("chunks without a hardware row: " + ", ".join(f"{s}..{s + n - 1}" for s, n in r.missing[:20])
                     + (" ..." if len(r.missing) > 20 else ""))
    return L


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("result")
    ap.add_argument("--scalar-bin", required=True, help="the host harness' scalar build (test_spu_exhaustive.scalar)")
    ap.add_argument("--md", help="also write the report as markdown to this file")
    args = ap.parse_args(argv)
    try:
        groups = parse_result(args.result)
        if not groups:
            raise FormatError(f"{args.result}: no LANESUM rows (was the run configured with lanesum=?)")
        results = [compare_group(args.scalar_bin, g) for g in groups]
    except (FormatError, OSError) as e:
        print(f"lanesum_compare: {e}", file=sys.stderr)
        return 2
    print("\n".join(render(results, False)))
    if args.md:
        try:
            with open(args.md, "w", encoding="utf-8") as f:
                f.write("\n".join(render(results, True)) + "\n")
        except OSError as e:
            print(f"lanesum_compare: {e}", file=sys.stderr)
            return 2
    return 1 if any(r.layout_errors for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
