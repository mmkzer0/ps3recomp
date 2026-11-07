#!/usr/bin/env python3
"""Console run time ESTIMATE of spuexh configurations.

    estimate_runtime.py <dir with per-domain dry-run files (dryrun.sh smoke)> <spuexh_host> [cfg-or-preset ...]

The interpreter's instruction count for one job of each domain (block 0 of the smoke configuration, the file's
META,interp_steps) times the number of jobs of the configuration (spuexh_host --plan) gives the SPU instructions of the sweep.
Sweep time is estimated from the interpreter's instruction count as before. Probe B uses a separate failure-path floor: each
non-returning word is polled for max(2 s, b_timeout_ms) before terminate; normal returns and launch overhead are not measured.
"""
import glob
import os
import re
import subprocess
import sys
from pathlib import Path

import gen_tables

ROOT = Path(__file__).resolve().parents[4]
B_BATCHES = (
    "specialb_first.cfg",
    "specialb_4.cfg",
    "specialb_16.cfg",
    "specialb_4_both.cfg",
    "specialb_16_both.cfg",
)
EXIT_POLL_FLOOR_S = 2


def special_b_estimates():
    decode = gen_tables.load_decode(str(ROOT))[1]
    total = sum(op == "word" for op in decode)
    print()
    print("Probe B failure-path floor (every word needs termination; launch overhead excluded):")
    print("| config | prefixes | patterns | word attempts | per-word poll floor | batch floor |")
    print("|---|---:|---|---:|---:|---:|")
    for filename in B_BATCHES:
        values = {}
        with open(os.path.join(os.path.dirname(__file__), "cfg", filename), encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    key, value = line.split("=", 1)
                    values[key] = value
        start = _decimal_cfg(values, "special_b_from", 0)
        count = _decimal_cfg(values, "special_b_count", 0)
        maximum = _decimal_cfg(values, "special_b_max", 0)
        timeout_ms = _decimal_cfg(values, "b_timeout_ms", 600)
        limit = min(maximum, total) if maximum else total
        if start >= limit:
            raise ValueError(f"{filename}: special_b_from must be below its prefix total")
        prefixes = min(count, limit - start) if count else limit - start
        patterns = values.get("special_b_patterns", "both")
        if patterns not in ("zero", "both"):
            raise ValueError(f"{filename}: special_b_patterns must be zero or both")
        attempts = prefixes * (2 if patterns == "both" else 1)
        poll_floor = max(EXIT_POLL_FLOOR_S, timeout_ms / 1000)
        print(f"| `{filename}` | {prefixes} | {patterns} | {attempts} | {poll_floor:g} s | "
              f"{_format_seconds(attempts * poll_floor)} |")

    attempts = total * 2
    print(f"| full B | {total} | both | {attempts:,} | {EXIT_POLL_FLOOR_S} s | "
          f"{_format_seconds(attempts * EXIT_POLL_FLOOR_S)} |")


def _decimal_cfg(values, key, default):
    value = values.get(key, str(default))
    if not re.fullmatch(r"[0-9]+", value):
        raise ValueError(f"{key} must be decimal-only, got {value!r}")
    return int(value)


def _format_seconds(seconds):
    return f"{seconds:.0f} s" if seconds < 120 else f"{seconds / 60:.1f} min"

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
    special_b_estimates()


if __name__ == "__main__":
    main(sys.argv)
