#!/usr/bin/env python3
"""cellFs (ppu_fs.cpp) and sys_fs (sys_fs.c) share one fd space, with both real tables linked.

On hardware cellFsOpen / cellFsOpendir and the raw sys_fs_* calls draw from one kernel fd space and any call takes
any fd. This builds runtime/ppu/tests/test_fs_sys_bridge.cpp (which includes ppu_fs.cpp) against the real
runtime/syscalls/sys_fs.c and a ppu_recomp.h made from the lifter's own HEADER_PREAMBLE, and runs it: a cellFs
file and directory used through sys_fs_*, a sys_fs file and directory used through cellFs, distinct numbers across
both families, and eight threads opening and closing through both at once.

--tsan builds both files with -fsanitize=thread: the case that run is for is sys_fs_open against cellFsOpen on
several threads (the two tables' ownership checks raced before one lock guarded the space).

It needs a host C++ compiler (under CI=1 a missing one fails instead of skipping).

Usage:
    python3 tools/test_ppu_fs_sys_bridge.py [--tsan] [--exe-out PATH] [--build-only]
"""

import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
FIXTURE = os.path.join(ROOT, "runtime", "ppu", "tests", "test_fs_sys_bridge.cpp")
INCLUDE_DIRS = ["include", "runtime/ppu", "runtime/memory", "runtime", "libs", "."]


def _tool(env, *fallbacks):
    return shutil.which(os.environ.get(env, fallbacks[0])) or next(
        (p for p in map(shutil.which, fallbacks[1:]) if p), None)


def _stub_header():
    spec = importlib.util.spec_from_file_location(
        "ppu_lifter", os.path.join(TOOLS, "ppu_lifter.py"))
    lifter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lifter)
    return lifter.HEADER_PREAMBLE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tsan", action="store_true", help="build with -fsanitize=thread")
    ap.add_argument("--exe-out", help="keep the built test program at this path")
    ap.add_argument("--build-only", action="store_true", help="build and stop")
    args = ap.parse_args()

    if os.name == "nt":
        print("SKIP: the fd-space fixture uses POSIX file and directory calls")
        return 0

    cxx = _tool("CXX", "c++", "g++", "clang++")
    cc = _tool("CC", "cc", "gcc", "clang")
    if cxx is None or cc is None:
        if os.environ.get("CI"):
            print("FAIL: no C/C++ compiler found")
            return 1
        print("skip test_ppu_fs_sys_bridge: no C/C++ compiler")
        return 0
    # --tsan, or SANITIZERS=thread from the tsan ctest preset.
    tsan = args.tsan or os.environ.get("SANITIZERS") == "thread"
    san = ["-fsanitize=thread", "-g"] if tsan else []
    with tempfile.TemporaryDirectory() as td:
        with open(os.path.join(td, "ppu_recomp.h"), "w") as fh:
            fh.write(_stub_header())
        # The real sys_fs.c, as C: its handlers are what the fixture calls.
        sys_fs_obj = os.path.join(td, "sys_fs.o")
        cmd = [cc, "-std=gnu17", "-w", "-O1", "-UNDEBUG", *san,
               "-I" + os.path.join(ROOT, "include"), "-I" + ROOT,
               "-c", os.path.join(ROOT, "runtime/syscalls/sys_fs.c"), "-o", sys_fs_obj]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: sys_fs.c did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        exe = args.exe_out or os.path.join(td, "test_fs_sys_bridge")
        cmd = [cxx, "-std=gnu++17", "-w", "-O1", *san, "-I" + td]
        cmd += ["-I" + os.path.join(ROOT, d) for d in INCLUDE_DIRS]
        # ppu_fs.cpp's and sys_fs.c's migrated diagnostics call ps3_log_*; config.cpp answers ps3_config_get.
        # The fixture stubs the stop runtime and the quick-exit hook (its link stubs).
        cmd += ["-o", exe, FIXTURE, sys_fs_obj,
                os.path.join(ROOT, "runtime/diag/ps3_log.c"),
                os.path.join(ROOT, "runtime/config/config.cpp"), "-pthread"]
        if "mingw" in os.path.basename(cxx):
            cmd += ["-static"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        if args.build_only:
            print(f"built {exe}")
            return 0
        r = subprocess.run([exe, td], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        return r.returncode


if __name__ == "__main__":
    sys.exit(main())
