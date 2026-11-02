#!/usr/bin/env python3
"""ftrace_report: frame binning, import call sites, split, cadence, drawables, label-store waits, jq2 job queues, image identity and critical path, bad input."""
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

    # Stale port blocks: gap after playing audio with growing run length, resets on refill,
    # aligned vs unaligned with long HLE spans
    stale_frames = [0, 20, 40, 100, 120, 140, 160, 180, 200, 220, 240, 260, 280, 300]
    stale_test_lines = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 20, 40, 60)]
    for t in stale_frames:
        stale_test_lines.append(span(PRESENT, 0, 0, 0, t, t + 1))
    stale_test_lines.append(span(AUDIO_MIX, 0, 1, 500, 0, 1))

    # Long HLE span (25 ms > 10 ms) on tid 2 in Port2Sync (0xFACB3CED) at 70..95 ms
    stale_test_lines.append(span(HLE, 2, 0xFACB3CED, 0x1000, 70, 95))

    # Gap run 1 on Port 0: growing run length 1, 2, 3 (at 80, 85, 90 ms)
    # Event 1 (run start, b=1) at 80 ms: overlaps 60ms frame gap [40, 100] and long HLE [70, 95]
    stale_test_lines.append(span(AUDIO_STALE, 0, 2, 1, 80, 80))
    stale_test_lines.append(span(AUDIO_STALE, 0, 3, 2, 85, 85))
    stale_test_lines.append(span(AUDIO_STALE, 0, 4, 3, 90, 90))

    # Gap run 2 on Port 1: growing run length 1, 2 (at 210, 215 ms)
    # Event 4 (run start, b=1) at 210 ms: unaligned with >40ms gap (20ms gaps) and no HLE
    stale_test_lines.append(span(AUDIO_STALE, 1, 5, 1, 210, 210))
    stale_test_lines.append(span(AUDIO_STALE, 1, 6, 2, 215, 215))

    rc, out, err = run(tmp, trace(stale_test_lines), "--json")
    r = json.loads(out)
    aud = r["audio"]
    st = aud["stale"]
    check(st["total_blocks"] == 5, "stale total blocks: 5")
    check(st["runs"] == 2, "stale runs: 2")
    check(st["longest_run_blocks"] == 3, "stale longest run blocks: 3")
    check(abs(st["longest_run_ms"] - 3 * (16.0 / 3.0)) < 1e-4, "stale longest run ms: 16.0")
    check(st["by_port"]["0"]["runs"] == 1 and st["by_port"]["0"]["total_blocks"] == 3 and st["by_port"]["0"]["longest_run_blocks"] == 3,
          "stale port 0: 1 run, 3 blocks, longest 3")
    check(st["by_port"]["1"]["runs"] == 1 and st["by_port"]["1"]["total_blocks"] == 2 and st["by_port"]["1"]["longest_run_blocks"] == 2,
          "stale port 1: 1 run, 2 blocks, longest 2")
    st_evs = st["events"]
    check(len(st_evs) == 5, "stale events: 5")
    check(st_evs[0]["run_len"] == 1 and st_evs[1]["run_len"] == 2 and st_evs[2]["run_len"] == 3,
          "growing run length 1, 2, 3")
    check(st_evs[3]["run_len"] == 1 and st_evs[4]["run_len"] == 2, "run resets to 1, 2 on next gap")
    # Event 1 (run start at 80ms)
    check(abs(st_evs[0]["nearest_frame_gap_ms"] - 60.0) < 1e-4, "stale run 1: nearest gap 60 ms")
    check(st_evs[0]["overlap"] is True, "stale run 1: overlap True")
    check(len(st_evs[0]["hle_waits"]) == 1 and st_evs[0]["hle_waits"][0]["nid"] == "FACB3CED",
          "stale run 1: long HLE wait overlap Port2Sync")
    # Event 4 (run start at 210ms)
    check(abs(st_evs[3]["nearest_frame_gap_ms"] - 20.0) < 1e-4, "stale run 2: nearest gap 20 ms")
    check(st_evs[3]["overlap"] is False, "stale run 2: overlap False")
    check(len(st_evs[3]["hle_waits"]) == 0, "stale run 2: no HLE wait overlap")
    check(abs(st["overlap_share"] - 0.5) < 1e-4, "stale gap runs overlap_share is 0.5 (1/2)")
    check(len(aud["correlation"]["top_hle_waits"]) >= 1 and
          aud["correlation"]["top_hle_waits"][0]["nid"] == "FACB3CED",
          "top_hle_waits names Port2Sync")

    rc, out, err = run(tmp, trace(stale_test_lines))
    check(rc == 0 and "stale blocks: 5 across 2 audible gap run(s)" in out, "stale blocks text report")
    check(rc == 0 and "port 0: 1 run(s), 3 block(s), max 3 blk" in out, "port 0 text report")
    check(rc == 0 and "top overlapping long guest waits" in out and "Port2Sync" in out,
          "text report names overlapping HLE import")

# jq2 job queues: queue, worker and seq live in who/a/b; a fiber wait has no waiter id.
JQ2_PUSH, JQ2_JOB, JQ2_CREDIT, JQ2_SYNC_WAIT, JQ2_IDLE = 22, 23, 24, 25, 26
JQ2_HELD, JQ2_BARRIER = 27, 28
EVF, FIBER = 1, 2


def close(a, b, tol=1e-9):
    return a is not None and abs(a - b) <= tol


def qword(q, w):
    return q << 8 | w


def jq2_trace(lines, dropped=0, header=None):
    head = [header or f"# ps3_ftrace start_us=0 end_us=200000 spans=0 dropped={dropped}",
            "T 1 main", "T 2 audio", "T 10 encoder"]
    return "\n".join(head + lines) + "\n"


# Two queues with two workers each, both numbering their jobs from 1. Queue 0's window
# is 0-40 ms (3 pushes, 3 jobs, 3 credited), queue 1's is 100-115.5 ms (2 pushes, 2 jobs).
# Waiter 3 is handed out twice in queue 0, so the second wait must pair with the second
# credit, not the first.
jq2 = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 100, 200)]
jq2 += [span(JQ2_PUSH, 1, 1, 0 | 1 << 8, 0, 1),                       # seq 1, depth 1
        span(JQ2_JOB, qword(0, 0), 1, 2000, 2, 6),                    # latency 1, wall 4, CPU 2 ms
        span(JQ2_CREDIT, qword(0, 0), 1, 3 | EVF << 8 | 1 << 16, 6.2, 6.5),
        span(JQ2_SYNC_WAIT, 1, 3 | EVF << 8, 0, 6.1, 8.6),            # sync latency 2.1, wait 2.5
        span(JQ2_PUSH, 1, 2, 0 | 2 << 8, 10, 11),
        span(JQ2_JOB, qword(0, 1), 2, 3000, 12, 18),                  # latency 1, wall 6, CPU 3 ms
        span(JQ2_CREDIT, qword(0, 1), 2, 4 | EVF << 8 | 1 << 16, 18.1, 18.25),
        span(JQ2_SYNC_WAIT, 2, 4 | EVF << 8, 0, 18.05, 19.3),         # sync latency 1.05, wait 1.25
        span(JQ2_PUSH, 1, 3, 0 | 1 << 8, 30, 31),
        span(JQ2_JOB, qword(0, 0), 3, 1000, 32.5, 34.5),              # latency 1.5, wall 2, CPU 1 ms
        span(JQ2_CREDIT, qword(0, 0), 3, 3 | EVF << 8 | 1 << 16, 34.6, 34.7),
        span(JQ2_SYNC_WAIT, 1, 3 | EVF << 8, 0, 34.55, 35.8),         # waiter 3 again, sync latency 1.1
        span(JQ2_IDLE, qword(0, 0), 1, 0, 6.4, 9.4),                  # parked with work pending: 3 ms
        span(JQ2_IDLE, qword(0, 1), 0, 0, 18.5, 40)]                  # idle 21.5 ms, no depth: not parked
jq2 += [span(JQ2_PUSH, 10, 1, 1 | 1 << 8, 100, 101),                  # queue 1, seq 1 again
        span(JQ2_PUSH, 10, 2, 1 | 2 << 8, 110, 111),
        span(JQ2_JOB, qword(1, 0), 1, 4000, 103, 109),                # latency 2, wall 6, CPU 4 ms
        span(JQ2_CREDIT, qword(1, 0), 1, 5 | EVF << 8 | 1 << 16, 109.5, 109.8),
        span(JQ2_SYNC_WAIT, 10, 5 | EVF << 8, 1, 109.4, 112.9),        # sync latency 3.1
        span(JQ2_JOB, qword(1, 1), 2, 500, 112, 113),                 # latency 1, wall 1, CPU 0.5 ms
        span(JQ2_CREDIT, qword(1, 1), 2, 0xFF, 113.2, 113.3),         # nobody waited
        span(JQ2_IDLE, qword(1, 0), 0, 0, 113.5, 115.5)]              # idle 2 ms

