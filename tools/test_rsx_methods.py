#!/usr/bin/env python3
"""NV0039 (M2MF) copies, the walker's method verdicts, and the frame-dump list limit.

usage: test_rsx_methods.py [<test_gcm_nv0039> [<test_metal_frame_dump_list>]]

test_gcm_nv0039 (libs/video/tests/test_gcm_nv0039.c) runs the real FIFO walker over synthetic rings and checks
the destination bytes itself; this test runs its modes, checks the capture it leaves (the copy's source span is
a consumed read, the destination a write, an unknown method makes the frame invalid) and that `validate` and
`rsx-compare` refuse a frame with an unknown method. Run bare (the CI tool-suite job runs every
tools/test_*.py without arguments), only the validator checks run, on captures built here; the fixture parts are
skipped by name, never passed.
"""
import hashlib
import json
import re
import shutil
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from ps3diff.cli import main as cli_main  # noqa: E402
from ps3diff.rsx import validate_rsx  # noqa: E402
from ps3diff.schema import Ps3DiffError, canonical_json, read_ndjson  # noqa: E402
from test_rsx_capture import Builder  # noqa: E402

FAILS = 0


def check(ok, what):
    global FAILS
    print(("[PASS] " if ok else "[FAIL] ") + what)
    FAILS += not ok


def run(binary, *args):
    return subprocess.run([binary, *map(str, args)], capture_output=True, text=True, timeout=60)


def stop_of(proc):
    for line in reversed(proc.stderr.splitlines()):
        if line.startswith("PS3STOP v=1"):
            return dict(re.findall(r'(\w+)=("(?:[^"\\]|\\.)*"|\S+)', line))
    return None


def validation_error(path):
    try:
        validate_rsx(str(path))
    except Ps3DiffError as exc:
        return exc.message
    return None


def cli(*args):
    import contextlib
    import io
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = cli_main(list(args))
    return status, out.getvalue() + err.getvalue()


def events(d):
    return [rec for _, _, rec in read_ndjson(str(Path(d) / "capture.ndjson"))]


class FrameBuilder(Builder):
    """A capture with one frame whose unknown-method count is chosen."""

    def frame(self, unknown):
        h = lambda scope: {"algo": "sha256", "scope": scope, "value": "0" * 64}
        invalid = ["unknown-methods"] if unknown else []
        self.add("frame-complete", _input=False,
                 frame={"flip_id": 0, "guest_frame": 0, "display_buffer": 1, "present_seq": 0},
                 capture_hash={"algo": "sha256", "scope": "input-events/v1", "value": self.running.hexdigest()},
                 state_hash=h("frame-state/v1"), frame_hash=h("raw-readback/v1"),
                 readback={"format": "rgba8-unorm", "channel_order": "rgba", "row_pitch": 4, "alpha": "kept",
                           "colorspace": "linear", "clamp": "none", "width": 1, "height": 1},
                 draws={"submitted": 0, "executed": 0, "dropped": {}, "fixed_function": 0},
                 unknown_methods=unknown, resyncs=0, valid=not invalid, invalid=invalid)

    def finish_with_frame(self):
        self.add("capture-end", _input=False, frames=1, events=self.seq,
                 capture_hash={"algo": "sha256", "scope": "input-events/v1", "value": self.running.hexdigest()})
        (self.root / "capture.ndjson").write_text("\n".join(self.lines) + "\n")
        return self.root / "capture.ndjson"


def synth_frame(tmp, name, unknown):
    b = FrameBuilder(Path(tmp) / name)
    b.gcm_init()
    b.method(0x1000, 0x0100, [1])
    b.frame(unknown)
    return b.finish_with_frame()


