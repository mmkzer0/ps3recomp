#!/usr/bin/env python3
"""Verify SDCRZ storage controls using synthetic data and real channels."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

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
          "spurs_policy_blob_weak.c", "spu_interp.c"]
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
