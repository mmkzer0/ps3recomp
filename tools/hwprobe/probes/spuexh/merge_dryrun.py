#!/usr/bin/env python3
"""Merge per-domain result files of spuexh_host (dryrun.sh) into one result file: header of the first, all hash rows,
the sum of META,interp_steps, and the other META/FAULT/SKIP rows once. Usage: merge_dryrun.py <dir-with-txt> <out>"""
import glob
import os
import sys


def main(d, out):
    files = sorted(glob.glob(os.path.join(d, "*.txt")))
    if not files:
        sys.exit("no files in " + d)
    header, hashes, other, steps, seen = None, [], [], 0, set()
    for f in files:
        with open(f, encoding="utf-8") as fh:
            lines = fh.read().splitlines()
        for ln in lines:
            if header is None and ln.startswith("HWPROBE"):
                header = []
            if "\t" in ln and not ln.startswith("#"):
                hashes.append(ln)
            elif ln.startswith("META,interp_steps,"):
                steps += int(ln.split(",")[2])
            elif header is not None and len(header) < 11 and (ln.startswith(("HWPROBE", "HDR,"))):
                header.append(ln)
            elif ln.startswith(("FAULT", "RETRY", "FATAL", "TIMING_OVERFLOW", "SKIP", "SPEC", "#hwdom", "#gencheck")) and ln not in seen:
                seen.add(ln)
                other.append(ln)
    with open(out, "w", encoding="utf-8") as o:
        o.write("\n".join(header) + f"\nMETA,backend,host-interp\nMETA,merged_files,{len(files)}\nMETA,interp_steps,{steps}\n")
        o.write("\n".join(other) + ("\n" if other else ""))
        o.write("\n".join(hashes) + "\n")
    print(f"merge_dryrun: {len(files)} files, {len(hashes)} hash rows, {steps} interpreter steps -> {out}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
