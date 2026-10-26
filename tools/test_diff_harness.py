#!/usr/bin/env python3
"""Path-hygiene and skip contract of the differential-harness tools.

Firmware material and generated tables stay outside the repository. These are
the checks the tools perform themselves:

  * run_diff.py refuses --fw-dir / build / out inside the repository after
    realpath (a symlink pointing into it is caught too);
  * gen_tables.py refuses firmware inputs and output inside the repository,
    and still allows the checked-in synthetic fixture (--synthetic);
  * skip.py prints the visible `SKIP <module>: <reason>` line and exits 77.

The CMake configure-time refusal is exercised by a real configure in the
harness build (see docs/DIFF_HARNESS.md); spawning a configure is too heavy for
this test and needs the platform's SDL2.
"""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.realpath(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(ROOT, "tools", "diff_harness")


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def main():
    failures = []

    def expect(cond, msg):
        print(("ok   " if cond else "FAIL ") + msg)
        if not cond:
            failures.append(msg)

    # 1. a firmware directory inside the repository
    rc, out = run([sys.executable, os.path.join(TOOLS, "run_diff.py"), "--fw-dir", os.path.join(ROOT, "tests")])
    expect(rc != 0 and "refusing" in out, "run_diff refuses an in-repo --fw-dir")
    expect(rc != 0 and out.count("refusing"), "run_diff prints the reason on stderr")

    # 2. a symlink that resolves into the repository
    with tempfile.TemporaryDirectory() as td:
        link = os.path.join(td, "fw-ish")
        os.symlink(os.path.join(ROOT, "tests"), link)
        rc, out = run([sys.executable, os.path.join(TOOLS, "run_diff.py"), "--fw-dir", link])
        expect(rc != 0 and "refusing" in out, "run_diff resolves symlinks before the refusal")

    # 3. gen_tables: in-repo output with outside inputs is refused
    with tempfile.TemporaryDirectory() as td:
        img = os.path.join(td, "img.bin")
        open(img, "wb").write(b"\0" * 64)
        ex = os.path.join(td, "ex.json")
        im = os.path.join(td, "im.json")
        json.dump({"base": "0x02000000", "toc": "0x02000000", "exports": {}}, open(ex, "w"))
        json.dump({}, open(im, "w"))
        rc, out = run([sys.executable, os.path.join(TOOLS, "gen_tables.py"),
                       "--name", "x", "--prefix", "x_", "--header", "x.h",
                       "--exports", ex, "--imports", im, "--image", img,
                       "--out", os.path.join(ROOT, "build", "dh-hygiene-should-not-exist")])
        expect(rc != 0 and "refusing" in out, "gen_tables refuses an in-repo output")
        expect(not os.path.exists(os.path.join(ROOT, "build", "dh-hygiene-should-not-exist")),
               "gen_tables did not create the refused directory")

        # 4. an in-repo firmware input is refused too
        rc, out = run([sys.executable, os.path.join(TOOLS, "gen_tables.py"),
                       "--name", "x", "--prefix", "x_", "--header", "x.h",
                       "--exports", ex, "--imports", im, "--image", os.path.join(ROOT, "CMakeLists.txt"),
                       "--out", os.path.join(td, "out")])
        expect(rc != 0 and "refusing firmware-derived input" in out, "gen_tables refuses an in-repo input")

        # 5. the synthetic fixture is the explicit exception, and still writes
        #    its tables (no firmware involved)
        outdir = os.path.join(td, "synth-out")
        rc, out = run([sys.executable, os.path.join(TOOLS, "gen_tables.py"), "--synthetic",
                       "--name", "synth", "--prefix", "synth_", "--header", "synth.h",
                       "--exports", ex, "--imports", im, "--image", img, "--out", outdir])
        expect(rc == 0 and os.path.exists(os.path.join(outdir, "synth_harness_tables.cpp")),
               "gen_tables --synthetic accepts the checked-in fixture")

    # 6. the visible skip line
    rc, out = run([sys.executable, os.path.join(TOOLS, "skip.py"), "libsync2", "missing metadata"])
    expect(rc == 77 and "SKIP libsync2: missing metadata" in out, "skip.py prints SKIP and exits 77")

    if failures:
        print(f"{len(failures)} failure(s)", file=sys.stderr)
        return 1
    print("test_diff_harness: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
