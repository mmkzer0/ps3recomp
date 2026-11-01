#!/usr/bin/env python3
"""Frame-by-frame report of a PS3_FTRACE timeline.

  tools/ftrace_report.py <trace.txt> [--frames N] [--ms-per-col F] [--threads a,b]
                         [--split MS] [--cadence NID[@TID]] [--gap MS] [--json]

PS3_FTRACE=<start s>:<seconds>:<path> (toolkit, include/ps3emu/ps3_ftrace.h)
records spans: PPU syscalls that blocked and merged usleep poll runs (with the
guest call site), SPU busy/wait spans (busy tagged with the lifted image and the
SPURS workload id), the RSX walker blocked on a semaphore ACQUIRE, FIFO drains,
flips, presents and HLE import calls (NID and guest call site). A frame here is
the time between two guest flip requests.

The report says, for the mean frame, how each thread spent it: blocking
syscalls and usleep polls, and separately the time inside each HLE import,
by import and guest call site (an import can wait without a syscall: an lwmutex,
a job-queue sync). A span is shared out over the frames it overlaps.

  --split MS          the same breakdown twice: frames shorter than MS, and the
                      rest; what grows in the slow frames is the difference.
  --cadence NID[@TID] the gaps between successive calls of one import (NID in
                      hex), per calling thread or only TID; for every gap over
                      --gap MS (default 20), what the thread was inside during
                      it, as a share of the gap time; the worst gaps are listed.
  --json              the summary (frames, breakdowns, drawables, label waits,
                      cadence) as JSON on stdout instead of text; no timelines.

Two sections count per flip request (summed over the trace and divided by the
number of FLIP_REQ spans; a present runs behind the request that asked for it, so
frame windows would misplace it). A trace whose header says dropped > 0 gets counts
and shares but no rates.
  drawables   the Metal present's [CAMetalLayer nextDrawable] (kind 16): per flip
              split into new content (b > 0), no new content (b == 0, with the share
              that shows another surface than the last present) and nil (image
              dropped); mean wait.
  label_wait  a guest store to an RSX label that waited for a walker pass (kind 17):
              waits and ms per flip, p50/p99/max (nearest rank), the share with more
              than 64 spins (the wait had started to sleep) and the share that began
              while a pass was open. Each instant of a wait is charged to one walker
              span, in this order: drawable, gpu wait (in-flight), encode, present,
              drain, rest -- a drawable wait lies inside a drain pass, so the order
              keeps one millisecond from being counted twice.

A third section, SPURS job queues (jq2, kinds 22-26), splits a job's wait into
its links, per queue: pushes and push rate; queue latency (JOB t0 - PUSH t1),
execution wall and thread CPU time, completion delay (CREDIT t0 - JOB t1) and
sync latency (CREDIT t1 to the blocked thread running again), each with count,
mean, p50/p95/p99 and max; CPU/wall; per-worker utilisation and idle share of
the queue's window and the number of workers seen; queue depth from the push;
parks while work was pending; sync waits per PPU tid. Push, job and credit are
joined on (q, seq): seq counts per queue, so a join on seq alone pairs one
queue's push with another's job. Sync latency joins on (q, waiter) against the
last event-flag credit inside the wait; a fiber wait carries no waiter (0xFF,
wake 2), is reported apart and is not a join failure. A queue latency is
clamped at 0 when a worker admitted a job before the push span ended, and the
clamped joins are counted. A join that finds no partner is counted, never
dropped. A trace without kinds 22-26 prints no such section and its JSON key is
null.

Without --json it then draws a few frames as a timeline (one column per
--ms-per-col ms):
  PPU   '#' running   '.' usleep poll   's' other blocking syscall
  SPU   '#' busy      ' ' waiting on a channel
  RSX   'S' blocked on ACQUIRE   'D' FIFO drain   'P' present   '|' flip reached
        inside D/P: 'E' Metal encode   'G' waiting for the GPU   'R' render-target readback

Exit status: 0 report written; 2 unreadable or malformed trace, bad arguments,
or fewer than 3 flip requests in the window.
"""
import argparse
import bisect
import collections
import json
import re
import statistics
import sys

(SYSCALL, USLEEP, SPU_WAIT, SPU_BUSY, SEMA, SEMA_GAVEUP, DRAIN, FLIP_FIFO, FLIP_REQ, PRESENT,
 GPU_ENCODE, GPU_WAIT, READBACK, GPU_EXEC, HLE, DRAWABLE, LABEL_WAIT,
 AUDIO_MIX, AUDIO_PACE, AUDIO_UNDERRUN, AUDIO_STALE,
 JQ2_PUSH, JQ2_JOB, JQ2_CREDIT, JQ2_SYNC_WAIT, JQ2_IDLE) = range(1, 27)
AUDIO_PACE_REASON = {1: "queue full", 2: "clock pacing", 3: "backend wait"}
JQ2_FIBER_WAKE = 2       # wake field of a fiber wait or wake; it carries waiter 0xFF, so it cannot be joined
GPU_WAIT_KIND = {0: "gpu wait (flush)", 1: "gpu wait (present)", 2: "gpu wait (in-flight)"}
GPU_WAIT_INFLIGHT = 2
SPINS_SLEEPING = 64      # cellGcmSys.c yields for 64 spins, then sleeps 1 ms per spin
ATTRIBUTION = ("drawable", "gpu wait (in-flight)", "encode", "present", "drain", "rest")

SYSCALL_NAMES = {   # runtime/syscalls/lv2_syscall_table.h
    43: "thread_yield", 44: "thread_join", 85: "event_flag_wait", 92: "semaphore_wait",
    97: "lwmutex_lock", 102: "mutex_lock", 107: "cond_wait", 113: "lwcond_wait",
    122: "rwlock_rlock", 125: "rwlock_wlock", 130: "event_queue_receive", 138: "event_port_send",
    141: "usleep", 142: "sleep", 178: "spu_group_join", 801: "fs_open", 802: "fs_read", 818: "fs_lseek",
}


RSX_ROWS = ("acquire wait", "drain", "present", "encode (flush)", "encode (present)",
            "gpu wait (flush)", "gpu wait (present)", "gpu wait (in-flight)", "readback")


class BadInput(Exception):
    pass


def load(path):
    names, spans, header = {}, [], ""
    try:
        with open(path) as f:
            for no, line in enumerate(f, 1):
                if line.startswith("#"):
                    header = line.strip()
                    continue
                if line.startswith("T "):
                    _, tid, name = line.rstrip("\n").split(" ", 2)
                    names[int(tid)] = name
                    continue
                try:
                    k, who, a, b, t0, t1 = line.split()
                    spans.append((int(k), int(who), int(a, 16), int(b, 16), float(t0) / 1e3, float(t1) / 1e3))
                except ValueError:
                    raise BadInput(f"{path}:{no}: not a span line: {line.strip()[:80]!r}") from None
    except OSError as e:
        raise BadInput(f"{path}: {e.strerror}") from None
    spans.sort(key=lambda s: s[4])
    return header, names, spans


