#!/usr/bin/env python3
"""ftrace_report: frame binning, import call sites, split, cadence, bad input."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

TOOL = Path(__file__).resolve().parent / "ftrace_report.py"
FLIP_REQ, USLEEP, HLE, PRESENT = 9, 2, 15, 10
LOCK, SYNC, POLL = 0x1573DC3F, 0xFACB3CED, 0x74A66AF0     # lwmutex_lock, Port2Sync, GetPortConfig

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


def span(kind, who, a, b, t0_ms, t1_ms):
    return f"{kind} {who} {a:x} {b:x} {t0_ms * 1000:.0f} {t1_ms * 1000:.0f}"


def trace(lines):
    head = ["# ps3_ftrace start_us=0 end_us=200000 spans=0 dropped=0", "T 1 main", "T 2 audio"]
    return "\n".join(head + lines) + "\n"


def run(tmp, text, *args):
    p = Path(tmp) / "t.txt"
    p.write_text(text)
    q = subprocess.run([sys.executable, str(TOOL), str(p), *args], capture_output=True, text=True, timeout=60)
    return q.returncode, q.stdout, q.stderr


# Flips at 0, 20, 40, 80 ms: frames of 20, 20 and 40 ms.
lines = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 20, 40, 80)]
# main: a lock wait of 4 ms in every frame at one site; a job sync of 10 ms in
# the fast frames and 30 ms in the slow one, the slow one crossing no frame edge.
for f0 in (0, 20, 40):
    lines.append(span(HLE, 1, LOCK, 0x1000, f0 + 1, f0 + 5))
lines += [span(HLE, 1, SYNC, 0x2000, 6, 16), span(HLE, 1, SYNC, 0x2000, 26, 36),
          span(HLE, 1, SYNC, 0x2000, 46, 76)]
# A usleep poll that crosses the 20 ms frame edge: 3 ms before, 2 ms after.
lines.append(span(USLEEP, 1, 1, 0x3000, 17, 22))
lines.append(span(PRESENT, 0, 0, 0, 10, 12))
# audio: polls every 5 ms, then a 30 ms gap spent 24 ms in Port2Sync.
for t in (0, 5, 10, 15, 45, 50, 55):
    lines.append(span(HLE, 2, POLL, 0x4000, t, t + 0.1))
lines.append(span(HLE, 2, SYNC, 0x5000, 18, 42))

with tempfile.TemporaryDirectory() as tmp:
    rc, out, err = run(tmp, trace(lines), "--json", "--split", "30", "--cadence", f"{POLL:x}@2", "--gap", "20")
    check(rc == 0, f"exit 0: {err}")
    r = json.loads(out)
    check(r["frames"]["count"] == 3 and r["frames"]["median_ms"] == 20, "three frames, median 20 ms")
    main = next(t for t in r["all"]["hle"] if t["tid"] == 1)
    by = {(s["nid"], s["site"]): s["ms"] for s in main["sites"]}
    check(abs(by[(f"{LOCK:08X}", "00001000")] - 4) < 1e-6, "lock wait 4 ms per frame at its call site")
    check(abs(by[(f"{SYNC:08X}", "00002000")] - 50 / 3) < 1e-6, "job sync (10+10+30)/3 ms per frame")
    us = next(t for t in r["all"]["ppu_blocked"] if t["tid"] == 1)
    check(abs(us["ms"] - 5 / 3) < 1e-6, "a usleep crossing a frame edge counts once, shared out")
    check(abs(r["all"]["rsx"]["present"] - 2 / 3) < 1e-6, "present time per frame")
    fast, slow = r["split"]["fast"], r["split"]["slow"]
    check(fast["frames"] == 2 and slow["frames"] == 1, "split at 30 ms: two fast frames, one slow")
    sync = lambda bd: next(s["ms"] for t in bd["hle"] if t["tid"] == 1 for s in t["sites"] if s["nid"] == f"{SYNC:08X}")
    check(abs(sync(fast) - 10) < 1e-6 and abs(sync(slow) - 30) < 1e-6, "the slow frame's sync wait is 30 ms")
    cad = r["cadence"]["threads"]
    check(len(cad) == 1 and cad[0]["tid"] == 2, "--cadence NID@TID keeps only that thread")
    c = cad[0]
    check(c["calls"] == 7 and c["gap_ms"]["max"] == 30 and c["over"]["count"] == 1, "one 30 ms gap over 20 ms")
    share = c["over"]["inside_share"]
    check(abs(share.get("cellSpursJobQueuePort2Sync", share.get(f"nid {SYNC:08X}", 0)) - 24 / 30) < 1e-6,
          "the gap was 80 % inside the job sync")
    check(c["worst"][0]["gap_ms"] == 30 and abs(c["worst"][0]["at_s"] - 0.015) < 1e-9, "worst gap and its time")

    rc, out, err = run(tmp, trace(lines))
    check(rc == 0 and "Per frame" in out and "frame at" in out, "text report with timelines")

    rc, out, err = run(tmp, trace(lines[:2]))
    check(rc == 2 and "fewer than 3 flip requests" in err, "too few flips: exit 2")
    rc, out, err = run(tmp, trace(lines + ["9 1 zz 0 1 2"]))
    check(rc == 2 and "not a span line" in err and "Traceback" not in err, "malformed span: exit 2, no traceback")
    rc, out, err = run(tmp, trace(lines), "--cadence", "xyz")
    check(rc == 2 and "hex NID" in err, "bad --cadence: usage error")
    q = subprocess.run([sys.executable, str(TOOL), str(Path(tmp) / "missing.txt")], capture_output=True, text=True)
    check(q.returncode == 2 and "Traceback" not in q.stderr, "missing file: exit 2, no traceback")

print("ftrace_report: %s" % ("ok" if not failures else "%d failed" % failures))
sys.exit(1 if failures else 0)
