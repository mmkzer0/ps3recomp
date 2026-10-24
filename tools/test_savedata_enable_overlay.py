#!/usr/bin/env python3
"""cellSaveDataEnableOverlay is a registered cellSysutil export (NID 0xE7FA820B) and returns CELL_OK.

An import with no handler stops the title as unresolved-nid. Two checks:
  1. the NID is the hash of the name, and `gen_hle_nids.py --all` registers that NID to
     cellSaveDataEnableOverlay (the table the runtime dispatches through);
  2. libs/system/tests/test_savedata_enable_overlay.c calls the handler and checks its return.

It needs a host C compiler (under CI=1 a missing one fails instead of skipping).

Usage:
    python3 tools/test_savedata_enable_overlay.py
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
from nid_database import compute_nid  # noqa: E402

SOURCE = os.path.join(ROOT, "libs", "system", "tests", "test_savedata_enable_overlay.c")
NID = 0xE7FA820B
NAME = "cellSaveDataEnableOverlay"


def _cc():
    return (shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
            or shutil.which("gcc"))


def main():
    failed = 0
    ok = compute_nid(NAME) == NID
    print("%s: NID of %s is 0x%08X" % ("ok  " if ok else "FAIL", NAME, NID))
    failed += not ok

    with tempfile.TemporaryDirectory() as td:
        table = os.path.join(td, "ppu_hle_nids.cpp")
        r = subprocess.run([sys.executable, os.path.join(TOOLS, "gen_hle_nids.py"), "--all", "--out", table],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: gen_hle_nids.py --all")
            print((r.stdout + r.stderr)[-2000:])
            return 1
        with open(table, encoding="utf-8", errors="replace") as f:
            text = f.read()
        ok = re.search(r'ps3_hle_register\(0x%08Xu, "%s", &ps3_hle_adapt<&%s>\)' % (NID, NAME, NAME), text) is not None
        print("%s: the registration table binds 0x%08X to %s" % ("ok  " if ok else "FAIL", NID, NAME))
        failed += not ok

        cc = _cc()
        if cc is None:
            if os.environ.get("CI"):
                print("FAIL: no C compiler found")
                return 1
            print("skip test_savedata_enable_overlay.c: no C compiler")
            return 1 if failed else 0
        exe = os.path.join(td, "test_savedata_enable_overlay")
        cmd = [cc, "-std=gnu17", "-w", "-I" + os.path.join(ROOT, "include"), "-o", exe, SOURCE]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        r = subprocess.run([exe], capture_output=True, text=True)
        print("\n".join(l for l in r.stdout.splitlines() if not l.startswith("[cellSaveData]")))
        failed += r.returncode != 0
    print("PASS" if not failed else "FAIL")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