with tempfile.TemporaryDirectory() as tmp:
    rc, out, err = run(tmp, jq2_trace(jq2), "--json")
    check(rc == 0, f"jq2 trace: exit 0: {err}")
    r = json.loads(out)
    jq = r["jq2"]
    check(jq["dropped"] == 0 and [q["q"] for q in jq["queues"]] == [0, 1],
          "jq2: two queues, in queue order, no dropped spans")
    q0, q1 = jq["queues"]
    need = {"q", "window", "window_source", "pushes", "push_rate_per_s", "jobs", "workers",
            "queue_latency_ms",
            "queue_latency_clamped", "exec_wall_ms", "exec_cpu_ms", "completion_delay_ms",
            "sync_latency_ms", "cpu_wall", "per_worker", "parked_while_pending", "depth",
            "sync_wait_by_tid", "fiber_sync_wait", "join_failures"}
    check(need <= set(q0), f"jq2 keys present: {sorted(need - set(q0))}")
    stats = {"count", "mean", "p50", "p95", "p99", "max"}
    check(all(stats <= set(q0[k]) for k in ("queue_latency_ms", "exec_wall_ms", "exec_cpu_ms",
                                            "completion_delay_ms", "sync_latency_ms")),
          "jq2 latency rows carry count, mean, p50, p95, p99 and max")
    check(q0["pushes"] == 3 and q0["jobs"] == 3 and q0["workers"] == 2, "q0: 3 pushes, 3 jobs, 2 workers")
    check(q0["window_source"] == "header" and close(q0["window"]["start_ms"], 0) and
          close(q0["window"]["end_ms"], 200) and close(q0["window"]["ms"], 200) and
          close(q0["push_rate_per_s"], 15), "q0: the header's 0-200 ms window, 15 pushes/s")
    lat = q0["queue_latency_ms"]
    check(lat["count"] == 3 and close(lat["mean"], 3.5 / 3) and close(lat["p50"], 1) and
          close(lat["p95"], 1.5) and close(lat["max"], 1.5), f"q0 queue latency {lat}")
    check(q0["queue_latency_clamped"] == 0, "q0: no queue latency clamped")
    wall, cpu = q0["exec_wall_ms"], q0["exec_cpu_ms"]
    check(wall["count"] == 3 and close(wall["mean"], 4) and close(wall["p95"], 6) and close(wall["max"], 6),
          f"q0 execution wall {wall}")
    check(cpu["count"] == 3 and close(cpu["mean"], 2) and close(cpu["p50"], 2) and close(cpu["max"], 3),
          f"q0 execution CPU {cpu}")
    comp = q0["completion_delay_ms"]
    check(comp["count"] == 3 and close(comp["mean"], 0.4 / 3) and close(comp["p50"], 0.1) and
          close(comp["p95"], 0.2) and close(comp["max"], 0.2), f"q0 completion delay {comp}")
    syn = q0["sync_latency_ms"]
    check(syn["count"] == 3 and close(syn["mean"], 4.25 / 3) and close(syn["p50"], 1.1) and
          close(syn["p95"], 2.1) and close(syn["max"], 2.1), f"q0 sync latency {syn}")
    check(close(q0["cpu_wall"], 0.5), f"q0 cpu/wall {q0['cpu_wall']}")
    by_w = {p["w"]: p for p in q0["per_worker"]}
    check(sorted(by_w) == [0, 1] and by_w[0]["jobs"] == 2 and by_w[1]["jobs"] == 1, "q0: both workers seen")
    check(close(by_w[0]["busy_ms"], 6) and close(by_w[0]["utilisation"], 6 / 200) and
          close(by_w[0]["idle_ms"], 3) and close(by_w[0]["idle_fraction"], 3 / 200),
          f"q0 worker 0 busy/idle over the header window {by_w[0]}")
    check(close(by_w[1]["busy_ms"], 6) and close(by_w[1]["utilisation"], 6 / 200) and
          close(by_w[1]["idle_ms"], 21.5) and close(by_w[1]["idle_fraction"], 21.5 / 200),
          f"q0 worker 1 busy/idle over the header window {by_w[1]}")
    pk = q0["parked_while_pending"]
    check(pk["count"] == 1 and close(pk["total_ms"], 3), f"q0 parked while pending {pk}")
    dep = q0["depth"]
    check(dep["count"] == 3 and close(dep["p50"], 1) and close(dep["p95"], 2) and close(dep["max"], 2),
          f"q0 queue depth {dep}")
    waits = {row["tid"]: row for row in q0["sync_wait_by_tid"]}
    check(sorted(waits) == [1, 2] and waits[1]["name"] == "main" and waits[1]["count"] == 2 and
          close(waits[1]["total_ms"], 3.75) and close(waits[1]["p50"], 1.25) and
          close(waits[1]["p95"], 2.5) and close(waits[1]["p99"], 2.5),
          f"q0 sync waits on tid 1 {waits.get(1)}")
    check(waits[2]["count"] == 1 and close(waits[2]["total_ms"], 1.25), f"q0 sync waits on tid 2 {waits.get(2)}")
    check(q0["fiber_sync_wait"]["count"] == 0 and q0["fiber_sync_wait"]["p50"] is None,
          "q0: no fiber waits")
    jf = q0["join_failures"]
    check(jf["queue_latency"] == {"matched": 3, "push_without_job": 0, "job_without_push": 0} and
          jf["completion_delay"] == {"matched": 3, "job_without_credit": 0, "credit_without_job": 0} and
          jf["sync_latency"] == {"matched": 3, "wait_without_credit": 0, "credit_without_wait": 0},
          f"q0: every join found its partner {jf}")

    # The same seq numbers in queue 1: a join on seq alone would pair these with queue 0's.
    check(q1["pushes"] == 2 and q1["jobs"] == 2 and q1["workers"] == 2, "q1: 2 pushes, 2 jobs, 2 workers")
    check(q0["window"] == q1["window"] and q1["window_source"] == "header" and
          close(q1["window"]["ms"], 200) and close(q1["push_rate_per_s"], 2 / 0.2),
          "q1: the same header window as queue 0, 10 pushes/s")
    by_w1 = {p["w"]: p for p in q1["per_worker"]}
    check(close(by_w1[0]["utilisation"], 6 / 200) and close(by_w1[1]["utilisation"], 1 / 200) and
          close(by_w1[0]["idle_fraction"], 2 / 200),
          f"q1: utilisation over the header window, not the queue's 15.5 ms extent {q1['per_worker']}")
    check(close(q1["queue_latency_ms"]["mean"], 1.5) and close(q1["queue_latency_ms"]["p50"], 1) and
          close(q1["queue_latency_ms"]["p95"], 2) and q1["queue_latency_clamped"] == 0,
          f"q1 queue latency {q1['queue_latency_ms']}")
    check(close(q1["exec_wall_ms"]["mean"], 3.5) and close(q1["exec_wall_ms"]["max"], 6) and
          close(q1["exec_cpu_ms"]["mean"], 2.25) and close(q1["exec_cpu_ms"]["max"], 4),
          "q1 execution wall and CPU")
    check(close(q1["completion_delay_ms"]["mean"], 0.35) and close(q1["sync_latency_ms"]["mean"], 3.1) and
          close(q1["cpu_wall"], 4.5 / 7), "q1 completion delay, sync latency, cpu/wall")
    check([row["tid"] for row in q1["sync_wait_by_tid"]] == [10] and
          q1["sync_wait_by_tid"][0]["name"] == "encoder" and
          close(q1["sync_wait_by_tid"][0]["total_ms"], 3.5), "q1: sync wait on the encoder thread")

    rc, out, err = run(tmp, jq2_trace(jq2))
    check(rc == 0 and "SPURS job queues (jq2):" in out and "recorder dropped spans: 0" in out and
          "queue 0: 3 pushes, 3 jobs, 2 workers" in out and "queue latency (push->job)" in out and
          "cpu/wall 0.50" in out, "jq2 text: section with queue 0's counts and cpu/wall")
    check("window 0.00-200.00 ms (200.00 ms, header)" in out and
          "parked while work was pending: 1 spans, 3.00 ms" in out and
          "fiber sync waits (no waiter, not joined): n 0" in out,
          "jq2 text: the header window and its source, parked-while-pending, fiber row")
    check("join failures: push->job 0 (0 push without a job, 0 job without a push)" in out,
          "jq2 text: join failures")

    # Without a window in the header the shares fall back to each queue's own extent.
    rc, out, err = run(tmp, jq2_trace(jq2, header="# ps3_ftrace spans=0 dropped=0"), "--json")
    r = json.loads(out)
    q0, q1 = r["jq2"]["queues"]
    check(q0["window_source"] == "extent" and q1["window_source"] == "extent" and
          close(q0["window"]["ms"], 40) and close(q1["window"]["ms"], 15.5),
          "jq2: a header without a window falls back to each queue's extent")
    check(close(q0["push_rate_per_s"], 75) and close(q1["push_rate_per_s"], 2 / 0.0155),
          "jq2: push rate over the extent when the header names no window")
    check(close(q0["per_worker"][0]["utilisation"], 6 / 40) and
          close(q0["per_worker"][0]["idle_fraction"], 3 / 40) and
          close(q1["per_worker"][0]["utilisation"], 6 / 15.5),
          "jq2: utilisation and idle follow the extent without a header window")
    rc, out, err = run(tmp, jq2_trace(jq2, header="# ps3_ftrace spans=0 dropped=0"))
    check(rc == 0 and "window 0.00-40.00 ms (40.00 ms, extent)" in out and
          "worker 0: 2 jobs, busy 6.00 ms = 15 %, idle 3.00 ms = 8 %" in out,
          "jq2 text: the extent window and its shares")
    rc, out, err = run(tmp, jq2_trace(jq2, header="# ps3_ftrace start_us=5 end_us=5 spans=0 dropped=0"),
                       "--json")
    check(json.loads(out)["jq2"]["queues"][0]["window_source"] == "extent",
          "jq2: an empty header window counts as no window")

    rc, out, err = run(tmp, jq2_trace(jq2, dropped=7), "--json")
    check(json.loads(out)["jq2"]["dropped"] == 7, "jq2: dropped spans come from the trace header")
    rc, out, err = run(tmp, jq2_trace(jq2, dropped=7))
    check(rc == 0 and "recorder dropped spans: 7" in out, "jq2 text names the recorder's dropped count")

    # Window edges, a clamped queue latency and the fiber path: everything that cannot be joined.
    edge = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 20, 40)]
    edge += [span(JQ2_PUSH, 1, 1, 0 | 1 << 8, 0, 1),
             span(JQ2_JOB, qword(0, 0), 2, 1000, 2, 3),                    # no push: seq 2 never pushed
             span(JQ2_PUSH, 1, 3, 0 | 1 << 8, 4, 5),
             span(JQ2_JOB, qword(0, 0), 3, 500, 4.5, 6.5),                 # admitted before the push ended
             span(JQ2_CREDIT, qword(0, 0), 3, 0xFF, 6.6, 6.7),
             span(JQ2_PUSH, 1, 4, 0 | 1 << 8, 28, 29),                     # no job: cut at the window end
             span(JQ2_SYNC_WAIT, 1, 7 | EVF << 8, 0, 10, 12),              # event-flag wait, no credit
             span(JQ2_SYNC_WAIT, 1, 0xFF | FIBER << 8, 0, 14, 17),         # fiber wait: not joined
             span(JQ2_CREDIT, qword(0, 0), 9, 0xFF | FIBER << 8 | 1 << 16, 20, 20.5),
             span(JQ2_IDLE, qword(0, 0), 0, 0, 7, 30)]
    rc, out, err = run(tmp, jq2_trace(edge), "--json")
    check(rc == 0, f"jq2 edges: exit 0: {err}")
    e = json.loads(out)["jq2"]["queues"][0]
    check(e["queue_latency_ms"]["count"] == 1 and close(e["queue_latency_ms"]["p50"], 0) and
          e["queue_latency_clamped"] == 1, "jq2: a job admitted before its push ends clamps to 0")
    check(e["join_failures"]["queue_latency"] == {"matched": 1, "push_without_job": 2, "job_without_push": 1},
          f"jq2: pushes and jobs cut at the window edges count as failures {e['join_failures']}")
    check(e["join_failures"]["completion_delay"] == {"matched": 1, "job_without_credit": 1,
                                                     "credit_without_job": 1},
          "jq2: a job without a credit and a credit without a job are both counted")
    check(e["join_failures"]["sync_latency"] == {"matched": 0, "wait_without_credit": 1,
                                                 "credit_without_wait": 0},
          f"jq2: an event-flag wait with no credit fails the join; the fiber wake does not {e['join_failures']}")
    fib = e["fiber_sync_wait"]
    check(fib["count"] == 1 and close(fib["total_ms"], 3) and close(fib["p50"], 3) and
          close(fib["p99"], 3), f"jq2: fiber waits reported apart {fib}")
    check(close(e["per_worker"][0]["busy_ms"], 3) and close(e["per_worker"][0]["utilisation"], 3 / 200) and
          e["window_source"] == "header",
          "jq2: worker busy time and utilisation of the header window")

    # Spans reaching past the recorder's window are clipped before they are summed: a worker
    # that parked before start_us, or was still parked at end_us, must not charge time the
    # window does not own.
    clip = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 20, 40)]
    clip += [span(JQ2_JOB, qword(0, 0), 1, 0, 12, 24),          # 12 ms inside a 10-40 ms window
             span(JQ2_IDLE, qword(0, 0), 0, 0, 24, 48),         # ends 8 ms after it: counts 16
             span(JQ2_IDLE, qword(0, 1), 2, 1, 4, 12),          # starts 6 ms before it: counts 2
             span(JQ2_IDLE, qword(0, 1), 1, 1, 0, 5)]           # wholly before it: counts 0
    rc, out, err = run(tmp, jq2_trace(clip, header="# ps3_ftrace start_us=10000 end_us=40000"
                                                  " spans=0 dropped=0"), "--json")
    check(rc == 0, f"jq2 clip: exit 0: {err}")
    c = json.loads(out)["jq2"]["queues"][0]
    check(close(c["window"]["start_ms"], 10) and close(c["window"]["ms"], 30) and
          c["window_source"] == "header", f"jq2 clip: a 10-40 ms header window {c['window']}")
    cw = {p["w"]: p for p in c["per_worker"]}
    check(close(cw[0]["busy_ms"], 12) and cw[0]["jobs"] == 1 and close(cw[0]["utilisation"], 12 / 30),
          f"jq2 clip: worker 0 busy {cw.get(0)}")
    check(close(cw[0]["idle_ms"], 16) and close(cw[0]["idle_fraction"], 16 / 30),
          f"jq2 clip: an idle span ending after the window counts only to its end {cw.get(0)}")
    check(close(cw[1]["idle_ms"], 2) and close(cw[1]["idle_fraction"], 2 / 30),
          f"jq2 clip: an idle span starting before the window counts only from its start {cw.get(1)}")
    check(all(p["busy_ms"] + p["idle_ms"] <= c["window"]["ms"] + 1e-9 for p in c["per_worker"]),
          f"jq2 clip: busy + idle never exceeds the window {c['per_worker']}")
    check(c["parked_while_pending"] == {"count": 2, "total_ms": 2},
          f"jq2 clip: the parked-while-pending total is clipped too {c['parked_while_pending']}")

    # A trace without kinds 22-28 prints no section and has a null key.
    plain = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 20, 40)]
    rc, out, err = run(tmp, trace(plain), "--json")
    check(rc == 0 and json.loads(out)["jq2"] is None, "jq2: trace without the kinds has a null key")
    rc, out, err = run(tmp, trace(plain))
    check(rc == 0 and "SPURS job queues" not in out and "jq2" not in out,
          "jq2: trace without the kinds prints no section")

