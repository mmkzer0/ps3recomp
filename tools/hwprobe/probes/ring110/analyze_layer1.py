#!/usr/bin/env python3
"""Host analysis of a ring110 Layer 1 CSV (PROBE.md section 4, classification section 6).

Usage:  analyze_layer1.py RUN.csv [RUN2.csv ...] [--json]

Prints Q1-Q6, x_clear and the hypothesis class. Standard library only.
CSV format: FORMAT.md. Evidence tiers: everything printed is computed from the CSV
(OBSERVED on real data, SYNTHETIC on test data); the class is an INFERENCE from x_clear.
"""
import argparse
import bisect
import collections
import json
import math
import os
import random
import statistics
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
import hwprobe_format

P_MS = 256.0 / 48000.0 * 1000.0   # block period, 5.3333 ms
TOL_MS = 0.7                      # section 6 class tolerance

EV_IDX, EV_SENT, EV_REFILL, EV_GAP, EV_GWRITE, EV_GZERO = range(6)

CLASS_TEXT = {
    "B": "B: the toolkit's order is the hardware order (index names the next block to read)",
    "A": "A (read at the change), equivalently B + one-period prefetch",
    "intermediate": "intermediate: A with read delay x == B + prefetch e = P - x",
    "deeper": "deeper prefetch: the index runs ahead of the block being consumed by more than a period",
    "outside-table": "outside the section 6 table (x > P + 0.7 ms, or exactly on a class boundary)",
    "undetermined": "undetermined by Layer 1: no repeated clears to time (with refills off the hardware zeroes each block once; "
                    "if it never does, late writes would replay every lap); Layer 2 is needed",
}

# Named columns of the section 6 table: (label, x in ms)
TABLE_COLUMNS = [
    ("B (RPCS3 / toolkit)", P_MS),
    ("A, read at change", 0.0),
    ("A, read delayed 2.6 ms", 2.6),
    ("B + prefetch e = P/2", P_MS / 2.0),
]

# Q2 prediction of the block label that zeroes within +-1 ms of t_c(r)
EXPECTED_LABEL = {"B": "r-1", "A": "r", "intermediate": None, "deeper": None}


def classify(x_ms, p_ms=P_MS, tol=TOL_MS):
    """Section 6 'x estimate -> class' table."""
    if abs(x_ms - p_ms) < tol:
        return "B"
    if abs(x_ms) < tol:
        return "A"
    if tol < x_ms < p_ms - tol:
        return "intermediate"
    if x_ms < -tol:
        return "deeper"
    return "outside-table"


# ---------------------------------------------------------------- parsing

class Run:
    def __init__(self):
        self.meta = {}
        self.sync = []        # (tb_a, st_us, tb_b)
        self.idx = []         # (tb, idx, hi)
        self.sent = collections.defaultdict(list)    # blk -> [(tb, word)]
        self.refill = collections.defaultdict(list)  # blk -> [tb]
        self.gaps = []        # (tb, ticks)
        self.q4 = []
        self.qe = []
        self.qt = []
        self.sentword = {}
        self.finalword = {}
        self.complete = False
        self.header = None
        self.bad_lines = 0

    @property
    def tbfreq(self):
        return float(self.meta["tbfreq"])

    @property
    def n(self):
        return int(self.meta["blocks"])

    @property
    def discard(self):
        return int(self.meta.get("discard", 100))

    def ms(self, ticks):
        return ticks * 1000.0 / self.tbfreq


def _i(s):
    return int(s, 0)


