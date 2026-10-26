#!/usr/bin/env python3
"""ctest driver for the synthetic differential fixture.

    test_synth.py <dh_diff_synth> <synth-build-dir>

Runs the fixture once (all cases must reach their declared verdict), checks the
schema/aggregate/common-mode, and then proves every comparator dimension with
fail-before: disabling that dimension must make its mutation case go
undetected, which the runner must report as an unexpected verdict (exit 1).
A dimension that stops detecting its own mutation therefore fails this test,
not silently passes it.
"""
import hashlib
import json
import os
import subprocess
import sys

DST = 0x00040000
FAULT_EA = "0x90000000"
HELPER_NID = "0x91000100"


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(binary, out, extra=()):
    cmd = [binary, "--out", out] + list(extra)
    p = subprocess.run(cmd, capture_output=True, text=True)
    doc = None
    if os.path.exists(out):
        try:
            doc = json.load(open(out))
        except ValueError:
            doc = None
    return p.returncode, doc, p.stdout + p.stderr


def cases(doc):
    return {c["id"]: c for c in doc.get("cases", [])}


def check(cond, msg):
    if not cond:
        print("FAIL: " + msg, file=sys.stderr)
        raise SystemExit(1)


def main(argv):
    if len(argv) != 3:
        print("usage: test_synth.py <dh_diff_synth> <synth-build-dir>", file=sys.stderr)
        return 2
    binary, synth_dir = argv[1], argv[2]
    work = os.path.join(synth_dir, "test")
    os.makedirs(work, exist_ok=True)
    out = os.path.join(work, "report.json")
    image = os.path.join(synth_dir, "lift", "synth", "synth_image.bin")

    # ---- the full fixture -------------------------------------------------
    rc, doc, log = run(binary, out)
    if rc != 0 or doc is None:
        print(log, file=sys.stderr)
        print(f"FAIL: full run exited {rc}", file=sys.stderr)
        return 1
    check(doc.get("schema") == "ps3recomp.diffharness/1", "schema field")
    env = doc.get("environment", {})
    check(env.get("module") == "synth", "environment.module")
    check(bool(env.get("toolkit_sha")), "environment.toolkit_sha")
    check(env.get("image_sha256") == sha256_file(image), "environment.image_sha256")
    check(not os.path.exists(out + ".partial"), "no .partial left behind")

    s = doc.get("summary", {})
    check(s.get("unexpected") == 0, f"unexpected={s.get('unexpected')}")
    check(s.get("compared") == 16, f"compared={s.get('compared')} (want 16)")
    check(s.get("match") == 7, f"match={s.get('match')} (want 7)")
    check(s.get("diverge") == 6, f"diverge={s.get('diverge')} (want 6)")
    check(s.get("inconclusive") == 3, f"inconclusive={s.get('inconclusive')} (want 3)")
    check(s.get("no-hle") == 1, f"no-hle={s.get('no-hle')} (want 1)")
    check(s.get("common_mode") == 1, f"common_mode={s.get('common_mode')} (want 1)")
    check(s.get("independent_match") == 6, "independent_match")

    by_id = cases(doc)
    expected = {
        "ret_match": "match", "write_match": "match", "import_match": "match",
        "asm_match": "match", "cpp_match": "match", "fault_match": "match", "override_match": "match",
        "ret_wrong": "diverge", "write_missing": "diverge", "pad_extra": "diverge",
        "mask_adjacent": "diverge", "import_swap": "diverge", "fault_return": "diverge",
        "fault_span": "inconclusive", "fault_zero": "inconclusive", "hang": "inconclusive",
        "nohle": "no-hle",
    }
    for cid, verdict in expected.items():
        check(by_id.get(cid, {}).get("verdict") == verdict,
              f"{cid}: {by_id.get(cid, {}).get('verdict')} != {verdict}")

    check(by_id["import_match"]["common_mode"] is True, "import_match common_mode")
    check(by_id["import_match"]["forwarded_nids"] == [HELPER_NID],
          f"import_match forwarded_nids {by_id['import_match']['forwarded_nids']}")
    check(by_id["ret_match"]["common_mode"] is False, "ret_match must not be common mode")
    check(by_id["write_missing"]["missing_bytes"] > 0, "write_missing missing_bytes")
    check(by_id["pad_extra"]["extra_bytes"] >= 24, "pad_extra extra_bytes")
    check(int(by_id["pad_extra"]["first_extra"], 16) == DST + 8, "pad_extra first_extra")
    check(int(by_id["mask_adjacent"]["first_extra"], 16) == DST + 16,
          "mask_adjacent must still see the unmasked bytes at DST+16")
    check(by_id["fault_match"]["cause_kind"] == "fault", "fault_match cause")
    check(by_id["fault_span"]["cause_kind"] == "fault-span", "fault_span cause")
    check(by_id["fault_zero"]["cause_kind"] == "fault-span", "fault_zero cause")
    check(by_id["hang"]["cause_kind"] == "hang", "hang cause")

    # The seam: the report names the override and logs every handled call on the lifted side.
    check(env.get("overrides") == ["synth.internal_91000021"], f"environment.overrides {env.get('overrides')}")
    check(env.get("symbols_fingerprint") == sha256_file(image), "environment.symbols_fingerprint")
    ovr_calls = [i for i in by_id["override_match"]["imports"] if i["name"] == "override:synth.internal_91000021"]
    check(len(ovr_calls) == 1 and ovr_calls[0]["policy"] == "model" and ovr_calls[0]["args"][0] == "0x7"
          and ovr_calls[0]["ret"] == "0x15", f"override call record {ovr_calls}")

    agg = doc.get("functions", {})
    check(agg.get("synth_import", {}).get("0x91000003", {}).get("common_mode") == 1,
          "per-function common_mode")
    check(agg.get("synth_ret", {}).get("0x91000001", {}).get("match") == 1, "per-function match")

    # ---- fail-before: one dimension at a time ------------------------------
    # (dimension, filter, {case: verdict with the dimension disabled})
    flips = [
        ("return", "ret_wrong", {"ret_wrong": "match"}),
        ("writes", "write_missing", {"write_missing": "match"}),
        ("extra", "pad_extra", {"pad_extra": "match"}),
        ("imports", "import_swap", {"import_swap": "match"}),
        ("faults", "fault_match", {"fault_match": "diverge"}),
        ("fault-span", "fault_", {"fault_span": "match", "fault_zero": "match"}),
        ("hang", "hang", {"hang": "match"}),
    ]
    fail_out = os.path.join(work, "fail_before.json")
    for dim, filt, want in flips:
        rc, doc, log = run(binary, fail_out, ["--filter", filt, "--disable-dimension", dim])
        check(doc is not None, f"{dim}: no report")
        check(rc == 1, f"{dim}: disabling the dimension must make the run fail (exit {rc})")
        got = cases(doc)
        for cid, verdict in want.items():
            check(cid in got, f"{dim}: case {cid} not run")
            check(got[cid]["verdict"] == verdict,
                  f"{dim}: {cid} became {got[cid]['verdict']}, want {verdict} (undetected mutation)")
            check(got[cid]["unexpected"] is True,
                  f"{dim}: {cid} must be reported unexpected once undetected")

    # Dimension isolation: disabling `return` must not disturb the import witness.
    rc, doc, _log = run(binary, fail_out, ["--filter", "import_swap", "--disable-dimension", "return"])
    check(rc == 0, f"control: import_swap still diverges with return disabled (exit {rc})")
    check(cases(doc)["import_swap"]["verdict"] == "diverge", "control: import dimension intact")

    # ---- fail-before: the override seam -----------------------------------
    # override_match agrees with the HLE only because the lifted module's internal helper is
    # replaced: without the override the module computes 2a+1 and the case must diverge, and an
    # override that answers wrongly must flip it as well. Either way the run fails.
    for extra, why in ((["--no-overrides"], "no override installed"),
                       (["--case-flag", "mutate-override"], "mutated override answer")):
        rc, doc, _log = run(binary, fail_out, ["--filter", "override_match"] + extra)
        check(doc is not None and rc == 1, f"{why}: the run must fail (exit {rc})")
        c = cases(doc)["override_match"]
        check(c["verdict"] == "diverge" and c["unexpected"] is True and c["cause_kind"] == "return",
              f"{why}: override_match is {c['verdict']}/{c['cause_kind']}, want an unexpected return divergence")

    # ---- fail-closed runs --------------------------------------------------
    rc, _doc, _log = run(binary, fail_out, ["--filter", "does-not-exist"])
    check(rc == 2, f"an unmatched filter must be a harness error, got {rc}")
    rc, _doc, _log = run(binary, fail_out, ["--filter", "nohle"])
    check(rc == 2, f"a filter with no compared case must not succeed, got {rc}")
    rc, _doc, _log = run(binary, fail_out, ["--disable-dimension", "bogus"])
    check(rc == 2, f"an unknown dimension must be a usage error, got {rc}")
    # An override on a symbol the generated table does not have must stop the run, not fall back
    # to the lifted function (which would look like an ordinary divergence or even a match).
    rc, _doc, log = run(binary, fail_out, ["--case-flag", "bogus-override"])
    check(rc == 2 and "synth.no_such" in log and "generated symbol table" in log,
          f"an unknown override symbol must be a harness error naming it, got {rc}")

    print("test_synth: PASS (17 cases, 7 dimensions + override seam fail-before, fail-closed paths)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