# Kinds 27/28: held vs ready wait, park reasons, depth buckets, barriers and the legacy note, all
# hand-counted. seq 1 is never held, seq 2 and 3 each wait for a SYNC barrier, seq 4 has no job, seq 5 has
# no push, seq 6 has no job; the parks cover every reason plus a legacy 0. Queue 1 holds one job admissible
# for 20 ms while its two workers cover every kind of ready wait: a worker parked, both workers in a JOB
# span, and neither.
park = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 100, 200)]
park += [
    span(JQ2_PUSH, 1, 1, 0 | 1 << 8, 0, 1),                          # seq 1, depth 1
    span(JQ2_HELD, qword(0, 0), 1, 0, 0.5, 0.5),                     # never held
    span(JQ2_JOB, qword(0, 0), 1, 1000, 2, 6),                       # ready wait 1.5, wall 4
    span(JQ2_PUSH, 1, 2, 0 | 3 << 8, 10, 11),                        # seq 2, depth 3
    span(JQ2_HELD, qword(0, 1), 2, 1 | 1 << 4 | 2 << 8, 10.5, 16),   # tag 1, held 5.5, mask 0x0002
    span(JQ2_JOB, qword(0, 1), 2, 500, 20, 22),                      # ready wait 4
    span(JQ2_PUSH, 1, 3, 0 | 2 << 8, 30, 31),                        # seq 3, depth 2
    span(JQ2_HELD, qword(0, 0), 3, 2 | 1 << 4 | 4 << 8, 30.5, 40),   # tag 2, held 9.5, mask 0x0004
    span(JQ2_JOB, qword(0, 0), 3, 500, 45, 46),                      # ready wait 5
    span(JQ2_PUSH, 1, 4, 0 | 1 << 8, 50, 51),                        # seq 4: no job
    span(JQ2_JOB, qword(0, 1), 5, 500, 60, 61),                      # seq 5: no push, no HELD
    span(JQ2_HELD, qword(0, 0), 6, 0, 12, 14),                       # seq 6: held 2, no job
    span(JQ2_BARRIER, qword(0, 0), 2 | 2 << 16, 0 | 1 << 8, 11, 15),
    span(JQ2_BARRIER, qword(0, 1), 4 | 4 << 16, 0 | 3 << 8, 31, 39.5),
    span(JQ2_IDLE, qword(0, 0), 0 | 1 << 24, 0, 0, 5),               # reason 1 empty, 5 ms
    span(JQ2_IDLE, qword(0, 1), 2 | 5 << 24, 0, 12, 14),             # reason 5 paused, 2 ms
    span(JQ2_IDLE, qword(0, 0), 3 | 3 << 24, 0, 18, 19.5),           # reason 3 held, 1.5 ms
    span(JQ2_IDLE, qword(0, 1), 1 | 4 << 24, 0, 41, 44),             # reason 4 no-slot, 3 ms
    span(JQ2_IDLE, qword(0, 0), 0, 0, 100, 105),                     # legacy reason 0, 5 ms
    span(JQ2_IDLE, qword(0, 1), 2 | 2 << 24, 0, 150, 160),           # reason 2 unwritten, 10 ms
    # Queue 1: 100-102, 104-106 and 110-120 have neither worker parked and not both busy (transition),
    # 102-104 has a worker parked, and 106-110 and 116-118 have both workers inside a JOB span (capacity).
    span(JQ2_PUSH, 1, 1, 1 | 1 << 8, 98, 99),                        # queue 1, seq 1, depth 1
    span(JQ2_HELD, qword(1, 0), 1, 0 | 1 << 4 | 1 << 8, 98.5, 100),  # tag 0, cause 1, mask 0x0001
    span(JQ2_JOB, qword(1, 0), 1, 500, 120, 122),                    # ready wait 20 ms, w0
    span(JQ2_JOB, qword(1, 1), 2, 500, 104, 112),                    # w1 busy 104-112
    span(JQ2_JOB, qword(1, 0), 3, 500, 106, 110),                    # w0 busy 106-110
    span(JQ2_JOB, qword(1, 0), 4, 500, 116, 118),                    # w0 busy 116-118
    span(JQ2_JOB, qword(1, 1), 5, 500, 116, 118),                    # w1 busy 116-118
    span(JQ2_IDLE, qword(1, 0), 1 | 3 << 24, 0, 102, 104),           # w0 parked with work pending, 2 ms
]

