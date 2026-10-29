#!/usr/bin/env python3
"""Report on an Instruments Time Profiler export (`xctrace export` time-profile table).

  tools/xctrace_profile.py <time-profile.xml> [--from S] [--to S] [--thread TID[,..]]
      [--role R[,..]] [--top N] [--by self|inclusive|line] [--roles] [--timeline SEC]
      [--guest] [--gaps MS] [--runs MS] [--callers NAME] [--caller-depth N] [--json]

Input : the XML written by
          xcrun xctrace export --input run.trace \\
              --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' \\
              --output tp.xml
Output: a text report on stdout (default) or one JSON document (--json).
Exit  : 0 ok, 2 usage error or unreadable/malformed input (message on stderr).

What a sample is. Time Profiler stops the process every ~1 ms and records the
call stack of every thread that is RUNNING on a core. A thread that sleeps,
waits on a lock or is not scheduled produces no sample, so a gap between two
samples of a thread means "not running", not "sampling missed it". A sample is
1 ms of CPU (its weight). Stacks are leaf first. An inlined frame is a frame
with inlined="true": it is a distinct function in the stack but not a distinct
call. Times are seconds from the first sample of the whole file, so --from/--to
mean the same instants whatever the other filters are. The window is half open:
a sample at exactly --from is in, one at exactly --to is out.

Selection (applies to every section): --from/--to window, --thread (tids, hex
0x... or decimal), --role (names below; "spu-worker" selects busy and idle).
With no section flag, or with --by, the report has a hotspot table (default --by self).

Sections (any combination):
  (default) hotspot table of the selection, --by:
              self       leaf frame's function ("self time")
              inclusive  every function on the stack, counted once per sample
                         (recursion and inlined duplicates count once)
              line       leaf frame's source file:line (the function name and
                         binary when the frame has no source info)
  --roles          CPU per role (samples, ms, share of the window as cores),
                   P-core / E-core samples per role (from the core column's
                   "P Core"/"E Core"), and the busiest threads with their role.
  --timeline SEC   samples per role in SEC-second buckets from the window
                   start, with per-role median, min, max and the buckets that
                   fall below 50% or above 150% of the median.
  --guest          lifted guest PPU functions: samples that have a frame named
                   func_XXXXXXXX (XXXXXXXX = the guest address, hex) are
                   aggregated inclusive / self / own per guest address, then the
                   host leaf functions under guest code with the guest functions
                   that call them. self = the innermost guest frame (host
                   helpers it calls count to it), own = the leaf frame is the
                   guest function itself.
  --gaps MS        gaps longer than MS between consecutive samples of the
                   selection taken as one stream, with the samples of every
                   role that ran during each gap, plus per-thread gap totals
                   and the 1 s buckets without a sample. Select the thread or
                   role you care about first; on a thread that sleeps most of
                   the time long gaps are normal.
  --runs MS        bursts of CPU per thread: consecutive samples of one thread
                   closer than MS belong to one run (a run's length is last
                   sample - first sample + one weight). Per role: runs, runs per
                   second, median / p90 / p99 / max length in ms, the runs of
                   at least 5, 10 and 20 ms, plus the
                   longest runs. Use 2.5 for the 1 ms sampling tick; a run is
                   then a stretch in which the thread never left the CPU, e.g. a
                   job or a frame's worth of work.
  --callers NAME   samples whose leaf function contains NAME, grouped by the
                   calling frames (--caller-depth, default 1). When the leaf is
                   in a system library (/usr/lib, /System, /Library) the frames
                   of that same library are skipped, so the caller is the first
                   frame outside it, with its source file:line (a non-leaf
                   frame's line is the call site).

Roles (a sample's role comes from its thread: the thread gets the most common
signature among its samples, "other" only if every sample is "other"; the first
matching signature of a sample, in this order, is):
  audio-mix        a frame audio_mix_thread_func (the cellAudio mix thread)
  rsx              a frame whose name contains rsx_process_fifo
  spu-worker-*     a frame worker_loop (SPURS jq2 worker); the sample is
                   spu-worker-busy when it also has a frame run_job,
                   run_spurs_job or spu_run_*, else spu-worker-idle
  ppu-guest        a frame named func_XXXXXXXX (lifted PPU function)
  main/ui          a frame named main or _NSEventThread
  other            everything else (system audio, dispatch workers, ...)
"""
import argparse
import bisect
import collections
import json
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xctrace_xml  # noqa: E402

SCHEMA = "ps3recomp.xctrace-profile/1"
ROLES = ("spu-worker-busy", "spu-worker-idle", "rsx", "ppu-guest", "audio-mix", "main/ui", "other")
SPU_WORKER = "spu-worker"

