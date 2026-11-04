#!/usr/bin/env python3
"""Unit tests for tools/hwprobe/probes/ring110/analyze_layer1.py on SYNTHETIC CSVs
(deterministic seeds, no hardware, no toolchain), including the versioned result header.

Run:  python3 tools/test_hwprobe_ring110_analyze.py
"""
import contextlib
import io
import os
import sys
import tempfile
import unittest

HWPROBE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hwprobe")
sys.path.insert(0, HWPROBE)
sys.path.insert(0, os.path.join(HWPROBE, "probes", "ring110"))

import analyze_layer1 as an
import hwprobe_format
from synth_layer1 import MODELS, P_MS, synth_csv


def run_model(**kw):
    run = an.parse_lines(synth_csv(**kw).splitlines())
    return an.analyze(run)


class ModelClassification(unittest.TestCase):
    def test_B_x_equals_P(self):
        res = run_model(seed=11, **MODELS["B"])
        self.assertEqual(res["class"], "B")
        self.assertTrue(res["hw_clear"])
        self.assertLess(abs(res["x_clear_ms"] - P_MS), 0.7)
        self.assertEqual(res["label_within_1ms"], "r-1")
        self.assertTrue(res["label_consistent"])
        self.assertEqual(res["nearest_table_column"], "B (RPCS3 / toolkit)")

    def test_A_read_at_change(self):
        res = run_model(seed=12, **MODELS["A0"])
        self.assertEqual(res["class"], "A")
        self.assertLess(abs(res["x_clear_ms"]), 0.7)
        self.assertEqual(res["label_within_1ms"], "r")
        self.assertTrue(res["label_consistent"])

    def test_A_read_delayed_2_6(self):
        res = run_model(seed=13, **MODELS["A2.6"])
        self.assertEqual(res["class"], "intermediate")
        self.assertAlmostEqual(res["x_clear_ms"], 2.6, delta=0.15)
        self.assertIsNone(res["label_within_1ms"])
        self.assertEqual(res["nearest_table_column"], "A, read delayed 2.6 ms")

    def test_deeper_prefetch(self):
        res = run_model(seed=14, **MODELS["deeper"])
        self.assertEqual(res["class"], "deeper")
        self.assertLess(res["x_clear_ms"], -0.7)

    def test_prefetch_half_period_is_intermediate(self):
        res = run_model(seed=15, x_ms=P_MS / 2, noise=(-0.05, 0.05))
        self.assertEqual(res["class"], "intermediate")
        self.assertEqual(res["nearest_table_column"], "B + prefetch e = P/2")

    def test_classes_are_distinct_under_jitter(self):
        for seed in (1, 2, 3):
            classes = [run_model(seed=seed, jitter_ms=0.05, **MODELS[m])["class"] for m in ("B", "A0", "A2.6")]
            self.assertEqual(classes, ["B", "A", "intermediate"])

    def test_classify_table_boundaries(self):
        c = an.classify
        self.assertEqual(c(P_MS), "B")
        self.assertEqual(c(P_MS - 0.69), "B")
        self.assertEqual(c(P_MS - 0.71), "intermediate")
        self.assertEqual(c(0.69), "A")
        self.assertEqual(c(-0.69), "A")
        self.assertEqual(c(0.71), "intermediate")
        self.assertEqual(c(-0.71), "deeper")
        self.assertEqual(c(P_MS + 0.8), "outside-table")


class NoHardwareClear(unittest.TestCase):
    def test_no_clear(self):
        res = run_model(seed=21, x_ms=0.0, hw_clear=False)
        self.assertFalse(res["hw_clear"])
        self.assertEqual(res["class"], "undetermined")
        self.assertIsNone(res["x_clear_ms"])
        self.assertIsNone(res["label_consistent"])
        self.assertEqual(res["q2"]["n_zero_events"], 0)
        # Q1/Q5 still work without a clear
        self.assertEqual(res["q5"]["skips"], 0)
        self.assertAlmostEqual(res["q1_period_ms"]["mean"], P_MS, delta=0.01)


class IndexSkip(unittest.TestCase):
    def test_skipped_index_value(self):
        res = run_model(seed=31, skip_at=700, **MODELS["B"])
        self.assertEqual(res["q5"]["skips"], 1)
        self.assertEqual(res["q5"]["skip_at_ordinals"], [700])
        self.assertEqual(res["q5"]["repeats"], 0)
        self.assertEqual(res["class"], "B")

    def test_clean_run_has_no_skips(self):
        res = run_model(seed=32, **MODELS["B"])
        self.assertEqual(res["q5"], {"skips": 0, "skip_at_ordinals": [], "repeats": 0,
                                     "torn_hi_nonzero": 0, "out_of_range": 0})


