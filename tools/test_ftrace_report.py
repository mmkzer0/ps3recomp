#!/usr/bin/env python3
"""ftrace_report: frame binning, import call sites, split, cadence, drawables, label-store waits, bad input."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

TOOL = Path(__file__).resolve().parent / "ftrace_report.py"
FLIP_REQ, USLEEP, HLE, PRESENT = 9, 2, 15, 10
AUDIO_MIX, AUDIO_PACE, AUDIO_UNDERRUN, AUDIO_STALE = 18, 19, 20, 21
LOCK, SYNC, POLL = 0x1573DC3F, 0xFACB3CED, 0x74A66AF0     # lwmutex_lock, Port2Sync, GetPortConfig

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


def span(kind, who, a, b, t0_ms, t1_ms):
    return f"{kind} {who} {a:x} {b:x} {t0_ms * 1000:.0f} {t1_ms * 1000:.0f}"


def trace(lines, dropped=0):
    head = [f"# ps3_ftrace start_us=0 end_us=200000 spans=0 dropped={dropped}", "T 1 main", "T 2 audio"]
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

# Drawables and label-store waits: four flip requests (0, 20, 40, 60 ms), so every rate divides by 4.
DRAIN, GPU_ENCODE, GPU_WAIT, DRAWABLE, LABEL_WAIT = 7, 11, 12, 16, 17
plines = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 20, 40, 60)]
# nextDrawable: who 1 = engine, 2 = legacy; a bit 0 nil, bit 1 another surface; b = records since the last blit.
plines += [span(DRAWABLE, 1, 2, 5, 1, 2),      # new content, 1 ms
           span(DRAWABLE, 1, 2, 1, 21, 23),    # new content, 2 ms
           span(DRAWABLE, 1, 0, 0, 3, 4),      # held, same surface, 1 ms
           span(DRAWABLE, 1, 2, 0, 5, 6),      # held, another surface, 1 ms
           span(DRAWABLE, 1, 0, 0, 41, 44),    # held, 3 ms
           span(DRAWABLE, 1, 1, 0, 45, 46),    # nil: counted as nil, not as held
           span(DRAWABLE, 2, 0, 4, 61, 62)]    # legacy path, new content
# The walker's own spans around the waits. The in-flight wait counts; a present or flush wait does not.
plines += [span(DRAIN, 0, 0, 0, 10, 30), span(GPU_WAIT, 0, 2, 0, 12, 14), span(GPU_ENCODE, 0, 1, 3, 14, 16),
           span(PRESENT, 0, 0, 0, 15, 18), span(GPU_WAIT, 0, 1, 0, 50, 53)]
# Label-store waits: who, a = label offset, b = spins | mid-pass << 16.
plines += [span(LABEL_WAIT, 3, 0x450, 0x10, 0.5, 2.5),           # 2 ms: 1 in the drawable, 1 in no span
           span(LABEL_WAIT, 3, 0x450, 100 | 1 << 16, 11, 20),    # 9 ms: 2 in-flight, 2 encode (15-16 is
                                                                 #   encode's, not present's), 2 present, 3 drain
           span(LABEL_WAIT, 3, 0x450, 65, 20, 24),               # 4 ms: 2 in the drawable (inside the drain), 2 drain
           span(LABEL_WAIT, 3, 0x450, 0, 50, 53)]                # 3 ms: a present's gpu wait is no walker span here

with tempfile.TemporaryDirectory() as tmp:
    rc, out, err = run(tmp, trace(plines), "--json")
    check(rc == 0, f"drawables trace: exit 0: {err}")
    r = json.loads(out)
    check(r["flip_requests"] == 4 and r["rates"] is True and r["dropped"] == 0, "four flip requests, rates on")
    d = r["drawables"]
    check(d["count"] == 7 and d["per_flip"] == 7 / 4, "seven drawables, 1.75 per flip")
    check(d["new_content"] == {"count": 3, "per_flip": 3 / 4}, "three with new content")
    check(d["held"]["count"] == 3 and d["held"]["per_flip"] == 3 / 4, "three without new content")
    check(d["held"]["other_src"] == 1 and abs(d["held"]["other_src_share"] - 1 / 3) < 1e-9,
          "one of the b == 0 drawables shows another surface")
    check(d["nil"] == {"count": 1, "per_flip": 1 / 4}, "a nil drawable is counted as nil even with b == 0")
    check(abs(d["mean_wait_ms"] - 10 / 7) < 1e-9, "mean drawable wait 10/7 ms")
    w = r["label_wait"]
    check(w["count"] == 4 and w["per_flip"] == 1.0 and abs(w["ms_per_flip"] - 4.5) < 1e-9,
          "four label waits, 18 ms in all: 1.0 and 4.5 ms per flip")
    check(w["p50_ms"] == 3 and w["p99_ms"] == 9 and w["max_ms"] == 9, "nearest-rank p50 3, p99 and max 9 ms")
    check(w["spins_over_64"] == 0.5 and w["mid_pass"] == 0.25, "two of four slept, one began mid-pass")
    att = w["attribution"]["ms_per_flip"]
    want = {"drawable": 3, "gpu wait (in-flight)": 2, "encode": 2, "present": 2, "drain": 5, "rest": 4}
    check(all(abs(att[k] - v / 4) < 1e-9 for k, v in want.items()), f"attribution ms per flip {att}")
    check(abs(sum(att.values()) - w["ms_per_flip"]) < 1e-9, "every wait millisecond is charged exactly once")
    check(abs(w["attribution"]["share"]["drawable"] - 3 / 18) < 1e-9, "drawable share 3/18")

    rc, out, err = run(tmp, trace(plines, dropped=7), "--json")
    r = json.loads(out)
    check(rc == 0 and r["rates"] is False and r["dropped"] == 7, "dropped spans: rates off")
    check(r["drawables"]["per_flip"] is None and r["drawables"]["held"]["per_flip"] is None and
          r["label_wait"]["per_flip"] is None and r["label_wait"]["ms_per_flip"] is None and
          r["label_wait"]["attribution"]["ms_per_flip"]["drawable"] is None, "dropped spans: no rate anywhere")
    check(r["drawables"]["count"] == 7 and r["label_wait"]["count"] == 4 and
          abs(r["label_wait"]["attribution"]["share"]["drawable"] - 3 / 18) < 1e-9, "dropped spans: counts and shares stay")
    rc, out, err = run(tmp, trace(plines, dropped=7))
    check(rc == 0 and "7 spans dropped: per-flip rates withheld" in out and "/flip" not in out,
          "text report says why there are no rates and prints none")

    rc, out, err = run(tmp, trace(plines))
    check(rc == 0 and "Drawables (nextDrawable)" in out and "Label-store waits" in out and
          "1.75 /flip" in out and "4.50 ms/flip" in out and
          "drawable + gpu wait (in-flight) explain 28 % of the wait time" in out and
          "of those another surface: 1 (33 %)" in out, "text report: both sections with their rates and gate figures")

    rc, out, err = run(tmp, trace(lines), "--json")
    r = json.loads(out)
    check(r["drawables"]["count"] == 0 and r["drawables"]["mean_wait_ms"] is None and
          r["label_wait"]["count"] == 0 and r["label_wait"]["p50_ms"] is None, "no such spans: zero counts, no statistics")
    rc, out, err = run(tmp, trace(lines))
    check(rc == 0 and out.count("none recorded") == 2, "text report: both sections say none recorded")
    check(r.get("audio") is None, "trace without audio spans: audio key is None")
    check(rc == 0 and "no audio spans" in out, "text report: says no audio spans")

    # Steady 187.5 blocks/s with deep queue (0 underruns)
    flips = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 200, 400, 600)]
    steady_lines = list(flips)
    dt = 16.0 / 3.0
    for i in range(96):
        t0 = i * dt
        t1 = t0 + 0.5
        steady_lines.append(span(AUDIO_MIX, 0, 1, 1024, t0, t1))
        steady_lines.append(span(AUDIO_PACE, 0, 1, 1024, t1, t1 + 4.8))

    rc, out, err = run(tmp, trace(steady_lines), "--json")
    r = json.loads(out)
    aud = r["audio"]
    check(abs(aud["blocks_per_s"] - 187.5) < 0.01, "steady audio: exactly 187.5 blocks/s")
    check(aud["underruns"]["count"] == 0, "steady audio: 0 underruns")
    check(aud["underruns"]["rate"] == 0.0, "steady audio: 0.0 underrun rate")
    check(abs(aud["queue_depth_ms"]["p50"] - 1024 / 48.0) < 1e-4, "steady audio: queue depth p50 ~21.33 ms")
    check(abs(aud["mix_interval_ms"]["p50"] - dt) < 0.01, "steady audio: mix interval p50 ~5.33 ms")
    check(aud["pacing"]["by_reason"]["queue full"]["count"] == 96, "steady audio: 96 pacing waits")
    check(abs(aud["pacing"]["by_reason"]["queue full"]["ms"] - 96 * 4.8) < 1e-4, "steady audio: pacing total ms")
    rc, out, err = run(tmp, trace(steady_lines))
    check(rc == 0 and "187.5 blocks/s" in out and "underruns: 0" in out, "steady audio text report")

    # Starving queue with periodic underruns
    starve_lines = list(flips)
    for i in range(60):
        t0 = i * dt
        t1 = t0 + 0.5
        if i > 0 and i % 10 == 0:
            starve_lines.append(span(AUDIO_UNDERRUN, 0, 80, 10, t0, t0))
        starve_lines.append(span(AUDIO_MIX, 0, 1, 100, t0, t1))
        starve_lines.append(span(AUDIO_PACE, 0, 3, 100, t1, t1 + 4.8))

    rc, out, err = run(tmp, trace(starve_lines), "--json")
    r = json.loads(out)
    aud = r["audio"]
    check(aud["underruns"]["count"] == 5, "starving queue: 5 underruns")
    check(aud["underruns"]["rate"] is not None and aud["underruns"]["rate"] > 0, "starving queue: positive underrun rate")
    check(abs(aud["queue_depth_ms"]["p1"] - 100 / 48.0) < 1e-4, "starving queue: p1 depth ~2.08 ms")

    # Underruns aligned with long frame gaps vs not aligned
    corr_frames = [0, 20, 40, 100, 120, 140, 160, 180, 200, 220, 240, 260]
    corr_lines = [span(FLIP_REQ, 1, 0, 0, t, t) for t in corr_frames[:4]]
    for t in corr_frames:
        corr_lines.append(span(PRESENT, 0, 0, 0, t, t + 1))
    # Underrun 1 at t = 80 ms: inside 60ms gap (40..100). Window [30, 80] overlaps 60ms gap. ALIGNED.
    corr_lines.append(span(AUDIO_UNDERRUN, 0, 0, 10, 80, 80))
    # Underrun 2 at t = 200 ms: window [150, 200], all gaps are 20ms <= 40ms, no drawable/label_wait. NOT ALIGNED.
    corr_lines.append(span(AUDIO_UNDERRUN, 0, 0, 20, 200, 200))
    # DRAWABLE span at 235..240 ms
    corr_lines.append(span(DRAWABLE, 1, 0, 0, 235, 240))
    # Underrun 3 at t = 250 ms: window [200, 250] overlaps DRAWABLE at 235..240. ALIGNED.
    corr_lines.append(span(AUDIO_UNDERRUN, 0, 0, 10, 250, 250))
    corr_lines.append(span(AUDIO_MIX, 0, 1, 500, 0, 1))

    rc, out, err = run(tmp, trace(corr_lines), "--json")
    r = json.loads(out)
    aud = r["audio"]
    u_dets = aud["correlation"]["underruns"]
    check(len(u_dets) == 3, "correlation: 3 underruns")
    check(abs(u_dets[0]["nearest_frame_gap_ms"] - 60.0) < 1e-4, "underrun 1: nearest gap 60 ms")
    check(u_dets[0]["overlap"] is True, "underrun 1: overlap True")
    check(abs(u_dets[1]["nearest_frame_gap_ms"] - 20.0) < 1e-4, "underrun 2: nearest gap 20 ms")
    check(u_dets[1]["overlap"] is False, "underrun 2: overlap False")
    check(u_dets[2]["overlap"] is True, "underrun 3: overlap True via DRAWABLE")
    check(abs(aud["correlation"]["overlap_share"] - 2 / 3) < 1e-4, "correlation overlap_share is 2/3")
    check(aud["correlation"]["overlap_count"] == 2, "correlation overlap_count is 2")

    # Stale port blocks: silent vs non-silent, aligned vs unaligned with long HLE spans
    stale_frames = [0, 20, 40, 100, 120, 140, 160, 180, 200, 220, 240, 260, 280, 300]
    stale_test_lines = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 20, 40, 60)]
    for t in stale_frames:
        stale_test_lines.append(span(PRESENT, 0, 0, 0, t, t + 1))
    stale_test_lines.append(span(AUDIO_MIX, 0, 1, 500, 0, 1))
    # Long HLE span (25 ms > 10 ms) on tid 2 in Port2Sync (0xFACB3CED) at 70..95 ms
    stale_test_lines.append(span(HLE, 2, 0xFACB3CED, 0x1000, 70, 95))
    # Event 1: Port 0, block 2, AUDIBLE (b=1), at t = 80 ms: aligned with 60ms gap & long HLE
    stale_test_lines.append(span(AUDIO_STALE, 0, 2, 1, 80, 80))
    # Event 2: Port 0, block 3, SILENT (b=0), at t = 90 ms: silent block
    stale_test_lines.append(span(AUDIO_STALE, 0, 3, 0, 90, 90))
    # Event 3: Port 1, block 5, AUDIBLE (b=1), at t = 210 ms: 20ms gaps, no HLE
    stale_test_lines.append(span(AUDIO_STALE, 1, 5, 1, 210, 210))

    rc, out, err = run(tmp, trace(stale_test_lines), "--json")
    r = json.loads(out)
    aud = r["audio"]
    st = aud["stale"]
    check(st["count"] == 3, "stale count: 3")
    check(st["audible_count"] == 2, "stale audible: 2")
    check(st["silent_count"] == 1, "stale silent: 1")
    check(st["by_port"]["0"]["count"] == 2 and st["by_port"]["0"]["audible"] == 1 and st["by_port"]["0"]["silent"] == 1,
          "stale port 0 counts")
    check(st["by_port"]["1"]["count"] == 1 and st["by_port"]["1"]["audible"] == 1 and st["by_port"]["1"]["silent"] == 0,
          "stale port 1 counts")
    st_evs = st["events"]
    check(len(st_evs) == 3, "stale events: 3")
    # Event 1 (audible, at 80ms)
    check(abs(st_evs[0]["nearest_frame_gap_ms"] - 60.0) < 1e-4, "stale 1: nearest gap 60 ms")
    check(st_evs[0]["overlap"] is True, "stale 1: overlap True")
    check(len(st_evs[0]["hle_waits"]) == 1 and st_evs[0]["hle_waits"][0]["nid"] == "FACB3CED",
          "stale 1: long HLE wait overlap Port2Sync")
    # Event 3 (audible, at 210ms)
    check(abs(st_evs[2]["nearest_frame_gap_ms"] - 20.0) < 1e-4, "stale 3: nearest gap 20 ms")
    check(st_evs[2]["overlap"] is False, "stale 3: overlap False")
    check(len(st_evs[2]["hle_waits"]) == 0, "stale 3: no HLE wait overlap")
    check(abs(st["overlap_share"] - 0.5) < 1e-4, "stale audible overlap_share is 0.5")
    check(len(aud["correlation"]["top_hle_waits"]) >= 1 and
          aud["correlation"]["top_hle_waits"][0]["nid"] == "FACB3CED",
          "top_hle_waits names Port2Sync")

    rc, out, err = run(tmp, trace(stale_test_lines))
    check(rc == 0 and "stale blocks: 3 (2 audible" in out and "port 0: 1 audible, 1 silent" in out,
          "stale blocks text report")
    check(rc == 0 and "top overlapping long guest waits" in out and "Port2Sync" in out,
          "text report names overlapping HLE import")
print("ftrace_report: %s" % ("ok" if not failures else "%d failed" % failures))
sys.exit(1 if failures else 0)