F_AUDIO, F_RSX, F_WORKER, F_BUSY, F_GUEST, F_MAIN = 1, 2, 4, 8, 16, 32
GUEST_RE = re.compile(r"^func_([0-9A-Fa-f]{8})\b")
SYSTEM_PREFIXES = ("/usr/lib/", "/System/", "/Library/")
CORE_RE = re.compile(r"\((P|E) Core\)")
SORT_ROLE = {r: i for i, r in enumerate(ROLES)}


class BadInput(Exception):
    pass


def frame_flags(name):
    f = 0
    if name == "audio_mix_thread_func":
        f |= F_AUDIO
    if "rsx_process_fifo" in name:
        f |= F_RSX
    if name == "worker_loop" or "::Queue::worker_" in name:
        f |= F_WORKER
    if name == "run_job" or name.startswith("run_spurs_job") or name.startswith("spu_run_"):
        f |= F_BUSY
    if GUEST_RE.match(name):
        f |= F_GUEST
    if name == "main" or name == "_NSEventThread":
        f |= F_MAIN
    return f


class Frame:
    __slots__ = ("name", "binary", "bpath", "file", "line", "flags", "guest")

    def __init__(self, node):
        a = node.attrib
        self.name = a.get("name") or a.get("addr") or "<unknown>"
        b = node.find("binary")
        self.binary = b.attrib.get("name", "") if b is not None else ""
        self.bpath = b.attrib.get("path", "") if b is not None else ""
        s = node.find("source")
        self.file = None
        self.line = None
        if s is not None and "line" in s.attrib:
            try:
                self.line = int(s.attrib["line"])
            except ValueError:
                raise BadInput("frame %s has a non-numeric source line %r" % (self.name, s.attrib["line"]))
            p = s.find("path")
            self.file = p.text if p is not None else None
        self.flags = frame_flags(self.name)
        m = GUEST_RE.match(self.name)
        self.guest = int(m.group(1), 16) if m else None

    @property
    def site(self):
        """file:line of the frame, or None."""
        if self.file is not None and self.line is not None:
            return "%s:%d" % (self.file, self.line)
        return None


class Profile:
    """Time-sorted samples. recs[i] = (t_ns from first sample, tid, core, weight_ns, stack, role)."""

    def __init__(self):
        self.recs = []
        self.times = []
        self.frames = []
        self.tid_label = {}


def _need(row, col):
    n = row.get(col)
    if n is None:
        raise BadInput("row has no %s column (is this a time-profile table?)" % col)
    return n


def _rows(path):
    # Hand the reader an open file: it closes it deterministically, also on errors.
    with open(path, "rb") as fh:
        yield from xctrace_xml.iter_rows(fh, "time-profile")


def load(path):
    prof = Profile()
    cache = {}
    index = {}
    raw = []
    try:
        for schema, row in _rows(path):
            t = _need(row, "time").int()
            thread = _need(row, "thread")
            tidn = thread.find("tid")
            if tidn is None:
                raise BadInput("thread value has no tid")
            tid = tidn.int()
            prof.tid_label[tid] = tidn.fmt
            m = CORE_RE.search(_need(row, "core").fmt or "")
            core = m.group(1) if m else "?"
            w = _need(row, "weight").int()
            stack = []
            for fr in _need(row, "stack").children:
                if fr.tag != "frame":
                    continue
                hit = cache.get(id(fr))
                if hit is None:
                    info = Frame(fr)
                    key = (info.name, info.binary, info.file, info.line)
                    fid = index.get(key)
                    if fid is None:
                        fid = index[key] = len(prof.frames)
                        prof.frames.append(info)
                    hit = cache[id(fr)] = (fr, fid)   # keep fr alive so its id() is not reused
                stack.append(hit[1])
            raw.append((t, tid, core, w, tuple(stack)))
    except xctrace_xml.MalformedExport as e:
        raise BadInput(str(e))
    except xctrace_xml.ET.ParseError as e:
        raise BadInput("not well-formed XML: %s" % e)
    except OSError as e:
        raise BadInput("cannot read %s: %s" % (path, e.strerror or e))
    except ValueError as e:
        raise BadInput("bad value: %s" % e)
    if not raw:
        raise BadInput("no time-profile rows in %s" % path)
    raw.sort(key=lambda r: r[0])
    t0 = raw[0][0]
    frames = prof.frames

    sigs = []
    per_thread = collections.defaultdict(collections.Counter)
    for t, tid, core, w, stack in raw:
        m = 0
        for fid in stack:
            m |= frames[fid].flags
        if m & F_AUDIO:
            sig = "audio-mix"
        elif m & F_RSX:
            sig = "rsx"
        elif m & F_WORKER:
            sig = SPU_WORKER
        elif m & F_GUEST:
            sig = "ppu-guest"
        elif m & F_MAIN:
            sig = "main/ui"
        else:
            sig = "other"
        sigs.append((sig, bool(m & F_BUSY)))
        per_thread[tid][sig] += 1
    thread_role = {}
    order = (SPU_WORKER, "rsx", "ppu-guest", "audio-mix", "main/ui")
    for tid, c in per_thread.items():
        best = None
        for s in order:
            if c.get(s, 0) and (best is None or c[s] > c[best]):
                best = s
        thread_role[tid] = best or "other"
    prof.thread_role = thread_role
    for (t, tid, core, w, stack), (sig, busy) in zip(raw, sigs):
        role = thread_role[tid]
        if role == SPU_WORKER:
            role = "spu-worker-busy" if busy else "spu-worker-idle"
        prof.recs.append((t - t0, tid, core, w, stack, role))
    prof.times = [r[0] for r in prof.recs]
    return prof


