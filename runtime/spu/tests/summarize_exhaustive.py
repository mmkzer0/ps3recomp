#!/usr/bin/env python3
"""Build the op x domain x backend coverage/result table (markdown) from the hash files.
  summarize_exhaustive.py HASHDIR [--ref scalar] [--out table.md]
Cell = blocks equal to the reference / blocks both builds ran ("OK 512/512"); "-" = variant did not run that op/domain."""
import argparse
import collections
import glob
import os
import sys

# domain -> (kind, lanes per call, description)
DOMS = {
 "byte2": ("exhaustive", 16, "all 2^16 (a,b) byte pairs"), "half2": ("exhaustive", 8, "all 2^32 (a,b) halfword pairs"),
 "wcnt": ("exhaustive count x random data", 4, "every 32-bit count value once x seeded random data"),
 "qcnt": ("exhaustive low16 x 256 high16", 1, "count word: low 16 bits all, high 16 bits 256 patterns"), "qcnta": ("exhaustive low16 x 256 high16", 1, "same, count in operand a"),
 "h16ll": ("exhaustive", 4, "all 2^32 (a16,b16) for low halves"), "h16hh": ("exhaustive", 4, "all 2^32 (a16,b16) for high halves"), "h16hl": ("exhaustive", 4, "a-high x b-low, all 2^32"),
 "wpool2": ("structured exhaustive", 4, "all 1024x1024 pairs of edge words"), "wrand": ("sampled", 4, "seeded random words (mixed density)"),
 "w32": ("exhaustive", 4, "all 2^32 input words"), "w32e": ("exhaustive", 2, "all 2^32 words in words 0,2"), "w32o": ("exhaustive", 2, "all 2^32 words in words 1,3"),
 "bits16": ("exhaustive", 1, "all 2^16 LSB patterns of the 16 bytes"),
 "immb": ("exhaustive", 16, "2176 imm values x all 256 byte values"), "immh": ("exhaustive", 8, "2176 imm values x all 65536 halfwords"),
 "immw": ("imm exhaustive x sampled data", 4, "2176 imm values x 256 random calls"),
 "fsmbi": ("exhaustive", 1, "all 2^16 immediates"), "shufb_pos": ("exhaustive control x sampled data", 1, "256 control values x 16 positions x 256 random fills"),
 "shufb_rand": ("sampled", 1, "random control/data"), "shufb_all": ("exhaustive control", 1, "every value at every position (v + P*delta)"),
 "fpool2": ("structured exhaustive", 4, "all pairs of 4096 values (256 exps x 8 fractions x 2 signs)"), "frand2": ("sampled", 4, "uniform random 32-bit pairs"),
 "fnear2": ("sampled structured", 4, "exponents within +-40, equal/neg/adjacent, extremes"), "fovf2": ("sampled structured", 4, "product exponent near 0 and 255"),
 "fpool3": ("structured exhaustive", 4, "128 x 128 x 1024 (key exps, product exp +-32)"), "frand3": ("sampled", 4, "uniform random triples"), "fnear3": ("sampled structured", 4, "addend exponent within +-40 of product"),
 "fi": ("sampled structured", 4, "random estimate words with edge exp/base/step, random Y"),
 "dpool2": ("structured exhaustive", 2, "all pairs of 1024 doubles (128 exps x 4 fractions x 2 signs)"), "drand2": ("sampled", 2, "uniform random 64-bit pairs"),
 "dnear2": ("sampled structured", 2, "exponents within +-70, equal/neg/adjacent, extremes"), "dpool3": ("structured exhaustive", 2, "256^3 triples (32 exps x 4 fractions x 2 signs)"),
 "drand3": ("sampled", 2, "uniform random triples"), "dnear3": ("sampled structured", 2, "addend exponent within +-70 of product"),
 "dfrds": ("structured exhaustive", 2, "all 2048 exps x sign x 8 tie patterns x 32 high fractions"),
}
def load(p):
    h, cov = {}, {}
    with open(p) as fh:
        lines = fh.readlines()
    for ln in lines:
        if ln.startswith("#cov"):
            f = ln.rstrip("\n").split("\t"); cov[(f[1], f[2])] = f[3:]
        elif not ln.startswith("#") and ln.strip():
            op, dom, blk, hx = ln.rstrip("\n").split("\t"); h[(op, dom, int(blk))] = hx
    return h, cov
def main():
    ap = argparse.ArgumentParser(); ap.add_argument("dir"); ap.add_argument("--ref", default="scalar"); ap.add_argument("--out")
    a = ap.parse_args()
    files = {os.path.basename(f)[:-4]: f for f in glob.glob(os.path.join(a.dir, "*.txt")) if ".shard" not in f and ".compare" not in f}
    ref, refcov = load(files[a.ref]); order = [v for v in ("sse2", "sse41", "avx2", "avx512", "neon", "neon+rz", "scalar-gcc", "sse41-gcc", "avx2-gcc", "avx512-gcc") if v in files]
    data = {v: load(files[v]) for v in order}
    cells = collections.defaultdict(dict); selfc = collections.defaultdict(int)
    for v, (h, cov) in data.items():
        cnt = collections.defaultdict(lambda: [0, 0])
        for k, x in h.items():
            if k in ref: c = cnt[(k[0], k[1])]; c[1] += 1; c[0] += (ref[k] == x)
        for k, (ok, n) in cnt.items(): cells[k][v] = f"OK {ok}/{n}" if ok == n else f"**FAIL {n - ok}/{n}**"
        for k, f in cov.items(): selfc[(k[0], k[1], v)] = int(f[4])   # f = [blocks run, total, stride, seconds, fast != _ref calls, first]
    rows = []
    for (op, dom), f in sorted(refcov.items()):
        run, nb, stride = int(f[0]), int(f[1]), int(f[2]); kind, lanes = DOMS.get(dom, ("?", 4, ""))[:2]
        inputs = run * (1 << 20) * lanes
        full = (run == nb)
        cov = kind if full else f"sampled (every {stride}-th block, pseudo-random subset)"
        sc = [selfc.get((op, dom, v), 0) for v in order if v != "scalar"]
        rows.append((op, dom, cov, run, nb, inputs, [cells[(op, dom)].get(v, "-") for v in order], sum(sc)))
    out = ["| op | domain | coverage | blocks run/total | lane-inputs | " + " | ".join(order) + " | fast!=ref |", "|" + "---|" * (6 + len(order))]
    for op, dom, cov, run, nb, inputs, cl, sc in rows:
        out.append(f"| {op} | {dom} | {cov} | {run}/{nb} | {inputs:.3g} | {' | '.join(cl)} | {sc} |")
    txt = "\n".join(out)
    if a.out:
        with open(a.out, "w") as fh:
            fh.write(txt + "\n")
    else: print(txt)
    bad = sum(1 for r in rows for c in r[6] if "FAIL" in c)
    print(f"rows={len(rows)} failing cells={bad}", file=sys.stderr)
if __name__ == "__main__": main()
