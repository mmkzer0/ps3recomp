#!/usr/bin/env python3
"""Contract tests for tools/spu_xdiff_corpus.py.

A fake supervisor, written into a temporary directory, maps each fixture's content to a
canned verdict object and exit status, so every corpus shape here is assembled without a
title, a fixture binary or the real runner. The properties under test are the report and
exit contract of docs/SPU_XDIFF.md sections 8.3, 8.4 and 13: the verdict sums, the differ
class split, the exit precedence, a diagnostic that is never dropped, zstd fixtures that
are decoded into a private file and cleaned up, and a report that does not depend on
--jobs.
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import spu_xdiff_corpus as corpus

TOOL = TOOLS / "spu_xdiff_corpus.py"

# Fingerprints sort in this order, and a real one is 16 uppercase hex digits.
IMAGE_A = "0A0B0C0D0E0F1011"
IMAGE_B = "1A1B1C1D1E1F2021"
IMAGE_C = "2A2B2C2D2E2F3031"

FAKE_RUNNER = '''#!/usr/bin/env python3
"""A spu_job_replay stand-in: one canned case per fixture, keyed by the file's first line."""
import json
import os
import sys

argv = sys.argv[1:]
log = os.environ.get("XD_CORPUS_LOG")
if log:
    with open(log, "a", encoding="utf-8") as handle:
        handle.write(json.dumps(argv) + "\\n")
if "--json" not in argv:
    sys.stderr.write("fake runner: --json is missing\\n")
    sys.exit(1)
fixture = argv[-1]
with open(fixture, "rb") as handle:
    name = handle.read().decode("utf-8").splitlines()[0]
here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "cases.json"), encoding="utf-8") as handle:
    case = json.load(handle)[name]
if "stdout" in case:
    sys.stdout.write(case["stdout"])
else:
    document = case["json"]
    document["exit_code"] = case.get("json_exit", document["exit_code"])
    sys.stdout.write(json.dumps(document))
