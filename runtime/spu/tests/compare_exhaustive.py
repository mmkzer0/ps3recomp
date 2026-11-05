#!/usr/bin/env python3
"""Compare two test_spu_exhaustive hash files and localise every mismatch.

  compare_exhaustive.py REF.txt OTHER.txt [--bindir DIR] [--ref-variant scalar] [--variant neon]
                        [--max-report N] [--no-localise]

Only blocks present in BOTH files are compared (a build may run a strided subset). For every differing
(op, domain) the FIRST differing block is re-run in both builds with `--dump` and the first differing call
is printed: all operands, immediate, and both outputs, plus which lanes differ. Exit status 1 if any block
differs or the build under test reports a fast path that differs from its own _ref (SELFCHECK).
Variants map to commands in RUNNERS (aarch64 builds run under qemu-aarch64).
"""
import argparse
import collections
import os
import subprocess
import sys

SYSROOT = "/usr/aarch64-linux-gnu"
def runner(variant, bindir, extra=()):
    exe = os.path.join(bindir, "test_spu_exhaustive." + variant.split("+")[0])
    cmd = ["qemu-aarch64", "-L", SYSROOT, exe] if variant.startswith("neon") else [exe]
    if variant.endswith("+rz"): cmd += ["--fpcr-rz"]
    return cmd + list(extra)

def load(path):
    h, cov = {}, {}
    with open(path) as fh:
        lines = fh.readlines()
    for ln in lines:
        if ln.startswith("#cov"):
            f = ln.rstrip("\n").split("\t"); cov[(f[1], f[2])] = f[3:]
        elif not ln.startswith("#") and ln.strip():
            op, dom, blk, hx = ln.rstrip("\n").split("\t"); h[(op, dom, int(blk))] = hx
    return h, cov

def dump(variant, bindir, op, dom, blk):
    p = subprocess.run(runner(variant, bindir, ["--dump", op, dom, str(blk)]), capture_output=True, text=True, check=False)
    return p.stdout.splitlines()

def words(x): return [x[i:i + 8] for i in range(0, 32, 8)]

def localise(op, dom, blk, vref, voth, bindir):
    a, b = dump(vref, bindir, op, dom, blk), dump(voth, bindir, op, dom, blk)
    nd, first = 0, None
    for la, lb in zip(a, b):
        fa, fb = la.split(), lb.split()
        if fa[5] != fb[5]:          # output of the build under test (field 5); the reference build's own output is fa[5]
            nd += 1
            if first is None: first = (fa, fb)
    if first is None:
        return "  (block hash differs but no call differs in re-run: non-deterministic or environment-dependent)"
    fa, fb = first
    lanes = [k for k in range(4) if words(fa[5])[k] != words(fb[5])[k]]
    out = [f"  {nd} differing call(s) in block (first shown)",
           f"  call {fa[0]} imm={fa[4]}",
           "  a   = {}".format(" ".join(words(fa[1]))), "  b   = {}".format(" ".join(words(fa[2]))), "  c   = {}".format(" ".join(words(fa[3]))),
           f"  {vref:<8} out = {' '.join(words(fa[5]))}   (ref-build _ref: {' '.join(words(fa[6]))})",
           f"  {voth:<8} out = {' '.join(words(fb[5]))}   (own _ref:       {' '.join(words(fb[6]))})",
           f"  differing word lanes: {lanes}"]
    return "\n".join(out)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref"); ap.add_argument("other")
    ap.add_argument("--bindir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "build_exh"))
    ap.add_argument("--ref-variant", default="scalar"); ap.add_argument("--variant", default=None)
    ap.add_argument("--max-report", type=int, default=60); ap.add_argument("--no-localise", action="store_true")
    a = ap.parse_args()
    variant = a.variant or os.path.basename(a.other).split(".")[0]
    hr = load(a.ref)[0]; ho, co = load(a.other)
    common = [k for k in ho if k in hr]
    bad = [k for k in common if hr[k] != ho[k]]
    missing = sorted({(k[0], k[1]) for k in ho if k not in hr})
    print(f"{variant} vs {a.ref_variant}: {len(common)} blocks compared, {len(bad)} differ "
          f"({len({(k[0], k[1]) for k in ho})} op/domain pairs in other, {len(missing)} with no block in ref)")
    selfcheck = 0
    for k, v in sorted(co.items()):
        # v = [blocks run, blocks total, stride, seconds, calls with fast != _ref, first such call]
        if int(v[4]) > 0: selfcheck += 1; print(f"  SELFCHECK fast!=_ref: {k[0]}/{k[1]}  {v[4]} call(s), first call {v[5]}")
    per = collections.OrderedDict()
    for k in sorted(bad): per.setdefault((k[0], k[1]), []).append(k[2])
    n = 0
    for (op, dom), blks in per.items():
        print(f"MISMATCH {op}/{dom}: {len(blks)} differing block(s), first {blks[0]}")
        if not a.no_localise and n < a.max_report:
            print(localise(op, dom, blks[0], a.ref_variant, variant, a.bindir)); n += 1
    return 1 if bad or selfcheck else 0

if __name__ == "__main__":
    sys.exit(main())