def overlap(s0, s1, f0, f1):
    return max(0.0, min(s1, f1) - max(s0, f0))


def nid_namer():
    try:
        import nid_database
        db = nid_database.get_default_db()
        db.load_implemented()
    except Exception:
        db = None

    def name(nid):
        e = db.lookup_nid(nid) if db else None
        return e[1] if e else f"nid {nid:08X}"
    return name


def frame_key(k, who, a, b):
    """What a span counts as in a frame breakdown, or None."""
    if k == USLEEP:
        return ("ppu", who, "usleep", b)
    if k == SYSCALL:
        return ("ppu", who, SYSCALL_NAMES.get(a, f"sc{a}"), b)
    if k == HLE:
        return ("hle", who, a, b)
    if k == SPU_BUSY:
        return ("spu", who)
    if k in (SEMA, SEMA_GAVEUP):
        return ("rsx", "acquire wait")
    if k == DRAIN:
        return ("rsx", "drain")
    if k == PRESENT:
        return ("rsx", "present")
    if k == GPU_ENCODE:
        return ("rsx", "encode (present)" if a else "encode (flush)")
    if k == GPU_WAIT:
        return ("rsx", GPU_WAIT_KIND.get(a, "gpu wait"))
    if k == READBACK:
        return ("rsx", "readback")
    return None


def per_frame(spans, frames):
    """One {key: ms} per frame. Each span is binned once by bisection on the
    frame ends and shared out over the frames it overlaps, so the cost is the
    span count plus the overlaps, not frames times spans."""
    ends = [f1 for _, f1 in frames]
    acc = [collections.defaultdict(float) for _ in frames]
    for k, who, a, b, t0, t1 in spans:
        key = frame_key(k, who, a, b)
        if key is None or t1 <= t0:
            continue
        i = bisect.bisect_right(ends, t0)
        while i < len(frames) and frames[i][0] < t1:
            o = overlap(t0, t1, frames[i][0], frames[i][1])
            if o > 0:
                acc[i][key] += o
            i += 1
    return acc


def breakdown(acc, idx, names, name_of):
    """Means over the frames idx (a frame where a key never happened counts 0)."""
    n = len(idx)
    tot = collections.defaultdict(float)
    for i in idx:
        for key, ms in acc[i].items():
            tot[key] += ms
    ppu = collections.defaultdict(list)
    hle = collections.defaultdict(list)
    spu, rsx = {}, {w: 0.0 for w in RSX_ROWS}
    for key, ms in tot.items():
        if key[0] == "ppu":
            ppu[key[1]].append({"kind": key[2], "site": f"{key[3]:08X}", "ms": ms / n})
        elif key[0] == "hle":
            hle[key[1]].append({"import": name_of(key[2]), "nid": f"{key[2]:08X}",
                                "site": f"{key[3]:08X}", "ms": ms / n})
        elif key[0] == "spu":
            spu[key[1]] = ms / n
        else:
            rsx[key[1]] = rsx.get(key[1], 0.0) + ms / n

    def threads(d):
        rows = [{"tid": t, "name": names.get(t, "?"), "ms": sum(r["ms"] for r in v),
                 "sites": sorted(v, key=lambda r: -r["ms"])} for t, v in d.items()]
        return sorted(rows, key=lambda r: -r["ms"])
    return {"frames": n, "ppu_blocked": threads(ppu), "hle": threads(hle),
            "spu_busy": {str(s): ms for s, ms in sorted(spu.items())}, "rsx": rsx}


def cadence(spans, names, name_of, nid, only_tid, gap_ms, worst=8):
    """Gaps between calls of one import per thread, and what the thread was
    inside (other imports, usleep polls, blocking syscalls) during long gaps."""
    calls = collections.defaultdict(list)
    inside = collections.defaultdict(list)          # tid -> (t0, t1, label) on that thread
    for k, who, a, b, t0, t1 in spans:
        if k == HLE and a == nid:
            calls[who].append(t0)
        elif k in (HLE, USLEEP, SYSCALL):
            label = name_of(a) if k == HLE else "usleep" if k == USLEEP else SYSCALL_NAMES.get(a, f"sc{a}")
            inside[who].append((t0, t1, label))
    out = []
    for tid, ts in sorted(calls.items()):
        if (only_tid is not None and tid != only_tid) or len(ts) < 3:
            continue
        gaps = [(b - a, a, b) for a, b in zip(ts, ts[1:])]
        g = [x[0] for x in gaps]
        q = statistics.quantiles(g, n=100)
        its = sorted(inside[tid])
        starts = [s[0] for s in its]
        longest = max((s[1] - s[0] for s in its), default=0.0)

        def what(a, b):
            share = collections.defaultdict(float)
            i = bisect.bisect_left(starts, a - longest)
            while i < len(its) and its[i][0] < b:
                o = overlap(its[i][0], its[i][1], a, b)
                if o > 0:
                    share[its[i][2]] += o
                i += 1
            return share
        long_gaps = [x for x in gaps if x[0] > gap_ms]
        total = collections.defaultdict(float)
        for d, a, b in long_gaps:
            for label, ms in what(a, b).items():
                total[label] += ms
        span = sum(x[0] for x in long_gaps) or 1.0
        out.append({
            "tid": tid, "name": names.get(tid, "?"), "calls": len(ts),
            "gap_ms": {"median": statistics.median(g), "p90": q[89], "p99": q[98], "max": max(g)},
            "over": {"ms": gap_ms, "count": len(long_gaps),
                     "inside_share": {k: v / span for k, v in sorted(total.items(), key=lambda x: -x[1])}},
            "worst": [{"gap_ms": d, "at_s": a / 1000,
                       "inside_ms": dict(sorted(what(a, b).items(), key=lambda x: -x[1])[:4])}
                      for d, a, b in sorted(gaps, reverse=True)[:worst]],
        })
    return out


def print_breakdown(bd, title):
    print(f"\n{title} (mean over {bd['frames']} frames, ms):")
    print("  PPU threads (blocking syscalls and usleep polls, by kind and guest call site):")
    for r in bd["ppu_blocked"][:12]:
        desc = ", ".join(f"{s['kind']}@{s['site']} {s['ms']:.1f}" for s in r["sites"][:4])
        print(f"    tid {r['tid']:3d} {r['name'][:28]:28s} blocked {r['ms']:5.1f}  ({desc})")
    print("  HLE imports (time inside, by thread, import and guest call site):")
    rows = [(s["ms"], r, s) for r in bd["hle"] for s in r["sites"]]
    for ms, r, s in sorted(rows, key=lambda x: -x[0])[:14]:
        print(f"    tid {r['tid']:3d} {r['name'][:28]:28s} {ms:5.1f}  {s['import']}@{s['site']}")
    print("  SPUs (busy):")
    for sp, ms in bd["spu_busy"].items():
        print(f"    spu {sp:>3s} busy {ms:5.1f}")
    print("  RSX / present thread:")
    for what, ms in bd["rsx"].items():
        print(f"    {what:20s} {ms:5.1f}")


