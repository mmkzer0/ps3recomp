#!/usr/bin/env python3
"""Lift one packaged module with ppu_lifter.py in raw mode, exactly as the
firmware flow does (run_diff.py): the base and TOC come from <name>_exports.json,
the function seeds from <name>_functions.json.

    lift_module.py --meta DIR --name libsyn_c --prefix sync_ --stem lifted --out DIR

Writes <out>/<stem>.c, <out>/<stem>.h and the lifter's report. Exits with the
lifter's status; with --max-traps N it also fails when the report lists more than
N trapped (undecodable or unimplemented) instructions, so that a lifter gap in a
synthetic module is a build error rather than a silent runtime trap.
"""
import argparse, json, os, subprocess, sys

ROOT = os.path.realpath(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--meta", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--prefix", required=True, help="ppu_lifter --symbol-prefix")
    ap.add_argument("--stem", required=True, help="output file stem (<stem>.c, <stem>.h)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-traps", type=int, default=0)
    a = ap.parse_args()
    meta = lambda suffix: os.path.join(a.meta, a.name + suffix)
    ex = json.load(open(meta("_exports.json")))
    os.makedirs(a.out, exist_ok=True)
    cmd = [sys.executable, "-I", os.path.join(ROOT, "tools", "ppu_lifter.py"), meta("_image.bin"),
           "--raw", "--base", ex["base"], "--toc", ex["toc"], "--functions", meta("_functions.json"),
           "--symbol-prefix", a.prefix, "--header-name", a.stem + ".h", "--source-name", a.stem + ".c",
           "--single-file", "--output", a.out]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode:
        return r.returncode
    rep = json.load(open(os.path.join(a.out, "ppu_lift_report.json")))
    traps = rep["summary"]["trapped_instructions"]
    unres = rep["summary"]["unresolved_targets"]
    if traps > a.max_traps or unres:
        print("lift_module: %s: %d trapped instruction(s), %d unresolved target(s)" % (a.name, traps, unres), file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
