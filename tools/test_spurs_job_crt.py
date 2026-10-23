#!/usr/bin/env python3
"""The SPURS job loader's JOBCRT gate, Ver14 jm interface, kernel view and bss.

Builds runtime/spu/tests/test_spurs_job_crt_main.c against the real loader
(runtime/spu/spurs_job.c), channel layer, interpreter and lifted-call drain,
the typed configuration and the named-stop runtime, and runs one mode per
process (the stops end the process). The fixture's job images are synthetic:
hand-encoded SPU words over a deterministic junk pattern.

  ver13, ver14   start state, byte-checked: kernel view (0x175, 0x1C8, 0x1CC,
                 0x210), JobContext2 and ctx+0x14, the jm object and the
                 runtime area below it, zeroed bss, buffers and r1 above the
                 bss, and a CRT-like program that reaches main
  ver13 (hash)   the Ver13 local store at entry, whole, equals the hash the
                 loader produced before Ver14 existed (GOLDEN_VER13_LS)
  fit, overlap   image + bss ending exactly at the runtime area's base runs;
                 one quadword more is a spu-job-memory stop
  swap-*         jm word 1 called with a tag still incomplete returns once the
                 MFC has completed it, with every register but r3 preserved,
                 for an interpreted job and for lifted-style bisl code; with
                 every tag complete it returns after one poll
  word0-*        jm word 0 is a spu-job-jm-entry stop on both paths
  ver12, ver15, garbage, empty
                 spu-job-crt-version stops naming the marker bytes
"""
import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests" / "gates"))
import gates_common as g  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

# SHA-256 of the 256 KB local store the loader hands a Ver13 job at entry for
# the fixture's "ver13" image. Produced by the loader as of the commit before
# the Ver14 work (dc177d1); the Ver13 path must not change by a byte.
GOLDEN_VER13_LS = "044a8a8591f84fa1fce1d5c0c89ae921a462c1f45e51893300223827d68ba2df"

RT_LS = 0x3FAC0
IMAGE_SIZE = 0x1000


def build(tmp):
    spu = ROOT / "runtime" / "spu"
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    cflags = ["-std=gnu17", "-O1", "-UNDEBUG",
              "-I" + str(ROOT / "include"), "-I" + str(spu),
              "-I" + str(ROOT / "runtime/platform")]
    cxxflags = ["-std=c++20", "-O1", "-UNDEBUG", "-I" + str(ROOT / "include")]
    sources = ["spu_channels.c", "spu_interp.c", "spu_coherency.c", "spu_lockstep.c",
               "spu_drain.c", "spu_vm_pagemap.c", "spurs_job.c"]
    c_sources = [spu / "tests/test_spurs_job_crt_main.c", *[spu / s for s in sources],
                 ROOT / "runtime/memory/reservation.c",
                 ROOT / "runtime/platform/win32_compat.c"]
    objects = []
    for index, src in enumerate(c_sources):
        obj = tmp / f"{index}.o"
        subprocess.run([cc, *cflags, "-c", str(src), "-o", str(obj)], check=True)
        objects.append(str(obj))
    for src in (ROOT / "runtime/config/config.cpp", ROOT / "runtime/diag/stop.cpp"):
        obj = tmp / (src.stem + ".o")
        subprocess.run([cxx, *cxxflags, "-c", str(src), "-o", str(obj)], check=True)
        objects.append(str(obj))
    binary = tmp / "test_spurs_job_crt"
    subprocess.run([cxx, *objects, "-pthread", "-lm", "-o", str(binary)], check=True)
    return binary


def hexs(n):
    return f"0x{n:X}"


def main():
    if os.name == "nt":
        print("SKIP: fixture currently requires the POSIX compiler/linker")
        return 0
    # The ambient environment must not arm a knob the test did not choose.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("SPU_", "PS3_", "PS3RECOMP_", "YDKJ_", "SPURS_"))}

    with tempfile.TemporaryDirectory(prefix="spurs-job-crt-") as tmp:
        tmp = Path(tmp)
        binary = str(build(tmp))

        def run(*args):
            return g.run([binary, *args], env=env, timeout=120)

        def passes(mode, *args):
            p = run(mode, *args)
            assert p.returncode == 0 and f"PASS: {mode}" in p.stdout, (
                mode, p.returncode, p.stdout, p.stderr[-3000:])
            assert "FAIL" not in p.stdout, p.stdout

        dump = tmp / "ver13.ls"
        passes("ver13", str(dump))
        digest = hashlib.sha256(dump.read_bytes()).hexdigest()
        assert digest == GOLDEN_VER13_LS, (
            f"the Ver13 local store at entry changed: sha256 {digest}")
        for mode in ("ver14", "fit", "swap-interp", "swap-lifted",
                     "swap-idle-interp", "swap-idle-lifted"):
            passes(mode)

        # jm word 0: the trap, on both paths
        for mode in ("word0-interp", "word0-lifted"):
            f = g.expect_stop(run(mode), "spu-job-jm-entry")
            assert (f["job"], f["image"], f["entry"], int(f["pc"], 16)) == (
                "0x00010000", "7", "0x3FAC0", RT_LS), f
            assert int(f["lr"], 16) > 0, f
        assert int(g.expect_stop(run("word0-lifted"), "spu-job-jm-entry")["lr"], 16) == 0x2F0

        # the version gate
        for mode, marker, raw in (("ver12", "JOBCRT Ver12", b"JOBCRT Ver12"),
                                  ("ver15", "JOBCRT Ver15", b"JOBCRT Ver15"),
                                  ("garbage", "??JOBCRT ver", b"\x01\x02JOBCRT ver"),
                                  ("empty", "?" * 12, bytes(12))):
            f = g.expect_stop(run(mode), "spu-job-crt-version")
            assert (f["job"], f["image"], f["marker"], f["bytes"]) == (
                "0x00010000", "7", marker, raw.hex().upper()), f

        # image + bss one quadword past the runtime area
        bss = ((RT_LS - IMAGE_SIZE) // 16 + 1) * 16
        f = g.expect_stop(run("overlap"), "spu-job-memory")
        assert (f["job"], f["image"], f["binary"], f["bss"], f["memory"], f["limit"]) == (
            "0x00010000", "7", hexs(IMAGE_SIZE), hexs(bss), hexs(IMAGE_SIZE + bss),
            hexs(RT_LS)), f
    print("PASS: the JOBCRT gate, the Ver14 jm interface, kernel view and bss")
    return 0


if __name__ == "__main__":
    sys.exit(main())