def test_validator(tmp):
    ok = synth_frame(tmp, "frame-ok", 0)
    check(validation_error(ok) is None, f"a capture whose frame consumed no unknown method validates ({validation_error(ok)})")
    bad = synth_frame(tmp, "frame-unknown", 3)
    err = validation_error(bad)
    check(err is not None and "unknown_methods" in err and "3" in err,
          f"`validate` refuses a frame with unknown methods and names the count ({err})")
    status, text = cli("validate", str(bad))
    check(status != 0, f"the validate command exits non-zero on it ({status})")
    status, text = cli("rsx-compare", str(bad), str(ok))
    check(status not in (0, 1) and "unknown_methods" in text,
          f"`rsx-compare` is an error, not a pass or a divergence, when a side has an unknown method ({status})")
    status, text = cli("rsx-compare", str(ok), str(ok))
    check(status == 0, f"...while the same comparison of two clean captures passes ({status})")


def pat(n, seed):
    return bytes((seed + i * 7 + (i >> 3)) & 0xFF for i in range(n))


def test_fixture(binary, tmp):
    p = run(binary, "stops")
    n = p.stdout.count("a named stop")
    check(p.returncode == 0 and n == 9 and "[FAIL]" not in p.stdout,
          f"every way a copy must stop (bad increment, bad DMA object, wrapping spans) is a named stop with the destination untouched ({n} of 9)\n{p.stdout[-600:]}")

    p = run(binary, "run")
    check(p.returncode == 0 and "[FAIL]" not in p.stdout, f"copies and verdicts, no capture ({p.returncode})\n{p.stdout[-600:]}")
    check("destination holds the source" in p.stdout and "the source was read before it was written" in p.stdout,
          "the copy scenarios ran")

    d = Path(tmp) / "run"
    p = run(binary, "run", d)
    check(p.returncode == 0, f"the same under a capture ({p.returncode})\n{p.stdout[-600:]}{p.stderr[-300:]}")
    ev = events(d)
    copies = [e for e in ev if e["event"] == "fifo-method" and e["method"] == "0x0328"
              or (e["event"] == "fifo-method" and any(r.get("kind") == "dma-source" for r in e["reads"]))]
    check(len(copies) >= 10, f"each copy is a FIFO event that carries its transfer ({len(copies)})")
    first = copies[0]
    src = [r for r in first["reads"] if r.get("kind") == "dma-source"]
    dst = [w for w in first["writes"] if w.get("kind") == "dma-destination"]

    def blob(ref):
        return (d / "blobs/sha256" / ref["sha256"][:2] / ref["sha256"]).read_bytes()
    check(len(src) == 1 and blob(src[0]) == pat(64, 1),
          "the copy's source span is a consumed read: the capture holds the source bytes")
    check(len(dst) == 1 and blob(dst[0]) == pat(64, 1) and dst[0]["ea"] == "0x00380000",
          "...and its destination a write holding what the copy left there")
    check(src and src[0]["consume_seq"] < dst[0]["consume_seq"], "...consumed before it was written")
    frames = [e for e in ev if e["event"] == "frame-complete"]
    check(len(frames) == 1 and frames[0]["unknown_methods"] == 0 and frames[0]["valid"] is True,
          f"known gaps (missing states) do not invalidate a frame ({frames[0]['unknown_methods'] if frames else None})")
    err = validation_error(d / "capture.ndjson")
    check(err is None, f"`validate` accepts a capture whose only oddities are known gaps ({err})")

    c = Path(tmp) / "clean"
    p = run(binary, "clean", c)
    check(p.returncode == 0, f"copies and allow-listed methods under a capture ({p.returncode})\n{p.stdout[-400:]}")
    err = validation_error(c / "capture.ndjson")
    cap = events(c)
    fr = [e for e in cap if e["event"] == "frame-complete"]
    check(err is None and len(fr) == 1 and fr[0]["valid"] is True and fr[0]["unknown_methods"] == 0,
          f"an inert method is not counted: that frame is valid and the capture validates ({err})")


