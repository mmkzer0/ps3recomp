#!/usr/bin/env python3
"""The SPURS return codes the guest sees, through the real cellSpurs.c (fixture: tests/test_spurs_error_codes.c).

Builds the fixture with the runtime units the module needs -- the same set tools/test_spurs_dispatch_miss.py
uses, because CreateTask pulls in the taskset and scheduler code -- and runs it. The fixture pins the firmware
literals: a task code that drifts back to the old 0x804107xx range fails it.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

UNITS = ["spu_channels", "spu_drain", "spu_lockstep", "spu_coherency", "spu_workload",
         "spurs_policy", "spurs_job", "spu_tsp_weak", "spu_vm_pagemap",
         "spurs_policy_blob_weak", "spu_interp"]


def build(tmp):
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    cflags = ["-std=gnu17", "-D_GNU_SOURCE", "-O1", "-UNDEBUG", "-ffunction-sections", "-fdata-sections",
              "-I" + str(ROOT / "include"), "-I" + str(ROOT / "runtime/spu"),
              "-I" + str(ROOT / "runtime/platform")]
    cxxflags = ["-std=c++20", "-O1", "-UNDEBUG", "-ffunction-sections", "-fdata-sections",
                "-I" + str(ROOT / "include")]
    c_sources = [ROOT / "tests/test_spurs_error_codes.c",
                 *[ROOT / "runtime/spu" / f"{u}.c" for u in UNITS],
                 ROOT / "libs/spurs/spurs_taskset.c",
                 ROOT / "runtime/memory/reservation.c", ROOT / "runtime/memory/label_window.c",
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
    binary = tmp / "test_spurs_error_codes"
    link = ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]
    subprocess.run([cxx, *objects, *link, "-pthread", "-lm", "-o", str(binary)], check=True)
    return binary


def main():
    if os.name == "nt":
        print("SKIP: POSIX fixture")
        return 0
    with tempfile.TemporaryDirectory(prefix="spurs-error-codes-") as tmp:
        binary = build(Path(tmp))
        subprocess.run([str(binary)], check=True, timeout=30)
    return 0


if __name__ == "__main__":
    sys.exit(main())