def parse_lines(lines):
    lines = list(lines)
    header, start = hwprobe_format.parse_header(lines)
    if header["probe"] != "ring110":
        raise hwprobe_format.FormatError("result file is from probe {!r}, not ring110".format(header["probe"]))
    r = Run()
    r.header = header
    for line in lines[start:]:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = line.split(",")
        t = f[0]
        try:
            if t == "META":
                r.meta[f[1]] = ",".join(f[2:])
            elif t == "SYNC":
                r.sync.append((_i(f[1]), _i(f[2]), _i(f[3])))
            elif t == "EV":
                tb, kind, idx, hi, blk = _i(f[1]), _i(f[2]), _i(f[3]), _i(f[4]), _i(f[5])
                a, _b, _c = _i(f[6]), _i(f[7]), _i(f[8])
                if kind == EV_IDX:
                    r.idx.append((tb, idx, hi))
                elif kind == EV_SENT:
                    r.sent[blk].append((tb, a))
                elif kind == EV_REFILL:
                    r.refill[blk].append(tb)
                elif kind == EV_GAP:
                    r.gaps.append((tb, a))
            elif t == "Q4":
                v = [_i(x) for x in f[1:]]
                r.q4.append({
                    "tick": v[0], "idx": v[1], "tb0": v[2], "st_a": v[3], "tb1": v[4], "tb2": v[5],
                    "st_b": v[6], "tb3": v[7], "tag": v[8:11], "ts": v[11:14],
                    "rc_tag": v[14:17], "rc_ts": v[17:20],
                })
            elif t == "QE":
                r.qe.append({"tb": _i(f[1]), "source": _i(f[2]), "d": [_i(f[3]), _i(f[4]), _i(f[5])],
                             "idx_at_wake": _i(f[6]), "rc": _i(f[7])})
            elif t == "QT":
                r.qt.append({"tick": _i(f[1]), "m": _i(f[2]), "tb": _i(f[3]), "tag": _i(f[4]),
                             "ts": _i(f[5]), "rc": _i(f[6])})
            elif t == "SENTWORD":
                r.sentword[_i(f[1])] = _i(f[2])
            elif t == "FINALWORD":
                r.finalword[_i(f[1])] = _i(f[2])
            elif t == "END":
                r.complete = True
        except (IndexError, ValueError):
            r.bad_lines += 1
    for key in ("tbfreq", "blocks"):
        if key not in r.meta:
            raise ValueError(f"CSV has no META,{key} row")
    if int(r.meta["tbfreq"]) != header["tbfreq"]:
        raise hwprobe_format.FormatError("header tbfreq {} differs from META,tbfreq {}".format(
            header["tbfreq"], r.meta["tbfreq"]))
    return r


def parse_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return parse_lines(fh)


# ---------------------------------------------------------------- statistics helpers

def pct(sorted_v, p):
    if not sorted_v:
        return None
    k = (len(sorted_v) - 1) * p / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    return sorted_v[lo] + (sorted_v[hi] - sorted_v[lo]) * (k - lo)


def summarize(values):
    if not values:
        return {"n": 0}
    s = sorted(values)
    return {
        "n": len(s),
        "mean": statistics.fmean(s),
        "std": statistics.pstdev(s) if len(s) > 1 else 0.0,
        "median": statistics.median(s),
        "p10": pct(s, 10),
        "p90": pct(s, 90),
        "min": s[0],
        "max": s[-1],
    }


def bootstrap_median_ci(values, n_boot=400, seed=12345):
    if len(values) < 5:
        return None
    rng = random.Random(seed)
    meds = []
    for _ in range(n_boot):
        meds.append(statistics.median(rng.choices(values, k=len(values))))
    meds.sort()
    return (pct(meds, 2.5), pct(meds, 97.5))


# ---------------------------------------------------------------- Q1 / Q5

def analyze_q1_q5(run):
    n = run.n
    ev = run.idx
    out = {"n_idx_events": len(ev)}
    out["idx_before_start"] = run.meta.get("idx_before_start")
    out["idx_after_start"] = run.meta.get("idx_after_start")
    out["first_idx_change_to"] = ev[0][1] if ev else None

    dts, skips, repeats, torn, out_of_range = [], 0, 0, 0, 0
    skip_ordinals = []
    prev = None
    for ordinal, (tb, idx, hi) in enumerate(ev, start=1):
        if hi:
            torn += 1
        if idx >= n:
            out_of_range += 1
        if prev is not None:
            step = (idx - prev[1]) % n
            if step == 1:
                if ordinal > run.discard:
                    dts.append(run.ms(tb - prev[0]))
            elif step == 0:
                repeats += 1
            else:
                skips += 1
                skip_ordinals.append(ordinal)
        prev = (tb, idx)
    out["q1_period_ms"] = summarize(dts)
    out["q1_period_expected_ms"] = P_MS
    out["q5"] = {"skips": skips, "skip_at_ordinals": skip_ordinals[:10], "repeats": repeats,
                 "torn_hi_nonzero": torn, "out_of_range": out_of_range}
    max_gap = int(run.meta.get("max_gap_ticks", 0))
    out["max_gap_us"] = max_gap * 1e6 / run.tbfreq
    out["n_gap_events"] = len(run.gaps)
    return out


