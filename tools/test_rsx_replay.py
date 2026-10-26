#!/usr/bin/env python3
"""rsx_replay: capture -> replay on a Metal device, determinism, and every way a replay must refuse.

usage: test_rsx_replay.py <rsx_replay> <test_rsx_replay_scene>

The scene fixture (libs/video/tests/test_rsx_replay_scene.c) is a synthetic live run through the real
walker, draw engine and headless Metal backend: a clear, a triangle, a CALL/RET and a JUMP, a NV0039
copy, a rebind, a textured strip and two flips. rsx_replay replays what it captured. Each failure path
mutates a copy of a capture that replays cleanly, so the unmutated capture is the control: the named
error comes from the mutation and nothing else.

Run bare (the CI tool-suite job runs every tools/test_*.py that way) there is no binary to run, so the
whole test is skipped by name. CTest passes both binaries; the fixture exits 77 without a Metal device.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from ps3diff.cli import main as cli_main  # noqa: E402
from ps3diff.schema import canonical_json, read_ndjson  # noqa: E402

REPLAY = SCENE = None
FAILS = 0
INPUT_EVENTS = {"capture-start", "gcm-init", "gcm-state", "fifo-method", "fifo-branch", "fifo-word", "fifo-nop"}


def check(ok, what):
    global FAILS
    print(("[PASS] " if ok else "[FAIL] ") + what)
    FAILS += not ok


def cli(*args):
    import contextlib
    import io
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = cli_main(list(args))
    return status, out.getvalue() + err.getvalue()


def scene(tmp, name, *extra):
    d = Path(tmp) / name
    p = subprocess.run([SCENE, str(d), *extra], capture_output=True, text=True, timeout=120)
    if p.returncode == 77:
        print("[rsx_replay] SKIP: " + p.stdout.strip().splitlines()[-1])
        sys.exit(77)
    # The exit status and stderr name the cause (an ASan report, a stop line); the last stdout line alone does not.
    check(p.returncode == 0, f"the scene fixture ({name}) runs: exit {p.returncode}, "
                             f"{p.stdout.strip().splitlines()[-1:]}\n{p.stderr[-3000:]}")
    return d


def replay(tmp, cap, name, *args, env=None):
    """Run rsx_replay; returns (exit status, parsed report or None, stderr)."""
    out = Path(tmp) / f"{name}.json"
    e = dict(os.environ)
    e.update(env or {})
    p = subprocess.run([REPLAY, str(cap), "--out", str(out), "--manifest", str(Path(tmp) / f"{name}.manifest"), *args],
                       capture_output=True, text=True, timeout=240, env=e)
    rep = json.loads(out.read_text()) if out.exists() else None
    return p.returncode, rep, p.stderr


def records(cap):
    return [rec for _, _, rec in read_ndjson(str(Path(cap) / "capture.ndjson"))]


def write_records(cap, recs, rehash=True):
    """Write `recs` back as canonical lines. With `rehash` the capture hashes are recomputed the way the writer
    computes them, so the edit is the only thing wrong with the file."""
    if rehash:
        h = hashlib.sha256()
        for r in recs:
            line = canonical_json(r)
            if r["event"] in INPUT_EVENTS:
                h.update(line.encode("ascii") + b"\n")
            elif r["event"] in ("frame-complete", "capture-end"):
                r["capture_hash"]["value"] = h.hexdigest()
    (Path(cap) / "capture.ndjson").write_text("".join(canonical_json(r) + "\n" for r in recs))


def mutate(src, dst, edit, rehash=True):
    shutil.copytree(src, dst)
    recs = records(dst)
    edit(recs)
    write_records(dst, recs, rehash)
    return dst


def blob_path(cap, sha):
    return Path(cap) / "blobs" / "sha256" / sha[:2] / sha


def first_blob(recs, kind, where="reads"):
    for r in recs:
        for b in r.get(where, []):
            if b.get("kind") == kind:
                return r, b
    raise LookupError(kind)


def error_of(rep):
    return rep["error"]["name"] if rep and rep.get("error") else None


def test_round_trip(tmp, live):
    v = cli("validate", str(live / "capture.ndjson"))
    check(v[0] == 0 and "2 frames" in v[1], f"the live capture validates ({v[1].strip()})")

    s1, r1, _ = replay(tmp, live, "r1")
    s2, r2, _ = replay(tmp, live, "r2")
    check(s1 == 0 and s2 == 0 and r1 and r2, f"both replays exit 0 ({s1}, {s2})")
    check(r1["status"] == "equal" and r1["gate_valid"] and r1["compared"] == 2, "the replay is equal, gate-valid, over 2 frames")
    check(r1["checked_writes"] >= 2, f"the label RELEASE and the NV0039 destination were checked ({r1['checked_writes']} writes)")

    for row in r1["results"]:
        check(row["live"] == row["replay"] and row["result"] == "equal",
              f"flip {row['frame']['flip_id']}: live equals replay in capture, state and frame hash")
    check(r1["results"][0]["live"]["frame_hash"] != r1["results"][1]["live"]["frame_hash"], "the two frames differ, so a hash is not a constant")
    check([x["replay"] for x in r1["results"]] == [x["replay"] for x in r2["results"]],
          "two replays have identical capture, state and frame hashes")
    check(r1["capture_hash"] == r2["capture_hash"] != "", "both replays name the capture's input hash")

    for mf in ("r1", "r2"):
        s, text = cli("rsx-compare", str(live / "capture.ndjson"), str(Path(tmp) / f"{mf}.manifest" / "capture.ndjson"))
        check(s == 0 and "equal" in text, f"ps3diff rsx-compare live {mf}: equal ({s})")
    s, text = cli("rsx-compare", str(Path(tmp) / "r1.manifest" / "capture.ndjson"), str(Path(tmp) / "r2.manifest" / "capture.ndjson"))
    check(s == 0, f"ps3diff rsx-compare r1 r2: equal ({s})")

    s, r, _ = replay(tmp, live, "range", "--frames", "1")
    check(s == 0 and r["compared"] == 1 and r["frames"] == {"from": 1, "to": 1}, f"--frames 1 compares the one frame ({s})")
    s, r, err = replay(tmp, live, "norange", "--frames", "5-6")
    check(s == 3 and error_of(r) == "zero-frames", f"a range with no frame is an error, exit 3 ({s}, {error_of(r)})")
    return r1


def test_blobs(tmp, live):
    recs = records(live)
    _, b = first_blob(recs, "texture")

    mutate(live, Path(tmp) / "missing", lambda r: None)
    blob_path(Path(tmp) / "missing", b["sha256"]).unlink()
    s, r, _ = replay(tmp, Path(tmp) / "missing", "missing")
    check(s == 2 and error_of(r) == "missing-blob", f"a missing blob is a named error ({s}, {error_of(r)})")

    mutate(live, Path(tmp) / "altered", lambda r: None)
    p = blob_path(Path(tmp) / "altered", b["sha256"])
    data = bytearray(p.read_bytes())
    data[0] ^= 0xFF
    p.write_bytes(bytes(data))
    s, r, _ = replay(tmp, Path(tmp) / "altered", "altered")
    check(s == 2 and error_of(r) == "altered-blob", f"an altered blob is a named error ({s}, {error_of(r)})")


def test_manifest_edits(tmp, live):
    # An edit to a field the replay does not use leaves every step runnable: only the recorded hash catches it.
    def edit_type(recs):
        r = next(x for x in recs if x["event"] == "fifo-method" and x["type"] == 0)
        r["type"] = 2
    mutate(live, Path(tmp) / "stale-hash", edit_type, rehash=False)
    s, r, _ = replay(tmp, Path(tmp) / "stale-hash", "stale-hash")
    check(s == 2 and error_of(r) == "capture-hash", f"an altered manifest fails its recorded input hash ({s}, {error_of(r)})")

    # A step that does not start where the walker is.
    def shift(recs):
        methods = [x for x in recs if x["event"] == "fifo-method"]
        x = methods[len(methods) // 2]
        x["get"] = "0x%08x" % (int(x["get"], 16) + 4)
    mutate(live, Path(tmp) / "discontinuity", shift)
    s, r, _ = replay(tmp, Path(tmp) / "discontinuity", "discontinuity")
    check(s == 2 and error_of(r) == "step-discontinuity", f"a step off the walker's position is a named error ({s}, {error_of(r)})")

    # A step whose recorded length is not what the walker consumes.
    def length(recs):
        x = next(x for x in recs if x["event"] == "fifo-method" and x["count"] >= 1)
        x["count"] = x["count"] - 1
    mutate(live, Path(tmp) / "landing", length)
    s, r, _ = replay(tmp, Path(tmp) / "landing", "landing")
    check(s == 2 and error_of(r) == "step-landing", f"a step that does not land where the recording says is a named error ({s}, {error_of(r)})")

    # A recorded write the replay does not reproduce: the label's value is changed, and the manifest is
    # rehashed and the new value stored as a blob, so nothing else is wrong with it.
    def corrupt_label(recs):
        ev, w = first_blob(recs, "label", "writes")
        new = b"\x00\x00\x00\x08"
        sha = hashlib.sha256(new).hexdigest()
        blob_path_dir = Path(tmp) / "write-mismatch" / "blobs" / "sha256" / sha[:2]
        blob_path_dir.mkdir(parents=True, exist_ok=True)
        (blob_path_dir / sha).write_bytes(new)
        w["sha256"] = sha
    mutate(live, Path(tmp) / "write-mismatch", corrupt_label)
    s, r, err = replay(tmp, Path(tmp) / "write-mismatch", "write-mismatch")
    check(s == 2 and error_of(r) == "write-mismatch" and "label" in r["error"]["detail"],
          f"a recorded write the replay does not produce is a named error ({s}, {error_of(r)})")

    # A frame whose identity the replay does not reproduce.
    def identity(recs):
        f = [x for x in recs if x["event"] == "frame-complete"][1]
        f["frame"]["display_buffer"] = 2
    mutate(live, Path(tmp) / "identity", identity, rehash=False)
    s, r, _ = replay(tmp, Path(tmp) / "identity", "identity")
    check(s == 2 and error_of(r) == "frame-identity", f"a frame-identity mismatch is a named error ({s}, {error_of(r)})")

    # A frame hash that is not the replay's is a difference, not an error.
    def frame_hash(recs):
        f = [x for x in recs if x["event"] == "frame-complete"][1]
        f["frame_hash"]["value"] = "0" * 64
    mutate(live, Path(tmp) / "differs", frame_hash, rehash=False)
    s, r, _ = replay(tmp, Path(tmp) / "differs", "differs")
    check(s == 1 and r["status"] == "differ" and r["first"].startswith("flip 1: frame_hash"),
          f"a frame hash the replay does not reproduce is a difference, exit 1 ({s}, {r and r['first']})")

    # The environment is somebody else's machine.
    def gpu(recs):
        e = next(x for x in recs if x["event"] == "environment")
        e["machine"]["gpu"] = "Some Other GPU"
    mutate(live, Path(tmp) / "env", gpu, rehash=False)
    s, r, err = replay(tmp, Path(tmp) / "env", "env")
    check(s == 4 and r["status"] == "not-comparable" and r["environment"]["differs"] == ["gpu"] and "not comparable" in err,
          f"an environment mismatch reports not comparable, exit 4 ({s}, {r and r['status']})")
    check(r["results"] == [] and r["steps"] == 0, "...and nothing was replayed")

    # A capture that was never closed.
    shutil.copytree(live, Path(tmp) / "partial")
    os.rename(Path(tmp) / "partial" / "capture.ndjson", Path(tmp) / "partial" / "capture.ndjson.partial")
    s, r, _ = replay(tmp, Path(tmp) / "partial", "partial")
    check(s == 2 and error_of(r) == "unfinished-capture", f"an unfinished capture is refused by name ({s}, {error_of(r)})")


def patch_method(cap, old, new):
    """Rewrite the first fifo-method packet of `cap` to another method number, keeping every hash honest.

    Hand-building the packet through the live walker is impossible now that an unclassified method is a
    named stop: the capture has to exist first, so the fixture patches a valid one. The packet's header
    word is rewritten in its blob, the blob rehashed, and the capture hash recomputed."""
    recs = records(cap)
    for r in recs:
        if r["event"] == "fifo-method" and r["method"] == f"0x{old:04x}":
            rd = next(x for x in r["reads"] if x["kind"] == "fifo")
            path = blob_path(cap, rd["sha256"])
            data = bytearray(path.read_bytes())
            hdr = int.from_bytes(data[0:4], "big")
            data[0:4] = ((hdr & ~0x1FFC) | (new & 0x1FFC)).to_bytes(4, "big")
            sha = hashlib.sha256(bytes(data)).hexdigest()
            base = cap / "blobs" / "sha256" / sha[:2]
            base.mkdir(parents=True, exist_ok=True)
            (base / sha).write_bytes(bytes(data))
            rd["sha256"] = sha
            r["method"] = f"0x{new:04x}"
            write_records(cap, recs, rehash=True)
            return True
    return False


def test_refusals(tmp, live):
    s, r, _ = replay(tmp, live, "fixed-function", env={"PS3RECOMP_METAL_FIXED_FUNCTION": "1"})
    check(s == 2 and error_of(r) == "fixed-function", f"a fixed-function fallback is refused by name ({s}, {error_of(r)})")

    # Fail-before/after for the live stop, on the same capture: one method number differs.
    unk = scene(tmp, "unknown")
    check(patch_method(unk, 0x0a74, 0x1f80), "a valid capture carries an unclassified NV4097 method (0x1F80)")
    s, r, err = replay(tmp, unk, "unknown")
    fields = dict(part.split("=", 1) for part in err.split() if "=" in part)
    check(s == 70 and fields.get("kind") == "rsx-unknown-method",
          f"replaying it stops with the named stop rsx-unknown-method (exit {s})")
    check(fields.get("class") == "NV4097" and fields.get("method") == "0x1F80" and
          fields.get("subchannel") == "0" and fields.get("fifo", "").startswith("0x") and
          fields.get("word", "").startswith("0x"),
          f"the stop names class, method, subchannel, fifo and word ({fields.get('class')} {fields.get('method')} "
          f"sub {fields.get('subchannel')} fifo {fields.get('fifo')} word {fields.get('word')})")
    s, text = cli("validate", str(unk / "capture.ndjson"))
    check(s == 0, f"the capture itself is well-formed: validate accepts it ({s} {text[:80]})")

    mir = scene(tmp, "mirror")
    check(patch_method(mir, 0x0a74, 0x0100), "...and the mirror capture carries an allow-listed method instead (0x0100)")
    s, r, err = replay(tmp, mir, "mirror")
    check(s == 0 and r and r["results"] and error_of(r) is None,
          f"the mirror capture replays and produces its frame ({s}, {r and r['status']})")

    rep = scene(tmp, "report", "report")
    s, r, _ = replay(tmp, rep, "report-accepted", "--accept-unknown-methods")
    check(s == 5 and r["checked_writes"] >= 3, f"a GET_REPORT's write (with its recorded timer) is reproduced ({s}, {r and r['checked_writes']} writes)")
    s2, r2, _ = replay(tmp, rep, "report-accepted-2", "--accept-unknown-methods")
    check(s2 == 5 and [x["replay"] for x in r["results"]] == [x["replay"] for x in r2["results"]],
          "...and replays twice to the same hashes: no host clock in the replay path")

    def report_value(recs):
        ev, w = first_blob(recs, "report", "writes")
        d = bytearray(blob_path(rep, w["sha256"]).read_bytes())
        d[8] ^= 0x01
        sha = hashlib.sha256(bytes(d)).hexdigest()
        base = Path(tmp) / "report-mismatch" / "blobs" / "sha256" / sha[:2]
        base.mkdir(parents=True, exist_ok=True)
        (base / sha).write_bytes(bytes(d))
        w["sha256"] = sha
    mutate(rep, Path(tmp) / "report-mismatch", report_value)
    s, r, _ = replay(tmp, Path(tmp) / "report-mismatch", "report-mismatch", "--accept-unknown-methods")
    check(s == 2 and error_of(r) == "write-mismatch" and "report" in r["error"]["detail"],
          f"a report whose value the replay does not reproduce is a named error ({s}, {error_of(r)})")


def main():
    global REPLAY, SCENE
    if len(sys.argv) != 3:
        print("[rsx_replay] SKIP the replay runner and the scene fixture: run through ctest (rsx_replay), which builds them")
        print("[rsx_replay] PASS (0 failed, everything skipped by name)")
        return 0
    REPLAY, SCENE = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="rsx-replay-") as tmp:
        live = scene(tmp, "live")
        test_round_trip(tmp, live)
        test_blobs(tmp, live)
        test_manifest_edits(tmp, live)
        test_refusals(tmp, live)
    print(f"[rsx_replay] {'FAIL' if FAILS else 'PASS'} ({FAILS} failed)")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
