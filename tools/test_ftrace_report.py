#!/usr/bin/env python3
"""ftrace_report: frame binning, import call sites, split, cadence, drawables, label-store waits, jq2 job queues, bad input."""
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
EVF, FIBER = 1, 2


def close(a, b, tol=1e-9):
    return a is not None and abs(a - b) <= tol


def qword(q, w):
    return q << 8 | w


def jq2_trace(lines, dropped=0):
    head = [f"# ps3_ftrace start_us=0 end_us=200000 spans=0 dropped={dropped}",
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
    need = {"q", "window", "pushes", "push_rate_per_s", "jobs", "workers", "queue_latency_ms",
            "queue_latency_clamped", "exec_wall_ms", "exec_cpu_ms", "completion_delay_ms",
            "sync_latency_ms", "cpu_wall", "per_worker", "parked_while_pending", "depth",
            "sync_wait_by_tid", "fiber_sync_wait", "join_failures"}
    check(need <= set(q0), f"jq2 keys present: {sorted(need - set(q0))}")
    stats = {"count", "mean", "p50", "p95", "p99", "max"}
    check(all(stats <= set(q0[k]) for k in ("queue_latency_ms", "exec_wall_ms", "exec_cpu_ms",
                                            "completion_delay_ms", "sync_latency_ms")),
          "jq2 latency rows carry count, mean, p50, p95, p99 and max")
    check(q0["pushes"] == 3 and q0["jobs"] == 3 and q0["workers"] == 2, "q0: 3 pushes, 3 jobs, 2 workers")
    check(close(q0["window"]["start_ms"], 0) and close(q0["window"]["ms"], 40) and
          close(q0["push_rate_per_s"], 75), "q0: window 0-40 ms, 75 pushes/s")
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
    check(close(by_w[0]["busy_ms"], 6) and close(by_w[0]["utilisation"], 6 / 40) and
          close(by_w[0]["idle_ms"], 3) and close(by_w[0]["idle_fraction"], 3 / 40),
          f"q0 worker 0 busy/idle {by_w[0]}")
    check(close(by_w[1]["busy_ms"], 6) and close(by_w[1]["utilisation"], 6 / 40) and
          close(by_w[1]["idle_ms"], 21.5) and close(by_w[1]["idle_fraction"], 21.5 / 40),
          f"q0 worker 1 busy/idle {by_w[1]}")
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
    check(close(q1["window"]["ms"], 15.5) and close(q1["push_rate_per_s"], 2 / 0.0155),
          "q1: window 15.5 ms and its push rate")
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
    check("worker 0: 2 jobs, busy 6.00 ms = 15 %, idle 3.00 ms = 8 %" in out and
          "parked while work was pending: 1 spans, 3.00 ms" in out and
          "fiber sync waits (no waiter, not joined): n 0" in out,
          "jq2 text: worker utilisation, parked-while-pending, fiber row")
    check("join failures: push->job 0 (0 push without a job, 0 job without a push)" in out,
          "jq2 text: join failures")

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
    check(close(e["per_worker"][0]["busy_ms"], 3) and close(e["per_worker"][0]["utilisation"], 0.1),
          "jq2: worker busy time and utilisation of the window")

    # A trace without kinds 22-26 prints no section and has a null key.
    plain = [span(FLIP_REQ, 1, 0, 0, t, t) for t in (0, 20, 40)]
    rc, out, err = run(tmp, trace(plain), "--json")
    check(rc == 0 and json.loads(out)["jq2"] is None, "jq2: trace without the kinds has a null key")
    rc, out, err = run(tmp, trace(plain))
    check(rc == 0 and "SPURS job queues" not in out and "jq2" not in out,
          "jq2: trace without the kinds prints no section")

print("ftrace_report: %s" % ("ok" if not failures else "%d failed" % failures))
sys.exit(1 if failures else 0)
