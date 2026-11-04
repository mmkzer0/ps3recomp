#!/usr/bin/env python3
"""Tests for tools/hwprobe/probes/lv2conf/compare_lv2conf.py on SYNTHETIC result files.

Standalone: standard library only, no toolchain, console or host build. Every file here is written by
this test; none comes from a real run.

    python3 tools/test_hwprobe_lv2conf.py
"""
import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(TOOLS, "hwprobe", "probes", "lv2conf"))

import compare_lv2conf as cmp

_TMP = None


def setUpModule():
    global _TMP
    _TMP = tempfile.mkdtemp(prefix="hwprobe_lv2conf_")


def tearDownModule():
    shutil.rmtree(_TMP, ignore_errors=True)


def header(fw_type="CEX", model="CECHC04", probe="lv2conf", version=1, drop=()):
    rows = [f"HWPROBE,{version}"]
    hdr = {"probe": probe, "probe_version": "1", "git_describe": "unknown", "source_sha256": "unknown",
           "fw_version": "unknown", "fw_type": fw_type, "console_model": model,
           "console_info_source": "operator", "config": "default", "tbfreq": "79800000"}
    rows += [f"HDR,{k},{v}" for k, v in hdr.items() if k not in drop]
    return rows


_n = 0


def write(rows, **kw):
    """A result file: the header, then `rows`."""
    global _n
    _n += 1
    path = os.path.join(_TMP, f"r{_n}.csv")
    with open(path, "w") as f:
        f.write("\n".join(header(**kw) + list(rows)) + "\n")
    return path


def write_raw(text):
    global _n
    _n += 1
    path = os.path.join(_TMP, f"r{_n}.csv")
    with open(path, "w") as f:
        f.write(text)
    return path


def divergences(*lines):
    global _n
    _n += 1
    path = os.path.join(_TMP, f"d{_n}.txt")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return path


def run(a, b, div=None):
    """(exit status, stdout, stderr) of the CLI."""
    argv = [a, b] + (["--divergences", div] if div else [])
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stderr(err):
        rc = cmp.main(argv, out=out)
    return rc, out.getvalue(), err.getvalue()


SAME = ["CASE,mutex.create,rc,0x00000000", "CASE,mutex.create,id_nonzero,1", "CASE,event.flag,rc,0x80010002"]


class Match(unittest.TestCase):
    def test_identical_files_match(self):
        rc, out, _ = run(write(SAME), write(SAME, fw_type="host-hle", model="host"))
        self.assertEqual(rc, 0)
        self.assertIn("RESULT: ok", out)
        self.assertNotIn("Differing rows", out)

    def test_per_group_table_counts_cases(self):
        rows = SAME + ["CASE,mutex.lock,rc,0x00000000"]
        rep = cmp.compare(cmp.parse_result(write(rows)), cmp.parse_result(write(rows)), {})
        self.assertEqual(rep.groups["mutex"]["match"], 2)
        self.assertEqual(rep.groups["event"]["match"], 1)

    def test_meta_rows_are_not_compared(self):
        a = write(SAME + ["META,backend,console"])
        b = write(SAME + ["META,backend,host-hle", "META,toolkit,abc123"])
        self.assertEqual(run(a, b)[0], 0)


class Differ(unittest.TestCase):
    def test_differing_value_is_reported_with_both_values_and_fails(self):
        a = write(SAME)
        b = write(["CASE,mutex.create,rc,0x80010002" if "mutex.create,rc" in r else r for r in SAME])
        rc, out, _ = run(a, b)
        self.assertEqual(rc, 1)
        self.assertIn("mutex.create rc  A=0x00000000  B=0x80010002  [UNLISTED]", out)
        self.assertIn("FAIL (1 unlisted difference(s)", out)

    def test_key_missing_on_one_side_is_a_difference(self):
        a = write(SAME)
        b = write([r for r in SAME if "id_nonzero" not in r])
        rc, out, _ = run(a, b)
        self.assertEqual(rc, 1)
        self.assertIn("mutex.create id_nonzero  A=1  B=<absent>", out)

    def test_key_recorded_twice_compares_as_a_list(self):
        a = write(["CASE,x.y,v,1", "CASE,x.y,v,2"])
        self.assertEqual(run(a, write(["CASE,x.y,v,1", "CASE,x.y,v,2"]))[0], 0)
        self.assertEqual(run(a, write(["CASE,x.y,v,2", "CASE,x.y,v,1"]))[0], 1)
        self.assertEqual(run(a, write(["CASE,x.y,v,1"]))[0], 1)

    def test_value_with_commas_is_kept_whole(self):
        a = write(["CASE,x.y,name,a,b"])
        self.assertEqual(run(a, write(["CASE,x.y,name,a,c"]))[0], 1)

    def test_stop_row_against_golden_rows_differs(self):
        a = write(["CASE,sem.create,rc,0x00000000", "CASE,sem.create,id_nonzero,1"])
        b = write(["CASE,sem.create,stop,unimplemented-syscall"])
        rc, out, _ = run(a, b)
        self.assertEqual(rc, 1)
        self.assertIn("sem.create stop  A=<absent>  B=unimplemented-syscall", out)

    def test_case_with_rows_on_one_side_only_fails(self):
        a = write(SAME)
        no_event = write([r for r in SAME if not r.startswith("CASE,event.")])
        rc, out, _ = run(a, no_event)
        self.assertEqual(rc, 1)
        self.assertIn("Only in A", out)
        self.assertIn("  event.flag", out)
        rc, out, _ = run(no_event, a)
        self.assertEqual(rc, 1)
        self.assertIn("Only in B", out)