with tempfile.TemporaryDirectory() as tmp:
    rc, out, err = run(tmp, jq2_trace(park), "--json")
    check(rc == 0, f"jq2 parking: exit 0: {err}")
    p = json.loads(out)["jq2"]["queues"][0]
    check(p["decomposition_available"] is True, "jq2 parking: the decomposition is available")
    reasons = {r["reason"]: r for r in p["park_reasons"]}
    check(sorted(reasons) == [0, 1, 2, 3, 4, 5], f"jq2 parking: reasons {sorted(reasons)}")
    idle_total = 26.5
    check(close(reasons[0]["total_ms"], 5) and close(reasons[1]["total_ms"], 5) and
          close(reasons[2]["total_ms"], 10) and close(reasons[3]["total_ms"], 1.5) and
          close(reasons[4]["total_ms"], 3) and close(reasons[5]["total_ms"], 2),
          f"jq2 parking: reason totals {[reasons[r]['total_ms'] for r in sorted(reasons)]}")
    check(close(reasons[0]["share"], 5 / idle_total) and close(reasons[2]["share"], 10 / idle_total),
          "jq2 parking: reason shares of the idle time")
    check(reasons[0]["pending_count"] == 0 and reasons[1]["pending_count"] == 0 and
          reasons[2]["pending_count"] == 1 and close(reasons[2]["pending_ms"], 10) and
          reasons[3]["pending_count"] == 1 and close(reasons[3]["pending_ms"], 1.5) and
          reasons[4]["pending_count"] == 1 and reasons[5]["pending_count"] == 1,
          "jq2 parking: the depth in the low 24 bits decides pending")
    check(p["parked_while_pending"] == {"count": 4, "total_ms": 16.5},
          f"jq2 parking: parked while pending {p['parked_while_pending']}")

    dec = p["latency_decomposition"]
    held, ready = dec["held_ms"], dec["ready_wait_ms"]
    check(held["count"] == 3 and close(held["mean"], 5) and close(held["p50"], 5.5) and
          close(held["p95"], 9.5) and close(held["max"], 9.5), f"jq2 parking: held {held}")
    check(ready["count"] == 3 and close(ready["mean"], 3.5) and close(ready["p50"], 4) and
          close(ready["p95"], 5) and close(ready["max"], 5), f"jq2 parking: ready wait {ready}")
    check(close(dec["held_share"], 15 / 25.5) and close(dec["ready_share"], 10.5 / 25.5),
          f"jq2 parking: held/ready shares {dec['held_share']} {dec['ready_share']}")
    check(dec["ready_wait_clamped"] == 0, "jq2 parking: no ready wait clamped")
    rs = dec["ready_wait_split"]
    check(close(rs["parked_ms"], 6) and close(rs["all_busy_ms"], 0) and
          close(rs["transition_ms"], 4.5) and close(rs["total_ms"], 10.5) and
          close(rs["parked_share"], 6 / 10.5) and close(rs["all_busy_share"], 0) and
          close(rs["transition_share"], 4.5 / 10.5),
          f"jq2 parking: ready wait split {rs}")
    check(dec["join_failures"] == {"held_without_job": 1, "job_without_held": 1},
          f"jq2 parking: held joins {dec['join_failures']}")
    ct = p["counts_over_time"]
    check(close(ct["held"]["mean"], 17 / 200) and ct["held"]["max"] == 2,
          f"jq2 parking: held jobs over time {ct['held']}")
    check(close(ct["ready"]["mean"], 10.5 / 200) and ct["ready"]["max"] == 1,
          f"jq2 parking: admissible jobs over time {ct['ready']}")
    buckets = {b["bucket"]: b for b in p["depth_buckets"]}
    check(sorted(buckets) == ["1", "17-64", "2-4", "5-16", "65+"], f"jq2 parking: buckets {sorted(buckets)}")
    check(buckets["1"]["jobs"] == 1 and buckets["1"]["held_jobs"] == 1 and
          buckets["1"]["held_by_barrier"] == 0 and
          close(buckets["1"]["mean_held_ms"], 0) and close(buckets["1"]["mean_ready_wait_ms"], 1.5),
          f"jq2 parking: depth 1 {buckets['1']}")
    check(buckets["2-4"]["jobs"] == 2 and buckets["2-4"]["held_jobs"] == 2 and
          buckets["2-4"]["held_by_barrier"] == 2 and close(buckets["2-4"]["mean_held_ms"], 7.5) and
          close(buckets["2-4"]["mean_ready_wait_ms"], 4.5), f"jq2 parking: depth 2-4 {buckets['2-4']}")
    check(buckets["5-16"]["jobs"] == 0 and buckets["5-16"]["mean_held_ms"] is None,
          "jq2 parking: an empty depth bucket keeps its slot")
    bd = p["barriers"]
    check(bd["count"] == 2 and close(bd["rate_per_s"], 10), f"jq2 parking: barrier count and rate {bd}")
    check(close(bd["duration_ms"]["mean"], 6.25) and close(bd["duration_ms"]["p50"], 4) and
          close(bd["duration_ms"]["p95"], 8.5) and close(bd["duration_ms"]["max"], 8.5) and
          close(bd["waited_mean"], 2), f"jq2 parking: barrier durations {bd}")
    masks = bd["top_masks"]
    check([m["mask"] for m in masks] == [4, 2] and close(masks[0]["total_held_ms"], 9.5) and
          close(masks[1]["total_held_ms"], 5.5) and masks[0]["jobs"] == 1,
          f"jq2 parking: top masks {masks}")

    # Queue 1 covers the three-way split: 20 ms of ready wait, 2 ms with a worker parked, 6 ms with both
    # workers in a JOB span and 12 ms with neither.
    q1 = json.loads(out)["jq2"]["queues"][1]
    check(q1["pushes"] == 1 and q1["jobs"] == 5 and q1["workers"] == 2,
          f"jq2 split queue: {q1['pushes']} pushes, {q1['jobs']} jobs, {q1['workers']} workers")
    dec1 = q1["latency_decomposition"]
    rs1 = dec1["ready_wait_split"]
    check(close(rs1["total_ms"], 20) and close(rs1["parked_ms"], 2) and
          close(rs1["all_busy_ms"], 6) and close(rs1["transition_ms"], 12),
          f"jq2 three-way split: {rs1}")
    check(close(rs1["parked_share"], 0.1) and close(rs1["all_busy_share"], 0.3) and
          close(rs1["transition_share"], 0.6), f"jq2 three-way split shares: {rs1}")
    check(dec1["join_failures"] == {"held_without_job": 0, "job_without_held": 4},
          f"jq2 split queue joins: {dec1['join_failures']}")
    b1 = {b["bucket"]: b for b in q1["depth_buckets"]}["1"]
    check(b1["jobs"] == 1 and b1["held_jobs"] == 1 and b1["held_by_barrier"] == 1 and
          close(b1["mean_held_ms"], 1.5) and close(b1["mean_ready_wait_ms"], 20),
          f"jq2 depth 1 with a HELD span: {b1}")

    rc, out, err = run(tmp, jq2_trace(park))
    check(rc == 0 and "park reason  3 held" in out and "held (visible->admissible)" in out and
          "ready wait split: 6.00 ms with a worker parked = 57 %; 0.00 ms with every worker busy = 0 %;"
          " 4.50 ms in transition = 43 %" in out and
          "ready wait split: 2.00 ms with a worker parked = 10 %; 6.00 ms with every worker busy = 30 %;"
          " 12.00 ms in transition = 60 %" in out and
          "1 with a HELD span (0 held by a barrier)" in out and
          "2 with a HELD span (2 held by a barrier)" in out and
          "1 with a HELD span (1 held by a barrier)" in out and
          "barriers: 2 = 10.00/s" in out and "held/admissible decomposition needs" not in out,
          "jq2 parking text: the decomposition rows")

    # A trace without kinds 27/28 keeps the old section and says the decomposition needs them.
    rc, out, err = run(tmp, jq2_trace(jq2))
    check(rc == 0 and "held/admissible decomposition needs kinds 27-28" in out and "park reason" not in out,
          "jq2 legacy text: one note line, no decomposition rows")
    rc, out, err = run(tmp, jq2_trace(jq2), "--json")
    legacy = json.loads(out)["jq2"]["queues"][0]
    check(legacy["decomposition_available"] is False and legacy["latency_decomposition"] is None and
          legacy["park_reasons"] == [] and legacy["barriers"] is None and
          legacy["counts_over_time"] is None and legacy["depth_buckets"] is None,
          "jq2 legacy: the decomposition keys carry nothing")

