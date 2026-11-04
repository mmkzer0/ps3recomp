#!/usr/bin/env python3
"""Tests for tools/hwprobe/probes/ring110/analyze_layer2.py on SYNTHETIC CSV + WAV pairs (synth_layer2.py):
the recovered latch offset under recorder drift and noise, the agreement between the j estimates, the failure
classes (markers that repeat, dropped writes, cut-off captures), the format check and the WAV reader. No
hardware, no toolchain, a few seconds.

Run:  python3 tools/test_hwprobe_layer2.py
"""
import array
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

HWPROBE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hwprobe")
sys.path.insert(0, HWPROBE)
sys.path.insert(0, os.path.join(HWPROBE, "probes", "ring110"))

import analyze_layer2 as an
import hwprobe_format
import synth_layer1
import synth_layer2 as sy

P = sy.P_MS
TB0 = 5_000_000_000   # synth_layer2's timebase origin
_TMP = None
_CACHE = {}


def setUpModule():
    global _TMP
    _TMP = tempfile.mkdtemp(prefix="hwprobe_l2_")


def tearDownModule():
    shutil.rmtree(_TMP, ignore_errors=True)


def fine(x, **kw):
    """The second pass of RUN.md: 0.1 ms steps within +-1 ms of each j's threshold x + j*P."""
    d = {"js": (0, 1), "grids": {0: (x - 1.0, x + 1.0, 0.1), 1: (x + P - 1.0, x + P + 1.0, 0.1)}, "reps": 6,
         "ref_only": 20}
    d.update(kw)
    return d


SPECS = {
    "x1": dict(x_ms=1.0, drift_ppm=+100.0, noise_dbfs=-45.0, smear=1.5, seed=11, **fine(1.0)),
    "x4.5": dict(x_ms=4.5, drift_ppm=-100.0, noise_dbfs=-45.0, smear=1.5, seed=12, **fine(4.5)),
    # PROBE.md section 5 grid (fewer repetitions): j = -1 is never in time and j = 2 always is when x = 2.6 ms
    "main": {"x_ms": 2.6, "jitter_ms": 0.15, "reps": 4, "ref_only": 30, "drift_ppm": -60.0, "noise_dbfs": -50.0,
             "seed": 13},
    # latch jitter of 0.15 ms resolved by a 0.1 ms grid: the width of the hit-to-miss transition
    "jitter": dict(x_ms=1.0, jitter_ms=0.15, noise_dbfs=-50.0, seed=17, **fine(1.0, reps=10)),
    "swap": dict(x_ms=1.0, swap_channels=True, noise_dbfs=-50.0, seed=14, **fine(1.0)),
    "partial": dict(x_ms=1.0, partial_write=0.25, noise_dbfs=-50.0, seed=26, **fine(1.0)),
    "shift": dict(x_ms=1.0, shift_prob=0.25, shift_ms=2.0, noise_dbfs=-50.0, seed=27, **fine(1.0)),
    # the controls of PROBE.md section 7 as the presets of layer2/source/main.c write them
    "c1": {"x_ms": 2.6, "js": (3,), "grids": {3: (10.6667, 10.6667, 1.0)}, "reps": 30, "ref_only": 10,
           "preset": "c1", "noise_dbfs": -50.0, "seed": 31},
    "c3": {"x_ms": 2.6, "js": (0,), "grids": {0: (20.0, 20.0, 1.0)}, "reps": 20, "ref_only": 10,
           "preset": "c3", "noise_dbfs": -50.0, "seed": 32},
    "dc": dict(x_ms=1.0, dc=0.35, noise_dbfs=-50.0, seed=15, **fine(1.0)),
    "gaps": dict(x_ms=1.0, gap_prob=0.3, noise_dbfs=-50.0, seed=16, **fine(1.0)),
    "noclear": dict(x_ms=1.0, hw_clear=False, noise_dbfs=-50.0, seed=21, **fine(1.0)),
    "drop": dict(x_ms=1.0, drop_write=0.3, noise_dbfs=-50.0, seed=22, **fine(1.0)),
    "cut_header_full": dict(x_ms=1.0, truncate_s=40.0, noise_dbfs=-50.0, seed=23, **fine(1.0)),
    "cut_header_short": dict(x_ms=1.0, truncate_s=40.0, header_declares_full=False, noise_dbfs=-50.0, seed=23,
                             **fine(1.0)),
    "quiet": dict(x_ms=1.0, gain=0.04, noise_dbfs=-80.0, seed=24, **fine(1.0)),
    "loud": dict(x_ms=1.0, gain=3.0, noise_dbfs=-60.0, seed=25, **fine(1.0)),
}


