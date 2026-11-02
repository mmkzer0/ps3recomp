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

A third section, SPURS job queues (jq2, kinds 22-28), splits a job's wait into
its links, per queue: pushes and push rate; queue latency (JOB t0 - PUSH t1),
execution wall and thread CPU time, completion delay (CREDIT t0 - JOB t1) and
sync latency (CREDIT t1 to the blocked thread running again), each with count,
mean, p50/p95/p99 and max; CPU/wall; per-worker utilisation and idle share of
the recorder's window (the header's start_us..end_us, one denominator for every
queue, so the shares compare; the queue's own extent only when the header names
none, and which one was used is printed), each busy or idle span clipped to
that window before it is summed; the number of workers seen; queue depth from
the push; parks while work was pending; sync waits per PPU tid. IDLE's a is a
depth in its low 24 bits and a park reason in the top byte (0 unrecorded, 1
empty, 2 unwritten, 3 held, 4 no-slot, 5 paused, 15 unclassified), reported as
count, time and share of idle time per reason. HELD and BARRIER (kinds 27, 28)
split the queue wait into held (guest ordering: a SYNC barred the job's tag)
and ready wait (host: admissible and not admitted), each with mean/p50/p95 and
its share, the ready wait split per job into time with a worker of the queue
parked (avoidable host scheduling), time with every worker in a JOB span
(capacity) and the rest (workers in transition), the number of held and
admissible jobs over time, the same by depth at push with how many of those
jobs a barrier actually held, and the barriers with their rate, duration and
the masks that held the most time.
Push, job and credit are joined on (q, seq): seq counts per queue, so a join on
seq alone pairs one queue's push with another's job. Sync latency joins on
(q, waiter) against the last event-flag credit inside the wait; a fiber wait
carries no waiter id (0xFF, wake 2), so it is reported apart and is not a join
failure. A queue latency is clamped at 0 when a worker admitted a job before
the push span ended, and the clamped joins are counted. A join that finds no
partner is counted, never dropped. A trace without kinds 22-28 prints no such
section and its JSON key is null; a trace without kinds 27-28 prints the
section it always did plus a line that the decomposition needs them.

A trace that carries job image identity (kind 29, the `J` image lines, and the
header's `jq2_images`/`jq2_image_overflow`) adds, per queue: the image table
and the IMAGE time as a share of the queue's JOB time; the critical path of
every event-flag wait, a backward walk from the job its due credit names
through the capacity, barrier-release and push links that kept the thread
blocked, partitioned into credit+wake, exec, image (trace), admission, parked,
barrier-release, push and unresolved; the images grouped by fingerprint and
ranked by their share of that blocking time next to their CPU time; and the
attribution of every audio stale block (one event per not-refilled block, `b` =
its position in the run) to the job the waiting threads were blocked on, with
the run-length histogram rebuilt per port. A loss counter or a full image table
marks the section INCOMPLETE. A trace without kind 29 prints one line saying it
has no image identity and changes nothing else.

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
 JQ2_PUSH, JQ2_JOB, JQ2_CREDIT, JQ2_SYNC_WAIT, JQ2_IDLE, JQ2_HELD, JQ2_BARRIER,
 JQ2_IMAGE) = range(1, 30)
AUDIO_PACE_REASON = {1: "queue full", 2: "clock pacing", 3: "backend wait"}
JQ2_FIBER_WAKE = 2       # wake field of a fiber wait or wake; it carries waiter 0xFF, so it cannot be joined
JQ2_PARK_REASONS = {0: "unrecorded", 1: "empty", 2: "unwritten", 3: "held", 4: "no-slot", 5: "paused",
                    15: "unclassified"}   # kind 26's top byte, first match wins
JQ2_DEPTH_BUCKETS = ("1", "2-4", "5-16", "17-64", "65+")  # PUSH depth, b >> 8
JQ2_IDLE_DEPTH = 0xFFFFFF  # kind 26: depth at park | reason << 24
JQ2_IMAGE_UNKNOWN = 0xFFFFFFFF  # kind 29 b: the image table was full, so the job has no J line
JQ2_WALK_LIMIT = 100000  # steps of the backward critical-path walk per wait, then the rest is unresolved
JQ2_CP_CATEGORIES = ("credit_wake", "exec", "trace", "admission", "parked", "barrier_release",
                     "push", "unresolved")   # the partition of a wait's [S0, S1]
JQ2_CP_LABELS = {"credit_wake": "credit+wake", "exec": "exec (job minus image)",
                 "trace": "trace (image)", "admission": "admission", "parked": "parked",
                 "barrier_release": "barrier-release", "push": "push", "unresolved": "unresolved"}
AUDIO_BLOCK_MS = 16.0 / 3.0  # the firmware's 256-sample block at 48 kHz; the stale report's unit
AUDIO_STALE_BANDS = ((1, 1, "1"), (2, 2, "2"), (3, 4, "3-4"), (5, 8, "5-8"), (9, None, "9+"))
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
    names, spans, images, header = {}, [], {}, ""
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
                if line.startswith("J "):
                    # kind 29's image table: J <index> <fp, 16 hex> <image_ea, 8 hex> <bytes, decimal>.
                    # It is not a span; a trace written before kind 29 has none.
                    try:
                        _, index, fp, ea, nbytes = line.split()
                        images[int(index)] = (int(fp, 16), int(ea, 16), int(nbytes))
                    except ValueError:
                        raise BadInput(f"{path}:{no}: not an image line: {line.strip()[:80]!r}") from None
                    continue
                try:
                    k, who, a, b, t0, t1 = line.split()
                    spans.append((int(k), int(who), int(a, 16), int(b, 16), float(t0) / 1e3, float(t1) / 1e3))
                except ValueError:
                    raise BadInput(f"{path}:{no}: not a span line: {line.strip()[:80]!r}") from None
    except OSError as e:
        raise BadInput(f"{path}: {e.strerror}") from None
    spans.sort(key=lambda s: s[4])
    return header, names, spans, images


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


def header_jq2_images(header):
    """The image table's set slots from the header, or None for a trace written before it."""
    m = re.search(r"\bjq2_images=(\d+)", header)
    return int(m.group(1)) if m else None


def header_jq2_image_overflow(header):
    """The image table's lost-slot count from the header, or None when it names none."""
    m = re.search(r"\bjq2_image_overflow=(\d+)", header)
    return int(m.group(1)) if m else None


def header_window(header):
    """The recorder's window in ms from the trace header, or None when it names none.

    The spans use the same microsecond origin as start_us/end_us, so the window is one
    denominator for every queue; without it a queue whose spans happen to bracket a
    shorter stretch would look busier than one that ran the whole recording.
    """
    m = re.search(r"\bstart_us=([0-9.]+)\s+end_us=([0-9.]+)", header)
    if not m:
        return None
    start, end = float(m.group(1)) / 1000.0, float(m.group(2)) / 1000.0
    return (start, end) if end > start else None


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


def jq2_depth_bucket(depth):
    """The contract's PUSH depth buckets: 1, 2-4, 5-16, 17-64, 65+."""
    if depth <= 1:
        return "1"
    if depth <= 4:
        return "2-4"
    if depth <= 16:
        return "5-16"
    if depth <= 64:
        return "17-64"
    return "65+"


def jq2_open_counts(intervals, w0, w1):
    """Time-weighted mean and max of the number of `intervals` open at once over [w0, w1].

    The intervals are half-open, so a job admissible at t is not counted as held at t and the
    boundary where one job's held interval ends and its ready interval begins counts neither.
    """
    events = []
    for a, b in intervals:
        a, b = max(a, w0), min(b, w1)
        if b > a:
            events.append((a, 1))
            events.append((b, -1))
    events.sort()
    area, cur, peak, last = 0.0, 0, 0, None
    for t, delta in events:
        if last is not None and cur > 0:
            area += cur * (t - last)
        last, cur = t, cur + delta
        peak = max(peak, cur)
    return {"mean": area / (w1 - w0) if w1 > w0 else None, "max": peak}


def jq2_overlap_len(a0, a1, iv):
    """The time of [a0, a1] inside the disjoint sorted intervals iv."""
    total = 0.0
    for b0, b1 in iv:
        if b1 <= a0:
            continue
        if b0 >= a1:
            break
        total += min(a1, b1) - max(a0, b0)
    return total


def jq2_intersect(a, b):
    """The intersection of two disjoint sorted interval lists, as a disjoint sorted list."""
    out, i, j = [], 0, 0
    while i < len(a) and j < len(b):
        lo, hi = max(a[i][0], b[j][0]), min(a[i][1], b[j][1])
        if hi > lo:
            out.append((lo, hi))
        if a[i][1] < b[j][1]:
            i += 1
        else:
            j += 1
    return out


def jq2_job_before(entry, t, exclude=None):
    """The latest job of one worker (or of the queue) ending at or before t; None when none.

    entry is (jobs sorted by end, their ends). A tie on the end keeps the job that started
    later, so a zero-length predecessor cannot hide the one that actually ran. `exclude` is
    the seq of the job being walked: a zero-length job must not be its own predecessor.
    """
    if entry is None:
        return None
    jobs, ends = entry
    i = bisect.bisect_right(ends, t) - 1
    while i >= 0 and exclude is not None and jobs[i][1] == exclude:
        i -= 1
    return jobs[i] if i >= 0 else None


def jq2_barrier_releaser(ctx, tag, release, job_seq):
    """The job whose completion released the barrier that held `tag` at `release`.

    The release time is recorded once, so the barrier is the span of this queue whose t1
    equals the release exactly and whose tag set includes the tag; the releaser is its
    releasing worker's latest job ending at or before the release. A trace without kinds
    28 falls back to the queue's latest job ending by the release. None when the releaser
    cannot be established: a wrong guess would put another job's image on the critical
    path. The released job is never its own releaser.
    """
    for barrier in ctx["barriers_by_t1"].get(release, ()):
        if barrier[1] & (1 << tag):
            return jq2_job_before(ctx["jobs_of_worker"].get(barrier[0]), release, job_seq)
    return jq2_job_before(ctx["jobs_all"], release, job_seq)


def jq2_walk(ctx, s0, s1, terminal_seq):
    """One event-flag wait's backward walk over the jobs and barriers that kept it blocked.

    The walk starts at the terminal job (the (q, seq) of the due credit that ended the wait)
    and follows, per contract, whichever link kept each job from starting earlier: capacity
    (the previous job of the same worker was still running when the job was admissible), the
    release of the barrier that held its tag, or the push that made it visible. Every
    segment is clipped to [s0, s1] and to a cursor that only decreases, so the categories
    add up to s1 - s0; a link that cannot be followed leaves the rest unresolved instead of
    guessing. The image identification span of every job on the chain is `trace`, the rest
    of its JOB span is `exec` and is charged to that job's fingerprint.

    Returns the partition, the exec milliseconds per fingerprint, the number of jobs walked,
    and whether the step guard fired.
    """
    parts = dict.fromkeys(JQ2_CP_CATEGORIES, 0.0)
    fp_exec = collections.defaultdict(float)
    job = ctx["job_of_seq"].get(terminal_seq)
    result = {"parts": parts, "fp_exec": fp_exec, "chain": 0, "guard": False,
              "terminal_seq": terminal_seq, "has_terminal": True,
              "terminal_fp": None if job is None else ctx["fp_of_seq"].get(terminal_seq)}
    if job is None:
        # The credit names a (q, seq) with no JOB span: the wait is attributed to that job by
        # seq, but there is no interval to walk.
        parts["unresolved"] += s1 - s0
        return result

    c = s1
    chain, steps, guard, first = 0, 0, False, True

    def adv(raw):
        nonlocal c
        c = min(c, raw)

    def take(cat, a, b):
        lo, hi = max(a, s0), min(b, c, s1)
        if hi > lo:
            parts[cat] += hi - lo

    def take_exec(j):
        lo, hi = max(j[3], s0), min(c, s1)
        if hi > lo:
            img = ctx["image_of_seq"].get(j[1])
            cut = (max(img[1], lo), min(img[2], hi)) if img is not None else None
            if cut is not None and cut[1] > cut[0]:
                parts["trace"] += cut[1] - cut[0]
                pieces = ((lo, cut[0]), (cut[1], hi))
            else:
                pieces = ((lo, hi),)
            fp = ctx["fp_of_seq"].get(j[1])
            for a, b in pieces:
                if b > a:
                    parts["exec"] += b - a
                    fp_exec[fp] += b - a
        adv(j[3])

    while True:
        steps += 1
        if steps > JQ2_WALK_LIMIT:
            guard = True
            take("unresolved", s0, c)
            adv(s0)
            break
        if first:
            take("credit_wake", job[4], c)
            adv(job[4])
            first = False
        chain += 1
        take_exec(job)
        if c <= s0:
            break
        held = ctx["held_of_seq"].get(job[1])
        push = ctx["push_of_seq"].get(job[1])
        if held is not None:
            a = held[2]
        elif push is not None:
            a = push[1]
        else:
            take("unresolved", s0, c)
            adv(s0)
            break
        w = job[0]
        k = jq2_job_before(ctx["jobs_of_worker"].get(w), job[3], job[1])
        if k is not None and k[4] > a:
            take("admission", k[4], c)
            adv(k[4])
            job = k
            continue
        a = min(a, c)
        lo = max(a, s0)
        if c > lo:
            parked = jq2_overlap_len(lo, c, ctx["idle_of_worker"].get(w, ()))
            parts["parked"] += parked
            parts["admission"] += (c - lo) - parked
        adv(a)
        if c <= s0:
            break
        if held is not None and (held[0] >> 4) & 0xF == 1:
            r = jq2_barrier_releaser(ctx, held[0] & 0xF, held[2], job[1])
            if r is None:
                take("unresolved", s0, c)
                adv(s0)
                break
            take("barrier_release", r[4], c)
            adv(r[4])
            job = r
            continue
        take("push", s0, c)
        adv(s0)
        break
    result["chain"] = chain
    result["guard"] = guard
    return result


def jq2_mean(xs):
    return sum(xs) / len(xs) if xs else None


def jq2_exec_stats(walls):
    """total, mean, nearest-rank p50 and p95 of the exec walls."""
    xs = sorted(walls)
    total = sum(xs)
    return {"total_ms": total, "mean_ms": total / len(xs) if xs else None,
            "p50_ms": nearest_rank(xs, 50) if xs else None,
            "p95_ms": nearest_rank(xs, 95) if xs else None}


def jq2_image_rows(jobs, cp_exec, total_wait_ms):
    """The image table of one queue (or of all queues): jobs grouped by fingerprint.

    A fingerprint can run from more than one (ea, bytes) triple and on more than one queue;
    the rows list every variant and the queues the jobs were seen on. `cp_exec` is the exec
    time the walks charged to each fingerprint, so the share is of the blocking-wait time,
    not of CPU: a short image that blocks a thread outranks a long one that blocks nobody.
    The image identification span is already out of the exec wall (JOB minus IMAGE); CPU is
    JOB.b and does include it. Rows sort by critical path, then exec total; cp_rank follows
    that order and cpu_rank the CPU total, so the two are side by side.
    """
    groups = {}
    for j in jobs:
        g = groups.setdefault(j["fp"], {"jobs": 0, "walls": [], "cpu": 0.0, "lat": [], "held": [],
                                        "ready": [], "variants": set(), "queues": set(), "clamped": 0})
        g["jobs"] += 1
        g["walls"].append(j["wall"])
        g["cpu"] += j["cpu"]
        g["clamped"] += 1 if j["clamped"] else 0
        if j["lat"] is not None:
            g["lat"].append(j["lat"])
        if j["held"] is not None:
            g["held"].append(j["held"])
        if j["ready"] is not None:
            g["ready"].append(j["ready"])
        if j["variant"] is not None:
            g["variants"].add(j["variant"])
        g["queues"].add(j["q"])
    rows = []
    for fp, g in groups.items():
        cp = cp_exec.get(fp, 0.0)
        rows.append({
            "fingerprint": fp, "unknown": fp is None,
            "variants": [{"ea": ea, "bytes": nb} for ea, nb in sorted(g["variants"])],
            "queues": sorted(g["queues"]), "jobs": g["jobs"],
            "exec_wall_ms": jq2_exec_stats(g["walls"]), "exec_wall_clamped": g["clamped"],
            "cpu_ms": g["cpu"],
            "queue_latency_mean_ms": jq2_mean(g["lat"]), "queue_latency_count": len(g["lat"]),
            "held_mean_ms": jq2_mean(g["held"]), "held_count": len(g["held"]),
            "ready_mean_ms": jq2_mean(g["ready"]), "ready_count": len(g["ready"]),
            "critical_path_ms": cp,
            "critical_path_share": cp / total_wait_ms if total_wait_ms > 0 else None,
        })
    rows.sort(key=lambda r: (-r["critical_path_ms"], -r["exec_wall_ms"]["total_ms"],
                             r["fingerprint"] or ""))
    for rank, i in enumerate(sorted(range(len(rows)),
                                    key=lambda i: (-rows[i]["cpu_ms"], rows[i]["fingerprint"] or "")), 1):
        rows[i]["cpu_rank"] = rank
    for rank, row in enumerate(rows, 1):
        row["cp_rank"] = rank
    return rows, sum(row["exec_wall_clamped"] for row in rows)


def jq2_audio_stale(stale_spans, wait_records, names, window=None, window_source="extent"):
    """Attribute every AUDIO_STALE block to the job waits of the threads that have them.

    AUDIO_STALE is emitted once per not-refilled block, with b = the block's position in its
    run (1, 2, ..., n; a non-silent block resets it), so the runs are rebuilt per port in time
    order: a run starts at b == 1 and its length is the last b before the port's next run start
    or the window end. A port's first event in the window with b > 1 opened before the window;
    it is counted with the length seen and flagged open_at_start, so the histogram's block
    total (the sum of the run lengths) can exceed the number of events in the window.

    Attribution stays per block: the block has a port, not a thread, so every thread with
    event-flag waits is asked whether the instant lies in one of its waits; a thread whose wait
    covers it names the terminal job and the wait's dominant image (the fingerprint with the
    most exec ms in that wait's partition). The block is the firmware's 256 samples at 48 kHz =
    16/3 ms. Fiber waits are not waits that a job can be named for and are not consulted.

    An in-wait block is a coincidence in time, not proof of causation: a thread that waits a
    large share of the window is in a wait at that share of random instants. Each thread row
    therefore carries its wait share (its event-flag wait time clipped to the window divided by
    the window), the count expected from that share times the stale blocks, and the ratio of
    observed to expected; the fingerprint rows do the same for their share of the terminal-job
    wait time, summed over threads. A ratio near 1 is no association.
    """
    total = len(stale_spans)
    win0, win1 = window if window is not None else (None, None)
    window_ms = (win1 - win0) if window is not None else None
    window_ok = window_ms is not None and window_ms > 0
    wait_ms_by_tid = collections.Counter()
    terminal_wait_by_tid = collections.defaultdict(collections.Counter)
    if window_ok:
        for rec in wait_records:
            dur = overlap(rec["s0"], rec["s1"], win0, win1)
            if dur > 0:
                wait_ms_by_tid[rec["tid"]] += dur
                if rec["has_terminal"]:
                    terminal_wait_by_tid[rec["tid"]][rec["terminal_fp"]] += dur
    by_port = collections.defaultdict(list)
    for i, ev in enumerate(stale_spans):
        by_port[ev[1]].append((i, ev))

    def band_of(n):
        for i, (lo, hi, _) in enumerate(AUDIO_STALE_BANDS):
            if n >= lo and (hi is None or n <= hi):
                return i
        return 0 if n < 1 else len(AUDIO_STALE_BANDS) - 1

    def new_hist():
        return [{"band": label, "min_blocks": lo, "max_blocks": hi, "runs": 0, "blocks": 0}
                for lo, hi, label in AUDIO_STALE_BANDS]

    def fill_hist(selected):
        hist = new_hist()
        for run in selected:
            h = hist[band_of(run["length"])]
            h["runs"] += 1
            h["blocks"] += run["length"]
        for h in hist:
            h["ms"] = h["blocks"] * AUDIO_BLOCK_MS
            h["mean_ms"] = h["ms"] / h["runs"] if h["runs"] else None
        return hist

    # Rebuild each port's runs. A b == 1 closes the previous run with the last position seen;
    # a first event with b > 1 is a run that began before the window.
    runs, run_of_block = [], {}
    for port in sorted(by_port):
        cur = None
        for i, ev in by_port[port]:
            pos = ev[3]
            if pos == 1:
                if cur is not None:
                    runs.append(cur)
                cur = {"port": port, "length": 0, "open_at_start": False}
            elif cur is None:
                cur = {"port": port, "length": 0, "open_at_start": True}
            cur["length"] = pos
            run_of_block[i] = cur
        if cur is not None:
            runs.append(cur)

    overall_hist = fill_hist(runs)
    per_port = []
    for port in sorted(by_port):
        port_runs = [r for r in runs if r["port"] == port]
        per_port.append({
            "port": port, "runs": len(port_runs),
            "blocks": sum(r["length"] for r in port_runs),
            "runs_ge_2_blocks": sum(1 for r in port_runs if r["length"] >= 2),
            "open_at_start": sum(1 for r in port_runs if r["open_at_start"]),
            "histogram": fill_hist(port_runs),
        })

    waits_by_tid = collections.defaultdict(list)
    for rec in wait_records:
        waits_by_tid[rec["tid"]].append(rec)
    starts = {}
    for tid, recs in waits_by_tid.items():
        recs.sort(key=lambda r: r["s0"])
        starts[tid] = [r["s0"] for r in recs]

    def wait_at(tid, t):
        recs = waits_by_tid.get(tid)
        if not recs:
            return None
        i = bisect.bisect_right(starts[tid], t) - 1
        return recs[i] if i >= 0 and t <= recs[i]["s1"] else None

    threads = [{"tid": tid, "name": (names or {}).get(tid, "?"), "waits": len(recs),
                "blocks_in_wait": 0, "blocks_not_in_wait": 0,
                "wait_share": (wait_ms_by_tid[tid] / window_ms) if window_ok else None,
                "expected_blocks_in_wait": ((wait_ms_by_tid[tid] / window_ms) * total
                                            if window_ok else None),
                "in_wait_ratio": None}
               for tid, recs in sorted(waits_by_tid.items())]
    thread_of = {row["tid"]: row for row in threads}
    fp_counts = collections.defaultdict(lambda: {"terminal_blocks": 0, "dominant_blocks": 0})
    events, in_any = [], 0
    for i, ev in enumerate(stale_spans):
        t, pos = ev[4], ev[3]
        run = run_of_block[i]
        hits = []
        for tid in sorted(waits_by_tid):
            rec = wait_at(tid, t)
            if rec is None:
                thread_of[tid]["blocks_not_in_wait"] += 1
                continue
            thread_of[tid]["blocks_in_wait"] += 1
            if rec["has_terminal"]:
                fp_counts[rec["terminal_fp"]]["terminal_blocks"] += 1
            if rec["has_exec"]:
                fp_counts[rec["dominant_fp"]]["dominant_blocks"] += 1
            hits.append({
                "tid": tid, "q": rec["q"], "seq": rec["terminal_seq"],
                "terminal": ({"fingerprint": rec["terminal_fp"], "unknown": rec["terminal_fp"] is None}
                             if rec["has_terminal"] else None),
                "waited_ms": t - rec["s0"],
                "dominant": ({"fingerprint": rec["dominant_fp"], "unknown": rec["dominant_fp"] is None}
                             if rec["has_exec"] else None),
            })
        if hits:
            in_any += 1
        events.append({"at_ms": t, "port": ev[1], "block": ev[2], "run_pos": pos,
                       "run_len": run["length"], "run_ms": pos * AUDIO_BLOCK_MS,
                       "band": AUDIO_STALE_BANDS[band_of(run["length"])][2], "waits": hits})
    for row in threads:
        exp = row["expected_blocks_in_wait"]
        row["in_wait_ratio"] = (row["blocks_in_wait"] / exp) if exp else None
    fp_expected = collections.Counter()
    if window_ok:
        for by_fp in terminal_wait_by_tid.values():
            for fp, ms in by_fp.items():
                fp_expected[fp] += (ms / window_ms) * total
    fingerprints = []
    for fp in sorted(set(fp_counts) | set(fp_expected),
                     key=lambda fp: (-(fp_counts.get(fp, {}).get("terminal_blocks", 0) +
                                       fp_counts.get(fp, {}).get("dominant_blocks", 0)),
                                     fp is None, fp or "")):
        v = fp_counts.get(fp, {"terminal_blocks": 0, "dominant_blocks": 0})
        exp = fp_expected[fp] if window_ok else None
        fingerprints.append({"fingerprint": fp, "unknown": fp is None,
                             "terminal_blocks": v["terminal_blocks"],
                             "dominant_blocks": v["dominant_blocks"],
                             "terminal_expected_blocks": exp,
                             "terminal_ratio": (v["terminal_blocks"] / exp) if exp else None})
    return {"count": total, "block_ms": AUDIO_BLOCK_MS,
            "window": {"start_ms": win0, "end_ms": win1, "ms": window_ms,
                       "source": window_source if window_ok else None},
            "runs": len(runs), "blocks": sum(r["length"] for r in runs),
            "runs_ge_2_blocks": sum(1 for r in runs if r["length"] >= 2),
            "open_at_start": sum(1 for r in runs if r["open_at_start"]),
            "histogram": overall_hist, "per_port": per_port,
            "blocks_in_wait": in_any, "blocks_not_in_wait": total - in_any,
            "threads": threads, "fingerprints": fingerprints, "events": events[:20]}


def jq2_section(spans, header, names=None, images=None):
    """The SPURS job queue (jq2) section, or None when the trace has no kinds 22-28.

    Queue latency, execution wall and CPU, and completion delay are keyed on (q, seq):
    seq counts per queue and every queue starts at 1, so a join that ignores q pairs one
    queue's push with another queue's job and reports a latency neither saw. Sync
    latency is the wait for a completion signal to reach the blocked PPU thread, joined
    on (q, waiter) against the last event-flag credit that landed inside that wait; the
    fiber path has no waiter id (0xFF, wake 2), so a fiber wait cannot be joined and is
    reported apart rather than miscounted against the event flag. A join that finds no
    partner is counted, never dropped: a span cut off at a window edge loses its
    partner, and hiding that would make the latencies look better than the trace is.

    Kinds 27 (HELD) and 28 (BARRIER) split a job's wait into the part the guest ordered
    (a SYNC barrier held its tag) and the part the host owes (it was admissible and a
    worker did not take it): held = HELD.t1 - HELD.t0, ready wait = JOB.t0 - HELD.t1,
    both joined on (q, seq). The ready wait is split per job by the queue's own spans:
    time with a worker parked (IDLE), time with every worker of the queue in a JOB span
    (capacity), and the rest (workers in transition). Per job, so the three parts add
    up to the summed ready wait inside the recorder's window. A trace without the two
    kinds (a recording made before them) reports the old section and says the
    decomposition needs them.

    Kind 29 (IMAGE) and the `J` lines add image identity: `b` is the image table index, the
    `J` line names (fingerprint, image_ea, image_bytes), and 0xFFFFFFFF or an index without a
    `J` line is the unknown image. Every event-flag wait then gets a backward walk from the
    job its due credit names through the capacity, barrier-release and push links that kept
    it blocked, which is how an image is charged with the blocking time it caused rather than
    with the CPU it used. A trace without kind 29 prints no image sections.
    """
    js = [s for s in spans if JQ2_PUSH <= s[0] <= JQ2_IMAGE]
    if not js:
        return None
    has_images = any(k == JQ2_IMAGE for k, _, _, _, _, _ in js)
    img_table = images or {}
    by_q = collections.defaultdict(lambda: {"push": [], "job": [], "credit": [], "wait": [],
                                            "idle": [], "held": [], "barrier": [], "image": [],
                                            "bounds": []})
    for k, who, a, b, t0, t1 in js:
        if k == JQ2_PUSH:                 # who = tid, a = seq, b = q | depth << 8
            g = by_q[b & 0xFF]
            g["push"].append((a, t0, t1, b >> 8))
        elif k == JQ2_SYNC_WAIT:          # who = tid, a = waiter | wake << 8 | mode << 16, b = q
            g = by_q[b & 0xFF]
            g["wait"].append((who, a & 0xFF, a >> 8 & 0xFF, t0, t1))
        else:                             # who = q << 8 | w
            g = by_q[who >> 8]
            if k == JQ2_JOB:              # a = seq, b = thread CPU time, us
                g["job"].append((who & 0xFF, a, b, t0, t1))
            elif k == JQ2_CREDIT:         # a = seq, b = waiter | wake << 8 | due << 16
                g["credit"].append((who & 0xFF, a, b, t0, t1))
            elif k == JQ2_HELD:           # a = seq, b = tag | cause << 4 | mask << 8
                g["held"].append((who & 0xFF, a, b, t0, t1))
            elif k == JQ2_BARRIER:        # a = tags | mask << 16, b = q | waited << 8
                g["barrier"].append((who & 0xFF, a, b, t0, t1))
            elif k == JQ2_IMAGE:          # a = seq, b = image table index (J lines)
                g["image"].append((who & 0xFF, a, b, t0, t1))
            else:                         # JQ2_IDLE: a = depth | reason << 24, b = depth at wake
                g["idle"].append((who & 0xFF, a, b, t0, t1))
        g["bounds"].append((t0, t1))

    queues = []
    cp_rows_all, cp_queue_rows, images_per_queue, trace_cost_rows = [], [], [], []
    all_image_jobs, all_wait_records = [], []
    overall_cp_exec = collections.defaultdict(float)
    overall_cp_total = 0.0
    image_clamped_total = 0
    hw = header_window(header)
    # Kind 27 and 28 are what the decomposition rests on; a trace that predates them has neither, and its
    # section stays as it was plus one note line.
    has_decomp = any(k in (JQ2_HELD, JQ2_BARRIER) for k, _, _, _, _, _ in js)
    for q in sorted(by_q):
        g = by_q[q]
        pushes, jobs, credits = g["push"], g["job"], g["credit"]
        waits, idles, held, barriers = g["wait"], g["idle"], g["held"], g["barrier"]
        w0 = min(t0 for t0, _ in g["bounds"])
        w1 = max(t1 for _, t1 in g["bounds"])
        # One window for every queue: the recorder's, so the shares compare; the queue's
        # own extent only stands in when the header names no window.
        win_start, win_end = hw if hw is not None else (w0, w1)
        window = win_end - win_start

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
        paired_waits = []   # (tid, t0, t1, waiter, terminal credit or None), in wait order
        fiber = [t1 - t0 for _, _, wake, t0, t1 in waits if wake == JQ2_FIBER_WAKE]
        for tid, waiter, wake, wt0, wt1 in sorted(waits, key=lambda s: s[3]):
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
                paired_waits.append((tid, wt0, wt1, waiter, None))
            else:
                used.add(hit[1])
                sync.append(wt1 - hit[0])
                paired_waits.append((tid, wt0, wt1, waiter, credits[hit[1]]))
        due_total = sum(len(v) for v in due.values())

        # Kind 29 joins to JOB by (q, seq); the image table's index -> (fp, ea, bytes) turns it
        # into the fingerprint a port registers a lifted image under. 0xFFFFFFFF (the table was
        # full) and an index without a J line are the unknown image, counted as one row, never
        # dropped.
        image_by_seq = {}
        for _, seq, index, it0, it1 in g["image"]:
            image_by_seq.setdefault(seq, (index, it0, it1))
        fp_of_seq = {}
        for seq, (index, _, _) in image_by_seq.items():
            entry = None if index == JQ2_IMAGE_UNKNOWN else img_table.get(index)
            fp_of_seq[seq] = f"{entry[0]:016x}" if entry is not None else None
        held_by_seq = {}
        for _, seq, hb, ht0, ht1 in held:
            held_by_seq.setdefault(seq, (hb, ht0, ht1))
        push_of_seq = {seq: (p0, p1, depth) for seq, p0, p1, depth in pushes}

        # The walk asks for the latest job of a worker (capacity) or of the queue (a barrier
        # releaser without its own BARRIER span) ending at or before a time, and for the idle
        # spans of a worker (parked admission), all of one queue.
        jobs_sorted = sorted(jobs, key=lambda j: (j[4], j[3]))
        per_worker = collections.defaultdict(list)
        for j in jobs_sorted:
            per_worker[j[0]].append(j)
        jobs_of_worker = {w: (v, [j[4] for j in v]) for w, v in per_worker.items()}
        idle_of_worker = collections.defaultdict(list)
        for w, _, _, it0, it1 in idles:
            idle_of_worker[w].append((it0, it1))
        barriers_by_t1 = collections.defaultdict(list)
        for barrier in barriers:
            barriers_by_t1[barrier[4]].append(barrier)
        ctx = {"job_of_seq": {j[1]: j for j in jobs}, "fp_of_seq": fp_of_seq,
               "image_of_seq": image_by_seq, "held_of_seq": held_by_seq,
               "push_of_seq": push_of_seq, "jobs_of_worker": jobs_of_worker,
               "jobs_all": (jobs_sorted, [j[4] for j in jobs_sorted]),
               "barriers_by_t1": barriers_by_t1,
               "idle_of_worker": {w: merged(v) for w, v in idle_of_worker.items()}}

        # Section 1: every event-flag wait is walked back from its terminal job, so the
        # partition and the exec time charged to each image are per (q, tid).
        q_cp_exec = collections.defaultdict(float)
        cp_by_tid = collections.defaultdict(lambda: {"waits": 0, "without_credit": 0,
                                                     "total_ms": 0.0,
                                                     "parts": dict.fromkeys(JQ2_CP_CATEGORIES, 0.0),
                                                     "chains": [], "guard_hits": 0})
        wait_records = []
        q_cp_total = 0.0
        for tid, wt0, wt1, _, credit in paired_waits:
            row = cp_by_tid[tid]
            row["waits"] += 1
            row["total_ms"] += wt1 - wt0
            q_cp_total += wt1 - wt0
            if credit is None:
                row["without_credit"] += 1
                row["parts"]["unresolved"] += wt1 - wt0
                wait_records.append({"q": q, "tid": tid, "s0": wt0, "s1": wt1,
                                     "terminal_seq": None, "terminal_fp": None,
                                     "has_terminal": False, "has_exec": False, "dominant_fp": None})
                continue
            walk = jq2_walk(ctx, wt0, wt1, credit[1])
            for cat, ms in walk["parts"].items():
                row["parts"][cat] += ms
            row["chains"].append(walk["chain"])
            row["guard_hits"] += 1 if walk["guard"] else 0
            for fp, ms in walk["fp_exec"].items():
                q_cp_exec[fp] += ms
            fp_exec = walk["fp_exec"]
            has_exec = bool(fp_exec)
            dominant = (max(fp_exec.items(), key=lambda kv: (kv[1], kv[0] is not None, kv[0] or ""))[0]
                        if has_exec else None)
            wait_records.append({"q": q, "tid": tid, "s0": wt0, "s1": wt1,
                                 "terminal_seq": walk["terminal_seq"],
                                 "terminal_fp": walk["terminal_fp"], "has_terminal": True,
                                 "has_exec": has_exec, "dominant_fp": dominant})
        fiber_by_tid = collections.Counter(tid for tid, _, wake, _, _ in waits
                                           if wake == JQ2_FIBER_WAKE)
        cp_rows = []
        for tid in sorted(set(cp_by_tid) | set(fiber_by_tid)):
            row = cp_by_tid.get(tid)
            if row is None:
                row = {"waits": 0, "without_credit": 0, "total_ms": 0.0,
                       "parts": dict.fromkeys(JQ2_CP_CATEGORIES, 0.0), "chains": [],
                       "guard_hits": 0}
            total = row["total_ms"]
            cp_rows.append({
                "q": q, "tid": tid, "name": (names or {}).get(tid, "?"),
                "waits": row["waits"], "fiber_waits": fiber_by_tid.get(tid, 0),
                "waits_without_credit": row["without_credit"], "total_ms": total,
                "partition_ms": dict(row["parts"]),
                "partition_share": {cat: (row["parts"][cat] / total if total > 0 else None)
                                    for cat in JQ2_CP_CATEGORIES},
                "mean_chain": jq2_mean(row["chains"]), "guard_hits": row["guard_hits"],
            })
        cp_queue_rows.append({"q": q, "waits": len(paired_waits),
                              "fiber_waits": sum(fiber_by_tid.values()),
                              "waits_without_credit": sum(r["waits_without_credit"] for r in cp_rows),
                              "total_ms": q_cp_total,
                              "guard_hits": sum(r["guard_hits"] for r in cp_rows)})
        cp_rows_all.extend(cp_rows)
        overall_cp_total += q_cp_total

        # Section 2: one row per fingerprint that ran on this queue. Exec wall is the JOB span
        # minus its IMAGE span (identification is reported as trace cost, section 4); CPU is
        # JOB.b and includes that span.
        image_jobs = []
        for w, seq, cpu_us, jt0, jt1 in jobs:
            img = image_by_seq.get(seq)
            entry = None
            if img is not None and img[0] != JQ2_IMAGE_UNKNOWN:
                entry = img_table.get(img[0])
            ident = (img[2] - img[1]) if img is not None else 0.0
            raw_wall = (jt1 - jt0) - ident
            pushed = push_of_seq.get(seq)
            hb = held_by_seq.get(seq)
            image_jobs.append({
                "fp": f"{entry[0]:016x}" if entry is not None else None,
                "wall": max(0.0, raw_wall), "clamped": raw_wall < 0.0,
                "cpu": cpu_us / 1000.0,
                "lat": None if pushed is None else max(0.0, jt0 - pushed[1]),
                "held": None if hb is None else hb[2] - hb[1],
                "ready": None if hb is None else max(0.0, jt0 - hb[2]),
                "variant": (entry[1], entry[2]) if entry is not None else None, "q": q})
        image_rows, image_clamped = jq2_image_rows(image_jobs, q_cp_exec, q_cp_total)
        images_per_queue.append({"q": q, "rows": image_rows})
        all_image_jobs.extend(image_jobs)
        image_clamped_total += image_clamped
        for fp, ms in q_cp_exec.items():
            overall_cp_exec[fp] += ms
        trace_cost_rows.append({"q": q,
                                "image_ms": sum(img[4] - img[3] for img in g["image"]),
                                "job_ms": sum(j[4] - j[3] for j in jobs)})
        all_wait_records.extend(wait_records)

        # A worker that parked before the recorder opened, or whose span was still open when
        # it closed, reaches past the window; summed whole it charges time the window does
        # not own, and busy + idle can pass the window itself. Clip first.
        busy, idle_ms, job_count = (collections.defaultdict(float), collections.defaultdict(float),
                                    collections.Counter())
        for w, _, _, jt0, jt1 in jobs:
            busy[w] += overlap(jt0, jt1, win_start, win_end)
            job_count[w] += 1
        for w, _, _, it0, it1 in idles:
            idle_ms[w] += overlap(it0, it1, win_start, win_end)
        workers = sorted(set(busy) | set(idle_ms))
        parked = [overlap(t0, t1, win_start, win_end) for _, a, _, t0, t1 in idles
                  if a & JQ2_IDLE_DEPTH]  # depth at park is the low 24 bits of a

        depths = sorted(p[3] for p in pushes)
        waits_by_tid = collections.defaultdict(list)
        for tid, _, _, t0, t1 in waits:
            waits_by_tid[tid].append(t1 - t0)
        total_wall = sum(wall)

        # Park reasons (kind 26's top byte), summed in the same clipped window as the idle time above
        # so the shares add up to the idle the report shows.
        idle_total = sum(idle_ms.values())
        parks = {}
        for _, a, _, it0, it1 in idles:
            row = parks.setdefault(a >> 24, {"count": 0, "total_ms": 0.0, "pending_count": 0,
                                             "pending_ms": 0.0})
            span_ms = overlap(it0, it1, win_start, win_end)
            row["count"] += 1
            row["total_ms"] += span_ms
            if a & JQ2_IDLE_DEPTH:
                row["pending_count"] += 1
                row["pending_ms"] += span_ms
        park_reasons = [
            {"reason": reason, "name": JQ2_PARK_REASONS.get(reason, f"reason {reason}"),
             "count": v["count"], "total_ms": v["total_ms"],
             "share": (v["total_ms"] / idle_total) if idle_total > 0 else None,
             "pending_count": v["pending_count"], "pending_ms": v["pending_ms"],
             "pending_share": (v["pending_ms"] / idle_total) if idle_total > 0 else None}
            for reason, v in sorted(parks.items())]

        # Kind 27 and 28 are what the decomposition rests on. Availability is a property of the trace: a queue
        # with no held job of its own still belongs to a recording that has the kinds.
        available = has_decomp
        held_ms, ready_wait = [], []
        ready_clamped = 0
        for _, seq, _, jt0, _ in jobs:
            hb = held_by_seq.get(seq)
            if hb is None:
                continue  # HELD without JOB: the join failure below counts it
            held_ms.append(hb[2] - hb[1])
            d = jt0 - hb[2]
            if d < 0.0:  # impossible by construction; a clock that went backwards is counted, not hidden
                d, ready_clamped = 0.0, ready_clamped + 1
            ready_wait.append(d)
        held_sum, ready_sum = sum(held_ms), sum(ready_wait)
        both = held_sum + ready_sum

        # The ready wait split by the queue's own spans, per job interval: time with at least one worker
        # of the queue parked (host scheduling it could have avoided), time with every worker of the queue
        # inside a JOB span (capacity: all busy), and the rest (workers in transition: intake pass,
        # admission, credit). Weighted per job, so the three parts add up to the summed ready wait inside
        # the window. "Every worker" is the workers list above: distinct in JOB or IDLE for this queue.
        idle_union = merged([(t0, t1) for _, _, _, t0, t1 in idles])
        busy_by_worker = collections.defaultdict(list)
        for w, _, _, jt0, jt1 in jobs:
            busy_by_worker[w].append((jt0, jt1))
        all_busy = None
        for w in workers:
            interval = merged(busy_by_worker.get(w, []))
            all_busy = interval if all_busy is None else jq2_intersect(all_busy, interval)
        all_busy = all_busy or []
        parked_ready = all_busy_ready = transition_ready = open_ready = 0.0
        for _, seq, _, jt0, _ in jobs:
            hb = held_by_seq.get(seq)
            if hb is None:
                continue
            r0, r1 = max(hb[2], win_start), min(jt0, win_end)
            if r1 <= r0:
                continue
            open_ready += r1 - r0
            rest = minus([(r0, r1)], idle_union)
            parked_ready += (r1 - r0) - sum(b - a for a, b in rest)
            for a0, a1 in rest:
                # busy_part, not busy: the per-worker busy sums collected above are read by per_worker below.
                busy_part = jq2_overlap_len(a0, a1, all_busy)
                all_busy_ready += busy_part
                transition_ready += (a1 - a0) - busy_part

        depth_of = {seq: d for seq, _, _, d in pushes}
        buckets = {label: {"jobs": 0, "held": [], "ready": [], "held_by_barrier": 0}
                   for label in JQ2_DEPTH_BUCKETS}
        for _, seq, _, jt0, _ in jobs:
            depth = depth_of.get(seq)
            if depth is None:
                continue  # JOB without PUSH: already counted as a join failure
            bucket = buckets[jq2_depth_bucket(depth)]
            bucket["jobs"] += 1
            hb = held_by_seq.get(seq)
            if hb is not None:
                bucket["held"].append(hb[2] - hb[1])
                bucket["ready"].append(max(0.0, jt0 - hb[2]))
                if (hb[0] >> 4) & 0xF == 1:
                    bucket["held_by_barrier"] += 1

        # HELD's mask is the SYNC mask of the barrier that held the job, which attributes held time to the
        # barriers the guest pushed.
        mask_held = {}
        for _, seq, hb, ht0, ht1 in held:
            if (hb >> 4) & 0xF != 1:
                continue  # cause 0: never held, no mask
            row = mask_held.setdefault((hb >> 8) & 0xFFFF, {"jobs": 0, "total_ms": 0.0})
            row["jobs"] += 1
            row["total_ms"] += ht1 - ht0
        top_masks = [{"mask": mask, "jobs": v["jobs"], "total_held_ms": v["total_ms"],
                      "mean_held_ms": v["total_ms"] / v["jobs"]}
                     for mask, v in sorted(mask_held.items(),
                                           key=lambda kv: (-kv[1]["total_ms"], kv[0]))[:5]]

        barrier_ms = [t1 - t0 for _, _, _, t0, t1 in barriers]
        barrier_waited = [b >> 8 for _, _, b, _, _ in barriers]

        queues.append({
            "q": q,
            "window": {"start_ms": win_start, "end_ms": win_end, "ms": window},
            "window_source": "header" if hw is not None else "extent",
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
            "park_reasons": park_reasons if available else [],
            "decomposition_available": available,
            "latency_decomposition": {
                "held_ms": jq2_stats(held_ms),
                "ready_wait_ms": jq2_stats(ready_wait),
                "held_share": (held_sum / both) if both > 0 else None,
                "ready_share": (ready_sum / both) if both > 0 else None,
                "ready_wait_clamped": ready_clamped,
                "ready_wait_split": {
                    "parked_ms": parked_ready, "all_busy_ms": all_busy_ready,
                    "transition_ms": transition_ready, "total_ms": open_ready,
                    "parked_share": (parked_ready / open_ready) if open_ready > 0 else None,
                    "all_busy_share": (all_busy_ready / open_ready) if open_ready > 0 else None,
                    "transition_share": (transition_ready / open_ready) if open_ready > 0 else None},
                "join_failures": {"held_without_job": len(held) - len(ready_wait),
                                  "job_without_held": len(jobs) - len(ready_wait)},
            } if available else None,
            "counts_over_time": {
                # Every HELD span is a job that waited, including one cut off from its JOB at a window edge;
                # a ready interval exists only where the join found both ends.
                "held": jq2_open_counts([(ht0, ht1) for _, _, _, ht0, ht1 in held], win_start, win_end),
                "ready": jq2_open_counts([(held_by_seq[s][2], jt0) for _, s, _, jt0, _ in jobs
                                          if s in held_by_seq], win_start, win_end),
            } if available else None,
            "depth_buckets": [
                {"bucket": label, "jobs": v["jobs"], "held_jobs": len(v["held"]),
                 "held_by_barrier": v["held_by_barrier"],
                 "mean_held_ms": (sum(v["held"]) / len(v["held"])) if v["held"] else None,
                 "mean_ready_wait_ms": (sum(v["ready"]) / len(v["ready"])) if v["ready"] else None}
                for label, v in buckets.items()
            ] if available else None,
            "barriers": {
                "count": len(barriers),
                "rate_per_s": len(barriers) / (window / 1000.0) if window > 0 else None,
                "duration_ms": jq2_stats(barrier_ms),
                "waited_mean": (sum(barrier_waited) / len(barrier_waited)) if barrier_waited else None,
                "top_masks": top_masks,
            } if available else None,
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
    dropped = header_dropped(header)
    overflow = header_jq2_image_overflow(header)
    result = {"dropped": dropped, "decomposition_available": has_decomp,
              "images_available": has_images, "jq2_images": header_jq2_images(header),
              "jq2_image_overflow": overflow, "incomplete": None, "critical_path": None,
              "images": None, "audio_stale": None, "trace_cost": None, "queues": queues}
    if not has_images:
        return result
    # A loss counter or a full image table makes the image sections incomplete; saying so is
    # the only honest reading of a trace whose jobs or images are missing.
    result["incomplete"] = (dropped or 0) > 0 or (overflow or 0) > 0
    overall_rows, _ = jq2_image_rows(all_image_jobs, overall_cp_exec, overall_cp_total)
    result["critical_path"] = {"total_ms": overall_cp_total, "rows": cp_rows_all,
                               "per_queue": cp_queue_rows}
    result["images"] = {"exec_wall_clamped": image_clamped_total, "overall": overall_rows,
                        "per_queue": images_per_queue}
    # The base rate's denominator is the recorder's window, the same one the queue shares use;
    # without one in the header, the traced extent stands in so the share has a stated source.
    audio_window, audio_source = hw, "header"
    if audio_window is None:
        t0, t1 = min(s[4] for s in spans), max(s[5] for s in spans)
        audio_window, audio_source = ((t0, t1) if t1 > t0 else None), "extent"
    result["audio_stale"] = jq2_audio_stale([s for s in spans if s[0] == AUDIO_STALE],
                                           all_wait_records, names, audio_window, audio_source)
    result["trace_cost"] = {
        "dropped": dropped, "jq2_images": result["jq2_images"], "jq2_image_overflow": overflow,
        "image_ms": [{"q": r["q"], "image_ms": r["image_ms"], "job_ms": r["job_ms"],
                      "share": (r["image_ms"] / r["job_ms"]) if r["job_ms"] > 0 else None}
                     for r in trace_cost_rows]}
    return result


def _jq2_ms(x):
    return "n/a" if x is None else f"{x:.2f}"


def _jq2_pct(x):
    return "n/a" if x is None else f"{100 * x:.0f} %"


def _jq2_ratio(x):
    return "n/a" if x is None else f"{x:.2f}"


def print_jq2_critical_path(cp):
    print("  critical path of blocking waits (event-flag waits; fiber waits excluded):")
    if not cp["rows"]:
        print("    none recorded")
        return
    for row in cp["rows"]:
        print(f"    q{row['q']} tid {row['tid']:3d} {row['name'][:20]:20s}: {row['waits']} wait(s),"
              f" {row['fiber_waits']} fiber excluded, {row['waits_without_credit']} without a"
              f" terminal credit, {row['total_ms']:.2f} ms")
        print("      partition: " + ", ".join(
            f"{JQ2_CP_LABELS[cat]} {row['partition_ms'][cat]:.2f} ms"
            f" ({_jq2_pct(row['partition_share'][cat])})" for cat in JQ2_CP_CATEGORIES))
        print(f"      mean chain {_jq2_ms(row['mean_chain'])} jobs, {row['guard_hits']} guard hits")


def print_jq2_images(images):
    print("  job images by fingerprint (CPU is the job's thread CPU time and includes"
          " identification):")

    def table(title, rows):
        print(f"    {title}:")
        if not rows:
            print("      none recorded")
            return
        for r in rows:
            ex = r["exec_wall_ms"]
            print(f"      {r['fingerprint'] or 'unknown':16s}  jobs {r['jobs']:4d}"
                  f"  exec {ex['total_ms']:8.2f} ms (mean {_jq2_ms(ex['mean_ms'])},"
                  f" p50 {_jq2_ms(ex['p50_ms'])}, p95 {_jq2_ms(ex['p95_ms'])})"
                  f"  CPU {r['cpu_ms']:8.2f} ms")
            print(f"        latency mean {_jq2_ms(r['queue_latency_mean_ms'])} ms,"
                  f" held mean {_jq2_ms(r['held_mean_ms'])} ms,"
                  f" ready mean {_jq2_ms(r['ready_mean_ms'])} ms")
            print(f"        critical path {r['critical_path_ms']:.2f} ms"
                  f" = {_jq2_pct(r['critical_path_share'])} of the blocking wait"
                  f"  CP rank {r['cp_rank']}, CPU rank {r['cpu_rank']}")
            variants = ", ".join(f"ea 0x{v['ea']:08x} / {v['bytes']} B" for v in r["variants"])
            print(f"        image {variants or 'unknown'}; queues"
                  f" {', '.join(str(q) for q in r['queues']) or 'none'}")
    for qd in images["per_queue"]:
        table(f"q{qd['q']}", qd["rows"])
    table("overall", images["overall"])


def print_jq2_audio_stale(aud):
    w = aud["window"]
    where = ("window n/a" if w["ms"] is None else
             f"window {w['start_ms']:.2f}-{w['end_ms']:.2f} ms ({w['source']})")
    print(f"  audio stale attribution ({where}; block = 256 samples at 48 kHz ="
          f" {AUDIO_BLOCK_MS:.2f} ms; b = the block's position in its run):")
    print(f"    {aud['count']} stale block(s): {aud['blocks_in_wait']} in a wait,"
          f" {aud['blocks_not_in_wait']} not")
    if not aud["count"]:
        return
    print(f"    runs: {aud['runs']}, {aud['blocks']} block(s),"
          f" {aud['runs_ge_2_blocks']} run(s) of 2+ blocks (10.67 ms or more),"
          f" {aud['open_at_start']} open at the start")
    for h in aud["histogram"]:
        print(f"    run length {h['band']:3s}: {h['runs']:4d} run(s), {h['blocks']:6d} block(s),"
              f" {h['ms']:9.2f} ms, mean {_jq2_ms(h['mean_ms'])} ms")
    for p in aud["per_port"]:
        print(f"    port {p['port']}: {p['runs']} run(s), {p['blocks']} block(s),"
              f" {p['runs_ge_2_blocks']} of 2+ blocks, {p['open_at_start']} open at the start;"
              " by length " + ", ".join(f"{h['band']} {h['runs']}/{h['blocks']}"
                                        for h in p["histogram"]))
    for t in aud["threads"]:
        print(f"    tid {t['tid']:3d} {t['name'][:20]:20s}: {t['blocks_in_wait']} block(s) in a"
              f" wait, {t['blocks_not_in_wait']} not; expected by chance"
              f" {_jq2_ms(t['expected_blocks_in_wait'])} (wait share {_jq2_pct(t['wait_share'])})"
              f", ratio {_jq2_ratio(t['in_wait_ratio'])}")
    for f in aud["fingerprints"]:
        print(f"    fp {f['fingerprint'] or 'unknown'}: {f['terminal_blocks']} block(s) as terminal"
              f" job, {f['dominant_blocks']} as dominant image; expected by chance"
              f" {_jq2_ms(f['terminal_expected_blocks'])} terminal,"
              f" ratio {_jq2_ratio(f['terminal_ratio'])}")
    print(f"    first {len(aud['events'])} block(s):")
    for e in aud["events"]:
        hits = "; ".join(
            f"tid {h['tid']} q{h['q']} seq {h['seq']}"
            f" terminal {h['terminal']['fingerprint'] if h['terminal'] else 'none'}"
            f" dominant {h['dominant']['fingerprint'] if h['dominant'] else 'none'}"
            f" waited {h['waited_ms']:.2f} ms" for h in e["waits"])
        print(f"      {e['at_ms']:9.2f} ms port {e['port']} block {e['block']}"
              f" run pos {e['run_pos']} of {e['run_len']} ({e['run_ms']:.2f} ms, {e['band']}):"
              f" {hits or 'no thread in a wait'}")


def print_jq2_trace_cost(tc):
    print("  image identification cost (IMAGE spans, as a share of the queue's JOB time):")
    if not tc["image_ms"]:
        print("    none recorded")
        return
    for r in tc["image_ms"]:
        print(f"    q{r['q']}: {r['image_ms']:.2f} ms of {r['job_ms']:.2f} ms job time"
              f" = {_jq2_pct(r['share'])}")


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
    if jq.get("images_available", False):
        entries = jq["jq2_images"]
        overflow = jq["jq2_image_overflow"]
        print(f"  image table: {'n/a' if entries is None else entries} entries,"
              f" overflow {'not recorded' if overflow is None else overflow}")
        if jq["incomplete"]:
            lost = []
            if dropped:
                lost.append(f"{dropped} span(s) dropped")
            if overflow:
                lost.append(f"{overflow} image table slot(s) lost")
            print("  INCOMPLETE: " + ", ".join(lost))
    for qd in jq["queues"]:
        w = qd["window"]
        rate = (f"{qd['push_rate_per_s']:.2f} pushes/s" if qd["push_rate_per_s"] is not None
                else "rate n/a")
        print(f"  queue {qd['q']}: {qd['pushes']} pushes, {qd['jobs']} jobs, {qd['workers']} workers,"
              f" window {w['start_ms']:.2f}-{w['end_ms']:.2f} ms ({w['ms']:.2f} ms,"
              f" {qd['window_source']}), {rate}")
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
              f" {sl['wait_without_credit']} wait without a credit,"
              f" {sl['credit_without_wait']} due credit without a wait)")
        dec = qd["latency_decomposition"]
        if dec is None:
            continue
        for row in qd["park_reasons"]:
            print(f"    park reason {row['reason']:2d} {row['name']:12s} n {row['count']:5d}"
                  f"  {row['total_ms']:8.2f} ms = {pct(row['share'])} of idle"
                  f"; pending > 0: n {row['pending_count']}, {row['pending_ms']:.2f} ms"
                  f" = {pct(row['pending_share'])} of idle")
        stat("held (visible->admissible)", dec["held_ms"])
        stat("ready wait (admissible->job)", dec["ready_wait_ms"])
        print(f"    held vs ready share: {pct(dec['held_share'])} / {pct(dec['ready_share'])}"
              f"  ({dec['ready_wait_clamped']} ready waits clamped at 0)")
        rs = dec["ready_wait_split"]
        print(f"    ready wait split: {rs['parked_ms']:.2f} ms with a worker parked"
              f" = {pct(rs['parked_share'])}; {rs['all_busy_ms']:.2f} ms with every worker busy"
              f" = {pct(rs['all_busy_share'])}; {rs['transition_ms']:.2f} ms in transition"
              f" = {pct(rs['transition_share'])}")
        ct = qd["counts_over_time"]
        print(f"    jobs over time: held mean {ms(ct['held']['mean'])}, max {ct['held']['max']};"
              f" admissible mean {ms(ct['ready']['mean'])}, max {ct['ready']['max']}")
        for b in qd["depth_buckets"]:
            print(f"    depth {b['bucket']:5s}: {b['jobs']:5d} jobs, {b['held_jobs']:5d} with a HELD span"
                  f" ({b['held_by_barrier']} held by a barrier), mean held {ms(b['mean_held_ms'])} ms,"
                  f" mean ready wait {ms(b['mean_ready_wait_ms'])} ms")
        bd = qd["barriers"]
        print(f"    barriers: {bd['count']} = {ms(bd['rate_per_s'])}/s, mean duration"
              f" {ms(bd['duration_ms']['mean'])} ms, mean waited {ms(bd['waited_mean'])}")
        for m in bd["top_masks"]:
            print(f"      mask 0x{m['mask']:04X}: {m['jobs']} jobs held, {m['total_held_ms']:.2f} ms held,"
                  f" mean {ms(m['mean_held_ms'])} ms")
        jf = dec["join_failures"]
        print(f"    held joins: {jf['held_without_job']} HELD without a job,"
              f" {jf['job_without_held']} job without a HELD")
    if not jq.get("decomposition_available", False):
        print("  held/admissible decomposition needs kinds 27-28 (HELD, BARRIER); this trace has none")
    if not jq.get("images_available", False):
        print("  no image identity in this trace (kind 29)")
        return
    print_jq2_critical_path(jq["critical_path"])
    print_jq2_images(jq["images"])
    print_jq2_audio_stale(jq["audio_stale"])
    print_jq2_trace_cost(jq["trace_cost"])


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
        header, names, spans, images = load(args.trace)
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
    report["jq2"] = jq2_section(spans, header, names, images)
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