# Kinds 29 + J lines: job image identity, the critical path of every event-flag wait, the image
# ranking and the audio stale attribution. Every scenario is hand-counted; each critical-path row
# also asserts that its partition sums to the wait's total, so the invariant is checked per wait
# (each synthetic wait is the only one of its thread).
JQ2_IMAGE = 29
FP_A, FP_B, FP_X = "00000000000000a1", "00000000000000c3", "00000000000000b2"
IMG_HEADER = ("# ps3_ftrace start_us=0 end_us=200000 spans=0 dropped=0"
              " jq2_images=2 jq2_image_overflow=0")


def cp_row(r, tid=1, total=None):
    rows = [row for row in r["jq2"]["critical_path"]["rows"] if row["tid"] == tid]
    check(len(rows) == 1, f"one critical-path row for tid {tid}: {len(rows)}")
    row = rows[0]
    parts = row["partition_ms"]
    check(close(sum(parts.values()), row["total_ms"]),
          f"tid {tid}: the partition sums to the wait ({sum(parts.values())} vs {row['total_ms']})")
    if total is not None:
        check(close(row["total_ms"], total), f"tid {tid}: wait total {row['total_ms']} vs {total}")
    return row


# A barrier chain: job A is held on tag 0 until t=10; the BARRIER at t1=10 was released by worker 0
# after job X ended at 9. A waits (1-20) and its due credit names A. The walk pays X's exec and the
# barrier release, not just A's.
barrier = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 100, 200)]
barrier += [
    span(JQ2_PUSH, 1, 1, 0 | 1 << 8, 0, 1),                        # job X: pushed, never held
    span(JQ2_PUSH, 1, 2, 0 | 1 << 8, 0.5, 1.5),                    # job A: held by the barrier
    span(JQ2_JOB, qword(0, 0), 1, 1000, 4, 9),                     # X: 0.5 ms identification
    span(JQ2_IMAGE, qword(0, 0), 1, 1, 4.5, 5),
    span(JQ2_JOB, qword(0, 0), 2, 2000, 11, 15),                   # A: 0.5 ms identification
    span(JQ2_IMAGE, qword(0, 0), 2, 0, 11.5, 12),
    span(JQ2_HELD, qword(0, 0), 2, 0 | 1 << 4 | 1 << 8, 0.5, 10),  # tag 0, cause 1, mask 0x0001
    span(JQ2_BARRIER, qword(0, 0), 1 | 1 << 16, 0 | 1 << 8, 9, 10),
    span(JQ2_CREDIT, qword(0, 0), 2, 5 | EVF << 8 | 1 << 16, 15.5, 16),
    span(JQ2_SYNC_WAIT, 1, 5 | EVF << 8, 0, 1, 20),
    span(AUDIO_STALE, 0, 7, 1, 12, 12),                            # run 1: inside the wait
    span(AUDIO_STALE, 1, 9, 3, 25, 25),                            # run 3: after it
    f"J 0 {FP_A} 00100000 4096",
    f"J 1 {FP_X} 00200000 2048",
]

