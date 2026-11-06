#!/usr/bin/env python3
"""Exports the runtime must not answer stay out of the generated HLE NID table.

An HLE handler that answers a title compat value without doing the work is success for work that was not done.
The runtime's remedy for an import it cannot honour is no handler at all: the import stops at the unresolved
NID (kind=unresolved-nid), named. These functions once existed as such answers and are gone; the check
generates the table the way the build does (tools/gen_hle_nids.py --all) and requires that none of them
is registered again, while the exports next to them still are (so an empty table cannot pass).

Usage: check_hle_unregistered.py
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# name -> why it is not registered
ABSENT = {
    "cellSysutilNpEulaCheck": "returned 1 ('accepted') without consulting its callback or any EULA state",
}
PRESENT = ("sceNpEulaCheckEulaStatus", "sceNpEulaAbort")


def main():
    with tempfile.TemporaryDirectory(prefix="hle-unregistered-") as tmp:
        out = os.path.join(tmp, "ppu_hle_nids.cpp")
        run = subprocess.run(
            [sys.executable, os.path.join(ROOT, "tools", "gen_hle_nids.py"), "--out", out, "--all"],
            capture_output=True, text=True, check=False)
        if run.returncode != 0 or not os.path.exists(out):
            print(f"FAIL: gen_hle_nids.py --all exited {run.returncode}\n{run.stderr}")
            return 1
        with open(out, encoding="utf-8") as table:
            text = table.read()
    failures = 0
    for name in PRESENT:
        if f'"{name}"' not in text:
            print(f"FAIL: {name} is not registered; the table check is vacuous")
            failures += 1
    for name, why in ABSENT.items():
        if f'"{name}"' in text:
            print(f"FAIL: {name} is registered again ({why})")
            failures += 1
    if failures:
        return 1
    print(f"OK: {len(ABSENT)} removed exports are absent from the generated table")
    return 0


if __name__ == "__main__":
    sys.exit(main())