def merged(spans):
    """Disjoint, sorted (t0, t1) intervals covering the spans."""
    out = []
    for t0, t1 in sorted(spans):
        if out and t0 <= out[-1][1]:
            out[-1][1] = max(out[-1][1], t1)
        else:
            out.append([t0, t1])
    return [(a, b) for a, b in out]


def clipped(iv, starts, w0, w1):
    """The part of the disjoint sorted intervals iv inside [w0, w1]; starts = their t0 list."""
    i = max(0, bisect.bisect_right(starts, w0) - 1)
    out = []
    while i < len(iv) and iv[i][0] < w1:
        a, b = max(iv[i][0], w0), min(iv[i][1], w1)
        if b > a:
            out.append((a, b))
        i += 1
    return out


def minus(a, b):
    """a without b; both disjoint and sorted."""
    out, j = [], 0
    for t0, t1 in a:
        while j < len(b) and b[j][1] <= t0:
            j += 1
        k = j
        while k < len(b) and b[k][0] < t1:
            if b[k][0] > t0:
                out.append((t0, b[k][0]))
            t0 = max(t0, b[k][1])
            k += 1
        if t1 > t0:
            out.append((t0, t1))
    return out


def length(iv):
    return sum(b - a for a, b in iv)


def nearest_rank(xs, p):
    """The p-th percentile of the sorted xs by nearest rank (an observed value, not an interpolation)."""
    return xs[max(1, -(-p * len(xs) // 100)) - 1]


def header_dropped(header):
    m = re.search(r"\bdropped=(\d+)", header)
    return int(m.group(1)) if m else None


def drawable_label_sections(spans, flip_requests, dropped):
    """The drawable and label-store-wait sections.

    Rates are sums over the whole trace divided by the number of flip requests, not
    means over frame windows: a present runs behind the flip request that asked for
    it, so a frame window holds the previous frame's present. A trace that dropped
    spans has no honest sum, so it gets counts and shares but no rates.
    """
    rates = dropped == 0

    def rate(x):
        return x / flip_requests if rates else None

    def share(x, n):
        return x / n if n else None

    draws = [s for s in spans if s[0] == DRAWABLE]
    nil = [s for s in draws if s[2] & 1]
    new = [s for s in draws if not s[2] & 1 and s[3] > 0]
    held = [s for s in draws if not s[2] & 1 and s[3] == 0]
    other = [s for s in held if s[2] & 2]
    waits = [s[5] - s[4] for s in draws]
    d = {"count": len(draws), "per_flip": rate(len(draws)),
         "mean_wait_ms": sum(waits) / len(waits) if waits else None,
         "new_content": {"count": len(new), "per_flip": rate(len(new))},
         "held": {"count": len(held), "per_flip": rate(len(held)),
                  "other_src": len(other), "other_src_share": share(len(other), len(held))},
         "nil": {"count": len(nil), "per_flip": rate(len(nil))}}

    lw = [s for s in spans if s[0] == LABEL_WAIT]
    durs = sorted(s[5] - s[4] for s in lw)
    total = sum(durs)
    classes = (("drawable", [(s[4], s[5]) for s in spans if s[0] == DRAWABLE]),
               ("gpu wait (in-flight)", [(s[4], s[5]) for s in spans
                                         if s[0] == GPU_WAIT and s[2] == GPU_WAIT_INFLIGHT]),
               ("encode", [(s[4], s[5]) for s in spans if s[0] == GPU_ENCODE]),
               ("present", [(s[4], s[5]) for s in spans if s[0] == PRESENT]),
               ("drain", [(s[4], s[5]) for s in spans if s[0] == DRAIN]))
    classes = [(name, iv, [x[0] for x in iv]) for name, iv in ((n, merged(v)) for n, v in classes)]
    ms = dict.fromkeys(ATTRIBUTION, 0.0)
    for s in lw:
        # Each instant of a wait belongs to the first class that covers it, in ATTRIBUTION
        # order: a drawable wait lies inside a drain pass, so without a fixed order the
        # same milliseconds would be counted twice.
        left = [(s[4], s[5])]
        for name, iv, starts in classes:
            if not left:
                break
            rest = minus(left, clipped(iv, starts, s[4], s[5]))
            ms[name] += length(left) - length(rest)
            left = rest
        ms["rest"] += length(left)
    l = {"count": len(lw), "per_flip": rate(len(lw)), "total_ms": total,
         "ms_per_flip": rate(total),
         "p50_ms": nearest_rank(durs, 50) if durs else None,
         "p99_ms": nearest_rank(durs, 99) if durs else None,
         "max_ms": durs[-1] if durs else None,
         "spins_over_64": share(sum(1 for s in lw if (s[3] & 0xFFFF) > SPINS_SLEEPING), len(lw)),
         "mid_pass": share(sum(1 for s in lw if s[3] >> 16 & 1), len(lw)),
         "attribution": {"ms_per_flip": {k: rate(v) for k, v in ms.items()},
                         "share": {k: share(v, total) for k, v in ms.items()}}}
    return {"flip_requests": flip_requests, "dropped": dropped, "rates": rates,
            "drawables": d, "label_wait": l}


def print_drawable_label(sec):
    n, d, l = sec["flip_requests"], sec["drawables"], sec["label_wait"]
    if not sec["rates"]:
        why = "no header" if sec["dropped"] is None else f"{sec['dropped']} spans dropped"
        print(f"\n({why}: per-flip rates withheld; counts and shares only. Raise PS3_FTRACE_CAP.)")

    def per(x):
        return f"{x:5.2f} /flip" if x is not None else "     n/a    "

    def pct(x):
        return f"{100 * x:.0f} %" if x is not None else "n/a"

    print(f"\nDrawables (nextDrawable), per flip request ({n} flip requests):")
    if not d["count"]:
        print("  none recorded")
    else:
        print(f"  all                      {d['count']:7d}  {per(d['per_flip'])}  mean wait {d['mean_wait_ms']:.2f} ms")
        print(f"  new content (b > 0)      {d['new_content']['count']:7d}  {per(d['new_content']['per_flip'])}")
        h = d["held"]
        print(f"  no new content (b == 0)  {h['count']:7d}  {per(h['per_flip'])}"
              f"  of those another surface: {h['other_src']} ({pct(h['other_src_share'])})")
        print(f"  nil (image dropped)      {d['nil']['count']:7d}  {per(d['nil']['per_flip'])}")
    print("\nLabel-store waits (guest thread waiting for an RSX pass), per flip request:")
    if not l["count"]:
        print("  none recorded")
        return
    ms = f"{l['ms_per_flip']:.2f} ms/flip" if l["ms_per_flip"] is not None else f"{l['total_ms']:.1f} ms in all"
    print(f"  {l['count']} waits  {per(l['per_flip'])}  {ms}")
    print(f"  duration p50 {l['p50_ms']:.2f}  p99 {l['p99_ms']:.2f}  max {l['max_ms']:.2f} ms;"
          f" spins > {SPINS_SLEEPING}: {pct(l['spins_over_64'])}; a pass already open: {pct(l['mid_pass'])}")
    print("  where the wait time went (one span per instant, in this order):")
    for k in ATTRIBUTION:
        v = l["attribution"]["ms_per_flip"][k]
        print(f"    {k:22s} {f'{v:6.2f} ms/flip' if v is not None else '      n/a      '}"
              f"  {pct(l['attribution']['share'][k]):>6s}")
    sh = l["attribution"]["share"]
    if sh["drawable"] is not None:
        print(f"  drawable + gpu wait (in-flight) explain {pct(sh['drawable'] + sh['gpu wait (in-flight)'])}"
              " of the wait time")


def audio_section(spans, dropped, name_of=None):
    if name_of is None:
        name_of = lambda nid: f"nid {nid:08X}"
    mix_spans = [s for s in spans if s[0] == AUDIO_MIX]
    pace_spans = [s for s in spans if s[0] == AUDIO_PACE]
    underrun_spans = [s for s in spans if s[0] == AUDIO_UNDERRUN]
    stale_spans = [s for s in spans if s[0] == AUDIO_STALE]

    if not (mix_spans or pace_spans or underrun_spans or stale_spans):
        return None

    rates = dropped == 0

    # 1. Blocks/s and mix intervals
    if mix_spans:
        mix_times = [s[4] for s in mix_spans]
        intervals = [b - a for a, b in zip(mix_times, mix_times[1:])]
        sorted_intervals = sorted(intervals)
        total_blocks = sum(s[2] for s in mix_spans)
        if len(intervals) > 0 and sum(intervals) > 0:
            blocks_per_s = len(intervals) / (sum(intervals) / 1000.0) * (total_blocks / len(mix_spans))
        else:
            blocks_per_s = 0.0
        mix_interval = {
            "p50": nearest_rank(sorted_intervals, 50) if sorted_intervals else None,
            "p90": nearest_rank(sorted_intervals, 90) if sorted_intervals else None,
            "p99": nearest_rank(sorted_intervals, 99) if sorted_intervals else None,
            "max": sorted_intervals[-1] if sorted_intervals else None,
        }
        # Queue depth p1/p5/p50 in ms (48 frames per ms at 48 kHz)
        depths_ms = sorted(s[3] / 48.0 for s in mix_spans)
        queue_depth = {
            "p1": nearest_rank(depths_ms, 1) if depths_ms else None,
            "p5": nearest_rank(depths_ms, 5) if depths_ms else None,
            "p50": nearest_rank(depths_ms, 50) if depths_ms else None,
        }
    else:
        blocks_per_s = 0.0
        mix_interval = {"p50": None, "p90": None, "p99": None, "max": None}
        queue_depth = {"p1": None, "p5": None, "p50": None}

    # 2. Underrun and stale counts and rates
    audio_all = mix_spans + pace_spans + underrun_spans + stale_spans
    t_min = min(s[4] for s in audio_all)
    t_max = max(s[5] for s in audio_all)
    audio_dur_s = (t_max - t_min) / 1000.0 if t_max > t_min else 0.0
    u_count = len(underrun_spans)
    if rates:
        u_rate = (u_count / audio_dur_s) if audio_dur_s > 0 else (0.0 if u_count == 0 else None)
    else:
        u_rate = None

    stale_count = len(stale_spans)
    run_starts = [s for s in stale_spans if s[3] == 1]
    gap_runs_count = len(run_starts)
    longest_run_blocks = max((s[3] for s in stale_spans), default=0)
    longest_run_ms = longest_run_blocks * (16.0 / 3.0)
    if rates:
        stale_rate = (gap_runs_count / audio_dur_s) if audio_dur_s > 0 else (0.0 if gap_runs_count == 0 else None)
    else:
        stale_rate = None

    stale_by_port = {}
    for s in stale_spans:
        p = str(s[1])
        if p not in stale_by_port:
            stale_by_port[p] = {"count": 0, "total_blocks": 0, "runs": 0,
                                "longest_run_blocks": 0, "longest_run_ms": 0.0}
        stale_by_port[p]["count"] += 1
        stale_by_port[p]["total_blocks"] += 1
        if s[3] == 1:
            stale_by_port[p]["runs"] += 1
        if s[3] > stale_by_port[p]["longest_run_blocks"]:
            stale_by_port[p]["longest_run_blocks"] = s[3]
            stale_by_port[p]["longest_run_ms"] = s[3] * (16.0 / 3.0)
    # 3. Pacing totals by reason
    pacing_totals = {}
    for code, name in AUDIO_PACE_REASON.items():
        sp = [s for s in pace_spans if s[2] == code]
        tot_ms = sum(s[5] - s[4] for s in sp)
        pacing_totals[name] = {"count": len(sp), "ms": tot_ms, "total_ms": tot_ms}
    total_pacing_ms = sum(s[5] - s[4] for s in pace_spans)

    # 4. Correlation with frame gaps, DRAWABLE/LABEL_WAIT spans, and long HLE waits (>10 ms)
    frame_spans = [s for s in spans if s[0] in (PRESENT, DRAWABLE)]
    if frame_spans:
        raw_times = sorted(s[4] for s in frame_spans)
        frame_events = []
        for t in raw_times:
            if not frame_events or t - frame_events[-1] > 5.0:
                frame_events.append(t)
    else:
        flip_spans = [s for s in spans if s[0] == FLIP_REQ]
        frame_events = sorted(s[4] for s in flip_spans)

    frame_gaps = []
    for i in range(len(frame_events) - 1):
        g0 = frame_events[i]
        g1 = frame_events[i + 1]
        frame_gaps.append((g0, g1, g1 - g0))

    d_lw_spans = [s for s in spans if s[0] in (DRAWABLE, LABEL_WAIT)]
    long_hle_spans = [s for s in spans if s[0] == HLE and (s[5] - s[4]) > 10.0]

    underruns_detail = []
    overlap_count = 0
    for u in underrun_spans:
        u_t = u[4]
        nearest_gap_ms = None
        if frame_gaps:
            def dist_to_gap(gap):
                g0, g1, _ = gap
                if g0 <= u_t <= g1:
                    return 0.0
                if u_t < g0:
                    return g0 - u_t
                return u_t - g1
            nearest = min(frame_gaps, key=dist_to_gap)
            nearest_gap_ms = nearest[2]

        w0 = u_t - 50.0
        w1 = u_t
        has_overlap = False
        for g0, g1, dur in frame_gaps:
            if dur > 40.0 and overlap(g0, g1, w0, w1) > 0:
                has_overlap = True
                break
        if not has_overlap:
            for s in d_lw_spans:
                if overlap(s[4], s[5], w0, w1) > 0:
                    has_overlap = True
                    break

        if has_overlap:
            overlap_count += 1

        hle_waits = []
        for h in long_hle_spans:
            if overlap(h[4], h[5], w0, w1) > 0:
                hle_waits.append({
                    "import": name_of(h[2]),
                    "nid": f"{h[2]:08X}",
                    "tid": h[1],
                    "dur_ms": h[5] - h[4],
                })

        underruns_detail.append({
            "at_s": u_t / 1000.0,
            "depth_frames": u[2],
            "depth_ms": u[2] / 48.0,
            "blocks_since": u[3],
            "nearest_frame_gap_ms": nearest_gap_ms,
            "overlap": has_overlap,
            "hle_waits": hle_waits,
        })

    overlap_share = (overlap_count / u_count) if u_count > 0 else None

    stale_detail = []
    stale_overlap_count = 0
    for s in stale_spans:
        s_t = s[4]
        nearest_gap_ms = None
        if frame_gaps:
            def dist_to_gap(gap):
                g0, g1, _ = gap
                if g0 <= s_t <= g1:
                    return 0.0
                if s_t < g0:
                    return g0 - s_t
                return s_t - g1
            nearest = min(frame_gaps, key=dist_to_gap)
            nearest_gap_ms = nearest[2]

        w0 = s_t - 50.0
        w1 = s_t
        has_overlap = False
        for g0, g1, dur in frame_gaps:
            if dur > 40.0 and overlap(g0, g1, w0, w1) > 0:
                has_overlap = True
                break
        if not has_overlap:
            for d in d_lw_spans:
                if overlap(d[4], d[5], w0, w1) > 0:
                    has_overlap = True
                    break

        if s[3] == 1 and has_overlap:
            stale_overlap_count += 1

        hle_waits = []
        for h in long_hle_spans:
            if overlap(h[4], h[5], w0, w1) > 0:
                hle_waits.append({
                    "import": name_of(h[2]),
                    "nid": f"{h[2]:08X}",
                    "tid": h[1],
                    "dur_ms": h[5] - h[4],
                })

        stale_detail.append({
            "at_s": s_t / 1000.0,
            "port": s[1],
            "block": s[2],
            "run_len": s[3],
            "nearest_frame_gap_ms": nearest_gap_ms,
            "overlap": has_overlap,
            "hle_waits": hle_waits,
        })

    stale_overlap_share = (stale_overlap_count / gap_runs_count) if gap_runs_count > 0 else None

    hle_counts = collections.defaultdict(int)
    for ev in underruns_detail + stale_detail:
        for hw in ev["hle_waits"]:
            hle_counts[(hw["nid"], hw["import"], hw["tid"])] += 1

    top_hle = [{"import": imp, "nid": nid, "tid": tid, "count": count}
               for (nid, imp, tid), count in sorted(hle_counts.items(), key=lambda x: -x[1])]

    return {
        "blocks_per_s": blocks_per_s,
        "blocks_s": blocks_per_s,
        "mix_interval_ms": mix_interval,
        "queue_depth_ms": queue_depth,
        "underruns": {
            "count": u_count,
            "rate": u_rate,
            "per_s": u_rate,
        },
        "underrun_count": u_count,
        "underrun_rate": u_rate,
        "stale": {
            "count": stale_count,
            "total_blocks": stale_count,
            "runs": gap_runs_count,
            "gap_runs": gap_runs_count,
            "longest_run_blocks": longest_run_blocks,
            "longest_run_ms": longest_run_ms,
            "rate": stale_rate,
            "per_s": stale_rate,
            "by_port": stale_by_port,
            "events": stale_detail,
            "overlap_count": stale_overlap_count,
            "overlap_share": stale_overlap_share,
        },
        "stale_count": stale_count,
        "stale_runs": gap_runs_count,
        "stale_total_blocks": stale_count,
        "stale_longest_run_ms": longest_run_ms,
        "stale_rate": stale_rate,
        "pacing": {
            "total_ms": total_pacing_ms,
            "by_reason": pacing_totals,
        },
        "pacing_by_reason": pacing_totals,
        "correlation": {
            "underruns": underruns_detail,
            "stale": stale_detail,
            "overlap_count": overlap_count,
            "overlap_share": overlap_share,
            "stale_overlap_count": stale_overlap_count,
            "stale_overlap_share": stale_overlap_share,
            "top_hle_waits": top_hle,
        },
        "overlap_share": overlap_share,
        "top_hle_waits": top_hle,
    }


def print_audio_section(aud):
    print("\nAudio:")
    if aud is None:
        print("  no audio spans")
        return
    fr = aud["mix_interval_ms"]
    qd = aud["queue_depth_ms"]
    u = aud["underruns"]
    st = aud["stale"]
    p = aud["pacing"]
    c = aud["correlation"]

    def ms_fmt(x):
        return f"{x:.2f}" if x is not None else "n/a"

    print(f"  {aud['blocks_per_s']:.1f} blocks/s, mix interval p50 {ms_fmt(fr['p50'])}"
          f" p90 {ms_fmt(fr['p90'])} p99 {ms_fmt(fr['p99'])} max {ms_fmt(fr['max'])} ms")
    print(f"  queue depth p1 {ms_fmt(qd['p1'])} p5 {ms_fmt(qd['p5'])} p50 {ms_fmt(qd['p50'])} ms")
    rate_str = f"{u['rate']:.1f} /s" if u['rate'] is not None else "n/a"
    print(f"  underruns: {u['count']} ({rate_str})")
    st_rate_str = f"{st['rate']:.1f} runs/s" if st['rate'] is not None else "n/a"
    print(f"  stale blocks: {st['total_blocks']} across {st['runs']} audible gap run(s)"
          f" ({st_rate_str}, longest {st['longest_run_blocks']} blk / {ms_fmt(st['longest_run_ms'])} ms)")
    if st["by_port"]:
        port_strs = [f"port {p}: {info['runs']} run(s), {info['total_blocks']} block(s), max {info['longest_run_blocks']} blk ({ms_fmt(info['longest_run_ms'])} ms)"
                     for p, info in sorted(st["by_port"].items())]
        print(f"    by port: {', '.join(port_strs)}")
    print(f"  pacing wait: {p['total_ms']:.1f} ms in all")
    for name, r in p["by_reason"].items():
        print(f"    {name:14s} {r['ms']:6.1f} ms ({r['count']} waits)")
    if not u["count"] and not st["count"]:
        print("  correlation: no underruns or stale blocks")
    else:
        print("  correlation (50 ms before event):")
        if u["count"]:
            pct = f"{100 * c['overlap_share']:.0f}%" if c['overlap_share'] is not None else "n/a"
            print(f"    underruns overlapping a >40 ms frame gap or drawable/label wait: {pct} ({c['overlap_count']}/{u['count']})")
        if st["runs"]:
            pct = f"{100 * st['overlap_share']:.0f}%" if st['overlap_share'] is not None else "n/a"
            print(f"    audible gap runs overlapping a >40 ms frame gap or drawable/label wait: {pct} ({st['overlap_count']}/{st['runs']})")
        if c["top_hle_waits"]:
            print("    top overlapping long guest waits (>10 ms HLE):")
            for h in c["top_hle_waits"][:6]:
                print(f"      tid {h['tid']:3d} {h['import']} ({h['nid']}): {h['count']}")
        else:
            print("    overlapping long guest waits (>10 ms HLE): none")


def jq2_stats(xs):
    """count, mean, nearest-rank p50/p95/p99 and max of xs; None where there is nothing to average."""
    s = sorted(xs)
    if not s:
        return {"count": 0, "mean": None, "p50": None, "p95": None, "p99": None, "max": None}
    return {"count": len(s), "mean": sum(s) / len(s),
            "p50": nearest_rank(s, 50), "p95": nearest_rank(s, 95),
            "p99": nearest_rank(s, 99), "max": s[-1]}


def jq2_ranks(xs):
    """nearest-rank (p50, p95, p99) of xs, or (None, None, None) when there is nothing to rank."""
    s = sorted(xs)
    if not s:
        return None, None, None
    return nearest_rank(s, 50), nearest_rank(s, 95), nearest_rank(s, 99)


def jq2_section(spans, header, names=None):
    """The SPURS job queue (jq2) section, or None when the trace has no kinds 22-26.

    Queue latency, execution wall and CPU, and completion delay are keyed on (q, seq):
    seq counts per queue and every queue starts at 1, so a join that ignores q pairs one
    queue's push with another queue's job and reports a latency neither saw. Sync
    latency is the wait for a completion signal to reach the blocked PPU thread, joined
    on (q, waiter) against the last event-flag credit that landed inside that wait; the
    fiber path has no waiter id (0xFF, wake 2), so a fiber wait cannot be joined and is
    reported apart rather than miscounted against the event flag. A join that finds no
    partner is counted, never dropped: a span cut off at a window edge loses its
    partner, and hiding that would make the latencies look better than the trace is.
    """
    js = [s for s in spans if JQ2_PUSH <= s[0] <= JQ2_IDLE]
    if not js:
        return None
    by_q = collections.defaultdict(lambda: {"push": [], "job": [], "credit": [], "wait": [],
                                            "idle": [], "bounds": []})
    for k, who, a, b, t0, t1 in js:
        if k == JQ2_PUSH:                 # who = tid, a = seq, b = q | depth << 8
            g = by_q[b & 0xFF]
            g["push"].append((a, t0, t1, b >> 8))
        elif k == JQ2_SYNC_WAIT:          # who = tid, a = waiter | wake << 8 | mode << 16, b = q
            g = by_q[b & 0xFF]
            g["wait"].append((who, a & 0xFF, a >> 8 & 0xFF, t0, t1))
        else:                             # who = q << 8 | w, a = seq
            g = by_q[who >> 8]
            if k == JQ2_JOB:              # b = thread CPU time, us
                g["job"].append((who & 0xFF, a, b, t0, t1))
            elif k == JQ2_CREDIT:         # b = waiter | wake << 8 | due << 16
                g["credit"].append((who & 0xFF, a, b, t0, t1))
            else:                         # JQ2_IDLE: a = depth at park, b = depth at wake
                g["idle"].append((who & 0xFF, a, b, t0, t1))
        g["bounds"].append((t0, t1))

    queues = []
    for q in sorted(by_q):
        g = by_q[q]
        pushes, jobs, credits = g["push"], g["job"], g["credit"]
        waits, idles = g["wait"], g["idle"]
        w0 = min(t0 for t0, _ in g["bounds"])
        w1 = max(t1 for _, t1 in g["bounds"])
        window = w1 - w0

        push_end = {seq: t1 for seq, _, t1, _ in pushes}
        latency, clamped = [], 0
        for _, seq, _, jt0, _ in jobs:
            end = push_end.get(seq)
            if end is None:
                continue
            d = jt0 - end
            if d < 0.0:                   # a busy worker admitted the job before its push span ended
                d, clamped = 0.0, clamped + 1
            latency.append(d)
        paired = {seq for _, seq, _, _, _ in jobs if seq in push_end}

        credit_start = {seq: c0 for _, seq, _, c0, _ in credits}
        completion = [credit_start[seq] - jt1 for _, seq, _, _, jt1 in jobs if seq in credit_start]
        credited = {seq for _, seq, _, _, _ in jobs if seq in credit_start}

        wall = [jt1 - jt0 for _, _, _, jt0, jt1 in jobs]
        cpu = [us / 1000.0 for _, _, us, _, _ in jobs]

        # A due event-flag credit is the only thing that can end an event-flag wait. The
        # last one inside the wait is the wake it saw; credits are consumed as they pair,
        # because one wake ends one wait and a reused waiter id must not reuse a credit.
        due = collections.defaultdict(list)
        for i, (_, _, b, _, c1) in enumerate(credits):
            if b >> 16 & 1 and (b >> 8) & 0xFF != JQ2_FIBER_WAKE:
                due[b & 0xFF].append((c1, i))
        for v in due.values():
            v.sort()
        used, sync, wait_without_credit = set(), [], 0
        fiber = [t1 - t0 for _, _, wake, t0, t1 in waits if wake == JQ2_FIBER_WAKE]
        for _, waiter, wake, wt0, wt1 in sorted(waits, key=lambda s: s[3]):
            if wake == JQ2_FIBER_WAKE:
                continue
            hit = None
            for c1, i in reversed(due.get(waiter, ())):
                if c1 > wt1:
                    continue
                if c1 < wt0:
                    break
                if i not in used:
                    hit = (c1, i)
                    break
            if hit is None:
                wait_without_credit += 1
            else:
                used.add(hit[1])
                sync.append(wt1 - hit[0])
        due_total = sum(len(v) for v in due.values())

        busy, idle_ms, job_count = (collections.defaultdict(float), collections.defaultdict(float),
                                    collections.Counter())
        for w, _, _, jt0, jt1 in jobs:
            busy[w] += jt1 - jt0
            job_count[w] += 1
        for w, _, _, it0, it1 in idles:
            idle_ms[w] += it1 - it0
        workers = sorted(set(busy) | set(idle_ms))
        parked = [t1 - t0 for _, depth, _, t0, t1 in idles if depth > 0]

        depths = sorted(p[3] for p in pushes)
        waits_by_tid = collections.defaultdict(list)
        for tid, _, _, t0, t1 in waits:
            waits_by_tid[tid].append(t1 - t0)
        total_wall = sum(wall)

        queues.append({
            "q": q,
            "window": {"start_ms": w0, "end_ms": w1, "ms": window},
            "pushes": len(pushes),
            "push_rate_per_s": len(pushes) / (window / 1000.0) if window > 0 else None,
            "jobs": len(jobs),
            "workers": len(workers),
            "queue_latency_ms": jq2_stats(latency),
            "queue_latency_clamped": clamped,
            "exec_wall_ms": jq2_stats(wall),
            "exec_cpu_ms": jq2_stats(cpu),
            "completion_delay_ms": jq2_stats(completion),
            "sync_latency_ms": jq2_stats(sync),
            "cpu_wall": sum(cpu) / total_wall if total_wall > 0 else None,
            "per_worker": [
                {"w": w, "jobs": job_count[w], "busy_ms": busy[w],
                 "utilisation": busy[w] / window if window > 0 else None,
                 "idle_ms": idle_ms[w],
                 "idle_fraction": idle_ms[w] / window if window > 0 else None}
                for w in workers],
            "parked_while_pending": {"count": len(parked), "total_ms": sum(parked)},
            "depth": {"count": len(depths),
                      "p50": nearest_rank(depths, 50) if depths else None,
                      "p95": nearest_rank(depths, 95) if depths else None,
                      "max": depths[-1] if depths else None},
            "sync_wait_by_tid": [
                {"tid": tid, "name": (names or {}).get(tid, "?"), "count": len(v),
                 "total_ms": sum(v), "p50": jq2_ranks(v)[0], "p95": jq2_ranks(v)[1],
                 "p99": jq2_ranks(v)[2]}
                for tid, v in sorted(waits_by_tid.items())],
            "fiber_sync_wait": {"count": len(fiber), "total_ms": sum(fiber),
                                "p50": jq2_ranks(fiber)[0], "p95": jq2_ranks(fiber)[1],
                                "p99": jq2_ranks(fiber)[2]},
            "join_failures": {
                "queue_latency": {"matched": len(paired),
                                  "push_without_job": len(pushes) - len(paired),
                                  "job_without_push": len(jobs) - len(paired)},
                "completion_delay": {"matched": len(credited),
                                     "job_without_credit": len(jobs) - len(credited),
                                     "credit_without_job": len(credits) - len(credited)},
                "sync_latency": {"matched": len(sync),
                                 "wait_without_credit": wait_without_credit,
                                 "credit_without_wait": due_total - len(used)},
            },
        })
    return {"dropped": header_dropped(header), "queues": queues}


def print_jq2_section(jq):
    if jq is None:
        return

    def ms(x):
        return "n/a" if x is None else f"{x:.2f}"

    def stat(label, st, extra=""):
        print(f"    {label:30s} n {st['count']:5d}  mean {ms(st['mean']):>7s}"
              f"  p50 {ms(st['p50']):>7s}  p95 {ms(st['p95']):>7s}"
              f"  p99 {ms(st['p99']):>7s}  max {ms(st['max']):>7s} ms{extra}")

    def pct(x):
        return "n/a" if x is None else f"{100 * x:.0f} %"

    dropped = jq["dropped"]
    print("\nSPURS job queues (jq2):")
    print("  recorder dropped spans: " + ("no header" if dropped is None else str(dropped)))
    for qd in jq["queues"]:
        w = qd["window"]
        rate = (f"{qd['push_rate_per_s']:.2f} pushes/s" if qd["push_rate_per_s"] is not None
                else "rate n/a")
        print(f"  queue {qd['q']}: {qd['pushes']} pushes, {qd['jobs']} jobs, {qd['workers']} workers,"
              f" window {w['start_ms']:.2f}-{w['end_ms']:.2f} ms ({w['ms']:.2f} ms), {rate}")
        stat("queue latency (push->job)", qd["queue_latency_ms"],
             f"  ({qd['queue_latency_clamped']} clamped at 0)")
        stat("execution wall (job)", qd["exec_wall_ms"])
        stat("execution CPU (thread)", qd["exec_cpu_ms"])
        stat("completion delay (job->credit)", qd["completion_delay_ms"])
        stat("sync latency (credit->resume)", qd["sync_latency_ms"])
        print(f"    cpu/wall {ms(qd['cpu_wall'])}")
        for pw in qd["per_worker"]:
            print(f"    worker {pw['w']}: {pw['jobs']} jobs, busy {pw['busy_ms']:.2f} ms"
                  f" = {pct(pw['utilisation'])}, idle {pw['idle_ms']:.2f} ms"
                  f" = {pct(pw['idle_fraction'])}")
        pk = qd["parked_while_pending"]
        print(f"    parked while work was pending: {pk['count']} spans, {pk['total_ms']:.2f} ms")
        d = qd["depth"]
        print(f"    queue depth: p50 {ms(d['p50'])}  p95 {ms(d['p95'])}  max {ms(d['max'])}"
              f"  ({d['count']} pushes)")
        for row in qd["sync_wait_by_tid"]:
            print(f"    sync wait tid {row['tid']:3d} {row['name'][:28]:28s} n {row['count']:5d}"
                  f"  {row['total_ms']:8.2f} ms  p50 {ms(row['p50'])}"
                  f"  p95 {ms(row['p95'])}  p99 {ms(row['p99'])} ms")
        fs = qd["fiber_sync_wait"]
        print(f"    fiber sync waits (no waiter, not joined): n {fs['count']}"
              f"  {fs['total_ms']:.2f} ms  p50 {ms(fs['p50'])}  p95 {ms(fs['p95'])}"
              f"  p99 {ms(fs['p99'])} ms")
        ql = qd["join_failures"]["queue_latency"]
        cd = qd["join_failures"]["completion_delay"]
        sl = qd["join_failures"]["sync_latency"]
        print(f"    join failures: push->job {ql['push_without_job'] + ql['job_without_push']}"
              f" ({ql['push_without_job']} push without a job, {ql['job_without_push']} job without a"
              f" push); job->credit {cd['job_without_credit'] + cd['credit_without_job']}"
              f" ({cd['job_without_credit']} job without a credit, {cd['credit_without_job']} credit"
              f" without a job); credit->wait"
              f" {sl['wait_without_credit'] + sl['credit_without_wait']}"
              f" ({sl['wait_without_credit']} wait without a credit,"
              f" {sl['credit_without_wait']} due credit without a wait)")


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("trace")
    ap.add_argument("--frames", type=int, default=3, help="frames drawn as a timeline")
    ap.add_argument("--ms-per-col", type=float, default=1.0)
    ap.add_argument("--threads", default="", help="PPU tids to draw (default: the six least blocked)")
    ap.add_argument("--split", type=float, metavar="MS", help="break down frames < MS and >= MS separately")
    ap.add_argument("--cadence", metavar="NID[@TID]", help="gaps between calls of one import (hex NID)")
    ap.add_argument("--gap", type=float, default=20.0, metavar="MS", help="cadence gap threshold (default 20)")
    ap.add_argument("--json", action="store_true", help="summary as JSON, no timelines")
    args = ap.parse_args()
    cad_nid = cad_tid = None
    if args.cadence:
        try:
            nid_s, _, tid_s = args.cadence.partition("@")
            cad_nid = int(nid_s, 16)
            cad_tid = int(tid_s) if tid_s else None
        except ValueError:
            ap.error(f"--cadence wants NID[@TID] with a hex NID, got {args.cadence!r}")

    try:
        header, names, spans = load(args.trace)
    except BadInput as e:
        print(f"ftrace_report: {e}", file=sys.stderr)
        sys.exit(2)
    flips = [s[4] for s in spans if s[0] == FLIP_REQ]
    if len(flips) < 3:
        print("ftrace_report: fewer than 3 flip requests in the window", file=sys.stderr)
        sys.exit(2)
    frames = list(zip(flips, flips[1:]))
    durs = [b - a for a, b in frames]
    name_of = nid_namer()
    acc = per_frame(spans, frames)
    report = {"header": header,
              "frames": {"count": len(frames), "median_ms": statistics.median(durs),
                         "min_ms": min(durs), "max_ms": max(durs)},
              "all": breakdown(acc, range(len(frames)), names, name_of)}
    if args.split is not None:
        fast = [i for i, d in enumerate(durs) if d < args.split]
        slow = [i for i, d in enumerate(durs) if d >= args.split]
        report["split"] = {"ms": args.split,
                           "fast": breakdown(acc, fast, names, name_of) if fast else None,
                           "slow": breakdown(acc, slow, names, name_of) if slow else None}
    report.update(drawable_label_sections(spans, len(flips), header_dropped(header)))
    report["audio"] = audio_section(spans, header_dropped(header), name_of)
    report["jq2"] = jq2_section(spans, header, names)
    if cad_nid is not None:
        report["cadence"] = {"nid": f"{cad_nid:08X}", "import": name_of(cad_nid),
                             "threads": cadence(spans, names, name_of, cad_nid, cad_tid, args.gap)}
    if args.json:
        json.dump(report, sys.stdout, indent=1, sort_keys=True)
        print()
        return

    print(header)
    fr = report["frames"]
    print(f"{fr['count']} frames, median {fr['median_ms']:.1f} ms "
          f"({1000 / fr['median_ms']:.1f} fps), min {fr['min_ms']:.1f}, max {fr['max_ms']:.1f}")
    print_breakdown(report["all"], "Per frame")
    if "split" in report:
        for part, rel in (("fast", "<"), ("slow", ">=")):
            if report["split"][part]:
                print_breakdown(report["split"][part], f"Frames {rel} {args.split:g} ms")
    print_drawable_label(report)
    print_audio_section(report["audio"])
    print_jq2_section(report["jq2"])
    if "cadence" in report:
        c = report["cadence"]
        print(f"\nCadence of {c['import']} ({c['nid']}):")
        for t in c["threads"]:
            g = t["gap_ms"]
            print(f"  tid {t['tid']:3d} {t['name'][:28]:28s} calls {t['calls']}  gap median {g['median']:.1f}"
                  f" p90 {g['p90']:.1f} p99 {g['p99']:.1f} max {g['max']:.1f} ms;"
                  f" {t['over']['count']} gaps > {t['over']['ms']:g} ms")
            share = ", ".join(f"{k} {100 * v:.0f}%" for k, v in list(t["over"]["inside_share"].items())[:5])
            print(f"    inside those gaps: {share or 'nothing traced'}")
            for w in t["worst"][:5]:
                ins = ", ".join(f"{k} {v:.1f}" for k, v in w["inside_ms"].items())
                print(f"    {w['gap_ms']:6.1f} ms at {w['at_s']:.3f} s: {ins or 'nothing traced'}")

    # GPU execution per command buffer (headless commits wait for each one).
    n = len(frames)
    ex = [s for s in spans if s[0] == GPU_EXEC and flips[0] <= s[5] <= flips[-1]]
    if ex:
        print("  Metal command buffers (GPU's own execution time):")
        for kind, label in ((1, "present"), (0, "flush")):
            xs = [s for s in ex if s[3] == kind]
            if not xs:
                continue
            ms = [s[5] - s[4] for s in xs]
            print(f"    {label:8s} {len(xs) / n:4.1f}/frame  gpu {statistics.median(ms):6.2f} ms median"
                  f"  ({sum(ms) / n:5.1f} ms/frame)  passes {statistics.median([s[1] for s in xs]):.0f}"
                  f"  depth resolves {statistics.median([s[2] & 0xFFFF for s in xs]):.0f}"
                  f"  colour copies {statistics.median([s[2] >> 16 for s in xs]):.0f}")

    # Timelines.
    spus = sorted({s[1] for s in spans if s[0] in (SPU_BUSY, SPU_WAIT)})
    if args.threads:
        try:
            draw = [int(x) for x in args.threads.split(",")]
        except ValueError:
            ap.error(f"--threads wants comma-separated tids, got {args.threads!r}")
    else:
        rows = [(r["ms"], r["tid"]) for r in report["all"]["ppu_blocked"]]
        draw = [t for _, t in sorted(rows)[:6]]   # the threads that work, not the idle ones
    pick = [frames[len(frames) // 2 + i] for i in range(args.frames) if len(frames) // 2 + i < len(frames)]
    for f0, f1 in pick:
        cols = int((f1 - f0) / args.ms_per_col) + 1
        print(f"\nframe at {f0 / 1000:.3f} s, {f1 - f0:.1f} ms ({args.ms_per_col:g} ms/col)")
        lines = {}
        for t in draw:
            lines[("ppu", t)] = ["#"] * cols
        for sp in spus:
            lines[("spu", sp)] = [" "] * cols
        lines[("rsx",)] = [" "] * cols

        def paint(row, t0, t1, ch, prio=False):
            c0 = max(0, int((t0 - f0) / args.ms_per_col))
            c1 = min(cols - 1, int((t1 - f0) / args.ms_per_col))
            for c in range(c0, c1 + 1):
                if prio or row[c] in " #":
                    row[c] = ch

        window = [s for s in spans if s[5] >= f0 and s[4] <= f1]
        for k, who, a, b, t0, t1 in window:
            if k == USLEEP and ("ppu", who) in lines:
                paint(lines[("ppu", who)], t0, t1, ".", True)
            elif k == SYSCALL and ("ppu", who) in lines:
                paint(lines[("ppu", who)], t0, t1, "s", True)
            elif k == SPU_BUSY and ("spu", who) in lines:
                paint(lines[("spu", who)], t0, t1, "#")
            elif k in (SEMA, SEMA_GAVEUP):
                paint(lines[("rsx",)], t0, t1, "S", True)
            elif k == DRAIN:
                paint(lines[("rsx",)], t0, t1, "D")
            elif k == PRESENT:
                paint(lines[("rsx",)], t0, t1, "P", True)
            elif k == FLIP_FIFO:
                paint(lines[("rsx",)], t0, t0, "|", True)
        for k, who, a, b, t0, t1 in window:   # second pass: what the drain/present was doing
            if k == GPU_ENCODE:
                paint(lines[("rsx",)], t0, t1, "E", True)
            elif k == GPU_WAIT:
                paint(lines[("rsx",)], t0, t1, "G", True)
            elif k == READBACK:
                paint(lines[("rsx",)], t0, t1, "R", True)
        for key, row in lines.items():
            if key[0] == "ppu":
                label = f"{key[1]:3d} {names.get(key[1], '?')[:14]:14s}"
            elif key[0] == "spu":
                label = f"spu {key[1]:<14d}"
            else:
                label = "rsx" + " " * 15
            print(f"  {label} {''.join(row)}")


if __name__ == "__main__":
    main()