# --------------------------------------------------------------------------- selection

def parse_threads(text):
    out = set()
    for tok in text.split(","):
        tok = tok.strip()
        if not tok:
            continue
        try:
            out.add(int(tok, 0))
        except ValueError:
            raise BadInput("--thread: %r is not a thread id (hex 0x.. or decimal)" % tok)
    return out


def parse_roles(text):
    out = set()
    for tok in text.split(","):
        tok = tok.strip()
        if not tok:
            continue
        if tok == SPU_WORKER:
            out.update(("spu-worker-busy", "spu-worker-idle"))
        elif tok in ROLES:
            out.add(tok)
        else:
            raise BadInput("--role: unknown role %r (one of %s, %s)" % (tok, ", ".join(ROLES), SPU_WORKER))
    return out


class View:
    """The window (all roles) and the selection (window + thread/role filters)."""

    def __init__(self, prof, args):
        self.prof = prof
        last = prof.times[-1]
        self.start = 0 if args.from_s is None else int(round(args.from_s * 1e9))
        self.has_to = args.to_s is not None
        self.end = int(round(args.to_s * 1e9)) if self.has_to else max(last + 1, self.start + 1)
        if self.end <= self.start:
            raise BadInput("the window is empty: --to %s is not after --from %s" % (args.to_s, args.from_s or 0))
        lo = bisect.bisect_left(prof.times, self.start)
        hi = bisect.bisect_left(prof.times, self.end)
        self.window = prof.recs[lo:hi]
        threads = args.threads
        roles = args.roles_filter
        self.sel = [r for r in self.window
                    if (threads is None or r[1] in threads) and (roles is None or r[5] in roles)]
        self.span_ns = self.end - self.start

    def tid(self, tid):
        return self.prof.tid_label.get(tid, str(tid))


def sec(ns):
    return round(ns / 1e9, 3)


def trunc(s, n=100):
    return s if len(s) <= n else s[:n - 3] + "..."


def pct(a, b):
    return round(100.0 * a / b, 2) if b else 0.0


def ranked(counter, top):
    return sorted(counter.items(), key=lambda kv: (-kv[1], str(kv[0])))[:top]


def fmt_table(headers, rows, left=(0,)):
    rows = [[str(c) for c in r] for r in rows]
    widths = [len(h) for h in headers]
    for r in rows:
        for i, c in enumerate(r):
            widths[i] = max(widths[i], len(c))

    def line(r):
        return "  " + "  ".join(c.ljust(widths[i]) if i in left else c.rjust(widths[i])
                                for i, c in enumerate(r)).rstrip()
    return [line(headers)] + [line(r) for r in rows]


# --------------------------------------------------------------------------- sections

def build_summary(v):
    sel = v.sel
    ms = sum(r[3] for r in sel) / 1e6
    return {
        "window_s": [sec(v.start), sec(v.end) if v.has_to else sec(v.prof.times[-1])],
        "samples_in_window": len(v.window),
        "samples_selected": len(sel),
        "cpu_ms_selected": round(ms, 3),
        "threads_selected": len({r[1] for r in sel}),
        "span_s": sec(v.span_ns),
        "avg_cores_selected": round(ms * 1e6 / v.span_ns, 3),
    }