class Skipped(unittest.TestCase):
    def test_skip_on_either_side_is_not_a_difference(self):
        a = write(SAME)
        b = write(["SKIP,mutex.create,needs-threads", "CASE,event.flag,rc,0x80010002"])
        rc, out, _ = run(a, b)
        self.assertEqual(rc, 0)
        self.assertIn("Skipped:", out)
        self.assertIn("mutex.create (B: needs-threads)", out)
        rep = cmp.compare(cmp.parse_result(a), cmp.parse_result(b), {})
        self.assertEqual(rep.groups["mutex"]["skipped"], 1)
        self.assertEqual(rep.groups["event"]["match"], 1)

    def test_skip_on_the_golden_side(self):
        a = write(["SKIP,mutex.create,hw-only"] + SAME[2:])
        self.assertEqual(run(a, write(SAME))[0], 0)


class ListedDivergence(unittest.TestCase):
    def setUp(self):
        self.a = write(SAME)
        self.b = write(["CASE,mutex.create,rc,0x80010002" if "mutex.create,rc" in r else r for r in SAME])

    def test_listed_difference_passes_and_names_the_issue(self):
        d = divergences("# a comment", "", "mutex.create rc #96 slot answers CELL_EINVAL for a valid attribute")
        rc, out, _ = run(self.a, self.b, d)
        self.assertEqual(rc, 0)
        self.assertIn("[listed #96: slot answers CELL_EINVAL for a valid attribute]", out)

    def test_listing_one_key_does_not_cover_another(self):
        b = write(["CASE,mutex.create,rc,0x80010002", "CASE,mutex.create,id_nonzero,0", "CASE,event.flag,rc,0x80010002"])
        rc, out, _ = run(self.a, b, divergences("mutex.create rc #96 reason"))
        self.assertEqual(rc, 1)
        self.assertIn("mutex.create id_nonzero  A=1  B=0  [UNLISTED]", out)

    def test_star_covers_every_key_of_the_case(self):
        a = write(["CASE,sem.create,rc,0x00000000", "CASE,sem.create,id_nonzero,1"])
        b = write(["CASE,sem.create,stop,unimplemented-syscall"])
        rc, out, _ = run(a, b, divergences("sem.create * #96 slot has no handler"))
        self.assertEqual(rc, 0)
        self.assertEqual(out.count("[listed #96"), 3)

    def test_a_listing_is_per_case(self):
        rc, _, _ = run(self.a, self.b, divergences("event.flag rc #96 not this case"))
        self.assertEqual(rc, 1)


class StaleDivergence(unittest.TestCase):
    def test_listed_row_that_no_longer_differs_fails(self):
        rc, out, _ = run(write(SAME), write(SAME), divergences("mutex.create rc #96 fixed since"))
        self.assertEqual(rc, 1)
        self.assertIn("Stale divergences", out)
        self.assertIn("mutex.create rc #96 fixed since", out)

    def test_entry_for_a_case_neither_run_has_is_stale(self):
        rc, out, _ = run(write(SAME), write(SAME), divergences("gone.case rc #1 removed case"))
        self.assertEqual(rc, 1)
        self.assertIn("gone.case rc #1", out)

    def test_entry_for_a_skipped_case_is_stale(self):
        a = write(SAME)
        b = write(["SKIP,mutex.create,needs-threads"] + SAME[2:])
        rc, _, _ = run(a, b, divergences("mutex.create rc #96 reason"))
        self.assertEqual(rc, 1)

    def test_star_entry_is_stale_when_the_case_matches(self):
        rc, _, _ = run(write(SAME), write(SAME), divergences("mutex.create * #96 reason"))
        self.assertEqual(rc, 1)

    def test_exact_entry_used_leaves_star_entry_stale(self):
        b = write(["CASE,mutex.create,rc,0x80010002" if "mutex.create,rc" in r else r for r in SAME])
        rc, out, _ = run(write(SAME), b, divergences("mutex.create rc #96 a", "mutex.create * #97 b"))
        self.assertEqual(rc, 1)
        self.assertIn("mutex.create * #97 b", out)


