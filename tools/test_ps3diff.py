#!/usr/bin/env python3
"""Contract tests for tools/ps3diff.py.

Everything here is synthetic: the fixtures under ps3diff/fixtures/ carry
invented labels, hashes of invented bytes, and no game or firmware material.
The real traces in odin-sphere-m0 are used only for the local T1 round-trip,
never as a fixture.

The properties under test are the A4 gate: event/1 converts losslessly,
validation catches a broken per-context order, and diff aligns per context
and reports exactly the first divergence instead of guessing.
"""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
TOOL = HERE / "ps3diff.py"
FIXTURES = HERE / "ps3diff" / "fixtures"

V1 = "rpcs3-gdb-trace.event/1"
V2 = "ps3diff.event/2"
REPORT = "ps3diff.report/1"


def run_tool(*args):
    return subprocess.run([sys.executable, str(TOOL), *map(str, args)],
                          capture_output=True, text=True)


def read_records(path):
    return [json.loads(line) for line in
            Path(path).read_text(encoding="utf-8").splitlines()]


def write_records(path, records):
    path = Path(path)
    path.write_text("".join(json.dumps(rec, separators=(",", ":")) + "\n"
                            for rec in records), encoding="utf-8")
    return path


def write_json(path, value):
    path = Path(path)
    path.write_text(json.dumps(value, indent=1) + "\n", encoding="utf-8")
    return path


def base_v2():
    return read_records(FIXTURES / "v2-basic.ndjson")


class ToolTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="ps3diff-test-")
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def assert_exit(self, result, expected):
        self.assertEqual(
            result.returncode, expected,
            f"exit {result.returncode}, expected {expected}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")

    def path(self, name):
        return self.tmp / name


class Convert(ToolTestCase):
    def test_round_trip_is_byte_identical(self):
        v2 = self.path("trace.v2.ndjson")
        v1 = self.path("trace.v1.ndjson")
        result = run_tool("convert", "--to", 2, FIXTURES / "v1-basic.ndjson",
                          "-o", v2)
        self.assert_exit(result, 0)
        self.assertEqual(v2.read_bytes(),
                         (FIXTURES / "v2-basic.ndjson").read_bytes(),
                         "the v2 fixture pins the converter's output bytes")

        result = run_tool("convert", "--to", 1, v2, "-o", v1)
        self.assert_exit(result, 0)
        self.assertEqual(v1.read_bytes(),
                         (FIXTURES / "v1-basic.ndjson").read_bytes())

    def test_round_trip_keeps_intervened_unicode_and_error_dumps(self):
        v2 = self.path("trace.v2.ndjson")
        self.assert_exit(run_tool("convert", "--to", 2,
                                  FIXTURES / "v1-basic.ndjson", "-o", v2), 0)
        records = read_records(v2)

        intervened = next(rec for rec in records if "intervened" in rec)
        self.assertEqual(intervened["intervened"], {"r3": "0x3"})
        self.assertEqual(intervened["ctx"], {"kind": "ppu",
                                             "id": intervened["thread"]})

        unicode_record = next(rec for rec in records
                              if rec["label"] == "\u540c\u671f\u30e9\u30d9\u30eb")
        self.assertEqual(unicode_record["event"], "call")
        self.assertEqual(unicode_record["source"],
                         {"producer": "rpcs3_gdb_trace", "schema": V1})

        dumps = records[0]["dumps"]
        self.assertEqual(dumps[1], {"name": "bad_len", "ea": "0x00100010",
                                    "len": 0,
                                    "error": "length 0 outside 1..4096"})
        self.assertEqual(dumps[2], {"name": "unreadable_ptr",
                                    "error": "pointer at 0x00100020 unreadable"})

        counters = {}
        for rec in records:
            key = f"{rec['ctx']['kind']}:{rec['ctx']['id']}"
            self.assertEqual(rec["ctx_seq"], counters.get(key, 0))
            counters[key] = counters.get(key, 0) + 1

    def test_noncanonical_line_is_refused_with_its_number(self):
        out = self.path("never.ndjson")
        result = run_tool("convert", "--to", 2,
                          FIXTURES / "v1-noncanonical.ndjson", "-o", out)
        self.assert_exit(result, 2)
        self.assertIn("line 2", result.stderr)
        self.assertIn("not canonical JSON", result.stderr)
        self.assertFalse(out.exists(), "no output file on a refused input")

    def test_new_kinds_and_payload_refuse_downgrade(self):
        out = self.path("never.ndjson")
        result = run_tool("convert", "--to", 1, FIXTURES / "v2-kinds.ndjson",
                          "-o", out)
        self.assert_exit(result, 2)
        self.assertIn("line 1", result.stderr)
        self.assertIn("state_hash", result.stderr)
        self.assertIn("event/1", result.stderr)
        self.assertFalse(out.exists())

    def test_mixed_schema_file_is_refused(self):
        mixed = self.path("mixed.ndjson")
        first = (FIXTURES / "v1-basic.ndjson").read_text(encoding="utf-8").splitlines()[0]
        second = (FIXTURES / "v2-basic.ndjson").read_text(encoding="utf-8").splitlines()[0]
        mixed.write_text(first + "\n" + second + "\n", encoding="utf-8")
        result = run_tool("convert", "--to", 2, mixed, "-o", self.path("out"))
        self.assert_exit(result, 2)
        self.assertIn("line 2", result.stderr)
        self.assertIn("mixes with", result.stderr)