def build_hotspots(v, by, top):
    frames = v.prof.frames
    c = collections.Counter()
    for r in v.sel:
        stack = r[4]
        if not stack:
            c["<no stack>"] += 1
            continue
        if by == "self":
            f = frames[stack[0]]
            c[(f.name, f.binary)] += 1
        elif by == "line":
            f = frames[stack[0]]
            c[f.site or "%s [%s] (no source)" % (f.name, f.binary)] += 1
        else:
            seen = set()
            for fid in stack:
                f = frames[fid]
                seen.add((f.name, f.binary))
            for k in seen:
                c[k] += 1
    total = len(v.sel)
    rows = []
    for k, n in ranked(c, top):
        if isinstance(k, tuple):
            rows.append({"name": k[0], "binary": k[1], "samples": n, "pct": pct(n, total)})
        else:
            rows.append({"site": k, "samples": n, "pct": pct(n, total)})
    return {"by": by, "total_samples": total, "rows": rows}


def render_hotspots(d):
    out = ["", "== hotspots by %s (of %d selected samples) ==" % (d["by"], d["total_samples"])]
    rows = []
    for r in d["rows"]:
        label = r["site"] if "site" in r else trunc(r["name"]) + (" [%s]" % r["binary"] if r["binary"] else "")
        rows.append([r["samples"], "%.2f%%" % r["pct"], label])
    out += fmt_table(["samples", "share", d["by"] if d["by"] == "line" else "function"], rows, left=(2,))
    return out


def build_roles(v, top):
    window_ms = v.span_ns / 1e6
    per_role = {}
    per_thread = {}
    for r in v.sel:
        _, tid, core, w, stack, role = r
        e = per_role.setdefault(role, {"threads": set(), "samples": 0, "ms": 0.0, "P": 0, "E": 0, "unknown": 0})
        e["threads"].add(tid)
        e["samples"] += 1
        e["ms"] += w / 1e6
        e["P" if core == "P" else "E" if core == "E" else "unknown"] += 1
        t = per_thread.setdefault(tid, {"samples": 0, "P": 0, "E": 0, "unknown": 0,
                                        "roles": collections.Counter(), "self": collections.Counter()})
        t["samples"] += 1
        t["P" if core == "P" else "E" if core == "E" else "unknown"] += 1
        t["roles"][role] += 1
        if stack:
            f = v.prof.frames[stack[0]]
            t["self"][f.name] += 1
    roles = []
    for role in sorted(per_role, key=lambda x: SORT_ROLE[x]):
        e = per_role[role]
        roles.append({"role": role, "threads": len(e["threads"]), "samples": e["samples"],
                      "ms": round(e["ms"], 3), "cores": round(e["ms"] / window_ms, 3),
                      "p_core_samples": e["P"], "e_core_samples": e["E"], "unknown_core_samples": e["unknown"],
                      "e_core_pct": pct(e["E"], e["samples"])})
    threads = []
    for tid, t in sorted(per_thread.items(), key=lambda kv: (-kv[1]["samples"], kv[0]))[:top]:
        role = sorted(t["roles"].items(), key=lambda kv: (-kv[1], SORT_ROLE[kv[0]]))[0][0]
        name, n = sorted(t["self"].items(), key=lambda kv: (-kv[1], kv[0]))[0] if t["self"] else ("", 0)
        threads.append({"tid": v.tid(tid), "tid_dec": tid, "role": role, "samples": t["samples"],
                        "cores": round(t["samples"] / window_ms, 3),
                        "p_core_samples": t["P"], "e_core_samples": t["E"],
                        "top_self": name, "top_self_samples": n})
    return {"window_ms": round(window_ms, 3), "roles": roles, "threads": threads}


def render_roles(d):
    out = ["", "== roles (window %.0f ms; cores = ms / window) ==" % d["window_ms"]]
    out += fmt_table(["role", "threads", "samples", "ms", "cores", "P-core", "E-core", "E%"],
                     [[r["role"], r["threads"], r["samples"], "%.0f" % r["ms"], "%.3f" % r["cores"],
                       r["p_core_samples"], r["e_core_samples"], "%.1f" % r["e_core_pct"]]
                      for r in d["roles"]])
    out += ["", "== busiest threads =="]
    out += fmt_table(["tid", "role", "samples", "cores", "P-core", "E-core", "top self function"],
                     [[t["tid"], t["role"], t["samples"], "%.3f" % t["cores"], t["p_core_samples"],
                       t["e_core_samples"], "%s (%d)" % (trunc(t["top_self"], 60), t["top_self_samples"])]
                      for t in d["threads"]], left=(0, 1, 6))
    return out


