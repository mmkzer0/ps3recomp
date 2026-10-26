#!/usr/bin/env python3
"""Lift (if needed), build and run the call-level differential harness.

    python3 tools/diff_harness/run_diff.py --fw-dir /path/to/fw [--module libsync2|libfiber|all]
                                           [--build-dir DIR] [--filter STR] [--allow-missing]

--fw-dir holds the user-supplied firmware material and must be OUTSIDE this
repository (after realpath, symlinks included):
    lift/<mod>/<mod>_{image.bin,exports.json,imports.json,functions.json}
(from tools/lift_prx.py). If lift/<mod>_c/ is missing or its input fingerprint
changed, ppu_lifter.py is run to (re)create it. The build tree defaults to
<fw-dir>/build-diff for the same reason.

Output: one JSON document per module (schema ps3recomp.diffharness/1), written
by the runner through a .partial file and an atomic rename:
    <build-dir>/diff_results.<mod>.json   (diff_results.json for one module)

Exit status: 0 every case matched its declared expectation; 1 some did not;
2 a build/usage/harness error; 77 a requested module was skipped (visible
`SKIP <module>: <reason>`), including a missing module under --module all
unless --allow-missing is given.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

ROOT = os.path.realpath(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


# module -> (lifter --symbol-prefix, lifted file stem)
MODULES = {"libsync2": ("fwsync2_", "fwsync2"), "libfiber": ("fwfiber_", "fwfiber"),
           "libspurs_jq": ("fwjq_", "fwjq"), "libsysutil": ("fwusys_", "fwusys")}


def inside_repo(p):
    p = os.path.realpath(p)
    return p == ROOT or p.startswith(ROOT + os.sep)


def realish(p):
    p = os.path.abspath(p)
    return os.path.join(os.path.realpath(os.path.dirname(p)), os.path.basename(p))


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sh(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, **kw).returncode


def lift_inputs(meta, mod):
    return [os.path.join(meta, mod + suffix) for suffix in
            ("_image.bin", "_functions.json", "_exports.json", "_imports.json")]


def ensure_lift(fw, mod, prefix, stem):
    meta = os.path.join(fw, "lift", mod)
    lifted = os.path.join(fw, "lift", mod + "_c")
    stamp_path = os.path.join(lifted, "." + stem + ".inputs.json")
    inputs = lift_inputs(meta, mod)
    want = {"module": mod, "prefix": prefix,
            "lifter_sha256": sha256_file(os.path.join(ROOT, "tools", "ppu_lifter.py")),
            "sha256": {os.path.basename(p): sha256_file(p) for p in inputs}}
    try:
        if json.load(open(stamp_path)) == want and os.path.exists(os.path.join(lifted, stem + ".c")):
            return 0
    except (OSError, ValueError):
        pass
    ex = json.load(open(os.path.join(meta, mod + "_exports.json")))
    rc = sh([sys.executable, "-I", os.path.join(ROOT, "tools", "ppu_lifter.py"),
             os.path.join(meta, mod + "_image.bin"), "--raw", "--base", ex["base"], "--toc", ex["toc"],
             "--functions", os.path.join(meta, mod + "_functions.json"),
             "--symbol-prefix", prefix, "--header-name", stem + ".h", "--source-name", stem + ".c",
             "--single-file", "--output", lifted])
    if rc:
        return 2
    os.makedirs(lifted, exist_ok=True)
    json.dump(want, open(stamp_path, "w"), indent=1)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fw-dir", required=True)
    ap.add_argument("--build-dir")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--filter", default="")
    ap.add_argument("--out", help="result path prefix (default: <build-dir>/diff_results)")
    ap.add_argument("--cc", default="clang")
    ap.add_argument("--cxx", default="clang++")
    ap.add_argument("--build-type", default="Debug")
    ap.add_argument("--module", default="all", choices=sorted(MODULES) + ["all"])
    ap.add_argument("--allow-missing", action="store_true",
                    help="run the modules that are present and return their status even when a "
                         "requested module has no lifted metadata (the SKIP is still printed)")
    ap.add_argument("--disable-dimension", action="append", default=[],
                    help="TEST ONLY: disable one comparator dimension (return|writes|extra|imports|"
                         "faults|fault-span|hang); production runs leave all enabled")
    a = ap.parse_args()

    fw = os.path.realpath(a.fw_dir)
    build = os.path.realpath(a.build_dir or os.path.join(fw, "build-diff"))
    out_prefix = realish(a.out) if a.out else os.path.join(build, "diff_results")
    for label, p in (("firmware", fw), ("build", build), ("output", out_prefix)):
        if inside_repo(p):
            sys.exit(f"refusing to use a path inside the repository for {label}: {p}")

    requested = sorted(MODULES) if a.module == "all" else [a.module]
    present, missing = [], []
    for m in requested:
        if os.path.exists(os.path.join(fw, "lift", m, m + "_image.bin")):
            present.append(m)
        else:
            missing.append(m)
            print(f"SKIP {m}: no lifted metadata under {fw}/lift/{m}", flush=True)
    if not present:
        print("no requested module is present; nothing to run", file=sys.stderr)
        return 77 if a.allow_missing else 77

    for m in present:
        prefix, stem = MODULES[m]
        if ensure_lift(fw, m, prefix, stem):
            return 2

    # Configure every run: an existing build.ninja keyed to another --fw-dir or
    # compiler would otherwise run stale tables. cmake is a no-op when nothing
    # changed, and this also picks up a freshly regenerated lift.
    rc = sh(["cmake", "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=" + a.build_type,
             "-DCMAKE_C_COMPILER=" + a.cc, "-DCMAKE_CXX_COMPILER=" + a.cxx,
             "-DPS3RECOMP_BUILD_TESTS=ON", "-DPS3RECOMP_DIFF_FW_DIR=" + fw])
    if rc:
        return 2
    if sh(["ninja", "-C", build, "-j%d" % a.jobs] + ["dh_diff_" + m for m in present]):
        return 2

    status = 0
    for m in present:
        out = out_prefix + "." + m + ".json" if len(present) > 1 else out_prefix + ".json"
        cmd = [os.path.join(build, "tests", "diff_harness", "dh_diff_" + m), "--out", out]
        if a.filter:
            cmd += ["--filter", a.filter]
        for dim in a.disable_dimension:
            cmd += ["--disable-dimension", dim]
        rc = sh(cmd)
        if rc == 2 or not os.path.exists(out):
            print(f"harness error for {m} (exit {rc})", file=sys.stderr)
            return 2
        doc = json.load(open(out))
        s = doc.get("summary", {})
        print(f"\nsummary {m}: cases {s.get('cases')}, compared {s.get('compared')}, "
              f"match {s.get('match')} ({s.get('common_mode', 0)} common-mode), "
              f"diverge {s.get('diverge')}, no-hle {s.get('no-hle')}, "
              f"inconclusive {s.get('inconclusive')}, lifted-blocked {s.get('lifted-blocked')}, "
              f"unexpected {s.get('unexpected')} -> {out}")
        for fn, by_nid in doc.get("functions", {}).items():
            for nid, f in by_nid.items():
                print(f"  {fn} {nid}: cases {f['cases']} match {f['match']} diverge {f['diverge']} "
                      f"no-hle {f['no-hle']} inconclusive {f['inconclusive']} "
                      f"lifted-blocked {f['lifted-blocked']} common-mode {f['common_mode']}")
        for r in doc.get("cases", []):
            if r.get("unexpected") or r.get("verdict") != "match":
                print(f"  {r['id']:<34} {r['verdict']:<14} {r.get('row', ''):<4} "
                      f"{r.get('cause', '')[:110]}{'  [UNEXPECTED]' if r.get('unexpected') else ''}")
        if rc == 1 or s.get("unexpected"):
            status = 1

    if missing and not a.allow_missing:
        print("requested module(s) skipped: " + ", ".join(missing) +
              " (pass --allow-missing to accept a partial suite)", file=sys.stderr)
        return 77
    return status


if __name__ == "__main__":
    sys.exit(main())