sys.stderr.write(case.get("stderr", ""))
sys.exit(case.get("exit", 0))
'''


def verdict_json(verdict, reason, kind="none", image=IMAGE_A, first=None):
    """A verdict object with the fields the driver reads, shaped like section 13's."""
    code = {"equal": 0, "malformed": 2, "not-found": 3, "differ": 4, "void": 5}[verdict]
    return {"schema": "ps3recomp.spujob-verdict/1", "tool": "spu_job_replay", "version": 1,
            "verdict": verdict, "exit_code": code, "reason": reason, "match": "same",
            "class": kind,
            "fixture": {"path": "", "image_fingerprint": image, "authority_tier": "interp",
                        "effects": 1, "toolkit_commit": "0" * 40},
            "subject": {"tier": "registered", "lifter_abi": 1, "code_end": 64,
                        "lift_options": "0" * 16, "native_rev": 0},
            "first": first, "budget": {"limit": None, "spent": 1}, "seconds": 0.001}


def counts(fixtures=0, equal=0, visible=0, latent=0, void=0, malformed=0, not_found=0,
           driver_errors=0, reasons=None):
    """The count block the report repeats for the corpus and for one image."""
    return {"fixtures": fixtures, "equal": equal,
            "differ": {"visible": visible, "latent": latent},
            "void": void, "malformed": malformed, "not-found": not_found,
            "driver_errors": driver_errors, "reasons": reasons or {}}


class CorpusDriver(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.corpus = self.root / "corpus"
        self.corpus.mkdir()
        self.runner = self.root / "fake_runner.py"
        self.runner.write_text(FAKE_RUNNER, encoding="utf-8")
        self.runner.chmod(0o755)
        self.cases = {}
        self.log = self.root / "calls.ndjson"

    # -- corpus assembly ---------------------------------------------------------------

    def case(self, name, *, verdict=None, reason=None, kind="none", image=IMAGE_A, first=None,
             stdout=None, stderr=None, exit=None, json_exit=None):
        spec = {}
        if verdict is not None:
            spec["json"] = verdict_json(verdict, reason, kind, image, first)
            if exit is None:
                exit = spec["json"]["exit_code"]
        if stdout is not None:
            spec["stdout"] = stdout
        if stderr is not None:
            spec["stderr"] = stderr
        if exit is not None:
            spec["exit"] = exit
        if json_exit is not None:
            spec["json_exit"] = json_exit
        self.cases[name] = spec

    def fixture(self, relative, case, *, data=None, corpus_dir=None):
        path = (corpus_dir or self.corpus) / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes((case + "\n").encode() if data is None else data)
        return path

    def run_driver(self, *options, corpus_dir=None):
        (self.root / "cases.json").write_text(json.dumps(self.cases), encoding="utf-8")
        self.log.unlink(missing_ok=True)
        environment = dict(os.environ, XD_CORPUS_LOG=str(self.log))
        return subprocess.run([sys.executable, str(TOOL), "--runner", str(self.runner),
                               *options, str(corpus_dir or self.corpus)],
                              capture_output=True, text=True, env=environment, check=False)

    def run_tool(self, *arguments):
        return subprocess.run([sys.executable, str(TOOL), *map(str, arguments)],
                              capture_output=True, text=True, check=False)

    def calls(self):
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text(encoding="utf-8").splitlines()]

    def summary(self, *options, corpus_dir=None):
        result = self.run_driver("--json", *options, corpus_dir=corpus_dir)
        self.assertIn(result.returncode, (0, 2, 3, 4, 5), result.stderr)
        return result, json.loads(result.stdout)

    def compress(self, data):
        backend = corpus.zstd_backend()
        if backend is None:
            self.skipTest("no zstd decoder: " + corpus.ZSTD_NO_BACKEND)
        name, handle = backend
        if name == "stdlib":
            return handle.compress(data)
        return subprocess.run([handle, "-c", "-q"], input=data, capture_output=True,
                              check=True).stdout

    # -- verdicts and the exit contract ------------------------------------------------

    def test_all_equal_is_zero(self):
        self.case("one", verdict="equal", reason="none", image=IMAGE_A)
        self.case("two", verdict="equal", reason="none", image=IMAGE_B)
        self.fixture("a.spujob", "one")
        self.fixture("nested/b.spujob", "two")
        result, doc = self.summary()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(doc["totals"], {**counts(fixtures=2, equal=2,
                                                  reasons={"none": 2}),
                                         "not_run": 0, "exit": 0})
        self.assertEqual([row["path"] for row in doc["fixtures"]], ["a.spujob", "nested/b.spujob"])
        self.assertEqual([row["verdict"] for row in doc["fixtures"]], ["equal", "equal"])
        self.assertEqual(len(doc["images"]), 2)

    def test_differ_class_split_and_first_differ(self):
        self.case("latent", verdict="differ", reason="end-regs", kind="latent", image=IMAGE_A,
                  first={"effect_index": None, "where": 128})
        self.case("visible", verdict="differ", reason="effect-payload", kind="visible",
                  image=IMAGE_A, first={"effect_index": 3, "where": 64})
        self.fixture("a-latent.spujob", "latent")
        self.fixture("b-visible.spujob", "visible")
        result, doc = self.summary()
        self.assertEqual(result.returncode, 4, result.stderr)
        image = doc["images"][0]
        self.assertEqual(image["fingerprint"], IMAGE_A)
        self.assertEqual(image["differ"], {"visible": 1, "latent": 1})
        self.assertEqual(image["fixtures"], 2)
        self.assertEqual(image["first_differ"],
                         {"path": "a-latent.spujob", "effect_index": None,
                          "reason": "end-regs", "where": 128})
        self.assertEqual(doc["totals"]["differ"], {"visible": 1, "latent": 1})
        self.assertEqual(doc["totals"]["exit"], 4)
        self.assertEqual([row["class"] for row in doc["fixtures"]], ["latent", "visible"])

    def test_only_void_is_five(self):
        self.case("void", verdict="void", reason="budget", image=IMAGE_A)
        self.fixture("a.spujob", "void")
        result, doc = self.summary()
        self.assertEqual(result.returncode, 5, result.stderr)
        self.assertEqual(doc["totals"]["void"], 1)
        self.assertEqual(doc["totals"]["exit"], 5)

    def test_only_not_found_is_three(self):
        self.case("missing", verdict="not-found", reason="no-body", image=IMAGE_A)
        self.fixture("a.spujob", "missing")
        result, doc = self.summary()
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertEqual(doc["totals"]["not-found"], 1)
        self.assertEqual(doc["totals"]["exit"], 3)

    def test_malformed_is_two(self):
        self.case("broken", verdict="malformed", reason="fixture-format", image=IMAGE_A)
        self.fixture("a.spujob", "broken")
        result, doc = self.summary()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(doc["totals"]["malformed"], 1)
        self.assertEqual(doc["totals"]["exit"], 2)

    def test_verdict_without_a_readable_image_is_still_counted(self):
        self.case("anonymous", verdict="malformed", reason="fixture-format", image=None)
        self.fixture("a.spujob", "anonymous")
        self.case("numeric", verdict="equal", reason="none", image=42)
        self.fixture("b.spujob", "numeric")
        result, doc = self.summary()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(doc["totals"]["malformed"], 1)
        self.assertEqual(doc["totals"]["equal"], 1)
        self.assertEqual([row["image"] for row in doc["fixtures"]],
                         [None, "000000000000002A"])
        self.assertEqual([entry["fingerprint"] for entry in doc["images"]],
                         ["000000000000002A", None])

    # -- diagnostics are rows, never dropped -------------------------------------------

    def test_garbage_is_a_named_driver_error(self):
        self.case("equal", verdict="equal", reason="none", image=IMAGE_A)
        self.fixture("a.spujob", "equal")
        self.case("garbage", stdout="this is not json\n", stderr="fake runner exploded\n", exit=4)
        self.fixture("b-garbage.spujob", "garbage")
        text = self.run_driver()
        self.assertEqual(text.returncode, 2, text.stderr)
        self.assertIn("driver error b-garbage.spujob: bad-json:", text.stdout)
        self.assertIn("fake runner exploded", text.stdout)
        self.assertIn("driver errors 1", text.stdout)
        result, doc = self.summary()
        self.assertEqual(result.returncode, 2)
        row = next(item for item in doc["fixtures"] if item["verdict"] == "driver-error")
        self.assertEqual(row["path"], "b-garbage.spujob")
        self.assertIsNone(row["reason"])
        self.assertIsNone(row["class"])
        self.assertIsNone(row["image"])
        self.assertEqual(row["error"]["cause"], "bad-json")
        self.assertIn("fake runner exploded", row["error"]["detail"])
        self.assertEqual(doc["totals"]["driver_errors"], 1)
        self.assertEqual(doc["totals"]["equal"], 1)
        unattributed = [image for image in doc["images"] if image["fingerprint"] is None]
        self.assertEqual(len(unattributed), 1)
        self.assertEqual(unattributed[0]["driver_errors"], 1)

    def test_exit_code_disagreement_is_a_driver_error(self):
        self.case("mismatch", verdict="differ", reason="effect-payload", kind="visible",
                  first={"effect_index": 3, "where": 64}, exit=0, json_exit=4)
        self.fixture("a.spujob", "mismatch")
        result, doc = self.summary()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(doc["totals"]["driver_errors"], 1)
        self.assertEqual(doc["totals"]["differ"], {"visible": 0, "latent": 0})
        row = doc["fixtures"][0]
        self.assertEqual(row["error"]["cause"], "exit-mismatch")
        self.assertIn("the object reports exit_code 4, the process exited 0", row["error"]["detail"])

    def test_bad_class_is_a_driver_error(self):
        self.case("wrong-class", verdict="differ", reason="effect-payload", kind="none",
                  first={"effect_index": 3, "where": 64})
        self.fixture("a.spujob", "wrong-class")
        result, doc = self.summary()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(doc["totals"]["driver_errors"], 1)
        self.assertIn("a differ needs class visible or latent",
                      doc["fixtures"][0]["error"]["detail"])

    def test_keep_going_controls_the_remaining_fixtures(self):
        self.case("empty-output", stdout="", stderr="fake runner exploded\n", exit=4)
        self.fixture("a-garbage.spujob", "empty-output")
        self.case("equal", verdict="equal", reason="none")
        self.fixture("b-equal.spujob", "equal")
        result, doc = self.summary()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(doc["totals"]["not_run"], 1)
        self.assertEqual([row["path"] for row in doc["fixtures"]], ["a-garbage.spujob"])
        self.assertEqual(doc["fixtures"][0]["error"]["cause"], "no-output")
        result, doc = self.summary("--keep-going")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(doc["totals"]["not_run"], 0)
        self.assertEqual(doc["totals"]["equal"], 1)
        self.assertEqual(len(doc["fixtures"]), 2)

    # -- compressed fixtures -----------------------------------------------------------

    def test_compressed_fixture_is_decoded_and_removed(self):
        self.case("equal", verdict="equal", reason="none", image=IMAGE_A)
        self.case("crash", stdout="", stderr="fake runner exploded\n", exit=9)
        compressed = self.compress(b"equal\n")
        self.assertTrue(compressed.startswith(corpus.ZSTD_MAGIC))
        first = self.fixture("a-compressed.spujob", "", data=compressed)
        second = self.fixture("nested/b-compressed-without-extension", "",
                              data=self.compress(b"crash\n"))
        result, doc = self.summary("--keep-going")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(doc["totals"]["equal"], 1)
        self.assertEqual(doc["totals"]["driver_errors"], 1)
        calls = self.calls()
        self.assertEqual(len(calls), 2)
        for call in calls:
            decoded = Path(call[-1])
            self.assertNotEqual(decoded, first)
            self.assertNotEqual(decoded, second)
            self.assertFalse(decoded.exists(), f"decoded fixture left behind: {decoded}")
            self.assertNotEqual(decoded.parent, self.corpus)
            self.assertFalse(decoded.parent.exists(),
                             f"private temporary directory left behind: {decoded.parent}")
        self.assertTrue(first.exists(), "the corpus fixture must not be consumed")
        self.assertTrue(second.exists(), "the corpus fixture must not be consumed")

    # -- determinism, emptiness, usage -------------------------------------------------

    def test_jobs_does_not_change_the_report(self):
        self.case("equal", verdict="equal", reason="none", image=IMAGE_A)
        self.case("visible", verdict="differ", reason="effect-payload", kind="visible",
                  image=IMAGE_A, first={"effect_index": 3, "where": 64})
        self.case("latent", verdict="differ", reason="end-regs", kind="latent", image=IMAGE_B,
                  first={"effect_index": None, "where": 128})
        self.case("void", verdict="void", reason="budget", image=IMAGE_A)
        self.case("malformed", verdict="malformed", reason="fixture-format", image=IMAGE_B)
        self.case("missing", verdict="not-found", reason="no-body", image=IMAGE_C)
        for name, case in (("a-equal.spujob", "equal"),
                           ("b-visible.spujob", "visible"),
                           ("c-latent.spujob", "latent"),
                           ("d-void.spujob", "void"),
                           ("e-malformed.spujob", "malformed"),
                           ("f-notfound.spujob", "missing")):
            self.fixture(name, case)
        one = self.run_driver("--json", "--jobs", "1")
        four = self.run_driver("--json", "--jobs", "4")
        self.assertEqual(one.returncode, four.returncode)
        self.assertEqual(one.stdout, four.stdout)
        self.assertEqual(one.returncode, 4, one.stderr)
        self.assertEqual(len(json.loads(one.stdout)["fixtures"]), 6)

    def test_empty_corpus_is_not_found(self):
        result = self.run_driver()
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn("0 fixtures, 0 images", result.stdout)
        self.assertIn("no fixture found", result.stdout)
        result, doc = self.summary()
        self.assertEqual(result.returncode, 3)
        self.assertEqual(doc["fixtures"], [])
        self.assertEqual(doc["images"], [])
        self.assertEqual(doc["totals"], {**counts(), "not_run": 0, "exit": 3})

    def test_usage_errors_exit_one(self):
        result = self.run_tool(self.corpus)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("--runner", result.stderr)
        result = self.run_tool("--runner", self.root / "missing", self.corpus)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("no such executable file", result.stderr)
        result = self.run_tool("--runner", self.runner, self.root / "missing")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("no such directory", result.stderr)
        result = self.run_tool("--runner", self.runner, "--jobs", "0", self.corpus)
        self.assertEqual(result.returncode, 1, result.stderr)
        result = self.run_tool("--runner", self.runner, "--tier", "wat", self.corpus)
        self.assertEqual(result.returncode, 1, result.stderr)

    # -- the JSON and text contracts ---------------------------------------------------

    def test_json_summary_field_by_field(self):
        self.case("equal", verdict="equal", reason="none", image=IMAGE_A)
        self.case("visible", verdict="differ", reason="effect-payload", kind="visible",
                  image=IMAGE_A, first={"effect_index": 3, "where": 64})
        self.case("latent", verdict="differ", reason="end-regs", kind="latent", image=IMAGE_B,
                  first={"effect_index": None, "where": 128})
        self.case("void", verdict="void", reason="budget", image=IMAGE_A)
        self.case("malformed", verdict="malformed", reason="fixture-format", image=IMAGE_B)
        self.case("missing", verdict="not-found", reason="no-body", image=IMAGE_C)
        self.case("garbage", stdout="this is not json\n", stderr="fake runner exploded\n", exit=4)
        for name, case in (("a-differ-visible.spujob", "visible"),
                           ("b-differ-latent.spujob", "latent"),
                           ("c-void.spujob", "void"),
                           ("d-malformed.spujob", "malformed"),
                           ("e-not-found.spujob", "missing"),
                           ("f-garbage.spujob", "garbage"),
                           ("z-equal.spujob", "equal")):
            self.fixture(name, case)
        result, doc = self.summary("--keep-going")
        self.assertEqual(result.returncode, 4, result.stderr)
        self.assertEqual(set(doc), {"schema", "tool", "version", "corpus", "config",
                                    "images", "fixtures", "totals"})
        self.assertEqual(doc["schema"], "ps3recomp.spujob-corpus/1")
        self.assertEqual(doc["tool"], "spu_xdiff_corpus")
        self.assertEqual(doc["version"], 1)
        self.assertEqual(doc["corpus"], str(self.corpus))
        self.assertEqual(doc["config"], {"runner": str(self.runner), "tier": None,
                                         "budget": None, "timeout": None})
        expected_rows = [
            {"path": "a-differ-visible.spujob", "verdict": "differ",
             "reason": "effect-payload", "class": "visible", "image": IMAGE_A},
            {"path": "b-differ-latent.spujob", "verdict": "differ",
             "reason": "end-regs", "class": "latent", "image": IMAGE_B},
            {"path": "c-void.spujob", "verdict": "void", "reason": "budget",
             "class": "none", "image": IMAGE_A},
            {"path": "d-malformed.spujob", "verdict": "malformed", "reason": "fixture-format",
             "class": "none", "image": IMAGE_B},
            {"path": "e-not-found.spujob", "verdict": "not-found", "reason": "no-body",
             "class": "none", "image": IMAGE_C},
            {"path": "f-garbage.spujob", "verdict": "driver-error", "reason": None,
             "class": None, "image": None},
            {"path": "z-equal.spujob", "verdict": "equal", "reason": "none",
             "class": "none", "image": IMAGE_A},
        ]
        rows = doc["fixtures"]
        self.assertEqual([{key: value for key, value in row.items() if key != "error"}
                          for row in rows], expected_rows)
        self.assertEqual(rows[5]["error"]["cause"], "bad-json")
        self.assertIn("fake runner exploded", rows[5]["error"]["detail"])
        self.assertEqual(doc["images"], [
            {"fingerprint": IMAGE_A, **counts(fixtures=3, equal=1, visible=1, void=1,
                                              reasons={"budget": 1, "effect-payload": 1,
                                                       "none": 1}),
             "first_differ": {"path": "a-differ-visible.spujob", "effect_index": 3,
                              "reason": "effect-payload", "where": 64}},
            {"fingerprint": IMAGE_B, **counts(fixtures=2, latent=1, malformed=1,
                                              reasons={"end-regs": 1, "fixture-format": 1}),
             "first_differ": {"path": "b-differ-latent.spujob", "effect_index": None,
                              "reason": "end-regs", "where": 128}},
            {"fingerprint": IMAGE_C, **counts(fixtures=1, not_found=1,
                                              reasons={"no-body": 1}),
             "first_differ": None},
            {"fingerprint": None, **counts(fixtures=1, driver_errors=1),
             "first_differ": None},
        ])
        self.assertEqual(doc["totals"], {
            **counts(fixtures=7, equal=1, visible=1, latent=1, void=1, malformed=1,
                     not_found=1, driver_errors=1,
                     reasons={"budget": 1, "effect-payload": 1, "end-regs": 1,
                              "fixture-format": 1, "no-body": 1, "none": 1}),
            "not_run": 0, "exit": 4})

    def test_text_report_names_first_differ_and_driver_error(self):
        self.case("visible", verdict="differ", reason="effect-payload", kind="visible",
                  image=IMAGE_A, first={"effect_index": 3, "where": 64})
        self.fixture("a-differ.spujob", "visible")
        self.case("garbage", stdout="this is not json\n", stderr="fake runner exploded\n", exit=4)
        self.fixture("z-garbage.spujob", "garbage")
        result = self.run_driver()
        self.assertEqual(result.returncode, 4, result.stderr)
        self.assertIn(f"corpus {self.corpus}: 2 fixtures, 1 image", result.stdout)
        self.assertIn("differ 1 (visible 1, latent 0)", result.stdout)
        self.assertIn("driver errors 1", result.stdout)
        self.assertIn("per image", result.stdout)
        self.assertIn("per reason", result.stdout)
        self.assertIn("effect-payload 1", result.stdout)
        self.assertIn("first differ per image", result.stdout)
        self.assertIn(f"  {IMAGE_A}  a-differ.spujob  effect 3  effect-payload  at 64",
                      result.stdout)
        self.assertIn("driver error z-garbage.spujob: bad-json:", result.stdout)
        self.assertIn("fake runner exploded", result.stdout)

    def test_exit_precedence(self):
        self.case("visible", verdict="differ", reason="effect-payload", kind="visible",
                  first={"effect_index": 3, "where": 64})
        self.case("garbage", stdout="garbage\n", exit=4)
        self.case("void", verdict="void", reason="budget")
        self.case("malformed", verdict="malformed", reason="fixture-format")
        self.case("missing", verdict="not-found", reason="no-body")
        differ_and_error = self.root / "differ-and-error"
        differ_and_error.mkdir()
        self.fixture("a-differ.spujob", "visible", corpus_dir=differ_and_error)
        self.fixture("b-garbage.spujob", "garbage", corpus_dir=differ_and_error)
        result, doc = self.summary(corpus_dir=differ_and_error)
        self.assertEqual(result.returncode, 4, result.stderr)
        self.assertEqual(doc["totals"]["driver_errors"], 1)
        self.assertEqual(doc["totals"]["differ"], {"visible": 1, "latent": 0})
        malformed_and_void = self.root / "malformed-and-void"
        malformed_and_void.mkdir()
        self.fixture("a-void.spujob", "void", corpus_dir=malformed_and_void)
        self.fixture("b-malformed.spujob", "malformed", corpus_dir=malformed_and_void)
        result, doc = self.summary(corpus_dir=malformed_and_void)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(doc["totals"]["malformed"], 1)
        self.assertEqual(doc["totals"]["void"], 1)
        void_and_missing = self.root / "void-and-missing"
        void_and_missing.mkdir()
        self.fixture("a-void.spujob", "void", corpus_dir=void_and_missing)
        self.fixture("b-missing.spujob", "missing", corpus_dir=void_and_missing)
        result, doc = self.summary(corpus_dir=void_and_missing)
        self.assertEqual(result.returncode, 5, result.stderr)
        self.assertEqual(doc["totals"]["not-found"], 1)

    def test_forwarded_runner_arguments(self):
        self.case("equal", verdict="equal", reason="none")
        path = self.fixture("a.spujob", "equal")
        result = self.run_driver("--tier", "registered", "--budget", "7", "--timeout", "2.5",
                                 "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), [["--tier", "registered", "--budget", "7",
                                         "--timeout", "2.5", "--json", str(path)]])
        result = self.run_driver("--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), [["--json", str(path)]])


if __name__ == "__main__":
    unittest.main()
