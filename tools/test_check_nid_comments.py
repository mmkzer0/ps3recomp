#!/usr/bin/env python3
"""Contract tests for tools/check_nid_comments.py.

Every case writes throwaway sources into a temporary directory. The NID values
are computed here from the function name with the same database the tool uses,
so a case asserts the *pairing* the tool proves -- comment to the function that
immediately follows it -- rather than a number copied into two places.

Why these cases: the checker exists because 33 of 36 annotations in
cellGcmSys.c were wrong, most of them holding real NIDs of other functions, so
a case that only covers "the value does not match" would pass against a tool
that pairs comments and functions the same broken way the drift came about.
Each pairing rule therefore has a case that fails if the pairing is inferred
from anything other than the immediately following declarator.
"""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest

TOOLS = Path(__file__).resolve().parent
TOOL = TOOLS / "check_nid_comments.py"
sys.path.insert(0, str(TOOLS))
from nid_database import compute_nid  # noqa: E402

ALPHA = "cellTestAlpha"
BETA = "cellTestBeta"
GAMMA = "cellTestGamma"


def hexnid(name):
    return f"0x{compute_nid(name):08X}"


class CommentFixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="check-nid-comments-")
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(text).lstrip("\n"))
        return path

    def read(self, name):
        return (self.root / name).read_text()

    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(TOOL), *args],
                              cwd=str(self.root), capture_output=True,
                              text=True)

    def assert_exit(self, result, expected):
        self.assertEqual(
            result.returncode, expected,
            f"stdout={result.stdout!r} stderr={result.stderr!r}")


class Binding(CommentFixture):
    def test_correct_comment_passes(self):
        self.write("alpha.c", f"""
            /* NID: {hexnid(ALPHA)} */
            s32 cellTestAlpha(void) {{ return 0; }}
            """)
        result = self.run_tool("alpha.c")
        self.assert_exit(result, 0)
        self.assertIn("1 NID annotation(s)", result.stdout)
        self.assertIn("0 mismatch, 0 orphan, 0 malformed", result.stdout)

    def test_multi_line_signature_binds_to_its_definition(self):
        self.write("alpha.c", f"""
            /* NID: {hexnid(ALPHA)} */
            s32 cellTestAlpha(u32 a,
                              u32 b,
                              const char* c)
            {{
                (void)a; (void)b; (void)c;
                return 0;
            }}
            """)
        self.assert_exit(self.run_tool("alpha.c"), 0)

    def test_static_pointer_return_binds(self):
        # `static` and a pointer return place type noise between the comment
        # and the name; the name is still the last identifier before `(`.
        self.write("alpha.c", f"""
            /* NID: {hexnid(ALPHA)} */
            static u32* cellTestAlpha(void)
            {{
                return 0;
            }}
            """)
        self.assert_exit(self.run_tool("alpha.c"), 0)

    def test_attribute_and_pointer_return_bind(self):
        self.write("alpha.c", f"""
            /* NID: {hexnid(ALPHA)} */
            __attribute__((noinline)) static u32* cellTestAlpha(void)
            {{
                return 0;
            }}
            """)
        self.assert_exit(self.run_tool("alpha.c"), 0)

    def test_header_declaration_is_checked(self):
        # A header has no body to attach to; the declaration is the target.
        self.write("alpha.h", f"""
            /* NID: {hexnid(ALPHA)} */
            s32 cellTestAlpha(void);
            """)
        self.assert_exit(self.run_tool("alpha.h"), 0)

    def test_source_prototype_is_an_orphan(self):
        # In an implementation file the NID belongs to the definition; a
        # forward declaration there is exactly the mis-pairing this reports.
        self.write("alpha.c", f"""
            /* NID: {hexnid(ALPHA)} */
            s32 cellTestAlpha(void);

            s32 cellTestAlpha(void) {{ return 0; }}
            """)
        result = self.run_tool("alpha.c")
        self.assert_exit(result, 1)
        self.assertIn("alpha.c:1: orphan: a prototype follows it", result.stdout)
        self.assertNotIn(": mismatch:", result.stdout)


