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
  --json              the summary (frames, breakdowns, cadence) as JSON on
                      stdout instead of text; no timelines.

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
import statistics
import sys

(SYSCALL, USLEEP, SPU_WAIT, SPU_BUSY, SEMA, SEMA_GAVEUP, DRAIN, FLIP_FIFO, FLIP_REQ, PRESENT,
 GPU_ENCODE, GPU_WAIT, READBACK, GPU_EXEC, HLE) = range(1, 16)
GPU_WAIT_KIND = {0: "gpu wait (flush)", 1: "gpu wait (present)", 2: "gpu wait (in-flight)"}

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