class Case:
    """One synthetic run on disk."""

    def __init__(self, name, **kw):
        self.csv, self.wav, self.truth = sy.synth_run(**kw)
        self.csv_path = os.path.join(_TMP, name + ".csv")
        self.wav_path = os.path.join(_TMP, name + ".wav")
        with open(self.csv_path, "w") as fh:
            fh.write(self.csv)
        with open(self.wav_path, "wb") as fh:
            fh.write(self.wav)

    def run(self):
        return an.parse_lines(self.csv.splitlines())

    def analyze(self, **kw):
        return an.analyze(self.run(), an.read_wav(self.wav_path), **kw)


def case(name):
    if name not in _CACHE:
        _CACHE[name] = Case(name, **SPECS[name])
    return _CACHE[name]


def silence_path(noise):
    path = os.path.join(_TMP, f"silence_{noise}.wav")
    if not os.path.exists(path):
        sy.silence_wav(20.0, noise_dbfs=noise, path=path)
    return path


class LatchRecovery(unittest.TestCase):
    def check(self, name, x, drift):
        res = case(name).analyze()
        xl = res["x_latch"]
        self.assertIsNotNone(xl, res["warnings"])
        self.assertAlmostEqual(xl["x_ms"], x, delta=0.2)
        self.assertEqual(xl["js"], [0, 1])
        self.assertTrue(res["agreement"]["agree"], res["agreement"])
        for j in ("0", "1"):
            f = res["fits"][j]
            self.assertEqual(f["status"], "ok")
            self.assertAlmostEqual(f["x_ms"], x, delta=0.2)
        self.assertTrue(res["bounds"]["consistent"])
        self.assertAlmostEqual(res["sync"]["drift_ppm"], drift, delta=5.0)
        return res

    def test_x_1_0_with_positive_drift(self):
        res = self.check("x1", 1.0, +100.0)
        self.assertEqual(res["class"], "intermediate")

    def test_x_4_5_with_negative_drift(self):
        res = self.check("x4.5", 4.5, -100.0)
        self.assertEqual(res["nearest_table_column"], "B (RPCS3 / toolkit)")

    def test_clear_events_agree_with_the_latch(self):
        res = case("x1").analyze()
        xc = res["x_clear"]["ref_ms"]
        self.assertEqual(xc["n"], res["csv"]["started"])
        self.assertAlmostEqual(xc["median"], 1.0, delta=0.05)
        self.assertLess(xc["mad"], 0.1)    # one value per trial, all the same: a wrong block offset would scatter them
        self.assertLess(abs(res["x_clear_vs_latch_ms"]), 0.7)

    def test_hits_and_misses_follow_the_model(self):
        c = case("x1")
        res = c.analyze()
        self.assertEqual(res["classes"]["hit"], c.truth["hits"])
        self.assertEqual(res["classes"]["miss"], c.truth["misses"])
        self.assertEqual(res["classes"]["ref_only"], 20)
        self.assertLess(res["delta_error_ms"]["hit"]["max"], 0.1)
        self.assertGreater(res["delta_error_ms"]["hit"]["min"], -0.1)

    def test_standard_grid_bounds_the_unreached_j(self):
        res = case("main").analyze()
        self.assertEqual(res["fits"]["-1"]["status"], "all_miss")
        self.assertEqual(res["fits"]["2"]["status"], "all_hit")
        self.assertAlmostEqual(res["x_latch"]["x_ms"], 2.6, delta=0.3)
        self.assertEqual(res["x_latch"]["js"], [0, 1])
        self.assertLess(res["bounds"]["x_above_ms"], 2.6)
        self.assertGreater(res["bounds"]["x_below_ms"], 2.6)

    def test_width_of_the_transition_is_the_latch_jitter(self):
        # a Gaussian latch jitter of sigma 0.15 ms is a logistic of 10-90 % width 2.43 sigma = 0.36 ms
        res = case("jitter").analyze()
        for j in ("0", "1"):
            self.assertEqual(res["fits"][j]["status"], "ok")
            self.assertTrue(0.2 < res["fits"][j]["width_10_90_ms"] < 0.6, res["fits"][j])
        self.assertAlmostEqual(res["x_latch"]["x_ms"], 1.0, delta=0.15)

    def test_j_estimates_that_disagree_are_flagged(self):
        c = case("x1")
        run = c.run()
        # every j = 1 write is recorded 1.2 ms later than it was: that j's threshold, and so its x, moves by 1.2 ms
        for t in run.trials:
            if t["j"] == 1:
                t["o_actual_ms"] += 1.2
        res = an.analyze(run, an.read_wav(c.wav_path))
        self.assertFalse(res["agreement"]["agree"])
        self.assertGreater(res["agreement"]["spread_ms"], an.AGREE_MS)