class Findings(CommentFixture):
    def test_mismatch_reports_computed_value_and_permutation_hint(self):
        # The comment holds the *other* function's NID -- the shape the drift
        # in cellGcmSys took, and the reason a hint is worth printing.
        self.write("libs/alpha.c", f"""
            /* NID: {hexnid(BETA)} */
            s32 cellTestAlpha(void) {{ return 0; }}

            /* NID: {hexnid(BETA)} */
            s32 cellTestBeta(void) {{ return 0; }}
            """)
        result = self.run_tool("libs")
        self.assert_exit(result, 1)
        line = next(ln for ln in result.stdout.splitlines()
                    if "mismatch" in ln)
        self.assertIn("libs/alpha.c:1: mismatch: cellTestAlpha", line)
        self.assertIn(f"comment {hexnid(BETA)}", line)
        self.assertIn(f"computed {hexnid(ALPHA)}", line)
        self.assertIn(f"{hexnid(BETA)} is cellTestBeta's", line)

    def test_orphan_before_prototype_and_before_struct(self):
        self.write("alpha.c", f"""
            /* NID: {hexnid(ALPHA)} */
            s32 cellTestAlpha(void);

            /* NID: {hexnid(BETA)} */
            struct TestThing {{ int x; }};

            s32 cellTestAlpha(void) {{ return 0; }}
            """)
        result = self.run_tool("alpha.c")
        self.assert_exit(result, 1)
        self.assertIn("alpha.c:1: orphan:", result.stdout)
        self.assertIn("alpha.c:4: orphan:", result.stdout)
        self.assertIn("struct", result.stdout)

    def test_comment_between_comment_and_definition_is_an_orphan(self):
        self.write("alpha.c", f"""
            /* NID: {hexnid(ALPHA)} */
            /* why this handler exists */
            s32 cellTestAlpha(void) {{ return 0; }}
            """)
        result = self.run_tool("alpha.c")
        self.assert_exit(result, 1)
        self.assertIn("alpha.c:1: orphan: another comment comes between",
                      result.stdout)

    def test_trailing_comment_is_an_orphan(self):
        self.write("alpha.h", f"""
            s32 cellTestAlpha(void);   /* NID: {hexnid(ALPHA)} */
            """)
        result = self.run_tool("alpha.h")
        self.assert_exit(result, 1)
        self.assertIn("orphan: code precedes it on the line", result.stdout)

    def test_malformed_hex_is_reported_and_never_a_mismatch(self):
        self.write("alpha.c", f"""
            /* NID: 0xZZZZZZZZ */
            s32 cellTestAlpha(void) {{ return 0; }}

            /* NID: 0x1234 */
            s32 cellTestBeta(void) {{ return 0; }}

            /* NID: */
            s32 cellTestGamma(void) {{ return 0; }}
            """)
        result = self.run_tool("alpha.c")
        self.assert_exit(result, 1)
        self.assertEqual(result.stdout.count(": malformed:"), 3, result.stdout)
        self.assertNotIn(": mismatch:", result.stdout)

    def test_prose_mentioning_a_nid_is_not_an_annotation(self):
        # Only a comment that BEGINS with NID is an annotation; a value quoted
        # in prose documents a finding, it does not claim a registration.
        self.write("alpha.c", f"""
            /* the firmware links this as NID {hexnid(ALPHA)}, see above */
            s32 cellTestAlpha(void) {{ return 0; }}
            """)
        self.assert_exit(self.run_tool("alpha.c"), 0)

    def test_unscanned_extension_is_ignored(self):
        self.write("notes.md", f"""
            /* NID: 0xDEADBEEF */
            s32 nothing_here(void);
            """)
        self.assert_exit(self.run_tool("."), 0)


class FixAndExit(CommentFixture):
    def test_fix_rewrites_mismatches_only_and_keeps_their_form(self):
        path = self.write("alpha.c", f"""
            /* NID: {hexnid(BETA)} (disambiguation by context) */
            s32 cellTestAlpha(void) {{ return 0; }}

            /* NID {hexnid(ALPHA).lower()} -- lower case, no colon; the prose stays */
            s32 cellTestBeta(void) {{ return 0; }}

            /* NID: 0x12345678 */
            s32 cellTestGamma(void) {{ return 0; }}

            /* NID: 0x00000000 */
            s32 cellTestDelta(void);

            /* NID: 0xNOTHEX */
            s32 cellTestUnparsed(void);
            """)
        result = self.run_tool("--fix", "alpha.c")
        # The orphan and the malformed comment stay: exit 1, not 0.
        self.assert_exit(result, 1)
        self.assertIn("3 fixed", result.stdout)
        text = path.read_text()
        self.assertIn(f"/* NID: {hexnid(ALPHA)} (disambiguation by context) */",
                      text)
        self.assertIn(f"/* NID {hexnid(BETA).lower()} -- lower case, no colon;"
                      " the prose stays */", text)
        self.assertIn(f"/* NID: {hexnid(GAMMA)} */", text)
        # Never rewritten: an orphan has no right value, nor does garbage.
        self.assertIn("/* NID: 0x00000000 */", text)
        self.assertIn("/* NID: 0xNOTHEX */", text)

        once = path.read_text()
        again = self.run_tool("--fix", "alpha.c")
        self.assert_exit(again, 1)
        self.assertIn("0 fixed", again.stdout)
        self.assertEqual(once, path.read_text())

    def test_fix_exits_zero_and_is_idempotent_when_only_mismatches(self):
        path = self.write("alpha.c", f"""
            /* NID: 0x00000000 */
            s32 cellTestAlpha(void) {{ return 0; }}
            """)
        first = self.run_tool("--fix", "alpha.c")
        self.assert_exit(first, 0)
        self.assertIn(f"fixed alpha.c:1: cellTestAlpha: 0x00000000 -> "
                      f"{hexnid(ALPHA)}", first.stdout)
        fixed = path.read_text()
        self.assertIn(f"/* NID: {hexnid(ALPHA)} */", fixed)

        second = self.run_tool("--fix", "alpha.c")
        self.assert_exit(second, 0)
        self.assertIn("0 fixed", second.stdout)
        self.assertEqual(fixed, path.read_text())

    def test_exit_codes(self):
        self.write("alpha.c", f"""
            /* NID: {hexnid(ALPHA)} */
            s32 cellTestAlpha(void) {{ return 0; }}
            """)
        self.assert_exit(self.run_tool("alpha.c"), 0)

        self.write("bad.c", "/* NID: 0x00000000 */\ns32 cellTestBeta(void) { return 0; }\n")
        self.assert_exit(self.run_tool("bad.c"), 1)

        missing = self.run_tool("no-such-path")
        self.assert_exit(missing, 2)
        self.assertIn("no such file or directory", missing.stderr)

        usage = self.run_tool("--definitely-not-an-option")
        self.assert_exit(usage, 2)

    def test_default_path_is_libs(self):
        self.write("libs/alpha.c", f"""
            /* NID: 0x00000000 */
            s32 cellTestAlpha(void) {{ return 0; }}
            """)
        result = self.run_tool()
        self.assert_exit(result, 1)
        self.assertIn("libs/alpha.c:1: mismatch", result.stdout)


