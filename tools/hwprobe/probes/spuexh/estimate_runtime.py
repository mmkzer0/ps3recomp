#!/usr/bin/env python3
"""Console run time ESTIMATE of spuexh configurations.

    estimate_runtime.py <dir with per-domain dry-run files (dryrun.sh smoke)> <spuexh_host> [cfg-or-preset ...]

The interpreter's instruction count for one job of each domain (block 0 of the smoke configuration, the file's
META,interp_steps) times the number of jobs of the configuration (spuexh_host --plan) gives the SPU instructions of the sweep.
Time = instructions / (SPUs x 3.2 GHz x IPC) for an assumed IPC of 1.0 (optimistic) and 0.5 (the multiply chains of the
64-bit hash and the unhinted branches of the op stub stall the in-order SPU). It is not a measurement: no console has run it.
"""
import glob
import os
import re
import subprocess
import sys


def main(argv):
    d, host = argv[1], argv[2]
    cfgs = argv[3:] or ["smoke", "standard", "full"]
    steps = {}
    for f in glob.glob(os.path.join(d, "*.txt")):
        with open(f) as fh:
            t = fh.read()
        m = re.search(r"META,interp_steps,(\d+)", t)
        if m:
            steps[os.path.basename(f)[:-4]] = int(m.group(1))
    print("| config | jobs | hashes | op applications | SPU instructions | 6 SPUs, IPC 1.0 | 6 SPUs, IPC 0.5 |")
    print("|---|---|---|---|---|---|---|")
    for c in cfgs:
        sel = ["--cfg", c] if os.path.isfile(c) else ["--preset", c]
        out = subprocess.run([host] + sel + ["--plan"], capture_output=True, text=True, check=True).stdout.splitlines()
        head = re.match(r"plan preset=\S+ jobs=(\d+) hashes=(\d+) calls=(\d+)", out[0])
        tot, missing = 0, []
        for l in out[1:]:
            m = re.match(r"dom (\w+) jobs=(\d+)", l)
            if not m:
                continue
            if m.group(1) in steps:
                tot += int(m.group(2)) * steps[m.group(1)]
            else:
                missing.append(m.group(1))
        fmt = lambda s: f"{s:.0f} s" if s < 120 else f"{s / 60:.1f} min"
        print(f"| {os.path.basename(c)} | {head.group(1)} | {head.group(2)} | {int(head.group(3)):.2g} | {tot:.2g} | {fmt(tot / (6 * 3.2e9 * 1.0))} | {fmt(tot / (6 * 3.2e9 * 0.5))} |")
        if missing:
            print("<!-- no measurement for domains: {} -->".format(",".join(missing)))


if __name__ == "__main__":
    main(sys.argv)