class Detection(unittest.TestCase):
    def test_silence_only_gives_no_detections(self):
        for noise in (None, -70.0, -30.0):
            det = an.detect_markers(an.read_wav(silence_path(noise)))
            self.assertEqual(len(det.bursts), 0, noise)
            self.assertEqual(det.refs, [])

    def test_silence_with_a_csv_is_an_error_not_an_empty_result(self):
        with self.assertRaisesRegex(an.AnalysisError, "no marker detected"):
            an.analyze(case("x1").run(), an.read_wav(silence_path(-70.0)))

    def test_every_reference_is_found_at_the_right_level(self):
        c = case("x1")
        det = an.detect_markers(an.read_wav(c.wav_path))
        self.assertEqual(len(det.refs), c.truth["trials"])
        self.assertGreater(min(r[1] for r in det.refs), 0.8)
        res = c.analyze()
        self.assertAlmostEqual(res["capture"]["noise_rms_dbfs"], -45.0, delta=1.0)
        self.assertTrue(-12 < res["capture"]["marker_level_dbfs"] < -5)

    def test_false_alarms_on_reference_only_trials(self):
        fa = case("x1").analyze()["detection"]["false_alarm"]
        self.assertEqual(fa["ref_only_trials"], 20)
        self.assertEqual(fa["decoy_matches"], 0)
        self.assertLess(fa["decoy_rho_max"], 0.5)
        self.assertEqual(fa["ref_only_with_extra_burst"], 0)

    def test_a_foreign_burst_in_a_reference_only_window_is_counted(self):
        run = case("x1").run()
        k = next(i for i, t in enumerate(run.trials) if t["kind"] == 1)
        t_c_ms = (run.trials[k]["t_c"] - TB0) / run.tbfreq * 1000.0
        # a marker of an unrelated code, 80 ms after the trial's index change plus the model's capture delay
        cap_ms = 5000.0 + t_c_ms * (1 + 100e-6) + 37.0 + 80.0
        spec = dict(SPECS["x1"], extra_markers=[(cap_ms, 7777)])
        res = Case("foreign", **spec).analyze()
        self.assertEqual(res["detection"]["stray_bursts"], 1)
        self.assertEqual(res["detection"]["false_alarm"]["ref_only_with_extra_burst"], 1)
        self.assertTrue(any("match no trial" in w for w in res["warnings"]))

    def test_marker_on_one_channel_and_swapped_channels(self):
        res = case("swap").analyze()
        left, right = res["detection"]["rho_channels_median"]
        self.assertLess(abs(left), 0.2)       # the marker is on the right channel of the capture only
        self.assertGreater(right, 0.8)
        self.assertAlmostEqual(res["x_latch"]["x_ms"], 1.0, delta=0.2)

    def test_dc_offset_is_removed(self):
        c = case("dc")
        res = c.analyze()
        self.assertAlmostEqual(res["capture"]["dc"][0], 0.35, delta=0.01)
        self.assertEqual(res["detection"]["reference_assigned"], c.truth["trials"])
        self.assertAlmostEqual(res["x_latch"]["x_ms"], 1.0, delta=0.2)

    def test_polling_gap_at_t_c_leaves_the_trial_out_of_the_fit(self):
        c = case("gaps")
        res = c.analyze()
        self.assertGreater(res["excluded_tc_gap"], 20)
        self.assertEqual(c.analyze(max_tc_gap_ms=1.0)["excluded_tc_gap"], 0)
        self.assertAlmostEqual(res["x_latch"]["x_ms"], 1.0, delta=0.25)


