#!/usr/bin/env python3
"""Synthetic Layer 2 run (CSV + WAV) for testing analyze_layer2.py. No hardware involved.

SYNTHETIC data only: it encodes the PROBE.md section 1/2 model, not a measurement. The block named by the new
index, r, is latched for output at  T_r + x_ms + J  (T_r the true index change, J ~ N(0, jitter_ms) per trial,
common to every block of the trial); block r+k latches k*P later. A write that finishes before the latch is
heard in this lap (hit), a later one one lap (n*P) later (miss). The reference marker, in block r + n/2, is
always in time. The capture hears a block `capture_delay_ms` after its latch, on a recorder clock that is
off by `drift_ppm` against the console's, through a one-pole D/A smear, with noise and DC.

    python3 synth_layer2.py out/run --x-ms 1.0 --drift-ppm 80      # writes out/run.csv and out/run.wav

Faults (flags): --no-clear (the hardware never clears, so a marker repeats every lap until the guest zeroes it),
--drop-write P (a test write the hardware never plays), --partial-write P (a test marker the latch cut to 72
samples), --swap-channels (marker on the left channel only,
captured on the right), --dc OFFSET, --truncate-s T (the WAV file is cut after T seconds, its header intact),
--version N (header format version), --gap-prob P (polling gaps at t_c).
"""
import argparse
import array
import math
import random
import struct
import sys

P_MS = 256.0 / 48000.0 * 1000.0
FS = 48000
MARK_START = 32
MARK_LEN = 128
_TAB = 1 << 16
PARTIAL_LEN = 72   # samples of a marker the latch cut short: three envelope blocks, correlation about 0.75 of a whole one

try:
    from pn import pn_signs
except ImportError:  # imported from another directory without the probe on sys.path
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from pn import pn_signs


def _noise_table(rng, sigma_units, dc_units):
    tab = array.array("i")
    k = sigma_units / 0.40825   # (u1 + u2 - 1), u uniform on [0, 1), has sigma 0.40825
    for _ in range(_TAB):
        tab.append(round((rng.random() + rng.random() - 1.0) * k + dc_units))
    return tab


def _channel_base(rng, n_frames, sigma_units, dc_units):
    tab = _noise_table(rng, sigma_units, dc_units)
    reps = n_frames // _TAB + 1
    return (tab * reps)[:n_frames]


def smear_kernel(smear_samples):
    """One-pole low-pass, DC gain 1, truncated at 8 taps; smear_samples = 0 is the identity."""
    if smear_samples <= 0:
        return [1.0]
    a = math.exp(-1.0 / smear_samples)
    h = [(1.0 - a) * a ** k for k in range(8)]
    s = sum(h)
    return [v / s for v in h]