# ---------------------------------------------------------------- Q2 / Q3

def _usable_ticks(run):
    """Index events after the discard window, without a Q4 call on the same tick
    (the call delays the poller by tens of us, which would skew the clear timing)."""
    q4_ticks = {q["tick"] for q in run.q4}
    all_ticks = [(o, tb, idx) for o, (tb, idx, hi) in enumerate(run.idx, start=1) if o > run.discard]
    clean = [t for t in all_ticks if t[0] not in q4_ticks]
    if clean:
        return clean, False
    return all_ticks, bool(q4_ticks)


def analyze_q2(run):
    n = run.n
    p_ticks = run.tbfreq * 256.0 / 48000.0
    half = n / 2.0 * p_ticks
    zeros = {b: sorted(tb for tb, w in v if w == 0) for b, v in run.sent.items()}
    ticks, q4_contaminated = _usable_ticks(run)
    n_zero_events = sum(len(v) for v in zeros.values())
    out = {"ticks_used": len(ticks), "q4_contaminated": q4_contaminated, "n_zero_events": n_zero_events}
    per_k = {}
    for k in range(-3, 4):
        deltas = []
        for _o, tc, idx in ticks:
            lst = zeros.get((idx + k) % n)
            if not lst:
                continue
            j = bisect.bisect_left(lst, tc)
            cand = [lst[x] for x in (j - 1, j) if 0 <= x < len(lst)]
            z = min(cand, key=lambda t: abs(t - tc))
            if abs(z - tc) <= half:
                deltas.append(run.ms(z - tc))
        s = summarize(deltas)
        s["frac_within_1ms"] = (sum(1 for d in deltas if abs(d) <= 1.0) / len(ticks)) if ticks else 0.0
        per_k[k] = s
        s["_values"] = deltas
    out["per_k"] = {k: {kk: vv for kk, vv in v.items() if kk != "_values"} for k, v in per_k.items()}

    hw_clear = n_zero_events >= 0.1 * max(1, len(ticks)) and per_k[0]["n"] > 0
    out["hw_clear"] = hw_clear
    best = max(range(-3, 4), key=lambda k: per_k[k]["frac_within_1ms"])
    if per_k[best]["frac_within_1ms"] >= 0.5:
        out["label_within_1ms"] = "r" if best == 0 else f"r{best:+d}"
    else:
        out["label_within_1ms"] = None
    if hw_clear:
        vals = per_k[0]["_values"]
        out["x_clear_ms"] = statistics.median(vals)
        out["x_clear_ci95"] = bootstrap_median_ci(vals)
        out["x_clear_n"] = len(vals)
    else:
        out["x_clear_ms"] = None
        out["x_clear_ci95"] = None
        out["x_clear_n"] = 0
    return out


def analyze_q3(run):
    n = run.n
    p_ticks = run.tbfreq * 256.0 / 48000.0
    lap = n * p_ticks
    # per-block timeline of the sentinel word: hardware changes and guest refills
    timeline = {}
    for b in range(n):
        tl = [(tb, w) for tb, w in run.sent.get(b, [])]
        sw = run.sentword.get(b)
        if sw is not None:
            tl += [(tb, sw) for tb in run.refill.get(b, [])]
        tl.sort()
        timeline[b] = ([t for t, _ in tl], [w for _, w in tl])

    def state_at(b, t):
        times, words = timeline[b]
        j = bisect.bisect_right(times, t) - 1
        return words[j] if j >= 0 else run.sentword.get(b)

    def label(b, w):
        if w == 0:
            return "zero"
        if run.sentword.get(b) == w:
            return "sentinel"
        return f"other:0x{w:08x}"

    hist_changes = collections.Counter()
    for b, lst in run.sent.items():
        for _tb, w in lst:
            hist_changes[label(b, w)] += 1
    hist_after = collections.Counter()
    t_end = run.idx[-1][0] if run.idx else 0
    for b, lst in run.sent.items():
        for tb, w in lst:
            if w == 0 and tb + lap < t_end:
                # one lap minus P/2 after the zero: the word just before it is consumed again
                hist_after[label(b, state_at(b, tb + lap - p_ticks / 2.0))] += 1
    other = {k: v for k, v in hist_changes.items() if k.startswith("other")}
    return {
        "hardware_word_changes": dict(hist_changes),
        "state_one_lap_after_zero": dict(hist_after),
        "final_words": {b: f"0x{w:08x}" for b, w in sorted(run.finalword.items())},
        "hw_writes_other_than_zero": bool(other),
        "other_values": other,
    }