class FailureClasses(unittest.TestCase):
    def test_repeating_marker_is_no_hardware_clear_not_hits(self):
        c = case("noclear")
        res = c.analyze()
        self.assertTrue(res["no_hardware_clear"])
        self.assertEqual(res["classes"].get("hit", 0), 0)
        self.assertEqual(res["classes"].get("miss", 0), 0)
        self.assertEqual(res["classes"]["no_clear"], c.truth["trials"] - 20)
        self.assertEqual(res["classes"]["ref_only_repeat"], 20)
        self.assertIsNone(res["x_latch"])
        self.assertTrue(any("does not clear" in w for w in res["warnings"]))
        self.assertIn("hw clear = no", an.report(res))

    def test_first_occurrence_scores_repeating_trials_and_says_so(self):
        res = case("noclear").analyze(first_occurrence=True)
        self.assertTrue(res["no_hardware_clear"])
        self.assertTrue(res["first_occurrence"])
        self.assertAlmostEqual(res["x_latch"]["x_ms"], 1.0, delta=0.2)
        self.assertTrue(any("scored by their first arrival" in w for w in res["warnings"]), res["warnings"])

    def test_marker_cut_short_by_the_latch_is_partial_not_hit_or_miss(self):
        c = case("partial")
        res = c.analyze()
        tests = c.truth["trials"] - 20
        self.assertGreater(res["classes"]["partial"], 0.15 * tests)
        self.assertEqual(sum(res["classes"].get(k, 0) for k in ("partial", "hit", "miss", "no_test")), tests)
        self.assertAlmostEqual(res["x_latch"]["x_ms"], 1.0, delta=0.2)

    def test_marker_at_neither_delay_is_wrong_delta(self):
        c = case("shift")
        res = c.analyze()
        tests = c.truth["trials"] - 20
        self.assertGreater(res["classes"]["wrong_delta"], 0.15 * tests)
        self.assertEqual(res["classes"]["wrong_delta"] + res["classes"]["hit"] + res["classes"]["miss"], tests)
        # the shifted markers are not scored late: the fit sees only the trials played at the hit and miss delays
        self.assertAlmostEqual(res["x_latch"]["x_ms"], 1.0, delta=0.2)

    def test_control_c1_every_write_in_time(self):
        res = case("c1").analyze()
        self.assertEqual(res["control"], {"preset": "c1", "expected": "hit", "scored": 30, "as_expected": 30,
                                          "other_classes": {}})
        self.assertEqual(res["fits"]["3"]["status"], "all_hit")
        self.assertIn("30 of 30 scored trials were a hit", an.report(res))

    def test_control_c3_late_write_is_replayed_one_lap_later_and_cleared(self):
        res = case("c3").analyze()
        self.assertEqual(res["control"]["as_expected"], 20)
        self.assertEqual(res["control"]["scored"], 20)
        # a late write is heard n*P after the in-time delay, and the hardware clears the block after that read
        self.assertAlmostEqual(res["delta_error_ms"]["miss"]["median"], 0.0, delta=0.05)
        self.assertEqual(res["x_clear"]["test_miss_ms"]["n"], 20)
        self.assertAlmostEqual(res["x_clear"]["test_miss_ms"]["median"], 2.6, delta=0.05)

    def test_control_that_fails_says_so(self):
        c = case("c3")
        run = c.run()
        run.meta["l2_preset"] = "c1"       # the same late writes, judged as the in-time control
        res = an.analyze(run, an.read_wav(c.wav_path))
        self.assertEqual(res["control"]["as_expected"], 0)
        self.assertIn("NOT as expected", an.report(res))

    def test_dropped_write_is_not_a_miss(self):
        c = case("drop")
        res = c.analyze()
        tests = c.truth["trials"] - 20
        self.assertGreater(res["classes"]["no_test"], 0.15 * tests)
        self.assertEqual(res["classes"]["no_test"] + res["classes"]["hit"] + res["classes"]["miss"], tests)
        self.assertAlmostEqual(res["x_latch"]["x_ms"], 1.0, delta=0.2)

    def test_truncated_wav_is_reported_not_silently_fitted(self):
        for name, header_full in (("cut_header_full", True), ("cut_header_short", False)):
            res = case(name).analyze()
            self.assertEqual(res["capture"]["truncated"], header_full, name)
            self.assertAlmostEqual(res["capture"]["duration_s"], 40.0, delta=0.01)
            lost = res["classes"]["not_captured"]
            self.assertGreater(lost, 30, name)
            captured = res["csv"]["started"] - lost
            counted = sum(res["classes"].get(k, 0) for k in ("hit", "miss", "ref_only", "no_test"))
            self.assertEqual(counted, captured, name)
            self.assertTrue(any("audio ends or starts inside the run" in w for w in res["warnings"]), name)
            self.assertEqual(any("cut off" in w for w in res["warnings"]), header_full, name)
            self.assertIn("not_captured", an.report(res))

    def test_low_level_and_clipping_are_warned(self):
        quiet = case("quiet").analyze()
        self.assertTrue(any("is low" in w for w in quiet["warnings"]), quiet["warnings"])
        loud = case("loud").analyze()
        self.assertTrue(any("clips" in w for w in loud["warnings"]), loud["warnings"])

    def test_wrong_capture_for_this_csv(self):
        with self.assertRaisesRegex(an.AnalysisError, "do not line up"):
            an.analyze(case("x1").run(), an.read_wav(case("main").wav_path))