class Validate(ToolTestCase):
    def test_valid_schemas(self):
        for name in ("v1-basic.ndjson", "v1-noncanonical.ndjson",
                     "v2-basic.ndjson", "v2-kinds.ndjson"):
            with self.subTest(name=name):
                result = run_tool("validate", FIXTURES / name)
                self.assert_exit(result, 0)
                self.assertIn("valid:", result.stdout)

    def test_consistent_dump_hex_and_sha_pass(self):
        records = base_v2()
        dump = records[0]["dumps"][0]
        dump["hex"] = "ff00ff00ff00ff00ff00ff00ff00ff00"
        dump["sha256"] = hashlib.sha256(bytes.fromhex(dump["hex"])).hexdigest()
        consistent = write_records(self.path("consistent.ndjson"), records)
        result = run_tool("validate", consistent)
        self.assert_exit(result, 0)

    def test_dump_hex_sha_mismatch_names_the_dump(self):
        records = base_v2()
        dump = records[0]["dumps"][0]
        dump["hex"] = "1" + dump["hex"][1:]
        broken = write_records(self.path("sha-mismatch.ndjson"), records)
        result = run_tool("validate", broken)
        self.assert_exit(result, 2)
        self.assertIn("line 1", result.stderr)
        self.assertIn("dumps[0].hex", result.stderr)
        self.assertIn("'in'", result.stderr)
        self.assertIn("seq 0", result.stderr)
        self.assertIn("ppu:0000000001000000", result.stderr)
        self.assertIn("not the dump's", result.stderr)

    def test_dump_hex_shorter_than_read_names_the_dump(self):
        records = base_v2()
        dump = records[0]["dumps"][0]
        dump["hex"] = dump["hex"][:-2]
        broken = write_records(self.path("short-hex.ndjson"), records)
        result = run_tool("validate", broken)
        self.assert_exit(result, 2)
        self.assertIn("'in'", result.stderr)
        self.assertIn("15 bytes", result.stderr)
        self.assertIn("read says 16", result.stderr)

    def test_dump_hex_odd_length_names_the_dump(self):
        records = base_v2()
        dump = records[0]["dumps"][0]
        dump["hex"] = dump["hex"][:-1]
        broken = write_records(self.path("odd-hex.ndjson"), records)
        result = run_tool("validate", broken)
        self.assert_exit(result, 2)
        self.assertIn("'in'", result.stderr)
        self.assertIn("odd", result.stderr)

    def test_dump_hex_uppercase_names_the_dump(self):
        records = base_v2()
        dump = records[0]["dumps"][0]
        dump["hex"] = dump["hex"].upper()
        broken = write_records(self.path("upper-hex.ndjson"), records)
        result = run_tool("validate", broken)
        self.assert_exit(result, 2)
        self.assertIn("'in'", result.stderr)
        self.assertIn("lowercase hex digits", result.stderr)

    def test_broken_ctx_seq(self):
        records = base_v2()
        records[0]["ctx_seq"] = 1
        broken = write_records(self.path("broken.ndjson"), records)
        result = run_tool("validate", broken)
        self.assert_exit(result, 2)
        self.assertIn("line 1", result.stderr)
        self.assertIn("ctx_seq", result.stderr)
        self.assertIn("ordering", result.stderr)

    def test_dangling_call_seq(self):
        records = base_v2()
        ret = next(rec for rec in records if rec["event"] == "ret")
        ret["call_seq"] = 999
        broken = write_records(self.path("dangling.ndjson"), records)
        result = run_tool("validate", broken)
        self.assert_exit(result, 2)
        self.assertIn("call_seq", result.stderr)
        self.assertIn("not an earlier record", result.stderr)

    def test_call_seq_must_name_a_call(self):
        records = base_v2()
        rets = [rec for rec in records
                if rec["event"] == "ret" and rec["ctx"]["id"].endswith("1000000")]
        rets[1]["call_seq"] = rets[0]["seq"]
        broken = write_records(self.path("wrong-kind.ndjson"), records)
        result = run_tool("validate", broken)
        self.assert_exit(result, 2)
        self.assertIn("expected a call", result.stderr)

    def test_unknown_field(self):
        records = base_v2()
        records[0]["surprise"] = 1
        broken = write_records(self.path("unknown.ndjson"), records)
        result = run_tool("validate", broken)
        self.assert_exit(result, 2)
        self.assertIn("surprise", result.stderr)
        self.assertIn("unknown field", result.stderr)

    def test_x_extension_namespace_is_allowed(self):
        records = base_v2()
        records[0]["x-note"] = "extension"
        extended = write_records(self.path("extended.ndjson"), records)
        self.assert_exit(run_tool("validate", extended), 0)