# ---------------------------------------------------------------- Q4

def _fit_tb_to_us(run):
    pts = [((a + b) / 2.0, st) for a, st, b in run.sync]
    nominal = 1e6 / run.tbfreq
    if len(pts) >= 2 and pts[-1][0] - pts[0][0] > 5 * run.tbfreq:
        mx = statistics.fmean(p[0] for p in pts)
        my = statistics.fmean(p[1] for p in pts)
        sxx = sum((p[0] - mx) ** 2 for p in pts)
        slope = sum((p[0] - mx) * (p[1] - my) for p in pts) / sxx
        return lambda tb: my + slope * (tb - mx), slope / nominal - 1.0
    if pts:
        tb0, st0 = pts[0]
        return lambda tb: st0 + (tb - tb0) * nominal, None
    return None, None


def analyze_q4(run):
    out = {"n_rows": len(run.q4)}
    if not run.q4:
        return out
    to_us, drift = _fit_tb_to_us(run)
    out["drift_ppm_vs_nominal"] = None if drift is None else drift * 1e6
    labels = ["r-1", "r", "r+1"]
    d = {k: [] for k in range(3)}
    d_st = []
    mism = 0
    for q in run.q4:
        o = q["tick"]
        if not (1 <= o <= len(run.idx)) or run.idx[o - 1][1] != q["idx"]:
            mism += 1
            continue
        if to_us is None:
            continue
        tcus = to_us(run.idx[o - 1][0])
        for k in range(3):
            if q["rc_ts"][k] == 0 and q["rc_tag"][k] == 0:
                d[k].append(q["ts"][k] - tcus)
        if q["rc_ts"][1] == 0:
            d_st.append(q["ts"][1] - q["st_a"])
    out["tick_idx_mismatch"] = mism
    out["ts_minus_tc_us"] = {labels[k]: summarize(d[k]) for k in range(3)}
    out["ts0_minus_systime_before_us"] = summarize(d_st)
    tags = [q for q in run.q4 if all(c == 0 for c in q["rc_tag"])]
    out["tag_diff_r_minus_rm1"] = dict(collections.Counter(q["tag"][1] - q["tag"][0] for q in tags).most_common(5))
    out["tag_diff_rp1_minus_r"] = dict(collections.Counter(q["tag"][2] - q["tag"][1] for q in tags).most_common(5))
    # same block queried on consecutive ticks (needs q4_every = 1)
    by_tick = {q["tick"]: q for q in tags}
    same_p1 = changed_p1 = same_0 = changed_0 = 0
    for o, q in by_tick.items():
        nx = by_tick.get(o + 1)
        if nx is None:
            continue
        if q["tag"][2] == nx["tag"][1]:
            same_p1 += 1
        else:
            changed_p1 += 1
        if q["tag"][1] == nx["tag"][0]:
            same_0 += 1
        else:
            changed_0 += 1
    out["tag_consecutive_ticks"] = {
        "pairs": same_p1 + changed_p1,
        "block_r+1_tag_changed_between_t_c(r)_and_t_c(r+1)": changed_p1,
        "block_r+1_tag_unchanged": same_p1,
        "block_r_tag_changed_between_t_c(r)_and_t_c(r+1)": changed_0,
        "block_r_tag_unchanged": same_0,
    }
    rc = collections.Counter()
    for q in run.q4:
        for k in range(3):
            rc["tag:0x{:08x}".format(q["rc_tag"][k] & 0xFFFFFFFF)] += 1
            rc["ts:0x{:08x}".format(q["rc_ts"][k] & 0xFFFFFFFF)] += 1
    out["return_codes"] = dict(rc)
    ts_mono = all(b["ts"][1] >= a["ts"][1] for a, b in zip(run.q4, run.q4[1:]) if a["rc_ts"][1] == 0 == b["rc_ts"][1])
    out["ts_monotonic"] = ts_mono
    return out


# ---------------------------------------------------------------- Q4, timestamp window

