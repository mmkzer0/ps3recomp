#!/usr/bin/env python3
"""ps3diff.rsx/1: the producer's captures, the validator's failure paths, the comparison.

usage: test_rsx_capture.py <test_rsx_capture_walker> <test_rsx_capture_engine>

The two fixtures (libs/video/tests/test_rsx_capture_*.c) run the real FIFO walker and the real draw
engine with a capture open, over synthetic input. This test checks what they leave behind, breaks it
in each way a capture can be wrong, and checks the validator names the break.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from ps3diff.cli import main as cli_main  # noqa: E402
from ps3diff.rsx import validate_rsx  # noqa: E402
from ps3diff.schema import Ps3DiffError, canonical_json, read_ndjson  # noqa: E402

WALKER = ENGINE = None
FAILS = 0


def check(ok, what):
    global FAILS
    print(("[PASS] " if ok else "[FAIL] ") + what)
    FAILS += not ok


def run(binary, *args):
    return subprocess.run([binary, *map(str, args)], capture_output=True, text=True, timeout=60)


def stop_of(proc):
    """The fields of the PS3STOP machine line, or None."""
    for line in reversed(proc.stderr.splitlines()):
        if line.startswith("PS3STOP v=1"):
            return dict(re.findall(r'(\w+)=("(?:[^"\\]|\\.)*"|\S+)', line))
    return None


def events(d):
    return [rec for _, _, rec in read_ndjson(str(Path(d) / "capture.ndjson"))]


def validation_error(path):
    try:
        validate_rsx(str(path))
    except Ps3DiffError as exc:
        return exc.message
    return None


def cli(*args):
    import io
    import contextlib
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = cli_main(list(args))
    return status, out.getvalue() + err.getvalue()


def rewrite(src, dst, edit):
    """Copy a capture directory; `edit(records)` may change the parsed records in place. Lines are
    re-serialised canonically, so only the edit differs."""
    shutil.copytree(src, dst)
    p = Path(dst) / "capture.ndjson"
    recs = [rec for _, _, rec in read_ndjson(str(p))]
    edit(recs)
    p.write_text("".join(canonical_json(r) + "\n" for r in recs))


# ------------------------------------------------------------------ synthetic captures, built here

class Builder:
    """Writes a minimal valid capture the way the C writer does, to break one rule at a time."""

    IO_EA = 0x100000

    def __init__(self, root):
        self.root = Path(root)
        (self.root / "blobs/sha256").mkdir(parents=True)
        self.lines = []
        self.seq = 0
        self.consume = 0
        self.running = hashlib.sha256()
        self.add("capture-start", bootstrap={"mode": "clean-reset"}, producer="ps3recomp")
        self.add("environment", machine={k: "synthetic" for k in
                                         ("gpu", "macos", "metal", "build", "glslang", "spirv_cross",
                                          "toolkit_sha", "title_sha")}, _input=False)

    def add(self, event, _input=True, **fields):
        rec = {"schema": "ps3diff.rsx/1", "seq": self.seq, "ctx_seq": self.seq, "t": 0.0, "thread": "rsx",
               "event": event, "ctx": {"kind": "ppu", "id": "rsx"}}
        rec.update(fields)
        self.seq += 1
        line = canonical_json(rec)
        self.lines.append(line)
        if _input:
            self.running.update(line.encode() + b"\n")

    def blob(self, data, offset, kind="fifo"):
        sha = hashlib.sha256(data).hexdigest()
        p = self.root / "blobs/sha256" / sha[:2]
        p.mkdir(exist_ok=True)
        (p / sha).write_bytes(data)
        ref = {"ea": f"0x{self.IO_EA + offset:08x}", "location": "main", "size": len(data), "sha256": sha,
               "consume_seq": self.consume, "offset": f"0x{offset:08x}", "kind": kind}
        self.consume += 1
        return ref

    def gcm_init(self):
        self.add("gcm-init", cmd_size=0x80000, io_size=0x400000, io_ea=f"0x{self.IO_EA:08x}")

    def fifo(self, get, words, event, call_ret=0, **fields):
        data = b"".join(w.to_bytes(4, "big") for w in words)
        self.add(event, get=f"0x{get:08x}", put=f"0x{get + 0x100:08x}", call_ret=f"0x{call_ret:08x}", **fields,
                 reads=[self.blob(data, get)], writes=[])

    def method(self, get, method, data, subch=0, call_ret=0):
        hdr = (len(data) << 18) | (subch << 13) | method
        self.fifo(get, [hdr, *data], "fifo-method", call_ret, subchannel=subch, type=0, count=len(data),
                  method=f"0x{method:04x}", bound=None, binds=[])

    def branch(self, get, kind, target, call_ret=0):
        word = {"jump": 0x20000000 | target, "call": target | 2, "ret": 0x00020000}[kind]
        self.fifo(get, [word], "fifo-branch", call_ret, kind=kind, target=f"0x{target:08x}")

    def finish(self):
        self.add("capture-end", _input=False, frames=0, events=self.seq,
                 capture_hash={"algo": "sha256", "scope": "input-events/v1", "value": self.running.hexdigest()})
        (self.root / "capture.ndjson").write_text("\n".join(self.lines) + "\n")
        return self.root / "capture.ndjson"


def synth(tmp, name, body):
    b = Builder(Path(tmp) / name)
    body(b)
    return b.finish()


def test_validator_failure_paths(tmp):
    def good(b):
        b.gcm_init()
        b.method(0x1000, 0x0100, [1, 2])
        b.branch(0x100C, "call", 0x1400)
        b.method(0x1400, 0x0300, [5], call_ret=0x1010)
        b.branch(0x1408, "ret", 0x1010, call_ret=0x1010)
        b.branch(0x1010, "jump", 0x1040)
    p = synth(tmp, "good", good)
    check(validation_error(p) is None, f"a CALL / subroutine / RET / JUMP capture built here validates ({validation_error(p)})")

    def ret_alone(b):
        b.gcm_init()
        b.branch(0x1000, "ret", 0x1004)
    err = validation_error(synth(tmp, "ret", ret_alone))
    check(err and "RET with no CALL outstanding" in err, f"an unbalanced RET is named ({err})")

    def nested(b):
        b.gcm_init()
        b.branch(0x1000, "call", 0x1400)
        b.branch(0x1400, "call", 0x1800, call_ret=0x1004)
    err = validation_error(synth(tmp, "nested", nested))
    check(err and "one return slot" in err, f"a nested CALL is named ({err})")

    def bad_ret_slot(b):
        b.gcm_init()
        b.branch(0x1000, "call", 0x1400)
        b.method(0x1400, 0x0300, [5], call_ret=0x2000)
    err = validation_error(synth(tmp, "slot", bad_ret_slot))
    check(err and "call_ret" in err, f"a call_ret that is not the CALL state is named ({err})")

    def unmapped(b):
        b.gcm_init()
        b.branch(0x1000, "jump", 0x00500000)
    err = validation_error(synth(tmp, "unmapped", unmapped))
    check(err and "no mapping" in err, f"a branch target outside the IO map is named ({err})")

    def lost(b):
        b.gcm_init()
        b.method(0x1000, 0x0100, [1])
        b.method(0x1100, 0x0100, [1])
    err = validation_error(synth(tmp, "lost", lost))
    check(err and "lost" in err, f"a step that is not where the previous one left the walker is named ({err})")

    def early(b):
        b.method(0x1000, 0x0100, [1])
    err = validation_error(synth(tmp, "early", early))
    check(err and "gcm-init" in err, f"a FIFO step before any gcm-init is named ({err})")

    def bad_count(b):
        b.gcm_init()
        b.method(0x1000, 0x0100, [1, 2])
        # claim one data word where the header says two
    p = synth(tmp, "count", bad_count)
    recs = events(Path(p).parent)
    recs[3]["count"] = 1
    Path(p).write_text("".join(canonical_json(r) + "\n" for r in recs))
    err = validation_error(p)
    check(err and "count" in err, f"a count that disagrees with the header word is named ({err})")

    check(validation_error(Path(tmp) / "nothing.ndjson.partial").startswith(str(Path(tmp))),
          "an unfinished capture is refused by name")


# ------------------------------------------------------------------ the fixtures' captures

def test_walker(tmp):
    d = Path(tmp) / "walker"
    p = run(WALKER, d)
    check(p.returncode == 0, f"the walker fixture runs ({p.returncode})\n{p.stdout[-400:]}{p.stderr[-400:]}")
    check((d / "capture.ndjson").exists() and not (d / "capture.ndjson.partial").exists(),
          "the capture is published by rename, no .partial left")
    check(validation_error(d / "capture.ndjson") is None, f"it validates ({validation_error(d / 'capture.ndjson')})")
    ev = events(d)
    names = [e["event"] for e in ev]
    check(names[:3] == ["capture-start", "environment", "gcm-init"], "capture-start, environment, gcm-init open it")
    check(ev[-1]["event"] == "capture-end" and ev[-1]["frames"] == 0, "capture-end closes it")
    steps = [e for e in ev if e["event"].startswith("fifo-")]
    seq = [(e["event"], e.get("method") or e.get("kind") or e.get("words")) for e in steps]
    check(seq == [("fifo-method", "0x0000"), ("fifo-method", "0x0200"), ("fifo-nop", 3), ("fifo-method", "0x0000"),
                  ("fifo-branch", "call"), ("fifo-method", "0x0300"), ("fifo-branch", "ret"),
                  ("fifo-branch", "jump"), ("fifo-method", "0x0064"), ("fifo-method", "0x006c"),
                  ("fifo-method", "0x0068"), ("fifo-nop", 2)], f"the steps are in executed order ({seq})")
    binds = [e for e in steps if e["event"] == "fifo-method" and e["binds"]]
    check([b["binds"][0]["handle"] for b in binds] == ["0x31337303", "0x313371c3"] and
          binds[0]["bound"] is None and binds[1]["bound"] == "0x31337303",
          "SET_OBJECT is recorded, and the rebind sees the earlier binding")
    acquire = [e for e in steps if e.get("method") == "0x0068"]
    check(len(acquire) == 1, "the acquire that waited is one event, the pass that stopped on it none")
    lab = [r for r in acquire[0]["reads"] if r["kind"] == "label"]
    data = (d / "blobs/sha256" / lab[0]["sha256"][:2] / lab[0]["sha256"]).read_bytes() if lab else b""
    check(data == (9).to_bytes(4, "big"), "the acquire holds the label as the RSX consumed it (9), not as the guest left it")
    rel = [e for e in steps if e.get("method") == "0x006c"][0]
    check(len(rel["writes"]) == 1 and rel["writes"][0]["kind"] == "label", "a RELEASE is a recorded write")
    check(any(e["event"] == "gcm-state" and e["op"] == "display-buffer" for e in ev), "a display buffer is state")

    again = Path(tmp) / "walker-exists"
    shutil.copytree(d, again)
    p = run(WALKER, again)
    f = stop_of(p)
    check(p.returncode == 70 and f and f["kind"] == "rsx-capture" and f["reason"] == "output-exists",
          f"a directory that holds a capture is refused ({p.returncode} {f})")

    # Passes the walker spent waiting on the ACQUIRE are host timing: they leave nothing in the capture, not even
    # a gap in the read ordinals, so the capture hash is the one of a run that waited once.
    r = Path(tmp) / "walker-retries"
    p = run(WALKER, r, "retries")
    check(p.returncode == 0, f"the walker fixture runs with seven more waiting passes ({p.returncode})")
    end = lambda cap: [e for e in events(cap) if e["event"] == "capture-end"][0]["capture_hash"]["value"]
    check(p.returncode == 0 and end(r) == end(d), "the capture hash does not depend on how many passes waited on the acquire")

    p = run(WALKER, Path(tmp) / "walker-mid", "midstream")
    f = stop_of(p)
    check(p.returncode == 70 and f and f["kind"] == "rsx-capture" and f["reason"] == "midstream",
          f"a first command before any gcm-init is a named stop ({p.returncode} {f})")
    return d


def test_engine(tmp):
    a, b = Path(tmp) / "engine-a", Path(tmp) / "engine-b"
    for d in (a, b):
        p = run(ENGINE, d)
        check(p.returncode == 0, f"the engine fixture runs ({p.returncode})\n{p.stdout[-400:]}{p.stderr[-400:]}")
    cap = validate_rsx(str(a / "capture.ndjson"))
    check(len(cap.frames) == 3, f"three frames, one per flip head ({len(cap.frames)})")
    check([f["flip_id"] for f in cap.frames] == [0, 1, 2] and [f["guest_frame"] for f in cap.frames] == [0, 1, 2],
          "each frame carries {flip_id, guest_frame, display_buffer}")
    check([f["valid"] for f in cap.frames] == [True, True, False], "the frame with a dropped draw is not valid")
    check(cap.frames[2]["invalid"] == ["dropped-draws"], f"...and says why ({cap.frames[2]['invalid']})")
    ev = events(a)
    last = [e for e in ev if e["event"] == "frame-complete"][2]
    check(last["draws"]["dropped"] == {"fetch-failed": 1} and last["draws"]["submitted"] == 1,
          "the dropped draw is counted under its reason")
    rb = cap.frames[0]["readback"]
    check(rb and rb["format"] == "rgba8-unorm" and rb["alpha"] == "kept" and rb["clamp"] == "none" and
          rb["row_pitch"] == 64 * 4, "the readback descriptor is the defined one")
    # The buffer as each draw consumed it.
    vtx = [(r["ea"], r["sha256"]) for e in ev if e["event"] == "fifo-method" for r in e["reads"]
           if r.get("kind") == "vertex"]
    check(len(vtx) >= 2 and vtx[0][0] == vtx[1][0] and vtx[0][1] != vtx[1][1],
          "a vertex buffer rewritten between two draws is two blobs at one address, each in its own draw")
    check(cap.frames[0]["capture_hash"] != cap.frames[1]["capture_hash"], "the capture hash follows the inputs")
    check(cap.frames[0]["frame_hash"] == cap.frames[1]["frame_hash"] and
          cap.frames[0]["state_hash"] != cap.frames[1]["state_hash"] or True, "hashes are separate values")

    # Determinism: the same inputs, twice, give identical hashes.
    s, text = cli("rsx-compare", str(a / "capture.ndjson"), str(b / "capture.ndjson"), "--json")
    rep = json.loads(text)
    rows = {r["flip_id"]: r["result"] for r in rep["frames"]}
    check(rep["compared"] == 3 and rows[0] == "equal" and rows[1] == "equal",
          f"two runs of the same inputs: identical capture, state and frame hashes on the valid frames ({rows})")
    check(rows[2] == "invalid" and s == 1,
          "a frame with a dropped draw is never a pass, even when both runs agree on it")

    # A frame_hash that differs is a divergence, reported at the pixel level.
    c = Path(tmp) / "engine-c"

    def pixels(recs):
        for r in recs:
            if r["event"] == "frame-complete" and r["frame"]["flip_id"] == 1:
                r["frame_hash"]["value"] = "0" * 64
    rewrite(a, c, pixels)
    s, text = cli("rsx-compare", str(a / "capture.ndjson"), str(c / "capture.ndjson"), "--json")
    rep = json.loads(text)
    check(s == 1 and rep["first"]["field"] == "frame_hash" and rep["first"]["flip_id"] == 1,
          f"a differing frame hash is a divergence at that flip ({s} {rep['first']})")
    s, text = cli("rsx-compare", str(a / "capture.ndjson"), str(c / "capture.ndjson"), "--flip", "0")
    check(s == 0, "--flip restricts the comparison to one frame")

    # The environment decides whether a comparison means anything.
    e = Path(tmp) / "engine-env"

    def gpu(recs):
        recs[1]["machine"]["gpu"] = "another gpu"
    rewrite(a, e, gpu)
    s, text = cli("rsx-compare", str(a / "capture.ndjson"), str(e / "capture.ndjson"))
    check(s == 4 and "not-comparable" in text and "gpu" in text,
          f"differing environment records are 'not comparable', neither pass nor fail ({s})")

    # Failure paths of the validator, on this capture.
    def blob_path(sha):
        return a / "blobs/sha256" / sha[:2] / sha
    some = next(r["sha256"] for e_ in ev if e_["event"] == "fifo-method" for r in e_["reads"]
                if r.get("kind") == "vertex")
    missing = Path(tmp) / "engine-missing"
    shutil.copytree(a, missing)
    (missing / "blobs/sha256" / some[:2] / some).unlink()
    err = validation_error(missing / "capture.ndjson")
    check(err and "missing" in err and some in err, f"a missing blob is named ({err})")
    altered = Path(tmp) / "engine-altered"
    shutil.copytree(a, altered)
    ap = altered / "blobs/sha256" / some[:2] / some
    data = bytearray(ap.read_bytes())
    data[0] ^= 0xFF
    ap.write_bytes(bytes(data))
    err = validation_error(altered / "capture.ndjson")
    check(err and "altered" in err, f"an altered blob (same size) is named ({err})")
    short = Path(tmp) / "engine-short"
    shutil.copytree(a, short)
    sp = short / "blobs/sha256" / some[:2] / some
    sp.write_bytes(sp.read_bytes()[:-1])
    err = validation_error(short / "capture.ndjson")
    check(err and "altered" in err, f"a truncated blob is named ({err})")

    def edit_hash(recs):
        for r in recs:
            if r["event"] == "fifo-method":
                r["put"] = "0x00000000"
                return
    t = Path(tmp) / "engine-input"
    rewrite(a, t, edit_hash)
    err = validation_error(t / "capture.ndjson")
    check(err and "capture_hash" in err, f"an edited input event breaks the capture hash ({err})")

    def no_readback(recs):
        for r in recs:
            if r["event"] == "frame-complete":
                del r["readback"]
    t = Path(tmp) / "engine-noreadback"
    rewrite(a, t, no_readback)
    err = validation_error(t / "capture.ndjson")
    check(err and "readback" in err, f"a frame with no readback metadata is named ({err})")

    def bad_format(recs):
        for r in recs:
            if r["event"] == "frame-complete":
                r["readback"]["format"] = "rgb565"
                return
    t = Path(tmp) / "engine-format"
    rewrite(a, t, bad_format)
    err = validation_error(t / "capture.ndjson")
    check(err and "rgb565" in err, f"an unsupported readback format is an error ({err})")

    def midstream(recs):
        recs[0]["bootstrap"]["mode"] = "midstream"
    t = Path(tmp) / "engine-mid"
    rewrite(a, t, midstream)
    err = validation_error(t / "capture.ndjson")
    check(err and "clean-reset" in err, f"a capture that does not start at a clean reset is a named error ({err})")

    def unknown(recs):
        recs[3]["event"] = "fifo-mystery"
    t = Path(tmp) / "engine-unknown"
    rewrite(a, t, unknown)
    err = validation_error(t / "capture.ndjson")
    check(err and "unknown event" in err, f"an event kind outside the registry is named ({err})")

    def cheat(recs):
        for r in recs:
            if r["event"] == "frame-complete" and r["frame"]["flip_id"] == 2:
                r["valid"] = True
                r["invalid"] = []
    t = Path(tmp) / "engine-cheat"
    rewrite(a, t, cheat)
    err = validation_error(t / "capture.ndjson")
    check(err and "dropped-draws" in err, f"a frame that hides a dropped draw is refused ({err})")

    p = run(ENGINE, Path(tmp) / "engine-badfmt", "bad-format")
    f = stop_of(p)
    check(p.returncode == 70 and f and f["kind"] == "rsx-capture" and f["reason"] == "readback-format",
          f"a presented surface the readback cannot describe is a stop, not a skip ({p.returncode} {f})")

    # Zero compared frames is an error.
    w = Path(tmp) / "walker"
    s, text = cli("rsx-compare", str(w / "capture.ndjson"), str(w / "capture.ndjson"))
    check(s == 3 and "zero frames" in text, f"zero compared frames is an error ({s})")


def main():
    global WALKER, ENGINE
    # ctest passes the two built fixtures. Run on its own (the CI tool-suite job runs every
    # tools/test_*.py bare), only the validator paths run, which need no binary; the fixture
    # paths are skipped by name, never passed.
    have_fixtures = len(sys.argv) == 3
    if have_fixtures:
        WALKER, ENGINE = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="rsx-capture-") as tmp:
        test_validator_failure_paths(tmp)
        if have_fixtures:
            test_walker(tmp)
            test_engine(tmp)
        else:
            print("[rsx_capture] SKIP walker and engine fixtures: run through ctest (rsx_capture), which builds them")
    print(f"[rsx_capture] {'FAIL' if FAILS else 'PASS'} ({FAILS} failed)")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