def build_timeline(v, bucket_s):
    if bucket_s <= 0:
        raise BadInput("--timeline: bucket must be > 0 seconds")
    bucket = int(round(bucket_s * 1e9))
    n = max(1, -(-v.span_ns // bucket))
    counts = {}
    for r in v.sel:
        row = counts.setdefault(r[5], [0] * n)
        row[(r[0] - v.start) // bucket] += 1
    roles = sorted(counts, key=lambda x: SORT_ROLE[x])
    buckets = []
    for i in range(n):
        vals = {role: counts[role][i] for role in roles}
        buckets.append({"t_s": sec(v.start + i * bucket), "total": sum(vals.values()), "samples": vals})
    stats = []
    for role in roles:
        vals = counts[role]
        srt = sorted(vals)
        med = srt[len(srt) // 2] if len(srt) % 2 else (srt[len(srt) // 2 - 1] + srt[len(srt) // 2]) / 2
        lo = min(vals)
        hi = max(vals)
        low = [sec(v.start + i * bucket) for i, x in enumerate(vals) if med > 0 and x < 0.5 * med]
        high = [sec(v.start + i * bucket) for i, x in enumerate(vals) if med > 0 and x > 1.5 * med]
        stats.append({"role": role, "median": med, "min": lo, "min_at_s": sec(v.start + vals.index(lo) * bucket),
                      "max": hi, "max_at_s": sec(v.start + vals.index(hi) * bucket),
                      "zero_buckets": sum(1 for x in vals if x == 0),
                      "below_half_median_at_s": low, "above_1_5x_median_at_s": high})
    return {"bucket_s": bucket_s, "buckets": buckets, "stats": stats}


def render_timeline(d):
    roles = [s["role"] for s in d["stats"]]
    out = ["", "== timeline: samples per %gs bucket ==" % d["bucket_s"]]
    out += fmt_table(["t(s)", "total"] + roles,
                     [[b["t_s"], b["total"]] + [b["samples"][r] for r in roles] for b in d["buckets"]],
                     left=())
    out += ["", "== timeline statistics =="]
    out += fmt_table(["role", "median", "min", "@s", "max", "@s", "zero", "<50%", ">150%"],
                     [[s["role"], s["median"], s["min"], s["min_at_s"], s["max"], s["max_at_s"],
                       s["zero_buckets"], len(s["below_half_median_at_s"]), len(s["above_1_5x_median_at_s"])]
                      for s in d["stats"]])
    for s in d["stats"]:
        if s["below_half_median_at_s"]:
            out.append("  %s below 50%% of median at t(s): %s" % (s["role"], ", ".join("%g" % x for x in s["below_half_median_at_s"][:30])
                                                                    + (" ..." if len(s["below_half_median_at_s"]) > 30 else "")))
        if s["above_1_5x_median_at_s"]:
            out.append("  %s above 150%% of median at t(s): %s" % (s["role"], ", ".join("%g" % x for x in s["above_1_5x_median_at_s"][:30])
                                                                     + (" ..." if len(s["above_1_5x_median_at_s"]) > 30 else "")))
    return out


def guest_label(addr):
    return "0x%08X" % addr


def build_guest(v, top):
    frames = v.prof.frames
    incl = collections.Counter()
    self_ = collections.Counter()
    own = collections.Counter()
    helper_leaf = collections.Counter()
    helper_callers = collections.defaultdict(collections.Counter)
    callee = collections.Counter()
    callee_callers = collections.defaultdict(collections.Counter)
    total = 0
    names = {}
    for r in v.sel:
        stack = r[4]
        inner = None
        inner_pos = -1
        seen = set()
        for pos, fid in enumerate(stack):
            g = frames[fid].guest
            if g is None:
                continue
            names[g] = frames[fid].name
            seen.add(g)
            if inner is None:
                inner, inner_pos = g, pos
        if inner is None:
            continue
        total += 1
        for g in seen:
            incl[g] += 1
        self_[inner] += 1
        if inner_pos == 0:
            own[inner] += 1
        else:
            leaf = frames[stack[0]]
            helper_leaf[leaf.name] += 1
            helper_callers[leaf.name][inner] += 1
            direct = frames[stack[inner_pos - 1]]
            callee[direct.name] += 1
            callee_callers[direct.name][inner] += 1

    def fn_rows(counter):
        return [{"guest": guest_label(g), "name": names[g], "samples": n, "pct": pct(n, total),
                 "own": own.get(g, 0), "self": self_.get(g, 0), "inclusive": incl.get(g, 0)}
                for g, n in ranked(counter, top)]

    def helper_rows(counter, callers):
        rows = []
        for name, n in ranked(counter, top):
            rows.append({"name": name, "samples": n, "pct": pct(n, total),
                         "guest_callers": [{"guest": guest_label(g), "samples": k}
                                           for g, k in ranked(callers[name], 5)]})
        return rows

    return {"guest_samples": total,
            "inclusive": fn_rows(incl), "self": fn_rows(self_),
            "host_leaf": helper_rows(helper_leaf, helper_callers),
            "host_callee": helper_rows(callee, callee_callers)}


def render_guest(d):
    out = ["", "== guest PPU functions (%d samples with a func_XXXXXXXX frame) ==" % d["guest_samples"]]
    for key, title in (("inclusive", "top by inclusive"), ("self", "top by self (innermost guest frame)")):
        out += ["", "-- %s --" % title]
        out += fmt_table(["guest", "incl", "incl%", "self", "own"],
                         [[r["guest"], r["inclusive"], "%.2f%%" % pct(r["inclusive"], d["guest_samples"]),
                           r["self"], r["own"]] for r in d[key]], left=(0,))
    for key, title in (("host_leaf", "host leaf functions under guest code"),
                       ("host_callee", "host function called directly by the innermost guest frame")):
        out += ["", "-- %s --" % title]
        out += fmt_table(["samples", "share", "function", "innermost guest callers"],
                         [[r["samples"], "%.2f%%" % r["pct"], trunc(r["name"], 60),
                           " ".join("%s:%d" % (c["guest"], c["samples"]) for c in r["guest_callers"])]
                          for r in d[key]], left=(2, 3))
    return out


def build_gaps(v, gap_ms, top):
    if gap_ms <= 0:
        raise BadInput("--gaps: threshold must be > 0 ms")
    thr = int(round(gap_ms * 1e6))
    sel = v.sel
    times = [r[0] for r in sel]
    gaps = []
    for i in range(1, len(times)):
        if times[i] - times[i - 1] > thr:
            gaps.append((times[i] - times[i - 1], times[i - 1], times[i]))
    gaps_sorted = sorted(gaps, key=lambda g: (-g[0], g[1]))
    wtimes = [r[0] for r in v.window]
    listed = []
    for dt, a, b in gaps_sorted[:top]:
        lo = bisect.bisect_right(wtimes, a)
        hi = bisect.bisect_left(wtimes, b)
        c = collections.Counter(r[5] for r in v.window[lo:hi])
        listed.append({"from_s": sec(a), "to_s": sec(b), "gap_ms": round(dt / 1e6, 3),
                       "samples_by_role": {k: c[k] for k in sorted(c, key=lambda x: SORT_ROLE[x])}})
    per_thread = {}
    by_tid = collections.defaultdict(list)
    for r in sel:
        by_tid[r[1]].append(r[0])
    for tid, ts in by_tid.items():
        gs = [b - a for a, b in zip(ts, ts[1:]) if b - a > thr]
        per_thread[tid] = {"tid": v.tid(tid), "samples": len(ts), "gaps": len(gs),
                           "gap_ms_total": round(sum(gs) / 1e6, 3),
                           "longest_gap_ms": round(max(gs) / 1e6, 3) if gs else 0.0}
    nsec = max(1, -(-v.span_ns // 1_000_000_000))
    occupied = set((t - v.start) // 1_000_000_000 for t in times)
    empty = [sec(v.start + i * 1_000_000_000) for i in range(nsec) if i not in occupied]
    return {"threshold_ms": gap_ms, "selection_samples": len(sel), "gaps": len(gaps),
            "gap_ms_total": round(sum(g[0] for g in gaps) / 1e6, 3),
            "window_ms": round(v.span_ns / 1e6, 3),
            "longest": listed,
            "threads": [per_thread[t] for t in sorted(per_thread, key=lambda t: (-per_thread[t]["gaps"], t))][:top],
            "one_second_buckets": nsec, "one_second_buckets_without_sample": len(empty),
            "empty_bucket_starts_s": empty[:50]}


def render_gaps(d):
    out = ["", "== gaps > %g ms between consecutive samples of the selection ==" % d["threshold_ms"],
           "  %d gaps, %.0f ms of %.0f ms window; %d of %d one-second buckets have no sample"
           % (d["gaps"], d["gap_ms_total"], d["window_ms"], d["one_second_buckets_without_sample"],
              d["one_second_buckets"])]
    if d["empty_bucket_starts_s"]:
        out.append("  empty buckets start at (s): %s%s" % (", ".join("%g" % x for x in d["empty_bucket_starts_s"][:20]),
                                                          " ..." if len(d["empty_bucket_starts_s"]) > 20 else ""))
    out += ["", "-- longest gaps, samples of every role inside the gap --"]
    out += fmt_table(["from(s)", "to(s)", "gap ms", "samples inside the gap by role"],
                     [[g["from_s"], g["to_s"], "%.1f" % g["gap_ms"],
                       " ".join("%s=%d" % kv for kv in g["samples_by_role"].items()) or "none"]
                      for g in d["longest"]], left=(3,))
    out += ["", "-- per thread --"]
    out += fmt_table(["tid", "samples", "gaps", "gap ms total", "longest ms"],
                     [[t["tid"], t["samples"], t["gaps"], "%.0f" % t["gap_ms_total"], "%.1f" % t["longest_gap_ms"]]
                      for t in d["threads"]], left=(0,))
    return out


def build_runs(v, gap_ms, top):
    if gap_ms <= 0:
        raise BadInput("--runs: threshold must be > 0 ms")
    thr = int(round(gap_ms * 1e6))
    by_tid = collections.defaultdict(list)
    for r in v.sel:
        by_tid[r[1]].append(r)
    runs = []         # (length_ns, start_ns, tid, role)
    for tid in sorted(by_tid):
        rs = by_tid[tid]
        first = prev = rs[0]
        for r in rs[1:]:
            if r[0] - prev[0] > thr:
                runs.append((prev[0] - first[0] + prev[3], first[0], tid, first[5]))
                first = r
            prev = r
        runs.append((prev[0] - first[0] + prev[3], first[0], tid, first[5]))
    per_role = collections.defaultdict(list)
    for length, start, tid, role in runs:
        per_role[role].append(length)
    window_s = v.span_ns / 1e9

    def q(vals, p):
        return vals[min(len(vals) - 1, int(p * len(vals)))]
    roles = []
    for role in sorted(per_role, key=lambda x: SORT_ROLE[x]):
        vals = sorted(per_role[role])
        roles.append({"role": role, "runs": len(vals), "runs_per_s": round(len(vals) / window_s, 3),
                      "total_ms": round(sum(vals) / 1e6, 3), "median_ms": round(q(vals, 0.5) / 1e6, 3),
                      "p90_ms": round(q(vals, 0.9) / 1e6, 3), "p99_ms": round(q(vals, 0.99) / 1e6, 3),
                      "max_ms": round(vals[-1] / 1e6, 3),
                      "ge_5ms": sum(1 for x in vals if x >= 5_000_000),
                      "ge_10ms": sum(1 for x in vals if x >= 10_000_000),
                      "ge_20ms": sum(1 for x in vals if x >= 20_000_000)})
    longest = [{"tid": v.tid(tid), "role": role, "from_s": sec(start), "length_ms": round(length / 1e6, 3)}
               for length, start, tid, role in sorted(runs, key=lambda x: (-x[0], x[1], x[2]))[:top]]
    return {"threshold_ms": gap_ms, "roles": roles, "longest": longest}


def render_runs(d):
    out = ["", "== runs: consecutive samples of one thread closer than %g ms ==" % d["threshold_ms"]]
    out += fmt_table(["role", "runs", "runs/s", "total ms", "median", "p90", "p99", "max", ">=5ms", ">=10ms", ">=20ms"],
                     [[r["role"], r["runs"], "%.1f" % r["runs_per_s"], "%.0f" % r["total_ms"],
                       "%.1f" % r["median_ms"], "%.1f" % r["p90_ms"], "%.1f" % r["p99_ms"], "%.1f" % r["max_ms"],
                       r["ge_5ms"], r["ge_10ms"], r["ge_20ms"]]
                      for r in d["roles"]])
    out += ["", "-- longest runs --"]
    out += fmt_table(["tid", "role", "from(s)", "length ms"],
                     [[r["tid"], r["role"], r["from_s"], "%.1f" % r["length_ms"]] for r in d["longest"]],
                     left=(0, 1))
    return out


def build_callers(v, needle, depth, top):
    if depth < 1:
        raise BadInput("--caller-depth must be >= 1")
    frames = v.prof.frames
    c = collections.Counter()
    total = 0
    for r in v.sel:
        stack = r[4]
        if not stack or needle not in frames[stack[0]].name:
            continue
        total += 1
        leaf = frames[stack[0]]
        system = leaf.bpath.startswith(SYSTEM_PREFIXES)
        chain = []
        for fid in stack[1:]:
            f = frames[fid]
            if system and f.bpath == leaf.bpath:
                continue
            chain.append(f)
            if len(chain) == depth:
                break
        c[(leaf.name, tuple((f.name, f.site or "") for f in chain))] += 1
    rows = []
    for (leaf, chain), n in ranked(c, top):
        rows.append({"leaf": leaf, "samples": n, "pct": pct(n, total),
                     "callers": [{"name": nm, "site": site} for nm, site in chain]})
    return {"match": needle, "depth": depth, "matched_samples": total, "rows": rows}


def render_callers(d):
    out = ["", "== callers of leaf functions matching %r (%d samples) ==" % (d["match"], d["matched_samples"])]
    rows = []
    for r in d["rows"]:
        chain = "  <  ".join("%s %s" % (trunc(c["name"], 50), c["site"]) for c in r["callers"])
        rows.append([r["samples"], "%.2f%%" % r["pct"], trunc(r["leaf"], 40), chain])
    out += fmt_table(["samples", "share", "leaf", "called from"], rows, left=(2, 3))
    return out


# --------------------------------------------------------------------------- main

def positive(text):
    try:
        n = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError("%r is not an integer" % text)
    if n < 1:
        raise argparse.ArgumentTypeError("must be >= 1")
    return n


def parser():
    ap = argparse.ArgumentParser(
        prog="xctrace_profile.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("export", metavar="time-profile.xml", help="xctrace export of the time-profile table")
    ap.add_argument("--from", dest="from_s", type=float, default=None, metavar="S",
                    help="window start, seconds from the first sample (default 0); inclusive")
    ap.add_argument("--to", dest="to_s", type=float, default=None, metavar="S",
                    help="window end, seconds from the first sample (default: after the last sample); exclusive")
    ap.add_argument("--thread", default=None, metavar="TID[,..]",
                    help="only these thread ids (0x.. hex as Instruments prints them, or decimal)")
    ap.add_argument("--role", default=None, metavar="ROLE[,..]",
                    help="only these roles: %s, or spu-worker for both busy and idle" % ", ".join(ROLES))
    ap.add_argument("--top", type=positive, default=20, metavar="N", help="rows per table (default 20)")
    ap.add_argument("--by", choices=("self", "inclusive", "line"), default=None,
                    help="hotspot table key: self (default), inclusive or line; always prints the table")
    ap.add_argument("--roles", action="store_true", help="CPU per role, P/E cores, busiest threads")
    ap.add_argument("--timeline", type=float, default=None, metavar="SEC", help="samples per role per SEC-second bucket")
    ap.add_argument("--guest", action="store_true", help="lifted guest PPU functions (func_XXXXXXXX)")
    ap.add_argument("--gaps", type=float, default=None, metavar="MS", help="gaps longer than MS in the selection's samples")
    ap.add_argument("--runs", type=float, default=None, metavar="MS", help="CPU bursts per thread: samples closer than MS")
    ap.add_argument("--callers", default=None, metavar="NAME", help="callers of leaf functions containing NAME")
    ap.add_argument("--caller-depth", type=int, default=1, metavar="N", help="frames shown per caller chain (default 1)")
    ap.add_argument("--json", action="store_true", help="one JSON document instead of the text report")
    return ap


def run(args):
    args.threads = parse_threads(args.thread) if args.thread is not None else None
    args.roles_filter = parse_roles(args.role) if args.role is not None else None
    if args.from_s is not None and args.from_s < 0:
        raise BadInput("--from must be >= 0")
    prof = load(args.export)
    v = View(prof, args)
    doc = {"schema": SCHEMA, "input": os.path.basename(args.export), "summary": build_summary(v)}
    sections = (args.roles or args.timeline is not None or args.guest or args.gaps is not None
                or args.callers is not None or args.runs is not None)
    if not sections or args.by is not None:
        doc["hotspots"] = build_hotspots(v, args.by or "self", args.top)
    if args.roles:
        doc["roles"] = build_roles(v, args.top)
    if args.timeline is not None:
        doc["timeline"] = build_timeline(v, args.timeline)
    if args.guest:
        doc["guest"] = build_guest(v, args.top)
    if args.gaps is not None:
        doc["gaps"] = build_gaps(v, args.gaps, args.top)
    if args.runs is not None:
        doc["runs"] = build_runs(v, args.runs, args.top)
    if args.callers is not None:
        doc["callers"] = build_callers(v, args.callers, args.caller_depth, args.top)
    return doc


def render(doc):
    s = doc["summary"]
    out = ["%s: window %.3f-%.3f s, %d samples in window, %d selected (%.0f ms CPU, %.3f cores avg, %d threads)"
           % (doc["input"], s["window_s"][0], s["window_s"][1], s["samples_in_window"], s["samples_selected"],
              s["cpu_ms_selected"], s["avg_cores_selected"], s["threads_selected"])]
    for key, fn in (("hotspots", render_hotspots), ("roles", render_roles), ("timeline", render_timeline),
                    ("guest", render_guest), ("gaps", render_gaps), ("runs", render_runs),
                    ("callers", render_callers)):
        if key in doc:
            out += fn(doc[key])
    return "\n".join(out) + "\n"


def main(argv=None):
    try:
        args = parser().parse_args(argv)
    except SystemExit as e:
        return e.code if isinstance(e.code, int) else 2
    try:
        doc = run(args)
    except BadInput as e:
        print("xctrace_profile: %s" % e, file=sys.stderr)
        return 2
    if args.json:
        sys.stdout.write(json.dumps(doc, indent=2) + "\n")
    else:
        sys.stdout.write(render(doc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