class FormatChecks(unittest.TestCase):
    def test_wrong_version_is_rejected_with_a_clear_message(self):
        csv, _wav, _t = sy.synth_run(format_version=2, **fine(1.0, reps=1, ref_only=2))
        with self.assertRaisesRegex(hwprobe_format.FormatError, "unsupported hwprobe format version 2"):
            an.parse_lines(csv.splitlines())

    def test_cli_exit_code_and_message_on_a_wrong_version(self):
        csv, _wav, _t = sy.synth_run(format_version=2, **fine(1.0, reps=1, ref_only=2))
        p = os.path.join(_TMP, "v2.csv")
        with open(p, "w") as fh:
            fh.write(csv)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc = an.main([p, case("x1").wav_path])
        self.assertEqual(rc, 2)
        self.assertIn("unsupported hwprobe format version 2", err.getvalue())

    def test_no_header_missing_key_and_wrong_layer(self):
        body = case("x1").csv.splitlines()
        with self.assertRaisesRegex(hwprobe_format.FormatError, "not a hwprobe result file"):
            an.parse_lines([ln for ln in body if not ln.startswith(("HWPROBE", "HDR"))])
        with self.assertRaisesRegex(hwprobe_format.FormatError, "lacks required keys"):
            an.parse_lines([ln for ln in body if not ln.startswith("HDR,fw_type")])
        with self.assertRaisesRegex(hwprobe_format.FormatError, "analyze_layer1.py reads it"):
            an.parse_lines(synth_layer1.synth_csv(**synth_layer1.MODELS["B"]).splitlines())

    def test_csv_that_lost_its_tail_is_warned(self):
        c = case("x1")
        lines = [ln for ln in c.csv.splitlines() if not ln.startswith("END")]
        res = an.analyze(an.parse_lines(lines), an.read_wav(c.wav_path))
        self.assertTrue(any("no END row" in w for w in res["warnings"]))