class JsonOutput(CommentFixture):
    VERSION = "nid-comments/1"

    def schema(self, result):
        self.assert_exit(result, 1)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["schema"], self.VERSION)
        self.assertEqual(set(payload), {"schema", "files", "annotations", "fix",
                                        "findings", "fixed", "counts"})
        self.assertEqual(set(payload["counts"]),
                         {"mismatch", "orphan", "malformed"})
        return payload

    def test_json_findings_carry_the_full_schema(self):
        self.write("alpha.c", f"""
            /* NID: {hexnid(BETA)} */
            s32 cellTestAlpha(void) {{ return 0; }}

            /* NID: {hexnid(BETA)} */
            s32 cellTestBeta(void) {{ return 0; }}

            /* NID: {hexnid(ALPHA)} */
            s32 cellTestGamma(void);

            /* NID: 0xNOPE */
            s32 cellTestDelta(void) {{ return 0; }}
            """)
        payload = self.schema(self.run_tool("--json", "alpha.c"))
        self.assertEqual(payload["fix"], False)
        self.assertEqual(payload["files"], 1)
        self.assertEqual(payload["annotations"], 4)
        self.assertEqual(payload["counts"],
                         {"mismatch": 1, "orphan": 1, "malformed": 1})
        keys = {"kind", "path", "line", "function", "comment_nid",
                "computed_nid", "hint_function", "reason", "text"}
        by_kind = {}
        for finding in payload["findings"]:
            self.assertEqual(set(finding), keys)
            by_kind.setdefault(finding["kind"], []).append(finding)

        mismatch = by_kind["mismatch"][0]
        self.assertEqual(mismatch["path"], "alpha.c")
        self.assertEqual(mismatch["line"], 1)
        self.assertEqual(mismatch["function"], ALPHA)
        self.assertEqual(mismatch["comment_nid"], hexnid(BETA))
        self.assertEqual(mismatch["computed_nid"], hexnid(ALPHA))
        self.assertEqual(mismatch["hint_function"], BETA)
        self.assertIsNone(mismatch["reason"])

        orphan = by_kind["orphan"][0]
        self.assertEqual(orphan["function"], GAMMA)
        self.assertIsNone(orphan["comment_nid"])
        self.assertIn("prototype", orphan["reason"])

        malformed = by_kind["malformed"][0]
        self.assertIn("0xNOPE", malformed["text"])
        self.assertIsNone(malformed["computed_nid"])

    def test_json_reports_fixes(self):
        self.write("alpha.c", f"""
            /* NID: 0x00000000 */
            s32 cellTestAlpha(void) {{ return 0; }}
            """)
        result = self.run_tool("--json", "--fix", "alpha.c")
        self.assert_exit(result, 0)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["schema"], self.VERSION)
        self.assertEqual(payload["fix"], True)
        self.assertEqual(payload["findings"], [])
        self.assertEqual(payload["fixed"], [{
            "path": "alpha.c", "line": 1, "function": ALPHA,
            "from": "0x00000000", "to": hexnid(ALPHA)}])


if __name__ == "__main__":
    unittest.main(verbosity=2)