def analyze_qt(run):
    """QT rows: timestamp of the tag the current block had m ticks ago. Which m the firmware accepts."""
    out = {"n_rows": len(run.qt)}
    if not run.qt:
        return out
    to_us, _drift = _fit_tb_to_us(run)
    by_m = collections.defaultdict(list)
    by_tick = collections.defaultdict(dict)
    for t in run.qt:
        by_m[t["m"]].append(t)
        if t["rc"] == 0:
            by_tick[t["tick"]][t["m"]] = t["ts"]
    per_m = {}
    for m in sorted(by_m):
        rows = by_m[m]
        ok = [t for t in rows if t["rc"] == 0]
        diffs = []
        for t in ok:
            o = t["tick"]
            if to_us is not None and 1 <= o <= len(run.idx):
                diffs.append(t["ts"] - to_us(run.idx[o - 1][0]))
        rc = collections.Counter("0x{:08x}".format(t["rc"] & 0xFFFFFFFF) for t in rows)
        per_m[m] = {"n": len(rows), "n_ok": len(ok), "rc": dict(rc), "ts_minus_tc_us": summarize(diffs)}
    out["per_m"] = per_m
    out["accepted_m"] = [m for m, e in per_m.items() if e["n_ok"] == e["n"]]
    out["refused_m"] = [m for m, e in per_m.items() if e["n_ok"] == 0]
    steps = [ts[m] - ts[m + 1] for ts in by_tick.values() for m in ts if m + 1 in ts]
    out["ts_step_us"] = summarize(steps)
    tag_by_tick = {}
    for t in run.qt:
        tag_by_tick.setdefault(t["tick"], {})[t["m"]] = t["tag"]
    gaps = collections.Counter(tags[m] - tags[m + 1] for tags in tag_by_tick.values() for m in tags if m + 1 in tags)
    out["tag_step"] = gaps.most_common(1)[0][0] if gaps else None
    return out


# ---------------------------------------------------------------- Q6

def analyze_q6(run):
    out = {"n_events": len(run.qe)}
    if not run.qe or not run.idx:
        return out
    times = [tb for tb, _i_, _h in run.idx]
    dts, before, idx_ahead, idx_same = [], 0, 0, 0
    for e in run.qe:
        j = bisect.bisect_right(times, e["tb"])      # first change strictly after the event
        prev_i = j - 1
        next_i = j if j < len(times) else None
        d_prev = e["tb"] - times[prev_i] if prev_i >= 0 else None
        d_next = times[next_i] - e["tb"] if next_i is not None else None
        if d_prev is not None and (d_next is None or d_prev <= d_next):
            dt = run.ms(d_prev)
            ref_idx = run.idx[prev_i][1]
        elif d_next is not None:
            dt = -run.ms(d_next)
            ref_idx = run.idx[next_i][1]
            before += 1
        else:
            continue
        dts.append(dt)
        if dt >= 0:
            idx_same += int(e["idx_at_wake"] == ref_idx)
        else:
            idx_ahead += int(e["idx_at_wake"] == ref_idx)
    out["t_ev_minus_t_c_ms"] = summarize(dts)
    out["fraction_event_before_index_change"] = before / len(dts) if dts else None
    out["after_change_idx_at_wake_equals_published"] = idx_same
    out["before_change_idx_at_wake_already_new"] = idx_ahead
    out["note"] = ("t_ev includes the wake-up latency of the event thread; a negative sign is the "
                   "robust observation, a small positive value is not evidence of 'after'.")
    return out


# ---------------------------------------------------------------- top level

def analyze(run):
    res = {"header": run.header,
           "meta": {k: run.meta.get(k) for k in
                    ("layer", "run", "blocks", "channels", "tbfreq", "tag", "abort_reason", "nchg",
                     "ev_drop", "poll_prio", "q4_every", "refill", "readmode", "build", "ui_enabled",
                     "ui_overlaps", "ui_draws", "ui_deferred", "ui_max_draw_us")},
           "complete": run.complete, "bad_lines": run.bad_lines}
    res.update(analyze_q1_q5(run))
    q2 = analyze_q2(run)
    res["q2"] = q2
    res["q3"] = analyze_q3(run)
    res["q4"] = analyze_q4(run)
    res["qt"] = analyze_qt(run)
    res["q6"] = analyze_q6(run)

    x = q2["x_clear_ms"]
    if x is None:
        cls = "undetermined"
        nearest = None
    else:
        cls = classify(x)
        nearest = min(TABLE_COLUMNS, key=lambda c: abs(c[1] - x))
        nearest = nearest[0] if abs(nearest[1] - x) < TOL_MS else None
    exp_label = EXPECTED_LABEL.get(cls)
    res["x_clear_ms"] = x
    res["class"] = cls
    res["class_text"] = CLASS_TEXT[cls]
    res["nearest_table_column"] = nearest
    res["label_within_1ms"] = q2["label_within_1ms"]
    res["label_consistent"] = (None if cls in ("undetermined", "outside-table")
                               else (q2["label_within_1ms"] == exp_label))
    res["hw_clear"] = q2["hw_clear"]
    return res


