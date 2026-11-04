#!/usr/bin/env python3
"""ctest driver of the lv2 conformance suite: run the cases on the HLE, then compare with the golden.

usage: run_lv2conf.py <test_lv2conf> <host.csv> [--golden FILE --divergences FILE]

test_lv2conf runs every case and writes <host.csv>. With a golden (the hardware result) the only
comparator is tools/hwprobe/probes/lv2conf/compare_lv2conf.py, whose exit status is this test's. Without
one, the run itself is checked: the file must parse, and every registered case must have rows or a SKIP.
A case the HLE stops or hangs on is a result (a `stop` or `hang` row), not a failure.
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
COMPARE_DIR = os.path.join(ROOT, "tools", "hwprobe", "probes", "lv2conf")
sys.path.insert(0, COMPARE_DIR)

import compare_lv2conf as cmp


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("runner")
    ap.add_argument("host_csv")
    ap.add_argument("--golden")
    ap.add_argument("--divergences")
    args = ap.parse_args()
    if bool(args.golden) != bool(args.divergences):
        ap.error("--golden and --divergences go together")

    run = subprocess.run([args.runner, "--out", args.host_csv], check=False, capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    sys.stderr.write(run.stderr)
    if run.returncode != 0:
        print(f"run_lv2conf: {args.runner} exited {run.returncode}", file=sys.stderr)
        return 1

    if args.golden:
        return cmp.main([args.golden, args.host_csv, "--divergences", args.divergences])

    try:
        host = cmp.parse_result(args.host_csv)
    except ValueError as e:
        print(f"run_lv2conf: {e}", file=sys.stderr)
        return 1
    total = int(host.meta.get("cases_total", "-1"))
    empty = [cid for cid, c in host.cases.items() if c.skipped is None and not c.rows and not c.times]
    covered = len(host.cases) - len(empty)
    if empty or covered != total:
        print(f"run_lv2conf: {covered} of {total} registered cases have rows or a SKIP; empty: {empty}", file=sys.stderr)
        return 1
    print(f"run_lv2conf: no golden file yet; all {total} cases ran or were skipped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