class OtherObservations(unittest.TestCase):
    def test_q1_period_and_start_values(self):
        res = run_model(seed=41, **MODELS["B"])
        s = res["q1_period_ms"]
        self.assertAlmostEqual(s["mean"], P_MS, delta=0.005)
        self.assertLess(s["std"], 0.03)
        self.assertEqual(res["first_idx_change_to"], 1)
        self.assertEqual(res["idx_before_start"], f"0x{0:016x}")

    def test_q3_zero_only_and_other_values(self):
        res = run_model(seed=42, **MODELS["B"])
        self.assertFalse(res["q3"]["hw_writes_other_than_zero"])
        self.assertEqual(set(res["q3"]["hardware_word_changes"]), {"zero"})
        res = run_model(seed=42, hw_other_word=0xBEEF0001, **MODELS["B"])
        self.assertTrue(res["q3"]["hw_writes_other_than_zero"])
        self.assertIn("other:0xbeef0001", res["q3"]["other_values"])

    def test_q4_timestamp_relation(self):
        for name in ("B", "A0"):
            kw = MODELS[name]
            res = run_model(seed=43, **kw)
            q4 = res["q4"]
            self.assertGreater(q4["n_rows"], 400)
            self.assertEqual(q4["tick_idx_mismatch"], 0)
            # synthetic: ts - t_c = x + 2.0 ms for block r, offset by +-P for r+-1
            med = q4["ts_minus_tc_us"]["r"]["median"]
            self.assertAlmostEqual(med, (kw["x_ms"] + 2.0) * 1000.0, delta=300.0)
            step = q4["ts_minus_tc_us"]["r+1"]["median"] - med
            self.assertAlmostEqual(step, P_MS * 1000.0, delta=100.0)
            self.assertEqual(q4["tag_diff_rp1_minus_r"], {1: q4["n_rows"]})
            self.assertTrue(q4["ts_monotonic"])
            self.assertLess(abs(q4["drift_ppm_vs_nominal"]), 100)

    def test_timestamp_window_scan_finds_the_accepted_ages(self):
        res = run_model(seed=46, **MODELS["B"])
        qt = res["qt"]
        self.assertEqual(qt["accepted_m"], list(range(1, 15)))
        self.assertEqual(qt["refused_m"], [0, 15, 16])
        self.assertEqual(qt["per_m"][0]["rc"], {"0x8031070f": qt["per_m"][0]["n"]})
        # the time reported for a block m periods old is m periods before its own index change
        self.assertAlmostEqual(qt["ts_step_us"]["median"], P_MS * 1000.0, delta=100.0)
        self.assertAlmostEqual(qt["per_m"][1]["ts_minus_tc_us"]["median"] - qt["per_m"][2]["ts_minus_tc_us"]["median"],
                               P_MS * 1000.0, delta=100.0)
        self.assertEqual(qt["tag_step"], 1)

    def test_timestamp_window_with_no_accepted_age_is_reported_as_such(self):
        res = run_model(seed=47, qt_accept=(), **MODELS["B"])
        self.assertEqual(res["qt"]["accepted_m"], [])
        self.assertEqual(len(res["qt"]["refused_m"]), 17)
        self.assertIn("accepted m: none", an.report(res))

    def test_no_qt_rows_without_q4(self):
        self.assertEqual(run_model(seed=48, q4_every=0, **MODELS["B"])["qt"], {"n_rows": 0})

    def test_q6_event_order(self):
        res = run_model(seed=44, **MODELS["B"])
        q6 = res["q6"]
        self.assertGreater(q6["t_ev_minus_t_c_ms"]["median"], 0.0)
        self.assertLess(q6["fraction_event_before_index_change"], 0.01)
        res = run_model(seed=44, ev_before=True, **MODELS["B"])
        q6 = res["q6"]
        self.assertLess(q6["t_ev_minus_t_c_ms"]["median"], 0.0)
        self.assertGreater(q6["fraction_event_before_index_change"], 0.99)

    def test_q4_ticks_excluded_from_q2(self):
        # with q4_every = 1 every tick is contaminated: analysis falls back to all ticks and says so
        res = run_model(seed=45, q4_every=1, **MODELS["B"])
        self.assertTrue(res["q2"]["q4_contaminated"])
        self.assertEqual(res["class"], "B")
        res = run_model(seed=45, q4_every=4, **MODELS["B"])
        self.assertFalse(res["q2"]["q4_contaminated"])
        self.assertLess(res["q2"]["ticks_used"], 1900 * 0.8)