class DivergenceFile(unittest.TestCase):
    def test_malformed_lines_are_rejected(self):
        for bad in ("mutex.create rc", "mutex.create rc 96 no hash", "mutex.create rc #x reason", "mutex.create rc #96"):
            with self.subTest(bad=bad), self.assertRaises(cmp.DivergenceError):
                cmp.parse_divergences(divergences(bad))

    def test_duplicate_entry_is_rejected(self):
        with self.assertRaises(cmp.DivergenceError):
            cmp.parse_divergences(divergences("a.b rc #1 x", "a.b rc #2 y"))

    def test_cli_exit_2_on_a_bad_divergence_file(self):
        rc, _, err = run(write(SAME), write(SAME), divergences("mutex.create rc"))
        self.assertEqual(rc, 2)
        self.assertIn("expected '<case_id> <key> #<issue> <reason>'", err)

    def test_missing_divergence_file_is_exit_2(self):
        self.assertEqual(run(write(SAME), write(SAME), os.path.join(_TMP, "nope.txt"))[0], 2)

    def test_shipped_file_parses_and_lists_nothing_yet(self):
        path = os.path.normpath(os.path.join(TOOLS, "..", "tests", "syscalls", "lv2conf", "divergences.txt"))
        if os.path.exists(path):
            cmp.parse_divergences(path)


class HeaderRejection(unittest.TestCase):
    def check(self, a, fragment):
        rc, _, err = run(a, write(SAME))
        self.assertEqual(rc, 2)
        self.assertIn(fragment, err)

    def test_file_without_header(self):
        self.check(write_raw("CASE,mutex.create,rc,0x00000000\n"), "not a hwprobe result file")

    def test_unknown_format_version(self):
        self.check(write(SAME, version=2), "unsupported hwprobe format version 2")

    def test_missing_required_key(self):
        self.check(write(SAME, drop=("tbfreq",)), "header lacks required keys: tbfreq")

    def test_other_probe(self):
        self.check(write(SAME, probe="ring110"), "expected 'lv2conf'")

    def test_second_file_is_checked_too(self):
        rc, _, err = run(write(SAME), write(SAME, version=7))
        self.assertEqual(rc, 2)
        self.assertIn("unsupported hwprobe format version 7", err)

    def test_unreadable_path(self):
        rc, _, err = run(os.path.join(_TMP, "nope.csv"), write(SAME))
        self.assertEqual(rc, 2)
        self.assertIn("nope.csv", err)

    def test_unknown_row_type(self):
        self.check(write(SAME + ["WAT,mutex.create,rc,1"]), "unknown row type")

    def test_case_row_without_a_value(self):
        self.check(write(["CASE,mutex.create,rc"]), "needs <case_id>,<key>,<value>")

    def test_bad_case_id(self):
        self.check(write(["CASE,Mutex Create,rc,1"]), "bad case id")

    def test_hdr_row_after_the_header_block(self):
        self.check(write(SAME + ["HDR,late,1"]), "HDR row after the header block")


class TimeRows(unittest.TestCase):
    TIMES_A = ("TIME,mutex.lock,ns,100", "TIME,mutex.lock,ns,300", "TIME,mutex.lock,ns,200")
    TIMES_B = ("TIME,mutex.lock,ns,5", "TIME,mutex.lock,ns,9")

    def test_time_rows_never_cause_a_difference(self):
        a = write([*SAME, *self.TIMES_A])
        b = write([*SAME, *self.TIMES_B])
        rc, out, _ = run(a, b)
        self.assertEqual(rc, 0)
        self.assertIn("RESULT: ok", out)

    def test_time_rows_on_one_side_only_are_fine(self):
        self.assertEqual(run(write([*SAME, *self.TIMES_A]), write(SAME))[0], 0)

    def test_min_and_median_per_key(self):
        rep = cmp.compare(cmp.parse_result(write([*SAME, *self.TIMES_A])), cmp.parse_result(write([*SAME, *self.TIMES_B])), {})
        (cid, key, sa, sb), = rep.times
        self.assertEqual((cid, key), ("mutex.lock", "ns"))
        self.assertEqual(sa, (100, 200, 3))
        self.assertEqual(sb, (5, 7, 2))
        _rc, out, _ = run(write([*SAME, *self.TIMES_A]), write([*SAME, *self.TIMES_B]))
        self.assertIn("mutex.lock ns  A: min 100 med 200 n=3  B: min 5 med 7 n=2", out)

    def test_a_case_with_only_time_rows_is_not_a_difference(self):
        self.assertEqual(run(write(["TIME,t.x,ns,1"]), write(["TIME,t.x,ns,2"]))[0], 0)

    def test_time_value_must_be_an_integer(self):
        rc, _, err = run(write(["TIME,t.x,ns,fast"]), write(SAME))
        self.assertEqual(rc, 2)
        self.assertIn("TIME value is not an integer", err)