class Diff(ToolTestCase):
    def diff_json(self, a, b, *extra):
        result = run_tool("diff", a, b, "--json", *extra)
        return result, (json.loads(result.stdout) if result.stdout else None)

    def test_identical_runs_exit_zero_with_counts(self):
        result, report = self.diff_json(FIXTURES / "v2-basic.ndjson",
                                        FIXTURES / "v2-basic.ndjson")
        self.assert_exit(result, 0)
        self.assertEqual(report["counts"],
                         {"compared": 7, "contexts_a": 2, "contexts_b": 2,
                          "contexts_matched": 2})
        self.assertTrue(report["identical"])
        self.assertIsNone(report["first"])

    def test_event1_input_diffs_against_itself(self):
        result, report = self.diff_json(FIXTURES / "v1-basic.ndjson",
                                        FIXTURES / "v1-basic.ndjson")
        self.assert_exit(result, 0)
        self.assertEqual(report["counts"]["compared"], 7)
        self.assertEqual(report["counts"]["contexts_matched"], 2)

    def test_event1_and_its_conversion_are_equivalent(self):
        result, report = self.diff_json(FIXTURES / "v1-basic.ndjson",
                                        FIXTURES / "v2-basic.ndjson")
        self.assert_exit(result, 0)
        self.assertEqual(report["counts"]["compared"], 7)
        self.assertTrue(report["identical"])

    def test_changed_ret_reports_exactly_that_record(self):
        records = base_v2()
        records[2]["ret"] = "0x00000002"
        changed = write_records(self.path("changed.ndjson"), records)
        result, report = self.diff_json(FIXTURES / "v2-basic.ndjson", changed)
        self.assert_exit(result, 1)
        self.assertEqual(report["first"], {
            "ctx": "ppu:0000000001000000",
            "ctx_seq": 1,
            "label": "synthetic_call_a",
            "path": "ret",
            "a": "0x00000001",
            "b": "0x00000002",
        })
        self.assertEqual(report["contexts"][0]["compared"], 5)
        self.assertIsNone(report["contexts"][1]["first"])

        text = run_tool("diff", FIXTURES / "v2-basic.ndjson", changed)
        self.assert_exit(text, 1)
        self.assertIn("field=ret", text.stdout)
        self.assertIn("A: 0x00000001", text.stdout)
        self.assertIn("B: 0x00000002", text.stdout)

    def test_masked_field_does_not_diverge(self):
        records = base_v2()
        call = next(rec for rec in records if rec["event"] == "call"
                    and rec["args"]["r4"] == "0x10000")
        call["args"]["r4"] = "0x99"
        changed = write_records(self.path("masked.ndjson"), records)

        unmasked, report = self.diff_json(FIXTURES / "v2-basic.ndjson", changed)
        self.assert_exit(unmasked, 1)
        self.assertEqual(report["first"]["path"], "args.r4")

        masked, report = self.diff_json(FIXTURES / "v2-basic.ndjson", changed,
                                        "--mask", FIXTURES / "mask.json")
        self.assert_exit(masked, 0)
        self.assertTrue(report["identical"])
        self.assertEqual(report["counts"]["compared"], 7)

    def test_mask_normalises_an_address(self):
        records = base_v2()
        call = next(rec for rec in records if rec["event"] == "call"
                    and rec["args"]["r4"] == "0x10000")
        call["args"]["r4"] = "0x010000"
        changed = write_records(self.path("normalize.ndjson"), records)

        unmasked = run_tool("diff", FIXTURES / "v2-basic.ndjson", changed)
        self.assert_exit(unmasked, 1)
        masked = run_tool("diff", FIXTURES / "v2-basic.ndjson", changed,
                          "--mask", FIXTURES / "mask-normalize.json")
        self.assert_exit(masked, 0)

    def test_missing_tail_is_a_divergence(self):
        records = [rec for rec in base_v2() if rec["seq"] != 6]
        short = write_records(self.path("short.ndjson"), records)
        result, report = self.diff_json(FIXTURES / "v2-basic.ndjson", short)
        self.assert_exit(result, 1)
        self.assertEqual(report["counts"]["compared"], 6)
        self.assertEqual(report["first"], {
            "ctx": "ppu:0000000001000000",
            "ctx_seq": 4,
            "label": "synthetic_call_tail",
            "path": "record",
            "a": "present",
            "b": None,
        })

    def test_global_interleaving_is_not_a_divergence(self):
        order = [base_v2()[index] for index in (1, 0, 2, 4, 3, 5, 6)]
        remap = {rec["seq"]: index for index, rec in enumerate(order)}
        reordered = []
        for index, rec in enumerate(order):
            out = dict(rec)
            out["seq"] = index
            if "call_seq" in out:
                out["call_seq"] = remap[out["call_seq"]]
            reordered.append(out)
        shuffled = write_records(self.path("interleaved.ndjson"), reordered)
        result, report = self.diff_json(FIXTURES / "v2-basic.ndjson", shuffled)
        self.assert_exit(result, 0)
        self.assertTrue(report["identical"])
        self.assertEqual(report["counts"]["compared"], 7)

    def test_timing_and_sequence_identity_are_ignored(self):
        shifted = []
        for rec in base_v2():
            out = dict(rec, t=rec["t"] + 10.0, seq=rec["seq"] + 1000)
            if "latency_ms" in out:
                out["latency_ms"] += 5
            if "call_seq" in out:
                out["call_seq"] += 1000
            shifted.append(out)
        other = write_records(self.path("timing.ndjson"), shifted)
        result, report = self.diff_json(FIXTURES / "v2-basic.ndjson", other)
        self.assert_exit(result, 0)
        self.assertTrue(report["identical"])

    def test_zero_overlap_exits_three(self):
        records = base_v2()
        only_a = [rec for rec in records if rec["ctx"]["id"].endswith("1000000")]
        only_b = [rec for rec in records if rec["ctx"]["id"].endswith("100000b")]
        a = write_records(self.path("only-a.ndjson"), only_a)
        b = write_records(self.path("only-b.ndjson"), only_b)
        result, report = self.diff_json(a, b)
        self.assert_exit(result, 3)
        self.assertEqual(report["counts"]["compared"], 0)
        self.assertFalse(report["identical"])
        self.assertEqual(report["first"]["path"], "ctx")
        self.assertIn("zero records compared", result.stderr)

    def test_one_sided_context_exits_one_and_ctx_map_aligns(self):
        renamed = []
        for rec in base_v2():
            out = dict(rec)
            if out["ctx"]["id"] == "0000000001000000":
                out["ctx"] = {"kind": "ppu", "id": "0000000002000000"}
                out["thread"] = "0000000002000000"
            renamed.append(out)
        other = write_records(self.path("renamed.ndjson"), renamed)

        result, report = self.diff_json(FIXTURES / "v2-basic.ndjson", other)
        self.assert_exit(result, 1)
        self.assertEqual(report["counts"]["compared"], 2)
        self.assertEqual({entry["ctx"]: entry["status"]
                          for entry in report["contexts"]},
                         {"ppu:0000000001000000": "a-only",
                          "ppu:000000000100000b": "matched",
                          "ppu:0000000002000000": "b-only"})

        result, report = self.diff_json(FIXTURES / "v2-basic.ndjson", other,
                                        "--ctx-map", FIXTURES / "ctx-map.json")
        self.assert_exit(result, 0)
        self.assertTrue(report["identical"])
        self.assertEqual(report["counts"]["compared"], 7)

    def test_json_report_schema_is_pinned(self):
        result, report = self.diff_json(FIXTURES / "v2-basic.ndjson",
                                        FIXTURES / "v2-basic.ndjson")
        self.assert_exit(result, 0)
        self.assertEqual(set(report),
                         {"schema", "a", "b", "identical", "counts", "first",
                          "contexts"})
        self.assertEqual(report["schema"], REPORT)
        self.assertIsInstance(report["a"], str)
        self.assertIsInstance(report["b"], str)
        self.assertIsInstance(report["identical"], bool)
        self.assertEqual(set(report["counts"]),
                         {"compared", "contexts_a", "contexts_b",
                          "contexts_matched"})
        self.assertIsNone(report["first"])
        keys = [entry["ctx"] for entry in report["contexts"]]
        self.assertEqual(keys, sorted(keys))
        for entry in report["contexts"]:
            self.assertEqual(set(entry), {"ctx", "compared", "first", "status"})
            self.assertIsInstance(entry["compared"], int)
            self.assertEqual(entry["status"], "matched")
            self.assertIsNone(entry["first"])

        again = run_tool("diff", FIXTURES / "v2-basic.ndjson",
                         FIXTURES / "v2-basic.ndjson", "--json")
        self.assert_exit(again, 0)
        self.assertEqual(again.stdout, result.stdout, "reports are deterministic")

    def test_dump_digest_and_error_are_compared(self):
        records = base_v2()
        ret = next(rec for rec in records if rec["event"] == "ret"
                   and any(dump["name"] == "out" for dump in rec["dumps"]))
        ret["dumps"][0]["sha256"] = "0" * 64
        changed = write_records(self.path("digest.ndjson"), records)
        result, report = self.diff_json(FIXTURES / "v2-basic.ndjson", changed)
        self.assert_exit(result, 1)
        self.assertEqual(report["first"]["path"], "dumps.out.sha256")

        records = base_v2()
        records[0]["dumps"][2]["error"] = "pointer at 0x00100020 vanished"
        changed = write_records(self.path("error.ndjson"), records)
        result, report = self.diff_json(FIXTURES / "v2-basic.ndjson", changed)
        self.assert_exit(result, 1)
        self.assertEqual(report["first"]["path"], "dumps.unreadable_ptr.error")

    def test_dump_length_is_compared(self):
        records = base_v2()
        ret = next(rec for rec in records if rec["event"] == "ret"
                   and any(dump["name"] == "out" for dump in rec["dumps"]))
        ret["dumps"][0]["len"] = 5
        changed = write_records(self.path("length.ndjson"), records)
        result, report = self.diff_json(FIXTURES / "v2-basic.ndjson", changed)
        self.assert_exit(result, 1)
        self.assertEqual(report["first"]["path"], "dumps.out.len")
        self.assertEqual(report["first"]["a"], "4")
        self.assertEqual(report["first"]["b"], "5")

    def test_state_hash_and_job_outputs_are_compared(self):
        records = read_records(FIXTURES / "v2-kinds.ndjson")
        changed = [dict(rec) for rec in records]
        changed[0]["state_hash"] = dict(changed[0]["state_hash"], value="0" * 64)
        state_path = write_records(self.path("state.ndjson"), changed)
        result, report = self.diff_json(FIXTURES / "v2-kinds.ndjson", state_path)
        self.assert_exit(result, 1)
        self.assertEqual(report["first"]["path"], "state_hash.value")

        changed = [dict(rec) for rec in read_records(FIXTURES / "v2-kinds.ndjson")]
        changed[3]["outputs"] = dict(changed[3]["outputs"], vertices=13)
        output_path = write_records(self.path("output.ndjson"), changed)
        result, report = self.diff_json(FIXTURES / "v2-kinds.ndjson", output_path)
        self.assert_exit(result, 1)
        self.assertEqual(report["first"]["ctx"], "spu:0")
        self.assertEqual(report["first"]["ctx_seq"], 2)
        self.assertEqual(report["first"]["path"], "outputs.vertices")
        self.assertEqual(report["first"]["a"], "12")
        self.assertEqual(report["first"]["b"], "13")

    def test_mask_path_that_matches_nothing_is_an_error(self):
        mask = write_json(self.path("typo.mask.json"),
                          {"ignore": ["args.not_a_register"]})
        result = run_tool("diff", FIXTURES / "v2-basic.ndjson",
                         FIXTURES / "v2-basic.ndjson", "--mask", mask)
        self.assert_exit(result, 2)
        self.assertIn("match no compared field", result.stderr)

    def test_ctx_map_to_absent_target_is_an_error(self):
        mapping = write_json(self.path("stale.map.json"),
                             {"ppu:0000000001000000":
                              "ppu:0000000009000000"})
        result = run_tool("diff", FIXTURES / "v2-basic.ndjson",
                         FIXTURES / "v2-basic.ndjson", "--ctx-map", mapping)
        self.assert_exit(result, 2)
        self.assertIn("B does not contain", result.stderr)


class Help(unittest.TestCase):
    def test_every_subcommand_has_examples(self):
        for command in ("convert", "validate", "diff"):
            with self.subTest(command=command):
                result = run_tool(command, "--help")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("examples:", result.stdout)
                self.assertIn("ps3diff", result.stdout)


if __name__ == "__main__":
    unittest.main()
