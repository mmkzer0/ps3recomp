#!/usr/bin/env python3
"""Synthetic-source tests for tools/hle_check_audit.py.

The audit's job is to find every `if (cond) return <error>;` / `if (cond)
ps3_stop*();` in a registered handler and attach the nearest evidence.  These
tests build a throw-away tree with one case per check shape and evidence class,
so a change in the parser or the classifier fails here before it changes a
report over the real libs/.

Run: python3 tools/test_hle_check_audit.py
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
TOOL = TOOLS / "hle_check_audit.py"

sys.path.insert(0, str(TOOLS))
from nid_database import compute_nid  # noqa: E402


MAIN = r'''/* Synthetic audit fixture. */
#include <stdint.h>
typedef int32_t s32;
typedef uint32_t u32;
#define CELL_OK 0
#define CELL_EINVAL 0x80010002
#define CELL_EPERM 0x80010009
#define CELL_TEST_ERROR_PARAM 0x80020001
void ps3_stop_test(const char*, const char*);
void ps3_stop(const char*, const char*);

s32 cellTestFwEa(s32 value)
{
    /* 0x02001234: the firmware rejects value < 0 with INVAL. */
    if (value < 0) return CELL_EINVAL;
    return CELL_OK;
}

s32 cellTestFwBare(s32 value)
{
    /* Firmware 020045E8 (libgcm_sys) rejects this. */
    if (value > 4) return CELL_EINVAL;
    return CELL_OK;
}

s32 cellTestOracle(u32 flags)
{
    /* RPCS3 cellGcmSys.cpp accepts only STRICT_ORDERING. */
    if (flags != 1) return CELL_EINVAL;
    return CELL_OK;
}

s32 cellTestOracleNoFile(u32 flags)
{
    /* RPCS3's implementation (see above). */
    if (flags != 1) return CELL_EINVAL;
    return CELL_OK;
}

s32 cellTestSdk(u32 mode)
{
    /* SDK cell/pad.h prototype: mode is 0..3. */
    if (mode > 3) return CELL_EINVAL;
    return CELL_OK;
}

s32 cellTestNone(u32 x)
{
    if (x == 7) return CELL_EINVAL;
    return CELL_OK;
}

s32 cellTestBlock(u32 x)
{
    /* 0x02002222: a block-bodied return with a cast and a u suffix. */
    if (x == 0) {
        return (s32)0x80010009u;
    }
    return CELL_OK;
}

s32 cellTestStop(u32 x)
{
    /* 0x02003333: the firmware stops. */
    if (x > 16) {
        ps3_stop_test(PS3_STOP_TEST, "x");
    }
    return CELL_OK;
}

s32 cellTestStopExact(s32 x)
{
    /* 0x02003333: the exact ps3_stop() form. */
    if (x < 0) ps3_stop(PS3_STOP_TEST, "x");
    return CELL_OK;
}

s32 cellTestSuccess(s32 x)
{
    if (x) return CELL_OK;
    return CELL_OK;
}

s32 cellTestTrail(u32 x)
{
    if (x > 1) return CELL_EINVAL;   /* 0x02004444: firmware range */
    return CELL_OK;
}

s32 cellTestTwo(u32 x)
{
    /* 0x02005555: evidence belongs to the first check only. */
    if (x == 1) return CELL_EINVAL;
    if (x == 2) return CELL_EPERM;
    return CELL_OK;
}

int test_ready;
s32 cellTestState(u32 x)
{
    if (!test_ready) return CELL_EINVAL;
    return CELL_OK;
}

s32 cellTestParam(s32 p)
{
    if (!p) return CELL_EINVAL;
    return CELL_OK;
}

s32 cellTestElse(s32 x)
{
    if (x > 0) {
        return CELL_OK;
    } else {
        return CELL_EINVAL;
    }
}

s32 cellTestDecl(s32 x)
{
    /* 0x02006666: the action follows a declaration in the same block. */
    if (x) {
        s32 unused = 1;
        return CELL_EINVAL;
    }
    return CELL_OK;
}

s32 cellTestSentinel(s32 x)
{
    if (x) return 0xFFFFFFFF;
    return CELL_OK;
}

s32 cellTestRc(s32 x, s32 rc)
{
    if (x) return rc;
    return CELL_OK;
}

static s32 helper(s32 x)
{
    if (x) return CELL_EINVAL;
    return CELL_OK;
}

void host_helper(s32 x)
{
    if (x) return;
}
'''

HEADER = r'''/* Synthetic header fixture. RPCS3 cellTestHeader.cpp: this contract is the oracle. */
#include <stdint.h>
typedef int32_t s32;
typedef uint32_t u32;
#define CELL_OK 0
#define CELL_EINVAL 0x80010002

s32 cellTestHeaderOracle(u32 x)
{
    if (x) return CELL_EINVAL;
    return CELL_OK;
}
'''


def run(args, cwd=ROOT):
    return subprocess.run([sys.executable, str(TOOL)] + args,
                          cwd=str(cwd), capture_output=True, text=True,
                          timeout=120)


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def write_tree(tmp):
    (tmp / "libs" / "system").mkdir(parents=True)
    (tmp / "libs" / "system" / "cellTestAudit.c").write_text(MAIN)
    (tmp / "libs" / "system" / "cellTestHeader.c").write_text(HEADER)


def by_handler(checks, name):
    return [c for c in checks if c["handler"] == name]


def test_classes_and_shapes():
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        write_tree(tmp)
        j = tmp / "audit.json"
        m = tmp / "audit.md"
        r = run(["--root", str(tmp), "--json", str(j), "--md", str(m)])
        check(r.returncode == 0, f"tool failed: {r.stderr}")
        rep = json.loads(j.read_text())
        checks = rep["checks"]

        got = {c["handler"]: c for c in checks}
        check(not any(c["handler"] == "helper" for c in checks),
              "static helper scanned")
        check(not any(c["handler"] == "host_helper" for c in checks),
              "host helper scanned")
        check(not by_handler(checks, "cellTestSuccess"),
              "CELL_OK path reported as a check")
        check(not by_handler(checks, "cellTestElse"),
              "else-path return reported as an if check (scope limit)")
        check(not by_handler(checks, "cellTestSentinel"),
              "0xFFFFFFFF sentinel reported as a Cell error code")
        check(not by_handler(checks, "cellTestRc"),
              "variable return (`return rc`) reported as a code")

        c = got["cellTestFwEa"]
        check(c["class"] == "fw-ea", f"fw-ea class: {c['class']}")
        check(c["condition"] == "value < 0", f"condition: {c['condition']}")
        check(c["code"] == "CELL_EINVAL", f"code: {c['code']}")
        check(c["kind"] == "return" and c["scope"] == "param", "fw-ea metadata")

        c = got["cellTestFwBare"]
        check(c["class"] == "fw-ea", f"bare EA class: {c['class']}")

        c = got["cellTestOracle"]
        check(c["class"] == "oracle-rpcs3", f"oracle class: {c['class']}")

        c = got["cellTestOracleNoFile"]
        check(c["class"] == "none",
              f"RPCS3 without a file/function must stay none: {c['class']}")

        c = got["cellTestSdk"]
        check(c["class"] == "sdk", f"sdk class: {c['class']}")

        c = got["cellTestNone"]
        check(c["class"] == "none", f"none class: {c['class']}")

        c = got["cellTestBlock"]
        check(c["class"] == "fw-ea" and c["code"] == "0x80010009u",
              f"block/cast: {c['class']} {c['code']}")
        check(c["line"] > c["if_line"], "block action line not after the if")

        c = got["cellTestStop"]
        check(c["kind"] == "stop" and c["code"] == "ps3_stop_test",
              f"family stop: {c['kind']} {c['code']}")
        c = got["cellTestStopExact"]
        check(c["kind"] == "stop" and c["code"] == "PS3_STOP_TEST",
              f"exact stop: {c['kind']} {c['code']}")

        c = got["cellTestTrail"]
        check(c["class"] == "fw-ea" and c["evidence_scope"] == "trailing",
              f"trailing: {c['class']} {c.get('evidence_scope')}")

        two = by_handler(checks, "cellTestTwo")
        check(len(two) == 2, f"cellTestTwo checks: {len(two)}")
        check(two[0]["class"] == "fw-ea", "first grouped check lost its comment")
        check(two[1]["class"] == "none",
              "comment above the first check leaked to the second")

        c = got["cellTestDecl"]
        check(c["class"] == "fw-ea" and c["code"] == "CELL_EINVAL",
              f"declaration before return: {c['class']} {c['code']}")

        check(got["cellTestState"]["scope"] == "state", "state scope")
        check(got["cellTestParam"]["scope"] == "param", "param scope")

        c = got["cellTestHeaderOracle"]
        check(c["class"] == "oracle-rpcs3" and c["evidence_scope"] == "file",
              f"file-header oracle: {c['class']} {c.get('evidence_scope')}")

        counts = rep["counts"]
        check(counts["by_class"] == {"fw-ea": 8, "oracle-rpcs3": 2,
                                     "sdk": 1, "none": 5},
              f"class counts: {counts['by_class']}")
        check(counts["by_kind"] == {"return": 14, "stop": 2},
              f"kind counts: {counts['by_kind']}")
        check(counts["by_scope"] == {"param": 15, "state": 1},
              f"scope counts: {counts['by_scope']}")
        check(len(checks) == 16, f"total checks: {len(checks)}")

        text = m.read_text()
        check("| handler | file:line | condition | code | class | source | evidence |"
              in text, "markdown table header")
        check("cellTestFwEa" in text and "0x02001234" in text,
              "markdown lost a row/evidence")


def test_imports_filter():
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        write_tree(tmp)
        wanted = [compute_nid("cellTestFwEa"), compute_nid("cellTestNone")]
        imports = tmp / "reached.txt"
        imports.write_text("\n".join(f"0x{n:08X}" for n in wanted)
                           + "\n0xDEADBEEF\n")
        j = tmp / "reached.json"
        r = run(["--root", str(tmp), "--imports", str(imports), "--json", str(j)])
        check(r.returncode == 0, f"tool failed: {r.stderr}")
        rep = json.loads(j.read_text())
        check(rep["imports_count"] == 3, "imports count")
        check(rep["handlers_scanned"] == 2,
              f"filtered handlers: {rep['handlers_scanned']}")
        check(rep["unmatched_imports"] == ["0xDEADBEEF"],
              f"unmatched: {rep['unmatched_imports']}")
        names = {c["handler"] for c in rep["checks"]}
        check(names == {"cellTestFwEa", "cellTestNone"},
              f"filtered handlers with checks: {names}")
        check(rep["counts"]["by_class"] == {"fw-ea": 1, "oracle-rpcs3": 0,
                                            "sdk": 0, "none": 1},
              f"filtered class counts: {rep['counts']['by_class']}")

        bad = tmp / "bad.txt"
        bad.write_text("not-a-nid\n")
        r = run(["--root", str(tmp), "--imports", str(bad)])
        check(r.returncode == 2 and "not a NID" in r.stderr,
              f"malformed imports: rc={r.returncode} stderr={r.stderr!r}")


def test_class_filter():
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        write_tree(tmp)
        j = tmp / "none.json"
        r = run(["--root", str(tmp), "--classes", "none", "--json", str(j)])
        check(r.returncode == 0, f"tool failed: {r.stderr}")
        rep = json.loads(j.read_text())
        check(len(rep["checks"]) == 5,
              f"none-only total: {len(rep['checks'])}")
        check(all(c["class"] == "none" for c in rep["checks"]),
              "class filter leaked another class")


def test_whole_tree():
    """The tree that ships must scan without warnings and find the handlers."""
    with tempfile.TemporaryDirectory() as td:
        j = Path(td) / "all.json"
        r = run(["--json", str(j)])
        check(r.returncode == 0, f"whole-tree run failed: {r.stderr}")
        rep = json.loads(j.read_text())
        check(rep["handlers_scanned"] > 1000,
              f"handlers scanned: {rep['handlers_scanned']}")
        check(len(rep["checks"]) > 100, f"checks: {len(rep['checks'])}")
        check(rep["warnings"] == [], f"warnings: {rep['warnings']}")
        check(set(rep["counts"]["by_class"]) ==
              {"fw-ea", "oracle-rpcs3", "sdk", "none"},
              f"classes: {rep['counts']['by_class']}")


def main():
    tests = [
        ("classes and check shapes", test_classes_and_shapes),
        ("imports filter", test_imports_filter),
        ("class filter", test_class_filter),
        ("whole tree", test_whole_tree),
    ]
    for name, fn in tests:
        fn()
        print(f"ok  {name}")
    print("all hle_check_audit tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