def _fmt(s, unit="ms", digits=3):
    if not s or not s.get("n"):
        return "n=0"
    return "n={} mean={:.{d}f} std={:.{d}f} median={:.{d}f} p10={:.{d}f} p90={:.{d}f} {}".format(
        s["n"], s["mean"], s["std"], s["median"], s["p10"], s["p90"], unit, d=digits)


def report(res):
    L = []
    m = res["meta"]
    L.append("== ring110 Layer 1: run {}, n={} blocks, {} ch, tbfreq {} Hz, tag '{}' ==".format(m["run"], m["blocks"], m["channels"], m["tbfreq"], m["tag"] or ""))
    if not res["complete"]:
        L.append("WARNING: CSV has no END row (truncated)")
    if res["bad_lines"]:
        L.append("WARNING: {} unparsable lines".format(res["bad_lines"]))
    if m["abort_reason"] not in (None, "0"):
        L.append("WARNING: probe aborted early, abort_reason={} (1 timeout, 2 index stalled, 3 user exit, "
                 "4 start failed, 5 polling thread not created)".format(m["abort_reason"]))
    if m["ev_drop"] not in (None, "0"):
        L.append("WARNING: {} log records dropped".format(m["ev_drop"]))
    if m["ui_overlaps"] not in (None, "0"):
        L.append("WARNING: {} screen draws overlapped the measurement (measurement integrity rule broken)".format(m["ui_overlaps"]))
    if m["ui_enabled"] == "0":
        L.append("note: screen was off for this run")
    h = res["header"]
    L.append("probe {} {} ({}, source {}), config {}, console {} fw {} {} [{}]".format(h["probe"], h["probe_version"], h["git_describe"], h["source_sha256"][:12], h["config"],
              h["console_model"], h["fw_version"], h["fw_type"], h["console_info_source"]))
    L.append("")
    L.append("Q1 period: {} (expected {:.4f} ms)".format(_fmt(res["q1_period_ms"]), res["q1_period_expected_ms"]))
    L.append("   index before start {}, after start returned {}, first change to {}".format(res["idx_before_start"], res["idx_after_start"], res["first_idx_change_to"]))
    L.append("   max polling gap {:.1f} us, gap events (>20 us) {}".format(res["max_gap_us"], res["n_gap_events"]))
    q5 = res["q5"]
    L.append("Q5 index consistency: skips={} repeats={} torn(hi!=0)={} out_of_range={}".format(
        q5["skips"], q5["repeats"], q5["torn_hi_nonzero"], q5["out_of_range"]))
    q2 = res["q2"]
    contaminated = " (Q4-contaminated, all ticks)" if q2["q4_contaminated"] else ""
    L.append("Q2 clear timing: {} ticks used{}, {} zero events, hardware clear: {}".format(
        q2["ticks_used"], contaminated, q2["n_zero_events"], "yes" if res["hw_clear"] else "NO REPEATED CLEARS"))
    if res["hw_clear"]:
        for k in sorted(q2["per_k"]):
            s = q2["per_k"][k]
            if s["n"]:
                head = f"Delta(r{k:+d})" if k else "Delta(r)  "
                L.append("   {} {}  within +-1ms: {:.0f}%".format(head, _fmt(s), 100 * s["frac_within_1ms"]))
        ci = q2["x_clear_ci95"]
        L.append("   block label zeroing within +-1 ms of t_c: {}".format(res["label_within_1ms"] or "none"))
        ci_text = "" if ci is None else " [95% CI {:.3f}, {:.3f}]".format(*ci)
        L.append("   x_clear = {:.3f} ms{} (n={})".format(res["x_clear_ms"], ci_text, q2["x_clear_n"]))
    else:
        L.append("   x_clear = n/a (fewer than 0.1 zero events per tick: the hardware zeroes a block once, or never, and nothing "
                 "refills it, so there is no repeated clear to time)")
    q3 = res["q3"]
    L.append("Q3 content after read: hardware word changes {}".format(q3["hardware_word_changes"]))
    L.append("   sentinel word at one lap - P/2 after a zero: {}; hardware writes other than zero: {}".format(q3["state_one_lap_after_zero"], "YES {}".format(q3["other_values"]) if q3["hw_writes_other_than_zero"] else "no"))
    q4 = res["q4"]
    L.append(f"Q4 tag/timestamp: {q4['n_rows']} rows")
    if q4["n_rows"]:
        for lab, s in q4["ts_minus_tc_us"].items():
            L.append("   ts(tag_{}) - t_c: {}".format(lab, _fmt(s, "us", 1)))
        L.append("   ts(tag_r) - sysGetSystemTime() before the calls: {}".format(_fmt(q4["ts0_minus_systime_before_us"], "us", 1)))
        L.append("   tag_r - tag_(r-1): {}   tag_(r+1) - tag_r: {}".format(q4["tag_diff_r_minus_rm1"], q4["tag_diff_rp1_minus_r"]))
        L.append("   consecutive ticks: {}".format(q4["tag_consecutive_ticks"]))
        L.append("   return codes: {}; ts monotonic: {}; drift vs nominal: {} ppm".format(q4["return_codes"], q4["ts_monotonic"],
                  None if q4.get("drift_ppm_vs_nominal") is None else "{:.1f}".format(q4["drift_ppm_vs_nominal"])))
    qt = res["qt"]
    if qt["n_rows"]:
        L.append("Q4 timestamp window (QT rows: tag of block r from m ticks ago; the firmware refuses tags of blocks not yet played):")
        L.append("   tag step between blocks {}; accepted m: {}; refused m: {}".format(
            qt["tag_step"], qt["accepted_m"] or "none", qt["refused_m"] or "none"))
        for m, e in qt["per_m"].items():
            L.append("   m={:2d} accepted {}/{}  return codes {}  ts - t_c: {}".format(
                m, e["n_ok"], e["n"], e["rc"], _fmt(e["ts_minus_tc_us"], "us", 1)))
        L.append("   ts(m) - ts(m+1) on one tick: {}".format(_fmt(qt["ts_step_us"], "us", 1)))
    q6 = res["q6"]
    L.append(f"Q6 event order: {q6['n_events']} events")
    if q6.get("t_ev_minus_t_c_ms"):
        L.append("   t_ev - t_c: {}; before index change: {:.1f}%".format(_fmt(q6["t_ev_minus_t_c_ms"]), 100 * q6["fraction_event_before_index_change"]))
        L.append("   ({})".format(q6["note"]))
    L.append("")
    x = res["x_clear_ms"]
    L.append("x_clear = {}".format("n/a" if x is None else f"{x:.3f} ms"))
    L.append("CLASS (section 6, from x_clear): {}".format(res["class"]))
    L.append("   " + res["class_text"])
    if res["nearest_table_column"]:
        L.append("   nearest table column: {}".format(res["nearest_table_column"]))
    if res["label_consistent"] is not None:
        L.append("   Q2 label check: {} (zeroing label within 1 ms: {})".format("consistent with the class" if res["label_consistent"] else "INCONSISTENT with the class",
                  res["label_within_1ms"] or "none"))
    L.append("   Layer 1 gives x_clear only; x_latch needs Layer 2 (x_clear must be within 0.7 ms of it).")
    return "\n".join(L)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--json", action="store_true", help="print JSON instead of text")
    args = ap.parse_args(argv)
    results = []
    for path in args.csv:
        try:
            res = analyze(parse_file(path))
        except (hwprobe_format.FormatError, ValueError) as e:
            print(f"{path}: {e}", file=sys.stderr)
            return 2
        res["file"] = path
        results.append(res)
        if not args.json:
            print("# " + path)
            print(report(res))
            print()
    if args.json:
        json.dump(results, sys.stdout, indent=1, default=str)
        print()
    if len(results) > 1 and not args.json:
        xs = [r["x_clear_ms"] for r in results if r["x_clear_ms"] is not None]
        if len(xs) > 1:
            print("x_clear over {} runs: {} ms, spread {:.3f} ms (section 7: if > 1 ms, x is not a constant)".format(
                len(xs), ", ".join(f"{v:.3f}" for v in xs), max(xs) - min(xs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