def write_wav(path_or_none, chans, bits=24, extensible=None, declared_frames=None, truncate_frames=None):
    """Interleave int arrays into a PCM WAV. extensible defaults to what ffmpeg writes (24 bit: yes).
    declared_frames overrides the data size in the header, truncate_frames cuts the file after that many frames."""
    nch = len(chans)
    n = len(chans[0])
    if extensible is None:
        extensible = bits > 16
    inter = array.array("i", [0]) * (n * nch)
    for c, ch in enumerate(chans):
        inter[c::nch] = ch
    if bits == 16:
        a16 = array.array("h", inter)
        if sys.byteorder == "big":
            a16.byteswap()
        raw = a16.tobytes()
    else:
        if sys.byteorder == "big":
            inter.byteswap()
        b = inter.tobytes()
        if bits == 32:
            raw = b
        else:
            raw = bytearray(3 * len(inter))
            raw[0::3] = b[0::4]
            raw[1::3] = b[1::4]
            raw[2::3] = b[2::4]
            raw = bytes(raw)
    align = nch * bits // 8
    if truncate_frames is not None:
        raw = raw[:truncate_frames * align]
    declared = (declared_frames if declared_frames is not None else n) * align
    if extensible:
        fmt = struct.pack("<HHIIHHHHIH14s", 0xFFFE, nch, FS, FS * align, align, bits, 22, bits, 3, 1,
                          b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71")
    else:
        fmt = struct.pack("<HHIIHH", 1, nch, FS, FS * align, align, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", declared) + raw
    data = b"RIFF" + struct.pack("<I", len(body)) + body
    if path_or_none:
        with open(path_or_none, "wb") as fh:
            fh.write(data)
    return data


def silence_wav(seconds, noise_dbfs=-70.0, bits=24, seed=7, path=None):
    """A capture with no marker in it."""
    rng = random.Random(seed)
    fsu = 2 ** (bits - 1) - 1
    n = int(seconds * FS)
    sig = 10 ** (noise_dbfs / 20.0) * fsu if noise_dbfs is not None else 0.0
    return write_wav(path, [_channel_base(rng, n, sig, 0.0), _channel_base(rng, n, sig, 0.0)], bits)


def synth_run(*, x_ms=1.0, x_clear_ms=None, jitter_ms=0.03, n=8, js=(-1, 0, 1, 2), o_min=0.0, o_max=12.0,
              o_step=0.5, grids=None, reps=10, ref_only=200, seed=1, capture_delay_ms=37.0, drift_ppm=0.0,
              smear=1.0, noise_dbfs=-60.0, gain=0.8, bits=24, lead_s=5.0, tail_s=5.0, hw_clear=True,
              drop_write=0.0, partial_write=0.0, swap_channels=False, dc=0.0, format_version=1, tbfreq=79_800_000,
              gap_prob=0.0, preset="main", shift_prob=0.0, shift_ms=2.0,
              discard=100, quiet_laps=4, truncate_s=None, header_declares_full=True, extra_markers=(),
              wav_path=None):
    """Return (csv_text, wav_bytes, truth). truth: trials, hits, misses, x_ms, markers.

    grids maps j to its own (o_min, o_max, o_step), for the finer second pass around each j's threshold."""
    rng = random.Random(seed)
    rng_audio = random.Random(seed + 1000)
    if x_clear_ms is None:
        x_clear_ms = x_ms
    P = P_MS
    TB0 = 5_000_000_000

    def ms2tb(ms):
        return round(ms * 1e-3 * tbfreq)

    def steps_of(lo, hi, st):
        return int((hi - lo) / st + 1e-6) + 1 if st > 0 and hi > lo else 1

    grids = grids or {}
    plan = []
    for j in js:
        lo, hi, st = grids.get(j, (o_min, o_max, o_step))
        plan += [(j, lo + k * st, 0) for k in range(steps_of(lo, hi, st)) for _ in range(reps)]
    plan += [(99, -1.0, 1)] * ref_only
    rng.shuffle(plan)
    quiet_ms = (quiet_laps * n + 1) * P

    def tick_t(i):
        return 25.0 + i * P

    ev = []        # (tb, kind, idx, blk, a, b)
    trials = []
    markers = []   # (console_latch_ms, code, which) which: "ref" or "test"
    partial_codes = set()
    cur = tick_t(discard)
    hits = misses = 0
    for k, (j, o, kind) in enumerate(plan):
        i_c = int((cur - 25.0) / P) + 1
        T_r = tick_t(i_c)
        d = rng.uniform(0.0, 0.002)
        gap = 0.0
        if gap_prob and rng.random() < gap_prob:
            gap = rng.uniform(0.05, 0.3)
            d = gap
        t_c = T_r + d
        r = i_c % n
        jit = rng.gauss(0.0, jitter_ms)
        ref_blk = (r + n // 2) % n
        t_ref = t_c + 0.004
        lat_ref = T_r + x_ms + (n // 2) * P + jit
        markers.append((lat_ref, 0, "ref"))
        ev.append((TB0 + ms2tb(t_ref), 4, r, ref_blk, 0, 0))
        if hw_clear:
            ev.append((TB0 + ms2tb(T_r + x_clear_ms + (n // 2) * P + jit + rng.uniform(0.0, 0.0015)), 1, r, ref_blk, 0, 0))
        if gap:
            ev.append((TB0 + ms2tb(t_c), 3, r, 0, ms2tb(gap), 0))
        code, test_blk, t_w, o_act = -1, -1, 0.0, -1.0
        played_test = None   # console latch time of the test marker's first play
        if kind == 0:
            code = k + 1
            test_blk = (r + j) % n
            t_w = t_c + o + rng.uniform(0.003, 0.03)
            lat_t = T_r + x_ms + j * P + jit
            ev.append((TB0 + ms2tb(t_w), 4, r, test_blk, code, 1))
            if t_w < lat_t:
                hit = True
                hits += 1
            else:
                hit = False
                misses += 1
                lat_t += n * P
            if rng.random() >= drop_write:
                if shift_prob and rng.random() < shift_prob:
                    lat_t += shift_ms   # a marker that plays neither in this lap nor in the next
                played_test = lat_t
                if partial_write and rng.random() < partial_write:
                    partial_codes.add(code)
                markers.append((lat_t, code, "test"))
                if hw_clear:
                    z = T_r + x_clear_ms + j * P + (0.0 if hit else n * P) + jit
                    ev.append((TB0 + ms2tb(z + rng.uniform(0.0, 0.0015)), 1, r, test_blk, 0, 0))
            o_act = (ms2tb(t_w) - ms2tb(t_c)) * 1000.0 / tbfreq
        t_end = (t_w if kind == 0 else t_ref) + quiet_ms
        if not hw_clear:
            # nothing clears the blocks: each marker is replayed every lap until the guest zeroes them at t_end
            for first, cd, which in ((lat_ref, 0, "ref"), (played_test, code, "test")):
                if first is None:
                    continue
                m = 1
                while first + m * n * P <= t_end:
                    markers.append((first + m * n * P, cd, which))
                    m += 1
            ev.append((TB0 + ms2tb(t_end), 5, r, ref_blk, 0, 0))
            if kind == 0:
                ev.append((TB0 + ms2tb(t_end), 5, r, test_blk, 0, 0))
        trials.append((k, j, o, o_act, TB0 + ms2tb(t_c), TB0 + ms2tb(t_w) if kind == 0 else 0, r, code, test_blk,
                       ref_blk, TB0 + ms2tb(t_ref), kind))
        cur = t_end + 0.01 + rng.uniform(0.0, P)
    last_tick = int((cur - 25.0) / P) + 1
    for i in range(1, last_tick + 1):
        ev.append((TB0 + ms2tb(tick_t(i)), 0, i % n, 0, 0, 0))
    ev.sort(key=lambda e: (e[0], e[1]))

    out = [f"HWPROBE,{format_version}"]
    for key, v in (("probe", "ring110"), ("probe_version", "1"), ("git_describe", "synthetic"),
                   ("source_sha256", "0" * 64), ("fw_version", "synthetic"), ("fw_type", "synthetic"),
                   ("console_model", "synthetic"), ("console_info_source", "operator"), ("config", "synthetic"),
                   ("tbfreq", tbfreq)):
        out.append(f"HDR,{key},{v}")
    meta = {
        "version": "ring110-probe 1 (synthetic)", "build": "synthetic", "layer": "L2", "run": 1, "tbfreq": tbfreq,
        "blocks": n, "channels": 2, "abort_reason": 0, "nchg": last_tick, "ev_drop": 0, "discard": discard,
        "ticks": 0, "q4_every": 0, "refill": 0, "seed": seed, "tag": "synthetic", "watch_word": MARK_START * 2,
        "sentinel_mode": 0, "ui_enabled": 1, "ui_overlaps": 0, "l2_ntrials": len(plan),
        "l2_grid_steps": steps_of(o_min, o_max, o_step),
        "l2_njset": len(js), "l2_jset": ";".join(str(j) for j in js), "l2_omin_ms": f"{o_min:.4f}",
        "l2_omax_ms": f"{o_max:.4f}", "l2_ostep_ms": f"{o_step:.4f}", "l2_reps": reps, "l2_ref_only": ref_only,
        "l2_quiet_laps": quiet_laps, "l2_preset": preset, "l2_c2": 0, "l2_c2_codes": "1001;1002",
        "l2_marker_start_sample": MARK_START, "l2_marker_len": MARK_LEN, "l2_marker_channels": "0;1",
        "l2_ref_code": 0, "l2_trials_done": len(plan)}
    for key, v in meta.items():
        out.append(f"META,{key},{v}")
    for b in range(n):
        out.append(f"FINALWORD,{b},0x00000000")
    out.append(f"SYNC,{TB0},1000000000,{TB0 + 40}")
    end_tb = ev[-1][0] + ms2tb(50)
    out.append(f"SYNC,{end_tb},{1000000000 + round((end_tb - TB0) / tbfreq * 1e6)},{end_tb + 40}")
    for tb, kind, idx, blk, a, b in ev:
        out.append(f"EV,{tb},{kind},{idx},0,{blk},0x{a:08x},0x{b:08x},0x00000000,0x00000000")
    for k, j, o, oa, tc, tw, r, code, tblk, rblk, tref, kind in trials:
        out.append(f"TRIAL,1,{k},{j},{o:.4f},{oa:.4f},{tc},{tw},{r},{code},{tblk},{rblk},{tref},{kind},1")
    out.append(f"END,{len(ev)},0")
    csv = "\n".join(out) + "\n"

    # ---- audio
    end_ms = cur
    dur_s = lead_s + end_ms * 1e-3 * (1.0 + drift_ppm * 1e-6) + capture_delay_ms * 1e-3 + tail_s
    n_frames = int(dur_s * FS)
    fsu = 2 ** (bits - 1) - 1
    sigma = 10 ** (noise_dbfs / 20.0) * fsu if noise_dbfs is not None else 0.0
    dc_units = dc * fsu
    chans = [_channel_base(rng_audio, n_frames, sigma, dc_units), _channel_base(rng_audio, n_frames, sigma, dc_units)]
    kern = smear_kernel(smear)
    amp = 0.5 * gain * fsu
    cache = {}
    nmark = 0
    placed = [(lead_s * 1000.0 + lat * (1.0 + drift_ppm * 1e-6) + capture_delay_ms + MARK_START / FS * 1000.0, code)
              for lat, code, _which in markers] + list(extra_markers)
    for cap_ms, code in placed:
        s0 = round(cap_ms * 1e-3 * FS)
        sig = cache.get(code)
        if sig is None:
            chips = [v * amp for v in pn_signs(code)][:PARTIAL_LEN if code in partial_codes else MARK_LEN]
            sig = [sum(kern[q] * chips[i - q] for q in range(len(kern)) if 0 <= i - q < len(chips))
                   for i in range(len(chips) + len(kern) - 1)]
            cache[code] = sig
        if s0 < 0 or s0 + len(sig) > n_frames:
            continue
        nmark += 1
        for c in ((0,) if swap_channels else (0, 1)):
            ch = chans[c]
            for i, v in enumerate(sig):
                ch[s0 + i] += round(v)
    if swap_channels:
        chans.reverse()
    lim = fsu
    for ch in chans:
        for i, v in enumerate(ch):
            if v > lim or v < -lim - 1:
                ch[i] = max(-lim - 1, min(lim, v))
    cut = None if truncate_s is None else int(truncate_s * FS)
    declared = n_frames if header_declares_full or cut is None else cut
    wav = write_wav(wav_path, chans, bits, declared_frames=declared, truncate_frames=cut)
    truth = {"trials": len(plan), "hits": hits, "misses": misses, "x_ms": x_ms, "markers": nmark,
             "duration_s": dur_s, "last_trial_end_s": lead_s + end_ms * 1e-3}
    return csv, wav, truth


def synth_c2(*, x_ms=1.0, n=8, noise_dbfs=-60.0, gain=0.8, bits=24, lead_s=5.0, tail_s=2.0, seed=3, hw_clear=True,
             tbfreq=79_800_000):
    """Control C2: code 1001 in block 0, code 1002 in block 1 before audioPortStart; block 0 is heard first."""
    rng = random.Random(seed)
    TB0 = 5_000_000_000
    n_ticks = 400

    def ms2tb(ms):
        return round(ms * 1e-3 * tbfreq)

    lat = [25.0 + b * P_MS + x_ms for b in range(2)]
    plays = [(lat[0], 1001), (lat[1], 1002)]
    if not hw_clear:
        plays += [(lat[0] + m * n * P_MS, 1001) for m in range(1, 5)] + [(lat[1] + m * n * P_MS, 1002) for m in range(1, 5)]
    ev = [(TB0 + ms2tb(25.0 + i * P_MS), 0, i % n, 0) for i in range(1, n_ticks + 1)]
    if hw_clear:
        ev += [(TB0 + ms2tb(lat[b] + rng.uniform(0.0, 0.0015)), 1, b + 1, b) for b in range(2)]
    ev.sort()
    out = ["HWPROBE,1"]
    for key, v in (("probe", "ring110"), ("probe_version", "1"), ("git_describe", "synthetic"),
                   ("source_sha256", "0" * 64), ("fw_version", "synthetic"), ("fw_type", "synthetic"),
                   ("console_model", "synthetic"), ("console_info_source", "operator"), ("config", "c2"),
                   ("tbfreq", tbfreq)):
        out.append(f"HDR,{key},{v}")
    for key, v in {"layer": "L2", "run": 1, "tbfreq": tbfreq, "blocks": n, "channels": 2, "abort_reason": 0,
                   "idx_after_start": "0x0000000000000000", "l2_c2": 1, "l2_preset": "c2", "tag": "c2"}.items():
        out.append(f"META,{key},{v}")
    for tb, kind, idx, blk in ev:
        out.append(f"EV,{tb},{kind},{idx},0,{blk},0x00000000,0x00000000,0x00000000,0x00000000")
    out.append(f"END,{len(ev)},0")
    fsu = 2 ** (bits - 1) - 1
    n_frames = int((lead_s + (25.0 + n_ticks * P_MS) * 1e-3 + tail_s) * FS)
    sigma = 10 ** (noise_dbfs / 20.0) * fsu
    chans = [_channel_base(random.Random(seed + 1), n_frames, sigma, 0.0),
             _channel_base(random.Random(seed + 2), n_frames, sigma, 0.0)]
    for t, code in plays:
        s0 = round((lead_s * 1000.0 + t + 37.0 + MARK_START / FS * 1000.0) * 1e-3 * FS)
        for i, sg in enumerate(pn_signs(code)):
            for ch in chans:
                ch[s0 + i] += round(sg * 0.5 * gain * fsu)
    return "\n".join(out) + "\n", write_wav(None, chans, bits)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("prefix", help="writes PREFIX.csv and PREFIX.wav")
    ap.add_argument("--x-ms", type=float, default=1.0)
    ap.add_argument("--jitter-ms", type=float, default=0.03)
    ap.add_argument("--n", type=int, default=8)
    ap.add_argument("--js", default="-1,0,1,2")
    ap.add_argument("--o-min", type=float, default=0.0)
    ap.add_argument("--o-max", type=float, default=12.0)
    ap.add_argument("--o-step", type=float, default=0.5)
    ap.add_argument("--reps", type=int, default=10)
    ap.add_argument("--ref-only", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--capture-delay-ms", type=float, default=37.0)
    ap.add_argument("--drift-ppm", type=float, default=0.0)
    ap.add_argument("--smear", type=float, default=1.0, help="D/A smear, one-pole time constant in samples (0 = none)")
    ap.add_argument("--noise-dbfs", type=float, default=-60.0)
    ap.add_argument("--bits", type=int, choices=(16, 24, 32), default=24)
    ap.add_argument("--no-clear", action="store_true")
    ap.add_argument("--drop-write", type=float, default=0.0)
    ap.add_argument("--partial-write", type=float, default=0.0)
    ap.add_argument("--swap-channels", action="store_true")
    ap.add_argument("--dc", type=float, default=0.0)
    ap.add_argument("--truncate-s", type=float, default=None)
    ap.add_argument("--version", type=int, default=1)
    ap.add_argument("--gap-prob", type=float, default=0.0)
    ap.add_argument("--fine-x", type=float, default=None,
                    help="second pass: j 0 and 1 on a 0.1 ms grid within +-1 ms of FINE_X + j*P (FINE_X: the first pass's x)")
    a = ap.parse_args(argv)
    js = tuple(int(v) for v in a.js.split(","))
    grids, reps, ref_only = None, a.reps, a.ref_only
    if a.fine_x is not None:
        js, reps, ref_only = (0, 1), 6, 20
        grids = {j: (a.fine_x + j * P_MS - 1.0, a.fine_x + j * P_MS + 1.0, 0.1) for j in js}
    csv, wav, truth = synth_run(
        x_ms=a.x_ms, jitter_ms=a.jitter_ms, n=a.n, js=js, o_min=a.o_min, grids=grids,
        o_max=a.o_max, o_step=a.o_step, reps=reps, ref_only=ref_only, seed=a.seed,
        capture_delay_ms=a.capture_delay_ms, drift_ppm=a.drift_ppm, smear=a.smear, noise_dbfs=a.noise_dbfs,
        bits=a.bits, hw_clear=not a.no_clear, drop_write=a.drop_write, partial_write=a.partial_write,
        swap_channels=a.swap_channels, dc=a.dc,
        truncate_s=a.truncate_s, format_version=a.version, gap_prob=a.gap_prob)
    with open(a.prefix + ".csv", "w") as fh:
        fh.write(csv)
    with open(a.prefix + ".wav", "wb") as fh:
        fh.write(wav)
    print("wrote {0}.csv and {0}.wav: {1} trials ({2} hit, {3} miss by the model), x = {4} ms, {5:.1f} s of audio".format(
        a.prefix, truth["trials"], truth["hits"], truth["misses"], truth["x_ms"], truth["duration_s"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