class WavReader(unittest.TestCase):
    SAMPLES = (0, 1, -1, 100, -100)

    def wav(self, bits, extensible=None, declared=None):
        a = array.array("i", [v * (2 ** (bits - 1) // 128) for v in self.SAMPLES] * 40)
        return sy.write_wav(None, [a, a], bits, extensible=extensible, declared_frames=declared)

    def read(self, data, name):
        p = os.path.join(_TMP, name)
        with open(p, "wb") as fh:
            fh.write(data)
        return an.read_wav(p)

    def test_16_24_32_bit_and_extensible(self):
        for bits, ext in ((16, False), (24, False), (24, True), (32, False)):
            cap = self.read(self.wav(bits, ext), f"r{bits}{ext}.wav")
            self.assertEqual((cap.n, cap.nch, cap.bits), (200, 2, bits))
            unit = 2 ** (bits - 1) // 128
            shift = 256 if bits == 24 else 1       # 24 bit is widened to 32 bit
            self.assertEqual(list(cap.ch[0][:5]), [v * unit * shift for v in self.SAMPLES], (bits, ext))
            self.assertEqual(cap.scale, 2.0 ** (15 if bits == 16 else 31))

    def test_rate_and_encoding_are_checked(self):
        data = bytearray(self.wav(16))
        data[24:28] = (44100).to_bytes(4, "little")
        with self.assertRaisesRegex(an.WavError, "need 48000 Hz"):
            self.read(bytes(data), "r44.wav")
        data = bytearray(self.wav(16))
        data[20:22] = (3).to_bytes(2, "little")
        with self.assertRaisesRegex(an.WavError, "integer PCM"):
            self.read(bytes(data), "rfloat.wav")
        with self.assertRaisesRegex(an.WavError, "not a RIFF/WAVE"):
            self.read(b"hello world, not a wav file", "rbad.wav")

    def test_unfinalised_header_reads_to_the_end(self):
        cap = self.read(self.wav(16, declared=0), "r0.wav")
        self.assertTrue(cap.header_unset)
        self.assertEqual(cap.n, 200)


class Control2(unittest.TestCase):
    def test_c2_reports_which_marker_is_heard_first(self):
        csv, wav = sy.synth_c2()
        p = os.path.join(_TMP, "c2.csv")
        w = os.path.join(_TMP, "c2.wav")
        with open(p, "w") as fh:
            fh.write(csv)
        with open(w, "wb") as fh:
            fh.write(wav)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = an.main([p, w, "--json"])
        self.assertEqual(rc, 0)
        res = json.loads(out.getvalue())
        self.assertEqual(res["heard_first"], "block 0 (code 1001)")
        self.assertAlmostEqual(res["arrival_1002_minus_1001_ms"], P, delta=0.1)
        self.assertAlmostEqual(res["zero_block1_minus_block0_ms"], P, delta=0.01)


class Cli(unittest.TestCase):
    def test_text_and_json(self):
        c = case("x1")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(an.main([c.csv_path, c.wav_path]), 0)
        text = out.getvalue()
        self.assertIn("x_latch = ", text)
        self.assertIn("Recorder clock against the console timebase: drift +", text)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(an.main([c.csv_path, c.wav_path, "--json", "--per-trial"]), 0)
        res = json.loads(out.getvalue())
        self.assertAlmostEqual(res["x_latch"]["x_ms"], 1.0, delta=0.2)
        self.assertEqual(len(res["trials"]), c.truth["trials"])

    def test_missing_wav_is_an_error_exit(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc = an.main([case("x1").csv_path, os.path.join(_TMP, "no_such.wav")])
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
