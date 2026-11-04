#!/usr/bin/env python3
"""Host analysis of a ring110 Layer 2 run: result CSV plus the audio capture (PROBE.md sections 5 and 6).

Usage:  analyze_layer2.py RUN.csv CAPTURE.wav [--json] [options]

Reads the run's CSV (trial table, hardware clear events) and a 48 kHz, 16/24/32-bit PCM WAV of the console's
output, finds every PN marker by matched filter, joins them to the trials by code id, classifies each trial as
hit, miss or other from Delta = arrival(test) - arrival(reference), fits hit probability against o_actual per
j and reports x_latch = o50(j) - j*P with the per-j agreement, the recorder drift and the detector's false
alarms. Standard library only (the CSV tools of this probe do not need numpy, so neither does this one).
CSV format: FORMAT.md. Evidence tiers: every number is computed from the two files (OBSERVED on real data,
SYNTHETIC on test data); the class is an INFERENCE from x_latch.
"""
import argparse
import array
import bisect
import collections
import json
import math
import os
import statistics
import struct
import sys
from operator import mul

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
import analyze_layer1 as an1
import hwprobe_format
import pn

FS = 48000                  # capture rate the probe's markers are sample-aligned to
P_MS = an1.P_MS             # block period, 5.3333 ms
BLK = 32                    # envelope block (samples); a marker spans 4 to 6 of them
MIN_MARK_BLK, MAX_MARK_BLK = 3, 8
MARK_LEN = pn.LEN
REF_CODE = 0
C2_CODES = (1001, 1002)     # control C2 markers: block 0 and block 1 (layer2/source/main.c)
AGREE_MS = 0.5              # PROBE.md section 5: the per-j estimates of x must agree within this
PARTIAL_FRAC = 0.8          # PROBE.md section 9: a marker peak below 0.8 of the reference's is not a marker
DRIFT_WARN_PPM = 1000.0


class AnalysisError(ValueError):
    """The two inputs cannot be analysed together (not a format error of one file)."""


class WavError(ValueError):
    """The capture is not a WAV this tool can read."""


# ---------------------------------------------------------------- CSV

class Run:
    def __init__(self):
        self.header = None
        self.meta = {}
        self.trials = []                          # dicts, in file order
        self.zeros = collections.defaultdict(list)  # block -> [tb] hardware wrote 0 into the watched word
        self.hw_other_words = 0                   # hardware changed the watched word to a nonzero value
        self.gaps = {}                            # tb -> gap ticks (polling gap logged at that iteration)
        self.guest_zeroes = 0
        self.complete = False
        self.bad_lines = 0

    @property
    def tbfreq(self):
        return int(self.meta["tbfreq"])

    @property
    def n(self):
        return int(self.meta["blocks"])


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
        try:
            if f[0] == "META":
                r.meta[f[1]] = ",".join(f[2:])
            elif f[0] == "EV":
                tb, kind, blk, a = _i(f[1]), _i(f[2]), _i(f[5]), _i(f[6])
                if kind == an1.EV_GAP:
                    r.gaps[tb] = a
                elif kind == an1.EV_SENT:
                    if a == 0:
                        r.zeros[blk].append(tb)
                    else:
                        r.hw_other_words += 1
                elif kind == an1.EV_GZERO:
                    r.guest_zeroes += 1
            elif f[0] == "TRIAL":
                r.trials.append({
                    "trial": _i(f[2]), "j": _i(f[3]), "o_target_ms": float(f[4]), "o_actual_ms": float(f[5]),
                    "t_c": _i(f[6]), "t_w": _i(f[7]), "idx": _i(f[8]), "code": _i(f[9]),
                    "test_blk": _i(f[10]), "ref_blk": _i(f[11]), "t_ref": _i(f[12]), "kind": _i(f[13]),
                    "done": _i(f[14])})
            elif f[0] == "END":
                r.complete = True
        except (IndexError, ValueError):
            r.bad_lines += 1
    for key in ("tbfreq", "blocks", "layer"):
        if key not in r.meta:
            raise hwprobe_format.FormatError(f"CSV has no META,{key} row")
    if r.meta["layer"] != "L2":
        raise hwprobe_format.FormatError(
            "this is a ring110 {} result; analyze_layer1.py reads it, analyze_layer2.py needs a Layer 2 CSV".format(
                r.meta["layer"]))
    if int(r.meta["tbfreq"]) != header["tbfreq"]:
        raise hwprobe_format.FormatError("header tbfreq {} differs from META,tbfreq {}".format(
            header["tbfreq"], r.meta["tbfreq"]))
    for z in r.zeros.values():
        z.sort()
    return r


def parse_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return parse_lines(fh)


# ---------------------------------------------------------------- WAV

class Capture:
    """Decoded capture: per-channel int arrays; `scale` is the full-scale value of those ints."""

    def __init__(self, ch, scale, bits, header_frames, truncated, header_unset):
        self.ch = ch
        self.nch = len(ch)
        self.n = len(ch[0])
        self.scale = float(scale)
        self.bits = bits
        self.header_frames = header_frames
        self.truncated = truncated          # the header declares more data than the file holds
        self.header_unset = header_unset    # data size 0 or 0xFFFFFFFF: recording not finalised

    @property
    def duration_s(self):
        return self.n / FS


