#!/usr/bin/env python3
"""The lifted-code ABI gate: a lift stamped with the ABI this runtime implements
runs; one stamped with another stops by name before the file is read; and a lift
from before the stamp existed does not link at all (never a silent downgrade).

usage: test_lifter_abi.py <probe_match> <probe_mismatch> <source-root>

The probes are the same source plus a stamp; the mismatch one is built with the
stamp set to PPU_RUNTIME_LIFTER_ABI + 1, so it stays a mismatch when the ABI
moves. The link case compiles the runtime's check and the probe without a stamp,
with the compilers CTest passes (CC, CXX), spelled for their driver.
"""
import re
import sys
import tempfile
from pathlib import Path

import gates_common as g

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from host_cc import EXE, c_compiler, cxx_compiler  # noqa: E402

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
    assert f["processor"] == "ppu", f
    assert f["got"] != expected, f
    # The banner names both sides so a reader knows which pair is wrong.
    assert re.search(rf"^\s+expected\s+{re.escape(expected)}$", p.stderr, re.M), p.stderr
    assert re.search(rf"^\s+got\s+{re.escape(f['got'])}$", p.stderr, re.M), p.stderr


def test_lift_without_the_stamp_does_not_link():
    # The "sources lifted before the stamp existed" case: the runtime references
    # ppu_lifter_abi and the lift defines nothing, so the link must fail and name
    # the symbol -- a run that silently skipped the check would be the bug.
    cc, cxx = c_compiler(), cxx_compiler()
    assert cc and cxx, "no C or C++ compiler (CC/CXX unset and none on PATH)"
    include = Path(ROOT) / "include"
    with tempfile.TemporaryDirectory(prefix="lifter-abi-") as tmp:
        tmp = Path(tmp)
        objs = []
        for src, compiler, std, name in [
            (Path(ROOT) / "runtime" / "diag" / "lifter_abi.c", cc, "c11", "lifter_abi.o"),
            (Path(ROOT) / "runtime" / "diag" / "stop.cpp", cxx, "c++17", "stop.o"),
            (Path(__file__).with_name("probe_lifter_abi.c"), cc, "c11", "probe.o"),
        ]:
            obj = tmp / name
            r = compiler.run(compiler.obj(src, obj, std=std, includes=[include]))
            assert r.returncode == 0, f"compiling {src} failed:\n{r.stdout}{r.stderr}"
            objs.append(obj)
        r = cxx.run(cxx.exe(objs, tmp / ("probe" + EXE), flags=[] if cxx.msvc else ["-pthread"]))
        out = r.stdout + r.stderr          # link.exe reports on stdout
        assert r.returncode != 0, "a lift without the ABI stamp linked; the gate can be skipped silently"
        assert "ppu_lifter_abi" in out, out


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("usage: test_lifter_abi.py <probe_match> <probe_mismatch> <source-root>",
              file=sys.stderr)
        sys.exit(2)
    MATCH, MISMATCH, ROOT = sys.argv[1], sys.argv[2], sys.argv[3]
    sys.exit(g.main([(n, f) for n, f in sorted(globals().items()) if n.startswith("test_")]))
