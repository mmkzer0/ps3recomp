#!/usr/bin/env python3
"""Verify SDCRZ storage controls using synthetic data and real channels.

Valid ranges are exercised in-process. Every malformed form and the stalled-tag
case is one more process, because the MFC error is a named stop (kind
spu-mfc-error) that ends the process at the cause."""
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tests" / "gates"))
import gates_common as g  # noqa: E402

if os.name == "nt":
    print("SKIP: SDCRZ fixture currently requires a POSIX C compiler")
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / "runtime/spu"
cflags = ["-std=gnu17", "-O1", "-UNDEBUG",
          *shlex.split(os.environ.get("CFLAGS", "")), "-pthread",
          "-I" + str(root / "include"), "-I" + str(spu),
          "-I" + str(root / "runtime/platform")]
cxxflags = ["-std=c++20", "-O1", "-UNDEBUG", "-I" + str(root / "include")]
common = ["spu_drain.c", "spu_lockstep.c", "spu_coherency.c", "spu_workload.c",
          "spurs_policy.c", "spurs_job.c", "spu_tsp_weak.c", "spu_vm_pagemap.c",
          "spurs_policy_blob_weak.c", "spu_interp.c", "../memory/reservation.c"]
env = {k: v for k, v in os.environ.items()
       if not k.startswith(("SPU_", "SPURS_", "YDKJ_", "LBP_", "GH3_"))}
with tempfile.TemporaryDirectory(prefix="spu-sdcrz-") as tmp:
    tmp = Path(tmp)
    # spu_dma.h calls ps3_cfg_* and ps3_stop directly (strong references), so
    # a fixture that inlines mfc_submit() must link the typed configuration
    # and the named-stop runtime.
    c_sources = [spu / "tests/test_spu_sdcrz.c", *[spu / name for name in common],
                 root / "runtime/platform/win32_compat.c"]
    objects = []
    for index, src in enumerate(c_sources):
        obj = tmp / f"{index}.o"
        subprocess.run([os.environ.get("CC", "cc"), *cflags, "-c", str(src),
                        "-o", str(obj)], check=True)
        objects.append(str(obj))
    for src in (root / "runtime/config/config.cpp", root / "runtime/diag/stop.cpp"):
        obj = tmp / (src.stem + ".o")
        subprocess.run([os.environ.get("CXX", "c++"), *cxxflags, "-c", str(src),
                        "-o", str(obj)], check=True)
        objects.append(str(obj))
    binary = tmp / "fixture"
    subprocess.run([os.environ.get("CXX", "c++"), *objects, "-pthread", "-lm",
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10, env=env, cwd=tmp)

    # Malformed storage controls and the stalled-tag case stop with the staged
    # command as the subject. The fixture sets LSA to SPU_LS_SIZE-1 (SDCRZ has
    # no LSA operand), tag 11, and leaves pc at 0 and the SPU id at 0.
    cases = [
        ("size", "size", 16512, 0x20000),
        ("class-size", "size", 16512, 0x20000),
        ("alignment", "alignment", 2048, 0x20001),
        ("size-alignment", "alignment", 16, 0x20000),
        ("ea-high", "ea-uncommitted", 2048, 0x100020000),
        ("ea-wrap", "ea-uncommitted", 256, 0xFFFFFF80),
        ("no-vm", "ea-uncommitted", 2048, 0x20000),
        ("stalled-tag", "sdczr-stalled-tag", 2048, 0x20000),
    ]
    for case, reason, size, ea in cases:
        p = subprocess.run([str(binary), case], capture_output=True, text=True,
                           timeout=10, env=env, cwd=tmp)
        f = g.expect_stop(p, "spu-mfc-error")
        assert f["reason"] == reason, (case, f)
        assert f["cmd"] == "0x89", (case, f)
        assert f["lsa"] == "0x3FFFF", (case, f)
        assert f["size"] == str(size), (case, f)
        assert f["ea"] == "0x%X" % ea, (case, f)
        assert f["tag"] == "11", (case, f)
        assert f["tid"] == "0", (case, f)
