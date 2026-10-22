#!/usr/bin/env python3
"""Tests for tools/check_commit_msg.py: normalisation and the subject/body rules."""

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

_PATH = Path(__file__).with_name("check_commit_msg.py")
_SPEC = importlib.util.spec_from_file_location("check_commit_msg", _PATH)
assert _SPEC is not None and _SPEC.loader is not None
ccm = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = ccm
_SPEC.loader.exec_module(ccm)


class Normalise(unittest.TestCase):
    def test_whitespace_comments_and_blank_runs(self):
        raw = ("\n\ncellGcmSys: keep the ring  \r\nwhy it matters\t\n\n\n\nmore\n"
               "# Please enter the commit message\n#\n\n")
        self.assertEqual(ccm.normalise(raw),
                         "cellGcmSys: keep the ring\n\nwhy it matters\n\nmore\n")

    def test_scissors_drops_the_verbose_diff(self):
        raw = ("ci: x\n\nbody\n" + ccm.SCISSORS + "\ndiff --git a/x b/x\n+long line\n")
        self.assertEqual(ccm.normalise(raw), "ci: x\n\nbody\n")

    def test_idempotent(self):
        once = ccm.normalise("spu_dma: a\nb\n\n\nc  \n")
        self.assertEqual(ccm.normalise(once), once)

    def test_only_comments_is_empty(self):
        self.assertEqual(ccm.normalise("# nothing\n#\n"), "")


class Problems(unittest.TestCase):
    def test_repository_style_subjects_pass(self):
        for subject in ("cellGcmSys: publish get past a semaphore RELEASE before its label",
                        "HLE_TOP / SPU_INTERP_STATS: don't reach into cellGcmSys for the clock",
                        "fix(template): mmap the guest arena on POSIX",
                        "README: v0.17.0 changelog"):
            with self.subTest(subject=subject):
                self.assertEqual(ccm.problems(subject + "\n"), [])

    def test_subject_without_area_fails(self):
        self.assertTrue(any("area" in p for p in ccm.problems("fix the thing\n")))

    def test_subject_length_boundary(self):
        at_limit = "ci: " + "x" * (ccm.SUBJECT_MAX - 4)
        self.assertEqual(ccm.problems(at_limit + "\n"), [])
        self.assertTrue(ccm.problems(at_limit + "x\n"))

    def test_trailing_period_fails(self):
        self.assertIn("subject ends with a period", ccm.problems("ci: run it.\n"))

    def test_long_body_line_fails_but_a_bare_url_passes(self):
        long_prose = "word " * 20
        url = "https://example.invalid/" + "a" * 90
        found = ccm.problems(f"ci: x\n\n{long_prose}\n{url}\n")
        self.assertEqual(len(found), 1)
        self.assertIn("line 3", found[0])

    def test_generated_messages_are_exempt(self):
        for subject in ("Merge branch 'dev'", 'Revert "ci: x"', "fixup! ci: x"):
            with self.subTest(subject=subject):
                self.assertEqual(ccm.problems(subject + "\n"), [])

    def test_empty_message_fails(self):
        self.assertEqual(ccm.problems(""), ["the message is empty"])


class Main(unittest.TestCase):
    def run_main(self, text, *flags):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "COMMIT_EDITMSG"
            path.write_text(text, encoding="utf-8")
            code = ccm.main([*flags, str(path)])
            return code, path.read_text(encoding="utf-8")

    def test_fix_rewrites_and_accepts(self):
        code, text = self.run_main("ci: x  \nbody\n", "--fix")
        self.assertEqual((code, text), (0, "ci: x\n\nbody\n"))

    def test_without_fix_the_file_is_untouched(self):
        code, text = self.run_main("ci: x  \nbody\n")
        self.assertEqual((code, text), (0, "ci: x  \nbody\n"))

    def test_rejection_exits_1(self):
        code, _ = self.run_main("no area here\n", "--fix")
        self.assertEqual(code, 1)


if __name__ == "__main__":
    unittest.main()