class Cli(unittest.TestCase):
    def test_table_lists_groups_and_total(self):
        _, out, _ = run(write(SAME), write(SAME))
        self.assertIn("group", out)
        self.assertIn("only-in-A", out)
        self.assertIn("total", out)
        self.assertIn("A: ", out)
        self.assertIn("fw_type=host-hle", run(write(SAME), write(SAME, fw_type="host-hle"))[1])


DRIVER = os.path.join(TOOLS, "..", "tests", "syscalls", "run_lv2conf.py")


class Driver(unittest.TestCase):
    """tests/syscalls/run_lv2conf.py, the ctest driver, with a stand-in for test_lv2conf that writes a canned file."""

    def runner(self, rows, status=0, **kw):
        """A stand-in test_lv2conf: writes the header and `rows` to its --out path, exits `status`."""
        canned = write(rows, **kw)
        global _n
        _n += 1
        path = os.path.join(_TMP, f"runner{_n}.py")
        with open(path, "w") as f:
            f.write(f"#!{sys.executable}\nimport shutil, sys\nshutil.copy({canned!r}, sys.argv[2])\nsys.exit({status})\n")
        os.chmod(path, 0o755)
        return path

    def drive(self, runner, *extra):
        out = os.path.join(_TMP, "driven.csv")
        p = subprocess.run([sys.executable, DRIVER, runner, out, *extra], check=False, capture_output=True, text=True)
        return p.returncode, p.stdout, p.stderr

    HOST = ("CASE,mutex.create,rc,0x00000000", "SKIP,threads.x,threads", "META,backend,host", "META,cases_total,2")

    def test_no_golden_every_case_ran_or_skipped(self):
        rc, out, _ = self.drive(self.runner(self.HOST))
        self.assertEqual(rc, 0)
        self.assertIn("all 2 cases ran or were skipped", out)

    def test_no_golden_a_registered_case_with_no_row_fails(self):
        rc, _, err = self.drive(self.runner([*self.HOST[:-1], "META,cases_total,3"]))
        self.assertEqual(rc, 1)
        self.assertIn("2 of 3 registered cases", err)

    def test_no_golden_a_stopped_case_is_a_result_not_a_failure(self):
        rows = ["CASE,sem.create,stop,unimplemented-syscall", "META,cases_total,1"]
        self.assertEqual(self.drive(self.runner(rows))[0], 0)

    def test_failing_runner_fails_the_driver(self):
        rc, _, err = self.drive(self.runner(self.HOST, status=1))
        self.assertEqual(rc, 1)
        self.assertIn("exited 1", err)

    def test_unparsable_host_file_fails_the_driver(self):
        rc, _, err = self.drive(self.runner(self.HOST, probe="ring110"))
        self.assertEqual(rc, 1)
        self.assertIn("expected 'lv2conf'", err)

    def test_golden_and_divergences_go_together(self):
        self.assertEqual(self.drive(self.runner(self.HOST), "--golden", write(SAME))[0], 2)

    def test_golden_is_compared_by_the_python_comparator(self):
        golden = write(["CASE,mutex.create,rc,0x00000000", "SKIP,threads.x,threads"])
        none = divergences("# nothing")
        self.assertEqual(self.drive(self.runner(self.HOST), "--golden", golden, "--divergences", none)[0], 0)
        differing = write(["CASE,mutex.create,rc,0x80010002"])
        rc, out, _ = self.drive(self.runner(self.HOST), "--golden", differing, "--divergences", none)
        self.assertEqual(rc, 1)
        self.assertIn("mutex.create rc  A=0x80010002  B=0x00000000  [UNLISTED]", out)
        listed = divergences("mutex.create rc #96 reason")
        self.assertEqual(self.drive(self.runner(self.HOST), "--golden", differing, "--divergences", listed)[0], 0)
        stale = divergences("mutex.create rc #96 reason")
        self.assertEqual(self.drive(self.runner(self.HOST), "--golden", golden, "--divergences", stale)[0], 1)


if __name__ == "__main__":
    unittest.main()
