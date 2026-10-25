#!/usr/bin/env python3
"""The lifted-code ABI gate: a lift stamped with the ABI this runtime implements
runs; one stamped with another stops by name before the file is read; and a lift
from before the stamp existed does not link at all (never a silent downgrade).

usage: test_lifter_abi.py <probe_match> <probe_mismatch> <source-root>

The probes are the same source plus a stamp; the mismatch one is built with the
stamp set to PPU_RUNTIME_LIFTER_ABI + 1, so it stays a mismatch when the ABI
moves. The link case compiles the runtime's check and the probe without a stamp.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import gates_common as g

MATCH = None
MISMATCH = None
ROOT = None


def runtime_abi():
    """PPU_RUNTIME_LIFTER_ABI as the runtime header defines it."""
    text = (Path(ROOT) / "include" / "ps3emu" / "lifter_abi.h").read_text()
    for line in text.splitlines():
        code = line.split("/*")[0].strip()
        if code.startswith("#define PPU_RUNTIME_LIFTER_ABI"):
            return code.split()[-1].rstrip("uU")
    raise AssertionError("PPU_RUNTIME_LIFTER_ABI not defined in ps3emu/lifter_abi.h")


def test_matching_abi_runs():
    p = g.run([MATCH])
    assert p.returncode == 0, f"exit {p.returncode}\nstderr:\n{p.stderr}"
    assert not g.stop_lines(p.stderr), p.stderr


def test_mismatched_abi_stops_by_name():
    expected = runtime_abi()
    p = g.run([MISMATCH])
    f = g.expect_stop(p, "lifter-abi")
    assert f["expected"] == expected, f
    assert f["got"] != expected, f
    # The banner names both sides so a reader knows which pair is wrong.
    assert re.search(rf"^\s+expected\s+{re.escape(expected)}$", p.stderr, re.M), p.stderr
    assert re.search(rf"^\s+got\s+{re.escape(f['got'])}$", p.stderr, re.M), p.stderr


def _build(compiler, std, src, obj, include):
    r = subprocess.run([compiler, std, "-O0", "-I", str(include), "-c", str(src),
                        "-o", str(obj)], capture_output=True, text=True)
    assert r.returncode == 0, f"compiling {src} failed:\n{r.stderr}"


def test_lift_without_the_stamp_does_not_link():
    # The "sources lifted before the stamp existed" case: the runtime references
    # ppu_lifter_abi and the lift defines nothing, so the link must fail and name
    # the symbol -- a run that silently skipped the check would be the bug.
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    include = Path(ROOT) / "include"
    with tempfile.TemporaryDirectory(prefix="lifter-abi-") as tmp:
        tmp = Path(tmp)
        objs = []
        for src, compiler, std, name in [
            (Path(ROOT) / "runtime" / "diag" / "lifter_abi.c", cc, "-std=c11", "lifter_abi.o"),
            (Path(ROOT) / "runtime" / "diag" / "stop.cpp", cxx, "-std=c++17", "stop.o"),
            (Path(__file__).with_name("probe_lifter_abi.c"), cc, "-std=c11", "probe.o"),
        ]:
            obj = tmp / name
            _build(compiler, std, src, obj, include)
            objs.append(obj)
        r = subprocess.run([cxx, "-pthread", *map(str, objs), "-o", str(tmp / "probe")],
                           capture_output=True, text=True)
        assert r.returncode != 0, "a lift without the ABI stamp linked; the gate can be skipped silently"
        assert "ppu_lifter_abi" in r.stderr, r.stderr


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("usage: test_lifter_abi.py <probe_match> <probe_mismatch> <source-root>",
              file=sys.stderr)
        sys.exit(2)
    MATCH, MISMATCH, ROOT = sys.argv[1], sys.argv[2], sys.argv[3]
    sys.exit(g.main([(n, f) for n, f in sorted(globals().items()) if n.startswith("test_")]))