def _decode(raw, bits):
    if bits == 24:
        n = len(raw) // 3
        buf = bytearray(4 * n)
        buf[1::4] = raw[0:3 * n:3]
        buf[2::4] = raw[1:3 * n:3]
        buf[3::4] = raw[2:3 * n:3]
        a = array.array("i")
        a.frombytes(bytes(buf))
    else:
        a = array.array("h" if bits == 16 else "i")
        a.frombytes(raw[:len(raw) // a.itemsize * a.itemsize])
    if sys.byteorder == "big":
        a.byteswap()
    return a


def read_wav(path):
    """Read an integer-PCM WAV (16, 24 or 32 bit, 48 kHz, 1 or 2 channels).

    Own RIFF reader instead of `wave`: a recording that was cut off, or streamed to a pipe, has a data size in
    its header that does not match the file, and recorders write WAVE_FORMAT_EXTENSIBLE for 24 bit; both have
    to be read, the first one reported.
    """
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise WavError(f"{path}: not a RIFF/WAVE file")
    pos, fmt, body = 12, None, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = int.from_bytes(data[pos + 4:pos + 8], "little")
        start = pos + 8
        if cid == b"fmt ":
            if size < 16 or start + 16 > len(data):
                raise WavError(f"{path}: fmt chunk too short")
            tag, nch, rate, _byterate, align, bits = struct.unpack("<HHIIHH", data[start:start + 16])
            if tag == 0xFFFE and size >= 26 and start + 26 <= len(data):
                tag = struct.unpack("<H", data[start + 24:start + 26])[0]
            fmt = (tag, nch, rate, align, bits)
        elif cid == b"data":
            body = (start, size)
            break
        pos = start + size + (size & 1)
    if fmt is None:
        raise WavError(f"{path}: no fmt chunk")
    if body is None:
        raise WavError(f"{path}: no data chunk (file cut inside the header)")
    tag, nch, rate, align, bits = fmt
    if tag != 1:
        raise WavError(f"{path}: WAVE format tag {tag}, need integer PCM (record pcm_s24le or pcm_s16le)")
    if bits not in (16, 24, 32):
        raise WavError(f"{path}: {bits}-bit samples, need 16, 24 or 32")
    if rate != FS:
        raise WavError(f"{path}: {rate} Hz, need {FS} Hz (resampling moves the markers; record at 48 kHz)")
    if nch not in (1, 2):
        raise WavError(f"{path}: {nch} channels, need 2 (the AV cable's left and right)")
    if align != nch * bits // 8:
        raise WavError(f"{path}: block align {align} does not fit {nch} ch x {bits} bit")
    start, declared = body
    avail = len(data) - start
    header_unset = declared in (0, 0xFFFFFFFF)
    size = avail if header_unset else min(declared, avail)
    frames = size // align
    raw = data[start:start + frames * align]
    a = _decode(raw, bits)
    chans = [a[c::nch] for c in range(nch)]
    return Capture(chans, 2.0 ** (15 if bits == 16 else 31), bits,
                   frames if header_unset else declared // align,
                   (not header_unset) and declared > avail, header_unset)


# ---------------------------------------------------------------- detection

class Noise:
    pass


def _percentile(sorted_v, p):
    return an1.pct(sorted_v, p)


def noise_floor(cap, silence_s):
    """DC, rms and block-peak statistics of the leading silence, and the burst threshold derived from them."""
    nsil = min(cap.n, int(silence_s * FS))
    if nsil < FS // 2:
        raise AnalysisError("the capture is shorter than 0.5 s, no leading silence to measure the noise floor on")
    nz = Noise()
    nz.silence_samples = nsil
    nz.dc, rms = [], []
    for ch in cap.ch:
        seg = ch[:nsil]
        dc = sum(seg) / nsil
        nz.dc.append(dc)
        rms.append(math.sqrt(max(sum(map(mul, seg, seg)) / nsil - dc * dc, 0.0)))
    nz.rms = max(rms)
    return nz


def block_envelope(cap, nz):
    """Peak |x - dc| per BLK samples, max over channels. A marker is +-0.5 for 128 samples, so its blocks
    stay at the marker amplitude however the sign pattern runs; a peak detector needs no filter."""
    nb = cap.n // BLK
    env = [0.0] * nb
    chans = [(ch, dc) for ch, dc in zip(cap.ch, nz.dc)]
    for b in range(nb):
        lo = b * BLK
        hi = lo + BLK
        e = 0.0
        for ch, dc in chans:
            seg = ch[lo:hi]
            v = max(max(seg) - dc, dc - min(seg))
            e = max(e, v)
        env[b] = e
    return env


class Burst:
    __slots__ = ("b0", "b1", "kind", "peak", "s_est")

    def __init__(self, b0, b1, peak):
        self.b0, self.b1, self.peak = b0, b1, peak
        nblk = b1 - b0 + 1
        self.kind = "marker" if MIN_MARK_BLK <= nblk <= MAX_MARK_BLK else ("short" if nblk < MIN_MARK_BLK else "long")
        self.s_est = None


def find_bursts(env, thr):
    out = []
    nb = len(env)
    b = 0
    while b < nb:
        if env[b] > thr:
            b1 = b
            peak = env[b]
            while b1 + 1 < nb and env[b1 + 1] > thr:
                b1 += 1
                peak = max(peak, env[b1])
            out.append(Burst(b, b1, peak))
            b = b1 + 1
        else:
            b += 1
    return out


def start_estimate(cap, nz, burst, thr):
    """First sample of the burst above a quarter of its peak: the matched filter then searches a few samples
    around it (the D/A smear delays and rounds the first chip)."""
    te = max(thr, 0.25 * burst.peak)
    lo = max(0, (burst.b0 - 1) * BLK)
    hi = min(cap.n, (burst.b0 + 2) * BLK)
    best = None
    for ch, dc in zip(cap.ch, nz.dc):
        for i in range(lo, hi):
            if abs(ch[i] - dc) >= te:
                if best is None or i < best:
                    best = i
                break
    return best if best is not None else burst.b0 * BLK


_CODES = {}


def code_signs(cid):
    c = _CODES.get(cid)
    if c is None:
        c = _CODES[cid] = [float(v) for v in pn.pn_signs(cid)]
    return c


def match_codes(cap, nz, s_est, code_ids, half=8):
    """Normalised correlation (cosine) of the capture around `s_est` with each code, best lag, both channels.

    Returns {code: (rho, pos, rho_ch0, rho_ch1)}: rho is the larger of the two channels' |cosine| at the best
    lag (a marker on one channel only, or inverted on one, still counts), pos the sample position of the
    marker's first chip with a parabolic sub-sample correction, rho_chN the signed cosine per channel.
    """
    inv = 1.0 / cap.scale
    out = {}
    for width in (half, 3 * half):
        i0 = s_est - width
        n = 2 * width + MARK_LEN
        chans = []
        for ch, dc in zip(cap.ch, nz.dc):
            lo, hi = max(i0, 0), min(i0 + n, cap.n)
            seg = [0.0] * (lo - i0) + [(v - dc) * inv for v in ch[lo:hi]] + [0.0] * max(i0 + n - hi, 0)
            cs = [0.0]
            acc = 0.0
            for v in seg:
                acc += v * v
                cs.append(acc)
            chans.append((seg, cs))
        edge = False
        out = {}
        for cid in code_ids:
            code = code_signs(cid)
            ys, per = [], []
            for lag in range(2 * width + 1):
                best, rhos = 0.0, []
                for seg, cs in chans:
                    e = cs[lag + MARK_LEN] - cs[lag]
                    rho = sum(map(mul, seg[lag:lag + MARK_LEN], code)) / math.sqrt(e * MARK_LEN) if e > 1e-14 else 0.0
                    rhos.append(rho)
                    best = max(best, abs(rho))
                ys.append(best)
                per.append(rhos)
            k = max(range(len(ys)), key=ys.__getitem__)
            delta = 0.0
            if 0 < k < len(ys) - 1:
                den = ys[k - 1] - 2.0 * ys[k] + ys[k + 1]
                if den < 0:
                    delta = max(-1.0, min(1.0, 0.5 * (ys[k - 1] - ys[k + 1]) / den))
            elif width == half:
                edge = True
            rc = per[k] + [0.0] * (2 - len(per[k]))
            out[cid] = (ys[k], i0 + k + delta, rc[0], rc[1])
        if not edge:
            break
    return out


class Detection:
    """What the capture alone shows (no CSV needed): noise floor, bursts and the reference-code detections."""

    def __init__(self):
        self.noise = None
        self.thr = 0.0
        self.plateau = 0.0
        self.bursts = []
        self.refs = []       # (pos_s, rho, rho_ch0, rho_ch1, burst index)
        self.nonref = []     # burst indices of marker-shaped bursts that are not the reference code
        self.short = []      # burst indices
        self.long = []


def detect_markers(cap, silence_s=3.0, min_corr=0.5):
    d = Detection()
    d.noise = nz = noise_floor(cap, silence_s)
    env = block_envelope(cap, nz)
    nsb = nz.silence_samples // BLK
    sil_peak = max(env[:nsb]) if nsb else 0.0
    d.sil_peak = sil_peak
    d.thr = thr = max(1.5 * sil_peak, 8.0 * nz.rms, 4.0)
    d.bursts = find_bursts(env, thr)
    act = sorted(e for e in env if e > thr)
    d.plateau = _percentile(act, 50) if act else 0.0
    d.n_active_blocks = len(act)
    if act and sil_peak > 0.1 * d.plateau:
        raise AnalysisError(
            f"the leading {nz.silence_samples / FS:.1f} s is not silent (peak {_dbfs(sil_peak, cap.scale):.1f} dBFS against marker level {_dbfs(d.plateau, cap.scale):.1f} dBFS): start the "
            "recording at least 5 s before the probe, or lower --silence-s")
    for bi, b in enumerate(d.bursts):
        if b.kind == "short":
            d.short.append(bi)
            continue
        if b.kind == "long":
            d.long.append(bi)
            continue
        b.s_est = start_estimate(cap, nz, b, thr)
        rho, pos, c0, c1 = match_codes(cap, nz, b.s_est, [REF_CODE])[REF_CODE]
        if rho >= min_corr:
            d.refs.append((pos / FS, rho, c0, c1, bi))
        else:
            d.nonref.append(bi)
    return d


def _dbfs(v, scale):
    return 20.0 * math.log10(v / scale) if v > 0 else float("-inf")


# ---------------------------------------------------------------- joining the capture to the trials

def _linfit(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx <= 0:
        return 1.0, my - mx
    s = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return s, my - s * mx


def _near_trial(tcs, c, s, R, tol):
    """(trial index, R - predicted arrival) for the last trial whose predicted arrival is <= R + tol."""
    x = (R - c) / s
    k = bisect.bisect_right(tcs, x + tol / s) - 1
    if k < 0:
        return None, None
    return k, R - (c + s * tcs[k])


def _seed_offset(ref_t, tcs, lap_s, tol=0.0025):
    """Offset c with R = c + tc for the first reference markers. Every (first few refs, trial) pair proposes
    one; the proposal that explains most of the first 16 reference markers (as a trial's own marker or as a
    repeat some laps later) wins.

    The trials start on index changes, so their spacing is a whole number of block periods: a wrong shift by a
    trial or two still lands within a period of most markers. `tol` is therefore below half a block period
    (2.67 ms), and a proposal has to explain 70 % of the markers, or the capture is not this run's."""
    probe = ref_t[:16]
    best_score, best_c = 0, None
    for r0 in ref_t[:10]:
        for tc in tcs:
            c = r0 - tc
            score = 0
            for r in probe:
                k, d = _near_trial(tcs, c, 1.0, r, tol)
                if k is None:
                    continue
                if abs(d) < tol or any(abs(d - m * lap_s) < tol for m in range(1, 9)):
                    score += 1
            if score > best_score:
                best_score, best_c = score, c
    if best_c is None or best_score < max(4, 0.7 * len(probe)):
        raise AnalysisError(
            "the reference markers of the capture do not line up with the trial times of the CSV (best offset "
            f"explains {best_score} of the first {len(probe)} reference markers): wrong WAV for this run, or the "
            "capture starts after the run")
    return best_c


def _track(ref_t, tcs, c, s, lap_s, tol, adapt):
    """Assign reference-code bursts to trials in time order: the first one within `tol` of a trial's predicted
    arrival is its reference, later ones m laps after it are its repeats, anything else is a stray."""
    prim, reps, strays, pairs = {}, collections.defaultdict(list), [], []
    for ri, r in enumerate(ref_t):
        k, d = _near_trial(tcs, c, s, r, tol)
        if k is not None and abs(d) < tol:
            if k in prim:
                strays.append(ri)
                continue
            prim[k] = ri
            pairs.append((tcs[k], r))
            if adapt and len(pairs) >= 8 and len(pairs) % 4 == 0:
                s2, c2 = _linfit([p[0] for p in pairs[-30:]], [p[1] for p in pairs[-30:]])
                s, c = max(0.998, min(1.002, s2)), c2
                if s != s2:
                    c = statistics.fmean(p[1] - s * p[0] for p in pairs[-30:])
            continue
        if k is not None and k in prim:
            m = round(d / (lap_s * s))
            if 1 <= m <= 8 and abs(d - m * lap_s * s) < tol:
                reps[k].append(ri)
                continue
        strays.append(ri)
    return prim, reps, strays, pairs, c, s


def align_trials(ref_t, tcs, lap_s, tol=0.003):
    """Fit R = c + s * t_c (capture seconds against console timebase seconds) and assign the reference markers."""
    c0 = _seed_offset(ref_t, tcs, lap_s)
    _prim, _reps, _strays, pairs, c, s = _track(ref_t, tcs, c0, 1.0, lap_s, 0.006, True)
    if len(pairs) < 4:
        raise AnalysisError("fewer than 4 reference markers could be assigned to trials")
    keep = pairs
    for _ in range(4):
        s, c = _linfit([p[0] for p in keep], [p[1] for p in keep])
        res = [p[1] - (c + s * p[0]) for p in keep]
        sig = max(1.4826 * statistics.median(abs(r) for r in res), 0.00005)
        nxt = [p for p, r in zip(keep, res) if abs(r) < 3 * sig + 0.0002]
        if len(nxt) < 4 or len(nxt) == len(keep):
            break
        keep = nxt
    s, c = _linfit([p[0] for p in keep], [p[1] for p in keep])
    prim, reps, strays, pairs, c, s = _track(ref_t, tcs, c, s, lap_s, tol, False)
    res = [r - (c + s * tc) for tc, r in pairs]
    return {"c": c, "s": s, "prim": prim, "reps": reps, "strays": strays, "residuals_s": res}


# ---------------------------------------------------------------- logistic fit

def _softplus(z):
    return z + math.log1p(math.exp(-z)) if z > 0 else math.log1p(math.exp(z))


def _loglik(o, y, o50, s):
    inv = 1.0 / s
    ll = 0.0
    for oi, yi in zip(o, y):
        z = (oi - o50) * inv
        sp = _softplus(z)
        ll -= sp if yi else sp - z
    return ll


S_GRID = [0.02 * (150.0 ** (i / 15.0)) for i in range(16)]   # 0.02 .. 3 ms


def fit_logistic(o, y):
    """P(hit | o) = 1 / (1 + exp((o - o50) / s)), maximum likelihood by profile grid over (o50, s).

    A sharp step has no finite maximum (s -> 0): then o50 is the middle of the gap between the last hit and the
    first miss, which is what the data fixes, and the interval is the profile-likelihood region (drop of 1.92 in
    the log-likelihood), not a Hessian that does not exist. Width 10-90 % = 2 ln 9 s.
    """
    lo, hi = min(o) - 1.0, max(o) + 1.0
    coarse = [lo + 0.1 * i for i in range(int((hi - lo) / 0.1) + 1)]
    best = max((_loglik(o, y, o50, s), o50) for o50 in coarse for s in S_GRID)
    centre = best[1]
    fine = [centre - 1.0 + 0.01 * i for i in range(201)]
    prof, arg_s = [], []
    for o50 in fine:
        v, sv = max((_loglik(o, y, o50, s), s) for s in S_GRID)
        prof.append(v)
        arg_s.append(sv)
    top = max(prof)
    plateau = [f for f, v in zip(fine, prof) if v >= top - 0.05]
    o50 = 0.5 * (min(plateau) + max(plateau))
    k = min(range(len(fine)), key=lambda i: abs(fine[i] - o50))
    inside = [f for f, v in zip(fine, prof) if v >= top - 1.921]
    ci_lo, ci_hi = min(inside), max(inside)
    open_ended = ci_lo <= fine[0] + 1e-9 or ci_hi >= fine[-1] - 1e-9
    s_hat = arg_s[k]
    return {"o50_ms": o50, "s_ms": s_hat, "width_10_90_ms": 2.0 * math.log(9.0) * s_hat,
            "ci95_ms": [ci_lo, ci_hi], "ci_open": open_ended, "loglik": top}


def fit_j(points):
    """points: [(o_actual_ms, hit 0/1)] of one j. Returns a dict with status ok | all_hit | all_miss | no_transition."""
    n = len(points)
    hits = sum(h for _, h in points)
    res = {"n": n, "hits": hits, "misses": n - hits}
    if n == 0:
        res["status"] = "no_data"
        return res
    o = [p[0] for p in points]
    y = [p[1] for p in points]
    res["o_min_ms"], res["o_max_ms"] = min(o), max(o)
    if hits == 0:
        res["status"] = "all_miss"
        return res
    if hits == n:
        res["status"] = "all_hit"
        return res
    f = fit_logistic(o, y)
    res.update(f)
    inside = (res["o_min_ms"] - 0.25 <= f["o50_ms"] <= res["o_max_ms"] + 0.25
              and not f["ci_open"] and f["ci95_ms"][1] - f["ci95_ms"][0] <= 3.0)
    res["status"] = "ok" if inside and hits >= 3 and n - hits >= 3 else "no_transition"
    return res


# ---------------------------------------------------------------- analysis

def _stats(v):
    if not v:
        return {"n": 0}
    s = sorted(v)
    return {"n": len(s), "median": statistics.median(s), "mad": statistics.median(abs(x - statistics.median(s)) for x in s),
            "min": s[0], "max": s[-1]}


def _clear_x(run, trials, classes):
    """x_clear from the CSV's own clear events (no capture): ref block, and test block of hit and miss trials."""
    tbf = run.tbfreq
    n = run.n
    win = int((n + 3) * P_MS * 1e-3 * tbf)
    out = {"ref": [], "test_hit": [], "test_miss": []}

    def first_zero(blk, after):
        z = run.zeros.get(blk)
        if not z:
            return None
        i = bisect.bisect_right(z, after)
        return z[i] if i < len(z) and z[i] - after <= win else None

    for t, cls in zip(trials, classes):
        zr = first_zero(t["ref_blk"], t["t_ref"])
        if zr is not None:
            out["ref"].append((zr - t["t_c"]) * 1000.0 / tbf - (n // 2) * P_MS)
        if t["kind"] == 0 and cls in ("hit", "miss"):
            zt = first_zero(t["test_blk"], t["t_w"])
            if zt is not None:
                x = (zt - t["t_c"]) * 1000.0 / tbf - t["j"] * P_MS - (n * P_MS if cls == "miss" else 0.0)
                out["test_hit" if cls == "hit" else "test_miss"].append(x)
    return out


def analyze(run, cap, silence_s=3.0, min_corr=0.5, delta_tol_ms=0.5, first_occurrence=False,
            max_tc_gap_ms=0.02, per_trial=False):
    n = run.n
    tbf = run.tbfreq
    lap_s = n * P_MS * 1e-3
    warnings = []
    if not run.complete:
        warnings.append("CSV has no END row (truncated file)")
    if run.bad_lines:
        warnings.append(f"{run.bad_lines} unparsable CSV lines")
    ab = run.meta.get("abort_reason")
    if ab not in (None, "0", "3"):
        warnings.append(f"probe aborted early, abort_reason={ab}")
    if run.meta.get("ev_drop") not in (None, "0"):
        warnings.append("{} log records dropped".format(run.meta["ev_drop"]))
    if run.meta.get("ui_overlaps") not in (None, "0"):
        warnings.append("{} screen draws overlapped the measurement".format(run.meta["ui_overlaps"]))
    if cap.truncated:
        warnings.append(f"the WAV header declares {cap.header_frames / FS:.1f} s of audio but the file holds {cap.duration_s:.1f} s: the file is cut off")
    if cap.header_unset:
        warnings.append("the WAV header has no data size (recording not finalised); read to the end of the file")

    det = detect_markers(cap, silence_s, min_corr)
    started = [t for t in run.trials if t["t_c"] > 0]
    if not started:
        raise AnalysisError("the CSV has no started trial")
    if not det.bursts:
        raise AnalysisError(
            f"no marker detected: the capture holds no signal above the threshold ({_dbfs(det.thr, cap.scale):.1f} dBFS; noise floor {_dbfs(det.noise.rms, cap.scale):.1f} dBFS). "
            "Silence or a level that is far too low; check the cabling and the recorder gain with cfg/l2_c1.cfg")
    if not det.refs:
        raise AnalysisError(f"signal found ({len(det.bursts)} bursts) but none matches the reference marker code")

    # nominal arrival time of the reference marker as a function of the console timebase
    tc0 = started[0]["t_c"]
    tcs = [(t["t_c"] - tc0) / tbf for t in started]
    ref_t = [r[0] for r in det.refs]
    al = align_trials(ref_t, tcs, lap_s)
    c, s = al["c"], al["s"]
    drift_ppm = (s - 1.0) * 1e6
    if abs(drift_ppm) > DRIFT_WARN_PPM:
        warnings.append(f"recorder clock differs from the console timebase by {drift_ppm:.0f} ppm (more than {DRIFT_WARN_PPM:.0f}); "
                        "differences within 0.2 s are unaffected, check the recorder")
    prs = [c + s * tc for tc in tcs]
    res_ms = [r * 1000.0 for r in al["residuals_s"]]

    # reference strength: partial markers are judged against it
    ref_rho = sorted(det.refs[ri][1] for ri in al["prim"].values())
    ref_rho_med = statistics.median(ref_rho) if ref_rho else 1.0

    # test markers: each marker-shaped burst that is not the reference is tried against the codes of the
    # trials whose reference arrival lies within reach
    codes = {t["code"]: k for k, t in enumerate(started) if t["kind"] == 0 and t["code"] >= 0}
    before = 0.5 * lap_s + 2 * P_MS * 1e-3
    after = 6.5 * lap_s
    dets = collections.defaultdict(list)     # trial index -> [(pos_s, rho)]
    stray_bursts = []
    for bi in det.nonref:
        b = det.bursts[bi]
        a = b.s_est / FS
        lo = bisect.bisect_left(prs, a - after)
        hi = bisect.bisect_right(prs, a + before)
        cand = [started[k]["code"] for k in range(lo, hi) if started[k]["kind"] == 0 and started[k]["code"] >= 0]
        best = None
        if cand:
            m = match_codes(cap, det.noise, b.s_est, cand)
            cid = max(m, key=lambda x: m[x][0])
            if m[cid][0] >= min_corr:
                best = (cid, m[cid])
        if best is None:
            stray_bursts.append(a)
        else:
            dets[codes[best[0]]].append((best[1][1] / FS, best[1][0]))
    for k in dets:
        dets[k].sort()

    # detector false alarms: the reference burst of a reference-only trial against decoy test codes
    decoys = sorted(codes)[:: max(1, len(codes) // 4)][:4] if codes else []
    ref_only = [k for k, t in enumerate(started) if t["kind"] == 1]
    fa_decoy, fa_max = 0, 0.0
    for k in ref_only:
        ri = al["prim"].get(k)
        if ri is None or not decoys:
            continue
        b = det.bursts[det.refs[ri][4]]
        m = match_codes(cap, det.noise, b.s_est, decoys)
        top = max(v[0] for v in m.values())
        fa_max = max(fa_max, top)
        if top >= min_corr:
            fa_decoy += 1
    ro_measured = sum(1 for k in ref_only if k in al["prim"])
    extra_in_window = 0
    for k in ref_only:
        if any(prs[k] - 0.02 <= a <= prs[k] + after for a in stray_bursts):
            extra_in_window += 1

    # classification
    sig_n = lap_s * 1000.0 * s
    classes, details = [], []
    cover_end = cap.duration_s
    for k, t in enumerate(started):
        j = t["j"]
        info = {"trial": t["trial"], "j": j, "kind": t["kind"], "o_actual_ms": t["o_actual_ms"], "delta_ms": None,
                "repeats": 0, "rho_test": None}
        reach = prs[k] + ((n // 2 + max(j if t["kind"] == 0 else 0, 0) + 1) * P_MS * 1e-3) * s
        gap_ms = run.gaps.get(t["t_c"], 0) * 1000.0 / tbf
        info["tc_gap_ms"] = gap_ms
        if prs[k] - before < 0 or reach > cover_end:
            cls = "not_captured"
        elif not t["done"]:
            cls = "incomplete"
        elif k not in al["prim"]:
            cls = "no_ref"
        else:
            a_ref = det.refs[al["prim"][k]][0]
            info["ref_repeats"] = len(al["reps"].get(k, []))
            if t["kind"] == 1:
                cls = "ref_only_repeat" if info["ref_repeats"] else "ref_only"
            else:
                d = dets.get(k, [])
                if not d:
                    cls = "no_test"
                else:
                    first = d[0]
                    info["rho_test"] = first[1]
                    info["delta_ms"] = (first[0] - a_ref) * 1000.0
                    exp_hit = -(n / 2.0 - j) * P_MS * s
                    exp_miss = exp_hit + sig_n
                    info["exp_hit_ms"], info["exp_miss_ms"] = exp_hit, exp_miss
                    reps = len(d) - 1
                    info["repeats"] = reps
                    spacing_ok = all(abs((d[i + 1][0] - d[i][0]) * 1000.0 - sig_n) < 1.0 for i in range(reps))
                    if reps and spacing_ok:
                        cls = "no_clear"
                    elif reps:
                        cls = "multiple"
                    else:
                        cls = None
                    if cls is None or (cls == "no_clear" and first_occurrence):
                        if first[1] < PARTIAL_FRAC * ref_rho_med:
                            cls = "partial"
                        elif abs(info["delta_ms"] - exp_hit) <= delta_tol_ms:
                            cls = "hit"
                        elif abs(info["delta_ms"] - exp_miss) <= delta_tol_ms:
                            cls = "miss"
                        else:
                            cls = "wrong_delta"
                        if reps:
                            info["repeats"] = reps
                            info["first_occurrence"] = True
        classes.append(cls)
        details.append(info)

    counts = collections.Counter(classes)
    test_idx = [k for k, t in enumerate(started) if t["kind"] == 0]
    with_marker = [k for k in range(len(started)) if classes[k] not in ("not_captured", "incomplete", "no_ref")]
    repeating = [k for k in with_marker if details[k]["repeats"] or details[k].get("ref_repeats")]
    no_hw_clear = bool(with_marker) and len(repeating) >= 0.5 * len(with_marker)

    # fit
    excluded_gap = 0
    per_j_points = collections.defaultdict(list)
    for k in test_idx:
        cls = classes[k]
        d = details[k]
        if cls not in ("hit", "miss"):
            continue
        if d["tc_gap_ms"] > max_tc_gap_ms:
            excluded_gap += 1
            continue
        per_j_points[started[k]["j"]].append((started[k]["o_actual_ms"], 1 if cls == "hit" else 0))
    fits = {j: fit_j(pts) for j, pts in sorted(per_j_points.items())}
    for j, f in fits.items():
        if f["status"] == "ok":
            f["x_ms"] = f["o50_ms"] - j * P_MS
            f["x_ci95_ms"] = [f["ci95_ms"][0] - j * P_MS, f["ci95_ms"][1] - j * P_MS]
        elif f["status"] == "all_hit":
            f["x_bound"] = (">", f["o_max_ms"] - j * P_MS)
        elif f["status"] == "all_miss":
            f["x_bound"] = ("<", f["o_min_ms"] - j * P_MS)
    ok = [j for j, f in fits.items() if f["status"] == "ok"]
    x_latch = None
    agree = None
    if ok:
        w = []
        for j in ok:
            f = fits[j]
            sg = max((f["x_ci95_ms"][1] - f["x_ci95_ms"][0]) / (2 * 1.96), 0.02)
            w.append(1.0 / (sg * sg))
        xs = [fits[j]["x_ms"] for j in ok]
        x = sum(wi * xi for wi, xi in zip(w, xs)) / sum(w)
        half = 1.96 / math.sqrt(sum(w))
        spread = max(xs) - min(xs)
        x_latch = {"x_ms": x, "ci95_ms": [x - half, x + half], "js": ok, "spread_ms": spread}
        agree = {"checked": len(ok) > 1, "agree": spread <= AGREE_MS if len(ok) > 1 else None,
                 "spread_ms": spread, "limit_ms": AGREE_MS}
    lower = max((f["x_bound"][1] for f in fits.values() if f["status"] == "all_hit"), default=None)
    upper = min((f["x_bound"][1] for f in fits.values() if f["status"] == "all_miss"), default=None)
    bounds = {"x_above_ms": lower, "x_below_ms": upper, "consistent": True}
    if x_latch:
        slack = 0.25   # half the grid step: a bound is only as sharp as the o-grid
        bounds["consistent"] = ((lower is None or x_latch["x_ms"] > lower - slack)
                                and (upper is None or x_latch["x_ms"] < upper + slack))

    xc = _clear_x(run, started, classes)
    x_clear = _stats(xc["ref"])
    cls_x = None
    if x_latch:
        cls_x = an1.classify(x_latch["x_ms"])
    near = None
    if x_latch:
        near = min(an1.TABLE_COLUMNS, key=lambda col: abs(x_latch["x_ms"] - col[1]))[0]

    # hit-rate table
    table = {}
    for k in test_idx:
        if classes[k] in ("hit", "miss"):
            key = (started[k]["j"], round(started[k]["o_target_ms"], 4))
            h = table.setdefault(key, [0, 0])
            h[0] += classes[k] == "hit"
            h[1] += 1

    if counts.get("not_captured"):
        warnings.append("capture covers {} of {} trials: the audio ends or starts inside the run ({} trials have no "
                        "audio and are left out of every count and fit)".format(
                            len(started) - counts["not_captured"], len(started), counts["not_captured"]))
    if no_hw_clear:
        if first_occurrence:
            warnings.append("markers repeat every lap: the hardware does not clear consumed blocks. Repeating trials "
                            "are scored by their first arrival (--first-occurrence)")
        else:
            warnings.append("markers repeat every lap: the hardware does not clear consumed blocks. Trials that "
                            "repeat are not scored as hits (use --first-occurrence to score their first arrival)")
    n_measured = counts["hit"] + counts["miss"]
    n_test = len([k for k in test_idx if classes[k] not in ("not_captured", "incomplete")])
    if n_test and n_measured < 0.5 * n_test and not no_hw_clear:
        warnings.append(f"only {n_measured} of {n_test} test trials are hit or miss; see the class counts")
    if stray_bursts:
        warnings.append(f"{len(stray_bursts)} marker-shaped bursts match no trial (stray sound, or a test marker that matched no code)")
    if det.long:
        warnings.append(f"{len(det.long)} bursts longer than a marker (another sound in the capture)")
    peak_fs = det.plateau / cap.scale
    if peak_fs > 0.98:
        warnings.append(f"marker level {_dbfs(det.plateau, cap.scale):.1f} dBFS: the recorder clips")
    elif peak_fs < 0.03:
        warnings.append(f"marker level {_dbfs(det.plateau, cap.scale):.1f} dBFS is low (aim for about -6 dBFS): raise the recorder gain")
    snr = _dbfs(det.plateau, cap.scale) - _dbfs(det.noise.rms, cap.scale)
    if snr < 20:
        warnings.append(f"marker-to-noise ratio only {snr:.0f} dB")

    result = {
        "meta": {k: run.meta.get(k) for k in ("run", "blocks", "channels", "tbfreq", "tag", "l2_preset", "l2_jset",
                                              "l2_omin_ms", "l2_omax_ms", "l2_ostep_ms", "l2_reps", "l2_ref_only",
                                              "abort_reason", "ev_drop", "ui_overlaps", "build")},
        "header": {k: v for k, v in run.header.items()},
        "csv": {"trials": len(run.trials), "started": len(started), "complete": run.complete},
        "capture": {
            "duration_s": cap.duration_s, "bits": cap.bits, "channels": cap.nch, "truncated": cap.truncated,
            "header_unset": cap.header_unset, "noise_rms_dbfs": _dbfs(det.noise.rms, cap.scale),
            "threshold_dbfs": _dbfs(det.thr, cap.scale), "marker_level_dbfs": _dbfs(det.plateau, cap.scale),
            "marker_to_noise_db": snr, "silence_s": det.noise.silence_samples / FS,
            "silence_peak_dbfs": _dbfs(det.sil_peak, cap.scale), "dc": [d / cap.scale for d in det.noise.dc],
            "bursts": len(det.bursts), "short_bursts": len(det.short), "long_bursts": len(det.long)},
        "sync": {"drift_ppm": drift_ppm, "offset_s": c, "pairs": len(al["residuals_s"]),
                 "residual_ms": _stats(res_ms),
                 "residual_std_ms": statistics.pstdev(res_ms) if len(res_ms) > 1 else 0.0},
        "detection": {
            "reference_markers": len(det.refs), "reference_assigned": len(al["prim"]),
            "reference_repeats": sum(len(v) for v in al["reps"].values()),
            "reference_strays": len(al["strays"]), "test_markers": sum(len(v) for v in dets.values()),
            "stray_bursts": len(stray_bursts), "ref_rho_median": ref_rho_med,
            "rho_channels_median": [statistics.median(r[2] for r in det.refs), statistics.median(r[3] for r in det.refs)],
            "min_corr": min_corr, "decoys": decoys,
            "false_alarm": {"ref_only_trials": ro_measured, "decoy_matches": fa_decoy, "decoy_rho_max": fa_max,
                            "ref_only_with_extra_burst": extra_in_window}},
        "classes": dict(counts), "no_hardware_clear": no_hw_clear, "repeating_trials": len(repeating),
        "excluded_tc_gap": excluded_gap,
        "table": {f"{j}|{o}": v for (j, o), v in sorted(table.items())},
        "fits": {str(j): f for j, f in fits.items()},
        "x_latch": x_latch, "agreement": agree, "bounds": bounds,
        "x_clear": {"ref_ms": x_clear, "test_hit_ms": _stats(xc["test_hit"]), "test_miss_ms": _stats(xc["test_miss"]),
                    "hw_other_words": run.hw_other_words},
        "class": cls_x, "class_text": an1.CLASS_TEXT.get(cls_x) if cls_x else None, "nearest_table_column": near,
        "warnings": warnings, "first_occurrence": first_occurrence,
    }
    if x_latch and x_clear.get("n"):
        result["x_clear_vs_latch_ms"] = x_clear["median"] - x_latch["x_ms"]
    delta_hit = [d["delta_ms"] - d["exp_hit_ms"] for k, d in enumerate(details) if classes[k] == "hit"]
    delta_miss = [d["delta_ms"] - d["exp_miss_ms"] for k, d in enumerate(details) if classes[k] == "miss"]
    result["delta_error_ms"] = {"hit": _stats(delta_hit), "miss": _stats(delta_miss)}
    expect = {"c1": "hit", "c3": "miss"}.get(run.meta.get("l2_preset"))
    if expect:
        scored = [classes[k] for k in test_idx if classes[k] in ("hit", "miss")]
        result["control"] = {"preset": run.meta["l2_preset"], "expected": expect, "scored": len(scored),
                             "as_expected": sum(1 for c in scored if c == expect),
                             "other_classes": {c: counts[c] for c in ("no_test", "partial", "wrong_delta", "no_clear",
                                                                       "multiple") if counts.get(c)}}
    if per_trial:
        result["trials"] = [dict(d, cls=cl) for d, cl in zip(details, classes)]
    return result


# ---------------------------------------------------------------- control C2

def analyze_c2(run, cap, silence_s=3.0, min_corr=0.5):
    """Control C2: two markers sat in blocks 0 and 1 before audioPortStart. Which one is heard first, and what
    the hardware zeroes first; the capture's arrival difference against the CSV's clear events."""
    det = detect_markers(cap, silence_s, min_corr)
    if not det.bursts:
        raise AnalysisError("no marker detected in the capture of control C2")
    found = {}
    for bi, b in enumerate(det.bursts):
        if b.kind != "marker":
            continue
        b.s_est = start_estimate(cap, det.noise, b, det.thr)
        m = match_codes(cap, det.noise, b.s_est, C2_CODES)
        cid = max(m, key=lambda x: m[x][0])
        if m[cid][0] >= min_corr:
            found.setdefault(cid, []).append(m[cid][1] / FS)
    tbf = run.tbfreq
    z = {blk: (run.zeros[blk][0] if run.zeros.get(blk) else None) for blk in (0, 1)}
    res = {"codes_found": {str(k): v for k, v in found.items()},
           "first_arrival_s": {str(k): v[0] for k, v in found.items()},
           "zero_tb": z, "idx_after_start": run.meta.get("idx_after_start"),
           "blocks": run.n, "tbfreq": tbf,
           "capture": {"duration_s": cap.duration_s, "marker_level_dbfs": _dbfs(det.plateau, cap.scale),
                       "noise_rms_dbfs": _dbfs(det.noise.rms, cap.scale)}}
    if all(c in found for c in C2_CODES):
        res["arrival_1002_minus_1001_ms"] = (found[C2_CODES[1]][0] - found[C2_CODES[0]][0]) * 1000.0
        res["heard_first"] = "block 0 (code 1001)" if found[C2_CODES[0]][0] < found[C2_CODES[1]][0] else "block 1 (code 1002)"
    if z[0] is not None and z[1] is not None:
        res["zero_block1_minus_block0_ms"] = (z[1] - z[0]) * 1000.0 / tbf
    res["repeats_per_code"] = {str(k): len(v) for k, v in found.items()}
    return res


def report_c2(res, run):
    L = ["== ring110 Layer 2 control C2: markers in blocks 0 and 1 before audioPortStart, run {}, n={} blocks ==".format(
        run.meta.get("run"), run.n)]
    L.append("capture {:.1f} s, marker level {:.1f} dBFS, noise {:.1f} dBFS".format(
        res["capture"]["duration_s"], res["capture"]["marker_level_dbfs"], res["capture"]["noise_rms_dbfs"]))
    for c in C2_CODES:
        v = res["codes_found"].get(str(c))
        L.append("code {} (block {}): {}".format(c, c - 1001, "not found" if not v else
                 f"{len(v)} arrival(s), first at {v[0]:.4f} s"))
    if "heard_first" in res:
        L.append("heard first: {}; arrival(1002) - arrival(1001) = {:.3f} ms (one block = {:.3f} ms)".format(
            res["heard_first"], res["arrival_1002_minus_1001_ms"], P_MS))
    if "zero_block1_minus_block0_ms" in res:
        L.append("CSV: hardware zeroed block 1 {:.3f} ms after block 0; index after audioPortStart {}".format(
            res["zero_block1_minus_block0_ms"], res["idx_after_start"]))
    if any(len(v) > 1 for v in res["codes_found"].values()):
        L.append("a code arrived more than once: the hardware does not clear consumed blocks")
    return "\n".join(L)


# ---------------------------------------------------------------- report

def _ms(v, d=3):
    return "n/a" if v is None else f"{v:.{d}f}"


def report(res):
    L = []
    m = res["meta"]
    cp, sy, dt = res["capture"], res["sync"], res["detection"]
    L.append("== ring110 Layer 2: run {}, n={} blocks, {} ch, tbfreq {} Hz, tag '{}' ==".format(
        m["run"], m["blocks"], m["channels"], m["tbfreq"], m["tag"] or ""))
    for w in res["warnings"]:
        L.append("WARNING: " + w)
    h = res["header"]
    L.append("probe {} {} ({}, source {}), config {}, console {} fw {} {} [{}]".format(
        h["probe"], h["probe_version"], h["git_describe"], h["source_sha256"][:12], h["config"], h["console_model"],
        h["fw_version"], h["fw_type"], h["console_info_source"]))
    L.append("grid: preset {}, j {}, o {} .. {} ms step {} ms, {} reps, {} reference-only; {} of {} CSV trials started".format(
        m["l2_preset"], m["l2_jset"], m["l2_omin_ms"], m["l2_omax_ms"], m["l2_ostep_ms"], m["l2_reps"], m["l2_ref_only"],
        res["csv"]["started"], res["csv"]["trials"]))
    L.append("")
    L.append("Capture: {:.1f} s, {} bit, {} ch. Noise floor {:.1f} dBFS (leading {:.1f} s, peak {:.1f} dBFS), "
             "threshold {:.1f} dBFS, marker level {:.1f} dBFS, marker-to-noise {:.0f} dB".format(
                 cp["duration_s"], cp["bits"], cp["channels"], cp["noise_rms_dbfs"], cp["silence_s"],
                 cp["silence_peak_dbfs"], cp["threshold_dbfs"], cp["marker_level_dbfs"], cp["marker_to_noise_db"]))
    L.append("   bursts {} (short {}, long {}); DC {}".format(cp["bursts"], cp["short_bursts"], cp["long_bursts"],
                                                           ", ".join(f"{d:+.4f}" for d in cp["dc"])))
    L.append("Recorder clock against the console timebase: drift {:+.1f} ppm (from {} reference markers), arrival "
             "residual {:.3f} ms rms (latch jitter plus capture noise; the analysis uses only differences)".format(
                 sy["drift_ppm"], sy["pairs"], sy["residual_std_ms"]))
    L.append("Detection (matched filter, cosine >= {}): reference markers {} ({} assigned, {} repeats, {} strays), test "
             "markers {}, unmatched bursts {}; reference cosine median {:.2f}, per channel {:+.2f} / {:+.2f}".format(
                 dt["min_corr"], dt["reference_markers"], dt["reference_assigned"], dt["reference_repeats"],
                 dt["reference_strays"], dt["test_markers"], dt["stray_bursts"], dt["ref_rho_median"],
                 dt["rho_channels_median"][0], dt["rho_channels_median"][1]))
    fa = dt["false_alarm"]
    if fa["ref_only_trials"]:
        ub = 100.0 * (3.0 / fa["ref_only_trials"]) if fa["decoy_matches"] == 0 else None
        L.append("False alarms on {} reference-only trials: decoy test codes matched {} times (max cosine {:.2f}{}); "
                 "an extra burst in the window of {} of them".format(
                     fa["ref_only_trials"], fa["decoy_matches"], fa["decoy_rho_max"],
                     "" if ub is None else f", 95% upper bound {ub:.1f}%", fa["ref_only_with_extra_burst"]))
    L.append("")
    order = ("hit", "miss", "no_clear", "multiple", "partial", "wrong_delta", "no_test", "no_ref", "not_captured",
             "incomplete", "ref_only", "ref_only_repeat")
    L.append("Trial classes: " + ", ".join("{} {}".format(k, res["classes"][k]) for k in order if res["classes"].get(k)))
    if res["excluded_tc_gap"]:
        L.append("   {} hit/miss trials left out of the fit: a polling gap of more than 20 us at t_c".format(
            res["excluded_tc_gap"]))
    de = res["delta_error_ms"]
    if de["hit"]["n"]:
        L.append("   Delta - expected, hits: median {:+.3f} ms, MAD {:.3f} ms; misses: {}".format(
            de["hit"]["median"], de["hit"]["mad"],
            "none" if not de["miss"]["n"] else "median {:+.3f} ms, MAD {:.3f} ms".format(
                de["miss"]["median"], de["miss"]["mad"])))
    L.append("Hardware clear of consumed blocks: {}".format(
        "NO: markers repeat every lap" if res["no_hardware_clear"] else "yes (markers play once)"))
    ctl = res.get("control")
    if ctl:
        L.append("Control {} (every write {}): {} of {} scored trials were a {}{}{}".format(
            ctl["preset"].upper(), "in time" if ctl["expected"] == "hit" else "late", ctl["as_expected"], ctl["scored"],
            ctl["expected"], "" if ctl["as_expected"] == ctl["scored"] else " -- NOT as expected, investigate",
            "; unscored: " + ", ".join(f"{k} {v}" for k, v in ctl["other_classes"].items()) if ctl["other_classes"] else ""))
    xcl = res["x_clear"]
    for key, label in (("test_hit_ms", "test blocks of hits"), ("test_miss_ms", "test blocks of misses")):
        if xcl[key].get("n"):
            L.append("   x_clear from the {}: {:.3f} ms, MAD {:.3f} ms, n={}".format(
                label, xcl[key]["median"], xcl[key]["mad"], xcl[key]["n"]))
    if res["table"]:
        L.append("Hit rate by j and target o (hits/trials):")
        by_j = collections.defaultdict(list)
        for key, (hh, nn) in res["table"].items():
            j, o = key.split("|")
            by_j[int(j)].append((float(o), hh, nn))
        for j in sorted(by_j):
            L.append(f"   j={j:+d}: " + " ".join(f"{o:g}:{hh}/{nn}" for o, hh, nn in sorted(by_j[j])))
    L.append("")
    L.append("Logistic fit of P(hit) against o_actual per j (o50 = 50 % point, width = 10-90 %, x = o50 - j*P):")
    for j in sorted(res["fits"], key=int):
        f = res["fits"][j]
        jj = int(j)
        if f["status"] == "ok":
            step = f["s_ms"] <= S_GRID[0] * 1.001
            L.append("   j={:+d}: n={} ({} hit)  o50 = {:.3f} ms [{:.3f}, {:.3f}], width {}{:.3f} ms{}  ->  x = {:.3f} ms [{:.3f}, {:.3f}]".format(
                jj, f["n"], f["hits"], f["o50_ms"], f["ci95_ms"][0], f["ci95_ms"][1], "<= " if step else "",
                f["width_10_90_ms"], " (a step: the o grid is coarser than the jitter)" if step else "", f["x_ms"],
                f["x_ci95_ms"][0], f["x_ci95_ms"][1]))
        elif f["status"] in ("all_hit", "all_miss"):
            op, val = f["x_bound"]
            L.append("   j={:+d}: n={} ({} hit): every write {} -> o* is {} the swept range, x {} {:.3f} ms".format(
                jj, f["n"], f["hits"], "was in time" if f["status"] == "all_hit" else "was late",
                "above" if f["status"] == "all_hit" else "below", op, val))
        else:
            L.append("   j={:+d}: n={} ({} hit): no usable transition in the swept range (status {})".format(
                jj, f["n"], f["hits"], f["status"]))
    L.append("")
    xl = res["x_latch"]
    xc = res["x_clear"]["ref_ms"]
    if xl:
        L.append("x_latch = {:.3f} ms [95% CI {:.3f}, {:.3f}] from j = {}".format(
            xl["x_ms"], xl["ci95_ms"][0], xl["ci95_ms"][1], ", ".join(f"{j:+d}" for j in xl["js"])))
        ag = res["agreement"]
        if ag["checked"]:
            L.append("   per-j estimates {}: spread {:.3f} ms (limit {:.1f} ms)".format(
                "agree" if ag["agree"] else "DISAGREE", ag["spread_ms"], ag["limit_ms"]))
        else:
            L.append("   one j only: no consistency check between j")
        bd = res["bounds"]
        if bd["x_above_ms"] is not None or bd["x_below_ms"] is not None:
            L.append("   the j without a transition bound x to ({}, {}) ms: {}".format(
                _ms(bd["x_above_ms"]), _ms(bd["x_below_ms"]),
                "consistent" if bd["consistent"] else "INCONSISTENT with x_latch"))
    else:
        L.append("x_latch = n/a (no j has a transition inside the swept range)")
        bd = res["bounds"]
        if bd["x_above_ms"] is not None or bd["x_below_ms"] is not None:
            L.append("   the j without a transition bound x to ({}, {}) ms".format(
                _ms(bd["x_above_ms"]), _ms(bd["x_below_ms"])))
    if xc.get("n"):
        txt = "x_clear (this run's clear events, reference blocks) = {:.3f} ms, MAD {:.3f} ms, n={}".format(
            xc["median"], xc["mad"], xc["n"])
        if "x_clear_vs_latch_ms" in res:
            d = res["x_clear_vs_latch_ms"]
            txt += "; x_clear - x_latch = {:+.3f} ms ({})".format(
                d, "within 0.7 ms" if abs(d) < an1.TOL_MS else "OUTSIDE the 0.7 ms consistency check of section 6")
        L.append(txt)
    if res["class"]:
        L.append("CLASS (section 6, from x_latch): {}".format(res["class"]))
        L.append("   " + res["class_text"])
        if res["nearest_table_column"]:
            L.append("   nearest table column: {}".format(res["nearest_table_column"]))
    L.append("hw clear = {}".format("no" if res["no_hardware_clear"] else "yes"))
    return "\n".join(L)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("csv", help="ring110 Layer 2 result CSV")
    ap.add_argument("wav", help="capture of the console's output, 48 kHz PCM")
    ap.add_argument("--json", action="store_true", help="print JSON instead of text")
    ap.add_argument("--silence-s", type=float, default=3.0,
                    help="leading silence the noise floor and threshold are measured on (default 3)")
    ap.add_argument("--min-corr", type=float, default=0.5, help="matched-filter cosine that counts as a detection (default 0.5)")
    ap.add_argument("--delta-tol-ms", type=float, default=0.5, help="tolerance of Delta around the hit and miss values (default 0.5)")
    ap.add_argument("--max-tc-gap-ms", type=float, default=0.02,
                    help="leave out trials whose t_c iteration had a polling gap above this (default 0.02 = 20 us)")
    ap.add_argument("--first-occurrence", action="store_true",
                    help="when markers repeat (no hardware clear) score the first arrival instead of leaving the trial out")
    ap.add_argument("--per-trial", action="store_true", help="add the per-trial table to the JSON")
    args = ap.parse_args(argv)
    try:
        run = parse_file(args.csv)
        cap = read_wav(args.wav)
        if run.meta.get("l2_c2") == "1":
            res = analyze_c2(run, cap, args.silence_s, args.min_corr)
            text = report_c2(res, run)
        else:
            res = analyze(run, cap, args.silence_s, args.min_corr, args.delta_tol_ms, args.first_occurrence,
                          args.max_tc_gap_ms, args.per_trial)
            text = report(res)
    except (hwprobe_format.FormatError, AnalysisError, WavError, OSError) as e:
        print(f"{args.csv} {args.wav}: {e}", file=sys.stderr)
        return 2
    res["file"] = args.csv
    res["wav"] = args.wav
    if args.json:
        json.dump(res, sys.stdout, indent=1, default=str)
        print()
    else:
        print(f"# {args.csv} + {args.wav}")
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