class ParsingAndReport(unittest.TestCase):
    def test_truncated_file_is_flagged(self):
        res = run_model(seed=51, truncate=True, **MODELS["B"])
        self.assertFalse(res["complete"])
        self.assertIn("truncated", an.report(res))

    def test_report_and_cli(self):
        text = synth_csv(seed=52, **MODELS["A0"])
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as fh:
            fh.write(text)
            path = fh.name
        try:
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                self.assertEqual(an.main([path]), 0)
            self.assertIn("CLASS (section 6, from x_clear): A", buf.getvalue())
            res = an.analyze(an.parse_file(path))
            rep = an.report(res)
            self.assertIn("CLASS (section 6, from x_clear): A", rep)
            self.assertIn("x_clear = ", rep)
        finally:
            os.unlink(path)

    def test_missing_meta_is_an_error(self):
        lines = synth_csv(seed=53, **MODELS["B"]).splitlines()
        no_meta = [ln for ln in lines if not ln.startswith("META,tbfreq") and not ln.startswith("META,blocks")]
        with self.assertRaisesRegex(ValueError, "no META"):
            an.parse_lines(no_meta)


class ResultHeader(unittest.TestCase):
    def test_header_is_parsed_and_reported(self):
        res = run_model(seed=71, **MODELS["B"])
        h = res["header"]
        self.assertEqual(h["format_version"], 1)
        self.assertEqual(h["probe"], "ring110")
        self.assertEqual(h["config"], "synthetic")
        self.assertEqual(h["tbfreq"], 79_800_000)
        self.assertIn("probe ring110 1", an.report(res))

    def test_unknown_format_version_is_rejected_with_a_clear_error(self):
        text = synth_csv(seed=72, format_version=2, **MODELS["B"])
        with self.assertRaisesRegex(hwprobe_format.FormatError, "unsupported hwprobe format version 2"):
            an.parse_lines(text.splitlines())

    def test_cli_exit_code_and_message_for_unknown_version(self):
        text = synth_csv(seed=73, format_version=7, **MODELS["B"])
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as fh:
            fh.write(text)
            path = fh.name
        try:
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(an.main([path]), 2)
            self.assertIn("unsupported hwprobe format version 7", err.getvalue())
        finally:
            os.unlink(path)

    def test_file_without_header_is_rejected(self):
        lines = [ln for ln in synth_csv(seed=74, **MODELS["B"]).splitlines()
                 if not ln.startswith(("HWPROBE,", "HDR,"))]
        with self.assertRaisesRegex(hwprobe_format.FormatError, "not a hwprobe result file"):
            an.parse_lines(lines)

    def test_header_missing_a_required_key_is_rejected(self):
        lines = [ln for ln in synth_csv(seed=75, **MODELS["B"]).splitlines() if not ln.startswith("HDR,fw_type,")]
        with self.assertRaisesRegex(hwprobe_format.FormatError, "fw_type"):
            an.parse_lines(lines)

    def test_other_probe_is_rejected(self):
        lines = [ln.replace("HDR,probe,ring110", "HDR,probe,other") for ln in
                 synth_csv(seed=76, **MODELS["B"]).splitlines()]
        with self.assertRaisesRegex(hwprobe_format.FormatError, "not ring110"):
            an.parse_lines(lines)

    def test_header_and_meta_timebase_must_agree(self):
        lines = [ln.replace("HDR,tbfreq,79800000", "HDR,tbfreq,79800001") for ln in
                 synth_csv(seed=77, **MODELS["B"]).splitlines()]
        with self.assertRaisesRegex(hwprobe_format.FormatError, "tbfreq"):
            an.parse_lines(lines)

    def test_screen_overlap_is_a_warning(self):
        rep = an.report(run_model(seed=78, ui_overlaps=2, **MODELS["B"]))
        self.assertIn("2 screen draws overlapped the measurement", rep)
        self.assertNotIn("overlapped", an.report(run_model(seed=78, **MODELS["B"])))


class Q3Sampling(unittest.TestCase):
    def test_state_before_next_consumption_is_refilled_sentinel(self):
        res = run_model(seed=61, **MODELS["B"])
        after = res["q3"]["state_one_lap_after_zero"]
        self.assertEqual(set(after), {"sentinel"})

    def test_tag_consecutive_ticks_with_q4_every_1(self):
        res = run_model(seed=62, q4_every=1, **MODELS["B"])
        c = res["q4"]["tag_consecutive_ticks"]
        self.assertGreater(c["pairs"], 1900)
        self.assertEqual(c["block_r+1_tag_changed_between_t_c(r)_and_t_c(r+1)"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