with tempfile.TemporaryDirectory() as tmp:
    rc, out, err = run(tmp, jq2_trace(barrier, header=IMG_HEADER), "--json")
    check(rc == 0, f"image barrier chain: exit 0: {err}")
    r = json.loads(out)
    jq = r["jq2"]
    check(jq["images_available"] is True and jq["incomplete"] is False and
          jq["jq2_images"] == 2 and jq["jq2_image_overflow"] == 0,
          "image trace: header counters, complete")
    row = cp_row(r, 1, 19)
    p = row["partition_ms"]
    check(close(p["credit_wake"], 5) and close(p["exec"], 8) and close(p["trace"], 1) and
          close(p["admission"], 4) and close(p["parked"], 0) and
          close(p["barrier_release"], 1) and close(p["push"], 0) and close(p["unresolved"], 0),
          f"barrier chain partition {p}")
    check(row["waits"] == 1 and row["fiber_waits"] == 0 and row["waits_without_credit"] == 0 and
          close(row["mean_chain"], 2) and row["guard_hits"] == 0, "barrier chain counts")
    check(close(row["partition_share"]["exec"], 8 / 19) and
          close(row["partition_share"]["credit_wake"], 5 / 19), "barrier chain shares")
    check(close(jq["critical_path"]["total_ms"], 19) and
          jq["critical_path"]["per_queue"][0]["waits"] == 1,
          "critical path: queue and overall totals")

    imgs = jq["images"]
    check(len(imgs["per_queue"]) == 1 and imgs["per_queue"][0]["q"] == 0, "images: one queue")
    rows = {x["fingerprint"]: x for x in imgs["per_queue"][0]["rows"]}
    a, x = rows[FP_A], rows[FP_X]
    check(a["jobs"] == 1 and close(a["exec_wall_ms"]["total_ms"], 3.5) and
          close(a["exec_wall_ms"]["mean_ms"], 3.5) and close(a["exec_wall_ms"]["p50_ms"], 3.5) and
          close(a["exec_wall_ms"]["p95_ms"], 3.5) and close(a["cpu_ms"], 2),
          f"image A: exec wall is JOB minus IMAGE, CPU is JOB.b {a}")
    check(close(a["queue_latency_mean_ms"], 9.5) and close(a["held_mean_ms"], 9.5) and
          close(a["ready_mean_ms"], 1), f"image A: latency, held and ready means {a}")
    check(close(a["critical_path_ms"], 3.5) and close(a["critical_path_share"], 3.5 / 19) and
          close(x["critical_path_ms"], 4.5) and close(x["critical_path_share"], 4.5 / 19),
          "images: critical path is the exec time charged by the walks")
    check(x["cp_rank"] == 1 and a["cp_rank"] == 2 and a["cpu_rank"] == 1 and x["cpu_rank"] == 2,
          f"images: CP rank and CPU rank side by side; A {a['cp_rank']}/{a['cpu_rank']},"
          f" X {x['cp_rank']}/{x['cpu_rank']}")
    check(x["variants"] == [{"ea": 0x200000, "bytes": 2048}] and x["queues"] == [0],
          f"image X: variant and queue {x['variants']}")
    check(imgs["overall"][0]["fingerprint"] == FP_X and
          imgs["overall"][1]["fingerprint"] == FP_A,
          "images: overall table sorted by critical path")

    aud = jq["audio_stale"]
    check(aud["count"] == 2 and aud["blocks_in_wait"] == 1 and aud["blocks_not_in_wait"] == 1 and
          aud["runs"] == 2 and aud["blocks"] == 4 and aud["runs_ge_2_blocks"] == 1 and
          aud["open_at_start"] == 1,
          f"audio stale: two blocks, one run of 1 and one open run of 3 {aud}")
    check(len(aud["threads"]) == 1 and aud["threads"][0]["tid"] == 1 and
          aud["threads"][0]["blocks_in_wait"] == 1 and
          aud["threads"][0]["blocks_not_in_wait"] == 1 and
          aud["threads"][0]["waits"] == 1, f"audio stale: per-thread counts {aud['threads']}")
    ev = aud["events"][0]
    check(ev["run_pos"] == 1 and ev["run_len"] == 1 and close(ev["run_ms"], 16 / 3) and
          ev["band"] == "1" and
          len(ev["waits"]) == 1 and ev["waits"][0]["q"] == 0 and ev["waits"][0]["seq"] == 2 and
          ev["waits"][0]["terminal"]["fingerprint"] == FP_A and
          ev["waits"][0]["dominant"]["fingerprint"] == FP_X and
          close(ev["waits"][0]["waited_ms"], 11),
          f"audio stale: terminal job and dominant image {ev}")
    check(aud["events"][1]["waits"] == [] and aud["events"][1]["run_pos"] == 3 and
          aud["events"][1]["run_len"] == 3 and aud["events"][1]["band"] == "3-4",
          "audio stale: the block after the wait keeps its position and open run")
    fps = {f["fingerprint"]: f for f in aud["fingerprints"]}
    check(fps[FP_A]["terminal_blocks"] == 1 and fps[FP_A]["dominant_blocks"] == 0 and
          fps[FP_X]["terminal_blocks"] == 0 and fps[FP_X]["dominant_blocks"] == 1,
          f"audio stale: per-fingerprint block counts {aud['fingerprints']}")
    hist = {h["band"]: h for h in aud["histogram"]}
    check(hist["1"]["runs"] == 1 and hist["1"]["blocks"] == 1 and
          hist["3-4"]["runs"] == 1 and hist["3-4"]["blocks"] == 3 and
          close(hist["3-4"]["ms"], 3 * 16 / 3) and close(hist["9+"]["ms"], 0),
          f"audio stale: run-length histogram counts runs, blocks are run lengths {hist}")

    tc = jq["trace_cost"]
    check(tc["dropped"] == 0 and tc["jq2_images"] == 2 and tc["jq2_image_overflow"] == 0 and
          len(tc["image_ms"]) == 1 and tc["image_ms"][0]["q"] == 0 and
          close(tc["image_ms"][0]["image_ms"], 1) and close(tc["image_ms"][0]["job_ms"], 9) and
          close(tc["image_ms"][0]["share"], 1 / 9),
          f"trace cost: IMAGE spans as a share of the queue's JOB time {tc}")

    rc, out, err = run(tmp, jq2_trace(barrier, header=IMG_HEADER))
    check(rc == 0 and "critical path of blocking waits" in out and
          "credit+wake 5.00 ms (26 %)" in out and "barrier-release 1.00 ms (5 %)" in out and
          "job images by fingerprint" in out and "CP rank 2, CPU rank 1" in out and
          "audio stale attribution" in out and
          f"terminal {FP_A}" in out and f"dominant {FP_X}" in out and
          "image identification cost" in out and "q0: 1.00 ms of 9.00 ms job time = 11 %" in out,
          "image text report: the new sections")

    # AUDIO_STALE is one event per not-refilled block with b = its position in the run: port 0
    # has a 3-block run then a 1-block run, port 1 a 2-block run interleaved with it, and port 2
    # opens with b = 3 (a run that began before the window). The histogram is runs per band;
    # its blocks are the run lengths, so the open run's earlier blocks count too.
    blockstale = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 100, 200)]
    blockstale += [
        span(JQ2_PUSH, 1, 1, 0 | 1 << 8, 0, 1),
        span(JQ2_JOB, qword(0, 0), 1, 1000, 2, 6),
        span(JQ2_IMAGE, qword(0, 0), 1, 0, 2.2, 2.4),
        span(JQ2_CREDIT, qword(0, 0), 1, 5 | EVF << 8 | 1 << 16, 6.2, 6.5),
        span(JQ2_SYNC_WAIT, 1, 5 | EVF << 8, 0, 1, 10),
        span(AUDIO_STALE, 0, 10, 1, 2, 2),                         # port 0 run 3, block 1
        span(AUDIO_STALE, 1, 20, 1, 2.5, 2.5),                     # port 1 run 2, block 1
        span(AUDIO_STALE, 0, 11, 2, 3, 3),                         # port 0 run 3, block 2
        span(AUDIO_STALE, 1, 21, 2, 3.5, 3.5),                     # port 1 run 2, block 2
        span(AUDIO_STALE, 0, 12, 3, 4, 4),                         # port 0 run 3, block 3
        span(AUDIO_STALE, 2, 30, 3, 6, 6),                         # port 2: opened before 0
        span(AUDIO_STALE, 0, 13, 1, 12, 12),                       # port 0: 1-block run
        f"J 0 {FP_A} 00100000 4096",
    ]
    rc, out, err = run(tmp, jq2_trace(blockstale, header=IMG_HEADER), "--json")
    check(rc == 0, f"stale runs: exit 0: {err}")
    aud = json.loads(out)["jq2"]["audio_stale"]
    check(aud["count"] == 7 and aud["runs"] == 4 and aud["blocks"] == 9 and
          aud["runs_ge_2_blocks"] == 3 and aud["open_at_start"] == 1,
          f"stale runs: 7 blocks, 4 runs, 9 run-length blocks, 3 of 2+ and 1 open {aud}")
    check(aud["blocks_in_wait"] == 6 and aud["blocks_not_in_wait"] == 1,
          f"stale runs: attribution stays per block {aud['blocks_in_wait']}/"
          f"{aud['blocks_not_in_wait']}")
    th = aud["threads"][0]
    check(th["tid"] == 1 and th["waits"] == 1 and th["blocks_in_wait"] == 6 and
          th["blocks_not_in_wait"] == 1, f"stale runs: per-thread blocks {th}")
    hist = {h["band"]: h for h in aud["histogram"]}
    check(hist["1"]["runs"] == 1 and hist["1"]["blocks"] == 1 and
          hist["2"]["runs"] == 1 and hist["2"]["blocks"] == 2 and
          hist["3-4"]["runs"] == 2 and hist["3-4"]["blocks"] == 6 and
          hist["5-8"]["runs"] == 0 and hist["9+"]["runs"] == 0 and
          close(hist["3-4"]["ms"], 32) and
          sum(h["blocks"] for h in hist.values()) == 9,
          f"stale runs: band runs and run-length blocks {hist}")
    ports = {p["port"]: p for p in aud["per_port"]}
    check(ports[0]["runs"] == 2 and ports[0]["blocks"] == 4 and
          ports[0]["runs_ge_2_blocks"] == 1 and ports[0]["open_at_start"] == 0 and
          ports[1]["runs"] == 1 and ports[1]["blocks"] == 2 and
          ports[1]["runs_ge_2_blocks"] == 1 and
          ports[2]["runs"] == 1 and ports[2]["blocks"] == 3 and
          ports[2]["runs_ge_2_blocks"] == 1 and ports[2]["open_at_start"] == 1,
          f"stale runs: per-port runs, blocks, 2+ and open at start {aud['per_port']}")
    check(len(aud["events"]) == 7 and
          aud["events"][0]["port"] == 0 and aud["events"][0]["run_pos"] == 1 and
          aud["events"][0]["run_len"] == 3 and aud["events"][0]["band"] == "3-4" and
          aud["events"][2]["run_pos"] == 2 and aud["events"][3]["port"] == 1 and
          aud["events"][3]["run_pos"] == 2 and aud["events"][3]["run_len"] == 2 and
          aud["events"][5]["port"] == 2 and aud["events"][5]["run_pos"] == 3 and
          aud["events"][5]["run_len"] == 3 and aud["events"][6]["run_pos"] == 1 and
          aud["events"][6]["run_len"] == 1 and aud["events"][6]["waits"] == [],
          "stale runs: each block keeps its run position and its run's length")
    fps = {f["fingerprint"]: f for f in aud["fingerprints"]}
    check(fps[FP_A]["terminal_blocks"] == 6 and fps[FP_A]["dominant_blocks"] == 6,
          f"stale runs: fingerprint block counts {aud['fingerprints']}")
    rc, out, err = run(tmp, jq2_trace(blockstale, header=IMG_HEADER))
    check(rc == 0 and
          "runs: 4, 9 block(s), 3 run(s) of 2+ blocks (10.67 ms or more),"
          " 1 open at the start" in out and
          "run length 3-4:    2 run(s),      6 block(s),     32.00 ms" in out and
          "port 2: 1 run(s), 3 block(s), 1 of 2+ blocks, 1 open at the start" in out and
          "run pos 3 of 3" in out, "stale runs text: runs, bands, per port, positions")

    # Base rate: a thread waits 5 of the 10 ms window (half), so 2 of 4 stale blocks in its
    # waits is exactly chance. The two waits' terminal jobs split that 5 ms as 3 ms of fpA and
    # 2 ms of fpB, so fpA expects 1.2 blocks and fpB 0.8.
    base = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 5, 10)]
    base += [
        span(JQ2_PUSH, 1, 1, 0 | 1 << 8, 0, 0.5),
        span(JQ2_JOB, qword(0, 0), 1, 1000, 1.5, 3.5),
        span(JQ2_IMAGE, qword(0, 0), 1, 0, 1.8, 2),
        span(JQ2_CREDIT, qword(0, 0), 1, 5 | EVF << 8 | 1 << 16, 3.6, 3.9),
        span(JQ2_SYNC_WAIT, 1, 5 | EVF << 8, 0, 1, 4),             # 3 ms, terminal fpA
        span(JQ2_PUSH, 1, 2, 0 | 1 << 8, 4.8, 5.1),
        span(JQ2_JOB, qword(0, 0), 2, 1000, 5.2, 6.5),
        span(JQ2_IMAGE, qword(0, 0), 2, 1, 5.4, 5.6),
        span(JQ2_CREDIT, qword(0, 0), 2, 6 | EVF << 8 | 1 << 16, 6.6, 6.9),
        span(JQ2_SYNC_WAIT, 1, 6 | EVF << 8, 0, 5, 7),             # 2 ms, terminal fpB
        span(AUDIO_STALE, 0, 1, 1, 2, 2),                          # in W1
        span(AUDIO_STALE, 0, 2, 1, 3, 3),                          # in W1
        span(AUDIO_STALE, 0, 3, 1, 8, 8),                          # outside both
        span(AUDIO_STALE, 0, 4, 1, 9, 9),                          # outside both
        f"J 0 {FP_A} 00100000 4096", f"J 1 {FP_B} 00200000 32",
    ]
    base_header = ("# ps3_ftrace start_us=0 end_us=10000 spans=0 dropped=0"
                   " jq2_images=2 jq2_image_overflow=0")
    rc, out, err = run(tmp, jq2_trace(base, header=base_header), "--json")
    check(rc == 0, f"base rate: exit 0: {err}")
    aud = json.loads(out)["jq2"]["audio_stale"]
    check(aud["window"]["source"] == "header" and close(aud["window"]["ms"], 10),
          f"base rate: the header window {aud['window']}")
    th = aud["threads"][0]
    check(th["waits"] == 2 and th["blocks_in_wait"] == 2 and th["blocks_not_in_wait"] == 2 and
          close(th["wait_share"], 0.5) and close(th["expected_blocks_in_wait"], 2.0) and
          close(th["in_wait_ratio"], 1.0),
          f"base rate: half the window, 2 of 4 blocks, ratio 1 {th}")
    fps = {f["fingerprint"]: f for f in aud["fingerprints"]}
    check(fps[FP_A]["terminal_blocks"] == 2 and
          close(fps[FP_A]["terminal_expected_blocks"], 1.2) and
          close(fps[FP_A]["terminal_ratio"], 2 / 1.2) and
          fps[FP_B]["terminal_blocks"] == 0 and
          close(fps[FP_B]["terminal_expected_blocks"], 0.8) and
          close(fps[FP_B]["terminal_ratio"], 0),
          f"base rate: fingerprint terminal expectations {aud['fingerprints']}")
    rc, out, err = run(tmp, jq2_trace(base, header=base_header))
    check(rc == 0 and "expected by chance 2.00 (wait share 50 %), ratio 1.00" in out and
          "expected by chance 1.20 terminal, ratio 1.67" in out and
          "expected by chance 0.80 terminal, ratio 0.00" in out,
          "base rate text: expected counts, wait share and ratios")

    # A capacity link: A was admissible at 6, but worker 0 was still running X until 9, so X's
    # exec and the 9-10 admission are what A waited for.
    capacity = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 100, 200)]
    capacity += [
        span(JQ2_PUSH, 1, 1, 0 | 1 << 8, 0, 0.5),
        span(JQ2_PUSH, 1, 2, 0 | 1 << 8, 0, 1),
        span(JQ2_JOB, qword(0, 0), 1, 1000, 5, 9),
        span(JQ2_IMAGE, qword(0, 0), 1, 1, 5.5, 6),
        span(JQ2_JOB, qword(0, 0), 2, 2000, 10, 15),
        span(JQ2_IMAGE, qword(0, 0), 2, 0, 10.5, 11),
        span(JQ2_HELD, qword(0, 0), 2, 0, 6, 6),                   # cause 0: never held
        span(JQ2_CREDIT, qword(0, 0), 2, 5 | EVF << 8 | 1 << 16, 15.5, 16),
        span(JQ2_SYNC_WAIT, 1, 5 | EVF << 8, 0, 1, 20),
        f"J 0 {FP_A} 00100000 4096", f"J 1 {FP_X} 00200000 2048",
    ]
    rc, out, err = run(tmp, jq2_trace(capacity, header=IMG_HEADER), "--json")
    check(rc == 0, f"capacity link: exit 0: {err}")
    r = json.loads(out)
    row = cp_row(r, 1, 19)
    p = row["partition_ms"]
    check(close(p["credit_wake"], 5) and close(p["exec"], 8) and close(p["trace"], 1) and
          close(p["admission"], 5) and close(p["parked"], 0) and
          close(p["barrier_release"], 0) and close(p["push"], 0), f"capacity partition {p}")
    check(close(row["mean_chain"], 2), "capacity link: the chain has two jobs")
    imgs = {x["fingerprint"]: x for x in r["jq2"]["images"]["per_queue"][0]["rows"]}
    check(close(imgs[FP_A]["critical_path_ms"], 4.5) and
          close(imgs[FP_X]["critical_path_ms"], 3.5) and
          imgs[FP_A]["cp_rank"] == 1 and imgs[FP_X]["cp_rank"] == 2,
          "capacity link: the previous job's image is charged its exec time")

    # A push-gated chain whose push starts inside the wait: everything before the push's end is push.
    pushgated = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 100, 200)]
    pushgated += [
        span(JQ2_PUSH, 1, 1, 0 | 1 << 8, 11, 12),
        span(JQ2_JOB, qword(0, 0), 1, 1000, 13, 18),
        span(JQ2_IMAGE, qword(0, 0), 1, 0, 13.5, 14),
        span(JQ2_CREDIT, qword(0, 0), 1, 5 | EVF << 8 | 1 << 16, 18.2, 18.5),
        span(JQ2_SYNC_WAIT, 1, 5 | EVF << 8, 0, 10, 25),
        f"J 0 {FP_A} 00100000 4096",
    ]
    rc, out, err = run(tmp, jq2_trace(pushgated, header=IMG_HEADER), "--json")
    check(rc == 0, f"push-gated chain: exit 0: {err}")
    row = cp_row(json.loads(out), 1, 15)
    p = row["partition_ms"]
    check(close(p["credit_wake"], 7) and close(p["exec"], 4.5) and close(p["trace"], 0.5) and
          close(p["admission"], 1) and close(p["parked"], 0) and close(p["push"], 2) and
          close(p["barrier_release"], 0) and close(p["unresolved"], 0),
          f"push-gated partition {p}")
    check(close(row["mean_chain"], 1), "push-gated chain: one job")

    # The admission [A, c] is split by the worker's IDLE spans: 2 ms parked, 2 ms admission.
    parked = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 100, 200)]
    parked += [
        span(JQ2_PUSH, 1, 1, 0 | 1 << 8, 0, 1),
        span(JQ2_JOB, qword(0, 0), 1, 1000, 5, 9),
        span(JQ2_IMAGE, qword(0, 0), 1, 0, 5.5, 6),
        span(JQ2_CREDIT, qword(0, 0), 1, 5 | EVF << 8 | 1 << 16, 9.2, 9.5),
        span(JQ2_SYNC_WAIT, 1, 5 | EVF << 8, 0, 0, 12),
        span(JQ2_IDLE, qword(0, 0), 0, 0, 2, 4),
        f"J 0 {FP_A} 00100000 4096",
    ]
    rc, out, err = run(tmp, jq2_trace(parked, header=IMG_HEADER), "--json")
    check(rc == 0, f"parked split: exit 0: {err}")
    row = cp_row(json.loads(out), 1, 12)
    p = row["partition_ms"]
    check(close(p["credit_wake"], 3) and close(p["exec"], 3.5) and close(p["trace"], 0.5) and
          close(p["admission"], 2) and close(p["parked"], 2) and close(p["push"], 1),
          f"parked split partition {p}")

    # A JOB that starts before its PUSH ends: the cursor must not move forward at A = P1, so the
    # pre-job time stays 3.5 ms of push and the partition still sums to the 9 ms wait.
    clamp = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 100, 200)]
    clamp += [
        span(JQ2_PUSH, 1, 1, 0 | 1 << 8, 4, 5),
        span(JQ2_JOB, qword(0, 0), 1, 1000, 4.5, 8),
        span(JQ2_IMAGE, qword(0, 0), 1, 0, 4.6, 4.8),
        span(JQ2_CREDIT, qword(0, 0), 1, 5 | EVF << 8 | 1 << 16, 8.2, 8.5),
        span(JQ2_SYNC_WAIT, 1, 5 | EVF << 8, 0, 1, 10),
        f"J 0 {FP_A} 00100000 4096",
    ]
    rc, out, err = run(tmp, jq2_trace(clamp, header=IMG_HEADER), "--json")
    check(rc == 0, f"cursor clamp: exit 0: {err}")
    row = cp_row(json.loads(out), 1, 9)
    p = row["partition_ms"]
    check(close(p["credit_wake"], 2) and close(p["exec"], 3.3) and close(p["trace"], 0.2) and
          close(p["admission"], 0) and close(p["push"], 3.5),
          f"cursor clamp partition {p}")

    # A fiber wait is counted and not walked; an event-flag wait without a due credit is unresolved.
    fiber = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 100, 200)]
    fiber += [
        span(JQ2_PUSH, 1, 1, 0 | 1 << 8, 0, 1),
        span(JQ2_JOB, qword(0, 0), 1, 1000, 2, 4),
        span(JQ2_IMAGE, qword(0, 0), 1, 0, 2.2, 2.4),
        span(JQ2_CREDIT, qword(0, 0), 1, 9 | EVF << 8 | 1 << 16, 3.8, 3.9),
        span(JQ2_SYNC_WAIT, 2, 7 | EVF << 8, 0, 1, 5),
        span(JQ2_SYNC_WAIT, 2, 0xFF | FIBER << 8, 0, 2, 3),
        f"J 0 {FP_A} 00100000 4096",
    ]
    rc, out, err = run(tmp, jq2_trace(fiber, header=IMG_HEADER), "--json")
    check(rc == 0, f"fiber and unresolved: exit 0: {err}")
    r = json.loads(out)
    row = cp_row(r, 2, 4)
    check(row["waits"] == 1 and row["fiber_waits"] == 1 and row["waits_without_credit"] == 1 and
          close(row["partition_ms"]["unresolved"], 4) and close(row["partition_ms"]["exec"], 0) and
          row["mean_chain"] is None, f"fiber excluded, unresolved wait {row}")
    qd = r["jq2"]["critical_path"]["per_queue"][0]
    check(qd["fiber_waits"] == 1 and qd["waits_without_credit"] == 1, f"fiber count {qd}")

    # Image identity: J lines group by fingerprint across EAs and queues; 0xFFFFFFFF and an index
    # without a J line are one unknown row.
    ident = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 100, 200)]
    ident += [
        span(JQ2_PUSH, 1, 1, 0 | 1 << 8, 0, 0.2),
        span(JQ2_JOB, qword(0, 0), 1, 1000, 1, 3),
        span(JQ2_IMAGE, qword(0, 0), 1, 0, 1.5, 2),
        span(JQ2_PUSH, 1, 2, 0 | 1 << 8, 3, 3.2),
        span(JQ2_JOB, qword(0, 0), 2, 1000, 4, 7),
        span(JQ2_IMAGE, qword(0, 0), 2, 1, 4.5, 5.5),
        span(JQ2_PUSH, 1, 3, 0 | 1 << 8, 7, 7.2),
        span(JQ2_JOB, qword(0, 0), 3, 1000, 8, 9),
        span(JQ2_IMAGE, qword(0, 0), 3, 0xFFFFFFFF, 8.2, 8.4),
        span(JQ2_PUSH, 1, 4, 0 | 1 << 8, 9, 9.2),
        span(JQ2_JOB, qword(0, 0), 4, 1000, 10, 12),
        span(JQ2_IMAGE, qword(0, 0), 4, 7, 10.1, 10.2),
        span(JQ2_PUSH, 10, 1, 1 | 1 << 8, 0, 1),
        span(JQ2_JOB, qword(1, 0), 1, 500, 1, 2),
        span(JQ2_IMAGE, qword(1, 0), 1, 2, 1.2, 1.4),
        f"J 0 {FP_A} 00100000 64",
        f"J 1 {FP_A} 00200000 64",
        f"J 2 {FP_B} 00300000 32",
    ]
    header3 = ("# ps3_ftrace start_us=0 end_us=200000 spans=0 dropped=0"
               " jq2_images=3 jq2_image_overflow=0")
    rc, out, err = run(tmp, jq2_trace(ident, header=header3), "--json")
    check(rc == 0, f"image identity: exit 0: {err}")
    r = json.loads(out)
    imgs = r["jq2"]["images"]
    q0rows = {x["fingerprint"]: x for x in imgs["per_queue"][0]["rows"]}
    check(set(q0rows) == {FP_A, None},
          f"image identity: q0 groups by fingerprint {sorted(q0rows, key=lambda fp: fp or '')}")
    check(q0rows[FP_A]["jobs"] == 2 and
          q0rows[FP_A]["variants"] == [{"ea": 0x100000, "bytes": 64},
                                       {"ea": 0x200000, "bytes": 64}],
          f"image identity: one fingerprint, two EA/bytes variants {q0rows[FP_A]['variants']}")
    check(q0rows[None]["jobs"] == 2 and q0rows[None]["unknown"] is True and
          q0rows[None]["variants"] == [], "image identity: unknown jobs are one row")
    check(close(q0rows[FP_A]["exec_wall_ms"]["total_ms"], 3.5) and
          close(q0rows[None]["exec_wall_ms"]["total_ms"], 2.7),
          "image identity: exec wall subtracts each IMAGE span")
    overall = {x["fingerprint"]: x for x in imgs["overall"]}
    check(overall[FP_A]["jobs"] == 2 and overall[FP_A]["queues"] == [0] and
          overall[FP_B]["queues"] == [1] and overall[None]["jobs"] == 2,
          f"image identity: overall table lists the queues {imgs['overall']}")
    tc = {x["q"]: x for x in r["jq2"]["trace_cost"]["image_ms"]}
    check(close(tc[0]["image_ms"], 1.8) and close(tc[0]["job_ms"], 8) and
          close(tc[0]["share"], 1.8 / 8) and close(tc[1]["share"], 0.2),
          f"image identity: identification cost per queue {tc}")
    rc, out, err = run(tmp, jq2_trace(ident, header=header3))
    check(rc == 0 and "image table: 3 entries, overflow 0" in out,
          "image identity text: the header's table size")

    # A loss counter or an image table overflow marks the section INCOMPLETE, in text and JSON.
    lost = "# ps3_ftrace start_us=0 end_us=200000 spans=0 dropped=0 jq2_images=2 jq2_image_overflow=3"
    rc, out, err = run(tmp, jq2_trace(barrier, header=lost), "--json")
    check(rc == 0 and json.loads(out)["jq2"]["incomplete"] is True,
          "image losses: overflow marks the JSON incomplete")
    rc, out, err = run(tmp, jq2_trace(barrier, header=lost))
    check(rc == 0 and "INCOMPLETE: 3 image table slot(s) lost" in out,
          "image losses: overflow marks the text incomplete")
    rc, out, err = run(tmp, jq2_trace(barrier, header="# ps3_ftrace start_us=0 end_us=200000"
                                                " spans=0 dropped=7 jq2_images=2 jq2_image_overflow=0"))
    check(rc == 0 and "INCOMPLETE: 7 span(s) dropped" in out,
          "image losses: dropped spans mark the text incomplete")

    # An old trace (no kind 29, no J lines) gets one legacy line and no new sections.
    rc, out, err = run(tmp, jq2_trace(jq2), "--json")
    check(rc == 0, f"old trace: exit 0: {err}")
    jq = json.loads(out)["jq2"]
    check(jq["images_available"] is False and jq["critical_path"] is None and jq["images"] is None and
          jq["audio_stale"] is None and jq["trace_cost"] is None and jq["incomplete"] is None,
          f"old trace: the new sections are null {jq.keys()}")
    rc, out, err = run(tmp, jq2_trace(jq2))
    check(rc == 0 and out.count("no image identity in this trace (kind 29)") == 1 and
          "critical path of blocking waits" not in out and "job images by fingerprint" not in out and
          "image table:" not in out and "INCOMPLETE" not in out,
          "old trace: one legacy line, no image sections")

    rc, out, err = run(tmp, jq2_trace(barrier + ["J 9 zz 00100000 4"]))
    check(rc == 2 and "not an image line" in err and "Traceback" not in err,
          "malformed J line: exit 2, no traceback")

print("ftrace_report: %s" % ("ok" if not failures else "%d failed" % failures))
sys.exit(1 if failures else 0)
