#!/usr/bin/env python3
"""Path-hygiene and skip contract of the differential-harness tools.

Firmware material and generated tables stay outside the repository. These are
the checks the tools perform themselves:

  * run_diff.py refuses --fw-dir / build / out inside the repository after
    realpath (a symlink pointing into it is caught too);
  * gen_tables.py refuses firmware inputs and output inside the repository,
    and still allows the checked-in synthetic fixture (--synthetic);
  * skip.py prints the visible `SKIP <module>: <reason>` line and exits 77.

The CMake configure-time refusal is exercised by a real configure in the
harness build (see docs/DIFF_HARNESS.md); spawning a configure is too heavy for
this test and needs the platform's SDL2.
"""
import hashlib
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.realpath(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(ROOT, "tools", "diff_harness")


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def override_seam_contract(expect):
    """The override-symbol file and the wrapper generator: a symbol file made for another build,
    a bad entry or a definition that is not exactly one lifted function must be refused, and a
    valid one must wrap exactly the named functions."""
    image = bytes(range(256)) * 4                      # 1 KiB stand-in image at 0x02000000
    sha = hashlib.sha256(image).hexdigest()
    lifted = ("#include <stdint.h>\nstruct ppu_context;\ntypedef struct ppu_context ppu_context;\n"
              "\nvoid fwx_func_02000010(ppu_context* ctx) {\n        return;\n}\n"
              "\nvoid fwx_func_02000020(ppu_context* ctx) {\n        return;\n}\n")

    def entry(addr, ev="why"):
        return {"addr": addr, "evidence": ev}

    with tempfile.TemporaryDirectory() as td:
        img = os.path.join(td, "x_image.bin")
        open(img, "wb").write(image)
        ex = os.path.join(td, "x_exports.json")
        json.dump({"base": "0x02000000", "toc": "0x02000000", "exports": {}}, open(ex, "w"))
        src = os.path.join(td, "fwx.c")
        open(src, "w").write(lifted)

        def symbols(doc, name="sym.json"):
            p = os.path.join(td, name)
            json.dump(doc, open(p, "w"))
            return p

        def apply(sym, out=None, extra=()):
            out = out or os.path.join(td, "fwx_ovr.c")
            return run([sys.executable, os.path.join(TOOLS, "apply_overrides.py"), "--lifted", src,
                        "--prefix", "fwx_", "--image", img, "--exports", ex, "--symbols", sym,
                        "--out", out, *extra]), out

        good = symbols({"image_sha256": sha, "symbols": {"ring.write": entry("0x02000010")}})
        (rc, out), dst = apply(good)
        text = open(dst).read() if rc == 0 else ""
        expect(rc == 0 and text.count("dh_override_hit(0x02000010u, ctx)") == 1
               and "void fwx_func_02000010_orig(ppu_context* ctx) {" in text
               and "void fwx_func_02000020(ppu_context* ctx) {\n        return;" in text
               and "dh_override_wrapped_count = 1;" in text,
               "apply_overrides wraps exactly the named function and lists it")
        expect(not os.path.exists(dst + ".partial"), "apply_overrides writes atomically")

        for why, doc, want in (
            ("another build's image", {"image_sha256": "0" * 64, "symbols": {"a.b": entry("0x02000010")}}, "another build"),
            ("a name outside <group>.<name>", {"image_sha256": sha, "symbols": {"Bad": entry("0x02000010")}}, "<group>.<name>"),
            ("an address outside the image", {"image_sha256": sha, "symbols": {"a.b": entry("0x03000000")}}, "inside the image"),
            ("an unaligned address", {"image_sha256": sha, "symbols": {"a.b": entry("0x02000012")}}, "word-aligned"),
            ("two names on one address", {"image_sha256": sha, "symbols": {"a.b": entry("0x02000010"), "a.c": entry("0x02000010")}}, "same address"),
            ("a symbol without evidence", {"image_sha256": sha, "symbols": {"a.b": entry("0x02000010", "")}}, "no evidence"),
            ("an address that is not a lifted function", {"image_sha256": sha, "symbols": {"a.b": entry("0x02000030")}}, "not a lifted function entry"),
        ):
            (rc, out), _dst = apply(symbols(doc, "bad.json"))
            expect(rc != 0 and want in out, f"apply_overrides refuses {why}")

        # firmware-derived output inside the repository is refused, the synthetic fixture may
        (rc, out), _ = apply(good, out=os.path.join(ROOT, "build", "dh-hygiene-ovr-should-not-exist.c"))
        expect(rc != 0 and "refusing" in out and not os.path.exists(os.path.join(ROOT, "build", "dh-hygiene-ovr-should-not-exist.c")),
               "apply_overrides refuses an in-repo output")

        # gen_tables binds the symbol table to the image and carries the fingerprint
        im = os.path.join(td, "x_imports.json")
        json.dump({}, open(im, "w"))
        gen = [sys.executable, os.path.join(TOOLS, "gen_tables.py"), "--name", "x", "--prefix", "x_",
               "--header", "x.h", "--exports", ex, "--imports", im, "--image", img, "--out", os.path.join(td, "gen")]
        rc, out = run(gen + ["--symbols", good])
        tables = open(os.path.join(td, "gen", "x_harness_tables.cpp")).read() if rc == 0 else ""
        expect(rc == 0 and '{"ring.write", 0x02000010u}' in tables and f'"{sha}"' in tables,
               "gen_tables emits the symbol table bound to the image fingerprint")
        rc, out = run(gen + ["--symbols", symbols({"image_sha256": "0" * 64, "symbols": {"a.b": entry("0x02000010")}}, "stale.json")])
        expect(rc != 0 and "another build" in out, "gen_tables refuses symbols made for another image")
        rc, out = run(gen)
        tables = open(os.path.join(td, "gen", "x_harness_tables.cpp")).read() if rc == 0 else ""
        expect(rc == 0 and "nullptr, 0, \"\"" in tables, "gen_tables without symbols leaves the table empty")


def main():
    failures = []

    def expect(cond, msg):
        print(("ok   " if cond else "FAIL ") + msg)
        if not cond:
            failures.append(msg)

    # 1. a firmware directory inside the repository
    rc, out = run([sys.executable, os.path.join(TOOLS, "run_diff.py"), "--fw-dir", os.path.join(ROOT, "tests")])
    expect(rc != 0 and "refusing" in out, "run_diff refuses an in-repo --fw-dir")
    expect(rc != 0 and out.count("refusing"), "run_diff prints the reason on stderr")

    # 2. a symlink that resolves into the repository
    with tempfile.TemporaryDirectory() as td:
        link = os.path.join(td, "fw-ish")
        os.symlink(os.path.join(ROOT, "tests"), link)
        rc, out = run([sys.executable, os.path.join(TOOLS, "run_diff.py"), "--fw-dir", link])
        expect(rc != 0 and "refusing" in out, "run_diff resolves symlinks before the refusal")

    # 3. gen_tables: in-repo output with outside inputs is refused
    with tempfile.TemporaryDirectory() as td:
        img = os.path.join(td, "img.bin")
        open(img, "wb").write(b"\0" * 64)
        ex = os.path.join(td, "ex.json")
        im = os.path.join(td, "im.json")
        json.dump({"base": "0x02000000", "toc": "0x02000000", "exports": {}}, open(ex, "w"))
        json.dump({}, open(im, "w"))
        rc, out = run([sys.executable, os.path.join(TOOLS, "gen_tables.py"),
                       "--name", "x", "--prefix", "x_", "--header", "x.h",
                       "--exports", ex, "--imports", im, "--image", img,
                       "--out", os.path.join(ROOT, "build", "dh-hygiene-should-not-exist")])
        expect(rc != 0 and "refusing" in out, "gen_tables refuses an in-repo output")
        expect(not os.path.exists(os.path.join(ROOT, "build", "dh-hygiene-should-not-exist")),
               "gen_tables did not create the refused directory")

        # 4. an in-repo firmware input is refused too
        rc, out = run([sys.executable, os.path.join(TOOLS, "gen_tables.py"),
                       "--name", "x", "--prefix", "x_", "--header", "x.h",
                       "--exports", ex, "--imports", im, "--image", os.path.join(ROOT, "CMakeLists.txt"),
                       "--out", os.path.join(td, "out")])
        expect(rc != 0 and "refusing firmware-derived input" in out, "gen_tables refuses an in-repo input")

        # 5. the synthetic fixture is the explicit exception, and still writes
        #    its tables (no firmware involved)
        outdir = os.path.join(td, "synth-out")
        rc, out = run([sys.executable, os.path.join(TOOLS, "gen_tables.py"), "--synthetic",
                       "--name", "synth", "--prefix", "synth_", "--header", "synth.h",
                       "--exports", ex, "--imports", im, "--image", img, "--out", outdir])
        expect(rc == 0 and os.path.exists(os.path.join(outdir, "synth_harness_tables.cpp")),
               "gen_tables --synthetic accepts the checked-in fixture")

    # 6. the visible skip line
    rc, out = run([sys.executable, os.path.join(TOOLS, "skip.py"), "libsync2", "missing metadata"])
    expect(rc == 77 and "SKIP libsync2: missing metadata" in out, "skip.py prints SKIP and exits 77")

    override_seam_contract(expect)

    if failures:
        print(f"{len(failures)} failure(s)", file=sys.stderr)
        return 1
    print("test_diff_harness: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
