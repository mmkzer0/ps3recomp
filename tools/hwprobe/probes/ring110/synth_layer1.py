#!/usr/bin/env python3
"""Synthetic Layer 1 CSVs for testing analyze_layer1.py (no hardware involved).

SYNTHETIC data only: it encodes the section 2 / section 6 hypotheses, not measurements.
Model parameter `x_ms` is the latch offset of PROBE.md section 1; the sentinel of the block
named by the new index is zeroed at  t_c(r) + x_ms + U(noise_lo, noise_hi).

    B (RPCS3)            x_ms = P,  noise (-0.30, 0.0)   RPCS3 clears before publishing the next index
    A, read at change    x_ms = 0,  noise (0.0, 0.5)
    A, read delayed 2.6  x_ms = 2.6, noise (-0.05, 0.05)
"""
import bisect
import random

P_MS = 256.0 / 48000.0 * 1000.0
SENT_BASE = 0x3A83126F  # 0.001f; per-block sentinel words only need to differ from 0


def synth_csv(*, x_ms, noise=(0.0, 0.0), hw_clear=True, n=8, nticks=2100, discard=100, seed=1,
              skip_at=None, q4_every=4, tbfreq=79_800_000, jitter_ms=0.01, drift_ppm=20.0,
              ev_lag_ms=0.03, ev_before=False, ts_const_us=2000.0, st0_us=1_000_000_000,
              truncate=False, hw_other_word=None, format_version=1, ui_overlaps=0, qt_accept=range(1, 15)):
    rng = random.Random(seed)
    ms2tb = lambda ms: round(ms * 1e-3 * tbfreq)
    p_eff = P_MS * (1.0 + drift_ppm * 1e-6)
    t0 = 25.0
    tb_base = 5_000_000_000

    tcs, idxs = [], []
    for i in range(1, nticks + 1):
        tcs.append(t0 + i * p_eff + rng.gauss(0.0, jitter_ms))
        idxs.append((i + (1 if skip_at is not None and i >= skip_at else 0)) % n)
    tc_tb = [tb_base + ms2tb(t) + rng.randint(0, 24) for t in tcs]
    sentword = {b: SENT_BASE + b for b in range(n)}

    ev = []  # (tb, line-builder args)
    for i in range(nticks):
        ev.append((tc_tb[i], 0, idxs[i], 0, 0, 0, 0, 0))
    if hw_clear:
        for i in range(nticks):
            z = tcs[i] + x_ms + rng.uniform(*noise)
            ztb = tb_base + ms2tb(z)
            j = bisect.bisect_right(tc_tb, ztb) - 1
            cur = idxs[j] if j >= 0 else 0
            b = idxs[i] % n
            ev.append((ztb, 1, cur, b, 0, 0, 0, 0))
            if hw_other_word is not None and i % 50 == 0:
                ev.append((ztb + ms2tb(3.0), 1, cur, b, hw_other_word, 0, 0, 0))
            ev.append((ztb + ms2tb(2.5 * P_MS), 2, cur, b, sentword[b], 0, 0, 0))
    ev.append((tc_tb[nticks // 2] + ms2tb(1.0), 3, idxs[nticks // 2], 0, 1700, 0, 0, 0))
    ev.sort(key=lambda e: (e[0], e[1]))

    def st_of(tb):
        return st0_us + (tb - tb_base) / tbfreq * 1e6 * (1.0 + drift_ppm * 1e-6)

    out = [f"HWPROBE,{format_version}"]
    for k, v in (("probe", "ring110"), ("probe_version", "1"), ("git_describe", "synthetic"),
                 ("source_sha256", "0" * 64), ("fw_version", "unknown"), ("fw_type", "unknown"),
                 ("console_model", "unknown"), ("console_info_source", "unavailable"),
                 ("config", "synthetic"), ("tbfreq", tbfreq)):
        out.append(f"HDR,{k},{v}")
    zero64 = f"0x{0:016x}"
    meta = {
        "version": "synthetic", "layer": "L1", "run": 1, "tbfreq": tbfreq, "blocks": n, "channels": 2,
        "idx_before_start": zero64, "idx_after_start": zero64,
        "poll_prio": 0, "ev_prio": 1, "max_gap_ticks": ms2tb(0.04), "abort_reason": 0, "nchg": nticks,
        "ev_drop": 0, "discard": discard, "ticks": nticks - discard, "q4_every": q4_every, "ev_thread": 1,
        "refill": 1, "readmode": 0, "seed": seed, "tag": "synthetic", "watch_word": 0, "sentinel_mode": 1,
        "ui_enabled": 1, "ui_draws": 3, "ui_deferred": 0, "ui_overlaps": ui_overlaps, "ui_timed_sections": 1,
        "ui_max_draw_us": 90000,
    }
    for k, v in meta.items():
        out.append(f"META,{k},{v}")
    for b in range(n):
        out.append(f"SENTWORD,{b},0x{sentword[b]:08x}")
    for b in range(n):
        out.append(f"FINALWORD,{b},0x{sentword[b]:08x}")
    t_sync = tb_base
    last = tc_tb[-1] + ms2tb(50)
    while t_sync < last:
        out.append(f"SYNC,{t_sync},{int(st_of(t_sync + 20))},{t_sync + 40}")
        t_sync += 10 * tbfreq
    out.append(f"SYNC,{last},{int(st_of(last + 20))},{last + 40}")
    for tb, kind, cur, blk, a, b, c, zm in ev:
        out.append(f"EV,{tb},{kind},{cur},0,{blk},0x{a:08x},0x{b:08x},0x{c:08x},0x{zm:08x}")
    if q4_every:
        for i in range(nticks):
            ordinal = i + 1
            if ordinal % q4_every:
                continue
            tb0 = tc_tb[i] + 40
            tb1 = tb0 + 60
            tb2 = tb1 + 3000
            tb3 = tb2 + 60
            tags = [ordinal + k for k in (-1, 0, 1)]
            tcus = st_of(tc_tb[i])
            ts = [int(tcus + (x_ms + k * P_MS) * 1000.0 + ts_const_us + rng.gauss(0, 5)) for k in (-1, 0, 1)]
            out.append(
                f"Q4,{ordinal},{idxs[i]},{tb0},{int(st_of(tb0))},{tb1},{tb2},{int(st_of(tb2))},{tb3},"
                f"{tags[0]},{tags[1]},{tags[2]},{ts[0]},{ts[1]},{ts[2]},0,0,0,0,0,0")
            for m in range(21):
                if i - m < 0 or tags[1] - m <= 0:
                    break
                accepted = m in qt_accept
                # a block played m periods ago has the time of its own index change (+ the model's offset)
                ts_m = int(st_of(tc_tb[i - m]) + x_ms * 1000.0 + ts_const_us + rng.gauss(0, 5)) if accepted else 0
                rc = 0 if accepted else -2144270577  # 0x8031070f, TAG_NOT_FOUND
                out.append(f"QT,{ordinal},{m},{tb0 + 100 + 40 * m},{tags[1] - m},{ts_m},{rc}")
    for i in range(nticks):
        if ev_before:
            tbe = tc_tb[i] - ms2tb(0.2)
            idx_w = idxs[i - 1] if i else 0
        else:
            tbe = tc_tb[i] + ms2tb(ev_lag_ms + abs(rng.gauss(0, 0.01)))
            idx_w = idxs[i]
        out.append(f"QE,{tbe},0x1000000a,{i},0,0,{idx_w},0")
    if not truncate:
        out.append(f"END,{len(ev)},0")
    return "\n".join(out) + "\n"


MODELS = {
    "B": {"x_ms": P_MS, "noise": (-0.30, 0.0)},
    "A0": {"x_ms": 0.0, "noise": (0.0, 0.5)},
    "A2.6": {"x_ms": 2.6, "noise": (-0.05, 0.05)},
    "deeper": {"x_ms": -6.0, "noise": (-0.1, 0.1)},
}


if __name__ == "__main__":
    import sys
    name = sys.argv[1] if len(sys.argv) > 1 else "B"
    sys.stdout.write(synth_csv(**MODELS[name]))