def test_dump_list(binary):
    p = run(binary, 32)
    check(p.returncode == 0 and "kept 32 of 32, first 100, last 131" in p.stdout, f"a list of 32 is kept whole ({p.stdout.strip()})")
    p = run(binary, 33)
    f = stop_of(p)
    check(p.returncode == 70 and f and f["kind"] == "config-invalid" and "33" in p.stderr and
          "PS3RECOMP_METAL_FRAME_DUMP_LIST" in p.stderr,
          f"a list of 33 is a named stop that says it was 33 ({p.returncode} {f})")
    check("kept" not in p.stdout, "...before anything was kept or dumped")


def test_classification_table():
    """The header's table through a probe compiled from it (never parsed as text)."""
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        print("[rsx_methods] SKIP the classification probe: no C++ compiler (CXX, c++, clang++, g++)")
        return
    probe = Path(tempfile.gettempdir()) / "rsx_method_probe"
    src = ROOT / "libs/video/tests/rsx_method_probe.cpp"
    cmd = [cxx, "-std=c++17", "-O1", "-I", str(ROOT / "include"), "-I", str(ROOT / "libs/video"),
           str(src), str(ROOT / "runtime/diag/stop.cpp"), "-o", str(probe)]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        check(False, f"the classification probe compiles\n{p.stderr[-1500:]}")
        return
    p = subprocess.run([str(probe)], capture_output=True, text=True, timeout=60)
    for line in p.stdout.splitlines():
        if line.startswith("[PASS]") or line.startswith("[FAIL]"):
            print(line)
    check(p.returncode == 0, f"the classification probe passes every check (exit {p.returncode})")


def test_doc_matches_table(tmp):
    """docs/RSX_GRAPHICS.md's table must list exactly the header's rows, with the same verdicts."""
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        print("[rsx_methods] SKIP the table/doc comparison: no C++ compiler")
        return
    probe = Path(tmp) / "probe_dump"
    p = subprocess.run([cxx, "-std=c++17", "-O1", "-I", str(ROOT / "include"), "-I", str(ROOT / "libs/video"),
                        str(ROOT / "libs/video/tests/rsx_method_probe.cpp"),
                        str(ROOT / "runtime/diag/stop.cpp"), "-o", str(probe)],
                       capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        check(False, f"the probe builds for the doc comparison\n{p.stderr[-800:]}")
        return
    dump = subprocess.run([str(probe), "--dump"], capture_output=True, text=True, timeout=60)
    rows = set()
    for line in dump.stdout.splitlines():
        f = line.split()
        if len(f) == 6 and f[0] == "ROW":
            rows.add((f[1].lower(), f[2].lower(), f[4]))
    doc = (ROOT / "docs/RSX_GRAPHICS.md").read_text()
    doc_rows = set()
    for line in doc.splitlines():
        m = re.match(r"\|\s*(\w+)\s*\|\s*(0x[0-9A-Fa-f]{4})(?:\+\d+)?\s*\|\s*`[^`]*`\s*\|\s*(handled|inert|missing)\s*\|", line)
        if m:
            doc_rows.add((m.group(1).lower(), m.group(2).lower(), m.group(3)))
    missing = rows - doc_rows
    extra = doc_rows - rows
    check(not missing and not extra,
          f"docs/RSX_GRAPHICS.md lists every table row with its verdict ({len(rows)} rows; "
          f"missing {sorted(missing)[:3]}, extra {sorted(extra)[:3]})")


def main():
    binaries = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="rsx-methods-") as tmp:
        test_classification_table()
        test_doc_matches_table(tmp)
        test_validator(tmp)
        if binaries:
            test_fixture(binaries[0], tmp)
        else:
            print("[rsx_methods] SKIP the NV0039 / verdict fixture: run through ctest (rsx_methods), which builds it")
        if len(binaries) > 1:
            test_dump_list(binaries[1])
        else:
            print("[rsx_methods] SKIP the frame-dump list fixture (macOS, Metal backend): run through ctest (rsx_methods)")
    print(f"[rsx_methods] {'FAIL' if FAILS else 'PASS'} ({FAILS} failed)")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
